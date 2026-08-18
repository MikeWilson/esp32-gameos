// bomber_render.c — the two views.
//
// CRUISE: straight row-copies from the terrain ring (no world rotation —
// only the plane sprite banks; a rotated full-res view would eat the whole
// frame budget), then sprites, bursts, HUD, target brackets, edge markers.
// SIGHT: a draw-once instrument backdrop with a live circular porthole:
// 2x nearest-neighbor zoom of the ring around the nominal impact point,
// crosshair, drifting pipper, wind streaks, flak rim-blooms. Screen shake
// shifts the SOURCE sampling, so the bezel never needs repainting.

#include "bomber_int.h"

static inline void px(int x, int y, uint16_t c)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        CV[y * SCREEN_W + x] = c;
}

static void dim_screen_px(int x, int y, float f)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    uint16_t *p = &CV[y * SCREEN_W + x];
    int r = (*p >> 11) & 31, g = (*p >> 5) & 63, b = *p & 31;
    *p = (uint16_t)((((int)(r * f)) << 11) | (((int)(g * f)) << 5) | (int)(b * f));
}

static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) px(cx + dx, cy + dy, c);
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px(xx, yy, c);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ---------------------------------------------------------------------------
// The plane: a 15x11 half-silhouette mirrored, rotated by the bank angle.
// 0 = transparent, 1 = dark olive, 2 = olive, 3 = highlight, 4 = canopy.
// ---------------------------------------------------------------------------

static const uint8_t plane_art[11][15] = {
    { 0,0,0,0,0,0,0,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,4,2,1,0,0,0,0,0 },
    { 0,0,1,1,1,2,2,4,2,2,1,1,1,0,0 },
    { 1,2,2,2,2,2,3,3,3,2,2,2,2,2,1 },
    { 1,2,2,2,2,2,2,2,2,2,2,2,2,2,1 },
    { 0,1,1,1,2,2,2,2,2,2,2,1,1,1,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0 },
    { 0,0,0,0,1,2,2,3,2,2,1,0,0,0,0 },
    { 0,0,0,0,0,1,1,2,1,1,0,0,0,0,0 },
};

static const uint16_t plane_pal[5] = {
    0, 0, 0, 0, 0   // filled in first call (b565 isn't constant-foldable)
};

static void draw_plane(int cx, int cy, float bank, bool shadow)
{
    static uint16_t pal[5];
    if (!pal[1]) {
        pal[1] = b565(46, 52, 34);
        pal[2] = b565(86, 96, 58);
        pal[3] = b565(140, 148, 104);
        pal[4] = b565(70, 110, 130);
    }
    (void)plane_pal;
    // rotate by bank about the sprite center; scale 2x (art is 15x11)
    float ca = cosf(bank), sa = sinf(bank);
    for (int dy = -12; dy <= 12; dy++) {
        for (int dx = -16; dx <= 16; dx++) {
            // inverse-rotate destination into art space
            float ax = (dx * ca + dy * sa) / 2.0f + 7.0f;
            float ay = (-dx * sa + dy * ca) / 2.0f + 5.0f;
            int ix = (int)(ax + 0.5f), iy = (int)(ay + 0.5f);
            if (ix < 0 || ix > 14 || iy < 0 || iy > 10) continue;
            uint8_t c = plane_art[iy][ix];
            if (!c) continue;
            if (shadow) dim_screen_px(cx + dx, cy + dy, 0.62f);
            else px(cx + dx, cy + dy, pal[c]);
        }
    }
}

// ---------------------------------------------------------------------------
// Shared: predicted impact point (world coords) for a given time-of-fall
// ---------------------------------------------------------------------------

void impact_point(float tof, float *ix, float *iy)
{
    float vy = CRUISE_SPEED * (1.0f + G->trim_speed);
    *ix = G->plane_x + (G->trim_x + G->wind_x) * tof;
    *iy = G->plane_y + (vy + G->wind_y) * tof;
}

// ---------------------------------------------------------------------------
// Bursts (flak in cruise + bomb impacts): 3-stage procedural puffs
// ---------------------------------------------------------------------------

static void draw_burst_at(int sx, int sy, const burst_t *bu)
{
    float a = bu->age;
    if (bu->kind == 1) {                 // bomb impact: flash then smoke
        if (a < 0.12f) disc(sx, sy, 10 + (int)(a * 90), b565(255, 240, 190));
        else if (a < 0.5f) {
            int r = 12 + (int)(a * 26);
            disc(sx, sy, r, b565(120 - (int)(a * 90), 96 - (int)(a * 70), 70));
            disc(sx, sy - r / 2, r / 2, b565(70, 62, 54));
        } else {
            int r = (int)(18 - (a - 0.5f) * 10);
            if (r > 1) disc(sx, sy - 10, r, b565(88, 84, 80));
        }
    } else if (bu->kind == 0) {          // flak: crack then gray puff
        if (a < 0.08f) disc(sx, sy, 6, b565(255, 220, 140));
        else {
            int r = 5 + (int)(a * 14);
            if (r > 16) r = 16;
            int lum = 130 - (int)(a * 40);
            disc(sx, sy, r, b565(lum, lum, lum));
            disc(sx - r / 3, sy - r / 3, r / 2, b565(lum + 26, lum + 26, lum + 26));
        }
    } else {                             // fire puff on burning targets
        int r = 3 + ((int)(a * 10) % 3);
        disc(sx, sy, r, b565(240, 140 + (int)(bomber_hash((int)a * 7, sx) % 60u), 40));
        disc(sx - 1, sy - 4, r - 1, b565(90, 84, 80));
    }
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

static void draw_hud(void)
{
    // windsock, top-left: ring + streamer showing LIVE wind
    int wx0 = 40, wy0 = 42;
    for (int a = 0; a < 64; a++) {
        float th = a * 6.28f / 64;
        px(wx0 + (int)(cosf(th) * 22), wy0 + (int)(sinf(th) * 22), b565(230, 226, 210));
    }
    float wm = sqrtf(G->wind_x * G->wind_x + G->wind_y * G->wind_y);
    if (wm > 0.5f) {
        // screen-space: wind +y (ahead) blows targets DOWN-screen relative
        // drift, drawn as the streamer pointing where the wind pushes
        float dx = G->wind_x / wm, dy = -G->wind_y / wm;
        int len = 8 + (int)(wm * 0.45f);
        line(wx0, wy0, wx0 + (int)(dx * len), wy0 + (int)(dy * len), b565(255, 120, 90));
        line(wx0 + 1, wy0, wx0 + 1 + (int)(dx * len), wy0 + (int)(dy * len), b565(255, 120, 90));
    } else {
        disc(wx0, wy0, 3, b565(230, 226, 210));
    }
    gos_gfx_text565(14, wx0 - 24, wy0 + 28, b565(240, 236, 220), "%d mph",
                    (int)(wm * 0.6f + 0.5f));

    // bombs, top-right: little bomb pips
    for (int i = 0; i < MAX_BOMBS; i++) {
        int bx = SCREEN_W - 30 - i * 18, by = 30;
        uint16_t c = i < G->bombs_left ? b565(235, 230, 210) : b565(70, 74, 66);
        rect(bx - 2, by - 6, 5, 10, c);
        px(bx, by - 8, c);
        px(bx - 3, by + 5, c); px(bx + 3, by + 5, c);
    }

    // damage bar, bottom-left; score bottom-right; sortie tag
    int dw = (int)(G->damage * 1.2f);
    rect(20, SCREEN_H - 26, 122, 10, b565(50, 54, 48));
    if (dw > 0)
        rect(21, SCREEN_H - 25, dw, 8,
             G->damage > 70 ? b565(220, 60, 50) :
             G->damage > 40 ? b565(230, 160, 60) : b565(120, 180, 90));
    gos_gfx_text565(14, 20, SCREEN_H - 46, b565(240, 236, 220), "DMG");
    gos_gfx_text565(18, SCREEN_W - 130, SCREEN_H - 34, b565(240, 236, 220),
                    "%d", G->score);
    gos_gfx_text565(14, SCREEN_W - 130, SCREEN_H - 52, b565(180, 178, 160),
                    "SORTIE %d", G->sortie_num);
}

static void draw_bracket(int sx, int sy, uint16_t c)
{
    for (int i = 0; i < 8; i++) {
        px(sx - 26 + i, sy - 26, c); px(sx - 26, sy - 26 + i, c);
        px(sx + 26 - i, sy - 26, c); px(sx + 26, sy - 26 + i, c);
        px(sx - 26 + i, sy + 26, c); px(sx - 26, sy + 26 - i, c);
        px(sx + 26 - i, sy + 26, c); px(sx + 26, sy + 26 - i, c);
    }
}

// edge marker on the plane->target ray (playbook: never let a trackable
// vanish without a pointer, and anchor markers on the true line)
static void draw_edge_marker(const target_t *t, bool with_text)
{
    float dx = t->x - G->plane_x, dy = -(t->y - G->plane_y);   // screen sense
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    dx /= len; dy /= len;
    float bx = w2sx(G->plane_x), by = (float)PLANE_SCREEN_Y;
    float tmax = 1e9f;
    if (dx > 0.001f) tmax = fminf(tmax, (SCREEN_W - 24 - bx) / dx);
    if (dx < -0.001f) tmax = fminf(tmax, (24 - bx) / dx);
    if (dy > 0.001f) tmax = fminf(tmax, (SCREEN_H - 70 - by) / dy);
    if (dy < -0.001f) tmax = fminf(tmax, (80 - by) / dy);
    if (tmax < 30) tmax = 30;
    int ax = (int)(bx + dx * tmax), ay = (int)(by + dy * tmax);
    uint16_t c = t->defended ? b565(255, 120, 90) : b565(240, 236, 210);
    line(ax, ay, ax - (int)(dx * 12) + (int)(dy * 6), ay - (int)(dy * 12) - (int)(dx * 6), c);
    line(ax, ay, ax - (int)(dx * 12) - (int)(dy * 6), ay - (int)(dy * 12) + (int)(dx * 6), c);
    if (with_text)
        gos_gfx_text565(14, ax - (int)(dx * 34) - 10, ay - (int)(dy * 34) - 8, c,
                        "%d", (int)((t->y - G->plane_y)));
}

// ---------------------------------------------------------------------------
// CRUISE
// ---------------------------------------------------------------------------

void render_cruise(void)
{
    int top = (int)cam_top();
    int camx = (int)G->cam_x;
    if (camx < 0) camx = 0;
    if (camx > STRIP_W - SCREEN_W) camx = STRIP_W - SCREEN_W;

    int shx = (int)((bomber_hash((int)(G->shake * 97), 3) % 9u) - 4) ;
    if (G->shake <= 0.1f) shx = 0;
    for (int sy = 0; sy < SCREEN_H; sy++) {
        int wr = top - sy;
        int cx2 = camx + (G->shake > 0.1f ? shx : 0);
        if (cx2 < 0) cx2 = 0;
        if (cx2 > STRIP_W - SCREEN_W) cx2 = STRIP_W - SCREEN_W;
        if (wr < 0 || wr >= G->gen_row || wr <= G->gen_row - RING_H) {
            // behind the strip = the sea you took off over (never black)
            uint16_t *dst = &CV[sy * SCREEN_W];
            for (int x = 0; x < SCREEN_W; x++)
                dst[x] = bomber_sea_px(wr, cx2 + x);
            continue;
        }
        memcpy(&CV[sy * SCREEN_W], &G->ring[(wr & RING_MASK) * STRIP_W + cx2],
               SCREEN_W * 2);
    }

    // convoy trucks (moving sprites on the road)
    for (int i = 0; i < G->n_trucks; i++) {
        if (!G->trucks[i].alive) continue;
        int sx = w2sx(road_cx(G->trucks[i].y)), sy = w2sy(G->trucks[i].y);
        if (sy < -8 || sy > SCREEN_H + 8) continue;
        rect(sx - 4, sy - 6, 9, 13, b565(58, 64, 48));
        rect(sx - 3, sy - 4, 7, 6, b565(96, 102, 78));
    }

    // burning-target fires + all bursts
    for (int i = 0; i < G->n_targets; i++) {
        target_t *t = &G->targets[i];
        if (t->state != TS_BURNING) continue;
        int sx = w2sx(t->x), sy = w2sy(t->y);
        if (sx < -40 || sx > SCREEN_W + 40 || sy < -40 || sy > SCREEN_H + 40) continue;
        burst_t fb = { .age = (float)((gos_time_us() / 1000) % 900) / 1000.0f, .kind = 2 };
        draw_burst_at(sx + (int)(bomber_hash(i, 3) % 11u) - 5, sy - 4, &fb);
    }
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (!G->bursts[i].alive) continue;
        draw_burst_at(w2sx(G->bursts[i].x), w2sy(G->bursts[i].y), &G->bursts[i]);
    }

    // target brackets on-screen; edge markers off-screen (live targets only)
    for (int i = 0; i < G->n_targets; i++) {
        target_t *t = &G->targets[i];
        if (t->state == TS_DEAD) continue;
        float tx = t->kind == TGT_CONVOY ? road_cx(t->y) : t->x;
        int sx = w2sx(tx), sy = w2sy(t->y);
        bool on = sx >= 30 && sx < SCREEN_W - 30 && sy >= 30 && sy < SCREEN_H - 30;
        if (on) {
            uint16_t c = t->defended ? b565(255, 120, 90) : b565(240, 236, 210);
            draw_bracket(sx, sy, c);
            if (t->defended)
                gos_gfx_text565(14, sx - 20, sy - 46, c, "FLAK");
        }
    }
    // off-screen markers: only the two nearest ahead (a stack of markers
    // overprinted their labels), and only the nearest carries the distance
    {
        int near1 = -1, near2 = -1;
        for (int i = 0; i < G->n_targets; i++) {
            target_t *t = &G->targets[i];
            if (t->state == TS_DEAD || t->y <= G->plane_y - 60) continue;
            float tx = t->kind == TGT_CONVOY ? road_cx(t->y) : t->x;
            int sx = w2sx(tx), sy = w2sy(t->y);
            if (sx >= 30 && sx < SCREEN_W - 30 && sy >= 30 && sy < SCREEN_H - 30)
                continue;   // on-screen: bracketed above
            if (near1 < 0 || t->y < G->targets[near1].y) { near2 = near1; near1 = i; }
            else if (near2 < 0 || t->y < G->targets[near2].y) near2 = i;
        }
        if (near1 >= 0) draw_edge_marker(&G->targets[near1], true);
        if (near2 >= 0) draw_edge_marker(&G->targets[near2], false);
    }

    // plane: shadow offset by "altitude", then the banked sprite
    int psx = w2sx(G->plane_x);
    draw_plane(psx + 14, PLANE_SCREEN_Y + 18, G->bank, true);
    draw_plane(psx, PLANE_SCREEN_Y, G->bank, false);

    // smoke trail when damaged
    if (G->damage >= 40) {
        int n = G->damage >= 70 ? 6 : 3;
        for (int i = 1; i <= n; i++) {
            int r = G->damage >= 70 ? 4 : 2;
            uint32_t h = bomber_hash(i, (int)(gos_time_us() / 90000));
            disc(psx - (int)(G->bank * 20) + (int)(h % 5u) - 2,
                 PLANE_SCREEN_Y + 12 + i * 9, r + (int)(h % 2u),
                 b565(70 + i * 6, 68 + i * 6, 66 + i * 6));
        }
    }

    draw_hud();
}

// ---------------------------------------------------------------------------
// SIGHT
// ---------------------------------------------------------------------------

void sight_backdrop(void)
{
    // dark instrument surround + bezel ring; drawn once per sight entry
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            int dx = x - SIGHT_CX, dy = y - SIGHT_CY;
            int d2 = dx * dx + dy * dy;
            if (d2 <= SIGHT_R * SIGHT_R) continue;   // porthole repaints live
            float d = sqrtf((float)d2);
            if (d < SIGHT_R + 6) CV[y * SCREEN_W + x] = b565(160, 150, 120);
            else if (d < SIGHT_R + 10) CV[y * SCREEN_W + x] = b565(74, 68, 54);
            else {
                int lum = 34 - (int)((d - SIGHT_R) * 0.03f);
                if (lum < 20) lum = 20;
                CV[y * SCREEN_W + x] = b565(lum, lum + 2, lum);
            }
        }
    }
    gos_gfx_text565(18, 24, 20, b565(190, 184, 160), "BOMBSIGHT");
    // the FIRE button: a fat red mushroom cap on the lower bezel
    for (int dy = -FIRE_BTN_R - 4; dy <= FIRE_BTN_R + 4; dy++) {
        for (int dx = -FIRE_BTN_R - 4; dx <= FIRE_BTN_R + 4; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            int px2 = FIRE_BTN_X + dx, py2 = FIRE_BTN_Y + dy;
            if (px2 < 0 || px2 >= SCREEN_W || py2 < 0 || py2 >= SCREEN_H) continue;
            if (d > FIRE_BTN_R + 4) continue;
            if (d > FIRE_BTN_R) CV[py2 * SCREEN_W + px2] = b565(60, 56, 48);   // collar
            else {
                float lam = 1.0f - d / (FIRE_BTN_R * 1.6f);
                int hi = dx - dy < -8 ? 30 : 0;                                // sheen
                CV[py2 * SCREEN_W + px2] =
                    b565((int)(190 * lam) + 40 + hi, (int)(44 * lam) + 12 + hi / 2,
                         (int)(40 * lam) + 10);
            }
        }
    }
    gos_gfx_text565(18, FIRE_BTN_X - gos_gfx_text565_w(18, "FIRE") / 2,
                    FIRE_BTN_Y - 10, b565(255, 230, 220), "FIRE");
    gos_gfx_text565(14, 16, SCREEN_H - 30, b565(150, 146, 130), "A: DROP");
    gos_gfx_text565(14, SCREEN_W - 128, SCREEN_H - 30, b565(150, 146, 130),
                    "TAP FRAME: EXIT");
}

void render_sight(void)
{
    // FIRE button press flash (fresh drop = bright ring for a beat)
    if (G->last_drop_us && gos_time_us() - G->last_drop_us < 160000) {
        for (int a = 0; a < 80; a++) {
            float th = a * 6.28f / 80;
            int px2 = FIRE_BTN_X + (int)(cosf(th) * (FIRE_BTN_R + 2));
            int py2 = FIRE_BTN_Y + (int)(sinf(th) * (FIRE_BTN_R + 2));
            if (px2 >= 0 && px2 < SCREEN_W && py2 >= 0 && py2 < SCREEN_H)
                CV[py2 * SCREEN_W + px2] = b565(255, 240, 200);
        }
    }

    // live HUD strips on the backdrop (small, over static labels' clear zones)
    rect(24, 44, 130, 20, b565(24, 26, 24));
    gos_gfx_text565(14, 24, 46, b565(220, 214, 190), "BOMBS %d   DMG %d%%",
                    G->bombs_left, (int)G->damage);

    // porthole interior: 2x zoom centered on the NOMINAL impact point
    // (no trim, no wind) so the crosshair is the "dumb bomb" answer and
    // the pipper shows what wind + trim actually do
    float src_cx = G->plane_x;
    float src_cy = G->plane_y + CRUISE_SPEED * BOMB_TOF;
    int shx = 0, shy = 0;
    if (G->shake > 0.1f) {
        uint32_t h = bomber_hash((int)(gos_time_us() / 33000), 5);
        shx = (int)(h % 9u) - 4;
        shy = (int)((h >> 4) % 9u) - 4;
    }

    for (int sy = SIGHT_CY - SIGHT_R; sy <= SIGHT_CY + SIGHT_R; sy++) {
        int dy = sy - SIGHT_CY;
        int half = (int)sqrtf((float)(SIGHT_R * SIGHT_R - dy * dy));
        int wr = (int)(src_cy - dy / SIGHT_ZOOM) + shy;
        uint16_t *dst = &CV[sy * SCREEN_W];
        bool have = wr >= 0 && wr < G->gen_row && wr > G->gen_row - RING_H;
        const uint16_t *src = have ? &G->ring[(wr & RING_MASK) * STRIP_W] : NULL;
        for (int dx = -half; dx <= half; dx++) {
            int wx = (int)(src_cx + dx / SIGHT_ZOOM) + shx;
            uint16_t c = (have && wx >= 0 && wx < STRIP_W) ? src[wx]
                       : bomber_sea_px(wr, wx);
            // vignette toward the rim
            int d2 = dx * dx + dy * dy;
            if (d2 > (SIGHT_R - 18) * (SIGHT_R - 18)) {
                int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                c = (uint16_t)(((r * 3 / 4) << 11) | ((g * 3 / 4) << 5) | (b * 3 / 4));
            }
            dst[SIGHT_CX + dx] = c;
        }
    }

    // wind streaks inside the glass (advected, so the wind is visible)
    float wm = sqrtf(G->wind_x * G->wind_x + G->wind_y * G->wind_y);
    if (wm > 0.5f) {
        float dx = G->wind_x / wm, dy = -G->wind_y / wm;
        for (int i = 0; i < 10; i++) {
            uint32_t h = bomber_hash(i * 13, 77);
            float ph = fmodf((float)(gos_time_us() % 2000000) / 2000000.0f +
                             (h % 100u) / 100.0f, 1.0f);
            float cx2 = SIGHT_CX + ((int)(h % 220u) - 110) + dx * ph * 90;
            float cy2 = SIGHT_CY + ((int)((h >> 8) % 220u) - 110) + dy * ph * 90;
            int ddx = (int)(cx2 - SIGHT_CX), ddy = (int)(cy2 - SIGHT_CY);
            if (ddx * ddx + ddy * ddy > (SIGHT_R - 14) * (SIGHT_R - 14)) continue;
            int len = 5 + (int)(wm * 0.2f);
            line((int)cx2, (int)cy2, (int)(cx2 + dx * len), (int)(cy2 + dy * len),
                 b565(225, 225, 230));
        }
    }

    // targets get tick brackets inside the porthole
    for (int i = 0; i < G->n_targets; i++) {
        target_t *t = &G->targets[i];
        if (t->state == TS_DEAD) continue;
        float tx = t->kind == TGT_CONVOY ? road_cx(t->y) : t->x;
        int sx = SIGHT_CX + (int)((tx - src_cx) * SIGHT_ZOOM);
        int sy = SIGHT_CY - (int)((t->y - src_cy) * SIGHT_ZOOM);
        int ddx = sx - SIGHT_CX, ddy = sy - SIGHT_CY;
        if (ddx * ddx + ddy * ddy > (SIGHT_R - 24) * (SIGHT_R - 24)) continue;
        uint16_t c = b565(255, 235, 150);
        for (int k = 4; k <= 10; k++) {
            px(sx - k, sy, c); px(sx + k, sy, c);
            px(sx, sy - k, c); px(sx, sy + k, c);
        }
    }

    // crosshair (the no-wind answer)
    for (int k = -SIGHT_R + 12; k <= SIGHT_R - 12; k++) {
        if (abs(k) < 8) continue;
        if ((k & 3) < 2) {
            px(SIGHT_CX + k, SIGHT_CY, b565(30, 32, 30));
            px(SIGHT_CX, SIGHT_CY + k, b565(30, 32, 30));
        }
    }

    // pipper: the true predicted impact under trim + live wind
    float ix, iy;
    impact_point(BOMB_TOF, &ix, &iy);
    int psx = SIGHT_CX + (int)((ix - src_cx) * SIGHT_ZOOM);
    int psy = SIGHT_CY - (int)((iy - src_cy) * SIGHT_ZOOM);
    int pdx = psx - SIGHT_CX, pdy = psy - SIGHT_CY;
    if (pdx * pdx + pdy * pdy < (SIGHT_R - 10) * (SIGHT_R - 10)) {
        uint16_t c = b565(255, 90, 70);
        for (int k = 3; k <= 7; k++) {
            px(psx - k, psy, c); px(psx + k, psy, c);
            px(psx, psy - k, c); px(psx, psy + k, c);
        }
        disc(psx, psy, 1, c);
    }

    // bombs in flight: shrinking dots walking toward their impact
    for (int i = 0; i < BOMBS_INFLIGHT; i++) {
        if (!G->bombs[i].alive) continue;
        int bx = SIGHT_CX + (int)((G->bombs[i].x - src_cx) * SIGHT_ZOOM);
        int by = SIGHT_CY - (int)((G->bombs[i].y - src_cy) * SIGHT_ZOOM);
        int r = 1 + (int)(G->bombs[i].t_left * 2.5f);
        int ddx = bx - SIGHT_CX, ddy = by - SIGHT_CY;
        if (ddx * ddx + ddy * ddy < (SIGHT_R - 6) * (SIGHT_R - 6))
            disc(bx, by, r > 4 ? 4 : r, b565(30, 30, 28));
    }

    // impact bursts visible through the glass
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (!G->bursts[i].alive || G->bursts[i].kind != 1) continue;
        int bx = SIGHT_CX + (int)((G->bursts[i].x - src_cx) * SIGHT_ZOOM);
        int by = SIGHT_CY - (int)((G->bursts[i].y - src_cy) * SIGHT_ZOOM);
        int ddx = bx - SIGHT_CX, ddy = by - SIGHT_CY;
        if (ddx * ddx + ddy * ddy < (SIGHT_R - 20) * (SIGHT_R - 20))
            draw_burst_at(bx, by, &G->bursts[i]);
    }

    // flak while sighted: bright blooms at the bezel toward each burst
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (!G->bursts[i].alive || G->bursts[i].kind != 0) continue;
        if (G->bursts[i].age > 0.5f) continue;
        float ang = atan2f(-(G->bursts[i].y - G->plane_y),
                           G->bursts[i].x - G->plane_x);
        for (int s = -14; s <= 14; s++) {
            float a2 = ang + s * 0.006f;
            int bx = SIGHT_CX + (int)(cosf(a2) * (SIGHT_R - 4));
            int by = SIGHT_CY + (int)(sinf(a2) * (SIGHT_R - 4));
            int lum = 255 - (int)(G->bursts[i].age * 360) - abs(s) * 6;
            if (lum > 60) disc(bx, by, 2, b565(lum, lum - 40, lum - 90));
        }
    }
}
