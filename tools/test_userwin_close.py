#!/usr/bin/env python3
"""Exercise the real compositor destroy path with user-window registry storage."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_userwin_headless import HARNESS as HEADLESS_HARNESS

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
int main(void) {
    uint32_t pixels[64] = {0};
    fw = 1024; fh = 768;
    for (int mode = 0; mode < 2; mode++) {
        for (int i = 0; i < 64; i++) {
            int id = uwin_create(8, 8, "close"); CHECK(id >= 0);
            windows[0] = &bound; window_count = 1; focused_id = bound.id;
            CHECK(uwin_present(id, pixels, 8, 8) == 0);
            int before = frees;
            if (mode == 0) CHECK(uwin_destroy(id) == 0);
            else window_destroy(bound.id);
            CHECK(!uwin_slot(id));
            CHECK(bound.reserved == 0);
            CHECK(window_count == 0 && windows[0] == 0);
            CHECK(frees == before + 2); /* backing and window, never static registry */
            CHECK(uwin_destroy(id) == 0 && frees == before + 2);
        }
    }
    CHECK(focus_changes == 128);
    printf("PASS %d real compositor close checks\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    original = ROOT/'kernel/gui/core/userwin.c'
    compositor = (ROOT/'kernel/gui/core/compositor.c').read_text()
    start = compositor.index('void window_destroy(int id) {')
    destroy = compositor[start:compositor.index('\n}\n', start)+3]
    prefix = HEADLESS_HARNESS.split('int main(void)')[0]
    prefix = prefix.replace('creates,destroys,checks,unavailable', 'creates,checks,unavailable')
    prefix = prefix.replace('static window_t bound;', '''static window_t bound;
static int frees, focus_changes, window_count, focused_id;
static window_t* windows[MAX_WINDOWS];
static window_t* find_window(int id) { return windows[0] && windows[0]->id == id ? windows[0] : 0; }
static void focus_next_window(void) { focus_changes++; }''')
    prefix = prefix.replace('void kfree(void* p) { free(p); }', 'void kfree(void* p) { if (!p) return; frees++; if (p != &bound) free(p); }')
    old_stub = 'void window_destroy(int id) { (void)id;destroys++;if(bound.on_close)bound.on_close(&bound); }'
    assert old_stub in prefix
    prefix = prefix.replace(old_stub, destroy)
    with tempfile.TemporaryDirectory(prefix='nyx-userwin-close-') as temp:
        work = Path(temp)

        def run(source, label):
            src = work/(label+'-source.c'); src.write_text(source)
            harness = work/(label+'.c'); harness.write_text(prefix.replace('@SOURCE@', str(src))+TEST)
            exe = work/label
            subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(original.parent),
                str(harness),'-o',str(exe)],check=True)
            return subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)

        result = run(original.read_text(), 'current')
        assert result.returncode == 0, result.stdout+result.stderr
        print(result.stdout,end='')
        if args.compare_ref:
            previous = subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/core/userwin.c'],text=True)
            result = run(previous,'previous')
            assert result.returncode != 0 and 'attempting free on address which was not malloc' in result.stderr, result.stdout+result.stderr
            print('PASS baseline compositor attempts to free static user-window registry')


if __name__ == '__main__':
    main()
