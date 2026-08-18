// golf_int.h — shared internals of the Infinite Golf port (logic in golf.c,
// rendering + background pipeline in golf_render.c). The port is 1:1 with
// the standalone infinite-golf firmware: full 368x448 RGB565 rendering via
// the OS's GOS_CAP_FB565 direct mode, original world scale, original
// constants, Montserrat type via gos_gfx_text565.
#pragma once
#include "gos.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define SCREEN_W GOS_PANEL_W
#define SCREEN_H GOS_PANEL_H
#define WORLD_W 736
#define WORLD_H 896

// Logic tick. The standalone's loop was NOMINALLY 20 ms, but its
// bsp_display_lock stalls stretched every iteration 2-3x — the game the
// user tuned and played ticked at ~20-25 Hz wall-clock. All per-tick
// constants (ball velocity, friction, pans, wipes) were calibrated under
// that stall, so the port ticks at the delivered rate, not the nominal one.
// User-tuned by feel: 50 Hz was exactly 2x too fast, 25 Hz still "a bit
// too fast", 20 Hz close — 16.7 Hz with render interpolation is the match.
// The renderer lerps ball + camera between ticks, so the tick rate is a
// pure speed knob and never a smoothness cost.
#define TICK_S 0.06f

#define BALL_RADIUS 7
#define HOLE_RADIUS 8
#define MAX_POWER 15.0f
#define FRICTION 0.977f
#define SAND_FRICTION 0.55f
#define ROUGH_FRICTION 0.90f
#define MIN_VELOCITY 0.4f
#define DRAG_SCALE 0.12f

typedef enum { CLUB_PUTTER = 0, CLUB_CHIPPER, CLUB_DRIVER, CLUB_COUNT } club_t;
extern const char *club_names[CLUB_COUNT];
extern const float club_mult[CLUB_COUNT], club_airk[CLUB_COUNT],
    club_keep[CLUB_COUNT], club_curl[CLUB_COUNT], club_sand_eff[CLUB_COUNT],
    club_sand_wobble[CLUB_COUNT], club_zoom[CLUB_COUNT];

typedef enum {
    GST_LOADING, GST_TITLE, GST_INTRO, GST_READY, GST_AIMING, GST_ARMED,
    GST_MOVING, GST_HOLE_IN, GST_CARD, GST_WIPE, GST_ROUND_END, GST_INITIALS,
    GST_OPTIONS, GST_CLEAR_CONFIRM, GST_PLAYER_COUNT, GST_PASS
} golf_state_t;

typedef enum {
    LAYOUT_STRAIGHT = 0, LAYOUT_DOGLEG, LAYOUT_SHORT, LAYOUT_WATER,
    LAYOUT_SCURVE, LAYOUT_COUNT
} layout_t;

// swing detector (the standalone's swing_task constants, verbatim)
typedef enum { SWING_SETTLE, SWING_WAIT, SWING_BACK, SWING_STROKE } swing_phase_t;
#define SWING_SETTLE_US 150000
#define SWING_BACK_TH 0.22f
#define SWING_FWD_TH 0.25f
#define SWING_END_TH 0.12f
#define SWING_END_QUIET_US 60000
#define SWING_MAX_STROKE_US 500000
#define SWING_BACK_TIMEOUT_US 1200000
#define SWING_V_MIN 0.04f
#define SWING_V_MAX 0.28f
#define SWING_MIN_POWER 2.5f

#define MAX_BLOBS 5
#define MAX_HAZARDS 8
#define MAX_LOBES 10
#define ROUND_HOLES 18
#define BOARD_SIZE 5
#define MAX_CONF 56
#define MAX_PLAYERS 4
#define BALL_COLORS 6
#define INTRO_HOLD 8
#define DROP_ANIM_FRAMES 14
#define SDF_STEP 4
#define GRID_W (WORLD_W / SDF_STEP + 2)
#define GRID_H (WORLD_H / SDF_STEP + 2)
#define TITLE_HOLD_US 5000000

// options rows (SOUND deliberately absent: the OS owns volume)
#define OPT_RESUME_Y0 92
#define OPT_RESUME_Y1 156
#define OPT_SWING_Y0 174
#define OPT_SWING_Y1 238
#define OPT_NEWRND_Y0 256
#define OPT_NEWRND_Y1 320
#define CLR_CLEAR_Y0 272
#define CLR_CLEAR_Y1 336
#define INI_COL_X(i) (114 + (i) * 70)
#define INI_TOP_Y 150
#define INI_MID_Y 233
#define INI_BOT_Y 316
#define INI_OK_Y 324
#define PC_ROW_Y(i) (168 + (i) * 66)

typedef struct {
    int nlobes;
    float lx[MAX_LOBES], ly[MAX_LOBES], lr[MAX_LOBES];
    int type;    // 0=sand, 1=water
    int bridge;
} hazard_t;

typedef struct {
    char ini[4];
    int16_t diff;
    uint16_t strokes;
} board_entry_t;

typedef struct {
    float x, y, sx, sy;
    int strokes, round_strokes;
    club_t club;
    bool holed, seen_intro;
    uint8_t color;
} player_t;

typedef enum { BG_IDLE, BG_ROUGH, BG_GRIDS, BG_PAINT, BG_PATH, BG_TREES,
               BG_DECOR, BG_DONE } bg_phase_t;

typedef struct {
    golf_state_t st;
    uint32_t rng_state, run_seed, hole_seed;
    float acc;                    // 20 ms logic accumulator

    // hole
    int num_blobs;
    float blob_x[MAX_BLOBS], blob_y[MAX_BLOBS], blob_r[MAX_BLOBS];
    float spine_tee_x, spine_tee_y, spine_ctl_x, spine_ctl_y;
    float spine_pin_x, spine_pin_y;
    hazard_t hazards[MAX_HAZARDS];
    int num_hazards;
    float hole_x, hole_y;
    float wind_x, wind_y, wind_speed;
    int par, current_hole;
    layout_t prev_layout;

    // ball / round
    float ball_x, ball_y, ball_vx, ball_vy;
    float shot_start_x, shot_start_y;
    int stroke_count, round_strokes, round_par, holes_completed;
    int stat_aces, stat_under, stat_pars, stat_over;
    int air_time, air_total;
    club_t club, shot_club;

    // camera
    float cam_cx, cam_cy, cam_x, cam_y, cam_zoom;
    int intro_t, intro_pan;

    // render interpolation: previous-tick snapshots + blend factor, so the
    // 60 fps renderer glides between logic ticks (lerped in render_frame;
    // teleports are snap-guarded by distance)
    float prev_ball_x, prev_ball_y;
    float prev_cam_x, prev_cam_y, prev_cam_zoom;
    float render_alpha;

    // input
    bool touch_was, is_dragging;
    float drag_start_x, drag_start_y, drag_cur_x, drag_cur_y;
    int hold_frames, release_age, idle_frames;

    // multiplayer
    player_t players[MAX_PLAYERS];
    int num_players, cur_player;
    bool intro_to_pass, hole_scored;

    // swing mode
    bool swing_mode, imu_ok;
    float aim_dx, aim_dy;
    swing_phase_t swing_phase;
    float grav_x, grav_y, grav_z, back_x, back_y, back_z, back_w, stroke_v;
    int64_t phase_start_us, quiet_us, last_us;
    bool swing_listen, swing_fired;
    float swing_frac, swing_bar_fill, swing_tick;
    int swing_bar_frames;

    // cards / flow
    int wipe_x, card_frames;
    bool card_drawn, next_ready;
    int drop_anim;
    float drop_from_x, drop_from_y;
    int64_t holein_us, notice_until_us, title_hold_us;
    char notice[40];
    float title_phase;
    int title_t;
    bool options_from_title, pending_round, attract_rebuild;

    // confetti
    float conf_x[MAX_CONF], conf_y[MAX_CONF], conf_vx[MAX_CONF], conf_vy[MAX_CONF];
    int16_t conf_life[MAX_CONF];
    uint16_t conf_col[MAX_CONF];
    bool conf_active;

    // leaderboard
    board_entry_t board[BOARD_SIZE];
    int board_count;
    char entry_ini[3];

    // hole indicator label anchor (set by the renderer each frame)
    bool ind_visible;
    float ind_x, ind_y;

    // background pipeline
    bg_phase_t bg_phase;
    int bg_row;
    bool rough_done;              // once per launch
    int path_np;
    float path_xs[224], path_ys[224];

    // world art (state lands in PSRAM via the shell's 8BIT fallback)
    float green_grid[GRID_W * GRID_H];
    float sand_grid[GRID_W * GRID_H];
    float water_grid[GRID_W * GRID_H];
    float band_lut[WORLD_W + WORLD_H];
    int8_t mottle[WORLD_W * WORLD_H];
    uint16_t rough[WORLD_W * WORLD_H];
    uint16_t background[WORLD_W * WORLD_H];

    gos_rng_t *hwrng;
} golf_t;

extern golf_t *G;
extern uint16_t *CV;              // the 368x448 RGB565 canvas (OS-owned)
extern const uint8_t ball_rgb[BALL_COLORS][3];

// ---- shared helpers (golf.c) ----
float golf_randf(void);
float noise2d(float x, float y);
uint32_t pixel_hash(int x, int y);
float clampf(float v, float lo, float hi);
float smoothstepf(float e0, float e1, float x);
float dist2d(float x1, float y1, float x2, float y2);
float sdf_green(float x, float y);
float sdf_hazard(int h, float x, float y);
float lie_wobble(void);
float shot_travel_frac(float frac);
float pin_power_frac(void);
void course_path_point(float p, float *ox, float *oy);
void golf_sfx(int id);

// sfx ids (mapped to gos synth tables in golf.c)
enum { SFXG_PUTT, SFXG_CHIP, SFXG_DRIVE, SFXG_SAND, SFXG_ARM, SFXG_LAND,
       SFXG_SPLASH, SFXG_CUP, SFXG_TAP, SFXG_TICK, SFXG_CLUB, SFXG_POP,
       SFXG_WIPE, SFXG_PAR, SFXG_BIRDIE, SFXG_BOGEY, SFXG_ACE, SFXG_ROUND,
       SFXG_TITLE };

// ---- rendering (golf_render.c) ----
static inline uint16_t rgb565c(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
uint16_t pack_rgb(int r, int g, int b);
uint16_t dim565f(uint16_t c, float f);
static inline float w2sx(float wx) { return (wx - G->cam_x) * G->cam_zoom; }
static inline float w2sy(float wy) { return (wy - G->cam_y) * G->cam_zoom; }

void golf_bg_tick(void);          // one background-build slice (20 ms tick)
void blit_view(int max_cols);
void render_frame(void);          // play states, verbatim original
void draw_loading(void);
void title_frame(void);
void draw_options_panel(void);
void show_scorecard_card(void);   // draw-once canvas-holding screens
void card_tap_hint(void);         // "tap to continue" once next hole is ready
void show_round_end_card(void);
void draw_initials_screen(void);
void enter_initials_card(void);
void show_clear_confirm_card(void);
void show_player_count_card(void);
void draw_pass_card(int i);
void dim_canvas(float f);
void spawn_confetti(void);
void update_confetti(void);
const char *score_name(int strokes, int hole_par);
