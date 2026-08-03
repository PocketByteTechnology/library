#include "lcd.h"
#include "hw.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "pax_gfx.h"

extern void pb_gfx_init(void);
extern esp_lcd_panel_handle_t pb_panel_handle;

#define TAG "pb_gfx_pax"

static pax_buf_t gfx_buf;

void pb_gfx_pax_init(void)
{
    pb_gfx_init();
    pax_buf_init(&gfx_buf, NULL, LCD_HEIGHT, LCD_WIDTH, PAX_BUF_16_565RGB);
    pax_buf_reversed(&gfx_buf, true);
    pax_buf_set_orientation(&gfx_buf, PAX_O_ROT_CCW);
}

void pb_gfx_pax_flush(void)
{
    esp_lcd_panel_draw_bitmap(pb_panel_handle, 0, 0, LCD_HEIGHT, LCD_WIDTH,
        pax_buf_get_pixels(&gfx_buf));
}

pax_buf_t *pb_gfx_pax_get_buf(void)
{
    return &gfx_buf;
}
