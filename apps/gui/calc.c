/* calc: an integer calculator driven by mouse or keyboard. */
#include <vortex.h>

#define BTN   56
#define GAP   6
#define W     (4 * BTN + 5 * GAP)
#define TOP   64
#define H     (TOP + 4 * (BTN + GAP))

static const char keys[4][4] = {
    { '7', '8', '9', '/' },
    { '4', '5', '6', '*' },
    { '1', '2', '3', '-' },
    { 'C', '0', '=', '+' },
};

static int win;
static int acc, cur, entering, error;
static char op;

static void draw(void)
{
    char text[24];

    vx_win_fill(win, 0, 0, W, H, VX_RGB(0x26, 0x2B, 0x36));
    vx_win_fill(win, GAP, GAP, W - 2 * GAP, TOP - 2 * GAP, VX_RGB(0xDD, 0xE6, 0xD5));
    if (error) snprintf(text, sizeof(text), "Error");
    else snprintf(text, sizeof(text), "%d", entering ? cur : acc);
    vx_win_text(win, W - GAP - 10 - (int)strlen(text) * VX_FONT_W, 24, text, VX_RGB(0x10, 0x18, 0x10));
    if (op) {
        char o[2] = { op, 0 };
        vx_win_text(win, GAP + 8, 24, o, VX_RGB(0x60, 0x70, 0x60));
    }

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            char label[2] = { keys[r][c], 0 };
            int x = GAP + c * (BTN + GAP), y = TOP + r * (BTN + GAP);
            int digit = label[0] >= '0' && label[0] <= '9';
            vx_win_fill(win, x, y, BTN, BTN,
                        digit ? VX_RGB(0x4A, 0x52, 0x63)
                              : label[0] == '=' ? VX_RGB(0x3D, 0x8B, 0xFD) : VX_RGB(0xE0, 0x8A, 0x2E));
            vx_win_text(win, x + BTN / 2 - 4, y + BTN / 2 - 8, label, VX_RGB(255, 255, 255));
        }
}

static void apply(void)
{
    if (!op) { acc = cur; return; }
    switch (op) {
    case '+': acc += cur; break;
    case '-': acc -= cur; break;
    case '*': acc *= cur; break;
    case '/':
        if (cur == 0) error = 1;
        else acc /= cur;
        break;
    }
}

static void press(char k)
{
    if (k == 'C' || k == 'c' || error) {
        acc = cur = entering = error = 0;
        op = 0;
        if (k < '0' || k > '9') return;
    }
    if (k >= '0' && k <= '9') {
        if (!entering) { cur = 0; entering = 1; }
        if (cur < 100000000) cur = cur * 10 + (k - '0');
    } else if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (entering) apply();
        op = k;
        entering = 0;
    } else if (k == '=' || k == '\n') {
        if (entering) apply();
        op = 0;
        entering = 0;
    } else if (k == '\b' && entering) {
        cur /= 10;
    }
}

int main(void)
{
    struct vx_event ev;

    win = vx_win_create("Calculator", W, H);
    if (win < 0) {
        printf("calc: needs the desktop (run 'desktop' first)\n");
        return 1;
    }
    draw();
    for (;;) {
        vx_win_event(win, &ev, -1);
        if (ev.type == VX_EV_KEY) {
            if (ev.key == 27) break;
            press((char)ev.key);
        } else if (ev.type == VX_EV_MOUSE_DOWN && ev.y >= TOP) {
            int c = (ev.x - GAP) / (BTN + GAP), r = (ev.y - TOP) / (BTN + GAP);
            if (ev.x >= GAP && c < 4 && r < 4) press(keys[r][c]);
        } else {
            continue;
        }
        draw();
    }
    return 0;
}
