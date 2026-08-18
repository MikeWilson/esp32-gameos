// apps.c — the two built-in harness apps, written against the game contract
// like any other title.
//
//   AIM TEST — OS milestone 3, the risk gate: acquire a 12 px target, hold
//   500 ms, 30 trials, median time-to-acquire readout, live tuning sliders.
//
//   DIAG — OS milestone 2: every HAL subsystem readable from one screen.
#include "gos.h"
#include "gos_core.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "gos_hal.h"

// ---------------------------------------------------------------------------
// AIM TEST
// ---------------------------------------------------------------------------

#define TRIALS 30

typedef struct {
    int16_t tx, ty;          // target center
    uint32_t target_ms;      // when this target appeared
    uint32_t dwell_ms;       // accumulated in-target time
    uint32_t last_ms;
    int trial;
    uint16_t times[TRIALS];  // ms to acquire
    bool done;
    int drag_slider;         // -1 = none
} aimtest_t;

static const gos_note_t n_hit[] = { { 880, 1320, 60, GOS_W_SQ50, 160 } };
static const gos_sfx_t sfx_hit = { n_hit, 1, 1 };

static void at_new_target(aimtest_t *s, gos_rng_t *rng)
{
    s->tx = (int16_t)gos_rand_range(rng, 20, GOS_SCREEN_W - 20);
    s->ty = (int16_t)gos_rand_range(rng, 30, GOS_SCREEN_H - 60);
    s->target_ms = gos_time_ms();
    s->dwell_ms = 0;
}

static void at_init(gos_ctx_t *ctx)
{
    aimtest_t *s = ctx->state;
    s->drag_slider = -1;
    at_new_target(s, &ctx->rng);
}

typedef struct { const char *name; float *val; float lo, hi; } slider_t;

static slider_t at_sliders(int i)
{
    gos_aim_cfg_t *c = gos_input_aim_config();
    switch (i) {
    case 0: return (slider_t){ "CUT", &c->min_cutoff, 0.1f, 3.f };
    case 1: return (slider_t){ "BETA", &c->beta, 0.f, 0.03f };
    case 2: return (slider_t){ "RNG", &c->range_deg, 10.f, 45.f };
    default: return (slider_t){ "CRV", &c->curve_pow, 1.f, 2.2f };
    }
}

static void at_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    aimtest_t *s = ctx->state;
    uint32_t now = gos_time_ms();

    // sliders live in the bottom strip; dragging one adjusts it
    if (in->touching && in->touch.y > GOS_SCREEN_H - 46) {
        int i = (in->touch.x - 6) / 44;
        if (i < 0) i = 0;
        if (i > 3) i = 3;
        if (s->drag_slider < 0) s->drag_slider = i;
        slider_t sl = at_sliders(s->drag_slider);
        int x0 = s->drag_slider * 44 + 6;
        float f = (in->touch.x - x0) / 36.f;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        *sl.val = sl.lo + f * (sl.hi - sl.lo);
    } else {
        s->drag_slider = -1;
    }

    if (s->done) {
        // restart on a touch that isn't a slider grab
        if ((in->pressed & GOS_BTN_FIRE) && in->touch.y < GOS_SCREEN_H - 50) {
            s->trial = 0;
            s->done = false;
            at_new_target(s, &ctx->rng);
        }
        return;
    }

    int cx = GOS_SCREEN_W / 2 + (int)(in->aim_x * (GOS_SCREEN_W / 2 - 10));
    int cy = GOS_SCREEN_H / 2 + (int)(in->aim_y * (GOS_SCREEN_H / 2 - 10));
    int dx = cx - s->tx, dy = cy - s->ty;
    if (dx * dx + dy * dy <= 8 * 8) {
        s->dwell_ms += (uint32_t)(dt * 1000);
        if (s->dwell_ms >= 500) {
            uint32_t t = now - s->target_ms - 500;
            s->times[s->trial++] = (uint16_t)(t > 60000 ? 60000 : t);
            gos_audio_play(&sfx_hit, 200);
            if (s->trial >= TRIALS) s->done = true;
            else at_new_target(s, &ctx->rng);
        }
    } else {
        s->dwell_ms = 0;
    }
    s->last_ms = now;
}

static uint16_t median(uint16_t *v, int n)
{
    uint16_t tmp[TRIALS];
    memcpy(tmp, v, n * sizeof *v);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && tmp[j - 1] > tmp[j]; j--) {
            uint16_t t = tmp[j];
            tmp[j] = tmp[j - 1];
            tmp[j - 1] = t;
        }
    return tmp[n / 2];
}

static void at_render(gos_ctx_t *ctx)
{
    aimtest_t *s = ctx->state;
    const gos_input_t *in = gos_input_get();
    gos_gfx_clear(GOS_BLACK);

    if (s->done) {
        gos_gfx_text(1, 20, 60, GOS_WHITE, "MEDIAN");
        gos_gfx_text(1, 20, 80, GOS_CYAN, "%d MS", median(s->times, TRIALS));
        gos_gfx_text(0, 20, 110, GOS_GRAY, "PASS < 1200 MS");
        gos_gfx_text(0, 20, 126, GOS_LTGRAY, "TAP TO RETRY");
    } else {
        // target
        gos_gfx_circle(s->tx, s->ty, 6, s->dwell_ms ? GOS_GREEN : GOS_RED);
        gos_gfx_pixel(s->tx, s->ty, GOS_WHITE);
        // crosshair
        int cx = GOS_SCREEN_W / 2 + (int)(in->aim_x * (GOS_SCREEN_W / 2 - 10));
        int cy = GOS_SCREEN_H / 2 + (int)(in->aim_y * (GOS_SCREEN_H / 2 - 10));
        gos_gfx_line(cx - 6, cy, cx + 6, cy, GOS_WHITE);
        gos_gfx_line(cx, cy - 6, cx, cy + 6, GOS_WHITE);
        gos_gfx_text(0, 8, 7, GOS_GRAY, "TRIAL %d/%d", s->trial + 1, TRIALS);
        if (s->trial > 0)
            gos_gfx_text(0, 100, 7, GOS_GRAY, "MED %dMS",
                         median(s->times, s->trial));
        if (s->dwell_ms)
            gos_gfx_rect_fill((gos_rect_t){ 8, 17, (int16_t)(s->dwell_ms * 60 / 500), 3 },
                              GOS_GREEN);
    }

    // sliders
    for (int i = 0; i < 4; i++) {
        slider_t sl = at_sliders(i);
        int x0 = i * 44 + 6;
        int y = GOS_SCREEN_H - 34;
        float f = (*sl.val - sl.lo) / (sl.hi - sl.lo);
        gos_gfx_text(0, x0, y - 10, GOS_GRAY, "%s", sl.name);
        gos_gfx_hline(x0, x0 + 36, y + 4, GOS_DARKGRAY);
        gos_gfx_rect_fill((gos_rect_t){ (int16_t)(x0 + (int)(f * 34)), (int16_t)y, 4, 9 },
                          GOS_CYAN);
        gos_gfx_text(0, x0, y + 12, GOS_GRAY, "%.2f", (double)*sl.val);
    }
}

static void at_noop(gos_ctx_t *ctx) { (void)ctx; }

const gos_game_t game_aimtest = {
    .id = "aimtest",
    .title = "AIM TEST",
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_TOUCH_FIRE,
    .state_size = sizeof(aimtest_t),
    .init = at_init,
    .update = at_update,
    .render = at_render,
    .suspend = at_noop,
    .resume = at_noop,
    .teardown = at_noop,
};

// ---------------------------------------------------------------------------
// DIAG
// ---------------------------------------------------------------------------

typedef struct { uint32_t frames; int grid_n; } diag_t;

static const gos_note_t n_beep[] = {
    { 660, 660, 80, GOS_W_TRI, 200 }, { 990, 990, 120, GOS_W_TRI, 200 },
};
static const gos_sfx_t sfx_beep = { n_beep, 2, 1 };

static void dg_init(gos_ctx_t *ctx)
{
    gos_grid_init(16, 1200, 1200);
}

static void dg_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    diag_t *s = ctx->state;
    s->frames++;
    if (in->pressed & GOS_BTN_FIRE) gos_audio_play(&sfx_beep, 220);

    // grid smoke test each frame: insert a scatter, query the middle
    gos_grid_clear();
    gos_rng_t r = { 42 };
    for (int i = 0; i < 100; i++)
        gos_grid_insert((uint16_t)i, (int16_t)gos_rand_range(&r, 0, 1199),
                        (int16_t)gos_rand_range(&r, 0, 1199));
    uint16_t out[32];
    s->grid_n = gos_grid_query(600, 600, 200, out, 32);
}

static void dg_render(gos_ctx_t *ctx)
{
    diag_t *s = ctx->state;
    const gos_input_t *in = gos_input_get();
    hal_batt_t b;
    hal_power_get(&b);

    gos_gfx_clear(GOS_BLACK);
    gos_gfx_text(1, 8, 7, GOS_WHITE, "DIAG");
    int y = 28;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "PITCH %+7.2f ROLL %+7.2f", (double)in->pitch, (double)in->roll); y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "GYRO %+6.1f %+6.1f %+6.1f", (double)in->gx, (double)in->gy, (double)in->gz); y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "AIM %+5.2f %+5.2f SHK %4.2f", (double)in->aim_x, (double)in->aim_y, (double)in->shake); y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "TOUCH %3d %3d %s", in->touch.x, in->touch.y, in->touching ? "DOWN" : "UP"); y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "BTN %02X", (unsigned)in->buttons); y += 12;
    if (b.present)
        gos_gfx_text(0, 8, y, GOS_LTGRAY, "BATT %d%% %s", b.percent, b.charging ? "CHG" : "");
    else
        gos_gfx_text(0, 8, y, GOS_LTGRAY, "BATT: NONE (USB)");
    y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "HEAP INT %uK PSRAM %uK",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)); y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "GRID Q %d/100", s->grid_n); y += 12;
    gos_gfx_text(0, 8, y, GOS_LTGRAY, "UPTIME %lus FRAME %lu",
                 (unsigned long)(gos_time_ms() / 1000), (unsigned long)s->frames); y += 16;
    gos_gfx_text(0, 8, y, GOS_CYAN, "TAP LOWER 2/3 = BEEP");

    // touch crosshair + palette strip
    if (in->touching) {
        gos_gfx_line(in->touch.x - 5, in->touch.y, in->touch.x + 5, in->touch.y, GOS_YELLOW);
        gos_gfx_line(in->touch.x, in->touch.y - 5, in->touch.x, in->touch.y + 5, GOS_YELLOW);
    }
    for (int i = 0; i < 80; i++) {
        gos_gfx_vline(8 + i * 2, GOS_SCREEN_H - 30, GOS_SCREEN_H - 18, (gos_color_t)i);
        gos_gfx_vline(9 + i * 2, GOS_SCREEN_H - 30, GOS_SCREEN_H - 18, (gos_color_t)i);
    }
}

static void dg_noop(gos_ctx_t *ctx) { (void)ctx; }

const gos_game_t game_diag = {
    .id = "diag",
    .title = "DIAG",
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_TOUCH_FIRE,
    .state_size = sizeof(diag_t),
    .init = dg_init,
    .update = dg_update,
    .render = dg_render,
    .suspend = dg_noop,
    .resume = dg_noop,
    .teardown = dg_noop,
};
