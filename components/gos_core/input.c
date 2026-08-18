// input.c — the normalized input snapshot. Pose from the IMU HAL, touch from
// the touch HAL, BOOT button; aim mapping per spec §6: neutral offset ->
// range clamp -> 1.4-power response curve -> deadzone -> one-euro filter.
#include "gos.h"
#include "gos_core.h"
#include <math.h>
#include <string.h>
#include "gos_hal.h"
#include "nvs.h"

static gos_input_t in;
static gos_aim_mode_t aim_mode = GOS_AIM_TILT_ABS;
static gos_aim_cfg_t cfg = {
    .range_deg = 25.f, .curve_pow = 1.4f, .deadzone = 0.04f,
    .min_cutoff = 1.0f, .beta = 0.007f, .d_cutoff = 1.0f,
    .gyro_gain = 1.0f,
};

// GYRO_RATE: rates below this are sensor noise, not intent — without a rate
// deadzone the held aim wanders now that there is no friction pulling it back
#define GYRO_DEAD_DPS 2.5f
static float neutral_pitch, neutral_roll;
static float gyro_aim_x, gyro_aim_y;      // GYRO_RATE integrator
static float drag_aim_x, drag_aim_y;      // TOUCH_DRAG state
static gos_pt_t drag_last;
static bool was_touching;

// --- one-euro filter -------------------------------------------------------
typedef struct { float x_prev, dx_prev; bool init; } euro_t;
static euro_t fx, fy;

static float lowpass(float x, float prev, float alpha) { return alpha * x + (1 - alpha) * prev; }
static float euro_alpha(float cutoff, float dt)
{
    float tau = 1.0f / (2.0f * (float)M_PI * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

static float euro_filter(euro_t *f, float x, float dt)
{
    if (!f->init) { f->x_prev = x; f->dx_prev = 0; f->init = true; return x; }
    float dx = (x - f->x_prev) / dt;
    float edx = lowpass(dx, f->dx_prev, euro_alpha(cfg.d_cutoff, dt));
    f->dx_prev = edx;
    float cutoff = cfg.min_cutoff + cfg.beta * fabsf(edx);
    float ex = lowpass(x, f->x_prev, euro_alpha(cutoff, dt));
    f->x_prev = ex;
    return ex;
}

// --- curve -----------------------------------------------------------------
static float shape(float a)
{
    float s = a < 0 ? -1.f : 1.f;
    float m = fabsf(a);
    if (m > 1.f) m = 1.f;
    m = powf(m, cfg.curve_pow);
    if (m < cfg.deadzone) return 0.f;
    m = (m - cfg.deadzone) / (1.f - cfg.deadzone);
    return s * m;
}

void gos_input_init(void) { memset(&in, 0, sizeof in); }

void gos_input_update(float dt)
{
    hal_pose_t pose;
    hal_imu_get(&pose);

    // touch (raw 368x448 -> render 184x224)
    int16_t tx, ty;
    bool touching = hal_touch_read(&tx, &ty);
    if (touching) {
        in.touch.x = (int16_t)(tx / 2);
        in.touch.y = (int16_t)(ty / 2);
    }

    // buttons
    uint32_t btn = 0;
    // BOOT is a GAME button (A) — the shell pauses via swipe-down instead,
    // so games get a real physical control (user-approved repurposing)
    if (hal_button_down()) btn |= GOS_BTN_A;
    // FIRE = touch in lower 2/3 (only granted when the game asked for it —
    // the shell masks caps before handing the snapshot over)
    if (touching && in.touch.y > GOS_SCREEN_H / 3) btn |= GOS_BTN_FIRE;
    uint32_t prev = in.buttons;
    in.buttons = btn;
    in.pressed = btn & ~prev;
    in.released = prev & ~btn;
    in.touching = touching;

    // pose diagnostics
    in.pitch = pose.pitch - neutral_pitch;
    in.roll = pose.roll - neutral_roll;
    in.gx = pose.gx; in.gy = pose.gy; in.gz = pose.gz;
    in.shake = pose.shake;

    float ax = 0, ay = 0;
    bool active = false;
    switch (aim_mode) {
    case GOS_AIM_TILT_ABS:
        ax = shape(in.roll / cfg.range_deg);
        ay = shape(in.pitch / cfg.range_deg);
        active = pose.ok;
        break;
    case GOS_AIM_GYRO_RATE: {
        // device rotation integrates 1:1 into turret deflection: turning the
        // device by range_deg degrees sweeps full scale, and the aim HOLDS
        // where pointed (no friction drift-back). pose.gx/gy arrive with the
        // HAL's learned signs applied, so gx pairs with roll/aim_x and gy
        // with pitch/aim_y on every unit.
        float rx = fabsf(pose.gx) > GYRO_DEAD_DPS ? pose.gx : 0.f;
        float ry = fabsf(pose.gy) > GYRO_DEAD_DPS ? pose.gy : 0.f;
        gyro_aim_x += rx * cfg.gyro_gain * dt / cfg.range_deg;
        gyro_aim_y += ry * cfg.gyro_gain * dt / cfg.range_deg;
        if (gyro_aim_x > 1) gyro_aim_x = 1;
        if (gyro_aim_x < -1) gyro_aim_x = -1;
        if (gyro_aim_y > 1) gyro_aim_y = 1;
        if (gyro_aim_y < -1) gyro_aim_y = -1;
        ax = gyro_aim_x; ay = gyro_aim_y;
        active = pose.ok;
        break;
    }
    case GOS_AIM_TOUCH_DRAG:
        if (touching) {
            if (was_touching) {
                drag_aim_x += (in.touch.x - drag_last.x) / 45.f;
                drag_aim_y += (in.touch.y - drag_last.y) / 45.f;
                if (drag_aim_x > 1) drag_aim_x = 1;
                if (drag_aim_x < -1) drag_aim_x = -1;
                if (drag_aim_y > 1) drag_aim_y = 1;
                if (drag_aim_y < -1) drag_aim_y = -1;
            }
            drag_last = in.touch;
        }
        ax = drag_aim_x; ay = drag_aim_y;
        active = true;
        break;
    }
    was_touching = touching;

    if (aim_mode == GOS_AIM_TOUCH_DRAG) {
        // direct-manipulation channel: no invert (inverted drag is never
        // wanted) and no one-euro (it only adds lag between finger and aim);
        // keep the filters re-armed for a clean switch back to an IMU mode
        in.aim_x = ax;
        in.aim_y = ay;
        fx.init = fy.init = false;
    } else {
        if (cfg.invert_x) ax = -ax;
        if (cfg.invert_y) ay = -ay;
        in.aim_x = euro_filter(&fx, ax, dt);
        in.aim_y = euro_filter(&fy, ay, dt);
    }
    in.aim_active = active;
}

const gos_input_t *gos_input_get(void) { return &in; }

// raw accel stream: thin pass-through to the HAL's sample ring (the structs
// are field-identical; games see the gos_ type, the HAL keeps its own)
int gos_imu_accel_read(gos_accel_sample_t *out, int max)
{
    return hal_imu_accel_read((hal_accel_sample_t *)out, max);
}

void gos_input_set_aim_mode(gos_aim_mode_t m)
{
    aim_mode = m;
    gyro_aim_x = gyro_aim_y = drag_aim_x = drag_aim_y = 0;
    fx.init = fy.init = false;
}

gos_aim_mode_t gos_input_get_aim_mode(void) { return aim_mode; }

void gos_input_calibrate_neutral(void)
{
    hal_pose_t pose;
    hal_imu_get(&pose);
    neutral_pitch = pose.pitch;
    neutral_roll = pose.roll;
    gyro_aim_x = gyro_aim_y = 0;
    drag_aim_x = drag_aim_y = 0;
    fx.init = fy.init = false;
}

void gos_input_set_aim_config(const gos_aim_cfg_t *c) { cfg = *c; }
gos_aim_cfg_t *gos_input_aim_config(void) { return &cfg; }

// --- per-game neutral persistence (sys namespace) --------------------------

void gos_input_save_neutral(const char *game_id)
{
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof key, "n_%.12s", game_id);
    float v[2] = { neutral_pitch, neutral_roll };
    nvs_set_blob(h, key, v, sizeof v);
    nvs_commit(h);
    nvs_close(h);
}

void gos_input_load_neutral(const char *game_id)
{
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READONLY, &h) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof key, "n_%.12s", game_id);
    float v[2];
    size_t len = sizeof v;
    if (nvs_get_blob(h, key, v, &len) == ESP_OK && len == sizeof v) {
        neutral_pitch = v[0];
        neutral_roll = v[1];
    }
    nvs_close(h);
    fx.init = fy.init = false;
}
