#include "i2c_bus.h"
#include "hw.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "pb_i2c"

esp_err_t pb_i2c_init(void)
{
    if (pb_i2c_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = MAIN_I2C_SDA,
        .scl_io_num = MAIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &pb_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed");
        return ret;
    }
    ESP_LOGI(TAG, "Bus ok (SDA:%d, SCL:%d)", MAIN_I2C_SDA, MAIN_I2C_SCL);
    return ESP_OK;
}

esp_err_t pb_i2c_probe(uint8_t addr)
{
    if (!pb_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(pb_i2c_bus, addr, 50);
}
