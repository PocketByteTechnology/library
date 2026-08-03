#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void pb_nvs_init(void);
void pb_nvs_deinit(void);

void pb_nvs_save_u32(const char *key, uint32_t value);
uint32_t pb_nvs_load_u32(const char *key, uint32_t default_value);

void pb_nvs_save_i32(const char *key, int32_t value);
int32_t pb_nvs_load_i32(const char *key, int32_t default_value);

void pb_nvs_save_str(const char *key, const char *value);
esp_err_t pb_nvs_load_str(const char *key, char *buf, size_t *len);

void pb_nvs_save_blob(const char *key, const void *data, size_t len);
esp_err_t pb_nvs_load_blob(const char *key, void *buf, size_t *len);

void pb_nvs_erase_key(const char *key);
void pb_nvs_erase_all(void);

#ifdef __cplusplus
}
#endif
