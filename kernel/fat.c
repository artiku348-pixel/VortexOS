/* FAT16 volume on the ATA disk, mounted at /disk.
 *
 * The filesystem tree lives in memory (fs.c). Mounting reads the whole
 * volume into the tree; when something below /disk changes, the "diskd"
 * kernel thread writes the tree back: file data, directories (with long
 * file names) and both FAT copies. Simple, and good for small volumes. */
#include "kernel.h"

#define SECTOR      512
#define ATTR_DIR    0x10
#define ATTR_VOLUME 0x08
#define ATTR_LFN    0x0F
#define SYNC_DELAY  (TIMER_HZ / 2)      /* quiet time before an automatic sync */

struct bpb {                            /* boot sector fields, unpacked */
    u32 spc, reserved, fats, root_entries, total, fat_sectors;
    u32 root_start, root_sectors, data_start, clusters;
};

static struct bpb v;
static struct fs_node *mount;           /* the /disk directory, NULL if not mounted */
static volatile int dirty;
static u32 dirty_since, sync_count;
static int full;                        /* the tree does not fit on the volume */

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return rd16(p) | (u32)rd16(p + 2) << 16; }
static void wr16(u8 *p, u32 x) { p[0] = (u8)x; p[1] = (u8)(x >> 8); }
static void wr32(u8 *p, u32 x) { wr16(p, x); wr16(p + 2, x >> 16); }

static u32 cluster_lba(u32 c) { return v.data_start + (c - 2) * v.spc; }
static u32 cluster_bytes(void) { return v.spc * SECTOR; }

/* ---- reading ----------------------------------------------------------- */

static u16 *fat;                        /* in-memory copy of the allocation table */

static int read_fat(void)
{
    u8 sec[SECTOR];
    fat = kmalloc((v.clusters + 2) * sizeof(u16));
    if (!fat) return -1;
    for (u32 i = 0; i < v.clusters + 2; i++) {
        if (i % (SECTOR / 2) == 0 && ata_read(v.reserved + i / (SECTOR / 2), sec)) return -1;
        fat[i] = rd16(sec + (i % (SECTOR / 2)) * 2);
    }
    return 0;
}

/* Read a cluster chain of at most `max` bytes into a new buffer. */
static u8 *read_chain(u32 c, u32 max, u32 *len)
{
    u32 cap = 0, n = 0;
    for (u32 x = c; x >= 2 && x < v.clusters + 2 && n <= v.clusters; x = fat[x], n++)
        cap += cluster_bytes();
    if (cap > max) cap = (max + SECTOR - 1) / SECTOR * SECTOR;
    u8 *buf = kmalloc(cap ? cap : 1);
    if (!buf) return NULL;

    u32 pos = 0;
    for (; pos < cap; c = fat[c])
        for (u32 s = 0; s < v.spc && pos < cap; s++, pos += SECTOR)
            if (ata_read(cluster_lba(c) + s, buf + pos)) { kfree(buf); return NULL; }
    *len = cap;
    return buf;
}

/* Turn the 8.3 name of a directory entry into "name.ext". */
static void short_name(const u8 *e, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)e[i];
        if ((e[12] & 0x08) && c >= 'A' && c <= 'Z') c += 32;    /* lower-case base */
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            if ((e[12] & 0x10) && c >= 'A' && c <= 'Z') c += 32; /* lower-case extension */
            out[n++] = c;
        }
    }
    out[n] = 0;
}

static u8 lfn_checksum(const u8 *name11)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

static const u8 lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static int load_dir(struct fs_node *dir, const u8 *ents, u32 bytes, int depth);

static int load_entry(struct fs_node *dir, const u8 *e, const char *name, int depth)
{
    char path[VX_PATH_MAX];
    u32 c = rd16(e + 26), size = rd32(e + 28), len = 0;
    int is_dir = e[11] & ATTR_DIR;

    fs_abspath(dir, path, sizeof(path));
    if (strlen(path) + strlen(name) + 2 > sizeof(path)) return 0;   /* too deep: skip */
    size_t at = strlen(path);
    path[at] = '/';
    strcpy(path + at + 1, name);
    struct fs_node *n = fs_create(path, is_dir);
    if (!n) return 0;

    if (is_dir) {
        if (depth > 16 || c < 2) return 0;
        u8 *buf = read_chain(c, 0xFFFFFFFF, &len);
        if (!buf) return -1;
        int r = load_dir(n, buf, len, depth + 1);
        kfree(buf);
        return r;
    }
    if (size && c >= 2) {
        u8 *buf = read_chain(c, size, &len);
        if (!buf) return -1;
        if (len < size) size = len;                 /* damaged chain: keep what exists */
        fs_write(n, buf, size, 0);
        kfree(buf);
    }
    return 0;
}

static int load_dir(struct fs_node *dir, const u8 *ents, u32 bytes, int depth)
{
    char lfn[256], name[13];
    int have_lfn = 0;
    u8 sum = 0;

    for (const u8 *e = ents; e + 32 <= ents + bytes && e[0]; e += 32) {
        if (e[0] == 0xE5) { have_lfn = 0; continue; }           /* deleted */
        if (e[11] == ATTR_LFN) {                                /* part of a long name */
            int seq = e[0] & 0x1F;
            if (seq < 1 || seq > 19) { have_lfn = 0; continue; }
            if (e[0] & 0x40) { memset(lfn, 0, sizeof(lfn)); have_lfn = 1; sum = e[13]; }
            for (int i = 0; i < 13; i++) {
                u16 ch = rd16(e + lfn_pos[i]);
                lfn[(seq - 1) * 13 + i] = ch == 0xFFFF ? 0 : ch < 128 ? (char)ch : '_';
            }
            continue;
        }
        int use_lfn = have_lfn && sum == lfn_checksum(e) && lfn[0] && strlen(lfn) < VX_NAME_MAX;
        have_lfn = 0;
        if (e[11] & ATTR_VOLUME) continue;
        short_name(e, name);
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (load_entry(dir, e, use_lfn ? lfn : name, depth)) return -1;
    }
    return 0;
}

/* ---- writing ----------------------------------------------------------- */

static u32 next_free;                   /* clusters are handed out sequentially */
static u16 fat_date, fat_time;

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int short_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("_-~!#$%&@^", c));
}

/* Fill the 11-byte short name; returns the case flags, or -1 if the name
 * does not fit the 8.3 scheme and needs long-name entries. */
static int name_83(const char *name, u8 *out)
{
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name), ext = dot ? strlen(dot + 1) : 0;
    int lower[2] = { 0, 0 }, caps[2] = { 0, 0 };

    memset(out, ' ', 11);
    if (!base || base > 8 || ext > 3 || (dot && !ext)) return -1;
    for (size_t i = 0; i < base + ext; i++) {
        int part = i >= base;
        char c = part ? dot[1 + i - base] : name[i];
        if (!short_char(upper(c))) return -1;
        if (c >= 'a' && c <= 'z') lower[part] = 1;
        if (c >= 'A' && c <= 'Z') caps[part] = 1;
        out[part ? 8 + i - base : i] = (u8)upper(c);
    }
    if ((lower[0] && caps[0]) || (lower[1] && caps[1])) return -1;     /* mixed case */
    return (lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0);
}

/* Short alias for a long name: "NAME~12.EXT", unique by its index. */
static void alias_83(const char *name, u32 index, u8 *out)
{
    char num[8] = "~";
    const char *dot = strrchr(name, '.');
    int n = 0, digits = 1;

    memset(out, ' ', 11);
    index %= 100000;
    for (u32 x = index; x >= 10; x /= 10) digits++;
    for (int i = digits; i >= 1; i--, index /= 10) num[i] = (char)('0' + index % 10);
    num[digits + 1] = 0;
    int keep = 8 - (int)strlen(num);
    for (const char *p = name; *p && p != dot && n < keep; p++)
        if (short_char(upper(*p))) out[n++] = (u8)upper(*p);
    memcpy(out + n, num, strlen(num));
    n = 8;
    for (const char *p = dot ? dot + 1 : ""; *p && n < 11; p++)
        if (short_char(upper(*p))) out[n++] = (u8)upper(*p);
}

static u32 entries_for(const struct fs_node *n)
{
    u8 tmp[11];
    return name_83(n->name, tmp) >= 0 ? 1 : 1 + ((u32)strlen(n->name) + 12) / 13;
}

static u32 dir_entries(const struct fs_node *dir)
{
    u32 total = dir == mount ? 1 : 2;               /* volume label, or "." and ".." */
    for (const struct fs_node *n = dir->children; n; n = n->next) total += entries_for(n);
    return total;
}

/* Allocate a chain of `count` clusters; returns the first one (0 = full). */
static u32 alloc_chain(u32 count)
{
    u32 first = next_free;
    if (!count || next_free + count > v.clusters + 2) return 0;
    for (u32 i = 0; i < count; i++, next_free++)
        fat[next_free] = (u16)(i + 1 < count ? next_free + 1 : 0xFFFF);
    return first;
}

static int write_bytes(u32 lba, const u8 *data, u32 len)
{
    u8 sec[SECTOR];
    for (u32 pos = 0; pos < len; pos += SECTOR, lba++) {
        u32 n = len - pos < SECTOR ? len - pos : SECTOR;
        memset(sec, 0, SECTOR);
        memcpy(sec, data + pos, n);
        if (ata_write(lba, sec)) return -1;
    }
    return 0;
}

static u8 *put_entry(u8 *e, const u8 *name11, int flags, int attr, u32 cluster, u32 size)
{
    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = (u8)attr;
    e[12] = (u8)flags;
    wr16(e + 14, fat_time); wr16(e + 16, fat_date); wr16(e + 18, fat_date);
    wr16(e + 22, fat_time); wr16(e + 24, fat_date);
    wr16(e + 26, cluster);
    wr32(e + 28, size);
    return e + 32;
}

static u8 *put_long_name(u8 *e, const char *name, const u8 *name11)
{
    int len = (int)strlen(name), parts = (len + 12) / 13;
    u8 sum = lfn_checksum(name11);

    for (int seq = parts; seq >= 1; seq--, e += 32) {
        memset(e, 0, 32);
        e[0] = (u8)(seq | (seq == parts ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            int k = (seq - 1) * 13 + i;
            wr16(e + lfn_pos[i], k < len ? (u8)name[k] : k == len ? 0 : 0xFFFF);
        }
    }
    return e;
}

/* Write directory `dir` (and everything below it) whose own entries go to
 * the already allocated `self` cluster chain (0 = the fixed root area). */
static int write_dir(const struct fs_node *dir, u32 self, u32 parent)
{
    static const u8 dot[11] = ".          ", dotdot[11] = "..         ", label[11] = "VORTEXOS   ";
    u32 bytes = dir_entries(dir) * 32, index = 1;
    u32 room = self ? (bytes + cluster_bytes() - 1) / cluster_bytes() * cluster_bytes()
                    : v.root_entries * 32;
    if (bytes > room) return -1;

    u8 *buf = kmalloc(room), *e = buf;
    if (!buf) return -1;
    memset(buf, 0, room);
    if (self) {
        e = put_entry(e, dot, 0, ATTR_DIR, self, 0);
        e = put_entry(e, dotdot, 0, ATTR_DIR, parent, 0);
    } else {
        e = put_entry(e, label, 0, ATTR_VOLUME, 0, 0);
    }

    int result = 0;
    for (const struct fs_node *n = dir->children; n && !result; n = n->next, index++) {
        u8 name11[11];
        u32 cluster = 0;
        int flags = name_83(n->name, name11);

        if (n->is_dir) {
            u32 need = (dir_entries(n) * 32 + cluster_bytes() - 1) / cluster_bytes();
            cluster = alloc_chain(need);
            if (!cluster || write_dir(n, cluster, self)) result = -1;
        } else if (n->size) {
            cluster = alloc_chain((n->size + cluster_bytes() - 1) / cluster_bytes());
            if (!cluster || write_bytes(cluster_lba(cluster), n->data, n->size)) result = -1;
        }
        if (flags < 0) {
            alias_83(n->name, index, name11);
            e = put_long_name(e, n->name, name11);
            flags = 0;
        }
        e = put_entry(e, name11, flags, n->is_dir ? ATTR_DIR : 0x20, cluster, n->is_dir ? 0 : n->size);
    }
    if (!result) result = write_bytes(self ? cluster_lba(self) : v.root_start, buf, room);
    kfree(buf);
    return result;
}

static u32 clusters_needed(const struct fs_node *dir)
{
    u32 total = 0;
    for (const struct fs_node *n = dir->children; n; n = n->next) {
        if (n->is_dir)
            total += (dir_entries(n) * 32 + cluster_bytes() - 1) / cluster_bytes() + clusters_needed(n);
        else
            total += (n->size + cluster_bytes() - 1) / cluster_bytes();
    }
    return total;
}

/* Write the whole /disk tree to the volume.
 * Returns 0 on success, -1 on I/O errors, -2 if the tree does not fit. */
int fat_sync(void)
{
    struct vx_time now;
    u8 sec[SECTOR];

    if (!mount) return -1;
    dirty = 0;
    dirty_since = timer_ticks();
    if (clusters_needed(mount) > v.clusters || dir_entries(mount) > v.root_entries) {
        if (!full) kputs("\ndisk: no space left on /disk, changes are NOT saved\n");
        full = 1;
        return -2;                                  /* nothing was written */
    }
    full = 0;
    rtc_read(&now);
    fat_date = (u16)(((now.year - 1980) << 9) | (now.month << 5) | now.day);
    fat_time = (u16)((now.hour << 11) | (now.minute << 5) | (now.second / 2));

    memset(fat, 0, (v.clusters + 2) * sizeof(u16));
    fat[0] = 0xFFF8;                                /* media descriptor */
    fat[1] = 0xFFFF;
    next_free = 2;
    int result = write_dir(mount, 0, 0);

    /* The allocation table goes last: until then the old one stays valid. */
    for (u32 s = 0; s < v.fat_sectors && !result; s++) {
        for (u32 i = 0; i < SECTOR / 2; i++) {
            u32 c = s * (SECTOR / 2) + i;
            wr16(sec + i * 2, c < v.clusters + 2 ? fat[c] : 0);
        }
        for (u32 copy = 0; copy < v.fats && !result; copy++)
            result = ata_write(v.reserved + copy * v.fat_sectors + s, sec);
    }
    if (!result) result = ata_flush();
    if (result) dirty = 1;                          /* keep trying (e.g. after `rm`) */
    else sync_count++;
    dirty_since = timer_ticks();
    return result;
}

/* ---- mounting ---------------------------------------------------------- */

void fat_changed(const struct fs_node *n)
{
    if (!mount) return;
    for (; n; n = n->parent)
        if (n == mount) { dirty = 1; dirty_since = timer_ticks(); return; }
}

int fat_is_mount(const struct fs_node *n) { return mount && n == mount; }

void fat_info(u32 *total_kb, u32 *used_kb, u32 *syncs, int *pending)
{
    *total_kb = mount ? v.clusters * cluster_bytes() / 1024 : 0;
    *used_kb = mount ? (next_free - 2) * cluster_bytes() / 1024 : 0;
    *syncs = sync_count;
    *pending = full ? 2 : dirty;
}

/* Kernel thread: writes changes to the disk once things have been quiet. */
static void diskd(void *arg)
{
    (void)arg;
    for (;;) {
        if (dirty && timer_ticks() - dirty_since >= SYNC_DELAY) fat_sync();
        sleep_ms(100);
    }
}

/* Returns the number of KiB on the volume, 0 if there is no FAT16 disk. */
u32 fat_mount(void)
{
    u8 b[SECTOR];

    if (!ata_init() || ata_read(0, b)) return 0;
    if (b[510] != 0x55 || b[511] != 0xAA || rd16(b + 11) != SECTOR) return 0;
    v.spc = b[13];
    v.reserved = rd16(b + 14);
    v.fats = b[16];
    v.root_entries = rd16(b + 17);
    v.total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    v.fat_sectors = rd16(b + 22);
    if (!v.spc || !v.reserved || !v.fats || !v.root_entries || !v.fat_sectors) return 0;
    if (v.total > ata_sectors()) return 0;
    v.root_start = v.reserved + v.fats * v.fat_sectors;
    v.root_sectors = (v.root_entries * 32 + SECTOR - 1) / SECTOR;
    v.data_start = v.root_start + v.root_sectors;
    if (v.total <= v.data_start) return 0;
    v.clusters = (v.total - v.data_start) / v.spc;
    if (v.clusters < 4085 || v.clusters >= 65525) return 0;            /* FAT12 / FAT32 */
    if (v.fat_sectors * (SECTOR / 2) < v.clusters + 2) return 0;

    u32 len = v.root_sectors * SECTOR;
    u8 *rootdir = kmalloc(len);
    if (!rootdir || read_fat()) return 0;
    for (u32 s = 0; s < v.root_sectors; s++)
        if (ata_read(v.root_start + s, rootdir + s * SECTOR)) return 0;

    struct fs_node *dir = fs_create("/disk", 1);
    if (!dir) return 0;
    int failed = load_dir(dir, rootdir, len, 0);
    kfree(rootdir);
    if (failed) kputs("disk: read errors, some files are missing\n");

    next_free = 2;                                  /* for `disk`: clusters in use */
    for (u32 c = 2; c < v.clusters + 2; c++) if (fat[c]) next_free++;
    mount = dir;                                    /* from now on changes are tracked */
    struct task *t = task_create("diskd", diskd, NULL);
    if (t) t->system = 1;
    return v.clusters * cluster_bytes() / 1024;
}
