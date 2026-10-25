#!/usr/bin/env python3
"""Test actual ATA completion and account I/O failure paths with host mocks."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from test_vfs_mount_paths import function

ROOT = Path(__file__).resolve().parents[1]
COMMON = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static int checks,failures;
#define CHECK(x) do { checks++;if(!(x)){failures++;fprintf(stderr,"line %d: %s\n",__LINE__,#x);} } while(0)
'''
ATA = r'''
static uint8_t status=ATA_STATUS_DRDY,completion=ATA_STATUS_DRDY,ready=ATA_STATUS_DRQ;
static int words;
static uint8_t inb(uint16_t port) { assert(port==ATA_PRIMARY_CMD);return status; }
static void outb(uint16_t port,uint8_t value) {
    if(port==ATA_PRIMARY_CMD) {
        if(value==ATA_CMD_WRITE) { status=ready;words=0; }
        else if(value==ATA_CMD_CACHE_FLUSH) status=completion;
        else abort();
    }
}
static void outw(uint16_t port,uint16_t value) {
    (void)value;assert(port==ATA_PRIMARY_DATA);if(++words==256) status=completion;
}
@FUNCTIONS@
int main(void) {
    uint16_t data[256]={0};
    CHECK(ata_write_sectors(0,400,1,data)==1);CHECK(words==256);
    CHECK(ata_flush()==0);
    const uint8_t faults[]={ATA_STATUS_ERR,0x20,ATA_STATUS_ERR|0x20};
    for(unsigned i=0;i<sizeof(faults);i++) {
        status=ATA_STATUS_DRDY;completion=ATA_STATUS_DRDY|faults[i];
        CHECK(ata_write_sectors(0,400,1,data)==-1);CHECK(words==256);
        status=ATA_STATUS_DRDY;CHECK(ata_flush()==-1);
        status=ATA_STATUS_DRDY;ready=ATA_STATUS_DRQ|faults[i];
        CHECK(ata_write_sectors(0,400,1,data)==-1);CHECK(words==0);
        ready=ATA_STATUS_DRQ;
    }
    status=ATA_STATUS_BSY;CHECK(ata_busy_wait(ATA_PRIMARY_CTRL,1)==-1);
    status=0;CHECK(ata_drq_wait(ATA_PRIMARY_DATA)==-1);
    status=ATA_STATUS_DRDY;completion=ATA_STATUS_DRDY;
    CHECK(ata_write_sectors(0,400,0,data)==-1);
    CHECK(ata_write_sectors(0,0x10000000,1,data)==-1);
    CHECK(ata_write_sectors(0,400,1,data)==1);CHECK(ata_flush()==0);
    printf("ATA errors: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
'''
AUTH = r'''
#define SHA256_DIGEST_SIZE 32
#define SALT_HEX_LEN 32
#define PBKDF2_ITERATIONS 1000
#define AUTH_MIN_PASS 4
static struct { int block_size; } ext2_fs={1024};
static int64_t resolved=1;
static int read_result=-1,fallbacks,writes,additions,hashes,fb_count;
static uint64_t ext2_lock_acquire(void) { return 0; }
static void ext2_lock_release(uint64_t f) { (void)f; }
static int64_t ext2_resolve(const char* p) { (void)p;return resolved; }
static int read_passwd(char* p,uint32_t n) { (void)n;p[0]=0;return read_result; }
static int write_passwd(const char* p,uint32_t n) { (void)p;writes++;return (int)n; }
static int fallback_verify(const char* u,const char* p) { (void)u;(void)p;fallbacks++;return 1; }
static void add_fallback(const char* u,const char* p,int a) { (void)u;(void)p;(void)a;additions++; }
static int fallback_add(const char* u,const char* p,int a) { add_fallback(u,p,a);return 0; }
static void gen_random_salt(char* s) { memset(s,0,SALT_HEX_LEN+1); }
static void hash_password(const char* p,const char* s,uint32_t it,uint8_t* h) { (void)p;(void)s;(void)it;memset(h,0,32);hashes++; }
static void format_entry(char* p,size_t n,const char* u,const char* s,uint32_t it,const uint8_t* h,int a) {
    (void)u;(void)s;(void)it;(void)h;(void)a;snprintf(p,n,"entry\n");
}
static int parse_entry(const char* p,char* u,size_t un,char* s,size_t sn,uint32_t* it,uint8_t* h) {
    (void)p;(void)u;(void)un;(void)s;(void)sn;(void)it;(void)h;return -1;
}
@FUNCTIONS@
int main(void) {
    resolved=-1;
    CHECK(passwd_exists()==-1);CHECK(auth_setup()==-1);
    CHECK(writes==0 && additions==0 && hashes==0);
    CHECK(auth_verify_core("nyx","nyx")==0);CHECK(fallbacks==0);
    auth_add_user("test","longpassword",0);
    CHECK(writes==0 && additions==0 && hashes==0);
    resolved=1;
    CHECK(passwd_exists()==1);CHECK(auth_setup()==0);
    CHECK(auth_verify_core("nyx","nyx")==0);CHECK(fallbacks==0);
    auth_add_user("test","longpassword",0);
    CHECK(writes==0 && additions==0 && hashes==0);
    resolved=0;CHECK(passwd_exists()==0);
    CHECK(auth_verify_core("nyx","nyx")==1);CHECK(fallbacks==1);
    ext2_fs.block_size=0;resolved=-1;
    CHECK(passwd_exists()==0);CHECK(auth_verify_core("nyx","nyx")==1);
    CHECK(fallbacks==2);
    printf("Account storage errors: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    specs = [
        ('ata', 'kernel/drivers/misc/ata', ATA,
         ['int ata_lba28_valid(', 'static int ata_busy_wait(', 'static int ata_drq_wait(',
          'int ata_write_sectors(', 'int ata_flush(']),
        ('auth', 'kernel/auth/auth', AUTH,
         ['static int passwd_exists(', 'int auth_setup(', 'static int auth_verify_core(',
          'void auth_add_user(']),
    ]
    failed = False
    with tempfile.TemporaryDirectory(prefix='nyx-storage-errors-') as td:
        work = Path(td)
        for name, path, harness, signatures in specs:
            source = (args.source_root / (path + '.c')).read_text(encoding='utf-8')
            header = (args.source_root / (path + '.h')).read_text(encoding='utf-8')
            code = COMMON + header + harness.replace('@FUNCTIONS@', '\n'.join(
                function(source, sig) for sig in signatures))
            c, exe = work / (name + '.c'), work / name
            c.write_text(code, encoding='utf-8')
            subprocess.run(['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-pie', '-no-pie', str(c), '-o', str(exe)], check=True)
            failed |= subprocess.run([str(exe)]).returncode != 0
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
