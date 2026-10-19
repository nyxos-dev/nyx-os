#!/usr/bin/env python3
"""Check desktop calculator arithmetic, error persistence and input recovery."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@SOURCE@"
extern void* malloc(size_t);
extern void free(void*);
extern int puts(const char*);
static int fail_alloc, checks;
void* kmalloc(size_t n) { return fail_alloc ? 0 : malloc(n); }
void memset_asm(void* p, uint8_t v, size_t n) { uint8_t* b=p; while(n--) *b++=v; }
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d check %d\n",__LINE__,checks); return 1; } } while(0)
static void keys(window_t* win, const char* text) { while(*text) calc_win_key(win,*text++); }
static int is_error(calc_win_t* c) { return !strcmp(c->display,"ERR"); }
static uint64_t random_state=0x563789abcdeULL;
static int64_t next_value(void) { random_state=random_state*6364136223846793005ULL+1; return (int64_t)random_state; }
static int arithmetic(int64_t a,int64_t b,char op) {
    calc_win_t c={0};c.mem_val=a;c.current_val=b;c.op=op;
    __int128 result=0;
    int bad=op=='/' && (b==0 || (a==(-9223372036854775807LL-1) && b==-1));
    if(op=='+') result=(__int128)a+b;
    if(op=='-') result=(__int128)a-b;
    if(op=='*') result=(__int128)a*b;
    if(op=='/' && !bad) result=a/b;
    bad |= result>9223372036854775807LL || result<(-(__int128)9223372036854775807LL-1);
    calc_handle_btn(&c,10);
    CHECK(is_error(&c)==bad);
    if(!bad) { CHECK(c.current_val==(int64_t)result); CHECK(c.mem_val==(int64_t)result); }
    else {
        CHECK(!c.op && c.new_input);
        calc_handle_btn(&c,11); CHECK(is_error(&c));
        calc_handle_btn(&c,10); CHECK(is_error(&c));
        calc_handle_btn(&c,16); CHECK(is_error(&c));
        calc_handle_btn(&c,7); CHECK(!strcmp(c.display,"7") && c.current_val==7);
    }
    return 0;
}
int main(int argc,char** argv) {
    calc_win_t c={0};window_t win={0};win.reserved=&c;
    if(argc>1 && !strcmp(argv[1],"digit-overflow")) {
        keys(&win,"9223372036854775808");CHECK(is_error(&c));return 0;
    }
    if(argc>1 && !strcmp(argv[1],"division-overflow"))
        return arithmetic(-9223372036854775807LL-1,-1,'/');
    keys(&win,"8/0=");CHECK(is_error(&c));
    keys(&win,"+*/=\b");CHECK(is_error(&c));
    keys(&win,"7+2=");CHECK(!strcmp(c.display,"9"));
    keys(&win,"c123\b4=");CHECK(!strcmp(c.display,"124"));
    keys(&win,"c9223372036854775807=");CHECK(!strcmp(c.display,"9223372036854775807"));
    keys(&win,"+1=");CHECK(is_error(&c));
    keys(&win,"C12+3*2=");CHECK(!strcmp(c.display,"30"));
    keys(&win,"c9223372036854775808");CHECK(is_error(&c));
    keys(&win,"C0-9223372036854775807=-1=");CHECK(!strcmp(c.display,"-9223372036854775808"));
    keys(&win,"-1=");CHECK(is_error(&c));
    fail_alloc=1;CHECK(!calc_create_ctx());fail_alloc=0;
    calc_win_t* made=calc_create_ctx();CHECK(made && !strcmp(made->display,"0"));free(made);
    const int64_t edges[]={-9223372036854775807LL-1,-9223372036854775807LL,-3037000500LL,-10,-1,0,1,9,10,3037000499LL,3037000500LL,9223372036854775807LL};
    const char ops[]={'+','-','*','/'};
    for(int i=0;i<12;i++) for(int j=0;j<12;j++) for(int k=0;k<4;k++)
        if(arithmetic(edges[i],edges[j],ops[k])) return 1;
    for(int i=0;i<5000;i++) {
        int64_t a=next_value(),b=next_value();
        if(arithmetic(a,b,ops[i%4])) return 1;
    }
    printf("PASS %d calculator checks, 5576 arithmetic pairs plus keyboard/error/recovery/allocation cases\n",checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    original = ROOT / 'kernel/gui/apps/calc_win.c'
    with tempfile.TemporaryDirectory(prefix='nyx-calc-') as temp:
        work = Path(temp)
        def build(text,label):
            source=work/(label+'-source.c');source.write_text(text)
            harness=work/(label+'.c');harness.write_text(HARNESS.replace('@SOURCE@',str(source)))
            exe=work/label
            subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                            '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                            '-I',str(original.parent),str(harness),'-o',str(exe)],check=True)
            return exe
        current=build(original.read_text(),'current')
        for mode in ([],['digit-overflow'],['division-overflow']):
            r=subprocess.run([str(current),*mode],capture_output=True,text=True,timeout=30)
            assert r.returncode==0,r.stdout+r.stderr
            print(r.stdout,end='')
        if args.compare_ref:
            old=subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/apps/calc_win.c'],text=True)
            previous=build(old,'previous')
            for mode in ([],['digit-overflow'],['division-overflow']):
                r=subprocess.run([str(previous),*mode],capture_output=True,text=True,timeout=30)
                assert r.returncode!=0 and ('FAIL' in r.stdout or 'runtime error:' in r.stderr),r.stdout+r.stderr
                print('PASS preceding failure:',mode or ['zero-error'],(r.stdout+r.stderr).strip().splitlines()[0])


if __name__=='__main__':
    main()
