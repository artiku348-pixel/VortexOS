/* 80x25 VGA text-mode console. */
#include "kernel.h"

#define COLS 80
#define ROWS 25
static u16 *const vram = (u16 *)0xB8000;
static int row, col;
static u8 attr;

static inline u16 cell(char c) { return (u16)(u8)c | ((u16)attr << 8); }

static void sync_cursor(void)
{
    u16 pos = (u16)(row * COLS + col);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)pos);
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)(pos >> 8));
}

static void scroll(void)
{
    memmove(vram, vram + COLS, (ROWS - 1) * COLS * sizeof(u16));
    for (int x = 0; x < COLS; x++) vram[(ROWS - 1) * COLS + x] = cell(' ');
    row = ROWS - 1;
}

void vga_set_color(u8 fg, u8 bg) { attr = (u8)((bg << 4) | (fg & 0x0F)); }

void vga_clear(void)
{
    for (int i = 0; i < COLS * ROWS; i++) vram[i] = cell(' ');
    row = col = 0;
    sync_cursor();
}

void vga_putc(char c)
{
    switch (c) {
    case '\n': col = 0; row++; break;
    case '\r': col = 0; break;
    case '\t': col = (col + 8) & ~7; break;
    case '\b':
        if (col > 0) col--;
        else if (row > 0) { row--; col = COLS - 1; }
        vram[row * COLS + col] = cell(' ');
        break;
    default:
        vram[row * COLS + col] = cell(c);
        col++;
    }
    if (col >= COLS) { col = 0; row++; }
    if (row >= ROWS) scroll();
    sync_cursor();
}

void vga_init(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}
