#ifndef HCI_TRANSPORT_H4_IPOD_H
#define HCI_TRANSPORT_H4_IPOD_H

#include "hci_transport.h"

const hci_transport_t * hci_transport_h4_ipod_instance(void);

// path that wakes the controller while open, "/dev/btwake" on iOS
void hci_transport_h4_ipod_set_enforce_wake_device(const char * path);

#endif
