/* snake: arrow keys (or WASD) to steer, Space to restart, Esc to quit. */
#include <vortex.h>

#define CELL  16
#define COLS  28
#define ROWS  20
#define BAR   24
#define W     (COLS * CELL)
#define H     (ROWS * CELL + BAR)
#define MAXLEN (COLS * ROWS)

static int win;
static int sx[MAXLEN], sy[MAXLEN], len;
static int dx, dy, fx, fy, score, dead;

static void cell(int x, int y, uint32_t color)
{
    vx_win_fill(win, x * CELL + 1, BAR + y * CELL + 1, CELL - 2, CELL - 2, color);
}

static void place_food(void)
{
    for (;;) {
        int ok = 1;
        fx = rand() % COLS;
        fy = rand() % ROWS;
        for (int i = 0; i < len; i++)
            if (sx[i] == fx && sy[i] == fy) ok = 0;
        if (ok) return;
    }
}

static void draw(void)
{
    char text[48];

    vx_win_fill(win, 0, 0, W, BAR, VX_RGB(0x1E, 0x24, 0x30));
    snprintf(text, sizeof(text), dead ? "Game over! Score: %d  (Space = again)" : "Score: %d", score);
    vx_win_text(win, 8, 4, text, VX_RGB(255, 255, 255));

    vx_win_fill(win, 0, BAR, W, ROWS * CELL, VX_RGB(0x10, 0x1C, 0x14));
    cell(fx, fy, VX_RGB(0xE7, 0x4C, 0x3C));
    for (int i = 0; i < len; i++)
        cell(sx[i], sy[i], i == 0 ? VX_RGB(0xA6, 0xE2, 0x2E) : VX_RGB(0x5C, 0xB8, 0x5C));
}

static void reset(void)
{
    len = 4;
    for (int i = 0; i < len; i++) { sx[i] = COLS / 2 - i; sy[i] = ROWS / 2; }
    dx = 1; dy = 0;
    score = dead = 0;
    place_food();
}

static void step(void)
{
    int nx = sx[0] + dx, ny = sy[0] + dy;

    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) { dead = 1; return; }
    for (int i = 0; i < len - 1; i++)
        if (sx[i] == nx && sy[i] == ny) { dead = 1; return; }

    int grow = nx == fx && ny == fy;
    if (grow && len < MAXLEN) len++;
    for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
    sx[0] = nx; sy[0] = ny;
    if (grow) { score += 10; place_food(); }
}

static void turn(int ndx, int ndy)
{
    if (ndx == -dx && ndy == -dy) return;           /* no U-turns */
    dx = ndx; dy = ndy;
}

int main(void)
{
    struct vx_event ev;

    win = vx_win_create("Snake", W, H);
    if (win < 0) {
        printf("snake: needs the desktop (run 'desktop' first)\n");
        return 1;
    }
    srand(vx_ticks());
    reset();
    draw();

    unsigned next = vx_ticks() + 12;
    for (;;) {
        int left = (int)(next - vx_ticks());
        if (left > 0 && vx_win_event(win, &ev, left * 10) == 1) {
            if (ev.type != VX_EV_KEY) continue;
            switch (ev.key) {
            case 27: return 0;
            case ' ': if (dead) { reset(); draw(); } break;
            case VX_KEY_UP:    case 'w': turn(0, -1); break;
            case VX_KEY_DOWN:  case 's': turn(0, 1);  break;
            case VX_KEY_LEFT:  case 'a': turn(-1, 0); break;
            case VX_KEY_RIGHT: case 'd': turn(1, 0);  break;
            }
            continue;
        }
        next = vx_ticks() + 12;                     /* ~8 moves per second */
        if (!dead) { step(); draw(); }
    }
}
