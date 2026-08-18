// golf_render.c — Infinite Golf rendering, 1:1 from the standalone main.c:
// the incremental background pipeline (rough texture, SDF grids, terrain
// paint, cart path, trees, decor), the fixed-point camera blit with the
// fused CRT stripes, and the in-play frame (AA ball, aim previews, meters,
// wind rose, edge arrow, HUD pills). All pixels land in CV, the OS's
// full-res 368x448 RGB565 buffer.

#include "golf_int.h"

#define randf golf_randf

uint16_t pack_rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return rgb565c(r, g, b);
}

uint16_t dim565f(uint16_t c, float f)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (int)(r * f); g = (int)(g * f); b = (int)(b * f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void dim_canvas(float f)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        CV[i] = dim565f(CV[i], f);
}

static inline void put_px(int x, int y, uint16_t c)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        CV[y * SCREEN_W + x] = c;
}

// ---------------------------------------------------------------------------
// Background pipeline — the standalone's render passes, sliced per tick
// ---------------------------------------------------------------------------

// Rough texture + band/mottle inputs (once per launch, ~40 rows per slice)
static void bg_rough_slice(void)
{
    if (G->bg_row == 0)
        for (int y = 0; y < WORLD_H; y++)
            for (int x = 0; x < WORLD_W; x += 4) {   // mottle field, coarse fill
                float v = (noise2d(x * 0.07f, y * 0.07f) - 0.5f) * 100.0f;
                G->mottle[y * WORLD_W + x] = (int8_t)v;
                G->mottle[y * WORLD_W + x + 1] = (int8_t)v;
                G->mottle[y * WORLD_W + x + 2] = (int8_t)v;
                G->mottle[y * WORLD_W + x + 3] = (int8_t)v;
            }
    int end = G->bg_row + 40;
    if (end > WORLD_H) end = WORLD_H;
    for (int y = G->bg_row; y < end; y++) {
        for (int x = 0; x < WORLD_W; x++) {
            float fx = (float)x, fy = (float)y;
            uint32_t hsh = pixel_hash(x, y);
            float n = noise2d(fx * 0.045f, fy * 0.045f) * 0.65f +
                      noise2d(fx * 0.14f + 31.0f, fy * 0.14f) * 0.35f;
            int r_ = 26 + (int)(n * 24);
            int g_ = 64 + (int)(n * 34);
            int b_ = 24 + (int)(n * 15);
            int sp = (int)(hsh % 31u);
            if (sp == 0) { g_ -= 16; r_ -= 6; }
            else if (sp == 1) { g_ += 14; r_ += 8; }
            float vd = dist2d(fx, fy, WORLD_W * 0.5f, WORLD_H * 0.5f);
            float vg = 1.0f - 0.15f * clampf((vd - 330.0f) / 240.0f, 0.0f, 1.0f);
            r_ = (int)(r_ * vg); g_ = (int)(g_ * vg); b_ = (int)(b_ * vg);
            G->rough[y * WORLD_W + x] = pack_rgb(r_, g_, b_);
        }
    }
    G->bg_row = end;
    if (G->bg_row >= WORLD_H) {
        G->rough_done = true;
        G->bg_phase = BG_GRIDS;
        G->bg_row = 0;
    }
}

static void bg_grids_slice(void)
{
    float hx0[MAX_HAZARDS], hx1[MAX_HAZARDS], hy0[MAX_HAZARDS], hy1[MAX_HAZARDS];
    for (int h = 0; h < G->num_hazards; h++) {
        hx0[h] = 1e9f; hx1[h] = -1e9f; hy0[h] = 1e9f; hy1[h] = -1e9f;
        for (int i = 0; i < G->hazards[h].nlobes; i++) {
            hx0[h] = fminf(hx0[h], G->hazards[h].lx[i] - G->hazards[h].lr[i]);
            hx1[h] = fmaxf(hx1[h], G->hazards[h].lx[i] + G->hazards[h].lr[i]);
            hy0[h] = fminf(hy0[h], G->hazards[h].ly[i] - G->hazards[h].lr[i]);
            hy1[h] = fmaxf(hy1[h], G->hazards[h].ly[i] + G->hazards[h].lr[i]);
        }
        hx0[h] -= 32; hx1[h] += 32; hy0[h] -= 32; hy1[h] += 32;
    }
    int end = G->bg_row + 16;
    if (end > GRID_H) end = GRID_H;
    for (int gy = G->bg_row; gy < end; gy++) {
        for (int gx = 0; gx < GRID_W; gx++) {
            float x = (float)(gx * SDF_STEP), y = (float)(gy * SDF_STEP);
            int gi = gy * GRID_W + gx;
            G->green_grid[gi] = sdf_green(x, y);
            float sd = 1e9f, wd = 1e9f;
            for (int h = 0; h < G->num_hazards; h++) {
                if (x < hx0[h] || x > hx1[h] || y < hy0[h] || y > hy1[h]) continue;
                float d = sdf_hazard(h, x, y);
                if (G->hazards[h].type == 0) sd = fminf(sd, d);
                else wd = fminf(wd, d);
            }
            G->sand_grid[gi] = sd;
            G->water_grid[gi] = wd;
        }
    }
    G->bg_row = end;
    if (G->bg_row >= GRID_H) {
        G->bg_phase = BG_PAINT;
        G->bg_row = 0;
    }
}

// Terrain paint: rough memcpy + bilerped SDF cells, exact original colors
static void bg_paint_slice(void)
{
    const int CELLS_H = WORLD_H / SDF_STEP;
    int end = G->bg_row + 16;   // 16 cell rows = 64 world rows per tick
    if (end > CELLS_H) end = CELLS_H;
    const float inv = 1.0f / SDF_STEP;

    memcpy(G->background + G->bg_row * SDF_STEP * WORLD_W,
           G->rough + G->bg_row * SDF_STEP * WORLD_W,
           (size_t)(end - G->bg_row) * SDF_STEP * WORLD_W * 2);

    for (int cy = G->bg_row; cy < end; cy++) {
        for (int cx = 0; cx < WORLD_W / SDF_STEP; cx++) {
            int gi = cy * GRID_W + cx;
            float g00 = G->green_grid[gi], g10 = G->green_grid[gi + 1];
            float g01 = G->green_grid[gi + GRID_W], g11 = G->green_grid[gi + GRID_W + 1];
            float s00 = G->sand_grid[gi], s10 = G->sand_grid[gi + 1];
            float s01 = G->sand_grid[gi + GRID_W], s11 = G->sand_grid[gi + GRID_W + 1];
            float w00 = G->water_grid[gi], w10 = G->water_grid[gi + 1];
            float w01 = G->water_grid[gi + GRID_W], w11 = G->water_grid[gi + GRID_W + 1];
            if (g00 > 0 && g10 > 0 && g01 > 0 && g11 > 0 &&
                s00 > 0 && s10 > 0 && s01 > 0 && s11 > 0 &&
                w00 > 0 && w10 > 0 && w01 > 0 && w11 > 0) continue;

            for (int sy = 0; sy < SDF_STEP; sy++) {
                int y = cy * SDF_STEP + sy;
                float ty = sy * inv;
                float gl = g00 + (g01 - g00) * ty, gr = g10 + (g11 - g10) * ty;
                float sl = s00 + (s01 - s00) * ty, sr = s10 + (s11 - s10) * ty;
                float wl = w00 + (w01 - w00) * ty, wr = w10 + (w11 - w10) * ty;
                for (int sx = 0; sx < SDF_STEP; sx++) {
                    int x = cx * SDF_STEP + sx;
                    float tx = sx * inv;
                    float g = gl + (gr - gl) * tx;
                    float sd = sl + (sr - sl) * tx;
                    float wd = wl + (wr - wl) * tx;
                    if (g >= 0 && sd >= 0 && wd >= 0) continue;

                    int idx = y * WORLD_W + x;
                    float fx = (float)x, fy = (float)y;
                    uint32_t hsh = pixel_hash(x, y);
                    int r_, g_, b_;
                    if (sd < 0 && sd <= wd) {
                        float hd = sd;
                        float rip = sinf(fx * 0.30f + fy * 0.12f +
                                         noise2d(fx * 0.045f, fy * 0.045f) * 5.0f);
                        float ripf = 0.94f + 0.075f * rip;
                        int grain = (int)(hsh % 21u) - 10;
                        r_ = (int)(208 * ripf) + grain;
                        g_ = (int)(184 * ripf) + grain;
                        b_ = (int)(138 * ripf) + (grain * 2) / 3;
                        float lip = clampf((hd + 9.0f) / 9.0f, 0.0f, 1.0f);
                        float lf = 1.0f - lip * 0.30f;
                        r_ = (int)(r_ * lf); g_ = (int)(g_ * lf); b_ = (int)(b_ * lf);
                    } else if (wd < 0) {
                        float hd = wd;
                        float depth = clampf(-hd / 30.0f, 0.0f, 1.0f);
                        r_ = 66 + (int)((16 - 66) * depth);
                        g_ = 132 + (int)((52 - 132) * depth);
                        b_ = 176 + (int)((108 - 176) * depth);
                        float wv = sinf(fy * 0.22f + noise2d(fx * 0.05f, fy * 0.05f) * 7.0f + fx * 0.02f);
                        if (wv > 0.82f) {
                            int lift = (int)((wv - 0.82f) * 95.0f);
                            r_ += lift; g_ += lift; b_ += lift;
                        }
                        if ((hsh % 89u) == 0 && depth > 0.25f) { r_ += 46; g_ += 50; b_ += 42; }
                        if (hd > -3.5f) {
                            float sh = (hd + 3.5f) / 3.5f * 0.8f;
                            r_ += (int)((44 - r_) * sh);
                            g_ += (int)((74 - g_) * sh);
                            b_ += (int)((64 - b_) * sh);
                        }
                    } else {
                        float band = G->band_lut[x + y];
                        float nn = G->mottle[idx] * 0.01f;
                        int ir = (int)(52 + band * 10 + nn * 16);
                        int ig = (int)(138 + band * 16 + nn * 24);
                        int ib = (int)(46 + band * 8 + nn * 12);
                        int fr = (int)(40 + nn * 16);
                        int fg = (int)(106 + nn * 28);
                        int fb = (int)(38 + nn * 12);
                        float t = smoothstepf(0.0f, 1.0f, clampf(-g / 13.0f, 0.0f, 1.0f));
                        r_ = fr + (int)((ir - fr) * t);
                        g_ = fg + (int)((ig - fg) * t);
                        b_ = fb + (int)((ib - fb) * t);
                        float dh = dist2d(fx, fy, G->hole_x, G->hole_y);
                        if (dh < 110.0f)
                            dh += (noise2d(fx * 0.030f + 7.7f, fy * 0.030f) - 0.5f) * 42.0f;
                        float gt = smoothstepf(76.0f, 60.0f, dh);
                        if (gt > 0.0f) {
                            float band2 = smoothstepf(-0.45f, 0.45f, sinf((fx - fy) * 0.42f));
                            int pr = (int)(64 + band2 * 8 + nn * 10);
                            int pg = (int)(156 + band2 * 12 + nn * 16);
                            int pb = (int)(55 + band2 * 6 + nn * 8);
                            r_ += (int)((pr - r_) * gt);
                            g_ += (int)((pg - g_) * gt);
                            b_ += (int)((pb - b_) * gt);
                            float rim = gt * (1.0f - gt) * 4.0f;
                            float rf = 1.0f - rim * 0.10f;
                            r_ = (int)(r_ * rf); g_ = (int)(g_ * rf); b_ = (int)(b_ * rf);
                        }
                        if (g > -2.0f) {
                            r_ = (int)(r_ * 0.72f); g_ = (int)(g_ * 0.72f); b_ = (int)(b_ * 0.72f);
                        }
                    }
                    G->background[idx] = pack_rgb(r_, g_, b_);
                }
            }
        }
    }
    G->bg_row = end;
    if (G->bg_row >= CELLS_H) {
        G->bg_phase = BG_PATH;
        G->bg_row = 0;
    }
}

// Cart path: obstacle field, wall-follow marches, subdivision, ribbon
// stroke — the standalone's audited router, verbatim
static float path_field(float x, float y)
{
    float d = sdf_green(x, y);
    for (int h = 0; h < G->num_hazards; h++) {
        if (G->hazards[h].bridge) continue;
        float hd = sdf_hazard(h, x, y) + 16.0f;
        if (hd < d) d = hd;
    }
    return d;
}

static int path_march(float sx0, float sy0, float dir, float bias,
                      float *outx, float *outy, int cap)
{
    float mx = sx0, my = sy0;
    float ckx = sx0, cky = sy0;
    int n = 0;
    bool squeeze = false;
    while (n < cap) {
        if (dir > 0 ? my >= WORLD_H - 12 : my <= 12) break;
        if (squeeze) {
            float prev = my;
            my += dir * 14.0f;
            for (int it = 0; it < 20 && path_field(mx, my) < 10.0f; it++) {
                float gx = path_field(mx + 2, my) - path_field(mx - 2, my);
                float gy = path_field(mx, my + 2) - path_field(mx, my - 2);
                float gl = sqrtf(gx * gx + gy * gy);
                if (gl < 0.01f) break;
                mx += gx / gl * 4.0f;
                my += gy / gl * 4.0f;
            }
            if ((my - prev) * dir < 6.0f) my = prev + dir * 6.0f;
        } else {
            float sxp = 0.0f, syp = dir * 16.0f;
            float d0 = path_field(mx, my);
            if (d0 < 44.0f) {
                float gx = path_field(mx + 2, my) - path_field(mx - 2, my);
                float gy = path_field(mx, my + 2) - path_field(mx, my - 2);
                float gl = sqrtf(gx * gx + gy * gy);
                if (gl > 0.01f) {
                    gx /= gl; gy /= gl;
                    float tx_ = -gy * bias, ty_ = gx * bias;
                    float push = clampf(38.0f - d0, 0.0f, 20.0f) * 0.5f;
                    sxp = tx_ * 16.0f + gx * push;
                    syp = ty_ * 16.0f + gy * push;
                }
            }
            mx += sxp;
            my += syp;
            if (n % 6 == 5) {
                if (dist2d(mx, my, ckx, cky) < 25.0f) squeeze = true;
                ckx = mx;
                cky = my;
            }
        }
        mx = clampf(mx, 8, WORLD_W - 8);
        outx[n] = mx;
        outy[n] = my;
        n++;
    }
    return n;
}

static void bg_build_path(void)
{
    float *pxs = G->path_xs, *pys = G->path_ys;
    int np;
    G->rng_state = G->hole_seed ^ 0xCA47u;

    float side_first = (randf() < 0.5f) ? -1.0f : 1.0f;
    float side = side_first, best_score = 1e9f;
    for (int cand = 0; cand < 2; cand++) {
        float cs = cand == 0 ? side_first : -side_first;
        float score = 0, dprev = -1;
        for (int i = 0; i <= 39; i += 2) {
            float t = (float)i / 39.0f;
            float omt = 1.0f - t;
            float sx = omt * omt * G->spine_tee_x + 2 * omt * t * G->spine_ctl_x + t * t * G->spine_pin_x;
            float sy = omt * omt * G->spine_tee_y + 2 * omt * t * G->spine_ctl_y + t * t * G->spine_pin_y;
            float tx_ = 2 * omt * (G->spine_ctl_x - G->spine_tee_x) + 2 * t * (G->spine_pin_x - G->spine_ctl_x);
            float ty_ = 2 * omt * (G->spine_ctl_y - G->spine_tee_y) + 2 * t * (G->spine_pin_y - G->spine_ctl_y);
            float tl = sqrtf(tx_ * tx_ + ty_ * ty_);
            if (tl < 0.01f) continue;
            float ppx = -ty_ / tl * cs, ppy = tx_ / tl * cs;
            float lo = 16, hi = 300;
            for (int it = 0; it < 12; it++) {
                float mid = (lo + hi) * 0.5f;
                if (sdf_green(sx + ppx * mid, sy + ppy * mid) < 40.0f) lo = mid;
                else hi = mid;
            }
            float d = (lo + hi) * 0.5f;
            float ox = sx + ppx * d;
            if (ox < 8 || ox > WORLD_W - 8) score += 3;
            if (d > 150) score += 2;
            if (dprev >= 0 && fabsf(d - dprev) > 60) score += 5;
            dprev = d;
        }
        if (score < best_score) {
            best_score = score;
            side = cs;
        }
    }

    float wx[41], wy[41];
    int nw = 0;
    for (int i = 0; i <= 39; i++) {
        float t = (float)i / 39.0f;
        float omt = 1.0f - t;
        float sx = omt * omt * G->spine_tee_x + 2 * omt * t * G->spine_ctl_x + t * t * G->spine_pin_x;
        float sy = omt * omt * G->spine_tee_y + 2 * omt * t * G->spine_ctl_y + t * t * G->spine_pin_y;
        float tx_ = 2 * omt * (G->spine_ctl_x - G->spine_tee_x) + 2 * t * (G->spine_pin_x - G->spine_ctl_x);
        float ty_ = 2 * omt * (G->spine_ctl_y - G->spine_tee_y) + 2 * t * (G->spine_pin_y - G->spine_ctl_y);
        float tl = sqrtf(tx_ * tx_ + ty_ * ty_);
        if (tl < 0.01f) continue;
        float ppx = -ty_ / tl * side, ppy = tx_ / tl * side;
        float lo = 16, hi = 300;
        for (int it = 0; it < 16; it++) {
            float mid = (lo + hi) * 0.5f;
            if (sdf_green(sx + ppx * mid, sy + ppy * mid) < 40.0f) lo = mid;
            else hi = mid;
        }
        float d = (lo + hi) * 0.5f;
        for (int q = 0; q < 10; q++) {
            bool bad = false;
            for (int h = 0; h < G->num_hazards && !bad; h++) {
                if (G->hazards[h].bridge) continue;
                if (sdf_hazard(h, sx + ppx * d, sy + ppy * d) < 22.0f) bad = true;
            }
            if (!bad) break;
            d += 10.0f;
        }
        float ox = sx + ppx * d, oy = sy + ppy * d;
        for (int it = 0; it < 30 && sdf_green(ox, oy) < 30.0f; it++) {
            float gx = sdf_green(ox + 2, oy) - sdf_green(ox - 2, oy);
            float gy = sdf_green(ox, oy + 2) - sdf_green(ox, oy - 2);
            float gl = sqrtf(gx * gx + gy * gy);
            if (gl < 0.01f) break;
            ox += gx / gl * 6.0f;
            oy += gy / gl * 6.0f;
        }
        wx[nw] = clampf(ox, 8, WORLD_W - 8);
        wy[nw] = oy;
        nw++;
    }

    float ma_x[80], ma_y[80], mb_x[80], mb_y[80];
    int na = path_march(wx[0], wy[0], 1.0f, -1.0f, ma_x, ma_y, 78);
    int nb = path_march(wx[0], wy[0], 1.0f, 1.0f, mb_x, mb_y, 78);
    bool a_ok = na > 0 && ma_y[na - 1] >= WORLD_H - 30;
    bool b_ok = nb > 0 && mb_y[nb - 1] >= WORLD_H - 30;
    float *mex = ma_x, *mey = ma_y;
    int ne = na;
    if ((b_ok && !a_ok) || (a_ok == b_ok && nb < na)) {
        mex = mb_x; mey = mb_y; ne = nb;
    }
    np = 0;
    float endx = ne > 0 ? mex[ne - 1] : wx[0];
    pxs[np] = clampf(endx + (randf() - 0.5f) * 30.0f, 8, WORLD_W - 8);
    pys[np] = WORLD_H + 8;
    np++;
    for (int i = ne - 1; i >= 0; i--) {
        pxs[np] = mex[i];
        pys[np] = mey[i];
        np++;
    }
    for (int i = 0; i < nw; i++) {
        pxs[np] = wx[i];
        pys[np] = wy[i];
        np++;
    }
    na = path_march(wx[nw - 1], wy[nw - 1], -1.0f, -1.0f, ma_x, ma_y, 78);
    nb = path_march(wx[nw - 1], wy[nw - 1], -1.0f, 1.0f, mb_x, mb_y, 78);
    a_ok = na > 0 && ma_y[na - 1] <= 30;
    b_ok = nb > 0 && mb_y[nb - 1] <= 30;
    mex = ma_x; mey = ma_y; ne = na;
    if ((b_ok && !a_ok) || (a_ok == b_ok && nb < na)) {
        mex = mb_x; mey = mb_y; ne = nb;
    }
    for (int i = 0; i < ne; i++) {
        pxs[np] = mex[i];
        pys[np] = mey[i];
        np++;
    }
    endx = ne > 0 ? mex[ne - 1] : wx[nw - 1];
    pxs[np] = clampf(endx + (randf() - 0.5f) * 30.0f, 8, WORLD_W - 8);
    pys[np] = -8;
    np++;

    for (int pass = 0; pass < 8; pass++) {
        bool clean = true;
        for (int s = 0; s < np - 1 && np < 220; s++) {
            float mx = (pxs[s] + pxs[s + 1]) * 0.5f;
            float my = (pys[s] + pys[s + 1]) * 0.5f;
            bool bad = sdf_green(mx, my) < 8.0f;
            for (int h = 0; h < G->num_hazards && !bad; h++) {
                if (G->hazards[h].bridge) continue;
                if (sdf_hazard(h, mx, my) < 14.0f) bad = true;
            }
            if (!bad) continue;
            clean = false;
            for (int it = 0; it < 40; it++) {
                if (sdf_green(mx, my) >= 34.0f) break;
                float gx = sdf_green(mx + 2, my) - sdf_green(mx - 2, my);
                float gy = sdf_green(mx, my + 2) - sdf_green(mx, my - 2);
                float gl = sqrtf(gx * gx + gy * gy);
                if (gl < 0.01f) break;
                mx += gx / gl * 6.0f;
                my += gy / gl * 6.0f;
            }
            for (int h = 0; h < G->num_hazards; h++) {
                if (G->hazards[h].bridge) continue;
                for (int it = 0; it < 12 && sdf_hazard(h, mx, my) < 18.0f; it++) {
                    float gx = sdf_hazard(h, mx + 2, my) - sdf_hazard(h, mx - 2, my);
                    float gy = sdf_hazard(h, mx, my + 2) - sdf_hazard(h, mx, my - 2);
                    float gl = sqrtf(gx * gx + gy * gy);
                    if (gl < 0.01f) break;
                    mx += gx / gl * 5.0f;
                    my += gy / gl * 5.0f;
                }
            }
            for (int j = np; j > s + 1; j--) {
                pxs[j] = pxs[j - 1];
                pys[j] = pys[j - 1];
            }
            pxs[s + 1] = clampf(mx, 8, WORLD_W - 8);
            pys[s + 1] = my;
            np++;
            s++;
        }
        if (clean) break;
    }
    G->path_np = np;

    for (int s = 0; s < np - 1; s++) {
        float x0 = pxs[s], y0 = pys[s], x1 = pxs[s + 1], y1 = pys[s + 1];
        int minx = (int)fminf(x0, x1) - 7, maxx = (int)fmaxf(x0, x1) + 7;
        int miny = (int)fminf(y0, y1) - 7, maxy = (int)fmaxf(y0, y1) + 7;
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx > WORLD_W - 1) maxx = WORLD_W - 1;
        if (maxy > WORLD_H - 1) maxy = WORLD_H - 1;
        float segx = x1 - x0, segy = y1 - y0;
        float seg2 = segx * segx + segy * segy;
        for (int y = miny; y <= maxy; y++) {
            for (int x = minx; x <= maxx; x++) {
                float t = seg2 > 0 ? ((x - x0) * segx + (y - y0) * segy) / seg2 : 0;
                t = clampf(t, 0, 1);
                float ddx = x - (x0 + segx * t), ddy = y - (y0 + segy * t);
                float pd = sqrtf(ddx * ddx + ddy * ddy);
                if (pd > 5.5f) continue;
                if (sdf_green((float)x, (float)y) < 1.0f) continue;
                bool wet = false;
                for (int h = 0; h < G->num_hazards && !wet; h++) {
                    if (G->hazards[h].bridge) continue;
                    if (sdf_hazard(h, (float)x, (float)y) < 4.0f) wet = true;
                }
                if (wet) continue;
                int gr = (int)(pixel_hash(x, y) % 11u) - 5;
                float f = (pd > 4.2f) ? 0.82f : 1.0f - (pd / 5.5f) * 0.10f;
                G->background[y * WORLD_W + x] =
                    pack_rgb((int)((100 + gr) * f), (int)((97 + gr) * f),
                             (int)((92 + gr) * f));
            }
        }
    }
    G->bg_phase = BG_TREES;
}

static void bg_build_trees(void)
{
    G->rng_state = G->hole_seed ^ 0x7EE5u;
    int want_trees = 14 + (int)(randf() * 8);
    for (int t = 0, placed = 0; t < want_trees * 5 && placed < want_trees; t++) {
        float tx = 24 + randf() * (WORLD_W - 48);
        float ty = 24 + randf() * (WORLD_H - 48);
        if (sdf_green(tx, ty) < 65.0f) continue;
        bool clear = true;
        for (int h = 0; h < G->num_hazards && clear; h++)
            if (sdf_hazard(h, tx, ty) < 24.0f) clear = false;
        for (int s = 0; s < G->path_np - 1 && clear; s++) {
            float ex = G->path_xs[s + 1] - G->path_xs[s];
            float ey = G->path_ys[s + 1] - G->path_ys[s];
            float l2 = ex * ex + ey * ey;
            float tt = l2 > 0 ? ((tx - G->path_xs[s]) * ex + (ty - G->path_ys[s]) * ey) / l2 : 0;
            tt = clampf(tt, 0, 1);
            if (dist2d(tx, ty, G->path_xs[s] + ex * tt, G->path_ys[s] + ey * tt) < 30.0f)
                clear = false;
        }
        if (!clear) continue;
        placed++;

        float lx[3], ly[3], lr[3];
        lx[0] = tx; ly[0] = ty; lr[0] = 10.0f + randf() * 5.0f;
        lx[1] = tx - 5 - randf() * 4; ly[1] = ty - 3 - randf() * 4; lr[1] = 7 + randf() * 3;
        lx[2] = tx + 5 + randf() * 3; ly[2] = ty - 2 - randf() * 4; lr[2] = 7 + randf() * 3;

        for (int dy = -12; dy <= 12; dy++) {
            for (int dx = -16; dx <= 16; dx++) {
                float e = (dx * dx) / (16.0f * 16.0f) + (dy * dy) / (10.0f * 10.0f);
                if (e > 1.0f) continue;
                int px = (int)tx + 4 + dx, py = (int)ty + 5 + dy;
                if (px < 0 || px >= WORLD_W || py < 0 || py >= WORLD_H) continue;
                int idx = py * WORLD_W + px;
                G->background[idx] = dim565f(G->background[idx], 0.80f);
            }
        }
        for (int dy = -22; dy <= 18; dy++) {
            for (int dx = -20; dx <= 20; dx++) {
                float px = tx + dx, py = ty + dy;
                float d = 1e9f;
                for (int l = 0; l < 3; l++) {
                    float dl = dist2d(px, py, lx[l], ly[l]) - lr[l];
                    float h = clampf(0.5f + 0.5f * (dl - d) / 6.0f, 0.0f, 1.0f);
                    d = dl + (d - dl) * h - 6.0f * h * (1.0f - h);
                }
                if (d >= 0) continue;
                int ipx = (int)px, ipy = (int)py;
                if (ipx < 0 || ipx >= WORLD_W || ipy < 0 || ipy >= WORLD_H) continue;
                uint32_t hsh = pixel_hash(ipx, ipy);
                int mot = (int)(hsh % 15u) - 7;
                float depth = clampf(-d / 9.0f, 0.0f, 1.0f);
                int cr = 18 + (int)(depth * 16) + mot / 2;
                int cg = 46 + (int)(depth * 34) + mot;
                int cb = 18 + (int)(depth * 12) + mot / 2;
                if (dx < 2 && dy < 0 && depth > 0.4f) { cg += 12; cr += 5; }
                if (d > -1.5f) { cr = 12; cg = 28; cb = 12; }
                G->background[ipy * WORLD_W + ipx] = pack_rgb(cr, cg, cb);
            }
        }
    }
    G->bg_phase = BG_DECOR;
}

static void bg_build_decor(void)
{
    // tee markers
    float ta = atan2f(G->hole_y - G->ball_y, G->hole_x - G->ball_x) + (float)M_PI * 0.5f;
    for (int side = -1; side <= 1; side += 2) {
        float mx = G->ball_x + cosf(ta) * 14.0f * side;
        float my = G->ball_y + sinf(ta) * 14.0f * side;
        for (int dy = -3; dy <= 4; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                float d = sqrtf((float)(dx * dx + dy * dy));
                int px = (int)mx + dx, py = (int)my + dy;
                if (px < 0 || px >= WORLD_W || py < 0 || py >= WORLD_H) continue;
                int idx = py * WORLD_W + px;
                if (d < 2.7f) {
                    float lam = clampf(0.78f - (dx * 0.08f + dy * 0.12f), 0.5f, 1.0f);
                    G->background[idx] = pack_rgb((int)(216 * lam), (int)(54 * lam),
                                                  (int)(58 * lam));
                } else if (d < 3.7f && dy > 0) {
                    G->background[idx] = dim565f(G->background[idx], 0.75f);
                }
            }
        }
    }

    // cup: soil ring, white liner, shaded interior
    for (int dy = -HOLE_RADIUS - 3; dy <= HOLE_RADIUS + 3; dy++) {
        for (int dx = -HOLE_RADIUS - 3; dx <= HOLE_RADIUS + 3; dx++) {
            int px = (int)G->hole_x + dx;
            int py = (int)G->hole_y + dy;
            if (px < 0 || px >= WORLD_W || py < 0 || py >= WORLD_H) continue;
            float d = sqrtf((float)(dx * dx + dy * dy));
            int idx = py * WORLD_W + px;
            if (d < HOLE_RADIUS - 1.6f) {
                float v = clampf((float)(dx + dy) / (2.0f * HOLE_RADIUS) + 0.5f, 0.0f, 1.0f);
                int lum = 6 + (int)(v * 16);
                G->background[idx] = pack_rgb(lum, lum + 2, lum);
            } else if (d < HOLE_RADIUS + 0.8f) {
                int lum = 205 - (int)(clampf((float)dy / HOLE_RADIUS, -1, 1) * 35.0f + 35.0f);
                G->background[idx] = pack_rgb(lum, lum + 3, lum);
            } else if (d < HOLE_RADIUS + 2.6f) {
                G->background[idx] = dim565f(G->background[idx], 0.62f);
            }
        }
    }

    // flagstick shadow baked into the world (the stick itself is the
    // per-frame overlay so the ball can never roll over it)
    for (int i = 0; i < 15; i++) {
        int px = (int)(G->hole_x + 3 + i * 0.55f);
        int py = (int)(G->hole_y + 2 + i * 0.32f);
        if (px >= 0 && px < WORLD_W && py >= 0 && py < WORLD_H) {
            int idx = py * WORLD_W + px;
            G->background[idx] = dim565f(G->background[idx], 0.72f);
        }
    }
    G->bg_phase = BG_DONE;
}

void golf_bg_tick(void)
{
    switch (G->bg_phase) {
    case BG_ROUGH: bg_rough_slice(); break;
    case BG_GRIDS: bg_grids_slice(); break;
    case BG_PAINT: bg_paint_slice(); break;
    case BG_PATH:  bg_build_path(); break;
    case BG_TREES: bg_build_trees(); break;
    case BG_DECOR: bg_build_decor(); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Camera blit with the fused CRT stripes (verbatim)
// ---------------------------------------------------------------------------

static inline uint16_t dim565_q(uint16_t c, uint32_t q)
{
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return (uint16_t)((((r * q) >> 8) << 11) | (((g * q) >> 8) << 5) | ((b * q) >> 8));
}

void blit_view(int max_cols)
{
    float inv = 1.0f / G->cam_zoom;
    uint32_t step = (uint32_t)(inv * 65536.0f);
    for (int y = 0; y < SCREEN_H; y++) {
        int sy = (int)(G->cam_y + y * inv);
        if (sy < 0) sy = 0;
        if (sy > WORLD_H - 1) sy = WORLD_H - 1;
        const uint16_t *src = G->background + sy * WORLD_W;
        uint16_t *dst = CV + y * SCREEN_W;
        uint32_t fx = (uint32_t)(G->cam_x * 65536.0f);
        int dimrow = (y % 3) == 2;
        int ph = 0;
        for (int x = 0; x < max_cols; x++) {
            uint32_t sx = fx >> 16;
            if (sx > WORLD_W - 1) sx = WORLD_W - 1;
            uint16_t c = src[sx];
            fx += step;
            if (ph == 2) c = dim565_q(c, 205);
            else if (dimrow) c = dim565_q(c, 238);
            dst[x] = c;
            if (++ph == 3) ph = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Sprites and overlays (verbatim)
// ---------------------------------------------------------------------------

static void draw_ball_px(float bx, float by, float radius, float sh_dx, float sh_dy,
                         const uint8_t *tint)
{
    if (radius < 1.0f) return;
    int ir = (int)radius + 2;

    int shr = BALL_RADIUS;
    for (int dy = -shr - 1; dy <= shr + 1; dy++) {
        for (int dx = -shr - 1; dx <= shr + 1; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d > shr + 1.0f) continue;
            int px = (int)(bx + dx + sh_dx);
            int py = (int)(by + dy + sh_dy);
            if (px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;
            float f = 0.52f + 0.44f * (d / (shr + 1.0f));
            int idx = py * SCREEN_W + px;
            CV[idx] = dim565f(CV[idx], f);
        }
    }

    for (int dy = -ir; dy <= ir; dy++) {
        for (int dx = -ir; dx <= ir; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            float cov = clampf(radius - d + 0.5f, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            int px = (int)(bx + dx);
            int py = (int)(by + dy);
            if (px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;

            float nx = (float)dx / radius, ny = (float)dy / radius;
            float lam = 0.74f + 0.26f * clampf(-(nx * 0.55f + ny * 0.72f), -1.0f, 1.0f);
            float hx = nx + 0.35f, hy = ny + 0.42f;
            float hd = sqrtf(hx * hx + hy * hy);
            float spec = clampf(1.0f - hd / 0.45f, 0.0f, 1.0f) * 40.0f;
            int cr = (int)(tint[0] * lam + spec);
            int cg = (int)(tint[1] * lam + spec);
            int cb = (int)(tint[2] * lam + spec);
            if (cr > 255) cr = 255;
            if (cg > 255) cg = 255;
            if (cb > 255) cb = 255;

            int idx = py * SCREEN_W + px;
            if (cov >= 1.0f) {
                CV[idx] = pack_rgb(cr, cg, cb);
            } else {
                uint16_t bg = CV[idx];
                int br = ((bg >> 11) & 0x1F) << 3, bgc = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
                CV[idx] = pack_rgb((int)(br + (cr - br) * cov),
                                   (int)(bgc + (cg - bgc) * cov),
                                   (int)(bb + (cb - bb) * cov));
            }
        }
    }
}

void spawn_confetti(void)
{
    static const uint8_t pal[6][3] = {
        { 232, 62, 62 }, { 242, 202, 82 }, { 250, 250, 250 },
        { 96, 204, 122 }, { 236, 122, 192 }, { 112, 192, 238 },
    };
    for (int i = 0; i < MAX_CONF; i++) {
        float ang = -(float)M_PI * 0.5f + (randf() - 0.5f) * 1.9f;
        float spd = 2.0f + randf() * 4.5f;
        G->conf_x[i] = G->hole_x;
        G->conf_y[i] = G->hole_y;
        G->conf_vx[i] = cosf(ang) * spd;
        G->conf_vy[i] = sinf(ang) * spd - 1.0f;
        G->conf_life[i] = 55 + (int)(randf() * 40);
        const uint8_t *c = pal[(int)(randf() * 5.99f)];
        G->conf_col[i] = rgb565c(c[0], c[1], c[2]);
    }
    G->conf_active = true;
}

void update_confetti(void)
{
    if (!G->conf_active) return;
    G->conf_active = false;
    for (int i = 0; i < MAX_CONF; i++) {
        if (G->conf_life[i] <= 0) continue;
        G->conf_life[i]--;
        G->conf_vy[i] += 0.12f;
        G->conf_vx[i] *= 0.985f;
        G->conf_vy[i] *= 0.99f;
        G->conf_x[i] += G->conf_vx[i] + sinf(G->conf_life[i] * 0.55f + i) * 0.6f;
        G->conf_y[i] += G->conf_vy[i];
        if (G->conf_life[i] > 0) G->conf_active = true;
    }
}

static void draw_confetti(void)
{
    if (!G->conf_active) return;
    for (int i = 0; i < MAX_CONF; i++) {
        if (G->conf_life[i] <= 0) continue;
        int sx = (int)w2sx(G->conf_x[i]), sy = (int)w2sy(G->conf_y[i]);
        int sz = G->conf_life[i] > 35 ? 2 : 1;
        for (int dy = 0; dy <= sz; dy++)
            for (int dx = 0; dx <= sz; dx++)
                put_px(sx + dx, sy + dy, G->conf_col[i]);
    }
}

static void draw_flag_overlay(void)
{
    float s = G->cam_zoom;
    float fx = w2sx(G->hole_x), fy = w2sy(G->hole_y);
    if (fx < -40 || fx > SCREEN_W + 40 || fy < -20 || fy > SCREEN_H + 80) return;
    int fdir = (G->wind_speed > 0) ? (G->wind_x >= 0 ? 1 : -1)
                                   : ((G->hole_x > WORLD_W - 64) ? -1 : 1);
    int top = (int)(fy - 54 * s), bot = (int)(fy - 2 * s);
    for (int y = top; y < bot; y++) {
        put_px((int)fx, y, rgb565c(232, 232, 236));
        put_px((int)fx + 1, y, rgb565c(196, 197, 203));
        put_px((int)fx + 2, y, rgb565c(148, 150, 158));
    }
    for (int dy = -2; dy <= 0; dy++)
        for (int dx = 0; dx <= 2; dx++)
            put_px((int)fx + dx, top - 1 + dy, rgb565c(240, 208, 92));

    int prow = (int)(14 * s);
    if (G->wind_speed > 0) {
        float stretch = 0.60f + 0.55f * G->wind_speed;
        for (int i = 0; i < prow; i++) {
            int fw = (int)((prow - i) * stretch);
            for (int j = (int)(2 * s); j <= (int)(2 * s) + fw; j++) {
                float shade = 1.0f - i * 0.018f - j * 0.008f;
                put_px((int)(fx + fdir * j), top + 1 + i,
                       pack_rgb((int)(230 * shade), (int)(48 * shade), (int)(54 * shade)));
            }
        }
    } else {
        for (int i = 0; i < prow + 1; i++) {
            int fw = (prow - i) / 6 + 1;
            for (int j = (int)(2 * s); j < (int)(2 * s) + fw; j++) {
                float shade = 0.92f - i * 0.012f;
                put_px((int)(fx + fdir * j), top + 1 + i,
                       pack_rgb((int)(205 * shade), (int)(42 * shade), (int)(48 * shade)));
            }
        }
    }
}

static void draw_aim_dot(int ax, int ay, int rad)
{
    for (int dy = -(rad + 2); dy <= rad + 2; dy++) {
        for (int dx = -(rad + 2); dx <= rad + 2; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > (rad + 2) * (rad + 2)) continue;
            int px = ax + dx, py = ay + dy;
            if (px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;
            int idx = py * SCREEN_W + px;
            if (d2 <= rad * rad) CV[idx] = rgb565c(255, 255, 255);
            else CV[idx] = dim565f(CV[idx], 0.60f);
        }
    }
}

void golf_draw_triangle(float x0, float y0, float x1, float y1,
                        float x2, float y2, uint16_t col)
{
    int minx = (int)fminf(fminf(x0, x1), x2), maxx = (int)fmaxf(fmaxf(x0, x1), x2) + 1;
    int miny = (int)fminf(fminf(y0, y1), y2), maxy = (int)fmaxf(fmaxf(y0, y1), y2) + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > SCREEN_W - 1) maxx = SCREEN_W - 1;
    if (maxy > SCREEN_H - 1) maxy = SCREEN_H - 1;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float e0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
            float e1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
            float e2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                CV[py * SCREEN_W + px] = col;
        }
    }
}
#define draw_triangle golf_draw_triangle

static void draw_hole_indicator(void)
{
    float hsx = w2sx(G->hole_x), hsy = w2sy(G->hole_y);
    if (hsx >= 8 && hsx < SCREEN_W - 8 && hsy >= 8 && hsy < SCREEN_H - 8) return;

    float bx = w2sx(G->ball_x), by = w2sy(G->ball_y);
    float dx = hsx - bx, dy = hsy - by;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    dx /= len; dy /= len;

    float tmax = 1e9f;
    if (dx > 0.0001f) tmax = fminf(tmax, (SCREEN_W - 26 - bx) / dx);
    if (dx < -0.0001f) tmax = fminf(tmax, (26 - bx) / dx);
    if (dy > 0.0001f) tmax = fminf(tmax, (SCREEN_H - 70 - by) / dy);
    if (dy < -0.0001f) tmax = fminf(tmax, (74 - by) / dy);
    if (tmax < 16.0f) tmax = 16.0f;
    float ax = bx + dx * tmax, ay = by + dy * tmax;

    if (ax < 132.0f && ay < 132.0f) {
        float tx2 = dx < -0.0001f ? (132.0f - bx) / dx : 0.0f;
        float ty2 = dy < -0.0001f ? (132.0f - by) / dy : 0.0f;
        float tb = fmaxf(tx2, ty2);
        if (tb > 16.0f) {
            ax = bx + dx * tb;
            ay = by + dy * tb;
        }
    }

    float px_ = -dy, py_ = dx;
    draw_triangle(ax + dx * 13, ay + dy * 13,
                  ax - dx * 8 + px_ * 10, ay - dy * 8 + py_ * 10,
                  ax - dx * 8 - px_ * 10, ay - dy * 8 - py_ * 10,
                  rgb565c(255, 255, 255));
    draw_triangle(ax + dx * 9, ay + dy * 9,
                  ax - dx * 5 + px_ * 6, ay - dy * 5 + py_ * 6,
                  ax - dx * 5 - px_ * 6, ay - dy * 5 - py_ * 6,
                  rgb565c(214, 40, 46));

    G->ind_visible = true;
    G->ind_x = ax;
    G->ind_y = ay;
}

static void draw_wind_indicator(void)
{
    float cx = 62.0f, cy = 76.0f;
    if (G->wind_speed <= 0.0f) {
        for (int dy = -8; dy <= 8; dy++) {
            for (int dx = -8; dx <= 8; dx++) {
                float d = sqrtf((float)(dx * dx + dy * dy));
                int px = (int)cx + dx, py = (int)cy + dy;
                if (px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;
                int idx = py * SCREEN_W + px;
                if (d >= 3.6f && d <= 5.4f) CV[idx] = rgb565c(255, 255, 255);
                else if (d < 7.4f && d > 1.8f) CV[idx] = dim565f(CV[idx], 0.55f);
            }
        }
    } else {
        float m = sqrtf(G->wind_x * G->wind_x + G->wind_y * G->wind_y);
        float dx = G->wind_x / m, dy = G->wind_y / m;
        float len = 14.0f + G->wind_speed * 22.0f;

        float x0 = cx - dx * len * 0.5f, y0 = cy - dy * len * 0.5f;
        float x1 = cx + dx * len * 0.35f, y1 = cy + dy * len * 0.35f;
        int minx = (int)fminf(x0, x1) - 4, maxx = (int)fmaxf(x0, x1) + 4;
        int miny = (int)fminf(y0, y1) - 4, maxy = (int)fmaxf(y0, y1) + 4;
        float sdx2 = x1 - x0, sdy2 = y1 - y0;
        float seg2 = sdx2 * sdx2 + sdy2 * sdy2;
        for (int y = miny; y <= maxy; y++) {
            for (int x = minx; x <= maxx; x++) {
                if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) continue;
                float t = seg2 > 0 ? ((x - x0) * sdx2 + (y - y0) * sdy2) / seg2 : 0;
                t = clampf(t, 0, 1);
                float ddx = x - (x0 + sdx2 * t), ddy = y - (y0 + sdy2 * t);
                float d = sqrtf(ddx * ddx + ddy * ddy);
                int idx = y * SCREEN_W + x;
                if (d <= 1.4f) CV[idx] = rgb565c(255, 255, 255);
                else if (d <= 3.2f) CV[idx] = dim565f(CV[idx], 0.55f);
            }
        }

        float px_ = -dy, py_ = dx;
        draw_triangle(cx + dx * (len * 0.5f + 6), cy + dy * (len * 0.5f + 6),
                      cx + dx * len * 0.18f + px_ * 7, cy + dy * len * 0.18f + py_ * 7,
                      cx + dx * len * 0.18f - px_ * 7, cy + dy * len * 0.18f - py_ * 7,
                      rgb565c(24, 48, 24));
        draw_triangle(cx + dx * (len * 0.5f + 4), cy + dy * (len * 0.5f + 4),
                      cx + dx * len * 0.22f + px_ * 5, cy + dy * len * 0.22f + py_ * 5,
                      cx + dx * len * 0.22f - px_ * 5, cy + dy * len * 0.22f - py_ * 5,
                      rgb565c(255, 255, 255));
    }
    // wind intensity number rides under the rose (the old wind_label)
    gos_gfx_text565(14, 36, 96, rgb565c(255, 255, 255), "%d mph",
                    G->wind_speed > 0 ? (int)ceilf(G->wind_speed * 12.0f) : 0);
}

static void draw_power_meter(float pr)
{
    float wob = lie_wobble();
    if (wob > 0)
        pr = clampf(pr + sinf((float)(gos_time_us() % 1256637) * 5e-6f * 9.0f) *
                             0.05f * wob,
                    0.0f, 1.0f);
    int bx = (int)w2sx(G->ball_x) - 40;
    int by = (int)w2sy(G->ball_y);
    int x0 = bx - 5, x1 = bx + 5, y0 = by - 44, y1 = by + 44;
    if (x0 < 2 || x1 >= SCREEN_W - 2 || y0 < 12 || y1 >= SCREEN_H - 2) return;

    int inner_h = y1 - y0 - 2;
    int fill_h = (int)(pr * inner_h);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int idx = y * SCREEN_W + x;
            if (x == x0 || x == x1 || y == y0 || y == y1)
                CV[idx] = rgb565c(190, 178, 146);
            else if ((y1 - 1 - y) < fill_h)
                CV[idx] = rgb565c(255, 255, 255);
            else
                CV[idx] = dim565f(CV[idx], 0.45f);
        }
    }

    float pin_d = dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y);
    if (shot_travel_frac(1.0f) >= pin_d) {
        float lo = 0.02f, hi = 1.0f;
        for (int it = 0; it < 12; it++) {
            float mid = (lo + hi) * 0.5f;
            if (shot_travel_frac(mid) < pin_d) lo = mid;
            else hi = mid;
        }
        int ty = y1 - 1 - (int)((lo + hi) * 0.5f * inner_h);
        for (int y = ty - 1; y <= ty + 1; y++) {
            if (y <= y0 || y >= y1) continue;
            for (int x = x0 - 3; x <= x1 + 3; x++) {
                if (x < 0 || x >= SCREEN_W) continue;
                CV[y * SCREEN_W + x] = rgb565c(214, 40, 46);
            }
        }
    } else {
        draw_triangle((float)bx, (float)(y0 - 10), (float)(bx - 5), (float)(y0 - 3),
                      (float)(bx + 5), (float)(y0 - 3), rgb565c(214, 40, 46));
    }
}

static void draw_aim_preview(void)
{
    float dx = G->drag_start_x - G->drag_cur_x;
    float dy = G->drag_start_y - G->drag_cur_y;
    float drag_dist = sqrtf(dx * dx + dy * dy);
    if (drag_dist <= 6) return;

    float power = drag_dist * DRAG_SCALE;
    if (power > MAX_POWER) power = MAX_POWER;

    float v = power * club_mult[G->club];
    float vx = dx / drag_dist * v, vy = dy / drag_dist * v;
    float wob = lie_wobble();
    float ph = (float)(gos_time_us() % 1256637) * 5e-6f;
    float px = G->ball_x, py = G->ball_y;
    int airT = (int)(v * club_airk[G->club]);
    float curl = club_curl[G->club];

    int step = 0, dots = 0;
    for (int f = 0; f < 120 && dots < 12; f++) {
        if (airT > 0) {
            if (curl != 0) {
                float ca = cosf(curl), sa = sinf(curl);
                float nvx = vx * ca - vy * sa;
                vy = vx * sa + vy * ca;
                vx = nvx;
            }
            px += vx; py += vy;
            airT--;
            if (airT == 0) { vx *= club_keep[G->club]; vy *= club_keep[G->club]; }
        } else {
            px += vx; py += vy;
            vx *= FRICTION; vy *= FRICTION;
            if (vx * vx + vy * vy < 2.5f) break;
        }
        if (++step == 3) {
            step = 0;
            float ox = 0, oy = 0;
            if (wob > 0) {
                float vl = sqrtf(vx * vx + vy * vy);
                if (vl > 0.01f) {
                    float w = sinf(dots * 1.1f - ph * 6.0f) * (1.5f + dots * 0.35f) * wob;
                    ox = -vy / vl * w;
                    oy = vx / vl * w;
                }
            }
            int ax = (int)w2sx(px + ox), ay = (int)w2sy(py + oy);
            if (ax >= 5 && ax < SCREEN_W - 5 && ay >= 5 && ay < SCREEN_H - 5)
                draw_aim_dot(ax, ay, dots < 6 ? 3 : 2);
            dots++;
        }
    }
}

static void draw_aim_ray(float dirx, float diry)
{
    float wob = lie_wobble();
    float ph = (float)(gos_time_us() % 1256637) * 5e-6f;
    float perpx = -diry, perpy = dirx;
    for (int i = 1; i <= 10; i++) {
        float w = wob > 0 ? sinf(i * 1.1f - ph * 6.0f) * (1.5f + i * 0.35f) * wob
                          : 0.0f;
        float px = G->ball_x + dirx * i * 18.0f + perpx * w;
        float py = G->ball_y + diry * i * 18.0f + perpy * w;
        int ax = (int)w2sx(px), ay = (int)w2sy(py);
        if (ax >= 5 && ax < SCREEN_W - 5 && ay >= 5 && ay < SCREEN_H - 5)
            draw_aim_dot(ax, ay, i < 6 ? 3 : 2);
    }
}

static void draw_swing_bar(void)
{
    int x0 = 14, x1 = 26;
    int y0 = (SCREEN_H - 180) / 2, y1 = y0 + 180;
    int inner_h = y1 - y0 - 2;
    float fill = G->swing_bar_fill;
    float wob = G->st == GST_ARMED ? lie_wobble() : 0.0f;
    if (wob > 0)
        fill = clampf(fill + sinf((float)(gos_time_us() % 1256637) * 5e-6f * 9.0f) *
                                 0.05f * wob,
                      0.0f, 1.0f);
    int fill_h = (int)(fill * inner_h);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int idx = y * SCREEN_W + x;
            if (x == x0 || x == x1 || y == y0 || y == y1)
                CV[idx] = rgb565c(190, 178, 146);
            else if ((y1 - 1 - y) < fill_h)
                CV[idx] = rgb565c(255, 255, 255);
            else
                CV[idx] = dim565f(CV[idx], 0.45f);
        }
    }
    if (G->swing_tick >= 0) {
        int ty = y1 - 1 - (int)(G->swing_tick * inner_h);
        for (int y = ty - 1; y <= ty + 1; y++) {
            if (y <= y0 || y >= y1) continue;
            for (int x = x0 - 3; x <= x1 + 3; x++) {
                if (x < 0 || x >= SCREEN_W) continue;
                CV[y * SCREEN_W + x] = rgb565c(214, 40, 46);
            }
        }
    } else {
        float mx = (x0 + x1) * 0.5f;
        draw_triangle(mx, (float)(y0 - 10), mx - 5, (float)(y0 - 3),
                      mx + 5, (float)(y0 - 3), rgb565c(214, 40, 46));
    }
}

// ---------------------------------------------------------------------------
// HUD pills (the standalone's LVGL labels, redrawn as canvas pills)
// ---------------------------------------------------------------------------

// cream stadium capsule blended over the course, tan border, given opacity
static void blend_pill(int x0, int y0, int x1, int y1, float opa)
{
    int r = (y1 - y0) / 2;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) continue;
            int cx2 = x < x0 + r ? x0 + r - x : (x > x1 - r ? x - (x1 - r) : 0);
            int cy2 = y < y0 + r ? y0 + r - y : (y > y1 - r ? y - (y1 - r) : 0);
            int d2 = cx2 * cx2 + cy2 * cy2;
            if (d2 > r * r) continue;
            bool border = d2 > (r - 2) * (r - 2) ||
                          x < x0 + 2 || x > x1 - 2 || y < y0 + 2 || y > y1 - 2;
            int pr = border ? 190 : 243, pg = border ? 178 : 236, pb = border ? 146 : 214;
            int idx = y * SCREEN_W + x;
            uint16_t o = CV[idx];
            int orr = ((o >> 11) & 0x1F) << 3;
            int ogg = ((o >> 5) & 0x3F) << 2;
            int obb = (o & 0x1F) << 3;
            CV[idx] = pack_rgb((int)(orr + (pr - orr) * opa),
                               (int)(ogg + (pg - ogg) * opa),
                               (int)(obb + (pb - obb) * opa));
        }
    }
}

static void hud_pill(int px, int py, int align_right, int font,
                     uint16_t txt_col, float opa, const char *txt)
{
    int tw = gos_gfx_text565_w(font, txt);
    int th = gos_gfx_text565_h(font);
    int pad_h = font >= 22 ? 16 : 13, pad_v = font >= 22 ? 9 : 5;
    int x0 = align_right ? SCREEN_W - px - tw - 2 * pad_h : px;
    int y0 = py;
    blend_pill(x0, y0, x0 + tw + 2 * pad_h, y0 + th + 2 * pad_v, opa);
    gos_gfx_text565(font, x0 + pad_h, y0 + pad_v, txt_col, "%s", txt);
}

static void draw_hud_pills(void)
{
    char buf[48];
    uint16_t green = rgb565c(0x1E, 0x45, 0x26);

    snprintf(buf, sizeof buf, "Hole %d  \xE2\x80\xA2  Par %d", G->current_hole + 1, G->par);
    hud_pill(20, 16, 0, 14, green, 0.60f, buf);

    if (G->num_players > 1)
        snprintf(buf, sizeof buf, "P%d  \xE2\x80\xA2  Strokes %d", G->cur_player + 1,
                 G->stroke_count);
    else
        snprintf(buf, sizeof buf, "Strokes %d", G->stroke_count);
    hud_pill(20, 16, 1, 14, green, 0.60f, buf);
    if (G->num_players > 1) {
        // border doubles as the player's color swatch: a ring redraw
        const uint8_t *t = ball_rgb[G->players[G->cur_player].color];
        int tw = gos_gfx_text565_w(14, buf) + 26;
        int x0 = SCREEN_W - 20 - tw, y1 = 16 + gos_gfx_text565_h(14) + 10;
        for (int x = x0; x < x0 + tw; x++) {
            put_px(x, 16, rgb565c(t[0], t[1], t[2]));
            put_px(x, y1, rgb565c(t[0], t[1], t[2]));
        }
    }

    // club button pill, dimmed except when usable
    float opa = G->st == GST_READY ? 0.90f : 0.30f;
    hud_pill(18, SCREEN_H - 16 - gos_gfx_text565_h(22) - 18, 0, 22,
             G->st == GST_READY ? green : rgb565c(90, 104, 84), opa,
             club_names[G->club]);

    // status pill, bottom-right (notice while it lasts; SWING! while armed)
    const char *status = NULL;
    if (G->st == GST_ARMED) status = "SWING!";
    else if (G->st == GST_READY && gos_time_us() < G->notice_until_us)
        status = G->notice;
    if (status && status[0]) {
        int th = gos_gfx_text565_h(14);
        hud_pill(18, SCREEN_H - 18 - th - 10, 1, 14, green, 0.90f, status);
    }

    // idle tooltip: bare white text under the ball after ~2.5s of nothing
    if (G->st == GST_READY && G->idle_frames > 125) {
        const char *tip = "Drag anywhere to aim";
        int tw = gos_gfx_text565_w(14, tip);
        gos_gfx_text565(14, SCREEN_W / 2 - tw / 2, SCREEN_H / 2 - 70,
                        rgb565c(255, 255, 255), "%s", tip);
    }
}

// ---------------------------------------------------------------------------
// The in-play frame (render_frame, verbatim structure)
// ---------------------------------------------------------------------------

// lerp helper with a teleport snap-guard: anything moving farther than one
// tick could carry it was repositioned (splash return, turn swap, new hole)
// and must not streak across the world
static inline float lerp_snap(float prev, float cur, float a, float max_step)
{
    float d = cur - prev;
    if (d > max_step || d < -max_step) return cur;
    return prev + d * a;
}

static void render_frame_at(void);

void render_frame(void)
{
    G->ind_visible = false;

    // draw this frame at an interpolated instant between logic ticks: the
    // ball and camera glide at 60 fps however slow the simulation ticks
    float a = G->render_alpha;
    float save_bx = G->ball_x, save_by = G->ball_y;
    float save_cx = G->cam_x, save_cy = G->cam_y, save_cz = G->cam_zoom;
    G->ball_x = lerp_snap(G->prev_ball_x, G->ball_x, a, 40.0f);
    G->ball_y = lerp_snap(G->prev_ball_y, G->ball_y, a, 40.0f);
    G->cam_x = lerp_snap(G->prev_cam_x, G->cam_x, a, 90.0f);
    G->cam_y = lerp_snap(G->prev_cam_y, G->cam_y, a, 90.0f);
    G->cam_zoom = lerp_snap(G->prev_cam_zoom, G->cam_zoom, a, 0.2f);
    render_frame_at();
    G->ball_x = save_bx;
    G->ball_y = save_by;
    G->cam_x = save_cx;
    G->cam_y = save_cy;
    G->cam_zoom = save_cz;
}

static void render_frame_at(void)
{
    if (G->st == GST_WIPE) {
        int w = G->wipe_x < SCREEN_W ? G->wipe_x : SCREEN_W;
        blit_view(w);
        for (int y = 0; y < SCREEN_H; y++)
            for (int x = w; x < w + 2 && x < SCREEN_W; x++)
                CV[y * SCREEN_W + x] = rgb565c(238, 231, 210);
        return;
    }

    blit_view(SCREEN_W);

    float ball_sx = w2sx(G->ball_x), ball_sy = w2sy(G->ball_y);

    if (G->is_dragging && G->st == GST_AIMING) {
        if (G->swing_mode && G->imu_ok) {
            float ddx = G->drag_start_x - G->drag_cur_x;
            float ddy = G->drag_start_y - G->drag_cur_y;
            float dd = sqrtf(ddx * ddx + ddy * ddy);
            if (dd > 6) draw_aim_ray(ddx / dd, ddy / dd);
        } else {
            draw_aim_preview();
            if (G->club != CLUB_PUTTER) {
                float ddx = G->drag_start_x - G->drag_cur_x;
                float ddy = G->drag_start_y - G->drag_cur_y;
                float dd = sqrtf(ddx * ddx + ddy * ddy);
                if (dd > 6) {
                    float power = dd * DRAG_SCALE;
                    if (power > MAX_POWER) power = MAX_POWER;
                    draw_power_meter(power / MAX_POWER);
                }
            }
        }
    } else if (G->st == GST_ARMED) {
        draw_aim_ray(G->aim_dx, G->aim_dy);
    }

    if (G->st == GST_ARMED ||
        (G->swing_bar_frames > 0 && (G->st == GST_MOVING || G->st == GST_READY)))
        draw_swing_bar();

    if (G->num_players > 1) {
        for (int i = 0; i < G->num_players; i++) {
            if (i == G->cur_player || G->players[i].holed) continue;
            draw_ball_px(w2sx(G->players[i].x), w2sy(G->players[i].y),
                         BALL_RADIUS * G->cam_zoom, 2.5f * G->cam_zoom,
                         3.0f * G->cam_zoom, ball_rgb[G->players[i].color]);
        }
    }

    if (G->st == GST_HOLE_IN) {
        if (G->drop_anim > 0) {
            float t = 1.0f - (float)G->drop_anim / DROP_ANIM_FRAMES;
            float bx = G->drop_from_x + (G->hole_x - G->drop_from_x) * t;
            float by = G->drop_from_y + (G->hole_y - G->drop_from_y) * t;
            draw_ball_px(w2sx(bx), w2sy(by),
                         BALL_RADIUS * G->cam_zoom * (1.0f - t * 0.85f), 2.0f, 2.0f,
                         ball_rgb[G->players[G->cur_player].color]);
        }
    } else if (G->st != GST_INTRO || G->intro_t > INTRO_HOLD) {
        float h = 0.0f;
        if (G->air_total > 0 && G->air_time > 0) {
            float p = 1.0f - (float)G->air_time / G->air_total;
            h = 4.0f * p * (1.0f - p);
        }
        draw_ball_px(ball_sx, ball_sy - h * 10.0f * G->cam_zoom,
                     BALL_RADIUS * G->cam_zoom * (1.0f + 0.55f * h),
                     (2.5f + h * 10.0f) * G->cam_zoom, (3.0f + h * 16.0f) * G->cam_zoom,
                     ball_rgb[G->players[G->cur_player].color]);
    }

    draw_flag_overlay();
    if (G->st == GST_HOLE_IN) draw_confetti();

    if (G->st == GST_READY || G->st == GST_AIMING || G->st == GST_ARMED ||
        G->st == GST_MOVING)
        draw_hole_indicator();

    if (G->st == GST_READY || G->st == GST_AIMING || G->st == GST_ARMED ||
        G->st == GST_MOVING || G->st == GST_INTRO)
        draw_wind_indicator();

    // HUD pills (hidden during the flyover's flag-hold beat, like hud_show)
    if (G->st != GST_INTRO || G->intro_t > INTRO_HOLD) draw_hud_pills();

    // distance number pinned just inside the edge arrow
    if (G->ind_visible) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d",
                 (int)dist2d(G->ball_x, G->ball_y, G->hole_x, G->hole_y));
        float ddx = SCREEN_W * 0.5f - G->ind_x, ddy = SCREEN_H * 0.5f - G->ind_y;
        float dl = sqrtf(ddx * ddx + ddy * ddy);
        if (dl > 1) { ddx /= dl; ddy /= dl; }
        gos_gfx_text565(14, (int)(G->ind_x + ddx * 30) - 12,
                        (int)(G->ind_y + ddy * 30) - 9,
                        rgb565c(255, 255, 255), "%s", buf);
    }

    if (G->st == GST_INTRO) {
        const char *skip = "tap to skip";
        int tw = gos_gfx_text565_w(14, skip);
        gos_gfx_text565(14, SCREEN_W - tw - 20, SCREEN_H - 30,
                        rgb565c(243, 236, 216), "%s", skip);
    }
}
