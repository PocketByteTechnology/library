#include "lcd.h"
#include "hw.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#define LVGL_DRAW_BUF_LINES    40
#define LVGL_TICK_PERIOD_MS    2
#define LCD_HOST               SPI3_HOST
#define TAG                    "pb_gfx_lvgl"

extern void pb_gfx_init(void);
extern esp_lcd_panel_handle_t pb_panel_handle;
extern esp_lcd_panel_io_handle_t pb_io_handle;

static bool lvgl_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_update_callback(lv_display_t *disp)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    switch (lv_display_get_rotation(disp)) {
        case LV_DISPLAY_ROTATION_0:
            esp_lcd_panel_swap_xy(panel, false);
            esp_lcd_panel_mirror(panel, false, false);
            break;
        case LV_DISPLAY_ROTATION_90:
            esp_lcd_panel_swap_xy(panel, true);
            esp_lcd_panel_mirror(panel, false, true);
            break;
        case LV_DISPLAY_ROTATION_180:
            esp_lcd_panel_swap_xy(panel, false);
            esp_lcd_panel_mirror(panel, true, true);
            break;
        case LV_DISPLAY_ROTATION_270:
            esp_lcd_panel_swap_xy(panel, true);
            esp_lcd_panel_mirror(panel, true, false);
            break;
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lvgl_update_callback(disp);
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    int x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;
    lv_draw_sw_rgb565_swap(px_map, (x2 + 1 - x1) * (y2 + 1 - y1));
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

void pb_gfx_lvgl_init(void)
{
    pb_gfx_init();

    lv_init();

    lv_display_t *disp = lv_display_create(LCD_HEIGHT, LCD_WIDTH);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);

    size_t db_sz = LCD_WIDTH * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = heap_caps_malloc(db_sz, MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(db_sz, MALLOC_CAP_DMA);
    assert(buf1 && buf2);

    lv_display_set_buffers(disp, buf1, buf2, db_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, pb_panel_handle);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_notify_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(pb_io_handle, &cbs, disp));

    ESP_LOGI(TAG, "LVGL ready (%dx%d, %d lines buf)", LCD_WIDTH, LCD_HEIGHT, LVGL_DRAW_BUF_LINES);
}

void pb_gfx_lvgl_tick(void)
{
    lv_timer_handler();
}
