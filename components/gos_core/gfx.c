// gfx.c — immediate-mode renderer into a 184x224 indexed-8 framebuffer.
// Double-buffered in internal SRAM; present() hands the finished buffer to
// the display HAL and swaps. Pure C except for present(); everything above
// the present call compiles on a host for the simulator workflow.

#include "gos.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>

#ifndef GOS_HOST_SIM
#include "gos_hal.h"
#include "esp_heap_caps.h"
#endif

#define W GOS_SCREEN_W
#define H GOS_SCREEN_H

static uint8_t fb[2][W * H];
static int draw_idx;
static uint8_t *dst = fb[0];
static gos_rect_t clip = { 0, 0, W, H };

static uint16_t lut[256];        // panel byte order
static uint16_t lut_dim[256];
static bool scanlines_on;

extern const uint8_t gos_font5x7[95][5];

static int16_t sin_tab[256];

// ---------------------------------------------------------------------------

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static uint16_t dim565(uint16_t c)
{
    // 25% darker, operating on normal-order RGB565
    uint16_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (uint16_t)(((r * 3 / 4) << 11) | ((g * 3 / 4) << 5) | (b * 3 / 4));
}

void gos_gfx_set_palette(const uint16_t *rgb565, int count, int offset)
{
    for (int i = 0; i < count; i++) {
        int d = offset + i;
        if (d < 0 || d > 255) continue;
        lut[d] = bswap16(rgb565[i]);
        lut_dim[d] = bswap16(dim565(rgb565[i]));
    }
#ifdef GOS_HOST_SIM
    extern void sim_note_palette(const uint16_t *rgb565, int count, int offset);
    sim_note_palette(rgb565, count, offset);
#endif
}

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

void gos_gfx_set_default_palette(void)
{
    static const uint16_t named[16] = {
        C565(0, 0, 0),       C565(60, 60, 68),    C565(128, 128, 136),
        C565(190, 190, 196), C565(255, 255, 255), C565(230, 50, 40),
        C565(240, 140, 30),  C565(245, 215, 60),  C565(70, 200, 90),
        C565(70, 200, 220),  C565(70, 110, 240),  C565(170, 90, 220),
        C565(20, 100, 50),   C565(20, 40, 90),    C565(140, 90, 50),
        C565(220, 170, 40),
    };
    gos_gfx_set_palette(named, 16, 0);
    uint16_t ramp[64];
    for (int i = 0; i < 64; i++) {
        int v = i * 255 / 63;
        ramp[i] = C565(v, v, v);
    }
    gos_gfx_set_palette(ramp, 64, 16);
    // fill the rest with magenta so a bad index is visible, not subtle
    uint16_t mag = C565(255, 0, 255);
    for (int i = 80; i < 256; i++) gos_gfx_set_palette(&mag, 1, i);
}

void gos_gfx_scanlines(bool on) { scanlines_on = on; }
uint8_t *gos_gfx_fb(void) { return dst; }

// --- Full-resolution RGB565 direct mode (GOS_CAP_FB565 games) -------------
// One 368x448 RGB565 buffer in PSRAM, presented as-is (no LUT, no upscale).
// Single-buffered like the standalone LVGL-canvas games this exists for.
static uint16_t *fb565;
static bool direct565;

uint16_t *gos_gfx_fb565(void)
{
#ifndef GOS_HOST_SIM
    if (!fb565)
        fb565 = heap_caps_calloc(GOS_PANEL_W * GOS_PANEL_H, 2, MALLOC_CAP_8BIT);
#else
    if (!fb565) fb565 = calloc(GOS_PANEL_W * GOS_PANEL_H, 2);
#endif
    return fb565;
}

void gos_gfx_direct565(bool on) { direct565 = on && gos_gfx_fb565(); }
bool gos_gfx_direct565_on(void) { return direct565; }

void gos_gfx_init(void)
{
    for (int i = 0; i < 256; i++)
        sin_tab[i] = (int16_t)lrintf(sinf(i * (float)M_PI / 128.f) * 16384.f);
    gos_gfx_set_default_palette();
    memset(fb, 0, sizeof fb);
}

int16_t gos_sin256(uint8_t a) { return sin_tab[a]; }
int16_t gos_cos256(uint8_t a) { return sin_tab[(uint8_t)(a + 64)]; }

#ifndef GOS_HOST_SIM
void gos_gfx_present(void)
{
    if (direct565) {
        hal_display_present_565(fb565);
        return;
    }
    hal_display_present(dst, lut, lut_dim, scanlines_on);
    draw_idx ^= 1;
    dst = fb[draw_idx];
}
#endif

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void gos_gfx_set_clip(gos_rect_t r)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > W ? W : r.x + r.w;
    int y1 = r.y + r.h > H ? H : r.y + r.h;
    clip = (gos_rect_t){ (int16_t)x0, (int16_t)y0,
                         (int16_t)(x1 > x0 ? x1 - x0 : 0),
                         (int16_t)(y1 > y0 ? y1 - y0 : 0) };
}

void gos_gfx_clear_clip(void) { clip = (gos_rect_t){ 0, 0, W, H }; }

void gos_gfx_clear(gos_color_t c) { memset(dst, c, W * H); }

void gos_gfx_pixel(int x, int y, gos_color_t c)
{
    if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) return;
    dst[y * W + x] = c;
}

void gos_gfx_hline(int x0, int x1, int y, gos_color_t c)
{
    if (y < clip.y || y >= clip.y + clip.h) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < clip.x) x0 = clip.x;
    if (x1 >= clip.x + clip.w) x1 = clip.x + clip.w - 1;
    if (x0 > x1) return;
    memset(dst + y * W + x0, c, (size_t)(x1 - x0 + 1));
}

void gos_gfx_vline(int x, int y0, int y1, gos_color_t c)
{
    if (x < clip.x || x >= clip.x + clip.w) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < clip.y) y0 = clip.y;
    if (y1 >= clip.y + clip.h) y1 = clip.y + clip.h - 1;
    uint8_t *p = dst + y0 * W + x;
    for (int y = y0; y <= y1; y++, p += W) *p = c;
}

void gos_gfx_line(int x0, int y0, int x1, int y1, gos_color_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gos_gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void gos_gfx_rect(gos_rect_t r, gos_color_t c)
{
    gos_gfx_hline(r.x, r.x + r.w - 1, r.y, c);
    gos_gfx_hline(r.x, r.x + r.w - 1, r.y + r.h - 1, c);
    gos_gfx_vline(r.x, r.y, r.y + r.h - 1, c);
    gos_gfx_vline(r.x + r.w - 1, r.y, r.y + r.h - 1, c);
}

void gos_gfx_rect_fill(gos_rect_t r, gos_color_t c)
{
    for (int y = r.y; y < r.y + r.h; y++)
        gos_gfx_hline(r.x, r.x + r.w - 1, y, c);
}

void gos_gfx_circle(int cx, int cy, int r, gos_color_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gos_gfx_pixel(cx + x, cy + y, c); gos_gfx_pixel(cx - x, cy + y, c);
        gos_gfx_pixel(cx + x, cy - y, c); gos_gfx_pixel(cx - x, cy - y, c);
        gos_gfx_pixel(cx + y, cy + x, c); gos_gfx_pixel(cx - y, cy + x, c);
        gos_gfx_pixel(cx + y, cy - x, c); gos_gfx_pixel(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gos_gfx_circle_fill(int cx, int cy, int r, gos_color_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int w2 = r * r - dy * dy;
        int hw = 0;
        while ((hw + 1) * (hw + 1) <= w2) hw++;
        gos_gfx_hline(cx - hw, cx + hw, cy + dy, c);
    }
}

// integer sqrt for d2 <= 256 + a bit (soft blobs are radius <= 16)
static uint8_t isqrt_small(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return (uint8_t)r;
}

void gos_gfx_circle_soft(int cx, int cy, int r, uint8_t hot, uint8_t base)
{
    if (r < 1) r = 1;
    if (r > 16) r = 16;
    int span = hot - base;
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < clip.y || y >= clip.y + clip.h) continue;
        uint8_t *row = dst + y * W;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < clip.x || x >= clip.x + clip.w) continue;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            int level = hot - span * isqrt_small(d2) / r;
            if (level > row[x]) row[x] = (uint8_t)level;
        }
    }
}

void gos_gfx_quad_fill(const gos_pt_t p[4], gos_color_t c)
{
    int ymin = p[0].y, ymax = p[0].y;
    for (int i = 1; i < 4; i++) {
        if (p[i].y < ymin) ymin = p[i].y;
        if (p[i].y > ymax) ymax = p[i].y;
    }
    if (ymin < clip.y) ymin = clip.y;
    if (ymax >= clip.y + clip.h) ymax = clip.y + clip.h - 1;
    for (int y = ymin; y <= ymax; y++) {
        int xl = 32767, xr = -32768;
        for (int i = 0; i < 4; i++) {
            gos_pt_t a = p[i], b = p[(i + 1) & 3];
            if (a.y == b.y) {
                if (y == a.y) {
                    if (a.x < xl) xl = a.x;
                    if (b.x < xl) xl = b.x;
                    if (a.x > xr) xr = a.x;
                    if (b.x > xr) xr = b.x;
                }
                continue;
            }
            int y0 = a.y, y1 = b.y;
            if (y0 > y1) { gos_pt_t t = a; a = b; b = t; y0 = a.y; y1 = b.y; }
            if (y < y0 || y > y1) continue;
            int x = a.x + (int)(b.x - a.x) * (y - y0) / (y1 - y0);
            if (x < xl) xl = x;
            if (x > xr) xr = x;
        }
        if (xl <= xr) gos_gfx_hline(xl, xr, y, c);
    }
}

void gos_gfx_blit(const gos_sprite_t *s, int x, int y)
{
    for (int sy = 0; sy < s->h; sy++) {
        int dy = y + sy;
        if (dy < clip.y || dy >= clip.y + clip.h) continue;
        const uint8_t *src = s->px + sy * s->w;
        uint8_t *row = dst + dy * W;
        for (int sx = 0; sx < s->w; sx++) {
            uint8_t v = src[sx];
            if (v == 0xFF) continue;
            int dx = x + sx;
            if (dx < clip.x || dx >= clip.x + clip.w) continue;
            row[dx] = v;
        }
    }
}

void gos_gfx_blit_rot(const gos_sprite_t *s, int x, int y, uint8_t angle256)
{
    // inverse-map over the dest bounding box; Q14 rotation, no trig
    int32_t c = gos_cos256(angle256), n = gos_sin256(angle256);
    int hw = s->w / 2, hh = s->h / 2;
    int rad = (hw > hh ? hw : hh) * 3 / 2 + 1;
    for (int dy = -rad; dy <= rad; dy++) {
        int py = y + dy;
        if (py < clip.y || py >= clip.y + clip.h) continue;
        uint8_t *row = dst + py * W;
        for (int dx = -rad; dx <= rad; dx++) {
            int px = x + dx;
            if (px < clip.x || px >= clip.x + clip.w) continue;
            int sx = (int)((c * dx + n * dy) >> 14) + hw;
            int sy = (int)((-n * dx + c * dy) >> 14) + hh;
            if (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) continue;
            uint8_t v = s->px[sy * s->w + sx];
            if (v != 0xFF) row[px] = v;
        }
    }
}

// ---------------------------------------------------------------------------
// Text — 5x7 bitmap font, column-major bytes, LSB = top row.
// ---------------------------------------------------------------------------

static void draw_glyph(int x, int y, char ch, gos_color_t c, int scale)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = gos_font5x7[ch - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row))) continue;
            if (scale == 1) gos_gfx_pixel(x + col, y + row, c);
            else gos_gfx_rect_fill((gos_rect_t){ (int16_t)(x + col * 2),
                                                 (int16_t)(y + row * 2), 2, 2 }, c);
        }
    }
}

void gos_gfx_text(int font, int x, int y, gos_color_t c, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int scale = font ? 2 : 1;
    int adv = 6 * scale;
    for (const char *s = buf; *s; s++) {
        draw_glyph(x, y, *s, c, scale);
        x += adv;
    }
}

int gos_gfx_text_w(int font, const char *s)
{
    int n = (int)strlen(s);
    return n ? n * 6 * (font ? 2 : 1) - (font ? 2 : 1) : 0;
}
