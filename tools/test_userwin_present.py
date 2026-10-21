#!/usr/bin/env python3
"""Run actual presentation syscall bodies against the user-window registry."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_userwin_headless import HARNESS

ROOT = Path(__file__).resolve().parents[1]
SUPPORT = r'''
extern void* realloc(void*, size_t);
static size_t allocated, copied;
static int refuse_alloc, refuse_copy;
void* krealloc(void* p, size_t n) {
    if (refuse_alloc) return 0;
    allocated += n;
    return realloc(p, n);
}
static int user_ptr_ok(uint64_t p, uint64_t n) { return p && n; }
int copy_from_user(void* dst, uint64_t src, uint64_t n) {
    if (refuse_copy) return -1;
    copied += n;
    unsigned char* d = dst; const unsigned char* s = (const void*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    return 0;
}
static uint64_t present_call(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    switch (op) {
@CASES@
    }
    return (uint64_t)-1;
}
static int full(int id, const uint32_t* p, uint32_t w, uint32_t h) {
    return (int64_t)present_call(SYS_WIN_PRESENT, id, (uint64_t)p, w, h);
}
static int rect(int id, const uint32_t* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return (int64_t)present_call(SYS_WIN_PRESENT_RECT, id, (uint64_t)p,
                               ((uint64_t)y << 16) | x, ((uint64_t)h << 16) | w);
}
'''
TEST = r'''
int main(void) {
    static uint32_t pixels[1024 * 1024];
    pixels[0] = 0x123456;
    int old = uwin_create(8, 8, "old"); CHECK(old >= 0);
    CHECK(uwin_destroy(old) == 0);
    int id = uwin_create(8, 8, "live"); CHECK(id >= 0);
    int bad[] = {-1, old, 0x7fffffff};
    for (int i = 0; i < 3; i++) {
        CHECK(full(bad[i], pixels, 1024, 1024) == -1);
        CHECK(allocated == 0 && copied == 0);
        CHECK(rect(bad[i], pixels, 0, 0, 1024, 1024) == -1);
        CHECK(allocated == 0 && copied == 0);
    }
    CHECK(full(id, pixels, 1024, 1024) == -1);
    CHECK(full(id, pixels, 0, 8) == -1);
    CHECK(rect(id, pixels, 8, 0, 1, 1) == -1);
    CHECK(rect(id, pixels, 0, 8, 1, 1) == -1);
    CHECK(rect(id, pixels, 7, 7, 2, 1) == -1);
    CHECK(rect(id, pixels, 7, 7, 1, 2) == -1);
    CHECK(rect(id, pixels, 65535, 65535, 1, 1) == -1);
    CHECK(rect(id, pixels, 0, 0, 0, 1) == -1);
    CHECK(allocated == 0 && copied == 0 && !uwin_slot(id)->backing);
    refuse_alloc = 1;
    CHECK(full(id, pixels, 8, 8) == -1 && !uwin_slot(id)->backing);
    refuse_alloc = 0;
    CHECK(full(id, 0, 8, 8) == -1 && allocated == 0);
    refuse_copy = 1;
    CHECK(full(id, pixels, 8, 8) == -1 && !uwin_slot(id)->backing);
    refuse_copy = 0;
    CHECK(full(id, pixels, 8, 8) == 0);
    CHECK(uwin_slot(id)->backing[0] == 0x123456);
    size_t a = allocated, c = copied;
    CHECK(full(old, pixels, 1024, 1024) == -1);
    CHECK(rect(id, pixels, 7, 7, 1024, 1024) == -1);
    CHECK(allocated == a && copied == c && uwin_slot(id)->backing[0] == 0x123456);
    CHECK(rect(id, pixels, 7, 7, 1, 1) == 0);
    CHECK(uwin_slot(id)->backing[63] == 0x123456);
    CHECK(uwin_present_rect(id, pixels, 0x7fffffff, 0, 0x7fffffff, 1) == -1);
    CHECK(uwin_present_rect(id, pixels, 0, -1, 1, 1) == -1);
    CHECK(uwin_resize(id, 4, 4) == 0);
    a = allocated; c = copied;
    CHECK(full(id, pixels, 8, 8) == -1 && allocated == a && copied == c);
    CHECK(rect(id, pixels, 3, 3, 2, 1) == -1 && allocated == a && copied == c);
    CHECK(full(id, pixels, 4, 4) == 0);
    CHECK(uwin_destroy(id) == 0);
    a = allocated; c = copied;
    CHECK(full(id, pixels, 1024, 1024) == -1);
    CHECK(allocated == a && copied == c);
    printf("PASS %d presentation preflight, pixel and failure checks\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref', help='reproduce scratch growth before rejection')
    args = parser.parse_args()
    original = ROOT/'kernel/gui/core/userwin.c'
    with tempfile.TemporaryDirectory(prefix='nyx-present-') as temp:
        work = Path(temp)
        def run(userwin, syscall, label):
            source = work/(label+'-source.c'); source.write_text(userwin)
            cases = []
            for begin, end in [('SYS_WIN_PRESENT', 'SYS_WIN_POLL_EVENT'),
                               ('SYS_WIN_PRESENT_RECT', 'SYS_WIN_SET_TITLE')]:
                cases.append(syscall[syscall.index('        case '+begin+':'):
                                     syscall.index('        case '+end+':')])
            body = HARNESS.split('int main(void)')[0].replace('@SOURCE@', str(source))
            harness = work/(label+'.c')
            harness.write_text(body+SUPPORT.replace('@CASES@', '\n'.join(cases))+TEST)
            exe = work/label
            subprocess.run(['gcc', '-std=gnu99', '-O1', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                '-ffunction-sections', '-fdata-sections', '-no-pie', '-Wl,--gc-sections',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(original.parent),
                str(harness), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        current = run(original.read_text(), (ROOT/'kernel/core/syscall.c').read_text(), 'current')
        assert current.returncode == 0, current.stdout+current.stderr
        print(current.stdout, end='')
        if args.compare_ref:
            old = lambda p: subprocess.check_output(['git', 'show', args.compare_ref+':'+p], text=True)
            previous = run(old('kernel/gui/core/userwin.c'), old('kernel/core/syscall.c'), 'previous')
            assert previous.returncode != 0 and 'FAIL' in previous.stdout, previous.stdout+previous.stderr
            print('PASS baseline allocates/copies before rejecting the invalid handle')


if __name__ == '__main__':
    main()
