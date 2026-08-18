// slots.c — LUCKY 7, a one-armed bandit for gameos.
//
// The "pull" is a physical yank of the device: a detector over the raw 200 Hz
// accel ring integrates the stroke impulse (hand speed), so a lazy tug spins
// short and a real yank spins hard. Drag-down on the glass is the touch
// fallback, and a side lever animates either way.
//
// Full-bleed machine face: header / three 3-symbol reel windows / control
// deck, edge to edge. Reel motion is mechanical, not eased: backward
// anticipation, constant-velocity scroll with double-exposure motion blur,
// then a detent-stepped crawl into a sprung stop. Indexed pipeline with a
// custom palette in 80..159 (16..79 gray ramp untouched — shell overlay
// safe); cabinet ramps are rewritten live for the payout spotlight dim and
// the jackpot gold strobe. Cash/bet/stats persist in the save blob.

#include "gos.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// --- layout: full-bleed machine face ----------------------------------------
#define REELS   3
#define STRIP   20
#define REEL_Y  44           // glass top
#define REEL_H  102          // exactly 3 symbol rows
#define REEL_W  52
#define PITCH   34
#define PAYLINE 95           // center row
static const int reel_x[REELS] = { 4, 58, 112 };
// header y0..37, reel unit y38..152 (lever channel x166..183), deck y152..206,
// hint line y210 (H-14 per the safe-zone convention)

#define TOUCH_Y_BIAS 12      // measured parallax compensation

enum { S_CHERRY, S_LEMON, S_ORANGE, S_BAR, S_BELL, S_DIAMOND, S_SEVEN, SYM_N };
static const uint8_t  sym_count[SYM_N] = { 4, 3, 4, 3, 3, 2, 1 };   // = STRIP
static const uint16_t pay3[SYM_N]      = { 8, 4, 5, 10, 15, 25, 100 };

static const int bets[] = { 1, 2, 5, 10, 25 };
#define NBETS      5
#define START_CASH 200
#define LOAN       200

enum { ST_IDLE, ST_SPIN, ST_PAYOUT, ST_BROKE };
enum { PD_WAIT, PD_STROKE };

// --- palette (80..159; 0..15 defaults kept, 16..79 reserved for the shell) --
#define P_GOLD  80    // trim, marquee, win effects
#define P_WOOD  88    // (kept for the paytable card)
#define P_FELT  96    // unused backdrop ramp (spare)
#define P_CHR   104   // machine face, bezels, buttons, lever
#define P_LAMP  112   // spare glow ramp
#define P_RED   120   // seven, cherries, red LED digits
#define P_ICE   128   // diamond
#define P_AMB   136   // bell, lemon, orange, gold LED digits, coins
#define P_GRN   144   // stems, leaves
#define P_CREAM 152   // reel paper

typedef struct { uint8_t r, g, b; } rgb_t;

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static void ramp8(int base, rgb_t a, rgb_t b, rgb_t c, float scale, float gold)
{
    uint16_t buf[8];
    for (int i = 0; i < 8; i++) {
        float t = i / 7.f, r, g, bl;
        if (t < 0.5f) {
            float u = t * 2;
            r = a.r + (b.r - a.r) * u; g = a.g + (b.g - a.g) * u; bl = a.b + (b.b - a.b) * u;
        } else {
            float u = (t - 0.5f) * 2;
            r = b.r + (c.r - b.r) * u; g = b.g + (c.g - b.g) * u; bl = b.b + (c.b - b.b) * u;
        }
        if (gold > 0) { r += (255 - r) * gold; g += (230 - g) * gold; bl += (120 - bl) * gold; }
        r *= scale; g *= scale; bl *= scale;
        buf[i] = RGB565((int)r, (int)g, (int)bl);
    }
    gos_gfx_set_palette(buf, 8, base);
}

// cabinet ramps rewritten live: 0 = normal, 1 = payout spotlight dim,
// 2 = jackpot gold flash. Symbol/LED/coin/paper ramps stay full-bright.
static void set_cabinet_ramps(int mode)
{
    float sc = mode == 1 ? 0.45f : 1.f, gd = mode == 2 ? 0.55f : 0.f;
    ramp8(P_GOLD, (rgb_t){60,40,8},   (rgb_t){200,150,30}, (rgb_t){255,240,150}, sc, gd);
    ramp8(P_WOOD, (rgb_t){40,24,12},  (rgb_t){96,60,30},   (rgb_t){150,100,60},  sc, gd);
    ramp8(P_CHR,  (rgb_t){22,25,32},  (rgb_t){95,104,118}, (rgb_t){235,240,250}, sc, gd);
    ramp8(P_GRN,  (rgb_t){10,48,20},  (rgb_t){60,150,70},  (rgb_t){140,230,140}, sc, gd);
}

static void set_fixed_ramps(void)
{
    ramp8(P_FELT,  (rgb_t){4,22,12},    (rgb_t){20,64,32},    (rgb_t){44,120,64},   1, 0);
    ramp8(P_LAMP,  (rgb_t){70,10,4},    (rgb_t){255,120,20},  (rgb_t){255,255,200}, 1, 0);
    ramp8(P_RED,   (rgb_t){70,10,10},   (rgb_t){210,40,35},   (rgb_t){255,130,110}, 1, 0);
    ramp8(P_ICE,   (rgb_t){16,60,92},   (rgb_t){80,180,220},  (rgb_t){230,252,255}, 1, 0);
    ramp8(P_AMB,   (rgb_t){90,55,12},   (rgb_t){225,165,45},  (rgb_t){255,235,140}, 1, 0);
    ramp8(P_CREAM, (rgb_t){150,140,120},(rgb_t){215,205,185}, (rgb_t){255,250,238}, 1, 0);
}

// --- sfx (recipes per docs/audio.md) ----------------------------------------
#define SFX(name, prio_, ...) \
    static const gos_note_t name##_n[] = { __VA_ARGS__ }; \
    static const gos_sfx_t name = { name##_n, \
        (uint8_t)(sizeof(name##_n) / sizeof(gos_note_t)), prio_ };

SFX(sfx_tap,  1, {880, 880, 18, GOS_W_SQ25, 110})
SFX(sfx_bet,  1, {520, 700, 22, GOS_W_SQ25, 110})
SFX(sfx_pull, 3, {180, 70, 55, GOS_W_NOISE, 150}, {130, 85, 80, GOS_W_SQ50, 140})
SFX(sfx_ratchet, 2, {900, 900, 10, GOS_W_NOISE, 90}, {0, 0, 22, GOS_W_REST, 0},
                    {900, 900, 10, GOS_W_NOISE, 85}, {0, 0, 20, GOS_W_REST, 0},
                    {900, 900, 10, GOS_W_NOISE, 80}, {0, 0, 18, GOS_W_REST, 0},
                    {900, 900, 10, GOS_W_NOISE, 75})
SFX(sfx_tick, 0, {2300, 2300, 9, GOS_W_TRI, 55})
SFX(sfx_stop, 2, {150, 105, 45, GOS_W_SQ50, 150}, {85, 85, 45, GOS_W_TRI, 110})
SFX(sfx_thump, 2, {70, 52, 70, GOS_W_SQ50, 130})
SFX(sfx_coin, 2, {1500, 2100, 24, GOS_W_SQ25, 100})
SFX(sfx_win1, 4, {523, 523, 70, GOS_W_TRI, 130}, {659, 659, 70, GOS_W_TRI, 130},
                 {784, 784, 70, GOS_W_TRI, 130}, {1046, 1046, 130, GOS_W_TRI, 140})
SFX(sfx_win2, 5, {523, 523, 80, GOS_W_SQ25, 140}, {659, 659, 80, GOS_W_SQ25, 140},
                 {784, 784, 80, GOS_W_SQ25, 140}, {1046, 1046, 80, GOS_W_SQ25, 150},
                 {784, 784, 60, GOS_W_SQ25, 130}, {1046, 1318, 200, GOS_W_TRI, 150})
SFX(sfx_jack, 6, {392, 392, 110, GOS_W_SQ50, 150}, {392, 392, 110, GOS_W_SQ50, 150},
                 {523, 523, 110, GOS_W_SQ50, 150}, {659, 659, 110, GOS_W_SQ50, 150},
                 {784, 784, 200, GOS_W_TRI, 160}, {659, 659, 90, GOS_W_TRI, 140},
                 {784, 1046, 320, GOS_W_TRI, 160})
SFX(sfx_broke, 4, {300, 150, 200, GOS_W_SAW, 120}, {150, 90, 280, GOS_W_SAW, 110})
SFX(sfx_loan,  3, {200, 400, 80, GOS_W_TRI, 120}, {400, 620, 140, GOS_W_TRI, 120})

// --- state ------------------------------------------------------------------
typedef struct {
    uint8_t  ver, bet_ix;
    uint16_t loans;
    int32_t  cash, best_win;
    uint32_t spins;
} slots_save_t;

#define NPART 56
typedef struct { float x, y, vx, vy; int16_t life; } part_t;

typedef struct {
    int32_t  cash, best_win, win, cur_bet;
    uint32_t spins;
    uint16_t loans;
    uint8_t  bet_ix, st;

    uint8_t  strip[REELS][STRIP];
    int      stop[REELS], last_sym[REELS];
    float    pos[REELS], pos0[REELS], travel[REELS], dur[REELS], crawl[REELS],
             vel[REELS];
    bool     done[REELS];
    int64_t  t0, land_us[REELS], last_tick_us;

    int64_t  t_pay0, resolve_us;
    int32_t  paid, coin_step, last_coin;
    uint8_t  match_mask;
    bool     jack, tease;
    int64_t  thump_us;

    // presentation
    int      pal_mode;
    int64_t  lever_us;
    float    lever_depth, lever_live;
    bool     pays_open;
    part_t   part[NPART];

    // pull detector (integrated impulse over the accel ring)
    uint8_t  pd_phase;
    int      pd_warm, burst;
    float    ge[3], imp, quiet;
    int64_t  pd_t0, pd_prev_us;

    // touch (release-tap vs drag disambiguation)
    bool     was_touch;
    int16_t  ox, oy, lx, ly;
} slots_t;

static void save_now(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    slots_save_t s = { 1, g->bet_ix, g->loans, g->cash, g->best_win, g->spins };
    gos_save_write(ctx, &s, sizeof s);
}

// clamp the bet down to something affordable (never below the minimum)
static void clamp_bet(slots_t *g)
{
    while (g->bet_ix > 0 && bets[g->bet_ix] > g->cash) g->bet_ix--;
}

// --- spin -------------------------------------------------------------------
static int32_t eval_line(const slots_t *g)
{
    int a = g->strip[0][g->stop[0]], b = g->strip[1][g->stop[1]],
        c = g->strip[2][g->stop[2]];
    int ch = (a == S_CHERRY) + (b == S_CHERRY) + (c == S_CHERRY);
    if (a == b && b == c) return (int32_t)pay3[a] * g->cur_bet;
    if (ch == 2) return 3 * g->cur_bet;
    if (ch == 1) return g->cur_bet;
    return 0;
}

// mechanical motion profile (per reel, wall-clock):
//   PRE: backward anticipation pull, then
//   main: 0.2 s accel ramp into constant Vmax scroll, then
//   crawl: last 1.6 symbols in detent steps (dwell..SNAP per symbol), then
//   land: sprung overshoot wobble.
#define SPIN_PRE  0.12f
#define PRE_BACK  0.6f
#define CRAWL_SYM 1.6f
#define CRAWL_T   0.45f

static void start_spin(gos_ctx_t *ctx, float strength)
{
    slots_t *g = ctx->state;
    clamp_bet(g);
    g->cur_bet = bets[g->bet_ix];
    g->cash -= g->cur_bet;
    g->spins++;
    g->t0 = gos_time_us();
    float vmax = 20.f + 10.f * strength;              // sym/s
    for (int r = 0; r < REELS; r++) {
        g->stop[r] = gos_rand_range(&ctx->rng, 0, STRIP - 1);
        float p = fmodf(g->pos[r], STRIP);
        if (p < 0) p += STRIP;
        g->pos[r] = g->pos0[r] = p;
        int loops = 1 + r + (strength > 0.7f ? 1 : 0);
        int delta = ((g->stop[r] - (int)floorf(p)) % STRIP + STRIP) % STRIP;
        g->travel[r] = loops * STRIP + delta - (p - floorf(p));
        g->crawl[r] = CRAWL_T;
        g->dur[r] = (g->travel[r] + PRE_BACK - CRAWL_SYM) / vmax + 0.1f + g->crawl[r];
        g->done[r] = false;
        g->last_sym[r] = (int)floorf(p);
        g->land_us[r] = 0;
        g->vel[r] = 0;
    }
    // near-miss tease: first two reels will match -> the third's crawl turns
    // into an agonizing detent-by-detent creep. Timing only; the stop indices
    // above are already final, odds untouched.
    g->tease = g->strip[0][g->stop[0]] == g->strip[1][g->stop[1]];
    if (g->tease) {
        g->dur[2] += 1.45f;
        g->crawl[2] = CRAWL_T + 1.45f;
    }
    g->win = 0;
    g->match_mask = 0;
    g->st = ST_SPIN;
    g->lever_us = g->t0;
    g->lever_depth = 0.7f + 0.3f * strength;
    for (int i = 0; i < NPART; i++) g->part[i].life = 0;
    gos_audio_play(&sfx_ratchet, 180);
    gos_audio_play(&sfx_pull, 210);
}

static void resolve(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    g->win = eval_line(g);
    g->resolve_us = gos_time_us();
    if (g->win > 0) {
        int a = g->strip[0][g->stop[0]], b = g->strip[1][g->stop[1]],
            c = g->strip[2][g->stop[2]];
        if (a == b && b == c) g->match_mask = 7;
        else g->match_mask = (uint8_t)((a == S_CHERRY) | ((b == S_CHERRY) << 1) |
                                       ((c == S_CHERRY) << 2));
        g->cash += g->win;
        if (g->win > g->best_win) g->best_win = g->win;
        g->jack = g->win >= g->cur_bet * 50;
        g->st = ST_PAYOUT;
        g->t_pay0 = g->resolve_us;
        g->paid = 0;
        g->last_coin = -1;
        g->coin_step = g->win / 12 > 0 ? g->win / 12 : 1;
        gos_audio_play(g->jack ? &sfx_jack :
                       g->win >= g->cur_bet * 10 ? &sfx_win2 : &sfx_win1, 230);
    } else if (g->cash <= 0) {
        g->st = ST_BROKE;
        gos_audio_play(&sfx_broke, 200);
    } else {
        g->st = ST_IDLE;
    }
    clamp_bet(g);
    save_now(ctx);
}

// --- pull detector ----------------------------------------------------------
// Drains the ring every update (single consumer). Integrates |linear accel|
// over the stroke; fires on quiet/timeout/cap. See docs/design-playbook.md
// ("Sensors as game mechanics") — integrated impulse, never a sampled peak.
static void pull_detector(gos_ctx_t *ctx, bool listen)
{
    slots_t *g = ctx->state;
    gos_accel_sample_t s[64];
    int n = gos_imu_accel_read(s, 64);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        float dt = 0.005f;
        if (g->pd_prev_us) {
            dt = (s[i].t_us - g->pd_prev_us) * 1e-6f;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.02f)  dt = 0.02f;
        }
        g->pd_prev_us = s[i].t_us;

        float al = g->pd_warm < 40 ? 0.2f : (g->pd_phase == PD_STROKE ? 0.f : 0.02f);
        if (g->pd_warm < 40) g->pd_warm++;
        g->ge[0] += al * (s[i].ax - g->ge[0]);
        g->ge[1] += al * (s[i].ay - g->ge[1]);
        g->ge[2] += al * (s[i].az - g->ge[2]);
        float mx = s[i].ax - g->ge[0], my = s[i].ay - g->ge[1],
              mz = s[i].az - g->ge[2];
        float mag = sqrtf(mx * mx + my * my + mz * mz);

        if (g->pd_phase == PD_WAIT) {
            if (!listen || g->pd_warm < 40) { g->burst = 0; continue; }
            if (mag > 0.35f) {
                if (++g->burst >= 2) {
                    g->pd_phase = PD_STROKE;
                    g->imp = 0;
                    g->quiet = 0;
                    g->pd_t0 = s[i].t_us;
                }
            } else g->burst = 0;
        } else { // PD_STROKE
            g->imp += mag * dt;
            g->quiet = mag < 0.12f ? g->quiet + dt : 0.f;
            if (g->quiet > 0.08f || s[i].t_us - g->pd_t0 > 600000 || g->imp > 0.6f) {
                g->pd_phase = PD_WAIT;
                g->burst = 0;
                if (listen && g->imp >= 0.05f) {
                    float str = (g->imp - 0.05f) / 0.30f;
                    start_spin(ctx, str < 0 ? 0 : str > 1 ? 1 : str);
                    listen = false;   // one pull per drain
                }
            }
        }
    }
}

// --- touch ------------------------------------------------------------------
static void do_loan(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    g->cash += LOAN;
    g->loans++;
    g->st = ST_IDLE;
    gos_audio_play(&sfx_loan, 210);
    save_now(ctx);
}

static void tap(gos_ctx_t *ctx, int x, int y)
{
    slots_t *g = ctx->state;
    // no EXIT chip — quitting is the OS-level escape (swipe down from top)
    int py = y - TOUCH_Y_BIAS;
    if (g->pays_open) {                           // any tap closes the paytable
        g->pays_open = false;
        gos_audio_play(&sfx_tap, 180);
        return;
    }
    if (g->st == ST_IDLE && py <= 34) {           // header/marquee -> paytable
        g->pays_open = true;
        gos_audio_play(&sfx_tap, 180);
        return;
    }
    if (py < 146) return;                         // control deck (zones to edge)
    if (g->st == ST_BROKE) {
        if (x >= 50 && x <= 134) do_loan(ctx);
        return;
    }
    if (g->st != ST_IDLE) return;
    if (x <= 52 && g->bet_ix > 0) {
        g->bet_ix--;
        gos_audio_play(&sfx_bet, 190);
    } else if (x >= 132 && g->bet_ix < NBETS - 1 &&
               bets[g->bet_ix + 1] <= g->cash) {
        g->bet_ix++;
        gos_audio_play(&sfx_bet, 190);
    }
}

// --- lifecycle --------------------------------------------------------------
static void slots_init(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    g->cash = START_CASH;
    g->bet_ix = 2;
    slots_save_t s;
    if (gos_save_read(ctx, &s, sizeof s) == (int)sizeof s && s.ver == 1) {
        g->cash = s.cash;
        g->bet_ix = s.bet_ix < NBETS ? s.bet_ix : 2;
        g->loans = s.loans;
        g->best_win = s.best_win;
        g->spins = s.spins;
    }
    // one weighted strip per reel, shuffled so neighbors differ per reel
    for (int r = 0; r < REELS; r++) {
        int k = 0;
        for (int sym = 0; sym < SYM_N; sym++)
            for (int c = 0; c < sym_count[sym]; c++) g->strip[r][k++] = sym;
        for (int i = STRIP - 1; i > 0; i--) {
            int j = gos_rand_range(&ctx->rng, 0, i);
            uint8_t t = g->strip[r][i];
            g->strip[r][i] = g->strip[r][j];
            g->strip[r][j] = t;
        }
        g->pos[r] = g->stop[r] = gos_rand_range(&ctx->rng, 0, STRIP - 1);
    }
    g->st = g->cash > 0 ? ST_IDLE : ST_BROKE;
    g->ge[2] = 1.f;   // sane gravity guess until the EMA warms up
    set_fixed_ramps();
    set_cabinet_ramps(0);
    g->pal_mode = 0;
}

static void slots_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    (void)dt;
    slots_t *g = ctx->state;
    int64_t now = gos_time_us();

    pull_detector(ctx, g->st == ST_IDLE && !g->pays_open && g->cash > 0 &&
                       !in->touching);

    // touch: release-tap = button, drag down = lever pull
    if (in->touching) {
        if (!g->was_touch) { g->ox = g->lx = in->touch.x; g->oy = g->ly = in->touch.y; }
        else { g->lx = in->touch.x; g->ly = in->touch.y; }
    } else if (g->was_touch) {
        int dx = g->lx - g->ox, dy = g->ly - g->oy;
        if (abs(dx) < 10 && abs(dy) < 10)
            tap(ctx, g->ox, g->oy);
        else if (dy > 34 && g->oy > 32 && g->oy < 146 &&
                 g->st == ST_IDLE && !g->pays_open && g->cash > 0) {
            float str = (dy - 34) / 90.f;
            start_spin(ctx, str > 1 ? 1 : str);
        }
    }
    g->was_touch = in->touching;

    // live lever follow while dragging
    if (in->touching && g->st == ST_IDLE && !g->pays_open &&
        g->oy > 32 && g->oy < 146 && g->ly > g->oy) {
        float lp = (g->ly - g->oy) / 110.f;
        g->lever_live = lp > 1 ? 1 : lp;
    } else g->lever_live *= 0.75f;

    if (g->st == ST_SPIN) {
        float t = (now - g->t0) * 1e-6f;
        bool all_done = true;
        for (int r = 0; r < REELS; r++) {
            float prev = g->pos[r];
            if (g->done[r]) { g->vel[r] = 0; continue; }
            if (t < SPIN_PRE) {
                float b = t / SPIN_PRE;
                g->pos[r] = g->pos0[r] - PRE_BACK * b * b;
                all_done = false;
            } else {
                float tm = t - SPIN_PRE;
                float main_dur = g->dur[r] - g->crawl[r];
                float P0 = g->pos0[r] - PRE_BACK;
                float T = g->travel[r] + PRE_BACK;
                if (tm < main_dur) {
                    // 0.2 s accel ramp, then constant Vmax scroll
                    float te = tm < 0.2f ? tm * tm / 0.4f : tm - 0.1f;
                    g->pos[r] = P0 + (T - CRAWL_SYM) * (te / (main_dur - 0.1f));
                    all_done = false;
                } else if (tm < g->dur[r]) {
                    // detent crawl: dwell then SNAP through each of the last
                    // symbols (cubic frac = slow start, hard click)
                    float c = (tm - main_dur) / g->crawl[r] * CRAWL_SYM;
                    float fl = floorf(c), fr = c - fl;
                    g->pos[r] = P0 + (T - CRAWL_SYM) + fl + fr * fr * fr;
                    all_done = false;
                } else {
                    g->pos[r] = g->pos0[r] + g->travel[r];
                    g->done[r] = true;
                    g->land_us[r] = now;
                    gos_audio_play(&sfx_stop, 200);
                }
                int fs = (int)floorf(g->pos[r]);
                if (!g->done[r] && fs != g->last_sym[r]) {
                    g->last_sym[r] = fs;
                    if (now - g->last_tick_us > 25000) {
                        g->last_tick_us = now;
                        gos_audio_play(&sfx_tick, 120);
                    }
                }
            }
            g->vel[r] = (g->pos[r] - prev) * 60.f;
        }
        // near-miss heartbeat while the third reel creeps
        if (g->tease && g->done[0] && g->done[1] && !g->done[2] &&
            now - g->thump_us > 420000) {
            g->thump_us = now;
            gos_audio_play(&sfx_thump, 180);
        }
        if (all_done && now - g->land_us[REELS - 1] > 350000)
            resolve(ctx);
    } else if (g->st == ST_PAYOUT) {
        float durp = 0.03f * g->win;
        if (durp > 1.4f) durp = 1.4f;
        if (durp < 0.25f) durp = 0.25f;
        float tp = (now - g->t_pay0) * 1e-6f;
        float f = tp / durp;
        g->paid = f >= 1.f ? g->win : (int32_t)(g->win * f);
        int32_t coin = g->paid / g->coin_step;
        if (coin != g->last_coin && g->paid < g->win) {
            g->last_coin = coin;
            gos_audio_play(&sfx_coin, 150);
        }
        // coin fountain out of the win tray, arcing over the glass
        if (g->paid < g->win) {
            int spawn = g->jack ? 6 : 3;
            for (int i = 0; i < NPART && spawn; i++)
                if (g->part[i].life <= 0) {
                    g->part[i] = (part_t){ 92 + (gos_randf(&ctx->rng) - 0.5f) * 52,
                                           190,
                                           (gos_randf(&ctx->rng) - 0.5f) * 3.6f,
                                           -(3.0f + gos_randf(&ctx->rng) * 2.2f), 110 };
                    spawn--;
                }
        }
        if (tp > durp + 0.7f)
            g->st = g->cash > 0 ? ST_IDLE : ST_BROKE;
    }

    // particles fall regardless of state (they die off-screen)
    for (int i = 0; i < NPART; i++)
        if (g->part[i].life > 0) {
            g->part[i].vy += 0.13f;
            g->part[i].x += g->part[i].vx;
            g->part[i].y += g->part[i].vy;
            if (--g->part[i].life <= 0 || g->part[i].y > 228) g->part[i].life = 0;
        }
}

// --- LED seven-segment digits ------------------------------------------------
static void led_digit(int x, int y, int d, gos_color_t on, gos_color_t off)
{
    static const uint8_t seg_tab[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                         0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    static const int8_t R[7][4] = { {1,0,6,2}, {6,1,2,6}, {6,7,2,6}, {1,12,6,2},
                                    {0,7,2,6}, {0,1,2,6}, {1,6,6,2} };
    uint8_t s = d < 0 ? 0 : seg_tab[d];
    for (int i = 0; i < 7; i++)
        gos_gfx_rect_fill((gos_rect_t){ (int16_t)(x + R[i][0]), (int16_t)(y + R[i][1]),
                                        R[i][2], R[i][3] },
                          (s >> i) & 1 ? on : off);
}

static void led_num(int x_right, int y, long v, int ndig,
                    gos_color_t on, gos_color_t off)
{
    if (v < 0) v = 0;
    for (int k = 0; k < ndig; k++) {
        int d = (v > 0 || k == 0) ? (int)(v % 10) : -1;   // ghost leading zeros
        led_digit(x_right - (k + 1) * 11, y, d, on, off);
        v /= 10;
    }
}

// --- symbols (procedural, ~30 px cells, shaded with the custom ramps) --------
static void draw_symbol(int sym, int cx, int cy)
{
    switch (sym) {
    case S_CHERRY:
        gos_gfx_line(cx - 7, cy + 1, cx + 1, cy - 13, P_GRN + 3);
        gos_gfx_line(cx - 6, cy + 1, cx + 2, cy - 13, P_GRN + 3);
        gos_gfx_line(cx + 8, cy + 3, cx + 1, cy - 13, P_GRN + 3);
        gos_gfx_rect_fill((gos_rect_t){ (int16_t)(cx + 1), (int16_t)(cy - 15), 8, 4 }, P_GRN + 5);
        gos_gfx_circle_fill(cx - 8, cy + 8, 8, P_RED + 4);
        gos_gfx_circle(cx - 8, cy + 8, 8, P_RED + 2);
        gos_gfx_circle_fill(cx + 9, cy + 10, 7, P_RED + 4);
        gos_gfx_circle(cx + 9, cy + 10, 7, P_RED + 2);
        gos_gfx_circle_fill(cx - 11, cy + 5, 2, P_RED + 7);
        gos_gfx_circle_fill(cx + 7, cy + 8, 1, P_RED + 7);
        break;
    case S_LEMON:
        gos_gfx_circle_fill(cx - 10, cy, 5, P_AMB + 6);
        gos_gfx_circle_fill(cx + 10, cy, 5, P_AMB + 6);
        gos_gfx_circle_fill(cx, cy, 11, P_AMB + 7);
        gos_gfx_circle(cx, cy, 11, P_AMB + 4);
        gos_gfx_circle_fill(cx - 4, cy - 4, 2, GOS_WHITE);
        break;
    case S_ORANGE:
        gos_gfx_rect_fill((gos_rect_t){ (int16_t)(cx - 1), (int16_t)(cy - 15), 4, 5 }, P_GRN + 4);
        gos_gfx_circle_fill(cx, cy + 1, 12, GOS_ORANGE);
        gos_gfx_circle(cx, cy + 1, 12, P_AMB + 2);
        gos_gfx_circle_fill(cx - 4, cy - 3, 2, P_AMB + 7);
        break;
    case S_BAR: {
        gos_rect_t r = { (int16_t)(cx - 22), (int16_t)(cy - 9), 44, 18 };
        gos_gfx_rect_fill(r, P_GOLD + 5);
        gos_gfx_rect(r, P_GOLD + 2);
        gos_gfx_hline(cx - 21, cx + 20, cy - 8, P_GOLD + 7);
        gos_gfx_text(1, cx - 15, cy - 7, GOS_BLACK, "BAR");
        break;
    }
    case S_BELL: {
        gos_pt_t q[4] = { { (int16_t)(cx - 13), (int16_t)(cy + 7) },
                          { (int16_t)(cx - 6),  (int16_t)(cy - 9) },
                          { (int16_t)(cx + 6),  (int16_t)(cy - 9) },
                          { (int16_t)(cx + 13), (int16_t)(cy + 7) } };
        gos_gfx_quad_fill(q, P_AMB + 4);
        gos_gfx_circle_fill(cx, cy - 8, 7, P_AMB + 5);
        gos_gfx_circle_fill(cx, cy - 15, 2, P_AMB + 2);
        gos_gfx_circle_fill(cx - 4, cy - 10, 2, P_AMB + 7);
        gos_gfx_hline(cx - 14, cx + 14, cy + 8, P_AMB + 1);
        gos_gfx_circle_fill(cx, cy + 12, 3, GOS_GRAY);
        break;
    }
    case S_DIAMOND: {
        gos_pt_t l[4] = { { (int16_t)cx, (int16_t)(cy - 15) },
                          { (int16_t)(cx - 12), (int16_t)cy },
                          { (int16_t)cx, (int16_t)(cy + 15) },
                          { (int16_t)cx, (int16_t)(cy - 15) } };
        gos_pt_t r[4] = { { (int16_t)cx, (int16_t)(cy - 15) },
                          { (int16_t)(cx + 12), (int16_t)cy },
                          { (int16_t)cx, (int16_t)(cy + 15) },
                          { (int16_t)cx, (int16_t)(cy - 15) } };
        gos_gfx_quad_fill(l, P_ICE + 6);
        gos_gfx_quad_fill(r, P_ICE + 4);
        gos_gfx_line(cx - 5, cy - 2, cx, cy - 8, GOS_WHITE);
        gos_gfx_line(cx, cy - 8, cx + 5, cy - 2, GOS_WHITE);
        break;
    }
    case S_SEVEN:
        gos_gfx_rect_fill((gos_rect_t){ (int16_t)(cx - 12), (int16_t)(cy - 15), 25, 7 }, P_RED + 5);
        gos_gfx_hline(cx - 12, cx + 12, cy - 15, P_RED + 7);
        for (int rr = 0; rr <= 22; rr++) {
            int x = cx + 9 - (13 * rr) / 22;
            gos_gfx_hline(x - 3, x + 3, cy - 8 + rr, P_RED + 5);
        }
        gos_gfx_hline(cx - 7, cx - 1, cy + 15, P_RED + 2);
        break;
    }
}

// --- render pieces -----------------------------------------------------------
static void draw_reel(const slots_t *g, int r, int64_t now, int ny)
{
    int rx = reel_x[r];
    int wy = REEL_Y + ny;
    gos_gfx_set_clip((gos_rect_t){ (int16_t)rx, (int16_t)wy, REEL_W, REEL_H });
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)rx, (int16_t)wy, REEL_W, REEL_H }, P_CREAM + 5);
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)rx, (int16_t)(wy + 40), REEL_W, 22 }, P_CREAM + 7);

    float p = g->pos[r];
    float wob = 0;
    if (g->done[r] && g->land_us[r]) {
        // sprung mechanical stop: visible overshoot, quick decay
        float ts = (now - g->land_us[r]) * 1e-6f;
        if (ts < 0.4f) wob = 6.5f * expf(-ts * 7.f) * sinf(ts * 30.f);
    }
    // double-exposure motion blur: extra full draws offset against travel
    float vpx = g->vel[r] * PITCH / 60.f;             // px per frame
    int ghosts = vpx > 11.f ? 2 : vpx > 5.f ? 1 : 0;
    int base = (int)floorf(p);
    for (int j = -2; j <= 2; j++) {
        int k = base + j;
        int idx = (k % STRIP + STRIP) % STRIP;
        int cy = PAYLINE + ny + (int)((p - k) * PITCH + wob);
        if (cy <= wy - 24 || cy >= wy + REEL_H + 24) continue;
        draw_symbol(g->strip[r][idx], rx + REEL_W / 2, cy);
        if (ghosts >= 1)
            draw_symbol(g->strip[r][idx], rx + REEL_W / 2, cy - (int)(vpx * 0.45f));
        if (ghosts >= 2)
            draw_symbol(g->strip[r][idx], rx + REEL_W / 2, cy + (int)(vpx * 0.35f));
    }

    // cylinder shading: solid dark rows at the lip, ordered dither inward
    for (int band = 0; band < 2; band++)
        for (int rr = 0; rr < 9; rr++) {
            int y = band ? wy + REEL_H - 1 - rr : wy + rr;
            if (rr < 3)
                gos_gfx_hline(rx, rx + REEL_W - 1, y, P_CREAM + 1);
            else if (rr < 6)
                for (int x = rx + (y & 1); x < rx + REEL_W; x += 2)
                    gos_gfx_pixel(x, y, P_CREAM + 2);
            else
                for (int x = rx + ((y & 1) * 2); x < rx + REEL_W; x += 4)
                    gos_gfx_pixel(x, y, P_CREAM + 3);
        }
    // payline printed on the glass: short red dashes at the window edges
    for (int x = 0; x < 7; x += 3) {
        gos_gfx_hline(rx + x, rx + x + 1, PAYLINE + ny, P_RED + 5);
        gos_gfx_hline(rx + REEL_W - 2 - x, rx + REEL_W - 1 - x, PAYLINE + ny, P_RED + 5);
    }
    gos_gfx_clear_clip();

    // chrome bezel: bright top-left, dark bottom-right
    gos_gfx_rect((gos_rect_t){ (int16_t)(rx - 1), (int16_t)(wy - 1),
                               REEL_W + 2, REEL_H + 2 }, P_CHR + 3);
    gos_gfx_hline(rx - 1, rx + REEL_W, wy - 1, P_CHR + 6);
    gos_gfx_vline(rx - 1, wy - 1, wy + REEL_H, P_CHR + 5);
    gos_gfx_hline(rx - 1, rx + REEL_W, wy + REEL_H, P_CHR + 1);
}

static void draw_lever(const slots_t *g, int64_t now, int ny)
{
    gos_gfx_rect_fill((gos_rect_t){ 172, (int16_t)(42 + ny), 6, 98 }, P_CHR + 1);
    gos_gfx_vline(172, 42 + ny, 139 + ny, P_CHR + 4);
    float lp = g->lever_live;
    if (g->lever_us) {
        float t = (now - g->lever_us) * 1e-6f, ap = 0;
        if (t < 0.12f) ap = g->lever_depth * (t / 0.12f);
        else if (t < 1.4f) ap = g->lever_depth * expf(-5.f * (t - 0.12f)) *
                                cosf(14.f * (t - 0.12f));
        if (fabsf(ap) > fabsf(lp)) lp = ap;
    }
    if (lp < -0.12f) lp = -0.12f;
    if (lp > 1) lp = 1;
    int top = 50 + ny;
    int ky = top + (int)(lp * 78);
    gos_gfx_vline(174, ky < top ? ky : top, ky < top ? top : ky, P_CHR + 5);
    gos_gfx_vline(175, ky < top ? ky : top, ky < top ? top : ky, P_CHR + 3);
    gos_gfx_circle_fill(175, ky, 7, P_RED + 4);
    gos_gfx_circle(175, ky, 7, P_RED + 2);
    gos_gfx_circle_fill(172, ky - 3, 2, P_RED + 7);
}

static void draw_paytable(const slots_t *g)
{
    (void)g;
    gos_gfx_rect_fill((gos_rect_t){ 6, 40, 172, 168 }, P_CREAM + 6);
    gos_gfx_rect((gos_rect_t){ 6, 40, 172, 168 }, P_WOOD + 2);
    gos_gfx_rect((gos_rect_t){ 7, 41, 170, 166 }, P_GOLD + 4);
    gos_gfx_text(0, 60, 46, P_WOOD + 1, "PAYS  X BET");
    gos_gfx_hline(12, 171, 54, P_GOLD + 3);
    static const uint8_t order[7] = { S_SEVEN, S_DIAMOND, S_BELL, S_BAR,
                                      S_CHERRY, S_ORANGE, S_LEMON };
    for (int i = 0; i < 7; i++) {
        int col = i / 4, row = i % 4;
        int cx = col ? 112 : 30, y = 74 + row * 33;
        draw_symbol(order[i], cx, y);
        char b[8];
        snprintf(b, sizeof b, "%d", pay3[order[i]]);
        gos_gfx_text(1, cx + 26, y - 7, P_WOOD + 1, "%s", b);
    }
    gos_gfx_text(0, 96, 174, P_WOOD + 1, "2 CHERRY = 3");
    gos_gfx_text(0, 96, 186, P_WOOD + 1, "1 CHERRY = 1");
    gos_gfx_text(0, 56, 198, P_WOOD + 2, "TAP TO CLOSE");
}

static void slots_render(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    int64_t now = gos_time_us();
    uint32_t ms = gos_time_ms();
    bool winning = g->st == ST_PAYOUT;

    // live cabinet-ramp mode: spotlight dim during payout, gold strobe on jack
    int mode = 0;
    if (winning) {
        float tp = (now - g->t_pay0) * 1e-6f;
        mode = (g->jack && tp < 1.0f) ? (((ms / 80) & 1) ? 2 : 0) : 1;
    }
    if (mode != g->pal_mode) { set_cabinet_ramps(mode); g->pal_mode = mode; }

    gos_gfx_clear(GOS_BLACK);

    // screen nudge when a reel slams home
    int ny = 0;
    for (int r = 0; r < REELS; r++)
        if (g->land_us[r] && now - g->land_us[r] < 50000) ny = 1;
    if (g->jack && winning && now - g->resolve_us < 120000) ny = 2;

    // ---- header, one row: marquee title left, credits LED right ------------
    {
        gos_gfx_text(1, 8, 10, P_GOLD + 5, "LUCKY ");
        gos_gfx_text(1, 8 + gos_gfx_text_w(1, "LUCKY "), 10, P_RED + 5, "7");
        int sx = (int)((ms / 14) % (GOS_SCREEN_W + 60)) - 30;   // shimmer sweep
        gos_gfx_set_clip((gos_rect_t){ (int16_t)sx, 10, 12, 14 });
        gos_gfx_text(1, 8, 10, P_GOLD + 7, "LUCKY ");
        gos_gfx_text(1, 8 + gos_gfx_text_w(1, "LUCKY "), 10, P_RED + 7, "7");
        gos_gfx_clear_clip();
    }
    gos_gfx_rect_fill((gos_rect_t){ 94, 7, 84, 20 }, GOS_BLACK);
    gos_gfx_rect((gos_rect_t){ 94, 7, 84, 20 }, P_CHR + 3);
    gos_gfx_text(0, 98, 13, P_GOLD + 4, "$");
    int32_t shown = winning ? g->cash - g->win + g->paid : g->cash;
    led_num(175, 9, shown > 999999 ? 999999 : shown, 6, P_RED + 6, GOS_GRAY64(3));

    // ---- reel unit: brushed metal face, gold rails, three windows ----------
    gos_gfx_rect_fill((gos_rect_t){ 0, (int16_t)(30 + ny), 184, 122 }, P_CHR + 2);
    gos_gfx_hline(0, 183, 30 + ny, P_GOLD + 5);
    gos_gfx_hline(0, 183, 31 + ny, P_GOLD + 2);
    gos_gfx_hline(0, 183, 150 + ny, P_GOLD + 2);
    gos_gfx_hline(0, 183, 151 + ny, P_GOLD + 5);
    for (int r = 0; r < REELS; r++) {     // vent slots on the brow
        int rx = reel_x[r];
        gos_gfx_rect_fill((gos_rect_t){ (int16_t)(rx + 8), (int16_t)(36 + ny),
                                        REEL_W - 16, 3 }, P_CHR + 0);
        gos_gfx_hline(rx + 8, rx + REEL_W - 9, 39 + ny, P_CHR + 4);
    }

    for (int r = 0; r < REELS; r++) draw_reel(g, r, now, ny);

    // idle sparkle glint sweeping a payline symbol
    if (g->st == ST_IDLE && !g->pays_open) {
        int64_t ph = now % 4000000;
        if (ph < 300000) {
            float f = ph / 300000.f;
            int rx = reel_x[(now / 4000000) % 3];
            int px = rx + 10 + (int)(f * 30), py = 84 + (int)(f * 20) + ny;
            gos_gfx_pixel(px, py, GOS_WHITE);
            gos_gfx_pixel(px + 1, py, P_CREAM + 7);
            gos_gfx_pixel(px, py + 1, P_CREAM + 7);
        }
    }

    // payline arrows on the rails (pulse bright during the near-miss creep)
    {
        bool crawl = g->tease && g->done[0] && g->done[1] && !g->done[2];
        gos_color_t ac = crawl && ((ms / 210) & 1) ? P_RED + 7 : P_RED + 5;
        gos_pt_t l[4] = { {0, (int16_t)(89 + ny)}, {3, (int16_t)(95 + ny)},
                          {0, (int16_t)(101 + ny)}, {0, (int16_t)(89 + ny)} };
        gos_pt_t rr[4] = { {167, (int16_t)(89 + ny)}, {164, (int16_t)(95 + ny)},
                           {167, (int16_t)(101 + ny)}, {167, (int16_t)(89 + ny)} };
        gos_gfx_quad_fill(l, ac);
        gos_gfx_quad_fill(rr, ac);
    }

    // win celebration: payline strobe + rings around the matched symbols
    if (winning) {
        if ((ms / 70) & 1)
            for (int r = 0; r < REELS; r++)
                for (int y = 93; y <= 97; y += 2)
                    for (int x = reel_x[r] + ((y / 2) & 1); x < reel_x[r] + REEL_W; x += 2)
                        gos_gfx_pixel(x, y + ny, P_GOLD + 7);
        int pr = 16 + (int)((ms / 90) % 3);
        for (int r = 0; r < REELS; r++)
            if (g->match_mask & (1 << r)) {
                gos_gfx_set_clip((gos_rect_t){ (int16_t)reel_x[r], (int16_t)(REEL_Y + ny),
                                               REEL_W, REEL_H });
                gos_gfx_circle(reel_x[r] + REEL_W / 2, PAYLINE + ny, pr, P_GOLD + 6);
                gos_gfx_clear_clip();
            }
        if (g->jack && ((ms / 130) & 1))
            gos_gfx_text(1, (GOS_SCREEN_W - gos_gfx_text_w(1, "JACKPOT!")) / 2,
                         56 + ny, ((ms / 260) & 1) ? P_GOLD + 7 : P_RED + 6, "JACKPOT!");
    }
    if (g->st == ST_BROKE && ((ms / 200) & 1))
        gos_gfx_text(1, (GOS_SCREEN_W - gos_gfx_text_w(1, "BUSTED!")) / 2,
                     88 + ny, P_RED + 6, "BUSTED!");

    draw_lever(g, now, ny);

    // ---- control deck: big chrome bet buttons around the BET/WIN LEDs ------
    gos_gfx_rect_fill((gos_rect_t){ 0, 152, 184, 56 }, P_CHR + 1);
    for (int y = 155; y < 206; y += 5) gos_gfx_hline(0, 183, y, P_CHR + 0);
    if (g->st == ST_BROKE) {
        gos_gfx_rect_fill((gos_rect_t){ 50, 156, 84, 46 }, GOS_RED);
        gos_gfx_rect((gos_rect_t){ 50, 156, 84, 46 }, P_RED + 7);
        gos_gfx_text(0, 65, 162, GOS_WHITE, "TAP: LOAN");
        gos_gfx_text(1, 67, 176, GOS_WHITE, "+$%d", LOAN);
    } else {
        for (int b = 0; b < 2; b++) {
            int bx = b ? 138 : 2;
            gos_gfx_rect_fill((gos_rect_t){ (int16_t)bx, 156, 44, 46 }, P_CHR + 3);
            gos_gfx_hline(bx, bx + 43, 156, P_CHR + 6);
            gos_gfx_vline(bx, 156, 201, P_CHR + 5);
            gos_gfx_hline(bx, bx + 43, 201, P_CHR + 1);
            gos_gfx_text(1, bx + 17, 172, GOS_BLACK, b ? "+" : "-");
        }
        gos_gfx_rect_fill((gos_rect_t){ 50, 156, 84, 46 }, GOS_BLACK);
        gos_gfx_rect((gos_rect_t){ 50, 156, 84, 46 }, P_GOLD + 4);
        gos_gfx_text(0, 56, 162, P_GOLD + 4, "BET");
        led_num(128, 158, bets[g->bet_ix], 2, P_RED + 6, GOS_GRAY64(3));
        gos_gfx_text(0, 56, 184, P_GOLD + 4, "WIN");
        led_num(128, 180, winning ? g->paid : g->win, 4, P_AMB + 6, GOS_GRAY64(3));
    }

    // coin fountain: fat bright coins with glints
    for (int i = 0; i < NPART; i++)
        if (g->part[i].life > 0) {
            int px = (int)g->part[i].x, py = (int)g->part[i].y;
            gos_gfx_circle_fill(px, py, 2, P_AMB + 6);
            gos_gfx_pixel(px, py - 1, P_AMB + 7);
            gos_gfx_pixel(px - 1, py, i & 1 ? GOS_WHITE : P_AMB + 7);
        }

    if (g->pays_open) draw_paytable(g);

    // bottom hint (H-14 per safe zone), cycling
    {
        const char *h = NULL;
        if (g->pays_open) h = NULL;
        else if (g->st == ST_BROKE) h = "OUT OF CASH";
        else if (g->st == ST_IDLE) {
            int c = (ms / 2600) % 3;
            static char bb[28];
            if (c == 0) h = "YANK OR DRAG DOWN TO PULL";
            else if (c == 1) h = "TAP MARQUEE FOR PAYS";
            else if (g->best_win > 0) {
                snprintf(bb, sizeof bb, "BEST WIN $%ld", (long)g->best_win);
                h = bb;
            } else h = "YANK OR DRAG DOWN TO PULL";
        }
        if (h)
            gos_gfx_text(0, (GOS_SCREEN_W - gos_gfx_text_w(0, h)) / 2, 212,
                         P_GOLD + 4, "%s", h);
    }
}

static void slots_suspend(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    for (int i = 0; i < NPART; i++) g->part[i].life = 0;
    save_now(ctx);
}

static void slots_resume(gos_ctx_t *ctx)
{
    slots_t *g = ctx->state;
    g->pd_phase = PD_WAIT;     // motion during the overlay must not fire a pull
    g->burst = 0;
    g->was_touch = false;
    g->lever_live = 0;
    g->pal_mode = -1;          // force a cabinet-ramp rewrite next frame
    set_fixed_ramps();
}

static void slots_teardown(gos_ctx_t *ctx) { save_now(ctx); }

const gos_game_t game_slots = {
    .id = "slots",
    .title = "LUCKY 7",
    .icon = NULL,
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_SAVE,
    .state_size = sizeof(slots_t),
    .init = slots_init,
    .update = slots_update,
    .render = slots_render,
    .suspend = slots_suspend,
    .resume = slots_resume,
    .teardown = slots_teardown,
};
