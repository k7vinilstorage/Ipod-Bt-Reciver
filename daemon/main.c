/*
 * a2dpsinkd - turns a jailbroken iPod touch 5 (or other BCM-based iOS 6-9 device)
 * into a Bluetooth A2DP receiver.
 *
 * Started/stopped by launchd: the job runs while SINK_ENABLED_FLAG exists (the app
 * creates/removes it). Bluetooth is taken away from iOS when the receiver becomes active
 * and given back on exit.
 *
 * "Only while charging" (settings file): the daemon waits without touching Bluetooth
 * until external power (charger, computer, dock) is connected. When power goes away for
 * more than a few seconds it exits; launchd starts it again and it goes back to waiting.
 * One process = at most one Bluetooth session, so every session starts from a clean state.
 *
 * "Keep music service running" (settings file): the receiver is the iPod's playing music
 * app all the time, Bluetooth on or off (sink.c). It starts with the process, before
 * Bluetooth, so a waiting daemon already plays when the car's head unit connects; the
 * restart after power loss happens while the car is off.
 *
 * Usage: a2dpsinkd [-f] [-d]
 *   -f  run even if the enabled flag is missing (manual testing over SSH)
 *   -d  write an HCI packet log to /tmp/a2dpsink.pklg (open with Wireshark / PacketLogger)
 */

#include "btstack_config.h"

#include <dlfcn.h>
#include <errno.h>
#include <notify.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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

#define HCI_DUMP_PATH          "/tmp/a2dpsink.pklg"
#define POLL_INTERVAL_MS       1000
#define IDLE_POLL_INTERVAL_MS  5000     // fallback; power changes also arrive via notify
#define POWER_LOSS_GRACE_MS    5000     // ignore short cable wiggles
#define STARTUP_TIMEOUT_MS     20000
#define SHUTDOWN_TIMEOUT_MS    4000

// notify(3) name posted by powerd when the power source changes (kIOPSNotifyPowerSource)
#define POWER_SOURCE_NOTIFICATION "com.apple.system.powersources.source"

// --- IOKit (no headers in the SDK) ---
typedef uint32_t IOPMAssertionID;
typedef uint32_t IOPMAssertionLevel;
int IOPMAssertionCreateWithName(CFStringRef type, IOPMAssertionLevel level, CFStringRef name, IOPMAssertionID * id);
int IOPMAssertionRelease(IOPMAssertionID id);
#define kIOPMAssertionLevelOn 255
CFTypeRef       IOPSCopyPowerSourcesInfo(void);
CFStringRef     IOPSGetProvidingPowerSourceType(CFTypeRef snapshot);
CFArrayRef      IOPSCopyPowerSourcesList(CFTypeRef snapshot);
CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef snapshot, CFTypeRef source);

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
static btstack_data_source_t  power_notify_data_source;
static int signal_pipe[2] = { -1, -1 };
static int ignore_enabled_flag;
static int shutting_down;
static int bluetooth_started;
static int hci_working;
static uint32_t power_lost_since_ms;
static char local_name[64] = SINK_DEFAULT_NAME;

// settings (written by the app)
static int setting_only_when_charging;
static int setting_auto_reconnect = 1;
static int setting_auto_play;
static int setting_media_controls = 1;   // lock screen + car/USB buttons, on by default
static int setting_music_service;        // stay the playing music app all the time

// --- helpers ---

static void load_settings(void){
    setting_only_when_charging = 0;
    setting_auto_reconnect = 1;
    setting_auto_play = 0;
    setting_media_controls = 1;
    setting_music_service = 0;
    FILE * f = fopen(SINK_SETTINGS_FILE, "r");
    if (!f) return;
    char line[64];
    int value;
    while (fgets(line, sizeof(line), f)){
        if (sscanf(line, "only_when_charging=%d", &value) == 1) setting_only_when_charging = value != 0;
        if (sscanf(line, "auto_reconnect=%d", &value) == 1)     setting_auto_reconnect = value != 0;
        if (sscanf(line, "auto_play=%d", &value) == 1)          setting_auto_play = value != 0;
        if (sscanf(line, "media_controls=%d", &value) == 1)     setting_media_controls = value != 0;
        if (sscanf(line, "music_service=%d", &value) == 1)      setting_music_service = value != 0;
    }
    fclose(f);
}

// external power (charger, computer, car) and the battery level in percent (-1 = unknown)
static void read_power(int * external, int * battery_percent){
    static int warned;
    *external = 1;
    *battery_percent = -1;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info){
        if (!warned) log_error("a2dpsinkd: cannot read power source, assuming external power");
        warned = 1;
        return;
    }
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    *external = type && CFStringCompare(type, CFSTR("AC Power"), 0) == kCFCompareEqualTo;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    if (list){
        CFIndex i;
        for (i = 0; i < CFArrayGetCount(list) && *battery_percent < 0; i++){
            CFDictionaryRef ps = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
            if (!ps) continue;
            CFTypeRef cur = CFDictionaryGetValue(ps, CFSTR("Current Capacity"));
            CFTypeRef max = CFDictionaryGetValue(ps, CFSTR("Max Capacity"));
            int c = 0, m = 0;
            if (cur && max && CFGetTypeID(cur) == CFNumberGetTypeID() && CFGetTypeID(max) == CFNumberGetTypeID()
                && CFNumberGetValue((CFNumberRef) cur, kCFNumberIntType, &c)
                && CFNumberGetValue((CFNumberRef) max, kCFNumberIntType, &m) && m > 0){
                *battery_percent = c * 100 / m;
            }
        }
        CFRelease(list);
    }
    CFRelease(info);
}

static int on_external_power(void){
    int external, battery;
    read_power(&external, &battery);
    return external;
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

// Bluetooth I/O and SBC decoding run on this thread: keep iOS from treating it as
// background work (that gets worse while the screen is off).
static void raise_priority(void){
    if (setpriority(PRIO_PROCESS, 0, -10) != 0){
        printf("a2dpsinkd: setpriority failed: %s\n", strerror(errno));
    }
    typedef int (*set_qos_class_t)(unsigned int qos_class, int relative_priority);
    set_qos_class_t set_qos = (set_qos_class_t) dlsym(RTLD_DEFAULT, "pthread_set_qos_class_self_np");  // iOS 8+
    if (set_qos) set_qos(0x21 /* QOS_CLASS_USER_INTERACTIVE */, 0);
}

// --- shutdown ---

static void finish(int exit_code){
    sink_shutdown_audio();     // also the music service, which runs without Bluetooth
    if (bluetooth_started){
        btstack_control_ipod_restore_system_bluetooth();
        btstack_tlv_posix_deinit(&tlv_context);
    }
    if (power_assertion) IOPMAssertionRelease(power_assertion);
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
    finish(0);
}

static void begin_shutdown(void){
    if (shutting_down) return;
    shutting_down = 1;
    log_info("a2dpsinkd: shutting down");
    sink_status_set_state(SINK_STATE_STOPPING);
    btstack_run_loop_remove_timer(&poll_timer);
    btstack_run_loop_remove_timer(&startup_timer);

    if (!hci_working) finish(0);

    btstack_run_loop_set_timer_handler(&shutdown_timer, &shutdown_timeout_handler);
    btstack_run_loop_set_timer(&shutdown_timer, SHUTDOWN_TIMEOUT_MS);
    btstack_run_loop_add_timer(&shutdown_timer);

    sink_disconnect();
    hci_power_control(HCI_POWER_OFF);   // -> HCI_STATE_OFF -> finish()
}

// --- Bluetooth session ---

static void startup_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (!hci_working) fail("Bluetooth controller did not respond");
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
                    sink_media_prepare();    // lock screen / car controls (isolated, may be unavailable)
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

static void start_bluetooth(void){
    if (bluetooth_started) return;
    bluetooth_started = 1;
    printf("a2dpsinkd: starting Bluetooth on %s%s\n", btstack_control_ipod_machine(),
           setting_only_when_charging ? " (external power connected)" : "");

    sink_status_set_state(SINK_STATE_STARTING);

    // keep the iPod awake while the receiver is active
    IOPMAssertionCreateWithName(CFSTR("PreventUserIdleSystemSleep"), kIOPMAssertionLevelOn,
                                CFSTR("A2DP receiver"), &power_assertion);

    // pairings
    const btstack_tlv_t * tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, SINK_LINK_KEY_DB);
    btstack_tlv_set_instance(tlv_impl, &tlv_context);

    hci_init(hci_transport_h4_ipod_instance(), &transport_config);
    hci_set_control(btstack_control_ipod_instance());
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));
    hci_transport_h4_ipod_set_enforce_wake_device("/dev/btwake");

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sink_setup(local_name);
    sink_set_auto_reconnect(setting_auto_reconnect);
    sink_set_auto_play(setting_auto_play);
    sink_set_media_controls(setting_media_controls);

    btstack_run_loop_set_timer_handler(&startup_timer, &startup_timeout_handler);
    btstack_run_loop_set_timer(&startup_timer, STARTUP_TIMEOUT_MS);
    btstack_run_loop_add_timer(&startup_timer);

    hci_power_control(HCI_POWER_ON);   // blocks while BlueTool boots the chip
}

// --- periodic / event driven checks ---

static void evaluate(void){
    if (shutting_down) return;

    if (!ignore_enabled_flag && access(SINK_ENABLED_FLAG, F_OK) != 0){
        begin_shutdown();
        return;
    }

    load_settings();
    int external, battery;
    read_power(&external, &battery);
    int wanted = !setting_only_when_charging || external;

    // Now Playing and the music service work with or without Bluetooth
    sink_set_power_state(external, battery);
    sink_set_media_controls(setting_media_controls);
    sink_set_music_service(setting_music_service);

    if (!bluetooth_started){
        if (wanted) start_bluetooth();
        return;
    }

    if (!wanted){
        uint32_t now = btstack_run_loop_get_time_ms();
        if (power_lost_since_ms == 0){
            power_lost_since_ms = now ? now : 1;
            log_info("a2dpsinkd: external power gone");
        } else if (now - power_lost_since_ms >= POWER_LOSS_GRACE_MS){
            log_info("a2dpsinkd: still no external power, handing Bluetooth back to iOS");
            begin_shutdown();   // launchd restarts us -> waiting for power
            return;
        }
    } else {
        power_lost_since_ms = 0;
    }

    sink_set_auto_reconnect(setting_auto_reconnect);
    sink_set_auto_play(setting_auto_play);

    if (hci_working && access(SINK_FORGET_FLAG, F_OK) == 0){
        log_info("a2dpsinkd: forgetting all paired devices");
        unlink(SINK_FORGET_FLAG);
        unlink(SINK_LAST_DEVICE);
        sink_forget_last_device();
        gap_delete_all_link_keys();
    }
}

static void poll_timer_handler(btstack_timer_source_t * ts){
    evaluate();
    if (shutting_down) return;
    btstack_run_loop_set_timer(ts, bluetooth_started ? POLL_INTERVAL_MS : IDLE_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void power_notify_handler(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(type);
    int token;
    if (read(ds->source.fd, &token, sizeof(token)) <= 0) return;
    log_info("a2dpsinkd: power source changed (%s)", on_external_power() ? "external power" : "battery");
    evaluate();
}

static void setup_power_notifications(void){
    int fd = -1, token = 0;
    if (notify_register_file_descriptor(POWER_SOURCE_NOTIFICATION, &fd, 0, &token) != NOTIFY_STATUS_OK){
        log_error("a2dpsinkd: notify_register_file_descriptor failed, polling only");
        return;
    }
    btstack_run_loop_set_data_source_fd(&power_notify_data_source, fd);
    btstack_run_loop_set_data_source_handler(&power_notify_data_source, &power_notify_handler);
    btstack_run_loop_enable_data_source_callbacks(&power_notify_data_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&power_notify_data_source);
}

// --- signals ---

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
    raise_priority();

    mkdir(SINK_DATA_DIR, 0755);
    if (!ignore_enabled_flag && access(SINK_ENABLED_FLAG, F_OK) != 0){
        printf("a2dpsinkd: %s missing, receiver is switched off\n", SINK_ENABLED_FLAG);
        return 0;
    }

    load_name();
    load_settings();
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

    setup_signals();
    setup_power_notifications();

    // audio first: with the music service it plays (silently) before Bluetooth is up, so a
    // head unit connecting at the same time as the power finds a playing iPod
    int external, battery;
    read_power(&external, &battery);
    sink_init_audio(btstack_audio_ios_sink_get_instance());
    sink_set_power_state(external, battery);
    sink_set_media_controls(setting_media_controls);
    sink_set_music_service(setting_music_service);

    btstack_run_loop_set_timer_handler(&poll_timer, &poll_timer_handler);

    if (!setting_only_when_charging || external){
        start_bluetooth();
        btstack_run_loop_set_timer(&poll_timer, POLL_INTERVAL_MS);
    } else {
        printf("a2dpsinkd: 'only while charging' is on and there is no external power, waiting%s\n",
               setting_music_service ? " (music service running)" : "");
        sink_status_set_state(SINK_STATE_WAITING_POWER);
        btstack_run_loop_set_timer(&poll_timer, IDLE_POLL_INTERVAL_MS);
    }
    btstack_run_loop_add_timer(&poll_timer);

    btstack_run_loop_execute();
    return 0;
}
