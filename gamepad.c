#include "gamepad.h"
#include "i2c_bus.h"
#include "hw.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MATRIX_ROWS 2
#define MATRIX_COLS 4

#define MCP23017_ADDR 0x20

#define MCP23017_IODIRA 0x00
#define MCP23017_IODIRB 0x01
#define MCP23017_GPPU   0x0C
#define MCP23017_GPIOB  0x13
#define MCP23017_OLATA  0x14
#define MCP23017_OLATB  0x15

#define ROW1_PIN 0
#define ROW2_PIN 1
#define COL1_PIN 2
#define COL3_PIN 4
#define COL2_PIN 3
#define COL4_PIN 5
#define FUNC_BTN_PIN 6
#define LED_FUNC_PIN 7

#define COL_MASK_16 ((1 << (COL1_PIN + 8)) | (1 << (COL2_PIN + 8)) | \
                     (1 << (COL3_PIN + 8)) | (1 << (COL4_PIN + 8)) | \
                     (1 << (FUNC_BTN_PIN + 8)))

static const char *TAG = "pb_gamepad";

i2c_master_bus_handle_t pb_i2c_bus = NULL;

static i2c_master_dev_handle_t mcp23017_dev = NULL;

static uint32_t current_state = 0;
static uint32_t last_state = 0;
static uint32_t latched_presses = 0;
static uint8_t led_a_mask = 0;
static bool led_b_up = false;

static const uint8_t row_pins[MATRIX_ROWS] = { ROW1_PIN, ROW2_PIN };
static const uint8_t col_pins[MATRIX_COLS] = { COL1_PIN, COL2_PIN, COL3_PIN, COL4_PIN };

static const pb_button_t button_map[MATRIX_ROWS][MATRIX_COLS] = 
{
    { PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT },
    { PB_A,  PB_B,    PB_X,    PB_Y     }
};

static esp_err_t mcp23017_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t cmd[] = {reg, val};
    return i2c_master_transmit(mcp23017_dev, cmd, sizeof(cmd), -1);
}

static esp_err_t mcp23017_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(mcp23017_dev, &reg, 1, val, 1, -1);
}

static esp_err_t mcp23017_write_reg16(uint8_t reg, uint16_t val)
{
    uint8_t cmd[] = {reg, val & 0xFF, (val >> 8) & 0xFF};
    return i2c_master_transmit(mcp23017_dev, cmd, sizeof(cmd), -1);
}

void pb_gamepad_init(void) 
{
    if (pb_i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not ready");
        return;
    }

    if (pb_i2c_probe(MCP23017_ADDR) != ESP_OK) {
        ESP_LOGI(TAG, "MCP23017 not found at 0x%02X", MCP23017_ADDR);
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MCP23017_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(pb_i2c_bus, &dev_cfg, &mcp23017_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP23017 device failed");
        return;
    }

    mcp23017_write_reg(MCP23017_IODIRB, 0x7F);
    mcp23017_write_reg16(MCP23017_GPPU, COL_MASK_16);
    mcp23017_write_reg(MCP23017_OLATB, 0x80);

    mcp23017_write_reg(MCP23017_IODIRA, 0x00);
    mcp23017_write_reg(MCP23017_OLATA, 0xFF);
    led_a_mask = 0;
    led_b_up = false;

    ESP_LOGI(TAG, "Initialized with latching logic");
}

bool pb_gamepad_is_present(void)
{
    return mcp23017_dev != NULL;
}

void pb_gamepad_poll(void) 
{
    if (mcp23017_dev == NULL) {
        return;
    }

    last_state = current_state;
    uint32_t new_state = 0;

    for (size_t r = 0; r < MATRIX_ROWS; ++r) {
        mcp23017_write_reg(MCP23017_IODIRB, 0x7F);

        uint8_t row_dir_mask = 0x7F & ~(1 << row_pins[r]);
        mcp23017_write_reg(MCP23017_IODIRB, row_dir_mask);

        esp_rom_delay_us(50);

        uint8_t port_val;
        mcp23017_read_reg(MCP23017_GPIOB, &port_val);

        for (size_t c = 0; c < MATRIX_COLS; ++c) {
            if (!(port_val & (1 << col_pins[c]))) {
                new_state |= button_map[r][c];
            }
        }
    }

    uint8_t gpio_val;
    mcp23017_read_reg(MCP23017_GPIOB, &gpio_val);
    if (!(gpio_val & (1 << FUNC_BTN_PIN))) {
        new_state |= PB_FUNC;
    }

    current_state = new_state;

    latched_presses |= (current_state & ~last_state);

    if (current_state != last_state) {
        ESP_LOGI(TAG, "Mask: 0x%03lX", current_state);
    }
}

bool pb_gamepad_button_pressed(pb_button_t mask) 
{
    if (latched_presses & mask) {
        latched_presses &= ~mask;
        return true;
    }
    return false;
}

bool pb_gamepad_button_held(pb_button_t mask) 
{
    return (current_state & mask);
}

bool pb_gamepad_button_released(pb_button_t mask) 
{
    return !(current_state & mask) && (last_state & mask);
}

void pb_gamepad_set_button_led(pb_button_t button, bool on)
{
    if (mcp23017_dev == NULL) {
        return;
    }

    if (button == PB_UP) {
        led_b_up = on;
        uint8_t val;
        mcp23017_read_reg(MCP23017_OLATB, &val);
        if (on) {
            val &= ~(1 << 7);
        } else {
            val |= (1 << 7);
        }
        mcp23017_write_reg(MCP23017_OLATB, val);
        return;
    }

    uint8_t pin;
    if (button == PB_DOWN) {
        pin = 0;
    } else if (button == PB_LEFT) {
        pin = 1;
    } else if (button == PB_RIGHT) {
        pin = 2;
    } else if (button == PB_A) {
        pin = 3;
    } else if (button == PB_B) {
        pin = 4;
    } else if (button == PB_X) {
        pin = 5;
    } else if (button == PB_Y) {
        pin = 6;
    } else if (button == PB_FUNC) {
        pin = LED_FUNC_PIN;
    } else {
        return;
    }

    if (on) {
        led_a_mask |= (1 << pin);
    } else {
        led_a_mask &= ~(1 << pin);
    }
    mcp23017_write_reg(MCP23017_OLATA, (~led_a_mask) & 0xFF);
}

void pb_gamepad_set_button_leds(pb_button_t mask)
{
    if (mcp23017_dev == NULL) {
        return;
    }

    led_a_mask = (mask & ~PB_UP) >> 1;
    mcp23017_write_reg(MCP23017_OLATA, (~led_a_mask) & 0xFF);

    led_b_up = (mask & PB_UP) != 0;
    uint8_t val;
    mcp23017_read_reg(MCP23017_OLATB, &val);
    if (led_b_up) {
        val &= ~(1 << 7);
    } else {
        val |= (1 << 7);
    }
    mcp23017_write_reg(MCP23017_OLATB, val);
}

void pb_gamepad_clear_button_leds(void)
{
    if (mcp23017_dev == NULL) {
        return;
    }
    led_a_mask = 0;
    led_b_up = false;
    mcp23017_write_reg(MCP23017_OLATA, 0xFF);
    uint8_t val;
    mcp23017_read_reg(MCP23017_OLATB, &val);
    val |= (1 << 7);
    mcp23017_write_reg(MCP23017_OLATB, val);
}
