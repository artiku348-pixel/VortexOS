/* files: a small file manager. Click (or arrows + Enter) to open a folder,
 * view a text file or start a program; Backspace goes up, Delete removes. */
#include <vortex.h>

#define W 440
#define H 344
#define ROW 18
#define TOP 30
#define ROWS ((H - TOP - 22) / ROW)
#define MAX_ENTRIES 128

#define C_BG     VX_RGB(0xF4, 0xF6, 0xF8)
#define C_BAR    VX_RGB(0x2B, 0x35, 0x45)
#define C_TEXT   VX_RGB(0x1B, 0x25, 0x33)
#define C_DIM    VX_RGB(0x70, 0x7C, 0x8C)
#define C_SELECT VX_RGB(0xCF, 0xE2, 0xFF)
#define C_FOLDER VX_RGB(0xF0, 0xB4, 0x3C)
#define C_FILE   VX_RGB(0x8F, 0xA3, 0xB8)
#define C_APP    VX_RGB(0x4C, 0xAF, 0x50)

struct entry { char name[VX_NAME_MAX]; unsigned size; int is_dir, is_app; };

static int win;
static char cwd[VX_PATH_MAX] = "/";
static struct entry entries[MAX_ENTRIES];
static int count, selected, scroll;
static char status[56];
static char view[2048];                 /* text shown by the viewer */
static int viewing;

static void join(char *out, const char *name)
{
    snprintf(out, VX_PATH_MAX, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", name);
}

static void load(void)
{
    char path[VX_PATH_MAX];
    unsigned char magic[4];
    struct vx_stat st;

    count = 0;
    for (int i = 0; count < MAX_ENTRIES && vx_readdir(cwd, i, entries[count].name) > 0; i++) {
        struct entry *e = &entries[count];
        join(path, e->name);
        if (vx_stat(path, &st) < 0) continue;
        e->size = st.size;
        e->is_dir = st.is_dir;
        e->is_app = !st.is_dir && vx_read_file(path, magic, 4, 0) == 4 &&
                    memcmp(magic, "\x7f" "ELF", 4) == 0;
        count++;
    }
    for (int i = 1; i < count; i++) {               /* folders first, then by name */
        struct entry e = entries[i];
        int j = i;
        while (j > 0 && (entries[j - 1].is_dir < e.is_dir ||
               (entries[j - 1].is_dir == e.is_dir && strcmp(entries[j - 1].name, e.name) > 0))) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = e;
    }
    selected = scroll = 0;
    snprintf(status, sizeof(status), "%d item(s)", count);
}

static void draw(void)
{
    char line[64];

    vx_win_fill(win, 0, 0, W, H, C_BG);
    vx_win_fill(win, 0, 0, W, TOP - 4, C_BAR);
    vx_win_fill(win, 6, 4, 26, 18, VX_RGB(0x3D, 0x8B, 0xFD));       /* "up" button */
    vx_win_text(win, 11, 5, "..", VX_RGB(255, 255, 255));
    vx_win_text(win, 42, 5, cwd, VX_RGB(255, 255, 255));

    if (viewing) {
        int x = 0, y = 0;
        char ch[2] = { 0, 0 };
        for (const char *p = view; *p && y < (H - TOP - 22) / VX_FONT_H; p++) {
            if (*p == '\n' || x >= (W - 16) / VX_FONT_W) { x = 0; y++; if (*p == '\n') continue; }
            ch[0] = (*p >= 32 && *p < 127) ? *p : '.';
            vx_win_text(win, 8 + x++ * VX_FONT_W, TOP + y * VX_FONT_H, ch, C_TEXT);
        }
    } else {
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + ROWS) scroll = selected - ROWS + 1;
        for (int i = scroll; i < count && i < scroll + ROWS; i++) {
            const struct entry *e = &entries[i];
            int y = TOP + (i - scroll) * ROW;
            if (i == selected) vx_win_fill(win, 2, y, W - 4, ROW, C_SELECT);
            vx_win_fill(win, 10, y + 4, 12, 10, e->is_dir ? C_FOLDER : e->is_app ? C_APP : C_FILE);
            if (e->is_dir) vx_win_fill(win, 10, y + 2, 6, 2, C_FOLDER);
            vx_win_text(win, 30, y + 1, e->name, C_TEXT);
            if (e->is_dir) strcpy(line, "folder");
            else snprintf(line, sizeof(line), "%u B%s", e->size, e->is_app ? "  program" : "");
            vx_win_text(win, 300, y + 1, line, C_DIM);
        }
        if (!count) vx_win_text(win, 30, TOP + 1, "(empty folder)", C_DIM);
    }
    vx_win_fill(win, 0, H - 20, W, 20, VX_RGB(0xE1, 0xE6, 0xEC));
    vx_win_text(win, 8, H - 18, status, C_DIM);
}

static void go_up(void)
{
    if (viewing) { viewing = 0; snprintf(status, sizeof(status), "%d item(s)", count); return; }
    char *slash = strrchr(cwd, '/');
    if (slash == cwd) cwd[1] = 0; else *slash = 0;
    load();
}

static void open_selected(void)
{
    char path[VX_PATH_MAX];
    if (viewing || selected >= count) return;
    const struct entry *e = &entries[selected];
    join(path, e->name);

    if (e->is_dir) {
        strcpy(cwd, path);
        load();
    } else if (e->is_app) {
        char *argv[2] = { path, NULL };
        int id = vx_spawn(path, 1, argv);
        snprintf(status, sizeof(status), id < 0 ? "cannot start %s" : "started %s", e->name);
    } else {
        int n = vx_read_file(path, view, sizeof(view) - 1, 0);
        view[n > 0 ? n : 0] = 0;
        viewing = 1;
        snprintf(status, sizeof(status), "%s: %u bytes (Backspace: back)", e->name, e->size);
    }
}

static void remove_selected(void)
{
    char path[VX_PATH_MAX], name[VX_NAME_MAX];
    if (viewing || selected >= count) return;
    strcpy(name, entries[selected].name);
    join(path, name);
    int r = vx_remove(path);
    load();
    snprintf(status, sizeof(status), r ? "cannot delete %s" : "deleted %s", name);
}

int main(int argc, char **argv)
{
    struct vx_event ev;
    struct vx_stat st;

    win = vx_win_create("Files", W, H);
    if (win < 0) {
        printf("files: needs the desktop (run 'desktop' first)\n");
        return 1;
    }
    if (argc > 1 && strlen(argv[1]) < VX_PATH_MAX && argv[1][0] == '/' &&
        vx_stat(argv[1], &st) == 0 && st.is_dir)
        strcpy(cwd, argv[1]);
    load();

    for (;;) {
        draw();
        vx_win_event(win, &ev, -1);
        if (ev.type == VX_EV_MOUSE_DOWN) {
            if (ev.y < TOP - 4) { if (ev.x < 36) go_up(); }
            else if (!viewing && ev.y >= TOP && ev.y < TOP + ROWS * ROW) {
                int i = scroll + (ev.y - TOP) / ROW;
                if (i < count) {
                    if (i == selected) open_selected();     /* click the selection to open */
                    else selected = i;
                }
            }
        } else if (ev.type == VX_EV_KEY) {
            if (ev.key == 27) break;
            else if (ev.key == VX_KEY_UP && selected > 0) selected--;
            else if (ev.key == VX_KEY_DOWN && selected + 1 < count) selected++;
            else if (ev.key == '\n') open_selected();
            else if (ev.key == '\b' || ev.key == VX_KEY_LEFT) go_up();
            else if (ev.key == VX_KEY_DELETE) remove_selected();
        }
    }
    return 0;
}
