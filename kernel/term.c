/* A text terminal rendered with the 8x16 font onto a pixel surface.
 * Used for terminal windows and for the full-screen graphics console. */
#include "kernel.h"

static const u32 palette[16] = {
    0x101418, 0x3465A4, 0x4E9A06, 0x06989A, 0xCC0000, 0x75507B, 0xC4A000, 0xD3D7CF,
    0x555753, 0x729FCF, 0x8AE234, 0x34E2E2, 0xEF2929, 0xAD7FA8, 0xFCE94F, 0xFFFFFF,
};

static void cursor(struct term *t, int on)
{
    gfx_fill(t->surf, t->col * VX_FONT_W, t->row * VX_FONT_H + VX_FONT_H - 2,
             VX_FONT_W, 2, on ? t->fg : t->bg);
}

void term_color(struct term *t, u8 fg, u8 bg)
{
    t->fg = palette[fg & 15];
    t->bg = palette[bg & 15];
}

void term_clear(struct term *t)
{
    gfx_fill(t->surf, 0, 0, t->surf->w, t->surf->h, t->bg);
    t->col = t->row = 0;
    cursor(t, 1);
    wm_mark_dirty();
}

void term_init(struct term *t, struct surface *surf)
{
    t->surf = surf;
    t->cols = surf->w / VX_FONT_W;
    t->rows = surf->h / VX_FONT_H;
    term_color(t, VX_LIGHT_GREY, VX_BLACK);
    term_clear(t);
}

static void scroll(struct term *t)
{
    struct surface *s = t->surf;
    int keep = (t->rows - 1) * VX_FONT_H;
    memcpy32(s->pix, s->pix + VX_FONT_H * s->w, (size_t)(keep * s->w));
    gfx_fill(s, 0, keep, s->w, s->h - keep, t->bg);
    t->row = t->rows - 1;
}

void term_putc(struct term *t, char c)
{
    cursor(t, 0);
    switch (c) {
    case '\n': t->col = 0; t->row++; break;
    case '\r': t->col = 0; break;
    case '\t': t->col = (t->col + 8) & ~7; break;
    case '\b':
        if (t->col > 0) t->col--;
        else if (t->row > 0) { t->row--; t->col = t->cols - 1; }
        gfx_char_bg(t->surf, t->col * VX_FONT_W, t->row * VX_FONT_H, ' ', t->fg, t->bg);
        break;
    default:
        gfx_char_bg(t->surf, t->col * VX_FONT_W, t->row * VX_FONT_H, c, t->fg, t->bg);
        t->col++;
    }
    if (t->col >= t->cols) { t->col = 0; t->row++; }
    if (t->row >= t->rows) scroll(t);
    cursor(t, 1);
    wm_mark_dirty();
}
