/*
 * hci_transport_h4_ipod.c
 *
 * H4 transport to the built-in Broadcom Bluetooth controller of iOS devices.
 *
 * iOS does not expose the Bluetooth UART as a tty. Since iOS 4, BTServer talks to it
 * through a netgraph socket node called "com.apple.uart.bluetooth". The magic ioctl /
 * sockopt values below were reverse engineered from BTServer by Matthias Ringwald for
 * "BTstack for iOS" (bluekitchen/btstack, port/ios/src/hci_transport_h4_iphone.c),
 * which shipped for iOS 9. This file is that implementation, updated to the current
 * BTstack hci_transport_t interface.
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
#include <sys/ioctl.h>
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

typedef enum {
    H4_W4_PACKET_TYPE,
    H4_W4_EVENT_HEADER,
    H4_W4_ACL_HEADER,
    H4_W4_SCO_HEADER,
    H4_W4_PAYLOAD,
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
static btstack_data_source_t h4_data_source;
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

static void h4_deliver_packet(void){
    if (read_pos < 3) return;
    packet_handler(hci_packet[0], &hci_packet[1], read_pos - 1);
    h4_reset_statemachine();
}

static void h4_statemachine(void){
    switch (h4_state){
        case H4_W4_PACKET_TYPE:
            switch (hci_packet[0]){
                case HCI_EVENT_PACKET:
                    bytes_to_read = HCI_EVENT_HEADER_SIZE;
                    h4_state = H4_W4_EVENT_HEADER;
                    break;
                case HCI_ACL_DATA_PACKET:
                    bytes_to_read = HCI_ACL_HEADER_SIZE;
                    h4_state = H4_W4_ACL_HEADER;
                    break;
                case HCI_SCO_DATA_PACKET:
                    bytes_to_read = HCI_SCO_HEADER_SIZE;
                    h4_state = H4_W4_SCO_HEADER;
                    break;
                default:
                    log_error("h4: invalid packet type 0x%02x", hci_packet[0]);
                    h4_reset_statemachine();
                    break;
            }
            break;
        case H4_W4_EVENT_HEADER:
            bytes_to_read = hci_packet[2];
            h4_state = H4_W4_PAYLOAD;
            break;
        case H4_W4_ACL_HEADER:
            bytes_to_read = little_endian_read_16(hci_packet, 3);
            if (bytes_to_read > HCI_ACL_PAYLOAD_SIZE){
                log_error("h4: ACL payload too large (%d), resync", bytes_to_read);
                h4_reset_statemachine();
                return;
            }
            h4_state = H4_W4_PAYLOAD;
            break;
        case H4_W4_SCO_HEADER:
            bytes_to_read = hci_packet[3];
            h4_state = H4_W4_PAYLOAD;
            break;
        case H4_W4_PAYLOAD:
            h4_deliver_packet();
            return;
        default:
            break;
    }
    // zero-length payload
    if (h4_state == H4_W4_PAYLOAD && bytes_to_read == 0){
        h4_deliver_packet();
    }
}

static void h4_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback_type){
    UNUSED(ds); UNUSED(callback_type);
    if (uart_fd < 0) return;
    ssize_t bytes_read = read(uart_fd, &hci_packet[read_pos], bytes_to_read);
    if (bytes_read <= 0) return;
    bytes_to_read -= (int) bytes_read;
    read_pos      += (int) bytes_read;
    if (bytes_to_read > 0) return;
    h4_statemachine();
}

static void h4_init(const void * transport_config){
    if (!transport_config || ((const hci_transport_config_t*)transport_config)->type != HCI_TRANSPORT_CONFIG_UART){
        log_error("h4_ipod: need hci_transport_config_uart_t");
        return;
    }
    uart_config = (const hci_transport_config_uart_t *) transport_config;
}

static int h4_open(void){
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

    uart_fd = fd;
    btstack_run_loop_set_data_source_fd(&h4_data_source, fd);
    btstack_run_loop_set_data_source_handler(&h4_data_source, &h4_process);
    btstack_run_loop_enable_data_source_callbacks(&h4_data_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&h4_data_source);

    h4_reset_statemachine();
    log_info("h4_open: connected to %s", SOCKET_DEVICE);
    return 0;

fail:
    close(fd);
    return -1;
}

static int h4_close(void){
    if (uart_fd >= 0){
        btstack_run_loop_remove_data_source(&h4_data_source);
        close(uart_fd);
        uart_fd = -1;
    }
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
