#!/usr/bin/env python3
"""Check actual shell word splitting and expansion using recording directory input."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
def fn(s,sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
h=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_GLOB 32
typedef struct {char name[64];unsigned type;} nyx_dirent_t;
static nyx_dirent_t entries[64];
static int count;
static char asked[512];
static long getdents(const char* path,nyx_dirent_t* out,int max) {
 strcpy(asked,path);if(count<0)return count;
 if(count>max){abort();}
 memcpy(out,entries,count*sizeof(*out));return count;
}
@GLOBS@
@SPLIT@
static int checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;if(failures<10)fprintf(stderr,"line %d: %s\n",__LINE__,#x);}}while(0)
static void reset(int n) {count=n;asked[0]=0;memset(entries,0,sizeof(entries));for(int i=0;i<n;i++)snprintf(entries[i].name,64,"file%02d",i);}
int main(void) {
 char word[520],line[520];char* av[40];int ac;char** out;
 for(int len=1;len<=300;len++){
  reset(0);memset(word,'x',len);word[len]=0;av[0]="echo";av[1]=word;ac=2;
  out=expand_globs(av,&ac);
  if(len<=127){CHECK(out && ac==2);CHECK(out && !strcmp(out[1],word));}
  else CHECK(!out);
 }
 for(int len=90;len<=270;len++) {
  reset(0);memset(word,'x',len);word[len-1]='*';word[len]=0;av[0]=word;ac=1;
  out=expand_globs(av,&ac);CHECK(len<=127?(out && !strcmp(out[0],word)):!out);
 }
 for(int n=1;n<=34;n++) {
  reset(0);for(int i=0;i<n;i++)av[i]="word";ac=n;out=expand_globs(av,&ac);
  CHECK(n<=32?(out && ac==n && out[n]==0):!out);
 }
 for(int n=0;n<=64;n++) {
  reset(n);av[0]="echo";av[1]="file*";ac=2;out=expand_globs(av,&ac);
  CHECK(n<=31?(out && ac==(n?n+1:2)):!out);
  if(out && n && n<ac)CHECK(!strcmp(out[n],entries[n-1].name));
 }
 reset(31);av[0]="echo";av[1]="file*";av[2]="tail";ac=3;CHECK(!expand_globs(av,&ac));
 reset(64);av[0]="absent*";ac=1;CHECK(!expand_globs(av,&ac));
 for(int len=75;len<=130;len++) {
  reset(1);memset(word,'d',len);word[len]='/';word[len+1]='*';word[len+2]=0;
  av[0]=word;ac=1;out=expand_globs(av,&ac);
  CHECK(len<=125 ? strlen(asked)==(size_t)len : !asked[0]);
  if(len+1+6<=127)CHECK(out && strlen(out[0])==(size_t)(len+7) && !memcmp(out[0],word,len+1));
  else CHECK(!out);
 }
 reset(-1);av[0]="missing*";ac=1;out=expand_globs(av,&ac);CHECK(out && !strcmp(out[0],"missing*"));
 reset(2);strcpy(entries[0].name,".hidden");strcpy(entries[1].name,"shown");av[0]="*";ac=1;out=expand_globs(av,&ac);CHECK(out && ac==1 && !strcmp(out[0],"shown"));
 for(int n=0;n<=15;n++)for(int trailing=0;trailing<2;trailing++) {
  memset(line,0,sizeof(line));for(int i=0;i<n;i++)strcat(line,i?" x":"x");if(trailing)strcat(line,"   ");
  ac=split_args(line,av,12);CHECK(n<=11?(ac==n && av[n]==0):ac==-1);
 }
 printf("Shell expansion: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
'''
def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-sh-expand-') as td:
        p = Path(td)
        def suite(ref=None):
            source = (subprocess.check_output(['git', 'show', ref + ':user/sh.c'], text=True)
                      if ref else (ROOT / 'user/sh.c').read_text(encoding='utf-8'))
            a = source.index('#define MAX_GLOB 32')
            b = source.index('/* Run one command line', a)
            code = h.replace('@GLOBS@', source[a:b]).replace('@SPLIT@', fn(source, 'static int split_args('))
            (p / 'test.c').write_text(code, encoding='utf-8')
            subprocess.run(['gcc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(p / 'test.c'), '-o', str(p / 'test')], check=True)
            return subprocess.run([str(p / 'test')], text=True, capture_output=True)
        result = suite()
        print(result.stdout, end='')
        assert result.returncode == 0 and not result.stderr, result.stderr
        if args.compare_ref:
            result = suite(args.compare_ref)
            assert result.returncode == 1 and 'failures' in result.stdout, result
            assert 'runtime error:' not in result.stderr and 'AddressSanitizer' not in result.stderr, result.stderr
            print('Baseline: ' + result.stdout.strip())


if __name__ == '__main__':
    main()
