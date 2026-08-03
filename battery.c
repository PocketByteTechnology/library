#include "battery.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"

#define PB_BATTERY_MIN_MV 3150
#define PB_BATTERY_MAX_MV 4100

#define PB_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - PB_BATTERY_MIN_MV) / (PB_BATTERY_MAX_MV - PB_BATTERY_MIN_MV) * 100.f)

static const char *TAG = "BATTERY";

static esp_adc_cal_characteristics_t adc_chars;

void pb_battery_init(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL_0, ADC_ATTEN_DB_11);

    esp_adc_cal_characterize(
        ADC_UNIT_1,
        ADC_ATTEN_DB_11,
        ADC_WIDTH_BIT_12,
        1100,
        &adc_chars
    );
}

float pb_battery_get_percentage(void)
{
    int raw = adc1_get_raw(ADC_CHANNEL_0);
    if (raw <= 0) {
        return 0.f;
    }

    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
    float percent = PB_BATTERY_CALC_PERCENT(mv);

    if (percent < 0.f) {
        percent = 0.f;
    }
    if (percent > 100.f) {
        percent = 100.f;
    }

    return percent;
}
