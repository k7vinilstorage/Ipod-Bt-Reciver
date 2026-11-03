/*
 * a2dpsinkd - turns a jailbroken iPod touch 5 (or other BCM-based iOS 6-9 device)
 * into a Bluetooth A2DP receiver.
 *
 * Started/stopped by launchd: the job runs while SINK_ENABLED_FLAG exists (the app
 * creates/removes it). On start it takes the Bluetooth controller away from iOS, on
 * exit it gives it back.
 *
 * Usage: a2dpsinkd [-f] [-d]
 *   -f  run even if the enabled flag is missing (manual testing over SSH)
 *   -d  write an HCI packet log to /tmp/a2dpsink.pklg (open with Wireshark / PacketLogger)
 */

#include "btstack_config.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv_posix.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hci_dump_posix_fs.h"
#include "hci_dump_posix_stdout.h"

#include "btstack_audio_ios.h"
#include "btstack_control_ipod.h"
#include "hci_transport_h4_ipod.h"
#include "sink.h"
#include "sink_status.h"

#define HCI_DUMP_PATH        "/tmp/a2dpsink.pklg"
#define POLL_INTERVAL_MS     1000
#define STARTUP_TIMEOUT_MS   20000
#define SHUTDOWN_TIMEOUT_MS  4000

// --- IOKit power assertion: keep the iPod from sleeping while the receiver is on ---
typedef uint32_t IOPMAssertionID;
typedef uint32_t IOPMAssertionLevel;
int IOPMAssertionCreateWithName(CFStringRef type, IOPMAssertionLevel level, CFStringRef name, IOPMAssertionID * id);
int IOPMAssertionRelease(IOPMAssertionID id);
#define kIOPMAssertionLevelOn 255

static IOPMAssertionID power_assertion;

static hci_transport_config_uart_t transport_config = {
    .type          = HCI_TRANSPORT_CONFIG_UART,
    .baudrate_init = 0,                     // 0 = native speed from IORegistry
    .baudrate_main = 0,
    .flowcontrol   = 1,
    .device_name   = "com.apple.uart.bluetooth",
    .parity        = 0,
};

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_tlv_posix_t tlv_context;
static btstack_timer_source_t poll_timer;
static btstack_timer_source_t startup_timer;
static btstack_timer_source_t shutdown_timer;
static btstack_data_source_t  signal_data_source;
static int signal_pipe[2] = { -1, -1 };
static int ignore_enabled_flag;
static int shutting_down;
static int hci_working;
static char local_name[64] = SINK_DEFAULT_NAME;

static void finish(int exit_code){
    sink_shutdown_audio();
    btstack_control_ipod_restore_system_bluetooth();
    if (power_assertion) IOPMAssertionRelease(power_assertion);
    btstack_tlv_posix_deinit(&tlv_context);
    log_info("a2dpsinkd: exit %d", exit_code);
    exit(exit_code);
}

// fatal error: report it to the app, switch the receiver off so launchd doesn't loop
static void fail(const char * message){
    log_error("a2dpsinkd: %s", message);
    sink_status_set_error(message);
    if (!ignore_enabled_flag) unlink(SINK_ENABLED_FLAG);
    finish(1);
}

static void shutdown_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    log_error("a2dpsinkd: HCI did not power off in time, forcing exit");
    sink_status_set_state(SINK_STATE_STOPPING);
    finish(0);
}

static void begin_shutdown(void){
    if (shutting_down) return;
    shutting_down = 1;
    log_info("a2dpsinkd: shutting down");
    sink_status_set_state(SINK_STATE_STOPPING);
    btstack_run_loop_remove_timer(&poll_timer);
    btstack_run_loop_remove_timer(&startup_timer);

    btstack_run_loop_set_timer_handler(&shutdown_timer, &shutdown_timeout_handler);
    btstack_run_loop_set_timer(&shutdown_timer, SHUTDOWN_TIMEOUT_MS);
    btstack_run_loop_add_timer(&shutdown_timer);

    if (hci_working){
        sink_disconnect();
        hci_power_control(HCI_POWER_OFF);   // -> HCI_STATE_OFF -> finish()
    } else {
        finish(0);
    }
}

static void startup_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (!hci_working) fail("Bluetooth controller did not respond");
}

static void poll_timer_handler(btstack_timer_source_t * ts){
    if (!ignore_enabled_flag && access(SINK_ENABLED_FLAG, F_OK) != 0){
        begin_shutdown();
        return;
    }
    if (hci_working && access(SINK_FORGET_FLAG, F_OK) == 0){
        log_info("a2dpsinkd: forgetting all paired devices");
        unlink(SINK_FORGET_FLAG);
        unlink(SINK_LAST_DEVICE);
        gap_delete_all_link_keys();
    }
    btstack_run_loop_set_timer(ts, POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void signal_handler(int sig){
    char c = (char) sig;
    if (write(signal_pipe[1], &c, 1) < 0) { /* nothing we can do */ }
}

static void signal_data_source_handler(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(type);
    char c;
    if (read(ds->source.fd, &c, 1) == 1){
        log_info("a2dpsinkd: signal %d", c);
        begin_shutdown();
    }
}

static void setup_signals(void){
    if (pipe(signal_pipe) != 0) return;
    btstack_run_loop_set_data_source_fd(&signal_data_source, signal_pipe[0]);
    btstack_run_loop_set_data_source_handler(&signal_data_source, &signal_data_source_handler);
    btstack_run_loop_enable_data_source_callbacks(&signal_data_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&signal_data_source);
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    signal(SIGHUP,  signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_POWERON_FAILED:
            fail("Could not open the Bluetooth controller");
            break;
        case BTSTACK_EVENT_STATE:
            switch (btstack_event_state_get_state(packet)){
                case HCI_STATE_WORKING: {
                    hci_working = 1;
                    btstack_run_loop_remove_timer(&startup_timer);
                    bd_addr_t addr;
                    gap_local_bd_addr(addr);
                    log_info("a2dpsinkd: Bluetooth up, %s, name '%s'", bd_addr_to_str(addr), local_name);
                    sink_status_set_local_addr(bd_addr_to_str(addr));
                    sink_status_set_state(SINK_STATE_READY);
                    sink_reconnect_last_device();
                    break;
                }
                case HCI_STATE_OFF:
                    if (shutting_down) finish(0);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void load_name(void){
    FILE * f = fopen(SINK_NAME_FILE, "r");
    if (!f) return;
    char line[sizeof(local_name)];
    if (fgets(line, sizeof(line), f)){
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) strcpy(local_name, line);
    }
    fclose(f);
}

int main(int argc, const char * argv[]){
    int hci_dump = 0;
    int i;
    for (i = 1; i < argc; i++){
        if (strcmp(argv[i], "-f") == 0) ignore_enabled_flag = 1;
        else if (strcmp(argv[i], "-d") == 0) hci_dump = 1;
        else {
            fprintf(stderr, "usage: %s [-f] [-d]\n", argv[0]);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    mkdir(SINK_DATA_DIR, 0755);
    if (!ignore_enabled_flag && access(SINK_ENABLED_FLAG, F_OK) != 0){
        printf("a2dpsinkd: %s missing, receiver is switched off\n", SINK_ENABLED_FLAG);
        return 0;
    }

    load_name();
    sink_status_set_name(local_name);
    sink_status_set_state(SINK_STATE_STARTING);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());

    if (hci_dump){
        hci_dump_posix_fs_open(HCI_DUMP_PATH, HCI_DUMP_PACKETLOGGER);
        hci_dump_init(hci_dump_posix_fs_get_instance());
        printf("a2dpsinkd: HCI log at %s\n", HCI_DUMP_PATH);
    } else {
        // log_info/log_error go through hci_dump: print them to stdout (launchd log),
        // but not every HCI packet (audio streaming would flood the log)
        hci_dump_init(hci_dump_posix_stdout_get_instance());
        hci_dump_enable_packet_log(false);
    }

    if (!btstack_control_ipod_is_supported()){
        char msg[96];
        snprintf(msg, sizeof(msg), "%s is not supported", btstack_control_ipod_machine());
        sink_status_set_error(msg);
        if (!ignore_enabled_flag) unlink(SINK_ENABLED_FLAG);
        return 1;
    }

    IOPMAssertionCreateWithName(CFSTR("PreventUserIdleSystemSleep"), kIOPMAssertionLevelOn,
                                CFSTR("A2DP receiver"), &power_assertion);

    setup_signals();

    // pairings
    const btstack_tlv_t * tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, SINK_LINK_KEY_DB);
    btstack_tlv_set_instance(tlv_impl, &tlv_context);

    hci_init(hci_transport_h4_ipod_instance(), &transport_config);
    hci_set_control(btstack_control_ipod_instance());
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));
    hci_transport_h4_ipod_set_enforce_wake_device("/dev/btwake");

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sink_setup(local_name, btstack_audio_ios_sink_get_instance());

    btstack_run_loop_set_timer_handler(&poll_timer, &poll_timer_handler);
    btstack_run_loop_set_timer(&poll_timer, POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(&poll_timer);

    btstack_run_loop_set_timer_handler(&startup_timer, &startup_timeout_handler);
    btstack_run_loop_set_timer(&startup_timer, STARTUP_TIMEOUT_MS);
    btstack_run_loop_add_timer(&startup_timer);

    printf("a2dpsinkd: starting on %s\n", btstack_control_ipod_machine());
    hci_power_control(HCI_POWER_ON);

    btstack_run_loop_execute();
    return 0;
}
