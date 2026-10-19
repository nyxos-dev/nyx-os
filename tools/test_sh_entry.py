#!/usr/bin/env python3
"""Exercise the actual sh -c entry with a recording command-list stub."""
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


if __name__ == '__main__':
    main()
