#!/usr/bin/env python3
"""Compare desktop/CLI checkpoint validation and exercise desktop load cleanup.

Run on Linux x86_64 or WSL. The desktop source runs with a simulated VFS and
allocator; model fixtures and binaries live in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "user/pkg/nyxgen/model.bin"

HARNESS = r'''
#include "@SOURCE@"
extern void* malloc(size_t size);
extern void free(void* ptr);
extern int open(const char* path, int flags, ...);
extern long read(int fd, void* buf, unsigned long size);
extern int close(int fd);

static uint8_t* file_data;
static uint32_t file_size;
static int no_open, no_data, no_alloc, opens, closes, live_allocs, bad_fd;
void* kmalloc(size_t size) {
    if (no_alloc) return NULL;
    void* p=malloc(size);
    if (p) live_allocs++;
    return p;
}
void kfree(void* p) { if (p) { live_allocs--; free(p); } }
void memset_asm(void* dst, uint8_t value, size_t size) {
    uint8_t* p=(uint8_t*)dst;
    while (size--) *p++=value;
}
int vfs_open(const char* path, int flags, mode_t mode) {
    (void)path; (void)flags; (void)mode;
    opens++;
    return no_open ? -1 : 42;
}
uint8_t* vfs_fdata(int fd) { if (fd!=42) bad_fd=1; return no_data ? NULL : file_data; }
uint32_t vfs_fsize(int fd) { if (fd!=42) bad_fd=1; return file_size; }
int vfs_close(int fd) { if (fd!=42) bad_fd=1; closes++; return 0; }
int main(int argc, char** argv) {
#ifdef MODEL_KAT
    if (argc==2 && !strcmp(argv[1], "--selftest")) {
        int code=oneiros_model_selftest();
        if (code || live_allocs) { printf("KAT %d, allocations %d\n", code, live_allocs); return 2; }
        return 0;
    }
#endif
    if (argc<2) return 2;
    int host_fd=open(argv[1], 0);
    if (host_fd<0) return 2;
    const uint32_t capacity=32+sizeof(Params)+64;
    file_data=(uint8_t*)malloc(capacity);
    if (!file_data) { close(host_fd); return 2; }
    while (file_size<capacity) {
        long got=read(host_fd, file_data+file_size, capacity-file_size);
        if (got<0) { close(host_fd); free(file_data); return 2; }
        if (!got) break;
        file_size+=(uint32_t)got;
    }
    close(host_fd);
    if (argc==3) {
        no_open=!strcmp(argv[2], "--no-open");
        no_data=!strcmp(argv[2], "--no-data");
        no_alloc=!strcmp(argv[2], "--no-alloc");
    }
    Params* model=load_model();
    int accepted=model!=NULL;
    if (model) {
        const uint8_t* copy=(const uint8_t*)model;
        for (size_t i=0;i<sizeof(Params);i++)
            if (copy[i]!=file_data[32+i]) { kfree(model); free(file_data); return 2; }
        /* A successful load must own its copy after the VFS handle closes. */
        file_data[32]^=0xFF;
        if (copy[0]==file_data[32]) { kfree(model); free(file_data); return 2; }
        kfree(model);
    }
    free(file_data);
    if (opens!=1 || closes!=(no_open?0:1) || bad_fd || live_allocs) {
        printf("cleanup: opens=%d closes=%d bad_fd=%d allocations=%d\n", opens, closes, bad_fd, live_allocs);
        return 2;
    }
    return accepted ? 0 : 1;
}
'''


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], capture_output=True,
                          text=True, timeout=30, **kwargs)


def require(command):
    result = run(command)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "kernel/gui/apps/oneiros_win.c",
                        help="desktop source to check, including a previous revision")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("run this test on Linux x86_64 or WSL")
    source = args.source.resolve()
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=gnu99", "-O2", "-ffreestanding", "-Wall", "-Wextra", "-Wshadow", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="nyx-oneiros-model-") as temporary:
        work = Path(temporary)
        harness = work / "desktop.c"
        harness.write_text(HARNESS.replace("@SOURCE@", source.as_posix()), encoding="utf-8")
        has_kat = "int oneiros_model_selftest(void)" in source.read_text()
        desktop, cli = work / "desktop", work / "nyxgen"
        include = ["-I", ROOT / "kernel/gui/apps"]
        require([*cc, *flags, *include, "-ffunction-sections", "-fdata-sections", "-no-pie",
                 "-Wl,--gc-sections", *(["-DMODEL_KAT"] if has_kat else []), harness, "-o", desktop])
        require([*cc, *flags, ROOT / "user/pkg/nyxgen/nyxgen.c", "-o", cli])
        # Keep the actual kernel ABI build covered: no float arguments/returns
        # may accidentally appear in code used by the loader.
        require([*cc, *flags, *include, "-m64", "-mno-red-zone", "-mno-sse", "-mno-sse2",
                 "-mno-mmx", "-mcmodel=large", "-fno-pie", "-c", source, "-o", work / "desktop.o"])
        if has_kat:
            require([desktop, "--selftest"])

        model = MODEL.read_bytes()
        fixtures = [("shipped", model, True),
                    ("extra-payload", model + b"unexpected", False),
                    ("empty", b"", False), ("short-magic", model[:7], False),
                    ("short-header", model[:31], False),
                    ("bad-magic", b"BROKEN!!" + model[8:], False),
                    ("short-payload", model[:-1], False)]
        for index in range(6):
            offset = 8 + index * 4
            fixtures.append((f"dimension-{index}", model[:offset] + struct.pack("<i", -1)
                             + model[offset + 4:], False))
        for label, bits, offset, accepted in (("nan-first", 0x7FC00000, 32, False),
                                    ("infinity-middle", 0x7F800000, 32 + (len(model) - 32) // 2, False),
                                    ("negative-infinity-last", 0xFF800000, len(model) - 4, False),
                                    ("signalling-nan-last", 0x7F800001, len(model) - 4, False),
                                    ("finite-negative-zero", 0x80000000, 32, True),
                                    ("finite-subnormal", 0x00000001, 32, True),
                                    ("finite-max", 0x7F7FFFFF, 32, True)):
            fixtures.append((label, model[:offset] + struct.pack("<I", bits)
                             + model[offset + 4:], accepted))
        for label, content, accepted in fixtures:
            path = work / f"{label}.bin"
            path.write_bytes(content)
            gui_result = run([desktop, path])
            cli_result = run([cli, "0"], env={**os.environ, "ONEIROS_MODEL": str(path)})
            expected = 0 if accepted else 1
            if gui_result.returncode != expected or cli_result.returncode != expected:
                raise RuntimeError(f"{label}: expected {expected}, desktop={gui_result.returncode} "
                                   f"CLI={cli_result.returncode}\n{gui_result.stdout}{gui_result.stderr}"
                                   f"{cli_result.stdout}{cli_result.stderr}")
        for failure in ("--no-open", "--no-data", "--no-alloc"):
            result = run([desktop, MODEL, failure])
            if result.returncode != 1:
                raise RuntimeError(f"{failure}: {result.returncode}\n{result.stdout}{result.stderr}")
        print(f"OK: {len(fixtures)} CLI/desktop model cases; fd/allocation cleanup and copied payload")
        print("OK: desktop host and kernel no-SSE compilation; model KAT when present")


if __name__ == "__main__":
    main()
