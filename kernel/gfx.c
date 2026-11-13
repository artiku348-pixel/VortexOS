/* Graphics: Bochs VBE ("BGA") linear framebuffer, as provided by QEMU
 * (-vga std, the default), Bochs and VirtualBox. Plus clipped 2D drawing
 * primitives that work on any 32-bit surface. */
#include "kernel.h"

#define BGA_INDEX 0x1CE
#define BGA_DATA  0x1CF
enum { BGA_ID, BGA_XRES, BGA_YRES, BGA_BPP, BGA_ENABLE };

#define SCREEN_W 1024
#define SCREEN_H 768

static struct surface screen;
static int active;

static void bga_write(u16 index, u16 value)
{
    outw(BGA_INDEX, index);
    outw(BGA_DATA, value);
}

static u16 bga_read(u16 index)
{
    outw(BGA_INDEX, index);
    return inw(BGA_DATA);
}

static u32 pci_read(u32 bus, u32 dev, u32 off)
{
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | off);
    return inl(0xCFC);
}

/* The framebuffer address is BAR0 of the PCI display adapter. */
static u32 find_framebuffer(void)
{
    for (u32 bus = 0; bus < 8; bus++)
        for (u32 dev = 0; dev < 32; dev++) {
            if ((pci_read(bus, dev, 0x00) & 0xFFFF) == 0xFFFF) continue;
            if ((pci_read(bus, dev, 0x08) >> 24) != 0x03) continue;     /* display class */
            u32 bar0 = pci_read(bus, dev, 0x10);
            if (!(bar0 & 1) && (bar0 & ~0xFu)) return bar0 & ~0xFu;
        }
    return 0xE0000000;                  /* ISA Bochs default */
}

int gfx_available(void)
{
    u16 id = bga_read(BGA_ID);
    return id >= 0xB0C0 && id <= 0xB0CF;
}

int gfx_active(void) { return active; }
struct surface *gfx_screen(void) { return &screen; }

int gfx_init(void)
{
    if (active) return 0;
    if (!gfx_available()) return -1;
    bga_write(BGA_ENABLE, 0);
    bga_write(BGA_XRES, SCREEN_W);
    bga_write(BGA_YRES, SCREEN_H);
    bga_write(BGA_BPP, 32);
    bga_write(BGA_ENABLE, 0x01 | 0x40); /* enabled + linear framebuffer */
    if (bga_read(BGA_XRES) != SCREEN_W || bga_read(BGA_YRES) != SCREEN_H) {
        bga_write(BGA_ENABLE, 0);
        return -1;
    }
    screen.pix = (u32 *)find_framebuffer();
    screen.w = SCREEN_W;
    screen.h = SCREEN_H;
    active = 1;
    gfx_fill(&screen, 0, 0, SCREEN_W, SCREEN_H, 0);
    return 0;
}

void gfx_fill(struct surface *s, int x, int y, int w, int h, u32 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++)
        memset32(s->pix + row * s->w + x, color, (size_t)w);
}

void gfx_frame(struct surface *s, int x, int y, int w, int h, u32 color)
{
    gfx_fill(s, x, y, w, 1, color);
    gfx_fill(s, x, y + h - 1, w, 1, color);
    gfx_fill(s, x, y, 1, h, color);
    gfx_fill(s, x + w - 1, y, 1, h, color);
}

static const u8 *glyph(char c)
{
    return font8x16[(c < 32 || c > 126) ? '?' - 32 : c - 32];
}

void gfx_char(struct surface *s, int x, int y, char c, u32 fg)
{
    const u8 *g = glyph(c);
    for (int r = 0; r < VX_FONT_H; r++) {
        int py = y + r;
        if (py < 0 || py >= s->h) continue;
        for (int b = 0; b < VX_FONT_W; b++) {
            int px = x + b;
            if ((g[r] & (0x80 >> b)) && px >= 0 && px < s->w)
                s->pix[py * s->w + px] = fg;
        }
    }
}

/* Opaque glyph; the caller guarantees the cell is fully inside the surface. */
void gfx_char_bg(struct surface *s, int x, int y, char c, u32 fg, u32 bg)
{
    const u8 *g = glyph(c);
    if (x < 0 || y < 0 || x + VX_FONT_W > s->w || y + VX_FONT_H > s->h) return;
    for (int r = 0; r < VX_FONT_H; r++) {
        u32 *p = s->pix + (y + r) * s->w + x;
        for (int b = 0; b < VX_FONT_W; b++) p[b] = (g[r] & (0x80 >> b)) ? fg : bg;
    }
}

void gfx_text(struct surface *s, int x, int y, const char *str, u32 fg)
{
    for (; *str; str++, x += VX_FONT_W) gfx_char(s, x, y, *str, fg);
}

void gfx_blit(struct surface *dst, int dx, int dy, const u32 *src, int sw, int sh)
{
    int x0 = 0, y0 = 0, w = sw, h = sh;
    if (dx < 0) { x0 = -dx; w += dx; dx = 0; }
    if (dy < 0) { y0 = -dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++)
        memcpy32(dst->pix + (dy + r) * dst->w + dx, src + (y0 + r) * sw + x0, (size_t)w);
}
