#ifndef SINK_H
#define SINK_H

#include "btstack_audio.h"

// audio output; call first, before Bluetooth (the music service runs without it)
void sink_init_audio(const btstack_audio_sink_t * audio);

// register A2DP sink / AVRCP services and SDP records; call before hci_power_control(ON)
void sink_setup(const char * local_name);

// start reconnecting to the phone used last time (call once HCI is working);
// keeps retrying after link loss, see sink.c
void sink_reconnect_last_device(void);

void sink_set_auto_reconnect(int enabled);   // default on
void sink_forget_last_device(void);         // after deleting pairings
void sink_set_auto_play(int enabled);       // send AVRCP Play once after connecting (default off)
void sink_set_media_controls(int enabled);  // lock screen + car/USB transport buttons (default on)
void sink_media_prepare(void);              // call once Bluetooth is up: starts media controls

// Keep the receiver the iPod's playing music app all the time (silence + a stand-in track
// while no phone audio plays), also while Bluetooth is off. Default off.
void sink_set_music_service(int enabled);

// external power (charger, car) and battery level (percent, -1 = unknown). Without
// external power and with the battery low, the music service pauses itself.
void sink_set_power_state(int on_external_power, int battery_percent);

void sink_disconnect(void);
void sink_shutdown_audio(void);

#endif
