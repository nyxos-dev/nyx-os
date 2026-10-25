#!/usr/bin/env python3
"""Check actual stack-canary scans against controlled process collection.

ASan/UBSan detect stale process/stack reads. Lock, interrupt and collector
events are host models; this does not model arbitrary SMP stack writes.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_process_reap import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define MAX_PROCESSES 512
typedef struct { uint32_t pid, reap_claimed; void *kernel_stack; } process_t;
static process_t *process_table[MAX_PROCESSES];
static int process_count, sched_lock, locked, locks, reads, reports;
static int oom, retire_on_read, retire_on_unlock, retire_on_print, retirements;
static int allocations, releases;
static void *buffer;
static char output[65536];
static size_t output_len;
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while (0)
static void retire_all(void) {
    CHECK(!locked);
    for (int i=0;i<process_count;i++) {
        process_t *p=process_table[i];
        if (!p) continue;
        if (p->kernel_stack && !p->reap_claimed)
            free((void*)((uintptr_t)p->kernel_stack-4096));
        free(p);process_table[i]=NULL;
    }
    process_count=0;retirements++;
}
static void *kmalloc(size_t n) {
    CHECK(!locked && n==MAX_PROCESSES*sizeof(uint32_t) && !buffer);
    allocations++;if (oom) return NULL;
    buffer=malloc(n);if (!buffer) abort();return buffer;
}
static void kfree(void *p) {
    CHECK(!locked && p==buffer && buffer);free(p);buffer=NULL;releases++;
}
static uint64_t spin_lock_irqsave(int *p) {
    CHECK(p==&sched_lock && !locked);locked=1;locks++;return 0x202;
}
static void spin_unlock_irqrestore(int *p,uint64_t flags) {
    CHECK(p==&sched_lock && locked && flags==0x202);locked=0;
    if (retire_on_unlock) { retire_on_unlock=0;retire_all(); }
}
static int report(const char *fmt,...) {
    CHECK(!locked);reports++;
    if (retire_on_print) { retire_on_print=0;retire_all(); }
    va_list args;va_start(args,fmt);
    int n=vsnprintf(output+output_len,sizeof(output)-output_len,fmt,args);
    va_end(args);if (n<0 || (size_t)n>=sizeof(output)-output_len) abort();
    output_len+=(size_t)n;return n;
}
'''
MIDDLE = r'''
static int stack_canary_intact(void *stack) {
    CHECK(locked);reads++;
    int intact=actual_stack_canary_intact(stack);
    if (retire_on_read) {
        retire_on_read=0;
        if (locked) retire_on_unlock=1;
        else retire_all();
    }
    return intact;
}
#define printf report
'''
TESTS = r'''
#undef printf
static void reset(void) {
    CHECK(!locked && !buffer);retire_all();
    locks=reads=reports=oom=retire_on_read=retire_on_unlock=retire_on_print=0;
    retirements=allocations=releases=0;output_len=0;output[0]=0;
}
static process_t *add(uint32_t pid,int corrupt,int claimed) {
    process_t *p=calloc(1,sizeof(*p));void *stack=malloc(4096);
    if (!p || !stack) abort();
    stack_canary_place(stack);if (corrupt) ((uint8_t*)stack)[0]^=1;
    p->pid=pid;p->reap_claimed=claimed;p->kernel_stack=(uint8_t*)stack+4096;
    process_table[process_count++]=p;
    /* A claimed object remains published while its stack is already freed. */
    if (claimed) free(stack);
    return p;
}
int main(int argc,char **argv) {
    const char *scenario=argc>1?argv[1]:"all";
    if (!strcmp(scenario,"claimed")) {
        add(7,0,1);CHECK(stack_canary_sweep()==0);CHECK(reads==0);
    } else if (!strcmp(scenario,"race")) {
        add(7,1,0);retire_on_read=1;
        CHECK(stack_canary_sweep()==1 && retirements==1);
        CHECK(strstr(output,"pid 7: kernel stack canary SMASHED"));
    } else {
        CHECK(stack_canary_selftest()==0);
        CHECK(stack_canary_sweep()==0 && !reads && !reports && locks==1);
        reset();process_table[process_count++]=NULL;
        process_t *p=calloc(1,sizeof(*p));p->pid=1;process_table[process_count++]=p;
        add(2,0,0);add(3,0,1);
        CHECK(stack_canary_sweep()==0 && reads==1 && !reports && releases==1);
        reset();add(7,1,0);add(UINT32_MAX,1,0);add(9,0,0);
        CHECK(stack_canary_sweep()==2 && reads==3 && reports==2);
        CHECK(strstr(output,"pid 7: kernel stack canary SMASHED"));
        CHECK(strstr(output,"pid 4294967295: kernel stack canary SMASHED"));
        reset();add(7,1,0);retire_on_read=1;
        CHECK(stack_canary_sweep()==1 && retirements==1 && !process_count);
        CHECK(strstr(output,"pid 7: kernel stack canary SMASHED"));
        reset();add(7,1,0);add(8,1,0);retire_on_print=1;
        CHECK(stack_canary_sweep()==2 && retirements==1 && reports==2);
        CHECK(strstr(output,"pid 8: kernel stack canary SMASHED"));
        reset();add(7,1,0);oom=1;cmd_stackcheck(0,NULL);
        CHECK(allocations==1 && !releases && !locks && !reads && reports==1);
        CHECK(!strcmp(output,"stackcheck: out of memory; scan not performed\n"));
        reset();add(7,0,0);cmd_stackcheck(0,NULL);
        CHECK(!strcmp(output,"stackcheck: all task kernel-stack canaries intact\n"));
        reset();add(7,1,0);cmd_stackcheck(0,NULL);
        CHECK(strstr(output,"stackcheck: 1 task kernel-stack(s) SMASHED\n"));
        reset();
        for (unsigned i=0;i<MAX_PROCESSES;i++) add(i+1,1,0);
        CHECK(stack_canary_sweep()==MAX_PROCESSES && reads==MAX_PROCESSES);
        CHECK(reports==MAX_PROCESSES && allocations==1 && releases==1);
        const char *at=output;
        for (unsigned i=0;i<MAX_PROCESSES;i++) {
            char expected[96];snprintf(expected,sizeof(expected),"[STACKGUARD] pid %u: kernel stack canary SMASHED\n",i+1);
            size_t n=strlen(expected);CHECK(!strncmp(at,expected,n));at+=n;
        }
        CHECK(!*at);
    }
    reset();printf("stack lifetime: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--scenario', choices=['all', 'claimed', 'race'], default='all')
    args = parser.parse_args()
    source = (args.source_root/'kernel/proc/process.c').read_text()
    shell = (args.source_root/'kernel/core/kernel.c').read_text()
    canary = next(line for line in source.splitlines() if line.startswith('#define STACK_CANARY '))
    code = PREFIX + canary + '\n' + function(source, 'static void stack_canary_place(')
    code += function(source, 'static int stack_canary_intact(').replace('stack_canary_intact(', 'actual_stack_canary_intact(', 1)
    code += function(source, 'int stack_canary_selftest(').replace('stack_canary_intact(', 'actual_stack_canary_intact(')
    code += MIDDLE + function(source, 'int stack_canary_sweep(')
    code += function(shell, 'static void cmd_stackcheck(int argc, char** argv) {') + TESTS
    with tempfile.TemporaryDirectory(prefix='nyx-stack-lifetime-') as temp:
        path = Path(temp)
        (path/'test.c').write_text(code)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer','-no-pie',str(path/'test.c'),
                        '-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test'),args.scenario],check=True)


if __name__ == '__main__':
    main()
