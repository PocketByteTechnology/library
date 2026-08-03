idf_component_register(
    SRCS "wifi.c" "tjpgd.c" "i2c_bus.c" "nvs.c" "timing.c" "imu.c" "audio.c" "battery.c" "gamepad.c" "lcd.c" "gfx_lvgl.c" "gfx_pax.c" "sd.c" "sys.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_lcd wpa_supplicant esp_timer esp_adc fatfs driver nvs_flash app_update esp_wifi esp_netif esp_event esp_http_client mbedtls json
)
