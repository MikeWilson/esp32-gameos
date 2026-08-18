// golf_cards.c — Infinite Golf's card and menu screens, 1:1 from the
// standalone: scorecard-chic cream panels with dark green header bands and
// gold rules, the title attract screen with the champion's plaque, the
// full-screen options page, initials entry, the clear-records confirm, the
// player-count picker and the pass-it-on card. All draw-once into CV; the
// canvas holds between redraws exactly like the LVGL canvas did.

#include "golf_int.h"

#define randf golf_randf
void golf_draw_triangle(float x0, float y0, float x1, float y1,
                        float x2, float y2, uint16_t col);
#define draw_triangle golf_draw_triangle

#define COL_CREAM 0xF3ECD8
#define COL_GREEN 0x1E4526
#define COL_META  0x2A3B24

static uint16_t hexc(uint32_t rgb)
{
    return rgb565c((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// centered text in [x0,x1] (the standalone's card_text/card_text_in);
// handles one embedded newline with the original 5 px line space
static void card_text_in(const char *txt, int px, uint32_t rgb, int x0, int x1, int y)
{
    char buf[120];
    snprintf(buf, sizeof buf, "%s", txt);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    int tw = gos_gfx_text565_w(px, buf);
    gos_gfx_text565(px, (x0 + x1) / 2 - tw / 2, y, hexc(rgb), "%s", buf);
    if (nl) {
        int lh = gos_gfx_text565_h(px) + 5;
        tw = gos_gfx_text565_w(px, nl + 1);
        gos_gfx_text565(px, (x0 + x1) / 2 - tw / 2, y + lh, hexc(rgb), "%s", nl + 1);
    }
}

static void card_text(const char *txt, int px, uint32_t rgb, int y)
{
    card_text_in(txt, px, rgb, 36, 332, y);
}

static void card_text_left(const char *txt, int px, uint32_t rgb, int x0, int y)
{
    gos_gfx_text565(px, x0, y, hexc(rgb), "%s", txt);
}

// The scorecard panel: cream card, dark green header band, gold rule,
// paper grain, tiny flag + ball — blended at 60% so the course shows
static void draw_card_panel(int y1)
{
    const int x0 = 34, y0 = 96, x1 = 334, r = 14;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int cx2 = x < x0 + r ? x0 + r - x : (x > x1 - r ? x - (x1 - r) : 0);
            int cy2 = y < y0 + r ? y0 + r - y : (y > y1 - r ? y - (y1 - r) : 0);
            int d2 = cx2 * cx2 + cy2 * cy2;
            if (d2 > r * r) continue;
            int pr, pg, pb;
            bool border = d2 > (r - 2) * (r - 2) ||
                          x < x0 + 2 || x > x1 - 2 || y < y0 + 2 || y > y1 - 2;
            if (border) {
                pr = 190; pg = 178; pb = 146;
            } else if (y < y0 + 54) {
                pr = 24; pg = 60 + (y - y0) / 4; pb = 32;
            } else if (y < y0 + 57) {
                pr = 214; pg = 186; pb = 90;
            } else {
                int gr = (int)(pixel_hash(x, y) % 9u) - 4;
                pr = 243 + gr; pg = 236 + gr; pb = 214 + gr;
            }
            int idx = y * SCREEN_W + x;
            uint16_t o = CV[idx];
            int orr = ((o >> 11) & 0x1F) << 3;
            int ogg = ((o >> 5) & 0x3F) << 2;
            int obb = (o & 0x1F) << 3;
            CV[idx] = pack_rgb((int)(orr + (pr - orr) * 0.60f),
                               (int)(ogg + (pg - ogg) * 0.60f),
                               (int)(obb + (pb - obb) * 0.60f));
        }
    }

    int fx0 = x1 - 44, fy0 = y0 + 11;
    for (int i = 0; i < 30; i++) {
        CV[(fy0 + i) * SCREEN_W + fx0] = rgb565c(235, 235, 238);
        CV[(fy0 + i) * SCREEN_W + fx0 + 1] = rgb565c(150, 152, 158);
    }
    for (int fy = 0; fy < 9; fy++) {
        int fw = 9 - fy;
        for (int fx = 2; fx <= 2 + fw; fx++)
            CV[(fy0 + fy) * SCREEN_W + fx0 + fx] = rgb565c(214, 40, 46);
    }
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx * dx + dy * dy <= 4)
                CV[(fy0 + 32 + dy) * SCREEN_W + fx0 + 1 + dx] = rgb565c(250, 250, 252);
}

// A cream capsule button (verbatim)
static void draw_capsule(int x0, int y0, int x1, int y1)
{
    int r = (y1 - y0) / 2;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int cx2 = x < x0 + r ? x0 + r - x : (x > x1 - r ? x - (x1 - r) : 0);
            int cy2 = y < y0 + r ? y0 + r - y : (y > y1 - r ? y - (y1 - r) : 0);
            int d2 = cx2 * cx2 + cy2 * cy2;
            if (d2 > r * r) continue;
            bool border = d2 > (r - 3) * (r - 3) ||
                          x < x0 + 3 || x > x1 - 3 || y < y0 + 3 || y > y1 - 3;
            int gr = (int)(pixel_hash(x, y) % 9u) - 4;
            CV[y * SCREEN_W + x] = border ? rgb565c(190, 178, 146)
                                          : pack_rgb(243 + gr, 236 + gr, 214 + gr);
        }
    }
}

// Opaque menu panel with header band (options/initials/pass/player-count)
static void draw_menu_panel(void)
{
    const int x0 = 34, y0 = 100, x1 = 334, y1 = 396, r = 14;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int cx2 = x < x0 + r ? x0 + r - x : (x > x1 - r ? x - (x1 - r) : 0);
            int cy2 = y < y0 + r ? y0 + r - y : (y > y1 - r ? y - (y1 - r) : 0);
            int d2 = cx2 * cx2 + cy2 * cy2;
            if (d2 > r * r) continue;
            uint16_t col;
            bool border = d2 > (r - 2) * (r - 2) ||
                          x < x0 + 2 || x > x1 - 2 || y < y0 + 2 || y > y1 - 2;
            if (border) col = rgb565c(190, 178, 146);
            else if (y < y0 + 44) col = pack_rgb(24, 60 + (y - y0) / 4, 32);
            else if (y < y0 + 47) col = rgb565c(214, 186, 90);
            else {
                int gr = (int)(pixel_hash(x, y) % 9u) - 4;
                col = pack_rgb(243 + gr, 236 + gr, 214 + gr);
            }
            CV[y * SCREEN_W + x] = col;
        }
    }
}

// ball sprite for the card screens (shares golf_render's renderer)
static void card_ball(float bx, float by, float r, const uint8_t *tint)
{
    // small self-contained shaded ball (the full AA renderer lives in
    // golf_render.c and reads camera state; cards draw in screen space)
    int ir = (int)r + 1;
    for (int dy = -ir - 2; dy <= ir + 2; dy++)
        for (int dx = -ir - 2; dx <= ir + 2; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            int px = (int)bx + dx + 2, py = (int)by + dy + 2;
            if (d <= r + 1 && px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                CV[py * SCREEN_W + px] = dim565f(CV[py * SCREEN_W + px], 0.62f);
        }
    for (int dy = -ir; dy <= ir; dy++) {
        for (int dx = -ir; dx <= ir; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            float cov = clampf(r - d + 0.5f, 0.0f, 1.0f);
            if (cov <= 0) continue;
            int px = (int)bx + dx, py = (int)by + dy;
            if (px < 0 || px >= SCREEN_W || py < 0 || py >= SCREEN_H) continue;
            float nx = dx / r, ny = dy / r;
            float lam = 0.74f + 0.26f * clampf(-(nx * 0.55f + ny * 0.72f), -1.0f, 1.0f);
            int cr = (int)(tint[0] * lam), cg = (int)(tint[1] * lam), cb = (int)(tint[2] * lam);
            int idx = py * SCREEN_W + px;
            if (cov >= 1.0f) CV[idx] = pack_rgb(cr, cg, cb);
            else {
                uint16_t bg = CV[idx];
                int br = ((bg >> 11) & 0x1F) << 3, bgg = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
                CV[idx] = pack_rgb((int)(br + (cr - br) * cov),
                                   (int)(bgg + (cg - bgg) * cov),
                                   (int)(bb + (cb - bb) * cov));
            }
        }
    }
}

const char *score_name(int strokes, int hole_par)
{
    if (strokes == 1) return "ACE!";
    int diff = strokes - hole_par;
    if (diff <= -2) return "EAGLE!";
    if (diff == -1) return "BIRDIE!";
    if (diff == 0) return "PAR";
    if (diff == 1) return "BOGEY";
    return "DOUBLE+";
}

// ---------------------------------------------------------------------------
// Scorecard (dim last frame once, hold)
// ---------------------------------------------------------------------------

void show_scorecard_card(void)
{
    dim_canvas(0.38f);
    draw_card_panel(G->num_players > 1 ? 352 : 312);

    char buf[120];
    snprintf(buf, sizeof buf, "HOLE %d  \xE2\x80\xA2  PAR %d", G->current_hole + 1, G->par);
    card_text(buf, 18, COL_CREAM, 110);

    if (G->num_players > 1) {
        card_text_in("hole", 18, COL_META, 150, 236, 152);
        card_text_in("total", 18, COL_META, 236, 322, 152);
        int y0 = 184 + (MAX_PLAYERS - G->num_players) * 20;
        for (int i = 0; i < G->num_players; i++) {
            int y = y0 + i * 42;
            card_ball(66.0f, (float)(y + 12), 8.0f, ball_rgb[G->players[i].color]);
            snprintf(buf, sizeof buf, "P%d", i + 1);
            card_text_left(buf, 22, COL_GREEN, 84, y);
            snprintf(buf, sizeof buf, "%d", G->players[i].strokes);
            card_text_in(buf, 22, COL_GREEN, 150, 236, y);
            snprintf(buf, sizeof buf, "%d", G->players[i].round_strokes);
            card_text_in(buf, 22, COL_GREEN, 236, 322, y);
        }
    } else {
        card_text(score_name(G->stroke_count, G->par), 28, COL_GREEN, 158);
        int diff = G->round_strokes - G->round_par;
        char overall[12];
        if (diff == 0) snprintf(overall, sizeof overall, "E");
        else snprintf(overall, sizeof overall, "%+d", diff);
        snprintf(buf, sizeof buf, "%d strokes  \xE2\x80\xA2  %s overall",
                 G->stroke_count, overall);
        card_text(buf, 22, COL_GREEN, 204);
        snprintf(buf, sizeof buf, "Aces %d  \xE2\x80\xA2  Under %d\nPars %d  \xE2\x80\xA2  Over %d",
                 G->stat_aces, G->stat_under, G->stat_pars, G->stat_over);
        card_text(buf, 18, COL_META, 246);
    }
}

// "tap to continue" hangs below the panel once the next hole has rendered
void card_tap_hint(void)
{
    card_text("tap to continue", 18, COL_CREAM, G->num_players > 1 ? 370 : 330);
}

// ---------------------------------------------------------------------------
// Round end / initials / clear confirm
// ---------------------------------------------------------------------------

void show_round_end_card(void)
{
    dim_canvas(0.38f);
    draw_card_panel(G->num_players > 1 ? 352 : 312);

    char buf[120];
    card_text("ROUND COMPLETE", 18, COL_CREAM, 110);

    if (G->num_players > 1) {
        int order[MAX_PLAYERS];
        for (int i = 0; i < G->num_players; i++) order[i] = i;
        for (int i = 1; i < G->num_players; i++) {
            int v = order[i], j = i;
            while (j > 0 &&
                   G->players[order[j - 1]].round_strokes > G->players[v].round_strokes) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = v;
        }
        snprintf(buf, sizeof buf, "PLAYER %d WINS!", order[0] + 1);
        card_text(buf, 28, COL_GREEN, 152);

        static const char *rank[MAX_PLAYERS] = { "1st", "2nd", "3rd", "4th" };
        int y0 = 204 + (MAX_PLAYERS - G->num_players) * 14;
        for (int i = 0; i < G->num_players; i++) {
            int y = y0 + i * 36;
            card_ball(100.0f, (float)(y + 11), 8.0f, ball_rgb[G->players[order[i]].color]);
            snprintf(buf, sizeof buf, "%s    P%d  \xE2\x80\xA2  %d strokes", rank[i],
                     order[i] + 1, G->players[order[i]].round_strokes);
            card_text_left(buf, 20, COL_GREEN, 122, y);
        }
        card_text("tap for the clubhouse", 18, COL_CREAM, 370);
    } else {
        int diff = G->round_strokes - G->round_par;
        char overall[12];
        if (diff == 0) snprintf(overall, sizeof overall, "EVEN");
        else snprintf(overall, sizeof overall, "%+d", diff);
        card_text(overall, 28, COL_GREEN, 158);
        snprintf(buf, sizeof buf, "%d strokes  \xE2\x80\xA2  par %d",
                 G->round_strokes, G->round_par);
        card_text(buf, 20, COL_GREEN, 204);
        snprintf(buf, sizeof buf, "Aces %d  \xE2\x80\xA2  Under %d\nPars %d  \xE2\x80\xA2  Over %d",
                 G->stat_aces, G->stat_under, G->stat_pars, G->stat_over);
        card_text(buf, 18, COL_META, 246);
        card_text("tap to sign your card", 18, COL_CREAM, 330);
    }
}

void draw_initials_screen(void)
{
    draw_menu_panel();
    for (int i = 0; i < 3; i++)
        draw_capsule(INI_COL_X(i) - 30, 208, INI_COL_X(i) + 30, 268);
    draw_capsule(62, 330, 306, 384);

    for (int i = 0; i < 3; i++) {
        float cx = (float)INI_COL_X(i);
        draw_triangle(cx, 168, cx - 15, 191, cx + 15, 191, rgb565c(30, 69, 38));
        draw_triangle(cx, 299, cx - 15, 276, cx + 15, 276, rgb565c(30, 69, 38));
    }

    card_text("SIGN YOUR CARD", 14, COL_CREAM, 114);
    for (int i = 0; i < 3; i++) {
        char s[2] = { G->entry_ini[i], 0 };
        card_text_in(s, 28, COL_GREEN, INI_COL_X(i) - 34, INI_COL_X(i) + 34, 220);
    }
    card_text("SAVE", 20, COL_GREEN, 344);
}

void enter_initials_card(void)
{
    dim_canvas(0.42f);
    draw_initials_screen();
}

void show_clear_confirm_card(void)
{
    dim_canvas(0.42f);
    draw_menu_panel();
    draw_capsule(62, 196, 306, 256);
    draw_capsule(62, CLR_CLEAR_Y0 + 4, 306, CLR_CLEAR_Y1 - 4);

    card_text("CLEAR RECORDS?", 14, COL_CREAM, 114);
    if (G->board_count > 0) {
        char d[8], lb[48];
        if (G->board[0].diff == 0) snprintf(d, sizeof d, "EVEN");
        else snprintf(d, sizeof d, "%+d", G->board[0].diff);
        snprintf(lb, sizeof lb, "%s  %s  leads the board", G->board[0].ini, d);
        card_text(lb, 18, COL_META, 158);
    }
    card_text("KEEP", 20, COL_GREEN, 212);
    card_text("CLEAR", 20, 0x8C1F24, CLR_CLEAR_Y0 + 20);
}

// ---------------------------------------------------------------------------
// Player count / pass card
// ---------------------------------------------------------------------------

void show_player_count_card(void)
{
    dim_canvas(0.42f);
    draw_menu_panel();
    card_text("HOW MANY PLAYERS?", 14, COL_CREAM, 114);
    for (int row = 0; row < 3; row++) {
        int y0 = PC_ROW_Y(row);
        draw_capsule(62, y0, 306, y0 + 56);
        char buf[16];
        snprintf(buf, sizeof buf, "%d PLAYERS", row + 2);
        card_text_in(buf, 20, COL_GREEN, 70, 214, y0 + 17);
        for (int k = 0; k < row + 2; k++)
            card_ball(224.0f + k * 20.0f, (float)(y0 + 28), 7.0f, ball_rgb[k]);
    }
}

void draw_pass_card(int i)
{
    draw_menu_panel();
    card_ball(184.0f, 190.0f, 17.0f, ball_rgb[G->players[i].color]);
    draw_capsule(62, 322, 306, 376);

    card_text("PASS IT ON", 14, COL_CREAM, 114);
    if (G->current_hole == 0)
        card_text("tap the ball to swap colors", 14, 0x6B7A5C, 150);
    char buf[48];
    snprintf(buf, sizeof buf, "PLAYER %d", i + 1);
    card_text(buf, 28, COL_GREEN, 222);
    if (G->players[i].holed)
        snprintf(buf, sizeof buf, "Hole %d  \xE2\x80\xA2  in the cup!", G->current_hole + 1);
    else
        snprintf(buf, sizeof buf, "Hole %d  \xE2\x80\xA2  Strokes %d", G->current_hole + 1,
                 G->stroke_count);
    card_text(buf, 18, COL_META, 262);
    card_text("TAP TO PLAY", 20, COL_GREEN, 336);
}

// ---------------------------------------------------------------------------
// Options page (full-screen; SOUND lives in OS settings — the deviation)
// ---------------------------------------------------------------------------

void draw_options_panel(void)
{
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t col;
            if (y < 62) col = pack_rgb(24, 60 + y / 6, 32);
            else if (y < 66) col = rgb565c(214, 186, 90);
            else {
                int gr = (int)(pixel_hash(x, y) % 9u) - 4;
                col = pack_rgb(243 + gr, 236 + gr, 214 + gr);
            }
            CV[y * SCREEN_W + x] = col;
        }
    }

    draw_capsule(40, OPT_RESUME_Y0, 328, OPT_RESUME_Y1);
    draw_capsule(40, OPT_SWING_Y0, 328, OPT_SWING_Y1);
    draw_capsule(40, OPT_NEWRND_Y0, 328, OPT_NEWRND_Y1);

    card_text("OPTIONS", 20, COL_CREAM, 20);
    card_text("RESUME", 20, COL_GREEN, OPT_RESUME_Y0 + 20);
    card_text(G->swing_mode ? "SWING: ON" : "SWING: OFF", 20, COL_GREEN,
              OPT_SWING_Y0 + 20);
    card_text("NEW ROUND", 20, COL_GREEN, OPT_NEWRND_Y0 + 20);
    if (G->swing_mode && !G->imu_ok)
        card_text("no IMU: drag power stands in", 14, 0x8C1F24, OPT_NEWRND_Y1 + 18);
    card_text("sound: OS settings on the launcher", 14, 0x6B7A5C, 408);
}

// ---------------------------------------------------------------------------
// Title (attract mode) + loading
// ---------------------------------------------------------------------------

void title_frame(void)
{
    // attract drift interpolated between ticks so it glides at 60 fps
    float phase = G->title_phase + G->render_alpha * 0.0045f;
    float s = 0.5f - 0.5f * cosf(phase);
    float tx, ty;
    course_path_point(s, &tx, &ty);
    float half_w = SCREEN_W * 0.5f / G->cam_zoom;
    float half_h = SCREEN_H * 0.5f / G->cam_zoom;
    G->cam_cx = clampf(tx, half_w, WORLD_W - half_w);
    G->cam_cy = clampf(ty, half_h, WORLD_H - half_h);
    G->cam_x = G->cam_cx - half_w;
    G->cam_y = G->cam_cy - half_h;

    blit_view(SCREEN_W);
    float fade = G->title_t < 20 ? (0.15f + 0.40f * (G->title_t / 20.0f)) : 0.55f;
    dim_canvas(fade);

    for (int y = 148; y <= 232; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t col;
            if (y < 150 || y > 230) col = rgb565c(190, 178, 146);
            else if (y < 154 || y > 226) col = rgb565c(214, 186, 90);
            else col = pack_rgb(24, 58 + (y - 154) / 5, 32);
            CV[y * SCREEN_W + x] = col;
        }
    }

    if (G->board_count > 0) {
        const int px0 = 110, px1 = 258, py0 = 232, pflat = 260, ptip = 276;
        float pcx = (px0 + px1) * 0.5f;
        float holdf = 0;
        if (G->title_hold_us > 0)
            holdf = clampf((gos_time_us() - G->title_hold_us) /
                           (float)TITLE_HOLD_US, 0.0f, 1.0f);
        int fill_x = px0 + (int)((px1 - px0) * holdf);
        for (int y = py0; y <= ptip; y++) {
            float half = (px1 - px0) * 0.5f;
            if (y > pflat)
                half *= 1.0f - (float)(y - pflat) / (float)(ptip - pflat);
            int hx0 = (int)(pcx - half), hx1 = (int)(pcx + half);
            if (hx1 - hx0 < 4) continue;
            int bw = y > pflat ? 5 : 2;
            for (int x = hx0; x <= hx1; x++) {
                uint16_t col;
                if (x < hx0 + bw || x > hx1 - bw) col = rgb565c(190, 178, 146);
                else if (x < fill_x) col = rgb565c(176, 146, 64);
                else col = rgb565c(214, 186, 90);
                CV[y * SCREEN_W + x] = col;
            }
        }
    }

    for (int cap = 0; cap < 3; cap++) {
        const int bx0 = cap == 1 ? 192 : cap == 0 ? 30 : 110;
        const int bx1 = cap == 0 ? 176 : cap == 1 ? SCREEN_W - 30 : SCREEN_W - 110;
        const int by0 = cap < 2 ? 288 : 396;
        const int by1 = cap < 2 ? 354 : 434;
        const int br = cap < 2 ? 33 : 19;
        for (int y = by0; y <= by1; y++) {
            for (int x = bx0; x <= bx1; x++) {
                int cx2 = x < bx0 + br ? bx0 + br - x : (x > bx1 - br ? x - (bx1 - br) : 0);
                int cy2 = y < by0 + br ? by0 + br - y : (y > by1 - br ? y - (by1 - br) : 0);
                int d2 = cx2 * cx2 + cy2 * cy2;
                if (d2 > br * br) continue;
                bool border = d2 > (br - 3) * (br - 3) ||
                              x < bx0 + 3 || x > bx1 - 3 || y < by0 + 3 || y > by1 - 3;
                int gr = (int)(pixel_hash(x, y) % 9u) - 4;
                CV[y * SCREEN_W + x] = border ? rgb565c(190, 178, 146)
                                              : pack_rgb(243 + gr, 236 + gr, 214 + gr);
            }
        }
    }

    card_text("INFINITE GOLF", 32, COL_CREAM, 170);
    card_text_in("SOLO", 20, COL_GREEN, 30, 176, 309);
    card_text_in("PARTY", 20, COL_GREEN, 192, SCREEN_W - 30, 309);
    card_text("OPTIONS", 14, COL_GREEN, 408);

    if (G->board_count > 0) {
        char d[8], lb[52];
        if (G->board[0].diff == 0) snprintf(d, sizeof d, "EVEN");
        else snprintf(d, sizeof d, "%+d", G->board[0].diff);
        snprintf(lb, sizeof lb, "%s   %s", G->board[0].ini, d);
        card_text(lb, 20, COL_GREEN, 239);
    }

    // OS-convention escape chip, top-left (the shell's swipe-down works too)
    gos_gfx_text565(14, 16, 14, rgb565c(243, 236, 216), "< exit");
}

void draw_loading(void)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) CV[i] = 0;
    card_text("INFINITE GOLF", 28, 0xFFFFFF, 200);
    card_text("mowing the course...", 14, 0x9AA88E, 250);
}
