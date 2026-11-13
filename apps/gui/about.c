/* about: a minimal window with text and a button. */
#include <vortex.h>

#define W 320
#define H 170

static int in_button(const struct vx_event *ev)
{
    return ev->x >= 120 && ev->x < 200 && ev->y >= 126 && ev->y < 154;
}

int main(void)
{
    struct vx_time t;
    struct vx_event ev;
    char line[40];

    int win = vx_win_create("About VortexOS", W, H);
    if (win < 0) {
        printf("about: needs the desktop (run 'desktop' first)\n");
        return 1;
    }
    vx_time(&t);
    vx_win_fill(win, 0, 0, W, H, VX_RGB(0xEC, 0xEF, 0xF1));
    vx_win_fill(win, 0, 0, W, 44, VX_RGB(0x2F, 0x6F, 0xBF));
    vx_win_text(win, 16, 14, "VortexOS", VX_RGB(255, 255, 255));
    vx_win_text(win, 16, 58, "A hobby operating system in C.", VX_RGB(0x1B, 0x25, 0x33));
    snprintf(line, sizeof(line), "Uptime: %u s", vx_ticks() / VX_TICKS_PER_SEC);
    vx_win_text(win, 16, 80, line, VX_RGB(0x1B, 0x25, 0x33));
    snprintf(line, sizeof(line), "Date:   %04d-%02d-%02d", t.year, t.month, t.day);
    vx_win_text(win, 16, 98, line, VX_RGB(0x1B, 0x25, 0x33));
    vx_win_fill(win, 120, 126, 80, 28, VX_RGB(0x3D, 0x8B, 0xFD));
    vx_win_text(win, 144, 132, "OK", VX_RGB(255, 255, 255));

    for (;;) {
        vx_win_event(win, &ev, -1);
        if (ev.type == VX_EV_MOUSE_DOWN && in_button(&ev)) break;
        if (ev.type == VX_EV_KEY && (ev.key == '\n' || ev.key == 27)) break;
    }
    return 0;
}
