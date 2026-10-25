#!/usr/bin/env python3
"""Decode the real boot timer's port writes against the 1 kHz mode-2 contract."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
static uint16_t ports[8];
static uint8_t values[8];
static int writes;
static uint32_t timer_frequency, timer_ticks, pit_divisor;
static void outb(uint16_t port, uint8_t value) {
    if (writes < 8) { ports[writes] = port; values[writes] = value; }
    writes++;
}
@DEFINES@
@INIT@
#define CHECK(x) do { if (!(x)) { printf("FAIL: %s\n", #x); return 1; } } while (0)
int main(void) {
    init_timer(1000);
    CHECK(writes == 3);
    CHECK(ports[0] == 0x43 && ports[1] == 0x40 && ports[2] == 0x40);
    CHECK((values[0] >> 6) == 0);          /* channel 0 */
    CHECK(((values[0] >> 4) & 3) == 3);   /* low byte, then high byte */
    CHECK(((values[0] >> 1) & 7) == 2);   /* rate generator, not square wave */
    CHECK((values[0] & 1) == 0);          /* binary count */
    uint32_t reload = values[1] | ((uint32_t)values[2] << 8);
    CHECK(reload == 1193);
    CHECK(1193180u / reload == 1000);
    puts("PIT boot programming: channel/access/mode/count checks passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timer-source', type=Path, default=ROOT/'kernel/core/timer.c')
    args = parser.parse_args()
    source = args.timer_source.read_text(encoding='utf-8')
    defines = '\n'.join(re.findall(r'^#define PIT_.*$', source, re.M))
    harness = HARNESS.replace('@DEFINES@', defines).replace('@INIT@', function(source, 'void init_timer('))
    with tempfile.TemporaryDirectory(prefix='nyx-pit-') as temp:
        path = Path(temp)
        (path/'test.c').write_text(harness, encoding='utf-8')
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)


if __name__ == '__main__':
    main()
