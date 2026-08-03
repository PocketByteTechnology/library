#include "timing.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_DT 0.1f

static uint64_t start_time;
static uint64_t last_frame_time;
static uint32_t total_frames;
static float delta_time_sec;
static float fps_value;
static uint32_t fps_count;
static uint64_t fps_reset;

void pb_timing_init(void)
{
    start_time = esp_timer_get_time();
    last_frame_time = start_time;
    total_frames = 0;
    delta_time_sec = 0.0f;
    fps_value = 0.0f;
    fps_count = 0;
    fps_reset = start_time;
}

void pb_timing_frame(void)
{
    uint64_t now = esp_timer_get_time();
    uint64_t elapsed = now - last_frame_time;
    last_frame_time = now;

    float dt = (float)elapsed / 1000000.0f;
    if (dt > MAX_DT) {
        dt = MAX_DT;
    }
    delta_time_sec = dt;

    total_frames++;
    fps_count++;

    uint64_t sec = now - fps_reset;
    if (sec >= 1000000) {
        fps_value = (float)fps_count * 1000000.0f / (float)sec;
        fps_count = 0;
        fps_reset = now;
    }
}

uint32_t pb_timing_frame_count(void)
{
    return total_frames;
}

float pb_timing_delta_time(void)
{
    return delta_time_sec;
}

float pb_timing_get_fps(void)
{
    if (total_frames == 0) {
        return 0.0f;
    }
    uint64_t now = esp_timer_get_time();
    uint64_t elapsed = now - fps_reset;
    if (elapsed == 0) {
        return 0.0f;
    }
    return (float)fps_count * 1000000.0f / (float)elapsed;
}

void pb_timing_frame_delay(uint32_t target_fps)
{
    if (target_fps == 0) {
        return;
    }
    uint64_t frame_time = 1000000 / target_fps;
    uint64_t target = last_frame_time + frame_time;
    uint64_t now = esp_timer_get_time();
    while (now < target) {
        uint64_t remaining = target - now;
        if (remaining > 10000) {
            vTaskDelay(1);
        } else if (remaining > 5) {
            esp_rom_delay_us((uint32_t)remaining);
        } else {
            break;
        }
        now = esp_timer_get_time();
    }
}

bool pb_timing_loop(uint32_t target_fps)
{
    if (total_frames == 0) {
        pb_timing_init();
        if (target_fps > 0) {
            pb_timing_frame();
        }
        return true;
    }
    if (target_fps > 0) {
        pb_timing_frame_delay(target_fps);
    }
    pb_timing_frame();
    return true;
}

void pb_timing_timer_start(pb_timing_timer_t *t, float seconds)
{
    t->start = esp_timer_get_time();
    t->duration_us = (uint64_t)(seconds * 1000000.0f);
    t->active = true;
}

bool pb_timing_timer_expired(pb_timing_timer_t *t)
{
    if (!t->active) {
        return true;
    }
    if ((esp_timer_get_time() - t->start) >= t->duration_us) {
        t->active = false;
        return true;
    }
    return false;
}

float pb_timing_timer_remaining(pb_timing_timer_t *t)
{
    if (!t->active) {
        return 0.0f;
    }
    uint64_t elapsed = esp_timer_get_time() - t->start;
    if (elapsed >= t->duration_us) {
        return 0.0f;
    }
    return (float)(t->duration_us - elapsed) / 1000000.0f;
}

void pb_timing_timer_stop(pb_timing_timer_t *t)
{
    t->active = false;
}
