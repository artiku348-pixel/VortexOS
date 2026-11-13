/* Desktop environment: a compositing window manager with a taskbar, a
 * launcher menu, draggable windows, terminal windows and a mouse cursor.
 *
 * The desktop loop runs in the kernel task. Every window has a pixel buffer
 * for its client area; applications draw into it through system calls and
 * the compositor redraws the screen whenever something changed. */
#include "kernel.h"

#define MAX_WIN    16
#define TITLE_H    24
#define TASKBAR_H  32
#define EVQ        32
#define MENU_W     190
#define MENU_ITEM  26
#define MENU_MAX   12
#define START_W    86
#define TASKBTN_W  150

#define C_TITLE_ACTIVE   0x2F6FBF
#define C_TITLE_INACTIVE 0x6C7A89
#define C_BORDER         0x1B2533
#define C_TASKBAR        0x1E2430
#define C_TASKBTN        0x323B4C
#define C_ACCENT         0x3D8BFD
#define C_MENU           0xF4F6F8
#define C_TEXT_DARK      0x1B2533
#define C_WHITE          0xFFFFFF

struct window {
    int id, owner;
    int x, y;                           /* top-left of the client area */
    struct surface surf;                /* client area pixels */
    char title[32];
    struct console *con;                /* terminal windows: where keys go */
    struct vx_event ev[EVQ];
    u32 eh, et;
};

struct menu_item {
    char label[24];
    char path[VX_PATH_MAX];             /* app to launch; "" = built-in action */
    int action;
};
enum { ACT_APP, ACT_TERMINAL, ACT_EXIT };

static struct window *z[MAX_WIN];       /* z-order, last = topmost / focused */
static int nwin, next_id = 1;
static int running, dirty, quit;
static struct surface back;             /* composed frame without the cursor */
static struct surface *scr;
static int mx, my, mbuttons;            /* mouse state */
static int cur_x, cur_y;                /* where the cursor was last drawn */
static struct window *drag;
static int drag_dx, drag_dy;
static int menu_open, nitems;
static struct menu_item items[MENU_MAX];
static struct vx_time now;

static const char *const cursor_shape[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ",
    "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..X X..X   ", "X.X  X..X   ", "XX    X..X  ",
    "X     X..X  ", "       XX   ",
};
#define CURSOR_W 12
#define CURSOR_H 18

int  wm_running(void) { return running; }
void wm_mark_dirty(void) { dirty = 1; }

/* ---- window list ------------------------------------------------------- */

static struct window *find(int owner, int id)
{
    for (int i = 0; i < nwin; i++)
        if (z[i]->id == id && z[i]->owner == owner) return z[i];
    return NULL;
}

static void raise(struct window *w)
{
    int i = 0;
    while (i < nwin && z[i] != w) i++;
    if (i == nwin) return;
    for (; i < nwin - 1; i++) z[i] = z[i + 1];
    z[nwin - 1] = w;
    dirty = 1;
}

static void remove_at(int i)
{
    struct window *w = z[i];
    if (drag == w) drag = NULL;
    for (; i < nwin - 1; i++) z[i] = z[i + 1];
    nwin--;
    kfree(w->surf.pix);
    kfree(w);
    dirty = 1;
}

void wm_destroy_owned(int task_id)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (z[i]->owner == task_id) remove_at(i);
}

int wm_create(int owner, const char *title, int w, int h)
{
    if (!running || nwin >= MAX_WIN) return -1;
    if (w < 80) w = 80;
    if (h < 40) h = 40;
    if (w > scr->w - 2) w = scr->w - 2;
    if (h > scr->h - TASKBAR_H - TITLE_H - 2) h = scr->h - TASKBAR_H - TITLE_H - 2;

    struct window *win = kmalloc(sizeof(*win));
    u32 *pix = kmalloc((size_t)(w * h) * sizeof(u32));
    if (!win || !pix) { kfree(win); kfree(pix); return -1; }
    memset(win, 0, sizeof(*win));
    win->id = next_id++;
    win->owner = owner;
    win->surf.pix = pix;
    win->surf.w = w;
    win->surf.h = h;
    strlcpy(win->title, title ? title : "Window", sizeof(win->title));
    gfx_fill(&win->surf, 0, 0, w, h, 0xECEFF1);

    int step = (win->id - 1) % 8;       /* cascade new windows */
    win->x = 60 + step * 36;
    win->y = 50 + TITLE_H + step * 30;
    if (win->x + w > scr->w) win->x = scr->w - w - 1;
    if (win->y + h > scr->h - TASKBAR_H) win->y = scr->h - TASKBAR_H - h - 1;

    z[nwin++] = win;
    dirty = 1;
    return win->id;
}

int wm_destroy(int owner, int id)
{
    for (int i = 0; i < nwin; i++)
        if (z[i]->id == id && z[i]->owner == owner) { remove_at(i); return 0; }
    return -1;
}

int wm_fill(int owner, int id, const struct vx_rect *r, u32 color)
{
    struct window *w = find(owner, id);
    if (!w) return -1;
    gfx_fill(&w->surf, r->x, r->y, r->w, r->h, color);
    dirty = 1;
    return 0;
}

int wm_text(int owner, int id, int x, int y, const char *s, u32 color)
{
    struct window *w = find(owner, id);
    if (!w) return -1;
    gfx_text(&w->surf, x, y, s, color);
    dirty = 1;
    return 0;
}

int wm_blit(int owner, int id, const struct vx_rect *r, const u32 *pixels)
{
    struct window *w = find(owner, id);
    if (!w || r->w <= 0 || r->h <= 0) return -1;
    gfx_blit(&w->surf, r->x, r->y, pixels, r->w, r->h);
    dirty = 1;
    return 0;
}

int wm_event(int owner, int id, struct vx_event *ev)
{
    struct window *w = find(owner, id);
    if (!w) return -1;
    if (w->eh == w->et) return 0;
    *ev = w->ev[w->et];
    w->et = (w->et + 1) % EVQ;
    return 1;
}

static void post(struct window *w, int type, int key, int x, int y)
{
    if (w->con) return;                 /* terminals take keys via their console */
    u32 last = (w->eh + EVQ - 1) % EVQ;
    struct vx_event *ev;

    if (type == VX_EV_MOUSE_MOVE && w->eh != w->et && w->ev[last].type == VX_EV_MOUSE_MOVE) {
        ev = &w->ev[last];              /* coalesce consecutive moves */
    } else {
        u32 next = (w->eh + 1) % EVQ;
        if (next == w->et) return;      /* queue full: drop */
        ev = &w->ev[w->eh];
        w->eh = next;
    }
    ev->type = type;
    ev->key = key;
    ev->x = x;
    ev->y = y;
    ev->buttons = mbuttons;
}

/* ---- terminals and the launcher ---------------------------------------- */

struct termwin {
    struct console con;
    struct term term;
};

static void shell_task(void *arg)
{
    (void)arg;
    shell_run();
}

static void open_terminal(void)
{
    struct task *t = task_create("shell", shell_task, NULL);
    if (!t) return;
    struct termwin *tw = task_alloc(t, sizeof(*tw));
    int id = wm_create(t->id, "Terminal", 80 * VX_FONT_W, 25 * VX_FONT_H);
    if (!tw || id < 0) { task_kill(t->id); return; }

    struct window *w = find(t->id, id);
    memset(tw, 0, sizeof(*tw));
    term_init(&tw->term, &w->surf);
    tw->con.term = &tw->term;
    w->con = &tw->con;
    t->con = t->own_con = &tw->con;
    t->cap = NULL;
    const char *home = env_get("HOME");
    if (home && fs_lookup(home)) strlcpy(t->cwd, home, sizeof(t->cwd));
}

static void add_item(const char *label, const char *path, int action)
{
    if (nitems >= MENU_MAX) return;
    strlcpy(items[nitems].label, label, sizeof(items[nitems].label));
    strlcpy(items[nitems].path, path, sizeof(items[nitems].path));
    items[nitems++].action = action;
}

/* The launcher lists "Terminal" plus every program in /apps. */
static void build_menu(void)
{
    nitems = 0;
    add_item("Terminal", "", ACT_TERMINAL);
    struct fs_node *dir = fs_lookup("/apps");
    for (struct fs_node *n = dir ? dir->children : NULL; n; n = n->next) {
        char label[24], path[VX_PATH_MAX];
        if (!exec_is_elf(n) || nitems >= MENU_MAX - 1) continue;
        strlcpy(label, n->name, sizeof(label));
        if (label[0] >= 'a' && label[0] <= 'z') label[0] = (char)(label[0] - 32);
        fs_abspath(n, path, sizeof(path));
        add_item(label, path, ACT_APP);
    }
    add_item("Exit desktop", "", ACT_EXIT);
}

static void activate(const struct menu_item *it)
{
    if (it->action == ACT_TERMINAL) open_terminal();
    else if (it->action == ACT_EXIT) quit = 1;
    else {
        char *argv[2] = { (char *)it->path, NULL };
        struct fs_node *n = fs_lookup(it->path);
        if (n) exec_spawn(n, 1, argv);
    }
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_wallpaper(void)
{
    int h = back.h - TASKBAR_H;
    for (int y = 0; y < h; y++) {       /* vertical gradient */
        u32 r = 0x12 + (u32)(0x1A * y / h);
        u32 g = 0x3A + (u32)(0x48 * y / h);
        u32 b = 0x5C + (u32)(0x50 * y / h);
        memset32(back.pix + y * back.w, (r << 16) | (g << 8) | b, (size_t)back.w);
    }
    gfx_text(&back, back.w - 8 * 14 - 12, h - 24, "VortexOS " VORTEX_VERSION, 0xCFE3F5);
}

static void draw_window(const struct window *w, int focused)
{
    int fx = w->x - 1, fy = w->y - TITLE_H - 1;
    int fw = w->surf.w + 2, fh = w->surf.h + TITLE_H + 2;

    gfx_fill(&back, fx + 4, fy + 4, fw, fh, 0x0E1A26);              /* shadow */
    gfx_frame(&back, fx, fy, fw, fh, C_BORDER);
    gfx_fill(&back, w->x, w->y - TITLE_H, w->surf.w, TITLE_H,
             focused ? C_TITLE_ACTIVE : C_TITLE_INACTIVE);
    gfx_text(&back, w->x + 8, w->y - TITLE_H + 4, w->title, C_WHITE);

    int bx = w->x + w->surf.w - 21, by = w->y - TITLE_H + 4;         /* close box */
    gfx_fill(&back, bx, by, 16, 16, 0xD9534F);
    gfx_char(&back, bx + 4, by, 'x', C_WHITE);

    gfx_blit(&back, w->x, w->y, w->surf.pix, w->surf.w, w->surf.h);
}

static void two_digits(char *dst, int v)
{
    dst[0] = (char)('0' + v / 10 % 10);
    dst[1] = (char)('0' + v % 10);
}

static void draw_taskbar(void)
{
    int y = back.h - TASKBAR_H;
    gfx_fill(&back, 0, y, back.w, TASKBAR_H, C_TASKBAR);
    gfx_fill(&back, 0, y, back.w, 1, 0x3A4458);

    gfx_fill(&back, 4, y + 4, START_W - 8, TASKBAR_H - 8, menu_open ? C_ACCENT : C_TASKBTN);
    gfx_text(&back, 4 + (START_W - 8 - 6 * 8) / 2, y + 8, "Vortex", C_WHITE);

    for (int i = 0; i < nwin; i++) {    /* one button per window */
        int bx = START_W + 4 + i * (TASKBTN_W + 4);
        char label[17];
        if (bx + TASKBTN_W > back.w - 70) break;
        strlcpy(label, z[i]->title, sizeof(label));
        gfx_fill(&back, bx, y + 4, TASKBTN_W, TASKBAR_H - 8, C_TASKBTN);
        if (i == nwin - 1) gfx_fill(&back, bx, y + TASKBAR_H - 6, TASKBTN_W, 2, C_ACCENT);
        gfx_text(&back, bx + 8, y + 8, label, C_WHITE);
    }

    char clock[6] = "00:00";
    two_digits(clock, now.hour);
    two_digits(clock + 3, now.minute);
    gfx_text(&back, back.w - 5 * 8 - 12, y + 8, clock, C_WHITE);
}

static int menu_top(void) { return scr->h - TASKBAR_H - nitems * MENU_ITEM - 8; }

static void draw_menu(void)
{
    int top = menu_top();
    gfx_fill(&back, 4, top, MENU_W, nitems * MENU_ITEM + 8, C_MENU);
    gfx_frame(&back, 4, top, MENU_W, nitems * MENU_ITEM + 8, C_BORDER);
    for (int i = 0; i < nitems; i++) {
        int iy = top + 4 + i * MENU_ITEM;
        if (mx >= 4 && mx < 4 + MENU_W && my >= iy && my < iy + MENU_ITEM)
            gfx_fill(&back, 6, iy, MENU_W - 4, MENU_ITEM, 0xCFE0F7);
        gfx_text(&back, 18, iy + 5, items[i].label, C_TEXT_DARK);
    }
}

static void draw_cursor(void)
{
    for (int r = 0; r < CURSOR_H; r++)
        for (int c = 0; c < CURSOR_W; c++) {
            char p = cursor_shape[r][c];
            int x = mx + c, y = my + r;
            if (p == ' ' || x >= scr->w || y >= scr->h) continue;
            scr->pix[y * scr->w + x] = p == 'X' ? 0x000000 : 0xFFFFFF;
        }
    cur_x = mx;
    cur_y = my;
}

/* Copy a rectangle of the composed frame to the screen. */
static void present(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr->w) w = scr->w - x;
    if (y + h > scr->h) h = scr->h - y;
    for (int r = y; r < y + h && w > 0; r++)
        memcpy32(scr->pix + r * scr->w + x, back.pix + r * back.w + x, (size_t)w);
}

static void compose(void)
{
    draw_wallpaper();
    for (int i = 0; i < nwin; i++) draw_window(z[i], i == nwin - 1);
    draw_taskbar();
    if (menu_open) draw_menu();
    present(0, 0, scr->w, scr->h);
    draw_cursor();
    dirty = 0;
}

/* ---- input ------------------------------------------------------------- */

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void on_press(void)
{
    if (menu_open) {
        int top = menu_top(), idx = (my - top - 4) / MENU_ITEM;
        menu_open = 0;
        dirty = 1;
        if (inside(mx, my, 4, top + 4, MENU_W, nitems * MENU_ITEM) && idx >= 0 && idx < nitems)
            activate(&items[idx]);
        return;
    }

    if (my >= scr->h - TASKBAR_H) {
        if (mx < START_W) {
            build_menu();
            menu_open = 1;
            dirty = 1;
        } else {
            int i = (mx - START_W - 4) / (TASKBTN_W + 4);
            if (i >= 0 && i < nwin) raise(z[i]);
        }
        return;
    }

    for (int i = nwin - 1; i >= 0; i--) {
        struct window *w = z[i];
        if (!inside(mx, my, w->x - 1, w->y - TITLE_H - 1, w->surf.w + 2, w->surf.h + TITLE_H + 2))
            continue;
        raise(w);
        if (my >= w->y) {
            post(w, VX_EV_MOUSE_DOWN, 0, mx - w->x, my - w->y);
        } else if (inside(mx, my, w->x + w->surf.w - 21, w->y - TITLE_H + 4, 16, 16)) {
            task_kill(w->owner);        /* close box: the owner dies, windows follow */
        } else {
            drag = w;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
        }
        return;
    }
}

static void on_move(void)
{
    if (drag) {
        drag->x = mx - drag_dx;
        drag->y = my - drag_dy;
        if (drag->y < TITLE_H + 1) drag->y = TITLE_H + 1;
        if (drag->y > scr->h - TASKBAR_H) drag->y = scr->h - TASKBAR_H;
        dirty = 1;
    } else if (menu_open) {
        dirty = 1;                      /* hover highlight */
    } else if (nwin) {
        struct window *w = z[nwin - 1];
        if (inside(mx, my, w->x, w->y, w->surf.w, w->surf.h))
            post(w, VX_EV_MOUSE_MOVE, 0, mx - w->x, my - w->y);
    }
}

static int handle_mouse(void)
{
    struct mouse_packet p;
    int moved = 0;
    while (mouse_poll(&p)) {
        int old = mbuttons;
        task_poke();
        mx += p.dx;
        my += p.dy;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > scr->w - 1) mx = scr->w - 1;
        if (my > scr->h - 1) my = scr->h - 1;
        mbuttons = p.buttons;
        if (p.dx || p.dy) { moved = 1; on_move(); }
        if ((mbuttons & VX_MOUSE_LEFT) && !(old & VX_MOUSE_LEFT)) on_press();
        if (!(mbuttons & VX_MOUSE_LEFT) && (old & VX_MOUSE_LEFT)) {
            drag = NULL;
            if (nwin) post(z[nwin - 1], VX_EV_MOUSE_UP, 0, mx - z[nwin - 1]->x, my - z[nwin - 1]->y);
        }
    }
    return moved;
}

static void handle_keys(void)
{
    int key;
    while ((key = keyboard_poll()) >= 0) {
        task_poke();
        if (!nwin) continue;
        struct window *w = z[nwin - 1];
        if (w->con) console_push_key(w->con, key);
        else post(w, VX_EV_KEY, key, 0, 0);
    }
}

/* ---- main loop --------------------------------------------------------- */

void desktop_run(void)
{
    if (running) return;
    if (gfx_init() != 0) {
        kputs("desktop: no supported graphics adapter (needs Bochs VBE, e.g. QEMU -vga std)\n");
        return;
    }
    scr = gfx_screen();
    back.w = scr->w;
    back.h = scr->h;
    back.pix = kmalloc((size_t)(back.w * back.h) * sizeof(u32));
    if (!back.pix) { kputs("desktop: out of memory\n"); console_takeover(); return; }

    while (keyboard_poll() >= 0) { }    /* drop keys typed before the switch */
    running = 1;
    quit = menu_open = 0;
    drag = NULL;
    mx = scr->w / 2;
    my = scr->h / 2;
    rtc_read(&now);
    open_terminal();
    dirty = 1;

    u32 last_sec = timer_ticks() / TIMER_HZ;
    while (!quit) {
        handle_keys();
        int moved = handle_mouse();

        u32 sec = timer_ticks() / TIMER_HZ;
        if (sec != last_sec) {          /* refresh the clock once a second */
            int minute = now.minute;
            last_sec = sec;
            rtc_read(&now);
            if (now.minute != minute) dirty = 1;
        }

        if (dirty) {
            compose();
        } else if (moved) {             /* only the cursor moved: cheap update */
            present(cur_x, cur_y, CURSOR_W, CURSOR_H);
            draw_cursor();
        }
        task_yield();
    }

    for (int i = 2; i < TASK_MAX; i++) {            /* close everything */
        struct task *t = task_slot(i);
        if (t->state == TASK_READY) task_kill(t->id);
    }
    while (nwin) remove_at(nwin - 1);
    kfree(back.pix);
    running = 0;
    console_takeover();
}
