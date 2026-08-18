// gos_hal.h — hardware abstraction layer, OS-internal.
// Games must never include this header; that is a failed review.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Display: 184x224 8bpp in, 368x448 RGB565 out (2x nearest, banded DMA) ---
bool hal_display_init(void);
// Queue a flush of `fb` through `lut` (RGB565, already panel-byte-order).
// Rows where (output_y % 3 == 2) use lut_dim when scanlines is set.
// Blocks only until the *previous* flush has finished (single frame in flight).
void hal_display_present(const uint8_t *fb, const uint16_t *lut,
                         const uint16_t *lut_dim, bool scanlines);
void hal_display_wait(void);             // wait for the in-flight flush
void hal_display_brightness(int pct);    // 0..100

void hal_display_present_565(const uint16_t *fb565);

// --- Touch (raw panel coords, 368x448, single point) ---
bool hal_touch_init(void);
bool hal_touch_read(int16_t *x, int16_t *y);

// --- IMU: 200 Hz sample+fusion task on core 0, double-buffered pose ---
typedef struct {
    float pitch, roll;      // degrees, absolute (not neutral-relative)
    float gx, gy, gz;       // gyro dps
    float shake;            // 0..1
    bool  ok;
} hal_pose_t;
bool hal_imu_init(void);
void hal_imu_get(hal_pose_t *out);

// Raw accel sample ring (g units, wall-clock stamped): the 200 Hz IMU task
// writes, one consumer drains. Games that need sensor-rate motion data
// (frame-rate sampling aliases short bursts) read the stream and run their
// own detectors. Returns samples copied, or -1 if the IMU is absent.
typedef struct { int64_t t_us; float ax, ay, az; } hal_accel_sample_t;
int hal_imu_accel_read(hal_accel_sample_t *out, int max);

// --- Audio: I2S + ES8311, pulls chunks from gos_core's mixer ---
bool hal_audio_init(void);
void hal_audio_volume(int pct);          // codec volume 0..100
void hal_audio_mute(bool on);
// Implemented by gos_core/mixer.c; called from the audio task.
// Returns false when all voices are idle (task may sleep).
bool gos_mixer_render(int16_t *out, int samples);

// --- Power (AXP2101; gracefully stubs out if the PMIC is absent) ---
typedef struct { int percent; bool charging; bool present; } hal_batt_t;
bool hal_power_init(void);
void hal_power_poll(void);               // call at ~1 Hz
void hal_power_get(hal_batt_t *out);

// --- BOOT button (GPIO0), the one physical input ---
bool hal_button_init(void);
bool hal_button_down(void);

#ifdef __cplusplus
}
#endif
