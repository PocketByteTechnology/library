#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t start;
    uint64_t duration_us;
    bool active;
} pb_timing_timer_t;

void pb_timing_init(void);
void pb_timing_frame(void);

uint32_t pb_timing_frame_count(void);
float pb_timing_delta_time(void);
float pb_timing_get_fps(void);
void pb_timing_frame_delay(uint32_t target_fps);
bool pb_timing_loop(uint32_t target_fps);

void pb_timing_timer_start(pb_timing_timer_t *t, float seconds);
bool pb_timing_timer_expired(pb_timing_timer_t *t);
float pb_timing_timer_remaining(pb_timing_timer_t *t);
void pb_timing_timer_stop(pb_timing_timer_t *t);

#ifdef __cplusplus
}
#endif
