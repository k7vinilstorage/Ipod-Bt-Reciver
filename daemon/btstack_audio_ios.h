#ifndef BTSTACK_AUDIO_IOS_H
#define BTSTACK_AUDIO_IOS_H

#include <stdint.h>
#include "btstack_audio.h"

// Output buffering held by the AudioQueue: 6 x 2048 frames (~280 ms at 44.1 kHz).
// Large enough for the 4096-frame render cycles iOS uses while the screen is locked.
#define BTSTACK_AUDIO_IOS_BUFFER_FRAMES  2048
#define BTSTACK_AUDIO_IOS_NUM_BUFFERS    6

const btstack_audio_sink_t * btstack_audio_ios_sink_get_instance(void);

// Optional: how many frames the playback callback can deliver right now without running
// dry. Buffers are only filled when that covers a whole buffer.
void     btstack_audio_ios_set_available_handler(uint32_t (*handler)(void));

// New audio was buffered: fill AudioQueue buffers that are waiting for data.
void     btstack_audio_ios_kick(void);

// Frames handed to the AudioQueue and not played yet.
uint32_t btstack_audio_ios_get_queued_frames(void);

// Number of times the AudioQueue ran dry since the stream started.
uint32_t btstack_audio_ios_get_underruns(void);

// Another app (typically the Music app, started by a car head unit) interrupted our audio
// session. The driver takes the audio back on the BTstack thread: before_reclaim runs first
// (e.g. pause the Music app), after_reclaim once our queue plays again (e.g. re-assert
// Now Playing). Either may be NULL.
void     btstack_audio_ios_set_reclaim_handlers(void (*before_reclaim)(void), void (*after_reclaim)(void));

// Number of times the audio was taken back from another app since the stream started.
uint32_t btstack_audio_ios_get_reclaims(void);

#endif
