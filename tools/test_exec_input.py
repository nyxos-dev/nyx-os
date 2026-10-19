#!/usr/bin/env python3
"""Check exec's actual bounded user-string/vector copying with mapped-memory mocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned char memory[8192];
static uint64_t fault = UINT64_MAX;
static uint64_t user_v2p(uint64_t va) {
    if (va < 0x10000 || va >= 0x10000 + sizeof(memory) || va == fault) return 0;
    return (uint64_t)(uintptr_t)(memory + va - 0x10000);
}
static int copy_from_user(void *dst, uint64_t src, uint64_t size) {
    for (uint64_t i=0;i<size;i++) {
        uint64_t p=user_v2p(src+i); if(!p)return -1;
        ((unsigned char*)dst)[i]=*(unsigned char*)(uintptr_t)p;
    }
    return 0;
}
'''
TEST = r'''
static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void) {
    char dst[258];
    const unsigned widths[]={0,1,64,128,160,256};
    for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);w++) {
        unsigned width=widths[w];
        for(unsigned len=0;len<=257;len++) {
            memset(memory,'x',sizeof(memory));memory[len]=0;
            memset(dst,0x5a,sizeof(dst));
            int r=copy_exec_string(dst+1,0x10000,width);
            CHECK((r==0)==(len<width));
            CHECK(dst[0]==0x5a && dst[width+1]==0x5a);
            if(r==0)CHECK(!memcmp(dst+1,memory,len+1));
        }
    }
    memset(memory,'x',sizeof(memory));memory[255]=0;
    for(unsigned i=0;i<=255;i++) {
        fault=0x10000+i;CHECK(copy_exec_string(dst,0x10000,256)==-1);
    }
    fault=UINT64_MAX;
    CHECK(copy_exec_string(dst,0,256)==-1);
    CHECK(copy_exec_string(dst,UINT64_MAX-10,256)==-1);
    CHECK(copy_exec_string(dst,0x12000,256)==-1);
    char store[32][256];char* ptrs[32];
    CHECK(copy_exec_vector(0,&store[0][0],ptrs,32,256)==0);
    for(int limit=16;limit<=32;limit+=16) {
        int width=limit==16?160:256;
        for(int count=0;count<=limit+1;count++) {
            memset(memory,0,sizeof(memory));
            memset(memory+4096,'v',width-1);
            for(int i=0;i<count;i++) { uint64_t p=0x11000;memcpy(memory+i*8,&p,8); }
            int got=copy_exec_vector(0x10000,&store[0][0],ptrs,limit,width);
            CHECK(got==(count<=limit?count:-1));
            if(got>=0)for(int i=0;i<got;i++) {
                CHECK(ptrs[i]==&store[0][0]+i*width);
                CHECK(strlen(ptrs[i])==(unsigned)width-1);
            }
            if(count==limit) {
                for(int byte=0;byte<8;byte++) {
                    fault=0x10000+limit*8+byte;
                    CHECK(copy_exec_vector(0x10000,&store[0][0],ptrs,limit,width)==-1);
                }
                fault=UINT64_MAX;
            }
        }
        memset(memory,0,sizeof(memory));uint64_t invalid=1;memcpy(memory,&invalid,8);
        CHECK(copy_exec_vector(0x10000,&store[0][0],ptrs,limit,width)==-1);
        uint64_t valid=0x11000;memcpy(memory,&valid,8);memset(memory+4096,'v',width);
        CHECK(copy_exec_vector(0x10000,&store[0][0],ptrs,limit,width)==-1);
    }
    printf("PASS %u exec string/vector checks under ASan/UBSan\n",checks);
    return 0;
}
'''


def main():
    source = (ROOT / 'kernel/core/syscall.c').read_text(encoding='utf-8')
    start = source.index('static int copy_str_from_user(')
    end = source.index('/* Resolve `path`', start)
    # Keep the entry wiring under test as well as the shared copy implementation.
    entry = source[source.index('        case SYS_EXECVE: {'):source.index('        case SYS_DUP2: {')]
    for expected in ('copy_exec_string(path, a1, sizeof(path))', 'kargv_store[32][256]',
                     'copy_exec_vector(a2,', 'copy_exec_vector(a3,',
                     'if (argc < 0) return -1;', 'if (envc < 0) return -1;'):
        assert expected in entry, expected
    with tempfile.TemporaryDirectory(prefix='nyx-exec-input-') as temp:
        path, exe = Path(temp) / 'input.c', Path(temp) / 'input'
        path.write_text(PREFIX + source[start:end] + TEST, encoding='utf-8')
        subprocess.run(['gcc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror', '-no-pie',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
