/* ATA (IDE) disk driver: primary master, PIO mode, LBA28, polling. */
#include "kernel.h"

#define ATA_DATA    0x1F0
#define ATA_COUNT   0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7               /* write: command, read: status */
#define ATA_CTRL    0x3F6

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

static u32 sectors;                     /* 0 = no disk */

/* Wait until the drive is no longer busy; with `drq`, until it wants data. */
static int wait(int drq)
{
    for (int i = 0; i < 4; i++) inb(ATA_CTRL);          /* 400 ns settle delay */
    for (u32 i = 0; i < 10000000; i++) {
        u8 st = inb(ATA_CMD);
        if (st & ST_BSY) continue;
        if (st & (ST_ERR | ST_DF)) return -1;
        if (!drq || (st & ST_DRQ)) return 0;
    }
    return -1;
}

static int command(u32 lba, u8 cmd)
{
    if (wait(0)) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));       /* master, LBA mode */
    outb(ATA_COUNT, 1);
    outb(ATA_LBA0, (u8)lba);
    outb(ATA_LBA1, (u8)(lba >> 8));
    outb(ATA_LBA2, (u8)(lba >> 16));
    outb(ATA_CMD, cmd);
    return 0;
}

u32 ata_init(void)
{
    u16 id[256];

    sectors = 0;
    outb(ATA_CTRL, 0x02);                               /* no interrupts: we poll */
    outb(ATA_DRIVE, 0xA0);
    u8 st = inb(ATA_CMD);
    if (st == 0xFF || st == 0x00) return 0;             /* nothing on the bus */

    outb(ATA_COUNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                                /* IDENTIFY DEVICE */
    if (inb(ATA_CMD) == 0) return 0;
    if (wait(1)) return 0;                              /* error: ATAPI or absent */
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    if (!(id[49] & 0x200)) return 0;                    /* LBA not supported */
    sectors = id[60] | ((u32)id[61] << 16);
    return sectors;
}

u32 ata_sectors(void) { return sectors; }

int ata_read(u32 lba, void *buf)
{
    u16 *p = buf;
    if (lba >= sectors || command(lba, 0x20) || wait(1)) return -1;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);
    return 0;
}

int ata_write(u32 lba, const void *buf)
{
    const u16 *p = buf;
    if (lba >= sectors || command(lba, 0x30) || wait(1)) return -1;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);
    return wait(0);
}

int ata_flush(void)
{
    if (!sectors || wait(0)) return -1;
    outb(ATA_DRIVE, 0xE0);
    outb(ATA_CMD, 0xE7);                                /* FLUSH CACHE */
    return wait(0);
}
