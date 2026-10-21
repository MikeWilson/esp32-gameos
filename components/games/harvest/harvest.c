// harvest.c — HARVEST, a zen isometric combine for gameos.
//
// Marble-on-a-plane steering: tilt where you want the little combine to go —
// it turns toward the tilt direction, tilt magnitude sets the speed, and a
// flat device eases it to a stop. The renderer is a
// 2.5D relief pass over a world texture: standing wheat is drawn on a raised
// plane (tree canopies higher still), so every pass of the header carves a
// visible trench with a golden side wall — the rows are the game. Wind lives
// in the RENDERER, not the palette: render_world travels the wave phase
// through space, because a palette cycle can only pulse a static pattern in
// place (tried that twice, rejected twice). The wheat ramp only breathes.
//
// Indexed pipeline, custom palette in 80..151 + 232..255 (16..79 gray ramp
// untouched — shell overlay safe). No fail state, no timer pressure: clear
// the field, watch the waves, get a card, get a bigger field.

#include "gos.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- world -------------------------------------------------------------------
// WORLD is sized so the texture edge lies beyond visual range from any
// REACHABLE position (gates stop road travel just past the corner
// crossroads): every pixel the player can ever see is baked terrain with
// true relief. Never try to blend the boundary instead — it shows.
#define WORLD      768          // world texture is WORLD x WORLD 8bpp texels
#define GRID       32           // coarse remain grid (completion + markers)
#define CELL       (WORLD / GRID)

// --- projection ------------------------------------------------------------------
// North-up top-down: world north IS screen up (rotation 0 — a tilted view kept
// reading as "everything's offset", user-reported twice). The vertical squash
// keeps the standing-crop relief. Affine world->screen:
//   sx = (wx*ISO_C - wy*ISO_S)*ISO_SC            - camx
//   sy = (wx*ISO_S + wy*ISO_C)*ISO_SC*ISO_T      - camy - height
#define ISO_C      1.0f         // cos 0
#define ISO_S      0.0f         // sin 0
#define ISO_SC     1.15f        // world->screen scale
#define ISO_T      0.78f        // vertical squash (lower = flatter camera)
#define WHEAT_PX   6            // standing wheat height, screen px
#define TREE_PX    11           // tree canopy height, screen px
// Q16 per-screen-pixel world steps: inverse of the matrix above
#define DWX_DX     ((int32_t)(65536.0f * ISO_C / ISO_SC))
#define DWY_DX     ((int32_t)(65536.0f * -ISO_S / ISO_SC))
#define DWX_DY     ((int32_t)(65536.0f * ISO_S / (ISO_SC * ISO_T)))
#define DWY_DY     ((int32_t)(65536.0f * ISO_C / (ISO_SC * ISO_T)))
#define WHEAT_OFFXQ (WHEAT_PX * DWX_DY)
#define WHEAT_OFFYQ (WHEAT_PX * DWY_DY)
// intermediate stalk-wall sample heights (see render_world)
#define STALK4_XQ   (4 * DWX_DY)
#define STALK4_YQ   (4 * DWY_DY)
#define STALK2_XQ   (2 * DWX_DY)
#define STALK2_YQ   (2 * DWY_DY)
#define TREE_OFFXQ  (TREE_PX * DWX_DY)
#define TREE_OFFYQ  (TREE_PX * DWY_DY)

// --- palette map (0..15 defaults kept, 16..79 reserved for the shell) --------
#define P_MACH   80    // combine body red ramp (8)
#define P_CREAM  88    // header / cab cream-steel ramp (8)
#define P_GRASS  96    // margin grass (8)
#define P_SOIL   104   // headland dirt (8)
#define P_STUB   112   // cut stubble straw (16)
#define P_WATER  128   // pond (8, shimmer-animated)
#define P_TREE   136   // tree canopy (8)
#define P_DUST   144   // chaff / dust (8)
#define P_ROAD   152   // gravel roads (8)
#define P_FAR    160   // neighbour plots beyond the field (8)
#define P_CONC   168   // concrete turbine pads (8, collides)
#define P_GLASS  176   // cab glass tint (8, static — NOT the live pond ramp)
#define P_WSIDE  232   // standing-wheat side wall (8)
#define P_WHEAT  240   // standing-wheat top (16, wind in the renderer)

#define IS_TREE(v)  ((uint8_t)((v) - P_TREE) < 8)
#define IS_WATER(v) ((uint8_t)((v) - P_WATER) < 8)
#define IS_CONC(v)  ((uint8_t)((v) - P_CONC) < 8)

// --- machine ------------------------------------------------------------------
// proportions: compact machine, WIDE header (~3x the body) — the tool selling
// the scale, per user direction
#define HEADER_X   7.0f         // header cut line, forward of center
#define HEADER_HW  13           // half swath width in world texels (26 wide)
#define WHEEL_Y    4.6f         // wheel track lateral offset from center
#define WHEEL_X    -4.0f        // wheel track aft of center
// marble-on-a-plane steering: the tilt vector is a screen-space direction —
// the combine turns toward it and tilt magnitude sets the speed; flat = stop
#define HV_DEAD    0.12f        // tilt magnitude below this = come to rest
#define TURN_MAX   2.0f         // rad/s toward the tilt direction
#define ERR_GAIN   3.0f         // turn-rate proportional gain on heading error
#define SPD_MAX    33.0f        // world px/s at full tilt (unhurried, on purpose)
#define ACRE_TEXELS 12000

enum { HV_BRIEF, HV_PLAY, HV_CELEB, HV_DONE };

#define NPART 48
typedef struct { float x, y, h, vx, vy, vh; uint8_t life, kind; } hv_part_t;

typedef struct {
    uint8_t  ver, _pad[3];
    uint32_t fields;            // lifetime fields cleared
    uint32_t acres_x10;         // lifetime acres * 10
} hv_save_t;

typedef struct {
    int      st;
    int      field;             // 1-based current field number
    int      fsize;             // active field square, world texels
    int      f0;                // field origin (centered in the world)

    float    px, py;            // combine center, world
    float    hx, hy;            // last stamped header center (cut continuity)
    float    heading;           // radians, 0 = +x
    float    speed;
    float    camx, camy;
    bool     cam_init;

    uint32_t total_wheat, cut_wheat;
    uint16_t remain[GRID * GRID];
    int      milestone;         // next chime percent (25/50/75)
    float    intake;            // smoothed texels/frame being cut

    int64_t  t_start_us;        // field start (suspend-compensated)
    int64_t  t_sus_us;
    int64_t  t_celeb_us;
    uint32_t field_ms;          // finished-field time for the card

    hv_part_t part[NPART];
    int16_t  twx[2], twy[2];    // last track-pressed texel per wheel

    // scenery structures (drawn as vector art over the terrain)
    int16_t  polex[24], poley[24];   // power line (runs past the world edge)
    uint16_t turbx[3], turby[3];     // wind turbines (pad collides via P_CONC)
    uint8_t  npoles, nturb;
    uint16_t road_ax, road_by;       // road centerlines (straight — grid roads)
    int      mark_bx, mark_by;  // straggler edge-marker block (-1 = none)
    uint32_t mark_next_ms;

    // audio
    int      h_eng, h_whine, h_cut;
    int      v_eng, v_whine, v_cut;
    uint32_t thud_ms;

    // touch tap tracking
    bool     was_touch;
    bool     a_latch;        // BOOT edge already consumed for this snapshot
    gos_pt_t tp_last;
    int      tmove;
    uint32_t flash_ms;          // recenter feedback

    hv_save_t sv;

    uint8_t  world[WORLD * WORLD];
} hv_t;

// --- palette ------------------------------------------------------------------
typedef struct { uint8_t r, g, b; } hv_rgb_t;
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static void hv_ramp(int base, int n, hv_rgb_t a, hv_rgb_t b)
{
    uint16_t buf[16];
    for (int i = 0; i < n; i++) {
        float t = n > 1 ? (float)i / (n - 1) : 0;
        buf[i] = RGB565((int)(a.r + (b.r - a.r) * t),
                        (int)(a.g + (b.g - a.g) * t),
                        (int)(a.b + (b.b - a.b) * t));
    }
    gos_gfx_set_palette(buf, n, base);
}

static void hv_fixed_palette(void)
{
    hv_ramp(P_MACH,  8, (hv_rgb_t){70, 16, 12},   (hv_rgb_t){235, 72, 48});
    hv_ramp(P_CREAM, 8, (hv_rgb_t){96, 84, 62},   (hv_rgb_t){245, 236, 212});
    hv_ramp(P_GRASS, 8, (hv_rgb_t){34, 56, 28},   (hv_rgb_t){96, 122, 58});
    hv_ramp(P_SOIL,  8, (hv_rgb_t){56, 38, 24},   (hv_rgb_t){126, 96, 60});
    hv_ramp(P_STUB, 16, (hv_rgb_t){124, 96, 50},  (hv_rgb_t){212, 180, 108});
    hv_ramp(P_TREE,  8, (hv_rgb_t){16, 36, 18},   (hv_rgb_t){78, 128, 54});
    hv_ramp(P_DUST,  8, (hv_rgb_t){150, 128, 94}, (hv_rgb_t){238, 224, 192});
    hv_ramp(P_ROAD,  8, (hv_rgb_t){92, 86, 74},   (hv_rgb_t){172, 162, 142});
    hv_ramp(P_FAR,   8, (hv_rgb_t){88, 102, 46},  (hv_rgb_t){182, 178, 94});
    hv_ramp(P_CONC,  8, (hv_rgb_t){66, 68, 72},   (hv_rgb_t){152, 154, 152});
    hv_ramp(P_GLASS, 8, (hv_rgb_t){28, 58, 66},   (hv_rgb_t){120, 176, 188});
    hv_ramp(P_WSIDE, 8, (hv_rgb_t){118, 86, 30},  (hv_rgb_t){172, 132, 52});
}

// wheat + water ramps are rewritten every frame: the wind and the shimmer
static void hv_live_palette(uint32_t ms)
{
    uint16_t buf[16];
    float ph = ms * 0.0011f;
    for (int i = 0; i < 16; i++) {
        float t = i / 15.0f;
        float r = 199 + (247 - 199) * t;
        float g = 157 + (215 - 157) * t;
        float b = 62  + (118 - 62)  * t;
        // whisper of breathing only — the actual gust motion happens in the
        // renderer (render_world), where the wave phase travels in space
        float w = 1.0f + 0.04f * sinf(ph - i * 0.55f);
        int ri = (int)(r * w), gi = (int)(g * w), bi = (int)(b * w);
        if (ri > 255) ri = 255;
        if (gi > 255) gi = 255;
        if (bi > 255) bi = 255;
        buf[i] = RGB565(ri, gi, bi);
    }
    gos_gfx_set_palette(buf, 16, P_WHEAT);

    for (int i = 0; i < 8; i++) {
        float t = i / 7.0f;
        float w = 1.0f + 0.07f * sinf(ms * 0.0035f - i * 1.1f);
        int ri = (int)((20 + 100 * t) * w);
        int gi = (int)((46 + 140 * t) * w);
        int bi = (int)((76 + 134 * t) * w);
        if (bi > 255) bi = 255;
        buf[i] = RGB565(ri, gi, bi);
    }
    gos_gfx_set_palette(buf, 8, P_WATER);
}

// --- sfx ------------------------------------------------------------------------
#define SFX(name, prio_, ...) \
    static const gos_note_t name##_n[] = { __VA_ARGS__ }; \
    static const gos_sfx_t name = { name##_n, \
        (uint8_t)(sizeof(name##_n) / sizeof(gos_note_t)), prio_ };

SFX(sfx_eng, 1, {54, 54, 92, GOS_W_SQ50, 120}, {50, 50, 88, GOS_W_SQ50, 106},
                {56, 56, 90, GOS_W_SQ50, 116}, {48, 48, 86, GOS_W_SQ50, 102})
SFX(sfx_whine, 1, {164, 169, 240, GOS_W_TRI, 84}, {169, 163, 260, GOS_W_TRI, 80})
SFX(sfx_cut, 1, {760, 520, 140, GOS_W_NOISE, 86}, {600, 760, 120, GOS_W_NOISE, 78})
SFX(sfx_thud, 2, {75, 46, 70, GOS_W_SQ50, 130}, {52, 52, 55, GOS_W_TRI, 70})
SFX(sfx_tick, 1, {1400, 1400, 14, GOS_W_TRI, 55})
SFX(sfx_mile, 3, {1046, 1046, 70, GOS_W_TRI, 105}, {1318, 1318, 95, GOS_W_TRI, 105})
SFX(sfx_start, 3, {523, 523, 60, GOS_W_TRI, 90}, {784, 784, 90, GOS_W_TRI, 95})
SFX(sfx_done, 4, {784, 784, 90, GOS_W_TRI, 110}, {880, 880, 90, GOS_W_TRI, 110},
                 {1046, 1046, 90, GOS_W_SQ25, 100}, {1174, 1174, 90, GOS_W_TRI, 110},
                 {1568, 1568, 240, GOS_W_TRI, 115})

// --- helpers --------------------------------------------------------------------
static inline uint32_t hv_hash(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline void hv_proj(const hv_t *g, float wx, float wy, float h,
                           int *sx, int *sy)
{
    *sx = (int)((wx * ISO_C - wy * ISO_S) * ISO_SC - g->camx + 0.5f);
    *sy = (int)((wx * ISO_S + wy * ISO_C) * ISO_SC * ISO_T - g->camy - h + 0.5f);
}

// --- field generation ------------------------------------------------------------
// soil apron: bare dirt for APRON px around every obstacle so no wheat grows
// where the header (collision-probed) can never reach it — the last 2% must
// always be cuttable
#define APRON 16

static void hv_soil_disc(hv_t *g, int cx, int cy, int r)
{
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            if ((uint32_t)x >= WORLD || (uint32_t)y >= WORLD) continue;
            int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            int edge = (int)(hv_hash(x, y) % 4);
            if (d < (r - edge) * (r - edge))
                g->world[y * WORLD + x] = P_SOIL + 2 + (hv_hash(x, y) & 3);
        }
}

// The countryside beyond the parcel: rolling hills. A long-wave height field
// gives every texel a hillshade (sunlit NW slopes bright, SE shade dark) and
// domain-warps the plot lattice so field boundaries drape around the hills
// instead of sitting on a perfect grid (user: "not so perfectly gridded, go
// around hills"). Pure function of (x, y, field) — the generator bakes it
// in-world and the render loop continues it past the world edge, seamlessly.
static int hv_hills(int x, int y)
{
    return 5 * gos_sin256((uint8_t)((x >> 1) + (y >> 2) + 40))
         + 4 * gos_sin256((uint8_t)((y >> 1) - (x >> 2) + 130))
         + 3 * gos_sin256((uint8_t)(((x + y) >> 2) + 200));
}

static uint8_t hv_country_px(const hv_t *g, int x, int y, int field)
{
    uint32_t h = hv_hash(x, y);
    int Hc = hv_hills(x, y);
    int Ho = hv_hills(x + 4, y + 4);
    int ds = (Hc - Ho) >> 13;               // hillshade, sun from the NW
    if (ds < -3) ds = -3;
    if (ds > 3) ds = 3;
    int wx2 = x + ((Hc * 2) >> 15);         // plots drift with the terrain
    int wy2 = y + (((Hc - 2 * Ho) * 2) >> 15);
    // ONE survey grid for the whole county: lattice pitch = the home parcel
    // (field + margins) anchored at the roads, so the home field occupies
    // exactly one grid cell, the roads run on grid lines, and every
    // neighbouring field is the same size as the one being cut (smaller
    // odd-grid parcels next to a giant home field broke the fiction)
    int pitch = g->fsize + 36;
    int rx = wx2 - (int)g->road_ax, ry = wy2 - (int)g->road_by;
    int cx2 = rx >= 0 ? rx / pitch : (rx - pitch + 1) / pitch;
    int cy2 = ry >= 0 ? ry / pitch : (ry - pitch + 1) / pitch;
    uint32_t hp = hv_hash(cx2 * 3 + 1, cy2 + field * 7);
    int bx2 = rx - cx2 * pitch, by2 = ry - cy2 * pitch;
    if (bx2 < 2 && hp % 3 == 0)        return P_TREE + 2 + (h & 3);   // hedgerow
    if (by2 < 2 && (hp >> 4) % 3 == 0) return P_TREE + 2 + (h & 3);
    if (bx2 < 2 || by2 < 2)            return P_GRASS + (h & 1);      // worn line
    int idx;
    if (((hp >> 8) & 7) < 3) {              // a big field of grass
        idx = 2 + (int)(h & 3) + ds;
        if (idx < 0) idx = 0;
        if (idx > 7) idx = 7;
        return P_GRASS + (uint8_t)idx;
    }
    // ...but mostly the neighbours farm the same thing: standing wheat
    int wb = gos_sin256((uint8_t)(x + 2 * y));
    idx = 6 + (wb * 3) / 16384 + (int)(h & 3) + ds * 2;
    if (idx < 1) idx = 1;
    if (idx > 13) idx = 13;
    return P_WHEAT + (uint8_t)idx;
}

// section roads run on EVERY survey line of the grid (pitch = parcel size),
// modular in both axes, so the road network repeats to the horizon in every
// direction. Returns the distance to the nearest survey road line.
static int hv_road_d(const hv_t *g, int v, int anchor)
{
    int P = g->fsize + 36;
    int r = (v - anchor) % P;
    if (r < 0) r += P;
    return r < P - r ? r : P - r;
}

// road cross-section by distance from the centerline: gravel body, worn
// wheel strips at +-2, dark edges at +-4 (verge handled by the caller)
static uint8_t hv_road_px(int d, uint32_t h)
{
    if (d == 4) return P_ROAD + 1 + (h & 1);
    if (d == 2) return P_ROAD + 5 + (h & 1);
    return P_ROAD + 3 + (h & 1);
}

static void hv_stamp_pond(hv_t *g, gos_rng_t *rng, int cx, int cy, int r)
{
    int cx2 = cx + gos_rand_range(rng, -r, r), cy2 = cy + gos_rand_range(rng, -r / 2, r / 2);
    int r2 = r * 3 / 4;
    hv_soil_disc(g, cx, cy, r + APRON);
    hv_soil_disc(g, cx2, cy2, r2 + APRON);
    int x0 = cx - r * 2, x1 = cx + r * 2, y0 = cy - r * 2, y1 = cy + r * 2;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if ((uint32_t)x >= WORLD || (uint32_t)y >= WORLD) continue;
            int da = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            int db = (x - cx2) * (x - cx2) + (y - cy2) * (y - cy2);
            if (da < (r - 2) * (r - 2) || db < (r2 - 2) * (r2 - 2))
                g->world[y * WORLD + x] = P_WATER + 2 + (hv_hash(x, y + 7) & 3);
        }
}

static void hv_stamp_trees(hv_t *g, gos_rng_t *rng, int cx, int cy, int n)
{
    int tx[10], ty[10], tr[10];
    if (n > 10) n = 10;
    for (int k = 0; k < n; k++) {
        tx[k] = cx + gos_rand_range(rng, -18, 18);
        ty[k] = cy + gos_rand_range(rng, -18, 18);
        tr[k] = gos_rand_range(rng, 5, 9);
    }
    for (int k = 0; k < n; k++)              // aprons first, then canopies
        hv_soil_disc(g, tx[k], ty[k], tr[k] + APRON);
    for (int k = 0; k < n; k++)
        for (int y = ty[k] - tr[k]; y <= ty[k] + tr[k]; y++)
            for (int x = tx[k] - tr[k]; x <= tx[k] + tr[k]; x++) {
                if ((uint32_t)x >= WORLD || (uint32_t)y >= WORLD) continue;
                int d = (x - tx[k]) * (x - tx[k]) + (y - ty[k]) * (y - ty[k]);
                if (d <= tr[k] * tr[k])
                    g->world[y * WORLD + x] =
                        P_TREE + 2 + ((hv_hash(x, y) >> 3) % 5);
            }
}

static void hv_gen_field(hv_t *g, gos_rng_t *rng)
{
    g->field++;
    int fs = 280 + g->field * 26;
    if (fs > 384) fs = 384;      // cap so all four section roads fit in-world
    g->fsize = fs;
    int f0 = (WORLD - fs) / 2;   // field centered in the world: room to
    g->f0 = f0;                  // overshoot on EVERY side, no world-edge bonk

    // the survey grid IS a road grid: section roads on every survey line
    // (pitch = parcel size), so the home parcel is one road-bounded block
    // exactly like every neighbouring field — no more "field plopped down
    // inside somebody else's grid" (user-reported twice)
    g->road_ax = (uint16_t)(f0 + fs + 18);   // east road; west = ax - pitch
    g->road_by = (uint16_t)(f0 - 18);        // north road; south = by + pitch

    // base terrain: the field (grass verge -> headland -> wheat) sits in a
    // countryside of domain-warped plots — hay, pasture, plough — divided
    // by worn lines and the odd hedgerow (regular grids read as a
    // checkerboard, user-reported; warp the lattice, vary the crops)
    for (int y = 0; y < WORLD; y++)
        for (int x = 0; x < WORLD; x++) {
            uint32_t h = hv_hash(x, y);
            uint8_t v;
            int fx = x - f0, fy = y - f0;
            if (fx >= 0 && fy >= 0 && fx < fs && fy < fs) {
                int d = fx < fy ? fx : fy;
                if (fs - 1 - fx < d) d = fs - 1 - fx;
                if (fs - 1 - fy < d) d = fs - 1 - fy;
                if (d < 6)
                    v = P_GRASS + 1 + (h & 3) + ((h >> 4) % 3 == 0 ? 2 : 0);
                else if (d < 16)
                    v = P_SOIL + 2 + (h & 3);
                else {
                    // static texture only — the moving wind is added per
                    // pixel in the renderer, where it can actually travel
                    int wb = gos_sin256((uint8_t)(x + 2 * y));
                    int sh = 6 + (wb * 3) / 16384 + (int)(h & 3);
                    v = P_WHEAT + (uint8_t)sh;
                }
            } else {
                // outside the crop: section roads first, then their mowed
                // verges, then the rest of the home cell as farm grass —
                // the parcel fills its whole survey cell wall-to-wall, so
                // no countryside sliver can ever wedge in beside it
                int rdx = hv_road_d(g, x, (int)g->road_ax);
                int rdy = hv_road_d(g, y, (int)g->road_by);
                int rd = rdx < rdy ? rdx : rdy;
                bool homecell =
                    x >= f0 - 18 && x <= f0 + fs + 18 &&
                    y >= f0 - 18 && y <= f0 + fs + 18;
                if (rd <= 4)
                    v = hv_road_px(rd, h);
                else if (rd <= 6 || homecell)
                    v = P_GRASS + 2 + (h & 1);           // verge / farm margin
                else
                    v = hv_country_px(g, x, y, g->field);   // countryside
            }
            g->world[y * WORLD + x] = v;
        }

    // obstacles ramp in from field 2 (field 1 is a clean zen mow). All of
    // them — ponds, tree clumps, turbines — share one placement pass with a
    // 90-px minimum spacing (blocking shapes then always leave a corridor
    // wider than the swath) and a keep-out around the start corner; crowded
    // random spawns made small fields fiddly rather than zen
    g->nturb = 0;
    {
        int nobs = g->field - 1;
        if (nobs > 4) nobs = 4;
        int wantturb = g->field >= 2 ? (g->field >= 5 ? 2 : 1) : 0;
        int ocx[8], ocy[8], noc = 0;
        for (int k = 0; k < nobs + wantturb; k++) {
            bool turb = k >= nobs;
            int ox = 0, oy = 0, ok = 0;
            for (int t = 0; t < 24 && !ok; t++) {
                ox = f0 + gos_rand_range(rng, 70, fs - 70);
                oy = f0 + gos_rand_range(rng, 70, fs - 70);
                ok = 1;
                int dsx = ox - (f0 + 26), dsy = oy - (f0 + fs - 26);
                if (dsx * dsx + dsy * dsy < 110 * 110) { ok = 0; continue; }
                for (int i = 0; i < noc; i++) {
                    int ddx = ox - ocx[i], ddy = oy - ocy[i];
                    if (ddx * ddx + ddy * ddy < 90 * 90) { ok = 0; break; }
                }
            }
            if (!ok) continue;         // field too small for another one
            ocx[noc] = ox; ocy[noc] = oy; noc++;
            if (turb && g->nturb < 3) {
                hv_soil_disc(g, ox, oy, 4 + APRON);
                for (int yy = oy - 4; yy <= oy + 4; yy++)
                    for (int xx = ox - 4; xx <= ox + 4; xx++)
                        if ((xx - ox) * (xx - ox) + (yy - oy) * (yy - oy) <= 12)
                            g->world[yy * WORLD + xx] =
                                P_CONC + 3 + (hv_hash(xx, yy) & 3);
                g->turbx[g->nturb] = (uint16_t)ox;
                g->turby[g->nturb] = (uint16_t)oy;
                g->nturb++;
            } else if (k & 1)
                hv_stamp_pond(g, rng, ox, oy, gos_rand_range(rng, 13, 20));
            else
                hv_stamp_trees(g, rng, ox, oy, gos_rand_range(rng, 5, 8));
        }
    }

    // trees dotted through the neighbour plots (no aprons out here)
    for (int k = 0; k < 9; k++) {
        int tx = gos_rand_range(rng, 12, WORLD - 12);
        int ty = gos_rand_range(rng, 12, WORLD - 12);
        if (tx > f0 - 14 && tx < f0 + fs + 14 &&
            ty > f0 - 14 && ty < f0 + fs + 14) continue;
        // ...and never over the combine's start pose (see g->px/py below). A
        // canopy there makes hv_blocked_at true for the machine CENTRE, and
        // the move gate then zeroes speed on every frame while the probes
        // stay clear so speed keeps ramping: rotation cannot escape a
        // blockage at the centre, so the field is unrecoverable.
        int sdx = tx - (f0 - 12), sdy = ty - (f0 + fs - 26);
        if (sdx * sdx + sdy * sdy < 44 * 44) continue;
        int r = gos_rand_range(rng, 4, 8);
        for (int yy = ty - r; yy <= ty + r; yy++)
            for (int xx = tx - r; xx <= tx + r; xx++) {
                if ((uint32_t)xx >= WORLD || (uint32_t)yy >= WORLD) continue;
                uint8_t *p = &g->world[yy * WORLD + xx];
                bool open = (uint8_t)(*p - P_GRASS) < 8 || (uint8_t)(*p - P_FAR) < 8;
                if (open && (xx - tx) * (xx - tx) + (yy - ty) * (yy - ty) <= r * r)
                    *p = P_TREE + 2 + ((hv_hash(xx, yy) >> 3) % 5);
            }
    }

    // power line strung along the east side of the long road, running past
    // the world edge in BOTH directions so it reads as continuing to the
    // horizon (poles are pure vector art — they don't need world texels)
    g->npoles = 0;
    for (int y = -80; y < WORLD + 80 && g->npoles < 24;
         y += 40 + (int)(hv_hash(y, 5) % 9)) {
        int px_ = (int)g->road_ax + 10 + (int)(hv_hash(y, 9) % 4);
        int py_ = y;
        if (px_ > f0 - 8 && px_ < f0 + fs + 8 &&
            py_ > f0 - 8 && py_ < f0 + fs + 8)
            continue;    // never plant a pole in the crop
        g->polex[g->npoles] = (int16_t)px_;
        g->poley[g->npoles] = (int16_t)py_;
        g->npoles++;
    }

    // census
    g->total_wheat = 0;
    memset(g->remain, 0, sizeof g->remain);
    for (int y = f0; y < f0 + fs; y++)
        for (int x = f0; x < f0 + fs; x++)
            if (g->world[y * WORLD + x] >= P_WHEAT) {
                g->total_wheat++;
                g->remain[(y / CELL) * GRID + x / CELL]++;
            }
    g->cut_wheat = 0;
    g->milestone = 25;
    g->intake = 0;
    g->mark_bx = -1;

    // combine starts OFF the crop, on the grass lane west of the field's
    // bottom corner, facing in — like it just came through the gate
    g->px = f0 - 12; g->py = f0 + fs - 26;
    g->hx = g->px + cosf(0) * HEADER_X; g->hy = g->py;
    g->heading = 0;
    g->speed = 0;
    g->cam_init = false;
    g->twx[0] = g->twx[1] = g->twy[0] = g->twy[1] = -1;
    for (int i = 0; i < NPART; i++) g->part[i].life = 0;
}

// --- cutting ---------------------------------------------------------------------
static uint32_t hv_stamp_span(hv_t *g, float cx, float cy, float pxn, float pyn)
{
    uint32_t cut = 0;
    for (int oi = -HEADER_HW; oi <= HEADER_HW; oi++) {
        int x = (int)(cx + pxn * oi + 0.5f);
        int y = (int)(cy + pyn * oi + 0.5f);
        // only ever cut the HOME field — the neighbours' standing wheat is
        // scenery, and the completion math counts home texels only
        if (x < g->f0 || y < g->f0 ||
            x >= g->f0 + g->fsize || y >= g->f0 + g->fsize) continue;
        uint8_t *p = &g->world[y * WORLD + x];
        if (*p < P_WHEAT) continue;
        uint32_t h = hv_hash(x, y);
        uint8_t v;
        int ao = oi < 0 ? -oi : oi;
        if (ao <= 2)                       v = P_STUB + 2 + (h & 1);   // straw swath
        else if (((oi + HEADER_HW) % 6) < 3) v = P_STUB + 9 + (h & 1); // light row
        else                               v = P_STUB + 6 + (h & 1);   // dark row
        *p = v;
        cut++;
        uint16_t *r = &g->remain[(y / CELL) * GRID + x / CELL];
        if (*r) (*r)--;
    }
    return cut;
}

// forward march of the header from its last stamped position to `to`,
// stamping swath spans every ~0.6 texels so no gaps open at speed. The span
// runs along the machine's LATERAL axis (latx, laty) — never the path
// perpendicular: during a fast turn the header-center path rotates ahead of
// the machine and a path-perpendicular span cuts like an invisible scythe
// leading the turn (user-reported).
static uint32_t hv_cut_to(hv_t *g, float tox, float toy, float latx, float laty)
{
    float dx = tox - g->hx, dy = toy - g->hy;
    float d = sqrtf(dx * dx + dy * dy);
    uint32_t cut = 0;
    if (d < 0.01f) return 0;
    int n = (int)(d / 0.6f) + 1;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n;
        cut += hv_stamp_span(g, g->hx + dx * t, g->hy + dy * t, latx, laty);
    }
    g->hx = tox; g->hy = toy;
    return cut;
}

// wheel tracks: a light multiplicative press — step the texel one shade down
// its own material ramp, so the surface pattern shows through the track and
// nothing turns into a hard dark line. One press per texel per pass (the
// last-texel latch); driving the same line again later deepens it one more
// shade. The world texture is the memory, so tracks persist all field.
static void hv_press_track(hv_t *g, int wi, float wx, float wy)
{
    int x = (int)(wx + 0.5f), y = (int)(wy + 0.5f);
    if (x == g->twx[wi] && y == g->twy[wi]) return;
    g->twx[wi] = (int16_t)x; g->twy[wi] = (int16_t)y;
    for (int k = 0; k < 2; k++) {
        int xx = x + k;
        if ((uint32_t)xx >= WORLD || (uint32_t)y >= WORLD) continue;
        uint8_t *p = &g->world[y * WORLD + xx];
        uint8_t v = *p;
        if ((uint8_t)(v - P_STUB) < 16)      { if (v > P_STUB + 1)  *p = v - 2; }
        else if ((uint8_t)(v - P_GRASS) < 8) { if (v > P_GRASS)     *p = v - 1; }
        else if ((uint8_t)(v - P_SOIL) < 8)  { if (v > P_SOIL)      *p = v - 1; }
    }
}

static void hv_tracks(hv_t *g, float c, float s)
{
    for (int w = 0; w < 2; w++) {
        float side = w ? WHEEL_Y : -WHEEL_Y;
        hv_press_track(g, w, g->px + c * WHEEL_X - s * side,
                             g->py + s * WHEEL_X + c * side);
    }
}

// --- collision --------------------------------------------------------------------
// where the machine may drive: the home parcel (field + collar) and the two
// roads it fronts onto. Everything beyond is the neighbours' land, behind
// barbed wire (hv_draw_fences draws the wire exactly on these boundaries).
static bool hv_accessible(const hv_t *g, float wx, float wy)
{
    float lo = (float)(g->f0 - 17), hi = (float)(g->f0 + g->fsize + 17);
    if (wx >= lo && wx <= hi && wy >= lo && wy <= hi) return true;
    // the four ring roads are drivable up to the farm gates 16 px past each
    // crossroads — travel beyond is gated so the player can never carry the
    // camera within sight of the world-texture edge
    float P = (float)(g->fsize + 36);
    float ax = (float)g->road_ax, wl = ax - P;
    float by = (float)g->road_by, sl = by + P;
    bool onv = (fabsf(wx - ax) <= 7 || fabsf(wx - wl) <= 7) &&
               wy >= by - 16 && wy <= sl + 16;
    bool onh = (fabsf(wy - by) <= 7 || fabsf(wy - sl) <= 7) &&
               wx >= wl - 16 && wx <= ax + 16;
    return onv || onh;
}

static bool hv_blocked_at(const hv_t *g, float wx, float wy)
{
    int x = (int)wx, y = (int)wy;
    if (x < 2 || y < 2 || x >= WORLD - 2 || y >= WORLD - 2) return true;
    if (!hv_accessible(g, wx, wy)) return true;
    uint8_t v = g->world[y * WORLD + x];
    if (IS_WATER(v) || IS_TREE(v) || IS_CONC(v)) return true;
    for (int i = 0; i < g->npoles; i++) {
        float dx = wx - g->polex[i], dy = wy - g->poley[i];
        if (dx * dx + dy * dy < 5.0f) return true;
    }
    return false;
}

// header probe bitmask: 1 = +lateral tip, 2 = center, 4 = -lateral tip.
// A single blocked tip is a GRAZE — the machine deflects and shoulders past
// instead of slamming to a stop (a full stop on every pond kiss was
// frustrating, not zen).
static int hv_probe_mask(const hv_t *g, float nx, float ny, float c, float s)
{
    float bx = nx + c * (HEADER_X + 1.5f), by = ny + s * (HEADER_X + 1.5f);
    float px = -s, py = c;
    int m = 0;
    if (hv_blocked_at(g, bx + px * HEADER_HW, by + py * HEADER_HW)) m |= 1;
    if (hv_blocked_at(g, bx, by)) m |= 2;
    if (hv_blocked_at(g, bx - px * HEADER_HW, by - py * HEADER_HW)) m |= 4;
    return m;
}

// --- particles ---------------------------------------------------------------------
static void hv_spawn_part(hv_t *g, float x, float y, float h,
                          float vx, float vy, float vh, uint8_t life, uint8_t kind)
{
    for (int i = 0; i < NPART; i++)
        if (g->part[i].life == 0) {
            g->part[i] = (hv_part_t){ x, y, h, vx, vy, vh, life, kind };
            return;
        }
}

static void hv_update_parts(hv_t *g, float dt)
{
    for (int i = 0; i < NPART; i++) {
        hv_part_t *p = &g->part[i];
        if (!p->life) continue;
        p->x += p->vx * dt; p->y += p->vy * dt;
        p->h += p->vh * dt; p->vh -= 26.0f * dt;
        if (p->h < 0) p->h = 0;
        p->life--;
    }
}

// --- rendering ----------------------------------------------------------------------
// elevated-plane sample with edge clamping: a crop field can straddle the
// world-texture boundary (the countryside continues procedurally past it) —
// bounds-checking to 0 there drew a phantom crop WALL across the middle of a
// continuous field. Clamp within a small margin; deep outside returns 0 and
// falls through to the procedural ground path.
static inline uint8_t hv_relief_samp(const uint8_t *w, int32_t xq, int32_t yq)
{
    int x = (int)(xq >> 16), y = (int)(yq >> 16);
    if (x < -12 || y < -12 || x >= WORLD + 12 || y >= WORLD + 12) return 0;
    if (x < 0) x = 0; else if (x >= WORLD) x = WORLD - 1;
    if (y < 0) y = 0; else if (y >= WORLD) y = WORLD - 1;
    return w[y * WORLD + x];
}

static void render_world(hv_t *g)
{
    uint8_t *fb = gos_gfx_fb();
    const uint8_t *w = g->world;
    // travelling wind: two waves whose PHASE advances with time, so the gust
    // pattern physically moves across the crop (palette tricks can only pulse
    // a static pattern in place — user-rejected twice). Both waves drift the
    // same general direction at different speeds; cost is 2 Q14 table lookups
    // per wheat pixel.
    uint32_t wms = gos_time_ms();
    int t1 = (int)(wms >> 4), t2 = (int)(wms >> 5);
    for (int y = 0; y < GOS_SCREEN_H; y++) {
        float u0 = g->camx;                    // screen coords of pixel (0,y)
        float v0 = (float)y + g->camy;
        int32_t wxq = (int32_t)((u0 * (ISO_C / ISO_SC) +
                                 v0 * (ISO_S / (ISO_SC * ISO_T))) * 65536.0f);
        int32_t wyq = (int32_t)((u0 * (-ISO_S / ISO_SC) +
                                 v0 * (ISO_C / (ISO_SC * ISO_T))) * 65536.0f);
        uint8_t *row = fb + y * GOS_SCREEN_W;
        for (int x = 0; x < GOS_SCREEN_W; x++) {
            uint8_t out;
            uint8_t vt = hv_relief_samp(w, wxq + TREE_OFFXQ, wyq + TREE_OFFYQ);
            if (IS_TREE(vt)) out = vt;                       // canopy top
            else {
                uint32_t hx = (uint32_t)((wxq + WHEAT_OFFXQ) >> 16);
                uint32_t hy = (uint32_t)((wyq + WHEAT_OFFYQ) >> 16);
                uint8_t vw = hv_relief_samp(w, wxq + WHEAT_OFFXQ, wyq + WHEAT_OFFYQ);
                if (vw >= P_WHEAT) {                         // wheat top + wind
                    // 2x2 ordered dither on the shade quantization: without
                    // it every pixel of a small lone tuft crosses the shade
                    // threshold on the same frame and the tuft blinks.
                    // WORLD-anchored (hx/hy, not screen x/y) so the pattern
                    // rides with the terrain instead of crawling over it
                    int dith = ((((int)hx & 1) | (((int)hy & 1) << 1)) << 11) - 3072;
                    int wsh = (int)(vw - P_WHEAT) +
                        ((gos_sin256((uint8_t)(((hx + hy) << 1) + t1)) +
                          gos_sin256((uint8_t)((hx * 3 - hy) + t2)) + dith) >> 13);
                    if (wsh < 0) wsh = 0;
                    if (wsh > 15) wsh = 15;
                    out = (uint8_t)(P_WHEAT + wsh);
                } else if (IS_TREE(vw)) out = P_TREE + 1;    // canopy side
                else {
                    // stalk wall: sample two intermediate heights too, so a
                    // thin strip of standing wheat shows a continuous stalk
                    // from head to ground (top+ground alone left a floating
                    // head over a gap on strips narrower than the crop
                    // height — user-reported). Shaded light->dark downward.
                    uint8_t v4 = hv_relief_samp(w, wxq + STALK4_XQ, wyq + STALK4_YQ);
                    uint32_t gx = (uint32_t)(wxq >> 16), gy = (uint32_t)(wyq >> 16);
                    uint8_t vg = (gx < WORLD && gy < WORLD) ? w[gy * WORLD + gx] : 0;
                    if (v4 >= P_WHEAT)
                        out = P_WSIDE + 5 + ((v4 - P_WHEAT) >> 3);
                    else if (IS_TREE(v4)) out = P_TREE + 1;
                    else {
                        uint8_t v2 = hv_relief_samp(w, wxq + STALK2_XQ, wyq + STALK2_YQ);
                        if (v2 >= P_WHEAT)
                            out = P_WSIDE + 3 + ((v2 - P_WHEAT) >> 3);
                        else if (IS_TREE(v2)) out = P_TREE + 1;
                        else if (vg >= P_WHEAT)
                            out = P_WSIDE + 1 + ((vg - P_WHEAT) >> 3);
                        else if (IS_TREE(vg)) out = P_TREE;     // trunk shade
                        else if (vg == 0) {
                            // beyond the world texture: continue the roads
                            // and the rolling countryside by the very same
                            // formulas the generator baked
                            int ix = (int)gx, iy = (int)gy;
                            int dxr = hv_road_d(g, ix, (int)g->road_ax);
                            int dyr = hv_road_d(g, iy, (int)g->road_by);
                            int ao = dxr < dyr ? dxr : dyr;
                            if (ao <= 4) {
                                out = hv_road_px(ao, (gx ^ gy) & 1);
                            } else if (ao <= 6) {
                                out = P_GRASS + 2 + ((gx ^ gy) & 1);
                            } else {
                                out = hv_country_px(g, ix, iy, g->field);
                                // crops continue past the texture edge AS
                                // crops, wind and all — the old hay remap
                                // made a straddling field look planted only
                                // along its near strip (user-reported)
                                if (out >= P_WHEAT) {
                                    int wsh = (int)(out - P_WHEAT) +
                                        ((gos_sin256((uint8_t)(((ix + iy) << 1) + t1)) +
                                          gos_sin256((uint8_t)((ix * 3 - iy) + t2))) >> 13);
                                    if (wsh < 0) wsh = 0;
                                    if (wsh > 15) wsh = 15;
                                    out = (uint8_t)(P_WHEAT + wsh);
                                }
                            }
                        } else out = vg;
                    }
                }
            }
            row[x] = out;
            wxq += DWX_DX; wyq += DWY_DX;
        }
    }
}

// one box part of the machine: local rect (forward x, lateral y), heights
// h0..h1; draws 4 side walls then the top face (painter overdraw is cheap)
static void hv_box(const hv_t *g, float c, float s,
                   float x0, float y0, float x1, float y1,
                   float h0, float h1, uint8_t ctop, uint8_t cside)
{
    float lx[4] = { x0, x1, x1, x0 }, ly[4] = { y0, y0, y1, y1 };
    int bx[4], by[4], ty[4];
    for (int i = 0; i < 4; i++) {
        float wx = g->px + lx[i] * c - ly[i] * s;
        float wy = g->py + lx[i] * s + ly[i] * c;
        int sx, sy;
        hv_proj(g, wx, wy, h0, &sx, &sy);
        bx[i] = sx; by[i] = sy;
        ty[i] = sy - (int)(h1 - h0);
    }
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        gos_pt_t q[4] = { { (int16_t)bx[i], (int16_t)by[i] },
                          { (int16_t)bx[j], (int16_t)by[j] },
                          { (int16_t)bx[j], (int16_t)ty[j] },
                          { (int16_t)bx[i], (int16_t)ty[i] } };
        gos_gfx_quad_fill(q, cside);
    }
    gos_pt_t t[4] = { { (int16_t)bx[0], (int16_t)ty[0] },
                      { (int16_t)bx[1], (int16_t)ty[1] },
                      { (int16_t)bx[2], (int16_t)ty[2] },
                      { (int16_t)bx[3], (int16_t)ty[3] } };
    gos_gfx_quad_fill(t, ctop);
}

static void hv_draw_header(const hv_t *g, float c, float s, uint32_t ms)
{
    hv_box(g, c, s, 4.0f, -(float)HEADER_HW, 7.5f, (float)HEADER_HW,
           0, 2.5f, P_CREAM + 5, P_CREAM + 2);
    // spinning reel: two bright bars chasing across the header face
    for (int k = 0; k < 2; k++) {
        float ph = ms * 0.012f + k * 3.14159f;
        float h = 2.2f + 1.4f * sinf(ph);
        float fx = 7.8f + 0.6f * cosf(ph);
        int ax, ay, bx2, by2;
        float wx = g->px + fx * c - (-(float)HEADER_HW + 1) * s;
        float wy = g->py + fx * s + (-(float)HEADER_HW + 1) * c;
        hv_proj(g, wx, wy, h, &ax, &ay);
        wx = g->px + fx * c - ((float)HEADER_HW - 1) * s;
        wy = g->py + fx * s + ((float)HEADER_HW - 1) * c;
        hv_proj(g, wx, wy, h, &bx2, &by2);
        gos_gfx_line(ax, ay, bx2, by2, k ? P_CREAM + 7 : P_MACH + 4);
    }
}

static void hv_draw_combine(hv_t *g, uint32_t ms)
{
    float c = cosf(g->heading), s = sinf(g->heading);
    // ISO_S is 0, so hv_proj's screen y depends on wy alone, and the header
    // sits HEADER_X*s along wy: it is nearer the camera exactly when s > 0.
    // (c + s) was the test for the rotated projection this renderer dropped;
    // it drew the machine inside-out across two 45-degree wedges of heading.
    bool header_front = s > 0;

    if (!header_front) hv_draw_header(g, c, s, ms);

    // wheels: big drive wheels forward, smaller steer wheels aft — dark tire
    // blocks standing proud of the body sides (same stance as the tracks
    // they press, WHEEL_Y). Body rides raised on top of them.
    for (int ws = -1; ws <= 1; ws += 2) {
        hv_box(g, c, s, 0.4f, ws * 4.6f - 0.9f, 3.6f, ws * 4.6f + 0.9f,
               0, 3.2f, GOS_DARKGRAY, GOS_BLACK);
        hv_box(g, c, s, -5.6f, ws * 4.2f - 0.8f, -3.4f, ws * 4.2f + 0.8f,
               0, 2.4f, GOS_DARKGRAY, GOS_BLACK);
    }
    hv_box(g, c, s, -6.0f, -4.5f, 4.0f, 4.5f, 2, 7, P_MACH + 5, P_MACH + 3);
    // grain tank hatch + heap as the tank fills
    hv_box(g, c, s, -5.0f, -3.2f, -0.5f, 3.2f, 7, 7.6f, P_MACH + 2, P_MACH + 2);
    if (g->cut_wheat > g->total_wheat / 50) {
        int sx, sy;
        float wx = g->px + (-2.8f) * c, wy = g->py + (-2.8f) * s;
        hv_proj(g, wx, wy, 8.2f, &sx, &sy);
        int r = 1 + (int)(3.0f * g->cut_wheat / (g->total_wheat + 1));
        gos_gfx_circle_fill(sx, sy, r, P_WHEAT + 9);
    }
    hv_box(g, c, s, 0.5f, -3.6f, 4.0f, 3.6f, 7, 10.5f, P_CREAM + 6, P_GLASS + 5);

    if (header_front) hv_draw_header(g, c, s, ms);
}

static void hv_draw_parts(const hv_t *g)
{
    for (int i = 0; i < NPART; i++) {
        const hv_part_t *p = &g->part[i];
        if (!p->life) continue;
        int sx, sy;
        hv_proj(g, p->x, p->y, p->h, &sx, &sy);
        if (p->kind == 0) {
            gos_gfx_pixel(sx, sy, P_DUST + 3 + (p->life & 3));
            if (p->life > 14) gos_gfx_pixel(sx + 1, sy, P_DUST + 5);
        } else {
            gos_gfx_pixel(sx, sy, (p->life & 2) ? GOS_WHITE : P_WHEAT + 14);
            gos_gfx_pixel(sx, sy - 1, P_WHEAT + 12);
        }
    }
}

// scenery: wooden power poles with sagging wires, and wind turbines whose
// rotors idle around slowly. Drawn in two passes bracketing the combine so
// depth sorts correctly (structures north of the machine draw behind it).
static int hv_pole_h(const hv_t *g, int i)
{ return 17 + (int)(hv_hash(g->polex[i], g->poley[i]) & 3); }

static void hv_draw_pole(const hv_t *g, int i)
{
    int bx, by;
    hv_proj(g, g->polex[i], g->poley[i], 0, &bx, &by);
    if (bx < -20 || bx > GOS_SCREEN_W + 20 || by < -30 || by > GOS_SCREEN_H + 30)
        return;
    int hh = hv_pole_h(g, i);
    gos_gfx_vline(bx, by - hh, by, P_SOIL + 1);
    gos_gfx_pixel(bx, by - hh, P_SOIL + 3);              // weathered top
    gos_gfx_hline(bx - 4, bx + 4, by - hh + 2, P_SOIL + 1);
    gos_gfx_pixel(bx - 4, by - hh + 1, P_CREAM + 6);
    gos_gfx_pixel(bx + 4, by - hh + 1, P_CREAM + 6);
}

// one catenary strand as 4 segments; sag grows with span length
static void hv_wire(int ax, int ay, int bx, int by)
{
    int dx = bx - ax, dy = by - ay;
    int span = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (ady > span) span = ady;
    int sag = 2 + span / 10;
    if (sag > 9) sag = 9;
    int px = ax, py = ay;
    for (int k = 1; k <= 4; k++) {
        float t = k / 4.0f;
        int x = ax + (int)(dx * t);
        int y = ay + (int)(dy * t + sag * 4 * t * (1 - t));
        gos_gfx_line(px, py, x, y, GOS_DARKGRAY);
        px = x; py = y;
    }
}

static void hv_draw_wires(const hv_t *g)
{
    for (int i = 0; i + 1 < g->npoles; i++) {
        int ax, ay, bx, by;
        hv_proj(g, g->polex[i], g->poley[i], (float)(hv_pole_h(g, i) - 1), &ax, &ay);
        hv_proj(g, g->polex[i + 1], g->poley[i + 1],
                (float)(hv_pole_h(g, i + 1) - 1), &bx, &by);
        if ((ax < -30 && bx < -30) || (ax > GOS_SCREEN_W + 30 && bx > GOS_SCREEN_W + 30))
            continue;
        hv_wire(ax - 4, ay, bx - 4, by);      // one strand per insulator
        hv_wire(ax + 4, ay, bx + 4, by);
    }
}

static void hv_draw_turbine(const hv_t *g, int i, uint32_t ms)
{
    int bx, by;
    hv_proj(g, g->turbx[i], g->turby[i], 0, &bx, &by);
    if (bx < -30 || bx > GOS_SCREEN_W + 30 || by < -40 || by > GOS_SCREEN_H + 40)
        return;
    int hub = by - 26;
    gos_gfx_vline(bx, hub, by, P_CREAM + 4);
    gos_gfx_vline(bx + 1, hub + 4, by, P_CREAM + 2);
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)(bx - 1), (int16_t)(hub - 1), 4, 3 },
                      P_CREAM + 6);
    uint8_t a = (uint8_t)(ms / 28) + (uint8_t)(i * 40);
    for (int k = 0; k < 3; k++) {
        uint8_t ang = (uint8_t)(a + k * 85);
        int dx = (gos_cos256(ang) * 11) >> 14;
        int dy = (gos_sin256(ang) * 11) >> 14;
        gos_gfx_line(bx, hub, bx + dx, hub + dy, P_CREAM + 7);
    }
    gos_gfx_pixel(bx, hub, GOS_DARKGRAY);
}

// barbed-wire fences: short posts on a fixed 14-texel grid with two wire
// strands between consecutive posts. Straight world-axis runs only; each
// post y-sorts against the combine like the other structures.
static void hv_fence_post(const hv_t *g, int wx, int wy, int combine_sy,
                          bool behind, int px[], int py[], int *have)
{
    int bx, by;
    hv_proj(g, wx, wy, 0, &bx, &by);
    // generous margin: a strand's far post can sit well off-screen — a tight
    // margin made wire segments pop in and out at the screen edge
    bool vis = bx >= -72 && bx <= GOS_SCREEN_W + 72 &&
               by >= -72 && by <= GOS_SCREEN_H + 72;
    if (vis && ((by <= combine_sy) == behind)) {
        gos_gfx_vline(bx, by - 5, by, P_SOIL + 1);
        gos_gfx_pixel(bx, by - 5, P_SOIL + 3);
        // ONE strand back to the previous post — two strands left a 1px gap
        // of animated wheat between them that read as strobing
        if (*have)
            gos_gfx_line(px[0], py[0] - 3, bx, by - 3, GOS_DARKGRAY);
    }
    px[0] = bx; py[0] = by; *have = vis;
}

// one straight fence run; vertical = along world y at x, else along x at y.
// Two gaps cut openings at the crossroads. Posts sit on the global 14-grid
// so runs never crawl as the camera moves.
static void hv_fence_run(const hv_t *g, bool vertical, int at, int from, int to,
                         int gap_a, int gap_b, int combine_sy, bool behind)
{
    int px[1], py[1], have = 0;
    int k0 = from / 14 - 1, k1 = to / 14 + 1;
    for (int k = k0; k <= k1; k++) {
        int c = k * 14;
        if (c < from || c > to) { have = 0; continue; }
        int ga = c - gap_a, gb = c - gap_b;
        if (ga < 0) ga = -ga;
        if (gb < 0) gb = -gb;
        if (ga < 9 || gb < 9) { have = 0; continue; }
        if (vertical) hv_fence_post(g, at, c, combine_sy, behind, px, py, &have);
        else          hv_fence_post(g, c, at, combine_sy, behind, px, py, &have);
    }
}

// a metal farm gate across a road: end posts + two rails. These sit exactly
// on the accessibility limits so "can't go further" always has a visible why
static void hv_draw_gate(const hv_t *g, int wx0, int wy0, int wx1, int wy1,
                         int combine_sy, bool behind)
{
    int ax_, ay_, bx_, by_;
    hv_proj(g, wx0, wy0, 0, &ax_, &ay_);
    hv_proj(g, wx1, wy1, 0, &bx_, &by_);
    if (ax_ < -40 || ax_ > GOS_SCREEN_W + 40 || ay_ < -40 || ay_ > GOS_SCREEN_H + 40)
        return;
    if ((ay_ <= combine_sy) != behind) return;
    gos_gfx_vline(ax_, ay_ - 6, ay_, P_CREAM + 3);
    gos_gfx_vline(bx_, by_ - 6, by_, P_CREAM + 3);
    gos_gfx_line(ax_, ay_ - 5, bx_, by_ - 5, P_CREAM + 5);
    gos_gfx_line(ax_, ay_ - 2, bx_, by_ - 2, P_CREAM + 5);
}

static void hv_draw_fences(hv_t *g, int combine_sy, bool behind)
{
    // barbed wire on the FAR side of each of the four section roads around
    // the home block — the neighbours' fences, gapped at the crossroads.
    // The home parcel itself opens onto its roads on all four sides.
    int P = g->fsize + 36;
    int ax = g->road_ax, by = g->road_by;
    int wl = ax - P, sl = by + P;    // west road x, south road y
    hv_fence_run(g, true,  ax + 8, -84, WORLD + 84, by, sl, combine_sy, behind);
    hv_fence_run(g, true,  wl - 8, -84, WORLD + 84, by, sl, combine_sy, behind);
    hv_fence_run(g, false, by - 8, -84, WORLD + 84, wl, ax, combine_sy, behind);
    hv_fence_run(g, false, sl + 8, -84, WORLD + 84, wl, ax, combine_sy, behind);
    // farm gates across each road just past the crossroads (travel limits)
    hv_draw_gate(g, wl - 16, by - 8, wl - 16, by + 8, combine_sy, behind);
    hv_draw_gate(g, ax + 16, by - 8, ax + 16, by + 8, combine_sy, behind);
    hv_draw_gate(g, wl - 16, sl - 8, wl - 16, sl + 8, combine_sy, behind);
    hv_draw_gate(g, ax + 16, sl - 8, ax + 16, sl + 8, combine_sy, behind);
    hv_draw_gate(g, wl - 8, by - 16, wl + 8, by - 16, combine_sy, behind);
    hv_draw_gate(g, ax - 8, by - 16, ax + 8, by - 16, combine_sy, behind);
    hv_draw_gate(g, wl - 8, sl + 16, wl + 8, sl + 16, combine_sy, behind);
    hv_draw_gate(g, ax - 8, sl + 16, ax + 8, sl + 16, combine_sy, behind);
}

static void hv_draw_structs(hv_t *g, uint32_t ms, int combine_sy, bool behind)
{
    hv_draw_fences(g, combine_sy, behind);
    if (behind) hv_draw_wires(g);
    for (int i = 0; i < g->npoles; i++) {
        int bx, by;
        hv_proj(g, g->polex[i], g->poley[i], 0, &bx, &by);
        if ((by <= combine_sy) == behind) hv_draw_pole(g, i);
    }
    for (int i = 0; i < g->nturb; i++) {
        int bx, by;
        hv_proj(g, g->turbx[i], g->turby[i], 0, &bx, &by);
        if ((by <= combine_sy) == behind) hv_draw_turbine(g, i, ms);
    }
}

static void hv_center_text(int font, int y, gos_color_t col, const char *s)
{
    gos_gfx_text(font, (GOS_SCREEN_W - gos_gfx_text_w(font, s)) / 2, y, col, "%s", s);
}

static void hv_draw_card(hv_t *g, uint32_t ms)
{
    gos_rect_t box = { 20, 52, 144, 122 };
    gos_gfx_rect_fill(box, GOS_GRAY64(52));
    gos_gfx_rect(box, GOS_GRAY64(18));
    gos_gfx_rect((gos_rect_t){ 22, 54, 140, 118 }, GOS_GRAY64(38));
    bool blink = (ms / 500) & 1;
    char b[40];

    if (g->st == HV_BRIEF) {
        hv_center_text(1, 62, P_WSIDE + 3, "HARVEST");
        hv_center_text(0, 88, GOS_GRAY64(4), "TILT WHERE TO GO");
        hv_center_text(0, 100, GOS_GRAY64(4), "MORE TILT = FASTER");
        hv_center_text(0, 112, GOS_GRAY64(4), "HOLD FLAT = STOP");
        hv_center_text(0, 124, GOS_GRAY64(4), "TAP = RECENTER TILT");
        hv_center_text(0, 136, GOS_GRAY64(12), "SWIPE DOWN = MENU");
        if (blink) hv_center_text(0, 156, P_WSIDE + 3, "TAP TO BEGIN");
    } else {
        hv_center_text(1, 62, P_WSIDE + 3, "FIELD CLEAR");
        snprintf(b, sizeof b, "FIELD %d  TIME %lu:%02lu", g->field,
                 (unsigned long)(g->field_ms / 60000),
                 (unsigned long)(g->field_ms / 1000 % 60));
        hv_center_text(0, 92, GOS_GRAY64(4), b);
        uint32_t a10 = g->total_wheat * 10 / ACRE_TEXELS;
        snprintf(b, sizeof b, "+%lu.%lu ACRES",
                 (unsigned long)(a10 / 10), (unsigned long)(a10 % 10));
        hv_center_text(0, 106, GOS_GRAY64(4), b);
        snprintf(b, sizeof b, "LIFETIME %lu.%lu AC",
                 (unsigned long)(g->sv.acres_x10 / 10),
                 (unsigned long)(g->sv.acres_x10 % 10));
        hv_center_text(0, 120, GOS_GRAY64(12), b);
        snprintf(b, sizeof b, "FIELDS CLEARED %lu", (unsigned long)g->sv.fields);
        hv_center_text(0, 132, GOS_GRAY64(12), b);
        if (blink) hv_center_text(0, 156, P_WSIDE + 3, "TAP FOR NEXT FIELD");
    }
}

static void hv_render(gos_ctx_t *ctx)
{
    hv_t *g = ctx->state;
    uint32_t ms = gos_time_ms();

    hv_live_palette(ms);
    render_world(g);
    hv_draw_parts(g);
    {
        int csx, csy;
        hv_proj(g, g->px, g->py, 0, &csx, &csy);
        hv_draw_structs(g, ms, csy, true);
        hv_draw_combine(g, ms);
        hv_draw_structs(g, ms, csy, false);
    }

    if (g->st == HV_BRIEF || g->st == HV_DONE) {
        hv_draw_card(g, ms);
        return;
    }

    // HUD (safe zone: x>=6, y>=6) — dark chips keep it legible over pale wheat
    int pct = g->total_wheat ? (int)(g->cut_wheat * 100 / g->total_wheat) : 0;
    char fb2[16];
    snprintf(fb2, sizeof fb2, "FIELD %d", g->field);
    int fw = gos_gfx_text_w(0, fb2);
    gos_gfx_rect_fill((gos_rect_t){ 4, 4, (int16_t)(fw + 6), 22 }, GOS_GRAY64(10));
    gos_gfx_text(0, 7, 6, GOS_WHITE, "%s", fb2);
    gos_gfx_text(0, 7, 16, P_WHEAT + 14, "%d%%", pct);
    uint32_t el = (uint32_t)((gos_time_us() - g->t_start_us) / 1000);
    char tb[12];
    snprintf(tb, sizeof tb, "%lu:%02lu",
             (unsigned long)(el / 60000), (unsigned long)(el / 1000 % 60));
    int tw = gos_gfx_text_w(0, tb);
    gos_gfx_rect_fill((gos_rect_t){ (int16_t)(GOS_SCREEN_W - 10 - tw), 4,
                                    (int16_t)(tw + 6), 12 }, GOS_GRAY64(10));
    gos_gfx_text(0, GOS_SCREEN_W - 7 - tw, 6, GOS_GRAY64(52), "%s", tb);

    // straggler pointer once the field is nearly done
    if (g->mark_bx >= 0 && ((ms / 300) & 1)) {
        int tx, ty, cx, cy;
        hv_proj(g, g->mark_bx * CELL + CELL / 2, g->mark_by * CELL + CELL / 2,
                WHEAT_PX, &tx, &ty);
        hv_proj(g, g->px, g->py, 4, &cx, &cy);
        if (tx < 10 || tx > 174 || ty < 18 || ty > 206) {
            float dx = tx - cx, dy = ty - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > 1) {
                dx /= d; dy /= d;
                float t = 1e9f;
                if (dx > 0.001f) t = fminf(t, (172.0f - cx) / dx);
                if (dx < -0.001f) t = fminf(t, (12.0f - cx) / dx);
                if (dy > 0.001f) t = fminf(t, (202.0f - cy) / dy);
                if (dy < -0.001f) t = fminf(t, (22.0f - cy) / dy);
                int mx = cx + (int)(dx * t), my = cy + (int)(dy * t);
                gos_gfx_circle_fill(mx, my, 2, P_WHEAT + 14);
                gos_gfx_pixel(mx + (int)(dx * 3), my + (int)(dy * 3), P_WHEAT + 15);
            }
        }
    }

    // recenter feedback cross
    if (ms - g->flash_ms < 350) {
        gos_gfx_hline(88, 96, 112, GOS_WHITE);
        gos_gfx_vline(92, 108, 116, GOS_WHITE);
    }

    if (g->st == HV_CELEB && ((ms / 400) & 1))
        hv_center_text(1, 40, P_WSIDE + 1, "FIELD CLEAR");
}

// --- update -----------------------------------------------------------------------
static void hv_set_loop_vols(hv_t *g)
{
    float sf = g->speed / SPD_MAX;
    if (sf > 1) sf = 1;
    int ve = 42 + (int)(sf * 44);
    int vw = (int)(sf * sf * 66);
    float inf = g->intake / 14.0f;
    if (inf > 1) inf = 1;
    int vc = (int)(inf * 84);
    if (g->st != HV_PLAY && g->st != HV_CELEB) { ve = 34; vw = 0; vc = 0; }
    // >=4-step gate keeps the mixer traffic quiet
    if (ve - g->v_eng >= 4 || g->v_eng - ve >= 4) { gos_audio_set_vol(g->h_eng, ve); g->v_eng = ve; }
    if (vw - g->v_whine >= 4 || g->v_whine - vw >= 4) { gos_audio_set_vol(g->h_whine, vw); g->v_whine = vw; }
    if (vc - g->v_cut >= 4 || g->v_cut - vc >= 4) { gos_audio_set_vol(g->h_cut, vc); g->v_cut = vc; }
}

static void hv_start_loops(hv_t *g)
{
    g->h_eng = gos_audio_loop(&sfx_eng, 34);
    g->h_whine = gos_audio_loop(&sfx_whine, 0);
    g->h_cut = gos_audio_loop(&sfx_cut, 0);
    g->v_eng = 34; g->v_whine = 0; g->v_cut = 0;
}

static void hv_save_now(gos_ctx_t *ctx)
{
    hv_t *g = ctx->state;
    g->sv.ver = 1;
    gos_save_write(ctx, &g->sv, sizeof g->sv);
}

static void hv_finish_field(gos_ctx_t *ctx)
{
    hv_t *g = ctx->state;
    // the breeze takes the stragglers: convert any leftover wheat
    for (int y = g->f0; y < g->f0 + g->fsize; y++)
        for (int x = g->f0; x < g->f0 + g->fsize; x++)
            if (g->world[y * WORLD + x] >= P_WHEAT)
                g->world[y * WORLD + x] = P_STUB + 8 + (hv_hash(x, y) & 3);
    memset(g->remain, 0, sizeof g->remain);
    g->cut_wheat = g->total_wheat;
    g->mark_bx = -1;

    g->field_ms = (uint32_t)((gos_time_us() - g->t_start_us) / 1000);
    g->sv.fields++;
    g->sv.acres_x10 += g->total_wheat * 10 / ACRE_TEXELS;
    hv_save_now(ctx);

    g->st = HV_CELEB;
    g->t_celeb_us = gos_time_us();
    gos_audio_play(&sfx_done, 200);
    for (int i = 0; i < 30; i++) {
        float a = (float)i * 0.21f;
        hv_spawn_part(g, g->px + cosf(a) * 3, g->py + sinf(a) * 3, 8,
                      cosf(a) * (6 + (i % 5) * 3), sinf(a) * (6 + (i % 5) * 3),
                      14 + (i % 7) * 3, 42 + (i % 3) * 8, 1);
    }
}

static void hv_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    hv_t *g = ctx->state;
    uint32_t ms = gos_time_ms();

    // --- tap detection (release-tap, parallax-tolerant) ---
    bool tap = false;
    if (in->touching) {
        if (!g->was_touch) g->tmove = 0;
        else g->tmove += (in->touch.x > g->tp_last.x ? in->touch.x - g->tp_last.x
                                                     : g->tp_last.x - in->touch.x)
                       + (in->touch.y > g->tp_last.y ? in->touch.y - g->tp_last.y
                                                     : g->tp_last.y - in->touch.y);
        g->tp_last = in->touch;
    } else if (g->was_touch && g->tmove < 10) {
        tap = true;
    }
    g->was_touch = in->touching;
    // shell.c runs update() up to 4x against the SAME input snapshot after a
    // frame overrun, so a press edge must be consumed once. (tap self-clears:
    // was_touch is assigned above.) Without this a single BOOT press can both
    // start play and then re-zero the tilt neutral on the catch-up update.
    bool a_edge = (in->pressed & GOS_BTN_A) != 0;
    bool act = tap || (a_edge && !g->a_latch);
    g->a_latch = a_edge;

    switch (g->st) {
    case HV_BRIEF:
        if (act) {
            g->st = HV_PLAY;
            g->t_start_us = gos_time_us();
            gos_audio_play(&sfx_start, 180);
            gos_input_calibrate_neutral();
        }
        break;

    case HV_DONE:
        if (act) {
            hv_gen_field(g, &ctx->rng);
            g->st = HV_PLAY;
            g->t_start_us = gos_time_us();
            gos_audio_play(&sfx_start, 160);
        }
        break;

    case HV_CELEB:
        if (gos_time_us() - g->t_celeb_us > 1400000) g->st = HV_DONE;
        break;

    case HV_PLAY: {
        if (act) {   // recenter tilt neutral
            gos_input_calibrate_neutral();
            g->flash_ms = ms;
            gos_audio_play(&sfx_tick, 120);
        }

        // marble-on-a-plane steering: tilt = screen-space direction to go.
        // Convert the tilt vector to a world heading through the inverse iso
        // projection, turn toward it, and let tilt magnitude set the speed
        // (scaled by alignment so a big direction change pivots before it
        // drives). Flat device = ease to a stop; no reverse gear — rotation
        // is never blocked, so tilting away always pivots you free.
        // Marble-on-a-plane needs the device's ACTUAL inclination, so read the
        // pose instead of aim_x/aim_y. Those follow the global AIM MODE: under
        // GYRO they integrate and HOLD where last pointed, under TOUCH they
        // hold the last drag — either way a device lying flat would drive the
        // combine forever and the brief card's HOLD FLAT = STOP would be a
        // lie. They also carry the INVERT toggles, which direct manipulation
        // never wants (input.c makes that same call for touch-drag). Scaled by
        // the aim service's own range so SENSITIVITY still means something.
        float range = gos_input_aim_config()->range_deg;
        if (range < 1.f) range = 25.f;          // never divide by a zeroed cfg
        float tlx = in->roll / range, tly = in->pitch / range;
        if (tlx > 1) tlx = 1; else if (tlx < -1) tlx = -1;
        if (tly > 1) tly = 1; else if (tly < -1) tly = -1;
        float mag = sqrtf(tlx * tlx + tly * tly);
        if (mag > 1) mag = 1;
        float tgt = 0;
        if (mag > HV_DEAD) {
            float ux = tlx * (ISO_C / ISO_SC) +
                       tly * (ISO_S / (ISO_SC * ISO_T));
            float uy = tlx * (-ISO_S / ISO_SC) +
                       tly * (ISO_C / (ISO_SC * ISO_T));
            float want = atan2f(uy, ux);
            float err = want - g->heading;
            while (err > 3.14159265f) err -= 6.2831853f;
            while (err < -3.14159265f) err += 6.2831853f;
            float turn = err * ERR_GAIN;
            if (turn > TURN_MAX) turn = TURN_MAX;
            if (turn < -TURN_MAX) turn = -TURN_MAX;
            g->heading += turn * dt;
            if (g->heading > 3.14159265f) g->heading -= 6.2831853f;
            if (g->heading < -3.14159265f) g->heading += 6.2831853f;
            float align = cosf(err);
            if (align < 0) align = 0;
            float t = (mag - HV_DEAD) / (1.0f - HV_DEAD);
            tgt = SPD_MAX * (0.25f + 0.75f * t) * align;
        }

        float c = cosf(g->heading), s = sinf(g->heading);
        int mask = hv_probe_mask(g, g->px, g->py, c, s);
        if (mask == 1 || mask == 4) {
            // one tip grazing: deflect away and slide past at reduced speed
            g->heading += (mask == 1 ? -1.4f : 1.4f) * dt;
            c = cosf(g->heading); s = sinf(g->heading);
            tgt *= 0.55f;
        } else if (mask) {
            tgt = 0;
            if (g->speed > 6 && ms - g->thud_ms > 600) {
                gos_audio_play(&sfx_thud, 170);
                g->thud_ms = ms;
            }
            if (g->speed > 0) g->speed = 0;
        }
        g->speed += (tgt - g->speed) * fminf(1.0f, 3.2f * dt);

        float nx = g->px + c * g->speed * dt;
        float ny = g->py + s * g->speed * dt;
        int mmask = hv_probe_mask(g, nx, ny, c, s);
        // move while the center is clear and we're not wedged tip-to-tip;
        // a grazing tip may sweep the obstacle's rim as it slides by.
        // The BODY is probed too (at center and half-way to the header):
        // a small tree lobe can fit between the header-line probes and
        // otherwise ends up under the machine
        if ((mmask & 2) == 0 && mmask != (1 | 4) &&
            !hv_blocked_at(g, nx, ny) &&
            !hv_blocked_at(g, nx + c * 5.0f, ny + s * 5.0f)) {
            g->px = nx; g->py = ny;
        } else {
            g->speed = 0;   // never creep into a blocked spot, however slowly
        }

        if (g->speed > 2.0f) hv_tracks(g, c, s);

        // cutting (forward motion only)
        uint32_t cut = 0;
        if (g->speed > 0.5f)
            cut = hv_cut_to(g, g->px + c * HEADER_X, g->py + s * HEADER_X, -s, c);
        else {
            g->hx = g->px + c * HEADER_X; g->hy = g->py + s * HEADER_X;
        }
        g->cut_wheat += cut;
        g->intake += ((float)cut - g->intake) * 0.12f;

        // chaff behind the machine while cutting
        if (cut > 0 && (ms & 1) == 0) {
            uint32_t h = hv_hash((int)(g->px * 8), (int)ms);
            float sp = ((h & 7) - 3.5f) * 1.2f;
            hv_spawn_part(g, g->px - c * 7.0f - s * sp * 0.4f,
                          g->py - s * 7.0f + c * sp * 0.4f, 5,
                          -c * (9 + (h >> 3 & 7)) - s * sp,
                          -s * (9 + (h >> 3 & 7)) + c * sp,
                          6 + (h >> 6 & 3), 22 + (h >> 8 & 15), 0);
        }

        // milestones
        int pct = g->total_wheat ? (int)(g->cut_wheat * 100 / g->total_wheat) : 0;
        if (pct >= g->milestone && g->milestone <= 75) {
            gos_audio_play(&sfx_mile, 170);
            g->milestone += 25;
        }

        // straggler marker refresh
        if (pct >= 90 && ms >= g->mark_next_ms) {
            g->mark_next_ms = ms + 350;
            g->mark_bx = -1;
            int best = 0x7fffffff;
            int cbx = (int)g->px / CELL, cby = (int)g->py / CELL;
            for (int by = 0; by < GRID; by++)
                for (int bx = 0; bx < GRID; bx++)
                    if (g->remain[by * GRID + bx]) {
                        int d = (bx - cbx) * (bx - cbx) + (by - cby) * (by - cby);
                        if (d < best) { best = d; g->mark_bx = bx; g->mark_by = by; }
                    }
        }

        // done? (a whisker of stragglers is forgiven — the breeze gets them)
        if (g->total_wheat && g->total_wheat - g->cut_wheat <= g->total_wheat / 250)
            hv_finish_field(ctx);
        break;
    }
    }

    // camera chases the combine with a velocity lookahead
    {
        float c = cosf(g->heading), s = sinf(g->heading);
        // stay centered on the machine — a light lead only, so the tilt
        // direction always reads from the middle of the screen
        float la = 5.0f + g->speed * 0.3f;
        float fx = g->px + c * la;
        float fy = g->py + s * la;
        float tx = (fx * ISO_C - fy * ISO_S) * ISO_SC - GOS_SCREEN_W / 2;
        float ty = (fx * ISO_S + fy * ISO_C) * ISO_SC * ISO_T - GOS_SCREEN_H / 2;
        if (!g->cam_init) { g->camx = tx; g->camy = ty; g->cam_init = true; }
        g->camx += (tx - g->camx) * fminf(1.0f, 3.2f * dt);
        g->camy += (ty - g->camy) * fminf(1.0f, 3.2f * dt);
        // settle EXACTLY when parked: the eased camera otherwise creeps
        // sub-pixel for seconds, and every class boundary (wheat wall edges,
        // fence wires) shimmers against the drift — user-visible flashing
        if (fabsf(g->speed) < 0.5f &&
            fabsf(tx - g->camx) < 2.0f && fabsf(ty - g->camy) < 2.0f) {
            g->camx = tx; g->camy = ty;
        }
    }

    hv_update_parts(g, dt);
    hv_set_loop_vols(g);
}

// --- lifecycle ----------------------------------------------------------------------
static void hv_init(gos_ctx_t *ctx)
{
    hv_t *g = ctx->state;
    hv_save_t sv;
    if (gos_save_read(ctx, &sv, sizeof sv) == (int)sizeof sv && sv.ver == 1)
        g->sv = sv;
    hv_fixed_palette();
    g->field = 0;
    hv_gen_field(g, &ctx->rng);
    g->st = HV_BRIEF;
    g->mark_bx = -1;
    hv_start_loops(g);
}

static void hv_suspend(gos_ctx_t *ctx)
{
    hv_t *g = ctx->state;
    gos_audio_stop(g->h_eng);
    gos_audio_stop(g->h_whine);
    gos_audio_stop(g->h_cut);
    g->t_sus_us = gos_time_us();
    hv_save_now(ctx);
}

static void hv_resume(gos_ctx_t *ctx)
{
    hv_t *g = ctx->state;
    if (g->st == HV_PLAY)
        g->t_start_us += gos_time_us() - g->t_sus_us;
    g->was_touch = false;
    g->intake = 0;
    hv_start_loops(g);
    hv_fixed_palette();
}

static void hv_teardown(gos_ctx_t *ctx)
{
    hv_save_now(ctx);
    gos_audio_stop_all();
}

const gos_game_t game_harvest = {
    .id = "harvest",
    .title = "HARVEST",
    .icon = NULL,
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_SAVE,
    .state_size = sizeof(hv_t),
    .init = hv_init,
    .update = hv_update,
    .render = hv_render,
    .suspend = hv_suspend,
    .resume = hv_resume,
    .teardown = hv_teardown,
};

// --- host-sim probe -------------------------------------------------------------------
#ifdef GOS_HOST_SIM
typedef struct {
    int st, field, fsize;
    float px, py, heading, speed;
    uint32_t cut, total;
    const uint8_t *world;
    const uint16_t *remain;
} hv_probe_t;

void harvest_sim_probe(gos_ctx_t *ctx, hv_probe_t *out)
{
    hv_t *g = ctx->state;
    out->st = g->st; out->field = g->field; out->fsize = g->fsize;
    out->px = g->px; out->py = g->py;
    out->heading = g->heading; out->speed = g->speed;
    out->cut = g->cut_wheat; out->total = g->total_wheat;
    out->world = g->world; out->remain = g->remain;
}
#endif
