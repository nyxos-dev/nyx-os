#!/usr/bin/env python3
"""Check PID signal posting against controlled collector interleavings.

Runs the actual lookup/posting bodies under ASan/UBSan. Scheduler lock and
retirement events are host models, not an emulation of SMP signal delivery.
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
#define NSIG 32
#define SIGINT 2
#define SIGKILL 9
#define SIGALRM 14
#define SIGCONT 18
#define SIGSTOP 19
#define PROC_RUN 1
#define PROC_ZOMBIE 2
#define PROC_BLOCKED 3
#define PROC_STOPPED 4
typedef struct {
    uint32_t pid, state, reap_claimed, stop_sig, sig_pending, waiting_for, wake_tick;
} process_t;
static process_t *process_table[4];
static int process_count, sched_lock, locked, locks, raises, lookups;
static int retire_at_unlock, retire_after_lookup, retirements;
static uint32_t retired_pending, g_foreground_pid;
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static void retire(void) {
    CHECK(!locked && process_count==1 && process_table[0]);
    retired_pending=process_table[0]->sig_pending;
    free(process_table[0]); process_table[0]=NULL; process_count=0; retirements++;
}
static uint64_t spin_lock_irqsave(int *lock) {
    CHECK(lock==&sched_lock && !locked); locked=1; locks++; return 73;
}
static void spin_unlock_irqrestore(int *lock,uint64_t flags) {
    CHECK(lock==&sched_lock && locked && flags==73); locked=0;
    if(retire_at_unlock) { retire_at_unlock=0; retire(); }
}
'''
MIDDLE = r'''
static process_t *find_process(uint64_t pid) {
    lookups++; CHECK(locked);
    process_t *p=actual_find_process(pid);
    if(retire_after_lookup && p) {
        /* Collector wins before post if lookup is unprotected. Otherwise it
         * can only remove the object after posting releases the lock. */
        retire_after_lookup=0;
        if(locked)retire_at_unlock=1;
        else retire();
    }
    return p;
}
static void signal_raise(process_t *p,int sig) {
    CHECK(locked); raises++; actual_signal_raise(p,sig);
}
'''
TESTS = r'''
static process_t *add(uint32_t state,int claimed) {
    process_t *p=calloc(1,sizeof(*p));if(!p)abort();
    p->pid=7;p->state=state;p->reap_claimed=claimed;
    p->waiting_for=99;p->wake_tick=123;p->stop_sig=SIGSTOP;
    process_table[process_count++]=p;return p;
}
static void reset(void) {
    CHECK(!locked);for(int i=0;i<process_count;i++)free(process_table[i]);
    memset(process_table,0,sizeof(process_table));process_count=0;
    locks=raises=lookups=retire_at_unlock=retire_after_lookup=retirements=0;
    retired_pending=g_foreground_pid=0;
}
int main(void) {
    reset();process_t *p=add(PROC_RUN,0), before=*p;
    CHECK(do_kill(7,-1)==-1 && do_kill(7,NSIG)==-1);
    CHECK(!memcmp(p,&before,sizeof(*p)) && locks==0 && lookups==0);
    CHECK(do_kill(8,SIGINT)==-1 && do_kill(8,0)==-1 && !locked);
    CHECK(do_kill(7,0)==0 && !memcmp(p,&before,sizeof(*p)) && raises==0);
    for(int sig=1;sig<NSIG;sig++) {
        p->sig_pending=0;
        CHECK(do_kill(7,sig)==0 && p->sig_pending==(1u<<sig));
    }
    for(int state=PROC_RUN;state<=PROC_STOPPED;state++) {
        reset();p=add((uint32_t)state,1);before=*p;
        CHECK(do_kill(7,0)==-1 && do_kill(7,SIGKILL)==-1);
        CHECK(!memcmp(p,&before,sizeof(*p)) && raises==0 && !locked);
    }
    reset();p=add(PROC_BLOCKED,0);
    CHECK(do_kill(7,SIGINT)==0 && p->state==PROC_RUN);
    CHECK(p->waiting_for==0 && p->wake_tick==0 && p->sig_pending==(1u<<SIGINT));
    for(int sig=SIGCONT;sig>=SIGKILL;sig-=SIGCONT-SIGKILL) {
        reset();p=add(PROC_STOPPED,0);
        CHECK(do_kill(7,sig)==0 && p->state==PROC_RUN && p->stop_sig==0);
        CHECK(p->sig_pending==(1u<<sig));
    }
    reset();p=add(PROC_STOPPED,0);
    CHECK(do_kill(7,SIGINT)==0 && p->state==PROC_STOPPED && p->stop_sig==SIGSTOP);
    reset();p=add(PROC_ZOMBIE,0);
    CHECK(do_kill(7,0)==0 && do_kill(7,SIGCONT)==0 && p->state==PROC_ZOMBIE);
    reset();p=add(PROC_RUN,0);signal_send_foreground(SIGINT);
    CHECK(lookups==0 && p->sig_pending==0);
    g_foreground_pid=8;signal_send_foreground(SIGINT);CHECK(p->sig_pending==0);
    g_foreground_pid=7;signal_send_foreground(SIGINT);CHECK(p->sig_pending==(1u<<SIGINT));
    p->sig_pending=0;p->reap_claimed=1;signal_send_foreground(SIGKILL);
    CHECK(p->sig_pending==0 && p->state==PROC_RUN);
    reset();p=add(PROC_BLOCKED,0);
    /* Alarm callers already hold the lock; low-level posting must not relock. */
    uint64_t flags=spin_lock_irqsave(&sched_lock);
    actual_signal_raise(p,SIGALRM);spin_unlock_irqrestore(&sched_lock,flags);
    CHECK(locks==1 && p->state==PROC_RUN && p->sig_pending==(1u<<SIGALRM));
    reset();add(PROC_RUN,0);retire_after_lookup=1;
    CHECK(do_kill(7,SIGINT)==0);
    CHECK(retirements==1 && !process_count && retired_pending==(1u<<SIGINT));
    reset();add(PROC_RUN,0);g_foreground_pid=7;retire_after_lookup=1;
    signal_send_foreground(SIGINT);
    CHECK(retirements==1 && !process_count && retired_pending==(1u<<SIGINT));
    reset();add(PROC_RUN,0);retire_after_lookup=1;CHECK(do_kill(7,0)==0);
    CHECK(retirements==1 && retired_pending==0 && raises==0);
    reset();printf("signal lifetime: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source_root/'kernel/proc/signal.c').read_text()
    process = (args.source_root/'kernel/proc/process.c').read_text()
    code = PREFIX + function(process, 'process_t* find_process(').replace('find_process(', 'actual_find_process(', 1)
    code += function(source, 'void signal_raise(').replace('signal_raise(', 'actual_signal_raise(', 1)
    code += MIDDLE + function(source, 'int do_kill(') + function(source, 'void signal_send_foreground(') + TESTS
    with tempfile.TemporaryDirectory(prefix='nyx-signal-lifetime-') as temp:
        path = Path(temp)
        (path/'test.c').write_text(code)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)


if __name__ == '__main__':
    main()
