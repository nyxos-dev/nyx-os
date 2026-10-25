#!/usr/bin/env python3
"""Fault-inject actual VFS move/copy functions with RAM and recording mount fixtures.

The mount callbacks simulate disk errors; this is not a durability or SMP test.
"""
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_NAME 64
#define MAX_PATH 256
#define MAX_CHILDREN 128
#define MAX_INODES 512
#define SYMLINK_MAX 8
#define VFS_SYMLINK 2
#define VFS_MAX_FILE_SIZE 0x7FFFFFFFu
typedef struct { char name[64]; } dirent_t;
@NODE@
@MOUNT@
static vfs_node_t nodes[32], *current_dir;
static int used, alloc_calls, fail_alloc, live, fail_node, free_nodes;
static int source_exists, destination_exists, source_dir, read_error, size_error;
static int write_error, unlink_error, resolve_error, reads, writes, unlinks;
static int64_t destination_id;
static char disk_data[16];
static const char* cwd;
static void* kmalloc(size_t n) {
    if (++alloc_calls == fail_alloc) return NULL;
    void* p = malloc(n); if (!p) abort(); live++; return p;
}
static void kfree(void* p) { if (p) { live--; free(p); } }
static vfs_node_t* alloc_node(void) {
    if (fail_node || used == 32) return NULL;
    return &nodes[used++];
}
static void free_node(vfs_node_t* n) { n->data=NULL; free_nodes++; }
@TREE@
static int64_t mounted_resolve(const char* p) {
    if (resolve_error && !strcmp(p,"/target")) return -1;
    if (!strcmp(p,"/note") || !strcmp(p,"/alias")) return source_exists ? 2 : 0;
    if (!strcmp(p,"/target")) return destination_exists ? destination_id : 0;
    return 0;
}
static int64_t mounted_size(const char* p) { (void)p; return size_error ? -1 : 4; }
static int mounted_read(const char* p, void* b, uint32_t n) {
    (void)p; reads++; memcpy(b,"KEEP",n); return read_error ? (read_error==1 ? -1 : 3) : (int)n;
}
static int mounted_write(const char* p, const void* b, uint32_t n) {
    (void)p; writes++; destination_exists=1; memcpy(disk_data,b,n);
    return write_error ? (write_error==1 ? -1 : 3) : (int)n;
}
static int mounted_readdir(const char* p, dirent_t* d, uint32_t n) {
    (void)p; (void)d; (void)n; return source_dir ? 0 : -1;
}
static int mounted_unlink(const char* p) {
    (void)p; unlinks++; if (unlink_error) return -1; source_exists=0; return 0;
}
static mount_entry_t mount;
static mount_entry_t* vfs_find_mount(const char* p) {
    return !strncmp(p,"/mnt/",5) || !strcmp(p,"/mnt") ? &mount : NULL;
}
static const char* vfs_getcwd(void) { return cwd; }
static const char* vfs_abs(const char* p, char* b, int n) { (void)b; (void)n; return p; }
static int vfs_stat(const char* p, uint32_t* s, int* d) {
    (void)s; (void)d;
    if (vfs_find_mount(p)) return mounted_resolve(p+4)>0 ? 0 : -1;
    return resolve_path(p) ? 0 : -1;
}
static int vfs_unlink(const char* p) {
    if (vfs_find_mount(p)) return mounted_unlink(p+4);
    unlinks++;
    if (unlink_error) return -1;
    char name[MAX_NAME]; vfs_node_t* parent=resolve_parent(p,name);
    if (!parent) return -1;
    for (uint32_t i=0;i<parent->child_count;i++) if (!strcmp(parent->children[i]->name,name)) {
        for (uint32_t j=i;j+1<parent->child_count;j++) parent->children[j]=parent->children[j+1];
        parent->child_count--; return 0;
    }
    return -1;
}
@COPY@
@NORMALIZE@
@MOVE@
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static void reset(void) {
    for (int i=0;i<used;i++) if (nodes[i].data) kfree(nodes[i].data);
    if (live) abort();
    memset(nodes,0,sizeof nodes); used=7; current_dir=&nodes[1]; cwd="/ram";
    alloc_calls=fail_alloc=fail_node=free_nodes=0;
    source_exists=1; destination_exists=source_dir=read_error=size_error=0;
    write_error=unlink_error=resolve_error=reads=writes=unlinks=0; destination_id=3;
    memset(disk_data,0,sizeof disk_data);
    mount=(mount_entry_t){.resolve=mounted_resolve,.get_size=mounted_size,
        .read_file=mounted_read,.write_file=mounted_write,.readdir=mounted_readdir,.unlink=mounted_unlink};
    strcpy(mount.mount_point,"/mnt");
    nodes[0].type=nodes[1].type=nodes[2].type=nodes[5].type=nodes[6].type=1;
    nodes[0].parent=&nodes[0];
    strcpy(nodes[1].name,"ram"); strcpy(nodes[2].name,"dst");
    nodes[1].parent=nodes[2].parent=&nodes[0];
    vfs_append_child(&nodes[0],&nodes[1]); vfs_append_child(&nodes[0],&nodes[2]);
    nodes[3].parent=&nodes[1]; strcpy(nodes[3].name,"note"); vfs_append_child(&nodes[1],&nodes[3]);
    nodes[3].size=4; nodes[3].data=kmalloc(4); memcpy(nodes[3].data,"KEEP",4); alloc_calls=0;
    nodes[4].parent=&nodes[2]; strcpy(nodes[4].name,"existing"); vfs_append_child(&nodes[2],&nodes[4]);
    nodes[5].parent=&nodes[1]; strcpy(nodes[5].name,"folder"); vfs_append_child(&nodes[1],&nodes[5]);
    nodes[6].parent=&nodes[5]; strcpy(nodes[6].name,"inner"); vfs_append_child(&nodes[5],&nodes[6]);
}
int main(void) {
    const char* targets[][2]={{"/ram/note","/dst/new"},{"/mnt/note","/mnt/target"},
        {"/mnt/note","/dst/new"},{"/ram/note","/mnt/target"},{"/mnt/note","/mnt/note"}};
    for (int i=0;i<5;i++) {
        reset(); fail_alloc=1;
        CHECK(vfs_rename(targets[i][0],targets[i][1])==-1);
        CHECK(source_exists && nodes[3].parent==&nodes[1] && !writes && !unlinks && live==1);
    }
    const char* aliases[]={"/mnt/note","/mnt/./note","/mnt/a/../note","/mnt//note","./note","../mnt/note"};
    for(int i=0;i<6;i++) for(int j=0;j<6;j++) {
        reset(); cwd="/mnt"; CHECK(vfs_rename(aliases[i],aliases[j])==0);
        CHECK(source_exists && !reads && !writes && !unlinks);
        source_exists=0; CHECK(vfs_rename(aliases[i],aliases[j])==-1); CHECK(!reads && !writes && !unlinks);
    }
    reset(); CHECK(vfs_rename("/mnt/note","/mnt/alias")==0); CHECK(source_exists && !writes && !unlinks);
    reset(); destination_exists=1; CHECK(vfs_rename("/mnt/note","/mnt/target")==-1); CHECK(source_exists && !writes && !unlinks);
    reset(); resolve_error=1; CHECK(vfs_rename("/mnt/note","/mnt/target")==-1); CHECK(source_exists && !writes && !unlinks);
    for(int fault=0;fault<9;fault++) {
        reset(); read_error=fault==1?1:fault==2?2:0; size_error=fault==3; fail_alloc=fault==4?2:0;
        write_error=fault==5?1:fault==6?2:0; unlink_error=fault==7; source_dir=fault==8;
        CHECK(vfs_rename("/mnt/note","/mnt/target")== (fault ? -1 : 0));
        CHECK(source_exists==(fault!=0)); CHECK(unlinks==(fault==0 || fault==7)); CHECK(live==1);
        if (!fault) CHECK(!memcmp(disk_data,"KEEP",4));
    }
    for(int fault=0;fault<5;fault++) {
        reset(); fail_alloc=fault==1?2:fault==2?3:0; fail_node=fault==3;
        if(fault==4) { for(int i=1;i<MAX_CHILDREN;i++) nodes[2].children[i]=&nodes[4]; nodes[2].child_count=MAX_CHILDREN; }
        CHECK(vfs_rename("/mnt/note","/dst/new")== (fault ? -1 : 0));
        CHECK(source_exists==(fault!=0)); CHECK(unlinks==(fault==0));
        vfs_node_t* n=find_child(&nodes[2],"new");
        if(fault) { CHECK(!n); CHECK(live==1); }
        else { CHECK(n && n->size==4 && !memcmp(n->data,"KEEP",4)); CHECK(live==2); }
        if(fault==2 || fault==4) CHECK(free_nodes==1);
    }
    for(int fault=0;fault<4;fault++) {
        reset(); fail_alloc=fault==1?2:0; write_error=fault==2; unlink_error=fault==3;
        CHECK(vfs_rename("/ram/note","/mnt/target")== (fault ? -1 : 0));
        CHECK((find_child(&nodes[1],"note")!=NULL)==(fault!=0)); CHECK(live==1);
    }
    reset(); CHECK(vfs_rename("/ram/note","/dst/new")==0);
    CHECK(nodes[3].parent==&nodes[2] && !strcmp(nodes[3].name,"new") && !memcmp(nodes[3].data,"KEEP",4));
    const char* bad[]={"/missing/new","/dst/existing","/dst/existing/child","/", ""};
    for(int i=0;i<5;i++) { reset(); CHECK(vfs_rename("/ram/note",bad[i])==-1); CHECK(nodes[3].parent==&nodes[1] && !strcmp(nodes[3].name,"note")); }
    reset(); CHECK(vfs_rename("/ram/folder","/ram/folder/inner/new")==-1); CHECK(nodes[5].parent==&nodes[1]);
    reset(); nodes[2].parent=&nodes[6]; nodes[5].parent=&nodes[2];
    CHECK(vfs_rename("/ram/note","/dst/new")==-1); CHECK(nodes[3].parent==&nodes[1]);
    reset(); for(int i=1;i<MAX_CHILDREN;i++) nodes[2].children[i]=&nodes[4]; nodes[2].child_count=MAX_CHILDREN;
    CHECK(vfs_rename("/ram/note","/dst/new")==-1); CHECK(nodes[3].parent==&nodes[1] && !strcmp(nodes[3].name,"note"));
    reset(); CHECK(vfs_rename("note","./note")==0); CHECK(nodes[3].parent==&nodes[1]);
    char big[300]; memset(big,'a',sizeof big); big[299]=0;
    CHECK(vfs_rename(big,"/mnt/target")==-1); CHECK(vfs_rename("/mnt/note",big)==-1); CHECK(!writes && !unlinks);
    char component[80]="/mnt/"; memset(component+5,'a',64); component[69]=0;
    CHECK(vfs_rename("/mnt/note",component)==-1 && !writes && !unlinks);
    reset(); strcpy(component,"/dst/"); memset(component+5,'a',63); component[68]=0;
    CHECK(vfs_rename("/ram/note",component)==0); CHECK(strlen(nodes[3].name)==63);
    CHECK(resolve_path(component)==&nodes[3] && !memcmp(nodes[3].data,"KEEP",4));
    reset(); for(int i=0;i<used;i++) if(nodes[i].data) { kfree(nodes[i].data); nodes[i].data=NULL; } CHECK(!live);
    printf("VFS moves: %d checks, 0 failures\n",checks); return 0;
}
'''


def check_stack_budget(work):
    recipes = subprocess.check_output(
        ['make', '-Bn', '--no-print-directory', 'vfs.o', 'ext2.o'],
        cwd=ROOT/'kernel', text=True)
    budgets = {'vfs_rename': 256, 'ext2_sync_superblock': 128}
    frames = {}
    for line in recipes.splitlines():
        if not line.startswith('gcc ') or ' -c ' not in line:
            continue
        command = shlex.split(line)
        obj = work / Path(command[command.index('-o') + 1]).name
        command[command.index('-o') + 1] = str(obj)
        subprocess.run(command + ['-fstack-usage'], cwd=ROOT/'kernel', check=True)
        for row in obj.with_suffix('.su').read_text().splitlines():
            location, size, kind = row.split('\t')
            name = location.rsplit(':', 1)[-1]
            if name in budgets:
                frames[name] = int(size)
                assert kind == 'static' and int(size) <= budgets[name], row
    assert set(frames) == set(budgets), frames
    print('Move stack frame budgets:', frames)


def main():
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutations',action='store_true')
    args=parser.parse_args()
    source = (ROOT/'kernel/fs/vfs.c').read_text(encoding='utf-8')
    header = (ROOT/'kernel/core/kernel.h').read_text(encoding='utf-8')
    node = re.search(r'typedef struct vfs_node \{.*?\} vfs_node_t;', source, re.S)[0]
    mount = re.search(r'typedef struct \{\s+int type;\s+char mount_point\[MAX_PATH\];.*?\} mount_entry_t;', header, re.S)[0]
    tree = '\n'.join(function(source, sig) for sig in [
        'static int vfs_append_child(', 'static vfs_node_t* find_child(', 'static char* path_token(',
        'static vfs_node_t* resolve_parent('])
    tree += '\nstatic vfs_node_t* resolve_path_d(const char* path, int depth);\n'
    tree += '\n'.join(function(source, sig) for sig in ['static vfs_node_t* deref_link(',
        'static vfs_node_t* resolve_path_d(const char* path, int depth) {', 'static vfs_node_t* resolve_path(const'])
    parts = {'NODE': node, 'MOUNT': mount, 'TREE': tree, 'COPY': function(source, 'int vfs_cp('),
             'NORMALIZE': function(source, 'int vfs_normalize_path('),
             'MOVE': function(source, 'static int vfs_move_path(')+'\n'+function(source, 'static int vfs_move_resolved(')+'\n'+function(source, 'int vfs_rename(')}
    code = HARNESS
    for marker, value in parts.items(): code = code.replace('@'+marker+'@', value)
    with tempfile.TemporaryDirectory(prefix='nyx-vfs-moves-') as tmp:
        p=Path(tmp); (p/'test.c').write_text(code,encoding='utf-8')
        check_stack_budget(p)
        def run(candidate):
            (p/'test.c').write_text(candidate,encoding='utf-8')
            subprocess.run(['gcc','-std=gnu99','-O1','-Wall','-Wextra','-Werror','-no-pie',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=all',p/'test.c','-o',p/'test'],check=True)
            return subprocess.run([p/'test'],capture_output=True,text=True)
        result=run(code)
        assert result.returncode==0, result.stdout+result.stderr
        print(result.stdout,end='')
        if args.mutations:
            mutations=[
                ('RAM allocation success', '(!ssize || dst_ino->data) && ', ''),
                ('copy failure deletion', 'if (vfs_cp(old, new) < 0) return -1;', '(void)vfs_cp(old, new);'),
                ('existing target overwrite', 'if (dst_id > 0) return -1;', ''),
                ('ancestor cycle', 'if (ancestor == ino) return -1;', 'if (ancestor == ino) break;'),
            ]
            for label, before, after in mutations:
                assert code.count(before)==1, label
                result=run(code.replace(before,after))
                assert result.returncode!=0, label+' was not caught'
                print('Rejected mutation: '+label)


if __name__ == '__main__':
    main()
