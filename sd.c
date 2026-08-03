#include "sd.h"
#include "hw.h"
#include "esp_log.h"

static const char *TAG = "pb_sd";

static bool sd_mounted = false;
static sdmmc_card_t *card;
static sdmmc_host_t host = SDSPI_HOST_DEFAULT();

void pb_sd_init(void)
{
    if (sd_mounted) {
        ESP_LOGW(TAG, "SD card already initialized");
        return;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Initializing SD card via SPI");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus failed");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Filesystem mount failed");
        } else {
            ESP_LOGE(TAG, "Card init failed (%s)", esp_err_to_name(ret));
        }
        spi_bus_free(host.slot);
        return;
    }

    sd_mounted = true;
    ESP_LOGI(TAG, "Filesystem mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);
}

void pb_sd_deinit(void)
{
    if (!sd_mounted) {
        return;
    }

    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    spi_bus_free(host.slot);
    sd_mounted = false;
    ESP_LOGI(TAG, "Card unmounted");
}

sdmmc_card_t *pb_sd_get_card(void)
{
    return card;
}
