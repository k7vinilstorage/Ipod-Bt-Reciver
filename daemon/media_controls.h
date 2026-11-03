#ifndef MEDIA_CONTROLS_H
#define MEDIA_CONTROLS_H

// Makes a2dpsinkd iOS's "Now Playing" application while audio is streaming, so:
//  - the connected phone's track shows on the lock screen / Control Center, and
//  - transport buttons from the lock screen AND from accessories that drive the
//    Now Playing app over USB (iAP) - e.g. a car head unit's seek/next buttons -
//    are delivered here and forwarded to the phone over AVRCP.
//
// Depends on a CoreFoundation run loop running on the main thread (command handlers
// are delivered on the main dispatch queue). Uses the public MediaPlayer framework
// plus the private MediaRemote call that lets a UI-less daemon be Now Playing.

typedef enum {
    MEDIA_CMD_PLAY,
    MEDIA_CMD_PAUSE,
    MEDIA_CMD_TOGGLE,
    MEDIA_CMD_NEXT,
    MEDIA_CMD_PREVIOUS,
    MEDIA_CMD_STOP,
} media_command_t;

// Called on the main (BTstack) thread when a remote command arrives.
typedef void (*media_command_handler_t)(media_command_t command);

// Register command handlers once at startup (handlers start disabled).
void media_controls_init(media_command_handler_t handler);

// Enable/disable being Now Playing. Disabling clears the lock-screen info.
void media_controls_set_active(int active);

// Publish/refresh the track shown on the lock screen and head unit.
void media_controls_set_metadata(const char * title, const char * artist, int playing);
void media_controls_set_playing(int playing);

#endif
