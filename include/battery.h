#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void pb_battery_init(void);

float pb_battery_get_percentage(void);

#ifdef __cplusplus
}
#endif
