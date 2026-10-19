#!/usr/bin/env python3
"""Exercise compositor minimize/taskbar recovery and floating geometry retention."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@SOURCE@"
extern int puts(const char*);
static uint32_t width=1024,height=768;
uint32_t fb_get_width(void) { return width; }
uint32_t fb_get_height(void) { return height; }
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { char msg[96]; snprintf(msg,sizeof msg,"FAIL line %d check %d",__LINE__,checks); puts(msg); return 1; } } while(0)
static void taskbar_recover(int tb_win_id) {
    window_t* w=find_window(tb_win_id);
    if (!w) return;
    @BRANCH@
}
int main(void) {
    const int states[]={WSTATE_NORMAL,WSTATE_MAXIMIZED,WSTATE_SNAP_LEFT,WSTATE_SNAP_RIGHT,
                        WSTATE_SNAP_TL,WSTATE_SNAP_TR,WSTATE_SNAP_BL,WSTATE_SNAP_BR};
    const int gaps[]={0,12,64};
    const uint32_t sizes[][2]={{1024,768},{800,600},{1280,1024}};
    for (int g=0;g<3;g++) for (int mode=0;mode<3;mode++) for (int n=0;n<8;n++) {
        window_t one={0},other={0};width=1024;height=768;g_gaps=gaps[g];
        one.id=1;one.visible=1;one.state=WSTATE_NORMAL;
        one.x=100;one.y=80;one.w=400;one.h=300;
        one.normal_x=100;one.normal_y=80;one.normal_w=400;one.normal_h=300;
        other.id=2;other.visible=1;other.focused=1;other.z_order=10;
        windows[0]=&one;windows[1]=&other;current_workspace=0;
        if (states[n]!=WSTATE_NORMAL) window_snap_to(1,states[n]);
        else { one.x=120;one.y=90;one.w=420;one.h=310; }
        int x=one.x,y=one.y;uint32_t w=one.w,h=one.h;
        window_minimize(1);window_minimize(1);
        CHECK(one.state==WSTATE_MINIMIZED && !one.focused && one.visible);
        width=sizes[mode][0];height=sizes[mode][1];windows_reflow(1024,768);
        int nx=one.normal_x,ny=one.normal_y;uint32_t nw=one.normal_w,nh=one.normal_h;
        current_workspace=2;taskbar_recover(1);
        CHECK(one.state==states[n] && one.workspace==2 && one.focused && focused_id==1);
        CHECK(!other.focused && one.z_order>other.z_order);
        if(n) {
            int rx,ry,rw,rh;
            CHECK(snap_zone_rect(states[n],width,height,TASKBAR_H,g_gaps,&rx,&ry,&rw,&rh));
            CHECK(one.x==rx && one.y==ry && one.w==(uint32_t)rw && one.h==(uint32_t)(rh-TITLE_H));
        } else {
            CHECK(one.x==scale_i(x,(width*SCALE_ONE)/1024) && one.y==scale_i(y,(height*SCALE_ONE)/768));
            CHECK(one.w==scale_u(w,(width*SCALE_ONE)/1024) && one.h==scale_u(h,(height*SCALE_ONE)/768));
            nx=one.x;ny=one.y;nw=one.w;nh=one.h;
        }
        int z=one.z_order;taskbar_recover(1);CHECK(one.z_order==z && one.state==states[n]);
        window_snap(1,1);window_restore(1);
        CHECK(one.state==WSTATE_NORMAL && one.x==nx && one.y==ny && one.w==nw && one.h==nh);
        window_minimize(1);taskbar_recover(1);
        CHECK(one.state==WSTATE_NORMAL && one.x==nx && one.y==ny && one.w==nw && one.h==nh);
    }
    windows[0]=0;windows[1]=0;
    window_minimize(-1);taskbar_recover(-1);
    CHECK(snap_gap_selftest()==0 && window_focus_selftest()==0);
    char msg[96];snprintf(msg,sizeof msg,"PASS %d minimize/recover checks across 72 state/gap/resolution combinations",checks);puts(msg);
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    original = ROOT / 'kernel/gui/core/compositor.c'
    with tempfile.TemporaryDirectory(prefix='nyx-minimize-') as temp:
        work = Path(temp)
        def test(text, label):
            source = work / (label + '-compositor.c')
            source.write_text(text)
            start = text.index('if (w->state == WSTATE_MINIMIZED) {', text.index('int tb_win_id;'))
            end = text.index('} else if (w->focused)', start)
            harness = work / (label + '.c')
            harness.write_text(HARNESS.replace('@SOURCE@', str(source)).replace('@BRANCH@', text[start:end]+'}'))
            exe = work / label
            subprocess.run(['gcc','-std=gnu99','-O0','-ffreestanding','-Wall','-Wextra','-Werror',
                            '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                            '-I',str(original.parent),str(harness),'-o',str(exe)],check=True)
            return subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        result = test(original.read_text(), 'current')
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout,end='')
        if args.compare_ref:
            previous = subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/core/compositor.c'],text=True)
            result = test(previous, 'previous')
            assert result.returncode == 1 and 'FAIL' in result.stdout, result.stdout + result.stderr
            print('PASS previous source fails arrangement recovery:',result.stdout.strip())

if __name__ == '__main__':
    main()
