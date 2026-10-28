#!/usr/bin/env python3
"""Exercise getprocs copyout and table retirement with the actual kernel bodies.

The lock, allocation and user-memory operations are modelled on the host.
This checks snapshot ownership and the 48-byte ABI, not general SMP safety.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_process_reap import function
from test_socket_return import body

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PROCESSES 512
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
@TYPE@
typedef struct { uint32_t pid,ppid,state,cpu_time,reap_claimed; char comm[32]; } process_t;
static process_t *process_table[MAX_PROCESSES];
static int process_count, sched_lock, locked, locks, allocs, frees, copies;
static int no_memory, bad_pointer, copy_fault, retire_on_copy;
static uint64_t a1,a2;
static process_info_t output[MAX_PROCESSES];
static uint64_t spin_lock_irqsave(int *lock) {
    CHECK(lock==&sched_lock && !locked); locked=1; locks++; return 71;
}
static void spin_unlock_irqrestore(int *lock,uint64_t flags) {
    CHECK(lock==&sched_lock && locked && flags==71); locked=0;
}
static void memset_asm(void *p,int c,size_t n) { memset(p,c,n); }
static void *kmalloc(size_t n) {
    CHECK(!locked && n>0 && n<=sizeof(output));
    if(no_memory)return NULL;
    allocs++; void *p=malloc(n); if(!p)abort(); memset(p,0xA5,n); return p;
}
static void kfree(void *p) { CHECK(!locked && p); frees++; free(p); }
static int user_ptr_ok(uint64_t ptr,uint64_t size) {
    CHECK(!locked && ptr==(uintptr_t)output && size<=sizeof(output)); return !bad_pointer;
}
static void retire_table(void) {
    CHECK(!locked);
    for(int i=0;i<process_count;i++) { free(process_table[i]); process_table[i]=NULL; }
    process_count=0;
}
static int copy_to_user(uint64_t dst,const void *src,size_t size) {
    CHECK(!locked && size==sizeof(process_info_t));
    copies++;
    if(retire_on_copy && copies==1)retire_table();
    if(copy_fault==copies)return -1;
    memcpy((void*)(uintptr_t)dst,src,size); return 0;
}
static void add(uint32_t pid,const char *name,int claimed) {
    process_t *p=calloc(1,sizeof(*p)); if(!p)abort();
    p->pid=pid;p->ppid=pid+100;p->state=pid%5;p->cpu_time=pid*3;p->reap_claimed=claimed;
    strncpy(p->comm,name,sizeof(p->comm));process_table[process_count++]=p;
}
static void reset(void) {
    retire_table(); CHECK(allocs==frees && !locked);
    allocs=frees=copies=locks=no_memory=bad_pointer=copy_fault=retire_on_copy=0;
    memset(output,0xCC,sizeof(output)); a1=(uintptr_t)output;a2=MAX_PROCESSES;
}
'''
TESTS = r'''
static void record(int index,uint32_t pid,const char *name) {
    process_info_t *p=&output[index];
    CHECK(p->pid==pid && p->ppid==pid+100 && p->state==pid%5 && p->cpu_time==pid*3);
    CHECK(!strncmp(p->comm,name,31));
    size_t end=strlen(name);if(end>31)end=31;
    for(size_t j=end;j<32;j++)CHECK(p->comm[j]==0);
}
int main(void) {
    CHECK(sizeof(process_info_t)==48);
    reset();add(1,"first-long-process-name",0);add(2,"x",0);
    CHECK(run()==2);record(0,1,"first-long-process-name");record(1,2,"x");
    CHECK(((unsigned char*)&output[2])[0]==0xCC);
    reset();add(1,"before",0);add(2,"after",0);retire_on_copy=1;
    CHECK(run()==2);record(0,1,"before");record(1,2,"after");
    reset();process_table[process_count++]=NULL;
    add(0,"unpublished",0);add(3,"claimed",1);add(4,"live",0);
    CHECK(run()==1);record(0,4,"live");
    reset();add(5,"abcdefghijklmnopqrstuvwxyz012345",0);a2=1;
    CHECK(run()==1);record(0,5,"abcdefghijklmnopqrstuvwxyz012345");
    reset();add(6,"first",0);add(7,"second",0);copy_fault=2;
    CHECK(run()==1);record(0,6,"first");CHECK(output[1].pid==0xCCCCCCCCu);
    reset();add(6,"first",0);copy_fault=1;
    CHECK(run()==0 && output[0].pid==0xCCCCCCCCu && allocs==frees);
    reset();add(8,"kept",0);no_memory=1;
    CHECK(run()==-1 && copies==0 && process_count==1);
    reset();bad_pointer=1;CHECK(run()==-1 && allocs==0 && copies==0);
    reset();a2=0;CHECK(run()==-1 && allocs==0 && copies==0);
    reset();a2=(uint64_t)-1;CHECK(run()==-1 && allocs==0 && copies==0);
    reset();CHECK(run()==0 && copies==0);
    reset();for(int i=1;i<=MAX_PROCESSES;i++)add((uint32_t)i,"p",0);
    a2=MAX_PROCESSES+1;CHECK(run()==MAX_PROCESSES);
    record(0,1,"p");record(MAX_PROCESSES-1,MAX_PROCESSES,"p");
    CHECK(locks==1);
    reset();printf("process snapshot: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''

TREE_TESTS = r'''
#include <stdarg.h>
#define PROC_ZOMBIE 2
#define PSTREE_MAXD 32
static char printed[16384];
static int retire_before_render, nested_render, renders;
static void cmd_pstree(int argc, char** argv);
@RENDER@
static void pstree_render(const int* pid, const int* ppid, const char* const* name,
                          int n, char* out, int cap) {
    CHECK(!locked); renders++;
    if (retire_before_render) { retire_before_render=0; retire_table(); }
    if (nested_render) {
        nested_render=0; add(99,"nested",0); process_table[0]->ppid=0;
        cmd_pstree(0,NULL); retire_table();
    }
    actual_pstree_render(pid,ppid,name,n,out,cap);
}
static int tree_printf(const char* fmt, ...) {
    CHECK(!locked);
    va_list ap; va_start(ap,fmt);
    size_t offset=strlen(printed);
    int result=vsnprintf(printed+offset,sizeof(printed)-offset,fmt,ap);
    va_end(ap);return result;
}
#define printf tree_printf
@COMMAND@
#undef printf
static void tree_reset(void) {
    reset(); printed[0]=0; retire_before_render=nested_render=renders=0;
}
static void node(uint32_t pid,uint32_t parent,const char* name,int state,int claimed) {
    add(pid,name,claimed);process_table[process_count-1]->ppid=parent;
    process_table[process_count-1]->state=state;
}
int main(void) {
    tree_reset();node(1,0,"init",1,0);node(2,1,"child",1,0);node(3,2,"grandchild",1,0);
    node(4,1,"zombie",PROC_ZOMBIE,0);node(5,1,"claimed",1,1);
    retire_before_render=1;cmd_pstree(0,NULL);
    CHECK(!strcmp(printed,"init(1)\n  child(2)\n    grandchild(3)\n"));
    CHECK(process_count==0 && allocs==frees && locks==1);
    tree_reset();node(10,0,"outer",1,0);retire_before_render=nested_render=1;
    cmd_pstree(0,NULL);CHECK(!strcmp(printed,"nested(99)\nouter(10)\n"));
    CHECK(allocs==frees && renders==2);
    tree_reset();node(1,0,"untouched",1,0);no_memory=1;cmd_pstree(0,NULL);
    CHECK(!strcmp(printed,"pstree: out of memory\n") && !locks && !renders);
    CHECK(process_count==1 && !strcmp(process_table[0]->comm,"untouched"));
    tree_reset();cmd_pstree(0,NULL);CHECK(!printed[0] && allocs==frees);
    tree_reset();node(1,0,"abcdefghijklmnopqrstuvwxyz012345",1,0);
    retire_before_render=1;cmd_pstree(0,NULL);
    CHECK(!strcmp(printed,"abcdefghijklmnopqrstuvwxyz01234(1)\n"));
    tree_reset();node(1,999,"orphan",1,0);node(2,1,"child",1,0);cmd_pstree(0,NULL);
    CHECK(!strcmp(printed,"orphan(1)\n  child(2)\n"));
    tree_reset();node(1,2,"cycle1",1,0);node(2,1,"cycle2",1,0);cmd_pstree(0,NULL);
    CHECK(!printed[0]);
    tree_reset();
    for(int i=1;i<=MAX_PROCESSES;i++)node(i,i-1,"deep",1,0);
    retire_before_render=1;cmd_pstree(0,NULL);
    CHECK(strstr(printed,"deep(33)\n") && !strstr(printed,"deep(34)\n"));
    tree_reset();
    for(int i=1;i<=MAX_PROCESSES;i++)node(i,0,"abcdefghijklmnopqrstuvwxyz012345",1,0);
    retire_before_render=1;cmd_pstree(0,NULL);
    CHECK(strlen(printed)==4095 && allocs==frees);
    tree_reset();printf("process tree ownership: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source_root/'kernel/proc/process.c').read_text()
    syscall = (args.source_root/'kernel/core/syscall.c').read_text()
    header = (args.source_root/'kernel/core/kernel.h').read_text()
    declaration = re.search(r'typedef struct \{[^{}]+\} process_info_t;', header)
    if declaration:
        record_type = declaration[0]
    else:
        user = (ROOT/'user/syscall.h').read_text()
        record_type = re.search(r'typedef struct \{[^{}]+\} nyx_procinfo_t;', user)[0].replace('nyx_procinfo_t', 'process_info_t')
    helper = function(source, 'static void process_copy_info(') if 'static void process_copy_info(' in source else ''
    helper += function(source, 'int process_snapshot(') if 'int process_snapshot(' in source else ''
    code = PREFIX.replace('@TYPE@', record_type) + helper
    code += '\nstatic long run(void) {\n' + body(syscall, 'case SYS_GETPROCS:') + '\n}\n' + TESTS
    with tempfile.TemporaryDirectory(prefix='nyx-process-snapshot-') as temp:
        path = Path(temp)
        (path/'test.c').write_text(code)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-fsanitize=address,undefined',
                        '-ftrivial-auto-var-init=pattern','-fno-omit-frame-pointer','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
        command_source = (args.source_root/'kernel/core/kernel.c').read_text()
        renderer = '\n'.join(function(command_source, name) for name in
                             ['static void pstree_emit(', 'static void pstree_walk(',
                              'static void pstree_render('])
        renderer = renderer.replace('static void pstree_render(', 'static void actual_pstree_render(')
        tree = PREFIX.replace('@TYPE@', record_type).replace('n<=sizeof(output)', 'n<=2*sizeof(output)')
        tree += helper + TREE_TESTS.replace('@RENDER@', renderer).replace(
            '@COMMAND@', function(command_source, 'static void cmd_pstree(int argc, char** argv) {'))
        (path/'tree.c').write_text(tree)
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-no-pie',
                        str(path/'tree.c'),'-o',str(path/'tree')],check=True)
        subprocess.run([str(path/'tree')],check=True)


if __name__ == '__main__':
    main()
