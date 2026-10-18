#!/usr/bin/env python3
"""Run the real lseek syscall branch with mocked descriptors and size metadata."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#define SYS_LSEEK 1
#define UFD_PIPE_FLAG 0x40000000
#define UFD_SOCK_FLAG 0x20000000
static uint32_t position, size;
static int handle, missing_offset, metadata_calls, checks, failures;
static int ufd_lookup(int fd, int* result) {
    if (fd != 3) return -1;
    *result = handle; return 0;
}
static uint32_t* ufd_offset_of(int fd) { (void)fd; return missing_offset ? NULL : &position; }
static uint32_t vfs_fsize(int fd) { (void)fd; metadata_calls++; return size; }
@HELPERS@
static int64_t dispatch(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (number) {
@CASE@
    default: return -1;
    }
}
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static void reset(uint32_t initial) { position = initial; handle = 1; size = 17; missing_offset = metadata_calls = 0; }
int main(void) {
    reset(7);
    CHECK(dispatch(SYS_LSEEK, 3, 1ULL << 32, 0) == -1);
    CHECK(position == 7);
    reset(7); CHECK(dispatch(SYS_LSEEK, 3, 1, 1ULL << 32) == -1); CHECK(position == 7);
    reset(7); size = UINT32_MAX; CHECK(dispatch(SYS_LSEEK, 3, 0, 2) == UINT32_MAX); CHECK(position == UINT32_MAX);
    uint32_t bases[] = {0, 1, 17, 0x7ffffffeu, 0x7fffffffu, 0xfffffffeu, UINT32_MAX};
    int64_t deltas[] = {INT64_MIN, INT64_MIN + 1, -4294967296LL, -4294967295LL,
        -2147483648LL, -18, -17, -1, 0, 1, 17, 2147483647LL, 2147483648LL,
        4294967294LL, 4294967295LL, 4294967296LL, INT64_MAX - 1, INT64_MAX};
    for (unsigned b = 0; b < sizeof(bases)/sizeof(bases[0]); b++) {
        for (unsigned d = 0; d < sizeof(deltas)/sizeof(deltas[0]); d++) {
            for (int whence = 0; whence < 3; whence++) {
                reset(bases[b]); size = bases[b];
                __int128 base = whence == 0 ? 0 : whence == 1 ? bases[b] : (uint32_t)size;
                __int128 expected = base + deltas[d];
                int valid = expected >= 0 && expected <= UINT32_MAX;
                int64_t result = dispatch(SYS_LSEEK, 3, (uint64_t)deltas[d], whence);
                CHECK(result == (valid ? (int64_t)expected : -1));
                CHECK(position == (valid ? (uint32_t)expected : bases[b]));
                CHECK(metadata_calls == (whence == 2));
            }
        }
    }
    uint64_t bad_whence[] = {3, UINT64_MAX, 0x100000000ULL, 0x100000001ULL, 0x100000002ULL};
    for (unsigned i = 0; i < sizeof(bad_whence)/sizeof(bad_whence[0]); i++) {
        reset(7); CHECK(dispatch(SYS_LSEEK, 3, 0, bad_whence[i]) == -1);
        CHECK(position == 7 && metadata_calls == 0);
    }
    reset(7); CHECK(dispatch(SYS_LSEEK, 99, 0, 0) == -1); CHECK(position == 7);
    reset(7); missing_offset = 1; CHECK(dispatch(SYS_LSEEK, 3, 0, 0) == -1); CHECK(position == 7);
    int nonseek[] = {UFD_PIPE_FLAG | 1, UFD_SOCK_FLAG | 1};
    for (unsigned i = 0; i < 2; i++) {
        reset(7); handle = nonseek[i]; CHECK(dispatch(SYS_LSEEK, 3, 0, 2) == -1);
        CHECK(position == 7 && metadata_calls == 0);
    }
    reset(7); size = 0; CHECK(dispatch(SYS_LSEEK, 3, 0, 2) == 0 && position == 0);
    reset(7); size = UINT32_MAX; CHECK(dispatch(SYS_LSEEK, 3, 3, 0) == 3 && metadata_calls == 0);
    reset(7); size = UINT32_MAX; CHECK(dispatch(SYS_LSEEK, 3, -2, 1) == 5 && metadata_calls == 0);
    @KAT@
    printf("lseek: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'kernel/core/syscall.c')
    args = parser.parse_args()
    source = args.source.read_text()
    branch = source[source.index('        case SYS_LSEEK:'):source.index('        case SYS_GETPPID:')]
    helpers = ''
    kat = ''
    if 'static int64_t syscall_seek_position(' in source:
        helpers = function(source, 'static int64_t syscall_seek_position(') + '\n' + function(source, 'int syscall_seek_selftest(')
        kat = 'CHECK(syscall_seek_selftest() == 0);'
    harness = HARNESS.replace('@HELPERS@', helpers).replace('@CASE@', branch).replace('@KAT@', kat)
    with tempfile.TemporaryDirectory(prefix='nyx-lseek-') as tmp:
        path = Path(tmp)
        (path/'test.c').write_text(harness)
        subprocess.run(['gcc', '-std=gnu99', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie',
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)


if __name__ == '__main__':
    main()
