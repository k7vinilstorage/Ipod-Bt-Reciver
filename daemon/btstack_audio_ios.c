/*
 * btstack_audio_ios.c
 *
 * btstack_audio_sink_t on top of AudioQueue (iOS 6 - 9).
 *
 * Buffering: the AudioQueue itself holds the output buffer, NUM_BUFFERS x BUFFER_FRAMES
 * (6 x 2048 frames, ~280 ms at 44.1 kHz). With the screen locked iOS saves power by
 * rendering in 4096-frame cycles (~93 ms); the previous 4 x 1024 queue could not cover a
 * single such cycle, so every cycle ran dry, incoming audio piled up and was dropped.
 *
 * Threading: buffers are filled (SBC decode + resample through the playback callback) and
 * enqueued on the BTstack thread. The AudioQueue callback runs on Core Audio's real-time
 * thread; it only hands the played buffer back through a lock-free queue and wakes the
 * BTstack thread with a byte on a pipe. No polling timer: a buffer is refilled as soon as
 * it is free, whatever iOS does to timers while the screen is off.
 *
 * A buffer is only filled when enough audio is buffered (available handler). After a
 * Bluetooth stall the queue runs dry once, refills a few buffers' worth and resumes
 * cleanly, instead of crackling with half-empty buffers.
 *
 * Interruptions: if another app takes the audio (a car head unit telling the Music app to
 * play, for example), iOS interrupts our session and stops the queue. An IsRunning
 * property listener notices, and the BTstack thread takes the audio back: pause the other
 * player (handler), re-activate the session, rebuild the queue from the buffered audio.
 */

#include "btstack_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>

#include "btstack_audio.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"

#include "btstack_audio_ios.h"

#define MAX_CHANNELS        2
#define NUM_BUFFERS         BTSTACK_AUDIO_IOS_NUM_BUFFERS
#define BUFFER_FRAMES       BTSTACK_AUDIO_IOS_BUFFER_FRAMES
#define FREE_QUEUE_SIZE     8       // power of two, > NUM_BUFFERS
#define AVAILABLE_MARGIN    256     // resampling can need a few more input frames
#define REBUFFER_BUFFERS    3       // after running dry, wait for this much audio
#define FALLBACK_TIMER_MS   40      // belt and braces; the pipe does the real work
#define RECLAIM_DELAY_MS    250     // let the other app's start settle first
#define RECLAIM_MAX_DELAY_MS 5000   // backoff limit while we keep losing the audio
#define RECLAIM_QUIET_MS    10000   // this long without an interruption resets the backoff

static void (*playback_callback)(int16_t * buffer, uint16_t num_samples, const btstack_audio_context_t * context);
static uint32_t (*available_handler)(void);

static AudioQueueRef       queue;
static AudioQueueBufferRef aq_buffers[NUM_BUFFERS];
static AudioStreamBasicDescription stream_format;
static uint32_t            sample_rate;
static uint8_t             num_channels;
static float               queue_volume = 1.0f;
static volatile int        stream_running;   // queue built and started (or starting)
static int                 stream_wanted;    // the sink wants audio: keep reclaiming until stop

// played buffers: single producer (AudioQueue thread), single consumer (BTstack thread)
static AudioQueueBufferRef free_queue[FREE_QUEUE_SIZE];
static volatile uint32_t   free_write;
static volatile uint32_t   free_read;
static AudioQueueBufferRef held_buffer;      // taken from free_queue, waiting for audio
static volatile int32_t    frames_queued;    // handed to the AudioQueue, not played yet
static int                 primed;           // at least one buffer was played
static int                 rebuffering;
static uint32_t            underruns;
static int                 silence_fill;     // keep-alive: play silence instead of waiting

static int                    wake_pipe[2] = { -1, -1 };
static btstack_data_source_t  wake_data_source;
static btstack_timer_source_t fallback_timer;

// interruption recovery
static volatile int           interrupted;    // set on the AudioQueue thread
static btstack_timer_source_t reclaim_timer;
static unsigned int           reclaim_streak; // consecutive interruptions -> backoff
static uint32_t               last_reclaim_ms;
static uint32_t               reclaims;
static void (*before_reclaim_handler)(void);
static void (*after_reclaim_handler)(void);

static AudioQueueBufferRef free_queue_pop(void){
    uint32_t r = free_read;
    if (r == __atomic_load_n(&free_write, __ATOMIC_ACQUIRE)) return NULL;
    AudioQueueBufferRef b = free_queue[r & (FREE_QUEUE_SIZE - 1)];
    __atomic_store_n(&free_read, r + 1, __ATOMIC_RELEASE);
    return b;
}

// AudioQueue (real-time) thread: no locks, no logging, no allocation
static void aq_output_callback(void * user_data, AudioQueueRef aq, AudioQueueBufferRef buffer){
    (void) user_data;
    (void) aq;
    if (!__atomic_load_n(&stream_running, __ATOMIC_ACQUIRE)) return;
    int32_t frames = (int32_t) (buffer->mAudioDataByteSize / stream_format.mBytesPerFrame);
    __atomic_sub_fetch(&frames_queued, frames, __ATOMIC_ACQ_REL);
    uint32_t w = free_write;
    free_queue[w & (FREE_QUEUE_SIZE - 1)] = buffer;
    __atomic_store_n(&free_write, w + 1, __ATOMIC_RELEASE);
    char c = 0;
    ssize_t n = write(wake_pipe[1], &c, 1);    // non-blocking: a full pipe means a wake is pending
    (void) n;
}

// AudioQueue thread: the queue stopped without us stopping it -> interrupted by another app
static void is_running_listener(void * user_data, AudioQueueRef aq, AudioQueuePropertyID property){
    (void) user_data;
    (void) property;
    if (!__atomic_load_n(&stream_running, __ATOMIC_ACQUIRE)) return;   // we stopped it ourselves
    UInt32 running = 1;
    UInt32 size = sizeof(running);
    if (AudioQueueGetProperty(aq, kAudioQueueProperty_IsRunning, &running, &size) != 0) return;
    if (running) return;
    __atomic_store_n(&interrupted, 1, __ATOMIC_RELEASE);
    char c = 0;
    ssize_t n = write(wake_pipe[1], &c, 1);
    (void) n;
}

// BTstack thread
static void reclaim_timer_handler(btstack_timer_source_t * ts);

static void schedule_reclaim(void){
    uint32_t now = btstack_run_loop_get_time_ms();
    if (now - last_reclaim_ms > RECLAIM_QUIET_MS) reclaim_streak = 0;
    uint32_t delay = RECLAIM_DELAY_MS << btstack_min(reclaim_streak, 5);
    if (delay > RECLAIM_MAX_DELAY_MS) delay = RECLAIM_MAX_DELAY_MS;
    btstack_run_loop_remove_timer(&reclaim_timer);
    btstack_run_loop_set_timer_handler(&reclaim_timer, &reclaim_timer_handler);
    btstack_run_loop_set_timer(&reclaim_timer, delay);
    btstack_run_loop_add_timer(&reclaim_timer);
}

static int enqueue(AudioQueueBufferRef b){
    btstack_audio_context_t context;
    memset(&context, 0, sizeof(context));
    (*playback_callback)((int16_t *) b->mAudioData, BUFFER_FRAMES, &context);
    b->mAudioDataByteSize = BUFFER_FRAMES * stream_format.mBytesPerFrame;
    __atomic_add_fetch(&frames_queued, BUFFER_FRAMES, __ATOMIC_ACQ_REL);
    OSStatus err = AudioQueueEnqueueBuffer(queue, b, 0, NULL);
    if (err){
        __atomic_sub_fetch(&frames_queued, BUFFER_FRAMES, __ATOMIC_ACQ_REL);
        log_error("AudioQueueEnqueueBuffer failed: %d", (int) err);
        return 0;
    }
    primed = 1;
    return 1;
}

static int enqueue_silence(AudioQueueBufferRef b){
    memset(b->mAudioData, 0, BUFFER_FRAMES * stream_format.mBytesPerFrame);
    b->mAudioDataByteSize = BUFFER_FRAMES * stream_format.mBytesPerFrame;
    __atomic_add_fetch(&frames_queued, BUFFER_FRAMES, __ATOMIC_ACQ_REL);
    OSStatus err = AudioQueueEnqueueBuffer(queue, b, 0, NULL);
    if (err){
        __atomic_sub_fetch(&frames_queued, BUFFER_FRAMES, __ATOMIC_ACQ_REL);
        log_error("AudioQueueEnqueueBuffer failed: %d", (int) err);
        return 0;
    }
    primed = 1;
    return 1;
}

static void fill_free_buffers(void){
    if (!__atomic_load_n(&stream_running, __ATOMIC_ACQUIRE) || !queue || !playback_callback) return;
    while (1){
        if (!held_buffer) held_buffer = free_queue_pop();
        if (!held_buffer) return;
        if (silence_fill){
            // phone paused: keep the queue playing (silently) so we stay the active player
            if (!enqueue_silence(held_buffer)) return;
            held_buffer = NULL;
            rebuffering = 0;
            continue;
        }
        if (primed && !rebuffering && __atomic_load_n(&frames_queued, __ATOMIC_ACQUIRE) <= 0){
            rebuffering = 1;    // queue ran dry: build up some audio before resuming
            underruns++;
        }
        if (available_handler){
            uint32_t needed = BUFFER_FRAMES + AVAILABLE_MARGIN;
            if (rebuffering) needed = REBUFFER_BUFFERS * BUFFER_FRAMES + AVAILABLE_MARGIN;
            if ((*available_handler)() < needed) return;    // wait for more Bluetooth data
        }
        rebuffering = 0;
        if (!enqueue(held_buffer)) return;
        held_buffer = NULL;
    }
}

static void wake_handler(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    (void) type;
    char buf[16];
    while (read(ds->source.fd, buf, sizeof(buf)) > 0) {}
    if (__atomic_exchange_n(&interrupted, 0, __ATOMIC_ACQ_REL)){
        log_info("audio: another app took the audio (AudioQueue stopped)");
        schedule_reclaim();
        return;
    }
    fill_free_buffers();
}

static void fallback_timer_handler(btstack_timer_source_t * ts){
    if (!__atomic_load_n(&stream_running, __ATOMIC_ACQUIRE)) return;
    fill_free_buffers();
    btstack_run_loop_set_timer(ts, FALLBACK_TIMER_MS);
    btstack_run_loop_add_timer(ts);
}

static int setup_wake_pipe(void){
    if (wake_pipe[0] >= 0) return 0;
    if (pipe(wake_pipe) != 0){
        log_error("audio: pipe() failed: %s", strerror(errno));
        return -1;
    }
    fcntl(wake_pipe[0], F_SETFL, fcntl(wake_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, fcntl(wake_pipe[1], F_GETFL) | O_NONBLOCK);
    btstack_run_loop_set_data_source_fd(&wake_data_source, wake_pipe[0]);
    btstack_run_loop_set_data_source_handler(&wake_data_source, &wake_handler);
    btstack_run_loop_enable_data_source_callbacks(&wake_data_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&wake_data_source);
    return 0;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void setup_audio_session(void){
    // C AudioSession API: works from a daemon, unlike AVAudioSession. Playback category keeps
    // audio going with the screen locked and ignores the ring/silent switch.
    static int session_ready;
    if (session_ready) return;
    OSStatus err = AudioSessionInitialize(NULL, NULL, NULL, NULL);
    if (err && err != kAudioSessionAlreadyInitialized){
        log_error("AudioSessionInitialize failed: %d", (int) err);
    }
    UInt32 category = kAudioSessionCategory_MediaPlayback;
    err = AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof(category), &category);
    if (err) log_error("AudioSession set category failed: %d", (int) err);
    err = AudioSessionSetActive(true);
    if (err) log_error("AudioSessionSetActive failed: %d", (int) err);
    session_ready = 1;
}
#pragma clang diagnostic pop

static void close_queue(void){
    if (!queue) return;
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
    queue = NULL;
}

static int create_queue(void){
    // NULL run loop: callbacks on an AudioQueue-internal (real-time) thread
    OSStatus err = AudioQueueNewOutput(&stream_format, &aq_output_callback, NULL, NULL, NULL, 0, &queue);
    if (err){
        log_error("AudioQueueNewOutput failed: %d", (int) err);
        queue = NULL;
        return -1;
    }
    err = AudioQueueAddPropertyListener(queue, kAudioQueueProperty_IsRunning, &is_running_listener, NULL);
    if (err) log_error("audio: cannot watch for interruptions: %d", (int) err);
    int i;
    for (i = 0; i < NUM_BUFFERS; i++){
        err = AudioQueueAllocateBuffer(queue, BUFFER_FRAMES * stream_format.mBytesPerFrame, &aq_buffers[i]);
        if (err){
            log_error("AudioQueueAllocateBuffer failed: %d", (int) err);
            close_queue();
            return -1;
        }
    }
    AudioQueueSetParameter(queue, kAudioQueueParam_Volume, queue_volume);
    return 0;
}

static int ios_audio_init(uint8_t channels, uint32_t samplerate,
                          void (*playback)(int16_t * buffer, uint16_t num_samples, const btstack_audio_context_t * context)){
    if (channels < 1 || channels > MAX_CHANNELS) return -1;

    close_queue();
    playback_callback = playback;
    num_channels = channels;
    sample_rate = samplerate;

    setup_audio_session();
    if (setup_wake_pipe()) return -1;

    memset(&stream_format, 0, sizeof(stream_format));
    stream_format.mSampleRate       = samplerate;
    stream_format.mFormatID         = kAudioFormatLinearPCM;
    stream_format.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    stream_format.mChannelsPerFrame = channels;
    stream_format.mBitsPerChannel   = 16;
    stream_format.mBytesPerFrame    = 2 * channels;
    stream_format.mFramesPerPacket  = 1;
    stream_format.mBytesPerPacket   = stream_format.mBytesPerFrame;

    log_info("audio: configured %u Hz, %u channels, %u x %u frames queued",
             samplerate, channels, NUM_BUFFERS, BUFFER_FRAMES);
    return 0;
}

static uint32_t ios_audio_get_samplerate(void){
    return sample_rate;
}

static void ios_audio_set_volume(uint8_t volume){
    // AVRCP absolute volume 0..127, squared for a more natural curve
    float v = (float) btstack_min(volume, 127) / 127.0f;
    queue_volume = v * v;
    if (queue) AudioQueueSetParameter(queue, kAudioQueueParam_Volume, queue_volume);
}

static OSStatus start_queue(void){
    if (!queue && create_queue()) return -1;

    // every buffer starts out free
    unsigned int i;
    for (i = 0; i < NUM_BUFFERS; i++) free_queue[i] = aq_buffers[i];
    free_read = 0;
    __atomic_store_n(&free_write, NUM_BUFFERS, __ATOMIC_RELEASE);
    __atomic_store_n(&frames_queued, 0, __ATOMIC_RELEASE);
    held_buffer = NULL;
    primed      = 0;
    rebuffering = 0;
    underruns   = 0;
    __atomic_store_n(&stream_running, 1, __ATOMIC_RELEASE);

    // prime as many buffers as there is audio for (the sink waits for ~all of them)
    fill_free_buffers();
    if (!primed){
        // not even one buffer's worth: start with what there is rather than not at all
        AudioQueueBufferRef b = held_buffer ? held_buffer : free_queue_pop();
        if (b && enqueue(b) && b == held_buffer) held_buffer = NULL;
    }

    OSStatus err = AudioQueueStart(queue, NULL);
    if (err) log_error("AudioQueueStart failed: %d", (int) err);

    btstack_run_loop_set_timer_handler(&fallback_timer, &fallback_timer_handler);
    btstack_run_loop_set_timer(&fallback_timer, FALLBACK_TIMER_MS);
    btstack_run_loop_add_timer(&fallback_timer);
    return err;
}

static void ios_audio_start_stream(void){
    if (stream_running || !playback_callback) return;
    stream_wanted = 1;
    reclaims = 0;
    reclaim_streak = 0;
    start_queue();
}

static void tear_down_queue(void){
    __atomic_store_n(&stream_running, 0, __ATOMIC_RELEASE);
    btstack_run_loop_remove_timer(&fallback_timer);
    close_queue();
    held_buffer = NULL;
    __atomic_store_n(&frames_queued, 0, __ATOMIC_RELEASE);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void reclaim_timer_handler(btstack_timer_source_t * ts){
    (void) ts;
    if (!stream_wanted) return;   // stream ended meanwhile
    last_reclaim_ms = btstack_run_loop_get_time_ms();
    reclaim_streak++;

    if (before_reclaim_handler) (*before_reclaim_handler)();      // e.g. pause the Music app

    OSStatus err = AudioSessionSetActive(true);
    if (err) log_error("audio: AudioSessionSetActive failed: %d", (int) err);

    // rebuild the queue from the buffered audio (an interrupted queue can be left in odd states)
    tear_down_queue();
    err = start_queue();
    if (err){
        log_error("audio: could not take the audio back (%d), retrying", (int) err);
        schedule_reclaim();
        return;
    }
    reclaims++;
    log_info("audio: took the audio back (%u time%s this stream)", reclaims, reclaims == 1 ? "" : "s");
    if (after_reclaim_handler) (*after_reclaim_handler)();         // e.g. re-assert Now Playing
}
#pragma clang diagnostic pop

static void ios_audio_stop_stream(void){
    stream_wanted = 0;
    silence_fill = 0;
    btstack_run_loop_remove_timer(&reclaim_timer);
    __atomic_store_n(&interrupted, 0, __ATOMIC_RELEASE);
    if (!stream_running) return;
    // tear the queue down; start_stream builds a fresh one
    tear_down_queue();
}

static void ios_audio_close(void){
    ios_audio_stop_stream();
    close_queue();
    playback_callback = NULL;
}

static const btstack_audio_sink_t btstack_audio_ios_sink = {
    .init           = &ios_audio_init,
    .get_samplerate = &ios_audio_get_samplerate,
    .set_volume     = &ios_audio_set_volume,
    .start_stream   = &ios_audio_start_stream,
    .stop_stream    = &ios_audio_stop_stream,
    .close          = &ios_audio_close,
};

const btstack_audio_sink_t * btstack_audio_ios_sink_get_instance(void){
    return &btstack_audio_ios_sink;
}

void btstack_audio_ios_set_available_handler(uint32_t (*handler)(void)){
    available_handler = handler;
}

void btstack_audio_ios_kick(void){
    fill_free_buffers();
}

uint32_t btstack_audio_ios_get_queued_frames(void){
    int32_t q = __atomic_load_n(&frames_queued, __ATOMIC_ACQUIRE);
    return q > 0 ? (uint32_t) q : 0;
}

uint32_t btstack_audio_ios_get_underruns(void){
    return underruns;
}

void btstack_audio_ios_set_reclaim_handlers(void (*before_reclaim)(void), void (*after_reclaim)(void)){
    before_reclaim_handler = before_reclaim;
    after_reclaim_handler  = after_reclaim;
}

uint32_t btstack_audio_ios_get_reclaims(void){
    return reclaims;
}

void btstack_audio_ios_set_silence_fill(int enabled){
    silence_fill = enabled ? 1 : 0;
    if (silence_fill) fill_free_buffers();
}

int btstack_audio_ios_is_running(void){
    return __atomic_load_n(&stream_running, __ATOMIC_ACQUIRE);
}
