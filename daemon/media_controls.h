#ifndef MEDIA_CONTROLS_H
#define MEDIA_CONTROLS_H

#include <stdint.h>

// Makes a2dpsinkd iOS's "Now Playing" application while audio is streaming, so:
//  - the connected phone's track shows on the lock screen / Control Center, and
//  - transport buttons from the lock screen AND from accessories that drive the
//    Now Playing app over USB (iAP) - e.g. a car head unit's seek/next buttons -
//    are delivered here and forwarded to the phone over AVRCP.
//
// MediaPlayer / MediaRemote are loaded at runtime (dlopen), so a missing framework,
// class or method only makes media controls unavailable - it can't stop the daemon.
// All functions must be called on the BTstack (main) thread.

typedef enum {
    MEDIA_CMD_PLAY = 0,
    MEDIA_CMD_PAUSE,
    MEDIA_CMD_TOGGLE,
    MEDIA_CMD_NEXT,
    MEDIA_CMD_PREVIOUS,
    MEDIA_CMD_STOP,
} media_command_t;

// Called on the BTstack (main) thread when a remote command arrives.
typedef void (*media_command_handler_t)(media_command_t command);

// Load MediaPlayer, register command handlers (disabled). 1 = ready, 0 = unavailable.
int  media_controls_init(media_command_handler_t handler);

// Enable/disable being Now Playing. Disabling clears the lock-screen info.
void media_controls_set_active(int active);

typedef struct {
    const char * title;
    const char * artist;
    const char * album;
    uint32_t     duration_ms;    // 0 = unknown
    uint32_t     position_ms;    // elapsed time right now
    int          playing;
    uint32_t     queue_index;    // 0-based position in the playlist shown to iOS / accessories
    uint32_t     queue_count;    // 0 = don't publish a playlist
} media_now_playing_t;

// Publish the track shown on the lock screen, Control Center and the head unit.
void media_controls_set_now_playing(const media_now_playing_t * info);

// Re-publish the current Now Playing info (after taking the audio back from another app).
void media_controls_refresh(void);

// Pause the built-in Music app's player if it is playing (MPMusicPlayerController, so this
// can never pause us). Returns 1 if it was playing. Loads MediaPlayer itself if needed.
int  media_controls_pause_system_music(void);

#endif
