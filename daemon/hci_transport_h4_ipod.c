/*
 * hci_transport_h4_ipod.c
 *
 * H4 transport to the built-in Broadcom Bluetooth controller of iOS devices.
 *
 * iOS does not expose the Bluetooth UART as a tty. Since iOS 4, BTServer talks to it
 * through a netgraph socket node called "com.apple.uart.bluetooth". The magic ioctl /
 * sockopt values below were reverse engineered from BTServer by Matthias Ringwald for
 * "BTstack for iOS" (bluekitchen/btstack, port/ios/src/hci_transport_h4_iphone.c),
 * which shipped for iOS 9. This file started from that implementation, updated to the
 * current BTstack hci_transport_t interface.
 *
 * Reading: a dedicated thread drains the socket into a large ring buffer as soon as data
 * arrives; the BTstack thread parses whatever is there. The socket must never fill up:
 * the netgraph node doesn't slow the controller down, it drops data, and with H4 framing
 * one lost byte makes everything after it unreadable. Before, the BTstack thread read the
 * socket itself, one header or payload per wakeup. Whenever it was held up for a moment,
 * bytes were lost, the framing was gone, and every following byte was logged as an
 * "invalid packet type": thousands of log lines per second, a log that filled the iPod.
 *
 * Lost sync: headers are checked (ACL data for a known connection, sane HCI events).
 * After a loss the parser looks for the next packet that checks out and goes on from
 * there - a short gap in the audio instead of a broken link. Garbage is never handed to
 * BTstack. If that doesn't work, the failure handler restarts Bluetooth.
 *
 * Original: Copyright (C) 2009-2012 by Matthias Ringwald - BTstack license
 * (free for non-commercial use, see btstack/LICENSE).
 */

#include "btstack_config.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"

#include "btstack_control_ipod.h"
#include "hci_transport_h4_ipod.h"

#define SOCKET_DEVICE "com.apple.uart.bluetooth"
#ifndef PF_NETGRAPH
#define PF_NETGRAPH       32          /* normally in sys/socket.h */
#endif
#define NG_CONTROL        2           /* from FreeBSD sys/ng_socket.h */
#define SOCK_IOCTL_VAL    0xC0644E03  /* from reversing BTServer */

#ifdef __LP64__
#define GETSOCKOPT_VAL    0x40487413
#define SETSOCKOPT_VAL    0x80487414
#else
// iPod touch 5 is 32-bit (A5)
#define GETSOCKOPT_VAL    0x402C7413
#define SETSOCKOPT_VAL    0x802C7414
#endif

// keep BT_WAKE asserted for 3 s after the last outgoing packet
#define HCI_WAKE_TIMER_MS 3000
#define HCI_WAKE_DURATION_US 10000

#define RX_RING_SIZE         (1024 * 1024)  // power of two; ~20 s of A2DP audio
#define RX_SELECT_TIMEOUT_US 100000         // the reader thread notices close() this fast
#define SOCKET_RCVBUF_BYTES  (512 * 1024)   // second line of defence, if the kernel allows it
#define RESYNC_WINDOW        9              // H4 type + ACL header + L2CAP length + CID
#define RESYNC_GIVE_UP_BYTES (64 * 1024)    // ~1.5 s of streaming without one valid packet
#define LOSS_LIMIT           6              // this many losses of sync ...
#define LOSS_WINDOW_MS       60000          // ... within a minute: restart Bluetooth

typedef enum {
    H4_W4_PACKET_TYPE,
    H4_W4_EVENT_HEADER,
    H4_W4_ACL_HEADER,
    H4_W4_PAYLOAD,
    H4_RESYNC,                  // lost sync: looking for the start of a packet
} h4_state_t;

/* netgraph sockaddr */
struct sockaddr_ng {
    uint8_t  sg_len;
    uint8_t  sg_family;
    uint16_t sg_subtype;
    uint32_t sg_node;
    uint32_t sg_null;
    uint8_t  sg_dummy[20];
};

static const hci_transport_config_uart_t * uart_config;
static int uart_fd = -1;

static btstack_timer_source_t wake_timer;
static const char * enforce_wake_device;
static int enforce_wake_fd = -1;

static void dummy_handler(uint8_t packet_type, uint8_t *packet, uint16_t size){
    UNUSED(packet_type); UNUSED(packet); UNUSED(size);
}
static void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = dummy_handler;

static h4_state_t h4_state;
static int bytes_to_read;
static int read_pos;
static uint8_t hci_packet[1 + HCI_INCOMING_PACKET_BUFFER_SIZE];

// A packet followed by more bytes is held until the next header checks out: if bytes were
// lost inside it, the bytes after it don't form a header and it is dropped, instead of
// handing a corrupted packet to BTstack. A packet that ends where the data ends is
// delivered at once (nothing to wait for).
static uint8_t held_packet[1 + HCI_INCOMING_PACKET_BUFFER_SIZE];
static int     held_len;

// reader thread -> BTstack thread: single producer, single consumer
static uint8_t           rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_write;           // advanced by the reader thread
static volatile uint32_t rx_read;            // advanced by the BTstack thread
static volatile int      rx_running;
static volatile int      rx_error;           // reader stopped: errno, or -1 for end of stream
static pthread_t         rx_thread;
static int               rx_thread_started;
static int               rx_pipe[2] = { -1, -1 };
static btstack_data_source_t rx_pipe_source;

// lost sync
static uint8_t  resync_window[RESYNC_WINDOW];
static int      resync_fill;
static uint32_t resync_skipped;
static uint32_t loss_times[LOSS_LIMIT];
static uint32_t loss_count;
static int      link_failed;                 // reported: parsing stopped
static char     failure_reason[96];
static btstack_timer_source_t failure_timer;
static void (*failure_handler)(const char * reason);

static void h4_parse(const uint8_t * data, uint32_t len);

static void h4_reset_statemachine(void){
    h4_state = H4_W4_PACKET_TYPE;
    read_pos = 0;
    bytes_to_read = 1;
}

static void h4_enforce_wake_off(void){
    btstack_run_loop_remove_timer(&wake_timer);
    if (enforce_wake_fd >= 0){
        close(enforce_wake_fd);
        enforce_wake_fd = -1;
    }
}

static void h4_enforce_wake_timeout(btstack_timer_source_t *ts){
    UNUSED(ts);
    h4_enforce_wake_off();
}

static void h4_enforce_wake_on(void){
    if (!enforce_wake_device) return;
    if (enforce_wake_fd < 0){
        // opening /dev/btwake asserts BT_WAKE
        enforce_wake_fd = open(enforce_wake_device, O_RDWR);
        usleep(HCI_WAKE_DURATION_US);
    }
    btstack_run_loop_remove_timer(&wake_timer);
    btstack_run_loop_set_timer(&wake_timer, HCI_WAKE_TIMER_MS);
    btstack_run_loop_set_timer_handler(&wake_timer, &h4_enforce_wake_timeout);
    btstack_run_loop_add_timer(&wake_timer);
}

// --- failure: report once, from a timer (the handler restarts Bluetooth) ---

static void failure_timer_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (failure_handler) (*failure_handler)(failure_reason);
}

static void h4_report_failure(const char * reason){
    if (link_failed) return;
    link_failed = 1;
    btstack_strcpy(failure_reason, sizeof(failure_reason), reason);
    log_error("h4: %s", reason);
    btstack_run_loop_remove_timer(&failure_timer);
    btstack_run_loop_set_timer_handler(&failure_timer, &failure_timer_handler);
    btstack_run_loop_set_timer(&failure_timer, 0);
    btstack_run_loop_add_timer(&failure_timer);
}

// --- header checks ---

#define EVENT_VARIABLE  -1
#define EVENT_UNKNOWN   -2

// Parameter length of the HCI events a BR/EDR controller sends (Core spec 7.7): fixed,
// variable, or not an event at all
static int event_expected_length(uint8_t code){
    switch (code){
        case 0x01: return 1;    // Inquiry Complete
        case 0x02: return EVENT_VARIABLE;   // Inquiry Result
        case 0x03: return 11;   // Connection Complete
        case 0x04: return 10;   // Connection Request
        case 0x05: return 4;    // Disconnection Complete
        case 0x06: return 3;    // Authentication Complete
        case 0x07: return 255;  // Remote Name Request Complete
        case 0x08: return 4;    // Encryption Change
        case 0x09: return 3;    // Change Connection Link Key Complete
        case 0x0A: return 4;    // Master Link Key Complete
        case 0x0B: return 11;   // Read Remote Supported Features Complete
        case 0x0C: return 8;    // Read Remote Version Information Complete
        case 0x0D: return 21;   // QoS Setup Complete
        case 0x0E: return EVENT_VARIABLE;   // Command Complete
        case 0x0F: return 4;    // Command Status
        case 0x10: return 1;    // Hardware Error
        case 0x11: return 2;    // Flush Occurred
        case 0x12: return 8;    // Role Change
        case 0x13: return EVENT_VARIABLE;   // Number Of Completed Packets
        case 0x14: return 6;    // Mode Change
        case 0x15: return EVENT_VARIABLE;   // Return Link Keys
        case 0x16: return 6;    // PIN Code Request
        case 0x17: return 6;    // Link Key Request
        case 0x18: return 23;   // Link Key Notification
        case 0x1A: return 1;    // Data Buffer Overflow
        case 0x1B: return 3;    // Max Slots Change
        case 0x1C: return 5;    // Read Clock Offset Complete
        case 0x1D: return 5;    // Connection Packet Type Changed
        case 0x1E: return 2;    // QoS Violation
        case 0x20: return 7;    // Page Scan Repetition Mode Change
        case 0x21: return 22;   // Flow Specification Complete
        case 0x22: return EVENT_VARIABLE;   // Inquiry Result with RSSI
        case 0x23: return 13;   // Read Remote Extended Features Complete
        case 0x2C: return 17;   // Synchronous Connection Complete
        case 0x2D: return 9;    // Synchronous Connection Changed
        case 0x2E: return 11;   // Sniff Subrating
        case 0x2F: return 255;  // Extended Inquiry Result
        case 0x30: return 3;    // Encryption Key Refresh Complete
        case 0x31: return 6;    // IO Capability Request
        case 0x32: return 9;    // IO Capability Response
        case 0x33: return 10;   // User Confirmation Request
        case 0x34: return 6;    // User Passkey Request
        case 0x35: return 6;    // Remote OOB Data Request
        case 0x36: return 7;    // Simple Pairing Complete
        case 0x38: return 4;    // Link Supervision Timeout Changed
        case 0x39: return 2;    // Enhanced Flush Complete
        case 0x3B: return 10;   // User Passkey Notification
        case 0x3C: return 7;    // Keypress Notification
        case 0x3D: return 14;   // Remote Host Supported Features Notification
        case 0x3E: return EVENT_VARIABLE;   // LE Meta
        case 0xFF: return EVENT_VARIABLE;   // vendor specific
        default:   return EVENT_UNKNOWN;
    }
}

static int event_header_plausible(uint8_t code, uint8_t len){
    int expected = event_expected_length(code);
    if (expected == EVENT_UNKNOWN) return 0;
    return expected == EVENT_VARIABLE || len == expected;
}

static int opcode_group_valid(uint8_t opcode_high_byte);

// the whole event is in: check the variable-length ones against their own counts
// (e = event code, length, parameters)
static int event_complete_plausible(const uint8_t * e){
    uint8_t len = e[1];
    const uint8_t * p = &e[2];
    switch (e[0]){
        case 0x02:   // Inquiry Result
        case 0x22:   // Inquiry Result with RSSI
            return len >= 1 && len == 1 + 14 * p[0];
        case 0x15:   // Return Link Keys
            return len >= 1 && len == 1 + 22 * p[0];
        case 0x13:   // Number Of Completed Packets
            return len >= 5 && p[0] >= 1 && len == 1 + 4 * p[0];
        case 0x0E:   // Command Complete: credits, opcode (0 = no command), return parameters
            return len >= 3 && ((p[1] == 0 && p[2] == 0) || opcode_group_valid(p[2]));
        case 0x0F:   // Command Status: status, credits, opcode
            return (p[2] == 0 && p[3] == 0) || opcode_group_valid(p[3]);
        default:
            return 1;
    }
}

// ACL data for a connection BTstack knows; strict: only the start of an L2CAP packet
static int acl_header_plausible(const uint8_t * header, int strict){
    uint16_t handle_and_flags = little_endian_read_16(header, 0);
    uint16_t length           = little_endian_read_16(header, 2);
    if ((handle_and_flags >> 14) != 0) return 0;          // broadcast flags: never used here
    if (length > HCI_ACL_PAYLOAD_SIZE) return 0;
    if (hci_connection_for_handle(handle_and_flags & 0x0fff) == NULL) return 0;
    if (strict){
        uint8_t pb = (handle_and_flags >> 12) & 0x03;
        if (pb != 0x00 && pb != 0x02) return 0;
    }
    return 1;
}

static int opcode_group_valid(uint8_t opcode_high_byte){
    uint8_t ogf = opcode_high_byte >> 2;
    return (ogf >= 0x01 && ogf <= 0x08) || ogf == 0x3F;
}

// Do the first n bytes of w start a packet? 1 = yes, 0 = no, -1 = need more bytes to tell.
// Strict, so random audio bytes practically never pass. Decided within RESYNC_WINDOW bytes,
// and for events within their own (short) length, so a lone short event is found too.
static int resync_check(const uint8_t * w, int n){
    if (w[0] == HCI_ACL_DATA_PACKET){
        if (n < RESYNC_WINDOW) return -1;
        if (!acl_header_plausible(&w[1], 1)) return 0;
        uint16_t acl_length   = little_endian_read_16(w, 3);
        uint16_t l2cap_length = little_endian_read_16(w, 5);
        uint16_t cid          = little_endian_read_16(w, 7);
        return (l2cap_length + 4u == acl_length) && (cid == 0x0001 || cid >= 0x0040);
    }
    if (w[0] == HCI_EVENT_PACKET){
        if (n < 3) return -1;
        uint8_t len = w[2];
        switch (w[1]){
            case 0x13:   // Number Of Completed Packets, for a known connection
                if (n < 6) return -1;
                if (w[3] < 1 || w[3] > 4 || len != 1 + 4 * w[3]) return 0;
                return hci_connection_for_handle(little_endian_read_16(w, 4) & 0x0fff) != NULL;
            case 0x0F:   // Command Status
                if (n < 7) return -1;
                return len == 4 && w[4] == 1 && opcode_group_valid(w[6]);
            case 0x0E:   // Command Complete
                if (n < 6) return -1;
                return len >= 4 && w[3] == 1 && opcode_group_valid(w[5]);
            case 0x05:   // Disconnection Complete, for a known connection
                if (n < 6) return -1;
                return len == 4 && hci_connection_for_handle(little_endian_read_16(w, 4) & 0x0fff) != NULL;
            default: {
                // other events with a fixed length that matches (the next header must
                // check out too before anything reaches BTstack)
                int expected = event_expected_length(w[1]);
                return expected >= 0 && len == expected;
            }
        }
    }
    return 0;
}

// --- parser (BTstack thread) ---

static void h4_lost_sync(const char * what, uint8_t value){
    uint32_t now = btstack_run_loop_get_time_ms();
    loss_times[loss_count % LOSS_LIMIT] = now ? now : 1;
    loss_count++;
    unsigned int i, recent = 0;
    for (i = 0; i < LOSS_LIMIT; i++){
        if (loss_times[i] && now - loss_times[i] < LOSS_WINDOW_MS) recent++;
    }
    log_error("h4: lost sync with the Bluetooth controller (%s 0x%02x)%s, looking for the next packet",
              what, value, held_len ? ", dropping the packet before it" : "");
    held_len       = 0;        // bytes were lost in it, or it isn't a packet at all
    h4_state       = H4_RESYNC;
    read_pos       = 0;
    resync_fill    = 0;
    resync_skipped = 0;
    if (recent >= LOSS_LIMIT) h4_report_failure("lost sync with the Bluetooth controller too often");
}

// the next header checked out: the held packet was complete and real
static void h4_deliver_held(void){
    if (!held_len) return;
    int n = held_len;
    held_len = 0;
    packet_handler(held_packet[0], &held_packet[1], (uint16_t) (n - 1));
}

// a packet is complete; more = bytes that follow it right now
static void h4_packet_complete(uint32_t more){
    if (read_pos >= 3){
        if (more){
            memcpy(held_packet, hci_packet, (size_t) read_pos);
            held_len = read_pos;
        } else {
            packet_handler(hci_packet[0], &hci_packet[1], (uint16_t) (read_pos - 1));
        }
    }
    h4_reset_statemachine();
}

static void h4_statemachine(uint32_t more){
    switch (h4_state){
        case H4_W4_PACKET_TYPE:
            switch (hci_packet[0]){
                case HCI_EVENT_PACKET:
                    bytes_to_read = HCI_EVENT_HEADER_SIZE;
                    h4_state = H4_W4_EVENT_HEADER;
                    return;
                case HCI_ACL_DATA_PACKET:
                    bytes_to_read = HCI_ACL_HEADER_SIZE;
                    h4_state = H4_W4_ACL_HEADER;
                    return;
                default:
                    // SCO is never set up: anything else follows lost bytes
                    h4_lost_sync("packet type", hci_packet[0]);
                    return;
            }
        case H4_W4_EVENT_HEADER:
            if (!event_header_plausible(hci_packet[1], hci_packet[2])){
                h4_lost_sync("event", hci_packet[1]);
                return;
            }
            bytes_to_read = hci_packet[2];
            h4_state = H4_W4_PAYLOAD;
            if (bytes_to_read) return;
            break;                                    // no parameters: complete
        case H4_W4_ACL_HEADER:
            if (!acl_header_plausible(&hci_packet[1], 0)){
                h4_lost_sync("ACL header", hci_packet[1]);
                return;
            }
            // ACL data for a known connection follows: the held packet was real
            h4_deliver_held();
            if (uart_fd < 0) return;                  // closed while it was handled
            bytes_to_read = little_endian_read_16(hci_packet, 3);
            h4_state = H4_W4_PAYLOAD;
            if (bytes_to_read) return;
            break;                                    // no payload: complete
        case H4_W4_PAYLOAD:
            break;
        default:
            return;
    }
    // packet complete. An event only vouches for the held packet once all of it checks out.
    if (hci_packet[0] == HCI_EVENT_PACKET){
        if (!event_complete_plausible(&hci_packet[1])){
            h4_lost_sync("event", hci_packet[1]);
            return;
        }
        h4_deliver_held();
        if (uart_fd < 0) return;
    }
    h4_packet_complete(more);
}

// lost sync: slide over the data until the bytes start a valid packet
static uint32_t h4_resync(const uint8_t * data, uint32_t len){
    uint32_t used = 0;
    while (used < len){
        resync_window[resync_fill++] = data[used++];
        while (resync_fill > 0){
            int verdict = resync_check(resync_window, resync_fill);
            if (verdict < 0 && resync_fill < RESYNC_WINDOW){
                // can't tell yet from the first byte - but a later one may already start a
                // packet that checks out (a short event that is the last thing sent, e.g. the
                // reply to a command: waiting for more bytes would wait forever)
                int k;
                for (k = 1; k < resync_fill; k++){
                    if (resync_check(&resync_window[k], resync_fill - k) > 0) break;
                }
                if (k == resync_fill) break;                           // need more bytes
                resync_skipped += (uint32_t) k;
                resync_fill -= k;
                memmove(resync_window, resync_window + k, (size_t) resync_fill);
                continue;
            }
            if (verdict > 0){
                uint8_t window[RESYNC_WINDOW];
                int n = resync_fill;
                memcpy(window, resync_window, (size_t) n);
                resync_fill = 0;
                log_info("h4: in sync again after skipping %u bytes", resync_skipped);
                h4_reset_statemachine();
                h4_parse(window, (uint32_t) n);
                return used;
            }
            // not the start of a packet: drop the first byte, try from the next one
            resync_fill--;
            memmove(resync_window, resync_window + 1, (size_t) resync_fill);
            resync_skipped++;
        }
    }
    if (resync_skipped > RESYNC_GIVE_UP_BYTES){
        h4_report_failure("no valid data from the Bluetooth controller any more");
    }
    return used;
}

static void h4_parse(const uint8_t * data, uint32_t len){
    while (len > 0 && uart_fd >= 0 && !link_failed){
        if (h4_state == H4_RESYNC){
            uint32_t used = h4_resync(data, len);
            data += used;
            len  -= used;
            continue;
        }
        uint32_t n = (uint32_t) bytes_to_read;
        if (n > len) n = len;
        memcpy(&hci_packet[read_pos], data, n);
        read_pos      += (int) n;
        bytes_to_read -= (int) n;
        data += n;
        len  -= n;
        if (bytes_to_read == 0) h4_statemachine(len);
    }
}

// everything the reader thread has buffered
static void h4_drain(void){
    while (uart_fd >= 0 && !link_failed){
        uint32_t r = rx_read;
        uint32_t w = __atomic_load_n(&rx_write, __ATOMIC_ACQUIRE);
        if (r == w) break;
        uint32_t pos = r & (RX_RING_SIZE - 1);
        uint32_t len = w - r;
        if (len > RX_RING_SIZE - pos) len = RX_RING_SIZE - pos;
        h4_parse(&rx_ring[pos], len);
        if (uart_fd < 0) return;            // closed while a packet was being handled
        __atomic_store_n(&rx_read, r + len, __ATOMIC_RELEASE);
    }
    int err = __atomic_load_n(&rx_error, __ATOMIC_ACQUIRE);
    if (err && uart_fd >= 0 && !link_failed){
        char reason[96];
        if (err > 0) snprintf(reason, sizeof(reason), "reading from the Bluetooth controller failed: %s", strerror(err));
        else         snprintf(reason, sizeof(reason), "the Bluetooth controller connection closed");
        h4_report_failure(reason);
    }
}

static void rx_pipe_handler(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(type);
    char buf[64];
    while (read(ds->source.fd, buf, sizeof(buf)) > 0) {}
    h4_drain();
}

// --- reader thread ---

static void rx_wake(void){
    char c = 0;
    ssize_t n = write(rx_pipe[1], &c, 1);    // non-blocking: a full pipe means a wake is pending
    (void) n;
}

static void * rx_thread_main(void * context){
    int fd = (int) (intptr_t) context;
    // drain the UART promptly whatever else the iPod is doing
    typedef int (*set_qos_class_t)(unsigned int qos_class, int relative_priority);
    set_qos_class_t set_qos = (set_qos_class_t) dlsym(RTLD_DEFAULT, "pthread_set_qos_class_self_np");  // iOS 8+
    if (set_qos) set_qos(0x21 /* QOS_CLASS_USER_INTERACTIVE */, 0);

    while (__atomic_load_n(&rx_running, __ATOMIC_ACQUIRE)){
        uint32_t w = rx_write;
        uint32_t space = RX_RING_SIZE - (w - __atomic_load_n(&rx_read, __ATOMIC_ACQUIRE));
        if (space == 0){
            usleep(2000);       // BTstack thread far behind: the socket buffer holds the rest meanwhile
            continue;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { 0, RX_SELECT_TIMEOUT_US };
        int ready = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0 && errno != EINTR){
            __atomic_store_n(&rx_error, errno, __ATOMIC_RELEASE);
            rx_wake();
            break;
        }
        if (ready <= 0) continue;
        uint32_t pos = w & (RX_RING_SIZE - 1);
        uint32_t chunk = RX_RING_SIZE - pos;
        if (chunk > space) chunk = space;
        ssize_t n = read(fd, &rx_ring[pos], chunk);
        if (n > 0){
            __atomic_store_n(&rx_write, w + (uint32_t) n, __ATOMIC_RELEASE);
            rx_wake();
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        __atomic_store_n(&rx_error, n < 0 ? errno : -1, __ATOMIC_RELEASE);
        rx_wake();
        break;
    }
    return NULL;
}

static int setup_rx_pipe(void){
    if (rx_pipe[0] >= 0) return 0;
    if (pipe(rx_pipe) != 0){
        log_error("h4_open: pipe() failed: %s", strerror(errno));
        return -1;
    }
    fcntl(rx_pipe[0], F_SETFL, fcntl(rx_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(rx_pipe[1], F_SETFL, fcntl(rx_pipe[1], F_GETFL) | O_NONBLOCK);
    btstack_run_loop_set_data_source_fd(&rx_pipe_source, rx_pipe[0]);
    btstack_run_loop_set_data_source_handler(&rx_pipe_source, &rx_pipe_handler);
    btstack_run_loop_enable_data_source_callbacks(&rx_pipe_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&rx_pipe_source);
    return 0;
}

// --- hci_transport_t ---

static void h4_init(const void * transport_config){
    if (!transport_config || ((const hci_transport_config_t*)transport_config)->type != HCI_TRANSPORT_CONFIG_UART){
        log_error("h4_ipod: need hci_transport_config_uart_t");
        return;
    }
    uart_config = (const hci_transport_config_uart_t *) transport_config;
}

static int h4_open(void){
    if (setup_rx_pipe()) return -1;

    int fd = socket(PF_NETGRAPH, SOCK_STREAM, NG_CONTROL);
    if (fd < 0){
        log_error("h4_open: socket(PF_NETGRAPH) failed: %s", strerror(errno));
        return -1;
    }

    // look up the netgraph node of the Bluetooth UART
    struct {
        uint32_t result;
        char socket_name[96];
    } ioctl_arg;
    memset(&ioctl_arg, 0, sizeof(ioctl_arg));
    strcpy(ioctl_arg.socket_name, SOCKET_DEVICE);
    if (ioctl(fd, SOCK_IOCTL_VAL, &ioctl_arg) != 0){
        log_error("h4_open: ioctl(%s) failed: %s", SOCKET_DEVICE, strerror(errno));
        goto fail;
    }

    struct sockaddr_ng sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sg_len     = sizeof(struct sockaddr_ng);
    sock_addr.sg_family  = PF_NETGRAPH;
    sock_addr.sg_subtype = 0x02;
    sock_addr.sg_node    = ioctl_arg.result;
    if (connect(fd, (const struct sockaddr *) &sock_addr, sizeof(sock_addr)) != 0){
        log_error("h4_open: connect failed: %s", strerror(errno));
        goto fail;
    }

    // configure UART through the netgraph node
    struct termios toptions;
    socklen_t toptions_len = sizeof(toptions);
    if (getsockopt(fd, SO_ACCEPTCONN, GETSOCKOPT_VAL, &toptions, &toptions_len) != 0){
        log_error("h4_open: getsockopt failed: %s", strerror(errno));
        goto fail;
    }

    // 0 = use the native speed BlueTool configured (from IORegistry "transport-speed")
    uint32_t baud = uart_config ? uart_config->baudrate_init : 0;
    if (baud == 0){
        baud = btstack_control_ipod_get_transport_speed();
    }
    log_info("h4_open: UART speed %u", baud);

    cfmakeraw(&toptions);
    cfsetspeed(&toptions, (speed_t) baud);
    toptions.c_iflag |= IGNPAR;
    toptions.c_cflag  = 0x00038b00;   // value used by BTServer (8N1 + RTS/CTS)
    if (setsockopt(fd, SO_ACCEPTCONN, SETSOCKOPT_VAL, &toptions, toptions_len) != 0){
        log_error("h4_open: setsockopt failed: %s", strerror(errno));
        goto fail;
    }

    // more room in the kernel too, if it lets us (the reader thread does the real work)
    int rcvbuf = SOCKET_RCVBUF_BYTES;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0){
        log_info("h4_open: socket buffer not enlarged: %s", strerror(errno));
    }
    int actual = 0;
    socklen_t actual_len = sizeof(actual);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &actual_len) == 0){
        log_info("h4_open: socket buffer %d bytes", actual);
    }

    h4_reset_statemachine();
    held_len = 0;
    resync_fill = 0;
    resync_skipped = 0;
    link_failed = 0;
    rx_read = 0;
    rx_write = 0;
    rx_error = 0;
    uart_fd = fd;
    __atomic_store_n(&rx_running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&rx_thread, NULL, &rx_thread_main, (void *) (intptr_t) fd) != 0){
        log_error("h4_open: cannot start the reader thread");
        __atomic_store_n(&rx_running, 0, __ATOMIC_RELEASE);
        uart_fd = -1;
        goto fail;
    }
    rx_thread_started = 1;
    log_info("h4_open: connected to %s", SOCKET_DEVICE);
    return 0;

fail:
    close(fd);
    return -1;
}

static int h4_close(void){
    if (rx_thread_started){
        __atomic_store_n(&rx_running, 0, __ATOMIC_RELEASE);
        pthread_join(rx_thread, NULL);       // within RX_SELECT_TIMEOUT_US
        rx_thread_started = 0;
    }
    if (uart_fd >= 0){
        close(uart_fd);
        uart_fd = -1;
    }
    btstack_run_loop_remove_timer(&failure_timer);
    h4_enforce_wake_off();
    return 0;
}

static void h4_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)){
    packet_handler = handler;
}

static int h4_write_all(const uint8_t * data, int size){
    while (size > 0){
        ssize_t n = write(uart_fd, data, size);
        if (n < 0){
            if (errno == EINTR || errno == EAGAIN){
                usleep(1000);
                continue;
            }
            log_error("h4: write failed: %s", strerror(errno));
            return -1;
        }
        data += n;
        size -= (int) n;
    }
    return 0;
}

// synchronous: the packet buffer can be reused when this returns
static int h4_send_packet(uint8_t packet_type, uint8_t * packet, int size){
    if (uart_fd < 0) return -1;
    h4_enforce_wake_on();
    if (h4_write_all(&packet_type, 1)) return -1;
    return h4_write_all(packet, size);
}

static const hci_transport_t h4_ipod_transport = {
    .name                    = "H4_IPOD_NETGRAPH",
    .init                    = &h4_init,
    .open                    = &h4_open,
    .close                   = &h4_close,
    .register_packet_handler = &h4_register_packet_handler,
    .can_send_packet_now     = NULL,
    .send_packet             = &h4_send_packet,
    .set_baudrate            = NULL,
    .reset_link              = NULL,
    .set_sco_config          = NULL,
};

const hci_transport_t * hci_transport_h4_ipod_instance(void){
    return &h4_ipod_transport;
}

void hci_transport_h4_ipod_set_enforce_wake_device(const char * path){
    enforce_wake_device = path;
}

void hci_transport_h4_ipod_set_failure_handler(void (*handler)(const char * reason)){
    failure_handler = handler;
}
