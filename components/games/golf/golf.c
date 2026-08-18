// golf.c — Infinite Golf on the gameos contract, 1:1 with the standalone
// firmware. Logic half: course generation, physics, clubs, wind, swing
// detection, multiplayer turn rotation, cards flow, persistence.
//
// Everything gameplay-visible is the original, verbatim: world 736x896,
// all club/physics constants, drag power (DRAG_SCALE 0.12 in full-res touch
// coords — gos touch arrives at half-res and is doubled at entry), the
// swing detector (fed by the OS's raw 200 Hz accel stream instead of its
// own task — same math, same thresholds), pass-and-play for 2-4, the title
// attract loop, plaque-hold record clearing, initials and the top-5 board.
// Logic ticks on a 20 ms accumulator: the original's loop cadence.
//
// The single deliberate deviation: the options page has no SOUND row — the
// OS owns the device volume (Settings on the launcher).

#include "golf_int.h"

golf_t *G;
uint16_t *CV;

const char *club_names[CLUB_COUNT] = { "PUTTER", "CHIPPER", "DRIVER" };
const float club_mult[CLUB_COUNT] = { 0.55f, 0.9f, 1.45f };
const float club_airk[CLUB_COUNT] = { 0.0f, 1.6f, 0.9f };
const float club_keep[CLUB_COUNT] = { 1.0f, 0.35f, 0.20f };
const float club_curl[CLUB_COUNT] = { 0.0f, 0.0f, -0.006f };
const float club_sand_eff[CLUB_COUNT] = { 0.22f, 1.0f, 0.5f };
const float club_sand_wobble[CLUB_COUNT] = { 1.3f, 0.6f, 1.8f };
const float club_zoom[CLUB_COUNT] = { 1.35f, 1.15f, 1.0f };

const uint8_t ball_rgb[BALL_COLORS][3] = {
    { 246, 246, 250 }, { 250, 210, 74 },  { 242, 116, 106 },
    { 118, 188, 248 }, { 186, 142, 246 }, { 104, 224, 188 },
};

static inline bool club_button_hit(float x, float y)
{
    return x < 185.0f && y > 385.0f;
}

// ---------------------------------------------------------------------------
// Audio — the standalone's synth note tables, retyped as gos notes
// ---------------------------------------------------------------------------

#define SFX(name, prio_, ...) \
    static const gos_note_t name##_n[] = { __VA_ARGS__ }; \
    static const gos_sfx_t name = { name##_n, sizeof(name##_n) / sizeof(gos_note_t), prio_ }

SFX(sfx_putt, 2,   { 900, 680, 36, GOS_W_TRI, 150 });
SFX(sfx_chip, 2,   { 3000, 1200, 12, GOS_W_NOISE, 140 }, { 620, 380, 48, GOS_W_TRI, 175 });
SFX(sfx_drive, 2,  { 2200, 800, 15, GOS_W_NOISE, 170 }, { 330, 120, 70, GOS_W_TRI, 225 });
SFX(sfx_sand, 2,   { 1400, 450, 90, GOS_W_NOISE, 160 });
SFX(sfx_arm, 2,    { 1200, 1200, 22, GOS_W_SQ25, 110 });
SFX(sfx_land, 2,   { 240, 165, 42, GOS_W_TRI, 140 });
SFX(sfx_splash, 3, { 2600, 500, 70, GOS_W_NOISE, 170 }, { 700, 170, 110, GOS_W_TRI, 150 });
SFX(sfx_cup, 3,    { 760, 230, 85, GOS_W_TRI, 170 }, { 180, 175, 70, GOS_W_TRI, 150 });
SFX(sfx_tap, 1,    { 1050, 1050, 24, GOS_W_SQ50, 120 });
SFX(sfx_tick, 1,   { 1500, 1500, 14, GOS_W_SQ25, 100 });
SFX(sfx_club, 1,   { 800, 800, 20, GOS_W_SQ25, 110 }, { 1100, 1100, 22, GOS_W_SQ25, 110 });
SFX(sfx_pop, 1,    { 480, 950, 30, GOS_W_SQ50, 130 });
SFX(sfx_wipe, 1,   { 280, 1250, 170, GOS_W_SQ25, 85 });
SFX(sfx_par, 4,    { 880, 880, 80, GOS_W_TRI, 150 });
SFX(sfx_birdie, 4, { 659, 659, 75, GOS_W_SQ25, 150 }, { 784, 784, 75, GOS_W_SQ25, 150 },
                   { 1047, 1047, 130, GOS_W_SQ25, 160 });
SFX(sfx_bogey, 4,  { 392, 360, 110, GOS_W_SQ50, 150 }, { 320, 280, 190, GOS_W_SQ50, 150 });
SFX(sfx_ace, 5,
    { 784, 784, 55, GOS_W_SQ25, 150 },   { 784, 784, 55, GOS_W_SQ25, 150 },
    { 0, 0, 25, GOS_W_REST, 0 },
    { 784, 784, 50, GOS_W_SQ25, 152 },   { 880, 880, 50, GOS_W_SQ25, 155 },
    { 988, 988, 90, GOS_W_SQ25, 158 },
    { 0, 0, 60, GOS_W_REST, 0 },
    { 1047, 1047, 95, GOS_W_SQ25, 162 }, { 0, 0, 40, GOS_W_REST, 0 },
    { 1319, 1319, 95, GOS_W_SQ25, 166 }, { 0, 0, 40, GOS_W_REST, 0 },
    { 1568, 1568, 95, GOS_W_SQ25, 170 },
    { 0, 0, 50, GOS_W_REST, 0 },
    { 1319, 1319, 55, GOS_W_SQ25, 165 }, { 1568, 1568, 55, GOS_W_SQ25, 170 },
    { 2093, 2093, 330, GOS_W_SQ25, 172 });
SFX(sfx_round, 4,  { 523, 523, 90, GOS_W_SQ25, 150 }, { 392, 392, 90, GOS_W_SQ25, 150 },
                   { 523, 523, 90, GOS_W_SQ25, 155 }, { 659, 659, 90, GOS_W_SQ25, 160 },
                   { 784, 784, 260, GOS_W_SQ25, 170 });
SFX(sfx_title, 3,  { 523, 523, 60, GOS_W_SQ25, 130 }, { 659, 659, 60, GOS_W_SQ25, 135 },
                   { 784, 784, 60, GOS_W_SQ25, 140 }, { 1047, 1047, 170, GOS_W_SQ25, 150 });

void golf_sfx(int id)
{
    static const gos_sfx_t *tab[] = {
        [SFXG_PUTT] = &sfx_putt,   [SFXG_CHIP] = &sfx_chip,
        [SFXG_DRIVE] = &sfx_drive, [SFXG_SAND] = &sfx_sand,
        [SFXG_ARM] = &sfx_arm,     [SFXG_LAND] = &sfx_land,
        [SFXG_SPLASH] = &sfx_splash, [SFXG_CUP] = &sfx_cup,
        [SFXG_TAP] = &sfx_tap,     [SFXG_TICK] = &sfx_tick,
        [SFXG_CLUB] = &sfx_club,   [SFXG_POP] = &sfx_pop,
        [SFXG_WIPE] = &sfx_wipe,   [SFXG_PAR] = &sfx_par,
        [SFXG_BIRDIE] = &sfx_birdie, [SFXG_BOGEY] = &sfx_bogey,
        [SFXG_ACE] = &sfx_ace,     [SFXG_ROUND] = &sfx_round,
        [SFXG_TITLE] = &sfx_title,
    };
    gos_audio_play(tab[id], 200);
}

// ---------------------------------------------------------------------------
// Math helpers (verbatim)
// ---------------------------------------------------------------------------

float golf_randf(void)
{
    G->rng_state = G->rng_state * 1103515245 + 12345;
    return (float)(G->rng_state >> 16) / 65536.0f;
}
#define randf golf_randf

float noise2d(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    uint32_t h00 = (ix * 374761393 + iy * 668265263) ^ 0x85ebca6b;
    uint32_t h10 = ((ix + 1) * 374761393 + iy * 668265263) ^ 0x85ebca6b;
    uint32_t h01 = (ix * 374761393 + (iy + 1) * 668265263) ^ 0x85ebca6b;
    uint32_t h11 = ((ix + 1) * 374761393 + (iy + 1) * 668265263) ^ 0x85ebca6b;
    float v00 = (float)(h00 & 0xFFFF) / 65536.0f;
    float v10 = (float)(h10 & 0xFFFF) / 65536.0f;
    float v01 = (float)(h01 & 0xFFFF) / 65536.0f;
    float v11 = (float)(h11 & 0xFFFF) / 65536.0f;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float v0 = v00 + sx * (v10 - v00), v1 = v01 + sx * (v11 - v01);
    return v0 + sy * (v1 - v0);
}

uint32_t pixel_hash(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float smoothstepf(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float smin(float a, float b, float k)
{
    float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1.0f - h);
}

float dist2d(float x1, float y1, float x2, float y2)
{
    float dx = x2 - x1, dy = y2 - y1;
    return sqrtf(dx * dx + dy * dy);
}

// ---------------------------------------------------------------------------
// SDFs (verbatim)
// ---------------------------------------------------------------------------

static float blobs_sdf(float x, float y)
{
    float d = 1e9f;
    for (int i = 0; i < G->num_blobs; i++) {
        float di = dist2d(x, y, G->blob_x[i], G->blob_y[i]) - G->blob_r[i];
        d = smin(d, di, 44.0f);
    }
    return d;
}

float sdf_green(float x, float y)
{
    float d0 = blobs_sdf(x, y);
    if (d0 > 34.0f || d0 < -34.0f) return d0;
    float wx = x + (noise2d(x * 0.021f, y * 0.021f) - 0.5f) * 30.0f;
    float wy = y + (noise2d(x * 0.021f + 43.7f, y * 0.021f + 17.3f) - 0.5f) * 30.0f;
    return blobs_sdf(wx, wy);
}

static float hazard_lobes_sdf(const hazard_t *hz, float x, float y)
{
    float d = 1e9f;
    for (int i = 0; i < hz->nlobes; i++)
        d = smin(d, dist2d(x, y, hz->lx[i], hz->ly[i]) - hz->lr[i], 18.0f);
    return d;
}

float sdf_hazard(int h, float x, float y)
{
    const hazard_t *hz = &G->hazards[h];
    float d0 = hazard_lobes_sdf(hz, x, y);
    if (d0 > 26.0f || d0 < -26.0f) return d0;
    float s = h * 57.0f;
    float wx = x + (noise2d(x * 0.045f + s, y * 0.045f) - 0.5f) * 13.0f;
    float wy = y + (noise2d(x * 0.045f + s + 11.1f, y * 0.045f + 5.7f) - 0.5f) * 13.0f;
    return hazard_lobes_sdf(hz, wx, wy);
}

static bool ball_in_sand(void)
{
    for (int h = 0; h < G->num_hazards; h++)
        if (G->hazards[h].type == 0 && sdf_hazard(h, G->ball_x, G->ball_y) < -1.0f)
            return true;
    return false;
}

static bool ball_in_rough(void)
{
    return sdf_green(G->ball_x, G->ball_y) >= 1.0f && !ball_in_sand();
}

float lie_wobble(void)
{
    if (ball_in_sand()) return club_sand_wobble[G->club];
    if (ball_in_rough()) return 1.0f;
    return 0.0f;
}

static void auto_putter(void)
{
    if (ball_in_sand()) {
        G->club = CLUB_CHIPPER;
        return;
    }
    if (dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y) < 64.0f &&
        sdf_green(G->ball_x, G->ball_y) < -2.0f)
        G->club = CLUB_PUTTER;
}

// ---------------------------------------------------------------------------
// Multiplayer plumbing (verbatim structure)
// ---------------------------------------------------------------------------

static void save_active_player(void)
{
    player_t *p = &G->players[G->cur_player];
    p->x = G->ball_x;
    p->y = G->ball_y;
    p->sx = G->shot_start_x;
    p->sy = G->shot_start_y;
    p->strokes = G->stroke_count;
    p->club = G->club;
}

static void load_player(int i)
{
    G->cur_player = i;
    player_t *p = &G->players[i];
    G->ball_x = p->x;
    G->ball_y = p->y;
    G->ball_vx = G->ball_vy = 0;
    G->shot_start_x = p->sx;
    G->shot_start_y = p->sy;
    G->stroke_count = p->strokes;
    G->club = p->club;
    G->air_time = G->air_total = 0;
}

static int next_unholed(int i)
{
    for (int k = 1; k <= G->num_players; k++) {
        int j = (i + k) % G->num_players;
        if (!G->players[j].holed) return j;
    }
    return -1;
}

static void mp_reset_hole(void)
{
    if (G->num_players <= 1) return;
    static const float ox[MAX_PLAYERS] = { 0, -14, 14, 0 };
    static const float oy[MAX_PLAYERS] = { 0, 9, 9, 18 };
    for (int i = 0; i < G->num_players; i++) {
        G->players[i].x = G->ball_x + ox[i];
        G->players[i].y = G->ball_y + oy[i];
        G->players[i].sx = G->players[i].x;
        G->players[i].sy = G->players[i].y;
        G->players[i].strokes = 0;
        G->players[i].club = CLUB_DRIVER;
        G->players[i].holed = false;
        G->players[i].seen_intro = false;
    }
    load_player(0);
}

static void cycle_player_color(int i)
{
    int nc = (G->players[i].color + 1) % BALL_COLORS;
    G->players[i].color = (uint8_t)nc;
    for (int j = 0; j < G->num_players; j++) {
        if (j == i || G->players[j].color != nc) continue;
        for (int step = 1; step < BALL_COLORS; step++) {
            int alt = (G->players[j].color + step) % BALL_COLORS;
            bool taken = false;
            for (int k = 0; k < G->num_players && !taken; k++)
                if (k != j && G->players[k].color == alt) taken = true;
            if (!taken) {
                G->players[j].color = (uint8_t)alt;
                break;
            }
        }
        break;
    }
}

// The hand-over card: dim the last frame once, draw the card; a tapped-
// through ace must not replay its confetti on the next player's hole-out
static void show_pass_screen(int i)
{
    load_player(i);
    G->st = GST_PASS;
    G->conf_active = false;
    for (int j = 0; j < MAX_CONF; j++) G->conf_life[j] = 0;
    G->notice_until_us = 0;
    dim_canvas(0.42f);
    draw_pass_card(i);
}

static void mp_after_rest(void)
{
    save_active_player();
    int nxt = next_unholed(G->cur_player);
    if (nxt >= 0 && nxt != G->cur_player) {
        show_pass_screen(nxt);
    } else {
        auto_putter();
        G->st = GST_READY;
    }
}

// ---------------------------------------------------------------------------
// Course generation (verbatim; unchanged from the half-res port — the world
// never changed scale, only the renderer did)
// ---------------------------------------------------------------------------

static bool place_bunker(float ax, float ay, float spread, float rmin, float rvar)
{
    if (G->num_hazards >= MAX_HAZARDS) return false;
    for (int attempt = 0; attempt < 30; attempt++) {
        float r = rmin + randf() * rvar;
        float x = ax + (randf() - 0.5f) * spread;
        float y = ay + (randf() - 0.5f) * spread;
        if (dist2d(x, y, G->hole_x, G->hole_y) < r + HOLE_RADIUS + 28) continue;
        if (dist2d(x, y, G->ball_x, G->ball_y) < r + 36) continue;
        if (sdf_green(x, y) > 6.0f) continue;
        hazard_t *h = &G->hazards[G->num_hazards];
        h->type = 0;
        h->lx[0] = x; h->ly[0] = y; h->lr[0] = r;
        float a2 = randf() * 2.0f * (float)M_PI;
        h->lx[1] = x + cosf(a2) * r * 0.7f;
        h->ly[1] = y + sinf(a2) * r * 0.7f;
        h->lr[1] = r * (0.55f + randf() * 0.25f);
        h->nlobes = 2;
        if (randf() < 0.45f) {
            float a3 = a2 + 1.6f + randf() * 2.0f;
            h->lx[2] = x + cosf(a3) * r * 0.85f;
            h->ly[2] = y + sinf(a3) * r * 0.85f;
            h->lr[2] = r * (0.45f + randf() * 0.30f);
            h->nlobes = 3;
        }
        h->bridge = 0;
        G->num_hazards++;
        return true;
    }
    return false;
}

static bool place_pond(float sx, float sy, float perp_x, float perp_y,
                       float base_r, float wrmin, float wrvar)
{
    if (G->num_hazards >= MAX_HAZARDS) return false;
    float side = (randf() < 0.5f) ? -1.0f : 1.0f;
    for (int attempt = 0; attempt < 30; attempt++) {
        float wr = wrmin + randf() * wrvar;
        float wx = sx + perp_x * side * (base_r * 0.72f + wr * 0.30f);
        float wy = sy + perp_y * side * (base_r * 0.72f + wr * 0.30f);
        float edge = sdf_green(wx, wy);
        if (edge < -wr * 0.5f || edge > wr * 0.5f) { side = -side; continue; }
        if (dist2d(wx, wy, G->hole_x, G->hole_y) < wr + HOLE_RADIUS + 36) continue;
        if (dist2d(wx, wy, G->ball_x, G->ball_y) < wr + 48) continue;
        hazard_t *h = &G->hazards[G->num_hazards];
        h->type = 1;
        h->lx[0] = wx; h->ly[0] = wy; h->lr[0] = wr;
        h->lx[1] = wx + perp_x * side * wr * 0.55f;
        h->ly[1] = wy + perp_y * side * wr * 0.55f;
        h->lr[1] = wr * 0.75f;
        float a3 = randf() * 2.0f * (float)M_PI;
        h->lx[2] = wx + cosf(a3) * wr * 0.65f;
        h->ly[2] = wy + sinf(a3) * wr * 0.65f;
        h->lr[2] = wr * (0.5f + randf() * 0.25f);
        h->nlobes = 3;
        if (randf() < 0.5f) {
            float a4 = a3 + 1.8f + randf() * 1.6f;
            h->lx[3] = wx + cosf(a4) * wr * 0.7f;
            h->ly[3] = wy + sinf(a4) * wr * 0.7f;
            h->lr[3] = wr * (0.45f + randf() * 0.25f);
            h->nlobes = 4;
        }
        h->bridge = 0;
        G->num_hazards++;
        return true;
    }
    return false;
}

static bool place_ribbon_sand(float tee_x, float tee_y, float ctl_x, float ctl_y,
                              float pin_x, float pin_y)
{
    if (G->num_hazards >= MAX_HAZARDS) return false;
    hazard_t *h = &G->hazards[G->num_hazards];
    float side = (randf() < 0.5f) ? -1.0f : 1.0f;
    int nl = 4 + (int)(randf() * 3.0f);
    if (nl > MAX_LOBES) nl = MAX_LOBES;
    float slen_est = dist2d(tee_x, tee_y, pin_x, pin_y);
    float spacing_px = 17.0f + randf() * 6.0f;
    float span = (nl - 1) * spacing_px / slen_est;
    float t0 = 0.20f + randf() * clampf(0.60f - span, 0.05f, 0.6f);

    for (int i = 0; i < nl; i++) {
        float t = t0 + span * i / (nl - 1);
        float omt = 1.0f - t;
        float sx = omt * omt * tee_x + 2 * omt * t * ctl_x + t * t * pin_x;
        float sy = omt * omt * tee_y + 2 * omt * t * ctl_y + t * t * pin_y;
        float tx_ = 2 * omt * (ctl_x - tee_x) + 2 * t * (pin_x - ctl_x);
        float ty_ = 2 * omt * (ctl_y - tee_y) + 2 * t * (pin_y - ctl_y);
        float tl = sqrtf(tx_ * tx_ + ty_ * ty_);
        if (tl < 0.01f) return false;
        float ppx = -ty_ / tl * side, ppy = tx_ / tl * side;
        float target = -(12.0f + randf() * 12.0f);
        float lo = 0, hi = 240;
        for (int it = 0; it < 14; it++) {
            float mid = (lo + hi) * 0.5f;
            if (sdf_green(sx + ppx * mid, sy + ppy * mid) < target) lo = mid;
            else hi = mid;
        }
        float d = (lo + hi) * 0.5f + (randf() - 0.5f) * 8.0f;
        float wx = sx + ppx * d, wy = sy + ppy * d;
        float r = (13.0f + randf() * 4.0f) *
                  (0.75f + 0.45f * sinf((float)M_PI * i / (nl - 1)));
        if (dist2d(wx, wy, G->hole_x, G->hole_y) < r + HOLE_RADIUS + 30) return false;
        if (dist2d(wx, wy, G->ball_x, G->ball_y) < r + 40) return false;
        h->lx[i] = wx; h->ly[i] = wy; h->lr[i] = r;
    }
    h->nlobes = nl;
    h->type = 0;
    h->bridge = 0;
    G->num_hazards++;
    return true;
}

static bool place_creek(float tee_x, float tee_y, float ctl_x, float ctl_y,
                        float pin_x, float pin_y, float base_r)
{
    if (G->num_hazards >= MAX_HAZARDS) return false;
    hazard_t *h = &G->hazards[G->num_hazards];
    float t = 0.34f + randf() * 0.26f;
    float omt = 1.0f - t;
    float sx = omt * omt * tee_x + 2 * omt * t * ctl_x + t * t * pin_x;
    float sy = omt * omt * tee_y + 2 * omt * t * ctl_y + t * t * pin_y;
    float tx_ = 2 * omt * (ctl_x - tee_x) + 2 * t * (pin_x - ctl_x);
    float ty_ = 2 * omt * (ctl_y - tee_y) + 2 * t * (pin_y - ctl_y);
    float tl = sqrtf(tx_ * tx_ + ty_ * ty_);
    if (tl < 0.01f) return false;
    float ppx = -ty_ / tl, ppy = tx_ / tl;
    float dxn = tx_ / tl, dyn = ty_ / tl;

    float half_span = base_r * 1.15f + 30.0f + randf() * 25.0f;
    int nl = 9;
    float spacing = 2.0f * half_span / (nl - 1);
    float phase = randf() * 6.28f;
    for (int i = 0; i < nl; i++) {
        float q = -1.0f + 2.0f * i / (nl - 1);
        float meander = sinf(phase + i * 1.1f) * 12.0f;
        float wx = sx + ppx * q * half_span + dxn * meander;
        float wy = sy + ppy * q * half_span + dyn * meander;
        float r = (spacing - 4.0f) * 0.5f * (0.95f + randf() * 0.3f);
        if (dist2d(wx, wy, G->hole_x, G->hole_y) < r + HOLE_RADIUS + 34) return false;
        if (dist2d(wx, wy, G->ball_x, G->ball_y) < r + 56) return false;
        h->lx[i] = wx; h->ly[i] = wy; h->lr[i] = r;
    }
    h->nlobes = nl;
    h->type = 1;
    h->bridge = 1;
    G->num_hazards++;
    return true;
}

static void generate_hole(int hole_num)
{
    G->hole_seed = G->run_seed + (uint32_t)hole_num * 7919u;
    G->rng_state = G->hole_seed;

    float ramp = clampf(hole_num / 18.0f, 0.0f, 1.0f);
    layout_t layout;
    if (hole_num == 0) {
        layout = LAYOUT_STRAIGHT;
    } else {
        float w_straight = 1.0f;
        float w_dogleg = 1.0f + 1.6f * ramp;
        float w_short = 1.0f - 0.35f * ramp;
        float w_water = 1.0f + 0.4f * ramp;
        float w_scurve = (hole_num >= 4) ? (0.4f + 0.9f * ramp) : 0.0f;
        do {
            float r = randf() * (w_straight + w_dogleg + w_short + w_water + w_scurve);
            if (r < w_straight) layout = LAYOUT_STRAIGHT;
            else if (r < w_straight + w_dogleg) layout = LAYOUT_DOGLEG;
            else if (r < w_straight + w_dogleg + w_short) layout = LAYOUT_SHORT;
            else if (r < w_straight + w_dogleg + w_short + w_water) layout = LAYOUT_WATER;
            else layout = LAYOUT_SCURVE;
        } while (layout == G->prev_layout);
    }
    G->prev_layout = layout;

    float mirror = (randf() < 0.5f) ? -1.0f : 1.0f;

    float tee_x = 140.0f + randf() * (WORLD_W - 280.0f);
    float tee_y = WORLD_H - 120.0f - randf() * 50.0f;
    float sep = (320.0f + 620.0f * ramp) * (0.85f + randf() * 0.30f);
    float pin_x = WORLD_W * 0.5f + (randf() - 0.5f) * 190.0f;
    float dxp = pin_x - tee_x;
    float dyp = sep * sep > dxp * dxp ? sqrtf(sep * sep - dxp * dxp) : 60.0f;
    float pin_y = tee_y - dyp;
    if (pin_y < 150.0f) {
        pin_y = 150.0f + randf() * 60.0f;
        float dy2 = tee_y - pin_y;
        if (sep > dy2) {
            float need = sqrtf(sep * sep - dy2 * dy2);
            float sgn = tee_x < WORLD_W * 0.5f ? 1.0f : -1.0f;
            pin_x = clampf(tee_x + sgn * need, 120.0f, WORLD_W - 120.0f);
        }
    }

    float base_r = 100, waist = 0.1f, bend = 0, scurve_amp = 0;
    switch (layout) {
    case LAYOUT_STRAIGHT:
        G->num_blobs = 4 + (randf() < 0.5f ? 1 : 0);
        base_r = 96 + randf() * 20;
        waist = 0.08f + randf() * 0.10f;
        bend = (randf() - 0.5f) * 90.0f;
        break;
    case LAYOUT_DOGLEG:
        G->num_blobs = 5; base_r = 92 + randf() * 16;
        waist = 0.24f + randf() * 0.12f;
        bend = mirror * (110.0f + 90.0f * ramp + randf() * 80.0f);
        tee_x = clampf(tee_x - bend * 0.30f, 175, WORLD_W - 175);
        pin_x = clampf(pin_x - bend * 0.30f, 175, WORLD_W - 175);
        break;
    case LAYOUT_SHORT:
        G->num_blobs = 3; base_r = 112 + randf() * 22; waist = 0.06f;
        bend = (randf() - 0.5f) * 50.0f;
        pin_y = tee_y - 280.0f - randf() * 100.0f;
        pin_x = tee_x + (randf() - 0.5f) * 130.0f;
        break;
    case LAYOUT_WATER:
        G->num_blobs = 4; base_r = 90 + randf() * 14; waist = 0.20f;
        bend = mirror * (40.0f + randf() * 50.0f);
        break;
    default:
        G->num_blobs = 5; base_r = 94 + randf() * 12;
        waist = 0.14f + randf() * 0.08f;
        bend = (randf() - 0.5f) * 40.0f;
        scurve_amp = mirror * (55.0f + 45.0f * ramp + randf() * 30.0f);
        break;
    }

    float sdx = pin_x - tee_x, sdy = pin_y - tee_y;
    float slen = sqrtf(sdx * sdx + sdy * sdy);
    float perp_x = -sdy / slen, perp_y = sdx / slen;
    float ctl_x = (tee_x + pin_x) * 0.5f + perp_x * bend;
    float ctl_y = (tee_y + pin_y) * 0.5f + perp_y * bend;
    G->spine_tee_x = tee_x; G->spine_tee_y = tee_y;
    G->spine_ctl_x = ctl_x; G->spine_ctl_y = ctl_y;
    G->spine_pin_x = pin_x; G->spine_pin_y = pin_y;

    for (int i = 0; i < G->num_blobs; i++) {
        float t = (float)i / (G->num_blobs - 1);
        float omt = 1.0f - t;
        float bx = omt * omt * tee_x + 2 * omt * t * ctl_x + t * t * pin_x;
        float by = omt * omt * tee_y + 2 * omt * t * ctl_y + t * t * pin_y;
        bx += perp_x * scurve_amp * sinf(t * 2.0f * (float)M_PI);
        by += perp_y * scurve_amp * sinf(t * 2.0f * (float)M_PI);
        float r = base_r * (1.0f - waist * sinf((float)M_PI * t)) * (0.94f + randf() * 0.12f);
        if (r > 150) r = 150;
        bx = clampf(bx, 24 + r, WORLD_W - 24 - r);
        by = clampf(by, 24 + r, WORLD_H - 26 - r);
        G->blob_x[i] = bx; G->blob_y[i] = by; G->blob_r[i] = r;
    }

    G->rng_state = G->hole_seed ^ 0x9E3779B9u;
    float pbx = G->blob_x[G->num_blobs - 1], pby = G->blob_y[G->num_blobs - 1];
    float pbr = G->blob_r[G->num_blobs - 1];
    G->hole_x = pbx; G->hole_y = pby;
    for (int attempt = 0; attempt < 60; attempt++) {
        float hx = pbx + (randf() - 0.5f) * pbr * 1.1f;
        float hy = pby + (randf() - 0.5f) * pbr * 0.9f;
        if (sdf_green(hx, hy) < -(HOLE_RADIUS + 16) && hy > 90) {
            G->hole_x = hx; G->hole_y = hy;
            break;
        }
    }

    float tbx = G->blob_x[0], tby = G->blob_y[0], tbr = G->blob_r[0];
    G->ball_x = tbx; G->ball_y = tby;
    for (int attempt = 0; attempt < 60; attempt++) {
        float bx = tbx + (randf() - 0.5f) * tbr;
        float by = tby + randf() * tbr * 0.6f;
        if (sdf_green(bx, by) < -(BALL_RADIUS + 12) &&
            dist2d(bx, by, G->hole_x, G->hole_y) > slen * 0.75f) {
            G->ball_x = bx; G->ball_y = by;
            break;
        }
    }

    G->num_hazards = 0;
    float ang_tee = atan2f(G->ball_y - G->hole_y, G->ball_x - G->hole_x);
    switch (layout) {
    case LAYOUT_STRAIGHT:
        for (int side = -1; side <= 1; side += 2) {
            float a = ang_tee + side * (0.7f + randf() * 0.5f);
            place_bunker(G->hole_x + cosf(a) * 64.0f, G->hole_y + sinf(a) * 64.0f,
                         28.0f, 19.0f, 9.0f);
        }
        break;
    case LAYOUT_DOGLEG: {
        float mx = G->blob_x[1] * 0.5f + G->blob_x[3] * 0.5f;
        float my = G->blob_y[1] * 0.5f + G->blob_y[3] * 0.5f;
        place_bunker(mx - perp_x * bend * 0.9f, my - perp_y * bend * 0.9f,
                     34.0f, 21.0f, 10.0f);
        float a = ang_tee + mirror * (0.8f + randf() * 0.4f);
        place_bunker(G->hole_x + cosf(a) * 62.0f, G->hole_y + sinf(a) * 62.0f,
                     26.0f, 18.0f, 8.0f);
        break;
    }
    case LAYOUT_SHORT:
        for (int k = -1; k <= 1; k++) {
            float a = ang_tee + k * (0.85f + randf() * 0.2f);
            place_bunker(G->hole_x + cosf(a) * 68.0f, G->hole_y + sinf(a) * 68.0f,
                         26.0f, 18.0f, 9.0f);
        }
        break;
    case LAYOUT_WATER: {
        float wt = 0.45f + randf() * 0.15f;
        float omt = 1.0f - wt;
        float sx = omt * omt * tee_x + 2 * omt * wt * ctl_x + wt * wt * pin_x;
        float sy = omt * omt * tee_y + 2 * omt * wt * ctl_y + wt * wt * pin_y;
        place_pond(sx, sy, perp_x, perp_y, base_r, 62.0f, 20.0f);
        float a = ang_tee + (float)M_PI + mirror * (0.7f + randf() * 0.5f);
        place_bunker(G->hole_x + cosf(a) * 60.0f, G->hole_y + sinf(a) * 60.0f,
                     24.0f, 16.0f, 7.0f);
        break;
    }
    default:
        for (int side = -1; side <= 1; side += 2) {
            float a = ang_tee + side * (0.7f + randf() * 0.5f);
            place_bunker(G->hole_x + cosf(a) * 64.0f, G->hole_y + sinf(a) * 64.0f,
                         28.0f, 18.0f, 10.0f);
        }
        break;
    }

    bool ribbon = randf() < 0.18f &&
                  place_ribbon_sand(tee_x, tee_y, ctl_x, ctl_y, pin_x, pin_y);
    int extras = ribbon ? 0
                        : (randf() < 0.80f ? 1 : 0) + (randf() < 0.50f ? 1 : 0) +
                          (randf() < 0.30f ? 1 : 0);
    for (int e = 0; e < extras; e++) {
        float t = 0.25f + randf() * 0.5f;
        float omt = 1.0f - t;
        float ax = omt * omt * tee_x + 2 * omt * t * ctl_x + t * t * pin_x;
        float ay = omt * omt * tee_y + 2 * omt * t * ctl_y + t * t * pin_y;
        ax += perp_x * (randf() - 0.5f) * base_r * 1.3f;
        ay += perp_y * (randf() - 0.5f) * base_r * 1.3f;
        place_bunker(ax, ay, 40.0f, 12.0f, 20.0f);
    }
    if ((layout == LAYOUT_STRAIGHT || layout == LAYOUT_DOGLEG ||
         layout == LAYOUT_SCURVE) && randf() < 0.20f) {
        float t = 0.30f + randf() * 0.40f;
        float omt = 1.0f - t;
        float sx = omt * omt * tee_x + 2 * omt * t * ctl_x + t * t * pin_x;
        float sy = omt * omt * tee_y + 2 * omt * t * ctl_y + t * t * pin_y;
        place_pond(sx, sy, perp_x, perp_y, base_r, 42.0f, 14.0f);
    }
    if (layout != LAYOUT_WATER && randf() < 0.10f)
        place_creek(tee_x, tee_y, ctl_x, ctl_y, pin_x, pin_y, base_r);

    float wang = randf() * 2.0f * (float)M_PI;
    G->wind_speed = clampf(0.05f + randf() * randf() + ramp * 0.30f * randf(), 0.0f, 1.0f);
    if (G->wind_speed < 0.06f) G->wind_speed = 0;
    G->wind_x = cosf(wang) * G->wind_speed * 0.22f;
    G->wind_y = sinf(wang) * G->wind_speed * 0.22f;

    float arc = 0, ax_ = G->spine_tee_x, ay_ = G->spine_tee_y;
    for (int i = 1; i <= 24; i++) {
        float t = i / 24.0f, omt = 1.0f - t;
        float sx = omt * omt * G->spine_tee_x + 2 * omt * t * G->spine_ctl_x +
                   t * t * G->spine_pin_x + perp_x * scurve_amp * sinf(t * 2.0f * (float)M_PI);
        float sy = omt * omt * G->spine_tee_y + 2 * omt * t * G->spine_ctl_y +
                   t * t * G->spine_pin_y + perp_y * scurve_amp * sinf(t * 2.0f * (float)M_PI);
        arc += dist2d(ax_, ay_, sx, sy);
        ax_ = sx; ay_ = sy;
    }
    G->par = 2;
    if (arc > 320.0f) G->par++;
    if (arc > 560.0f) G->par++;
    if (arc > 710.0f) G->par++;
    if (arc > 850.0f) G->par++;
    if (layout == LAYOUT_WATER && G->par < 3) G->par = 3;

    G->ball_vx = G->ball_vy = 0;
    G->stroke_count = 0;
    G->air_time = G->air_total = 0;
    G->shot_start_x = G->ball_x;
    G->shot_start_y = G->ball_y;
    G->notice_until_us = 0;
    G->drop_anim = 0;
    G->club = CLUB_DRIVER;
    G->conf_active = false;
    for (int i = 0; i < MAX_CONF; i++) G->conf_life[i] = 0;

    // hand the hole to the background pipeline (rough renders once/launch)
    G->bg_phase = G->rough_done ? BG_GRIDS : BG_ROUGH;
    G->bg_row = 0;
    G->next_ready = false;
}

// ---------------------------------------------------------------------------
// Camera + flow helpers
// ---------------------------------------------------------------------------

void course_path_point(float p, float *ox, float *oy)
{
    float t = 1.0f - p;
    float omt = 1.0f - t;
    float bx = omt * omt * G->spine_tee_x + 2 * omt * t * G->spine_ctl_x + t * t * G->spine_pin_x;
    float by = omt * omt * G->spine_tee_y + 2 * omt * t * G->spine_ctl_y + t * t * G->spine_pin_y;
    *ox = bx + (1.0f - p) * (G->hole_x - G->spine_pin_x) + p * (G->ball_x - G->spine_tee_x);
    *oy = by + (1.0f - p) * (G->hole_y - G->spine_pin_y) + p * (G->ball_y - G->spine_tee_y);
}

static void update_camera(void)
{
    if (G->st == GST_CARD || G->st == GST_TITLE || G->st == GST_OPTIONS)
        return;   // canvas is frozen anyway

    if (G->st == GST_INTRO) {
        if (G->intro_t <= INTRO_HOLD) {
            G->cam_cx = G->hole_x;
            G->cam_cy = G->hole_y;
        } else {
            float p = clampf((float)(G->intro_t - INTRO_HOLD) / G->intro_pan, 0.0f, 1.0f);
            p = p * p * p * (p * (p * 6.0f - 15.0f) + 10.0f);
            course_path_point(p, &G->cam_cx, &G->cam_cy);
        }
    } else if (G->st == GST_WIPE) {
        G->cam_cx = G->hole_x;
        G->cam_cy = G->hole_y;
        G->cam_zoom = 1.0f;
    } else if (G->st == GST_HOLE_IN) {
        G->cam_cx += (G->hole_x - G->cam_cx) * 0.12f;
        G->cam_cy += (G->hole_y - G->cam_cy) * 0.12f;
    } else {
        G->cam_cx += (G->ball_x - G->cam_cx) * 0.15f;
        G->cam_cy += (G->ball_y - G->cam_cy) * 0.15f;
    }

    float zt = 1.0f;
    if (G->st == GST_MOVING) zt = club_zoom[G->shot_club];
    else if (G->st == GST_READY || G->st == GST_AIMING || G->st == GST_ARMED ||
             G->st == GST_HOLE_IN) zt = club_zoom[G->club];
    G->cam_zoom += (zt - G->cam_zoom) * 0.18f;

    float half_w = SCREEN_W * 0.5f / G->cam_zoom;
    float half_h = SCREEN_H * 0.5f / G->cam_zoom;
    G->cam_cx = clampf(G->cam_cx, half_w, WORLD_W - half_w);
    G->cam_cy = clampf(G->cam_cy, half_h, WORLD_H - half_h);
    G->cam_x = G->cam_cx - half_w;
    G->cam_y = G->cam_cy - half_h;
}

static void start_intro(void)
{
    G->intro_t = 0;
    G->intro_to_pass = G->num_players > 1;
    G->intro_pan = (int)clampf(dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y) / 6.0f, 55, 95);
    G->cam_cx = G->hole_x;
    G->cam_cy = G->hole_y;
    G->st = GST_INTRO;
}

// ---------------------------------------------------------------------------
// Physics (verbatim)
// ---------------------------------------------------------------------------

static void set_notice(const char *msg)
{
    int i = 0;
    while (msg[i] && i < 39) { G->notice[i] = msg[i]; i++; }
    G->notice[i] = 0;
    G->notice_until_us = gos_time_us() + 1200000;
}

static void world_bounds(void)
{
    if (G->ball_x < 12) { G->ball_x = 12; G->ball_vx = fabsf(G->ball_vx) * 0.5f; }
    if (G->ball_x > WORLD_W - 12) { G->ball_x = WORLD_W - 12; G->ball_vx = -fabsf(G->ball_vx) * 0.5f; }
    if (G->ball_y < 12) { G->ball_y = 12; G->ball_vy = fabsf(G->ball_vy) * 0.5f; }
    if (G->ball_y > WORLD_H - 12) { G->ball_y = WORLD_H - 12; G->ball_vy = -fabsf(G->ball_vy) * 0.5f; }
}

static void splash_penalty(void)
{
    G->stroke_count++;
    golf_sfx(SFXG_SPLASH);
    set_notice("SPLASH!  +1 stroke");
    G->ball_x = G->shot_start_x;
    G->ball_y = G->shot_start_y;
    G->ball_vx = G->ball_vy = 0;
    G->air_time = 0;
    if (G->num_players > 1) {
        mp_after_rest();
    } else {
        auto_putter();
        G->st = GST_READY;
    }
}

static void update_ball_physics(void)
{
    if (G->air_time > 0) {
        float curl = club_curl[G->shot_club];
        if (curl != 0) {
            float ca = cosf(curl), sa = sinf(curl);
            float nvx = G->ball_vx * ca - G->ball_vy * sa;
            G->ball_vy = G->ball_vx * sa + G->ball_vy * ca;
            G->ball_vx = nvx;
        }
        G->ball_vx += G->wind_x;
        G->ball_vy += G->wind_y;
        G->ball_x += G->ball_vx;
        G->ball_y += G->ball_vy;
        world_bounds();
        G->air_time--;
        if (G->air_time == 0) {
            for (int h = 0; h < G->num_hazards; h++) {
                if (G->hazards[h].type == 1 && sdf_hazard(h, G->ball_x, G->ball_y) < -4.0f) {
                    splash_penalty();
                    return;
                }
            }
            G->ball_vx *= club_keep[G->shot_club];
            G->ball_vy *= club_keep[G->shot_club];
            golf_sfx(SFXG_LAND);
        }
        return;
    }

    G->ball_x += G->ball_vx;
    G->ball_y += G->ball_vy;
    world_bounds();

    bool in_sand = false;
    for (int h = 0; h < G->num_hazards; h++) {
        float d = sdf_hazard(h, G->ball_x, G->ball_y);
        if (G->hazards[h].type == 0) {
            if (d < -1.0f) in_sand = true;
        } else if (d < -4.0f) {
            splash_penalty();
            return;
        }
    }

    float fric = in_sand ? SAND_FRICTION
               : (sdf_green(G->ball_x, G->ball_y) < 0 ? FRICTION : ROUGH_FRICTION);
    G->ball_vx *= fric;
    G->ball_vy *= fric;

    float hd = dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y);
    float speed = sqrtf(G->ball_vx * G->ball_vx + G->ball_vy * G->ball_vy);
    if (hd > 0.5f && hd < HOLE_RADIUS + 4 && speed < 3.6f) {
        G->ball_vx += (G->hole_x - G->ball_x) / hd * 0.45f;
        G->ball_vy += (G->hole_y - G->ball_y) / hd * 0.45f;
    }
    if (speed < 1.2f && hd > HOLE_RADIUS + 4) {
        G->ball_vx *= 0.90f;
        G->ball_vy *= 0.90f;
    }
    if (speed < MIN_VELOCITY) {
        G->ball_vx = 0;
        G->ball_vy = 0;
    }
}

static bool check_hole_in(void)
{
    if (G->air_time > 0) return false;
    float dist = dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y);
    float speed = sqrtf(G->ball_vx * G->ball_vx + G->ball_vy * G->ball_vy);
    return (dist < HOLE_RADIUS - 2) && (speed < 5.5f);
}

float shot_travel_frac(float frac)
{
    float v = frac * MAX_POWER * club_mult[G->club];
    float vx = v, px = 0;
    int airT = (int)(v * club_airk[G->club]);
    for (int f = 0; f < 500; f++) {
        px += vx;
        if (airT > 0) {
            airT--;
            if (airT == 0) vx *= club_keep[G->club];
        } else {
            vx *= FRICTION;
            if (vx < 0.35f) break;
        }
    }
    return px;
}

float pin_power_frac(void)
{
    float pin_d = dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y);
    if (shot_travel_frac(1.0f) < pin_d) return -1.0f;
    float lo = 0.02f, hi = 1.0f;
    for (int it = 0; it < 12; it++) {
        float mid = (lo + hi) * 0.5f;
        if (shot_travel_frac(mid) < pin_d) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

static void fire_shot(float dirx, float diry, float power)
{
    G->shot_start_x = G->ball_x;
    G->shot_start_y = G->ball_y;
    float v = power * club_mult[G->club];
    bool sand = false;
    for (int h = 0; h < G->num_hazards; h++) {
        if (G->hazards[h].type == 0 && sdf_hazard(h, G->ball_x, G->ball_y) < -1.0f) {
            v *= club_sand_eff[G->club];
            sand = true;
            break;
        }
    }
    float wob = lie_wobble();
    if (wob > 0.0f) {
        float a = (randf() - 0.5f) * 0.16f * wob;
        float ca = cosf(a), sa = sinf(a);
        float ndx = dirx * ca - diry * sa;
        diry = dirx * sa + diry * ca;
        dirx = ndx;
        v *= 1.0f + (randf() - 0.5f) * 0.16f * wob;
    }
    G->ball_vx = dirx * v;
    G->ball_vy = diry * v;
    G->shot_club = G->club;
    G->air_total = (int)(v * club_airk[G->club]);
    G->air_time = G->air_total;
    G->stroke_count++;
    if (sand) golf_sfx(SFXG_SAND);
    else if (G->club == CLUB_PUTTER) golf_sfx(SFXG_PUTT);
    else if (G->club == CLUB_CHIPPER) golf_sfx(SFXG_CHIP);
    else golf_sfx(SFXG_DRIVE);
    G->st = GST_MOVING;
}

// ---------------------------------------------------------------------------
// Swing mode: the standalone's detector fed by the OS accel stream
// ---------------------------------------------------------------------------

static void arm_swing(void)
{
    golf_sfx(SFXG_ARM);
    G->swing_phase = SWING_SETTLE;
    G->stroke_v = 0;
    G->back_w = 0;
    G->last_us = 0;
    G->swing_frac = 0;
    G->swing_fired = false;
    G->swing_bar_fill = 0;
    G->swing_bar_frames = 0;
    G->swing_tick = pin_power_frac();
    gos_accel_sample_t drain[16];
    while (gos_imu_accel_read(drain, 16) > 0) {}
    G->swing_listen = true;
    G->st = GST_ARMED;
}

static void swing_step(const gos_accel_sample_t *s)
{
    int64_t now = s->t_us;
    float ax = s->ax, ay = s->ay, az = s->az;
    if (G->last_us == 0) {
        G->last_us = now;
        G->phase_start_us = now;
        G->grav_x = ax; G->grav_y = ay; G->grav_z = az;
        return;
    }
    float dt = (now - G->last_us) * 1e-6f;
    G->last_us = now;
    if (dt <= 0 || dt > 0.1f) return;

    if (G->swing_phase == SWING_SETTLE) {
        G->grav_x += (ax - G->grav_x) * 0.3f;
        G->grav_y += (ay - G->grav_y) * 0.3f;
        G->grav_z += (az - G->grav_z) * 0.3f;
        if (now - G->phase_start_us > SWING_SETTLE_US) G->swing_phase = SWING_WAIT;
        return;
    }

    float dx = ax - G->grav_x, dy = ay - G->grav_y, dz = az - G->grav_z;
    float mag = sqrtf(dx * dx + dy * dy + dz * dz);

    switch (G->swing_phase) {
    case SWING_WAIT: {
        float a = dt;
        if (a > 0.2f) a = 0.2f;
        G->grav_x += (ax - G->grav_x) * a;
        G->grav_y += (ay - G->grav_y) * a;
        G->grav_z += (az - G->grav_z) * a;
        if (mag > SWING_BACK_TH) {
            G->back_x = dx; G->back_y = dy; G->back_z = dz;
            G->back_w = mag;
            G->swing_phase = SWING_BACK;
            G->phase_start_us = now;
        }
        break;
    }
    case SWING_BACK: {
        float bn = sqrtf(G->back_x * G->back_x + G->back_y * G->back_y +
                         G->back_z * G->back_z);
        float ux = G->back_x / bn, uy = G->back_y / bn, uz = G->back_z / bn;
        float along = dx * ux + dy * uy + dz * uz;
        if (along > 0.1f) {
            G->back_x += dx * mag; G->back_y += dy * mag; G->back_z += dz * mag;
            G->back_w += mag;
        }
        if (along < -SWING_FWD_TH) {
            G->swing_phase = SWING_STROKE;
            G->phase_start_us = now;
            G->quiet_us = 0;
            G->stroke_v = -along * dt;
        } else if (now - G->phase_start_us > SWING_BACK_TIMEOUT_US) {
            G->swing_phase = SWING_WAIT;
        }
        break;
    }
    case SWING_STROKE: {
        float bn = sqrtf(G->back_x * G->back_x + G->back_y * G->back_y +
                         G->back_z * G->back_z);
        float along = -(dx * G->back_x + dy * G->back_y + dz * G->back_z) / bn;
        if (along > 0) G->stroke_v += along * dt;
        G->swing_frac = clampf((G->stroke_v - SWING_V_MIN) /
                               (SWING_V_MAX - SWING_V_MIN), 0.0f, 1.0f);
        bool done = false;
        if (mag < SWING_END_TH * 2 && fabsf(along) < SWING_END_TH) {
            G->quiet_us += (int64_t)(dt * 1e6f);
            if (G->quiet_us > SWING_END_QUIET_US) done = true;
        } else {
            G->quiet_us = 0;
        }
        if (now - G->phase_start_us > SWING_MAX_STROKE_US) done = true;
        if (G->stroke_v >= SWING_V_MAX) done = true;
        if (done) {
            G->swing_listen = false;
            G->swing_fired = true;
        }
        break;
    }
    default:
        break;
    }
}

static void swing_poll(void)
{
    if (!G->swing_listen) return;
    gos_accel_sample_t buf[16];
    int n;
    while (G->swing_listen && (n = gos_imu_accel_read(buf, 16)) > 0)
        for (int i = 0; i < n && G->swing_listen; i++)
            swing_step(&buf[i]);
}

// ---------------------------------------------------------------------------
// Persistence: leaderboard, last initials, swing toggle
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t version, count;
    char last_ini[3];
    uint8_t swing;
    board_entry_t board[BOARD_SIZE];
} golf_save_t;

static void save_board(gos_ctx_t *ctx)
{
    golf_save_t s = { .version = 2, .count = (uint8_t)G->board_count,
                      .swing = G->swing_mode ? 1 : 0 };
    memcpy(s.last_ini, G->entry_ini, 3);
    memcpy(s.board, G->board, sizeof s.board);
    gos_save_write(ctx, &s, sizeof s);
}

static void load_board(gos_ctx_t *ctx)
{
    golf_save_t s;
    int n = gos_save_read(ctx, &s, sizeof s);
    G->swing_mode = true;   // the preferred mode, default ON
    if (n == (int)sizeof s && s.version == 2) {
        G->board_count = s.count > BOARD_SIZE ? BOARD_SIZE : s.count;
        memcpy(G->entry_ini, s.last_ini, 3);
        memcpy(G->board, s.board, sizeof G->board);
        G->swing_mode = s.swing != 0;
    } else {
        G->entry_ini[0] = 'A'; G->entry_ini[1] = 'A'; G->entry_ini[2] = 'A';
    }
    for (int i = 0; i < 3; i++)
        if (G->entry_ini[i] < 'A' || G->entry_ini[i] > 'Z') G->entry_ini[i] = 'A';
}

// ---------------------------------------------------------------------------
// Round flow (verbatim structure)
// ---------------------------------------------------------------------------

static void enter_scorecard(void)
{
    G->st = GST_CARD;
    G->card_drawn = false;
    G->next_ready = false;
    G->card_frames = 0;
}

static void end_of_hole(void)
{
    if (G->holes_completed >= ROUND_HOLES) {
        golf_sfx(SFXG_ROUND);
        G->st = GST_ROUND_END;
        show_round_end_card();
    } else {
        enter_scorecard();
    }
}

static void after_hole_in(void)
{
    if (G->num_players <= 1) {
        end_of_hole();
        return;
    }
    save_active_player();
    int nxt = next_unholed(G->cur_player);
    if (nxt >= 0) {
        show_pass_screen(nxt);
        return;
    }
    for (int i = 0; i < G->num_players; i++)
        G->players[i].round_strokes += G->players[i].strokes;
    G->holes_completed++;
    end_of_hole();
}

static void reset_round_counters(void)
{
    G->current_hole = 0;
    G->round_strokes = G->round_par = G->holes_completed = 0;
    G->stat_aces = G->stat_under = G->stat_pars = G->stat_over = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) G->players[i].round_strokes = 0;
}

// fresh course; the LOADING screen (or a held card) covers the rebuild
static void deal_fresh_course(bool to_tee)
{
    reset_round_counters();
    G->run_seed = gos_rand(G->hwrng);
    G->prev_layout = LAYOUT_COUNT;
    generate_hole(0);
    G->pending_round = to_tee;
    G->st = GST_LOADING;
    draw_loading();
}

static void mp_finish_round(void)
{
    G->num_players = 1;
    deal_fresh_course(false);
}

static void board_insert(gos_ctx_t *ctx)
{
    board_entry_t e;
    memcpy(e.ini, G->entry_ini, 3);
    e.ini[3] = 0;
    e.diff = (int16_t)(G->round_strokes - G->round_par);
    e.strokes = (uint16_t)G->round_strokes;
    int pos = G->board_count;
    for (int i = 0; i < G->board_count; i++)
        if (e.diff < G->board[i].diff) { pos = i; break; }
    if (pos < BOARD_SIZE) {
        int end = G->board_count < BOARD_SIZE ? G->board_count : BOARD_SIZE - 1;
        for (int i = end; i > pos; i--) G->board[i] = G->board[i - 1];
        G->board[pos] = e;
        if (G->board_count < BOARD_SIZE) G->board_count++;
    }
    save_board(ctx);
}

// ---------------------------------------------------------------------------
// 20 ms logic tick — mirrors the standalone's main-loop switch
// ---------------------------------------------------------------------------

static void logic_tick(void)
{
    if (G->bg_phase != BG_DONE && G->bg_phase != BG_IDLE) {
        golf_bg_tick();
        if (G->bg_phase == BG_DONE && !G->next_ready) {
            G->next_ready = true;
            if (G->st == GST_CARD) card_tap_hint();
        }
    }

    switch (G->st) {
    case GST_LOADING:
        if (G->next_ready) {
            if (G->pending_round) {
                G->pending_round = false;
                mp_reset_hole();
                start_intro();
            } else {
                golf_sfx(SFXG_TITLE);
                G->title_phase = 0;
                G->title_t = 20;   // no fade-in on first entry
                G->st = GST_TITLE;
            }
        }
        break;
    case GST_TITLE:
        G->title_t++;
        G->title_phase += 0.0045f;
        if (G->attract_rebuild) {
            if (G->next_ready) {
                G->attract_rebuild = false;
                G->title_phase = 0;
                G->title_t = 0;   // fade back up
            }
        } else if (G->title_t == 720) {
            // dip to dark, then deal a fresh preview hole behind the dip
            dim_canvas(0.30f);
            G->run_seed = gos_rand(G->hwrng);
            G->prev_layout = LAYOUT_COUNT;
            generate_hole(0);
            G->attract_rebuild = true;
        }
        break;
    case GST_INTRO:
        G->intro_t++;
        if (G->intro_t > INTRO_HOLD + G->intro_pan + 2) {
            if (G->intro_to_pass) {
                G->players[0].seen_intro = true;
                show_pass_screen(0);
            } else {
                auto_putter();
                G->st = GST_READY;
            }
        }
        break;
    case GST_READY:
        G->idle_frames++;
        G->hole_scored = false;
        break;
    case GST_ARMED:
        swing_poll();
        G->swing_bar_fill = G->swing_frac > 0
            ? (SWING_MIN_POWER + G->swing_frac * (MAX_POWER - SWING_MIN_POWER)) / MAX_POWER
            : 0;
        if (G->swing_fired) {
            G->swing_fired = false;
            float power = SWING_MIN_POWER + G->swing_frac * (MAX_POWER - SWING_MIN_POWER);
            G->swing_bar_fill = power / MAX_POWER;
            G->swing_bar_frames = 150;
            fire_shot(G->aim_dx, G->aim_dy, power);
        }
        break;
    case GST_MOVING:
        update_ball_physics();
        if (G->st != GST_MOVING) break;
        if (check_hole_in()) {
            G->drop_from_x = G->ball_x;
            G->drop_from_y = G->ball_y;
            G->drop_anim = DROP_ANIM_FRAMES;
            G->holein_us = gos_time_us();
            G->hole_scored = false;
            golf_sfx(SFXG_CUP);
            G->st = GST_HOLE_IN;
        } else if (G->air_time == 0 && G->ball_vx == 0 && G->ball_vy == 0) {
            if (G->num_players > 1) {
                mp_after_rest();
            } else {
                auto_putter();
                G->st = GST_READY;
            }
        }
        break;
    case GST_HOLE_IN:
        if (G->drop_anim > 0) {
            G->drop_anim--;
            if (G->drop_anim == DROP_ANIM_FRAMES / 2) {
                if (G->stroke_count == 1) {
                    spawn_confetti();
                    golf_sfx(SFXG_ACE);
                } else if (G->stroke_count < G->par) golf_sfx(SFXG_BIRDIE);
                else if (G->stroke_count == G->par) golf_sfx(SFXG_PAR);
                else golf_sfx(SFXG_BOGEY);
            }
        }
        update_confetti();
        if (!G->hole_scored) {
            if (G->num_players > 1) {
                G->players[G->cur_player].holed = true;
            } else {
                G->round_strokes += G->stroke_count;
                G->round_par += G->par;
                G->holes_completed++;
                if (G->stroke_count == 1) G->stat_aces++;
                else if (G->stroke_count < G->par) G->stat_under++;
                else if (G->stroke_count == G->par) G->stat_pars++;
                else G->stat_over++;
            }
            G->hole_scored = true;
        }
        {
            int64_t held_ms = (gos_time_us() - G->holein_us) / 1000;
            if (G->drop_anim == 0 && held_ms > (G->stroke_count == 1 ? 5000 : 1400))
                after_hole_in();
        }
        break;
    case GST_CARD:
        if (!G->card_drawn) {
            show_scorecard_card();
            G->card_drawn = true;
        } else if (!G->next_ready && ++G->card_frames == 3) {
            // card is up: build the next hole behind it
            G->current_hole++;
            generate_hole(G->current_hole);
            mp_reset_hole();
        }
        break;
    case GST_WIPE:
        G->wipe_x += 20;
        if (G->wipe_x >= SCREEN_W + 4) start_intro();
        break;
    default:
        break;
    }

    if (G->swing_bar_frames > 0) G->swing_bar_frames--;
    if (G->st != GST_READY) G->idle_frames = 0;
}

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

static void golf_init(gos_ctx_t *ctx)
{
    G = ctx->state;
    G->hwrng = &ctx->rng;
    CV = gos_gfx_fb565();
    memset(CV, 0, SCREEN_W * SCREEN_H * 2);
    load_board(ctx);

    for (int i = 0; i < WORLD_W + WORLD_H; i++)
        G->band_lut[i] = smoothstepf(-0.45f, 0.45f, sinf((float)i * 0.19f));

    gos_accel_sample_t probe[1];
    G->imu_ok = gos_imu_accel_read(probe, 1) >= 0;

    G->num_players = 1;
    G->release_age = 1000;
    G->swing_tick = -1;
    G->cam_zoom = 1.0f;
    G->prev_layout = LAYOUT_COUNT;
    G->run_seed = gos_rand(&ctx->rng);
    G->current_hole = 0;
    generate_hole(0);
    G->st = GST_LOADING;
    draw_loading();
}

static void golf_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    // gos touch arrives in half-res render coords; the whole game speaks
    // the original full-res panel coords
    float tx = in->touch.x * 2.0f, ty = in->touch.y * 2.0f;
    bool tap = in->touching && !G->touch_was;
    bool release = !in->touching && G->touch_was;
    G->touch_was = in->touching;
    if (!in->touching && G->release_age < 1000) G->release_age++;
    if (tap) G->idle_frames = 0;

    G->acc += dt;
    while (G->acc >= TICK_S) {
        G->acc -= TICK_S;
        // snapshot the moving pieces so the renderer can lerp between ticks
        G->prev_ball_x = G->ball_x;
        G->prev_ball_y = G->ball_y;
        G->prev_cam_x = G->cam_x;
        G->prev_cam_y = G->cam_y;
        G->prev_cam_zoom = G->cam_zoom;
        logic_tick();
        update_camera();   // camera eases per tick, like the original loop
    }
    G->render_alpha = clampf(G->acc / TICK_S, 0.0f, 1.0f);

    switch (G->st) {
    case GST_TITLE:
        if (tap && ty < 70 && tx < 110) { gos_request_exit(); break; }
        if (tap && G->board_count > 0 && tx >= 100 && tx <= 268 &&
            ty >= 224 && ty <= 282) {
            G->title_hold_us = gos_time_us();
        } else if (tap && ty >= 384) {
            golf_sfx(SFXG_TAP);
            G->options_from_title = true;
            G->st = GST_OPTIONS;
            draw_options_panel();
        } else if (tap && ty >= 280) {
            golf_sfx(SFXG_TAP);
            if (tx < 184) {
                G->num_players = 1;
                reset_round_counters();
                start_intro();
            } else {
                G->st = GST_PLAYER_COUNT;
                show_player_count_card();
            }
        }
        if (in->touching && G->title_hold_us > 0) {
            if (tx < 92 || tx > 276 || ty < 216 || ty > 296) {
                G->title_hold_us = 0;
            } else if (gos_time_us() - G->title_hold_us > TITLE_HOLD_US) {
                G->title_hold_us = 0;
                G->st = GST_CLEAR_CONFIRM;
                show_clear_confirm_card();
            }
        }
        if (release) G->title_hold_us = 0;
        break;
    case GST_PLAYER_COUNT:
        if (!tap) break;
        if (tx < 34 || tx > 334 || ty < 100 || ty > 396) {
            G->st = GST_TITLE;
            G->title_t = 20;
        } else if (ty >= 147) {
            golf_sfx(SFXG_TAP);
            if (ty < 229) G->num_players = 2;
            else if (ty < 295) G->num_players = 3;
            else G->num_players = 4;
            for (int i = 0; i < MAX_PLAYERS; i++)
                G->players[i].color = (uint8_t)i;
            reset_round_counters();
            mp_reset_hole();
            start_intro();
        }
        break;
    case GST_PASS:
        if (!tap) break;
        if (G->current_hole == 0 && tx >= 148 && tx <= 220 && ty >= 154 && ty <= 226) {
            cycle_player_color(G->cur_player);
            golf_sfx(SFXG_POP);
            draw_pass_card(G->cur_player);   // redraw in place, never re-dim
        } else {
            G->notice_until_us = 0;
            if (!G->players[G->cur_player].seen_intro) {
                G->players[G->cur_player].seen_intro = true;
                start_intro();
                G->intro_to_pass = false;
            } else {
                auto_putter();
                G->st = GST_READY;
            }
        }
        break;
    case GST_OPTIONS:
        if (!tap) break;
        if (ty >= OPT_SWING_Y0 && ty <= OPT_SWING_Y1) {
            G->swing_mode = !G->swing_mode;
            save_board(ctx);
            golf_sfx(SFXG_TAP);
            draw_options_panel();            // relabel in place
        } else if (ty >= OPT_NEWRND_Y0 && ty <= OPT_NEWRND_Y1) {
            golf_sfx(SFXG_TAP);
            deal_fresh_course(true);
        } else {
            golf_sfx(SFXG_TAP);
            if (G->options_from_title) {
                G->st = GST_TITLE;
                G->title_t = 20;
            } else {
                G->st = GST_READY;
            }
        }
        break;
    case GST_CLEAR_CONFIRM:
        if (!tap) break;
        if (ty >= CLR_CLEAR_Y0 && ty <= CLR_CLEAR_Y1) {
            G->board_count = 0;
            save_board(ctx);
        }
        G->st = GST_TITLE;
        G->title_t = 20;
        break;
    case GST_INTRO:
        if (tap) G->intro_t = INTRO_HOLD + G->intro_pan + 10;
        break;
    case GST_READY:
        if (!tap) break;
        if (club_button_hit(tx, ty)) {
            G->club = (club_t)((G->club + CLUB_COUNT - 1) % CLUB_COUNT);
            golf_sfx(SFXG_CLUB);
            break;
        }
        {
            bool continuation = G->release_age < 14 &&
                                dist2d(tx, ty, G->drag_start_x, G->drag_start_y) < 22.0f;
            if (!continuation) {
                G->hold_frames = 0;
                G->drag_start_x = tx;
                G->drag_start_y = ty;
            }
            G->is_dragging = true;
            G->drag_cur_x = tx;
            G->drag_cur_y = ty;
            G->notice_until_us = 0;
            G->st = GST_AIMING;
        }
        break;
    case GST_AIMING:
        if (in->touching) {
            G->drag_cur_x = tx;
            G->drag_cur_y = ty;
            if (dist2d(G->drag_start_x, G->drag_start_y, tx, ty) < 16.0f) {
                if (++G->hold_frames > 45) {
                    G->is_dragging = false;
                    G->options_from_title = false;
                    G->st = GST_OPTIONS;
                    draw_options_panel();
                }
            } else {
                G->hold_frames = 0;
            }
        }
        if (release) {
            G->is_dragging = false;
            G->release_age = 0;
            float dx = G->drag_start_x - G->drag_cur_x;
            float dy = G->drag_start_y - G->drag_cur_y;
            float len = sqrtf(dx * dx + dy * dy);
            if (G->swing_mode && G->imu_ok) {
                if (len > 20.0f) {
                    G->aim_dx = dx / len;
                    G->aim_dy = dy / len;
                    arm_swing();
                } else {
                    G->st = GST_READY;
                }
            } else {
                float power = len * DRAG_SCALE;
                if (power > MAX_POWER) power = MAX_POWER;
                if (power > 0.5f) fire_shot(dx / len, dy / len, power);
                else G->st = GST_READY;
            }
        }
        break;
    case GST_ARMED:
        if (tap) {
            G->swing_listen = false;
            G->st = GST_READY;
        }
        break;
    case GST_HOLE_IN:
        if (tap && G->drop_anim == 0) after_hole_in();
        break;
    case GST_CARD:
        if (tap && G->next_ready) {
            G->wipe_x = 0;
            golf_sfx(SFXG_WIPE);
            G->st = GST_WIPE;
        }
        break;
    case GST_WIPE:
        if (tap) G->wipe_x = SCREEN_W;
        break;
    case GST_ROUND_END:
        if (!tap) break;
        if (G->num_players > 1) mp_finish_round();
        else {
            G->st = GST_INITIALS;
            enter_initials_card();
        }
        break;
    case GST_INITIALS:
        if (!tap) break;
        if (ty >= INI_OK_Y) {
            golf_sfx(SFXG_TAP);
            board_insert(ctx);
            deal_fresh_course(false);
        } else {
            for (int i = 0; i < 3; i++) {
                if (tx < INI_COL_X(i) - 35 || tx > INI_COL_X(i) + 35) continue;
                if (ty < INI_TOP_Y || ty > INI_BOT_Y) break;
                int step = ty < INI_MID_Y ? 1 : 25;
                G->entry_ini[i] = (char)('A' + (G->entry_ini[i] - 'A' + step) % 26);
                golf_sfx(SFXG_TICK);
                draw_initials_screen();
                break;
            }
        }
        break;
    default:
        break;
    }
}

static void golf_render(gos_ctx_t *ctx)
{
    (void)ctx;
    switch (G->st) {
    case GST_LOADING:
        break;   // draw_loading painted once; the canvas holds
    case GST_TITLE:
        if (!G->attract_rebuild) title_frame();
        break;   // during the rebuild the dimmed frame holds (the dip)
    case GST_CARD:
    case GST_ROUND_END:
    case GST_INITIALS:
    case GST_OPTIONS:
    case GST_CLEAR_CONFIRM:
    case GST_PLAYER_COUNT:
    case GST_PASS:
        break;   // canvas-holding screens: drawn once on entry
    default:
        render_frame();
        break;
    }
}

static void golf_suspend(gos_ctx_t *ctx) { save_board(ctx); }
static void golf_resume(gos_ctx_t *ctx) { (void)ctx; }
static void golf_teardown(gos_ctx_t *ctx)
{
    save_board(ctx);
    gos_audio_stop_all();
}

#ifdef GOS_HOST_SIM
void golf_sim_dbg(int *st, int *hole, int *completed, int *strokes,
                  float *bx, float *by, float *hx, float *hy, int *par)
{
    *st = (int)G->st;
    *hole = G->current_hole;
    *completed = G->holes_completed;
    *strokes = G->stroke_count;
    *bx = G->ball_x; *by = G->ball_y;
    *hx = G->hole_x; *hy = G->hole_y;
    *par = G->par;
}
#endif

const gos_game_t game_golf = {
    .id = "golf",
    .title = "GOLF",
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_SAVE | GOS_CAP_FB565,
    .state_size = sizeof(golf_t),
    .init = golf_init,
    .update = golf_update,
    .render = golf_render,
    .suspend = golf_suspend,
    .resume = golf_resume,
    .teardown = golf_teardown,
};
