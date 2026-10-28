#!/usr/bin/env python3
"""Exercise real monitor and process-list commands during process retirement."""
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
#define MON_HISTORY 96
#define MON_ROWS 64
#define FONT_HEIGHT 16
#define FONT_WIDTH 8
#define HEADER_H 18
#define ROW_H FONT_HEIGHT
#define GRAPH_H 56
#define LABEL_H 15
#define COL_PID 40
#define COL_NAME 190
#define COL_STATE 300
#define COL_CPU 380
#define THEME_WINDOW_BG 0
#define THEME_PANEL_HEADER 1
#define THEME_TEXT 2
#define PROC_RUN 1
#define PROC_ZOMBIE 2
#define PROC_BLOCKED 3
#define PROC_STOPPED 4
#define KEY_UP 1
#define KEY_DOWN 2
@TYPES@
typedef struct { uint32_t pid,ppid,state,cpu_time,reap_claimed,sched_managed; char comm[32]; uint64_t heap_start,program_break; } process_t;
@CONTEXT@
typedef struct { void *reserved; } window_t;
static process_t *process_table[MAX_PROCESSES];
static int process_count, sched_lock, locked, locks, retire_on_row, retire_on_print, no_memory, allocations, frees;
static uint64_t memory_total=5ULL*1024*1024*1024;
static unsigned checks,failures;
static char rendered[65536];
#define CHECK(x) do { checks++; if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);} } while(0)
static void retire(void) {
    CHECK(!locked);
    for(int i=0;i<process_count;i++){free(process_table[i]);process_table[i]=NULL;}
    process_count=0;
}
static uint64_t spin_lock_irqsave(int *p) { CHECK(p==&sched_lock&&!locked);locked=1;locks++;return 75; }
static void spin_unlock_irqrestore(int *p,uint64_t f) { CHECK(p==&sched_lock&&locked&&f==75);locked=0; }
static void memset_asm(void *p,int c,size_t n){memset(p,c,n);}
static void *kmalloc(size_t n) { CHECK(!locked);if(no_memory)return NULL;allocations++;return malloc(n); }
static void kfree(void *p) { CHECK(!locked&&p);frees++;free(p); }
static uint32_t fb_rgb(int r,int g,int b){return (uint32_t)(r+g+b);}
static void fb_fill_rect(int x,int y,uint32_t w,uint32_t h,uint32_t c) {
    (void)x;(void)y;(void)w;(void)c;CHECK(!locked);
    if(retire_on_row&&h==ROW_H){retire_on_row=0;retire();}
}
static void capture(const char *s) {
    size_t n=strlen(rendered),m=strlen(s);CHECK(n+m+2<sizeof(rendered));
    if(n+m+2<sizeof(rendered)){memcpy(rendered+n,s,m);rendered[n+m]='\n';rendered[n+m+1]=0;}
}
static void font_draw_string(int x,int y,const char *s,uint32_t fg,uint32_t bg) {
    (void)x;(void)y;(void)fg;(void)bg;CHECK(!locked);capture(s);
}
static uint32_t perf_cpu_percent(void){return 3;}
static void mon_mem(uint32_t *p,uint32_t *u,uint32_t *t){*p=3;*u=32;*t=1024;}
static void mon_draw_graph(int x,int y,int w,int h,const unsigned char *a,int n,int at,uint32_t t,uint32_t b,uint32_t c) {
    (void)x;(void)y;(void)w;(void)h;(void)a;(void)n;(void)at;(void)t;(void)b;(void)c;CHECK(!locked);
}
static void mon_size(char *s,int n,uint64_t v){snprintf(s,(size_t)n,"%llu",(unsigned long long)v);}
static int output(const char *fmt,...) {
    CHECK(!locked);
    if(retire_on_print && (strstr(fmt,"%-4") || strstr(fmt,"%u") || strstr(fmt,"%d"))){retire_on_print=0;retire();}
    char buf[256];va_list ap;va_start(ap,fmt);int n=vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);capture(buf);return n;
}
'''
TESTS = r'''
static process_t *add(uint32_t id,int claimed) {
    process_t *p=calloc(1,sizeof(*p));if(!p)abort();
    p->pid=id;p->ppid=1;p->state=PROC_RUN;p->cpu_time=id;p->reap_claimed=claimed;
    snprintf(p->comm,sizeof(p->comm),"process-%u",id);p->heap_start=4096;p->program_break=8192;
    process_table[process_count++]=p;return p;
}
static void reset(void) {
    retire();CHECK(allocations==frees);allocations=frees=locks=retire_on_row=retire_on_print=no_memory=0;rendered[0]=0;
}
int main(void) {
    reset();taskman_win_t tm={0};window_t win={.reserved=&tm};
    add(7,0);add(8,0);retire_on_row=1;
    taskman_win_draw(&win,0,0,520,430);
    CHECK(process_count==0 && strstr(rendered,"process-7") && strstr(rendered,"process-8"));
    CHECK(strstr(rendered,"4K") && strstr(rendered,"Procs: 2"));
    taskman_win_draw(&win,0,0,520,430);CHECK(tm.scroll_offset==0 && tm.process_total==0);
    taskman_win_key(&win,KEY_DOWN);CHECK(tm.scroll_offset==0);
    reset();add(7,0);add(8,0);retire_on_print=1;cmd_ps(0,NULL);
    CHECK(process_count==0 && strstr(rendered,"process-7") && strstr(rendered,"process-8"));
    CHECK(allocations==1 && frees==1);
    reset();add(7,0);no_memory=1;cmd_ps(0,NULL);
    CHECK(strstr(rendered,"ps: out of memory") && process_count==1 && allocations==0);
    reset();add(1,0);add(7,0)->sched_managed=1;add(8,0)->sched_managed=1;
    retire_on_print=1;cmd_jobs(0,NULL);
    CHECK(process_count==0 && strstr(rendered,"process-7") && strstr(rendered,"process-8"));
    CHECK(!strstr(rendered,"process-1") && allocations==1 && frees==1);
    reset();add(1,0);cmd_jobs(0,NULL);
    CHECK(strstr(rendered,"No background jobs.") && !strstr(rendered,"PID  PPID"));
    reset();add(7,0)->sched_managed=1;no_memory=1;cmd_jobs(0,NULL);
    CHECK(strstr(rendered,"jobs: out of memory") && process_count==1 && !locks && !allocations);
    char *listed[]={"pgrep","-l","process-"},*pids[]={"pgrep","process-"},*usage[]={"pgrep","-l"};
    reset();add(7,0);add(8,0);retire_on_print=1;cmd_pgrep(3,listed);
    CHECK(process_count==0 && strstr(rendered,"7 process-7") && strstr(rendered,"8 process-8"));
    CHECK(allocations==1 && frees==1);
    reset();add(7,0);add(8,0);retire_on_print=1;cmd_pgrep(2,pids);
    CHECK(process_count==0 && strstr(rendered,"7\n") && strstr(rendered,"8\n") && !strstr(rendered,"process-"));
    reset();add(7,0);no_memory=1;cmd_pgrep(3,listed);
    CHECK(strstr(rendered,"pgrep: out of memory") && process_count==1 && !locks && !allocations);
    reset();cmd_pgrep(1,usage);CHECK(strstr(rendered,"Usage:") && !allocations && !locks);
    reset();cmd_pgrep(2,usage);CHECK(strstr(rendered,"Usage:") && !allocations && !locks);
    reset();add(3,0);char *absent[]={"pgrep","missing"};cmd_pgrep(2,absent);
    CHECK(strstr(rendered,"pgrep: no process matches 'missing'") && allocations==frees);
    reset();add(7,0);add(8,0);char *empty[]={"pgrep","-l",""};cmd_pgrep(3,empty);
    CHECK(strstr(rendered,"7 process-7") && strstr(rendered,"8 process-8"));
    reset();add(7,1);add(8,0)->state=PROC_ZOMBIE;cmd_pgrep(3,listed);
    CHECK(!strstr(rendered,"process-7") && strstr(rendered,"process-8"));
    reset();add(UINT32_MAX,0);cmd_pgrep(2,pids);CHECK(strstr(rendered,"4294967295\n"));
    reset();add(7,0)->sched_managed=1;process_table[0]->state=PROC_ZOMBIE;
    add(8,0)->sched_managed=1;process_table[1]->state=PROC_BLOCKED;
    add(9,0)->sched_managed=1;process_table[2]->state=PROC_STOPPED;
    cmd_jobs(0,NULL);CHECK(strstr(rendered,"exited") && strstr(rendered,"blocked") && strstr(rendered,"?"));
#if HAS_JOBS_SNAPSHOT
    process_info_t jobs[3];
    reset();add(0,0)->sched_managed=1;add(1,1)->sched_managed=1;add(2,0);
    process_table[process_count++]=NULL;add(3,0)->sched_managed=1;add(4,0)->sched_managed=1;
    memset(jobs,0xA5,sizeof jobs);
    CHECK(process_jobs_snapshot(jobs,1)==1 && jobs[0].pid==3 && jobs[1].pid==0xA5A5A5A5u);
    CHECK(process_jobs_snapshot(jobs,3)==2 && jobs[1].pid==4);
    CHECK(process_jobs_snapshot(NULL,1)==0 && process_jobs_snapshot(jobs,0)==0 && process_jobs_snapshot(jobs,-1)==0);
    memset(process_table[4]->comm,'z',32);
    CHECK(process_jobs_snapshot(jobs,2)==2 && jobs[0].comm[30]=='z' && jobs[0].comm[31]==0);
    reset();for(int i=1;i<=MAX_PROCESSES;i++)add((uint32_t)i,0)->sched_managed=1;
    retire_on_print=1;cmd_jobs(0,NULL);
    CHECK(process_count==0 && strstr(rendered,"process-512") && allocations==frees);
    reset();for(int i=1;i<=MAX_PROCESSES;i++)add((uint32_t)i,0);
    retire_on_print=1;cmd_pgrep(3,listed);
    CHECK(process_count==0 && strstr(rendered,"512 process-512") && allocations==frees);
#endif
#if HAS_MONITOR_SNAPSHOT
    process_monitor_info_t rows[3];int offset,total;
    reset();add(0,0);add(1,1);process_table[process_count++]=NULL;
    for(int i=2;i<10;i++)add((uint32_t)i,0);
    offset=100;total=-1;memset(rows,0xA5,sizeof(rows));
    CHECK(process_monitor_snapshot(rows,2,&offset,&total)==2 && offset==6 && total==8);
    CHECK(rows[0].info.pid==8 && rows[1].info.pid==9 && rows[2].info.pid==0xA5A5A5A5u);
    CHECK(rows[0].heap_bytes==4096 && rows[0].info.comm[31]==0);
    offset=-10;CHECK(process_monitor_snapshot(rows,1,&offset,&total)==1 && offset==0 && rows[0].info.pid==2);
    retire();offset=500;CHECK(process_monitor_snapshot(rows,2,&offset,&total)==0 && !offset && !total);
    CHECK(process_monitor_snapshot(NULL,1,&offset,&total)==0);
    CHECK(process_monitor_snapshot(rows,0,&offset,&total)==0);
    CHECK(process_monitor_snapshot(rows,1,NULL,&total)==0);
    CHECK(process_monitor_snapshot(rows,1,&offset,NULL)==0);
    process_t *p=add(UINT32_MAX,0);p->program_break=UINT64_MAX;p->heap_start=0;offset=0;
    CHECK(process_monitor_snapshot(rows,1,&offset,&total)==1 && rows[0].heap_bytes==UINT64_MAX);
    p->program_break=1;p->heap_start=2;
    CHECK(process_monitor_snapshot(rows,1,&offset,&total)==1 && rows[0].heap_bytes==0);
    memset(p->comm,'x',32);CHECK(process_monitor_snapshot(rows,1,&offset,&total)==1);
    CHECK(rows[0].info.comm[30]=='x' && rows[0].info.comm[31]==0);
    reset();for(int i=1;i<=MAX_PROCESSES;i++)add((uint32_t)i,0);
    tm.scroll_offset=MAX_PROCESSES;taskman_win_draw(&win,0,0,520,430);
    CHECK(tm.process_total==MAX_PROCESSES && tm.scroll_offset<MAX_PROCESSES);
    int old=tm.scroll_offset;taskman_win_key(&win,KEY_UP);CHECK(tm.scroll_offset==old-1);
    retire();add(3,0);taskman_win_draw(&win,0,0,520,430);
    CHECK(tm.process_total==1 && tm.scroll_offset==0 && strstr(rendered,"process-3"));
#endif
    reset();printf("process monitor: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    process = (args.source_root/'kernel/proc/process.c').read_text()
    kernel = (args.source_root/'kernel/core/kernel.c').read_text()
    gui = (args.source_root/'kernel/gui/apps/taskman_win.c').read_text()
    header = (ROOT/'kernel/core/kernel.h').read_text()
    context = (ROOT/'kernel/gui/apps/taskman_win.h').read_text()
    types = '\n'.join(re.search(r'typedef struct \{[^{}]+\} '+name+r';',header)[0]
                      for name in ('process_info_t','process_monitor_info_t'))
    code = PREFIX.replace('@TYPES@',types).replace('@CONTEXT@',re.search(r'typedef struct \{[^{}]+\} taskman_win_t;',context)[0])
    for sig in ('static void process_copy_info(', 'int process_snapshot(', 'int process_jobs_snapshot(', 'int process_monitor_snapshot('):
        if sig in process:code += function(process,sig)
    code += '\n#define HAS_MONITOR_SNAPSHOT '+str(int('int process_monitor_snapshot(' in process))+'\n'
    code += function(gui,'static const char* state_str(')+function(gui,'void taskman_win_draw(')+function(gui,'void taskman_win_key(')
    code += '\n#define HAS_JOBS_SNAPSHOT '+str(int('int process_jobs_snapshot(' in process))+'\n'
    for command in ('ps','jobs','pgrep'):
        code += function(kernel,'static void cmd_'+command+'(int argc, char** argv) {').replace('printf(', 'output(')
    code += TESTS
    with tempfile.TemporaryDirectory(prefix='nyx-process-monitor-') as temp:
        path=Path(temp);(path/'test.c').write_text(code)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)


if __name__=='__main__':
    main()
