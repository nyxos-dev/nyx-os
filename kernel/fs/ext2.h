#ifndef EXT2_H
#define EXT2_H

#define EXT2_SUPER_MAGIC   0xEF53
#define EXT2_ROOT_INO      2

#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

typedef struct {
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t blocks_su;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev;
    uint32_t last_check;
    uint32_t check_interval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    uint8_t  volume_name[16];
    uint8_t  last_mounted[64];
    uint32_t algo_bitmap;
    uint8_t  prealloc_blocks;
    uint8_t  prealloc_dir_blocks;
    uint16_t pad1;
    uint8_t  journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    uint32_t hash_seed[4];
    uint8_t  def_hash_version;
    uint8_t  pad2[3];
    uint32_t default_mount_options;
    uint32_t first_meta_bg;
    uint8_t  unused[760];
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t  reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks_512;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint32_t osd2[3];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed)) ext2_dirent_t;

typedef struct {
    uint8_t drive;
    uint32_t part_start_lba;
    ext2_superblock_t sb;
    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t bgd_blocks;
    uint32_t bgd_block;
    uint32_t inode_size;
    uint32_t block_groups;      // number of block groups; bounds every group index (v5.9.118)
} ext2_fs_t;

extern ext2_fs_t ext2_fs;

int  ext2_mount(uint8_t drive, uint32_t part_lba);
int  ext2_read_inode(uint32_t ino, ext2_inode_t* inode);
int  ext2_read_block(uint32_t block, void* buf);
int  ext2_read_inode_block(ext2_inode_t* inode, uint32_t iblock, void* buf);
// resolve: inode > 0, absent = 0, error = -1; get_size: bytes >= 0 or error = -1.
int64_t ext2_resolve(const char* path);
int64_t ext2_get_size(const char* path);
int  ext2_read_file(const char* path, void* buf, uint32_t maxlen);
int  ext2_readdir(const char* path, dirent_t* entries, uint32_t max_entries);
// Adversarial self-test for the directory-block scanner (it parses untrusted
// on-disk directory blocks): returns 0 if all cases pass, else the failure count.
int  ext2_dir_selftest(void);

// Write operations
int  ext2_write_block(uint32_t block, const void* buf);
int  ext2_write_inode(uint32_t ino, const ext2_inode_t* inode);
int  ext2_sync_bgd(uint32_t group);
int  ext2_sync_superblock(void);
uint32_t ext2_alloc_block(void);
uint32_t ext2_alloc_inode(void);
int  ext2_write_file(const char* path, const void* buf, uint32_t len);
int  ext2_create_file(const char* path);

// mkfs: write a fresh single-block-group ext2 (1024-byte blocks) onto a disk at
// part_lba (raw, via ata_write_sectors). total_blocks = size in 1024-byte blocks
// (capped to one ~8 MB group). Returns 0 on success. ext2_format_selftest KATs it.
int  ext2_format(uint8_t drive, uint32_t part_lba, uint32_t total_blocks);
int  ext2_format_selftest(void);
int  ext2_format_mg_selftest(void);   // multi-block-group format KAT
int  ext2_mkdir(const char* path);
int  ext2_unlink(const char* path);

// SMP FS lock. External callers (vfs.c fs_* mount wrappers, auth.c passwd helpers)
// hold this around every ext2_* call — the driver's scratch buffers are shared
// across cores. ext2.c never takes it internally (its public fns call each other).
// v5.9.25.
uint64_t ext2_lock_acquire(void);
void     ext2_lock_release(uint64_t flags);

// Sector-cache hit/miss counters (v5.9.26), for `df`.
void ext2_cache_stats(uint64_t* hits, uint64_t* misses);

#endif
