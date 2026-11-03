/*
 * media_controls.m
 *
 * Now Playing / remote command integration (see media_controls.h).
 * Written for manual reference counting (no ARC).
 */

#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include "media_controls.h"
#include "btstack_debug.h"

// MediaRemote (private): allow this UI-less process to become the Now Playing app.
extern void MRMediaRemoteSetCanBeNowPlayingApplication(Boolean can);

static media_command_handler_t g_handler;
static int       g_registered;
static int       g_active;
static NSString * g_title;
static NSString * g_artist;
static int       g_playing;

static MPRemoteCommandHandlerStatus dispatch_cmd(media_command_t cmd){
    log_info("media_controls: remote command %d", (int) cmd);
    if (g_handler) g_handler(cmd);
    return MPRemoteCommandHandlerStatusSuccess;
}

static void set_all_enabled(BOOL enabled){
    MPRemoteCommandCenter * c = [MPRemoteCommandCenter sharedCommandCenter];
    c.playCommand.enabled            = enabled;
    c.pauseCommand.enabled           = enabled;
    c.togglePlayPauseCommand.enabled = enabled;
    c.nextTrackCommand.enabled       = enabled;
    c.previousTrackCommand.enabled   = enabled;
    c.stopCommand.enabled            = enabled;
    // commands we can't forward: always off, so the car UI doesn't offer them
    c.seekForwardCommand.enabled            = NO;
    c.seekBackwardCommand.enabled           = NO;
    c.skipForwardCommand.enabled            = NO;
    c.skipBackwardCommand.enabled           = NO;
    c.changePlaybackPositionCommand.enabled = NO;
    c.ratingCommand.enabled                 = NO;
    c.likeCommand.enabled                   = NO;
    c.dislikeCommand.enabled                = NO;
    c.bookmarkCommand.enabled               = NO;
}

void media_controls_init(media_command_handler_t handler){
    g_handler = handler;
    if (g_registered) return;
    g_registered = 1;

    MRMediaRemoteSetCanBeNowPlayingApplication(true);

    MPRemoteCommandCenter * c = [MPRemoteCommandCenter sharedCommandCenter];
    [c.playCommand            addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e){ (void)e; return dispatch_cmd(MEDIA_CMD_PLAY);     }];
    [c.pauseCommand           addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e){ (void)e; return dispatch_cmd(MEDIA_CMD_PAUSE);    }];
    [c.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e){ (void)e; return dispatch_cmd(MEDIA_CMD_TOGGLE);   }];
    [c.nextTrackCommand       addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e){ (void)e; return dispatch_cmd(MEDIA_CMD_NEXT);     }];
    [c.previousTrackCommand   addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e){ (void)e; return dispatch_cmd(MEDIA_CMD_PREVIOUS); }];
    [c.stopCommand            addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e){ (void)e; return dispatch_cmd(MEDIA_CMD_STOP);     }];
    set_all_enabled(NO);

    log_info("media_controls: command handlers registered");
}

static void publish(void){
    if (!g_active) return;
    NSMutableDictionary * info = [NSMutableDictionary dictionary];
    if (g_title)  [info setObject:g_title  forKey:MPMediaItemPropertyTitle];
    if (g_artist) [info setObject:g_artist forKey:MPMediaItemPropertyArtist];
    [info setObject:[NSNumber numberWithDouble:(g_playing ? 1.0 : 0.0)]
             forKey:MPNowPlayingInfoPropertyPlaybackRate];
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
}

void media_controls_set_active(int active){
    active = active ? 1 : 0;
    if (active == g_active) return;
    g_active = active;
    log_info("media_controls: %s", active ? "active (now playing)" : "inactive");
    set_all_enabled(active ? YES : NO);
    if (active){
        publish();
    } else {
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
    }
}

void media_controls_set_metadata(const char * title, const char * artist, int playing){
    @autoreleasepool {
        [g_title release];
        [g_artist release];
        g_title  = (title  && title[0])  ? [[NSString alloc] initWithUTF8String:title]  : nil;
        g_artist = (artist && artist[0]) ? [[NSString alloc] initWithUTF8String:artist] : nil;
        g_playing = playing ? 1 : 0;
        publish();
    }
}

void media_controls_set_playing(int playing){
    @autoreleasepool {
        g_playing = playing ? 1 : 0;
        publish();
    }
}
