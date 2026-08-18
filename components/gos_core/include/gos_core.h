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

void gos_save_init(void);
// consume a pending gos_request_exit() (shell only)
bool gos_core_take_exit(void);
// sys settings blob (shell-owned)
typedef struct {
    uint8_t version;
    uint8_t aim_mode;       // gos_aim_mode_t
    uint8_t sensitivity;    // 0..2 (LOW/MED/HIGH) -> 35/25/15 deg range
    uint8_t invert_x, invert_y;
    uint8_t volume;         // 0..3
    uint8_t brightness;     // 1..4 -> 25/50/75/100
    uint8_t calibrated;     // first-run wizard done
} gos_settings_t;
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
