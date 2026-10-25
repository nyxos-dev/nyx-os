#!/usr/bin/env python3
"""Actual socket ownership and dup/dup2 bodies; mocked transport, heap and locks.

Linux/WSL host test. This does not exercise shared-FD-table concurrency or a
close racing an in-flight blocking operation; those need separate ownership work.
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
typedef int spinlock_t;
#define SPINLOCK_INIT 0
static int locked, live, fail_alloc, child=-1, closes[64], polls, sleeps;
static uint64_t spin_lock_irqsave(spinlock_t* p){if(locked||*p)abort();locked=*p=1;return 7;}
static void spin_unlock_irqrestore(spinlock_t* p,uint64_t f){if(!locked||!*p||f!=7)abort();locked=*p=0;}
static void* kmalloc(size_t n){if(!locked)abort();if(fail_alloc)return NULL;live++;return malloc(n);}
static void kfree(void* p){if(!locked||!p||live<=0)abort();live--;free(p);}
static int tcp_listen(int port){if(!locked||!port)abort();return 7;}
static int tcp_accept(int listener){if(!locked||listener!=7)abort();int c=child;child=-1;return c;}
static void tcp_close(int c){if(!locked||c<0||c>=64)abort();closes[c]++;}
static void kernel_poll_net(void){if(locked)abort();polls++;}
static void sleep(int ms){if(locked||ms!=1)abort();sleeps++;}
'''

FD_PREFIX = r'''
typedef struct {
    uint8_t ufd_inuse[PROC_MAX_FDS];
    int ufd_handle[PROC_MAX_FDS];
    uint32_t ufd_offset[PROC_MAX_FDS];
} process_t;
static process_t proc;
static int absent, pipe_refs[8], file_refs[8], wrong_vfs;
static process_t* fd_owner(void){return absent?NULL:&proc;}
static void pipe_incref(int id,int write){(void)write;pipe_refs[id]++;}
static void pipe_close_end(int id,int write){(void)write;pipe_refs[id]--;}
static void vfs_dup(int id){if(id<0||id>=8){wrong_vfs++;return;}file_refs[id]++;}
static int vfs_close(int id){if(id<0||id>=8){wrong_vfs++;return -1;}file_refs[id]--;return 0;}
'''

TESTS = r'''
static int open_socket(int type){return (int)run_socket(2,type,0);}
static int duplicate(int fd){return (int)run_dup(fd,0,0);}
static int replace(int a,int b){return (int)run_dup2(a,b,0);}
static int close_fd(int fd){return (int)run_close(fd,0,0);}
static int bind_fd(int fd){return (int)run_bind(fd,0,19001);}
static void reset(void){
    for(int i=0;i<MAX_SOCKETS;i++)while(nsock_valid(i))nsock_close(i);
    CHECK(live==0&&!locked);
    memset(&proc,0,sizeof(proc));memset(closes,0,sizeof(closes));
    memset(pipe_refs,0,sizeof(pipe_refs));memset(file_refs,0,sizeof(file_refs));
    wrong_vfs=absent=fail_alloc=polls=sleeps=0;child=-1;
}
int main(void){
    reset();CHECK(nsock_close(-1)==-1);CHECK(nsock_close(MAX_SOCKETS)==-1);
    CHECK(duplicate(3)==-1);CHECK(replace(3,4)==-1);CHECK(close_fd(3)==-1);
    for(int type=SOCK_STREAM;type<=SOCK_DGRAM;type++){
        reset();int a=open_socket(type),b=duplicate(a);
        CHECK(a==3&&b==4);CHECK(close_fd(a)==0);CHECK(bind_fd(b)==0);
        CHECK(close_fd(b)==0);CHECK(live==0);CHECK(!wrong_vfs);

        reset();a=open_socket(type);b=duplicate(a);close_fd(a);
        int c=open_socket(type);close_fd(b);CHECK(bind_fd(c)==0);
        close_fd(c);CHECK(live==0);

        reset();a=open_socket(type);b=duplicate(a);
        CHECK(replace(a,b)==b);CHECK(bind_fd(a)==0);CHECK(bind_fd(b)==0);
        CHECK(replace(a,a)==a);close_fd(b);CHECK(bind_fd(a)==0);close_fd(a);
        CHECK(live==0);

        reset();a=open_socket(type);b=open_socket(type);
        CHECK(replace(a,b)==b);CHECK(bind_fd(a)==0);CHECK(bind_fd(b)==0);
        close_fd(a);CHECK(bind_fd(b)==0);close_fd(b);CHECK(live==0&&!wrong_vfs);

        reset();a=open_socket(type);b=open_socket(type);
        CHECK(replace(-1,b)==-1);CHECK(replace(a,-1)==-1);
        CHECK(replace(a,PROC_MAX_FDS)==-1);CHECK(bind_fd(a)==0&&bind_fd(b)==0);
        close_fd(a);close_fd(b);CHECK(live==0);

        reset();a=open_socket(type);
        for(int i=4;i<PROC_MAX_FDS;i++)CHECK(duplicate(a)==i);
        CHECK(duplicate(a)==-1);CHECK(open_socket(type)==-1);CHECK(bind_fd(a)==0);
        for(int i=3;i<PROC_MAX_FDS;i++)close_fd(i);
        CHECK(live==0);

        reset();int completed=0;
        for(int i=0;i<80;i++){
            a=open_socket(type);b=open_socket(type);
            if(a<0||b<0){if(a>=0)close_fd(a);if(b>=0)close_fd(b);break;}
            CHECK(replace(a,b)==b);close_fd(a);close_fd(b);completed++;
        }
        CHECK(completed==80);CHECK(live==0&&!wrong_vfs);
    }
    reset();int a=open_socket(SOCK_DGRAM);pipe_refs[1]=1;
    int b=ufd_alloc(UFD_PIPE_MAKE(1,1));CHECK(replace(a,b)==b);CHECK(pipe_refs[1]==0);
    close_fd(a);close_fd(b);CHECK(live==0);
    reset();a=open_socket(SOCK_DGRAM);file_refs[1]=1;b=ufd_alloc(1);
    CHECK(replace(a,b)==b);CHECK(file_refs[1]==0);close_fd(a);close_fd(b);CHECK(live==0);
    reset();a=open_socket(SOCK_DGRAM);pipe_refs[1]=1;b=ufd_alloc(UFD_PIPE_MAKE(1,1));
    CHECK(replace(b,a)==a);CHECK(live==0&&pipe_refs[1]==2&&!wrong_vfs);
    close_fd(a);close_fd(b);CHECK(pipe_refs[1]==0);
    reset();a=open_socket(SOCK_DGRAM);file_refs[1]=1;b=ufd_alloc(1);
    CHECK(replace(b,a)==a);CHECK(live==0&&!wrong_vfs);
    CHECK(!proc.ufd_inuse[b]);close_fd(a);CHECK(file_refs[1]==0);
    reset();fail_alloc=1;CHECK(open_socket(SOCK_DGRAM)==-1);CHECK(live==0);
    fail_alloc=0;CHECK(open_socket(SOCK_DGRAM)==3);close_fd(3);
    reset();absent=1;CHECK(open_socket(SOCK_DGRAM)==-1);CHECK(live==0);absent=0;

    // Accepted sockets need their initial owner too; only the last alias closes TCP.
    reset();a=nsock_create(2,SOCK_STREAM,0);nsock_bind(a,0,19001);nsock_listen(a,1);
    child=8;b=nsock_accept(a);CHECK(b>=0&&b!=a);
    int fd=ufd_alloc(UFD_SOCK_MAKE(b)),alias=duplicate(fd);
    close_fd(fd);CHECK(closes[8]==0);CHECK(bind_fd(alias)==0);
    close_fd(alias);CHECK(closes[8]==1);nsock_close(a);CHECK(closes[7]==1);
    // An accepted TCP child must not leak when all 32 socket slots are occupied.
    reset();a=nsock_create(2,SOCK_STREAM,0);nsock_bind(a,0,19001);nsock_listen(a,1);
    for(int i=1;i<MAX_SOCKETS;i++)CHECK(nsock_create(2,SOCK_DGRAM,0)==i);
    CHECK(nsock_create(2,SOCK_STREAM,0)==-1);child=8;CHECK(nsock_accept(a)==-1);
    CHECK(closes[8]==1);CHECK(!polls&&!sleeps);reset();
    printf("%u socket ownership checks; %u failures\n",checks,failures);
    return failures?1:0;
}
'''


def run(ref=None):
    def read(path):
        return (subprocess.check_output(['git','show',f'{ref}:{path}'],cwd=ROOT,text=True)
                if ref else (ROOT/path).read_text(encoding='utf-8'))
    net,sc,header=map(read,['kernel/net/net.c','kernel/core/syscall.c','kernel/core/kernel.h'])
    table=net[net.index('#define MAX_SOCKETS'):net.index('extern int rtl8139_init')]
    macros='\n'.join(line for line in (header+'\n'+sc).splitlines() if re.match(r'#define (?:PROC_MAX_FDS|UFD_)',line))
    code=PREFIX+table+'\n'
    for signature in ['static int nsock_valid(', 'int nsock_create(', 'int nsock_retain(',
                      'int nsock_close(', 'int nsock_bind(', 'int nsock_listen(', 'int nsock_accept(']:
        if signature in net:code+=function(net,signature)+'\n'
    code+=macros+'\n'+FD_PREFIX
    for signature in ['static int ufd_alloc(', 'static int ufd_lookup(', 'static void ufd_release(']:
        code+=function(sc,signature)+'\n'
    for name in ['socket','close','bind','dup','dup2']:
        block=function(sc,'case SYS_'+name.upper()+':')
        code+=f'\nstatic long run_{name}(long a1,long a2,long a3) {{ (void)a1;(void)a2;(void)a3;\n'+block[block.index('{')+1:]+'\n'
    code+=TESTS
    with tempfile.TemporaryDirectory(prefix='nyx-socket-owners-') as tmp:
        folder=Path(tmp);source=folder/'test.c';binary=folder/'test'
        source.write_text(code,encoding='utf-8')
        subprocess.run(['gcc','-O1','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',str(source),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],timeout=20)
        return result.returncode


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref',help='Require the older source to reproduce failures')
    args=parser.parse_args();failed=run()
    if args.compare_ref and run(args.compare_ref)==0:raise SystemExit('Baseline unexpectedly passed')
    raise SystemExit(bool(failed))
