#!/usr/bin/env python3
"""Exercise actual sh -c entry and pipeline preflight before a recording launch boundary."""
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


if __name__ == '__main__':
    main()
