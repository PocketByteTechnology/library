#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

extern i2c_master_bus_handle_t pb_i2c_bus;

#define LCD_SCLK_PIN 10
#define LCD_MOSI_PIN 11
#define LCD_DC_PIN   13
#define LCD_CS_PIN   14
#define LCD_RST_PIN  12

#define SD_SCLK_PIN  7
#define SD_MOSI_PIN  8
#define SD_MISO_PIN  6
#define SD_CS_PIN    9

#define MAIN_I2C_SCL 18
#define MAIN_I2C_SDA 17

#define I2S_BCLK_PIN  38
#define I2S_LRCLK_PIN 47
#define I2S_DOUT_PIN  21

#ifdef __cplusplus
}
#endif
