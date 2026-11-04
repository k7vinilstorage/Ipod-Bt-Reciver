/*
 * sink.c - A2DP Sink + AVRCP Controller/Target
 *
 * Adapted from BTstack's example/a2dp_sink_demo.c (BlueKitchen GmbH, BTstack license),
 * trimmed to what a headless receiver needs, plus status reporting for the app.
 *
 * Media path: L2CAP media packets -> SBC frames into a ring buffer -> decoded on demand
 * when the audio driver asks for PCM -> resampled slightly faster/slower to keep the
 * buffer level stable (phone and iPod clocks drift) -> AudioQueue.
 *
 * Music service (optional, "Keep Music Service Running"): the receiver stays the iPod's
 * playing music app all the time. While no phone audio plays, the AudioQueue plays silence
 * and Now Playing shows a "Waiting for phone" track - also while Bluetooth is off. A car
 * head unit that connects (ignition on) finds an iPod that is already playing and switches
 * its audio on; the phone's audio then follows the silence in the same, running queue.
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
#define SEAMLESS_START_FRAMES 4096                    // with the queue playing silence (~11000 frames
                                                      // queued) this is enough to switch to the audio
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
static int audio_configured;         // the driver has a format and our playback handler

// --- music service state (logic further down, after the audio pipeline) ---

#define MUSIC_SERVICE_MIN_BATTERY  20       // percent: below this, without a charger, it stops

static int      music_service;
static int      music_yielded;       // another app took the audio while no phone was connected, on battery
static int      external_power = 1;
static int      battery_stop;        // battery low and no charger
static uint32_t silence_rate = 44100;          // the last phone stream's sample rate
static uint32_t placeholder_since;

// the music service wants the queue playing (silence while no phone audio plays)
static int music_silence_wanted(void){
    return music_service && !battery_stop && !music_yielded;
}

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
static int remote_status_known;        // the phone has reported its play status at least once

// --- media controls / Now Playing (lock screen + car/USB transport buttons) ---
//
// Experimental and isolated: MediaPlayer is only touched after Bluetooth is up, and a
// guard file is written around each risky first call. If the daemon dies there, the
// next start finds the guard and leaves media controls off, so the receiver still
// works (the app shows it and can retry by toggling the setting).
//
// Like a music app we publish full Now Playing info: title/artist/album, duration and
// elapsed time (lock screen progress bar, head unit time display) and a playlist position.
// The playlist is synthetic - a long list with us in the middle - because iPod-mode head
// units only send next/previous when they believe there is a track to go to; on the
// one-track "playlist" iOS reported before, the Lancer sent stop instead of next.

#define QUEUE_COUNT             9999
#define QUEUE_START_INDEX       99      // shown as track 100, room for many "previous"
#define PLAY_STATUS_POLL_MS     15000   // re-sync the elapsed time with the phone
#define NAV_SETTLE_MS           2500    // a track change this soon after next/prev is that command
#define STOP_AS_SKIP_MS         2500    // "stop" + the Music app starting = the head unit's "next"
#define NOT_PLAYING_DEBOUNCE_MS 1500
#define EXPLICIT_PAUSE_MS       3000    // a pause the user asked for is shown at once
#define STATE_CHECK_DELAY_MS    700     // ask the phone for its state after play/pause
#define TIME_REFRESH_MS         1000
#define HEADUNIT_KICK_DELAY_MS  1500    // after the audio started
#define HEADUNIT_KICK_GAP_MS    10000
#define HEADUNIT_KICK_WAIT_MS   30000   // give up if the audio doesn't start in this time
#define TRACK_INFO_DELAY_MS     250     // phones update their metadata just after "track changed"
#define TRACK_INFO_RETRY_MS     400     // still the old title: ask again
#define TRACK_INFO_MAX_RETRIES  3
#define TRACK_INFO_TIMEOUT_MS   2500    // publish what we have if the phone doesn't answer
#define AVRCP_RETRY_MS          60
#define AVRCP_GIVE_UP_MS        5000    // controller stuck busy this long: drop queued requests
#define TRANSPORT_QUEUE_SIZE    8

// The music service's stand-in track while no phone track is known
#define PLACEHOLDER_TITLE      "BT Receiver"
#define PLACEHOLDER_WAITING    "Waiting for phone"
#define PLACEHOLDER_CONNECTED  "Phone connected"

enum { PUBLISHED_NOTHING, PUBLISHED_PLACEHOLDER, PUBLISHED_TRACK };

static void sink_media_command(media_command_t cmd);

static int  media_controls_enabled = 1;
static int  media_hci_ready;          // Bluetooth is up, safe to start media controls
static int  media_state;              // 0 = not started, 1 = ready, -1 = unavailable / failed
static int  media_activated_once;
static int  media_active;             // we are the Now Playing app (media_controls_set_active)

static char     cur_title[128];
static char     cur_artist[128];
static char     cur_album[128];
static uint32_t cur_duration_ms;      // 0 = unknown
static uint32_t cur_position_ms;      // elapsed time at cur_position_time
static uint32_t cur_position_time;
static int      position_running;     // elapsed time advancing (phone playing)
static uint32_t queue_index = QUEUE_START_INDEX;
static int      published_playing;    // debounced play state shown to iOS
static int      published_kind;       // PUBLISHED_*: what iOS shows right now
static uint32_t published_index;      // ... and at which playlist position
static int      have_track_info;      // the phone's track info came in since it connected
static uint32_t last_nav_time;        // last next/previous forwarded to the phone
static uint32_t last_stop_time;       // last (ignored) stop from the lock screen / head unit
static uint32_t last_pause_cmd_time;  // last pause the user asked for
static int      play_status_logged;   // logged the phone's timing for this track
static int      kick_done;            // announced the track once for this connection
static uint32_t last_kick_time;
static uint32_t kick_deadline;
static btstack_timer_source_t state_check_timer;
static btstack_timer_source_t time_refresh_timer;
static btstack_timer_source_t headunit_kick_timer;
static btstack_timer_source_t playing_debounce_timer;
static btstack_timer_source_t play_status_timer;

// A track change in progress: the new playlist position is held back until the new track's
// info is in. Head units read title/artist/album when they see the position change and don't
// read them again, so publishing the position first left them one track behind.
static int      track_info_pending;
static int      pending_index_step;
static int      pending_nav_step;      // direction of our last next/previous
static uint32_t track_change_time;
static char     prev_title[128];       // the previous track's, to spot stale info
static char     prev_artist[128];
static int      info_retries;
static btstack_timer_source_t track_info_timer;
static btstack_timer_source_t track_info_timeout_timer;

// BTstack's AVRCP controller runs one request at a time and rejects (COMMAND_DISALLOWED)
// anything sent meanwhile. That silently dropped the play-status requests (so no song time)
// and could drop button presses. Everything goes through this queue and is retried.
static uint8_t  transport_queue[TRANSPORT_QUEUE_SIZE];   // media_command_t, in order
static unsigned transport_head;
static unsigned transport_count;
static int      want_info;
static int      want_play_status;
static uint32_t avrcp_busy_since;
static btstack_timer_source_t avrcp_retry_timer;

static int media_is_playing(void){
    // The phone's own play status wins. The Bluetooth audio stream can stay open for seconds
    // after a pause (on some phones for good), so "stream open" made play/pause send "pause"
    // again and again and showed "playing" to iOS and the head unit.
    if (remote_status_known) return remote_playing;
    return a2dp_connection.playing;
}

static uint32_t now_ms(void){
    return btstack_run_loop_get_time_ms();
}

static uint32_t position_now_ms(void){
    uint32_t pos = cur_position_ms;
    if (position_running) pos += now_ms() - cur_position_time;
    if (cur_duration_ms && pos > cur_duration_ms) pos = cur_duration_ms;
    return pos;
}

static void position_set(uint32_t position_ms, int running){
    cur_position_ms   = position_ms;
    cur_position_time = now_ms();
    position_running  = running ? 1 : 0;
}

// freeze / unfreeze the elapsed time at its current value
static void position_set_running(int running){
    position_set(position_now_ms(), running);
}

static void reset_track_info(void){
    cur_title[0] = cur_artist[0] = cur_album[0] = 0;
    cur_duration_ms = 0;
    position_set(0, 0);
}

static void avrcp_pump(void);

static void avrcp_retry_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    avrcp_pump();
}

static void avrcp_queue_reset(void){
    transport_count = 0;
    want_info = 0;
    want_play_status = 0;
    avrcp_busy_since = 0;
    btstack_run_loop_remove_timer(&avrcp_retry_timer);
}

// the controller is busy with an earlier request: try again shortly
static void avrcp_wait(void){
    uint32_t now = btstack_run_loop_get_time_ms();
    if (avrcp_busy_since == 0) avrcp_busy_since = now;
    if (now - avrcp_busy_since > AVRCP_GIVE_UP_MS){
        log_error("AVRCP: controller busy for %u s, dropping queued requests", AVRCP_GIVE_UP_MS / 1000);
        avrcp_queue_reset();
        return;
    }
    btstack_run_loop_remove_timer(&avrcp_retry_timer);
    btstack_run_loop_set_timer_handler(&avrcp_retry_timer, &avrcp_retry_handler);
    btstack_run_loop_set_timer(&avrcp_retry_timer, AVRCP_RETRY_MS);
    btstack_run_loop_add_timer(&avrcp_retry_timer);
}

static uint8_t avrcp_send_transport(uint8_t cmd, uint16_t cid){
    switch ((media_command_t) cmd){
        case MEDIA_CMD_PLAY:     return avrcp_controller_play(cid);
        case MEDIA_CMD_PAUSE:    return avrcp_controller_pause(cid);
        case MEDIA_CMD_NEXT:     return avrcp_controller_forward(cid);
        case MEDIA_CMD_PREVIOUS: return avrcp_controller_backward(cid);
        default:                 return ERROR_CODE_SUCCESS;
    }
}

// send what is waiting, in order: button presses, then track info, then play status
static void avrcp_pump(void){
    uint16_t cid = avrcp_connection.avrcp_cid;
    if (!cid){
        avrcp_queue_reset();
        return;
    }
    uint8_t status;
    while (transport_count){
        status = avrcp_send_transport(transport_queue[transport_head], cid);
        if (status == ERROR_CODE_COMMAND_DISALLOWED){ avrcp_wait(); return; }
        if (status != ERROR_CODE_SUCCESS) log_error("AVRCP: command %u failed: 0x%02x", transport_queue[transport_head], status);
        transport_head = (transport_head + 1) % TRANSPORT_QUEUE_SIZE;
        transport_count--;
        avrcp_busy_since = 0;
    }
    if (want_info){
        status = avrcp_controller_get_now_playing_info(cid);
        if (status == ERROR_CODE_COMMAND_DISALLOWED){ avrcp_wait(); return; }
        want_info = 0;
        avrcp_busy_since = 0;
    }
    if (want_play_status){
        status = avrcp_controller_get_play_status(cid);
        if (status == ERROR_CODE_COMMAND_DISALLOWED){ avrcp_wait(); return; }
        want_play_status = 0;
        avrcp_busy_since = 0;
    }
}

static void avrcp_queue_transport(media_command_t cmd){
    if (transport_count == TRANSPORT_QUEUE_SIZE) return;     // button mashing: drop extras
    transport_queue[(transport_head + transport_count) % TRANSPORT_QUEUE_SIZE] = (uint8_t) cmd;
    transport_count++;
    avrcp_pump();
}

static void avrcp_request_info(void){
    want_info = 1;
    avrcp_pump();
}

static void queue_index_step(int step){
    int32_t i = (int32_t) queue_index + step;
    if (i < 1) i = 1;
    if (i > QUEUE_COUNT - 2) i = QUEUE_COUNT - 2;
    queue_index = (uint32_t) i;
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

// publish everything we know: the app's status file always, iOS when car controls are on.
// With the music service a stand-in track shows while no phone track is known.
static void media_publish(void){
    sink_status_set_timing(cur_duration_ms, position_now_ms(), position_running);
    if (track_info_pending) return;      // published together with the new track's info
    if (!media_active) return;
    int kind;
    if (a2dp_connection.connected && (have_track_info || !music_service)){
        kind = PUBLISHED_TRACK;
    } else if (music_service){
        kind = PUBLISHED_PLACEHOLDER;
    } else {
        return;
    }
    // head units read a track's info when the playlist position changes: the stand-in and
    // the phone's track must not show up at the same position
    if (published_kind != PUBLISHED_NOTHING && kind != published_kind && queue_index == published_index){
        queue_index_step(+1);
    }
    if (kind == PUBLISHED_PLACEHOLDER && published_kind != PUBLISHED_PLACEHOLDER) placeholder_since = now_ms();

    media_now_playing_t np;
    memset(&np, 0, sizeof(np));
    np.queue_index = queue_index;
    np.queue_count = QUEUE_COUNT;
    if (kind == PUBLISHED_TRACK){
        np.title       = cur_title;
        np.artist      = cur_artist;
        np.album       = cur_album;
        np.duration_ms = cur_duration_ms;
        np.position_ms = position_now_ms();
        np.playing     = published_playing;
    } else {
        np.title       = PLACEHOLDER_TITLE;
        np.artist      = a2dp_connection.connected ? PLACEHOLDER_CONNECTED : PLACEHOLDER_WAITING;
        np.album       = "";
        np.position_ms = now_ms() - placeholder_since;
        np.playing     = 1;      // a playing music app: head units switch their audio on for it
    }
    published_kind  = kind;
    published_index = queue_index;
    media_controls_set_now_playing(&np);
}

static void media_apply_active(void){
    // started once Bluetooth is up, or right away for the music service
    if (!media_hci_ready && !music_service && media_state <= 0) return;
    if (!media_controls_enabled){
        if (media_state > 0) media_controls_set_active(0);
        media_active = 0;
        published_kind = PUBLISHED_NOTHING;
        sink_status_set_media("off");
        return;
    }
    if (!media_ensure_ready()) return;
    int want = a2dp_connection.connected || music_silence_wanted();
    if (want && !media_activated_once){
        media_guard_set("activate");
        media_controls_set_active(1);
        media_active = 1;
        media_publish();
        media_guard_clear();
        media_activated_once = 1;
    } else {
        media_controls_set_active(want);
        media_active = want;
        if (want) media_publish();
        else published_kind = PUBLISHED_NOTHING;
    }
    sink_status_set_media(want ? "active" : "ready");
}

// Phones briefly report "not playing" around track changes. Publishing that makes some head
// units decide the iPod stopped and fall back to the Music app, so only a pause that lasts
// is published; "playing" goes out immediately.
static void playing_debounce_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (published_playing && !media_is_playing()){
        published_playing = 0;
        media_publish();
    }
}

// the phone's play state may have changed (AVRCP status, stream started / suspended)
static void media_playing_changed(void){
    int playing = media_is_playing();
    if (playing != position_running) position_set_running(playing);
    btstack_run_loop_remove_timer(&playing_debounce_timer);
    if (playing){
        published_playing = 1;
        media_publish();
        return;
    }
    if (published_playing && last_pause_cmd_time && now_ms() - last_pause_cmd_time < EXPLICIT_PAUSE_MS){
        published_playing = 0;           // the user paused: show it right away
        media_publish();
        return;
    }
    if (published_playing){
        btstack_run_loop_set_timer_handler(&playing_debounce_timer, &playing_debounce_handler);
        btstack_run_loop_set_timer(&playing_debounce_timer, NOT_PLAYING_DEBOUNCE_MS);
        btstack_run_loop_add_timer(&playing_debounce_timer);
    } else {
        media_publish();
    }
}

static void request_play_status(void){
    want_play_status = 1;
    avrcp_pump();
}

static void state_check_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    request_play_status();
}

// after play / pause: ask the phone what it is doing now, don't wait for its notification
static void schedule_state_check(void){
    btstack_run_loop_remove_timer(&state_check_timer);
    btstack_run_loop_set_timer_handler(&state_check_timer, &state_check_handler);
    btstack_run_loop_set_timer(&state_check_timer, STATE_CHECK_DELAY_MS);
    btstack_run_loop_add_timer(&state_check_timer);
}

// Re-publish the elapsed time every second while playing. The lock screen extrapolates it
// itself, but head units can be handed the last published value, which kept the Lancer at 0.
static void time_refresh_handler(btstack_timer_source_t * ts){
    if (!a2dp_connection.connected) return;
    if (published_playing && !track_info_pending && media_state > 0 && media_controls_enabled){
        media_controls_refresh();
    }
    btstack_run_loop_set_timer(ts, TIME_REFRESH_MS);
    btstack_run_loop_add_timer(ts);
}

static void start_time_refresh(void){
    btstack_run_loop_remove_timer(&time_refresh_timer);
    btstack_run_loop_set_timer_handler(&time_refresh_timer, &time_refresh_handler);
    btstack_run_loop_set_timer(&time_refresh_timer, TIME_REFRESH_MS);
    btstack_run_loop_add_timer(&time_refresh_timer);
}

// iPod-mode head units start their audio input when the iPod announces a new track - that's
// why the sound only came on after pressing next. Once our audio runs, announce the current
// track as a new one (playlist position +1, same song).
static void headunit_kick_handler(btstack_timer_source_t * ts){
    if (!a2dp_connection.connected || !(media_state > 0 && media_controls_enabled)) return;
    if (track_info_pending || !audio_stream_started){
        if ((int32_t) (now_ms() - kick_deadline) > 0) return;     // audio never started: skip
        btstack_run_loop_set_timer(ts, 500);
        btstack_run_loop_add_timer(ts);
        return;
    }
    log_info("media: announcing the track to the head unit so it starts its audio");
    last_kick_time = now_ms();
    queue_index_step(+1);
    media_publish();
}

static void headunit_kick_schedule(void){
    if (last_kick_time && now_ms() - last_kick_time < HEADUNIT_KICK_GAP_MS) return;
    kick_deadline = now_ms() + HEADUNIT_KICK_WAIT_MS;
    btstack_run_loop_remove_timer(&headunit_kick_timer);
    btstack_run_loop_set_timer_handler(&headunit_kick_timer, &headunit_kick_handler);
    btstack_run_loop_set_timer(&headunit_kick_timer, HEADUNIT_KICK_DELAY_MS);
    btstack_run_loop_add_timer(&headunit_kick_timer);
}

static void play_status_timer_handler(btstack_timer_source_t * ts){
    if (!a2dp_connection.connected || !avrcp_connection.avrcp_cid) return;   // restarted on reconnect
    if (media_is_playing()) request_play_status();
    btstack_run_loop_set_timer(ts, PLAY_STATUS_POLL_MS);
    btstack_run_loop_add_timer(ts);
}

static void start_play_status_polling(void){
    btstack_run_loop_remove_timer(&play_status_timer);
    btstack_run_loop_set_timer_handler(&play_status_timer, &play_status_timer_handler);
    btstack_run_loop_set_timer(&play_status_timer, PLAY_STATUS_POLL_MS);
    btstack_run_loop_add_timer(&play_status_timer);
}

static void finish_track_change(void){
    btstack_run_loop_remove_timer(&track_info_timer);
    btstack_run_loop_remove_timer(&track_info_timeout_timer);
    if (!track_info_pending) return;
    track_info_pending = 0;
    queue_index_step(pending_index_step);
    pending_index_step = 0;
    play_status_logged = 0;
    have_track_info = 1;
    log_info("track %u: '%s' by '%s'%s%s, length %u:%02u", queue_index + 1, cur_title, cur_artist,
             cur_album[0] ? " on " : "", cur_album, cur_duration_ms / 60000, (cur_duration_ms / 1000) % 60);
    media_publish();          // new position together with the new title / artist / album / length
    request_play_status();    // and the exact elapsed time
}

static void track_info_request_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (track_info_pending) avrcp_request_info();
}

static void track_info_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (!track_info_pending) return;
    log_info("track: no updated info from the phone in time, publishing what we have");
    finish_track_change();
}

static void schedule_track_info_request(uint32_t delay_ms){
    btstack_run_loop_remove_timer(&track_info_timer);
    btstack_run_loop_set_timer_handler(&track_info_timer, &track_info_request_handler);
    btstack_run_loop_set_timer(&track_info_timer, delay_ms);
    btstack_run_loop_add_timer(&track_info_timer);
}

static void cancel_track_change(void){
    track_info_pending = 0;
    pending_index_step = 0;
    pending_nav_step = 0;
    btstack_run_loop_remove_timer(&track_info_timer);
    btstack_run_loop_remove_timer(&track_info_timeout_timer);
}

// the phone reports a new track
static void media_track_changed(void){
    // right after our next/previous it is that command, otherwise the song ended by itself
    int step = (pending_nav_step && now_ms() - last_nav_time <= NAV_SETTLE_MS) ? pending_nav_step : +1;
    pending_nav_step = 0;
    if (!track_info_pending){
        btstack_strcpy(prev_title, sizeof(prev_title), cur_title);
        btstack_strcpy(prev_artist, sizeof(prev_artist), cur_artist);
        pending_index_step = 0;
    }
    pending_index_step += step;
    track_info_pending = 1;
    info_retries = 0;
    track_change_time = now_ms();
    cur_duration_ms = 0;                     // comes with the new track's info
    position_set(0, media_is_playing());     // the new track starts now
    schedule_track_info_request(TRACK_INFO_DELAY_MS);
    btstack_run_loop_remove_timer(&track_info_timeout_timer);
    btstack_run_loop_set_timer_handler(&track_info_timeout_timer, &track_info_timeout_handler);
    btstack_run_loop_set_timer(&track_info_timeout_timer, TRACK_INFO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&track_info_timeout_timer);
}

// remote command from the lock screen / car head unit -> forward to the phone
static void sink_media_command(media_command_t cmd){
    if (cmd == MEDIA_CMD_STOP){
        // iPod-mode head units send "stop" while changing tracks and at the end of what they
        // believe is the playlist. Forwarded, it ended the phone's stream: ignore it.
        log_info("media command stop ignored (head units send it around track changes)");
        last_stop_time = now_ms();
        return;
    }
    uint16_t cid = avrcp_connection.avrcp_cid;
    if (cid == 0){
        log_info("media command %d ignored: no AVRCP connection", (int) cmd);
        return;
    }
    switch (cmd){
        case MEDIA_CMD_PLAY:
        case MEDIA_CMD_PAUSE:
        case MEDIA_CMD_TOGGLE: {
            media_command_t c = cmd;
            if (cmd == MEDIA_CMD_TOGGLE) c = media_is_playing() ? MEDIA_CMD_PAUSE : MEDIA_CMD_PLAY;
            log_info("media: %s", c == MEDIA_CMD_PAUSE ? "pause" : "play");
            if (c == MEDIA_CMD_PAUSE) last_pause_cmd_time = now_ms();
            avrcp_queue_transport(c);
            schedule_state_check();
            break;
        }
        case MEDIA_CMD_NEXT:
            // the playlist position moves when the phone reports the new track (with its info)
            avrcp_queue_transport(MEDIA_CMD_NEXT);
            pending_nav_step = +1;
            last_nav_time = now_ms();
            break;
        case MEDIA_CMD_PREVIOUS:
            // the phone goes back a track (-> track changed) or restarts this one
            avrcp_queue_transport(MEDIA_CMD_PREVIOUS);
            pending_nav_step = -1;
            last_nav_time = now_ms();
            request_play_status();            // re-sync the time if it just restarted
            break;
        default:
            break;
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
    log_info("auto play: sending Play");
    avrcp_queue_transport(MEDIA_CMD_PLAY);
}

static void autoplay_start(void){
    btstack_run_loop_remove_timer(&autoplay_timer);
    autoplay_done = 0;
    autoplay_retries = 0;
    remote_playing = 0;
    remote_status_known = 0;
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
        uint32_t pos = position_now_ms();
        char song[24];      // log_info takes a limited number of arguments
        snprintf(song, sizeof(song), "%u:%02u/%u:%02u", pos / 60000, (pos / 1000) % 60,
                 cur_duration_ms / 60000, (cur_duration_ms / 1000) % 60);
        log_info("audio: %s last %u s: %u dropouts, %u SBC frames dropped, %u skips, %u taken back from other apps; buffered %u ms; song %s",
                 problems ? "PROBLEMS" : "ok,", STATS_INTERVAL_MS / 1000, new_underruns,
                 stat_dropped_sbc, stat_trims, new_reclaims, frames_to_ms(total), song);
    }
    stat_dropped_sbc = 0;
    stat_trims = 0;
    btstack_run_loop_set_timer(ts, STATS_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

// only with car controls on, and never after the media code crashed once
static void pause_music_app(void){
    if (!media_controls_enabled || media_state <= 0) return;
    media_guard_set("pause_music");
    media_controls_pause_system_music();
    media_guard_clear();
}

static void music_service_report(void);

// Another app took the audio - in a car usually the Music app, which the head unit tells to
// play after a track change. Pause it, then the audio driver takes the audio back.
// Returns 0 to leave the audio to the other app instead.
static int audio_before_reclaim(void){
    if (music_service && !audio_stream_started && !external_power){
        // just the music service's silence and no charger: someone uses the iPod for something
        // else. Let them; the music service comes back once power is connected (the phone's
        // audio takes over whenever it plays).
        log_info("music service: another app is playing on battery, leaving the audio to it until the iPod is charging");
        music_yielded = 1;
        media_apply_active();
        music_service_report();
        return 0;
    }
    if (a2dp_connection.connected && (media_is_playing() || published_playing)){
        // the head unit said "stop" and then started the Music app: its way of doing "next"
        if (last_stop_time && now_ms() - last_stop_time < STOP_AS_SKIP_MS){
            log_info("media: stop + Music app starting = the head unit's next, skipping on the phone");
            last_stop_time = 0;
            sink_media_command(MEDIA_CMD_NEXT);
        }
    } else if (a2dp_connection.connected){
        // phone paused and the head unit starts the Music app: that was its "play"
        // (or switching to the iPod / USB source), so play the phone instead
        log_info("media: the head unit started the Music app while the phone is paused, resuming the phone");
        sink_media_command(MEDIA_CMD_PLAY);
    }
    pause_music_app();
    return 1;
}

// Playing again: re-publish the track so we are the Now Playing app again and the head unit's
// buttons come back to us.
static void audio_after_reclaim(void){
    media_publish();
    if (a2dp_connection.connected) headunit_kick_schedule();   // the head unit may have switched to the Music app's audio
}

// --- audio output: the phone's audio, silence, or nothing ---
//
// While the phone is paused the queue keeps playing silently (keep-alive, up to
// KEEPALIVE_MAX_MS): iOS keeps treating us as the active player, the way it does for a
// paused music app, and if the head unit starts the Music app meanwhile we notice
// (interruption) and take the audio back. The music service plays silence whenever no
// phone audio plays, with no time limit.

#define KEEPALIVE_MAX_MS (10 * 60 * 1000)
static btstack_timer_source_t keepalive_timer;
static int keepalive_armed;

static void audio_configure(uint32_t rate){
    if (!audio_sink) return;
    audio_sink->init(NUM_CHANNELS, rate, &playback_handler);     // keeps a running queue of the same format
    audio_sink->set_volume(volume_percentage * 127 / 100);
    btstack_audio_ios_set_available_handler(&sink_pcm_frames_available);
    btstack_audio_ios_set_reclaim_handlers(&audio_before_reclaim, &audio_after_reclaim);
    audio_configured = 1;
}

static void audio_play_silence(void){
    if (!audio_sink) return;
    if (!audio_configured) audio_configure(silence_rate);
    btstack_audio_ios_set_silence_fill(1);
    if (!btstack_audio_ios_is_started()) audio_sink->start_stream();
}

static void audio_close(void){
    if (audio_sink && audio_configured) audio_sink->close();
    audio_configured = 0;
}

// no phone audio playing: keep the queue playing silence, or stop it
static void audio_idle_update(void){
    if (audio_stream_started) return;
    if (music_silence_wanted() || keepalive_armed){
        audio_play_silence();
    } else if (!media_initialized){
        audio_close();
    } else if (audio_sink && btstack_audio_ios_is_started()){
        audio_sink->stop_stream();
    }
}

static void keepalive_disarm(void){
    keepalive_armed = 0;
    btstack_run_loop_remove_timer(&keepalive_timer);
}

static void keepalive_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    keepalive_armed = 0;
    if (audio_stream_started) return;
    if (!music_silence_wanted()) log_info("audio: paused for %u min, stopping the silent stream", KEEPALIVE_MAX_MS / 60000);
    audio_idle_update();
}

static void keepalive_arm(void){
    keepalive_armed = 1;
    btstack_run_loop_remove_timer(&keepalive_timer);
    btstack_run_loop_set_timer_handler(&keepalive_timer, &keepalive_timeout_handler);
    btstack_run_loop_set_timer(&keepalive_timer, KEEPALIVE_MAX_MS);
    btstack_run_loop_add_timer(&keepalive_timer);
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

    // a queue already playing silence in this format keeps running: the audio follows it
    silence_rate = configuration->sampling_frequency;
    audio_configure(configuration->sampling_frequency);
    audio_stream_started = 0;
    media_initialized = 1;

    stat_dropped_sbc = stat_trims = stat_underruns_seen = stat_reclaims_seen = 0;
    stat_ticks = 0;
    btstack_run_loop_remove_timer(&stats_timer);
    btstack_run_loop_set_timer_handler(&stats_timer, &stats_timer_handler);
    btstack_run_loop_set_timer(&stats_timer, STATS_INTERVAL_MS);
    btstack_run_loop_add_timer(&stats_timer);
}

// enough of the phone's audio is buffered: play it
static void media_processing_start(void){
    if (!media_initialized) return;
    audio_stream_started = 1;
    keepalive_disarm();
    if (music_yielded){
        music_yielded = 0;          // the phone's audio takes the audio back in any case
        music_service_report();
    }
    if (!kick_done){
        kick_done = 1;
        // with the music service the head unit has been playing our silence: its audio is on
        if (!music_service) headunit_kick_schedule();
    }
    // a queue playing silence (keep-alive, music service): the audio follows it, no gap
    int seamless = btstack_audio_ios_is_running();
    uint32_t queued = seamless ? btstack_audio_ios_get_queued_frames() : 0;
    total_avg = (int32_t) (sink_pcm_frames_available() + queued);
    log_info("audio: starting playback with %u ms buffered%s", frames_to_ms(total_avg),
             seamless ? ", right after the silence" : "");
    btstack_audio_ios_set_silence_fill(0);
    if (seamless) btstack_audio_ios_kick();
    else if (audio_sink) audio_sink->start_stream();
}

// the phone paused (stream suspended)
static void media_processing_suspend(void){
    if (!media_initialized) return;
    int was_playing = audio_stream_started && btstack_audio_ios_is_running();
    audio_stream_started = 0;          // resuming waits for audio to be buffered again
    btstack_ring_buffer_reset(&decoded_audio_ring_buffer);
    btstack_ring_buffer_reset(&sbc_frame_ring_buffer);
    if (was_playing) keepalive_arm();
    audio_idle_update();
}

// the phone's stream ended (released, disconnected, reconfigured)
static void media_processing_close(void){
    keepalive_disarm();
    if (media_initialized){
        media_initialized = 0;
        audio_stream_started = 0;
        sbc_frame_size = 0;
        btstack_run_loop_remove_timer(&stats_timer);
    }
    audio_idle_update();       // the music service keeps the queue playing silence
}

// --- music service ---

static const char * music_service_state(void){
    if (!music_service) return "off";
    if (battery_stop)   return "battery";
    if (music_yielded)  return "yielded";
    return "running";
}

static void music_service_report(void){
    sink_status_set_service(music_service_state());
}

static void music_service_update(void){
    // left the audio to another app on battery: take it back once the iPod is charging
    // (plugged into the car, typically: the head unit is about to look for a player)
    if (music_yielded && external_power){
        music_yielded = 0;
        if (music_service){
            log_info("music service: external power, taking the audio back");
            pause_music_app();
        }
    }
    audio_idle_update();
    media_apply_active();
    music_service_report();
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
        // a queue playing silence covers most of the buffering: switch over right after it
        uint32_t needed = btstack_audio_ios_is_running() ? SEAMLESS_START_FRAMES : START_FRAMES;
        if (sink_pcm_frames_available() >= needed) media_processing_start();
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
            reset_track_info();
            cancel_track_change();
            have_track_info = 0;
            // with the music service the head unit has seen the stand-in at this position:
            // the playlist position goes on from there
            if (!music_service) queue_index = QUEUE_START_INDEX;
            published_playing = 0;
            last_nav_time = last_stop_time = last_pause_cmd_time = 0;
            play_status_logged = 0;
            kick_done = 0;
            last_kick_time = 0;
            start_time_refresh();
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
            media_playing_changed();
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            a2dp_connection.playing = 0;
            media_processing_suspend();
            sink_status_set_state(SINK_STATE_CONNECTED);
            media_playing_changed();
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
            reset_track_info();
            cancel_track_change();
            have_track_info = 0;
            published_playing = 0;
            btstack_run_loop_remove_timer(&time_refresh_timer);
            btstack_run_loop_remove_timer(&headunit_kick_timer);
            btstack_run_loop_remove_timer(&state_check_timer);
            btstack_run_loop_remove_timer(&playing_debounce_timer);
            btstack_run_loop_remove_timer(&play_status_timer);
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
            btstack_run_loop_remove_timer(&play_status_timer);
            avrcp_queue_reset();
            cancel_track_change();
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
            avrcp_request_info();
            request_play_status();
            start_play_status_polling();
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            if (avrcp_subevent_notification_track_changed_get_command_type(packet) == AVRCP_CTYPE_RESPONSE_CHANGED_STABLE){
                media_track_changed();
            } else {
                avrcp_request_info();     // first report after registering
            }
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            remote_playing = avrcp_subevent_notification_playback_status_changed_get_play_status(packet) == AVRCP_PLAYBACK_STATUS_PLAYING;
            remote_status_known = 1;
            media_playing_changed();
            request_play_status();     // exact position at the pause / resume
            break;
        case AVRCP_SUBEVENT_PLAY_STATUS: {
            if (track_info_pending){        // may still describe the old track; asked again after
                avrcp_pump();
                break;
            }
            uint32_t length   = avrcp_subevent_play_status_get_song_length(packet);
            uint32_t position = avrcp_subevent_play_status_get_song_position(packet);
            uint8_t  status   = avrcp_subevent_play_status_get_play_status(packet);
            if (length != 0 && length != 0xFFFFFFFFu) cur_duration_ms = length;
            if (position != 0xFFFFFFFFu) position_set(position, media_is_playing());
            if (!play_status_logged){
                play_status_logged = 1;
                if (length != 0 && length != 0xFFFFFFFFu){
                    log_info("track time from the phone: %u:%02u of %u:%02u, status %u",
                             position / 60000, (position / 1000) % 60, length / 60000, (length / 1000) % 60, status);
                } else {
                    log_info("track time from the phone: length unknown (0x%08x), status %u", length, status);
                }
            }
            if (status == AVRCP_PLAYBACK_STATUS_PLAYING || status == AVRCP_PLAYBACK_STATUS_PAUSED || status == AVRCP_PLAYBACK_STATUS_STOPPED){
                int playing = status == AVRCP_PLAYBACK_STATUS_PLAYING;
                int known_before = remote_status_known;
                remote_status_known = 1;
                if (playing != remote_playing || !known_before){
                    remote_playing = playing;
                    media_playing_changed();
                    avrcp_pump();
                    break;
                }
            }
            media_publish();
            avrcp_pump();
            break;
        }
        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            if (track_info_pending){
                int stale = strcmp(cur_title, prev_title) == 0 && strcmp(cur_artist, prev_artist) == 0;
                if (stale && info_retries < TRACK_INFO_MAX_RETRIES){
                    info_retries++;      // the phone hasn't updated its info yet: ask again
                    schedule_track_info_request(TRACK_INFO_RETRY_MS);
                } else {
                    finish_track_change();
                }
            } else {
                have_track_info = 1;     // with the music service: replaces the stand-in now
                media_publish();
                request_play_status();
            }
            avrcp_pump();
            break;
        case AVRCP_SUBEVENT_OPERATION_COMPLETE:
            avrcp_pump();       // a button press went through: send the next queued request
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            copy_avrcp_value(value, sizeof(value), avrcp_subevent_now_playing_title_info_get_value(packet),
                             avrcp_subevent_now_playing_title_info_get_value_len(packet));
            sink_status_set_track(value, NULL);
            btstack_strcpy(cur_title, sizeof(cur_title), value);
            media_publish();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            copy_avrcp_value(value, sizeof(value), avrcp_subevent_now_playing_artist_info_get_value(packet),
                             avrcp_subevent_now_playing_artist_info_get_value_len(packet));
            sink_status_set_track(NULL, value);
            btstack_strcpy(cur_artist, sizeof(cur_artist), value);
            media_publish();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            copy_avrcp_value(value, sizeof(value), avrcp_subevent_now_playing_album_info_get_value(packet),
                             avrcp_subevent_now_playing_album_info_get_value_len(packet));
            btstack_strcpy(cur_album, sizeof(cur_album), value);
            media_publish();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_SONG_LENGTH_MS_INFO: {
            uint32_t length = avrcp_subevent_now_playing_song_length_ms_info_get_song_length(packet);
            if (length != 0 && length != 0xFFFFFFFFu){
                cur_duration_ms = length;
                media_publish();
            }
            break;
        }
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

void sink_init_audio(const btstack_audio_sink_t * audio){
    audio_sink = audio;
    music_service_report();
}

void sink_set_music_service(int enabled){
    enabled = enabled ? 1 : 0;
    if (enabled == music_service) return;
    music_service = enabled;
    music_yielded = 0;
    log_info("music service: %s", enabled ? "on, the receiver stays the playing music app" : "off");
    music_service_update();
}

void sink_set_power_state(int on_external_power, int battery_percent){
    on_external_power = on_external_power ? 1 : 0;
    int stop = battery_stop;
    if (on_external_power) stop = 0;
    else if (battery_percent >= 0 && battery_percent < MUSIC_SERVICE_MIN_BATTERY) stop = 1;
    if (on_external_power == external_power && stop == battery_stop) return;
    int power_connected = on_external_power && !external_power;
    external_power = on_external_power;
    if (stop != battery_stop){
        battery_stop = stop;
        if (music_service && stop){
            log_info("music service: battery at %d%% and no charger, pausing it until the iPod is charging", battery_percent);
        } else if (music_service){
            log_info("music service: charging, running again");
        }
    }
    if (!music_service && !music_yielded) return;
    // ignition on: for the head unit that is connecting now the stand-in "track" starts now
    if (power_connected && published_kind == PUBLISHED_PLACEHOLDER) placeholder_since = now_ms();
    music_service_update();
}

void sink_setup(const char * local_name){
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
    media_active = 0;
    music_service = 0;
    media_processing_close();      // music service off: closes the audio
}
