// gos.h — the complete game-facing API of the handheld game OS.
//
// This is the only header a game may include. Games never touch the HAL:
// no display driver, no touch controller, no IMU. Everything arrives through
// the services below and the gos_game_t contract at the bottom.
//
// Deviation from the draft spec, agreed at bring-up: gos_color_t is an 8-bit
// palette index, not RGB565. The whole render pipeline is 184x224 indexed-8;
// the flush pass applies a 256-entry RGB565 LUT while it 2x-upscales to the
// 368x448 panel. The gunship needs the indexed path anyway and it halves fill
// bandwidth for everyone else.

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOS_SCREEN_W 184
#define GOS_SCREEN_H 224
// physical panel (full-res RGB565 direct mode renders at this size)
#define GOS_PANEL_W 368
#define GOS_PANEL_H 448

typedef uint8_t gos_color_t;                       // palette index
typedef struct { int16_t x, y; } gos_pt_t;
typedef struct { int16_t x, y, w, h; } gos_rect_t;

// Default (shell) palette. Restored automatically when a game exits.
// Indices 16..79 are a 64-step black->white ramp: GOS_GRAY64(0..63).
enum {
    GOS_BLACK = 0, GOS_DARKGRAY, GOS_GRAY, GOS_LTGRAY, GOS_WHITE,
    GOS_RED, GOS_ORANGE, GOS_YELLOW, GOS_GREEN, GOS_CYAN, GOS_BLUE,
    GOS_PURPLE, GOS_DARKGREEN, GOS_NAVY, GOS_BROWN, GOS_AMBER,
};
#define GOS_GRAY64(i) ((gos_color_t)(16 + (i)))

// ---------------------------------------------------------------------------
// Graphics — immediate mode, full redraw every frame.
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t w, h;
    const uint8_t *px;      // 8bpp palette indices, 0xFF = transparent
} gos_sprite_t;

void gos_gfx_clear(gos_color_t c);
void gos_gfx_pixel(int x, int y, gos_color_t c);
void gos_gfx_hline(int x0, int x1, int y, gos_color_t c);
void gos_gfx_vline(int x, int y0, int y1, gos_color_t c);
void gos_gfx_line(int x0, int y0, int x1, int y1, gos_color_t c);
void gos_gfx_rect(gos_rect_t r, gos_color_t c);
void gos_gfx_rect_fill(gos_rect_t r, gos_color_t c);
void gos_gfx_circle(int cx, int cy, int r, gos_color_t c);
void gos_gfx_circle_fill(int cx, int cy, int r, gos_color_t c);
// Radial-falloff blob: palette index `hot` at the center fading to `base` at
// the rim, max-blended into the framebuffer so overlapping blobs merge.
// The thermal workhorse. Radius capped at 16.
void gos_gfx_circle_soft(int cx, int cy, int r, uint8_t hot, uint8_t base);
// Filled convex quad (used for camera-rotated rectangles).
void gos_gfx_quad_fill(const gos_pt_t p[4], gos_color_t c);
void gos_gfx_blit(const gos_sprite_t *s, int x, int y);
// Rotation about the sprite center; angle256: 0..255 = full turn.
void gos_gfx_blit_rot(const gos_sprite_t *s, int x, int y, uint8_t angle256);
// font: 0 = 5x7, 1 = 10x14 (2x). Returns nothing; clips like everything else.
void gos_gfx_text(int font, int x, int y, gos_color_t c, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
int  gos_gfx_text_w(int font, const char *s);
void gos_gfx_set_clip(gos_rect_t r);
void gos_gfx_clear_clip(void);

// Palette upload (values are normal RGB565; the OS handles panel byte order).
void gos_gfx_set_palette(const uint16_t *rgb565, int count, int offset);
void gos_gfx_set_default_palette(void);
// Darken every 3rd output row during flush (thermal-monitor scanlines).
void gos_gfx_scanlines(bool on);
// Raw draw buffer (184*224 bytes, row-major) for custom effects (sensor noise).
uint8_t *gos_gfx_fb(void);

// --- Full-resolution direct mode (GOS_CAP_FB565) ---------------------------
// A game that declares GOS_CAP_FB565 renders straight into a 368x448 RGB565
// buffer (normal byte order; PSRAM; single-buffered) and none of the indexed
// primitives above apply to it — the game owns every pixel. The OS enables
// the mode at launch and always restores the indexed pipeline on exit, and
// its own overlays (pause, calibration) temporarily switch back to indexed.
uint16_t *gos_gfx_fb565(void);
void gos_gfx_direct565(bool on);   // shell-managed; games needn't call it
// Montserrat text into the fb565 buffer (px 14/18/22/28, alpha-blended,
// class-kerned — the same LVGL font data the standalone builds shipped)
void gos_gfx_text565(int px, int x, int y, uint16_t rgb565, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
int  gos_gfx_text565_w(int px, const char *s);
int  gos_gfx_text565_h(int px);

// Q14 sin/cos, angle256 domain — table lookups, never call trig per entity.
int16_t gos_sin256(uint8_t a);
int16_t gos_cos256(uint8_t a);

// ---------------------------------------------------------------------------
// Input — normalized snapshot; games never learn what produced it.
// ---------------------------------------------------------------------------

#define GOS_BTN_FIRE (1u << 0)
#define GOS_BTN_ALT  (1u << 1)
#define GOS_BTN_A    (1u << 2)
#define GOS_BTN_B    (1u << 3)
#define GOS_BTN_MENU (1u << 4)

typedef struct {
    float    aim_x, aim_y;   // -1..1, (0,0) = center of screen
    bool     aim_active;
    uint32_t buttons;        // held this frame
    uint32_t pressed;        // edge: went down this frame
    uint32_t released;       // edge: went up this frame
    gos_pt_t touch;          // render-space coords (184x224)
    bool     touching;
    float    shake;          // 0..1 impulse, decays
    // diagnostics only — do not build gameplay on these
    float    pitch, roll;    // fused, degrees, neutral-relative
    float    gx, gy, gz;     // gyro dps; x/y sign-corrected: +gx = roll/aim
                             // right, +gy = pitch/aim down (learned online)
} gos_input_t;

typedef enum { GOS_AIM_TILT_ABS, GOS_AIM_GYRO_RATE, GOS_AIM_TOUCH_DRAG } gos_aim_mode_t;

typedef struct {
    float range_deg;    // tilt covering full deflection (default 25)
    float curve_pow;    // response curve exponent (default 1.4)
    float deadzone;     // after curve (default 0.04)
    float min_cutoff;   // one-euro (default 1.0)
    float beta;         // one-euro (default 0.007)
    float d_cutoff;     // one-euro (default 1.0)
    float gyro_gain;    // GYRO_RATE multiplier: 1.0 = rotating the device by
                        // range_deg degrees sweeps full deflection (default 1.0)
    bool  invert_x, invert_y;
} gos_aim_cfg_t;

// --- Raw accel sample stream (g units, wall-clock stamped). The IMU samples
// at ~200 Hz; frame-rate polling aliases short motion bursts, so games that
// detect gestures (a golf swing, a shake-to-X) drain this ring each update
// and run their own detector over every sample. Single consumer. Returns
// samples copied since the last call, 0 if none, -1 if the IMU is absent.
typedef struct { int64_t t_us; float ax, ay, az; } gos_accel_sample_t;
int gos_imu_accel_read(gos_accel_sample_t *out, int max);

const gos_input_t *gos_input_get(void);
void gos_input_set_aim_mode(gos_aim_mode_t m);
gos_aim_mode_t gos_input_get_aim_mode(void);
void gos_input_calibrate_neutral(void);
void gos_input_set_aim_config(const gos_aim_cfg_t *cfg);
gos_aim_cfg_t *gos_input_aim_config(void);   // live pointer for tuning UIs

// ---------------------------------------------------------------------------
// Audio — 8-voice chip synth + PCM, 22050 Hz mono.
// ---------------------------------------------------------------------------

typedef enum { GOS_W_SQ50, GOS_W_SQ25, GOS_W_TRI, GOS_W_SAW, GOS_W_NOISE, GOS_W_REST } gos_wave_t;

typedef struct {
    uint16_t f0, f1;    // Hz, glides f0 -> f1 over the note
    uint16_t ms;
    uint8_t  wave;      // gos_wave_t
    uint8_t  vol;       // 0..255
} gos_note_t;

typedef struct {
    const gos_note_t *notes;
    uint8_t count;
    uint8_t prio;       // higher survives voice stealing
} gos_sfx_t;

int  gos_audio_play(const gos_sfx_t *s, uint8_t vol);    // vol 0..255; returns handle or -1
int  gos_audio_loop(const gos_sfx_t *s, uint8_t vol);    // repeats until stopped
void gos_audio_stop(int handle);
void gos_audio_set_vol(int handle, uint8_t vol);         // live (ducking, throttle)
void gos_audio_stop_all(void);
void gos_audio_master(int level);                        // 0=mute..3
int  gos_audio_master_get(void);

// ---------------------------------------------------------------------------
// RNG — per-game xorshift32 stream (in gos_ctx_t), replay-reproducible.
// ---------------------------------------------------------------------------

typedef struct { uint32_t s; } gos_rng_t;
uint32_t gos_rand(gos_rng_t *r);
int      gos_rand_range(gos_rng_t *r, int lo, int hi);   // inclusive
float    gos_randf(gos_rng_t *r);                        // [0,1)

// ---------------------------------------------------------------------------
// Spatial hash grid — one shared instance, rebuild each frame.
// ---------------------------------------------------------------------------

void gos_grid_init(int cell_size, int world_w, int world_h);
void gos_grid_clear(void);
void gos_grid_insert(uint16_t id, int16_t x, int16_t y);
int  gos_grid_query(int16_t x, int16_t y, int16_t r, uint16_t *out, int max);

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

uint32_t gos_time_ms(void);
int64_t  gos_time_us(void);

// ---------------------------------------------------------------------------
// Game contract
// ---------------------------------------------------------------------------

#define GOS_CAP_IMU        (1u << 0)
#define GOS_CAP_AUDIO      (1u << 1)
#define GOS_CAP_SAVE       (1u << 2)
// FIRE button = any touch in the lower 2/3 of the screen
#define GOS_CAP_TOUCH_FIRE (1u << 3)
// render full-res 368x448 RGB565 via gos_gfx_fb565() instead of the indexed
// 184x224 pipeline (for ports that need the panel's native fidelity)
#define GOS_CAP_FB565      (1u << 4)

struct gos_game_s;

typedef struct gos_ctx_s {
    void      *state;        // state_size bytes, zeroed, allocated by the OS
    gos_rng_t  rng;
    const struct gos_game_s *game;
} gos_ctx_t;

// Save data: one blob per game, versioned by the game itself, max 4 KB.
bool gos_save_write(gos_ctx_t *ctx, const void *blob, size_t len);
int  gos_save_read(gos_ctx_t *ctx, void *out, size_t max);   // bytes read, -1 if none

// Ask the OS to quit back to the launcher (honored after the current update;
// suspend/teardown run normally). For explicit EXIT buttons on game screens —
// the OS-level escape (swipe down from the top edge, or BOOT) always works too.
void gos_request_exit(void);

typedef struct gos_game_s {
    const char *id;              // stable, used as the save key
    const char *title;
    const gos_sprite_t *icon;    // 48x48 or NULL (shell draws a monogram)
    uint32_t caps;               // GOS_CAP_*
    size_t   state_size;

    void (*init)    (gos_ctx_t *ctx);
    void (*update)  (gos_ctx_t *ctx, float dt, const gos_input_t *in);
    void (*render)  (gos_ctx_t *ctx);
    void (*suspend) (gos_ctx_t *ctx);   // menu overlay / low battery interrupt
    void (*resume)  (gos_ctx_t *ctx);
    void (*teardown)(gos_ctx_t *ctx);
} gos_game_t;

#ifdef __cplusplus
}
#endif
