// bomber_cards.c — briefing-room screens: manila folder cards over a dimmed
// world, stamped headers, typewriter-adjacent layout. Draw-once and hold
// (single-buffered FB565 canvas), tap/A dismissal handled in bomber.c.

#include "bomber_int.h"
#include <stdarg.h>

#define COL_PAPER  0xD9CDA8
#define COL_INK    0x2A2A22
#define COL_STAMP  0xA03028
#define COL_FADED  0x6E685A
#define COL_CREAM  0xEFE8D2

static uint16_t hexc(uint32_t rgb)
{
    return b565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static void dimf(float f)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t c = CV[i];
        int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        CV[i] = (uint16_t)((((int)(r * f)) << 11) | (((int)(g * f)) << 5) |
                           (int)(b * f));
    }
}

static void fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) CV[y * SCREEN_W + x] = c;
}

// manila folder panel with grain, a header band, and a red file-stamp box
static void folder_panel(const char *header, const char *stamp)
{
    const int x0 = 30, y0 = 90, x1 = 338, y1 = 400;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            int gr = (int)((h >> 16) % 11u) - 5;
            uint16_t c;
            bool border = x < x0 + 3 || x > x1 - 3 || y < y0 + 3 || y > y1 - 3;
            if (border) c = b565(120, 106, 76);
            else if (y < y0 + 42) c = b565(102 + gr, 92 + gr, 66 + gr / 2);
            else c = b565(217 + gr, 205 + gr, 168 + gr);
            CV[y * SCREEN_W + x] = c;
        }
    }
    gos_gfx_text565(22, x0 + 16, y0 + 10, hexc(COL_CREAM), "%s", header);
    if (stamp) {
        int sw = gos_gfx_text565_w(18, stamp) + 16;
        int sx = x1 - sw - 14, sy = y0 + 10;
        for (int x = sx; x < sx + sw; x++) {
            CV[sy * SCREEN_W + x] = hexc(COL_STAMP);
            CV[(sy + 24) * SCREEN_W + x] = hexc(COL_STAMP);
        }
        for (int y = sy; y <= sy + 24; y++) {
            CV[y * SCREEN_W + sx] = hexc(COL_STAMP);
            CV[y * SCREEN_W + sx + sw - 1] = hexc(COL_STAMP);
        }
        gos_gfx_text565(18, sx + 8, sy + 3, hexc(COL_STAMP), "%s", stamp);
    }
}

static void center(int px, uint32_t rgb, int y, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int tw = gos_gfx_text565_w(px, buf);
    gos_gfx_text565(px, (SCREEN_W - tw) / 2, y, hexc(rgb), "%s", buf);
}

const char *target_name(int kind)
{
    switch (kind) {
    case TGT_DEPOT: return "supply depot";
    case TGT_BRIDGE: return "bridge";
    case TGT_RADAR: return "radar station";
    default: return "convoy";
    }
}

// ---------------------------------------------------------------------------

// title needs the scrolling world; reuse the cruise blit, banner over it
static void title_backdrop(void);

void render_title(void)
{
    title_backdrop();
}

static void title_backdrop(void)
{
    // world (borrow the cruise renderer's core by calling it, then paint
    // over its HUD region with the banner — cheaper than a special path)
    render_cruise();

    // dim for legibility, banner band, capsule hints
    for (int y = 120; y <= 214; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t c = CV[y * SCREEN_W + x];
            if (y < 124 || y > 210) c = b565(120, 106, 76);
            else {
                int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                c = (uint16_t)(((r / 4) << 11) | ((g / 4) << 5) | (b / 4));
            }
            CV[y * SCREEN_W + x] = c;
        }
    center(32, 0xEFE8D2, 138, "BOMBER");
    center(14, 0xC9BC96, 180, "tilt to steer   \xE2\x80\xA2   click or tap to bomb");

    if (G->board_count > 0)
        center(18, 0xEFE8D2, 240, "record  %s  %u", G->board[0].ini,
               (unsigned)G->board[0].score);

    center(20, 0xEFE8D2, 300, "PRESS THE BUTTON");
    center(14, 0x9C9480, 330, "or tap to take off");
    center(14, 0x9C9480, SCREEN_H - 34, "swipe down from top = menu");
}

// --- briefing-room furniture ----------------------------------------------

// sheared red rubber stamp: slightly skewed box, inked unevenly
static void stamp(int x, int y, const char *txt)
{
    int sw = gos_gfx_text565_w(14, txt) + 18;
    for (int yy = 0; yy < 28; yy++) {
        int sh = yy / 9;   // shear: the stamp landed crooked
        for (int xx = 0; xx < sw; xx++) {
            bool edge = yy < 2 || yy > 25 || xx < 2 || xx > sw - 3;
            if (!edge) continue;
            uint32_t h = (uint32_t)(xx * 31 + yy * 77);
            if ((h % 5u) == 0) continue;   // patchy ink
            int px = x + xx + sh, py = y + yy;
            if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                CV[py * SCREEN_W + px] = hexc(COL_STAMP);
        }
    }
    gos_gfx_text565(14, x + 9 + 1, y + 6, hexc(COL_STAMP), "%s", txt);
}

// a coffee ring soaked into the paper
static void coffee_ring(int cx, int cy)
{
    for (int a = 0; a < 220; a++) {
        float th = a * 6.28f / 220;
        uint32_t h = (uint32_t)(a * 2654435761u);
        if ((h % 7u) == 0) continue;      // broken ring
        float rr = 26 + ((h >> 8) % 5u) * 0.5f;
        int px = cx + (int)(cosf(th) * rr), py = cy + (int)(sinf(th) * rr * 0.8f);
        if (px < 34 || px >= 334 || py < 134 || py >= 396) continue;
        uint16_t *p = &CV[py * SCREEN_W + px];
        int r = (*p >> 11) & 31, g = (*p >> 5) & 63, b = *p & 31;
        *p = (uint16_t)((((r * 3 / 4) + 2) << 11) | (((g * 5 / 8)) << 5) | (b / 2));
    }
}

static void paperclip(int x, int y)
{
    for (int i = 0; i < 26; i++) {
        uint16_t c = b565(168, 170, 176);
        CV[(y + i) * SCREEN_W + x] = c;
        CV[(y + i) * SCREEN_W + x + 7] = c;
        if (i < 18) CV[(y + i) * SCREEN_W + x + 3] = c;
        if (i == 0 || i == 25)
            for (int k = 0; k <= 7; k++) CV[(y + i) * SCREEN_W + x + k] = c;
        if (i == 18) for (int k = 3; k <= 7; k++) CV[(y + i) * SCREEN_W + x + k] = c;
    }
}

// operation codename, dealt from the sortie seed
static const char *op_name(char *buf, size_t n)
{
    static const char *adj[] = { "IRON", "VELVET", "MARBLE", "TIGER", "WINTER",
                                 "CROWN", "GRANITE", "SILVER", "THUNDER",
                                 "QUIET", "COPPER", "FOXGLOVE" };
    static const char *noun[] = { "ANVIL", "GARDEN", "HARVEST", "LANTERN",
                                  "SICKLE", "CHARIOT", "RAMPART", "KETTLE",
                                  "ARROW", "SADDLE", "DRUM", "WICKET" };
    snprintf(buf, n, "%s %s", adj[(G->sortie_seed >> 4) % 12u],
             noun[(G->sortie_seed >> 11) % 12u]);
    return buf;
}

// The navigator's chart: the WHOLE sortie inked onto the folder — coasts at
// both ends, the river in blue, the lane in brown, targets as symbols with
// flak hatching. With results, strikes are marked in red grease pencil.
static void draw_chart(int x0, int y0, int w, int h, bool results)
{
    // chart paper, slightly whiter than the folder, with a thin frame
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            uint32_t hh = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
            int gr = (int)((hh >> 16) % 7u) - 3;
            bool fr = x == x0 || x == x0 + w - 1 || y == y0 || y == y0 + h - 1;
            CV[y * SCREEN_W + x] = fr ? b565(96, 88, 66)
                                      : b565(232 + gr, 224 + gr, 196 + gr);
        }
    // faint graticule
    for (int gy = y0 + 12; gy < y0 + h - 4; gy += 24)
        for (int x = x0 + 3; x < x0 + w - 3; x += 2)
            CV[gy * SCREEN_W + x] = b565(200, 192, 168);

    // row r maps to chart y (flight goes UP the chart)
    #define CHY(r) (y0 + h - 3 - (int)((float)(r) * (h - 6) / SORTIE_ROWS))
    #define CHX(wx) (x0 + 2 + (int)((float)(wx) * (w - 4) / STRIP_W))

    // sea bands at both ends
    for (int y = CHY(120); y < y0 + h - 1; y++)
        for (int x = x0 + 1; x < x0 + w - 1; x++)
            if (((x + y) & 3) < 2) CV[y * SCREEN_W + x] = b565(120, 150, 190);
    for (int y = y0 + 1; y < CHY(SORTIE_ROWS - COAST_ROWS); y++)
        for (int x = x0 + 1; x < x0 + w - 1; x++)
            if (((x + y) & 3) < 2) CV[y * SCREEN_W + x] = b565(120, 150, 190);

    // river in blue ink, lane in brown, sampled up the strip
    for (int r = 60; r < SORTIE_ROWS - 60; r += 12) {
        int y = CHY(r);
        int xr = CHX(river_cx((float)r));
        int xd = CHX(road_cx((float)r));
        if (xr > x0 + 1 && xr < x0 + w - 2) {
            CV[y * SCREEN_W + xr] = b565(50, 90, 170);
            CV[y * SCREEN_W + xr + 1] = b565(50, 90, 170);
        }
        if ((r / 12) % 2 == 0 && xd > x0 + 1 && xd < x0 + w - 2)
            CV[y * SCREEN_W + xd] = b565(140, 104, 60);
    }

    // targets: symbol per kind, red hatched ring when defended
    for (int i = 0; i < G->n_targets; i++) {
        target_t *t = &G->targets[i];
        int cx = CHX(t->kind == TGT_CONVOY ? road_cx(t->y) : t->x);
        int cy = CHY(t->y);
        uint16_t ink = b565(40, 40, 34);
        if (t->defended)   // AA hatching
            for (int a = 0; a < 12; a++) {
                float th = a * 6.28f / 12;
                int px = cx + (int)(cosf(th) * 8), py = cy + (int)(sinf(th) * 8);
                if (px > x0 && px < x0 + w - 1 && py > y0 && py < y0 + h - 1)
                    CV[py * SCREEN_W + px] = hexc(COL_STAMP);
            }
        for (int k = -3; k <= 3; k++) {   // the symbol: a small square + tick
            if (cx + k > x0 && cx + k < x0 + w - 1) {
                CV[(cy - 3) * SCREEN_W + cx + k] = ink;
                CV[(cy + 3) * SCREEN_W + cx + k] = ink;
            }
            if (cy + k > y0 && cy + k < y0 + h - 1) {
                CV[(cy + k) * SCREEN_W + cx - 3] = ink;
                CV[(cy + k) * SCREEN_W + cx + 3] = ink;
            }
        }
        if (results && t->state == TS_DEAD) {
            // grease-pencil strike: a fat red X through it
            for (int k = -6; k <= 6; k++) {
                for (int wdt = 0; wdt < 2; wdt++) {
                    int px1 = cx + k, py1 = cy + k + wdt, py2 = cy - k + wdt;
                    if (px1 > x0 && px1 < x0 + w - 1) {
                        if (py1 > y0 && py1 < y0 + h - 1)
                            CV[py1 * SCREEN_W + px1] = hexc(COL_STAMP);
                        if (py2 > y0 && py2 < y0 + h - 1)
                            CV[py2 * SCREEN_W + px1] = hexc(COL_STAMP);
                    }
                }
            }
        }
    }
    // ingress arrow at the bottom edge
    int ax = x0 + w / 2;
    for (int k = 0; k < 8; k++) {
        CV[(y0 + h - 4 - k) * SCREEN_W + ax] = hexc(COL_STAMP);
        if (k > 4) {
            CV[(y0 + h - 4 - k) * SCREEN_W + ax - (8 - k)] = hexc(COL_STAMP);
            CV[(y0 + h - 4 - k) * SCREEN_W + ax + (8 - k)] = hexc(COL_STAMP);
        }
    }
    #undef CHY
    #undef CHX
}

void draw_brief_card(void)
{
    dimf(0.35f);
    folder_panel("SORTIE ORDERS", NULL);
    paperclip(38, 84);

    char code[32];
    gos_gfx_text565(14, 52, 142, hexc(COL_FADED), "operation");
    gos_gfx_text565(18, 52, 160, hexc(COL_INK), "%s", op_name(code, sizeof code));

    // the navigator's chart, right side (the stamp lands ON it — clerks
    // don't aim)
    draw_chart(236, 146, 88, 236, false);
    stamp(216, 158, "MOST SECRET");
    gos_gfx_text565(14, 240, 384, hexc(COL_FADED), "route chart");

    int defended = 0;
    for (int i = 0; i < G->n_targets; i++) defended += G->targets[i].defended;
    float wm = sqrtf(G->wind_base_x * G->wind_base_x +
                     G->wind_base_y * G->wind_base_y);
    int y = 206;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "sortie");
    gos_gfx_text565(18, 130, y - 2, hexc(COL_INK), "%d", G->sortie_num);
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "targets");
    gos_gfx_text565(18, 130, y - 2, hexc(COL_INK), "%d", G->n_targets);
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "flak");
    gos_gfx_text565(18, 130, y - 2, hexc(COL_STAMP), "%d", defended);
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "wind");
    gos_gfx_text565(18, 130, y - 2, hexc(COL_INK), "%d", (int)(wm * 0.6f + 0.5f));
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "bombs");
    gos_gfx_text565(18, 130, y - 2, hexc(COL_INK), "%d", MAX_BOMBS);

    gos_gfx_text565(14, 52, 342, hexc(COL_FADED), "hold the run steady.");
    gos_gfx_text565(14, 52, 360, hexc(COL_FADED), "linger and the flak finds you.");
    coffee_ring(96, 316);
    center(18, 0xEFE8D2, 412, "button or tap to take off");
}

void draw_debrief_card(void)
{
    dimf(0.35f);
    folder_panel("STRIKE ASSESSMENT", NULL);

    // graded verdict, stamped crooked like it was slammed down
    int acc = G->sortie_drops ? (100 * G->sortie_hits) / G->sortie_drops : 0;
    const char *verdict =
        G->sortie_kills >= G->n_targets ? "DEVASTATING" :
        G->sortie_kills * 2 >= G->n_targets ? "EFFECTIVE" :
        G->sortie_kills > 0 ? "MARGINAL" : "SCRUBBED";
    paperclip(38, 84);

    // the same chart, now marked up with strike results
    draw_chart(236, 146, 88, 236, true);
    stamp(206, 158, verdict);
    gos_gfx_text565(14, 240, 384, hexc(COL_FADED), "strike photo");

    char code[32];
    gos_gfx_text565(14, 52, 142, hexc(COL_FADED), "operation");
    gos_gfx_text565(18, 52, 160, hexc(COL_INK), "%s", op_name(code, sizeof code));

    int y = 206;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "destroyed");
    gos_gfx_text565(18, 140, y - 2, hexc(COL_INK), "%d/%d", G->sortie_kills,
                    G->n_targets);
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "accuracy");
    gos_gfx_text565(18, 140, y - 2, hexc(COL_INK), "%d%%", acc);
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "damage");
    gos_gfx_text565(18, 140, y - 2,
                    G->damage > 60 ? hexc(COL_STAMP) : hexc(COL_INK),
                    "%d%%", (int)G->damage);
    y += 28;
    gos_gfx_text565(14, 52, y, hexc(COL_FADED), "score");
    gos_gfx_text565(18, 140, y - 2, hexc(COL_INK), "%d", G->score);

    // the bomb tally: pips for every bomb, red-crossed where they hit
    for (int i = 0; i < MAX_BOMBS; i++) {
        int bx = 56 + i * 22, by = 330;
        uint16_t c = i < G->sortie_drops ? hexc(COL_INK) : b565(178, 168, 140);
        for (int yy = -5; yy <= 5; yy++)
            for (int xx = -2; xx <= 2; xx++)
                if (bx + xx >= 0 && bx + xx < SCREEN_W)
                    CV[(by + yy) * SCREEN_W + bx + xx] = c;
        if (i < G->sortie_hits)
            for (int k = -4; k <= 4; k++) {
                CV[(by + k) * SCREEN_W + bx + k] = hexc(COL_STAMP);
                CV[(by - k) * SCREEN_W + bx + k] = hexc(COL_STAMP);
            }
    }
    gos_gfx_text565(14, 52, 344, hexc(COL_FADED), "bombs away / on target");
    coffee_ring(288, 128);
    center(18, 0xEFE8D2, 412, "button or tap for the next sortie");
}

void draw_run_end_card(void)
{
    dimf(0.30f);
    folder_panel("AFTER ACTION", G->end_reason);
    int y = 156;
    center(28, COL_INK, y, "%d", G->score);
    y += 46;
    center(18, COL_FADED, y, "final score");
    y += 44;
    gos_gfx_text565(18, 56, y, hexc(COL_INK), "sorties flown:  %d", G->sortie_num);
    y += 30;
    gos_gfx_text565(18, 56, y, hexc(COL_INK), "targets destroyed:  %d",
                    G->run_targets);
    center(18, 0xEFE8D2, 412, "tap to sign the record");
}

void draw_initials_card(void)
{
    dimf(0.30f);
    folder_panel("SERVICE RECORD", NULL);
    center(14, COL_FADED, 142, "sign here, pilot");
    for (int i = 0; i < 3; i++) {
        int cx = 114 + i * 70;
        fill(cx - 30, 208, cx + 30, 268, hexc(COL_CREAM));
        for (int x = cx - 30; x <= cx + 30; x++) {
            CV[208 * SCREEN_W + x] = b565(120, 106, 76);
            CV[268 * SCREEN_W + x] = b565(120, 106, 76);
        }
        for (int y = 208; y <= 268; y++) {
            CV[y * SCREEN_W + cx - 30] = b565(120, 106, 76);
            CV[y * SCREEN_W + cx + 30] = b565(120, 106, 76);
        }
        char s[2] = { G->entry_ini[i], 0 };
        int tw = gos_gfx_text565_w(28, s);
        gos_gfx_text565(28, cx - tw / 2, 220, hexc(COL_INK), "%s", s);
        // up/down chevrons
        for (int k = 0; k < 10; k++) {
            CV[(190 - k / 2) * SCREEN_W + cx - k + 5] = hexc(COL_INK);
            CV[(190 - k / 2) * SCREEN_W + cx + k - 5] = hexc(COL_INK);
            CV[(286 + k / 2) * SCREEN_W + cx - k + 5] = hexc(COL_INK);
            CV[(286 + k / 2) * SCREEN_W + cx + k - 5] = hexc(COL_INK);
        }
    }
    fill(62, 334, 306, 384, b565(102, 92, 66));
    center(20, 0xEFE8D2, 348, "FILE THE RECORD");
}
