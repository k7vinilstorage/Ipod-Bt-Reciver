/*
 * btstack_control_ipod.c
 *
 * Power up / configure the Broadcom Bluetooth controller with Apple's BlueTool and hand it
 * over to BTstack, then give it back to iOS on exit.
 *
 * Based on BTstack for iOS (bluekitchen/btstack, port/ios/src/btstack_control_iphone.m)
 * by Matthias Ringwald, PowerManagement by Jens David. Reduced to the iOS 6+ / Broadcom
 * path (iPhone 4S/5, iPod touch 5, iPad 2+), with the Apple Bluetooth daemons that get
 * stopped detected at runtime instead of hard-coded.
 *
 * Original: Copyright (C) 2009-2012 by Matthias Ringwald - BTstack license
 * (free for non-commercial use, see btstack/LICENSE).
 */

#include "btstack_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

#include "btstack_debug.h"
#include "btstack_util.h"
#include "hci_transport.h"

#include "btstack_control_ipod.h"

// --- minimal IOKit declarations (the SDK ships IOKit.tbd but no headers) ---
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef char io_name_t[128];
kern_return_t IOMasterPort(mach_port_t bootstrapPort, mach_port_t * masterPort);
CFMutableDictionaryRef IOServiceNameMatching(const char * name);
io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
CFTypeRef IORegistryEntrySearchCFProperty(io_object_t entry, const io_name_t plane, CFStringRef key,
                                          CFAllocatorRef allocator, uint32_t options);
kern_return_t IOObjectRelease(io_object_t object);
#define kIORegistryIterateRecursively 1

// Apple's Bluetooth launchd jobs. Names changed between iOS releases, so every one that
// exists gets stopped and the same ones get started again on exit.
static const char * apple_bt_jobs[] = {
    "/System/Library/LaunchDaemons/com.apple.BTServer.plist",
    "/System/Library/LaunchDaemons/com.apple.BTServer.le.plist",
    "/System/Library/LaunchDaemons/com.apple.BTLEServer.plist",
    "/System/Library/LaunchDaemons/com.apple.bluetoothd.plist",
    "/System/Library/LaunchDaemons/com.apple.BlueTool.plist",
};
#define NUM_APPLE_BT_JOBS (sizeof(apple_bt_jobs) / sizeof(apple_bt_jobs[0]))
static int apple_bt_job_unloaded[NUM_APPLE_BT_JOBS];

static const char * bluetool_candidates[] = {
    "/usr/sbin/BlueTool", "/usr/bin/BlueTool", "/usr/local/bin/BlueTool", "/sbin/BlueTool",
};

static const hci_transport_config_uart_t * uart_config;
static bd_addr_t local_mac_address;
static uint32_t  transport_speed = 3000000;
static int       controller_owned;
static char      buffer[128];

static const char * bluetool_path(void){
    unsigned int i;
    for (i = 0; i < sizeof(bluetool_candidates) / sizeof(bluetool_candidates[0]); i++){
        if (access(bluetool_candidates[i], X_OK) == 0) return bluetool_candidates[i];
    }
    return "BlueTool";
}

static int run(const char * cmd){
    log_info("$ %s", cmd);
    int rc = system(cmd);
    if (rc != 0) log_info("  -> exit status %d", rc);
    return rc;
}

static const char * machine_name(void){
    static struct utsname info;
    uname(&info);
    return info.machine;
}

int btstack_control_ipod_is_supported(void){
    const char * machine = machine_name();
    // iPhone 2G/3G use a CSR chip and iPod touch 1G has no Bluetooth: not supported here
    if (strncmp(machine, "iPhone1,", 8) == 0) return 0;
    if (strncmp(machine, "iPod1,", 6) == 0) return 0;
    return 1;
}

const char * btstack_control_ipod_machine(void){
    return machine_name();
}

static void ioregistry_get_info(void){
    mach_port_t master = MACH_PORT_NULL;
    IOMasterPort(MACH_PORT_NULL, &master);
    io_service_t bt_service = IOServiceGetMatchingService(master, IOServiceNameMatching("bluetooth"));
    if (!bt_service){
        log_error("IORegistry: no 'bluetooth' node");
        return;
    }

    CFTypeRef ref = IORegistryEntrySearchCFProperty(bt_service, "IODeviceTree", CFSTR("local-mac-address"),
                                                    kCFAllocatorDefault, kIORegistryIterateRecursively);
    if (ref && CFGetTypeID(ref) == CFDataGetTypeID() && CFDataGetLength((CFDataRef) ref) >= 6){
        CFDataGetBytes((CFDataRef) ref, CFRangeMake(0, 6), local_mac_address);
    }
    if (ref) CFRelease(ref);

    ref = IORegistryEntrySearchCFProperty(bt_service, "IODeviceTree", CFSTR("transport-speed"),
                                          kCFAllocatorDefault, kIORegistryIterateRecursively);
    if (ref && CFGetTypeID(ref) == CFDataGetTypeID() && CFDataGetLength((CFDataRef) ref) >= 4){
        uint32_t speed = 0;
        CFDataGetBytes((CFDataRef) ref, CFRangeMake(0, 4), (uint8_t *) &speed);
        if (speed) transport_speed = speed;
    }
    if (ref) CFRelease(ref);

    IOObjectRelease(bt_service);
    log_info("IORegistry: local-mac-address %s, transport-speed %u", bd_addr_to_str(local_mac_address), transport_speed);
}

uint32_t btstack_control_ipod_get_transport_speed(void){
    return transport_speed;
}

static void stop_apple_bluetooth(void){
    unsigned int i;
    for (i = 0; i < NUM_APPLE_BT_JOBS; i++){
        apple_bt_job_unloaded[i] = 0;
        if (access(apple_bt_jobs[i], R_OK) != 0) continue;
        snprintf(buffer, sizeof(buffer), "launchctl unload %s", apple_bt_jobs[i]);
        run(buffer);
        apple_bt_job_unloaded[i] = 1;
    }
    // make sure nothing still holds the UART
    run("killall -9 BlueTool >/dev/null 2>&1");
    run("killall -9 BTServer >/dev/null 2>&1");
    run("killall -9 bluetoothd >/dev/null 2>&1");
    sleep(1);
}

static void start_apple_bluetooth(void){
    int i;
    // reverse order: BlueTool first, BTServer last
    for (i = (int) NUM_APPLE_BT_JOBS - 1; i >= 0; i--){
        if (!apple_bt_job_unloaded[i]) continue;
        snprintf(buffer, sizeof(buffer), "launchctl load %s", apple_bt_jobs[i]);
        run(buffer);
        apple_bt_job_unloaded[i] = 0;
    }
}

void btstack_control_ipod_restore_system_bluetooth(void){
    if (!controller_owned) return;
    controller_owned = 0;
    start_apple_bluetooth();
}

static void bluetool_write(FILE * bluetool, const char * line){
    log_info("BlueTool < %s", line);
    fputs(line, bluetool);
    fputc('\n', bluetool);
}

static void ipod_init(const void * transport_config){
    uart_config = NULL;
    if (!transport_config || ((const hci_transport_config_t *) transport_config)->type != HCI_TRANSPORT_CONFIG_UART){
        log_error("control_ipod: need hci_transport_config_uart_t");
        return;
    }
    uart_config = (const hci_transport_config_uart_t *) transport_config;
}

static int ipod_on(void){
    log_info("control_ipod: power on (%s)", machine_name());

    if (!btstack_control_ipod_is_supported()){
        log_error("control_ipod: %s is not supported (no Broadcom controller)", machine_name());
        return -1;
    }

    ioregistry_get_info();

    controller_owned = 1;
    stop_apple_bluetooth();

    const char * bluetool = bluetool_path();

    // basic power-up with Apple's own boot/init scripts (firmware patch etc.)
    snprintf(buffer, sizeof(buffer), "%s -F boot", bluetool);
    run(buffer);
    snprintf(buffer, sizeof(buffer), "%s -F init", bluetool);
    run(buffer);

    // custom config: set BD_ADDR, enable UART sleep mode (BT_WAKE / HOST_WAKE)
    FILE * bt = popen(bluetool, "r+");
    if (!bt){
        log_error("control_ipod: cannot start %s: %s", bluetool, strerror(errno));
        return -1;
    }
    setvbuf(bt, NULL, _IONBF, 0);

    bluetool_write(bt, "device -D -S");
    bluetool_write(bt, "wake on");
    snprintf(buffer, sizeof(buffer), "bcm -a %02x:%02x:%02x:%02x:%02x:%02x",
             local_mac_address[0], local_mac_address[1], local_mac_address[2],
             local_mac_address[3], local_mac_address[4], local_mac_address[5]);
    if (!btstack_is_null_bd_addr(local_mac_address)){
        bluetool_write(bt, buffer);
    }
    if (uart_config && uart_config->baudrate_init){
        snprintf(buffer, sizeof(buffer), "bcm -b %u", uart_config->baudrate_init);
        bluetool_write(bt, buffer);
        bluetool_write(bt, "msleep 200");
    }
    bluetool_write(bt, "bcm -s 0x01,0x00,0x00,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x01");
    bluetool_write(bt, "msleep 50");
    bluetool_write(bt, "wake off");
    bluetool_write(bt, "quit");
    fflush(bt);

    // log what BlueTool says
    char line[160];
    while (fgets(line, sizeof(line), bt)){
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) log_info("BlueTool > %s", line);
    }
    int rc = pclose(bt);
    if (rc != 0){
        log_error("control_ipod: BlueTool exited with %d", rc);
    }
    return 0;
}

static int ipod_off(void){
    log_info("control_ipod: power off, returning Bluetooth to iOS");
    btstack_control_ipod_restore_system_bluetooth();
    return 0;
}

// the controller sleeps / wakes by itself (UART sleep mode set above)
static int ipod_sleep(void){ return 0; }
static int ipod_wake(void){ return 0; }

static const btstack_control_t control_ipod = {
    .init  = &ipod_init,
    .on    = &ipod_on,
    .off   = &ipod_off,
    .sleep = &ipod_sleep,
    .wake  = &ipod_wake,
    .register_for_power_notifications = NULL,
};

const btstack_control_t * btstack_control_ipod_instance(void){
    return &control_ipod;
}
