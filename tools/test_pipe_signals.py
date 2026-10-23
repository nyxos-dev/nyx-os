#!/usr/bin/env python3
"""Run actual pipe/signal wait logic with deterministic scheduler events on the host.

Privileged instructions and scheduling are simulated; this does not replace QEMU
checks of syscall return, signal delivery, stop/continue or process reaping.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    int state, blocked_in_kernel, waiting_for, wake_tick;
    uint32_t sig_pending, sig_mask, stop_sig, stopped_reported;
    uint64_t sig_handlers[32];
} process_t;
static process_t proc;
static process_t *current;
static uint64_t user_cr3 = 123, user_rsp = 456;
static int scenario, sleeps, stopped, id;
static process_t *get_current_process(void) { return current; }
static void wake_waiters(process_t *p) { (void)p; stopped++; }
static void host_sleep(void);
#define memset_asm memset
'''

SUFFIX = r'''
static void host_sleep(void) {
    assert(++sleeps < 4);  /* a missed signal must fail, never hang the host */
    user_cr3 = 999; user_rsp = 888;  /* another process ran */
    if (proc.state == PROC_STOPPED) {
        assert(scenario == 4 || scenario == 5 || scenario == 8);
        assert(pipes[id].reader_waiter == NULL);
        if (scenario == 8) pipe_close_end(id, 1);
        else assert(pipe_write(id, "S", 1) == 1);
        assert(proc.state == PROC_STOPPED); /* pipe activity cannot resume STOP */
        signal_raise(&proc, SIGCONT);
        return;
    }
    assert(proc.state == PROC_BLOCKED && proc.blocked_in_kernel);
    assert(pipes[id].reader_waiter == &proc);
    if (sleeps == 1) {
        if (scenario == 0 || scenario == 9) signal_raise(&proc, SIGINT);
        else if (scenario == 1) signal_raise(&proc, SIGKILL);
        else if (scenario == 2) signal_raise(&proc, SIGTERM);
        else if (scenario == 3) signal_raise(&proc, SIGUSR1);
        else if (scenario == 4 || scenario == 8) signal_raise(&proc, SIGSTOP);
        else if (scenario == 5 || scenario == 10 || scenario == 11)
            signal_raise(&proc, SIGTSTP);
        else if (scenario == 6) pipe_close_end(id, 1);
        else if (scenario == 7) proc.state = PROC_RUN;
        else if (scenario == 14) {
            signal_raise(&proc, SIGTSTP);
            signal_raise(&proc, SIGKILL);
        } else if (scenario == 15) signal_raise(&proc, SIGTSTP);
    } else {
        assert(scenario == 3 || scenario == 7 || scenario == 15);
        assert(pipe_write(id, "D", 1) == 1);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    scenario = atoi(argv[1]);
    if (scenario == 13) { assert(pipe_selftest() == 0); return 0; }
    current = &proc; proc.state = PROC_RUN;
    id = pipe_new(); assert(id >= 0);
    if (scenario == 3) proc.sig_mask = 1u << SIGUSR1;
    if (scenario == 15) proc.sig_mask = 1u << SIGTSTP;
    if (scenario == 9) {
        proc.sig_mask = 1u << SIGTSTP;
        proc.sig_pending = 1u << SIGTSTP;
    }
    if (scenario == 10) proc.sig_handlers[SIGTSTP] = 1234;
    if (scenario == 11) proc.sig_handlers[SIGTSTP] = SIG_IGN;
    if (scenario == 12) {
        signal_raise(&proc, SIGKILL);
        assert(pipe_write(id, "D", 1) == 1);
    }
    char out = '?';
    int got = pipe_read(id, &out, 1);
    assert(!proc.blocked_in_kernel);
    assert(pipes[id].reader_waiter == NULL);
    assert(user_cr3 == 123 && user_rsp == 456);
    if (scenario == 3 || scenario == 7 || scenario == 12 || scenario == 15)
        assert(got == 1 && out == 'D');
    else if (scenario == 4 || scenario == 5)
        assert(got == 1 && out == 'S' && stopped == 1);
    else if (scenario == 6 || scenario == 8)
        assert(got == 0 && out == '?');
    else {
        assert(got == -EINTR && out == '?' && stopped == 0);
        assert(proc.sig_pending); /* delivery belongs to syscall return */
    }
    /* An interrupted/reaped reader cannot be revived through a stale waiter. */
    proc.state = PROC_ZOMBIE;
    assert(pipe_write(id, "X", 1) == 1);
    assert(proc.state == PROC_ZOMBIE);
    pipe_close_end(id, 1);
    assert(proc.state == PROC_ZOMBIE);
    pipe_close_end(id, 0);
    assert(!pipes[id].used);
    return 0;
}
'''


def function(source, name):
    start = re.search(r'^(?:static )?(?:int|void) ' + name + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args = parser.parse_args()

    def read(path, ref):
        return (subprocess.check_output(['git', 'show', ref + ':' + path], text=True)
                if ref else (ROOT / path).read_text(encoding='utf-8'))

    with tempfile.TemporaryDirectory(prefix='nyx-pipe-signals-') as temp:
        work = Path(temp)

        def suite(ref=None):
            header = read('kernel/core/kernel.h', ref)
            defines = '\n'.join(line for line in header.splitlines()
                                if re.match(r'#define (NSIG|SIG\w+|PROC_(RUN|STOPPED|BLOCKED|ZOMBIE)|EINTR)\s', line))
            signal = read('kernel/proc/signal.c', ref)
            functions = ''.join(function(signal, n) for n in (
                'signal_raise', 'signal_next_deliverable', 'signal_pending', 'signal_check_stop'))
            pipe = read('kernel/proc/pipe.c', ref).replace('#include "../core/kernel.h"', '')
            source = PREFIX + defines + '\n' + functions + pipe + SUFFIX
            source = source.replace('__asm__ volatile("cli")', '(void)0')
            source = source.replace('__asm__ volatile("sti; hlt")', 'host_sleep()')
            path, exe = work / 'test.c', work / 'test'
            path.write_text(source, encoding='utf-8')
            subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            str(path), '-o', str(exe)], check=True)
            failed = []
            for case in range(16):
                result = subprocess.run([str(exe), str(case)], capture_output=True, text=True)
                if result.returncode:
                    failed.append(case)
                    if not ref:
                        print(result.stderr)
            return failed

        assert not suite(), 'current pipe/signal cases failed'
        print('PASS 16 pipe/signal scenarios under ASan/UBSan; scheduler events simulated')
        if args.compare_ref:
            failed = suite(args.compare_ref)
            assert {0, 1, 2, 4, 5}.issubset(failed), failed
            print(f'Baseline {args.compare_ref}: {len(failed)} failures, cases {failed}')


if __name__ == '__main__':
    main()
