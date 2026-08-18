// bomber_int.h — shared internals of BOMBER (contract/logic in bomber.c,
// terrain generation in bomber_terrain.c, frame rendering in
// bomber_render.c, card screens in bomber_cards.c).
//
// Top-down WWII bomber over an endless procedurally generated strip.
// CRUISE: tilt roll steers laterally over the scrolling landscape.
// SIGHT: the BOOT button (GOS_BTN_A) commits to a straight-and-level bomb
// run through a circular bombsight; tilt trims the predicted impact
// against live wind; BOOT again releases. Flak ramps accuracy the longer
// you stay sighted — the stay-or-bail bet is the core loop.
// Full-res RGB565 via GOS_CAP_FB565.
#pragma once
#include "gos.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SCREEN_W GOS_PANEL_W
#define SCREEN_H GOS_PANEL_H

// world strip: 2 screens wide; ring buffer of generated rows
#define STRIP_W 736
#define RING_H 1024
#define RING_MASK (RING_H - 1)

// flight
#define CRUISE_SPEED 90.0f      // px/s forward, constant
#define MAX_LATERAL 130.0f      // px/s at full tilt
#define PLANE_SCREEN_Y 312     // plane sits low: most of the screen is ahead
#define GEN_AHEAD 64            // generation slack beyond the visible top

// bombsight
#define SIGHT_CX 184
#define SIGHT_CY 234
#define SIGHT_R 150
#define SIGHT_ZOOM 2
#define BOMB_TOF 1.5f
#define TRIM_LATERAL 45.0f      // px/s of lateral slew at full roll
#define TRIM_SPEED 0.20f        // +-20% airspeed trim at full pitch
#define MAX_BOMBS 6
#define BOMBS_INFLIGHT 4

// combat
#define HIT_DIRECT 22.0f
#define HIT_NEAR 40.0f
#define MAX_TARGETS 16
#define MAX_FLAK 8
#define MAX_BURSTS 24
#define MAX_TRUCKS 12
#define FLAK_PERIOD_US 900000
#define FLAK_SIGMA_MIN 14.0f
#define FLAK_SIGMA_RANGE 46.0f
#define FLAK_RAMP_TAU 1.4f      // seconds of sight time to shed accuracy slack

// sortie
#define SORTIE_ROWS 5400
#define COAST_ROWS 300          // sea band at the strip's end
#define BOARD_SIZE 5

// on-screen FIRE button in the sight (touch players found tap-the-glass
// unintuitive): big red button on the lower bezel; frame taps bail
#define FIRE_BTN_X 184
#define FIRE_BTN_Y 414
#define FIRE_BTN_R 26

typedef enum {
    BST_TITLE, BST_BRIEF, BST_CRUISE, BST_SIGHT, BST_DEBRIEF,
    BST_RUN_END, BST_INITIALS
} bomber_state_t;

typedef enum { TGT_DEPOT, TGT_BRIDGE, TGT_RADAR, TGT_CONVOY } target_kind_t;
typedef enum { TS_INTACT, TS_BURNING, TS_DEAD } target_state_t;

typedef struct {
    float x, y;                 // center, world coords
    uint8_t kind, state, defended;
    uint8_t pad;
} target_t;

typedef struct {
    float x, y;
    int64_t next_fire_us;
    bool telegraphed;           // first shot is a guaranteed wide miss
} flak_t;

typedef struct {
    float x, y;                 // world coords
    float age;                  // seconds since burst
    uint8_t kind;               // 0 = flak burst, 1 = bomb impact, 2 = fire puff
    bool alive;
} burst_t;

typedef struct {
    float x, y, vx, vy;
    float t_left;               // seconds until impact
    bool alive;
} bomb_t;

typedef struct {                // convoy trucks (moving targets, sortie 3+)
    float y;                    // world row; x follows the road
    int8_t target_ix;           // owning convoy target (-1 dead truck)
    bool alive;
} truck_t;

typedef struct {
    char ini[4];
    uint32_t score;
    uint16_t sorties, targets;
} board_entry_t;

typedef struct {
    bomber_state_t st;
    uint32_t sortie_seed;
    int sortie_num;             // 1-based

    // flight
    float plane_x, plane_y, plane_vx;
    float cam_x;
    float bank;                 // smoothed, for the sprite

    // terrain generation frontier
    int gen_row;

    // sortie plan (deterministic from sortie_seed)
    float road_x0, road_x1;     // road polyline endpoints (lerp by row)
    float river_phase, river_amp, river_cx0;
    target_t targets[MAX_TARGETS];
    int n_targets;
    flak_t flak[MAX_FLAK];
    int n_flak;

    // wind
    float wind_base_x, wind_base_y, wind_base;   // px/s
    float wind_x, wind_y;       // live (gusts applied)
    float gust_t;

    // sight
    float trim_x, trim_speed;   // lateral px/s, airspeed factor offset
    int64_t sight_enter_us;
    float t_sight;              // continuous seconds in sight (wall clock)
    bool sight_backdrop_drawn;
    float shake;                // decaying screen shake, px

    // ordnance
    bomb_t bombs[BOMBS_INFLIGHT];
    int bombs_left;
    truck_t trucks[MAX_TRUCKS];
    int n_trucks;

    // combat
    burst_t bursts[MAX_BURSTS];
    float damage;               // 0..100
    int score, run_targets, sortie_hits, sortie_drops, sortie_kills;
    int64_t out_of_bombs_us;    // grace timer, 0 = not armed

    // input
    bool touch_was;
    int16_t press_x, press_y;   // full-res coords
    int64_t press_us;
    int64_t a_down_us;          // button press start (0 = press predates sight)
    bool a_bailed;              // this hold already bailed; ignore its release
    int64_t last_drop_us;       // FIRE button flash feedback
    uint8_t aim_mode;

    // cards / flow
    bool card_drawn;
    int64_t card_us, last_us;
    float title_drift;
    int64_t title_reseed_us;
    const char *end_reason;

    // leaderboard
    board_entry_t board[BOARD_SIZE];
    int board_count;
    char entry_ini[3];

    // audio
    int h_engine;

    // BIG: terrain ring (736*1024*2 = 1.44 MB -> PSRAM via shell fallback)
    uint16_t ring[RING_H * STRIP_W];

    gos_rng_t *hwrng;
} bomber_t;

// short names inside bomber's own files; the linker sees bomber_-prefixed
// symbols (golf already owns plain G/CV)
#define G bomber_G
#define CV bomber_CV
extern bomber_t *G;
extern uint16_t *CV;

// sfx ids (tables live in bomber.c)
enum { BSFX_CLUNK, BSFX_WHISTLE, BSFX_CRUMP, BSFX_FLAK_POP, BSFX_FLAK_CRACK,
       BSFX_HIT, BSFX_DESTROY, BSFX_BAIL, BSFX_DEBRIEF, BSFX_TITLE,
       BSFX_TAP, BSFX_TICK, BSFX_KLAXON };
void bomber_sfx(int id);

// ---- helpers shared across files ----
static inline uint16_t b565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline float bclampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
uint32_t bomber_hash(int x, int y);          // seeded per sortie
float bomber_noise(float x, float y);
float road_cx(float row);
float river_cx(float row);
uint16_t bomber_sea_px(int row, int x);      // open water for pre-strip rows

// screen <-> world (no rotation: the strip scrolls, the sprite banks)
static inline float cam_top(void) { return G->plane_y + PLANE_SCREEN_Y; }
static inline int w2sy(float wy) { return (int)(cam_top() - wy); }
static inline int w2sx(float wx) { return (int)(wx - G->cam_x); }

// ---- terrain (bomber_terrain.c) ----
void terrain_reset(uint32_t seed);           // plan the sortie + reset frontier
void terrain_gen_to(int row);                // generate rows up to (and incl.)
void terrain_stamp_crater(float wx, float wy);
void terrain_stamp_dead(const target_t *t);  // scorched art over a destroyed site
void paint_target_row(const target_t *t, int row, uint16_t *rowbuf);

// ---- rendering (bomber_render.c) ----
void render_cruise(void);
void render_sight(void);
void sight_backdrop(void);                   // vignette/bezel, drawn once
void impact_point(float tof, float *ix, float *iy);   // shared with logic

// ---- cards (bomber_cards.c) ----
void render_title(void);                     // per-frame attract
void draw_brief_card(void);
void draw_debrief_card(void);
void draw_run_end_card(void);
void draw_initials_card(void);
const char *target_name(int kind);
