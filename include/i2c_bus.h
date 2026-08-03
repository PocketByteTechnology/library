#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pb_i2c_init(void);
esp_err_t pb_i2c_probe(uint8_t addr);

#ifdef __cplusplus
}
#endif
