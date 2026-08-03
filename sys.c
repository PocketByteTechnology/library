#include "sys.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

static const char *TAG = "pb_sys";

#define REBOOT_MAGIC 0xDEADBEEF // Lmao

void pb_sys_reboot_to_loader(void)
{
    ESP_LOGI(TAG, "Rebooting to invalidate OTA data on next boot...");
    REG_WRITE(RTC_CNTL_STORE0_REG, REBOOT_MAGIC);
    esp_rom_software_reset_system();
}

void pb_sys_handle_pending_reboot(void)
{
    if (REG_READ(RTC_CNTL_STORE0_REG) != REBOOT_MAGIC) {
        return;
    }
    REG_WRITE(RTC_CNTL_STORE0_REG, 0);

    ESP_LOGI(TAG, "Deferred OTA data invalidation...");

    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);

    if (otadata) {
        esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to invalidate OTA data: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "OTA data partition not found in partition table");
    }

    ESP_LOGI(TAG, "Restarting into factory/launcher...");
    esp_rom_software_reset_system();
}
