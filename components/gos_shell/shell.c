// shell.c — launcher grid, settings, pause overlay, calibration wizard,
// debug overlay, and the per-frame orchestration of the running game.
#include "gos.h"
#include "gos_core.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "gos_hal.h"

typedef enum { SH_GRID, SH_SETTINGS, SH_RUNNING, SH_OVERLAY, SH_CALIB } sh_mode_t;

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

static void settings_frame(void)
{
    static const char *aim_names[] = { "TILT", "GYRO", "TOUCH" };
    static const char *sens_names[] = { "LOW", "MED", "HIGH" };
    static const char *vol_names[] = { "OFF", "LOW", "MED", "HIGH" };

    // list content: interactive rows + one section header. System settings
    // first; the shared tilt-aim service sits under its own header (it read
    // as gunship leftovers otherwise — user-reported)
    enum { R_VOLUME, R_BRIGHT, R_HDR, R_AIM, R_SENS, R_INVY, R_INVX,
           R_CALIB, R_RESET, R_COUNT };
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
            case R_CALIB:
                calib_return = SH_SETTINGS;
                mode = SH_CALIB;
                break;
            }
            if (i != R_CALIB) {
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
    if (mode == SH_RUNNING && swipe_armed && in.touching && t_press.y < 30 &&
        in.touch.y - t_press.y > 30) {
        ESP_LOGI("shell", "pause via swipe");
        cur->suspend(&ctx);
        mode = SH_OVERLAY;
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
