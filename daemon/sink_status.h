#ifndef SINK_STATUS_H
#define SINK_STATUS_H

#include <stdint.h>

// Shared with the app (runs as mobile): flag/config files and the status file
#define SINK_DATA_DIR      "/var/mobile/Library/A2DPSink"
#define SINK_ENABLED_FLAG  SINK_DATA_DIR "/enabled"      // exists -> launchd keeps daemon running
#define SINK_FORGET_FLAG   SINK_DATA_DIR "/forget"       // app requests: delete all pairings
#define SINK_NAME_FILE     SINK_DATA_DIR "/name"         // Bluetooth name, one line
#define SINK_STATUS_FILE   SINK_DATA_DIR "/status"       // written by daemon, key=value lines
#define SINK_LAST_DEVICE   SINK_DATA_DIR "/last_device"  // BD_ADDR of last connected phone
#define SINK_LINK_KEY_DB   SINK_DATA_DIR "/pairings.tlv"
#define SINK_MEDIA_GUARD   SINK_DATA_DIR "/media_guard"  // exists -> media controls crashed last time
#define SINK_SETTINGS_FILE SINK_DATA_DIR "/settings"     // app writes: only_when_charging, auto_reconnect, auto_play, media_controls, music_service (all 0/1)

#define SINK_DEFAULT_NAME  "iPod Receiver"

typedef enum {
    SINK_STATE_STARTING = 0,
    SINK_STATE_READY,        // discoverable, waiting for a phone
    SINK_STATE_CONNECTED,
    SINK_STATE_PLAYING,
    SINK_STATE_STOPPING,
    SINK_STATE_ERROR,
    SINK_STATE_WAITING_POWER, // "only while charging" mode, no external power
} sink_state_t;

void sink_status_set_state(sink_state_t state);
void sink_status_set_error(const char * message);
void sink_status_set_name(const char * name);
void sink_status_set_local_addr(const char * addr);
void sink_status_set_remote(const char * addr, const char * name);
void sink_status_set_codec(const char * description);
void sink_status_set_track(const char * title, const char * artist);
// device we are trying to reconnect to, NULL/"" when not reconnecting
void sink_status_set_reconnecting(const char * device);
// media controls: "off", "ready", "active", "unavailable" or "failed"
void sink_status_set_media(const char * state);
// song timing for the app: position was measured now; it advances while playing
void sink_status_set_timing(uint32_t duration_ms, uint32_t position_ms, int playing);
// music service: "off", "running", "yielded" (another app has the audio) or "battery"
void sink_status_set_service(const char * state);

// write the status file now (otherwise written by the setters)
void sink_status_flush(void);

#endif
