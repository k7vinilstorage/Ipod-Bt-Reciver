#include "sink_status.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char * state_names[] = {
    "starting", "ready", "connected", "playing", "stopping", "error", "waiting_power",
};

static sink_state_t state = SINK_STATE_STARTING;
static char error_message[128];
static char local_name[64];
static char local_addr[24];
static char remote_addr[24];
static char remote_name[64];
static char codec[64];
static char title[128];
static char artist[128];
static char reconnecting[64];
static char media[16];
static uint32_t timing_duration_ms;
static uint32_t timing_position_ms;
static int      timing_playing;
static unsigned long long timing_stamp_ms;   // unix time of timing_position_ms

static void copy_clean(char * dst, size_t size, const char * src){
    size_t i = 0;
    if (src){
        for (; src[i] && i + 1 < size; i++){
            char c = src[i];
            dst[i] = (c == '\n' || c == '\r') ? ' ' : c;
        }
    }
    dst[i] = 0;
}

void sink_status_flush(void){
    char tmp_path[] = SINK_STATUS_FILE ".tmp";
    FILE * f = fopen(tmp_path, "w");
    if (!f) return;
    fprintf(f, "state=%s\n", state_names[state]);
    fprintf(f, "pid=%d\n", (int) getpid());
    fprintf(f, "name=%s\n", local_name);
    fprintf(f, "address=%s\n", local_addr);
    fprintf(f, "remote_address=%s\n", remote_addr);
    fprintf(f, "remote_name=%s\n", remote_name);
    fprintf(f, "codec=%s\n", codec);
    fprintf(f, "title=%s\n", title);
    fprintf(f, "artist=%s\n", artist);
    fprintf(f, "reconnecting=%s\n", reconnecting);
    fprintf(f, "media=%s\n", media);
    fprintf(f, "duration_ms=%u\n", timing_duration_ms);
    fprintf(f, "position_ms=%u\n", timing_position_ms);
    fprintf(f, "position_time=%llu\n", timing_stamp_ms);
    fprintf(f, "playing=%d\n", timing_playing);
    fprintf(f, "error=%s\n", error_message);
    fclose(f);
    chmod(tmp_path, 0644);
    rename(tmp_path, SINK_STATUS_FILE);
}

void sink_status_set_state(sink_state_t new_state){
    state = new_state;
    if (state != SINK_STATE_ERROR) error_message[0] = 0;
    if (state != SINK_STATE_CONNECTED && state != SINK_STATE_PLAYING){
        remote_addr[0] = remote_name[0] = codec[0] = title[0] = artist[0] = 0;
        timing_duration_ms = timing_position_ms = 0;
        timing_playing = 0;
    }
    if (state != SINK_STATE_READY) reconnecting[0] = 0;
    sink_status_flush();
}

void sink_status_set_error(const char * message){
    copy_clean(error_message, sizeof(error_message), message);
    state = SINK_STATE_ERROR;
    sink_status_flush();
}

void sink_status_set_name(const char * name){
    copy_clean(local_name, sizeof(local_name), name);
    sink_status_flush();
}

void sink_status_set_local_addr(const char * addr){
    copy_clean(local_addr, sizeof(local_addr), addr);
    sink_status_flush();
}

void sink_status_set_remote(const char * addr, const char * name){
    if (addr) copy_clean(remote_addr, sizeof(remote_addr), addr);
    if (name) copy_clean(remote_name, sizeof(remote_name), name);
    sink_status_flush();
}

void sink_status_set_codec(const char * description){
    copy_clean(codec, sizeof(codec), description);
    sink_status_flush();
}

void sink_status_set_track(const char * new_title, const char * new_artist){
    if (new_title)  copy_clean(title, sizeof(title), new_title);
    if (new_artist) copy_clean(artist, sizeof(artist), new_artist);
    sink_status_flush();
}

void sink_status_set_reconnecting(const char * device){
    copy_clean(reconnecting, sizeof(reconnecting), device);
    sink_status_flush();
}

void sink_status_set_media(const char * state){
    char previous[sizeof(media)];
    memcpy(previous, media, sizeof(media));
    copy_clean(media, sizeof(media), state);
    if (strcmp(previous, media) != 0) sink_status_flush();
}

void sink_status_set_timing(uint32_t duration_ms, uint32_t position_ms, int playing){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    timing_stamp_ms    = (unsigned long long) tv.tv_sec * 1000ull + (unsigned long long) (tv.tv_usec / 1000);
    timing_duration_ms = duration_ms;
    timing_position_ms = position_ms;
    timing_playing     = playing ? 1 : 0;
    sink_status_flush();
}
