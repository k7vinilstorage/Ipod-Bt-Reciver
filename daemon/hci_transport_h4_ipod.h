#ifndef HCI_TRANSPORT_H4_IPOD_H
#define HCI_TRANSPORT_H4_IPOD_H

#include "hci_transport.h"

const hci_transport_t * hci_transport_h4_ipod_instance(void);

// path that wakes the controller while open, "/dev/btwake" on iOS
void hci_transport_h4_ipod_set_enforce_wake_device(const char * path);

// called (once, from a timer) when the link to the controller can't be used any more:
// reading failed, or sync with the controller was lost and could not be found again
void hci_transport_h4_ipod_set_failure_handler(void (*handler)(const char * reason));

#endif
