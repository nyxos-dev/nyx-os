#!/usr/bin/env python3
"""Exercise actual VFS open/flush and node layout with recording mount callbacks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define MAX_NAME 64
#define MAX_PATH 256
#define MAX_CHILDREN 128
#define VFS_MOUNT_DIRENTS 64
@NODE@
typedef struct { char name[128]; } dirent_t;
typedef struct {
    const char *mount_point;
    int (*readdir)(const char*,dirent_t*,int);
    uint32_t (*resolve)(const char*);
    int (*write_file)(const char*,const void*,uint32_t);
    uint32_t (*get_size)(const char*);
    int (*read_file)(const char*,void*,uint32_t);
} mount_entry_t;
static vfs_node_t node;
static int callbacks,allocs,live,fail_node,fail_alloc,isdir,exists;
static char written[512],cwd[256];
static void* kmalloc(size_t n) { allocs++;if(fail_alloc)return NULL;void* p=malloc(n);if(!p)abort();live++;return p; }
static void kfree(void* p) { if(p){free(p);live--;} }
static vfs_node_t* alloc_node(void) { if(fail_node)return NULL;memset(&node,0,sizeof(node));return &node; }
static void free_node(vfs_node_t* n) { (void)n; }
static int node_to_handle(vfs_node_t* n) { return n==&node?1:-1; }
static void proc_sync(void) {}
static vfs_node_t* resolve_path(const char* p) { (void)p;return NULL; }
static vfs_node_t* resolve_parent(const char* p,char* n) { (void)p;(void)n;return NULL; }
static int vfs_append_child(vfs_node_t* p,vfs_node_t* n) { (void)p;(void)n;return -1; }
static int disk_dir(const char* p,dirent_t* d,int n) { (void)p;(void)d;(void)n;callbacks++;return isdir?0:-1; }
static uint32_t disk_resolve(const char* p) { (void)p;callbacks++;return exists; }
static int disk_write(const char* p,const void* data,uint32_t n) { (void)data;callbacks++;strcpy(written,p);return n; }
static uint32_t disk_size(const char* p) { (void)p;callbacks++;return 2; }
static int disk_read(const char* p,void* data,uint32_t n) { (void)p;callbacks++;if(n!=2)abort();memcpy(data,"OK",2);return 2; }
static mount_entry_t mount={"/mnt",disk_dir,disk_resolve,disk_write,disk_size,disk_read};
static mount_entry_t* vfs_find_mount(const char* p) { return p && !strncmp(p,"/mnt",4) && (p[4]=='/' || !p[4])?&mount:NULL; }
static const char* vfs_getcwd(void) { return cwd; }
@JOIN@
@ABS@
@OPEN@
@FLUSH@
static int checks,failures;
#define CHECK(x) do { checks++;if(!(x)){failures++;if(failures<8)fprintf(stderr,"line %d: %s\n",__LINE__,#x);} } while(0)
static void reset(void) {
    kfree(node.data);memset(&node,0,sizeof(node));CHECK(live==0);
    callbacks=allocs=fail_node=fail_alloc=isdir=exists=0;written[0]=0;strcpy(cwd,"/");
}
int main(void) {
    char path[400];
    for(int len=1;len<=251;len++)for(int directory=0;directory<2;directory++) {
        reset();strcpy(path,"/mnt/");memset(path+5,'x',len-1);path[4+len]=0;
        isdir=directory;exists=len%2;
        CHECK(vfs_open(path,1,0644)==1);
        CHECK(!strcmp(node.mpath,path+4));
        CHECK(node.open_refs==1 && node.mount_backed && node.type==(unsigned)directory);
        if(!directory) {
            node.dirty=1;flush_mount_node(&node);
            CHECK(!strcmp(written,path+4) && node.dirty==0);
        }
    }
    reset();CHECK(vfs_open(NULL,1,0644)==-1);CHECK(callbacks==0 && allocs==0);
    for(int len=256;len<300;len++) {
        reset();strcpy(path,"/mnt/");memset(path+5,'x',len-5);path[len]=0;
        CHECK(vfs_open(path,1,0644)==-1);CHECK(callbacks==0 && allocs==0);
    }
    reset();strcpy(cwd,"/mnt/parent");
    CHECK(vfs_open("../file",1,0644)==1);CHECK(!strcmp(node.mpath,"/file"));
    node.dirty=1;flush_mount_node(&node);CHECK(!strcmp(written,"/file"));
    reset();strcpy(cwd,"/mnt/parent");memset(path,'x',243);path[243]=0;
    CHECK(vfs_open(path,1,0644)==1);CHECK(strlen(node.mpath)==251);
    reset();strcpy(cwd,"/mnt/parent");memset(path,'x',244);path[244]=0;
    CHECK(vfs_open(path,1,0644)==-1);CHECK(callbacks==0 && allocs==0);
    for(int dir=0;dir<2;dir++) {
        reset();isdir=dir;fail_node=1;CHECK(vfs_open("/mnt/new",1,0644)==-1);CHECK(live==0 && !written[0]);
    }
    reset();exists=1;CHECK(vfs_open("/mnt",0,0)==1);CHECK(!strcmp(node.mpath,"/"));
    reset();fail_alloc=1;fail_node=1;CHECK(vfs_open("/mnt/new",1,0)==-1);CHECK(!written[0] && live==0);
    reset();printf("VFS mount paths: %d checks, %d failures; node size %zu bytes\n",checks,failures,sizeof(node));
    return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-mount-paths-') as temp:
        work = Path(temp)
        def suite(ref=None):
            source = (subprocess.check_output(['git', 'show', ref + ':kernel/fs/vfs.c'], text=True)
                      if ref else (ROOT / 'kernel/fs/vfs.c').read_text(encoding='utf-8'))
            a=source.index('typedef struct vfs_node {');b=source.index('} vfs_node_t;',a)+len('} vfs_node_t;')
            code=HARNESS.replace('@NODE@',source[a:b])
            for marker,signature in [('@JOIN@','static void join_mount_relative('),
                                     ('@ABS@','static const char* vfs_abs(const char* path, char* buf, int n) {'),
                                     ('@OPEN@','int vfs_open('),('@FLUSH@','static void flush_mount_node(')]:
                code=code.replace(marker,function(source,signature))
            path,exe=work/'test.c',work/'test';path.write_text(code,encoding='utf-8')
            subprocess.run(['gcc','-std=c99','-O1','-Wall','-Wextra','-Werror','-no-pie',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',str(exe)],check=True)
            return subprocess.run([str(exe)],text=True,capture_output=True)
        result=suite();print(result.stdout,end='');assert result.returncode==0 and not result.stderr,result.stderr
        if args.compare_ref:
            result=suite(args.compare_ref);assert result.returncode==1,result
            assert 'node.mpath' in result.stderr and 'written' in result.stderr,result.stderr
            print('Baseline: '+result.stdout.strip())


if __name__=='__main__':
    main()
