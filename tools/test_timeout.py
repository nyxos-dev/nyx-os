#!/usr/bin/env python3
"""Run the kernel timeout command with virtual ticks and process stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
enum { PROC_READY, PROC_ZOMBIE, PROC_STOPPED, SIGCONT, SIGKILL };
typedef struct { int state; } process_t;
static process_t child;
uint32_t g_foreground_pid;
static uint32_t ticks, step;
static uint64_t elapsed, exit_at, killed_at;
static int spawned, killed, reaped, resumed, resolved, draws, checks;
static int resolve_ok, spawn_ok, exit_code;
static char output[512];
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static int test_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(output + strlen(output), sizeof(output) - strlen(output), fmt, ap);
    va_end(ap); return n;
}
static int resolve_user_elf(const char* name, char* out, int cap) {
    (void)name; (void)out; (void)cap; resolved++; return resolve_ok;
}
static int spawn_user_path_args(const char* path, char** argv, int argc) {
    (void)path; spawned++; CHECK(argc == 2 && !strcmp(argv[1], "argument"));
    return spawn_ok ? 42 : -1;
}
static process_t* find_process(uint32_t pid) { CHECK(pid == 42); return &child; }
static void signal_raise(process_t* p, int sig) {
    CHECK(p == &child);
    if (sig == SIGKILL) { killed++; killed_at = elapsed; child.state = PROC_ZOMBIE; }
    else { CHECK(sig == SIGCONT); resumed++; child.state = PROC_READY; }
}
static uint32_t get_ticks(void) { return ticks; }
static void compositor_redraw_now(void) { draws++; }
static void sleep(int ms) {
    CHECK(ms == 60 && elapsed < 10000000000ULL);
    ticks += step; elapsed += step;
    if (elapsed >= exit_at) child.state = PROC_ZOMBIE;
}
static int kwait(uint32_t pid) { CHECK(pid == 42); reaped++; return exit_code; }
#define printf test_printf
@PARSER@
@COMMAND@
#undef printf
static void reset(uint32_t start, uint32_t advance, uint64_t finish) {
    ticks = start; step = advance; elapsed = 0; exit_at = finish; killed_at = 0;
    spawned = killed = reaped = resumed = resolved = draws = 0;
    resolve_ok = spawn_ok = 1; exit_code = 7;
    child.state = PROC_READY; g_foreground_pid = 0; output[0] = 0;
}
static void run(const char* duration) {
    char* argv[] = {"timeout", (char*)duration, "fixture", "argument"};
    cmd_timeout(4, argv);
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "overflow")) {
        reset(0, 60, 120); run("4294968");
        CHECK(!resolved && !spawned && !reaped);
        return 0;
    }
    const char* invalid[] = {"", "0", "-1", "1x", "1.5", "1s", "1 2", "0x10",
        "4294968", "4294967296", "2147483647", "9999999999999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
        reset(0, 60, 120); run(invalid[i]);
        CHECK(!resolved && !spawned && !reaped && !draws && !g_foreground_pid);
        CHECK(strstr(output, "invalid duration"));
    }
    const char* valid[] = {"1", "0001", "+1", " 1 ", "2147484", "4294967"};
    for (unsigned i = 0; i < sizeof(valid)/sizeof(*valid); i++) {
        reset(0, 60, 120); run(valid[i]);
        CHECK(spawned == 1 && reaped == 1 && !killed && !g_foreground_pid);
        CHECK(strstr(output, "code 7"));
    }
    uint32_t starts[] = {0, 0xfffffe00u, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(starts)/sizeof(*starts); i++) {
        reset(starts[i], 60, UINT64_MAX); run("1");
        CHECK(killed == 1 && killed_at == 1020 && reaped == 1 && !g_foreground_pid);
        CHECK(strstr(output, "status 124"));
        reset(starts[i], 60, 960); run("1");
        CHECK(!killed && reaped == 1 && strstr(output, "code 7"));
        reset(starts[i], 4294967000u, UINT64_MAX); run("4294967");
        CHECK(killed == 1 && killed_at == 4294967000ULL && reaped == 1);
    }
    reset(0, 60, 120); child.state = PROC_STOPPED; run("1");
    CHECK(resumed == 1 && !killed && reaped == 1);
    reset(0, 60, 120); resolve_ok = 0; run("1");
    CHECK(resolved == 1 && !spawned && !reaped);
    reset(0, 60, 120); spawn_ok = 0; run("1");
    CHECK(spawned == 1 && !reaped && !g_foreground_pid);
    reset(0, 60, 120); char* args[] = {"timeout"}; cmd_timeout(1, args);
    CHECK(!spawned && strstr(output, "Usage:"));
    printf("PASS %d timeout command checks (virtual clock and process stubs)\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()
    numeric = (ROOT/'kernel/core/numparse.c').read_text()
    numeric = numeric[numeric.index('static int np_is_blank'):numeric.index('// KAT:')]
    with tempfile.TemporaryDirectory(prefix='nyx-timeout-') as temp:
        work = Path(temp)

        def run(source, arguments=()):
            start = source.index('static void cmd_timeout(int argc, char** argv) {')
            command = source[start:source.index('\n}\n', start)+3]
            path = work/'timeout.c'; exe = work/'timeout'
            path.write_text(HARNESS.replace('@PARSER@', numeric).replace('@COMMAND@', command))
            subprocess.run(['gcc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(path), '-o', str(exe)], check=True)
            return subprocess.run([str(exe), *arguments], capture_output=True, text=True, timeout=10)

        result = run((ROOT/'kernel/core/kernel.c').read_text())
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end='')
        if args.compare_ref:
            source = subprocess.check_output(['git', 'show', args.compare_ref+':kernel/core/kernel.c'], text=True)
            result = run(source)
            assert result.returncode != 0 and '!resolved && !spawned' in result.stderr, result.stdout+result.stderr
            print('PASS baseline incorrectly starts a child for malformed duration')
            result = run(source, ['overflow'])
            assert result.returncode != 0 and '!resolved && !spawned' in result.stderr, result.stdout+result.stderr
            print('PASS baseline incorrectly starts a child for overflowing milliseconds')


if __name__ == '__main__':
    main()
