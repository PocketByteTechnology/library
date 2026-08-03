#include "lcd.h"
#include "hw.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_st7789.h"


#define LCD_HOST SPI3_HOST
#define TAG "pb_lcd"

esp_lcd_panel_handle_t pb_panel_handle = NULL;
esp_lcd_panel_io_handle_t pb_io_handle = NULL;

static void lcd_init_spi_panel(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCLK_PIN,
        .mosi_io_num = LCD_MOSI_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC_PIN,
        .cs_gpio_num = LCD_CS_PIN,
        .pclk_hz = 62500000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 3,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &pb_io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(pb_io_handle, &panel_config, &pb_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(pb_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(pb_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(pb_panel_handle, true));

    {
        static const uint8_t d0[] = {0x20};
        static const uint8_t d1[] = {0x35};
        static const uint8_t d2[] = {0x12};
        static const uint8_t d3[] = {0xA4, 0xA1};
        static const uint8_t d4[] = {0xD0,0x04,0x0E,0x12,0x14,0x2A,0x3E,0x52,0x4A,0x17,0x0C,0x0A,0x1D,0x21};
        static const uint8_t d5[] = {0xD0,0x04,0x0D,0x12,0x14,0x2B,0x3E,0x44,0x4D,0x29,0x1A,0x1A,0x1C,0x20};
        const struct { uint8_t cmd; const uint8_t *data; uint8_t len; } tbl[] = {
            {0xBB, d0, 1},
            {0xB7, d1, 1},
            {0xC3, d2, 1},
            {0xD0, d3, 2},
            {0xE0, d4, 14},
            {0xE1, d5, 14},
        };
        for (int i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++) {
            esp_lcd_panel_io_tx_param(pb_io_handle, tbl[i].cmd, tbl[i].data, tbl[i].len);
        }
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(pb_panel_handle, true));
}

static void lcd_clear_screen(void)
{
    size_t sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    uint16_t *buf = malloc(sz);
    if (!buf) {
        ESP_LOGE(TAG, "Clear alloc failed");
        return;
    }
    memset(buf, 0, sz);
    esp_lcd_panel_draw_bitmap(pb_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
    free(buf);
}

void pb_gfx_init(void)
{
    lcd_init_spi_panel();
    lcd_clear_screen();
}
