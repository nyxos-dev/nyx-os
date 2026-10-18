#!/usr/bin/env python3
"""Run actual Selene form checks, allocation failures and rendering mutations."""
from pathlib import Path
import argparse
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MEMORY_HARNESS = runpy.run_path(str(ROOT/'tools/test_selene_memory.py'))['HARNESS']
BASE = MEMORY_HARNESS.split('static int grids_empty(')[0]
BASE += MEMORY_HARNESS[MEMORY_HARNESS.index('#define CHECK'):].split('int main(void) {')[0]
MAIN = r'''
int main(void) {
    int rc = selene_form_selftest();
    int allocations = calls;
    CHECK(rc == EXPECTED && !live);
#if EXPECTED == 0
    for (int i = 1; i <= allocations; i++) {
        calls = 0; fail_at = i;
        rc = selene_form_selftest();
        CHECK((rc == 0 || rc == -1) && !live);
    }
    fail_at = 0;
    CHECK(selene_form_selftest() == 0 && !live);
#endif
    printf("PASS form regression: expected=%d checks=%d allocation_sites=%d\n",EXPECTED,checks,allocations);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'kernel/gui/apps/selene_win.c')
    args = parser.parse_args()
    source = args.source.resolve().read_text()
    variants = [('normal',source,0,None)]
    mutations = [
        ('nbsp',r'{"nbsp",160,"\xff"}',r'{"nbsp",160," "}','HTML entities FAIL'),
        ('bullet',"char b = (char)0xF9;","char b = '-';",'list FAIL'),
        ('quote',"(q % SEL_QUOTE_INDENT == 0) ? (char)0xB3 : ' '","' '",'pre/blockquote FAIL'),
    ]
    for name,old,new,diagnostic in mutations:
        assert source.count(old)==1,(name,'mutation site changed')
        variants.append((name,source.replace(old,new,1),-1,diagnostic))
    # Simulate lost nested-cell content after rendering, without changing the oracle.
    anchor='    kfree(colstk);\n    return 0;'
    assert source.count(anchor)==1
    corrupt='''    for (int i = 0; i < s->num_lines; i++)
        if (strstr(s->lines[i], "|      | C D")) s->lines[i][9] = 'X';
'''
    variants.append(('nested-cell',source.replace(anchor,corrupt+anchor,1),-1,'nested table FAIL'))
    with tempfile.TemporaryDirectory(prefix='nyx-selene-forms-') as temp:
        directory=Path(temp)
        for name,body,expected,diagnostic in variants:
            selected=directory/(name+'-source.c');selected.write_text(body)
            harness=directory/(name+'.c')
            harness.write_text(BASE.replace('@SOURCE@',selected.as_posix())+MAIN)
            subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-no-pie',
                '-Wl,--gc-sections','-DEXPECTED='+str(expected),'-I',str(ROOT/'kernel/gui/apps'),
                str(harness),str(ROOT/'kernel/core/urlcodec.c'),str(ROOT/'kernel/net/http.c'),
                '-o',str(directory/name)],check=True)
            result=subprocess.run([str(directory/name)],capture_output=True,text=True,timeout=60)
            if result.returncode:
                raise RuntimeError(name+' failed:\n'+result.stdout[-5000:]+result.stderr)
            assert not result.stderr,result.stderr
            if diagnostic: assert diagnostic in result.stdout,(name,'missing failure diagnosis')
            else: assert 'self-test 14/14 passed' in result.stdout
            print(name+': '+result.stdout.splitlines()[-1])
    print('PASS normal suite, allocation recovery and four rejected rendering mutations')


if __name__=='__main__':
    main()
