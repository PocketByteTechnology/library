#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x;
    float y;
    float z;
} pb_imu_accel_t;

typedef struct {
    float x;
    float y;
    float z;
} pb_imu_gyro_t;

typedef struct {
    float roll;
    float pitch;
    float yaw;
} pb_imu_angle_t;

void pb_imu_init(void);

void pb_imu_read_accel(pb_imu_accel_t *accel);
void pb_imu_read_gyro(pb_imu_gyro_t *gyro);
void pb_imu_read(pb_imu_accel_t *accel, pb_imu_gyro_t *gyro);
float pb_imu_read_temperature(void);

void pb_imu_update(void);
pb_imu_angle_t pb_imu_get_angles(void);

#ifdef __cplusplus
}
#endif
