/* The file tree. Nodes outside /disk live only in memory (their initial
 * contents come from the initrd, a ustar archive passed by the bootloader).
 * Nodes below /disk mirror the FAT volume: fat.c fills them in on demand
 * (fs_open) and is told about every change. */
#include "kernel.h"

static struct fs_node root = { .name = "", .is_dir = 1 };

void fs_init(void)
{
    fs_create("/bin", 1);
    fs_create("/apps", 1);
    fs_create("/etc", 1);
    fs_create("/home", 1);
    fs_create("/tmp", 1);
}

int fs_open(struct fs_node *n)
{
    if (!n) return -1;
    if ((n->fat_flags & (FAT_ON_DISK | FAT_LOADED)) == FAT_ON_DISK) return fat_load(n);
    return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static struct fs_node *child(struct fs_node *dir, const char *name, size_t len)
{
    int nocase = dir->fat_flags & FAT_ON_DISK;      /* FAT names ignore case */
    if (fs_open(dir)) return NULL;
    for (struct fs_node *n = dir->children; n; n = n->next) {
        if (strlen(n->name) != len) continue;
        size_t i = 0;
        while (i < len && (nocase ? lower(n->name[i]) == lower(name[i]) : n->name[i] == name[i])) i++;
        if (i == len) return n;
    }
    return NULL;
}

/* Walk `path` from `from`, handling "." and "..". */
static struct fs_node *walk(struct fs_node *from, const char *path)
{
    struct fs_node *n = from;
    while (n && *path) {
        while (*path == '/') path++;
        size_t len = 0;
        while (path[len] && path[len] != '/') len++;
        if (!len) break;
        if (!n->is_dir) return NULL;
        if (len == 2 && path[0] == '.' && path[1] == '.') n = n->parent ? n->parent : n;
        else if (!(len == 1 && path[0] == '.')) n = child(n, path, len);
        path += len;
    }
    return n;
}

struct fs_node *fs_lookup(const char *path)
{
    struct task *t = task_current();
    struct fs_node *from = &root;
    if (path[0] != '/' && t) from = walk(&root, t->cwd);
    return walk(from, path);
}

/* Append a new node to `parent` (no checks: see fs_create). */
struct fs_node *fs_node_new(struct fs_node *parent, const char *name, int is_dir)
{
    struct fs_node *n = kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strlcpy(n->name, name, sizeof(n->name));
    n->is_dir = !!is_dir;
    n->parent = parent;

    struct fs_node **tail = &parent->children;      /* keep creation order */
    while (*tail) tail = &(*tail)->next;
    *tail = n;
    return n;
}

static void unlink(struct fs_node *n)
{
    struct fs_node **link = &n->parent->children;
    while (*link != n) link = &(*link)->next;
    *link = n->next;
}

struct fs_node *fs_create(const char *path, int is_dir)
{
    char dir[VX_PATH_MAX];
    const char *name = strrchr(path, '/');
    struct fs_node *parent;

    if (strlen(path) >= VX_PATH_MAX) return NULL;
    if (name) {
        size_t n = (size_t)(name - path);
        memcpy(dir, path, n);
        dir[n] = 0;
        parent = n ? fs_lookup(dir) : &root;
        name++;
    } else {
        parent = fs_lookup(".");
        name = path;
    }
    size_t len = strlen(name);
    if (!parent || !parent->is_dir || !len || len >= VX_NAME_MAX) return NULL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return NULL;

    struct fs_node *n = child(parent, name, len);
    if (n) return n->is_dir == !!is_dir ? n : NULL;

    if ((parent->fat_flags & FAT_ON_DISK) && fs_open(parent)) return NULL;
    n = fs_node_new(parent, name, is_dir);
    if (n && (parent->fat_flags & FAT_ON_DISK) && fat_create(n)) {
        unlink(n);                                  /* no room on the volume, bad name... */
        kfree(n);
        return NULL;
    }
    return n;
}

int fs_write(struct fs_node *n, const void *data, u32 len, int append)
{
    if (!n || n->is_dir) return -1;
    if (append && fs_open(n)) return -1;            /* need the old contents */
    u32 off = append ? n->size : 0;
    u32 need = off + len;
    if (need > n->cap) {
        u32 cap = need + need / 2 + 64;
        u8 *buf = kmalloc(cap);
        if (!buf) return -1;
        if (n->data) { memcpy(buf, n->data, off); kfree(n->data); }
        n->data = buf;
        n->cap = cap;
    }
    memcpy(n->data + off, data, len);
    n->size = need;
    if (n->fat_flags & FAT_ON_DISK) fat_dirty(n);
    return (int)len;
}

/* Returns 0, -1 (not found / root) or -2 (directory not empty). */
int fs_remove(const char *path)
{
    struct fs_node *n = fs_lookup(path);
    if (!n || n == &root || fat_is_mount(n)) return -1;
    if (n->is_dir && (fs_open(n) || n->children)) return -2;
    if ((n->fat_flags & FAT_ON_DISK) && fat_remove(n)) return -1;

    unlink(n);
    kfree(n->data);
    kfree(n);
    return 0;
}

void fs_abspath(const struct fs_node *n, char *buf, size_t max)
{
    const struct fs_node *chain[32];
    int depth = 0;
    size_t pos = 0;

    for (; n && n->parent && depth < 32; n = n->parent) chain[depth++] = n;
    if (!depth) { strlcpy(buf, "/", max); return; }
    while (depth--) {
        const char *s = chain[depth]->name;
        if (pos + 1 < max) buf[pos++] = '/';
        while (*s && pos + 1 < max) buf[pos++] = *s++;
    }
    buf[pos] = 0;
}

/* ---- initrd (ustar) ---------------------------------------------------- */

static u32 octal(const char *s, int n)
{
    u32 v = 0;
    while (n-- && *s >= '0' && *s <= '7') v = v * 8 + (u32)(*s++ - '0');
    return v;
}

static void make_parents(char *path)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') { *p = 0; fs_create(path, 1); *p = '/'; }
}

int fs_load_tar(const u8 *tar, u32 len)
{
    int files = 0;
    for (u32 off = 0; off + 512 <= len; ) {
        const char *h = (const char *)tar + off;
        if (!h[0]) break;                           /* end-of-archive marker */
        u32 size = octal(h + 124, 12);
        char type = h[156];

        char path[VX_PATH_MAX];
        const char *name = h;
        size_t nlen = 0;
        while (nlen < 100 && name[nlen]) nlen++;
        if (nlen >= 2 && name[0] == '.' && name[1] == '/') { name += 2; nlen -= 2; }
        while (nlen && name[nlen - 1] == '/') nlen--;

        if (nlen && nlen + 2 < sizeof(path)) {
            path[0] = '/';
            memcpy(path + 1, name, nlen);
            path[nlen + 1] = 0;
            make_parents(path);
            if (type == '5') {
                fs_create(path, 1);
            } else if (type == '0' || type == 0) {
                struct fs_node *n = fs_create(path, 0);
                if (n && off + 512 + size <= len && fs_write(n, tar + off + 512, size, 0) >= 0)
                    files++;
            }
        }
        off += 512 + ((size + 511) & ~511u);
    }
    return files;
}
