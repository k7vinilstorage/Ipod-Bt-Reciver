#ifndef BTSTACK_CONTROL_IPOD_H
#define BTSTACK_CONTROL_IPOD_H

#include <stdint.h>
#include "btstack_control.h"

const btstack_control_t * btstack_control_ipod_instance(void);

// UART speed reported by the IORegistry (valid after power on)
uint32_t btstack_control_ipod_get_transport_speed(void);

// device has a Broadcom controller this code knows how to drive
int btstack_control_ipod_is_supported(void);
const char * btstack_control_ipod_machine(void);

// restart Apple's Bluetooth daemons if we stopped them (safe to call more than once)
void btstack_control_ipod_restore_system_bluetooth(void);

#endif
