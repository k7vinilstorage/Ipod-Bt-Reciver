/*
 * btstack_audio_ios.c
 *
 * btstack_audio_sink_t on top of AudioQueue (iOS 6 - 9).
 *
 * Threading: BTstack is single threaded (posix run loop on the main thread). AudioQueue
 * calls us on its own thread. A timer on the BTstack run loop pulls PCM from the A2DP
 * code via the playback callback and pushes it into a single-producer/single-consumer
 * ring buffer; the AudioQueue callback only copies from that ring. Same model as
 * BTstack's PortAudio driver.
 */

#include "btstack_config.h"

#include <string.h>
#include <AudioToolbox/AudioToolbox.h>

#include "btstack_audio.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"

#include "btstack_audio_ios.h"

#define MAX_CHANNELS          2
#define FRAMES_PER_CHUNK      256      // what we ask the A2DP code for at a time
#define FRAMES_PER_AQ_BUFFER  1024     // ~23 ms at 44.1 kHz
#define NUM_AQ_BUFFERS        3
#define RING_FRAMES           4096     // power of two, ~93 ms at 44.1 kHz
#define RING_TARGET_FRAMES    (FRAMES_PER_AQ_BUFFER * 2 + FRAMES_PER_CHUNK)
#define FILL_TIMER_MS         5

static void (*playback_callback)(int16_t * buffer, uint16_t num_samples, const btstack_audio_context_t * context);

static AudioQueueRef       queue;
static AudioQueueBufferRef aq_buffers[NUM_AQ_BUFFERS];
static uint32_t            sample_rate;
static uint8_t             num_channels;
static volatile int        stream_running;
static AudioStreamBasicDescription stream_format;
static float               queue_volume = 1.0f;

static btstack_timer_source_t fill_timer;

// SPSC ring: written on BTstack thread, read on AudioQueue thread
static int16_t           ring[RING_FRAMES * MAX_CHANNELS];
static volatile uint32_t ring_write;   // frames written (monotonic)
static volatile uint32_t ring_read;    // frames read (monotonic)

static uint32_t load_acquire(volatile uint32_t * p){ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store_release(volatile uint32_t * p, uint32_t v){ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static uint32_t ring_fill(void){
    return load_acquire(&ring_write) - load_acquire(&ring_read);
}

// AudioQueue thread
static void aq_output_callback(void * user_data, AudioQueueRef aq, AudioQueueBufferRef buffer){
    (void) user_data;
    if (!__atomic_load_n(&stream_running, __ATOMIC_ACQUIRE)) return;   // queue is being torn down
    int16_t * out = (int16_t *) buffer->mAudioData;
    uint32_t frames_wanted = FRAMES_PER_AQ_BUFFER;
    uint32_t read  = ring_read;
    uint32_t avail = load_acquire(&ring_write) - read;
    uint32_t frames = btstack_min(avail, frames_wanted);
    uint32_t i;
    for (i = 0; i < frames; i++){
        uint32_t pos = ((read + i) & (RING_FRAMES - 1)) * num_channels;
        out[i * num_channels] = ring[pos];
        if (num_channels > 1) out[i * num_channels + 1] = ring[pos + 1];
    }
    store_release(&ring_read, read + frames);
    if (frames < frames_wanted){
        // underrun: pad with silence
        memset(&out[frames * num_channels], 0, (frames_wanted - frames) * num_channels * sizeof(int16_t));
    }
    buffer->mAudioDataByteSize = frames_wanted * num_channels * sizeof(int16_t);
    AudioQueueEnqueueBuffer(aq, buffer, 0, NULL);
}

// BTstack thread
static void fill_ring(void){
    if (!playback_callback) return;
    int16_t chunk[FRAMES_PER_CHUNK * MAX_CHANNELS];
    btstack_audio_context_t context;
    memset(&context, 0, sizeof(context));
    while (ring_fill() + FRAMES_PER_CHUNK <= RING_TARGET_FRAMES){
        (*playback_callback)(chunk, FRAMES_PER_CHUNK, &context);
        uint32_t write = ring_write;
        uint32_t i;
        for (i = 0; i < FRAMES_PER_CHUNK; i++){
            uint32_t pos = ((write + i) & (RING_FRAMES - 1)) * num_channels;
            ring[pos] = chunk[i * num_channels];
            if (num_channels > 1) ring[pos + 1] = chunk[i * num_channels + 1];
        }
        store_release(&ring_write, write + FRAMES_PER_CHUNK);
    }
}

static void fill_timer_handler(btstack_timer_source_t * ts){
    if (!stream_running) return;
    fill_ring();
    btstack_run_loop_set_timer(ts, FILL_TIMER_MS);
    btstack_run_loop_add_timer(ts);
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
    // NULL run loop: callbacks on an AudioQueue-internal thread
    OSStatus err = AudioQueueNewOutput(&stream_format, &aq_output_callback, NULL, NULL, NULL, 0, &queue);
    if (err){
        log_error("AudioQueueNewOutput failed: %d", (int) err);
        queue = NULL;
        return -1;
    }
    int i;
    for (i = 0; i < NUM_AQ_BUFFERS; i++){
        err = AudioQueueAllocateBuffer(queue, FRAMES_PER_AQ_BUFFER * stream_format.mBytesPerFrame, &aq_buffers[i]);
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

    memset(&stream_format, 0, sizeof(stream_format));
    stream_format.mSampleRate       = samplerate;
    stream_format.mFormatID         = kAudioFormatLinearPCM;
    stream_format.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    stream_format.mChannelsPerFrame = channels;
    stream_format.mBitsPerChannel   = 16;
    stream_format.mBytesPerFrame    = 2 * channels;
    stream_format.mFramesPerPacket  = 1;
    stream_format.mBytesPerPacket   = stream_format.mBytesPerFrame;

    log_info("audio: configured %u Hz, %u channels", samplerate, channels);
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

static void ios_audio_start_stream(void){
    if (stream_running || !playback_callback) return;
    if (!queue && create_queue()) return;

    // prime ring + all AudioQueue buffers, then start
    store_release(&ring_read, 0);
    store_release(&ring_write, 0);
    __atomic_store_n(&stream_running, 1, __ATOMIC_RELEASE);
    fill_ring();
    int i;
    for (i = 0; i < NUM_AQ_BUFFERS; i++){
        aq_output_callback(NULL, queue, aq_buffers[i]);
        fill_ring();
    }
    OSStatus err = AudioQueueStart(queue, NULL);
    if (err){
        log_error("AudioQueueStart failed: %d", (int) err);
    }

    btstack_run_loop_set_timer_handler(&fill_timer, &fill_timer_handler);
    btstack_run_loop_set_timer(&fill_timer, FILL_TIMER_MS);
    btstack_run_loop_add_timer(&fill_timer);
}

static void ios_audio_stop_stream(void){
    if (!stream_running) return;
    __atomic_store_n(&stream_running, 0, __ATOMIC_RELEASE);
    btstack_run_loop_remove_timer(&fill_timer);
    // tear the queue down; start_stream builds a fresh one
    close_queue();
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
