#include "../../core/kernel.h"
#include "ata.h"

// A 28-bit-LBA PIO request is valid iff the LBA fits 28 bits (bits 24-27 ride in the drive/head
// byte) and the sector count is 1..255. A wire count of 0 means "256 sectors", which this
// one-sector-at-a-time PIO loop cannot service; a too-large LBA would have its high bits masked
// and silently alias to a WRONG sector. Reject both up front so callers get a clean -1 instead
// of corrupt/desynced I/O (the on-disk block layer already clamps, but direct callers exist).
int ata_lba28_valid(uint32_t lba, uint8_t count) {
    return count != 0 && lba <= 0x0FFFFFFFu;
}

static int ata_busy_wait(uint16_t ctrl, int timeout_ms) {
    uint16_t cmd_port = ctrl - ATA_PRIMARY_CTRL + ATA_PRIMARY_CMD;
    while (timeout_ms--) {
        if (!(inb(cmd_port) & ATA_STATUS_BSY))
            return 0;
        for (volatile int i = 0; i < 1000; i++);
    }
    return -1;
}

static int ata_drq_wait(uint16_t data_port) {
    int timeout = 30000;
    while (timeout--) {
        uint8_t st = inb(data_port + ATA_PRIMARY_CMD - ATA_PRIMARY_DATA);
        if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) return -1;
        if (st & ATA_STATUS_DRQ) return 0;
        if (!(st & ATA_STATUS_BSY)) return -1;
    }
    return -1;
}

int ata_init(void) {
    outb(ATA_PRIMARY_CTRL, 0x04);
    for (volatile int i = 0; i < 20000; i++);
    return 0;
}

int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buf) {
    if (!ata_lba28_valid(lba, count))
        return -1;
    if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
        return -1;

    uint8_t drv = 0xE0 | (drive << 4) | ((lba >> 24) & 0x0F);
    outb(ATA_PRIMARY_DRIVE, drv);
    outb(ATA_PRIMARY_SECCNT, count);
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba));
    outb(ATA_PRIMARY_LBA_MI, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_CMD, ATA_CMD_READ);

    uint16_t* ptr = (uint16_t*)buf;
    for (int s = 0; s < count; s++) {
        if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
            return -1;
        if (ata_drq_wait(ATA_PRIMARY_DATA) < 0)
            return -1;
        for (int i = 0; i < 256; i++)
            ptr[i] = inw(ATA_PRIMARY_DATA);
        ptr += 256;
    }
    return count;
}

int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buf) {
    if (!ata_lba28_valid(lba, count))
        return -1;
    if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
        return -1;

    uint8_t drv = 0xE0 | (drive << 4) | ((lba >> 24) & 0x0F);
    outb(ATA_PRIMARY_DRIVE, drv);
    outb(ATA_PRIMARY_SECCNT, count);
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba));
    outb(ATA_PRIMARY_LBA_MI, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_CMD, ATA_CMD_WRITE);

    const uint16_t* ptr = (const uint16_t*)buf;
    for (int s = 0; s < count; s++) {
        if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
            return -1;
        if (ata_drq_wait(ATA_PRIMARY_DATA) < 0)
            return -1;
        for (int i = 0; i < 256; i++)
            outw(ATA_PRIMARY_DATA, ptr[i]);
        ptr += 256;
    }

    // Wait for command completion
    if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
        return -1;
    if (inb(ATA_PRIMARY_CMD) & (ATA_STATUS_ERR | ATA_STATUS_DF))
        return -1;

    return count;
}

// Flush the drive's write cache. Callers batch this ONCE at the end of an
// operation instead of paying a full FLUSH CACHE (a synchronous BSY wait) after
// every sector — that per-write flush made a 4 MB write ~8000 flushes / minutes;
// batching drops it to a handful. Data stays coherent within the session (the
// drive serves reads from its own cache); the flush only forces it to the medium.
int ata_flush(void) {
    outb(ATA_PRIMARY_CMD, ATA_CMD_CACHE_FLUSH);
    if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0) return -1;
    if (inb(ATA_PRIMARY_CMD) & (ATA_STATUS_ERR | ATA_STATUS_DF)) return -1;
    return 0;
}

int ata_identify(uint8_t drive, uint16_t* buf) {
    if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
        return -1;

    uint8_t drv = 0xE0 | (drive << 4);
    outb(ATA_PRIMARY_DRIVE, drv);
    outb(ATA_PRIMARY_SECCNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MI, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);

    uint8_t st = inb(ATA_PRIMARY_CMD);
    if (st == 0) return -1;

    if (ata_busy_wait(ATA_PRIMARY_CTRL, 30000) < 0)
        return -1;

    for (int i = 0; i < 256; i++)
        buf[i] = inw(ATA_PRIMARY_DATA);
    return 0;
}

// KAT (`ata`): pins the LBA28 request validation that guards ata_read/write_sectors — the
// count==0 ("256 sectors" on the wire) and >28-bit-LBA cases the PIO path must reject, plus the
// valid boundaries it must accept. Pure, so it runs with no disk attached.
int ata_lba28_selftest(void) {
    if (!ata_lba28_valid(0, 1))          return 1;   // smallest valid request
    if (!ata_lba28_valid(5, 128))        return 2;   // a typical multi-sector burst
    if (!ata_lba28_valid(0x0FFFFFFF, 1)) return 3;   // the largest LBA 28 bits can hold
    if (!ata_lba28_valid(0x0FFFFFFF, 255)) return 4; // max LBA + max count
    if (ata_lba28_valid(0, 0))           return 5;   // count 0 = 256 sectors -> rejected
    if (ata_lba28_valid(0x10000000, 1))  return 6;   // first LBA that overflows 28 bits
    if (ata_lba28_valid(0xFFFFFFFF, 200)) return 7;  // far past the 28-bit range
    return 0;
}
