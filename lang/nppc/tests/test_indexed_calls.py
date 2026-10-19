#!/usr/bin/env python3
"""Execute indexed Fn receivers once, through both N compiler implementations."""
import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def fixtures():
    base = (ROOT / 'lang/examples/indexed_callbacks.npp').read_text(encoding='utf-8')
    cases = {'one_argument': (base, 'result 17, index calls 1\n')}
    cases['zero_arguments'] = (base.replace('Fn(i64) -> i64', 'Fn() -> i64')
        .replace('.on_click(7)', '.on_click()')
        .replace('fn(x: i64) -> i64 { base + x }', 'fn() -> i64 { base }'),
        'result 10, index calls 1\n')
    cases['two_arguments'] = (base.replace('Fn(i64) -> i64', 'Fn(i64, i64) -> i64')
        .replace('.on_click(7)', '.on_click(7, 2)')
        .replace('fn(x: i64) -> i64 { base + x }', 'fn(x: i64, y: i64) -> i64 { base + x + y }'),
        'result 19, index calls 1\n')
    cases['nested_index'] = (base.replace('bs[index(counter)]', 'bs[counter[index(counter)] * 0]'),
        'result 17, index calls 1\n')
    cases['void_return'] = (base.replace('Fn(i64) -> i64', 'Fn(i64)')
        .replace('bs[index(counter)].on_click(7)', 'bs[index(counter)].on_click(7);\n    17')
        .replace('fn(x: i64) -> i64 { base + x }',
                 'fn(x: i64) { value := base + x; message := "value {value}\\n"; write(1, message.ptr as *u8, message.len as isize); }'),
        'value 17\nresult 17, index calls 1\n')
    cases['generic_holder'] = (base.replace('struct Button { on_click: Fn(i64) -> i64 }',
        'struct Button<T> { on_click: Fn(T) -> T }').replace('*Button', '*Button<i64>')
        .replace('Button{', 'Button<i64>{'), 'result 17, index calls 1\n')
    cases['generic_dispatch'] = (cases['generic_holder'][0]
        .replace('fn dispatch(bs: *Button<i64>, counter: *i64) -> i64',
                 'fn dispatch<T>(bs: *Button<T>, counter: *i64) -> T')
        .replace('dispatch(bs, counter)', 'dispatch<i64>(bs, counter)'),
        'result 17, index calls 1\n')
    return cases


def require(args):
    p = subprocess.run([str(x) for x in args], capture_output=True, timeout=90)
    if p.returncode:
        raise RuntimeError(f'{args}: exit {p.returncode}\n{p.stderr.decode(errors="replace")}')
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref', help='also reproduce the doubled index with an older nppc')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        parser.error('the runtime shim requires Linux x86_64; use WSL on Windows')
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror']
    with tempfile.TemporaryDirectory(prefix='npp-index-') as tmp:
        work = Path(tmp)
        for tool in ('nppc', 'ncc'):
            require([*cc, *flags, ROOT / f'lang/{tool}/{tool}.c', '-o', work / tool])
        shutil.copyfile(ROOT / 'user/nyxrt.c', work / 'nyxrt.c')
        shutil.copyfile(ROOT / 'lang/ncc/host/nyxrt.h', work / 'nyxrt.h')

        def build(src, binary):
            require([*cc, *flags, '-ffreestanding', src, work / 'nyxrt.c', '-o', binary])

        for tool, name in (('ngen', 'gen'), ('ncheck', 'check')):
            src = work / f'{tool}.c'
            require([work / 'ncc', ROOT / f'lang/selfhost/{name}.n', '-o', src])
            require([*cc, '-std=c99', '-pedantic-errors', '-ffreestanding', '-O2',
                     src, work / 'nyxrt.c', '-o', work / tool])
        if args.compare_ref:
            old = require(['git', '-C', ROOT, 'show', f'{args.compare_ref}:lang/nppc/nppc.c'])
            (work / 'old.c').write_bytes(old)
            require([*cc, *flags, work / 'old.c', '-o', work / 'old'])
        cases = fixtures()
        for name, (source, expected) in cases.items():
            path, lowered, emitted = (work / f'{name}{ext}' for ext in ('.npp', '.n', '.c'))
            path.write_text(source, encoding='utf-8')
            require([work / 'nppc', path, '-o', lowered])
            require([work / 'ncc', lowered, '-o', emitted])
            assert require([work / 'ncheck', lowered]) == b'', name
            other = require([work / 'ngen', lowered])
            assert other == emitted.read_bytes(), f'{name}: ncc/ngen C differs'
            (work / 'other.c').write_bytes(other)
            for src in (emitted, work / 'other.c'):
                build(src, work / 'run')
                assert require([work / 'run']) == expected.encode(), name
            if args.compare_ref:
                require([work / 'old', path, '-o', lowered])
                require([work / 'ncc', lowered, '-o', emitted])
                build(emitted, work / 'old-run')
                doubled = expected.replace('index calls 1', 'index calls 2').encode()
                assert require([work / 'old-run']) == doubled, f'{name}: baseline did not reproduce'
        print(f'PASS {len(cases)} indexed callback cases: ncheck, identical ncc/ngen C, both executions')
        if args.compare_ref:
            print(f'PASS {len(cases)} baseline reproductions: index evaluated twice')


if __name__ == '__main__':
    main()
