// gos_core.h — OS-internal core interfaces (shell and main only; not games).
#pragma once
#include "gos.h"

#ifdef __cplusplus
extern "C" {
#endif

void gos_gfx_init(void);
void gos_gfx_present(void);

void gos_input_init(void);
void gos_input_update(float dt);
// per-game neutral pose persistence (sys NVS)
void gos_input_load_neutral(const char *game_id);
void gos_input_save_neutral(const char *game_id);
// set the neutral from angles the caller computed itself (TILT SETUP, which
// knows the play pose from before it changed the axis map)
void gos_input_set_neutral(float pitch, float roll);

void gos_save_init(void);
// consume a pending gos_request_exit() (shell only)
bool gos_core_take_exit(void);
// sys settings blob (shell-owned). Version 2 added the tilt axis map; a
// version-1 blob is a different size and simply fails to load, which lands
// every field on its default, the same thing a first boot does.
typedef struct {
    uint8_t version;
    uint8_t aim_mode;       // gos_aim_mode_t
    uint8_t sensitivity;    // 0..2 (LOW/MED/HIGH) -> 35/25/15 deg range
    uint8_t invert_x, invert_y;
    uint8_t volume;         // 0..3
    uint8_t brightness;     // 1..4 -> 25/50/75/100
    uint8_t calibrated;     // first-run wizard done
    // accel->angle map measured by TILT SETUP; all zero = the shipped mapping
    uint8_t tilt_pitch_axis;   // 0 = accel X feeds pitch, 1 = accel Y
    uint8_t tilt_pitch_neg;    // pitch reads negated
    uint8_t tilt_roll_neg;     // roll reads negated
} gos_settings_t;
#define GOS_SETTINGS_VERSION 2
extern gos_settings_t g_settings;
void gos_settings_load(void);
void gos_settings_save(void);
void gos_settings_apply(void);

void gos_mixer_init(void);

// frame loop (never returns; runs the shell on core 1)
void gos_loop_start(void);

// shell entry points used by the loop
void shell_init(void);
void shell_frame(const gos_input_t *in, int n_updates);

// game registry
extern const gos_game_t *const g_games[];
extern const int g_game_count;

#ifdef __cplusplus
}
#endif
