#ifndef SINK_H
#define SINK_H

#include "btstack_audio.h"

// register A2DP sink / AVRCP services and SDP records; call before hci_power_control(ON)
void sink_setup(const char * local_name, const btstack_audio_sink_t * audio);

// start reconnecting to the phone used last time (call once HCI is working);
// keeps retrying after link loss, see sink.c
void sink_reconnect_last_device(void);

void sink_set_auto_reconnect(int enabled);   // default on
void sink_forget_last_device(void);         // after deleting pairings
void sink_set_auto_play(int enabled);       // send AVRCP Play once after connecting (default off)

void sink_disconnect(void);
void sink_shutdown_audio(void);

#endif
