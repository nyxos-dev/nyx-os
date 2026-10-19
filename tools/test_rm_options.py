#!/usr/bin/env python3
"""Check actual rm drivers against recording unlink/rmtree mocks; no real deletes."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdio.h>
#include <string.h>
static int fake_unlink(const char* path) {
    printf("CALL U %s\n",path);
    return !strcmp(path,"missing") || !strcmp(path,"denied") ? -1 : 0;
}
'''
USER_CASES = [
    ([],1,[]),(['--'],1,[]),(['file'],0,['U file']),
    (['first','second'],0,['U first','U second']),
    (['missing','second'],1,['U missing','U second']),
    (['denied'],1,['U denied']),(['-'],0,['U -']),
    (['--','-f'],0,['U -f']),(['--','-r','--'],0,['U -r','U --']),
    (['--','missing'],1,['U missing']),
    (['file','-f'],0,['U file','U -f']),
    (['./-f'],0,['U ./-f']),(['name with spaces'],0,['U name with spaces']),
] + [([flag,'kept'],2,[]) for flag in ['-f','-r','-R','-rf','-fr','-Z','--help','--invalid']]
KERNEL_CASES = [
    ([],0,[]),(['--'],0,[]),(['file'],0,['U file']),
    (['missing','second'],0,['U missing','U second']),
    (['-'],0,['U -']),(['--','-f'],0,['U -f']),
    (['--','--','-r'],0,['U --','U -r']),
    (['-r','--','-dir'],0,['R -dir']),
    (['-Rf','--','-dir','other'],0,['R -dir','R other']),
    (['-f'],0,[]),(['-f','missing'],0,['U missing']),
    (['-rf','missing'],0,['R missing']),
    (['-r','-Z','kept'],0,[]),(['-rfZ','kept'],0,[]),
    (['--invalid','kept'],0,[]),(['file','-f'],0,['U file','U -f']),
]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-rm-options-') as temp:
        work=Path(temp)
        def suite(ref=None):
            def read(path):
                if ref:
                    return subprocess.check_output(['git','show',ref+':'+path],text=True)
                return (ROOT/path).read_text()
            user=PREFIX+'\n#define unlink fake_unlink\n'+read('user/rm.c').replace('#include "libc.h"','')
            kernel=read('kernel/core/kernel.c')
            start=kernel.index('static void cmd_rm(int argc, char** argv) {')
            end=kernel.index('\n// shred ',start)
            kernel=PREFIX+'''\nstatic int fake_rmtree(const char* p) { printf("CALL R %s\\n",p); return !strcmp(p,"missing")?-1:0; }
#define vfs_unlink fake_unlink
#define vfs_rmtree fake_rmtree
'''+kernel[start:end]+'\nint main(int argc,char** argv) { cmd_rm(argc,argv); return 0; }\n'
            failures=[];count=0
            for name,text,cases in [('user',user,USER_CASES),('kernel',kernel,KERNEL_CASES)]:
                path=work/(name+'.c');path.write_text(text);exe=work/name
                subprocess.run(['gcc','-std=c99','-O1','-Wall','-Wextra','-Werror','-no-pie',
                                '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',str(exe)],check=True)
                for argv,code,calls in cases:
                    r=subprocess.run([str(exe),*argv],capture_output=True,text=True,timeout=10)
                    recorded=[l[5:] for l in r.stdout.splitlines() if l.startswith('CALL ')]
                    if r.returncode!=code or recorded!=calls or r.stderr:
                        failures.append((name,argv,r.returncode,recorded,r.stderr))
                    count+=1
            return count,failures
        count,failures=suite();assert not failures,failures
        print(f'PASS {count} rm option/operand scenarios under ASan/UBSan; no real filesystem deletion')
        if args.compare_ref:
            _,failures=suite(args.compare_ref);assert failures,'baseline unexpectedly passes'
            assert any(f[0]=='user' and f[1]==['-f','kept'] for f in failures)
            assert any(f[0]=='kernel' and f[1]==['--','-f'] for f in failures)
            print(f'PASS previous source fails {len(failures)} scenarios, including user -f and kernel --')

if __name__=='__main__':
    main()
