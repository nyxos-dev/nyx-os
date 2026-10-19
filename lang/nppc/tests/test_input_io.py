#!/usr/bin/env python3
"""Check N++ source-reader failures, cleanup and main/import output preservation."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'lang/nppc/nppc.c'
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE* test_open(const char*,const char*);
static int test_seek(FILE*,long,int);
static long test_tell(FILE*);
static size_t test_read(void*,size_t,size_t,FILE*);
static int test_error(FILE*);
static int test_get(FILE*);
static int test_close(FILE*);
static void* test_calloc(size_t,size_t);
static void test_free(void*);
#define main compiler_main
#define fopen test_open
#define fseek test_seek
#define ftell test_tell
#define fread test_read
#define ferror test_error
#define fgetc test_get
#define fclose test_close
#define calloc test_calloc
#define free test_free
#include "@SOURCE@"
#undef main
#undef fopen
#undef fseek
#undef ftell
#undef fread
#undef ferror
#undef fgetc
#undef fclose
#undef calloc
#undef free
static int fault,opens,closes,seeks,allocs,live,late_error;
static size_t requested;
static FILE* test_open(const char *p,const char *m) { opens++; return fault==14?NULL:fopen(p,m); }
static int test_seek(FILE *f,long off,int whence) {
    seeks++;
    if ((fault==1 && seeks==1) || (fault==3 && seeks==2)) return -1;
    return fseek(f,off,whence);
}
static long test_tell(FILE *f) {
    if (fault==2) return -1;
    if (fault==9) return 2147483648L;
    if (fault==10) return 0x7fffffffffffffffL;
    if (fault==11) return -7;
    if (fault==12) return 2147483647L;
    return ftell(f);
}
static size_t test_read(void *p,size_t sz,size_t n,FILE *f) {
    if (fault==4 && n) return fread(p,sz,n-1,f);
    return fread(p,sz,n,f);
}
static int test_error(FILE *f) { return fault==5 || late_error || ferror(f); }
static int test_get(FILE *f) {
    if (fault==7) return 'x';
    if (fault==13) { late_error=1; return EOF; }
    return fgetc(f);
}
static int test_close(FILE *f) { closes++; int r=fclose(f); return fault==6?EOF:r; }
static void* test_calloc(size_t n,size_t sz) {
    allocs++; requested=n*sz;
    if (fault==8 || fault==12) return NULL;
    void *p=calloc(n,sz); if (p) live++; return p;
}
static void test_free(void *p) { if (p) live--; free(p); }
int main(int argc,char **argv) {
    if (argc!=3) return 2;
    fault=atoi(argv[1]); long size=12345;
    char *p=read_file(argv[2],&size);
    int bad=opens!=1 || closes!=(fault==14?0:1);
    if (fault) bad |= p!=NULL || size!=0 || live!=0;
    else {
        bad |= !p || size<0 || (p && p[size]!=0);
        if (p) test_free(p);
        bad |= live!=0;
    }
    if (fault==1 || fault==2 || fault==3 || fault==9 || fault==10 || fault==11 || fault==14)
        bad |= allocs!=0;
    if (fault==12) bad |= allocs!=1 || requested!=2147483648UL;
    if (bad) fprintf(stderr,"fault=%d opens=%d closes=%d allocs=%d live=%d size=%ld\n",fault,opens,closes,allocs,live,size);
    return bad?1:0;
}
'''

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args],capture_output=True,timeout=30,**kwargs)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args=parser.parse_args()
    cc=shlex.split(os.environ.get('CC','cc'))
    flags=['-std=c99','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    with tempfile.TemporaryDirectory(prefix='nyx-npp-read-') as temporary:
        work=Path(temporary); harness=work/'reader.c';harness.write_text(HARNESS.replace('@SOURCE@',str(SOURCE)))
        def compile(source,name,previous=False):
            selected=[f for f in flags if not (previous and f=="-Werror")]
            exe=work/name;r=run([*cc,*selected,source,'-o',exe]);assert r.returncode==0,r.stderr.decode();return exe
        reader=compile(harness,'reader');compiler=compile(SOURCE,'nppc')
        source=work/'valid.npp';source.write_text('fn main() -> i64 { 0 }\n')
        empty=work/'empty.npp';empty.write_bytes(b'')
        checks=0
        for fault in range(15):
            r=run([reader,fault,source]);assert r.returncode==0,(fault,r.stderr.decode());checks+=1
        r=run([reader,0,empty]);assert r.returncode==0,r.stderr.decode();checks+=1
        # nppc retains successful parse arenas for process lifetime; the focused
        # reader harness above keeps leak detection enabled for failed buffers.
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
        output=work/'output.n';sentinel=b'PRESERVE\n'
        for path in (source,empty):
            r=run([compiler,path,'-o',output],env=env);assert r.returncode==0,r.stderr.decode()
            assert output.read_bytes()==path.read_bytes();checks+=1
        imported=work/'import.npp';imported.write_text('use "/dev/stdin";\nfn main() -> i64 { 0 }\n')
        for path in ('/dev/stdin',imported,work,work/'missing.npp'):
            output.write_bytes(sentinel)
            r=run([compiler,path,'-o',output],input=b'pub fn helper() -> i64 { 0 }\n',env=env)
            assert r.returncode==1 and b'cannot read' in r.stderr,(path,r.returncode,r.stderr.decode())
            assert b'Sanitizer' not in r.stderr and output.read_bytes()==sentinel;checks+=1
        missing_module=work/'missing-module.npp';missing_module.write_text('use "missing.npp";\nfn main() -> i64 { 0 }\n')
        output.write_bytes(sentinel);r=run([compiler,missing_module,'-o',output],env=env)
        assert r.returncode==1 and b'cannot read' in r.stderr and output.read_bytes()==sentinel;checks+=1
        if args.compare_ref:
            old=run(['git','show',args.compare_ref+':lang/nppc/nppc.c']);assert old.returncode==0
            previous=work/'previous.c';previous.write_bytes(old.stdout);baseline=compile(previous,'baseline',previous=True)
            for path in ('/dev/stdin',imported):
                output.write_bytes(sentinel)
                r=run([baseline,path,'-o',output],input=b'pub fn helper() -> i64 { 0 }\n',env=env)
                assert r.returncode and b'heap-buffer-overflow' in r.stderr and output.read_bytes()==sentinel;checks+=1
            for path in (source,empty):
                r=run([baseline,path,'-o',output],env=env);assert r.returncode==0 and output.read_bytes()==path.read_bytes();checks+=1
        print(f'PASS {checks} N++ input scenarios; reader ASan/UBSan cleanup, main/import refusal and output preservation')

if __name__=='__main__': main()


