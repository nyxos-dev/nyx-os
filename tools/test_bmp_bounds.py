#!/usr/bin/env python3
"""Exercise the real BMP decoder's signed geometry and failure paths with sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@SOURCE@"
extern void* malloc(size_t);
extern void free(void*);
static int allocations, live, fail, checks;
void* kmalloc(size_t n) {
    allocations++;
    if (fail) return 0;
    void* p = malloc(n);
    if (p) live++;
    return p;
}
void kfree(void* p) { if (p) { live--; free(p); } }
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void store32(uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8*i));
}
static int unchanged(image_t* im) {
    return im->width == 77 && im->height == 88 && !im->pixels;
}
static int geometry(uint32_t width, int32_t height) {
    uint32_t magnitude = height < 0 ? (uint32_t)(-(int64_t)height) : (uint32_t)height;
    uint32_t stride = (width * 3 + 3) & ~3u;
    uint32_t length = 54 + stride * magnitude;
    uint8_t* file = malloc(length);
    CHECK(file);
    __builtin_memset(file, 0, length);
    __builtin_memcpy(file, BMP24, 54);
    store32(file + 18, width); store32(file + 22, (uint32_t)height);
    for (uint32_t y = 0; y < magnitude; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint8_t* px = file + 54 + y * stride + x * 3;
            px[0] = (uint8_t)x; px[1] = (uint8_t)y; px[2] = (uint8_t)(x+y);
        }
    image_t im = {0};
    CHECK(bmp_decode(file, length, &im) == 0);
    CHECK(im.width == width && im.height == magnitude && im.pixels);
    int same = 1;
    for (uint32_t y = 0; y < magnitude; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t source_y = height < 0 ? y : magnitude - y - 1;
            uint8_t* px = im.pixels + ((uint64_t)y * width + x) * 4;
            if (px[0] != (uint8_t)(x+source_y) || px[1] != (uint8_t)source_y ||
                px[2] != (uint8_t)x || px[3] != 255) same = 0;
        }
    CHECK(same);
    kfree(im.pixels); free(file);
    CHECK(!live);
    return 0;
}
int main(void) {
    CHECK(bmp_selftest() == 0 && !live);
    const uint32_t heights[] = {0x80000000u, 0x80000001u, 0x7fffffffu, 0u,
                               4097u, (uint32_t)-4097};
    for (unsigned i = 0; i < sizeof(heights)/sizeof(heights[0]); i++) {
        uint8_t bad[sizeof(BMP24)]; __builtin_memcpy(bad, BMP24, sizeof bad);
        store32(bad + 22, heights[i]);
        image_t im = {.width=77, .height=88, .pixels=0};
        int before = allocations;
        CHECK(bmp_decode(bad, sizeof bad, &im) < 0);
        CHECK(allocations == before && !live && unchanged(&im));
    }
    for (uint32_t length = 0; length < sizeof(BMP24); length++) {
        image_t im = {.width=77, .height=88, .pixels=0};
        int before = allocations;
        CHECK(bmp_decode(BMP24, length, &im) < 0);
        CHECK(allocations == before && !live && unchanged(&im));
    }
    const uint8_t* files[] = {BMP24, BMP32, BMP8, BMP24TD};
    const uint32_t sizes[] = {sizeof(BMP24), sizeof(BMP32), sizeof(BMP8), sizeof(BMP24TD)};
    for (int i = 0; i < 4; i++) {
        image_t im = {.width=77, .height=88, .pixels=0};
        fail = 1;
        CHECK(bmp_decode(files[i], sizes[i], &im) < 0 && unchanged(&im) && !live);
        fail = 0;
        CHECK(bmp_decode(files[i], sizes[i], &im) == 0 && im.pixels);
        kfree(im.pixels); CHECK(!live);
    }
    CHECK(geometry(3, 1) == 0 && geometry(3, -1) == 0);
    CHECK(geometry(1, 4096) == 0 && geometry(1, -4096) == 0);
    CHECK(geometry(4096, 1) == 0 && geometry(4096, -1) == 0);
    CHECK(geometry(1024, 1024) == 0 && geometry(1024, -1024) == 0);
    uint8_t bad[sizeof(BMP24)]; __builtin_memcpy(bad, BMP24, sizeof bad);
    store32(bad + 18, 1024); store32(bad + 22, 1025);
    image_t im = {.width=77, .height=88, .pixels=0};
    int before = allocations;
    CHECK(bmp_decode(bad, sizeof bad, &im) < 0 && allocations == before && unchanged(&im));
    CHECK(!live);
    printf("PASS BMP geometry: %d checks; valid RGBA, signed bounds, truncation and allocation recovery\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'kernel/image/bmp.c')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-bmp-bounds-') as temp:
        work = Path(temp)
        harness = work/'test.c'
        harness.write_text(HARNESS.replace('@SOURCE@',args.source.resolve().as_posix()))
        subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                        '-I',str(ROOT/'kernel/image'),str(harness),'-o',str(work/'test')],check=True)
        result = subprocess.run([str(work/'test')],capture_output=True,text=True,timeout=60)
        print(result.stdout,end='')
        if result.returncode or result.stderr:
            raise RuntimeError(f'BMP regression failed ({result.returncode}):\n{result.stderr}')


if __name__ == '__main__':
    main()
