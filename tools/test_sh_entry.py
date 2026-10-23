#!/usr/bin/env python3
"""Exercise actual sh entry, pipeline preflight and job lifecycle with simulated syscalls."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int last_status;
static int run_command_list(char *line) {
    printf("CALL <%s>\n", line);
    last_status = atoi(getenv("NYX_TEST_STATUS"));
    return atoi(getenv("NYX_TEST_STOP"));
}
int main(int argc, char **argv) {
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    parser.add_argument('--compare-pipeline-ref')
    parser.add_argument('--compare-jobs-ref')
    args = parser.parse_args()
    cases = [(['-c', 'false'], status, stop, status, 'false')
             for status in (0, 1, 2, 127, 130, 255) for stop in (0, 1)]
    cases += [(['-c', command], 0, 0, 0, command)
              for command in ('', ' ', 'true', 'true' + ' ' * 123)]
    cases += [(['-c', command], 0, 0, 2, None)
              for command in ('x' * 128, 'x' * 4096, 'true' + ' ' * 123 + '; false',
                              '/rm.elf -- /tmp/fixture-' + 'a' * 130)]
    cases += [(['-c'], 0, 0, 2, None), ([], 0, 0, 99, None),
              (['-c', 'true', 'ignored'], 0, 0, 0, 'true')]
    with tempfile.TemporaryDirectory(prefix='nyx-sh-entry-') as temp:
        work = Path(temp)

        def suite(ref=None):
            source = (subprocess.check_output(['git', 'show', ref + ':user/sh.c'], text=True)
                      if ref else (ROOT / 'user/sh.c').read_text(encoding='utf-8'))
            start = source.index('    /* sh -c "one command line"')
            end = source.index('\n    /* Interactive REPL:', start)
            path, exe = work / 'entry.c', work / 'entry'
            path.write_text(PREFIX + source[start:end] + '\nreturn 99;\n}\n', encoding='utf-8')
            subprocess.run(['gcc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(path), '-o', str(exe)], check=True)
            failures = []
            for argv, status, stop, expected, call in cases:
                env = dict(os.environ, NYX_TEST_STATUS=str(status), NYX_TEST_STOP=str(stop))
                result = subprocess.run([str(exe), *argv], env=env, text=True,
                                        capture_output=True, timeout=10)
                recorded = [line for line in result.stdout.splitlines() if line.startswith('CALL ')]
                if result.returncode != expected or result.stderr or recorded != ([] if call is None else ['CALL <' + call + '>']):
                    failures.append((argv, status, stop, result.returncode, recorded, result.stderr))
            return failures

        failures = suite()
        assert not failures, failures
        print(f'PASS {len(cases)} sanitized sh entry cases; parser stub only, no commands executed')
        if args.compare_ref:
            failures = suite(args.compare_ref)
            assert any(f[0] == ['-c'] for f in failures), failures
            assert any(f[0] == ['-c', 'false'] and f[1] == 2 for f in failures), failures
            assert any(f[0] == ['-c', 'x' * 128] for f in failures), failures
            print(f'PASS baseline fails {len(failures)} cases: status, missing argument and truncation')


        def pipeline_suite(ref=None):
            source = (subprocess.check_output(['git', 'show', ref + ':user/sh.c'], text=True)
                      if ref else (ROOT / 'user/sh.c').read_text(encoding='utf-8'))
            trim = source[source.index('static char* trim('):source.index('/* Split a stage')]
            start = source.index('static void run_line(')
            end = source.index('    long pids[MAX_STAGES];', start)
            prefix = source[start:end]
            code = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_STAGES 4
static int last_status;
""" + trim + prefix + r"""
    (void)background; (void)jobcmd;
    for (int i=0;i<nstages;i++) printf("LAUNCH <%s>\n",trim(stages[i]));
}
int main(int argc,char**argv) {
    if(argc!=2) return 99;
    run_line(argv[1]);
    return last_status;
}
"""
            code = code.replace("#define MAX_STAGES 4", next(line for line in source.splitlines()
                                if line.startswith("#define MAX_STAGES ")))
            path, exe = work/'pipeline.c', work/'pipeline'
            path.write_text(code, encoding='utf-8')
            subprocess.run(['gcc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(path), '-o', str(exe)], check=True)
            # This records entry into launch; target tests separately check actual file preservation.
            checks=[]
            for n in range(1,9):
                parts=['echo x > /mnt/keep.txt']+['cat']*(n-1)
                for suffix in ('', ' &'):
                    checks.append((' | '.join(parts)+suffix, 0 if n<=4 else 2,
                                   parts if n<=4 else []))
            for text in ('', ' ', '|', ' | ', '| cat', 'cat |', 'cat |   ',
                         'cat | | cat', 'cat |   | cat', '&', ' | &',
                         'echo bad > /mnt/keep.txt |', 'cat | cat | cat |'):
                checks.append((text,2,[]))
            # Quoted pipe/space placeholders arrive already protected by preprocess_quotes.
            checks += [('echo a\x04b | cat',0,['echo a\x04b','cat']),
                       ('echo \x01 | cat &',0,['echo \x01','cat'])]
            failed=[]
            for line,status,stages in checks:
                result=subprocess.run([str(exe),line],text=True,capture_output=True,timeout=10)
                launched=[x for x in result.stdout.splitlines() if x.startswith('LAUNCH ')]
                if result.returncode!=status or result.stderr or launched!=['LAUNCH <'+x+'>' for x in stages]:
                    failed.append((line,result.returncode,launched,result.stderr))
            return checks,failed

        checks,failed=pipeline_suite()
        assert not failed,failed
        print(f'PASS {len(checks)} sanitized pipeline preflight cases; invalid stages never enter launch')
        if args.compare_pipeline_ref:
            checks,failed=pipeline_suite(args.compare_pipeline_ref)
            assert any(x[0]=='echo bad > /mnt/keep.txt |' for x in failed),failed
            assert any(x[0].count('|')==4 for x in failed),failed
            print(f'PASS pipeline baseline fails {len(failed)} cases: empty stages and capacity')

        failures = jobs_suite(work)
        assert not failures, failures
        print('PASS 17 sanitized shell job lifecycle cases; actual job/launch functions, simulated syscalls')
        if args.compare_jobs_ref:
            failures = jobs_suite(work, args.compare_jobs_ref)
            assert {1, 3, 5, 9, 10, 11, 13, 16}.issubset({n for n, _, _ in failures}), failures
            print(f'PASS job baseline fails {len(failures)} lifecycle cases')


def jobs_suite(work, ref=None):
    source = (subprocess.check_output(['git', 'show', ref + ':user/sh.c'], text=True)
              if ref else (ROOT / 'user/sh.c').read_text(encoding='utf-8'))
    trim = source[source.index('static char* trim('):source.index('/* Split a stage')]
    jobs = source[source.index('#define MAX_JOBS '):source.index('/* ---- filename globbing')]
    launch = source[source.index('static void run_line('):source.index('/* ---- command lists:')]
    code = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define MAX_STAGES 4
#define MAX_ARGS 12
#define WNOHANG 1
#define WUNTRACED 2
#define WSTOPPED 0x10000
#define WIFSTOPPED(s) ((s)&WSTOPPED)
#define SIGCONT 18
#define SIGKILL 9
#define O_RDONLY 0
#define O_CREAT 1
#define O_APPEND 2
#define O_TRUNC 4
#define UNUSED __attribute__((unused))
static int last_status, tick, forks, pipes, fail_fork, fail_pipe, closes, wildcard_waits;
static int fg_pid, conts[256], kills[256], waits[256], ready[256], status[256], consumed[256];
static int mock_pipe(int *p) { pipes++; if(pipes==fail_pipe) return -1; p[0]=100+pipes*2; p[1]=p[0]+1; return 0; }
static long mock_fork(void) { forks++; return forks==fail_fork?-1:9+forks; }
static long mock_close(int fd) { assert(fd>=0); closes++; return 0; }
static long mock_kill(int pid,int sig) {
    assert(pid>0 && pid<256);
    if(sig==SIGCONT) { conts[pid]++; status[pid]=0; ready[pid]=tick+1; }
    else { assert(sig==SIGKILL); kills[pid]++; status[pid]=137; ready[pid]=tick; }
    return 0;
}
static long mock_wait(int pid,int *st,int options) {
    if(pid==-1) { wildcard_waits++; return -1; }
    assert(pid>0 && pid<256); waits[pid]++;
    if(consumed[pid]) return -1;
    if(ready[pid]>tick) {
        if(options&WNOHANG) return 0;
        tick=ready[pid];
    }
    *st=status[pid];
    if(WIFSTOPPED(*st)) ready[pid]=100000;
    else consumed[pid]=1;
    return pid;
}
static long UNUSED mock_sleep(long ms) { assert(ms>0); assert(++tick<1000); return 0; }
static long mock_setfg(long pid) { assert(pid==99 || !consumed[pid]); fg_pid=(int)pid; return 0; }
static long mock_getpid(void) { return 99; }
#define pipe mock_pipe
#define fork mock_fork
#define close mock_close
#define kill mock_kill
#define waitpid3 mock_wait
#define sleep_ms mock_sleep
#define setfg mock_setfg
#define getpid mock_getpid
static int split_args(char *p,char **av,int max) { (void)p;(void)av;(void)max; abort(); }
static void parse_redir(char **av,int *ac,char **in,char **out,int *app,char **err,int *eapp) {
    (void)av;(void)ac;(void)in;(void)out;(void)app;(void)err;(void)eapp; abort();
}
static char **expand_globs(char **av,int *ac) { (void)ac; return av; }
static void q_unprotect(char *p) { (void)p; }
static long dup2(int a,int b) { (void)a;(void)b; abort(); }
static long open(const char *p,int f,int m) { (void)p;(void)f;(void)m; abort(); }
static void resolve(const char *p,char *out) { (void)p;(void)out; abort(); }
static char **env_build(void) { return 0; }
static long mock_exec(const char *p,char **av,char **env) { (void)p;(void)av;(void)env; abort(); }
#define execve mock_exec
''' + trim + jobs + launch + r'''
static void run(const char *s) { char b[128]; strcpy(b,s); run_line(b); }
static void event(int pid,int at,int st) { ready[pid]=at; status[pid]=st; }
int main(int argc,char **argv) {
    assert(argc==2);
    for(int i=0;i<256;i++) ready[i]=100;
    int test=atoi(argv[1]);
    switch(test) {
    case 1: /* Last stage exits while upstream still lives. */
        run("sleep 30 | true &"); event(11,0,0); reap_jobs();
        assert(jobs[0].pid); assert(!consumed[10] && consumed[11]); break;
    case 2: /* First stage exits before last. */
        run("true | sleep 30 &"); event(10,0,0); reap_jobs();
        assert(jobs[0].pid && consumed[10] && !consumed[11]); break;
    case 3: /* fg preserves already collected final-stage failure. */
        run("sleep 1 | false &"); event(11,0,7); event(10,2,0); reap_jobs();
        builtin_fg(""); assert(last_status==7 && consumed[10] && consumed[11] && !jobs[0].pid); break;
    case 4: /* Upstream stop, downstream already exited. */
        run("stop | true &"); event(10,0,WSTOPPED|19); event(11,0,0); reap_jobs();
        builtin_bg(""); assert(conts[10]==1 && conts[11]==0); break;
    case 5: /* Both stopped members must resume. */
        run("stop | stop &"); event(10,0,WSTOPPED|19); event(11,0,WSTOPPED|19); reap_jobs();
        builtin_bg(""); assert(conts[10]==1 && conts[11]==1); break;
    case 6: /* Killed stopped member is collected without bg. */
        run("stop &"); event(10,0,WSTOPPED|19); reap_jobs();
        mock_kill(10,SIGKILL); reap_jobs(); assert(consumed[10] && !jobs[0].pid); break;
    case 7: /* A late-stage stop must not wait for upstream termination. */
        event(10,10,0); event(11,0,WSTOPPED|19); run("reader | stop");
        assert(tick==0 && !consumed[10] && jobs[0].pid && fg_pid==99); break;
    case 8: /* fg waits for every live member. */
        run("sleep 2 | sleep 1 &"); event(10,2,0); event(11,1,0);
        builtin_fg(""); assert(consumed[10] && consumed[11] && tick>=2); break;
    case 9: /* Never steal an unrelated child's status. */
        run("true &"); event(10,0,0); reap_jobs(); assert(!wildcard_waits); break;
    case 10: /* Full table refuses launch, even for a foreground pipeline. */
        for(int i=0;i<MAX_JOBS;i++) run("sleep 30 &");
        run("echo bad > /mnt/keep.txt | cat");
        assert(forks==MAX_JOBS && pipes==0 && last_status==1); break;
    case 11: /* Failure creating second pipe closes prior end, kills/reaps first. */
        fail_pipe=2; run("cat | cat | cat");
        assert(forks==1 && closes==2 && kills[10]==1 && consumed[10] && last_status==1); break;
    case 12: /* First fork failure closes both newly allocated ends. */
        fail_fork=1; run("cat | cat");
        assert(closes==2 && !jobs[0].pid && last_status==1); break;
    case 13: /* Second fork failure closes old/new ends and all launched children. */
        fail_fork=2; run("cat | cat | cat");
        assert(closes==4 && kills[10]==1 && consumed[10] && last_status==1); break;
    case 14: /* Repeated sweeps never re-wait a consumed PID. */
        run("true &"); event(10,0,0); reap_jobs(); int n=waits[10]; reap_jobs();
        assert(!jobs[0].pid && waits[10]==n); break;
    case 15: /* Successful background launch sets status zero. */
        last_status=7; run("sleep 1 &"); assert(last_status==0); break;
    case 16: /* Keep four stages after the last three have exited. */
        run("sleep 30 | true | true | true &");
        for(int i=11;i<=13;i++) event(i,0,i);
        reap_jobs(); assert(jobs[0].pid && !consumed[10]);
        event(10,2,0); builtin_fg(""); assert(last_status==13 && consumed[10]); break;
    case 17: /* bg resumes stopped members without disturbing another job. */
        run("stop | stop &"); run("sleep 30 &");
        event(10,0,WSTOPPED|19); event(11,0,WSTOPPED|19); reap_jobs();
        builtin_bg("1"); assert(conts[10]==1 && conts[11]==1 && !conts[12]); break;
    default: abort();
    }
    builtin_jobs();
    return 0;
}
'''
    path, exe = work/'jobs.c', work/'jobs'
    path.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path), '-o', str(exe)], check=True)
    failed=[]
    for n in range(1,18):
        result=subprocess.run([str(exe),str(n)],text=True,capture_output=True,timeout=10)
        if result.returncode or result.stderr: failed.append((n,result.returncode,result.stderr))
    return failed


if __name__ == '__main__':
    main()
