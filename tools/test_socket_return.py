#!/usr/bin/env python3
"""Execute actual socket syscall bodies with simulated waits and address spaces.

Run on Linux/WSL. The scheduler, socket transport, copyout and IRQ masking are
mocked; real network behavior and the assembly return path require guest tests.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKERS = ["case SYS_CONNECT:", "case SYS_ACCEPT:",
           "if (internal & UFD_SOCK_FLAG) {                 /* socket read",
           "case SYS_RECVFROM:"]

PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UFD_SOCK_FLAG 0x2000
#define UFD_SOCK_ID(x) ((x)&0xfff)
#define UFD_SOCK_MAKE(x) ((x)|UFD_SOCK_FLAG)
static uint64_t user_cr3=123,user_rsp=456;
static int kind,delay,reply,fault,host_if,waits,allocs,frees,copies,fdallocs,closes;
static char buffer[16],kernel_buffer[16];
static unsigned int ip; static int port;
static uint64_t a1=3,a2,a3=8,a5,a6;
static int count=8,internal=UFD_SOCK_FLAG;
static void check_context(void) {
    assert(!host_if && user_cr3==123 && user_rsp==456);
}
static int ufd_lookup(int fd,int* value) {
    assert(fd==3); *value=fault==2?0:UFD_SOCK_FLAG; return fault==1?-1:0;
}
static int ufd_alloc(int value) {
    check_context(); assert(value==UFD_SOCK_MAKE(reply)); fdallocs++;
    return fault==5?-1:10;
}
static int user_ptr_ok(uint64_t ptr,uint64_t size) {
    assert(ptr && (size==8 || size==4)); return fault!=6;
}
static void* kmalloc(int size) {
    assert(size==8); if(fault==3)return NULL; allocs++; return kernel_buffer;
}
static void kfree(void* ptr) {
    check_context(); assert(ptr==kernel_buffer && allocs==1 && !frees); frees++;
}
static int copy_to_user(uint64_t ptr,const void* buf,int len) {
    check_context(); copies++; if(fault==4)return -1;
    assert(len==4); memcpy((void*)(uintptr_t)ptr,buf,(size_t)len); return 0;
}
static void wait_network(void) {
    check_context();
    for(int i=0;i<delay;i++) {
        waits++; host_if=1; user_cr3=777+i; user_rsp=888+i;
    }
}
static int nsock_connect(int s,uint32_t addr,uint16_t p) {
    (void)addr; assert(s==0 && p==8); wait_network(); return reply;
}
static int nsock_accept(int s) { assert(s==0); wait_network(); return reply; }
static void nsock_close(int s) { check_context(); assert(s==reply); closes++; }
static int nsock_recv(int s,void* buf,int len) {
    assert(s==0 && len==8); wait_network();
    if(reply>0)memcpy(buf,"SOCK",4);
    return reply;
}
static int nsock_recvfrom(int s,void* buf,int len,uint32_t* addr,uint16_t* p) {
    int n=nsock_recv(s,buf,len); if(n>0){*addr=42;*p=7;} return n;
}
'''

SUFFIX = r'''
int main(int argc,char**argv) {
    assert(argc==5); kind=atoi(argv[1]); delay=atoi(argv[2]);
    reply=atoi(argv[3]); fault=atoi(argv[4]);
    a2=(uintptr_t)buffer; a5=(uintptr_t)&ip; a6=(uintptr_t)&port;
    if(fault==7)a5=a6=0;
    long result=kind==0?run_0():kind==1?run_1():kind==2?run_2():run_3();
    check_context();
    int early=fault==1 || fault==2 || fault==3 || fault==6;
    assert(waits==(early?0:delay));
    int expected=early?-1:reply;
    if(!early && kind==1 && reply>=0)expected=fault==5?-1:10;
    if(!early && kind>=2 && reply>0 && fault==4)expected=-1;
    assert(result==expected);
    assert(allocs==frees);
    assert(allocs==(kind>=2 && fault!=1 && fault!=2 && fault!=3 && fault!=6));
    int delivered=!early && kind>=2 && reply>0;
    assert(copies==(delivered?(kind==3 && fault!=4 && fault!=7?3:1):0));
    if(delivered && fault!=4)assert(!memcmp(buffer,"SOCK",4));
    if(delivered && kind==3 && fault!=4 && fault!=7)assert(ip==42 && port==7);
    else assert(ip==0 && port==0);
    assert(fdallocs==(!early && kind==1 && reply>=0));
    assert(closes==(fdallocs && fault==5));
    return 0;
}
'''


def body(source, marker):
    start = source.index('{', source.index(marker))
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start + 1:end - 1]


def run(source, label):
    code = PREFIX
    for kind, marker in enumerate(MARKERS):
        extracted = body(source, marker).replace(
            '__asm__ volatile("cli" ::: "memory");', 'host_if=0;')
        code += f'\nstatic long run_{kind}(void) {{\n{extracted}\n}}\n'
    code += SUFFIX
    cases = []
    for kind in range(4):
        success = [0, 1, 4, 4][kind]
        for delay in (0, 1, 3):
            for reply in ([success, -1, 0] if kind >= 2 else [success, -1]):
                cases.append((kind, delay, reply, 0))
            for fault in ((4, 7) if kind == 3 else (4,) if kind == 2 else (5,) if kind == 1 else ()):
                cases.append((kind, delay, success, fault))
        # Outer SYS_READ validation is outside the extracted socket branch.
        for fault in ((1, 2, 3, 6) if kind == 3 else (3,) if kind == 2 else (1, 2)):
            cases.append((kind, 3, success, fault))
    failures = 0
    with tempfile.TemporaryDirectory(prefix='nyx-socket-return-') as tmp:
        directory = Path(tmp)
        (directory / 'test.c').write_text(code, encoding='utf-8')
        binary = directory / 'test'
        subprocess.run(['gcc', '-O1', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(directory / 'test.c'), '-o', str(binary)], check=True)
        for case in cases:
            result = subprocess.run([str(binary), *map(str, case)], capture_output=True, text=True, timeout=10)
            if result.returncode:
                failures += 1
                print(f'{label}: FAIL kind/delay/reply/fault={case}: {result.stderr.strip()}')
    print(f'{label}: {len(cases)} socket return cases, {failures} failures')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref', help='Also run the same cases on an earlier Git ref')
    args = parser.parse_args()
    failures = run((ROOT / 'kernel/core/syscall.c').read_text(encoding='utf-8'), 'current')
    if args.compare_ref:
        old = subprocess.check_output(['git', 'show', f'{args.compare_ref}:kernel/core/syscall.c'], cwd=ROOT, text=True)
        if not run(old, args.compare_ref):
            raise SystemExit('Comparison ref did not reproduce a failure')
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
