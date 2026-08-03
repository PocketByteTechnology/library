#pragma once

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNT_POINT "/sdcard"

void pb_sd_init(void);
void pb_sd_deinit(void);

sdmmc_card_t *pb_sd_get_card(void);

#ifdef __cplusplus
}
#endif
