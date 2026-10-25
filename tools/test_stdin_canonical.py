#!/usr/bin/env python3
"""Exercise canonical/raw read loops with a scripted keyboard and echo sink.

Run on Linux x86_64 or WSL. No process is scheduled and no privileged instruction
executes. Return-path checks extract the actual functions and simulate interrupts,
signals and scheduling; real syscall return and signal delivery require QEMU.
"""
import argparse
import os
import sys
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

sys.dont_write_bytecode = True
from test_pipe_signals import function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "@ROOT@/kernel/core/kernel.h"
#undef user_cr3
#undef user_rsp
static uint64_t test_cr3, test_rsp;
#define user_cr3 test_cr3
#define user_rsp test_rsp
#define getchar_poll test_getchar_poll
#define putchar test_putchar
char test_getchar_poll(void);
int test_putchar(int c);
#include "@ROOT@/kernel/core/syscall.c"

static const char* input;
static int input_pos, echo_len;
static char echoed[128];
static process_t raw_process, other_process;
static process_t* current_process;
static const int* raw_keys;
static int raw_pos, ready_calls;
process_t* get_current_process(void) { return current_process; }
int getkey_poll(void) { return raw_keys[raw_pos] ? raw_keys[raw_pos++] : 0; }
int keyboard_has_input(void) { ready_calls++; return raw_keys && raw_keys[raw_pos]; }
int signal_pending(process_t* p) { (void)p; return 0; }
int signal_check_stop(process_t* p) { (void)p; return 0; }
char test_getchar_poll(void) {
    /* Model scratch slots overwritten while another task runs. */
    test_cr3 = 0xBAD; test_rsp = 0xBAD;
    return input[input_pos] ? input[input_pos++] : 0;
}
int test_putchar(int c) {
    if (echo_len < (int)sizeof(echoed) - 1) echoed[echo_len++] = (char)c;
    echoed[echo_len] = 0;
    return c;
}
static void feed(const char* keys) {
    input = keys; input_pos = 0; echo_len = 0; echoed[0] = 0;
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
static int read_check(int capacity, const char* expected, int consumed, const char* echo) {
    char out[12];
    for (int i = 0; i < (int)sizeof(out); i++) out[i] = '!';
    test_cr3 = 0x1234; test_rsp = 0x5678;
    int size = stdin_read_line(out, capacity);
    CHECK(size == (int)strlen(expected));
    for (int i = 0; i < size; i++) CHECK(out[i] == expected[i]);
    CHECK(out[capacity] == '!');
    CHECK(input_pos == consumed && strcmp(echoed, echo) == 0);
    CHECK(test_cr3 == 0x1234 && test_rsp == 0x5678);
    return 0;
}
int main(void) {
    CHECK(stdin_line_selftest() == 0);
    feed("\004q\n");
    CHECK(!read_check(8, "", 1, ""));
    CHECK(!read_check(8, "q\n", 3, "q\n"));
    feed("ab\004c\n");
    CHECK(!read_check(8, "ab", 3, "ab"));
    CHECK(!read_check(8, "c\n", 5, "abc\n"));
    feed("abcdef\n");
    CHECK(!read_check(3, "abc", 3, "abc"));
    CHECK(!read_check(3, "def", 6, "abcdef"));
    CHECK(!read_check(3, "\n", 7, "abcdef\n"));
    feed("ab\bC\177D\rX\n");
    CHECK(!read_check(8, "aD\n", 7, "ab\b \bC\b \bD\n"));
    CHECK(!read_check(8, "X\n", 9, "ab\b \bC\b \bD\nX\n"));
    feed("\b\177\004");
    CHECK(!read_check(8, "", 3, ""));
    feed("ab\n");
    CHECK(!read_check(0, "", 0, ""));
    CHECK(!read_check(1, "a", 1, "a"));
    CHECK(!read_check(1, "b", 2, "ab"));
    CHECK(!read_check(1, "\n", 3, "ab\n"));
    static const int mixed[] = {'a',KEY_UP,'b',KEY_DOWN,KEY_RIGHT,KEY_LEFT,KEY_HOME,KEY_END,'\r',0};
    static const char expected[] = "a\033[Ab\033[B\033[C\033[D\033[H\033[F\n";
    char raw[8];
    current_process=&raw_process;
    for (int capacity=1;capacity<=6;capacity++) {
        raw_keys=mixed; raw_pos=0; raw_process.tty_pending_len=0;
        raw_process.tty_raw=TTY_RAW; int total=0;
        while (total<(int)sizeof(expected)-1) {
            for (int i=0;i<8;i++) raw[i]='!';
            test_cr3=0x1234; test_rsp=0x5678;
            int n=stdin_read_raw(raw,capacity);
            CHECK(n>0 && n<=capacity && raw[capacity]=='!');
            CHECK(test_cr3==0x1234 && test_rsp==0x5678);
            for (int i=0;i<n;i++) CHECK(raw[i]==expected[total++]);
            if (capacity>=3) for (int i=0;i<n;i++) if (raw[i]==27) CHECK(i+2<n);
        }
        CHECK(total==(int)sizeof(expected)-1 && raw_pos==9 && !raw_process.tty_pending_len);
    }
    static const int arrow[] = {KEY_UP,'x',0}, letter[] = {'z',0}, empty[] = {0};
    raw_keys=arrow; raw_pos=0;
    CHECK(stdin_read_raw(raw,0)==0 && raw_pos==0);
    CHECK(stdin_read_raw(raw,-1)==0 && raw_pos==0);
    CHECK(stdin_read_raw(raw,3)==3 && raw_pos==1 && raw[2]=='A');
    CHECK(stdin_read_raw(raw,1)==1 && raw[0]=='x' && raw_pos==2);
    raw_pos=0; CHECK(stdin_read_raw(raw,1)==1 && raw[0]==27 && raw_process.tty_pending_len==2);
    current_process=&other_process; raw_keys=letter; raw_pos=0;
    CHECK(stdin_read_raw(raw,1)==1 && raw[0]=='z' && !other_process.tty_pending_len);
    current_process=&raw_process; raw_keys=empty; raw_pos=0; ready_calls=0;
    CHECK(stdin_has_input() && ready_calls==0);
    CHECK(stdin_set_mode(&raw_process,TTY_RAW)==TTY_RAW && raw_process.tty_pending_len==2);
    CHECK(stdin_set_mode(&raw_process,7)==-1 && raw_process.tty_pending_len==2);
    CHECK(stdin_read_raw(raw,2)==2 && raw[0]=='[' && raw[1]=='A');
    CHECK(!stdin_has_input() && ready_calls==1);
    raw_process.tty_pending_len=2;
    CHECK(stdin_set_mode(&raw_process,TTY_CANON)==TTY_RAW && !raw_process.tty_pending_len);
    CHECK(stdin_set_mode(NULL,TTY_RAW)==-1);
    printf("OK: EOF, bounded reads without lost keys, editing, echo and scratch-slot restoration\n");
    printf("OK: raw capacities1..6, atomic escapes when possible, pending bytes, poll, mode reset and process isolation\n");
    return 0;
}
'''


RETURN_PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
enum { STDIN_LINE_ECHO=1, STDIN_LINE_ERASE=2, STDIN_LINE_DONE=4 };
enum { KEY_UP=128, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END };
typedef struct {
    int blocked_in_kernel, tty_pending_len;
    uint8_t tty_pending[3];
} process_t;
static process_t proc;
static uint64_t user_cr3=123, user_rsp=456;
static int host_if, mode, yields, stops;
volatile uint32_t tick_count;
static process_t* get_cur_proc(void) { return mode==5 ? NULL : &proc; }
static int getkey_poll(void) {
    if (mode==0 || (mode==1 && yields) || (mode==3 && stops)) return 'A';
    return 0;
}
static char getchar_poll(void) { return (char)getkey_poll(); }
static int signal_pending(process_t* p) {
    (void)p;
    return mode==2 || (mode==3 && !stops) || (mode==4 && yields);
}
static int signal_check_stop(process_t* p) {
    (void)p;
    assert(!host_if);
    if (mode!=3) return 0;
    stops++;
    user_cr3=777; user_rsp=888;
    return 1;
}
static void host_yield(void) {
    assert(++yields<=2);
    host_if=1;
    tick_count++;
    user_cr3=777; user_rsp=888;
    assert(mode==5 || proc.blocked_in_kernel);
}
'''
RETURN_MAIN = r'''
int main(int argc, char** argv) {
    assert(argc==3);
    int kind=atoi(argv[1]);
    mode=atoi(argv[2]);
    char out[2]={0};
    int result;
    if (kind==0) result=stdin_read_line(out,mode==6?0:1);
    else if (kind==1) result=stdin_read_raw(out,mode==6?0:1);
    else result=stdin_readkey(1);
    int expected=(mode==2||mode==4)?-EINTR:(mode==5||mode==6)?0:kind==2?'A':1;
    assert(result==expected && user_cr3==123 && user_rsp==456 && !proc.blocked_in_kernel);
    if (host_if) {
        fprintf(stderr,"RETURN_WITH_INTERRUPTS_ENABLED kind=%d mode=%d\n",kind,mode);
        return 2;
    }
    return 0;
}
'''


def check_returns(cc, work, compare_ref):
    def run(ref=None):
        source = (subprocess.check_output(
            ['git', 'show', f'{ref}:kernel/core/syscall.c'], cwd=ROOT, text=True)
            if ref else (ROOT / 'kernel/core/syscall.c').read_text(encoding='utf-8'))
        code = '\n'.join(function(source, name) for name in
                         ['stdin_line_key', 'stdin_read_line', 'stdin_read_raw', 'stdin_readkey'])
        code = code.replace('__asm__ volatile("cli")', '(host_if=0)')
        code = code.replace('__asm__ volatile("sti")', '(host_if=1)')
        code = code.replace('__asm__ volatile("sti; hlt")', 'host_yield()')
        assert '__asm__' not in code, 'Unmodelled privileged instruction'
        path, exe = work / 'returns.c', work / 'returns'
        path.write_text(RETURN_PREFIX + code + RETURN_MAIN, encoding='utf-8')
        subprocess.run([*cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path), '-o', str(exe)], check=True)
        failed = []
        for kind in range(3):
            for mode in range(7):
                result = subprocess.run([str(exe), str(kind), str(mode)],
                                        capture_output=True, text=True, timeout=10)
                if result.returncode:
                    assert result.returncode == 2 and result.stderr.strip() == (
                        f'RETURN_WITH_INTERRUPTS_ENABLED kind={kind} mode={mode}'), result.stderr
                    failed.append((kind, mode))
        return failed
    assert not run(), 'stdin return context must remain masked'
    print('PASS 21 stdin return cases under ASan/UBSan: canonical/raw/readkey, immediate/delayed key or signal, stop, no process, zero capacity/timeout; IRQs simulated')
    if compare_ref:
        failed = run(compare_ref)
        assert failed == [(kind, mode) for kind in range(3) for mode in (2, 3, 4)], failed
        print('PASS baseline exposes 9 interrupt/stop return failures; 12 controls pass')


def main():
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref', help='Pre-fix commit expected to fail the nine signal/stop cases')
    args = parser.parse_args()
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nyx-stdin-") as temporary:
        work = Path(temporary)
        check_returns(cc, work, args.compare_ref)
        source = work / "stdin_test.c"
        source.write_text(HARNESS.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        flags = ["-std=gnu99", "-O2", "-ffreestanding", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                 "-ffunction-sections", "-fdata-sections", "-no-pie", "-Wl,--gc-sections", "-fsanitize=address,undefined"]
        for command in ([*cc, *flags, str(source), "-o", str(work / "stdin_test")],
                        [str(work / "stdin_test")]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            if result.stdout:
                print(result.stdout.strip())


if __name__ == "__main__":
    main()
