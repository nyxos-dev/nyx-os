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


def field_fixtures():
    cases = {}
    for name, (source, expected) in fixtures().items():
        if name == 'generic_dispatch':
            source = source.replace('fn dispatch<T>(bs: *Button<T>, counter: *i64) -> T {',
                'struct Ui<T> { buttons: *Button<T> }\nfn dispatch<T>(ui: Ui<T>, counter: *i64) -> T {')
            source = source.replace('dispatch<i64>(bs, counter)', 'dispatch<i64>(Ui<i64>{ buttons: bs }, counter)')
        else:
            element = 'Button<i64>' if name == 'generic_holder' else 'Button'
            source = source.replace(f'fn dispatch(bs: *{element}, counter: *i64) -> i64 {{',
                f'struct Ui {{ buttons: *{element} }}\nfn dispatch(ui: Ui, counter: *i64) -> i64 {{')
            source = source.replace('dispatch(bs, counter)', 'dispatch(Ui{ buttons: bs }, counter)')
        source = source.replace('bs[index(counter)]', 'ui.buttons[index(counter)]')
        source = source.replace('bs[counter[index(counter)] * 0]', 'ui.buttons[counter[index(counter)] * 0]')
        cases['field_' + name] = (source, expected)
    base, expected = cases['field_one_argument']
    cases['field_local_literal'] = (base.replace('fn dispatch(ui: Ui, counter: *i64) -> i64 {',
        'fn dispatch(input: Ui, counter: *i64) -> i64 {\n    ui := Ui{ buttons: input.buttons };'), expected)
    cases['field_local_return'] = (base.replace('fn dispatch(ui: Ui, counter: *i64) -> i64 {',
        'fn copy_ui(input: Ui) -> Ui { input }\nfn dispatch(input: Ui, counter: *i64) -> i64 {\n    ui := copy_ui(input);'), expected)
    cases['field_example'] = ((ROOT / 'lang/examples/field_callbacks.npp').read_text(encoding='utf-8'), expected)
    return cases


def invalid_field_fixtures():
    base = field_fixtures()['field_one_argument'][0]
    return {
        'unknown_pointer_field': base.replace('ui.buttons[', 'ui.missing['),
        'unknown_callback': base.replace('].on_click(7)', '].missing(7)'),
        'wrong_arity': base.replace('].on_click(7)', '].on_click(7, 9)'),
        'scalar_pointer': base.replace('buttons: *Button', 'buttons: *i64').replace('Ui{ buttons: bs }', 'Ui{ buttons: bs as *i64 }'),
        'nonpointer_field': base.replace('buttons: *Button', 'buttons: Button').replace('Ui{ buttons: bs }', 'Ui{ buttons: bs[0] }'),
    }


def chain_fixtures():
    cases = {}
    for name, (source, expected) in field_fixtures().items():
        if name in ('field_local_literal', 'field_local_return', 'field_example'):
            continue
        element = 'Ui<i64>' if name == 'field_generic_dispatch' else 'Ui'
        source = source.replace('ui: Ui', 'ui: *Ui')
        literal = element + '{ buttons: bs }'
        source = source.replace('    result := dispatch', '    ui := sys_sbrk(8) as *' + element + ';\n    ui[0] = ' + literal + ';\n    result := dispatch')
        source = source.replace('(' + literal + ', counter)', '(ui, counter)')
        source = source.replace('ui.buttons[', 'ui[index(counter)].buttons[')
        cases[name.replace('field_', 'chain_', 1)] = source, expected.replace('index calls 1', 'index calls 2')
    base, expected = cases['chain_one_argument']
    cases['chain_local_cast'] = (base.replace('fn dispatch(ui: *Ui, counter: *i64) -> i64 {',
        'fn dispatch(input: *Ui, counter: *i64) -> i64 {\n    ui := input as *Ui;'), expected)
    cases['chain_value_root'] = (base.replace('fn dispatch(ui: *Ui, counter: *i64) -> i64 {',
        'struct Root { panels: *Ui }\nfn dispatch(root: Root, counter: *i64) -> i64 {')
        .replace('ui[index(counter)].buttons[', 'root.panels[index(counter)].buttons[')
        .replace('dispatch(ui, counter)', 'dispatch(Root{ panels: ui }, counter)'), expected)
    cases['chain_three_levels'] = (base.replace('fn dispatch(ui: *Ui, counter: *i64) -> i64 {',
        'struct Root { panels: *Ui }\nfn dispatch(root: *Root, counter: *i64) -> i64 {')
        .replace('ui[index(counter)].buttons[', 'root[index(counter)].panels[index(counter)].buttons[')
        .replace('    result := dispatch(ui, counter);',
                 '    root := sys_sbrk(8) as *Root;\n    root[0] = Root{ panels: ui };\n    result := dispatch(root, counter);'),
        expected.replace('index calls 2', 'index calls 3'))
    cases['chain_example'] = ((ROOT / 'lang/examples/chained_callbacks.npp').read_text(encoding='utf-8'), expected)
    return cases


def invalid_chain_fixtures():
    base = chain_fixtures()['chain_one_argument'][0]
    return {
        'chain_unknown_pointer_field': base.replace('].buttons[', '].missing['),
        'chain_unknown_callback': base.replace('].on_click(7)', '].missing(7)'),
        'chain_wrong_arity': base.replace('].on_click(7)', '].on_click(7, 9)'),
        'chain_scalar_pointer': base.replace('buttons: *Button', 'buttons: *i64').replace('Ui{ buttons: bs }', 'Ui{ buttons: bs as *i64 }'),
        'chain_nonpointer_field': base.replace('buttons: *Button', 'buttons: Button').replace('Ui{ buttons: bs }', 'Ui{ buttons: bs[0] }'),
    }


def require(args):
    p = subprocess.run([str(x) for x in args], capture_output=True, timeout=90)
    if p.returncode:
        raise RuntimeError(f'{args}: exit {p.returncode}\n{p.stderr.decode(errors="replace")}')
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref', help='also reproduce the doubled index with an older nppc')
    parser.add_argument('--compare-fields-ref', help='also check that an older nppc cannot lower direct field receivers')
    parser.add_argument('--compare-chains-ref', help='also check that an older nppc cannot lower chained indexed receivers')
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
        if args.compare_fields_ref:
            old = require(['git', '-C', ROOT, 'show', f'{args.compare_fields_ref}:lang/nppc/nppc.c'])
            (work / 'old-fields.c').write_bytes(old)
            require([*cc, *flags, work / 'old-fields.c', '-o', work / 'old-fields'])
        if args.compare_chains_ref:
            old = require(['git', '-C', ROOT, 'show', f'{args.compare_chains_ref}:lang/nppc/nppc.c'])
            (work / 'old-chains.c').write_bytes(old)
            require([*cc, *flags, work / 'old-chains.c', '-o', work / 'old-chains'])
        cases = fixtures()
        original_count = len(cases)
        original_names = set(cases)
        cases.update(field_fixtures())
        cases.update(chain_fixtures())
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
            if args.compare_ref and name in original_names:
                require([work / 'old', path, '-o', lowered])
                require([work / 'ncc', lowered, '-o', emitted])
                build(emitted, work / 'old-run')
                doubled = expected.replace('index calls 1', 'index calls 2').encode()
                assert require([work / 'old-run']) == doubled, f'{name}: baseline did not reproduce'
            if args.compare_fields_ref and name.startswith('field_'):
                require([work / 'old-fields', path, '-o', lowered])
                result = subprocess.run([work / 'ncc', lowered, '-o', emitted], capture_output=True, timeout=90)
                assert result.returncode == 1, f'{name}: baseline unexpectedly accepts field receiver'
            if args.compare_chains_ref and name.startswith('chain_'):
                require([work / 'old-chains', path, '-o', lowered])
                result = subprocess.run([work / 'ncc', lowered, '-o', emitted], capture_output=True, timeout=90)
                assert result.returncode == 1, f'{name}: baseline unexpectedly accepts chained receiver'
        invalid = {**invalid_field_fixtures(), **invalid_chain_fixtures()}
        for name, source in invalid.items():
            path, lowered, emitted = (work / f'invalid_{name}{ext}' for ext in ('.npp', '.n', '.c'))
            path.write_text(source, encoding='utf-8')
            require([work / 'nppc', path, '-o', lowered])
            for command in ([work / 'ncc', lowered, '-o', emitted], [work / 'ncheck', lowered]):
                result = subprocess.run(command, capture_output=True, timeout=90)
                assert result.returncode == 1, f'{name}: invalid receiver not rejected by {command[0]}'
        print(f'PASS {len(cases)} indexed callback cases: ncheck, identical ncc/ngen C, both executions')
        print(f'PASS {len(invalid)} invalid receiver cases rejected by both N checkers')
        if args.compare_ref:
            print(f'PASS {original_count} baseline reproductions: index evaluated twice')
        if args.compare_fields_ref:
            print(f'PASS {len(field_fixtures())} baseline field receivers cannot compile')

        if args.compare_chains_ref:
            print(f'PASS {len(chain_fixtures())} baseline chained receivers cannot compile')

if __name__ == '__main__':
    main()
