#!/usr/bin/env python3
"""Check actual syscall path copying and normalization with mapped-memory input."""
from pathlib import Path
import subprocess
import tempfile
r=Path(__file__).resolve().parents[1]
def fn(s,sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
h=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 256
static unsigned char memory[8192];
static uint64_t fault=UINT64_MAX;
static uint64_t user_v2p(uint64_t va) {
 if(va<0x10000 || va>=0x10000+sizeof(memory) || va==fault)return 0;
 return (uint64_t)(uintptr_t)(memory+va-0x10000);
}
typedef struct {char cwd[256];} process_t;
static process_t proc;
static process_t* get_cur_proc(void){return &proc;}
@COPY@
@CHECKEDCOPY@
@NORMALIZE@
@COPY_PATH@
@KAT@
@WRAPPER@
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;if(failures<7)fprintf(stderr,"line %d: %s\n",__LINE__,#x);}}while(0)
int main(void){
 char out[258],path[400],expected[400];strcpy(proc.cwd,"/");
 CHECK(path_resolve_selftest()==0);
 for(int n=0;n<=300;n++){
  memset(memory,'x',n);memory[n]=0;memset(out,'#',sizeof(out));
  int status=copy_path_from_user(out+1,256,0x10000);CHECK((status==0)==(n<=127));
  CHECK(out[0]=='#' && out[257]=='#');
  if(status==0){CHECK(strlen(out+1)==(size_t)(n?n+1:1));CHECK(out[1]=='/');}
  else CHECK(out[1]=='#');
 }
 memset(memory,'x',127);memory[127]=0;
 for(int i=0;i<=127;i++){fault=0x10000+i;memset(out,'#',sizeof(out));CHECK(copy_path_from_user(out,256,0x10000)==-1);CHECK(out[0]=='#');}
 fault=UINT64_MAX;
 for(int n=1;n<=40;n++){
  for(int i=0;i<n;i++){path[2*i]='/';path[2*i+1]='a';}path[2*n]=0;
  memset(out,'#',sizeof(out));int status=normalize("/",path,out,256);
  CHECK((status==0)==(n<=32));CHECK(status==0?!strcmp(out,path):out[0]=='#');
 }
 for(int clen=1;clen<=255;clen++)for(int plen=0;plen<=127;plen++){
  memset(proc.cwd,'d',clen);proc.cwd[0]='/';proc.cwd[clen]=0;
  memset(path,'x',plen);path[plen]=0;memset(out,'#',sizeof(out));
  int status=normalize(proc.cwd,path,out,256);CHECK((status==0)==(clen+1+plen<256));
  if(status==0){
   strcpy(expected,proc.cwd);if(plen){if(clen>1)strcat(expected,"/");strcat(expected,path);}
   CHECK(!strcmp(out,expected));
  }else CHECK(out[0]=='#');
 }
 for(int width=0;width<=256;width++){
  memset(out,'#',sizeof(out));int status=normalize("/","/a/b",out+1,width);
  CHECK((status==0)==(width>=5));CHECK(out[0]=='#' && out[257]=='#');
  CHECK(status==0?!strcmp(out+1,"/a/b"):out[1]=='#');
 }
 if(CHECKED){CHECK(normalize(NULL,NULL,out,256)==-1);CHECK(normalize("/","x",NULL,256)==-1);}
 printf("Syscall paths: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
'''
def main():
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-syscall-paths-') as td:
        p=Path(td)
        def suite(ref=None):
            source=(subprocess.check_output(['git','show',ref+':kernel/core/syscall.c'],text=True)
                    if ref else (r/'kernel/core/syscall.c').read_text(encoding='utf-8'))
            checked='static int path_resolve(' in source
            wrapper=('#define CHECKED 1\nstatic int normalize(const char*c,const char*p,char*o,int n){return path_resolve(c,p,o,n);}' if checked else '#define CHECKED 0\nstatic int normalize(const char*c,const char*p,char*o,int n){path_resolve(c,p,o,n);return 0;}')
            code=h.replace('@WRAPPER@',wrapper)
            for marker,sig in [('@COPY@','static int copy_str_from_user('),('@CHECKEDCOPY@','static int copy_exec_string('),('@NORMALIZE@',('static int' if checked else 'static void')+' path_resolve('),('@COPY_PATH@','static int copy_path_from_user('),('@KAT@','int path_resolve_selftest(')]:
                code=code.replace(marker,fn(source,sig))
            code=code.replace('static int copy_exec_string(', 'static __attribute__((unused)) int copy_exec_string(')
            (p/'test.c').write_text(code,encoding='utf-8')
            flags=['gcc','-std=c99','-O1','-Wall','-Wextra','-Werror','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all']
            if ref:flags.append('-Wno-error=format-truncation')
            subprocess.run(flags+[str(p/'test.c'),'-o',str(p/'test')],check=True)
            return subprocess.run([str(p/'test')],text=True,capture_output=True)
        result=suite();print(result.stdout,end='')
        assert result.returncode==0 and not result.stderr,result.stderr
        if args.compare_ref:
            result=suite(args.compare_ref)
            assert result.returncode==1 and 'failures' in result.stdout,result
            assert 'runtime error:' not in result.stderr and 'AddressSanitizer' not in result.stderr,result.stderr
            print('Baseline: '+result.stdout.strip())


if __name__=='__main__':
    main()
