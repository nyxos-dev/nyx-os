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
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    int state, blocked_in_kernel, waiting_for, wake_tick;
    uint32_t sig_pending, sig_mask, stop_sig, stopped_reported, pipe_wait_id;
    uint64_t sig_handlers[32], page_directory, sig_saved[18], sig_trampoline;
    uint32_t sig_active; int exit_code;
} process_t;
static process_t proc;
static process_t *process_table[512];
static int process_count;
static process_t *current;
static uint64_t user_cr3 = 123, user_rsp = 456;
static int scenario, sleeps, stopped, id;
static process_t *get_current_process(void) { return current; }
static void wake_waiters(process_t *p) { (void)p; stopped++; }
static void host_sleep(void);
static void multi_sleep(void);
static int multi_test(void);
static int lifetime_test(void);
static int reuse_test(void);
#define memset_asm memset

static jmp_buf terminated;
static int signal_events;
static int copy_to_user(uint64_t dst, const void *src, int n) {
    (void)dst; (void)src; (void)n; return 0;
}
static int stopped_test(void);
static void stopped_sleep(void);

'''

SUFFIX = r'''
static void host_sleep(void) {
    if (scenario >= 25) { stopped_sleep(); return; }
    if (scenario >= 23) {
        assert(++sleeps == 1);
        pipe_close_end(id, 0); pipe_close_end(id, 1);
        if (scenario == 24) {
            int replacement = pipe_new(); assert(replacement == id);
            assert(pipe_write(replacement, "X", 1) == 1);
        }
        return;
    }
    if (scenario >= 16) { multi_sleep(); return; }
    assert(++sleeps < 4);  /* a missed signal must fail, never hang the host */
    user_cr3 = 999; user_rsp = 888;  /* another process ran */
    if (proc.state == PROC_STOPPED) {
        assert(scenario == 4 || scenario == 5 || scenario == 8);
        assert(proc.pipe_wait_id == 0);
        if (scenario == 8) pipe_close_end(id, 1);
        else assert(pipe_write(id, "S", 1) == 1);
        assert(proc.state == PROC_STOPPED); /* pipe activity cannot resume STOP */
        signal_raise(&proc, SIGCONT);
        return;
    }
    assert(proc.state == PROC_BLOCKED && proc.blocked_in_kernel);
    assert(proc.pipe_wait_id == (uint32_t)(id + 1));
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
    if (scenario >= 25) return stopped_test();
    if (scenario >= 23) return reuse_test();
    if (scenario >= 20) return lifetime_test();
    if (scenario >= 16) return multi_test();
    if (scenario == 13) { assert(pipe_selftest() == 0); return 0; }
    current = &proc; proc.state = PROC_RUN;
    process_table[0] = &proc; process_count = 1;
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
    assert(proc.pipe_wait_id == 0);
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

static int reuse_test(void) {
    current = &proc; proc.state = PROC_RUN;
    process_table[0] = &proc; process_count = 1;
    id = pipe_new(); assert(id >= 0);
    char c = '?';
    assert(pipe_read(id, &c, 1) == -1 && c == '?');
    assert(!proc.pipe_wait_id && !proc.blocked_in_kernel);
    assert(user_cr3 == 123 && user_rsp == 456);
    if (scenario == 24) {
        assert(pipe_read(id, &c, 1) == 1 && c == 'X');
        pipe_close_end(id, 0); pipe_close_end(id, 1);
    } else assert(pipe_new() == id);
    return 0;
}

static void register_waiter(process_t *p, int pipe_id) {
    p->pipe_wait_id = (uint32_t)(pipe_id + 1);
    p->state = PROC_BLOCKED;
    /* BASELINE_REGISTER */
}

static int lifetime_test(void) {
    id = pipe_new(); assert(id >= 0);
    process_t *victim = calloc(1, sizeof(*victim)); assert(victim);
    process_table[0] = victim; process_count = 1;
    register_waiter(victim, id);
    if (scenario == 20) {
        /* Direct destroy/reap bypasses the suspended read's return path. */
        victim->state = PROC_ZOMBIE;
        assert(pipe_write(id, "X", 1) == 1);
        assert(victim->state == PROC_ZOMBIE);
        register_waiter(victim, id);
        process_table[0] = NULL; process_count = 0;
        free(victim);
        assert(pipe_write(id, "Y", 1) == 1);
        pipe_close_end(id, 1);
    } else if (scenario == 21) {
        int other = pipe_new(); assert(other >= 0);
        assert(pipe_write(other, "X", 1) == 1);
        assert(victim->state == PROC_BLOCKED);
        assert(pipe_write(id, "", 0) == 0);
        assert(victim->state == PROC_BLOCKED);
        victim->state = PROC_STOPPED;
        pipe_close_end(id, 1);
        assert(victim->state == PROC_STOPPED);
        free(victim); process_count = 0;
        pipe_close_end(other, 1); pipe_close_end(other, 0);
    } else {
        free(victim);
        process_t *all = calloc(512, sizeof(*all)); assert(all);
        process_count = 512;
        for (int i = 0; i < 512; i++) {
            process_table[i] = &all[i]; register_waiter(&all[i], id);
        }
        pipe_close_end(id, 1);
        for (int i = 0; i < 512; i++) assert(all[i].state == PROC_RUN);
        free(all); process_count = 0;
    }
    pipe_close_end(id, 0);
    return 0;
}

static process_t second;
static int multi_events;
static void multi_sleep(void) {
    assert(++multi_events < 5);
    if (current == &proc && multi_events == 1) {
        current = &second;
        user_cr3 = 222; user_rsp = 333;
        char c = '?';
        int n = pipe_read(id, &c, 1);
        assert(n == (scenario == 17 ? 0 : 1));
        if (n) assert(c == 'A');
        assert(user_cr3 == 222 && user_rsp == 333);
        current = &proc;
        return;
    }
    if (current == &second) {
        if (scenario == 17) pipe_close_end(id, 1);
        else if (scenario == 19) {
            signal_raise(&proc, SIGKILL);
            assert(pipe_write(id, "A", 1) == 1);
        } else assert(pipe_write(id, "AB", scenario == 18 ? 1 : 2) ==
                      (scenario == 18 ? 1 : 2));
        assert(proc.state == PROC_RUN && second.state == PROC_RUN);
        return;
    }
    assert(scenario == 18 && current == &proc && multi_events == 3);
    assert(proc.state == PROC_BLOCKED);
    assert(pipe_write(id, "B", 1) == 1);
}

static int multi_test(void) {
    current = &proc; proc.state = second.state = PROC_RUN;
    process_table[0] = &proc; process_table[1] = &second; process_count = 2;
    id = pipe_new(); assert(id >= 0);
    char c = '?';
    int n = pipe_read(id, &c, 1);
    assert(n == (scenario == 17 ? 0 : scenario == 19 ? -EINTR : 1));
    if (n == 1) assert(c == 'B');
    assert(user_cr3 == 123 && user_rsp == 456);
    assert(!proc.pipe_wait_id && !second.pipe_wait_id);
    assert(!proc.blocked_in_kernel && !second.blocked_in_kernel);
    pipe_close_end(id, 1); pipe_close_end(id, 0);
    return 0;
}

static void stopped_sleep(void) {
    if (proc.state == PROC_ZOMBIE) longjmp(terminated, 1);
    assert(++signal_events <= 2);
    if (proc.state == PROC_BLOCKED) {
        assert(scenario == 29);
        signal_raise(&proc, SIGSTOP);
        return;
    }
    assert(proc.state == PROC_STOPPED && proc.blocked_in_kernel);
    assert(!proc.pipe_wait_id);
    user_cr3 = 999; user_rsp = 888;
    signal_raise(&proc, SIGKILL);
    if (scenario == 27) signal_raise(&proc, SIGCONT);
    assert(proc.state == PROC_RUN);
}
static int stopped_test(void) {
    current = &proc; proc.state = PROC_RUN; proc.page_directory = 1;
    process_table[0] = &proc; process_count = 1;
    if (scenario == 25) {
        proc.state = PROC_STOPPED; proc.stop_sig = SIGSTOP;
        signal_raise(&proc, SIGKILL);
        assert(proc.state == PROC_RUN && proc.stop_sig == 0);
        assert(proc.sig_pending == (1u << SIGKILL));
        return 0;
    }
    if (setjmp(terminated)) {
        assert(proc.state == PROC_ZOMBIE && proc.exit_code == 137);
        assert(!proc.blocked_in_kernel && !proc.pipe_wait_id);
        assert(user_cr3 == 123 && user_rsp == 456);
        return 0;
    }
    if (scenario == 29) {
        id = pipe_new(); assert(id >= 0);
        char c = '?';
        assert(pipe_read(id, &c, 1) == -EINTR && c == '?');
        assert(!proc.blocked_in_kernel && !proc.pipe_wait_id);
        assert(user_cr3 == 123 && user_rsp == 456);
        pipe_close_end(id, 0); pipe_close_end(id, 1);
    } else if (scenario == 28) {
        proc.sig_pending = (1u << SIGINT) | (1u << SIGKILL);
        proc.sig_handlers[SIGINT] = 1234;
    } else proc.sig_pending = 1u << SIGSTOP;
    uint64_t frame[17] = {0};
    signal_dispatch(frame);
    assert(!"pending SIGKILL returned to user space");
    return 1;
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
                'uncatchable', 'default_is_ignore', 'signal_raise', 'signal_next_deliverable',
                'signal_pending', 'signal_check_stop', 'signal_terminate', 'signal_dispatch'))
            pipe = read('kernel/proc/pipe.c', ref).replace('#include "../core/kernel.h"', '')
            legacy = 'reader_waiter;' in pipe
            suffix = SUFFIX
            if legacy:
                suffix = suffix.replace('proc.pipe_wait_id == 0',
                                        'pipes[id].reader_waiter == NULL')
                suffix = suffix.replace('proc.pipe_wait_id == (uint32_t)(id + 1)',
                                        'pipes[id].reader_waiter == &proc')
                suffix = suffix.replace('/* BASELINE_REGISTER */',
                                        'pipes[pipe_id].reader_waiter = p;')
            source = PREFIX + defines + '\n' + functions + pipe + suffix
            source = source.replace('__asm__ volatile("cli")', '(void)0')
            source = source.replace('__asm__ volatile("sti")', '(void)0')
            source = source.replace('__asm__ volatile("hlt")', 'host_sleep()')
            source = source.replace('__asm__ volatile("sti; hlt")', 'host_sleep()')
            path, exe = work / 'test.c', work / 'test'
            path.write_text(source, encoding='utf-8')
            subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            str(path), '-o', str(exe)], check=True)
            failed = []
            for case in range(30):
                result = subprocess.run([str(exe), str(case)], capture_output=True, text=True)
                if result.returncode:
                    failed.append(case)
                    if not ref:
                        print(result.stderr)
            return failed

        assert not suite(), 'current pipe/signal cases failed'
        print('PASS 30 pipe/signal scenarios under ASan/UBSan; scheduler events simulated')
        if args.compare_ref:
            failed = suite(args.compare_ref)
            assert failed, 'baseline must reproduce a regression'
            print(f'Baseline {args.compare_ref}: {len(failed)} failures, cases {failed}')


if __name__ == '__main__':
    main()
