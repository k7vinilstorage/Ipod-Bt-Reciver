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
// (e.g. pause the Music app) and returns 1 to go ahead, or 0 to leave the audio to the other
// app (the stream is then stopped, start_stream starts it again). after_reclaim runs once
// our queue plays again (e.g. re-assert Now Playing). Either may be NULL.
void     btstack_audio_ios_set_reclaim_handlers(int (*before_reclaim)(void), void (*after_reclaim)(void));

// Number of times the audio was taken back from another app since the stream started.
uint32_t btstack_audio_ios_get_reclaims(void);

// While enabled the queue plays silence instead of waiting for audio, so it keeps running
// (phone paused, music service). The silence needs nothing from the BTstack thread.
// Switching it off lets queued audio follow the silence without a gap.
void     btstack_audio_ios_set_silence_fill(int enabled);

// The queue is built and playing (or starting).
int      btstack_audio_ios_is_running(void);

// start_stream was called and the stream wasn't stopped since (it may be recovering
// from an interruption, i.e. not running right now).
int      btstack_audio_ios_is_started(void);

#endif
