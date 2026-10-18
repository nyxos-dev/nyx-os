#!/usr/bin/env python3
"""Exercise bootstrap N/N++ output failures with real files and stdio faults."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = b"fn main() -> i64 { 0 }\n"
WRAPPER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static FILE* output;
static int sticky, injected;
FILE* __real_fopen(const char*, const char*);
int __real_fclose(FILE*);
int __real_fflush(FILE*);
int __real_ferror(FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
static int is_output(FILE* f) { return f == stdout || f == output; }
static int mode(const char* name) {
    const char* value = getenv("NYX_OUTPUT_FAULT");
    return value && strcmp(value, name) == 0;
}
FILE* __wrap_fopen(const char* path, const char* flags) {
    FILE* f = __real_fopen(path, flags);
    if (f && flags[0] == 'w') output = f;
    return f;
}
int __wrap_fprintf(FILE* f, const char* fmt, ...) {
    if (is_output(f) && mode("write")) { sticky = injected = 1; return -1; }
    if (is_output(f) && mode("sticky")) sticky = injected = 1;
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap); return n;
}
size_t __wrap_fwrite(const void* p, size_t size, size_t count, FILE* f) {
    if (is_output(f) && mode("write") && count) {
        injected = 1; /* A short count must fail even without ferror. */
        return __real_fwrite(p, size, count - 1, f);
    }
    if (is_output(f) && mode("sticky")) sticky = injected = 1;
    return __real_fwrite(p, size, count, f);
}
int __wrap_ferror(FILE* f) { return __real_ferror(f) || (is_output(f) && sticky); }
int __wrap_fflush(FILE* f) {
    int rc = __real_fflush(f);
    if (is_output(f) && mode("flush")) { injected = 1; return EOF; }
    return rc;
}
int __wrap_fclose(FILE* f) {
    int fault = f == output && mode("close");
    if (f == output) output = NULL;
    int rc = __real_fclose(f);
    if (fault) { injected = 1; return EOF; }
    return rc;
}
__attribute__((destructor)) static void check_cleanup(void) {
    if (output || !injected) _Exit(97);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ncc-source', type=Path, default=ROOT / 'lang/ncc/ncc.c')
    parser.add_argument('--nppc-source', type=Path, default=ROOT / 'lang/nppc/nppc.c')
    args = parser.parse_args()
    cc = shlex.split(os.environ.get('CC', 'cc'))
    failures, count = [], 0

    def check(label, condition):
        nonlocal count
        count += 1
        print(('PASS ' if condition else 'FAIL ') + label)
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory(prefix='nyx-n-output-') as temporary:
        work = Path(temporary)
        source = work / 'main.n'
        source.write_bytes(SOURCE)
        wrapper = work / 'fault.c'
        wrapper.write_text(WRAPPER)
        binaries = {}
        for name, path in [('ncc', args.ncc_source), ('nppc', args.nppc_source)]:
            for fault in [False, True]:
                binary = work / (name + ('-fault' if fault else ''))
                # Keep stdio calls visible to --wrap, including on fortified hosts.
                command = [*cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-fno-builtin', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
                           str(path), '-o', str(binary)]
                if fault:
                    command += [str(wrapper), *['-Wl,--wrap=' + f for f in
                        ['fopen', 'fclose', 'fflush', 'ferror', 'fprintf', 'fwrite']]]
                subprocess.run(command, check=True, capture_output=True, timeout=90)
                binaries[name, fault] = binary

        for name, mode in [('nppc', 'file'), ('ncc', 'file'), ('ncc', 'stdout'),
                           ('ncc', 'tokens'), ('ncc', 'ast')]:
            output = work / (name + '-' + mode + '.out')

            def run(fault=None, destination=None):
                command = [str(binaries[name, fault is not None]), str(source)]
                if mode == 'file':
                    command += ['-o', str(destination or output)]
                elif mode in ['tokens', 'ast']:
                    command += ['--' + mode]
                env = dict(os.environ)
                env.pop('NYX_OUTPUT_FAULT', None)
                if fault:
                    env['NYX_OUTPUT_FAULT'] = fault
                with open(destination if destination and mode != 'file' else os.devnull, 'wb') as sink:
                    return subprocess.run(command, stdout=sink if destination and mode != 'file' else subprocess.PIPE,
                        stderr=subprocess.PIPE, env=env, timeout=30)

            normal = run()
            good = output.read_bytes() if mode == 'file' else normal.stdout
            check(name + '-' + mode + '-success', normal.returncode == 0 and bool(good))
            if name == 'nppc':
                check('nppc-preserves-plain-source', good == SOURCE)
            if name == 'ncc' and mode == 'stdout':
                check('ncc-file-equals-stdout', good == (work / 'ncc-file.out').read_bytes())
            for fault in ['write', 'sticky', 'close' if mode == 'file' else 'flush']:
                result = run(fault)
                if result.returncode != 1 or b'write error' not in result.stderr:
                    print(f'  status={result.returncode} stderr={result.stderr!r}')
                check(name + '-' + mode + '-' + fault,
                      result.returncode == 1 and b'write error' in result.stderr and b': OK' not in result.stderr)
            result = run(destination=Path('/dev/full'))
            check(name + '-' + mode + '-dev-full',
                  result.returncode == 1 and b'write error' in result.stderr and b': OK' not in result.stderr)
            if mode == 'file':
                result = run(destination=work / 'missing' / 'output')
                check(name + '-open-error', result.returncode == 1 and b'cannot open' in result.stderr)
        check('packaged-ncc-matches-canonical',
              (ROOT / 'lang/ncc/ncc.c').read_bytes() == (ROOT / 'user/pkg/ncc/ncc.c').read_bytes())
    print(f'{count - len(failures)}/{count} checks passed')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
