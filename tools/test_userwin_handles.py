#!/usr/bin/env python3
"""Reject stale user-window handles after slot reuse, including exhaustion."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_userwin_headless import HARNESS as HEADLESS_HARNESS

ROOT = Path(__file__).resolve().parents[1]
HARNESS = HEADLESS_HARNESS.split('int main(void)')[0] + r'''
int main(void) {
    uint32_t pixels[64] = {0x123456};
    uwin_event_t ev;
    for (int graphical = 0; graphical < 2; graphical++) {
        fw = graphical ? 1024 : 0; fh = graphical ? 768 : 0;
        for (int cycle = 0; cycle < 128; cycle++) {
            int old = uwin_create(8, 8, "old"); CHECK(old >= 0);
            CHECK(uwin_destroy(old) == 0);
            int fresh = uwin_create(8, 8, "fresh");
            CHECK(fresh >= 0 && fresh != old);
            if (graphical) CHECK(bound.x == 80 && bound.y == 80);
            CHECK(uwin_present(fresh, pixels, 8, 8) == 0);
            user_win_t* s = uwin_slot(fresh);
            uint32_t* backing = s->backing;
            uwin_evq_push(&s->evq, UWE_KEY, 'z', 0, 0);
            int prior_destroys = destroys;
            CHECK(uwin_destroy(old) == 0 && destroys == prior_destroys);
            CHECK(uwin_present(old, pixels, 8, 8) == -1);
            CHECK(uwin_present_rect(old, pixels, 0, 0, 1, 1) == -1);
            CHECK(uwin_poll_event(old, &ev) == -1);
            CHECK(uwin_set_title(old, "stale") == -1);
            CHECK(uwin_resize(old, 4, 4) == -1);
            CHECK(uwin_slot(fresh) == s && s->backing == backing &&
                  s->w == 8 && s->h == 8 && backing[0] == 0x123456);
            CHECK(uwin_poll_event(fresh, &ev) == 1 && ev.kind == UWE_KEY && ev.a == 'z');
            CHECK(uwin_destroy(fresh) == 0);
        }
    }
    int closed = uwin_create(8, 8, "close box"); CHECK(closed >= 0);
    bound.on_close(&bound);
    CHECK(!uwin_slot(closed));
    int after_close = uwin_create(8, 8, "after close box");
    CHECK(after_close >= 0 && after_close != closed);
    CHECK(uwin_destroy(closed) == 0 && uwin_present(after_close, pixels, 8, 8) == 0);
    CHECK(uwin_destroy(after_close) == 0);
    fw = fh = 0;
    int ids[USERWIN_MAX];
    for (int i = 0; i < USERWIN_MAX; i++) { ids[i] = uwin_create(8, 8, "full"); CHECK(ids[i] >= 0); }
    CHECK(uwin_create(8, 8, "overflow") == -1);
    CHECK(uwin_destroy(ids[3]) == 0);
    int replacement = uwin_create(8, 8, "replacement");
    CHECK(replacement >= 0 && replacement != ids[3]);
    for (int i = 0; i < USERWIN_MAX; i++) CHECK(uwin_destroy(ids[i]) == 0);
    CHECK(uwin_present(replacement, pixels, 8, 8) == 0);
    CHECK(uwin_destroy(replacement) == 0);
    CHECK(uwin_selftest() == 0);
#ifdef USERWIN_HANDLE_LIMIT
    /* Drive the production allocator through its final two identities. */
    g_next_uwin_handle = USERWIN_HANDLE_LIMIT - 1;
    int penultimate = uwin_create(8, 8, "last but one");
    int last = uwin_create(8, 8, "last");
    CHECK(penultimate == 0x7ffffffe && last == 0x7fffffff);
    CHECK(uwin_destroy(penultimate) == 0);
    CHECK(uwin_create(8, 8, "must not wrap") == -1);
    CHECK(uwin_present(last, pixels, 8, 8) == 0);
    CHECK(uwin_destroy(last) == 0);
    CHECK(uwin_create(8, 8, "still exhausted") == -1);
#endif
    printf("PASS %d stale-handle, slot-reuse and exhaustion checks\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    original = ROOT / 'kernel/gui/core/userwin.c'
    with tempfile.TemporaryDirectory(prefix='nyx-userwin-handles-') as temp:
        work = Path(temp)

        def run(source, label):
            src = work / (label + '-source.c')
            src.write_text(source)
            harness = work / (label + '.c')
            harness.write_text(HARNESS.replace('@SOURCE@', str(src)))
            exe = work / label
            subprocess.run(['gcc', '-std=gnu99', '-O1', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                            '-ffunction-sections', '-fdata-sections', '-no-pie', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(original.parent),
                            str(harness), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)

        result = run(original.read_text(), 'current')
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end='')
        if args.compare_ref:
            old = subprocess.check_output(['git', 'show', args.compare_ref + ':kernel/gui/core/userwin.c'], text=True)
            result = run(old, 'previous')
            assert result.returncode != 0 and 'FAIL' in result.stdout, result.stdout + result.stderr
            print('PASS baseline reuses a stale window identity')


if __name__ == '__main__':
    main()
