// font565.c — Montserrat text for full-res RGB565 direct-mode games
// (GOS_CAP_FB565). Renders straight from LVGL's fmt_txt font data (A4
// alpha, continuous bit stream, class kerning) without any of LVGL's draw
// pipeline, so games keep the exact type the standalone LVGL builds had.
//
// Host simulators compile this same file against a small struct shim
// (lv_shim.h) plus copies of the font data files — designated initializers
// make the field layout differences irrelevant there.

#include "gos.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef GOS_HOST_SIM
#include "lv_shim.h"
#else
#include "lvgl.h"
#include "src/font/fmt_txt/lv_font_fmt_txt.h"
#endif

extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_18;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t lv_font_montserrat_22;
extern const lv_font_t lv_font_montserrat_28;
extern const lv_font_t lv_font_montserrat_32;

extern uint16_t *gos_gfx_fb565(void);

static const lv_font_t *pick(int px)
{
    if (px >= 32) return &lv_font_montserrat_32;
    if (px >= 28) return &lv_font_montserrat_28;
    if (px >= 22) return &lv_font_montserrat_22;
    if (px >= 20) return &lv_font_montserrat_20;
    if (px >= 18) return &lv_font_montserrat_18;
    return &lv_font_montserrat_14;
}

// minimal UTF-8 decode (the fonts carry the bullet U+2022 and degree sign)
static uint32_t utf8_next(const char *s, int *i)
{
    uint8_t c = (uint8_t)s[*i];
    if (!c) return 0;
    (*i)++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && s[*i]) {
        uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)s[*i] & 0x3F);
        (*i)++;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && s[*i] && s[*i + 1]) {
        uint32_t cp = ((c & 0x0F) << 12) | (((uint8_t)s[*i] & 0x3F) << 6) |
                      ((uint8_t)s[*i + 1] & 0x3F);
        *i += 2;
        return cp;
    }
    return '?';
}

static uint32_t utf8_peek(const char *s, int i)
{
    return utf8_next(s, &i);
}

static uint32_t glyph_id(const lv_font_fmt_txt_dsc_t *d, uint32_t cp)
{
    for (uint32_t c = 0; c < d->cmap_num; c++) {
        const lv_font_fmt_txt_cmap_t *m = &d->cmaps[c];
        if (cp < m->range_start || cp >= m->range_start + m->range_length)
            continue;
        uint32_t rcp = cp - m->range_start;
        if (!m->unicode_list && !m->glyph_id_ofs_list)
            return m->glyph_id_start + rcp;                    // format0 tiny
        if (!m->unicode_list)
            return m->glyph_id_start +
                   ((const uint8_t *)m->glyph_id_ofs_list)[rcp];   // format0 full
        for (uint32_t i = 0; i < m->list_length; i++) {        // sparse
            if (m->unicode_list[i] != rcp) continue;
            if (!m->glyph_id_ofs_list) return m->glyph_id_start + i;
            return m->glyph_id_start + ((const uint16_t *)m->glyph_id_ofs_list)[i];
        }
    }
    return 0;
}

static int kern(const lv_font_fmt_txt_dsc_t *d, uint32_t gl, uint32_t gr)
{
    if (!d->kern_dsc || !gl || !gr) return 0;
    int8_t v = 0;
    if (d->kern_classes) {
        const lv_font_fmt_txt_kern_classes_t *k = d->kern_dsc;
        uint8_t lc = k->left_class_mapping[gl];
        uint8_t rc = k->right_class_mapping[gr];
        if (lc && rc)
            v = k->class_pair_values[(lc - 1) * k->right_class_cnt + (rc - 1)];
    }
    return ((int)v * d->kern_scale) >> 4;   // 4.4 fixed, still in adv units
}

// blend an A4 glyph into the 565 buffer; returns the advance in px
static int draw_letter(const lv_font_t *f, int pen_x, int top_y, uint32_t cp,
                       uint32_t next, uint16_t color)
{
    const lv_font_fmt_txt_dsc_t *d = f->dsc;
    uint32_t gid = glyph_id(d, cp);
    if (!gid) return 0;
    const lv_font_fmt_txt_glyph_dsc_t *g = &d->glyph_dsc[gid];
    int adv = (g->adv_w + kern(d, gid, glyph_id(d, next)) + 8) >> 4;   // 8.4 -> px
    if (g->box_w == 0 || g->box_h == 0) return adv;

    uint16_t *fb = gos_gfx_fb565();
    int gx = pen_x + g->ofs_x;
    int gy = top_y + (f->line_height - f->base_line) - g->ofs_y - g->box_h;
    const uint8_t *bm = &d->glyph_bitmap[g->bitmap_index];
    int cr = (color >> 11) & 31, cg = (color >> 5) & 63, cb = color & 31;

    uint32_t bit = 0;   // continuous 4-bit stream, no row padding
    for (int py = 0; py < g->box_h; py++) {
        for (int px = 0; px < g->box_w; px++, bit += 4) {
            uint8_t byte = bm[bit >> 3];
            uint8_t a = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
            if (!a) continue;
            int x = gx + px, y = gy + py;
            if (x < 0 || x >= GOS_PANEL_W || y < 0 || y >= GOS_PANEL_H) continue;
            uint16_t *p = &fb[y * GOS_PANEL_W + x];
            if (a == 15) {
                *p = color;
                continue;
            }
            uint16_t o = *p;
            int orr = (o >> 11) & 31, og = (o >> 5) & 63, ob = o & 31;
            int al = a * 17;   // 0..255
            *p = (uint16_t)((((orr + (((cr - orr) * al) >> 8)) & 31) << 11) |
                            (((og + (((cg - og) * al) >> 8)) & 63) << 5) |
                            ((ob + (((cb - ob) * al) >> 8)) & 31));
        }
    }
    return adv;
}

void gos_gfx_text565(int px, int x, int y, uint16_t rgb565, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const lv_font_t *f = pick(px);
    int i = 0;
    for (;;) {
        uint32_t cp = utf8_next(buf, &i);
        if (!cp) break;
        x += draw_letter(f, x, y, cp, utf8_peek(buf, i), rgb565);
    }
}

int gos_gfx_text565_w(int px, const char *s)
{
    const lv_font_t *f = pick(px);
    const lv_font_fmt_txt_dsc_t *d = f->dsc;
    int w = 0, i = 0;
    for (;;) {
        uint32_t cp = utf8_next(s, &i);
        if (!cp) break;
        uint32_t gid = glyph_id(d, cp);
        if (!gid) continue;
        w += (d->glyph_dsc[gid].adv_w +
              kern(d, gid, glyph_id(d, utf8_peek(s, i))) + 8) >> 4;
    }
    return w;
}

int gos_gfx_text565_h(int px) { return pick(px)->line_height; }
