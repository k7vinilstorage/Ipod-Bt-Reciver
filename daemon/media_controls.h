#ifndef MEDIA_CONTROLS_H
#define MEDIA_CONTROLS_H

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

// Publish/refresh the track shown on the lock screen and head unit.
void media_controls_set_metadata(const char * title, const char * artist, int playing);
void media_controls_set_playing(int playing);

#endif
