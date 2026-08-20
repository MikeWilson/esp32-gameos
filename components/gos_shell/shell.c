// shell.c — launcher grid, settings, pause overlay, calibration wizard,
// debug overlay, and the per-frame orchestration of the running game.
#include "gos.h"
#include "gos_core.h"
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "gos_hal.h"

typedef enum { SH_GRID, SH_SETTINGS, SH_RUNNING, SH_OVERLAY, SH_CALIB,
               SH_TILT } sh_mode_t;

static sh_mode_t mode = SH_GRID;
static sh_mode_t calib_return = SH_GRID;   // where the wizard goes on done

static const gos_game_t *cur;
static gos_ctx_t ctx;

static hal_batt_t batt;
static uint32_t last_poll_ms;

// touch edges. Menus act on the PRESS edge, not a qualified release: this
// touch controller drops release edges and jitters coordinates, so a strict
// release-tap (small slop + timeout) silently eats real inputs — golf hit the
// identical failure mode on this hardware.
static bool t_was_down;
static gos_pt_t t_press;
static uint32_t t_press_ms;
static bool touch_edge;          // finger went down this frame
static bool tap;                 // release-tap (kept for non-critical uses)
static gos_pt_t tap_pt;

// MENU button gestures
static uint32_t menu_down_ms;
static bool debug_overlay;
// swipe gesture re-arms only after a clean no-touch frame, so a drag that
// began before a launch can't chain straight into pausing the new game
static bool swipe_armed = true;

// frame time ring for the debug overlay
static uint8_t ft_ring[64];
static int ft_idx;
static int64_t frame_t0;

// ---------------------------------------------------------------------------

static bool press_consumed;      // a press-edge action already fired this contact

static void detect_tap(const gos_input_t *in)
{
    tap = false;
    touch_edge = false;
    if (in->touching && !t_was_down) {
        t_press = in->touch;
        t_press_ms = gos_time_ms();
        touch_edge = true;
        press_consumed = false;
        ESP_LOGI("touch", "down %d,%d", in->touch.x, in->touch.y);
    } else if (!in->touching && t_was_down) {
        int dx = in->touch.x - t_press.x, dy = in->touch.y - t_press.y;
        ESP_LOGI("touch", "up %d,%d (moved %d)", in->touch.x, in->touch.y,
                 (int)(dx * dx + dy * dy));
        // no hold-time limit: a slow deliberate press is still a press
        if (!press_consumed && dx * dx + dy * dy < 1600) {
            tap = true;
            tap_pt = t_press;
        }
    }
    t_was_down = in->touching;
}

// Finger contact registers well BELOW where the user aims on this panel —
// measured from the field logs: presses meant for a button land 15–25 px
// under its text (golf's notes record the same "low thumbs" offset). All
// shell hit-testing shifts the touch point up by this much.
#define TOUCH_Y_BIAS 12

static bool pt_in(gos_pt_t p, int x, int y, int w, int h)
{
    int py = p.y - TOUCH_Y_BIAS;
    return p.x >= x && p.x < x + w && py >= y && py < y + h;
}

// menu hit test: fires on the press edge OR the release tap, whichever comes
// first — a press-edge hit consumes the contact so one touch acts once.
static bool tap_in(int x, int y, int w, int h)
{
    if (touch_edge && pt_in(t_press, x, y, w, h)) {
        press_consumed = true;
        return true;
    }
    return tap && pt_in(tap_pt, x, y, w, h);
}

// release-only hit test: for actions too big to fire by accident (launching)
static bool tap_release_in(int x, int y, int w, int h)
{
    return tap && pt_in(tap_pt, x, y, w, h);
}

// ---------------------------------------------------------------------------
// Launch / quit
// ---------------------------------------------------------------------------

static void launch(const gos_game_t *g)
{
    void *state = NULL;
    if (g->state_size) {
        state = heap_caps_calloc(1, g->state_size,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!state) state = heap_caps_calloc(1, g->state_size, MALLOC_CAP_8BIT);
        if (!state) {
            ESP_LOGE("shell", "no memory for %s state (%u)", g->id,
                     (unsigned)g->state_size);
            return;
        }
    }
    ESP_LOGI("shell", "launch %s", g->id);
    swipe_armed = false;
    cur = g;
    ctx = (gos_ctx_t){ .state = state, .rng = { esp_random() }, .game = g };
    gos_input_load_neutral(g->id);
    gos_core_take_exit();   // clear any stale exit request
    g->init(&ctx);
    mode = SH_RUNNING;
}

static void quit_game(void)
{
    if (!cur) return;
    ESP_LOGI("shell", "quit %s -> launcher", cur->id);
    // an in-game recenter (gos_input_calibrate_neutral) only touches RAM;
    // persist the neutral here so it survives relaunch like the calib screen's
    if (cur->caps & GOS_CAP_IMU) gos_input_save_neutral(cur->id);
    cur->teardown(&ctx);
    if (ctx.state) heap_caps_free(ctx.state);
    cur = NULL;
    gos_audio_stop_all();
    gos_gfx_set_default_palette();
    gos_gfx_scanlines(false);
    gos_gfx_clear_clip();
    gos_settings_apply();
    mode = SH_GRID;
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

// The rounded panel corners eat ~6 render px at each edge; everything that
// hugs an edge stays inside this margin.
#define SAFE 6

// --- Wii-menu-style shell theme -------------------------------------------
// Light gray field with fine stripes, white rounded "channel" cards, gray
// bottom bar with round buttons. Deliberately the opposite of the gunship's
// dark thermal look — the user confused the two. Built on the GRAY64 ramp
// (palette 16..79) plus GOS_CYAN, which is close to the Wii's accent blue.
#define UI_BG       GOS_GRAY64(50)
#define UI_STRIPE   GOS_GRAY64(52)
#define UI_CARD     GOS_GRAY64(61)
#define UI_EDGE     GOS_GRAY64(38)
#define UI_EMPTY    GOS_GRAY64(46)
#define UI_EMPTY_BD GOS_GRAY64(43)
#define UI_BAR      GOS_GRAY64(55)
#define UI_HILITE   GOS_GRAY64(63)
#define UI_TEXT     GOS_GRAY64(12)
#define UI_DIM      GOS_GRAY64(30)

// rounded rect, radius ~3, as three overlapping fills
static void round_fill(int x, int y, int w, int h, gos_color_t c)
{
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)(x + 2), (int16_t)y,
                                    (int16_t)(w - 4), (int16_t)h }, c);
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)x, (int16_t)(y + 2),
                                    (int16_t)w, (int16_t)(h - 4) }, c);
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)(x + 1), (int16_t)(y + 1),
                                    (int16_t)(w - 2), (int16_t)(h - 2) }, c);
}

static void round_border(int x, int y, int w, int h, gos_color_t c)
{
    gos_gfx_hline(x + 2, x + w - 3, y, c);
    gos_gfx_hline(x + 2, x + w - 3, y + h - 1, c);
    gos_gfx_vline(x, y + 2, y + h - 3, c);
    gos_gfx_vline(x + w - 1, y + 2, y + h - 3, c);
    gos_gfx_pixel(x + 1, y + 1, c);
    gos_gfx_pixel(x + w - 2, y + 1, c);
    gos_gfx_pixel(x + 1, y + h - 2, c);
    gos_gfx_pixel(x + w - 2, y + h - 2, c);
}

// striped background above the bar, Wii-menu style
static void draw_field(void)
{
    gos_gfx_clear(UI_BG);
    for (int y = 2; y < 196; y += 3)
        gos_gfx_hline(0, GOS_SCREEN_W - 1, y, UI_STRIPE);
}

// bottom bar with one round button on the left; battery lives bottom-right
static void draw_bottom_bar(const char *btn)
{
    gos_gfx_rect_fill((gos_rect_t){ 0, 198, GOS_SCREEN_W, 26 }, UI_BAR);
    gos_gfx_hline(0, GOS_SCREEN_W - 1, 198, UI_EDGE);
    gos_gfx_hline(0, GOS_SCREEN_W - 1, 199, UI_HILITE);

    gos_gfx_circle_fill(24, 211, 11, UI_CARD);
    gos_gfx_circle(24, 211, 11, UI_EDGE);
    int bw = gos_gfx_text_w(0, btn);
    gos_gfx_text(0, 24 - bw / 2, 208, UI_TEXT, "%s", btn);

    bool lowbat = batt.present && batt.percent >= 0 && batt.percent <= 15;
    const char *mid = lowbat ? "LOW BATT" : "GAMEOS";
    int gw = gos_gfx_text_w(0, mid);
    gos_gfx_text(0, GOS_SCREEN_W / 2 - gw / 2, 208, lowbat ? GOS_RED : UI_DIM,
                 "%s", mid);

    if (batt.present && batt.percent >= 0) {
        gos_color_t c = batt.percent <= 15 ? GOS_RED : UI_TEXT;
        gos_gfx_text(0, GOS_SCREEN_W - SAFE - 48, 208, c, "%s%3d%%",
                     batt.charging ? "+" : " ", batt.percent);
    } else {
        gos_gfx_text(0, GOS_SCREEN_W - SAFE - 18, 208, UI_DIM, "USB");
    }
}

// The channel grid scrolls vertically once it outgrows the screen: drag
// anywhere in the grid to scroll (same interaction as the settings list),
// release-tap to launch — a contact that scrolled never launches. With six
// or fewer games nothing scrolls and the layout is exactly the classic 2x3.
#define GRID_VIEW_Y0 4
#define GRID_VIEW_Y1 196
#define GRID_ROW_PITCH 64

static int grid_scroll;
static int16_t grid_last_y;
static bool grid_touching;
static int grid_moved;

static void grid_frame(void)
{
    int rows = (g_game_count + 1) / 2;
    if (rows < 3) rows = 3;             // always at least the classic 3 rows
    int content_h = 6 + rows * GRID_ROW_PITCH;
    int view_h = GRID_VIEW_Y1 - GRID_VIEW_Y0;
    int max_scroll = content_h > view_h ? content_h - view_h : 0;

    // drag-to-scroll: the grid follows the finger; real vertical movement
    // marks the contact as a scroll so its release can't launch anything
    const gos_input_t *in = gos_input_get();
    if (in->touching && in->touch.y < GRID_VIEW_Y1 + 20) {
        if (!grid_touching) {
            grid_touching = true;
            grid_last_y = in->touch.y;
            grid_moved = 0;
        } else {
            int dy = in->touch.y - grid_last_y;
            grid_moved += dy < 0 ? -dy : dy;
            grid_scroll -= dy;
            grid_last_y = in->touch.y;
            if (grid_scroll < 0) grid_scroll = 0;
            if (grid_scroll > max_scroll) grid_scroll = max_scroll;
        }
    } else if (!in->touching) {
        grid_touching = false;
    }
    bool can_launch = grid_moved < 12;

    draw_field();
    gos_gfx_set_clip((gos_rect_t){ 0, GRID_VIEW_Y0, GOS_SCREEN_W,
                                   GRID_VIEW_Y1 - GRID_VIEW_Y0 });

    // draw every slot up to a full final row; unfilled slots = empty channels
    int slots = rows * 2;
    for (int i = 0; i < slots; i++) {
        int col = i % 2, row = i / 2;
        int cx = 4 + col * 92, cy = 10 + row * GRID_ROW_PITCH - grid_scroll;
        if (cy + 58 < GRID_VIEW_Y0 || cy > GRID_VIEW_Y1) continue;
        if (i >= g_game_count) {
            round_fill(cx, cy, 84, 58, UI_EMPTY);
            round_border(cx, cy, 84, 58, UI_EMPTY_BD);
            continue;
        }
        const gos_game_t *g = g_games[i];
        round_fill(cx, cy, 84, 58, UI_CARD);
        round_border(cx, cy, 84, 58, UI_EDGE);
        if (g->icon) {
            gos_gfx_blit(g->icon, cx + 18, cy + 5);
        } else {
            char c[2] = { g->title[0], 0 };
            gos_gfx_text(1, cx + 37, cy + 10, GOS_CYAN, "%s", c);
            int tw = gos_gfx_text_w(0, g->title);
            gos_gfx_text(0, cx + 42 - tw / 2, cy + 38, UI_TEXT, "%s", g->title);
        }
        // release-only: a finger merely landing here must never launch a
        // game — and neither may a scroll gesture's release
        if (can_launch && tap_release_in(cx, cy, 84, 70)) {
            gos_gfx_clear_clip();
            launch(g);
            return;
        }
    }
    gos_gfx_clear_clip();

    // slim scrollbar so more games below are discoverable
    if (max_scroll > 0) {
        int track = GRID_VIEW_Y1 - GRID_VIEW_Y0 - 4;
        int th = track * view_h / content_h;
        if (th < 16) th = 16;
        int ty = GRID_VIEW_Y0 + 2 + (track - th) * grid_scroll / max_scroll;
        gos_gfx_vline(GOS_SCREEN_W - 3, GRID_VIEW_Y0 + 2, GRID_VIEW_Y1 - 2, UI_EMPTY);
        gos_gfx_vline(GOS_SCREEN_W - 3, ty, ty + th, UI_DIM);
    }

    draw_bottom_bar("SET");
    if (tap_in(0, GOS_SCREEN_H - 26, 74, 26)) {
        void settings_reset_view(void);
        settings_reset_view();
        mode = SH_SETTINGS;
    }
}

// Settings is a drag-scrollable list of BIG rows. The old fixed layout
// packed 20 px rows — under this panel's 15-25 px touch parallax they were
// untappable (user: "impossible to interact with"). Rows are 44 px pitch
// (well past the 26 px floor), activate on RELEASE only, and any contact
// that scrolled more than a few px never counts as a tap.
#define SET_VIEW_Y0 34
#define SET_VIEW_Y1 196
#define SET_ROW_PITCH 44
#define SET_ROW_H 36

static int set_scroll;        // list pixels scrolled up past the viewport
static int16_t set_last_y;
static bool set_touching;
static int set_moved;         // accumulated |dy| this contact
static int reset_hold;        // factory-reset hold ticks

void settings_reset_view(void)
{
    set_scroll = 0;
    set_touching = false;
    reset_hold = 0;
}

static void tilt_start(void);   // the TILT SETUP wizard, below

static void settings_frame(void)
{
    static const char *aim_names[] = { "TILT", "GYRO", "TOUCH" };
    static const char *sens_names[] = { "LOW", "MED", "HIGH" };
    static const char *vol_names[] = { "OFF", "LOW", "MED", "HIGH" };

    // list content: interactive rows + one section header. System settings
    // first; the shared tilt-aim service sits under its own header (it read
    // as gunship leftovers otherwise — user-reported)
    enum { R_VOLUME, R_BRIGHT, R_HDR, R_AIM, R_SENS, R_INVY, R_INVX,
           R_TILT, R_CALIB, R_RESET, R_COUNT };
    struct { const char *label; char val[12]; } rows[R_COUNT];
    rows[R_VOLUME].label = "VOLUME";
    snprintf(rows[R_VOLUME].val, 12, "%s", vol_names[g_settings.volume % 4]);
    rows[R_BRIGHT].label = "BRIGHTNESS";
    snprintf(rows[R_BRIGHT].val, 12, "%d%%", (g_settings.brightness % 5) * 25);
    rows[R_HDR].label = "TILT GAMES";
    rows[R_HDR].val[0] = 0;
    rows[R_AIM].label = "AIM MODE";
    snprintf(rows[R_AIM].val, 12, "%s", aim_names[g_settings.aim_mode % 3]);
    rows[R_SENS].label = "SENSITIVITY";
    snprintf(rows[R_SENS].val, 12, "%s", sens_names[g_settings.sensitivity % 3]);
    rows[R_INVY].label = "INVERT Y";
    snprintf(rows[R_INVY].val, 12, "%s", g_settings.invert_y ? "ON" : "OFF");
    rows[R_INVX].label = "INVERT X";
    snprintf(rows[R_INVX].val, 12, "%s", g_settings.invert_x ? "ON" : "OFF");
    rows[R_TILT].label = "TILT SETUP";
    rows[R_TILT].val[0] = 0;
    rows[R_CALIB].label = "RECALIBRATE AIM";
    rows[R_CALIB].val[0] = 0;
    rows[R_RESET].label = "FACTORY RESET";
    snprintf(rows[R_RESET].val, 12, "HOLD 3s");

    // row tops in list space (header is shorter than a full row)
    int row_top[R_COUNT], ry = 0;
    for (int i = 0; i < R_COUNT; i++) {
        row_top[i] = ry;
        ry += (i == R_HDR) ? 22 : SET_ROW_PITCH;
    }
    int content_h = ry;
    int view_h = SET_VIEW_Y1 - SET_VIEW_Y0;
    int max_scroll = content_h > view_h ? content_h - view_h : 0;

    // drag-to-scroll: the list follows the finger; any real vertical
    // movement marks this contact as a scroll, never a tap
    const gos_input_t *in = gos_input_get();
    if (in->touching && in->touch.y < SET_VIEW_Y1 + 20) {
        if (!set_touching) {
            set_touching = true;
            set_last_y = in->touch.y;
            set_moved = 0;
        } else {
            int dy = in->touch.y - set_last_y;
            set_moved += dy < 0 ? -dy : dy;
            set_scroll -= dy;
            set_last_y = in->touch.y;
            if (set_scroll < 0) set_scroll = 0;
            if (set_scroll > max_scroll) set_scroll = max_scroll;
        }
    } else if (!in->touching) {
        set_touching = false;
    }
    bool row_tap = tap && set_moved < 12;   // release-tap on a still finger

    draw_field();

    gos_gfx_set_clip((gos_rect_t){ 0, SET_VIEW_Y0, GOS_SCREEN_W,
                                   SET_VIEW_Y1 - SET_VIEW_Y0 });
    for (int i = 0; i < R_COUNT; i++) {
        int y = SET_VIEW_Y0 + row_top[i] - set_scroll;
        if (i == R_HDR) {
            if (y > SET_VIEW_Y0 - 12 && y < SET_VIEW_Y1)
                gos_gfx_text(0, 8, y + 8, UI_DIM, "%s", rows[i].label);
            continue;
        }
        if (y + SET_ROW_H < SET_VIEW_Y0 || y > SET_VIEW_Y1) continue;
        round_fill(4, y, 168, SET_ROW_H, UI_CARD);
        round_border(4, y, 168, SET_ROW_H, UI_EDGE);
        gos_color_t lc = i == R_RESET ? GOS_RED : UI_TEXT;
        gos_gfx_text(0, 12, y + (SET_ROW_H - 7) / 2, lc, "%s", rows[i].label);
        int vw = gos_gfx_text_w(0, rows[i].val);
        gos_gfx_text(0, 164 - vw, y + (SET_ROW_H - 7) / 2,
                     i == R_RESET ? GOS_RED : GOS_BLUE, "%s", rows[i].val);

        // factory reset: press-and-hold with a visible charge bar
        if (i == R_RESET) {
            if (in->touching && set_moved < 12 &&
                pt_in(in->touch, 0, y, GOS_SCREEN_W, SET_ROW_PITCH)) {
                if (++reset_hold > 180) {   // 3 s
                    nvs_flash_erase();
                    esp_restart();
                }
            } else if (!in->touching) {
                reset_hold = 0;
            }
            if (reset_hold > 0) {
                int w = 164 * reset_hold / 180;
                gos_gfx_rect_fill((gos_rect_t){ 6, (int16_t)(y + SET_ROW_H - 5),
                                                (int16_t)w, 3 }, GOS_RED);
            }
            continue;
        }

        // zones are the full 44 px pitch (pt_in already biases 12 px up
        // for the panel's low-thumb parallax)
        if (row_tap && pt_in(tap_pt, 0, y, GOS_SCREEN_W, SET_ROW_PITCH)) {
            row_tap = false;   // one row per tap
            switch (i) {
            case R_VOLUME: g_settings.volume = (g_settings.volume + 1) % 4; break;
            case R_BRIGHT: g_settings.brightness = g_settings.brightness % 4 + 1; break;
            case R_AIM: g_settings.aim_mode = (g_settings.aim_mode + 1) % 3; break;
            case R_SENS: g_settings.sensitivity = (g_settings.sensitivity + 1) % 3; break;
            case R_INVY: g_settings.invert_y ^= 1; break;
            case R_INVX: g_settings.invert_x ^= 1; break;
            case R_TILT:
                tilt_start();
                mode = SH_TILT;
                break;
            case R_CALIB:
                calib_return = SH_SETTINGS;
                mode = SH_CALIB;
                break;
            }
            if (i != R_CALIB && i != R_TILT) {
                gos_settings_save();
                gos_settings_apply();
            }
        }
    }
    gos_gfx_clear_clip();

    // slim scrollbar so it's obvious there's more below
    if (max_scroll > 0) {
        int track = SET_VIEW_Y1 - SET_VIEW_Y0 - 4;
        int th = track * view_h / content_h;
        if (th < 16) th = 16;
        int ty = SET_VIEW_Y0 + 2 + (track - th) * set_scroll / max_scroll;
        gos_gfx_vline(GOS_SCREEN_W - 3, SET_VIEW_Y0 + 2, SET_VIEW_Y1 - 2, UI_EMPTY);
        gos_gfx_vline(GOS_SCREEN_W - 3, ty, ty + th, UI_DIM);
    }

    gos_gfx_text(1, 8, 12, UI_TEXT, "SETTINGS");

    draw_bottom_bar("<");
    if (tap_in(0, GOS_SCREEN_H - 26, 60, 26)) mode = SH_GRID;
}

static void calib_frame(void)
{
    // can run mid-game (calib_return == SH_RUNNING), so GRAY64 ramp only:
    // games so far overwrite palette 0..15 and leave the ramp intact
    gos_gfx_clear(UI_BG);
    gos_gfx_text(1, 22, 60, UI_TEXT, "HOLD THE");
    gos_gfx_text(1, 22, 78, UI_TEXT, "DEVICE HOW");
    gos_gfx_text(1, 22, 96, UI_TEXT, "YOU PLAY");
    gos_gfx_text(0, 34, 140, GOS_GRAY64(0), "TAP TO SET CENTER");
    const gos_input_t *in = gos_input_get();
    gos_gfx_text(0, 40, 170, UI_DIM, "P%+6.1f R%+6.1f",
                 (double)in->pitch, (double)in->roll);
    if (tap) {
        gos_input_calibrate_neutral();
        if (cur) gos_input_save_neutral(cur->id);
        else gos_input_save_neutral("sys");
        if (!g_settings.calibrated) {
            g_settings.calibrated = 1;
            gos_settings_save();
        }
        if (calib_return == SH_RUNNING && cur) cur->resume(&ctx);
        mode = calib_return;
    }
}

// --- TILT SETUP -------------------------------------------------------------
// Teaches the OS how the accelerometer sits inside THIS unit. The part is not
// mounted the same way on every board of this model, so a fixed mapping aims
// left/right (or up/down) backwards on some of them and there is nothing the
// user can do about it from the aim settings.
//
// The gesture DEFINES the semantics. The wizard asks for "tilt to aim up" and
// whatever motion arrives becomes aim-up; it never dictates which way to turn
// the device, and no axis letter or sign ever reaches the screen. That is also
// why the arrows in the animation are double-headed: they name the axis to
// rock, never a direction.
//
// Detection runs in ANGLE space, and that is the whole trick. Held as played
// the screen is near vertical, so the screen-vertical channel already reads
// ~0.9 g: rocking further that way moves it about 0.06 g while rocking back
// moves it half a g. A raw-g threshold is therefore reachable in one direction
// only, which is exactly what the field reported (the wizard saw one direction
// and ignored the other). asin of that same channel does not saturate: those
// 0.06 g are a clean 10 degrees. The two axis angles are also exactly
// decoupled, since a rotation about one body axis leaves the other channel
// untouched.
//
// The recorded sign is minus the measured one, because each prompt names the
// gesture that must read NEGATIVE: aim up is negative pitch, aim left is
// negative roll, under the convention the fusion already publishes (+gy aims
// down, +gx aims right). The angles measured here use the same atan2 form as
// the HAL's map_angles(), so what we record is what the fusion will see.
#define TILT_DELTA_DEG 15.0f   // rotation from neutral that counts as intent
#define TILT_HOLD_MS   1000    // ...and how long it must be held there

enum { TS_NEUTRAL, TS_PITCH, TS_ROLL, TS_DONE };
static int ts_step;
static float ts_nx, ts_ny, ts_nz;      // raw gravity in the play pose
static float ts_nax, ts_nay;           // ...and its two axis angles, degrees
static uint8_t ts_pitch_axis;
static int8_t ts_pitch_sign, ts_roll_sign;
static int ts_cand_axis = -1;          // axis being held away from neutral
static int ts_cand_sign;
static uint32_t ts_cand_ms;            // when this candidate started
static uint32_t ts_msg_ms;             // TRY AGAIN flash / DONE dwell
// Step 3 only starts watching once the device is back near neutral. Without
// this the tail of step 2's own gesture, which the user has to unwind through,
// reads as "wrong axis" and lights a TRY AGAIN nobody can avoid.
static bool ts_armed;

static void tilt_start(void)
{
    ts_step = TS_NEUTRAL;
    ts_cand_axis = -1;
    ts_msg_ms = 0;
    ts_armed = true;
    ESP_LOGI("shell", "tilt setup: start");
}

// inclination of each accel axis against gravity, degrees
static void tilt_angles(float ax, float ay, float az, float *angx, float *angy)
{
    *angx = atan2f(ax, sqrtf(ay * ay + az * az)) * 57.29578f;
    *angy = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.29578f;
}

// dominant axis of the rotation away from neutral, if it is far enough to be
// intent rather than hand tremor. Direction-agnostic: either way triggers, and
// which way it went is the answer we are here to record.
static bool tilt_gesture(float dx, float dy, int *axis, int *sign)
{
    float mx = dx < 0 ? -dx : dx, my = dy < 0 ? -dy : dy;
    int a = my > mx ? 1 : 0;
    if ((a ? my : mx) < TILT_DELTA_DEG) return false;
    *axis = a;
    *sign = (a ? dy : dx) < 0 ? -1 : 1;
    return true;
}

// --- the rocking device drawing ---------------------------------------------
// One quad covers both axes: the roll step rotates it in plane, the pitch step
// tapers it (top edge nearer or further, the only cue a flat front view has
// for a rotation it cannot show). GRAY64 ramp and indexed primitives only, no
// assets. Wall clock drives the rock, never a frame count.
static void tilt_quad(gos_pt_t p[4], int cx, int cy, int hw, int hh,
                      int rot256, int taper)
{
    int c = gos_cos256((uint8_t)rot256), s = gos_sin256((uint8_t)rot256);
    static const int lx[4] = { -1, 1, 1, -1 }, ly[4] = { -1, -1, 1, 1 };
    for (int i = 0; i < 4; i++) {
        int x = lx[i] * hw * (16384 - taper * ly[i]) / 16384;   // Q14 width
        int y = ly[i] * hh;
        p[i].x = (int16_t)(cx + (x * c - y * s) / 16384);
        p[i].y = (int16_t)(cy + (x * s + y * c) / 16384);
    }
}

static void arrow_v(int x, int y0, int y1, gos_color_t c)
{
    gos_gfx_vline(x, y0, y1, c);
    for (int i = 1; i <= 4; i++) {
        gos_gfx_pixel(x - i, y0 + i, c);
        gos_gfx_pixel(x + i, y0 + i, c);
        gos_gfx_pixel(x - i, y1 - i, c);
        gos_gfx_pixel(x + i, y1 - i, c);
    }
}

static void arrow_h(int y, int x0, int x1, gos_color_t c)
{
    gos_gfx_hline(x0, x1, y, c);
    for (int i = 1; i <= 4; i++) {
        gos_gfx_pixel(x0 + i, y - i, c);
        gos_gfx_pixel(x0 + i, y + i, c);
        gos_gfx_pixel(x1 - i, y - i, c);
        gos_gfx_pixel(x1 - i, y + i, c);
    }
}

#define TILT_ART_CY 118

static void tilt_art(int step, uint32_t now)
{
    const int cx = GOS_SCREEN_W / 2, hw = 22, hh = 30;
    int osc = gos_sin256((uint8_t)((now / 9) & 0xFF));   // Q14, ~2.3 s a cycle
    int rot = 0, taper = 0;
    if (step == TS_PITCH) taper = osc * 5000 / 16384;    // top edge in and out
    else if (step == TS_ROLL) rot = osc * 12 / 16384;    // about +-17 degrees

    gos_pt_t body[4], scr[4];
    tilt_quad(body, cx, TILT_ART_CY, hw, hh, rot, taper);
    tilt_quad(scr, cx, TILT_ART_CY, hw - 5, hh - 7, rot, taper);
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        gos_gfx_line(body[i].x, body[i].y, body[j].x, body[j].y, UI_TEXT);
        gos_gfx_line(scr[i].x, scr[i].y, scr[j].x, scr[j].y, UI_DIM);
    }

    if (step == TS_PITCH)
        arrow_v(cx + 38, TILT_ART_CY - 22, TILT_ART_CY + 22, UI_TEXT);
    else if (step == TS_ROLL)
        arrow_h(TILT_ART_CY + 42, cx - 36, cx + 36, UI_TEXT);
}

static void tilt_commit(void)
{
    g_settings.tilt_pitch_axis = ts_pitch_axis;
    g_settings.tilt_pitch_neg = ts_pitch_sign < 0;
    g_settings.tilt_roll_neg = ts_roll_sign < 0;
    g_settings.calibrated = 1;
    gos_settings_save();
    gos_settings_apply();      // pushes the map into the running IMU task
    // The neutral was captured before the map existed, so convert that raw
    // vector rather than asking the user to hold the play pose a second time
    // (they are holding the device tilted at this exact moment).
    float p, r;
    hal_imu_angles_from_accel(ts_nx, ts_ny, ts_nz, &p, &r);
    gos_input_set_neutral(p, r);
    gos_input_save_neutral(cur ? cur->id : "sys");
    ESP_LOGI("shell", "tilt setup: saved, neutral P%+.1f R%+.1f",
             (double)p, (double)r);
}

static void tilt_frame(void)
{
    // GRAY64 ramp only, same rule as calib_frame
    gos_gfx_clear(UI_BG);

    float ax = 0, ay = 0, az = 0;
    bool imu = hal_imu_raw_accel(&ax, &ay, &az);
    uint32_t now = gos_time_ms();
    int progress = 0;      // hold bar, 0..100

    if (ts_step == TS_NEUTRAL) {
        // release-tap, and not in the top strip: that band belongs to the
        // cancel swipe
        if (imu && tap && pt_in(tap_pt, 0, 34, GOS_SCREEN_W, GOS_SCREEN_H - 34)) {
            ts_nx = ax; ts_ny = ay; ts_nz = az;
            tilt_angles(ax, ay, az, &ts_nax, &ts_nay);
            ts_step = TS_PITCH;
            ts_cand_axis = -1;
            ESP_LOGI("shell", "tilt setup: neutral %+.2f %+.2f %+.2f "
                     "(%+.1f %+.1f deg)", (double)ax, (double)ay, (double)az,
                     (double)ts_nax, (double)ts_nay);
        }
    } else if (ts_step != TS_DONE) {
        int axis, sign;
        float angx, angy;
        tilt_angles(ax, ay, az, &angx, &angy);
        bool moved = imu && tilt_gesture(angx - ts_nax, angy - ts_nay, &axis, &sign);
        if (!moved) {
            ts_cand_axis = -1;
            ts_armed = true;      // back near neutral, watch again
        } else if (ts_armed) {
            bool fresh = axis != ts_cand_axis || sign != ts_cand_sign;
            if (fresh) {
                ts_cand_axis = axis;
                ts_cand_sign = sign;
                ts_cand_ms = now;
            }
            if (ts_step == TS_ROLL && axis == ts_pitch_axis) {
                // the second gesture has to move the OTHER axis, or we learn
                // nothing about roll
                ts_msg_ms = now;
                if (fresh) ESP_LOGI("shell", "tilt setup: same axis as pitch, retry");
            } else if (now - ts_cand_ms >= TILT_HOLD_MS) {
                float d = axis ? angy - ts_nay : angx - ts_nax;
                if (ts_step == TS_PITCH) {
                    ts_pitch_axis = (uint8_t)axis;
                    ts_pitch_sign = (int8_t)-sign;
                    ESP_LOGI("shell", "tilt setup: aim up moved accel %c %+.0f deg,"
                             " pitch <- %c accel %c", axis ? 'Y' : 'X', (double)d,
                             ts_pitch_sign < 0 ? '-' : '+', axis ? 'Y' : 'X');
                    ts_step = TS_ROLL;
                    ts_cand_axis = -1;
                    ts_armed = false;   // let them unwind this one first
                } else {
                    ts_roll_sign = (int8_t)-sign;
                    ESP_LOGI("shell", "tilt setup: aim left moved accel %c %+.0f deg,"
                             " roll <- %c accel %c", axis ? 'Y' : 'X', (double)d,
                             ts_roll_sign < 0 ? '-' : '+', axis ? 'Y' : 'X');
                    tilt_commit();
                    ts_step = TS_DONE;
                    ts_msg_ms = now;
                }
            } else {
                progress = (int)((now - ts_cand_ms) * 100 / TILT_HOLD_MS);
            }
        } else {
            ts_cand_axis = -1;   // still unwinding step 2, nothing to read yet
        }
    } else if (now - ts_msg_ms > 900) {
        mode = SH_SETTINGS;     // only ever entered from settings
        return;
    }

    // The prompts name an outcome, not a motion: whatever the user does to
    // "aim up" becomes aim up on their unit.
    static const char *lines[4][3] = {
        { "HOLD THE", "DEVICE HOW", "YOU PLAY" },
        { "TILT TO", "AIM UP", NULL },
        { "TILT TO", "AIM LEFT", NULL },
        { "DONE", NULL, NULL },
    };
    if (ts_step != TS_DONE)
        gos_gfx_text(0, 8, 14, UI_DIM, "TILT SETUP  %d/3", ts_step + 1);
    int hy = ts_step == TS_DONE ? 100 : 28;   // nothing else on the DONE screen
    for (int i = 0; i < 3; i++) {
        const char *s = lines[ts_step][i];
        if (!s) break;
        int w = gos_gfx_text_w(1, s);
        gos_gfx_text(1, GOS_SCREEN_W / 2 - w / 2, hy + i * 18, UI_TEXT, "%s", s);
    }

    if (ts_step != TS_DONE) tilt_art(ts_step, now);

    if (ts_step == TS_NEUTRAL) {
        int w = gos_gfx_text_w(0, "TAP TO SET CENTER");
        gos_gfx_text(0, GOS_SCREEN_W / 2 - w / 2, 174, GOS_GRAY64(0),
                     "TAP TO SET CENTER");
    } else if (ts_step != TS_DONE) {
        const char *hint = now - ts_msg_ms < 900 ? "TRY AGAIN"
                                                 : "ANY WAY YOU LIKE, THEN HOLD";
        int w = gos_gfx_text_w(0, hint);
        gos_gfx_text(0, GOS_SCREEN_W / 2 - w / 2, 174, GOS_GRAY64(0), "%s", hint);
        // the hold bar makes "sustained" visible instead of mysterious
        gos_gfx_rect_fill((gos_rect_t){ 10, 188, 164, 4 }, UI_EMPTY);
        if (progress > 0)
            gos_gfx_rect_fill((gos_rect_t){ 10, 188,
                                            (int16_t)(164 * progress / 100), 4 },
                              GOS_GRAY64(0));
    }

    if (!imu) gos_gfx_text(0, 46, 196, UI_DIM, "NO MOTION SENSOR");
    if (ts_step != TS_DONE)
        gos_gfx_text(0, 8, GOS_SCREEN_H - 14, UI_DIM,
                     "SWIPE DOWN FROM TOP = EXIT");
}

static void overlay_frame(const gos_input_t *in)
{
    // frozen world behind the panel; a full-res-565 game's buffer can't be
    // composited under indexed drawing, so it gets a plain backdrop instead
    if (cur && (cur->caps & GOS_CAP_FB565)) gos_gfx_clear(GOS_GRAY64(6));
    else if (cur) cur->render(&ctx);

    // drawn over the game's palette: GRAY64 ramp only (see calib_frame note)
    // panel is 172 tall, vertically centered on the 224 screen (y 26..198 —
    // it used to hang low at 44..216, user-reported); the buttons and their
    // field-proven 44-tall overlapping zones shift with it unchanged
    bool lowbat = batt.present && batt.percent >= 0 && batt.percent <= 5;
    round_fill(20, 26, 144, 172, UI_BAR);
    round_border(20, 26, 144, 172, GOS_GRAY64(20));
    if (lowbat)
        gos_gfx_text(0, 42, 14, GOS_GRAY64(63), "BATTERY CRITICAL");

    static const char *rows[4] = { "RESUME", "RESTART", "CALIBRATE", "QUIT" };
    for (int i = 0; i < 4; i++) {
        int y = 34 + i * 40;   // big rounded buttons, generous spacing
        round_fill(28, y, 128, 32, UI_CARD);
        round_border(28, y, 128, 32, UI_DIM);
        int tw = gos_gfx_text_w(0, rows[i]);
        gos_gfx_text(0, 92 - tw / 2, y + 13, UI_TEXT, "%s", rows[i]);
        // zones overlap downward; top-down first-match resolves low presses
        // to the button the user was aiming at
        if (!tap_in(0, y - 4, GOS_SCREEN_W, 44)) continue;
        ESP_LOGI("shell", "overlay: %s", rows[i]);
        switch (i) {
        case 0:
            cur->resume(&ctx);
            mode = SH_RUNNING;
            break;
        case 1: {
            const gos_game_t *g = cur;
            quit_game();
            launch(g);
            break;
        }
        case 2:
            calib_return = SH_RUNNING;
            mode = SH_CALIB;
            break;
        case 3:
            quit_game();
            break;
        }
        return;   // zones overlap: first (topmost) match wins, once
    }
}

static void draw_debug_overlay(const gos_input_t *in)
{
    int ft_last = ft_ring[(ft_idx + 63) & 63];
    gos_gfx_text(0, 6, 18, GOS_YELLOW, "%2dms i%uk p%uk", ft_last,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    gos_gfx_text(0, 6, 28, GOS_YELLOW, "A%+.2f %+.2f P%+5.1f R%+5.1f",
                 (double)in->aim_x, (double)in->aim_y,
                 (double)in->pitch, (double)in->roll);
    for (int i = 0; i < 64; i++) {
        int h = ft_ring[(ft_idx + i) & 63];
        if (h > 33) h = 33;
        gos_gfx_vline(6 + i * 2, GOS_SCREEN_H - 6 - h, GOS_SCREEN_H - 6,
                      h > 16 ? GOS_RED : GOS_GREEN);
    }
}

// ---------------------------------------------------------------------------

void shell_init(void)
{
    gos_settings_load();
    gos_settings_apply();
    if (!g_settings.calibrated) {
        calib_return = SH_GRID;
        mode = SH_CALIB;
    }
    gos_input_load_neutral("sys");
}

void shell_frame(const gos_input_t *in_ro, int n_updates)
{
    frame_t0 = gos_time_us();
    gos_input_t in = *in_ro;   // local copy so caps masking can edit it

    // housekeeping at 1 Hz
    uint32_t now_ms = gos_time_ms();
    if (now_ms - last_poll_ms > 1000) {
        last_poll_ms = now_ms;
        hal_power_poll();
        hal_power_get(&batt);
        // one-shot: warn on the way down, re-arm only after real recovery
        static bool lowbat_warned;
        bool low = batt.present && batt.percent >= 1 && batt.percent <= 5 &&
                   !batt.charging;
        if (mode == SH_RUNNING && low && !lowbat_warned) {
            lowbat_warned = true;
            cur->suspend(&ctx);
            mode = SH_OVERLAY;
        }
        if (batt.percent > 10 || batt.charging) lowbat_warned = false;
    }

    detect_tap(&in);

    // universal escape: swipe down from the top edge pauses any game —
    // the ONLY pause gesture, since BOOT now belongs to games as button A.
    // Generous start zone: a real swipe-in from the bezel often first
    // registers well below the physical edge.
    if (!in.touching) swipe_armed = true;
    bool swipe_down = swipe_armed && in.touching && t_press.y < 30 &&
                      in.touch.y - t_press.y > 30;
    if (mode == SH_RUNNING && swipe_down) {
        ESP_LOGI("shell", "pause via swipe");
        cur->suspend(&ctx);
        mode = SH_OVERLAY;
    } else if (mode == SH_TILT && swipe_down) {
        // the setup wizard is a screen like any other: the universal escape
        // gets out of it, leaving the stored map untouched
        ESP_LOGI("shell", "tilt setup: exit via swipe");
        tap = false;   // this contact's release must not also hit a settings row
        press_consumed = true;
        mode = SH_SETTINGS;
    }

    // BOOT (game button A): games see raw press/release edges. The shell
    // keeps only two claims on it: a 600 ms hold toggles the debug overlay
    // (dev-only; the game sees the press-edge but the release is swallowed
    // so a hold never reads as a click), and a short press on the pause
    // overlay resumes (conflicts with nothing — the game isn't running).
    static bool a_swallow_release;
    if (in.pressed & GOS_BTN_A) menu_down_ms = now_ms;
    if ((in.buttons & GOS_BTN_A) && menu_down_ms &&
        now_ms - menu_down_ms > 600) {
        debug_overlay = !debug_overlay;
        menu_down_ms = 0;
        a_swallow_release = true;
    }
    if (in.released & GOS_BTN_A) {
        menu_down_ms = 0;
        if (a_swallow_release) {
            a_swallow_release = false;
            in.released &= ~GOS_BTN_A;
        } else if (mode == SH_OVERLAY) {
            ESP_LOGI("shell", "resume via button");
            cur->resume(&ctx);
            mode = SH_RUNNING;
        }
    }
    if (a_swallow_release) in.buttons &= ~GOS_BTN_A;

    // full-res direct mode is live exactly while a FB565 game is RUNNING;
    // everywhere else (launcher, overlay, calib) presents the indexed fb
    gos_gfx_direct565(cur && mode == SH_RUNNING &&
                      (cur->caps & GOS_CAP_FB565));

    switch (mode) {
    case SH_GRID: grid_frame(); break;
    case SH_SETTINGS: settings_frame(); break;
    case SH_CALIB: calib_frame(); break;
    case SH_TILT: tilt_frame(); break;
    case SH_OVERLAY: overlay_frame(&in); break;
    case SH_RUNNING: {
        if (!(cur->caps & GOS_CAP_TOUCH_FIRE)) {
            in.buttons &= ~GOS_BTN_FIRE;
            in.pressed &= ~GOS_BTN_FIRE;
            in.released &= ~GOS_BTN_FIRE;
        }
        for (int u = 0; u < n_updates; u++)
            cur->update(&ctx, 1.0f / 60.0f, &in);
        if (gos_core_take_exit()) {
            quit_game();
            grid_frame();
            break;
        }
        cur->render(&ctx);
        break;
    }
    }

    if (debug_overlay) draw_debug_overlay(&in);

    int ft = (int)((gos_time_us() - frame_t0) / 1000);
    ft_ring[ft_idx] = (uint8_t)(ft > 255 ? 255 : ft);
    ft_idx = (ft_idx + 1) & 63;

    gos_gfx_present();
}
