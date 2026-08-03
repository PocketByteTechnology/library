#include "imu.h"
#include "hw.h"
#include "icm42670.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "pb_imu"

static icm42670_handle_t sensor = NULL;
static pb_imu_angle_t angles = {0};
static bool inited = false;

void pb_imu_init(void)
{
    if (!pb_i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not ready");
        return;
    }
    esp_err_t ret = icm42670_create(pb_i2c_bus, ICM42670_I2C_ADDRESS, &sensor);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ICM42670 not found");
        return;
    }
    uint8_t dev_id;
    icm42670_get_deviceid(sensor, &dev_id);
    ESP_LOGI(TAG, "Found ICM42670 (ID 0x%02X)", dev_id);

    icm42670_config(sensor, &(icm42670_cfg_t){
        .acce_fs = ACCE_FS_2G,
        .acce_odr = ACCE_ODR_100HZ,
        .gyro_fs = GYRO_FS_250DPS,
        .gyro_odr = GYRO_ODR_100HZ,
    });
    icm42670_acce_set_pwr(sensor, ACCE_PWR_LOWNOISE);
    icm42670_gyro_set_pwr(sensor, GYRO_PWR_LOWNOISE);
    inited = true;
    ESP_LOGI(TAG, "Init ok (2G, 250dps, 100Hz)");
}

void pb_imu_read_accel(pb_imu_accel_t *accel)
{
    if (!inited || !accel) {
        return;
    }
    icm42670_value_t v;
    if (icm42670_get_acce_value(sensor, &v) == ESP_OK) {
        accel->x = v.x;
        accel->y = v.y;
        accel->z = v.z;
    }
}

void pb_imu_read_gyro(pb_imu_gyro_t *gyro)
{
    if (!inited || !gyro) {
        return;
    }
    icm42670_value_t v;
    if (icm42670_get_gyro_value(sensor, &v) == ESP_OK) {
        gyro->x = v.x;
        gyro->y = v.y;
        gyro->z = v.z;
    }
}

void pb_imu_read(pb_imu_accel_t *accel, pb_imu_gyro_t *gyro)
{
    if (accel) {
        pb_imu_read_accel(accel);
    }
    if (gyro) {
        pb_imu_read_gyro(gyro);
    }
}

float pb_imu_read_temperature(void)
{
    if (!inited) {
        return 0.0f;
    }
    float t;
    if (icm42670_get_temp_value(sensor, &t) == ESP_OK) {
        return t;
    }
    return 0.0f;
}

void pb_imu_update(void)
{
    if (!inited) {
        return;
    }
    icm42670_value_t accel;
    icm42670_value_t gyro;
    if (icm42670_get_acce_value(sensor, &accel) != ESP_OK) {
        return;
    }
    if (icm42670_get_gyro_value(sensor, &gyro) != ESP_OK) {
        return;
    }
    complimentary_angle_t comp;
    // Was this misspselled on porpoise?
    if (icm42670_complimentory_filter(sensor, &accel, &gyro, &comp) == ESP_OK) {
        angles.roll = comp.roll;
        angles.pitch = comp.pitch;
    }
}

pb_imu_angle_t pb_imu_get_angles(void)
{
    return angles;
}
