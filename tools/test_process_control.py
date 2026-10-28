#!/usr/bin/env python3
"""Exercise actual kill/pkill code with controlled retirement and compaction."""
import argparse
from pathlib import Path
import re
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
#define PROC_RUN 1
#define PROC_ZOMBIE 2
@TYPE@
typedef struct {
    uint32_t pid,ppid,state,cpu_time,reap_claimed,sched_managed;
    int exit_code; void *page_directory; char comm[32];
} process_t;
static process_t *process_table[MAX_PROCESSES];
static int process_count,sched_lock,locked,locks,active,no_memory,wakes,reaped;
static int allocations,frees,retire_unlocked_lookup,retire_before_control,protect_before_control;
static unsigned checks,failures;
static char output[65536];
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}} while(0)
static void retire_all(void) {
    CHECK(!locked);
    for(int i=0;i<process_count;i++)free(process_table[i]);
    memset(process_table,0,sizeof process_table);process_count=0;
}
static process_t *find_process(uint64_t pid) {
    for(int i=0;i<process_count;i++)if(process_table[i] && process_table[i]->pid==pid) {
        process_t *p=process_table[i];
        if(retire_unlocked_lookup && !locked){retire_unlocked_lookup=0;retire_all();}
        return p;
    }
    return NULL;
}
static uint64_t spin_lock_irqsave(int *p){CHECK(p==&sched_lock&&!locked);locked=1;locks++;return 79;}
static void spin_unlock_irqrestore(int *p,uint64_t flags){CHECK(p==&sched_lock&&locked&&flags==79);locked=0;}
static void memset_asm(void *p,int c,size_t n){memset(p,c,n);}
static void *kmalloc(size_t n){CHECK(!locked);if(no_memory)return NULL;allocations++;return malloc(n);}
static void kfree(void *p){CHECK(!locked&&p);frees++;free(p);}
static void wake_waiters_locked(process_t *p){CHECK(locked&&p);wakes++;}
static int reap_claim_locked(process_t *p){CHECK(locked&&p&&!p->reap_claimed);if(active)return 0;p->reap_claimed=1;return 1;}
static void reap_claimed_process(process_t *p){
    CHECK(!locked&&p->reap_claimed&&p->state==PROC_ZOMBIE&&!active);reaped++;
    for(int i=0;i<process_count;i++)if(process_table[i]==p){process_table[i]=process_table[--process_count];process_table[process_count]=NULL;break;}
    free(p);
}
static int capture(const char *fmt,...) {
    CHECK(!locked);
    size_t offset=strlen(output);va_list ap;va_start(ap,fmt);
    int n=vsnprintf(output+offset,sizeof(output)-offset,fmt,ap);va_end(ap);
    CHECK(n>=0 && (size_t)n<sizeof(output)-offset);return n;
}
'''
WRAPPER = r'''
static int process_terminate_user(uint64_t pid) {
    if(retire_before_control){retire_before_control=0;retire_all();}
    if(protect_before_control){
        protect_before_control=0;
        for(int i=0;i<process_count;i++)if(process_table[i]->pid==pid)process_table[i]->page_directory=NULL;
    }
    return actual_terminate_user(pid);
}
'''
TESTS = r'''
static process_t *add(uint32_t pid,const char *name,int managed,int user) {
    process_t *p=calloc(1,sizeof(*p));if(!p)abort();
    p->pid=pid;p->ppid=1;p->state=PROC_RUN;p->exit_code=73;p->sched_managed=managed;
    p->page_directory=user?(void*)0x1000:NULL;snprintf(p->comm,sizeof(p->comm),"%s",name);
    process_table[process_count++]=p;return p;
}
static void reset(void) {
    retire_all();CHECK(allocations==frees);
    active=no_memory=wakes=reaped=allocations=frees=locks=0;
    retire_unlocked_lookup=retire_before_control=protect_before_control=0;output[0]=0;
}
int main(void) {
#if !SKIP_KILL
    char *kill21[]={"kill","21"},*kill1[]={"kill","1"};
    reset();cmd_kill(1,kill21);CHECK(strstr(output,"Usage:")&&!locks);
    reset();cmd_kill(2,kill21);CHECK(strstr(output,"not found")&&!wakes&&!reaped);
    reset();process_t *p=add(1,"kernel",0,0);cmd_kill(2,kill1);
    CHECK(strstr(output,"kernel thread")&&p->state==PROC_RUN&&p->exit_code==73&&!wakes&&!reaped);
    reset();p=add(21,"child",1,1);retire_unlocked_lookup=1;cmd_kill(2,kill21);
    CHECK(strstr(output,"process 21 terminated")&&p->state==PROC_ZOMBIE&&p->exit_code==-1&&wakes==1&&!reaped);
    reset();p=add(21,"child",1,1);p->reap_claimed=1;cmd_kill(2,kill21);
    CHECK(strstr(output,"not found")&&p->state==PROC_RUN&&!wakes&&!reaped);
    reset();p=add(21,"child",1,1);p->state=PROC_ZOMBIE;cmd_kill(2,kill21);
    CHECK(p->exit_code==73&&wakes==1&&!reaped);
    reset();add(21,"child",0,1);cmd_kill(2,kill21);CHECK(!process_count&&reaped==1);
    reset();p=add(21,"child",0,1);active=1;cmd_kill(2,kill21);
    CHECK(p->state==PROC_ZOMBIE&&p->exit_code==-1&&!reaped);
#endif
    char *pkill[]={"pkill","worker"},*nothing[]={"pkill","absent"},*all[]={"pkill",""};
    reset();cmd_pkill(1,pkill);CHECK(strstr(output,"Usage:")&&!allocations&&!locks);
    reset();add(21,"worker-a",0,1);add(22,"worker-b",0,1);add(23,"worker-c",0,1);
    cmd_pkill(2,pkill);
    CHECK(!process_count&&reaped==3&&wakes==3&&strstr(output,"killed 21")&&strstr(output,"killed 22")&&strstr(output,"killed 23"));
    reset();add(1,"worker-kernel",0,0);add(21,"worker-user",0,1);add(22,"different",0,1);
    cmd_pkill(2,pkill);CHECK(process_count==2&&reaped==1&&wakes==1&&strstr(output,"killed 21"));
    CHECK(find_process(1)&&find_process(22));
    reset();add(1,"worker-kernel",0,0);cmd_pkill(2,pkill);
    CHECK(process_count==1&&!reaped&&!wakes&&strstr(output,"only protected kernel threads"));
    reset();add(21,"worker",0,1);cmd_pkill(2,nothing);
    CHECK(process_count==1&&!reaped&&!wakes&&strstr(output,"no process matches"));
    reset();add(21,"worker",0,1);no_memory=1;cmd_pkill(2,pkill);
    CHECK(process_count==1&&!reaped&&!wakes&&!locks&&strstr(output,"out of memory"));
    reset();add(21,"worker",0,1);retire_before_control=1;cmd_pkill(2,pkill);
    CHECK(!process_count&&!reaped&&!wakes&&strstr(output,"already exited"));
    reset();add(21,"worker",0,1);protect_before_control=1;cmd_pkill(2,pkill);
    CHECK(process_count==1&&!reaped&&!wakes&&strstr(output,"only protected kernel threads"));
    reset();add(1,"kernel",0,0);add(21,"worker",0,1);cmd_pkill(2,all);
    CHECK(process_count==1&&find_process(1)&&reaped==1);
    reset();add(21,"worker",0,1)->reap_claimed=1;cmd_pkill(2,pkill);
    CHECK(process_count==1&&!reaped&&!wakes&&strstr(output,"no process matches"));
    reset();add(UINT32_MAX,"worker",0,1);cmd_pkill(2,pkill);
    CHECK(!process_count&&strstr(output,"killed 4294967295"));
    reset();for(int i=1;i<=MAX_PROCESSES;i++)add((uint32_t)i,"worker",0,1);
    cmd_pkill(2,pkill);CHECK(!process_count&&reaped==MAX_PROCESSES&&strstr(output,"killed 512"));
    reset();printf("process control: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--only-pkill', action='store_true')
    args = parser.parse_args()
    process = (args.source_root / 'kernel/proc/process.c').read_text()
    kernel = (args.source_root / 'kernel/core/kernel.c').read_text()
    header = (ROOT / 'kernel/core/kernel.h').read_text()
    record = re.search(r'typedef struct \{[^{}]+\} process_info_t;', header)[0]
    code = PREFIX.replace('@TYPE@', record)
    for signature in ['static void process_copy_info(', 'int process_snapshot(',
                      'static int terminate_process(', 'void destroy_process(']:
        if signature in process:
            code += function(process, signature)
    if 'int process_terminate_user(' in process:
        code += function(process, 'int process_terminate_user(').replace(
            'int process_terminate_user(', 'static int actual_terminate_user(') + WRAPPER
    for name in ['kill', 'pkill']:
        code += function(kernel, 'static void cmd_' + name + '(int argc, char** argv) {').replace('printf(', 'capture(')
    code += '\n#define SKIP_KILL ' + str(int(args.only_pkill)) + '\n' + TESTS
    with tempfile.TemporaryDirectory(prefix='nyx-process-control-') as temp:
        src, exe = Path(temp) / 'test.c', Path(temp) / 'test'
        src.write_text(code)
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-no-pie',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
