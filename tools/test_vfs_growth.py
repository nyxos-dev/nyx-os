#!/usr/bin/env python3
"""Exercise real VFS write bounds, sparse growth and zero writes with sanitizers.

Linux/WSL: python3 tools/test_vfs_growth.py
--source accepts an older vfs.c for baseline regression checks.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define memset_asm memset
@DEFINES@
typedef struct { uint32_t type, dev_type, size; uint8_t* data; uint8_t dirty; } vfs_node_t;
static vfs_node_t node;
static int checks, failures, no_alloc, allocs, frees, live, flushes;
static size_t requested;
static vfs_node_t* handle_to_node(int fd) { return fd == 1 ? &node : NULL; }
static void* kmalloc(size_t n) {
    requested = n; allocs++;
    if (no_alloc || n > 1024 * 1024) return NULL;
    size_t* p = malloc(sizeof(size_t) + n);
    if (!p) abort();
    *p = n; live++; return p + 1;
}
static uint32_t ksize(void* p) { return p ? (uint32_t)((size_t*)p)[-1] : 0; }
static void kfree(void* p) { if (p) { live--; frees++; free((size_t*)p - 1); } }
static void flush_mount_node(vfs_node_t* n) { (void)n; flushes++; }
@FUNCTIONS@
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
static void reset(void) {
    kfree(node.data); memset(&node, 0, sizeof(node));
    CHECK(live == 0); no_alloc = allocs = frees = flushes = 0; requested = 0;
}
int main(void) {
    // Intercept allocation so a vulnerable baseline cannot perform its invalid copy.
    no_alloc = 1;
    CHECK(vfs_pwrite(1, "X", 1, 0xfffffffeu) == -1);
    CHECK(allocs == 0 && requested == 0 && node.size == 0);
    CHECK(vfs_write(1, "X", SIZE_MAX) == -1);
    CHECK(allocs == 0 && node.size == 0);
    CHECK(vfs_pwrite(1, NULL, 0, UINT32_MAX) == 0);
    CHECK(allocs == 0 && node.size == 0 && node.dirty == 0);
    if (failures) return 1;
    reset();
    uint32_t offsets[] = {0x7fffffffu, 0x80000000u, 0xfffffdffu, 0xfffffe00u, 0xfffffffeu, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        CHECK(vfs_pwrite(1, "X", 1, offsets[i]) == -1);
        CHECK(allocs == 0 && node.data == NULL && node.size == 0 && node.dirty == 0);
        CHECK(vfs_pwrite(1, NULL, 0, offsets[i]) == 0);
        CHECK(allocs == 0 && node.size == 0 && node.dirty == 0);
    }
    CHECK(vfs_pwrite(1, "X", 0x80000000u, 0) == -1);
    CHECK(vfs_write(1, "X", 0x80000000ull) == -1);
    CHECK(vfs_write(1, "X", UINT64_MAX - 511) == -1);
    CHECK(allocs == 0 && flushes == 0);
    CHECK(vfs_pwrite(0, NULL, 0, 0) == -1);
    node.type = 1; CHECK(vfs_pwrite(1, NULL, 0, 0) == -1); node.type = 0;
    CHECK(vfs_pwrite(1, "KEEP", 4, 0) == 4);
    CHECK(node.size == 4 && memcmp(node.data, "KEEP", 4) == 0 && node.dirty == 1);
    int calls = allocs; node.dirty = 0;
    uint8_t* old = node.data; uint32_t cap = ksize(old);
    CHECK(vfs_pwrite(1, NULL, 0, 8000) == 0);
    CHECK(node.data == old && node.size == 4 && node.dirty == 0 && allocs == calls);
    no_alloc = 1;
    CHECK(vfs_pwrite(1, "X", 1, 8000) == -1);
    CHECK(node.data == old && node.size == 4 && node.dirty == 0 && frees == 0);
    CHECK(memcmp(node.data, "KEEP", 4) == 0 && ksize(node.data) == cap);
    no_alloc = 0;
    CHECK(vfs_pwrite(1, "AB", 2, 20) == 2);
    CHECK(allocs == calls + 1 && node.data == old && node.size == 22);
    for (unsigned i = 4; i < 20; i++) CHECK(node.data[i] == 0);
    CHECK(memcmp(node.data, "KEEP", 4) == 0 && node.data[20] == 'A' && node.data[21] == 'B');
    CHECK(vfs_pwrite(1, "CD", 2, 8000) == 2);
    CHECK(node.size == 8002 && ksize(node.data) >= 8002 && live == 1);
    for (unsigned i = 22; i < 8000; i++) CHECK(node.data[i] == 0);
    CHECK(memcmp(node.data, "KEEP", 4) == 0 && node.data[8000] == 'C' && node.data[8001] == 'D');
    reset();
    char chunk[4096]; memset(chunk, 'a', sizeof(chunk));
    for (unsigned i = 0; i < 64; i++) CHECK(vfs_pwrite(1, chunk, sizeof(chunk), i * sizeof(chunk)) == sizeof(chunk));
    CHECK(node.size == 262144 && allocs < 12 && live == 1);
    for (unsigned i = 0; i < node.size; i += 4096) CHECK(node.data[i] == 'a');
    reset(); node.dev_type = 1;
    CHECK(vfs_pwrite(1, "X", 1, UINT32_MAX) == 1);
    CHECK(vfs_pwrite(1, "X", UINT32_MAX, 0) == -1);
    CHECK(allocs == 0 && node.size == 0 && node.dirty == 0);
    reset();
@CAPACITY_CHECKS@
    printf("VFS growth: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''

CAPACITY = r'''
    uint32_t boundaries[] = {0, 1, 511, 512, 513, 0x3fffffff, 0x40000000,
                            0x7ffffdff, 0x7ffffe00, 0x7ffffffe, 0x7fffffff};
    for (unsigned i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        for (unsigned j = 0; j < sizeof(boundaries)/sizeof(boundaries[0]); j++) {
            uint32_t end = boundaries[i], oldcap = boundaries[j];
            uint64_t expected = (uint64_t)end + 512;
            if (expected < (uint64_t)oldcap * 2) expected = (uint64_t)oldcap * 2;
            if (expected > 0x7fffffff) expected = 0x7fffffff;
            CHECK(vfs_growth_capacity(end, oldcap) == expected);
        }
    }
    CHECK(vfs_growth_capacity(UINT32_MAX, 0) == 0);
    CHECK(vfs_growth_capacity(1, UINT32_MAX) == 0x7fffffffu);
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/fs/vfs.c")
    args = parser.parse_args()
    source = args.source.read_text()
    helper = "static uint32_t vfs_growth_capacity(" in source
    signatures = (["static uint32_t vfs_growth_capacity("] if helper else []) + ["int vfs_write(", "int vfs_pwrite("]
    code = HARNESS.replace("@DEFINES@", "\n".join(re.findall(r"^#define (?:BLOCK_SIZE|VFS_MAX_FILE_SIZE)[^\n]*", source, re.M)))
    code = code.replace("@FUNCTIONS@", "\n".join(function(source, sig) for sig in signatures))
    code = code.replace("@CAPACITY_CHECKS@", CAPACITY if helper else "")
    with tempfile.TemporaryDirectory(prefix="nyx-vfs-growth-") as tmp:
        path = Path(tmp)
        (path / "test.c").write_text(code)
        subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
