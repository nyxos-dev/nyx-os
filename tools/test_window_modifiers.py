#!/usr/bin/env python3
"""Exercise captured modifiers through real compositor and application branches."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "@COMPOSITOR@"
extern int puts(const char*);
static int physical_ctrl, physical_alt, mouse_x_test=100, mouse_y_test=100;
int is_ctrl_pressed(void) { return physical_ctrl; }
int is_alt_pressed(void) { return physical_alt; }
int mouse_get_x(void) { return mouse_x_test; }
int mouse_get_y(void) { return mouse_y_test; }
void mouse_set_pos(int x,int y) { mouse_x_test=x;mouse_y_test=y; }
void mouse_kbd_click(int button) { (void)button; }
static void shortcut(int k,unsigned key_modifiers) {
    int redraw=0;
    @WORKSPACE@
    @CLOSE@
done_click: (void)redraw;
}
static void mousekey(int k,unsigned key_modifiers) {
    unsigned fw=1024,fh=768;
    @MOUSEKEY@
}
static int checks;
#define CHECK(x) do { checks++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
typedef struct {int active,ntabs;} test_tabs_t;
static void browser_shortcut(window_t* win,int key) {
    test_tabs_t* T=win->reserved;
    @SELENE@
}
int main(void) {
    window_t w[2]={0};
    for(int i=0;i<2;i++){w[i].id=i+1;w[i].workspace=i;w[i].visible=1;windows[i]=&w[i];}
    window_count=2;window_focus(1);
    // Physical state is already released when the stored Ctrl/Alt key arrives.
    shortcut('2',KEY_MOD_CTRL);CHECK(current_workspace==1 && focused_id==2);
    shortcut(KEY_F4,KEY_MOD_ALT);CHECK(w[1].close_requested && !w[0].close_requested);
    physical_ctrl=physical_alt=1;
    shortcut('1',0);CHECK(current_workspace==1 && focused_id==2);
    w[1].close_requested=0;shortcut(KEY_F4,0);CHECK(!w[1].close_requested);
    mousekey('d',0);CHECK(mouse_x_test==100);
    physical_alt=0;mousekey('d',KEY_MOD_ALT);CHECK(mouse_x_test==116);
    mousekey('w',KEY_MOD_ALT);CHECK(mouse_y_test==84);
    physical_ctrl=0;
    window_t app={0};
    test_tabs_t tabs={0,3};app.reserved=&tabs;app.on_key=browser_shortcut;
    for(int i=0;i<9;i++) {
        window_dispatch_key(&app,'\t',KEY_MOD_CTRL);CHECK(tabs.active==(i+1)%3);
        int before=tabs.active;
        physical_ctrl=1;window_dispatch_key(&app,KEY_PGUP,0);CHECK(tabs.active==before);
        physical_ctrl=0;window_dispatch_key(&app,KEY_PGUP,KEY_MOD_CTRL);CHECK(tabs.active==(before+2)%3);
        window_dispatch_key(&app,KEY_PGDN,KEY_MOD_CTRL);CHECK(tabs.active==before);
    }
    CHECK(window_key_modifiers()==0);
    printf("PASS %d delayed compositor/MouseKeys/browser shortcut checks\n",checks);
    return 0;
}
'''


def main():
    compositor = ROOT / 'kernel/gui/core/compositor.c'
    source = compositor.read_text(encoding='utf-8')

    def between(start, end, after):
        a = source.index(start, source.index(after))
        return source[a:source.index(end, a)]

    workspace = between('            if (', '\n\n', '// Ctrl+1..4:')
    close = between('            if (', '\n                    if (k == KEY_UP)',
                    '// Window-management chords.') + '\n                }\n            }\n'
    mousekey = between('        if (k &&', '\n\n', '// MouseKeys')
    selene = (ROOT / 'kernel/gui/apps/selene_win.c').read_text(encoding='utf-8')
    selene = '\n'.join(line for line in selene.splitlines() if 'window_key_modifiers()' in line)
    assert len(selene.splitlines()) == 3
    text = HARNESS
    for key, value in dict(COMPOSITOR=str(compositor), WORKSPACE=workspace,
                           CLOSE=close, MOUSEKEY=mousekey, SELENE=selene).items():
        text = text.replace('@' + key + '@', value)
    with tempfile.TemporaryDirectory(prefix='nyx-window-modifiers-') as temp:
        work = Path(temp)
        check = work / 'check.c'
        check.write_text(text, encoding='utf-8')
        exe = work / 'check'
        subprocess.run(['gcc', '-std=gnu99', '-O0', '-ffreestanding', '-Wall', '-Wextra',
                        '-Werror', '-ffunction-sections', '-fdata-sections', '-no-pie',
                        '-Wl,--gc-sections', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(check), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
