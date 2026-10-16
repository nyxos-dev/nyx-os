#!/usr/bin/env python3
"""Host regressions for memory-map accounting, Monitor units and free options.

Run on Linux x86_64 (or WSL). Only the boot-time map parser and presentation
helpers execute: no privileged instructions, live VM memory or disk images.
"""
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

FREE_SHIM = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _NYXOS_LIBC_H
#define O_RDONLY 0
static long fake_open(const char* path, int flags, int mode) {
    (void)path; (void)flags; (void)mode;
    return 42;
}
static long fake_read(int fd, void* out, unsigned long size) {
    (void)fd;
    const char* text = getenv("TEST_MEMINFO");
    unsigned long len = strlen(text);
    if (len > size) len = size;
    memcpy(out, text, len);
    return (long)len;
}
static long fake_close(int fd) { (void)fd; return 0; }
#define open fake_open
#define read fake_read
#define close fake_close
#include "@ROOT@/user/free.c"
'''

KERNEL_SHIM = r'''
#include "@ROOT@/kernel/mm/memory.c"
#include "@ROOT@/kernel/mm/paging.c"
#include "@ROOT@/kernel/gui/apps/taskman_win.c"
uint64_t memory_total, memory_used;
void memset_asm(void* dst, uint8_t val, size_t count) {
    uint8_t* p = dst;
    while (count--) *p++ = val;
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
#define MIB (1024ULL * 1024)
#define GIB (1024ULL * MIB)
int main(void) {
    // A 5 GiB machine whose PCI aperture splits RAM at 3/4 GiB. The basic
    // multiboot count is only 3 GiB; its full map also contains the high 2 GiB.
    const mb_mmap_entry_t five_gib[] = {
        {0, 640 * 1024, 1}, {640 * 1024, 384 * 1024, 2},
        {MIB, 3 * GIB - MIB, 1}, {3 * GIB, GIB, 2}, {4 * GIB, 2 * GIB, 1}
    };
    init_memory(3 * GIB, five_gib, 5);
    CHECK(memory_total == 5 * GIB - 384 * 1024);
    CHECK(get_total_pages() == 512 * MIB / PAGE_SIZE);
    CHECK(get_free_pages() == 511 * MIB / PAGE_SIZE);
    uint32_t used, free_kb, total;
    mem_pool_kb(&used, &free_kb, &total);
    CHECK(total == 512 * 1024 && used == 1024 && free_kb == 511 * 1024);
    uint32_t percent, used_mb, total_mb;
    mon_mem(&percent, &used_mb, &total_mb);
    CHECK(percent == 0 && used_mb == 1 && total_mb == 512);
    char size[32];
    mon_size(size, sizeof(size), memory_total);
    CHECK(strcmp(size, "5.0 GiB") == 0);
    mon_size(size, sizeof(size), 4 * GIB);
    CHECK(strcmp(size, "4.0 GiB") == 0);
    mon_size(size, sizeof(size), 512 * MIB);
    CHECK(strcmp(size, "512 MiB") == 0);

    // Do not narrow a physical address before capping it: this high region
    // used to alias low frame indexes after a uint32_t wrap at 16 TiB.
    const mb_mmap_entry_t high[] = {
        {0, 32 * MIB, 1}, {16 * 1024 * GIB + 64 * MIB, 64 * MIB, 1}
    };
    init_memory(32 * MIB, high, 2);
    CHECK(get_total_pages() == 512 * MIB / PAGE_SIZE);
    CHECK(get_free_pages() == 31 * MIB / PAGE_SIZE);
    CHECK(memory_total == 96 * MIB);

    const mb_mmap_entry_t hole[] = {
        {0, 32 * MIB, 1}, {32 * MIB, 32 * MIB, 2}, {64 * MIB, 448 * MIB, 1}
    };
    init_memory(512 * MIB, hole, 3);
    CHECK(get_free_pages() == 479 * MIB / PAGE_SIZE);
    CHECK(memory_total == 480 * MIB);
    CHECK(identity_map_entries(memory_total, get_total_pages()) == 256);
    CHECK(identity_map_entries(5 * GIB, get_total_pages()) == 512);
    CHECK(identity_map_entries(0, 0) == 32);
    CHECK(identity_map_entries(64 * MIB + 1, 0) == 33);
    init_memory(64 * 1024 * GIB, 0, 0);
    CHECK(get_total_pages() == 512 * MIB / PAGE_SIZE);
    CHECK(get_free_pages() == 511 * MIB / PAGE_SIZE);
    printf("OK: memory maps, pool accounting and Monitor GiB formatting\n");
    return 0;
}
'''


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, timeout=30, **kwargs)


def main():
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nyx-memory-") as temporary:
        work = Path(temporary)
        for name, source in (("free", FREE_SHIM), ("kernel", KERNEL_SHIM)):
            path = work / f"{name}.c"
            path.write_text(source.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
            flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-comment", "-ffreestanding"]
            if name == "kernel":
                flags += ["-ffunction-sections", "-fdata-sections", "-no-pie",
                          "-Wl,--gc-sections", "-Wl,--defsym,_kernel_end=0x100000"]
            result = run([*cc, *flags, str(path), "-o", str(work / name)])
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

        result = run([str(work / "kernel")])
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout.strip())

        def check(info, arguments, expected=(), status=0):
            result = run([str(work / "free"), *arguments],
                         env={**os.environ, "TEST_MEMINFO": info})
            if result.returncode != status or any(s not in result.stdout for s in expected):
                raise RuntimeError(f"free {arguments}: rc={result.returncode}, output={result.stdout!r}")

        large = "MemTotal: 5242880 kB\nMemUsed: 3145728 kB\nMemFree: 2097152 kB\n"
        check(large, ["-h"], ("5.0GiB", "3.0GiB", "2.0GiB"))
        check(large, ["-m"], ("Mem: (MiB)", "5120", "3072", "2048"))
        check(large, ["-g"], ("Mem: (GiB)", "           5", "           3", "           2"))
        check(large, [], ("Mem: (KiB)", "5242880", "3145728", "2097152"))
        check("MemFree: 523264 kB\nOther: 1 kB\nMemTotal: 524288 kB\nMemUsed: 1024 kB\n"
              "MemDetected: 5242496 kB\n", ["-h"],
              ("512.0MiB", "1.0MiB", "511.0MiB", "RAM detected: 5.0GiB; managed pool: 512.0MiB"))
        check("MemTotal: 0 kB\nMemUsed: 0 kB\nMemFree: 0 kB\n", ["-h"], ("0KiB",))
        check(large, ["--unknown"], ("unknown option",), status=1)
        check(large, ["--help"], ("Usage: free",))
        for malformed in ("", "MemTotal: 123 kB\n", large.replace("3145728", "-1"),
                          large.replace("2097152", "5242881"), large.replace(" kB", " MB"),
                          large.replace("5242880", "18446744073709551616")):
            check(malformed, ["-h"], status=1)
        print("OK: free units, 5 GiB counters, detected/pool separation and malformed input")


if __name__ == "__main__":
    main()
