/*
 * media_controls.m
 *
 * Now Playing / remote command integration (see media_controls.h).
 *
 * Robustness rules:
 *  - MediaPlayer and MediaRemote are loaded at runtime with dlopen, never linked. If a
 *    framework, class or symbol is missing, media controls are unavailable and the
 *    receiver keeps working.
 *  - Every command / property is checked with respondsToSelector: before use (some only
 *    exist on newer iOS versions), and every call into MediaPlayer is in @try/@catch.
 *  - BTstack keeps its posix (select) run loop on the main thread. MediaPlayer delivers
 *    callbacks through the main thread's CoreFoundation run loop / main dispatch queue,
 *    so a BTstack timer pumps that run loop without blocking (every 50 ms while active).
 *  - A remote command only bumps an atomic counter; it is executed from the pump timer on
 *    the BTstack thread, whichever thread MediaPlayer called the handler on.
 *
 * Manual reference counting (no ARC).
 */

#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "media_controls.h"

#define MEDIAPLAYER_PATH "/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer"
#define MEDIAREMOTE_PATH "/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote"

#define PUMP_ACTIVE_MS   50
#define PUMP_IDLE_MS     500
#define MAX_REPEAT       3      // presses of the same button handled per pump

// The MediaPlayer API we use, declared locally so nothing is linked at build time.
@protocol A2DPRemoteCommand <NSObject>
- (id)addTargetWithHandler:(NSInteger (^)(id event))handler;
- (void)setEnabled:(BOOL)enabled;
@end

@protocol A2DPNowPlayingInfoCenter <NSObject>
- (void)setNowPlayingInfo:(NSDictionary *)info;
@end

@protocol A2DPMusicPlayer <NSObject>
- (NSInteger)playbackState;   // MPMusicPlaybackState
- (void)pause;
@end

typedef void (*mr_set_can_be_now_playing_t)(Boolean);

// commands forwarded to the phone, in media_command_t order
static const char * forwarded_getters[] = {
    "playCommand",              // MEDIA_CMD_PLAY
    "pauseCommand",             // MEDIA_CMD_PAUSE
    "togglePlayPauseCommand",   // MEDIA_CMD_TOGGLE
    "nextTrackCommand",         // MEDIA_CMD_NEXT
    "previousTrackCommand",     // MEDIA_CMD_PREVIOUS
    "stopCommand",              // MEDIA_CMD_STOP
};
#define NUM_FORWARDED (sizeof(forwarded_getters) / sizeof(forwarded_getters[0]))

// commands we can't forward: kept disabled so the car UI doesn't offer them
static const char * disabled_getters[] = {
    "seekForwardCommand", "seekBackwardCommand", "skipForwardCommand", "skipBackwardCommand",
    "changePlaybackPositionCommand", "changePlaybackRateCommand", "ratingCommand",
    "likeCommand", "dislikeCommand", "bookmarkCommand",
};
#define NUM_DISABLED (sizeof(disabled_getters) / sizeof(disabled_getters[0]))

static media_command_handler_t g_handler;
static int        g_ready;
static int        g_active;
static Class      g_info_center_class;
static id         g_commands[NUM_FORWARDED];   // retained
static NSString * g_key_title;
static NSString * g_key_artist;
static NSString * g_key_rate;
static NSString * g_title;                     // retained
static NSString * g_artist;                    // retained
static int        g_playing;
static volatile uint32_t g_pending[NUM_FORWARDED];
static btstack_timer_source_t g_pump_timer;

static id send_id(id receiver, const char * selector){
    SEL sel = sel_registerName(selector);
    if (!receiver || ![receiver respondsToSelector:sel]) return nil;
    return ((id (*)(id, SEL)) objc_msgSend)(receiver, sel);
}

static NSString * string_constant(void * handle, const char * symbol, NSString * fallback){
    NSString ** p = (NSString **) dlsym(handle, symbol);
    return (p && *p) ? *p : fallback;
}

static void log_exception(const char * where, NSException * e){
    const char * name   = [[e name] UTF8String];
    const char * reason = [[e reason] UTF8String];
    log_error("media_controls: %s threw %s: %s", where, name ? name : "?", reason ? reason : "");
}

static void schedule_pump(void){
    btstack_run_loop_remove_timer(&g_pump_timer);
    btstack_run_loop_set_timer(&g_pump_timer, g_active ? PUMP_ACTIVE_MS : PUMP_IDLE_MS);
    btstack_run_loop_add_timer(&g_pump_timer);
}

static void pump_handler(btstack_timer_source_t * ts){
    (void) ts;
    // let MediaPlayer's main-queue / run-loop work run, without blocking
    @autoreleasepool {
        @try {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
        } @catch (NSException * e){
            log_exception("run loop", e);
        }
    }
    // execute queued remote commands on this (BTstack) thread
    unsigned int i;
    for (i = 0; i < NUM_FORWARDED; i++){
        uint32_t n = __atomic_exchange_n(&g_pending[i], 0, __ATOMIC_ACQ_REL);
        if (n > MAX_REPEAT) n = MAX_REPEAT;
        while (n--){
            log_info("media_controls: remote command %s", forwarded_getters[i]);
            if (g_handler) g_handler((media_command_t) i);
        }
    }
    schedule_pump();
}

static void set_forwarded_enabled(BOOL enabled){
    unsigned int i;
    for (i = 0; i < NUM_FORWARDED; i++){
        if (g_commands[i]) [(id<A2DPRemoteCommand>) g_commands[i] setEnabled:enabled];
    }
}

static void set_now_playing_info(NSDictionary * info){
    id center = send_id((id) g_info_center_class, "defaultCenter");
    if (center && [center respondsToSelector:@selector(setNowPlayingInfo:)]){
        [(id<A2DPNowPlayingInfoCenter>) center setNowPlayingInfo:info];
    }
}

int media_controls_init(media_command_handler_t handler){
    g_handler = handler;
    if (g_ready) return 1;

    void * mp = dlopen(MEDIAPLAYER_PATH, RTLD_LAZY | RTLD_GLOBAL);
    if (!mp){
        const char * err = dlerror();
        log_error("media_controls: cannot load MediaPlayer: %s", err ? err : "?");
        return 0;
    }
    Class center_class  = objc_getClass("MPRemoteCommandCenter");
    g_info_center_class = objc_getClass("MPNowPlayingInfoCenter");
    if (!center_class || !g_info_center_class){
        log_error("media_controls: MediaPlayer classes missing (needs iOS 7.1 or later)");
        return 0;
    }
    g_key_title  = string_constant(mp, "MPMediaItemPropertyTitle",  @"title");
    g_key_artist = string_constant(mp, "MPMediaItemPropertyArtist", @"artist");
    g_key_rate   = string_constant(mp, "MPNowPlayingInfoPropertyPlaybackRate", @"MPNowPlayingInfoPropertyPlaybackRate");

    unsigned int registered = 0;
    @try {
        @autoreleasepool {
            // let this UI-less process become the Now Playing app (private, optional)
            void * mr = dlopen(MEDIAREMOTE_PATH, RTLD_LAZY | RTLD_GLOBAL);
            mr_set_can_be_now_playing_t set_can = mr ?
                (mr_set_can_be_now_playing_t) dlsym(mr, "MRMediaRemoteSetCanBeNowPlayingApplication") : NULL;
            if (set_can){
                set_can(true);
            } else {
                log_info("media_controls: MRMediaRemoteSetCanBeNowPlayingApplication not available");
            }

            id center = send_id((id) center_class, "sharedCommandCenter");
            if (center){
                unsigned int i;
                for (i = 0; i < NUM_FORWARDED; i++){
                    id cmd = send_id(center, forwarded_getters[i]);
                    if (!cmd || ![cmd respondsToSelector:@selector(addTargetWithHandler:)]) continue;
                    unsigned int index = i;
                    NSInteger (^block)(id) = ^NSInteger(id event){
                        (void) event;
                        __atomic_fetch_add(&g_pending[index], 1, __ATOMIC_ACQ_REL);
                        return 0;   // MPRemoteCommandHandlerStatusSuccess
                    };
                    [(id<A2DPRemoteCommand>) cmd addTargetWithHandler:[[block copy] autorelease]];
                    [(id<A2DPRemoteCommand>) cmd setEnabled:NO];
                    g_commands[i] = [cmd retain];
                    registered++;
                }
                for (i = 0; i < NUM_DISABLED; i++){
                    id cmd = send_id(center, disabled_getters[i]);
                    if (cmd && [cmd respondsToSelector:@selector(setEnabled:)]){
                        [(id<A2DPRemoteCommand>) cmd setEnabled:NO];
                    }
                }
            }
        }
    } @catch (NSException * e){
        log_exception("init", e);
        return 0;
    }
    if (registered == 0){
        log_error("media_controls: no remote commands available");
        return 0;
    }

    log_info("media_controls: ready, %u remote commands registered", registered);
    g_ready = 1;
    btstack_run_loop_set_timer_handler(&g_pump_timer, &pump_handler);
    schedule_pump();
    return 1;
}

static void publish(void){
    if (!g_ready || !g_active) return;
    @try {
        @autoreleasepool {
            NSMutableDictionary * info = [NSMutableDictionary dictionary];
            if (g_title)  [info setObject:g_title  forKey:g_key_title];
            if (g_artist) [info setObject:g_artist forKey:g_key_artist];
            [info setObject:[NSNumber numberWithDouble:(g_playing ? 1.0 : 0.0)] forKey:g_key_rate];
            set_now_playing_info(info);
        }
    } @catch (NSException * e){
        log_exception("publish", e);
    }
}

void media_controls_set_active(int active){
    if (!g_ready) return;
    active = active ? 1 : 0;
    if (active == g_active) return;
    g_active = active;
    log_info("media_controls: %s", active ? "active (Now Playing)" : "inactive");
    @try {
        @autoreleasepool {
            set_forwarded_enabled(active ? YES : NO);
            if (!active) set_now_playing_info(nil);
        }
    } @catch (NSException * e){
        log_exception("set_active", e);
    }
    if (active) publish();
    schedule_pump();
}

void media_controls_set_metadata(const char * title, const char * artist, int playing){
    if (!g_ready) return;
    @autoreleasepool {
        [g_title release];
        [g_artist release];
        g_title  = (title  && title[0])  ? [[NSString alloc] initWithUTF8String:title]  : nil;
        g_artist = (artist && artist[0]) ? [[NSString alloc] initWithUTF8String:artist] : nil;
        g_playing = playing ? 1 : 0;
    }
    publish();
}

void media_controls_set_playing(int playing){
    if (!g_ready) return;
    playing = playing ? 1 : 0;
    if (playing == g_playing) return;
    g_playing = playing;
    publish();
}

void media_controls_refresh(void){
    publish();
}

int media_controls_pause_system_music(void){
    static void * mp;
    if (!mp) mp = dlopen(MEDIAPLAYER_PATH, RTLD_LAZY | RTLD_GLOBAL);
    if (!mp) return 0;
    Class player_class = objc_getClass("MPMusicPlayerController");
    if (!player_class) return 0;

    int paused = 0;
    @try {
        @autoreleasepool {
            id player = send_id((id) player_class, "systemMusicPlayer");     // iOS 8+
            if (!player) player = send_id((id) player_class, "iPodMusicPlayer");
            if (player && [player respondsToSelector:@selector(playbackState)]
                       && [player respondsToSelector:@selector(pause)]){
                NSInteger state = [(id<A2DPMusicPlayer>) player playbackState];
                // 0 stopped, 1 playing, 2 paused, 3 interrupted, 4/5 seeking
                if (state != 0 && state != 2){
                    [(id<A2DPMusicPlayer>) player pause];
                    paused = 1;
                }
                log_info("media_controls: Music app state %d%s", (int) state, paused ? ", paused it" : "");
            }
        }
    } @catch (NSException * e){
        log_exception("pause Music", e);
    }
    return paused;
}
