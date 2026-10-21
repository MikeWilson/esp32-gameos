// hal_imu.c — QMI8658 sampled at 200 Hz on core 0, complementary fusion to
// pitch/roll only (no magnetometer on this board: yaw drifts without bound
// and must never reach gameplay).
//
// Axis conventions here assume the board held portrait, screen up, USB at the
// bottom. WHICH accel axis is screen-vertical, and which way it points, is not
// the same on every unit of this panel, so the accel->pitch/roll map is
// runtime data (settings: TILT SETUP) rather than a constant, see the axis
// map below. Gyro axis *signs* are still learned online: each axis correlates
// its integrated rate against the accel-derived angle delta and flips itself
// if they disagree, so only the accel mapping ever needs a user's help.

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

// learned gyro sign per fused angle. The published pose carries these signs
// on gx/gy so every consumer agrees with fusion: positive gx always drives
// roll (aim right), positive gy pitch (aim down).
static float sign_pitch = 1.0f, sign_roll = 1.0f;
static float corr_pitch, corr_roll;

// --- accel axis map --------------------------------------------------------
// Which accel axis feeds pitch (the other feeds roll) plus a sign per angle.
// This is user data: the TILT SETUP wizard measures it and settings restores
// it at boot. The default (pitch<-+ax, roll<--ay) is the mapping measured on
// every unit anyone has checked, so a stock board is correct without ever
// opening the wizard; the wizard exists for a unit that differs.
//
// It is NOT the mapping the platform originally shipped. That one had roll
// positive, which aimed left/right backwards on all three boards measured,
// and the wrong sign hid behind an INVERT X that owners had flipped by hand.
//
// Packed into one aligned word: the 200 Hz task reads the map every sample and
// must never see half of an update.
#define MAP_PITCH_Y   1u    // accel Y feeds pitch (default: accel X does)
#define MAP_PITCH_NEG 2u
#define MAP_ROLL_NEG  4u
// Defaults to the measured-correct map so the IMU task is right from its
// first sample, before gos_settings_apply() has pushed stored settings in.
static volatile uint32_t axis_map = MAP_ROLL_NEG;

// Latest raw sample, published for the setup wizard. It cannot use the ring
// below: that has a single consumer and belongs to games.
static volatile float raw_ax, raw_ay, raw_az;

static void map_angles(uint32_t m, float ax, float ay, float az,
                       float *pitch, float *roll)
{
    float pa = (m & MAP_PITCH_Y) ? ay : ax;   // axis that feeds pitch
    float ra = (m & MAP_PITCH_Y) ? ax : ay;   // axis that feeds roll
    float p = atan2f(pa, sqrtf(ra * ra + az * az)) * RAD2DEG;
    float r = atan2f(ra, sqrtf(pa * pa + az * az)) * RAD2DEG;
    *pitch = (m & MAP_PITCH_NEG) ? -p : p;
    *roll  = (m & MAP_ROLL_NEG) ? -r : r;
}

void hal_imu_set_axis_map(const hal_imu_axis_map_t *in)
{
    uint32_t m = 0;
    if (in->pitch_axis) m |= MAP_PITCH_Y;
    if (in->pitch_sign < 0) m |= MAP_PITCH_NEG;
    if (in->roll_sign < 0) m |= MAP_ROLL_NEG;
    axis_map = m;   // aligned word store: atomic on Xtensa
    // Changing the map mid-run steps the fused angles; the complementary
    // filter pulls them back onto the accel within ~0.2 s and the gyro signs
    // relearn over the next second of handling.
    ESP_LOGI(TAG, "axis map: pitch<-%c%c roll<-%c%c",
             in->pitch_sign < 0 ? '-' : '+', in->pitch_axis ? 'Y' : 'X',
             in->roll_sign < 0 ? '-' : '+', in->pitch_axis ? 'X' : 'Y');
}

void hal_imu_get_axis_map(hal_imu_axis_map_t *out)
{
    uint32_t m = axis_map;
    out->pitch_axis = (m & MAP_PITCH_Y) ? 1 : 0;
    out->pitch_sign = (m & MAP_PITCH_NEG) ? -1 : 1;
    out->roll_sign = (m & MAP_ROLL_NEG) ? -1 : 1;
}

void hal_imu_angles_from_accel(float ax, float ay, float az,
                               float *pitch, float *roll)
{
    map_angles(axis_map, ax, ay, az, pitch, roll);
}

bool hal_imu_raw_accel(float *ax, float *ay, float *az)
{
    if (!imu_ok) return false;
    *ax = raw_ax; *ay = raw_ay; *az = raw_az;
    return true;
}

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
        raw_ax = ax; raw_ay = ay; raw_az = az;

        uint32_t m = axis_map;
        float ap, ar;                       // accel-derived pitch / roll
        map_angles(m, ax, ay, az, &ap, &ar);
        if (first) { pitch = ap; roll = ar; first = false; }

        // The gyro axis pairs with the accel axis the map did NOT pick for
        // that angle (a rotation about X moves the Y reading and vice versa).
        float g_pitch = (m & MAP_PITCH_Y) ? gx : gy;
        float g_roll  = (m & MAP_PITCH_Y) ? gy : gx;

        // online gyro-sign learning: correlate gyro rate with accel delta, so
        // the map only has to get the axis pairing right: the signs, including
        // any flip it asked for, are absorbed here within a second of handling
        float dap = ap - prev_ap, dar = ar - prev_ar;
        prev_ap = ap; prev_ar = ar;
        corr_pitch = corr_pitch * 0.995f + dap * g_pitch;
        corr_roll = corr_roll * 0.995f + dar * g_roll;
        if (corr_pitch > 2.f) sign_pitch = 1.f;
        else if (corr_pitch < -2.f) sign_pitch = -1.f;
        if (corr_roll > 2.f) sign_roll = 1.f;
        else if (corr_roll < -2.f) sign_roll = -1.f;

        pitch = FUSE_ALPHA * (pitch + sign_pitch * g_pitch * DT) + (1 - FUSE_ALPHA) * ap;
        roll  = FUSE_ALPHA * (roll  + sign_roll * g_roll * DT) + (1 - FUSE_ALPHA) * ar;

        float mag = sqrtf(ax * ax + ay * ay + az * az);
        float imp = fabsf(mag - 1.0f);
        shake *= 0.95f;
        if (imp * 1.5f > shake) shake = imp * 1.5f;
        if (shake > 1.f) shake = 1.f;

        int next = pose_idx ^ 1;
        pose_buf[next] = (hal_pose_t){
            .pitch = pitch, .roll = roll,
            .gx = sign_roll * g_roll, .gy = sign_pitch * g_pitch, .gz = gz,
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
