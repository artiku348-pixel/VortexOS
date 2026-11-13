/* FAT12 / FAT16 / FAT32 filesystem on the ATA disk, mounted at /disk.
 *
 * Supported: all three FAT variants (chosen by the cluster count, as the
 * specification demands), an optional MBR partition table, long file names
 * (VFAT) for reading and writing, lower-case 8.3 names, growing directories,
 * the FAT32 FSInfo sector, all FAT copies kept identical.
 *
 * How it fits into the file tree (fs.c):
 *  - Directories are read when they are first visited, file contents when
 *    they are first opened (fat_load).
 *  - Creating and deleting changes the directory entries and the allocation
 *    table right away, through a small write-back sector cache.
 *  - File data is kept in memory once opened; a changed file is marked dirty
 *    and written to its cluster chain by fat_sync(), which the "diskd" kernel
 *    thread calls when the volume has been quiet for a moment. */
#include "kernel.h"

#define SECTOR      512
#define ATTR_DIR    0x10
#define ATTR_VOLUME 0x08
#define ATTR_LFN    0x0F
#define SYNC_DELAY  (TIMER_HZ / 2)      /* quiet time before an automatic sync */
#define CACHE_SLOTS 256                 /* sectors of metadata kept in memory */
#define LFN_MAX     255

struct volume {
    int type;                           /* 12, 16 or 32 */
    u32 base;                           /* first sector of the volume on the disk */
    u32 spc, fats, fat_start, fat_sectors;
    u32 root_start, root_sectors;       /* FAT12/16: fixed root directory area */
    u32 root_cluster;                   /* FAT32: the root is a normal chain */
    u32 data_start, clusters, mask;
    u32 fsinfo;                         /* FAT32: FSInfo sector, 0 = none */
    u32 free, hint;                     /* free clusters, where to look next */
};

static struct volume v;
static struct fs_node *mount;           /* the /disk directory, NULL if not mounted */
static volatile int dirty;              /* something waits to be written */
static u32 dirty_since, sync_count;
static int full;                        /* a file did not fit on the volume */
static int io_error;                    /* sticky until the next sync */

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return rd16(p) | (u32)rd16(p + 2) << 16; }
static void wr16(u8 *p, u32 x) { p[0] = (u8)x; p[1] = (u8)(x >> 8); }
static void wr32(u8 *p, u32 x) { wr16(p, x); wr16(p + 2, x >> 16); }

static u32 cluster_lba(u32 c) { return v.data_start + (c - 2) * v.spc; }
static u32 cluster_bytes(void) { return v.spc * SECTOR; }

static void touch(void) { dirty = 1; dirty_since = timer_ticks(); }

/* ---- sector cache (metadata only: FAT, directories, FSInfo) ------------ */

struct buf { u32 lba; u8 valid, dirty; u8 data[SECTOR]; };
static struct buf *cache;

static int buf_write(struct buf *b)
{
    if (b->valid && b->dirty) {
        if (ata_write(v.base + b->lba, b->data)) { io_error = 1; return -1; }
        b->dirty = 0;
    }
    return 0;
}

/* Returns the cached sector; with `fresh` it is zero-filled instead of read.
 * The pointer is valid only until the next call. */
static u8 *bget(u32 lba, int fresh)
{
    static u8 scratch[SECTOR];          /* handed out after I/O errors */
    struct buf *b = &cache[lba % CACHE_SLOTS];

    if (!b->valid || b->lba != lba) {
        if (buf_write(b)) { memset(scratch, 0, SECTOR); return scratch; }
        b->valid = 0;
        if (fresh) memset(b->data, 0, SECTOR);
        else if (ata_read(v.base + lba, b->data)) {
            io_error = 1;
            memset(scratch, 0, SECTOR);
            return scratch;
        }
        b->lba = lba;
        b->valid = 1;
    } else if (fresh) {
        memset(b->data, 0, SECTOR);
    }
    if (fresh) { b->dirty = 1; touch(); }
    return b->data;
}

static u8 *bread(u32 lba) { return bget(lba, 0); }

static void bdirty(u32 lba)
{
    struct buf *b = &cache[lba % CACHE_SLOTS];
    if (b->valid && b->lba == lba) { b->dirty = 1; touch(); }
}

static int bflush(void)
{
    int r = 0;
    for (int i = 0; i < CACHE_SLOTS; i++) r |= buf_write(&cache[i]);
    return r;
}

/* File data bypasses the cache; forget a cached copy of that sector. */
static void bdrop(u32 lba)
{
    struct buf *b = &cache[lba % CACHE_SLOTS];
    if (b->valid && b->lba == lba) b->valid = 0;
}

/* ---- allocation table --------------------------------------------------- */

static u32 fat_bytes(u32 copy, u32 off, int n)
{
    u32 x = 0;
    for (int i = 0; i < n; i++, off++)
        x |= (u32)bread(v.fat_start + copy * v.fat_sectors + off / SECTOR)[off % SECTOR] << (8 * i);
    return x;
}

static void fat_put_bytes(u32 copy, u32 off, int n, u32 x)
{
    for (int i = 0; i < n; i++, off++, x >>= 8) {
        u32 lba = v.fat_start + copy * v.fat_sectors + off / SECTOR;
        bread(lba)[off % SECTOR] = (u8)x;
        bdirty(lba);
    }
}

static u32 fat_get(u32 c)
{
    if (v.type == 12) {
        u32 x = fat_bytes(0, c + c / 2, 2);
        return c & 1 ? x >> 4 : x & 0xFFF;
    }
    if (v.type == 16) return fat_bytes(0, c * 2, 2);
    return fat_bytes(0, c * 4, 4) & 0x0FFFFFFF;
}

static void fat_set(u32 c, u32 val)
{
    for (u32 copy = 0; copy < v.fats; copy++) {
        if (v.type == 12) {
            u32 old = fat_bytes(copy, c + c / 2, 2);
            u32 x = c & 1 ? (old & 0x000F) | (val << 4) : (old & 0xF000) | (val & 0xFFF);
            fat_put_bytes(copy, c + c / 2, 2, x);
        } else if (v.type == 16) {
            fat_put_bytes(copy, c * 2, 2, val);
        } else {                        /* the top four bits are reserved: keep them */
            u32 old = fat_bytes(copy, c * 4, 4);
            fat_put_bytes(copy, c * 4, 4, (old & 0xF0000000) | (val & 0x0FFFFFFF));
        }
    }
}

static int valid_cluster(u32 c) { return c >= 2 && c < v.clusters + 2; }

/* The cluster after `c` in its chain, 0 at the end (or on a damaged chain). */
static u32 chain_next(u32 c)
{
    u32 n = fat_get(c);
    return valid_cluster(n) ? n : 0;
}

static u32 chain_length(u32 c)
{
    u32 n = 0;
    for (; valid_cluster(c) && n <= v.clusters; c = chain_next(c)) n++;
    return n;
}

/* Take a free cluster and mark it as the end of a chain; 0 = volume full. */
static u32 alloc_cluster(void)
{
    if (!v.free) return 0;
    for (u32 i = 0; i < v.clusters; i++) {
        u32 c = 2 + (v.hint - 2 + i) % v.clusters;
        if (fat_get(c) == 0) {
            fat_set(c, v.mask);
            v.hint = c + 1 < v.clusters + 2 ? c + 1 : 2;
            v.free--;
            return c;
        }
    }
    v.free = 0;
    return 0;
}

static void free_chain(u32 c)
{
    for (u32 n = 0; valid_cluster(c) && n <= v.clusters; n++) {
        u32 next = chain_next(c);
        fat_set(c, 0);
        v.free++;
        if (c < v.hint) v.hint = c;     /* reuse space from the front */
        c = next;
    }
}

static void zero_cluster(u32 c)
{
    for (u32 s = 0; s < v.spc; s++) bget(cluster_lba(c) + s, 1);
}

/* ---- walking a directory ------------------------------------------------ */

/* Position of one 32-byte entry. `cluster` 0 = the fixed FAT12/16 root. */
struct dpos { u32 cluster, index, lba, off; };

static void dir_start(struct dpos *p, u32 cluster)
{
    p->cluster = cluster;
    p->index = 0;
    p->off = 0;
    p->lba = cluster ? cluster_lba(cluster) : v.root_start;
}

/* Step to the next entry. At the end of the directory: -1, or with `grow`
 * a new zeroed cluster is appended (impossible for the fixed root). */
static int dir_next(struct dpos *p, int grow)
{
    p->off += 32;
    if (p->off < SECTOR) return 0;
    p->off = 0;
    p->index++;
    if (!p->cluster) {
        if (p->index >= v.root_sectors) return -1;
        p->lba = v.root_start + p->index;
        return 0;
    }
    if (p->index < v.spc) { p->lba++; return 0; }

    u32 next = chain_next(p->cluster);
    if (!next) {
        if (!grow || !(next = alloc_cluster())) return -1;
        fat_set(p->cluster, next);
        zero_cluster(next);
    }
    p->cluster = next;
    p->index = 0;
    p->lba = cluster_lba(next);
    return 0;
}

static u32 dir_cluster(const struct fs_node *dir)
{
    return dir == mount ? v.root_cluster : dir->fat_cluster;
}

/* ---- names --------------------------------------------------------------- */

static const u8 lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int short_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("_-~!#$%&@^'(){}`", c));
}

/* Characters allowed in long names (ASCII subset). */
static int long_char(int c)
{
    return c >= 32 && c < 127 && !strchr("\"*/:<>?\\|", c);
}

static u8 lfn_checksum(const u8 *name11)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

/* Turn the 8.3 name of a directory entry into "name.ext". */
static void short_name(const u8 *e, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        int c = i == 0 && e[0] == 0x05 ? 0xE5 : e[i];
        if ((e[12] & 0x08) && c >= 'A' && c <= 'Z') c += 32;    /* lower-case base */
        out[n++] = c >= 32 && c < 127 ? (char)c : '_';
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            int c = e[i];
            if ((e[12] & 0x10) && c >= 'A' && c <= 'Z') c += 32; /* lower-case extension */
            out[n++] = c >= 32 && c < 127 ? (char)c : '_';
        }
    }
    out[n] = 0;
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

/* Short alias for a long name: "NAME~12.EXT". */
static void alias_83(const char *name, u32 number, u8 *out)
{
    char num[8] = "~";
    const char *dot = strrchr(name, '.');
    int n = 0, digits = 1;

    if (dot == name) dot = NULL;
    memset(out, ' ', 11);
    for (u32 x = number; x >= 10; x /= 10) digits++;
    for (int i = digits; i >= 1; i--, number /= 10) num[i] = (char)('0' + number % 10);
    num[digits + 1] = 0;
    int keep = 8 - (int)strlen(num);
    for (const char *p = name; *p && p != dot && n < keep; p++)
        if (short_char(upper(*p))) out[n++] = (u8)upper(*p);
    memcpy(out + n, num, strlen(num));
    n = 8;
    for (const char *p = dot ? dot + 1 : ""; *p && n < 11; p++)
        if (short_char(upper(*p))) out[n++] = (u8)upper(*p);
}

/* ---- reading ------------------------------------------------------------- */

static int load_dir(struct fs_node *dir)
{
    char lfn[LFN_MAX + 6], name[13];
    struct dpos p;
    int have_lfn = 0, run = 0;          /* run: long-name entries just before */
    u8 sum = 0, e[32];

    dir_start(&p, dir_cluster(dir));
    for (u32 count = 0; count < 65536; count++) {
        memcpy(e, bread(p.lba) + p.off, 32);
        if (io_error) return -1;
        if (e[0] == 0) break;                                   /* end of directory */
        if (e[0] == 0xE5) {                                     /* deleted */
            have_lfn = run = 0;
        } else if (e[11] == ATTR_LFN) {                         /* part of a long name */
            int seq = e[0] & 0x1F;
            if (run < 255) run++;
            if (e[0] & 0x40) { memset(lfn, 0, sizeof(lfn)); have_lfn = 1; sum = e[13]; }
            if (seq < 1 || seq > 20) have_lfn = 0;
            for (int i = 0; have_lfn && i < 13; i++) {
                u16 ch = rd16(e + lfn_pos[i]);
                if (ch == 0xFFFF) ch = 0;
                if (ch >= 127) have_lfn = 0;                    /* not ASCII: use the alias */
                else if ((seq - 1) * 13 + i < LFN_MAX) lfn[(seq - 1) * 13 + i] = (char)ch;
            }
        } else {
            int use_lfn = have_lfn && sum == lfn_checksum(e) && lfn[0] && strlen(lfn) < VX_NAME_MAX;
            short_name(e, name);
            if (!(e[11] & ATTR_VOLUME) && strcmp(name, ".") && strcmp(name, "..")) {
                struct fs_node *n = fs_node_new(dir, use_lfn ? lfn : name, e[11] & ATTR_DIR);
                if (!n) return -1;
                n->fat_cluster = rd16(e + 26) | (v.type == 32 ? (u32)rd16(e + 20) << 16 : 0);
                n->fat_lba = p.lba;
                n->fat_off = (u16)p.off;
                n->fat_lfn = (u8)run;
                n->fat_flags = FAT_ON_DISK;
                if (!n->is_dir) n->size = rd32(e + 28);
                if (!valid_cluster(n->fat_cluster)) {           /* empty (or damaged) */
                    n->fat_cluster = 0;
                    n->size = 0;
                    n->fat_flags |= FAT_LOADED;
                }
            }
            have_lfn = run = 0;
        }
        if (dir_next(&p, 0)) break;
    }
    return 0;
}

static int load_file(struct fs_node *n)
{
    u32 have = chain_length(n->fat_cluster) * cluster_bytes();
    if (n->size > have) n->size = have;             /* damaged chain: keep what exists */
    u32 cap = (n->size + SECTOR - 1) / SECTOR * SECTOR;
    u8 *buf = kmalloc(cap ? cap : 1);
    if (!buf) return -1;

    u32 pos = 0;
    for (u32 c = n->fat_cluster; pos < cap; c = chain_next(c))
        for (u32 s = 0; s < v.spc && pos < cap; s++, pos += SECTOR)
            if (ata_read(v.base + cluster_lba(c) + s, buf + pos)) { kfree(buf); return -1; }
    kfree(n->data);
    n->data = buf;
    n->cap = cap;
    return 0;
}

/* Bring a directory's children or a file's contents into memory. */
int fat_load(struct fs_node *n)
{
    if (!mount || !(n->fat_flags & FAT_ON_DISK)) return -1;
    if (n->fat_flags & FAT_LOADED) return 0;
    if (n->is_dir ? load_dir(n) : load_file(n)) return -1;
    n->fat_flags |= FAT_LOADED;
    return 0;
}

/* ---- directory entries --------------------------------------------------- */

static u16 fat_date, fat_time;

static void stamp(void)
{
    struct vx_time now;
    rtc_read(&now);
    if (now.year < 1980) now.year = 1980;
    fat_date = (u16)(((now.year - 1980) << 9) | (now.month << 5) | now.day);
    fat_time = (u16)((now.hour << 11) | (now.minute << 5) | (now.second / 2));
}

static void put_entry(u8 *e, const u8 *name11, int flags, int attr, u32 cluster, u32 size)
{
    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = (u8)attr;
    e[12] = (u8)flags;
    wr16(e + 14, fat_time); wr16(e + 16, fat_date); wr16(e + 18, fat_date);
    wr16(e + 20, cluster >> 16);
    wr16(e + 22, fat_time); wr16(e + 24, fat_date);
    wr16(e + 26, cluster);
    wr32(e + 28, size);
}

static void put_lfn(u8 *e, const char *name, int seq, int last, u8 sum)
{
    int len = (int)strlen(name);
    memset(e, 0, 32);
    e[0] = (u8)(seq | (last ? 0x40 : 0));
    e[11] = ATTR_LFN;
    e[13] = sum;
    for (int i = 0; i < 13; i++) {
        int k = (seq - 1) * 13 + i;
        wr16(e + lfn_pos[i], k < len ? (u8)name[k] : k == len ? 0 : 0xFFFF);
    }
}

/* Is the short name `name11` already used in the directory? */
static int short_name_used(u32 cluster, const u8 *name11)
{
    struct dpos p;
    dir_start(&p, cluster);
    for (u32 count = 0; count < 65536; count++) {
        const u8 *e = bread(p.lba) + p.off;
        if (e[0] == 0) break;
        if (e[0] != 0xE5 && e[11] != ATTR_LFN && memcmp(e, name11, 11) == 0) return 1;
        if (dir_next(&p, 0)) break;
    }
    return 0;
}

/* Find `need` free entries in a row; the directory grows if it has to. */
static int find_slots(u32 cluster, u32 need, struct dpos *at)
{
    struct dpos p;
    u32 run = 0;
    int past_end = 0;

    dir_start(&p, cluster);
    for (u32 count = 0; count < 65536; count++) {
        u8 first = bread(p.lba)[p.off];
        if (io_error) return -1;
        if (first == 0) past_end = 1;               /* everything from here on is free */
        if (past_end || first == 0xE5) {
            if (!run) *at = p;
            if (++run == need) return 0;
        } else {
            run = 0;
        }
        if (dir_next(&p, 1)) return -1;             /* root directory or volume full */
    }
    return -1;
}

/* Create the directory entry (and, for a directory, its first cluster) for a
 * node that fs.c has just linked into its parent. Returns 0 on success. */
int fat_create(struct fs_node *n)
{
    struct fs_node *dir = n->parent;
    u8 name11[11], e[32];
    size_t len = strlen(n->name);
    u32 parent = dir_cluster(dir), cluster = 0;
    struct dpos p;

    if (!mount || io_error || (dir != mount && !dir->fat_cluster)) return -1;
    for (size_t i = 0; i < len; i++) if (!long_char(n->name[i])) return -1;
    if (n->name[0] == ' ' || n->name[len - 1] == ' ' || n->name[len - 1] == '.') return -1;

    int flags = name_83(n->name, name11), lfn = 0;
    if (flags >= 0) {
        if (short_name_used(parent, name11)) return -1;
    } else {
        u32 number = 1;
        do alias_83(n->name, number, name11);
        while (short_name_used(parent, name11) && ++number < 100000);
        if (number == 100000) return -1;
        lfn = (int)(len + 12) / 13;
        flags = 0;
    }

    if (find_slots(parent, (u32)lfn + 1, &p)) return -1;
    stamp();
    if (n->is_dir) {
        static const u8 dot[11] = ".          ", dotdot[11] = "..         ";
        if (!(cluster = alloc_cluster())) return -1;
        zero_cluster(cluster);
        u8 *s = bread(cluster_lba(cluster));
        put_entry(s, dot, 0, ATTR_DIR, cluster, 0);
        put_entry(s + 32, dotdot, 0, ATTR_DIR, dir == mount ? 0 : parent, 0);
        bdirty(cluster_lba(cluster));
    }

    u8 sum = lfn_checksum(name11);
    for (int seq = lfn; seq >= 0; seq--) {
        if (seq) put_lfn(e, n->name, seq, seq == lfn, sum);
        else put_entry(e, name11, flags, n->is_dir ? ATTR_DIR : 0x20, cluster, 0);
        memcpy(bread(p.lba) + p.off, e, 32);
        bdirty(p.lba);
        if (seq && dir_next(&p, 0)) return -1;      /* cannot happen: slots were found */
    }
    n->fat_cluster = cluster;
    n->fat_lba = p.lba;
    n->fat_off = (u16)p.off;
    n->fat_lfn = (u8)lfn;
    n->fat_flags = FAT_ON_DISK | FAT_LOADED;
    return io_error ? -1 : 0;
}

/* Delete the directory entries of a file or an empty directory and free its
 * clusters. fs.c drops the node afterwards. */
int fat_remove(struct fs_node *n)
{
    struct dpos p;
    u32 parent = dir_cluster(n->parent), index = 0;
    int found = 0;

    if (!mount || n == mount || io_error) return -1;
    dir_start(&p, parent);
    for (; index < 65536; index++) {                /* which entry number is it? */
        if (p.lba == n->fat_lba && p.off == n->fat_off) { found = 1; break; }
        if (dir_next(&p, 0)) break;
    }
    if (!found) return -1;

    u32 first = index >= n->fat_lfn ? index - n->fat_lfn : 0;
    dir_start(&p, parent);
    for (u32 i = 0; i <= index; i++) {
        if (i >= first) {
            u8 *e = bread(p.lba) + p.off;
            if (i == index || e[11] == ATTR_LFN) { e[0] = 0xE5; bdirty(p.lba); }
        }
        if (i < index && dir_next(&p, 0)) break;
    }
    free_chain(n->fat_cluster);
    n->fat_cluster = 0;
    n->fat_flags = 0;
    return io_error ? -1 : 0;
}

void fat_dirty(struct fs_node *n)
{
    n->fat_flags |= FAT_LOADED | FAT_DIRTY;
    touch();
}

/* ---- writing file data --------------------------------------------------- */

/* Make the file's cluster chain match its size and write the contents.
 * Returns 0, -1 (I/O error) or -2 (volume full: nothing was changed). */
static int store_file(struct fs_node *n)
{
    u8 sec[SECTOR];
    u32 need = (n->size + cluster_bytes() - 1) / cluster_bytes();
    u32 have = chain_length(n->fat_cluster);

    if (need > have && need - have > v.free) return -2;
    if (!need) {
        free_chain(n->fat_cluster);
        n->fat_cluster = 0;
    } else {
        if (!have && !(n->fat_cluster = alloc_cluster())) return -2;
        u32 c = n->fat_cluster;
        for (u32 i = 1; i < need; i++) {
            u32 next = chain_next(c);
            if (!next) {
                if (!(next = alloc_cluster())) return -2;
                fat_set(c, next);
            }
            c = next;
        }
        u32 rest = chain_next(c);                   /* the file became shorter */
        fat_set(c, v.mask);
        free_chain(rest);
    }

    u32 pos = 0;
    for (u32 c = n->fat_cluster; pos < n->size; c = chain_next(c)) {
        for (u32 s = 0; s < v.spc && pos < n->size; s++, pos += SECTOR) {
            const u8 *src = n->data + pos;
            if (n->size - pos < SECTOR) {           /* last, partial sector */
                memset(sec, 0, SECTOR);
                memcpy(sec, src, n->size - pos);
                src = sec;
            }
            bdrop(cluster_lba(c) + s);
            if (ata_write(v.base + cluster_lba(c) + s, src)) { io_error = 1; return -1; }
        }
    }

    u8 *e = bread(n->fat_lba) + n->fat_off;         /* update the directory entry */
    e[11] |= 0x20;                                  /* "archive": changed */
    wr16(e + 20, n->fat_cluster >> 16);
    wr16(e + 22, fat_time);
    wr16(e + 24, fat_date);
    wr16(e + 18, fat_date);
    wr16(e + 26, n->fat_cluster);
    wr32(e + 28, n->size);
    bdirty(n->fat_lba);
    n->fat_flags &= (u8)~FAT_DIRTY;
    return io_error ? -1 : 0;
}

static int store_tree(struct fs_node *dir)
{
    int result = 0;
    for (struct fs_node *n = dir->children; n; n = n->next) {
        int r = 0;
        if (n->is_dir) r = store_tree(n);
        else if (n->fat_flags & FAT_DIRTY) r = store_file(n);
        if (r == -1) return -1;
        if (r) result = r;                          /* keep going: others may still fit */
    }
    return result;
}

/* Write everything that is still only in memory. */
int fat_sync(void)
{
    if (!mount) return -1;
    dirty = 0;
    dirty_since = timer_ticks();
    io_error = 0;
    stamp();

    int result = store_tree(mount);
    if (result == -2 && !full) kputs("\ndisk: no space left on /disk, some files are NOT saved\n");
    full = result == -2;

    if (v.fsinfo && result != -1) {                 /* FAT32: free-space summary */
        u8 *s = bread(v.fsinfo);
        if (rd32(s) == 0x41615252 && rd32(s + 484) == 0x61417272 &&
            (rd32(s + 488) != v.free || rd32(s + 492) != v.hint)) {
            wr32(s + 488, v.free);
            wr32(s + 492, v.hint);
            bdirty(v.fsinfo);
        }
    }
    if (bflush() || ata_flush() || io_error) result = -1;
    dirty = 0;
    if (result == -1) { dirty = 1; kputs("\ndisk: write error\n"); }
    else sync_count++;
    return result;
}

/* ---- mounting ------------------------------------------------------------ */

int fat_is_mount(const struct fs_node *n) { return mount && n == mount; }

void fat_info(struct fat_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!mount) return;
    info->type = v.type;
    info->cluster_bytes = cluster_bytes();
    info->total_kb = v.clusters * v.spc / 2;
    info->free_kb = v.free * v.spc / 2;
    info->partition_lba = v.base;
    info->syncs = sync_count;
    info->pending = full ? 2 : dirty;
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

/* Does this sector start a FAT volume? Fills in `v` (except v.base). */
static int parse_boot_sector(const u8 *b, u32 max_sectors)
{
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;                 /* x86 jump */
    if (rd16(b + 11) != SECTOR) return 0;
    v.spc = b[13];
    v.fats = b[16];
    u32 reserved = rd16(b + 14), root_entries = rd16(b + 17);
    u32 total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    v.fat_sectors = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    if (!v.spc || (v.spc & (v.spc - 1)) || !reserved || !v.fats || v.fats > 4) return 0;
    if (!v.fat_sectors || !total || total > max_sectors) return 0;

    v.fat_start = reserved;
    v.root_start = reserved + v.fats * v.fat_sectors;
    v.root_sectors = (root_entries * 32 + SECTOR - 1) / SECTOR;
    v.data_start = v.root_start + v.root_sectors;
    if (total <= v.data_start) return 0;
    v.clusters = (total - v.data_start) / v.spc;

    /* The FAT type is defined by the number of clusters, nothing else. */
    v.type = v.clusters < 4085 ? 12 : v.clusters < 65525 ? 16 : 32;
    v.mask = v.type == 12 ? 0xFFF : v.type == 16 ? 0xFFFF : 0x0FFFFFFF;
    v.root_cluster = 0;
    v.fsinfo = 0;
    if (v.type == 32) {
        if (root_entries || rd16(b + 22)) return 0;
        v.root_cluster = rd32(b + 44);
        if (!valid_cluster(v.root_cluster)) return 0;
        if (rd16(b + 48) && rd16(b + 48) < reserved) v.fsinfo = rd16(b + 48);
    } else if (!root_entries) {
        return 0;
    }
    u32 per_sector = v.type == 16 ? SECTOR / 2 : SECTOR / 4;        /* FAT entries */
    u32 entries = v.type == 12 ? v.fat_sectors * SECTOR * 2 / 3 : v.fat_sectors * per_sector;
    if (entries < v.clusters + 2) return 0;
    return 1;
}

/* Returns the size of the volume in KiB, 0 if there is no FAT disk. */
u32 fat_mount(void)
{
    u8 b[SECTOR];
    u32 disk = ata_init();

    if (!disk || ata_read(0, b)) return 0;
    v.base = 0;
    if (!parse_boot_sector(b, disk)) {              /* no volume here: partition table? */
        u8 mbr[SECTOR];
        int found = 0;
        memcpy(mbr, b, SECTOR);
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
        for (int i = 0; i < 4 && !found; i++) {
            const u8 *part = mbr + 446 + i * 16;
            u8 type = part[4];
            u32 start = rd32(part + 8), size = rd32(part + 12);
            if (type != 0x01 && type != 0x04 && type != 0x06 && type != 0x0B &&
                type != 0x0C && type != 0x0E) continue;
            if (!start || start >= disk || size > disk - start || ata_read(start, b)) continue;
            if (parse_boot_sector(b, size)) { v.base = start; found = 1; }
        }
        if (!found) return 0;
    }

    cache = kmalloc(CACHE_SLOTS * sizeof(struct buf));
    if (!cache) return 0;
    memset(cache, 0, CACHE_SLOTS * sizeof(struct buf));

    io_error = 0;
    v.free = 0;
    v.hint = 2;
    for (u32 c = v.clusters + 1; c >= 2; c--)       /* count free space */
        if (fat_get(c) == 0) { v.free++; v.hint = c; }
    if (io_error) { kfree(cache); return 0; }

    struct fs_node *dir = fs_create("/disk", 1);
    if (!dir) return 0;
    dir->fat_flags = FAT_ON_DISK;                   /* its entries are read on first use */
    mount = dir;
    dirty = 0;
    struct task *t = task_create("diskd", diskd, NULL);
    if (t) t->system = 1;
    return v.clusters * v.spc / 2;
}
