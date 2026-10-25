#!/usr/bin/env python3
"""Run real scheduler bodies against stale-cursor and outgoing-stack fixtures.

This tests C selection contracts; QEMU validates the final assembly stack switch.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_process_reap import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}} while(0)
typedef struct process {
    int state, sched_cpu, sched_managed, sched_quantum, sched_weight;
    uint32_t wake_tick, alarm_tick;
    void *stack, *page_directory, *kernel_stack;
} process_t;
typedef struct {
    void *sched_cur, *sched_active;
    uint64_t sc_saved_rsp, sc_next_rsp, sc_next_cr3, idle_rsp, ticks;
    int cpu_number, preempt_count, sched_scan;
} cpu_info_t;
typedef int spinlock_t;
spinlock_t sched_lock;
static int locked;
static cpu_info_t cpu;
static cpu_info_t* cpu_self(void) { return &cpu; }
process_t *process_table[MAX_PROCESSES];
int process_count;
static process_t running, idle, remote;
static process_t *idle_proc=&idle, *target;
static int current_idx, sched_enabled=1, g_cpu_idle;
static uint64_t kernel_pml4_phys=0x1000, frame[24], idle_frame[24], remote_frame[24];
static uint32_t tick_count;
#define saved_rsp (cpu.sc_saved_rsp)
#define next_rsp (cpu.sc_next_rsp)
#define next_cr3 (cpu.sc_next_cr3)
#define SIGALRM 14
static uint64_t spin_lock_irqsave(spinlock_t *p) {
    CHECK(p==&sched_lock && !locked);locked=1;return 0;
}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f) {
    CHECK(p==&sched_lock && locked && !f);locked=0;
}
static int g_perf_cpu;
static void perf_cpu_tick(int *p,int idle_tick) { (void)p;(void)idle_tick; }
static void signal_raise(process_t *p,int sig) { (void)p;(void)sig; }
static void apic_eoi(void) {}
static void sched_target(process_t *p) {
    CHECK(locked && cpu.sched_cur==p);
    target=p;next_rsp=(uintptr_t)p->stack;next_cr3=kernel_pml4_phys;
}
'''
TESTS = r'''
static void reset(void) {
    memset(&cpu,0,sizeof(cpu));memset(&running,0,sizeof(running));
    memset(&idle,0,sizeof(idle));memset(&remote,0,sizeof(remote));
    memset(process_table,0,sizeof(process_table));locked=0;target=NULL;
    running.state=PROC_RUN;running.stack=frame;running.page_directory=(void*)0x9000;
    running.sched_managed=1;running.sched_quantum=1;
    idle.state=PROC_RUN;idle.stack=idle_frame;
    remote.state=PROC_RUN;remote.stack=remote_frame;remote.sched_cpu=1;
    process_table[0]=&running;process_table[1]=&idle;process_table[2]=&remote;
    process_count=3;current_idx=0;cpu.sched_cur=&running;cpu.sched_active=&running;
    saved_rsp=(uintptr_t)frame;frame[18]=0x1b;cpu.idle_rsp=0x2000;
}
int main(void) {
#ifdef LOCKED_PUBLICATION
    reset();process_count=MAX_PROCESSES;
    CHECK(!publish_process(&remote) && process_count==MAX_PROCESSES && !locked);
    CHECK(process_table[0]==&running && process_table[MAX_PROCESSES-1]==NULL);
    process_count=MAX_PROCESSES-1;
    CHECK(publish_process(&remote) && process_count==MAX_PROCESSES && !locked);
    CHECK(process_table[MAX_PROCESSES-1]==&remote);
#endif
#ifdef BOOT_ADOPTION
    reset();sched_enabled=0;
    sched_adopt_boot_process(&idle);
    CHECK(cpu.sched_cur==&idle && cpu.sched_active==&idle && current_idx==1);
    CHECK(running.stack==NULL && get_current_process()==&idle && !locked);
    sched_enabled=1;
#endif
    reset();current_idx=2;running.sched_quantum=2;
    CHECK(get_current_process()==&running);
    irq_scheduler_tick();CHECK(!locked && !target);
    CHECK(next_rsp==(uintptr_t)frame && next_cr3==0x9000);
    CHECK(remote.stack==remote_frame && running.sched_quantum==1);
    for(int state=PROC_ZOMBIE;state<=PROC_BLOCKED;state++) {
        reset();running.state=state;current_idx=2;
        irq_scheduler_tick();
        CHECK(target==&idle && cpu.sched_cur==&idle && !locked);
        CHECK(cpu.sched_active==&running); // C selection must not retire the stack
        CHECK(running.stack==frame && remote.stack==remote_frame);
    }
    reset();cpu.preempt_count=1;running.state=PROC_ZOMBIE;
    irq_scheduler_tick();CHECK(!target && cpu.sched_cur==&running && !locked);
    reset();cpu.cpu_number=1;cpu.sched_cur=&remote;cpu.sched_active=&remote;
    cpu.sc_saved_rsp=(uintptr_t)remote_frame;cpu.sched_scan=0;
    CHECK(get_current_process()==&remote);
    ap_scheduler_tick();CHECK(target==&remote && !locked);
    reset();cpu.cpu_number=1;cpu.sched_cur=&remote;cpu.sched_active=&remote;
    cpu.sc_saved_rsp=(uintptr_t)remote_frame;remote.state=PROC_ZOMBIE;
    ap_scheduler_tick();
    CHECK(cpu.sched_cur==NULL && cpu.sched_active==&remote && !locked);
    CHECK(cpu.sc_next_rsp==cpu.idle_rsp && cpu.sc_next_cr3==kernel_pml4_phys);
    CHECK(get_current_process()==NULL);
    printf("scheduler handoff: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    root = parser.parse_args().source_root
    header = (root/'kernel/core/kernel.h').read_text()
    defines = '\n'.join(re.findall(r'^#define (?:PROC_RUN|PROC_BLOCKED|PROC_ZOMBIE|MAX_PROCESSES)\s+\S+', header, re.M))+'\n'
    source = (root/'kernel/proc/process.c').read_text()
    bodies = function(source, 'process_t* get_current_process(')
    if 'static int publish_process(' in source:
        defines += '#define LOCKED_PUBLICATION 1\n'
        bodies += function(source, 'static int publish_process(')
    if 'void sched_adopt_boot_process(' in source:
        defines += '#define BOOT_ADOPTION 1\n'
        bodies += function(source, 'void sched_adopt_boot_process(')
    bodies += function(source, 'void irq_scheduler_tick(')
    bodies += function((root/'kernel/core/smp.c').read_text(), 'void ap_scheduler_tick(')
    with tempfile.TemporaryDirectory(prefix='nyx-scheduler-handoff-') as tmp:
        src, binary = Path(tmp)/'test.c', Path(tmp)/'test'
        src.write_text(defines+PREFIX+bodies+TESTS)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-no-pie',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
