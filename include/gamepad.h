#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PB_UP    = (1 << 0),
    PB_DOWN  = (1 << 1),
    PB_LEFT  = (1 << 2),
    PB_RIGHT = (1 << 3),
    PB_A     = (1 << 4),
    PB_B     = (1 << 5),
    PB_X     = (1 << 6),
    PB_Y     = (1 << 7),
    PB_FUNC  = (1 << 8),
} pb_button_t;

void pb_gamepad_init(void);
bool pb_gamepad_is_present(void);
void pb_gamepad_poll(void);

bool pb_gamepad_button_pressed(pb_button_t button);
bool pb_gamepad_button_held(pb_button_t button);
bool pb_gamepad_button_released(pb_button_t button);

void pb_gamepad_set_button_led(pb_button_t button, bool on);
void pb_gamepad_set_button_leds(pb_button_t led_mask);
void pb_gamepad_clear_button_leds(void);

#ifdef __cplusplus
}
#endif
