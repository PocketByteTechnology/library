#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void pb_audio_init(void);

typedef struct {
    char title[64];
    char artist[64];
    char album[64];
    char year[16];
    char genre[32];
    // Note: currently, the cover art can only be JPG, not PNG. most
    // MP3 files use JPG anyway so writing all the PNG code wouldn't 
    // be worth it.
    uint8_t *cover_art; 
    size_t cover_art_size;
    char cover_mime[32];
} pb_audio_metadata_t;

bool pb_audio_get_metadata(const char *path, pb_audio_metadata_t *meta);
void pb_audio_metadata_free(pb_audio_metadata_t *meta);
uint8_t *pb_audio_decode_cover_art(const pb_audio_metadata_t *meta, int *out_w, int *out_h, int max_w, int max_h);

void pb_audio_play_tone(uint32_t freq, uint32_t dur_ms);
void pb_audio_no_tone(void);

void pb_audio_set_volume(uint8_t vol);
bool pb_audio_is_playing(void);
void pb_audio_play_sample(const int16_t *data, size_t count, uint32_t sample_rate_hz);
void pb_audio_play_mp3(const char *path);
void pb_audio_play_wav(const char *path);
void pb_audio_stop(void);
void pb_audio_toggle_pause(void);
bool pb_audio_is_paused(void);
void pb_audio_watchdog(void);
int64_t pb_audio_get_duration_ms(void);
int64_t pb_audio_get_position_ms(void);
bool pb_audio_is_finished(void);
const char *pb_audio_format_time(int64_t ms);
void pb_audio_seek_relative(int32_t delta_ms);

#ifdef __cplusplus
}
#endif
