#!/usr/bin/env python3
"""Exercise scheduler controls against process retirement and CPU handoff."""
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
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define PROC_RUN 1
#define PROC_ZOMBIE 2
#define PROC_BLOCKED 3
#define MAX_PROCESSES 512
#define MAX_CPUS 16
typedef struct {
    uint32_t pid;
    uint32_t state, reap_claimed, sched_weight, sched_quantum, sched_managed;
    int32_t sched_cpu;
    void *page_directory;
} process_t;
static process_t *process_table[MAX_PROCESSES];
static int process_count, sched_lock, locked, locks;
static struct {void *sched_cur, *sched_active, *fpu_owner;} cpu_info[MAX_CPUS];
static uint32_t cpu_count=4;
static int retire_lookup, retire_lock, retire_unlock, spawn_result=21;
static unsigned checks, failures;
static char output[4096];
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}} while(0)
static void retire_all(void) {
    CHECK(!locked);
    for(int i=0;i<process_count;i++)free(process_table[i]);
    memset(process_table,0,sizeof process_table);process_count=0;
}
static process_t *find_process(uint64_t pid) {
    for(int i=0;i<process_count;i++)if(process_table[i] && process_table[i]->pid==pid) {
        process_t *p=process_table[i];
        if(retire_lookup && !locked){retire_lookup=0;retire_all();}
        return p;
    }
    return NULL;
}
static uint64_t spin_lock_irqsave(int *p) {
    CHECK(p==&sched_lock&&!locked);
    if(retire_lock){retire_lock=0;retire_all();}
    locked=1;locks++;return 79;
}
static void spin_unlock_irqrestore(int *p,uint64_t flags) {
    CHECK(p==&sched_lock&&locked&&flags==79);locked=0;
    if(retire_unlock){retire_unlock=0;retire_all();}
}
static int capture(const char *fmt,...) {
    CHECK(!locked);
    size_t n=strlen(output);va_list ap;va_start(ap,fmt);
    int written=vsnprintf(output+n,sizeof output-n,fmt,ap);va_end(ap);
    CHECK(written>=0&&(size_t)written<sizeof output-n);return written;
}
static int spawn_user_path(const char *path) {CHECK(!locked&&path);return spawn_result;}
'''
TESTS = r'''
static process_t *add(uint64_t pid) {
    process_t *p=calloc(1,sizeof(*p));if(!p)abort();
    p->pid=pid;p->state=PROC_RUN;p->sched_weight=3;p->sched_quantum=7;
    p->sched_managed=1;p->page_directory=(void*)0x1000;
    process_table[process_count++]=p;return p;
}
static void reset(void) {
    retire_all();memset(cpu_info,0,sizeof cpu_info);output[0]=0;
    locks=retire_lookup=retire_lock=retire_unlock=0;cpu_count=4;spawn_result=21;
}
static void nice_cases(void) {
    char *a[]={"nice","9","/worker.elf"};
    reset();process_t *p=add(21);retire_lookup=1;cmd_nice(3,a);
    CHECK(p->sched_weight==9&&p->sched_quantum==7&&strstr(output,"weight 9"));
    reset();cmd_nice(2,a);CHECK(strstr(output,"Usage:")&&!locks);
    reset();spawn_result=-6;cmd_nice(3,a);CHECK(strstr(output,"could not load")&&!locks);
    reset();cmd_nice(3,a);CHECK(strstr(output,"exited before")&&!strstr(output,"running in background"));
    reset();p=add(21);p->state=PROC_ZOMBIE;cmd_nice(3,a);
    CHECK(p->sched_weight==3&&strstr(output,"exited before"));
    reset();add(21);retire_lock=1;cmd_nice(3,a);CHECK(!process_count&&strstr(output,"exited before"));
    reset();add(21);retire_unlock=1;cmd_nice(3,a);CHECK(!process_count&&strstr(output,"weight 9"));
}
static void renice_cases(void) {
    char *a[]={"renice","21","9"};
    reset();process_t *p=add(21);retire_lookup=1;cmd_renice(3,a);
    CHECK(p->sched_weight==9&&p->sched_quantum==7&&strstr(output,"weight set to 9"));
    reset();cmd_renice(2,a);CHECK(strstr(output,"Usage:")&&!locks);
    reset();cmd_renice(3,a);CHECK(strstr(output,"not found")&&!strstr(output,"weight set"));
    reset();p=add(21);p->reap_claimed=1;cmd_renice(3,a);
    CHECK(p->sched_weight==3&&strstr(output,"not found"));
    reset();p=add(21);p->state=PROC_ZOMBIE;cmd_renice(3,a);
    CHECK(p->sched_weight==3&&strstr(output,"already exited"));
    reset();p=add(21);p->page_directory=NULL;cmd_renice(3,a);CHECK(p->sched_weight==9);
    reset();p=add(21);a[2]="-1";cmd_renice(3,a);CHECK(p->sched_weight==1);
    a[2]="999";cmd_renice(3,a);CHECK(p->sched_weight==64);
    reset();add(21);retire_lock=1;cmd_renice(3,a);CHECK(!process_count&&strstr(output,"not found"));
    reset();add(21);retire_unlock=1;cmd_renice(3,a);CHECK(!process_count&&strstr(output,"weight set to 64"));
}
static void pin_cases(int retire) {
    char *a[]={"taskset","1","21"};
    reset();process_t *p=add(21);retire_lookup=retire;cmd_taskset(3,a);
    CHECK(p->sched_cpu==1&&strstr(output,"pinned PID 21 to CPU 1"));
    reset();cmd_taskset(2,a);CHECK(strstr(output,"Usage:")&&!locks);
    reset();cmd_taskset(3,a);CHECK(strstr(output,"not found"));
    reset();p=add(21);p->page_directory=NULL;cmd_taskset(3,a);
    CHECK(!p->sched_cpu&&strstr(output,"kernel thread"));
    for(int i=0;i<MAX_CPUS;i++)for(int active=0;active<3;active++) {
        reset();p=add(21);
        if(active==2)cpu_info[i].fpu_owner=p;
        else if(active==1)cpu_info[i].sched_active=p;else cpu_info[i].sched_cur=p;
        cmd_taskset(3,a);CHECK(!p->sched_cpu&&strstr(output,"retry later")&&!strstr(output,"pinned"));
        output[0]=0;a[1]="0";cmd_taskset(3,a);
        CHECK(!p->sched_cpu&&strstr(output,"pinned PID 21 to CPU 0"));a[1]="1";
        memset(cpu_info,0,sizeof cpu_info);output[0]=0;cmd_taskset(3,a);
        CHECK(p->sched_cpu==1&&strstr(output,"pinned PID 21 to CPU 1"));
    }
    reset();p=add(21);p->state=PROC_BLOCKED;cmd_taskset(3,a);CHECK(p->sched_cpu==1);
    reset();p=add(21);p->state=PROC_ZOMBIE;cmd_taskset(3,a);CHECK(!p->sched_cpu&&strstr(output,"already exited"));
    reset();p=add(21);p->reap_claimed=1;cmd_taskset(3,a);CHECK(!p->sched_cpu&&strstr(output,"not found"));
    reset();p=add(21);p->sched_managed=0;cmd_taskset(3,a);CHECK(!p->sched_cpu&&strstr(output,"not ready"));
    for(int claimed=0;claimed<2;claimed++) {
        reset();p=add(21);process_t *sibling=add(22);sibling->reap_claimed=claimed;
        if(claimed)sibling->state=PROC_ZOMBIE;
        cmd_taskset(3,a);CHECK(!p->sched_cpu&&strstr(output,"shares its address space"));
        output[0]=0;a[1]="0";cmd_taskset(3,a);CHECK(strstr(output,"pinned"));a[1]="1";
    }
    reset();p=add(21);add(22)->page_directory=(void*)0x2000;cmd_taskset(3,a);CHECK(p->sched_cpu==1);
    reset();p=add(21);a[1]="-1";cmd_taskset(3,a);CHECK(!p->sched_cpu&&!locks&&strstr(output,"cpu must be"));
    a[1]="4";cmd_taskset(3,a);CHECK(!p->sched_cpu&&!locks&&strstr(output,"cpu must be"));a[1]="1";
    reset();add(21);retire_lock=1;cmd_taskset(3,a);CHECK(!process_count&&strstr(output,"not found"));
    reset();add(21);retire_unlock=1;cmd_taskset(3,a);CHECK(!process_count&&strstr(output,"pinned"));
}
int main(int argc,char **argv) {
    const char *which=argc>1?argv[1]:"all";
    if(!strcmp(which,"all")||!strcmp(which,"nice"))nice_cases();
    if(!strcmp(which,"all")||!strcmp(which,"renice"))renice_cases();
    if(!strcmp(which,"all")||!strcmp(which,"taskset"))pin_cases(1);
    if(!strcmp(which,"pinning"))pin_cases(0);
#ifdef SAFE_SETTERS
    if(!strcmp(which,"all")) {
        reset();process_t *p=add(UINT32_MAX);p->state=PROC_BLOCKED;
        CHECK(process_set_weight(UINT32_MAX,0)==-2&&p->sched_weight==3&&!locks);
        CHECK(process_set_weight(UINT32_MAX,65)==-2&&p->sched_weight==3&&!locks);
        CHECK(process_set_weight(UINT32_MAX,1)==0&&p->sched_weight==1);
        CHECK(process_set_weight(UINT32_MAX,64)==0&&p->sched_weight==64&&p->sched_quantum==7);
        CHECK(process_set_cpu(UINT32_MAX,-1)==-3&&!p->sched_cpu);
        CHECK(process_set_cpu(UINT32_MAX,4)==-3&&!p->sched_cpu);
        CHECK(process_set_cpu(UINT32_MAX,3)==0&&p->sched_cpu==3);
        CHECK(process_set_cpu(UINT32_MAX,0)==0&&!p->sched_cpu);
        CHECK(process_set_weight(UINT64_MAX,1)==-1&&p->sched_weight==64);
        CHECK(process_set_cpu(UINT64_MAX,2)==-1&&!p->sched_cpu);
        reset();for(int i=0;i<MAX_PROCESSES;i++)add((uint64_t)i+1)->page_directory=(void*)(uintptr_t)(0x1000+i*4096);
        for(int i=0;i<MAX_PROCESSES;i++) {
            CHECK(process_set_weight(i+1,64)==0&&process_table[i]->sched_weight==64);
            CHECK(process_set_cpu(i+1,3)==0&&process_table[i]->sched_cpu==3);
        }
    }
#endif
    reset();printf("process scheduling: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--case', choices=['all', 'nice', 'renice', 'taskset', 'pinning'], default='all')
    args = parser.parse_args()
    process = (args.source_root/'kernel/proc/process.c').read_text()
    kernel = (args.source_root/'kernel/core/kernel.c').read_text()
    header = (ROOT/'kernel/core/kernel.h').read_text()
    defines = '\n'.join(re.findall(r'^#define SCHED_WEIGHT_(?:MIN|MAX)\s+\S+', header, re.M))+'\n'
    code = defines+PREFIX
    if 'int process_set_weight(' in process:
        code += '#define SAFE_SETTERS 1\n'
        code += function(process, 'int process_set_weight(')
        code += function(process, 'int process_set_cpu(')
    code += function(kernel, 'static uint32_t clamp_weight(int w) {')
    for name in ['nice', 'renice', 'taskset']:
        code += function(kernel, 'static void cmd_'+name+'(int argc, char** argv) {').replace('printf(', 'capture(')
    with tempfile.TemporaryDirectory(prefix='nyx-process-scheduling-') as temp:
        src, exe = Path(temp)/'test.c', Path(temp)/'test'
        src.write_text(code+TESTS)
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-no-pie',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe), args.case], check=True)


if __name__ == '__main__':
    main()
