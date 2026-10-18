#!/usr/bin/env python3
"""Fail each observed N driver allocation through the Linux syscall shim."""
import argparse
import os
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_driver_io import ROOT, SOURCE, TOOLS, require


def allocation_shim(text):
    anchor = 'static inline nyx_i64 __nyx_syscall6'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '#include <stdlib.h>\nstatic int allocation_calls;\n' + anchor)
    anchor = '    if (no == 7) {'
    hook = '''    if (no == 1 && a1 == 2 && getenv("NYX_ALLOC_STDERR_BROKEN")) return -5;
    if (no == 7) {
        const char* fail = getenv("NYX_ALLOC_FAIL");
        const char* value = getenv("NYX_ALLOC_VALUE");
        ++allocation_calls;
        if (getenv("NYX_ALLOC_TRACE")) {
            const char marker = 'A';
            long result;
            __asm__ volatile("syscall" : "=a"(result)
                : "a"(1L), "D"(2L), "S"(&marker), "d"(1L)
                : "rcx", "r11", "memory");
        }
        if (fail && allocation_calls == atoi(fail)) return value ? atol(value) : -1;
    }
'''
    assert text.count(anchor) == 1
    return text.replace(anchor, hook + anchor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, default=ROOT/'selfhost')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        parser.error('requires Linux x86_64 or WSL')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    failures, checks = [], 0
    with tempfile.TemporaryDirectory(prefix='n-alloc-') as temporary:
        work = Path(temporary)
        require(['gcc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', ROOT/'ncc/ncc.c', '-o', work/'ncc'])
        shutil.copyfile(ROOT.parent/'user/nyxrt.c', work/'nyxrt.c')
        (work/'nyxrt.h').write_text(allocation_shim((ROOT/'ncc/host/nyxrt.h').read_text()))
        simple = work/'input.n'; simple.write_bytes(SOURCE)
        fixtures = [simple] + [ROOT/'examples'/f'{name}.n' for name in
            ('hello', 'structs', 'structenum', 'matchexpr', 'results', 'ownnest', 'fntype')]
        env = {key: value for key, value in os.environ.items() if not key.startswith('NYX_ALLOC_')}
        def run(tool, source, **extra):
            return subprocess.run([work/tool, source], capture_output=True,
                                  timeout=30, env={**env, **extra})
        for tool in TOOLS:
            require([work/'ncc', args.sources.resolve()/f'{tool}.n', '-o', work/f'{tool}.c'])
            require(['gcc', '-std=c99', '-pedantic-errors', '-ffreestanding', '-O2', work/f'{tool}.c', work/'nyxrt.c', '-o', work/tool])
            observed = 0
            for source in fixtures:
                normal = run(tool, source)
                traced = run(tool, source, NYX_ALLOC_TRACE='1')
                assert normal.returncode == 0 and not normal.stderr, (tool, source, normal)
                assert traced.returncode == 0 and traced.stdout == normal.stdout
                count = len(traced.stderr)
                assert count >= 2 and traced.stderr == b'A' * count
                observed += count
                for allocation in range(1, count + 1):
                    for value, broken in (('-1', False), ('-12', False), ('0', False), ('-1', True)):
                        result = run(tool, source, NYX_ALLOC_FAIL=str(allocation), NYX_ALLOC_VALUE=value,
                                     **({'NYX_ALLOC_STDERR_BROKEN':'1'} if broken else {}))
                        checks += 1
                        if allocation == 1:
                            good = (result.returncode, result.stdout, result.stderr) == (
                                1, f'n{tool}: cannot allocate input buffer\n'.encode(), b'')
                        else:
                            error = b'' if broken else f'n{tool}: out of memory\n'.encode()
                            good = result.returncode == 1 and result.stderr == error and normal.stdout.startswith(result.stdout)
                        if not good:
                            failures.append(f'{tool}/{source.name}/allocation{allocation}/{value}/stderr-broken={broken}: '
                                            f'rc={result.returncode} out={result.stdout[:80]!r} err={result.stderr[:80]!r}')
                # A failure beyond the observed calls must leave successful output unchanged.
                after = run(tool, source, NYX_ALLOC_FAIL=str(count + 1))
                assert (after.returncode, after.stdout, after.stderr) == (0, normal.stdout, b'')
                checks += 1
            print(f'{tool}: {observed} allocations exercised across {len(fixtures)} valid sources', flush=True)
    for failure in failures[:20]:
        print('FAIL', failure)
    print(f'N selfhost allocations: {checks} checks, {len(failures)} failures')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
