// bomber_terrain.c — the endless countryside, one row at a time.
//
// Every pixel is a pure function of (sortie_seed, world row, x): hedgerowed
// fields with per-field tints, a meandering river, a north-south road with
// dashes, villages of little roofed houses, scattered trees, and a sea
// band at the strip's end. Target and flak-site art is painted into rows
// as they generate; craters and scorch marks are stamped destructively
// afterward and simply scroll away. Rows are generated exactly once.

#include "bomber_int.h"

static uint32_t seed_lo;

uint32_t bomber_hash(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed_lo;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float bomber_noise(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    float v00 = (bomber_hash(ix, iy) & 0xFFFF) / 65536.0f;
    float v10 = (bomber_hash(ix + 1, iy) & 0xFFFF) / 65536.0f;
    float v01 = (bomber_hash(ix, iy + 1) & 0xFFFF) / 65536.0f;
    float v11 = (bomber_hash(ix + 1, iy + 1) & 0xFFFF) / 65536.0f;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float v0 = v00 + sx * (v10 - v00), v1 = v01 + sx * (v11 - v01);
    return v0 + sy * (v1 - v0);
}

float road_cx(float row)
{
    // a country lane, not a motorway: broad drift plus noisy local wander
    float t = bclampf(row / (float)SORTIE_ROWS, 0.0f, 1.0f);
    float base = G->road_x0 + (G->road_x1 - G->road_x0) * t;
    return base + sinf(row * 0.006f + G->river_phase * 2.1f) * 40.0f +
           (bomber_noise(row * 0.015f, 91.7f) - 0.5f) * 44.0f;
}

uint16_t bomber_sea_px(int row, int x)
{
    int r_ = 30, g_ = 76, b_ = 140;
    float wv = sinf(row * 0.10f + bomber_noise(x * 0.03f, row * 0.03f) * 6.0f +
                    x * 0.015f);
    if (wv > 0.86f) { int l = (int)((wv - 0.86f) * 260); r_ += l; g_ += l; b_ += l; }
    if ((bomber_hash(x / 2, row) % 61u) == 0) { r_ += 55; g_ += 55; b_ += 45; }
    return b565(r_, g_, b_);
}

float river_cx(float row)
{
    // three meander scales + noise: a river that actually snakes
    return G->river_cx0 + sinf(row * 0.004f + G->river_phase) * G->river_amp +
           sinf(row * 0.0013f) * 90.0f +
           sinf(row * 0.012f + G->river_phase * 2.7f) * 46.0f +
           (bomber_noise(row * 0.02f, 17.3f) - 0.5f) * 36.0f;
}

// ---------------------------------------------------------------------------
// Sortie planning: deterministic layout of targets, flak, roads, river
// ---------------------------------------------------------------------------

static float plan_randf(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return (float)(*s >> 16) / 65536.0f;
}

void terrain_reset(uint32_t seed)
{
    seed_lo = seed;
    uint32_t s = seed ^ 0xB0B0E5u;

    G->road_x0 = 140 + plan_randf(&s) * 456;
    G->road_x1 = 140 + plan_randf(&s) * 456;
    G->river_phase = plan_randf(&s) * 6.28f;
    G->river_amp = 120 + plan_randf(&s) * 140;
    G->river_cx0 = 220 + plan_randf(&s) * 296;

    // targets every 700-900 rows, first at ~500 so its bracket is on screen
    // within a second or two of leaving the brief (playbook rule, sim-checked)
    int n = 4 + (int)(plan_randf(&s) * 2.99f);
    if (n > MAX_TARGETS) n = MAX_TARGETS;
    float defended_frac = bclampf(0.5f + 0.1f * (G->sortie_num - 1), 0.5f, 1.0f);
    float row = 420;   // first bracket on screen inside 1.5 s
    G->n_targets = 0;
    G->n_flak = 0;
    G->n_trucks = 0;
    for (int i = 0; i < n; i++) {
        target_t *t = &G->targets[G->n_targets];
        float r = plan_randf(&s);
        if (G->sortie_num >= 3 && r < 0.25f) t->kind = TGT_CONVOY;
        else if (r < 0.45f) t->kind = TGT_DEPOT;
        else if (r < 0.70f) t->kind = TGT_RADAR;
        else t->kind = TGT_BRIDGE;
        t->y = row;
        if (t->kind == TGT_BRIDGE) {
            t->x = river_cx(row);          // bridges live on the river
        } else if (t->kind == TGT_CONVOY) {
            t->x = road_cx(row);           // convoys live on the road
        } else {
            t->x = 90 + plan_randf(&s) * (STRIP_W - 180);
            // keep static sites out of the river
            float rc = river_cx(row);
            if (fabsf(t->x - rc) < 70) t->x = rc + (t->x > rc ? 70 : -70);
        }
        t->state = TS_INTACT;
        t->defended = plan_randf(&s) < defended_frac;
        if (t->defended && G->n_flak < MAX_FLAK) {
            flak_t *f = &G->flak[G->n_flak++];
            float a = plan_randf(&s) * 6.28f;
            f->x = bclampf(t->x + cosf(a) * 90, 30, STRIP_W - 30);
            f->y = t->y + sinf(a) * 90;
            f->next_fire_us = 0;
            f->telegraphed = false;
            // dense escalation: a second battery on late sorties
            if (G->sortie_num >= 4 && G->n_flak < MAX_FLAK && plan_randf(&s) < 0.5f) {
                flak_t *f2 = &G->flak[G->n_flak++];
                f2->x = bclampf(t->x - cosf(a) * 110, 30, STRIP_W - 30);
                f2->y = t->y - sinf(a) * 60;
                f2->next_fire_us = 0;
                f2->telegraphed = false;
            }
        }
        if (t->kind == TGT_CONVOY) {
            for (int k = 0; k < 3 && G->n_trucks < MAX_TRUCKS; k++) {
                G->trucks[G->n_trucks].y = row + k * 26;
                G->trucks[G->n_trucks].target_ix = (int8_t)G->n_targets;
                G->trucks[G->n_trucks].alive = true;
                G->n_trucks++;
            }
        }
        G->n_targets++;
        row += 700 + plan_randf(&s) * 200;
        if (row > SORTIE_ROWS - COAST_ROWS - 200) break;
    }

    // wind for the sortie
    float wang = plan_randf(&s) * 6.28f;
    G->wind_base = bclampf(8.0f + 6.0f * G->sortie_num, 8.0f, 45.0f);
    G->wind_base_x = cosf(wang) * G->wind_base;
    G->wind_base_y = sinf(wang) * G->wind_base;
    G->gust_t = 0;

    G->gen_row = 0;
}

// ---------------------------------------------------------------------------
// Per-row painter
// ---------------------------------------------------------------------------

// target art, painted per row while the terrain generates
void paint_target_row(const target_t *t, int row, uint16_t *rowbuf)
{
    int dy = row - (int)t->y;
    switch (t->kind) {
    case TGT_DEPOT: {
        // three gray sheds with ridged roofs inside a fenced yard
        if (dy < -34 || dy > 34) return;
        for (int x = -40; x <= 40; x++) {
            int px = (int)t->x + x;
            if (px < 0 || px >= STRIP_W) continue;
            bool fence = (dy == -34 || dy == 34 || x == -40 || x == 40);
            if (fence && ((x + dy) & 3) < 2) { rowbuf[px] = b565(92, 88, 78); continue; }
            int shed = -1;
            if (x >= -34 && x <= -12) shed = x + 34;
            else if (x >= -8 && x <= 14) shed = x + 8;
            else if (x >= 18 && x <= 36) shed = x - 18;
            if (shed >= 0 && dy >= -24 && dy <= 24) {
                int lum = 120 - abs(shed - 11) * 3 + ((dy & 7) == 0 ? 14 : 0);
                rowbuf[px] = b565(lum, lum, lum - 6);
            } else if (dy > -34 && dy < 34) {
                rowbuf[px] = b565(122, 110, 88);   // packed-earth yard
            }
        }
        break;
    }
    case TGT_RADAR: {
        // concrete apron ring + the dish (a bright ellipse with a shadow)
        if (dy < -30 || dy > 30) return;
        for (int x = -30; x <= 30; x++) {
            int px = (int)t->x + x;
            if (px < 0 || px >= STRIP_W) continue;
            float d = sqrtf((float)(x * x + dy * dy));
            if (d < 28 && d > 23) rowbuf[px] = b565(150, 148, 140);
            else if (d <= 23) rowbuf[px] = b565(108, 106, 100);
            if (d < 12) {
                int lum = 190 - (int)(d * 6) + ((x - dy) > 0 ? 20 : -10);
                rowbuf[px] = b565(lum, lum, lum);
            }
        }
        break;
    }
    case TGT_BRIDGE: {
        // deck across the river with trusses, and gravel approach tracks
        // running off both ends so the bridge meets the land naturally
        if (dy < -12 || dy > 12) return;
        int half = 40;
        if (dy >= -3 && dy <= 3) {
            for (int side = -1; side <= 1; side += 2) {
                for (int x = half; x <= half + 74; x++) {
                    int px = (int)t->x + side * x;
                    if (px < 0 || px >= STRIP_W) continue;
                    // the approach curves gently away and fades out
                    int bend = (x - half) * (x - half) / 260;
                    int py_ok = abs(dy + (side > 0 ? bend : -bend));
                    if (py_ok > 2) continue;
                    uint32_t h = bomber_hash(px, row);
                    if (x > half + 56 && (h % 3u) == 0) continue;   // fade
                    int lum = (int)(h % 11u);
                    rowbuf[px] = b565(140 + lum, 126 + lum, 94 + lum / 2);
                }
            }
        }
        for (int x = -half; x <= half; x++) {
            int px = (int)t->x + x;
            if (px < 0 || px >= STRIP_W) continue;
            if (dy >= -8 && dy <= 8) {
                bool truss = (abs(dy) == 8) || ((x & 15) == 0);
                rowbuf[px] = truss ? b565(70, 62, 52) : b565(128, 118, 96);
            }
        }
        break;
    }
    default:
        break;   // convoys are sprites, not stamped art
    }
}

static void paint_flak_row(const flak_t *f, int row, uint16_t *rowbuf)
{
    int dy = row - (int)f->y;
    if (dy < -9 || dy > 9) return;
    for (int x = -9; x <= 9; x++) {
        int px = (int)f->x + x;
        if (px < 0 || px >= STRIP_W) continue;
        float d = sqrtf((float)(x * x + dy * dy));
        if (d < 9 && d > 6) rowbuf[px] = b565(134, 120, 92);   // sandbag ring
        else if (d <= 3) rowbuf[px] = b565(60, 62, 58);        // the gun
    }
}

static void gen_row(int row)
{
    uint16_t *out = &G->ring[(row & RING_MASK) * STRIP_W];

    // coast bands: the sortie INGRESSES from the sea over a beach (the
    // plane takes off over water — rows behind the strip render as sea),
    // and the far end fades out to open sea again
    int to_coast = (SORTIE_ROWS - COAST_ROWS) - row;
    float sea = to_coast <= 0
        ? bclampf((float)(-to_coast) / 90.0f, 0.0f, 1.0f) : -1.0f;
    if (row < 140) {
        float s_in = bclampf((float)(140 - row) / 90.0f, 0.0f, 1.0f);
        if (s_in > sea) sea = s_in;
    }

    float rvc = river_cx((float)row);
    float rw = 13.0f + bomber_noise(row * 0.02f, 7.7f) * 6.0f;
    float rdc = road_cx((float)row);

    // coarse 2-octave meadow noise, sampled every 8 px and lerped
    float ns[STRIP_W / 8 + 2];
    for (int i = 0; i <= STRIP_W / 8; i++) {
        float wx = i * 8.0f;
        ns[i] = bomber_noise(wx * 0.02f, row * 0.02f) * 0.7f +
                bomber_noise(wx * 0.09f + 31.0f, row * 0.09f) * 0.3f;
    }
    // domain-warp offsets for the plot pattern, coarse-sampled: this bends
    // every field boundary organically so plots never read as graph paper
    float wox[STRIP_W / 8 + 2], woy[STRIP_W / 8 + 2];
    for (int i = 0; i <= STRIP_W / 8; i++) {
        float wx = i * 8.0f;
        wox[i] = (bomber_noise(wx * 0.011f + 57.0f, row * 0.011f) - 0.5f) * 64.0f;
        woy[i] = (bomber_noise(wx * 0.009f + 13.0f, row * 0.009f + 41.0f) - 0.5f) * 80.0f;
    }

    for (int x = 0; x < STRIP_W; x++) {
        float fr8 = (x & 7) / 8.0f;
        float n = ns[x >> 3] + (ns[(x >> 3) + 1] - ns[x >> 3]) * fr8;
        int r_, g_, b_;

        // distances to the water features FIRST: every land feature reacts
        // to how close the river is (the floodplain treatment)
        float rd = fabsf(x - rvc);
        float dd = fabsf(x - rdc);
        // floodplain: 0 far from the river -> 1 at the bank
        float fp = rd < rw + 26 ? bclampf((rw + 26 - rd) / 26.0f, 0.0f, 1.0f) : 0.0f;

        // field patchwork in WARPED coordinates: irregular plot shapes and
        // sizes, boundaries that meander like real hedgerows
        float wxa = x + wox[x >> 3] + (wox[(x >> 3) + 1] - wox[x >> 3]) * fr8;
        float wya = row + woy[x >> 3] + (woy[(x >> 3) + 1] - woy[x >> 3]) * fr8;
        int fx = (int)floorf(wxa / 56.0f), fy = (int)floorf(wya / 72.0f);
        uint32_t fh = bomber_hash(fx, fy * 7 + 3);
        int tint = (int)(fh % 5u);
        switch (tint) {
        case 0: r_ = 96; g_ = 128; b_ = 58; break;    // pasture
        case 1: r_ = 118; g_ = 138; b_ = 62; break;   // hay
        case 2: r_ = 132; g_ = 118; b_ = 70; break;   // stubble
        case 3: r_ = 108; g_ = 96; b_ = 66; break;    // plowed
        default: r_ = 84; g_ = 120; b_ = 66; break;   // young crop
        }
        int m = (int)((n - 0.5f) * 44.0f);
        r_ += m; g_ += m; b_ += m / 2;
        // plow furrows on worked fields (never plowed to the water's edge)
        if ((tint == 2 || tint == 3) && fp < 0.4f &&
            ((x + (int)(fh & 7)) % 6) < 2) {
            r_ -= 12; g_ -= 12; b_ -= 8;
        }
        // riverside meadow: cultivation gives way to lush rough grazing
        if (fp > 0.0f) {
            float k = fp * 0.7f;
            r_ = (int)(r_ + (74 - r_) * k);
            g_ = (int)(g_ + (128 - g_) * k);
            b_ = (int)(b_ + (58 - b_) * k);
        }
        // borders in warped space: usually a gappy hedgerow, but some plot
        // edges carry a dirt lane instead — an organic country road network.
        // Both stop short of the bank: nobody hedges into a river.
        float bxf = wxa - fx * 56.0f, byf = wya - fy * 72.0f;
        bool on_vx = bxf < 3.0f, on_hy = byf < 3.0f;
        if ((on_vx || on_hy) && fp < 0.55f) {
            uint32_t eh = on_vx ? bomber_hash(fx * 3 + 1, fy)      // per-EDGE
                                : bomber_hash(fx, fy * 5 + 2);     // identity
            if ((eh % 4u) == 0) {
                // dirt lane: rutted tan track
                int lum = (int)(bomber_hash(x, row) % 11u);
                r_ = 136 + lum; g_ = 120 + lum; b_ = 88 + lum / 2;
                if ((on_vx && bxf > 1.0f && bxf < 2.0f) ||
                    (on_hy && byf > 1.0f && byf < 2.0f)) {
                    r_ -= 18; g_ -= 16; b_ -= 12;   // wheel ruts
                }
            } else if ((bomber_hash(x >> 2, row >> 2) % 7u) != 0) {
                r_ = 52 + m / 3; g_ = 74 + m / 3; b_ = 40;   // hedgerow
            }
        }
        // trees: hashed 16px cells — and willows crowd the banks, so the
        // riverside band rolls the dice twice as often
        uint32_t th = bomber_hash(x >> 4, (row >> 4) * 3 + 1);
        bool tree_here = (th % 11u) == 0 || (fp > 0.35f && (th % 7u) == 1);
        if (tree_here) {
            int cx2 = (x & 15) - 8 + (int)(th % 5u) - 2;
            int cy2 = (row & 15) - 8 + (int)((th >> 4) % 5u) - 2;
            float td = sqrtf((float)(cx2 * cx2 + cy2 * cy2));
            if (td < 5.0f) {
                int lift = cx2 - cy2 < 0 ? 16 : -6;
                if (fp > 0.35f) { r_ = 52 + lift / 2; g_ = 92 + lift; b_ = 44; }
                else { r_ = 38 + lift / 2; g_ = 66 + lift; b_ = 34; }
            } else if (td < 6.5f) {
                r_ -= 20; g_ -= 20; b_ -= 12;   // canopy shadow rim
            }
        }

        // the main road: a narrow wandering gravel track (1940s country,
        // not motorway). Painted before the river so a crossing becomes a
        // FORD through the shallows rather than a hard cut.
        bool on_road = dd < 4.5f;
        if (on_road && rd >= rw) {
            int lum = (int)(bomber_hash(x, row) % 13u);
            r_ = 142 + lum; g_ = 128 + lum; b_ = 96 + lum / 2;
            if (dd > 1.2f && dd < 2.4f) { r_ -= 20; g_ -= 18; b_ -= 12; }  // ruts
        } else if (dd < 6.5f && rd >= rw + 3) {
            r_ = (r_ * 4) / 5; g_ = (g_ * 4) / 5; b_ = (b_ * 4) / 5;  // worn verge
        }

        // the river and everything that meets it
        if (rd < rw) {
            float depth = 1.0f - rd / rw;
            if (on_road) {
                // ford: the lane wades through on a gravel bar — pale
                // shallow water over stone, with a ripple line downstream
                int lum = (int)(bomber_hash(x, row) % 9u);
                r_ = 118 + lum; g_ = 126 + lum; b_ = 118 + lum;
                if (rd > rw - 3) { r_ += 22; g_ += 20; b_ += 8; }
            } else {
                r_ = 52 - (int)(depth * 22);
                g_ = 92 - (int)(depth * 26);
                b_ = 130 - (int)(depth * 20);
                // gravel shoals in the slack shallows near the banks
                float sh = bomber_noise(x * 0.05f + 7.0f, row * 0.05f);
                if (depth < 0.30f && sh > 0.68f) {
                    r_ = 140; g_ = 130; b_ = 96;
                    if (depth > 0.18f) { r_ -= 34; g_ -= 26; b_ -= 4; }  // awash
                } else if ((bomber_hash(x, row / 2) % 37u) == 0) {
                    r_ += 40; g_ += 40; b_ += 30;
                }
            }
        } else if (rd < rw + 2.0f) {
            // wet mud right at the waterline
            int lum = (int)(bomber_hash(x, row) % 7u);
            r_ = 104 + lum; g_ = 92 + lum; b_ = 68;
        } else if (rd < rw + 5.0f && !on_road) {
            // dry sandy bank above the mud
            r_ = 150; g_ = 138; b_ = 100;
        }
        // reeds: dark tufts scattered along the margins on both sides
        if (rd >= rw - 2 && rd < rw + 8 && !on_road &&
            (bomber_hash(x, row) % 5u) == 0) {
            r_ = 44; g_ = 88; b_ = 46;
        }

        // coast: fields -> dune grass -> wet sand -> surf -> open sea,
        // and the river fans out through it as an estuary
        if (sea >= 0) {
            float rwe = rw * (1.0f + sea * 2.2f);   // estuary widening
            bool in_est = rd < rwe;
            if (sea < 0.35f) {
                float t2 = sea / 0.35f;
                if (!in_est) {
                    r_ = (int)(r_ + (206 - r_) * t2);
                    g_ = (int)(g_ + (188 - g_) * t2);
                    b_ = (int)(b_ + (140 - b_) * t2);
                    // dune grass tussocks survive the upper beach
                    if (t2 < 0.7f && (bomber_hash(x >> 1, row >> 1) % 9u) == 0) {
                        r_ -= 60; g_ -= 30; b_ -= 50;
                    }
                }
            } else {
                float t2 = bclampf((sea - 0.35f) / 0.65f, 0.0f, 1.0f);
                if (!in_est) {
                    r_ = (int)(206 + (30 - 206) * t2);
                    g_ = (int)(188 + (76 - 188) * t2);
                    b_ = (int)(140 + (140 - 140) * t2);
                    if (t2 < 0.10f) { r_ -= 34; g_ -= 28; b_ -= 14; }   // wet sand
                    // breaking surf: a bright irregular foam line
                    if (t2 > 0.08f && t2 < 0.30f) {
                        float fo = bomber_noise(x * 0.06f, row * 0.11f);
                        if (fo > 0.42f) { r_ += 90; g_ += 88; b_ += 76; }
                    }
                    if (t2 > 0.3f && (bomber_hash(x / 2, row) % 43u) == 0) {
                        r_ += 60; g_ += 60; b_ += 50;   // whitecaps
                    }
                }
            }
            if (in_est) {
                // river water blending out into the sea
                float k = bclampf(sea, 0.0f, 1.0f);
                int er = (int)(58 + (30 - 58) * k);
                int eg = (int)(96 + (76 - 96) * k);
                int eb = (int)(128 + (140 - 128) * k);
                r_ = er; g_ = eg; b_ = eb;
                if ((bomber_hash(x, row / 2) % 41u) == 0) { r_ += 40; g_ += 40; b_ += 34; }
                // sandbars where the current drops its load
                float sb = bomber_noise(x * 0.03f + 3.0f, row * 0.03f);
                if (sb > 0.74f && k < 0.8f) { r_ = 176; g_ = 160; b_ = 118; }
            }
        }

        out[x] = b565(r_, g_, b_);
    }

    // stamp planned art whose bbox crosses this row
    for (int i = 0; i < G->n_targets; i++) {
        target_t *t = &G->targets[i];
        if (t->kind == TGT_CONVOY) continue;
        if (row >= (int)t->y - 40 && row <= (int)t->y + 40)
            paint_target_row(t, row, out);
    }
    for (int i = 0; i < G->n_flak; i++)
        if (row >= (int)G->flak[i].y - 9 && row <= (int)G->flak[i].y + 9)
            paint_flak_row(&G->flak[i], row, out);
}

void terrain_gen_to(int row)
{
    if (row >= G->gen_row + RING_H)   // never outrun the ring (bail-safe)
        G->gen_row = row - RING_H + 1;
    while (G->gen_row <= row)
        gen_row(G->gen_row++);
}

// ---------------------------------------------------------------------------
// Destructive stamps (into already-generated rows; they scroll away)
// ---------------------------------------------------------------------------

static void dim_px(int wx, int wy, float f)
{
    if (wx < 0 || wx >= STRIP_W) return;
    if (wy < 0 || wy >= G->gen_row || wy <= G->gen_row - RING_H) return;
    uint16_t *p = &G->ring[(wy & RING_MASK) * STRIP_W + wx];
    int r = (*p >> 11) & 31, g = (*p >> 5) & 63, b = *p & 31;
    *p = (uint16_t)((((int)(r * f)) << 11) | (((int)(g * f)) << 5) | (int)(b * f));
}

void terrain_stamp_crater(float wx, float wy)
{
    int cx = (int)wx, cy = (int)wy;
    for (int dy = -12; dy <= 12; dy++) {
        for (int dx = -12; dx <= 12; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d > 12) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= STRIP_W) continue;
            if (y < 0 || y >= G->gen_row || y <= G->gen_row - RING_H) continue;
            uint16_t *p = &G->ring[(y & RING_MASK) * STRIP_W + x];
            if (d < 7) {
                int lum = 30 + (int)(d * 5) + (int)(bomber_hash(x, y) % 9u);
                *p = b565(lum + 8, lum + 4, lum);
            } else if (d < 9 && dy < 0) {
                *p = b565(150, 132, 100);   // thrown-earth rim, lit side
            } else {
                dim_px(x, y, 0.72f);        // scorched fringe
            }
        }
    }
}

void terrain_stamp_dead(const target_t *t)
{
    int R = t->kind == TGT_RADAR ? 26 : 38;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d > R) continue;
            int x = (int)t->x + dx, y = (int)t->y + dy;
            if (x < 0 || x >= STRIP_W) continue;
            if (y < 0 || y >= G->gen_row || y <= G->gen_row - RING_H) continue;
            uint32_t h = bomber_hash(x, y * 5);
            uint16_t *p = &G->ring[(y & RING_MASK) * STRIP_W + x];
            if (d < R * 0.6f) {
                int lum = 28 + (int)(h % 26u);
                *p = b565(lum + 10, lum + 3, lum - 2);   // burnt rubble
            } else {
                dim_px(x, y, 0.55f);
            }
        }
    }
}
