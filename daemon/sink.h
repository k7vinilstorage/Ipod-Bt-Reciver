#ifndef SINK_H
#define SINK_H

#include "btstack_audio.h"

// register A2DP sink / AVRCP services and SDP records; call before hci_power_control(ON)
void sink_setup(const char * local_name, const btstack_audio_sink_t * audio);

// try to connect back to the phone used last time (call once HCI is working)
void sink_reconnect_last_device(void);

void sink_disconnect(void);
void sink_shutdown_audio(void);

#endif
