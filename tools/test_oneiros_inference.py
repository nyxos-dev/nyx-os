#!/usr/bin/env python3
"""Exercise actual Oneiros inference, numerical refusal and GUI retry on Linux/WSL."""
import argparse
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DESKTOP = ROOT / 'kernel/gui/apps/oneiros_win.c'
CLI = ROOT / 'user/pkg/nyxgen/nyxgen.c'
DESKTOP_MAIN = r'''
extern long write(int, const void*, unsigned long);
uint32_t get_ticks(void) { return 1; }
void clipboard_set(const char *p, uint32_t n) { (void)p; (void)n; }
int main(int argc, char **argv) {
    if (argc<2) return oneiros_inference_selftest();
    int fd=open(argv[1],0);
    if (fd<0) return 2;
    file_size=32+sizeof(Params); file_data=malloc(file_size);
    unsigned long used=0;
    while (used<file_size) {
        long n=read(fd,file_data+used,file_size-used);
        if (n<=0) return 2;
        used+=(unsigned long)n;
    }
    close(fd);
    Params *p=load_model();
    free(file_data);
    if (!p) return 2;
    Scratch *sc=kmalloc(sizeof(Scratch));
    unsigned char tok[OG_B]={0}; sc->temp=0.7f;
    int ok=forward(p,sc,tok), result=ok?0:1;
    if (argc==3 && !strcmp(argv[2],"--dump") && ok)
        if (write(1,sc->probs,sizeof(sc->probs))!=sizeof(sc->probs)) result=2;
    if (argc==3 && !strcmp(argv[2],"--gui")) {
        oneiros_win_t o={0}; o.model=p; o.sc=sc; o.loaded=1; o.temp=0.7f;
        strcpy(o.seed,"seed"); o.seed_len=4;
        window_t win={0}; win.reserved=&o;
        ow_start(&o);
        if (!oneiros_win_tick(&win) || o.generating || !o.numeric_error || o.done || strcmp(o.out,"seed")) result=2;
        else {
            Layout l; ow_layout(0,WIN_CLIENT_Y(&win),0,0,&l);
            oneiros_win_click(&win,l.clear.x+1,l.clear.y+1,1);
            if (o.numeric_error || o.out_len || o.generating) result=2;
            else {
                memset_asm(p,0,sizeof(*p)); ow_start(&o); oneiros_win_tick(&win);
                result=(!o.generating || o.numeric_error || o.done!=OW_STEPS || o.out_len!=4+OW_STEPS)?2:0;
                if (!result) {
                    char saved[OW_OUT_MAX]; strcpy(saved,o.out);
                    const uint32_t extreme=0x7F7FFFFFu;
                    for (size_t i=0;i<sizeof(*p);i+=4) memmove((uint8_t*)p+i,&extreme,4);
                    oneiros_win_tick(&win);
                    if (o.generating || !o.numeric_error || o.done!=OW_STEPS || strcmp(saved,o.out)) result=2;
                }
            }
        }
    }
    kfree(sc);kfree(p);
    if (live_allocs) return 2;
    return result;
}
'''
CLI_MAIN = r'''
#define main original_main
#include "@SOURCE@"
#undef main
int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--dump")) {
        Params *p=malloc(sizeof(Params)); unsigned char tok[B]={0}; float pr[V];
        if (!p || !load_ckpt(argv[2],p)) return 2;
        int ok=forward(p,tok,pr); free(p);
        if (!ok) return 1;
        return fwrite(pr,1,sizeof(pr),stdout)==sizeof(pr)?0:2;
    }
    return original_main(argc,argv);
}
'''

def run(args, **kwargs):
    return subprocess.run([str(x) for x in args],capture_output=True,timeout=45,**kwargs)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref',help='Compare valid raw probability vectors with this Git revision')
    args=parser.parse_args()
    sys.dont_write_bytecode=True
    spec=importlib.util.spec_from_file_location('model_test',ROOT/'tools/test_oneiros_model.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    prefix=module.HARNESS.split('int main(int argc, char** argv)')[0]
    flags=['-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Wshadow','-Werror','-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections']
    sanitizer=['-fsanitize=address,undefined,float-cast-overflow','-fno-sanitize-recover=all']
    with tempfile.TemporaryDirectory(prefix='nyx-oneiros-inference-') as temp:
        out=Path(temp)
        def build(name,body,sanitize=True):
            path=out/(name+'.c');path.write_text(body)
            exe=out/name
            r=run(['gcc',*flags,*(sanitizer if sanitize else []),'-I',ROOT/'kernel/gui/apps',path,'-o',exe])
            if r.returncode: raise RuntimeError(r.stderr.decode())
            return exe
        desktop=build('desktop',prefix.replace('@SOURCE@',str(DESKTOP))+DESKTOP_MAIN)
        cli=build('cli',CLI_MAIN.replace('@SOURCE@',str(CLI)))
        shipped=(ROOT/'user/pkg/nyxgen/model.bin').read_bytes()
        zero=shipped[:32]+bytes(len(shipped)-32)
        maxf=struct.pack('<I',0x7f7fffff);minf=struct.pack('<I',0xff7fffff)
        fixtures={'shipped':shipped,'zero':zero,'max':shipped[:32]+maxf*((len(shipped)-32)//4),'negative-max':shipped[:32]+minf*((len(shipped)-32)//4)}
        # Infinite logits from finite output weights and finite nonzero tanh activations.
        head=bytearray(zero);head[-(256*64+256+64)*4:-(256*64+256)*4]=struct.pack('<f',1.0)*64
        head[-(256*64+256)*4:-256*4]=maxf*(256*64)
        fixtures['output-overflow']=bytes(head)
        checks=0
        r=run([desktop]);assert r.returncode==0,r.stderr;checks+=1
        paths={}
        for label,data in fixtures.items():
            path=out/(label+'.bin');path.write_bytes(data);paths[label]=path
            valid=label in ('shipped','zero')
            for exe,command in [(desktop,[path,'--dump']),(cli,['--dump',path])]:
                r=run([exe,*command]);assert r.returncode==(0 if valid else 1),(label,r.returncode,r.stderr)
                assert not r.stderr,(label,r.stderr)
                if valid:
                    values=struct.unpack('<256f',r.stdout)
                    assert all(0<=v<=1 for v in values) and abs(sum(values)-1)<1e-5,(label,values)
                checks+=1
            env=dict(os.environ,ONEIROS_MODEL=str(path))
            r=run([cli,'3','0.7','seed'],env=env)
            assert r.returncode==(0 if valid else 1),(label,r.stderr)
            assert (b'numerical inference failure' in r.stderr)==(not valid),(label,r.stderr)
            if not valid:
                assert r.stdout==b'seed',r.stdout
                r=run([desktop,path,'--gui']);assert r.returncode==0,(label,r.stderr)
                checks+=1
            checks+=1
        if args.compare_ref:
            for label,source,current,harness in [('desktop',DESKTOP,desktop,prefix+DESKTOP_MAIN),('cli',CLI,cli,CLI_MAIN)]:
                old=run(['git','show',args.compare_ref+':'+source.relative_to(ROOT).as_posix()]);assert old.returncode==0
                oldpath=out/(label+'-previous.c');oldpath.write_bytes(old.stdout)
                if label=='desktop':
                    body=prefix.replace('@SOURCE@',str(oldpath))+DESKTOP_MAIN.replace('if (argc<2) return oneiros_inference_selftest();','if (argc<2) return 2;')
                    start=body.index('    if (argc==3 && !strcmp(argv[2],"--gui"))')
                    end=body.index('    kfree(sc);kfree(p);',start)
                    body=body[:start]+body[end:]
                    body=body.replace('int ok=forward(p,sc,tok)', 'forward(p,sc,tok); int ok=1')
                else:
                    body=harness.replace('@SOURCE@',str(oldpath)).replace('int ok=forward(p,tok,pr);','forward(p,tok,pr); int ok=1;')
                baseline=build(label+'-baseline',body)
                for name in ('shipped','zero'):
                    command=[paths[name],'--dump'] if label=='desktop' else ['--dump',paths[name]]
                    oldresult=run([baseline,*command]);newresult=run([current,*command])
                    assert oldresult.returncode==newresult.returncode==0 and oldresult.stdout==newresult.stdout,(label,name,oldresult.stderr)
                    checks+=1
                command=[paths['max'],'--dump'] if label=='desktop' else ['--dump',paths['max']]
                oldresult=run([baseline,*command]);assert oldresult.returncode and b'runtime error' in oldresult.stderr,(label,oldresult.stderr)
                checks+=1
        print(f'PASS {checks} Oneiros inference scenarios; ASan/UBSan, GUI stop/clear/retry and CLI refusal')

if __name__=='__main__': main()
