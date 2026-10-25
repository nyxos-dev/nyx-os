#ifndef ATA_H
#define ATA_H

#define ATA_PRIMARY_DATA    0x1F0
#define ATA_PRIMARY_ERR     0x1F1
#define ATA_PRIMARY_SECCNT  0x1F2
#define ATA_PRIMARY_LBA_LO  0x1F3
#define ATA_PRIMARY_LBA_MI  0x1F4
#define ATA_PRIMARY_LBA_HI  0x1F5
#define ATA_PRIMARY_DRIVE   0x1F6
#define ATA_PRIMARY_CMD     0x1F7
#define ATA_PRIMARY_CTRL    0x3F6

#define ATA_CMD_READ        0x20
#define ATA_CMD_WRITE       0x30
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_BSY  0x80

int ata_init(void);
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buf);
int ata_identify(uint8_t drive, uint16_t* buf);
int ata_flush(void);   // flush the drive write cache (batch once per operation)

// True iff (lba, count) is a valid 28-bit-LBA PIO request: LBA fits 28 bits and count is 1..255
// (an on-the-wire count of 0 means 256 sectors, which the one-sector-at-a-time PIO loop cannot
// service). The read/write paths reject anything else up front. Pure; pinned by the `ata` KAT.
int ata_lba28_valid(uint32_t lba, uint8_t count);
int ata_lba28_selftest(void);

#endif
