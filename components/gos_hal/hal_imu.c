// hal_imu.c — QMI8658 sampled at 200 Hz on core 0, complementary fusion to
// pitch/roll only (no magnetometer on this board: yaw drifts without bound
// and must never reach gameplay).
//
// Axis conventions here were chosen for the board held portrait, screen up,
// USB at the bottom; the accel signs are corrected by the neutral calibration
// plus the invert toggles in settings. Gyro axis *signs* are learned online:
// each axis correlates its integrated rate against the accel-derived angle
// delta and flips itself if they disagree — mounting guesses stop mattering
// after a second of handling.

#include "gos_hal.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#undef M_PI                 // vendor qmi8658.h redefines it unconditionally
#include "qmi8658.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

#define SAMPLE_HZ 200
#define DT (1.0f / SAMPLE_HZ)
#define FUSE_ALPHA 0.96f
#define RAD2DEG 57.29578f

static const char *TAG = "hal_imu";
static qmi8658_dev_t dev;
static bool imu_ok;

static hal_pose_t pose_buf[2];
static volatile int pose_idx;

// learned gyro sign per fused axis (pitch<-gy, roll<-gx). The published
// pose carries these signs on gx/gy so every consumer agrees with fusion:
// positive gx always drives roll (aim right), positive gy pitch (aim down).
static float sign_px = 1.0f, sign_ry = 1.0f;
static float corr_px, corr_ry;

// Raw accel ring: 200 Hz writer (this task), one consumer draining at frame
// rate. 64 deep = ~320 ms of slack before samples drop.
#define ACC_RING 64
static hal_accel_sample_t acc_ring[ACC_RING];
static volatile uint32_t acc_w;   // total written (monotonic)
static uint32_t acc_r;            // total read (consumer-side)

int hal_imu_accel_read(hal_accel_sample_t *out, int max)
{
    if (!imu_ok) return -1;
    uint32_t w = acc_w;
    if (w - acc_r > ACC_RING) acc_r = w - ACC_RING;   // overrun: drop oldest
    int n = 0;
    while (acc_r != w && n < max) {
        out[n++] = acc_ring[acc_r % ACC_RING];
        acc_r++;
    }
    return n;
}

static void imu_task(void *arg)
{
    float pitch = 0, roll = 0, shake = 0;
    float prev_ap = 0, prev_ar = 0;
    bool first = true;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
        float ax, ay, az, gx, gy, gz;
        // component defaults: accel in mg, gyro in dps (set explicitly at init)
        if (qmi8658_read_accel(&dev, &ax, &ay, &az) != ESP_OK) continue;
        if (qmi8658_read_gyro(&dev, &gx, &gy, &gz) != ESP_OK) continue;
        ax /= 1000.f; ay /= 1000.f; az /= 1000.f;   // mg -> g

        acc_ring[acc_w % ACC_RING] = (hal_accel_sample_t){
            .t_us = esp_timer_get_time(), .ax = ax, .ay = ay, .az = az,
        };
        acc_w = acc_w + 1;

        // On this board the accel Y axis runs along the screen's horizontal
        // (user-verified: the first guess had left/right driving up/down), so
        // pitch (screen-vertical aim) comes from X and roll from Y.
        float ap = atan2f(ax, sqrtf(ay * ay + az * az)) * RAD2DEG;  // accel pitch
        float ar = atan2f(ay, sqrtf(ax * ax + az * az)) * RAD2DEG;  // accel roll
        if (first) { pitch = ap; roll = ar; first = false; }

        // online gyro-sign learning: correlate gyro rate with accel delta
        // (axes pair with the swapped accel mapping: pitch<-gy, roll<-gx)
        float dap = ap - prev_ap, dar = ar - prev_ar;
        prev_ap = ap; prev_ar = ar;
        corr_px = corr_px * 0.995f + dap * gy;
        corr_ry = corr_ry * 0.995f + dar * gx;
        if (corr_px > 2.f) sign_px = 1.f; else if (corr_px < -2.f) sign_px = -1.f;
        if (corr_ry > 2.f) sign_ry = 1.f; else if (corr_ry < -2.f) sign_ry = -1.f;

        pitch = FUSE_ALPHA * (pitch + sign_px * gy * DT) + (1 - FUSE_ALPHA) * ap;
        roll  = FUSE_ALPHA * (roll  + sign_ry * gx * DT) + (1 - FUSE_ALPHA) * ar;

        float mag = sqrtf(ax * ax + ay * ay + az * az);
        float imp = fabsf(mag - 1.0f);
        shake *= 0.95f;
        if (imp * 1.5f > shake) shake = imp * 1.5f;
        if (shake > 1.f) shake = 1.f;

        int next = pose_idx ^ 1;
        pose_buf[next] = (hal_pose_t){
            .pitch = pitch, .roll = roll,
            .gx = sign_ry * gx, .gy = sign_px * gy, .gz = gz,
            .shake = shake, .ok = true,
        };
        pose_idx = next;   // aligned int store: atomic on Xtensa
    }
}

bool hal_imu_init(void)
{
    // Golf found the IMU at the HIGH address on this board; probe both anyway.
    if (qmi8658_init(&dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH) != ESP_OK &&
        qmi8658_init(&dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_LOW) != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 not found");
        return false;
    }
    qmi8658_set_accel_range(&dev, QMI8658_ACCEL_RANGE_4G);
    qmi8658_set_accel_odr(&dev, QMI8658_ACCEL_ODR_250HZ);
    qmi8658_set_gyro_range(&dev, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(&dev, QMI8658_GYRO_ODR_250HZ);
    qmi8658_set_accel_unit_mg(&dev, true);
    qmi8658_set_gyro_unit_dps(&dev, true);
    qmi8658_enable_sensors(&dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    imu_ok = true;
    xTaskCreatePinnedToCore(imu_task, "gos_imu", 4096, NULL, 6, NULL, 0);
    return true;
}

void hal_imu_get(hal_pose_t *out)
{
    if (!imu_ok) { memset(out, 0, sizeof *out); return; }
    *out = pose_buf[pose_idx];
}
