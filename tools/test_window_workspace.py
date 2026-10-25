#!/usr/bin/env python3
"""Check actual compositor workspace focus and input routes with host sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@SOURCE@"
extern int puts(const char*);
static int keys, moves, presses, wheel;
static unsigned received_modifiers;
static void receive(window_t* win, int key) { (void)win; (void)key; keys++; received_modifiers=window_key_modifiers(); }
static void move(window_t* win, int x, int y, int b) { (void)win; (void)x; (void)y; (void)b; moves++; }
void fileman_win_mousemove(window_t* win, int x, int y, int b) { move(win,x,y,b); }
static void press(window_t* win, int x, int y, int b) { (void)win; (void)x; (void)y; (void)b; presses++; }
int is_ctrl_pressed(void) { return 1; }
int mouse_get_z(void) { return wheel; }
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { char msg[128]; snprintf(msg,sizeof msg,"FAIL line %d check %d: %s",__LINE__,checks,#x); puts(msg); return 1; } } while (0)
static void select_workspace(int ws) {
    int k='1'+ws,redraw=0;unsigned key_modifiers=KEY_MOD_CTRL;(void)key_modifiers;
    @SWITCH@
done_click: (void)redraw;
}
static void dispatch_key(void) {
    int k='x',redraw=0;unsigned key_modifiers=0;(void)key_modifiers;
    @KEY@
    (void)redraw;
}
static void dispatch_close(void) {
    int k=KEY_F4,redraw=0;
    @CLOSE@
done_click: (void)redraw;
}
static void dispatch_pointer(void) {
    int mx=8,my=TITLE_H+8,btns=1,prev_btns=0,mm_dispatched=0,redraw=0;
    @MOTION@
    @WHEEL@
    @PRESS@
    (void)mm_dispatched;(void)redraw;(void)prev_btns;
}
static int nested_ok;
static void nested(window_t* win, int key) {
    unsigned before=window_key_modifiers();
    if (key=='x') {
        window_dispatch_key(win,'y',KEY_MOD_ALT);
        nested_ok += window_key_modifiers()==before && before==KEY_MOD_CTRL;
    } else nested_ok += before==KEY_MOD_ALT;
}
int main(void) {
    window_t nested_win={0};nested_win.on_key=nested;
    window_dispatch_key(&nested_win,'x',KEY_MOD_CTRL);
    CHECK(nested_ok==2 && window_key_modifiers()==0);
    window_dispatch_key(NULL,'x',KEY_MOD_CTRL);
    nested_win.on_key=NULL;window_dispatch_key(&nested_win,'x',KEY_MOD_CTRL);
    CHECK(window_key_modifiers()==0);
    window_t w[6]={0};
    for (int i=0;i<6;i++) {
        w[i].id=i+1;w[i].visible=1;w[i].w=400;w[i].h=300;
        w[i].workspace=i<4?i:0;w[i].z_order=i;
        w[i].on_key=receive;w[i].on_mousemove=move;w[i].on_pressed=press;
        windows[i]=&w[i];
    }
    w[4].state=WSTATE_MINIMIZED;w[5].visible=0;window_count=6;
    current_workspace=0;window_focus(1);
    for (int round=0;round<8;round++) for (int ws=0;ws<4;ws++) {
        select_workspace(ws);CHECK(current_workspace==ws && focused_id==ws+1);
        for(int i=0;i<6;i++) CHECK(w[i].focused==(i==ws));
        int before=keys;dispatch_key();CHECK(keys==before+1);
        window_cycle_focus();CHECK(focused_id==ws+1);
    }
    select_workspace(0);window_set_workspace(1,1);
    CHECK(focused_id==0 && !w[0].focused && w[0].workspace==1);
    int before=keys;dispatch_key();wheel++;dispatch_pointer();dispatch_close();
    CHECK(keys==before && !moves && !presses && !w[0].close_requested);
    window_cycle_focus();CHECK(focused_id==0);
    window_focus(1);CHECK(focused_id==0);
    window_focus(5);window_focus(6);CHECK(focused_id==0);
    window_set_workspace(1,-1);window_set_workspace(1,WORKSPACE_COUNT);
    CHECK(w[0].workspace==1);window_set_workspace(-1,0);
    select_workspace(1);CHECK(focused_id!=0);
    int same=focused_id;select_workspace(1);CHECK(focused_id==same);
    window_focus(1);window_set_workspace(1,1);CHECK(focused_id==1);
    window_set_workspace(1,2);CHECK(focused_id==2 && !w[0].focused && w[1].focused);
    window_set_workspace(2,2);CHECK(focused_id==0 && !w[1].focused);
    select_workspace(2);CHECK(focused_id==2); // highest rank retained in destination
    window_cycle_focus();CHECK(focused_id==3);window_cycle_focus();CHECK(focused_id==1);
    window_cycle_focus();CHECK(focused_id==2);
    window_focus(1);
    window_minimize(1);CHECK(focused_id==0);
    window_cycle_focus();CHECK(focused_id==2 && !w[0].focused);
    // Simulate a stale external focus after a client hides/minimizes/moves itself.
    for(int kind=0;kind<3;kind++) {
        window_focus(2);before=keys;int oldmoves=moves,oldpresses=presses;
        if(kind==0) w[1].visible=0;
        if(kind==1) w[1].state=WSTATE_MINIMIZED;
        if(kind==2) w[1].workspace=3;
        dispatch_key();wheel++;dispatch_pointer();dispatch_close();
        CHECK(keys==before && moves==oldmoves && presses==oldpresses && !w[1].close_requested);
        w[1].visible=1;w[1].state=WSTATE_NORMAL;w[1].workspace=2;
    }
    window_focus(2);before=keys;dispatch_key();wheel++;dispatch_pointer();dispatch_close();
    CHECK(keys==before+2 && moves==1 && presses==1 && w[1].close_requested);
    window_dispatch_key(&w[1], 'x', KEY_MOD_CTRL | KEY_MOD_ALT);
    CHECK(received_modifiers==(KEY_MOD_CTRL | KEY_MOD_ALT) && window_key_modifiers()==0);
    wheel++;dispatch_pointer();CHECK(received_modifiers==0 && window_key_modifiers()==0);
    // Stationary File Manager edges must obey the same visibility/focus gate.
    w[1].on_mousemove=fileman_win_mousemove;mouse_x=8;mouse_y=TITLE_H+8;
    for(int kind=0;kind<3;kind++) {
        window_focus(2);int oldmoves=moves;
        if(kind==0)w[1].visible=0;
        if(kind==1)w[1].state=WSTATE_MINIMIZED;
        if(kind==2)w[1].workspace=3;
        dispatch_pointer();CHECK(moves==oldmoves);
        w[1].visible=1;w[1].state=WSTATE_NORMAL;w[1].workspace=2;
    }
    window_focus(2);int oldmoves=moves;dispatch_pointer();CHECK(moves==oldmoves+1);
    char msg[96];snprintf(msg,sizeof msg,"PASS %d workspace focus/input checks",checks);puts(msg);
    return 0;
}
'''

def harness(text):
    def between(start, end, offset=0):
        a=text.index(start,offset)
        return text[a:text.index(end,a)]
    switch=between("            if (",'\n\n',text.index("// Ctrl+1..4: workspace switching"))
    key=between('            window_t* fwin =', '\n        }',text.index('// Route all keys'))
    close=between('                window_t* awin =', '                    if (k == KEY_UP)')+'                }\n'
    motion=between('        if (', '\n\n',text.index('// Dispatch mouse-move'))
    wheel=between('        int wz = mouse_get_z();','\n\n',text.index('// Dispatch mouse-wheel'))
    press=between('                    window_t* fwin =','\n                }',text.index('Button HELD'))
    result=HARNESS
    if 'window_key_modifiers(void)' not in text:
        result=result.replace('#include "@SOURCE@"', '#include "@SOURCE@"\nstatic unsigned host_modifiers;\nunsigned window_key_modifiers(void) { return host_modifiers; }\nstatic void window_dispatch_key(window_t* w,int k,unsigned m) { if(!w||!w->on_key)return;unsigned p=host_modifiers;host_modifiers=m;w->on_key(w,k);host_modifiers=p; }')
    for name,value in dict(SWITCH=switch,KEY=key,CLOSE=close,MOTION=motion,WHEEL=wheel,PRESS=press).items():
        result=result.replace('@'+name+'@',value)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args=parser.parse_args()
    original=ROOT/'kernel/gui/core/compositor.c'
    with tempfile.TemporaryDirectory(prefix='nyx-workspace-') as temp:
        work=Path(temp)
        def test(text,label):
            source=work/(label+'-compositor.c');source.write_text(text)
            check=work/(label+'.c');check.write_text(harness(text).replace('@SOURCE@',str(source)))
            exe=work/label
            subprocess.run(['gcc','-std=gnu99','-O0','-ffreestanding','-Wall','-Wextra','-Werror',
                '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(original.parent),
                str(check),'-o',str(exe)],check=True)
            return subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        result=test(original.read_text(),'current')
        assert result.returncode==0,result.stdout+result.stderr
        print(result.stdout,end='')
        if args.compare_ref:
            old=subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/core/compositor.c'],text=True)
            result=test(old,'previous')
            assert result.returncode==1 and 'FAIL' in result.stdout,result.stdout+result.stderr
            print('PASS previous source rejects workspace isolation:',result.stdout.strip())

if __name__=='__main__':
    main()
