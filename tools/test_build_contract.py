#!/usr/bin/env python3
"""Check target CPU flags, bounded RTC sampling and ELF stack declarations."""
import os
import re
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check_cpu_baseline():
    # Emulate a distribution whose compiler defaults to v3. Inspect the actual
    # expanded recipes, then ask GCC which ISA features their target flags enable.
    commands = subprocess.check_output(
        ['make', '-C', str(ROOT / 'kernel'), '-Bn', '--no-print-directory',
         'CC=gcc -march=x86-64-v3', 'all'], text=True)
    recipes = [shlex.split(line) for line in commands.splitlines()
               if line.startswith('gcc ') and ' -c ' in line]
    if len(recipes) < 100:
        raise RuntimeError('Expected kernel and user C compilation recipes')
    flag_sets = {tuple(arg for arg in recipe[1:] if arg.startswith('-m'))
                 for recipe in recipes}
    # DOOM can also be built directly, outside make. Exercise its shell-expanded
    # flags in a temporary source tree, with capture tools instead of a linker.
    with tempfile.TemporaryDirectory(prefix='nyx-cpu-contract-') as temporary:
        work = Path(temporary)
        (work / 'user').mkdir()
        (work / 'kernel/doom_src').mkdir(parents=True)
        script = work / 'user/build_doom.sh'
        script.write_bytes((ROOT / 'user/build_doom.sh').read_bytes())
        (work / 'kernel/doom_src/probe.c').touch()
        capture = work / 'capture'
        capture.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CAPTURE_LOG"\n')
        capture.chmod(0o755)
        env = dict(os.environ, CC=str(capture)+' -march=x86-64-v3', LD=str(capture),
                   CAPTURE_LOG=str(work / 'commands'))
        subprocess.run(['bash', str(script)], env=env, check=True, capture_output=True)
        doom_recipes = [shlex.split(line) for line in (work / 'commands').read_text().splitlines()
                        if ' -c ' in line]
        assert len(doom_recipes) == 4
        flag_sets.update(tuple(arg for arg in recipe if arg.startswith('-m'))
                         for recipe in doom_recipes)
    forbidden = {'__AVX__', '__AVX2__', '__BMI__', '__BMI2__', '__FMA__',
                 '__F16C__', '__LZCNT__', '__POPCNT__', '__SSE3__', '__SSSE3__',
                 '__SSE4_1__', '__SSE4_2__'}
    for flags in flag_sets:
        macros = subprocess.check_output(['gcc', *flags, '-dM', '-E', '-x', 'c', '-'], input='', text=True)
        enabled = set(re.findall(r'^#define (\w+)', macros, re.M))
        if enabled & forbidden:
            raise RuntimeError(f'Target recipe requires unsupported CPU features: {sorted(enabled & forbidden)}; {flags}')
    print(f'OK: CPU baseline for {len(recipes)} expanded kernel/user recipes and four standalone DOOM recipes, with simulated v3 compiler default')
HARNESS = r'''
#include "@ROOT@/kernel/core/kernel.h"
static void test_outb(uint16_t port, uint8_t value);
static uint8_t test_inb(uint16_t port);
#define outb test_outb
#define inb test_inb
#define io_wait() ((void)0)
#include "@ROOT@/kernel/drivers/misc/rtc.c"

static uint8_t reg;
static int reads, samples, mode, status_reads;
static void test_outb(uint16_t port, uint8_t value) {
    if (port == CMOS_ADDR) reg = value;
}
static uint8_t test_inb(uint16_t port) {
    (void)port;
    reads++;
    if (reg == RTC_STATUS_B) return 6;
    if (reg == RTC_STATUS_A) {
        status_reads++;
        if (mode == 2) return RTC_UIP;
        return mode == 1 && status_reads == 2 ? RTC_UIP : 0;
    }
    if (reg == RTC_SECONDS) { samples++; return (uint8_t)samples; }
    if (reg == RTC_MINUTES) return 42;
    if (reg == RTC_HOURS) return 13;
    if (reg == RTC_DAY) return 17;
    if (reg == RTC_MONTH) return 10;
    if (reg == RTC_YEAR) return 26;
    return 0;
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    rtc_time_t t;
    CHECK(rtc_selftest() == 0);
    rtc_read_time(NULL);
    CHECK(reads == 0);
    for (mode = 0; mode < 3; mode++) {
        samples = reads = status_reads = 0;
        rtc_read_time(&t);
        int expected = mode + 1;
        CHECK(samples == expected && t.second == expected);
        CHECK(t.minute == 42 && t.hour == 13 && t.day == 17 && t.month == 10 && t.year == 2026);
        CHECK(status_reads == (mode == 2 ? 3000003 : expected * 2));
    }
    printf("OK: RTC null input, decoding, first sample, retry and bounded stuck-UIP fallback\n");
    return 0;
}
'''


def elf(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError(f"{path}: expected little-endian ELF64")
    return data


def check_metadata():
    objects = [ROOT / "kernel" / f"{name}.o" for name in
               ("boot", "isr_stubs", "stubs", "switch", "trampoline")]
    objects += [ROOT / "user" / f"{name}.o" for name in ("crt0", "hello", "spin", "fdleak")]
    for path in objects:
        data = elf(path)
        offset, = struct.unpack_from("<Q", data, 40)
        size, count, names_index = struct.unpack_from("<HHH", data, 58)
        headers = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size) for i in range(count)]
        names_header = headers[names_index]
        names = data[names_header[4]:names_header[4] + names_header[5]]
        notes = [h for h in headers if names[h[0]:].split(b"\0", 1)[0] == b".note.GNU-stack"]
        if len(notes) != 1 or notes[0][1] != 1 or notes[0][2] != 0 or notes[0][5] != 0:
            raise RuntimeError(f"{path}: missing or executable/allocated GNU-stack note")
    programs = list((ROOT / "user").glob("*.elf"))
    if not programs:
        raise RuntimeError("Build userland before running this check")
    for path in programs:
        data = elf(path)
        offset, = struct.unpack_from("<Q", data, 32)
        size, count = struct.unpack_from("<HH", data, 54)
        stacks = [struct.unpack_from("<II", data, offset + i * size) for i in range(count)]
        stacks = [h for h in stacks if h[0] == 0x6474e551]
        if len(stacks) != 1 or stacks[0][1] != 6:
            raise RuntimeError(f"{path}: expected a read/write, non-executable GNU_STACK")
    print(f"OK: {len(objects)} assembly objects and {len(programs)} user ELF stack declarations")


def main():
    check_cpu_baseline()
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nyx-build-contract-") as temporary:
        work = Path(temporary)
        source = work / "rtc.c"
        source.write_text(HARNESS.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        subprocess.run([*cc, "-std=gnu99", "-Os", "-ffreestanding", "-Wall", "-Wextra", "-Wshadow",
                        "-Werror", "-fno-pie", "-no-pie", str(source), "-o", str(work / "rtc")], check=True)
        subprocess.run([str(work / "rtc")], check=True, timeout=30)
    check_metadata()


if __name__ == "__main__":
    main()
