#!/usr/bin/env python3
"""Verify hidden window ticks progress without damaging the active workspace."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r"""
#include "@SOURCE@"
extern int puts(const char*);
static int calls, mode, dirty, result_redraw, result_changed, result_full;
static int tick(window_t* w) {
    calls++;
    if(mode==1) w->workspace=0;
    if(mode==2) w->workspace=1;
    if(mode==3) w->state=WSTATE_MINIMIZED;
    if(mode==4) w->visible=0;
    if(mode==5) windows[w->id-1]=NULL;
    return dirty;
}
static void iteration(uint32_t now, int redraw) {
    static uint32_t game_tick_ms;
    @BODY@
    result_redraw=redraw;result_changed=tick_changed;result_full=redraw_pre_tick;
    for(int i=0;i<tick_changed && i<TICK_MAX_RECTS;i++)
        if(tick_idx[i]<0 || tick_idx[i]>=MAX_WINDOWS) __builtin_trap();
}
static int checks;
#define CHECK(x) do { checks++; if(!(x)) { char msg[128];snprintf(msg,sizeof msg,"FAIL line%d: %s",__LINE__,#x);puts(msg);return 1; } } while(0)
int main(void) {
    window_t w[MAX_WINDOWS]={0};uint32_t now=100;current_workspace=0;
    for(int state=0;state<4;state++) for(int changed=0;changed<2;changed++) for(int prior=0;prior<2;prior++) {
        w[0].id=1;w[0].visible=state!=3;w[0].workspace=state==1;
        w[0].state=state==2?WSTATE_MINIMIZED:WSTATE_NORMAL;w[0].on_tick=tick;
        windows[0]=&w[0];calls=0;dirty=changed;mode=0;now+=100;
        iteration(now,prior);
        CHECK(calls==(state!=3)); // minimized and other-workspace callbacks still advance
        CHECK(result_redraw==(prior || (state==0 && changed)));
        CHECK(result_changed==(state==0 && changed));CHECK(result_full==prior);
        iteration(now+10,0);CHECK(calls==(state!=3) && result_changed==0 && result_redraw==0);
    }
    for(int transition=1;transition<=5;transition++) {
        w[0].visible=1;w[0].workspace=transition==1;w[0].state=WSTATE_NORMAL;
        windows[0]=&w[0];mode=transition;dirty=1;calls=0;now+=100;iteration(now,0);
        CHECK(calls==1 && result_changed==1 && result_redraw==1 && result_full==1);
    }
    mode=0;dirty=1;
    for(int n=0;n<=MAX_WINDOWS;n++) {
        for(int i=0;i<MAX_WINDOWS;i++) {
            w[i].id=i+1;w[i].visible=1;w[i].state=WSTATE_NORMAL;w[i].workspace=i>=n;
            w[i].on_tick=tick;windows[i]=&w[i];
        }
        calls=0;now+=100;iteration(now,0);
        CHECK(calls==MAX_WINDOWS && result_changed==n && result_redraw==(n>0) && !result_full);
    }
    // Counter wrap and no-handler windows preserve the same pacing contract.
    for(int i=1;i<MAX_WINDOWS;i++)windows[i]=NULL;
    w[0].workspace=0;w[0].on_tick=NULL;calls=0;iteration(0xfffffff0u,0);
    CHECK(calls==0 && !result_redraw);w[0].on_tick=tick;iteration(20,0);
    CHECK(calls==1 && result_changed==1 && result_redraw==1);
    char msg[96];snprintf(msg,sizeof msg,"PASS %d background tick/damage checks",checks);puts(msg);
    return 0;
}
"""

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--compare-ref');args=parser.parse_args()
    original=ROOT/'kernel/gui/core/compositor.c'
    with tempfile.TemporaryDirectory(prefix='nyx-window-tick-') as tmp:
        work=Path(tmp)
        def run(text,label):
            source=work/(label+'-compositor.c');source.write_text(text,encoding='utf-8')
            start=text.index('        int redraw_pre_tick = redraw;')
            end=text.index('        // Animated wallpaper:',start)
            check=work/(label+'.c');check.write_text(HARNESS.replace('@SOURCE@',str(source)).replace('@BODY@',text[start:end]),encoding='utf-8')
            exe=work/label
            subprocess.run(['gcc','-std=gnu99','-O0','-ffreestanding','-Wall','-Wextra','-Werror',
                '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(original.parent),str(check),'-o',str(exe)],check=True)
            return subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        result=run(original.read_text(encoding='utf-8'),'current');assert result.returncode==0,result.stdout+result.stderr
        print(result.stdout,end='')
        if args.compare_ref:
            source=subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/core/compositor.c'],text=True,encoding='utf-8')
            result=run(source,'previous');assert result.returncode==1 and 'FAIL' in result.stdout,result.stdout+result.stderr
            print('PASS previous source rejects hidden redraw contract:',result.stdout.strip())

if __name__=='__main__': main()
