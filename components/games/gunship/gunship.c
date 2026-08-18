// gunship.c — Zombie Gunship. AC-130 thermal gunner over a dead town.
//
// Written entirely against gos.h — no HAL, no ESP headers. The whole world is
// filled shapes drawn through a 16-entry white-hot thermal palette; no sprite
// assets, no PCM assets (all audio is synth programs).
//
// Coordinates: world is a 1200x1200 top-down plane, mission center (600,600).
// The camera is an instrument, not a game camera: it orbits (view rotation
// theta at ~1.5 deg/s), drifts, and lags the true transform by ~80 ms.
// The reticle is aim-driven within the fixed view; rounds have time of
// flight and resolve at a *world* point captured at trigger time, so the
// orbit drift is what makes leading the target a skill.

#include "gos.h"
#include <math.h>
#include <string.h>

#define W GOS_SCREEN_W
#define H GOS_SCREEN_H
#define CX 600.0f
#define CY 600.0f

// Optics run below 1:1 so the sensor shows a broad slice of town — at 1:1
// the 184x224 view was a keyhole: survivors walked their whole route out of
// frame and died unseen. Live rendering uses g->zoom (eases to the selected
// weapon's optic); the spawn ring is computed from it too, see setup notes
// at the spawn site.
// Standoff foreshortening: the sensor looks IN at the town from an orbiting
// aircraft miles out, not straight down from a helicopter — the radial axis
// (screen vertical; the squash axis is screen-fixed as the world rotates,
// like a side-looking gun camera) compresses by this factor.
#define SLANT 0.80f

#define NZ   160
#define NS   12
#define NSH  40
#define NSC  64
#define NFL  16
#define NPING 8
#define NCLUSTER 6

// palette indices (see spec §3)
#define P_GROUND  2
#define P_ROAD    3
#define P_BLDG    1
#define P_WARM    5
#define P_ZOM     8
#define P_ZHOT    9
#define P_SURV    11
#define P_HOT     13
#define P_WHITE   15

enum { GS_BRIEF, GS_PLAY, GS_BREAK, GS_WIN, GS_LOSE };
enum { WPN_GAT, WPN_BOF, WPN_HOW };
enum { ZT_WALKER, ZT_RUNNER, ZT_BRUTE };
enum { SV_NONE, SV_MOVE, SV_COWER, SV_SAFE, SV_DEAD };
enum { UP_G_HEAT, UP_G_SPREAD, UP_B_RELOAD, UP_B_BLAST,
       UP_H_RELOAD, UP_H_BLAST, UP_SENSOR, UP_ORBIT, UP_COUNT };

typedef struct {
    uint8_t mode;
    float mode_t;
    int wave;                 // 0-based
    int mission;

    // camera: true + lagged transform, recoil offset
    float theta, ltheta;
    float camx, camy, lcamx, lcamy;
    float recx, recy;
    float tc, ts;             // cos/sin of lagged theta (this frame)
    float zoom;               // current view scale, eases to the weapon's optic

    // zombies (SoA)
    float zx[NZ], zy[NZ], zvx[NZ], zvy[NZ];
    int16_t zhp[NZ];
    uint8_t ztype[NZ], zseed[NZ];
    int nz_alive;

    // survivors
    float sx[NS], sy[NS];
    uint8_t sstate[NS];
    float scower[NS];

    // last rendered screen positions (thermal motion smear; includes the
    // camera's apparent motion). Sentinel < -9000 = no trail this frame.
    // The smear direction is a low-passed per-frame delta: building_repel
    // makes a wall-pinned entity oscillate ~1px every frame, and raw deltas
    // times the ghost offsets strobed the trail side to side (user-reported).
    float zpsx[NZ], zpsy[NZ], zvfx[NZ], zvfy[NZ];
    float spsx[NS], spsy[NS], svfx[NS], svfy[NS];

    // committed wall-circulation direction per entity (building_slide)
    int8_t zrot[NZ], srot[NS];

    // shells in flight (resolve at world target on expiry)
    float shx[NSH], shy[NSH], sht[NSH], shtof[NSH];
    uint8_t shw[NSH];
    bool sha[NSH];

    // scorch marks (thermal persistence, ~8 s cool-down)
    float scox[NSC], scoy[NSC], scoh[NSC];
    uint8_t scor[NSC];
    int sco_next;

    // muzzle/impact flashes
    float flx[NFL], fly[NFL], flt[NFL];
    uint8_t flr[NFL];

    // spawn telegraph pings (bearing ticks on the screen edge)
    uint8_t ping_a[NPING];
    float ping_t[NPING];

    // wave spawn clusters
    struct {
        uint8_t type;
        uint8_t bearing;      // angle256 from center
        int remaining;
        float timer, interval, telegraph;
    } cl[NCLUSTER];
    int ncl;
    int surv_pending;
    float surv_timer;

    // weapons
    int weapon;
    float heat;
    bool overheated;
    float cool[3];            // bofors/howitzer reload timers (index by weapon)
    float fire_acc;
    float retx, rety;         // reticle screen pos after traverse limiting
    float retvx, retvy;       // reticle velocity px/s (slew inertia)

    // input
    uint8_t aim_mode;         // gos_input_get_aim_mode(), cached per update
    bool ctl_press;           // current contact started on a HUD control
    bool fire_latch;          // touch mode: held FIRE button (sticky to release)
    bool trig_was;            // local trigger edge (dry-click)
    float twx, twy;           // touch mode: drag aim target, screen px

    // stats (run)
    int saved, lost, kills;
    int32_t salvage_run;
    float civ_flash;

    // meta (persisted)
    int32_t salvage;
    uint8_t up[UP_COUNT];
    uint8_t best_saved;
    uint8_t missions_won;

    // audio
    int h_engine, h_gat, h_servo;
    int servo_vol;            // last volume sent to the servo voice
    bool gat_on;
    bool touch_was;           // local touch edge (FIRE only covers lower 2/3)
    gos_pt_t tlast;           // touch mode: previous drag point
    bool twas;                // touch mode: had contact last update

    uint32_t frame;
    gos_rng_t *rng;
} gs_t;

// ---------------------------------------------------------------------------
// Static world geometry (world coords, axis-aligned; rotated at draw time)
// ---------------------------------------------------------------------------

typedef struct { int16_t x, y, w, h; } wrect_t;

static const wrect_t roads[] = {
    { 588, 380, 26, 440 },     // main street N-S
    { 380, 588, 440, 26 },     // cross street E-W
    { 380, 700, 246, 20 },     // south lane to the courtyard
};

// PATHING RULE: no two buildings may overlap, and every corridor between
// neighbors must be >= 24 px wide. The first layout had an overlapping pair
// and 14-20 px slots; entities wedged in the concave pockets where two
// walls' slide directions fight (user-reported stuck zombies, twice).
static const wrect_t bldgs[] = {
    { 460, 430, 90, 110 }, { 640, 430, 110, 70 }, { 660, 528, 70, 50 },
    { 430, 640, 90, 40 },  { 460, 740, 70, 60 },  { 650, 640, 100, 44 },
    { 700, 708, 60, 72 },  { 622, 764, 44, 40 },  { 398, 470, 36, 80 },
};

#define EXT_X 600.0f
#define EXT_Y 742.0f

// survivor spawn doorways — just OUTSIDE their building's wall margin (the
// old list had two points 5 px inside a building, spawning survivors into
// the slide-out shove)
static const gos_pt_t doors[] = {
    { 470, 545 }, { 655, 505 }, { 520, 545 }, { 420, 660 },
    { 700, 690 }, { 632, 466 },
};

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------

typedef struct {
    const char *chip;
    float rate;        // rounds/s (gatling) or 1/reload
    int dmg;
    float blast;       // px radius; 0 = point
    float tof;
    float spread;
    float traverse;    // deg/s of reticle travel
    float slew;        // px/s^2 the mount can accelerate the reticle
    float zoom;        // world->screen scale on this optic — the 25mm is the
                       // tight close-up, the 105 surveys the widest area
} wpn_t;

static const wpn_t wpns[3] = {
    // zoom ladder shifted one notch wider (user: 1.05 on the 25 hid the
    // survivor doors — "there's times where I can't see the door")
    { "25",  12.f,  20, 0.f,  0.15f, 4.f,  240.f, 5000.f, 0.85f },
    { "40",  1.5f,  90, 14.f, 0.5f,  1.5f, 140.f, 2200.f, 0.70f },
    { "105", 0.25f, 400, 40.f, 1.1f, 0.f,  60.f,  550.f,  0.60f },
};

// touch aim mode: finger px -> aim-target px
#define DRAG_GAIN 1.5f

// Control zones: weapon chips stacked TOP-RIGHT under the heat bar (user's
// pick after trying thumb height), plus — in touch aim mode only — the FIRE
// button in the bottom-right corner. Zones run to the right screen edge and
// sit well below the chip visuals: fingers land 15-25 px LOW on this panel.
// (No CAL chip: recentering lives in the pause overlay's CALIBRATE.)
static bool ctl_zone(gos_pt_t p, bool touch_mode)
{
    if (p.x >= 136 && p.y >= 20 && p.y < 140) return true;   // weapon stack
    return touch_mode && p.x >= 114 && p.y >= 190;           // FIRE button
}

// ---------------------------------------------------------------------------
// Audio — synth programs
// ---------------------------------------------------------------------------

#define SFX(name, ...) \
    static const gos_note_t name##_n[] = { __VA_ARGS__ }; \
    static const gos_sfx_t name = { name##_n, sizeof(name##_n) / sizeof(gos_note_t), 3 }

SFX(sfx_gat_loop, { 1600, 1100, 42, GOS_W_NOISE, 255 }, { 0, 0, 41, GOS_W_REST, 0 });
SFX(sfx_bofors,   { 380, 300, 35, GOS_W_NOISE, 255 }, { 95, 48, 140, GOS_W_TRI, 235 });
SFX(sfx_how_fire, { 240, 180, 45, GOS_W_NOISE, 255 }, { 60, 32, 420, GOS_W_TRI, 255 });
SFX(sfx_how_boom, { 120, 90, 90, GOS_W_NOISE, 255 }, { 46, 24, 620, GOS_W_TRI, 245 },
                  { 90, 40, 340, GOS_W_NOISE, 110 });
SFX(sfx_bof_boom, { 210, 140, 60, GOS_W_NOISE, 225 }, { 72, 42, 190, GOS_W_TRI, 205 });
SFX(sfx_saved,    { 880, 880, 60, GOS_W_SQ25, 170 }, { 1320, 1320, 95, GOS_W_SQ25, 170 });
SFX(sfx_lost,     { 660, 660, 50, GOS_W_SQ25, 190 }, { 620, 180, 130, GOS_W_SQ25, 170 },
                  { 900, 900, 210, GOS_W_NOISE, 95 });
SFX(sfx_ping,     { 1250, 1180, 95, GOS_W_TRI, 165 });
SFX(sfx_overheat, { 2800, 500, 320, GOS_W_NOISE, 135 });
SFX(sfx_click,    { 2200, 2200, 16, GOS_W_NOISE, 120 });
SFX(sfx_wave,     { 220, 220, 80, GOS_W_SQ50, 150 }, { 330, 330, 80, GOS_W_SQ50, 150 },
                  { 440, 440, 130, GOS_W_SQ50, 170 });
SFX(sfx_buy,      { 520, 780, 70, GOS_W_TRI, 190 });
// traverse servo: low throbbing whine, volume tracks slew speed (the synth
// has no live pitch — the f0->f1 glide inside the looped note is the motor)
SFX(sfx_servo,    { 58, 66, 130, GOS_W_TRI, 200 });
SFX(sfx_engine,   { 55, 55, 480, GOS_W_TRI, 80 }, { 52, 52, 480, GOS_W_TRI, 80 });

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t version;
    int32_t salvage;
    uint8_t up[UP_COUNT];
    uint8_t best_saved, missions_won;
} save_t;

static void save_meta(gs_t *g, gos_ctx_t *ctx)
{
    save_t s = { .version = 1, .salvage = g->salvage,
                 .best_saved = g->best_saved, .missions_won = g->missions_won };
    memcpy(s.up, g->up, UP_COUNT);
    gos_save_write(ctx, &s, sizeof s);
}

static void load_meta(gs_t *g, gos_ctx_t *ctx)
{
    save_t s;
    if (gos_save_read(ctx, &s, sizeof s) == (int)sizeof s && s.version == 1) {
        g->salvage = s.salvage < 0 ? 0 : s.salvage;   // heal old negative saves
        memcpy(g->up, s.up, UP_COUNT);
        g->best_saved = s.best_saved;
        g->missions_won = s.missions_won;
    }
}

// ---------------------------------------------------------------------------
// Camera transform
// ---------------------------------------------------------------------------

static void world_to_screen(const gs_t *g, float wx, float wy, float *sx, float *sy)
{
    float dx = wx - g->lcamx, dy = wy - g->lcamy;
    *sx = W / 2 + (dx * g->tc - dy * g->ts) * g->zoom + g->recx;
    *sy = H / 2 + (dx * g->ts + dy * g->tc) * g->zoom * SLANT + g->recy;
}

static void screen_to_world(const gs_t *g, float sx, float sy, float *wx, float *wy)
{
    float dx = (sx - W / 2 - g->recx) / g->zoom;
    float dy = (sy - H / 2 - g->recy) / (g->zoom * SLANT);
    *wx = g->lcamx + dx * g->tc + dy * g->ts;
    *wy = g->lcamy - dx * g->ts + dy * g->tc;
}

static void update_camera(gs_t *g, float dt)
{
    float rate = g->up[UP_ORBIT] ? 0.0175f : 0.0262f;   // rad/s (~1.0 / 1.5 deg)
    g->theta += rate * dt;

    // ease onto the selected weapon's optic (~0.25 s zoom transition)
    float zk = dt * 4.f;
    if (zk > 1.f) zk = 1.f;
    g->zoom += (wpns[g->weapon].zoom - g->zoom) * zk;

    // frame the action: the aim point pulls toward the live survivors —
    // they are what used to die off screen — clamped so the town core never
    // leaves the frame
    float ax = 0, ay = 0;
    int na = 0;
    for (int i = 0; i < NS; i++) {
        if (g->sstate[i] != SV_MOVE && g->sstate[i] != SV_COWER) continue;
        ax += g->sx[i];
        ay += g->sy[i];
        na++;
    }
    float tx = CX, ty = CY + 30.f;
    if (na) {
        float ox = ax / na - tx, oy = ay / na - ty;
        ox *= 0.7f;
        oy *= 0.7f;
        if (ox > 110.f) ox = 110.f;
        if (ox < -110.f) ox = -110.f;
        if (oy > 110.f) oy = 110.f;
        if (oy < -110.f) oy = -110.f;
        tx += ox;
        ty += oy;
    }
    tx += 20.f * sinf(g->theta * 2.3f);
    ty += 16.f * cosf(g->theta * 1.7f);
    // slow chase (~0.9 s) so spawns/extractions never jerk the frame
    float kc = dt / (0.9f + dt);
    g->camx += (tx - g->camx) * kc;
    g->camy += (ty - g->camy) * kc;
    // sensor lag ~80 ms
    float k = dt / (0.08f + dt);
    g->ltheta += (g->theta - g->ltheta) * k;
    g->lcamx += (g->camx - g->lcamx) * k;
    g->lcamy += (g->camy - g->lcamy) * k;
    g->recx *= 1.f - 6.f * dt;
    g->recy *= 1.f - 6.f * dt;
    g->tc = cosf(g->ltheta);
    g->ts = sinf(g->ltheta);
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

static void spawn_zombie(gs_t *g, uint8_t type, float x, float y)
{
    for (int i = 0; i < NZ; i++) {
        if (g->zhp[i] > 0) continue;
        static const int16_t hp[3] = { 60, 30, 350 };
        g->zx[i] = x;
        g->zy[i] = y;
        g->zvx[i] = g->zvy[i] = 0;
        g->zhp[i] = hp[type];
        g->ztype[i] = type;
        g->zseed[i] = (uint8_t)gos_rand(g->rng);
        g->zpsx[i] = g->zpsy[i] = -10000.f;   // no smear on the spawn frame
        g->zrot[i] = 0;
        g->nz_alive++;
        return;
    }
}

static void add_ping(gs_t *g, uint8_t bearing)
{
    for (int i = 0; i < NPING; i++) {
        if (g->ping_t[i] > 0) continue;
        g->ping_a[i] = bearing;
        g->ping_t[i] = 2.5f;
        return;
    }
}

static void setup_wave(gs_t *g)
{
    // composition table: walkers, runners, brutes, survivors
    static const uint8_t tab[10][4] = {
        { 8, 0, 0, 1 },  { 12, 0, 0, 1 }, { 14, 4, 0, 2 },  { 16, 6, 0, 2 },
        { 18, 6, 1, 2 }, { 20, 8, 1, 2 }, { 22, 10, 2, 2 }, { 24, 12, 2, 3 },
        { 26, 14, 3, 3 }, { 30, 16, 4, 3 },
    };
    const uint8_t *t = tab[g->wave];
    g->ncl = 0;
    int budget[3] = { t[0], t[1], t[2] };
    float telegraph = g->up[UP_SENSOR] ? 4.f : 2.f;
    for (int type = 0; type < 3; type++) {
        int left = budget[type];
        while (left > 0 && g->ncl < NCLUSTER) {
            int n = type == ZT_BRUTE ? left : (left + 1) / 2;
            if (n > 16) n = 16;
            uint8_t bearing = (uint8_t)gos_rand(g->rng);
            g->cl[g->ncl++] = (typeof(g->cl[0])){
                .type = (uint8_t)type, .bearing = bearing, .remaining = n,
                .timer = telegraph + gos_randf(g->rng) * 1.2f,
                .interval = type == ZT_RUNNER ? 0.15f : 0.22f,
                .telegraph = telegraph,
            };
            left -= n;
        }
    }
    g->surv_pending = t[3];
    g->surv_timer = 1.f;
    gos_audio_play(&sfx_wave, 210);
}

static void spawn_survivor(gs_t *g)
{
    for (int i = 0; i < NS; i++) {
        if (g->sstate[i] != SV_NONE && g->sstate[i] != SV_SAFE &&
            g->sstate[i] != SV_DEAD) continue;
        if (g->sstate[i] == SV_MOVE || g->sstate[i] == SV_COWER) continue;
        const gos_pt_t d = doors[gos_rand_range(g->rng, 0, 5)];
        g->sx[i] = d.x;
        g->sy[i] = d.y;
        g->sstate[i] = SV_MOVE;
        g->scower[i] = 0;
        g->spsx[i] = g->spsy[i] = -10000.f;   // no smear on the spawn frame
        g->srot[i] = 0;
        gos_audio_play(&sfx_saved, 120);   // radio: "moving out"
        return;
    }
}

// ---------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------

static void add_flash(gs_t *g, float x, float y, uint8_t r)
{
    for (int i = 0; i < NFL; i++) {
        if (g->flt[i] > 0) continue;
        g->flx[i] = x; g->fly[i] = y; g->flt[i] = 0.18f; g->flr[i] = r;
        return;
    }
}

static void add_scorch(gs_t *g, float x, float y, uint8_t r, float heat)
{
    int i = g->sco_next;
    g->sco_next = (g->sco_next + 1) % NSC;
    g->scox[i] = x; g->scoy[i] = y; g->scor[i] = r; g->scoh[i] = heat;
}

static void blast(gs_t *g, float wx, float wy, int weapon)
{
    const wpn_t *w = &wpns[weapon];
    float r = w->blast;
    if (weapon == WPN_BOF && g->up[UP_B_BLAST]) r *= 1.25f;
    if (weapon == WPN_HOW && g->up[UP_H_BLAST]) r *= 1.2f;
    // point weapons get a slightly generous hit radius: below-1:1 zoom means
    // a screen pixel of aim error is >1 world px, and the blobs read bigger than
    // their world footprint
    float query_r = r > 0 ? r : 6.f;

    uint16_t hits[64];
    int n = gos_grid_query((int16_t)wx, (int16_t)wy, (int16_t)query_r, hits, 64);
    for (int k = 0; k < n; k++) {
        int i = hits[k];
        if (i >= NZ || g->zhp[i] <= 0) continue;
        float dx = g->zx[i] - wx, dy = g->zy[i] - wy;
        float d = sqrtf(dx * dx + dy * dy);
        int dmg = w->dmg;
        if (r > 0) dmg = (int)(dmg * (1.f - 0.5f * d / (r + 1)));
        g->zhp[i] -= dmg;
        if (g->zhp[i] <= 0) {
            g->nz_alive--;
            g->kills++;
            // bounty by threat — the old flat +2/kill meant a whole wave paid
            // ~40 salvage against 300+ upgrade costs (user: "the money system
            // doesn't seem to work")
            static const int16_t bounty[3] = { 5, 8, 25 };
            g->salvage_run += bounty[g->ztype[i]];
            add_scorch(g, g->zx[i], g->zy[i], 3, 8.f);   // cooling body
        }
    }

    // friendly fire: survivors are 1 HP against any splash or near-direct hit
    float kill_r = r > 0 ? r * 0.9f : 3.f;
    for (int i = 0; i < NS; i++) {
        if (g->sstate[i] != SV_MOVE && g->sstate[i] != SV_COWER) continue;
        float dx = g->sx[i] - wx, dy = g->sy[i] - wy;
        if (dx * dx + dy * dy < kill_r * kill_r) {
            g->sstate[i] = SV_DEAD;
            g->lost++;
            g->salvage_run -= 150;
            g->civ_flash = 0.9f;
            gos_audio_play(&sfx_lost, 235);
            add_scorch(g, g->sx[i], g->sy[i], 3, 9.f);
        }
    }

    if (weapon != WPN_GAT) {
        add_flash(g, wx, wy, weapon == WPN_HOW ? 14 : 7);
        add_scorch(g, wx, wy, weapon == WPN_HOW ? 12 : 6, 12.f);
        gos_audio_play(weapon == WPN_HOW ? &sfx_how_boom : &sfx_bof_boom, 255);
        float kick = weapon == WPN_HOW ? 3.5f : 1.5f;
        g->recx += (gos_randf(g->rng) - 0.5f) * kick;
        g->recy += (gos_randf(g->rng) - 0.5f) * kick;
    } else {
        add_flash(g, wx, wy, 2);
    }
}

static void fire(gs_t *g, float dt)
{
    const wpn_t *w = &wpns[g->weapon];

    if (g->weapon == WPN_GAT) {
        float cap = g->up[UP_G_HEAT] ? 6.f : 4.f;   // seconds of sustained fire
        if (g->overheated) return;
        g->heat += dt / cap;
        if (g->heat >= 1.f) {
            g->overheated = true;
            g->gat_on = false;
            gos_audio_stop(g->h_gat);
            gos_audio_play(&sfx_overheat, 200);
            return;
        }
        if (!g->gat_on) {
            g->gat_on = true;
            g->h_gat = gos_audio_loop(&sfx_gat_loop, 235);
        }
        g->fire_acc += dt * w->rate;
        while (g->fire_acc >= 1.f) {
            g->fire_acc -= 1.f;
            float spread = w->spread * (g->up[UP_G_SPREAD] ? 0.6f : 1.f);
            float sx = g->retx + (gos_randf(g->rng) - 0.5f) * 2 * spread;
            float sy = g->rety + (gos_randf(g->rng) - 0.5f) * 2 * spread;
            float wx, wy;
            screen_to_world(g, sx, sy, &wx, &wy);
            for (int i = 0; i < NSH; i++) {
                if (g->sha[i]) continue;
                g->sha[i] = true;
                g->shx[i] = wx; g->shy[i] = wy;
                g->sht[i] = g->shtof[i] = w->tof;
                g->shw[i] = WPN_GAT;
                break;
            }
            g->recx += (gos_randf(g->rng) - 0.5f) * 0.5f;
        }
        return;
    }

    // bofors / howitzer: single shot when reloaded
    if (g->cool[g->weapon] > 0) return;
    float reload = 1.f / w->rate;
    if (g->weapon == WPN_BOF && g->up[UP_B_RELOAD]) reload *= 0.7f;
    if (g->weapon == WPN_HOW && g->up[UP_H_RELOAD]) reload *= 0.7f;
    g->cool[g->weapon] = reload;

    float sx = g->retx + (gos_randf(g->rng) - 0.5f) * 2 * w->spread;
    float sy = g->rety + (gos_randf(g->rng) - 0.5f) * 2 * w->spread;
    float wx, wy;
    screen_to_world(g, sx, sy, &wx, &wy);
    for (int i = 0; i < NSH; i++) {
        if (g->sha[i]) continue;
        g->sha[i] = true;
        g->shx[i] = wx; g->shy[i] = wy;
        g->sht[i] = g->shtof[i] = w->tof;
        g->shw[i] = (uint8_t)g->weapon;
        break;
    }
    gos_audio_play(g->weapon == WPN_HOW ? &sfx_how_fire : &sfx_bofors, 255);
    g->recx += (gos_randf(g->rng) - 0.5f) * (g->weapon == WPN_HOW ? 2.f : 0.8f);
    g->recy += g->weapon == WPN_HOW ? 1.2f : 0.4f;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

// Slide along building walls instead of grinding into them. The old repel
// nudge only cancelled the into-wall step, so an entity whose target sat on
// the far side of a building parked there forever (user-reported twice).
// The velocity loses its into-wall component and that blocked speed is
// redirected along the wall. `rot` is the entity's COMMITTED circulation
// direction (+1/-1, 0 = free): picked from the current tangential motion (or
// the nearest corner on a dead-perpendicular hit) on first wall contact and
// then held until the entity is wall-free — any per-frame direction choice
// has a stable flip-flop equilibrium when the target sits straight through
// a wall (the march test caught two), commitment is what rounds the corner.
static void building_slide(float *x, float *y, float *vx, float *vy, int8_t *rot)
{
    bool near = false;
    for (unsigned b = 0; b < sizeof(bldgs) / sizeof(bldgs[0]); b++) {
        const wrect_t *r = &bldgs[b];
        // velocity redirect works through the whole 9 px release band — the
        // outward nudge (act band only, 3 px) ejects the walker most frames,
        // and if steering runs uncontested on those frames it can exactly
        // cancel the slide's progress (the march test found two such stable
        // stall points). rot is held until the walker is 9 px clear.
        float m = 3.f, mr = 9.f;
        if (*x < r->x - mr || *x > r->x + r->w + mr ||
            *y < r->y - mr || *y > r->y + r->h + mr) continue;
        near = true;
        float lx = *x - (r->x - mr), rx = (r->x + r->w + mr) - *x;
        float ty = *y - (r->y - mr), by = (r->y + r->h + mr) - *y;
        float mn = lx;
        int side = 0;
        if (rx < mn) { mn = rx; side = 1; }
        if (ty < mn) { mn = ty; side = 2; }
        if (by < mn) { mn = by; side = 3; }
        float nx = 0, ny = 0;
        switch (side) {
        case 0: nx = -1; break;
        case 1: nx = 1; break;
        case 2: ny = -1; break;
        case 3: ny = 1; break;
        }
        if (*x >= r->x - m && *x <= r->x + r->w + m &&
            *y >= r->y - m && *y <= r->y + r->h + m) {
            *x += nx * 1.2f;
            *y += ny * 1.2f;
        }
        float vn = *vx * nx + *vy * ny;
        if (vn < 0) {
            *vx -= vn * nx;
            *vy -= vn * ny;
            // tangent for rot=+1 is the normal rotated 90°: (-ny, nx)
            if (!*rot) {
                float proj = *vx * -ny + *vy * nx;
                if (proj > 0.5f) *rot = 1;
                else if (proj < -0.5f) *rot = -1;
                else if (nx != 0)
                    *rot = (int8_t)((*y > r->y + r->h * 0.5f ? 1 : -1) *
                                    (nx > 0 ? 1 : -1));
                else
                    *rot = (int8_t)((*x > r->x + r->w * 0.5f ? 1 : -1) *
                                    (ny > 0 ? -1 : 1));
            }
            *vx += -ny * (float)*rot * -vn * 0.8f;
            *vy += nx * (float)*rot * -vn * 0.8f;
        }
    }
    if (!near) *rot = 0;
}

static void update_play(gs_t *g, gos_ctx_t *ctx, float dt, const gos_input_t *in,
                        bool tap)
{
    g->frame++;
    update_camera(g, dt);

    bool touch_mode = g->aim_mode == GOS_AIM_TOUCH_DRAG;

    // --- control press latch: a contact that starts on a control belongs to
    // it until release — sliding off must never start firing, and (touch
    // mode) sliding off the FIRE button must not drop the trigger mid-burst.
    if (tap) {
        g->ctl_press = ctl_zone(in->touch, touch_mode);
        if (touch_mode && in->touch.x >= 114 && in->touch.y >= 190)
            g->fire_latch = true;
    }
    if (!in->touching) {
        g->ctl_press = false;
        g->fire_latch = false;
    }

    // --- aim target: tilt/gyro from the OS aim channel; touch mode drags a
    // raw screen-space target (no filter — the turret dynamics below are the
    // only lag between finger and gun)
    float want_x, want_y;
    if (touch_mode) {
        if (in->touching) {
            // accumulate only while the finger is actually in the playfield:
            // a control-origin contact never aims, and a playfield drag that
            // strays onto a control freezes instead of dragging the aim along
            if (g->twas && !g->ctl_press && !ctl_zone(in->touch, touch_mode)) {
                g->twx += (in->touch.x - g->tlast.x) * DRAG_GAIN;
                g->twy += (in->touch.y - g->tlast.y) * DRAG_GAIN;
                if (g->twx < 12) g->twx = 12;
                if (g->twx > W - 12) g->twx = W - 12;
                if (g->twy < 12) g->twy = 12;
                if (g->twy > H - 12) g->twy = H - 12;
            }
            g->tlast = in->touch;
            g->twas = true;
        } else {
            g->twas = false;
        }
        want_x = g->twx;
        want_y = g->twy;
    } else {
        want_x = W / 2 + in->aim_x * (W / 2 - 12);
        want_y = H / 2 + in->aim_y * (H / 2 - 12);
    }

    // --- turret slew: the mount accelerates toward the target at the
    // weapon's slew rating, capped at its traverse speed, with a braking
    // curve (sqrt(2*a*d)) so it decelerates INTO the target instead of
    // overshooting. Heavy howitzer winds up; gatling stays nimble.
    const wpn_t *wp = &wpns[g->weapon];
    float trav_px = wp->traverse * (W / 50.f);   // ~50 deg FOV
    float dx = want_x - g->retx, dy = want_y - g->rety;
    float d = sqrtf(dx * dx + dy * dy);
    float des_vx = 0, des_vy = 0;
    if (d > 0.3f) {
        float des = 10.f * d;
        float brake = 0.9f * sqrtf(2.f * wp->slew * d);
        if (des > trav_px) des = trav_px;
        if (des > brake) des = brake;
        des_vx = dx / d * des;
        des_vy = dy / d * des;
    }
    float dvx = des_vx - g->retvx, dvy = des_vy - g->retvy;
    float dv = sqrtf(dvx * dvx + dvy * dvy);
    float dvmax = wp->slew * dt;
    if (dv > dvmax && dv > 0.001f) {
        dvx *= dvmax / dv;
        dvy *= dvmax / dv;
    }
    g->retvx += dvx;
    g->retvy += dvy;
    float stx = g->retvx * dt, sty = g->retvy * dt;
    if (sqrtf(stx * stx + sty * sty) >= d) {
        g->retx = want_x;                        // land, kill the ring-out
        g->rety = want_y;
        g->retvx *= 0.25f;
        g->retvy *= 0.25f;
    } else {
        g->retx += stx;
        g->rety += sty;
    }

    // --- weapon stack taps, TAP-edge only — a held finger must never repeat
    // these. Rows pitch 26 px with the touch point biased up 16: an aimed
    // press lands 15-25 px low, which is most of a row height.
    if (tap && in->touch.x >= 136 && in->touch.y >= 20 && in->touch.y < 140) {
        int row = (in->touch.y - 16 - 20) / 30;
        if (row < 0) row = 0;
        if (row > 2) row = 2;
        g->weapon = row;
    }

    // --- firing: touch mode fires from the held FIRE button; tilt/gyro from
    // the OS touch-anywhere FIRE bit, masked while the contact belongs to a
    // control (the controls sit inside the OS lower-2/3 fire zone)
    bool trigger = touch_mode ? g->fire_latch
                              : ((in->buttons & GOS_BTN_FIRE) && !g->ctl_press);
    bool trig_edge = trigger && !g->trig_was;
    g->trig_was = trigger;
    if (trigger) fire(g, dt);
    if (!trigger || g->weapon != WPN_GAT) {
        if (g->gat_on) {
            g->gat_on = false;
            gos_audio_stop(g->h_gat);
        }
    }
    if (!trigger || g->weapon != WPN_GAT || g->overheated) {
        g->heat -= dt / 3.f;
        if (g->heat < 0) g->heat = 0;
        if (g->overheated && g->heat < 0.3f) g->overheated = false;
    }
    for (int i = 0; i < 3; i++)
        if (g->cool[i] > 0) g->cool[i] -= dt;
    if (trig_edge && g->weapon != WPN_GAT && g->cool[g->weapon] > 0)
        gos_audio_play(&sfx_click, 130);
    gos_audio_set_vol(g->h_engine, trigger ? 40 : 80);   // duck under gunfire

    // --- servo whine: audible while the mount is actually moving, scaled by
    // slew speed relative to this weapon's traverse cap, ducked under fire.
    // Only push volume changes >= 4 to keep zipper noise out of the mixer.
    {
        float spd = sqrtf(g->retvx * g->retvx + g->retvy * g->retvy);
        int sv = 0;
        if (spd > 8.f) {
            sv = (int)(spd / trav_px * 90.f);
            if (sv > 85) sv = 85;
        }
        if (trigger) sv /= 2;
        if (sv - g->servo_vol >= 4 || g->servo_vol - sv >= 4 || (sv == 0 && g->servo_vol)) {
            g->servo_vol = sv;
            gos_audio_set_vol(g->h_servo, (uint8_t)sv);
        }
    }

    // --- shells ---
    for (int i = 0; i < NSH; i++) {
        if (!g->sha[i]) continue;
        g->sht[i] -= dt;
        if (g->sht[i] <= 0) {
            g->sha[i] = false;
            blast(g, g->shx[i], g->shy[i], g->shw[i]);
        }
    }

    // --- spawn clusters + telegraphs ---
    bool spawning = false;
    for (int c = 0; c < g->ncl; c++) {
        if (g->cl[c].remaining <= 0) continue;
        spawning = true;
        float was = g->cl[c].timer;
        g->cl[c].timer -= dt;
        if (was > g->cl[c].telegraph && g->cl[c].timer <= g->cl[c].telegraph) {
            add_ping(g, g->cl[c].bearing);
            gos_audio_play(&sfx_ping, 180);
        }
        if (g->cl[c].timer <= 0) {
            g->cl[c].timer = g->cl[c].interval;
            uint8_t a = (uint8_t)(g->cl[c].bearing + gos_rand_range(g->rng, -10, 10));
            // ring just outside the CURRENT optic's view, around the CAMERA
            // (not the town center — 200+ px past the view meant staring at
            // empty streets for 15 s+, field-reported). Anchoring to the
            // WIDEST optic instead pushed spawns ~80 px out and survivors
            // strolled to the bunker untouched (user-reported); zooming out
            // later just reveals walkers already inbound, which is fine.
            float hw = 92.f / g->zoom, hh = 112.f / (g->zoom * SLANT);
            float view_r = sqrtf(hw * hw + hh * hh);
            float r = view_r + 5.f + gos_randf(g->rng) * 18.f;
            spawn_zombie(g, g->cl[c].type,
                         g->camx + gos_cos256(a) * r / 16384.f,
                         g->camy + gos_sin256(a) * r / 16384.f);
            g->cl[c].remaining--;
        }
    }
    for (int i = 0; i < NPING; i++)
        if (g->ping_t[i] > 0) g->ping_t[i] -= dt;

    // --- survivors trickle in ---
    if (g->surv_pending > 0) {
        g->surv_timer -= dt;
        if (g->surv_timer <= 0) {
            g->surv_timer = 2.f;
            spawn_survivor(g);
            g->surv_pending--;
        }
    }

    // --- rebuild spatial grid with live zombies ---
    gos_grid_clear();
    for (int i = 0; i < NZ; i++)
        if (g->zhp[i] > 0)
            gos_grid_insert((uint16_t)i, (int16_t)g->zx[i], (int16_t)g->zy[i]);

    // --- zombies: steer to nearest survivor within 400, else extraction ---
    static const float speed[3] = { 19.f, 36.f, 10.f };
    for (int i = 0; i < NZ; i++) {
        if (g->zhp[i] <= 0) continue;
        float tx = EXT_X, ty = EXT_Y;
        float best = 400.f * 400.f;
        for (int s = 0; s < NS; s++) {
            if (g->sstate[s] != SV_MOVE && g->sstate[s] != SV_COWER) continue;
            float ddx = g->sx[s] - g->zx[i], ddy = g->sy[s] - g->zy[i];
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best) { best = d2; tx = g->sx[s]; ty = g->sy[s]; }
        }
        float ddx = tx - g->zx[i], ddy = ty - g->zy[i];
        float len = sqrtf(ddx * ddx + ddy * ddy) + 0.001f;
        float sp = speed[g->ztype[i]];
        float vx = ddx / len * sp, vy = ddy / len * sp;

        // separation
        uint16_t nb[8];
        int n = gos_grid_query((int16_t)g->zx[i], (int16_t)g->zy[i], 12, nb, 8);
        for (int k = 0; k < n; k++) {
            int j = nb[k];
            if (j == i || j >= NZ || g->zhp[j] <= 0) continue;
            float sx2 = g->zx[i] - g->zx[j], sy2 = g->zy[i] - g->zy[j];
            float d2 = sx2 * sx2 + sy2 * sy2 + 0.01f;
            if (d2 < 100.f) {
                float f = 8.f / d2;
                vx += sx2 * f;
                vy += sy2 * f;
            }
        }

        building_slide(&g->zx[i], &g->zy[i], &vx, &vy, &g->zrot[i]);
        g->zvx[i] = vx;
        g->zvy[i] = vy;
        g->zx[i] += vx * dt;
        g->zy[i] += vy * dt;

        // contact kill
        for (int s = 0; s < NS; s++) {
            if (g->sstate[s] != SV_MOVE && g->sstate[s] != SV_COWER) continue;
            float kdx = g->sx[s] - g->zx[i], kdy = g->sy[s] - g->zy[i];
            if (kdx * kdx + kdy * kdy < 16.f) {
                g->sstate[s] = SV_DEAD;
                g->lost++;
                g->salvage_run -= 150;
                g->civ_flash = 0.9f;
                gos_audio_play(&sfx_lost, 235);
                add_scorch(g, g->sx[s], g->sy[s], 3, 9.f);
            }
        }
    }

    // --- survivors ---
    for (int i = 0; i < NS; i++) {
        if (g->sstate[i] != SV_MOVE && g->sstate[i] != SV_COWER) continue;
        // cower when infected are close: the moment the player must act
        uint16_t nb[4];
        int near = gos_grid_query((int16_t)g->sx[i], (int16_t)g->sy[i], 30, nb, 4);
        g->sstate[i] = near > 0 ? SV_COWER : SV_MOVE;
        if (g->sstate[i] == SV_COWER) continue;
        float ddx = EXT_X - g->sx[i], ddy = EXT_Y - g->sy[i];
        float len = sqrtf(ddx * ddx + ddy * ddy);
        if (len < 8.f) {
            g->sstate[i] = SV_SAFE;
            g->saved++;
            g->salvage_run += 100;
            add_flash(g, g->sx[i], g->sy[i], 5);
            gos_audio_play(&sfx_saved, 220);
            continue;
        }
        float svx = ddx / len * 21.f, svy = ddy / len * 21.f;
        building_slide(&g->sx[i], &g->sy[i], &svx, &svy, &g->srot[i]);
        g->sx[i] += svx * dt;
        g->sy[i] += svy * dt;
    }

    // --- flashes, scorches, civ flash ---
    for (int i = 0; i < NFL; i++)
        if (g->flt[i] > 0) g->flt[i] -= dt;
    for (int i = 0; i < NSC; i++)
        if (g->scoh[i] > 4.f) g->scoh[i] -= dt;   // ~8 s from 12 down to 4
    if (g->civ_flash > 0) g->civ_flash -= dt;

    // --- lose / wave end ---
    if (g->lost >= 6) {
        g->mode = GS_LOSE;
        g->mode_t = 0;
        if (g->gat_on) { g->gat_on = false; gos_audio_stop(g->h_gat); }
        // the run pot can go negative (civ deaths -150), but the SAVED total
        // never does — repeated losses once banked -1600 (user-reported)
        g->salvage += g->salvage_run;
        if (g->salvage < 0) g->salvage = 0;
        save_meta(g, ctx);
        return;
    }
    bool surv_open = g->surv_pending > 0;
    for (int i = 0; i < NS && !surv_open; i++)
        if (g->sstate[i] == SV_MOVE || g->sstate[i] == SV_COWER) surv_open = true;
    if (!spawning && g->nz_alive == 0 && !surv_open) {
        if (g->gat_on) { g->gat_on = false; gos_audio_stop(g->h_gat); }
        if (g->wave >= 9) {
            g->mode = GS_WIN;
            g->mode_t = 0;
            g->missions_won++;
            if (g->saved > g->best_saved) g->best_saved = (uint8_t)g->saved;
            g->salvage += g->salvage_run;
            if (g->salvage < 0) g->salvage = 0;
            save_meta(g, ctx);
        } else {
            g->mode = GS_BREAK;
            g->mode_t = 12.f;
            g->salvage += g->salvage_run;
            if (g->salvage < 0) g->salvage = 0;
            g->salvage_run = 0;
            save_meta(g, ctx);
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void draw_wrect(const gs_t *g, const wrect_t *r, gos_color_t c)
{
    float x0, y0, x1, y1, x2, y2, x3, y3;
    world_to_screen(g, r->x, r->y, &x0, &y0);
    world_to_screen(g, r->x + r->w, r->y, &x1, &y1);
    world_to_screen(g, r->x + r->w, r->y + r->h, &x2, &y2);
    world_to_screen(g, r->x, r->y + r->h, &x3, &y3);
    // quick reject: everything far off screen
    if ((x0 < -60 && x1 < -60 && x2 < -60 && x3 < -60) ||
        (x0 > W + 60 && x1 > W + 60 && x2 > W + 60 && x3 > W + 60) ||
        (y0 < -60 && y1 < -60 && y2 < -60 && y3 < -60) ||
        (y0 > H + 60 && y1 > H + 60 && y2 > H + 60 && y3 > H + 60)) return;
    gos_pt_t q[4] = {
        { (int16_t)x0, (int16_t)y0 }, { (int16_t)x1, (int16_t)y1 },
        { (int16_t)x2, (int16_t)y2 }, { (int16_t)x3, (int16_t)y3 },
    };
    gos_gfx_quad_fill(q, c);
}

static void render_world(gs_t *g)
{
    gos_gfx_clear(P_GROUND);

    // ground texture: world-anchored thermal mottle — dirt patches read cold
    // (flat fill, painted before the roads so they stay under everything),
    // dry scrub reads faintly warm (soft blob). Deterministic hash keeps
    // every patch pinned to the world as the camera orbits.
    {
        uint32_t s = 0xC0FFEE1u;
        for (int i = 0; i < 56; i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            float wx = 360.f + (float)(s & 0x1FF);
            float wy = 360.f + (float)((s >> 9) & 0x1FF);
            float sx, sy;
            world_to_screen(g, wx, wy, &sx, &sy);
            if (sx < -12 || sx > W + 12 || sy < -12 || sy > H + 12) continue;
            int r = 2 + (int)((s >> 18) & 7);
            if (s & (1u << 21))
                gos_gfx_circle_fill((int)sx, (int)sy, r > 5 ? 4 : r, P_GROUND - 1);
            else
                gos_gfx_circle_soft((int)sx, (int)sy, r, P_ROAD, P_GROUND);
        }
    }

    for (unsigned i = 0; i < sizeof(roads) / sizeof(roads[0]); i++)
        draw_wrect(g, &roads[i], P_ROAD);
    for (unsigned i = 0; i < sizeof(bldgs) / sizeof(bldgs[0]); i++)
        draw_wrect(g, &bldgs[i], P_BLDG);

    // extraction bunker: sunken dugout with a north-facing mouth toward the
    // town — the survivors run INTO something, not onto a bare pad. Visual
    // only, deliberately absent from bldgs[]: walls as obstacles would make
    // the mouth a chokepoint that jams the crowd.
    {
        static const wrect_t bunk_floor = { 588, 726, 24, 36 };
        static const wrect_t bunk_walls[] = {
            { 582, 726, 6, 42 },     // west wall
            { 612, 726, 6, 42 },     // east wall
            { 582, 762, 36, 6 },     // south (back) wall
        };
        draw_wrect(g, &bunk_floor, P_ROAD);
        for (unsigned i = 0; i < 3; i++)
            draw_wrect(g, &bunk_walls[i], P_BLDG);
    }
    float ex, ey;
    world_to_screen(g, EXT_X, EXT_Y, &ex, &ey);
    if ((g->frame / 20) & 1) {
        gos_gfx_line((int)ex - 12, (int)ey - 12, (int)ex - 6, (int)ey - 12, P_HOT);
        gos_gfx_line((int)ex - 12, (int)ey - 12, (int)ex - 12, (int)ey - 6, P_HOT);
        gos_gfx_line((int)ex + 12, (int)ey + 12, (int)ex + 6, (int)ey + 12, P_HOT);
        gos_gfx_line((int)ex + 12, (int)ey + 12, (int)ex + 12, (int)ey + 6, P_HOT);
    }

    // scorch marks (world remembers what you did)
    for (int i = 0; i < NSC; i++) {
        if (g->scoh[i] <= 4.f) continue;
        float sx, sy;
        world_to_screen(g, g->scox[i], g->scoy[i], &sx, &sy);
        gos_gfx_circle_soft((int)sx, (int)sy, g->scor[i],
                            (uint8_t)g->scoh[i], P_GROUND);
    }

    // zombies, with thermal motion smear along their apparent (screen-space)
    // travel — the per-frame delta includes the camera's own drift, so the
    // whole scene smears the way the real sensor footage did
    for (int i = 0; i < NZ; i++) {
        if (g->zhp[i] <= 0) continue;
        float sx, sy;
        world_to_screen(g, g->zx[i], g->zy[i], &sx, &sy);
        float ddx = sx - g->zpsx[i], ddy = sy - g->zpsy[i];
        if (g->zpsx[i] > -9000.f && ddx * ddx + ddy * ddy < 25.f) {
            g->zvfx[i] = g->zvfx[i] * 0.7f + ddx * 0.3f;
            g->zvfy[i] = g->zvfy[i] * 0.7f + ddy * 0.3f;
        } else {
            g->zvfx[i] = g->zvfy[i] = 0;
        }
        g->zpsx[i] = sx;
        g->zpsy[i] = sy;
        float vsx = g->zvfx[i], vsy = g->zvfy[i];
        bool trail = vsx * vsx + vsy * vsy > 0.02f;
        if (sx < -8 || sx > W + 8 || sy < -8 || sy > H + 8) continue;
        int bob = ((g->frame >> 3) + g->zseed[i]) & 1;
        if (g->ztype[i] == ZT_BRUTE) {
            if (trail) {
                gos_gfx_circle_soft((int)(sx - vsx * 4), (int)(sy - vsy * 4), 4,
                                    P_ZOM, P_WARM);
                gos_gfx_circle_soft((int)(sx - vsx * 8), (int)(sy - vsy * 8), 3,
                                    P_ZOM - 2, P_WARM);
            }
            gos_gfx_circle_soft((int)sx, (int)sy - bob, 5, P_ZHOT + 1, P_WARM);
        } else {
            if (trail) {
                gos_gfx_circle_soft((int)(sx - vsx * 4), (int)(sy - vsy * 4), 2,
                                    P_ZOM - 2, P_WARM);
                if (g->ztype[i] == ZT_RUNNER)
                    gos_gfx_circle_soft((int)(sx - vsx * 8), (int)(sy - vsy * 8),
                                        2, P_ZOM - 4, P_WARM);
            }
            gos_gfx_circle_soft((int)sx, (int)sy - bob, 3, P_ZOM, P_WARM);
        }
    }

    // survivors: hotter, bigger, and the heaviest smear on the screen —
    // their big signatures drag a visible tail through the thermal
    for (int i = 0; i < NS; i++) {
        if (g->sstate[i] != SV_MOVE && g->sstate[i] != SV_COWER) continue;
        float sx, sy;
        world_to_screen(g, g->sx[i], g->sy[i], &sx, &sy);
        float ddx = sx - g->spsx[i], ddy = sy - g->spsy[i];
        if (g->spsx[i] > -9000.f && ddx * ddx + ddy * ddy < 25.f) {
            g->svfx[i] = g->svfx[i] * 0.7f + ddx * 0.3f;
            g->svfy[i] = g->svfy[i] * 0.7f + ddy * 0.3f;
        } else {
            g->svfx[i] = g->svfy[i] = 0;
        }
        g->spsx[i] = sx;
        g->spsy[i] = sy;
        float vsx = g->svfx[i], vsy = g->svfy[i];
        bool trail = vsx * vsx + vsy * vsy > 0.02f;
        if (trail) {
            gos_gfx_circle_soft((int)(sx - vsx * 3), (int)(sy - vsy * 3), 4,
                                P_SURV - 2, P_WARM + 1);
            gos_gfx_circle_soft((int)(sx - vsx * 6), (int)(sy - vsy * 6), 3,
                                P_SURV - 4, P_WARM);
            gos_gfx_circle_soft((int)(sx - vsx * 9), (int)(sy - vsy * 9), 2,
                                P_WARM + 1, P_WARM);
        }
        gos_gfx_circle_soft((int)sx, (int)sy, 4, P_SURV, P_WARM + 1);
        gos_gfx_circle_soft((int)sx, (int)sy + 2, 2, P_SURV - 3, P_WARM);
        if (g->sstate[i] == SV_COWER && ((g->frame / 8) & 1))
            gos_gfx_circle((int)sx, (int)sy, 7, P_SURV);
    }

    // incoming shells: shrinking ring + x at the impact point
    for (int i = 0; i < NSH; i++) {
        if (!g->sha[i] || g->shw[i] == WPN_GAT) continue;
        float sx, sy;
        world_to_screen(g, g->shx[i], g->shy[i], &sx, &sy);
        float frac = g->sht[i] / g->shtof[i];
        int rr = (int)(3 + 14 * frac);
        gos_gfx_circle((int)sx, (int)sy, rr, P_WARM + 1);
        gos_gfx_line((int)sx - 2, (int)sy, (int)sx + 2, (int)sy, P_ZOM);
        gos_gfx_line((int)sx, (int)sy - 2, (int)sx, (int)sy + 2, P_ZOM);
    }

    // flashes
    for (int i = 0; i < NFL; i++) {
        if (g->flt[i] <= 0) continue;
        float sx, sy;
        world_to_screen(g, g->flx[i], g->fly[i], &sx, &sy);
        float t = g->flt[i] / 0.18f;
        gos_gfx_circle_soft((int)sx, (int)sy, (int)(g->flr[i] * (1.2f - t)),
                            P_WHITE, P_ZHOT);
        gos_gfx_circle((int)sx, (int)sy, (int)(g->flr[i] * (1.5f - t) + 2),
                       P_HOT);
    }

    // sensor noise, straight into the framebuffer
    int noise = g->up[UP_SENSOR] ? 80 : 200;
    uint8_t *fb = gos_gfx_fb();
    for (int i = 0; i < noise; i++) {
        uint32_t r = gos_rand(g->rng);
        uint8_t *p = fb + (r % (W * H));
        if (*p < 12) (*p)++;
    }
}

// project a screen-space unit direction from center onto the screen edge,
// `inset` px in from the border
static void edge_point(float ca, float sa, int inset, int *px, int *py)
{
    float t = 1e9f;
    if (ca > 0.001f) t = (W / 2 - inset) / ca;
    else if (ca < -0.001f) t = -(W / 2 - inset) / ca;
    float t2 = 1e9f;
    if (sa > 0.001f) t2 = (H / 2 - inset) / sa;
    else if (sa < -0.001f) t2 = -(H / 2 - inset) / sa;
    if (t2 < t) t = t2;
    *px = (int)(W / 2 + ca * t);
    *py = (int)(H / 2 + sa * t);
}

static void render_hud(gs_t *g)
{
    const wpn_t *w = &wpns[g->weapon];
    bool touch_mode = g->aim_mode == GOS_AIM_TOUCH_DRAG;

    // top strip: run readouts left, heat/reload bar top right — nothing
    // interactive lives up here, so the OS swipe-down-pause owns the top edge
    gos_color_t cc = (g->civ_flash > 0 && ((g->frame / 4) & 1)) ? P_WHITE : P_ZHOT;
    gos_gfx_text(0, 6, 8, P_ZHOT, "SAVED %d", g->saved);
    gos_gfx_text(0, 64, 8, cc, "LOST %d/6", g->lost);
    gos_gfx_text(0, 6, 20, P_ZHOT, "WV %d/10", g->wave + 1);
    {
        long bank = (long)(g->salvage + g->salvage_run);
        gos_gfx_text(0, 64, 20, P_SURV, "$%ld", bank < 0 ? 0 : bank);
    }
    if (g->civ_flash > 0)
        gos_gfx_text(0, 64, 32, P_WHITE, "CIV DOWN");

    float f = 0;
    if (g->weapon == WPN_GAT) f = g->heat;
    else {
        float reload = 1.f / w->rate;
        if (g->weapon == WPN_BOF && g->up[UP_B_RELOAD]) reload *= 0.7f;
        if (g->weapon == WPN_HOW && g->up[UP_H_RELOAD]) reload *= 0.7f;
        f = g->cool[g->weapon] > 0 ? g->cool[g->weapon] / reload : 0;
    }
    gos_gfx_rect((gos_rect_t){ 136, 7, 42, 9 }, P_ZOM);
    gos_gfx_rect_fill((gos_rect_t){ 137, 8, (int16_t)(40 * f), 7 },
                      g->overheated ? P_WHITE : P_ZHOT);
    if (g->overheated && ((g->frame / 6) & 1))
        gos_gfx_text(0, 136, 18, P_WHITE, "HOT");

    // weapon stack, top-right under the heat bar
    for (int i = 0; i < 3; i++) {
        gos_rect_t r = { 142, (int16_t)(22 + 30 * i), 36, 18 };
        if (i == g->weapon) gos_gfx_rect_fill(r, P_WARM);
        gos_gfx_rect(r, i == g->weapon ? P_WHITE : P_ZOM);
        gos_gfx_text(0, 149, r.y + 6, i == g->weapon ? P_WHITE : P_ZOM,
                     "%s", wpns[i].chip);
    }
    if (touch_mode) {
        // touch mode: hold-to-shoot trigger in the bottom-right corner
        gos_rect_t r = { 122, 200, 56, 18 };
        if (g->fire_latch) gos_gfx_rect_fill(r, P_WARM);
        gos_gfx_rect(r, g->fire_latch ? P_WHITE : P_ZHOT);
        gos_gfx_text(0, 138, 206, g->fire_latch ? P_WHITE : P_ZHOT, "FIRE");
    }

    // per-weapon reticle (the blast weapons draw their true blast ring)
    int rx = (int)g->retx, ry = (int)g->rety;
    switch (g->weapon) {
    case WPN_GAT:   // 25mm: tight corner brackets, point fire
        gos_gfx_line(rx - 8, ry - 8, rx - 4, ry - 8, P_WHITE);
        gos_gfx_line(rx - 8, ry - 8, rx - 8, ry - 4, P_WHITE);
        gos_gfx_line(rx + 8, ry - 8, rx + 4, ry - 8, P_WHITE);
        gos_gfx_line(rx + 8, ry - 8, rx + 8, ry - 4, P_WHITE);
        gos_gfx_line(rx - 8, ry + 8, rx - 4, ry + 8, P_WHITE);
        gos_gfx_line(rx - 8, ry + 8, rx - 8, ry + 4, P_WHITE);
        gos_gfx_line(rx + 8, ry + 8, rx + 4, ry + 8, P_WHITE);
        gos_gfx_line(rx + 8, ry + 8, rx + 8, ry + 4, P_WHITE);
        break;
    case WPN_BOF: { // 40mm: blast ring + cardinal ticks, open center
        int br = (int)(wpns[WPN_BOF].blast * (g->up[UP_B_BLAST] ? 1.25f : 1.f)
                       * g->zoom * (1.f + SLANT) * 0.5f);
        gos_gfx_circle(rx, ry, br, P_WHITE);
        gos_gfx_line(rx - br - 4, ry, rx - br + 2, ry, P_WHITE);
        gos_gfx_line(rx + br - 2, ry, rx + br + 4, ry, P_WHITE);
        gos_gfx_line(rx, ry - br - 4, rx, ry - br + 2, P_WHITE);
        gos_gfx_line(rx, ry + br - 2, rx, ry + br + 4, P_WHITE);
        break; }
    case WPN_HOW: { // 105mm: wide blast ring + ticks, nothing inside — the
                    // target has to stay visible under the gun
        int br = (int)(wpns[WPN_HOW].blast * (g->up[UP_H_BLAST] ? 1.2f : 1.f)
                       * g->zoom * (1.f + SLANT) * 0.5f);
        gos_gfx_circle(rx, ry, br, P_WHITE);
        gos_gfx_line(rx - br - 5, ry, rx - br + 3, ry, P_WHITE);
        gos_gfx_line(rx + br - 3, ry, rx + br + 5, ry, P_WHITE);
        gos_gfx_line(rx, ry - br - 5, rx, ry - br + 3, P_WHITE);
        gos_gfx_line(rx, ry + br - 3, rx, ry + br + 5, P_WHITE);
        break; }
    }
    gos_gfx_pixel(rx, ry, P_WHITE);

    // predicted impact marker: where the round would land, given TOF and the
    // camera's continuing orbit — learn to trust the x, not the brackets
    {
        float wx, wy;
        screen_to_world(g, g->retx, g->rety, &wx, &wy);
        float rate = g->up[UP_ORBIT] ? 0.0175f : 0.0262f;
        float th = g->ltheta + rate * w->tof;
        float c = cosf(th), s = sinf(th);
        float dx = wx - g->lcamx, dy = wy - g->lcamy;
        // include the recoil offset, exactly as world_to_screen does — without
        // it the X drifts off the true impact point while recoil decays
        int px = (int)(W / 2 + (dx * c - dy * s) * g->zoom + g->recx);
        int py = (int)(H / 2 + (dx * s + dy * c) * g->zoom * SLANT + g->recy);
        gos_gfx_line(px - 3, py - 3, px + 3, py + 3, P_HOT);
        gos_gfx_line(px - 3, py + 3, px + 3, py - 3, P_HOT);
    }

    // off-screen survivor markers: a survivor you cannot see is one you
    // cannot defend — mark the screen edge toward each live one out of frame,
    // blinking hard while they cower under attack
    for (int i = 0; i < NS; i++) {
        if (g->sstate[i] != SV_MOVE && g->sstate[i] != SV_COWER) continue;
        float sx, sy;
        world_to_screen(g, g->sx[i], g->sy[i], &sx, &sy);
        if (sx >= 4 && sx < W - 4 && sy >= 4 && sy < H - 4) continue;
        float dx = sx - W / 2, dy = sy - H / 2;
        float len = sqrtf(dx * dx + dy * dy) + 0.001f;
        int px, py;
        edge_point(dx / len, dy / len, 10, &px, &py);
        // keep clear of the top readouts; controls are drawn before markers,
        // so a marker landing on a chip stays visible on top of it
        if (py < 30) py = 30;
        if (py > H - 22) py = H - 22;
        bool urgent = g->sstate[i] == SV_COWER;
        if (!urgent || ((g->frame / 5) & 1)) {
            gos_gfx_circle_soft(px, py, 3, P_SURV, P_WARM);
            gos_gfx_circle(px, py, urgent ? 6 : 5, P_SURV - (urgent ? 0 : 3));
        }
    }

    // spawn telegraph pings: ticks on the screen edge toward the bearing
    for (int i = 0; i < NPING; i++) {
        if (g->ping_t[i] <= 0) continue;
        // bearing is world angle; rotate into screen space
        float a = g->ping_a[i] * (2 * (float)M_PI / 256) + g->ltheta;
        int px, py;
        edge_point(cosf(a), sinf(a), 10, &px, &py);
        if ((g->frame / 10) & 1)
            gos_gfx_circle_fill(px, py, 2, P_WHITE);
    }

}

// ---------------------------------------------------------------------------
// Upgrade / meta screens
// ---------------------------------------------------------------------------

static const struct { const char *name; int cost; } up_defs[UP_COUNT] = {
    { "25 HEAT+", 300 },  { "25 TIGHT", 350 }, { "40 FAST", 450 },
    { "40 BLAST", 500 },  { "105 FAST", 700 }, { "105 BLAST", 750 },
    { "SENSOR+", 400 },   { "STEADY", 600 },
};

static void break_frame(gs_t *g, gos_ctx_t *ctx, float dt, const gos_input_t *in,
                        bool tap)
{
    // no countdown: the break lasts until the player taps READY
    update_camera(g, dt);

    if (tap) {
        // top-down first-match: READY first, then the upgrade grid — the
        // bottom card row used to overlap the READY band, so one tap both
        // bought an upgrade and skipped the wave clock
        if (in->touch.y >= 196) {
            g->mode_t = 0;                        // READY skips the clock
        } else {
            // upgrade chips (2 cols x 4 rows starting y=52); touch point
            // biased up ~12 px for the low-press offset of this panel, each
            // row's band bounded so adjacent rows keep a dead seam
            int ty = in->touch.y - 12;
            int col = in->touch.x / (W / 2);
            int row = (ty - 50) / 34;
            if (ty >= 50 && row >= 0 && row < 4 && (ty - 50) - row * 34 < 32) {
                int i = row * 2 + col;
                if (i < UP_COUNT && !g->up[i] && g->salvage >= up_defs[i].cost) {
                    g->up[i] = 1;
                    g->salvage -= up_defs[i].cost;
                    gos_audio_play(&sfx_buy, 220);
                    save_meta(g, ctx);
                }
            }
        }
    }

    if (g->mode_t <= 0) {
        g->wave++;
        setup_wave(g);
        g->mode = GS_PLAY;
    }
}

static void render_break(gs_t *g)
{
    render_world(g);
    gos_gfx_rect_fill((gos_rect_t){ 0, 0, W, 46 }, P_BLDG);
    gos_gfx_text(1, 8, 6, P_WHITE, "WAVE %d CLEAR", g->wave + 1);
    gos_gfx_text(0, 8, 26, P_SURV, "SALVAGE %ld", (long)g->salvage);

    for (int i = 0; i < UP_COUNT; i++) {
        int col = i % 2, row = i / 2;
        gos_rect_t r = { (int16_t)(6 + col * 88), (int16_t)(52 + row * 34), 84, 30 };
        bool afford = !g->up[i] && g->salvage >= up_defs[i].cost;
        gos_gfx_rect_fill(r, g->up[i] ? P_WARM : P_BLDG);
        gos_gfx_rect(r, g->up[i] ? P_SURV : (afford ? P_ZHOT : P_ROAD + 1));
        gos_gfx_text(0, r.x + 4, r.y + 5, g->up[i] ? P_WHITE : P_ZHOT,
                     "%s", up_defs[i].name);
        if (g->up[i])
            gos_gfx_text(0, r.x + 4, r.y + 17, P_SURV, "OWNED");
        else
            gos_gfx_text(0, r.x + 4, r.y + 17, afford ? P_SURV : P_ZOM - 2,
                         "$%d", up_defs[i].cost);
    }
    gos_gfx_rect((gos_rect_t){ 54, 196, 76, 22 }, P_ZHOT);
    gos_gfx_text(0, 71, 204, P_WHITE, "READY >");
}

static void start_mission(gs_t *g)
{
    memset(g->zhp, 0, sizeof g->zhp);
    memset(g->sstate, 0, sizeof g->sstate);
    memset(g->sha, 0, sizeof g->sha);
    for (int i = 0; i < NSC; i++) g->scoh[i] = 0;
    for (int i = 0; i < NFL; i++) g->flt[i] = 0;
    for (int i = 0; i < NPING; i++) g->ping_t[i] = 0;
    g->nz_alive = 0;
    g->saved = g->lost = g->kills = 0;
    g->salvage_run = 0;
    // upgrades are per-RUN: they reset every mission (banked salvage does
    // not) — persisted, they were all owned within a couple of games and
    // the wave-break shop went dead (user-reported)
    memset(g->up, 0, sizeof g->up);
    g->wave = 0;
    g->heat = 0;
    g->overheated = false;
    g->weapon = WPN_GAT;
    g->zoom = wpns[WPN_GAT].zoom;
    g->retx = g->twx = W / 2;
    g->rety = g->twy = H / 2;
    g->retvx = g->retvy = 0;
    g->twas = false;
    g->ctl_press = g->fire_latch = g->trig_was = false;
    for (int i = 0; i < NZ; i++) g->zpsx[i] = g->zpsy[i] = -10000.f;
    for (int i = 0; i < NS; i++) g->spsx[i] = g->spsy[i] = -10000.f;
    setup_wave(g);
    g->mode = GS_PLAY;
}

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

static void gun_init(gos_ctx_t *ctx)
{
    gs_t *g = ctx->state;
    g->rng = &ctx->rng;
    load_meta(g, ctx);

    // 16-entry white-hot thermal palette (spec §3), indices 0..15
    static const uint16_t pal[16] = {
        // cold ground -> warm -> bodies -> blown-out
        0x0841, 0x18C3, 0x2124, 0x31A6,   // 0-3 cold
        0x4A69, 0x630C, 0x7BCF,           // 4-6 warm ground
        0x9CF3, 0xAD75, 0xBDF7,           // 7-9 infected
        0xD69A, 0xE71C, 0xEF5D,           // 10-12 survivors
        0xF79E, 0xFFDF, 0xFFFF,           // 13-15 flash
    };
    gos_gfx_set_palette(pal, 16, 0);
    gos_gfx_scanlines(true);

    gos_grid_init(16, 1200, 1200);
    g->theta = 0;
    g->ltheta = 0;
    g->camx = g->lcamx = CX;
    g->camy = g->lcamy = CY + 30.f;
    g->tc = 1;
    g->ts = 0;
    g->zoom = wpns[WPN_GAT].zoom;
    g->retx = W / 2;
    g->rety = H / 2;
    g->mode = GS_BRIEF;
    g->h_engine = gos_audio_loop(&sfx_engine, 80);
    g->h_servo = gos_audio_loop(&sfx_servo, 0);
    g->servo_vol = 0;
}

static void gun_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    gs_t *g = ctx->state;
    g->aim_mode = (uint8_t)gos_input_get_aim_mode();
    bool tap = in->touching && !g->touch_was;
    g->touch_was = in->touching;
    // every menu screen carries an EXIT chip in the top-left corner
    // (zone extends well below the chip: finger contact lands ~20 px under
    // where the user aims on this panel)
    bool exit_tap = tap && in->touch.y < 44 && in->touch.x < 64;

    switch (g->mode) {
    case GS_PLAY:
        update_play(g, ctx, dt, in, tap);
        break;
    case GS_BREAK:
        break_frame(g, ctx, dt, in, tap);
        break;
    case GS_BRIEF:
        update_camera(g, dt);
        if (in->pressed & GOS_BTN_FIRE) start_mission(g);
        break;
    case GS_WIN:
    case GS_LOSE:
        update_camera(g, dt);
        g->mode_t += dt;
        if (exit_tap) gos_request_exit();
        else if (g->mode_t > 1.f && (in->pressed & GOS_BTN_FIRE)) {
            if (g->mode == GS_WIN) g->mission++;
            g->mode = GS_BRIEF;
        }
        break;
    }

    // leaving GS_PLAY mid-slew must not leave the servo whining
    if (g->mode != GS_PLAY && g->servo_vol) {
        g->servo_vol = 0;
        gos_audio_set_vol(g->h_servo, 0);
    }
}

static void draw_exit_chip(void)
{
    gos_gfx_rect((gos_rect_t){ 6, 5, 44, 14 }, P_ZOM);
    gos_gfx_text(0, 10, 9, P_ZHOT, "< EXIT");
}

static void gun_render(gos_ctx_t *ctx)
{
    gs_t *g = ctx->state;
    switch (g->mode) {
    case GS_PLAY:
        render_world(g);
        render_hud(g);
        break;
    case GS_BREAK:
        render_break(g);
        break;
    case GS_BRIEF:
        render_world(g);
        gos_gfx_rect_fill((gos_rect_t){ 0, 56, W, 96 }, P_BLDG);
        gos_gfx_text(1, 26, 62, P_WHITE, "GUNSHIP");
        gos_gfx_text(0, 14, 84, P_SURV, "CLEAR THE PATH.");
        switch (g->aim_mode) {
        case GOS_AIM_GYRO_RATE:
            gos_gfx_text(0, 14, 96, P_ZHOT, "TURN DEVICE=AIM  TOUCH=FIRE");
            gos_gfx_text(0, 14, 108, P_ZHOT, "RIGHT CHIPS=GUN  SWIPE=RECENTER");
            break;
        case GOS_AIM_TOUCH_DRAG:
            gos_gfx_text(0, 14, 96, P_ZHOT, "DRAG=AIM  FIRE BTN=SHOOT");
            gos_gfx_text(0, 14, 108, P_ZHOT, "RIGHT CHIPS=GUN");
            break;
        default:
            gos_gfx_text(0, 14, 96, P_ZHOT, "TILT=AIM  TOUCH=FIRE");
            gos_gfx_text(0, 14, 108, P_ZHOT, "RIGHT CHIPS=GUN  SWIPE=RECENTER");
        }
        gos_gfx_text(0, 14, 120, P_ZHOT, "SWIPE DOWN FROM TOP = MENU");
        gos_gfx_text(0, 14, 138, P_WHITE, "TAP TO START");
        if (g->missions_won)
            gos_gfx_text(0, 120, 138, P_SURV, "WINS %d", g->missions_won);
        break;
    case GS_WIN:
    case GS_LOSE:
        render_world(g);
        draw_exit_chip();
        gos_gfx_rect_fill((gos_rect_t){ 0, 56, W, 96 }, P_BLDG);
        gos_gfx_text(1, 20, 64, P_WHITE,
                     g->mode == GS_WIN ? "TOWN HELD" : "OVERRUN");
        gos_gfx_text(0, 14, 88, P_SURV, "SAVED %d  LOST %d", g->saved, g->lost);
        gos_gfx_text(0, 14, 100, P_ZHOT, "KILLS %d", g->kills);
        gos_gfx_text(0, 14, 112, P_SURV, "SALVAGE %+ld  (TOTAL %ld)",
                     (long)g->salvage_run, (long)g->salvage);
        gos_gfx_text(0, 14, 132, P_WHITE, "TAP TO CONTINUE");
        break;
    }
}

static void gun_suspend(gos_ctx_t *ctx)
{
    gs_t *g = ctx->state;
    if (g->gat_on) {
        g->gat_on = false;
        gos_audio_stop(g->h_gat);
    }
    gos_audio_stop(g->h_engine);
    gos_audio_stop(g->h_servo);
    save_meta(g, ctx);
}

static void gun_resume(gos_ctx_t *ctx)
{
    gs_t *g = ctx->state;
    g->h_engine = gos_audio_loop(&sfx_engine, 80);
    g->h_servo = gos_audio_loop(&sfx_servo, 0);
    g->servo_vol = 0;
}

static void gun_teardown(gos_ctx_t *ctx)
{
    gs_t *g = ctx->state;
    save_meta(g, ctx);
    gos_audio_stop_all();
}

const gos_game_t game_gunship = {
    .id = "gunship",
    .title = "GUNSHIP",
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_SAVE | GOS_CAP_TOUCH_FIRE,
    .state_size = sizeof(gs_t),
    .init = gun_init,
    .update = gun_update,
    .render = gun_render,
    .suspend = gun_suspend,
    .resume = gun_resume,
    .teardown = gun_teardown,
};

#ifdef GOS_HOST_SIM
// host-sim probes: scripted harnesses assert against live state without
// gs_t escaping this file (mirror of golf's sim-gated debug hooks)
typedef struct {
    int mode, weapon, shells, ups;
    float heat, retx, rety, retvx, retvy, mode_t, twx, twy;
    long salvage;
    int fire_latch, ctl_press;
    int saved, lost, wave, nz_alive;
} gunship_sim_probe_t;

void gunship_sim_probe(gos_ctx_t *ctx, gunship_sim_probe_t *p)
{
    gs_t *g = ctx->state;
    int sh = 0, ups = 0;
    for (int i = 0; i < NSH; i++) sh += g->sha[i];
    for (int i = 0; i < UP_COUNT; i++) ups += g->up[i];
    *p = (gunship_sim_probe_t){
        .mode = g->mode, .weapon = g->weapon, .shells = sh, .ups = ups,
        .heat = g->heat, .retx = g->retx, .rety = g->rety,
        .retvx = g->retvx, .retvy = g->retvy,
        .mode_t = g->mode_t, .twx = g->twx, .twy = g->twy,
        .salvage = (long)g->salvage,
        .fire_latch = g->fire_latch, .ctl_press = g->ctl_press,
        .saved = g->saved, .lost = g->lost, .wave = g->wave,
        .nz_alive = g->nz_alive,
    };
}

void gunship_sim_force_break(gos_ctx_t *ctx)
{
    gs_t *g = ctx->state;
    g->mode = GS_BREAK;
    g->mode_t = 12.f;
    g->salvage = 5000;
}

void gunship_sim_slide(float *x, float *y, float *vx, float *vy, int8_t *rot)
{
    building_slide(x, y, vx, vy, rot);
}
#endif
