/* paint: draw with the mouse. Click a colour to select it, C clears. */
#include <vortex.h>

#define W       480
#define H       340
#define BAR     28
#define SWATCH  24

static const uint32_t colors[] = {
    0x000000, 0xD9534F, 0xF0AD4E, 0x5CB85C, 0x3D8BFD, 0x9B59B6, 0xFFFFFF,
};
#define NCOLORS ((int)(sizeof(colors) / sizeof(colors[0])))

static int win, color_idx;

static void draw_bar(void)
{
    vx_win_fill(win, 0, 0, W, BAR, VX_RGB(0x32, 0x3B, 0x4C));
    for (int i = 0; i < NCOLORS; i++) {
        int x = 4 + i * (SWATCH + 4);
        if (i == color_idx) vx_win_fill(win, x - 2, 0, SWATCH + 4, BAR, VX_RGB(255, 255, 255));
        vx_win_fill(win, x, 2, SWATCH, BAR - 4, colors[i]);
    }
    vx_win_text(win, W - 15 * VX_FONT_W, 6, "C = clear all", VX_RGB(255, 255, 255));
}

static void clear(void)
{
    vx_win_fill(win, 0, BAR, W, H - BAR, VX_RGB(255, 255, 255));
}

static void brush(int x, int y)
{
    if (y - 3 < BAR) return;
    vx_win_fill(win, x - 3, y - 3, 6, 6, colors[color_idx]);
}

/* Fill the gap between two mouse samples so fast strokes stay continuous. */
static void stroke(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady) / 2 + 1;
    for (int i = 1; i <= steps; i++) brush(x0 + dx * i / steps, y0 + dy * i / steps);
}

int main(void)
{
    struct vx_event ev;
    int drawing = 0, lx = 0, ly = 0;

    win = vx_win_create("Paint", W, H);
    if (win < 0) {
        printf("paint: needs the desktop (run 'desktop' first)\n");
        return 1;
    }
    clear();
    draw_bar();

    for (;;) {
        vx_win_event(win, &ev, -1);
        switch (ev.type) {
        case VX_EV_KEY:
            if (ev.key == 27) return 0;
            if (ev.key == 'c' || ev.key == 'C') clear();
            break;
        case VX_EV_MOUSE_DOWN:
            if (ev.y < BAR) {
                int i = (ev.x - 4) / (SWATCH + 4);
                if (ev.x >= 4 && i < NCOLORS) { color_idx = i; draw_bar(); }
            } else {
                drawing = 1;
                lx = ev.x; ly = ev.y;
                brush(lx, ly);
            }
            break;
        case VX_EV_MOUSE_MOVE:
            if (drawing && (ev.buttons & VX_MOUSE_LEFT)) {
                stroke(lx, ly, ev.x, ev.y);
                lx = ev.x; ly = ev.y;
            }
            break;
        case VX_EV_MOUSE_UP:
            drawing = 0;
            break;
        }
    }
}
