/* VortexOS kernel ABI: shared between the kernel and the application SDK.
 * System calls use `int $0x80`: EAX = number, EBX/ECX/EDX/ESI/EDI = arguments,
 * result in EAX (negative = error). */
#ifndef VORTEX_ABI_H
#define VORTEX_ABI_H

enum {
    /* process / console */
    VX_SYS_EXIT = 0, VX_SYS_WRITE, VX_SYS_GETKEY, VX_SYS_SLEEP, VX_SYS_TICKS,
    VX_SYS_TIME, VX_SYS_YIELD, VX_SYS_COLOR, VX_SYS_CLEAR, VX_SYS_SBRK,
    VX_SYS_RESERVED, VX_SYS_SPAWN, VX_SYS_WAIT, VX_SYS_GETENV, VX_SYS_SETENV,
    VX_SYS_GETCWD, VX_SYS_CHDIR,
    /* files */
    VX_SYS_FS_READ = 20, VX_SYS_FS_WRITE, VX_SYS_FS_STAT, VX_SYS_FS_READDIR,
    VX_SYS_FS_MKDIR, VX_SYS_FS_REMOVE,
    /* windows */
    VX_SYS_WIN_CREATE = 30, VX_SYS_WIN_DESTROY, VX_SYS_WIN_FILL,
    VX_SYS_WIN_TEXT, VX_SYS_WIN_BLIT, VX_SYS_WIN_EVENT,
};

#define VX_TICKS_PER_SEC 100

/* Key codes: printable ASCII, '\n', '\b', '\t', 27 (Esc), Ctrl+letter = 1..26,
 * and the special keys below. */
enum {
    VX_KEY_UP = 0x80, VX_KEY_DOWN, VX_KEY_LEFT, VX_KEY_RIGHT,
    VX_KEY_HOME, VX_KEY_END, VX_KEY_PGUP, VX_KEY_PGDN, VX_KEY_DELETE,
};

/* Console colours (same as the VGA text palette). */
enum {
    VX_BLACK, VX_BLUE, VX_GREEN, VX_CYAN, VX_RED, VX_MAGENTA, VX_BROWN,
    VX_LIGHT_GREY, VX_DARK_GREY, VX_LIGHT_BLUE, VX_LIGHT_GREEN, VX_LIGHT_CYAN,
    VX_LIGHT_RED, VX_LIGHT_MAGENTA, VX_YELLOW, VX_WHITE,
};

/* Window events. Coordinates are relative to the window's client area. */
enum { VX_EV_NONE, VX_EV_KEY, VX_EV_MOUSE_DOWN, VX_EV_MOUSE_UP, VX_EV_MOUSE_MOVE };
#define VX_MOUSE_LEFT  1
#define VX_MOUSE_RIGHT 2

struct vx_event {
    int type;
    int key;                    /* VX_EV_KEY */
    int x, y;                   /* mouse events */
    int buttons;                /* VX_MOUSE_* mask */
};

struct vx_rect { int x, y, w, h; };

struct vx_time { int year, month, day, hour, minute, second; };

struct vx_stat {
    unsigned size;
    int is_dir;
};

#define VX_NAME_MAX 64
#define VX_PATH_MAX 128
#define VX_FONT_W 8
#define VX_FONT_H 16
#define VX_RGB(r, g, b) (((unsigned)(r) << 16) | ((unsigned)(g) << 8) | (unsigned)(b))

#endif
