#!/usr/bin/env python3
"""Actual waitpid/CPU-owner cleanup bodies, with mocked resources and interrupts.

Run on Linux/WSL. This verifies reference retirement and existing wait results;
it does not simulate concurrent schedulers or prove remote-task quiescence.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %d: %s\n",__LINE__,#x);}} while(0)
typedef struct process {
    uint32_t pid, ppid, waiting_for;
    int state, blocked_in_kernel, stopped_reported, stop_sig, exit_code;
    void *page_directory, *kernel_stack;
} process_t;
static process_t parent, child, other, kernel_task;
static process_t *process_table[8], *self;
static struct { process_t *sched_cur, *fpu_owner; } cpu_info[MAX_CPUS];
static int process_count, current_idx, locked, sched_lock, lock_calls;
static int freed, stack_freed, closed, unmapped, directories, shared, heir;
static uint64_t user_cr3, user_rsp;
static process_t* get_current_process(void) { return self; }
static void mock_cli(void) { CHECK(!locked); }
static void mock_wait(void) { fputs("Unexpected blocking wait\n",stderr);abort(); }
static uint64_t spin_lock_irqsave(int* p) {
    CHECK(p==&sched_lock && !locked); locked=1;lock_calls++;return 0;
}
static void spin_unlock_irqrestore(int* p,uint64_t flags) {
    CHECK(p==&sched_lock && locked && flags==0);
    CHECK(process_count==2 && process_table[2]==NULL);
    CHECK(process_table[0]==&parent && process_table[1]==&other);
    for(int i=0;i<MAX_CPUS;i++) {
        CHECK(cpu_info[i].sched_cur!=&child);
        CHECK(cpu_info[i].fpu_owner!=&child);
    }
    locked=0;
}
static int tg_reassign_leader(process_t* p) { CHECK(p==&child && !locked);return heir; }
static void close_proc_fds(process_t* p) { CHECK(p==&child && !locked);closed++; }
static void mmap_free_bufs(process_t* p) { CHECK(p==&child && !locked);unmapped++; }
static int addr_space_shared(void* p,process_t* proc) {
    CHECK(p==child.page_directory && proc==&child && !locked);return shared;
}
void free_page_directory(uint64_t* p) { CHECK(p==child.page_directory && !locked);directories++; }
static void kfree(void* p) {
    CHECK(!locked);
    if(p==(void*)0x4000) {stack_freed++;return;}
    CHECK(p==&child && lock_calls==1);
    for(int i=0;i<MAX_CPUS;i++) {
        CHECK(cpu_info[i].sched_cur!=p);
        CHECK(cpu_info[i].fpu_owner!=p);
    }
    freed++;
}
'''

TESTS = r'''
static void reset(void) {
    memset(&parent,0,sizeof(parent));memset(&child,0,sizeof(child));
    memset(&other,0,sizeof(other));memset(cpu_info,0,sizeof(cpu_info));
    memset(process_table,0,sizeof(process_table));
    parent.pid=20;parent.state=PROC_RUN;parent.page_directory=(void*)0x1000;
    child.pid=21;child.ppid=20;child.state=PROC_ZOMBIE;child.exit_code=73;
    child.page_directory=(void*)0x2000;child.kernel_stack=(void*)0x5000;
    other.pid=22;other.ppid=99;other.state=PROC_RUN;
    process_table[0]=&parent;process_table[1]=&child;process_table[2]=&other;
    process_count=3;current_idx=2;self=&parent;
    freed=stack_freed=closed=unmapped=directories=shared=heir=locked=lock_calls=0;
    user_cr3=0xabc;user_rsp=0xdef;
    for(int i=0;i<MAX_CPUS;i++) {
        cpu_info[i].sched_cur=&kernel_task;
        cpu_info[i].fpu_owner=&child;
    }
    cpu_info[1].sched_cur=&parent;cpu_info[1].fpu_owner=&parent;
}
static void unchanged(void) {
    CHECK(!locked && !freed && !stack_freed && !closed && !unmapped && !directories);
    CHECK(process_count==3 && process_table[1]==&child && current_idx==2);
    CHECK(user_cr3==0xabc && user_rsp==0xdef);
}
int main(void) {
    int status;
    reset();self=NULL;CHECK(do_waitpid(21,&status,WNOHANG)==-1);unchanged();
    reset();status=-7;CHECK(do_waitpid(99,&status,WNOHANG)==-1);CHECK(status==-7);unchanged();
    reset();child.state=PROC_RUN;status=-7;
    CHECK(do_waitpid(21,&status,WNOHANG)==0);CHECK(status==-7);unchanged();
    reset();child.state=PROC_STOPPED;child.stop_sig=19;status=-7;
    CHECK(do_waitpid(21,&status,WNOHANG)==0);CHECK(status==-7 && !child.stopped_reported);unchanged();
    CHECK(do_waitpid(21,&status,WNOHANG|WUNTRACED)==21);
    CHECK(status==(WSTOPPED|19) && child.stopped_reported);unchanged();
    status=-7;CHECK(do_waitpid(21,&status,WNOHANG|WUNTRACED)==0);CHECK(status==-7);unchanged();
    for(int mode=0;mode<4;mode++) {
        reset();shared=mode&1;heir=(mode>>1)&1;status=-7;
        int got=do_waitpid(mode&1?-1:21,mode==3?NULL:&status,0);
        CHECK(got==21);CHECK(status==(mode==3?-7:73));
        CHECK(process_count==2 && current_idx==1 && process_table[1]==&other && !process_table[2]);
        CHECK(freed==1 && stack_freed==1 && closed==!heir && unmapped==1 && directories==!shared);
        CHECK(!locked && lock_calls==1 && user_cr3==0xabc && user_rsp==0xdef);
        CHECK(cpu_info[1].sched_cur==&parent && cpu_info[1].fpu_owner==&parent);
        for(int i=0;i<MAX_CPUS;i++) {
            CHECK(cpu_info[i].sched_cur==(i==1?&parent:&kernel_task));
            CHECK(cpu_info[i].fpu_owner==(i==1?&parent:NULL));
        }
    }
    reset();cpu_info[3].sched_cur=&child;
    sched_forget(NULL);CHECK(cpu_info[3].sched_cur==&child);
    sched_forget(&child);
    CHECK(cpu_info[3].sched_cur==NULL && cpu_info[1].sched_cur==&parent);
    for(int i=0;i<MAX_CPUS;i++)CHECK(cpu_info[i].fpu_owner==(i==1?&parent:NULL));
    printf("process reap: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source_root/'kernel/proc/process.c').read_text()
    header = (args.source_root/'kernel/core/kernel.h').read_text()
    states = '\n'.join(re.findall(r'^#define (?:PROC_RUN|PROC_BLOCKED|PROC_ZOMBIE|PROC_STOPPED|WNOHANG|WUNTRACED|WSTOPPED)\s+\S+', header, re.M))
    smp = (ROOT/'kernel/core/smp.h').read_text()
    states += '\n' + re.search(r'^#define MAX_CPUS\s+\d+', smp, re.M)[0] + '\n'
    wait = function(source, 'int do_waitpid(')
    wait = wait.replace('__asm__ volatile("cli");', 'mock_cli();')
    wait = wait.replace('__asm__ volatile("sti; hlt");', 'mock_wait();')
    with tempfile.TemporaryDirectory(prefix='nyx-process-reap-') as tmp:
        cfile, binary = Path(tmp)/'test.c', Path(tmp)/'test'
        cfile.write_text(states+PREFIX+function(source, 'void sched_forget(')+wait+TESTS)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer','-no-pie',str(cfile),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
