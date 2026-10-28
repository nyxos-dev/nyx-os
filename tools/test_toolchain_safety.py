#!/usr/bin/env python3
"""Exercise shipped compiler arithmetic/diagnostics and libc test failure paths."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]


def compiler_harness(root):
    gen = (root / 'user/tcc/tccgen.c').read_text()
    expressions = [
        re.search(r'c \+= ([^;]*index[^;]*elem_size);', gen)[1],
        re.search(r'c \+= ([^;]*index[^;]*type_size\(type, &align\));', gen)[1],
        re.search(r'vset\(type, VT_LOCAL\|VT_LVAL, (c \+ [^;]*elem_size[^;]*)\);', gen)[1],
        re.search(r'c_end = ([^;]+);', gen[gen.index('static int decl_designator('):])[1],
        re.search(r'c \+= ([^;]*nb_elems[^;]*type_size\(type, &align\));', gen)[1],
        re.search(r'init_putv\(t1, sec, (c \+ [^;]*size1)\);', gen)[1],
    ]
    expected = ['(uint64_t)index*elem_size', '(uint64_t)index*elem_size',
                'c+(uint64_t)elem_size*i', 'c+(uint64_t)nb_elems*elem_size',
                '(uint64_t)nb_elems*elem_size', 'c+((uint64_t)len+i)*size1']
    checks = '\n'.join(f'CHECK(({x}) == ({y}));' for x, y in zip(expressions, expected))
    diagnostics = []
    for path, pattern in [
        ('tccgen.c', r'tcc_error\("internal compiler error: vstack leak .*?;'),
        ('tccasm.c', r'tcc_error\("local label .*?not found backward.*?;'),
        ('tccelf.c', r'tcc_error_noabort\("Invalid relocation entry .*?;'),
        ('tccpp.c', r'tcc_warning\("pasting .*?;'),
    ]:
        diagnostics.append(re.search(pattern, (root/'user/tcc'/path).read_text(), re.S)[0])
    return r'''
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
static int checks, elem_size;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int type_size(void *t, int *a) { (void)t; (void)a; return elem_size; }
static char messages[4][256];
static int message_count;
static void diagnostic(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void diagnostic(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vsnprintf(messages[message_count++],256,fmt,ap); va_end(ap);
}
#define tcc_error diagnostic
#define tcc_error_noabort diagnostic
#define tcc_warning diagnostic
int main(void) {
    int values[] = {0, 1, 65536, INT_MAX/2, INT_MAX};
    for (unsigned a=0; a<sizeof(values)/sizeof(*values); a++) {
        for (unsigned b=1; b<sizeof(values)/sizeof(*values); b++) {
            int index=values[a], i=values[b], nb_elems=values[a], len=values[a];
            int size1=values[b], align=0; elem_size=values[b];
            unsigned long c=17; void *type=NULL;
            @CHECKS@
        }
    }
    int stack[8], *vtop=stack+7, *pvtop=stack;
    { @DIAG0@ }
    { uint64_t n=UINT64_C(4294967297); @DIAG1@ }
    { int i=3; char strsec[]=".text"; struct {unsigned sh_name;} h={0},*sh=&h;
      struct {uint64_t r_offset;} r={UINT64_C(4294967298)},*rel=&r; @DIAG2@ }
    { int n=2; struct {void *data;} cstr={(void*)"ab+"}; @DIAG3@ }
    CHECK(!strcmp(messages[0],"internal compiler error: vstack leak (7)"));
    CHECK(!strcmp(messages[1],"local label '4294967297' not found backward"));
    CHECK(strstr(messages[2],"100000002") != NULL);
    CHECK(!strcmp(messages[3],"pasting \"ab\" and \"+\" does not give a valid preprocessing token"));
    printf("Compiler arithmetic and diagnostics: %d checks\n",checks);
    return 0;
}
'''.replace('@CHECKS@', checks).replace('@DIAG0@', diagnostics[0]).replace('@DIAG1@', diagnostics[1]).replace('@DIAG2@', diagnostics[2]).replace('@DIAG3@', diagnostics[3])


def libc_harness(root):
    source = (root/'user/libctest.c').read_text()
    moves = '\n'.join(function(source, sig) for sig in
                      ['static int move_bytes(', 'static int move_absent(', 'static int test_moves('])
    allocation = source.split('    char* p = (char*)malloc(8);', 1)[1].split('    free(p);', 1)[0]
    allocation = 'char* p = (char*)malloc(8);' + allocation + '    free(p);'
    return r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { O_RDONLY=0, O_CREAT=1, O_TRUNC=2 };
enum { NORMAL, MKDIR_FAIL, OPEN_FAIL, SHORT_WRITE, SHORT_READ, CLOSE_FAIL,
       RENAME_FAIL, LEAVE_SOURCE, BAD_CONTENTS, BAD_COLLISION };
static int fault, exists[2], opened[2], dir_exists, descriptors, checks;
static char bytes[2][4];
#define CHECK(x) do { checks++; if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int slot(const char *p) { const char *s=strrchr(p,'/'); return s && !strcmp(s,"/a") ? 0 : s && !strcmp(s,"/b") ? 1 : -1; }
static int nyx_open(const char *p, int flags, int mode) {
    (void)mode; int i=slot(p); if (i<0 || fault==OPEN_FAIL) return -1;
    if(flags&O_CREAT) exists[i]=1;
    if(!exists[i]) return -1;
    CHECK(!opened[i]); opened[i]=1; descriptors++; return i;
}
static int nyx_close(int fd) { CHECK(opened[fd]); opened[fd]=0; descriptors--; return fault==CLOSE_FAIL ? -1 : 0; }
static long nyx_write(int fd, const void *p, long n) { CHECK(opened[fd]); memcpy(bytes[fd],p,4); return fault==SHORT_WRITE ? n-1 : n; }
static long nyx_read(int fd, void *p, long n) { CHECK(opened[fd] && n>=4); memcpy(p,bytes[fd],4); return fault==SHORT_READ ? 3 : 4; }
static int nyx_mkdir(const char *p,int m) { (void)p;(void)m; if(fault==MKDIR_FAIL)return -1; dir_exists=1;return 0; }
static int nyx_unlink(const char *p) { int i=slot(p); if(i>=0){exists[i]=0;return 0;} dir_exists=0;return 0; }
static int nyx_rename(const char *a,const char *b) {
    int s=slot(a),d=slot(b); if(fault==RENAME_FAIL || !exists[s])return -1;
    if(s==d)return 0;
    if(exists[d])return fault==BAD_COLLISION ? 0 : -1;
    memcpy(bytes[d],bytes[s],4); exists[d]=1;
    if(fault!=LEAVE_SOURCE)exists[s]=0;
    if(fault==BAD_CONTENTS)bytes[d][0]='!'; return 0;
}
#define open nyx_open
#define close nyx_close
#define read nyx_read
#define write nyx_write
#define mkdir nyx_mkdir
#define unlink nyx_unlink
#define rename nyx_rename
#define getpid() 42
@MOVES@
static char alloc[64]; static int alloc_fault, allocations, frees;
static void *nyx_malloc(unsigned long n) { CHECK(n==8); if(alloc_fault==1)return NULL; allocations++;return alloc; }
static void *nyx_realloc(void *p,unsigned long n) { CHECK(p==alloc && n==64); return alloc_fault==2 ? NULL : p; }
static void nyx_free(void *p) { if(p){CHECK(p==alloc);frees++;} }
#define malloc nyx_malloc
#define realloc nyx_realloc
#define free nyx_free
static int run_allocation(void) { int ok=1; @ALLOC@ return ok; }
int main(void) {
    for(fault=NORMAL; fault<=BAD_COLLISION;fault++) {
        memset(exists,0,sizeof(exists));memset(opened,0,sizeof(opened));descriptors=dir_exists=0;
        CHECK(test_moves()==(fault==NORMAL)); CHECK(descriptors==0 && !dir_exists);
    }
    for(alloc_fault=0;alloc_fault<3;alloc_fault++) {
        allocations=frees=0; CHECK(run_allocation()==(alloc_fault==0)); CHECK(allocations==frees);
    }
    printf("libctest failure paths: %d checks\n",checks); return 0;
}
'''.replace('@MOVES@', moves).replace('@ALLOC@', allocation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-toolchain-safety-') as tmp:
        for name, code in [('compiler', compiler_harness(args.source_root)),
                           ('libctest', libc_harness(args.source_root))]:
            src = Path(tmp)/f'{name}.c'
            exe = src.with_suffix('')
            src.write_text(code)
            subprocess.run(['gcc','-std=gnu99','-O1','-Wall','-Wextra','-Werror',
                            '-Wno-misleading-indentation','-Wformat=2','-no-pie',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                            str(src),'-o',str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
