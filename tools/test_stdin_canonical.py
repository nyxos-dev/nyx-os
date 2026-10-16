#!/usr/bin/env python3
"""Exercise the real canonical read loop with a scripted keyboard and echo sink.

Run on Linux x86_64 or WSL. No process is scheduled and no privileged instruction
executes; blocking, signal delivery and IRQ scheduling still require kernel tests.
"""
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

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
process_t* get_current_process(void) { return NULL; }
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
    printf("OK: EOF, bounded reads without lost keys, editing, echo and scratch-slot restoration\n");
    return 0;
}
'''


def main():
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nyx-stdin-") as temporary:
        work = Path(temporary)
        source = work / "stdin_test.c"
        source.write_text(HARNESS.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        flags = ["-std=gnu99", "-O2", "-ffreestanding", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                 "-ffunction-sections", "-fdata-sections", "-no-pie", "-Wl,--gc-sections"]
        for command in ([*cc, *flags, str(source), "-o", str(work / "stdin_test")],
                        [str(work / "stdin_test")]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            if result.stdout:
                print(result.stdout.strip())


if __name__ == "__main__":
    main()
