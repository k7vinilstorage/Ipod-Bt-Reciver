/*
 * sink.c - A2DP Sink + AVRCP Controller/Target
 *
 * Adapted from BTstack's example/a2dp_sink_demo.c (BlueKitchen GmbH, BTstack license),
 * trimmed to what a headless receiver needs, plus status reporting for the app.
 *
 * Media path: L2CAP media packets -> SBC frames into a ring buffer -> decoded on demand
 * when the audio driver asks for PCM -> resampled slightly faster/slower to keep the
 * buffer level stable (phone and iPod clocks drift) -> AudioQueue.
 */

#include "btstack_config.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "btstack.h"
#include "btstack_resample.h"
#include "btstack_ring_buffer.h"

#include "btstack_audio_ios.h"
#include "media_controls.h"
#include "sink.h"
#include "sink_status.h"

#define NUM_CHANNELS                   2
#define BYTES_PER_FRAME                (2 * NUM_CHANNELS)
#define MAX_SBC_FRAME_SIZE             120
#define SBC_MAX_AUDIO_FRAMES_PER_BLOCK 128
#define RESAMPLE_ADDITIONAL_FRAMES     16
#define RESAMPLE_OUTPUT_FRAMES         (SBC_MAX_AUDIO_FRAMES_PER_BLOCK + RESAMPLE_ADDITIONAL_FRAMES)

// Buffering: the AudioQueue holds ~280 ms (6 x 2048 frames, btstack_audio_ios.h). The SBC
// ring in front of it absorbs Bluetooth bursts and stalls. A tiny resampling correction keeps
// the *total* buffered audio (AudioQueue + SBC) near TARGET_FRAMES, so clock drift between the
// phone and the iPod neither drains nor overfills the buffers.
#define AQ_FRAMES           (BTSTACK_AUDIO_IOS_NUM_BUFFERS * BTSTACK_AUDIO_IOS_BUFFER_FRAMES)
#define START_FRAMES        (AQ_FRAMES + 1024)        // buffered before playback starts (~300 ms)
#define TARGET_FRAMES       (AQ_FRAMES + 2048)        // steady-state total (~325 ms)
#define BAND_FRAMES         3072                      // within +-70 ms of target: no correction
#define TRIM_ABOVE_FRAMES   (TARGET_FRAMES + 22050)   // >0.5 s too much: skip ahead once
#define SBC_STORAGE_FRAMES  512                       // ~1.5 s of SBC
#define STATS_INTERVAL_MS   5000
#define STATS_HEALTH_TICKS  6                         // health line every 30 s

static btstack_packet_callback_registration_t hci_event_callback_registration;

static uint8_t sdp_avdtp_sink_service_buffer[150];
static uint8_t sdp_avrcp_target_service_buffer[150];
static uint8_t sdp_avrcp_controller_service_buffer[200];
static uint8_t device_id_sdp_service_buffer[100];

// accept all SBC configurations, bitpool 2-53
static uint8_t media_sbc_codec_capabilities[] = { 0xFF, 0xFF, 2, 53 };

static const btstack_sbc_decoder_t *   sbc_decoder_instance;
static btstack_sbc_decoder_bluedroid_t sbc_decoder_context;

static uint8_t sbc_frame_storage[SBC_STORAGE_FRAMES * MAX_SBC_FRAME_SIZE];
static btstack_ring_buffer_t sbc_frame_ring_buffer;
static unsigned int sbc_frame_size;
static unsigned int pcm_frames_per_sbc_frame = 128;
static int32_t      total_avg;              // smoothed total buffered frames
static uint32_t     stat_dropped_sbc;
static uint32_t     stat_trims;
static uint32_t     stat_underruns_seen;
static uint32_t     stat_reclaims_seen;
static unsigned int stat_ticks;
static btstack_timer_source_t stats_timer;

static uint8_t decoded_audio_storage[RESAMPLE_OUTPUT_FRAMES * BYTES_PER_FRAME];
static btstack_ring_buffer_t decoded_audio_ring_buffer;

static int media_initialized;
static int audio_stream_started;
static btstack_resample_t resample_instance;
static uint32_t resampling_min_factor;

static int16_t * request_buffer;
static int       request_frames;

static int volume_percentage = 100;

typedef struct {
    uint8_t  reconfigure;
    uint8_t  num_channels;
    uint16_t sampling_frequency;
    uint8_t  block_length;
    uint8_t  subbands;
    uint8_t  min_bitpool_value;
    uint8_t  max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

static struct {
    uint8_t  a2dp_local_seid;
    uint8_t  media_sbc_codec_configuration[4];
} stream_endpoint;

static struct {
    bd_addr_t addr;
    uint16_t  a2dp_cid;
    int       connected;
    int       playing;
    media_codec_configuration_sbc_t sbc_configuration;
} a2dp_connection;

static struct {
    uint16_t avrcp_cid;
} avrcp_connection;

static const btstack_audio_sink_t * audio_sink;

// --- auto reconnect ---
//
// Reconnect to the last phone when the receiver starts and after the link was lost
// (out of range, phone Bluetooth toggled, ...): every 10 s for 2 minutes, then every
// 60 s until it comes back. Not after the user disconnected on the phone on purpose.

#define RECONNECT_FIRST_DELAY_MS     1000
#define RECONNECT_AFTER_LOSS_MS      3000
#define RECONNECT_FAST_INTERVAL_MS   10000
#define RECONNECT_FAST_ATTEMPTS      12
#define RECONNECT_SLOW_INTERVAL_MS   60000
#define CONNECT_ATTEMPT_TIMEOUT_MS   30000
#define AVRCP_CONNECT_DELAY_MS       2000

static int       auto_reconnect = 1;
static int       reconnect_blocked;          // user disconnected on the phone
static int       reconnect_attempts;
static int       connect_attempt_pending;
static uint32_t  connect_attempt_started_ms;
static btstack_timer_source_t reconnect_timer;
static btstack_timer_source_t avrcp_connect_timer;
static hci_con_handle_t a2dp_con_handle = HCI_CON_HANDLE_INVALID;

static int       have_last_device;
static bd_addr_t last_device_addr;
static char      last_device_name[64];

static void save_last_device(void){
    FILE * f = fopen(SINK_LAST_DEVICE, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n", bd_addr_to_str(last_device_addr), last_device_name);
    fclose(f);
}

static void load_last_device(void){
    char line[80];
    have_last_device = 0;
    last_device_name[0] = 0;
    FILE * f = fopen(SINK_LAST_DEVICE, "r");
    if (!f) return;
    if (fgets(line, sizeof(line), f)){
        line[strcspn(line, "\r\n")] = 0;
        have_last_device = sscanf_bd_addr(line, last_device_addr);
    }
    if (fgets(line, sizeof(line), f)){
        line[strcspn(line, "\r\n")] = 0;
        btstack_strcpy(last_device_name, sizeof(last_device_name), line);
    }
    fclose(f);
}

static void remember_device(const bd_addr_t addr, const char * name){
    if (!have_last_device || bd_addr_cmp(addr, last_device_addr) != 0){
        last_device_name[0] = 0;        // different phone: forget old name
    }
    memcpy(last_device_addr, addr, 6);
    have_last_device = 1;
    if (name && name[0]) btstack_strcpy(last_device_name, sizeof(last_device_name), name);
    save_last_device();
}

static int reconnect_possible(void){
    return auto_reconnect && !reconnect_blocked && have_last_device && !a2dp_connection.connected;
}

static void reconnect_timer_handler(btstack_timer_source_t * ts);

static void schedule_reconnect(uint32_t delay_ms){
    btstack_run_loop_remove_timer(&reconnect_timer);
    if (!reconnect_possible()){
        sink_status_set_reconnecting(NULL);
        return;
    }
    btstack_run_loop_set_timer_handler(&reconnect_timer, &reconnect_timer_handler);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_add_timer(&reconnect_timer);
}

static void reconnect_timer_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (!reconnect_possible()){
        sink_status_set_reconnecting(NULL);
        return;
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    if (connect_attempt_pending && (now - connect_attempt_started_ms) > CONNECT_ATTEMPT_TIMEOUT_MS){
        log_info("reconnect: attempt timed out");
        connect_attempt_pending = 0;
    }
    if (!connect_attempt_pending){
        uint16_t cid;
        uint8_t status = a2dp_sink_establish_stream(last_device_addr, &cid);
        reconnect_attempts++;
        log_info("reconnect: attempt %d to %s -> status 0x%02x", reconnect_attempts, bd_addr_to_str(last_device_addr), status);
        if (status == ERROR_CODE_SUCCESS){
            connect_attempt_pending = 1;
            connect_attempt_started_ms = now;
        }
        sink_status_set_reconnecting(last_device_name[0] ? last_device_name : bd_addr_to_str(last_device_addr));
    }
    schedule_reconnect(reconnect_attempts < RECONNECT_FAST_ATTEMPTS ? RECONNECT_FAST_INTERVAL_MS : RECONNECT_SLOW_INTERVAL_MS);
}

// if the phone didn't open AVRCP itself (typical when we initiated), do it for volume / track info
static void avrcp_connect_timer_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (!a2dp_connection.connected || avrcp_connection.avrcp_cid) return;
    uint16_t cid;
    uint8_t status = avrcp_connect(a2dp_connection.addr, &cid);
    log_info("AVRCP: connecting to %s -> status 0x%02x", bd_addr_to_str(a2dp_connection.addr), status);
}

// --- auto play ---
//
// Once per connection: shortly after AVRCP is up (and A2DP is connected), send "Play"
// unless the phone is already playing. The delay gives the phone time to report its
// playback status, so we don't toggle a player that just started by itself.

#define AUTOPLAY_DELAY_MS        2500
#define AUTOPLAY_RETRY_MS        1000
#define AUTOPLAY_MAX_RETRIES     10     // wait up to ~10 s for A2DP when AVRCP came first

static int auto_play;
static int autoplay_done;
static int autoplay_retries;
static int remote_playing;             // from AVRCP playback status notifications

// --- media controls / Now Playing (lock screen + car/USB transport buttons) ---
//
// Experimental and isolated: MediaPlayer is only touched after Bluetooth is up, and a
// guard file is written around each risky first call. If the daemon dies there, the
// next start finds the guard and leaves media controls off, so the receiver still
// works (the app shows it and can retry by toggling the setting).

static void sink_media_command(media_command_t cmd);

static int  media_controls_enabled = 1;
static int  media_hci_ready;          // Bluetooth is up, safe to start media controls
static int  media_state;              // 0 = not started, 1 = ready, -1 = unavailable / failed
static int  media_activated_once;
static char cur_title[128];
static char cur_artist[128];

static int media_is_playing(void){
    return remote_playing || a2dp_connection.playing;
}

static void media_guard_set(const char * phase){
    FILE * f = fopen(SINK_MEDIA_GUARD, "w");
    if (!f) return;
    fprintf(f, "%s\n", phase);
    fclose(f);   // in the kernel now: survives the process crashing
}

static void media_guard_clear(void){
    unlink(SINK_MEDIA_GUARD);
}

static int media_ensure_ready(void){
    if (media_state) return media_state > 0;
    if (access(SINK_MEDIA_GUARD, F_OK) == 0){
        log_error("media controls: the last start crashed in media code, keeping them off");
        media_state = -1;
        sink_status_set_media("failed");
        return 0;
    }
    media_guard_set("init");
    int ok = media_controls_init(&sink_media_command);
    media_guard_clear();
    media_state = ok ? 1 : -1;
    sink_status_set_media(ok ? "ready" : "unavailable");
    return ok;
}

static void media_apply_active(void){
    if (!media_hci_ready) return;
    if (!media_controls_enabled){
        if (media_state > 0) media_controls_set_active(0);
        sink_status_set_media("off");
        return;
    }
    if (!media_ensure_ready()) return;
    int want = a2dp_connection.connected;
    if (want && !media_activated_once){
        media_guard_set("activate");
        media_controls_set_active(1);
        media_controls_set_metadata(cur_title, cur_artist, media_is_playing());
        media_guard_clear();
        media_activated_once = 1;
    } else {
        media_controls_set_active(want);
    }
    sink_status_set_media(want ? "active" : "ready");
}

static void media_refresh_metadata(void){
    if (media_state > 0 && media_controls_enabled && a2dp_connection.connected){
        media_controls_set_metadata(cur_title, cur_artist, media_is_playing());
    }
}

// Phones briefly report "not playing" around track changes. Publishing that makes some head
// units decide the iPod stopped and tell the Music app to play, so only a pause that lasts
// is published; "playing" goes out immediately.
#define NOT_PLAYING_DEBOUNCE_MS 1500
static btstack_timer_source_t playing_debounce_timer;

static void playing_debounce_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (media_state > 0 && media_controls_enabled && a2dp_connection.connected){
        media_controls_set_playing(media_is_playing());
    }
}

static void media_refresh_playing(void){
    if (!(media_state > 0 && media_controls_enabled && a2dp_connection.connected)) return;
    btstack_run_loop_remove_timer(&playing_debounce_timer);
    if (media_is_playing()){
        media_controls_set_playing(1);
        return;
    }
    btstack_run_loop_set_timer_handler(&playing_debounce_timer, &playing_debounce_handler);
    btstack_run_loop_set_timer(&playing_debounce_timer, NOT_PLAYING_DEBOUNCE_MS);
    btstack_run_loop_add_timer(&playing_debounce_timer);
}

// remote command from the lock screen / car head unit -> forward to the phone
static void sink_media_command(media_command_t cmd){
    uint16_t cid = avrcp_connection.avrcp_cid;
    if (cid == 0){
        log_info("media command %d ignored: no AVRCP connection", (int) cmd);
        return;
    }
    switch (cmd){
        case MEDIA_CMD_PLAY:     avrcp_controller_play(cid); break;
        case MEDIA_CMD_PAUSE:    avrcp_controller_pause(cid); break;
        case MEDIA_CMD_TOGGLE:   if (remote_playing) avrcp_controller_pause(cid); else avrcp_controller_play(cid); break;
        case MEDIA_CMD_NEXT:     avrcp_controller_forward(cid); break;
        case MEDIA_CMD_PREVIOUS: avrcp_controller_backward(cid); break;
        case MEDIA_CMD_STOP:     avrcp_controller_stop(cid); break;
        default: break;
    }
}
static btstack_timer_source_t autoplay_timer;

static void autoplay_timer_handler(btstack_timer_source_t * ts){
    if (!auto_play || autoplay_done || !avrcp_connection.avrcp_cid) return;
    if (!a2dp_connection.connected){
        if (++autoplay_retries > AUTOPLAY_MAX_RETRIES) return;
        btstack_run_loop_set_timer(ts, AUTOPLAY_RETRY_MS);
        btstack_run_loop_add_timer(ts);
        return;
    }
    autoplay_done = 1;
    if (remote_playing || a2dp_connection.playing){
        log_info("auto play: phone is already playing");
        return;
    }
    uint8_t status = avrcp_controller_play(avrcp_connection.avrcp_cid);
    log_info("auto play: sending Play -> status 0x%02x", status);
}

static void autoplay_start(void){
    btstack_run_loop_remove_timer(&autoplay_timer);
    autoplay_done = 0;
    autoplay_retries = 0;
    remote_playing = 0;
    if (!auto_play) return;
    btstack_run_loop_set_timer_handler(&autoplay_timer, &autoplay_timer_handler);
    btstack_run_loop_set_timer(&autoplay_timer, AUTOPLAY_DELAY_MS);
    btstack_run_loop_add_timer(&autoplay_timer);
}

// --- audio pipeline ---

static void playback_handler(int16_t * buffer, uint16_t num_audio_frames, const btstack_audio_context_t * context){
    UNUSED(context);

    if (sbc_frame_size == 0){
        memset(buffer, 0, num_audio_frames * BYTES_PER_FRAME);
        return;
    }

    // first from resampled leftovers
    uint32_t bytes_read;
    btstack_ring_buffer_read(&decoded_audio_ring_buffer, (uint8_t *) buffer, num_audio_frames * BYTES_PER_FRAME, &bytes_read);
    buffer           += bytes_read / NUM_CHANNELS;
    num_audio_frames -= bytes_read / BYTES_PER_FRAME;

    // then decode SBC frames straight into the request buffer
    request_buffer = buffer;
    request_frames = num_audio_frames;
    while (request_frames && btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) >= sbc_frame_size){
        uint8_t sbc_frame[MAX_SBC_FRAME_SIZE];
        btstack_ring_buffer_read(&sbc_frame_ring_buffer, sbc_frame, sbc_frame_size, &bytes_read);
        sbc_decoder_instance->decode_signed_16(&sbc_decoder_context, 0, sbc_frame, sbc_frame_size);
    }

    // buffer ran dry: fill rest with silence
    if (request_frames > 0){
        memset(request_buffer, 0, request_frames * BYTES_PER_FRAME);
        request_frames = 0;
    }
}

static void handle_pcm_data(int16_t * data, int num_audio_frames, int num_channels, int sample_rate, void * context){
    UNUSED(sample_rate); UNUSED(context); UNUSED(num_channels);

    int16_t  output_buffer[RESAMPLE_OUTPUT_FRAMES * NUM_CHANNELS];
    uint32_t resampled_frames = btstack_resample_block(&resample_instance, data, num_audio_frames, output_buffer);

    int frames_to_copy = btstack_min(resampled_frames, request_frames);
    memcpy(request_buffer, output_buffer, frames_to_copy * BYTES_PER_FRAME);
    request_frames -= frames_to_copy;
    request_buffer += frames_to_copy * NUM_CHANNELS;

    int frames_to_store = resampled_frames - frames_to_copy;
    if (frames_to_store){
        if (btstack_ring_buffer_write(&decoded_audio_ring_buffer, (uint8_t *) &output_buffer[frames_to_copy * NUM_CHANNELS],
                                      frames_to_store * BYTES_PER_FRAME)){
            log_error("PCM ring buffer full");
        }
    }
}

static uint32_t sbc_frames_buffered(void){
    if (sbc_frame_size == 0) return 0;
    return btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) / sbc_frame_size;
}

// PCM frames the playback handler can deliver right now (before resampling)
static uint32_t sink_pcm_frames_available(void){
    return sbc_frames_buffered() * pcm_frames_per_sbc_frame
         + btstack_ring_buffer_bytes_available(&decoded_audio_ring_buffer) / BYTES_PER_FRAME;
}

static void drop_sbc_frames(uint32_t frames){
    uint8_t  scratch[MAX_SBC_FRAME_SIZE];
    uint32_t bytes_read;
    while (frames-- && sbc_frame_size && btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) >= sbc_frame_size){
        btstack_ring_buffer_read(&sbc_frame_ring_buffer, scratch, sbc_frame_size, &bytes_read);
        stat_dropped_sbc++;
    }
}

static uint32_t frames_to_ms(int32_t frames){
    uint32_t rate = a2dp_connection.sbc_configuration.sampling_frequency;
    if (rate == 0 || frames <= 0) return 0;
    return (uint32_t) frames * 1000u / rate;
}

// summarized instead of one log line per event: logging on this thread costs audio time
static void stats_timer_handler(btstack_timer_source_t * ts){
    uint32_t underruns = btstack_audio_ios_get_underruns();
    if (underruns < stat_underruns_seen) stat_underruns_seen = 0;     // stream restarted
    uint32_t new_underruns = underruns - stat_underruns_seen;
    stat_underruns_seen = underruns;
    uint32_t reclaims = btstack_audio_ios_get_reclaims();
    if (reclaims < stat_reclaims_seen) stat_reclaims_seen = 0;
    uint32_t new_reclaims = reclaims - stat_reclaims_seen;
    stat_reclaims_seen = reclaims;
    stat_ticks++;
    int problems = new_underruns || stat_dropped_sbc || stat_trims || new_reclaims;
    if (audio_stream_started && (problems || stat_ticks % STATS_HEALTH_TICKS == 0)){
        int32_t total = (int32_t) (sink_pcm_frames_available() + btstack_audio_ios_get_queued_frames());
        log_info("audio: %s last %u s: %u dropouts, %u SBC frames dropped, %u skips, %u taken back from other apps; buffered %u ms",
                 problems ? "PROBLEMS" : "ok,", STATS_INTERVAL_MS / 1000, new_underruns,
                 stat_dropped_sbc, stat_trims, new_reclaims, frames_to_ms(total));
    }
    stat_dropped_sbc = 0;
    stat_trims = 0;
    btstack_run_loop_set_timer(ts, STATS_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

// Another app took the audio - in a car usually the Music app, which the head unit tells to
// play after a track change. Pause it (only with car controls on, and never after the media
// code crashed once), then the audio driver takes the audio back.
static void audio_before_reclaim(void){
    if (!media_controls_enabled || media_state <= 0) return;
    media_guard_set("pause_music");
    media_controls_pause_system_music();
    media_guard_clear();
}

// Playing again: re-publish the track so we are the Now Playing app again and the head unit's
// buttons come back to us.
static void audio_after_reclaim(void){
    if (media_state > 0 && media_controls_enabled && a2dp_connection.connected){
        media_controls_refresh();
    }
}

static void media_processing_init(media_codec_configuration_sbc_t * configuration){
    if (media_initialized) return;
    sbc_decoder_instance = btstack_sbc_decoder_bluedroid_init_instance(&sbc_decoder_context);
    sbc_decoder_instance->configure(&sbc_decoder_context, SBC_MODE_STANDARD, handle_pcm_data, NULL);

    btstack_ring_buffer_init(&sbc_frame_ring_buffer, sbc_frame_storage, sizeof(sbc_frame_storage));
    btstack_ring_buffer_init(&decoded_audio_ring_buffer, decoded_audio_storage, sizeof(decoded_audio_storage));
    btstack_resample_init(&resample_instance, configuration->num_channels);
    resampling_min_factor = btstack_resample_get_min_factor_for_output_capacity(SBC_MAX_AUDIO_FRAMES_PER_BLOCK, RESAMPLE_OUTPUT_FRAMES);

    unsigned int blocks = configuration->block_length;
    unsigned int bands  = configuration->subbands;
    pcm_frames_per_sbc_frame = (blocks >= 4 && blocks <= 16 && (bands == 4 || bands == 8)) ? blocks * bands : 128;
    sbc_frame_size = 0;

    if (audio_sink){
        audio_sink->init(NUM_CHANNELS, configuration->sampling_frequency, &playback_handler);
        audio_sink->set_volume(volume_percentage * 127 / 100);
    }
    btstack_audio_ios_set_available_handler(&sink_pcm_frames_available);
    btstack_audio_ios_set_reclaim_handlers(&audio_before_reclaim, &audio_after_reclaim);
    audio_stream_started = 0;
    media_initialized = 1;

    stat_dropped_sbc = stat_trims = stat_underruns_seen = stat_reclaims_seen = 0;
    stat_ticks = 0;
    btstack_run_loop_remove_timer(&stats_timer);
    btstack_run_loop_set_timer_handler(&stats_timer, &stats_timer_handler);
    btstack_run_loop_set_timer(&stats_timer, STATS_INTERVAL_MS);
    btstack_run_loop_add_timer(&stats_timer);
}

static void media_processing_start(void){
    if (!media_initialized) return;
    audio_stream_started = 1;
    total_avg = (int32_t) sink_pcm_frames_available();
    log_info("audio: starting playback with %u ms buffered", frames_to_ms(total_avg));
    if (audio_sink) audio_sink->start_stream();
}

static void media_processing_pause(void){
    if (!media_initialized) return;
    audio_stream_started = 0;
    if (audio_sink) audio_sink->stop_stream();
    btstack_ring_buffer_reset(&decoded_audio_ring_buffer);
    btstack_ring_buffer_reset(&sbc_frame_ring_buffer);
}

static void media_processing_close(void){
    if (!media_initialized) return;
    media_initialized = 0;
    audio_stream_started = 0;
    sbc_frame_size = 0;
    btstack_run_loop_remove_timer(&stats_timer);
    if (audio_sink) audio_sink->close();
}

static int read_media_data_header(uint8_t * packet, int size, int * offset){
    // RTP header without CSRC list: 12 bytes
    UNUSED(packet);
    if (size - *offset < 12) return 0;
    *offset += 12;
    return 1;
}

static int read_sbc_header(uint8_t * packet, int size, int * offset, uint8_t * num_frames){
    if (size - *offset < 1) return 0;
    *num_frames = packet[*offset] & 0x0f;
    *offset += 1;
    return *num_frames != 0;
}

static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t * packet, uint16_t size){
    UNUSED(seid);
    int pos = 0;
    uint8_t num_frames;
    if (!read_media_data_header(packet, size, &pos)) return;
    if (!read_sbc_header(packet, size, &pos, &num_frames)) return;
    if (!media_initialized) return;

    int packet_length = size - pos;
    uint8_t * packet_begin = packet + pos;

    unsigned int frame_size = packet_length / num_frames;
    if (frame_size == 0 || frame_size > MAX_SBC_FRAME_SIZE) return;
    if (frame_size != sbc_frame_size){
        // the phone changed the bitpool: buffered frames of the old size would be misparsed
        if (sbc_frame_size){
            log_info("audio: SBC frame size %u -> %u, flushing %u frames", sbc_frame_size, frame_size, sbc_frames_buffered());
            btstack_ring_buffer_reset(&sbc_frame_ring_buffer);
        }
        sbc_frame_size = frame_size;
    }

    // never lose the newest audio: if the ring is full, drop the oldest frames instead
    if (btstack_ring_buffer_bytes_free(&sbc_frame_ring_buffer) < (uint32_t) packet_length){
        drop_sbc_frames((packet_length + frame_size - 1) / frame_size);
    }
    btstack_ring_buffer_write(&sbc_frame_ring_buffer, packet_begin, packet_length);

    if (!audio_stream_started){
        if (sink_pcm_frames_available() >= START_FRAMES) media_processing_start();
        return;
    }

    // keep the total buffered audio (SBC + AudioQueue) near the target
    int32_t total = (int32_t) (sink_pcm_frames_available() + btstack_audio_ios_get_queued_frames());
    total_avg += (total - total_avg) / 16;
    if (total_avg > TRIM_ABOVE_FRAMES){
        // far too much (the phone caught up in a burst after a long stall):
        // skip ahead once instead of carrying the extra delay
        drop_sbc_frames((uint32_t) (total_avg - TARGET_FRAMES) / pcm_frames_per_sbc_frame);
        stat_trims++;
        total_avg = (int32_t) (sink_pcm_frames_available() + btstack_audio_ios_get_queued_frames());
    }
    const uint32_t nominal_factor = 0x10000;
    const uint32_t compensation   = 0x00100;     // 0.4 %: inaudible
    uint32_t resampling_factor = nominal_factor;
    if (total_avg < TARGET_FRAMES - BAND_FRAMES){
        resampling_factor = nominal_factor - compensation;      // running low: stretch
    } else if (total_avg > TARGET_FRAMES + BAND_FRAMES){
        resampling_factor = nominal_factor + compensation;      // too much: compress
    }
    btstack_resample_set_factor(&resample_instance, btstack_max(resampling_factor, resampling_min_factor));

    // AudioQueue buffers may be waiting for exactly this data
    btstack_audio_ios_kick();
}

// --- volume ---

static void apply_volume(uint8_t avrcp_volume){
    volume_percentage = avrcp_volume * 100 / 127;
    if (audio_sink) audio_sink->set_volume(avrcp_volume);
}

static void set_volume_percentage(int new_percentage){
    volume_percentage = btstack_max(0, btstack_min(100, new_percentage));
    uint8_t volume = volume_percentage * 127 / 100;
    if (avrcp_connection.avrcp_cid) avrcp_target_volume_changed(avrcp_connection.avrcp_cid, volume);
    if (audio_sink) audio_sink->set_volume(volume);
}

// --- event handlers ---

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t address;
    switch (hci_event_packet_get_type(packet)){
        case HCI_EVENT_PIN_CODE_REQUEST:
            // phones from before Bluetooth 2.1: use the classic speaker PIN
            hci_event_pin_code_request_get_bd_addr(packet, address);
            log_info("legacy pairing with %s, using PIN 0000", bd_addr_to_str(address));
            gap_pin_code_response(address, "0000");
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            // headless receiver: accept like any Bluetooth speaker
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            log_info("SSP pairing with %s, accepting", bd_addr_to_str(address));
            gap_ssp_confirmation_response(address);
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            if (hci_event_remote_name_request_complete_get_status(packet) != ERROR_CODE_SUCCESS) break;
            hci_event_remote_name_request_complete_get_bd_addr(packet, address);
            if (a2dp_connection.connected && bd_addr_cmp(address, a2dp_connection.addr) == 0){
                const char * name = hci_event_remote_name_request_complete_get_remote_name(packet);
                sink_status_set_remote(NULL, name);
                remember_device(address, name);
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (hci_event_disconnection_complete_get_connection_handle(packet) != a2dp_con_handle) break;
            a2dp_con_handle = HCI_CON_HANDLE_INVALID;
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            if (reason == ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION || reason == ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST){
                // disconnected on purpose (phone UI, or we are shutting down): don't chase the phone
                log_info("reconnect: link closed on purpose (reason 0x%02x), not reconnecting", reason);
                reconnect_blocked = 1;
            } else {
                log_info("reconnect: link lost (reason 0x%02x)", reason);
                reconnect_attempts = 0;
                schedule_reconnect(RECONNECT_AFTER_LOSS_MS);
            }
            break;
        }
        default:
            break;
    }
}

static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    char codec[64];
    bd_addr_t address;
    media_codec_configuration_sbc_t * sbc = &a2dp_connection.sbc_configuration;

    switch (packet[2]){
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            connect_attempt_pending = 0;
            if (a2dp_subevent_signaling_connection_established_get_status(packet) != ERROR_CODE_SUCCESS){
                log_info("A2DP connection failed, status 0x%02x", a2dp_subevent_signaling_connection_established_get_status(packet));
                break;
            }
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, address);
            memcpy(a2dp_connection.addr, address, 6);
            a2dp_connection.a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            a2dp_con_handle = a2dp_subevent_signaling_connection_established_get_con_handle(packet);
            a2dp_connection.connected = 1;
            log_info("A2DP connected to %s", bd_addr_to_str(address));
            // connected (by us or by the phone): stop reconnecting, allow it again later
            btstack_run_loop_remove_timer(&reconnect_timer);
            reconnect_blocked = 0;
            reconnect_attempts = 0;
            remember_device(address, NULL);
            cur_title[0] = 0;
            cur_artist[0] = 0;
            sink_status_set_state(SINK_STATE_CONNECTED);
            sink_status_set_remote(bd_addr_to_str(address), "");
            gap_remote_name_request(address, 0x01, 0);
            // connected: stop inquiry scanning, it steals radio time from the audio link
            gap_discoverable_control(0);
            media_apply_active();
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
            sbc->reconfigure        = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            sbc->num_channels       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            sbc->sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            sbc->block_length       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            sbc->subbands           = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            sbc->min_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            sbc->max_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            sbc->allocation_method  = (btstack_sbc_allocation_method_t)(a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1);
            switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)){
                case AVDTP_CHANNEL_MODE_JOINT_STEREO: sbc->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO; break;
                case AVDTP_CHANNEL_MODE_STEREO:       sbc->channel_mode = SBC_CHANNEL_MODE_STEREO;       break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: sbc->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
                default:                              sbc->channel_mode = SBC_CHANNEL_MODE_MONO;         break;
            }
            snprintf(codec, sizeof(codec), "SBC %u Hz, %s, bitpool %u-%u", sbc->sampling_frequency,
                     sbc->num_channels == 1 ? "mono" : "stereo", sbc->min_bitpool_value, sbc->max_bitpool_value);
            log_info("codec: %s", codec);
            sink_status_set_codec(codec);
            break;
        }

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            if (a2dp_subevent_stream_established_get_status(packet) != ERROR_CODE_SUCCESS) break;
            a2dp_connection.a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            btstack_run_loop_remove_timer(&avrcp_connect_timer);
            btstack_run_loop_set_timer_handler(&avrcp_connect_timer, &avrcp_connect_timer_handler);
            btstack_run_loop_set_timer(&avrcp_connect_timer, AVRCP_CONNECT_DELAY_MS);
            btstack_run_loop_add_timer(&avrcp_connect_timer);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            a2dp_connection.playing = 1;
            if (sbc->reconfigure) media_processing_close();
            media_processing_init(sbc);   // audio starts once enough frames are buffered
            sink_status_set_state(SINK_STATE_PLAYING);
            media_refresh_playing();
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            a2dp_connection.playing = 0;
            media_processing_pause();
            sink_status_set_state(SINK_STATE_CONNECTED);
            media_refresh_playing();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            a2dp_connection.playing = 0;
            media_processing_close();
            if (a2dp_connection.connected) sink_status_set_state(SINK_STATE_CONNECTED);
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            log_info("A2DP disconnected");
            a2dp_connection.a2dp_cid = 0;
            a2dp_connection.connected = 0;
            a2dp_connection.playing = 0;
            btstack_run_loop_remove_timer(&avrcp_connect_timer);
            media_processing_close();
            cur_title[0] = 0;
            cur_artist[0] = 0;
            media_apply_active();
            gap_discoverable_control(1);       // findable again for pairing
            sink_status_set_state(SINK_STATE_READY);
            break;

        default:
            break;
    }
}

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            if (avrcp_subevent_connection_established_get_status(packet) != ERROR_CODE_SUCCESS){
                avrcp_connection.avrcp_cid = 0;
                return;
            }
            avrcp_connection.avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            avrcp_target_support_event(avrcp_connection.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_support_event(avrcp_connection.avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_target_battery_status_changed(avrcp_connection.avrcp_cid, AVRCP_BATTERY_STATUS_EXTERNAL);
            avrcp_controller_get_supported_events(avrcp_connection.avrcp_cid);
            autoplay_start();
            break;
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            avrcp_connection.avrcp_cid = 0;
            btstack_run_loop_remove_timer(&autoplay_timer);
            break;
        default:
            break;
    }
}

static void copy_avrcp_value(char * dst, size_t dst_size, const uint8_t * value, uint16_t len){
    size_t n = btstack_min(len, dst_size - 1);
    memcpy(dst, value, n);
    dst[n] = 0;
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (avrcp_connection.avrcp_cid == 0) return;

    char value[128];
    switch (packet[2]){
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:
            avrcp_controller_enable_notification(avrcp_connection.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(avrcp_connection.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_controller_get_now_playing_info(avrcp_connection.avrcp_cid);
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            avrcp_controller_get_now_playing_info(avrcp_connection.avrcp_cid);
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            remote_playing = avrcp_subevent_notification_playback_status_changed_get_play_status(packet) == AVRCP_PLAYBACK_STATUS_PLAYING;
            media_refresh_playing();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            copy_avrcp_value(value, sizeof(value), avrcp_subevent_now_playing_title_info_get_value(packet),
                             avrcp_subevent_now_playing_title_info_get_value_len(packet));
            sink_status_set_track(value, NULL);
            btstack_strcpy(cur_title, sizeof(cur_title), value);
            media_refresh_metadata();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            copy_avrcp_value(value, sizeof(value), avrcp_subevent_now_playing_artist_info_get_value(packet),
                             avrcp_subevent_now_playing_artist_info_get_value_len(packet));
            sink_status_set_track(NULL, value);
            btstack_strcpy(cur_artist, sizeof(cur_artist), value);
            media_refresh_metadata();
            break;
        default:
            break;
    }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            // phone moved its volume slider (absolute volume)
            apply_volume(avrcp_subevent_notification_volume_changed_get_absolute_volume(packet));
            break;
        case AVRCP_SUBEVENT_OPERATION:
            if (avrcp_subevent_operation_get_button_pressed(packet) == 0) break;
            switch ((avrcp_operation_id_t) avrcp_subevent_operation_get_operation_id(packet)){
                case AVRCP_OPERATION_ID_VOLUME_UP:   set_volume_percentage(volume_percentage + 10); break;
                case AVRCP_OPERATION_ID_VOLUME_DOWN: set_volume_percentage(volume_percentage - 10); break;
                default: break;
            }
            break;
        default:
            break;
    }
}

// --- public ---

void sink_setup(const char * local_name, const btstack_audio_sink_t * audio){
    audio_sink = audio;

    l2cap_init();
    sdp_init();

    a2dp_sink_init();
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    a2dp_sink_register_packet_handler(&a2dp_sink_packet_handler);
    a2dp_sink_register_media_handler(&handle_l2cap_media_data_packet);
    avdtp_stream_endpoint_t * local_stream_endpoint = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO,
        AVDTP_CODEC_SBC, media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
        stream_endpoint.media_sbc_codec_configuration, sizeof(stream_endpoint.media_sbc_codec_configuration));
    btstack_assert(local_stream_endpoint != NULL);
    stream_endpoint.a2dp_local_seid = avdtp_local_seid(local_stream_endpoint);

    avrcp_register_packet_handler(&avrcp_packet_handler);
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // SDP records: A2DP sink (speaker), AVRCP controller + target, Device ID
    memset(sdp_avdtp_sink_service_buffer, 0, sizeof(sdp_avdtp_sink_service_buffer));
    a2dp_sink_create_sdp_record(sdp_avdtp_sink_service_buffer, sdp_create_service_record_handle(),
                                AVDTP_SINK_FEATURE_MASK_SPEAKER, NULL, NULL);
    sdp_register_service(sdp_avdtp_sink_service_buffer);

    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(),
                                       1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, sdp_create_service_record_handle(),
                                   1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
    sdp_register_service(sdp_avrcp_target_service_buffer);

    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(device_id_sdp_service_buffer);

    // GAP: show up as a loudspeaker, pair without user interaction
    gap_set_local_name(local_name);
    gap_discoverable_control(1);
    gap_set_class_of_device(0x240414);   // Audio/Rendering service, Audio/Video major, Loudspeaker minor
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    // role switch allowed, but no sniff mode: sniff on the ACL link is a common source
    // of periodic small audio dropouts while streaming
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    gap_set_allow_role_switch(true);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
}

void sink_reconnect_last_device(void){
    load_last_device();
    reconnect_blocked = 0;
    reconnect_attempts = 0;
    if (have_last_device) log_info("reconnect: last device %s '%s'", bd_addr_to_str(last_device_addr), last_device_name);
    schedule_reconnect(RECONNECT_FIRST_DELAY_MS);
}

void sink_set_auto_reconnect(int enabled){
    enabled = enabled ? 1 : 0;
    if (enabled == auto_reconnect) return;
    auto_reconnect = enabled;
    log_info("reconnect: auto reconnect %s", enabled ? "on" : "off");
    reconnect_attempts = 0;
    schedule_reconnect(RECONNECT_FIRST_DELAY_MS);   // clears status if now off
}

void sink_set_media_controls(int enabled){
    enabled = enabled ? 1 : 0;
    if (enabled == media_controls_enabled) return;
    media_controls_enabled = enabled;
    // switched back on after a failure and the app cleared the guard: try again
    if (enabled && media_state < 0 && access(SINK_MEDIA_GUARD, F_OK) != 0){
        media_state = 0;
    }
    media_apply_active();
}

void sink_media_prepare(void){
    media_hci_ready = 1;
    media_apply_active();
}

void sink_set_auto_play(int enabled){
    auto_play = enabled ? 1 : 0;
    if (!auto_play) btstack_run_loop_remove_timer(&autoplay_timer);
}

void sink_forget_last_device(void){
    have_last_device = 0;
    last_device_name[0] = 0;
    btstack_run_loop_remove_timer(&reconnect_timer);
    sink_status_set_reconnecting(NULL);
}

void sink_disconnect(void){
    btstack_run_loop_remove_timer(&reconnect_timer);
    reconnect_blocked = 1;
    if (a2dp_connection.a2dp_cid) a2dp_sink_disconnect(a2dp_connection.a2dp_cid);
    if (avrcp_connection.avrcp_cid) avrcp_disconnect(avrcp_connection.avrcp_cid);
}

void sink_shutdown_audio(void){
    if (media_state > 0) media_controls_set_active(0);
    media_processing_close();
}
