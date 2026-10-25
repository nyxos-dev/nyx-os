#!/usr/bin/env python3
"""Run the EXT2 driver against an in-memory disk with every I/O failure injected.

Requires gcc and e2fsprogs. --source-root can select a baseline source tree.
This tests error propagation, not journalling or recovery after power loss.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define MAX_FILENAME 128
#define MAX_PATH 256
#define memset_asm memset
typedef struct { uint32_t ino; char name[MAX_FILENAME]; uint32_t type; } dirent_t;
typedef struct { unsigned year,month,day,hour,minute,second; } rtc_time_t;
static void rtc_read_time(rtc_time_t* t) { *t=(rtc_time_t){2026,1,1,0,0,0}; }
typedef int spinlock_t;
#define SPINLOCK_INIT 0
static uint64_t spin_lock_irqsave(spinlock_t* p) { (void)p;return 0; }
static void spin_unlock_irqrestore(spinlock_t* p,uint64_t f) { (void)p;(void)f; }
static void* kmalloc(size_t n) { return malloc(n); }
static void kfree(void* p) { free(p); }
static unsigned char *disk,*original;
static size_t disk_size;
static int counts[3], fail_kind=-1, fail_at, triggered, writes_after_error;
static int fault(int kind) {
    if(kind==1 && triggered) writes_after_error++;
    counts[kind]++;
    if(kind==fail_kind && counts[kind]==fail_at) { triggered=1;return 1; }
    return 0;
}
static int blk_read1(uint8_t dev,uint32_t lba,void* p) {
    assert(dev==0 && (size_t)lba*512+512<=disk_size);
    if(fault(0)) return -1;
    memcpy(p,disk+(size_t)lba*512,512);return 0;
}
static int blk_write1(uint8_t dev,uint32_t lba,const void* p) {
    assert(dev==0 && (size_t)lba*512+512<=disk_size);
    if(fault(1)) return -1;
    memcpy(disk+(size_t)lba*512,p,512);return 0;
}
static int blk_flush(uint8_t dev) { assert(dev==0);return fault(2)?-1:0; }
'''
SUFFIX = r'''
static int checks,failures;
#define CHECK(x) do { checks++;if(!(x)){failures++;if(failures<20)fprintf(stderr,"line %d: %s\n",__LINE__,#x);} } while(0)
static unsigned char data[280*1024];
static int64_t operation(int op) {
    dirent_t entries[32];
    switch(op) {
    case 0:return ext2_write_file("/probe",data,9);
    case 1:return ext2_write_file("/created",data,15*1024);
    case 2:return ext2_write_file("/created",data,sizeof(data));
    case 3:return ext2_mkdir("/newdir");
    case 4:return ext2_unlink("/probe");
    case 5:return ext2_unlink("/large");
    case 6:return ext2_read_file("/large",data,sizeof(data));
    case 7:return ext2_readdir("/",entries,32);
    case 8:return ext2_get_size("/probe");
    case 9:return ext2_resolve("/probe");
    case 10:return ext2_resolve("/absent");
    case 11:return ext2_create_file("/empty");
    default:abort();
    }
}
static void reset(void) {
    memcpy(disk,original,disk_size);
    fail_kind=-1;triggered=writes_after_error=0;
    CHECK(ext2_mount(0,0)==0);
    memset(counts,0,sizeof(counts));
    memset(data,'N',sizeof(data));
}
int main(int argc,char** argv) {
    assert(argc==2);
    FILE* f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);disk_size=(size_t)ftell(f);rewind(f);
    original=malloc(disk_size);disk=malloc(disk_size);assert(original&&disk);
    assert(fread(original,1,disk_size,f)==disk_size);fclose(f);
    init_ext2();
    for(int op=0;op<12;op++) {
        reset();CHECK(operation(op)>=0);
        int totals[3];memcpy(totals,counts,sizeof(totals));
        if(op<=2) {
            uint32_t size=op==0?9:op==1?15*1024:(uint32_t)sizeof(data);
            memset(data,0,sizeof(data));
            CHECK(ext2_read_file(op==0?"/probe":"/created",data,sizeof(data))==(int)size);
            for(uint32_t i=0;i<size;i++) if(data[i]!='N') { CHECK(0);break; }
        }
        for(int kind=0;kind<3;kind++) for(int at=1;at<=totals[kind];at++) {
            reset();fail_kind=kind;fail_at=at;
            int64_t rc=operation(op);
            CHECK(triggered);CHECK(rc<0);CHECK(writes_after_error==0);
            fail_kind=-1;
            int writes=counts[1];
            CHECK(ext2_write_file("/keep","BAD",3)<0);
            CHECK(counts[1]==writes);
            char keep[16]={0};
            CHECK(ext2_read_file("/keep",keep,sizeof(keep))==10);
            CHECK(!memcmp(keep,"KEEP_SAFE\n",10));
            if(failures) { fprintf(stderr,"operation=%d kind=%d transfer=%d rc=%lld\n",op,kind,at,(long long)rc);goto done; }
        }
        printf("EXT2 operation %d: %d read, %d write, %d flush failure points\n",op,totals[0],totals[1],totals[2]);
    }
    reset();
    CHECK(ext2_get_size("/empty-existing")==0);
    int64_t missing_size=ext2_get_size("/absent");CHECK(missing_size==-1);
    CHECK(ext2_resolve("/absent")==0);
    for(int at=1;at<=2;at++) {
        reset();memset(counts,0,sizeof(counts));fail_kind=0;fail_at=at;
        CHECK(ext2_mount(0,0)<0);CHECK(ext2_fs.block_size==0);
        int64_t probe=ext2_resolve("/probe");CHECK(probe<0);
        int writes=counts[1];ext2_inode_t inode;
        CHECK(ext2_write_block(1,data)<0);CHECK(ext2_write_inode(2,&inode)<0);
        CHECK(ext2_sync_superblock()<0);CHECK(ext2_sync_bgd(0)<0);
        CHECK(ext2_alloc_inode()==0 && ext2_alloc_block()==0);CHECK(counts[1]==writes);
    }
    reset();
    // A failed data write must not make subsequent reads return the rejected bytes.
    ext2_inode_t inode;CHECK(ext2_read_inode((uint32_t)ext2_resolve("/probe"),&inode)==0);
    unsigned char block[4096];memset(block,'X',sizeof(block));
    memset(counts,0,sizeof(counts));fail_kind=1;fail_at=1;
    CHECK(ext2_write_block(inode.block[0],block)<0);
    fail_kind=-1;memset(block,0,sizeof(block));
    CHECK(ext2_read_block(inode.block[0],block)==0);
    CHECK(!memcmp(block,"OLD_DATA\n",9));
done:
    for(int i=0;i<5;i++) kfree(ext2_bufs[i]);
    free(disk);free(original);
    printf("EXT2 I/O: %d checks, %d failures (mock device, ASan/UBSan)\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source_root / 'kernel/fs/ext2.c').read_text(encoding='utf-8')
    # Keep the complete driver through unlink, excluding the separate raw formatter.
    source = source[:source.index('// mkfs — format')]
    source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    header = (args.source_root / 'kernel/fs/ext2.h').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='nyx-ext2-io-') as td:
        work = Path(td)
        disk = work / 'test.img'
        with disk.open('wb') as f:
            f.truncate(8 * 1024 * 1024)
        subprocess.run(['mkfs.ext2', '-q', '-b', '1024', '-I', '128', '-O',
                        '^resize_inode,^dir_index,^ext_attr', str(disk)], check=True)
        for name, data in [('probe', b'OLD_DATA\n'), ('keep', b'KEEP_SAFE\n'),
                           ('large', b'L' * (280 * 1024)), ('empty-existing', b'')]:
            fixture = work / name
            fixture.write_bytes(data)
            subprocess.run(['debugfs', '-w', '-R', f'write {fixture} /{name}', str(disk)],
                           check=True, capture_output=True)
        c = work / 'test.c'
        c.write_text(PREFIX + header + source + SUFFIX, encoding='utf-8')
        exe = work / 'test'
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-pie', '-no-pie', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(disk)], check=True)


if __name__ == '__main__':
    main()
