#!/usr/bin/env python3
"""Run real Mnemosyne open/save/edit functions against a fault-injecting host VFS.

Linux/WSL: python3 tools/test_editor_io.py
Use --source /path/to/editor_win.c to review an isolated candidate or baseline.
No kernel build, disk image or source modification is needed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "kernel/gui/apps/editor_win.c").is_file())

HARNESS = r'''
#include "@SOURCE@"
@CLIPBOARD@
extern void* malloc(size_t size);
extern void free(void* ptr);
extern int open(const char* path, int flags, ...);
extern long read(int fd, void* buf, unsigned long size);
extern long write(int fd, const void* buf, unsigned long size);
extern int close(int fd);

char g_login_home[64] = "/mnt/home/test";
static char cwd[MAX_PATH] = "/mnt/home/test";
static uint8_t *input_data;
static uint32_t input_size, image_size;
static int output_fd, opens, closes, handles, bad_fd, live_allocs, writes, guard_error;
static int no_open, no_data, no_stat, is_dir, no_alloc, no_resolve, size_changed, size_calls;
static int write_mode, ram_file;
static const char* linked_path;
static char opened_path[MAX_PATH], saved_path[MAX_PATH];

// Check both sides of every editor allocation, including undo and maximum save.
void* kmalloc(size_t size) {
    if (no_alloc) return NULL;
    uint8_t* base = malloc(size + 64);
    if (!base) return NULL;
    *((size_t*)base) = size;
    for (int i = 8; i < 32; i++) base[i] = 0xA7;
    for (int i = 0; i < 32; i++) base[32 + size + i] = 0xB9;
    live_allocs++;
    return base + 32;
}
void kfree(void* ptr) {
    if (!ptr) return;
    uint8_t* base = (uint8_t*)ptr - 32;
    size_t size = *((size_t*)base);
    for (int i = 8; i < 32; i++) if (base[i] != 0xA7) guard_error = 1;
    for (int i = 0; i < 32; i++) if (base[32 + size + i] != 0xB9) guard_error = 1;
    live_allocs--;
    free(base);
}
void memset_asm(void* dst, uint8_t value, size_t size) {
    uint8_t* p = dst; while (size--) *p++ = value;
}
void memcpy_asm(void* dst, const void* src, size_t size) {
    uint8_t* d = dst; const uint8_t* s = src; while (size--) *d++ = *s++;
}
uint32_t get_ticks(void) { return 0; }
#ifndef EDITOR_CLOSE_REGISTRY
void window_request_close(int id) { (void)id; }
#endif
const char* vfs_getcwd(void) { return cwd; }
int vfs_readlink(const char* path, char* target, int cap) {
    if (!linked_path || strcmp(path, linked_path)) return -1;
    if (cap > 0) target[0] = 0;
    return 0;
}
@PATH_FUNCTIONS@
static int64_t backing_resolve(const char* path) { (void)path; return no_resolve == 2 ? -1 : no_resolve ? 0 : 1; }
static int64_t backing_size(const char* path) {
    if (size_changed == 2) return -1;
    (void)path; size_calls++;
    return input_size + (size_changed && size_calls > 1 ? 1 : 0);
}
static mount_entry_t mount;
mount_entry_t* vfs_find_mount(const char* path) {
    if (!ram_file && !strncmp(path, "/mnt", 4) && (!path[4] || path[4] == '/')) return &mount;
    return NULL;
}
int vfs_open(const char* path, int flags, mode_t mode) {
    (void)flags; (void)mode; opens++;
    strcpy(opened_path, path);
    if (no_open) return -1;
    handles++;
    return 42;
}
uint32_t vfs_fsize(int fd) { if (fd != 42 || handles != 1) bad_fd++; return image_size; }
uint8_t* vfs_fdata(int fd) { if (fd != 42 || handles != 1) bad_fd++; return no_data ? NULL : input_data; }
int vfs_fstat(int fd, uint32_t* size, int* dir) {
    if (fd != 42 || handles != 1) bad_fd++;
    if (no_stat) return -1;
    *size = image_size; *dir = is_dir; return 0;
}
int vfs_close(int fd) {
    if (fd != 42 || handles != 1) bad_fd++;
    handles--; closes++; return 0;
}
int vfs_write_file(const char* path, const void* data, uint32_t len) {
    writes++;
    strcpy(saved_path, path);
    if (write_mode == 2) return -1;
    if (write_mode == 1 && len) len--;
    return (int)write(output_fd, data, len);
}
static int same(const void* a, const void* b, size_t size) {
    const uint8_t* x = a; const uint8_t* y = b;
    for (size_t i = 0; i < size; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static int state_preserved(editor_win_t* ed, const editor_win_t* before) {
    char status[64]; memcpy_asm(status, ed->status, sizeof(status));
    memcpy_asm(ed->status, before->status, sizeof(ed->status));
    int ok = same(ed, before, sizeof(*ed));
    memcpy_asm(ed->status, status, sizeof(status));
    return ok;
}
static void seed(editor_win_t* ed) {
    strcpy(ed->lines[0], "previous"); strcpy(ed->lines[1], "keep"); ed->line_count = 2;
    strcpy(ed->filename, "/mnt/original.txt");
    ed->cursor_y = 1; ed->cursor_x = 2; ed->scroll_y = 1; ed->scroll_x = 1;
    ed->selection_x = 1; ed->selection_y = 0; ed->selection_active = 1; ed->selecting = 1;
    ed->modified = 1;
#ifdef LOSSLESS_FIELDS
    ed->crlf = 1;
#endif
    editor_push_undo(ed);
}
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); failed = 1; goto done; } } while (0)

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    int host_fd = open(argv[1], 0);
    if (host_fd < 0) return 2;
    input_data = malloc(1048576);
    if (!input_data) return 2;
    while (input_size < 1048576) {
        long got = read(host_fd, input_data + input_size, 1048576 - input_size);
        if (got < 0) return 2;
        if (!got) break;
        input_size += (uint32_t)got;
    }
    close(host_fd);
    image_size = input_size;
    output_fd = open(argv[2], 1 | 64 | 512, 0600);
    if (output_fd < 0) return 2;
    memset_asm(&mount, 0, sizeof(mount));
    strcpy(mount.mount_point, "/mnt"); mount.resolve = backing_resolve; mount.get_size = backing_size;
    const char* mode = argv[3];
    const char* path = "/mnt/source.txt";
    char pathbuf[MAX_PATH + 16];
    int failed = 0, rejected = 0;
    editor_win_t* ed = editor_create_ctx();
    editor_win_t* before = malloc(sizeof(*before));
    if (!ed || !before) return 2;
    window_t win; memset_asm(&win, 0, sizeof(win)); win.reserved = ed; win.w = 640; win.h = 400;
    seed(ed);
    memcpy_asm(before, ed, sizeof(*ed));
    char undo_copy[2 * EDITOR_LINE_LEN]; memcpy_asm(undo_copy, ed->undo[0].text, sizeof(undo_copy));
    if (!strcmp(mode, "reject")) rejected = 1;
    if (!strcmp(mode, "missing")) { no_open = 1; rejected = 1; }
    if (!strcmp(mode, "directory")) { is_dir = 1; rejected = 1; }
    if (!strcmp(mode, "fstat-error")) { no_stat = 1; rejected = 1; }
    if (!strcmp(mode, "null-data")) { no_data = 1; rejected = 1; }
    if (!strcmp(mode, "prefix")) { image_size--; rejected = 1; }
    if (!strcmp(mode, "read-error")) { image_size = 0; rejected = 1; }
    if (!strcmp(mode, "open-oom")) { image_size = 0; no_data = 1; rejected = 1; }
    if (!strcmp(mode, "metadata-change")) { size_changed = 1; rejected = 1; }
    if (!strcmp(mode, "no-resolve")) { no_resolve = 1; rejected = 1; }
    if (!strcmp(mode, "resolve-error")) { no_resolve = 2; rejected = 1; }
    if (!strcmp(mode, "size-error")) { size_changed = 2; rejected = 1; }
    if (!strcmp(mode, "no-resolve-callback")) { mount.resolve = NULL; rejected = 1; }
    if (!strcmp(mode, "no-size-callback")) { mount.get_size = NULL; rejected = 1; }
    if (!strcmp(mode, "empty-null")) no_data = 1;
    if (!strcmp(mode, "ram") || !strcmp(mode, "ram-oversize")) {
        ram_file = 1; if (!strcmp(mode, "ram-oversize")) rejected = 1;
    }
    if (!strcmp(mode, "path-63") || !strcmp(mode, "path-64")) {
        int size = !strcmp(mode, "path-63") ? 63 : 64;
        memcpy_asm(pathbuf, "/mnt/", 5); for (int i = 5; i < size; i++) pathbuf[i] = 'x';
        pathbuf[size] = 0; path = pathbuf; rejected = size == 64;
    }
    if (!strcmp(mode, "raw-path-limit")) {
        pathbuf[0] = '/'; for (int i = 1; i < MAX_PATH; i++) pathbuf[i] = 'x';
        pathbuf[MAX_PATH] = 0; path = pathbuf; rejected = 1;
    }
    if (!strcmp(mode, "cwd-path-limit")) {
        cwd[0] = '/'; for (int i = 1; i < MAX_PATH - 2; i++) cwd[i] = 'x';
        cwd[MAX_PATH - 2] = 0; path = "../short"; rejected = 1;
    }
    if (!strcmp(mode, "relative")) path = "../test/./source.txt";
    if (!strcmp(mode, "proc")) { path = "/mnt/../proc/meminfo"; rejected = 1; }
    if (!strcmp(mode, "dev")) { path = "/dev/null"; rejected = 1; }
    if (!strcmp(mode, "empty-path")) { path = ""; rejected = 1; }
    if (!strcmp(mode, "symlink-file")) { path = "/tmp/link"; linked_path = path; rejected = 1; }
    if (!strcmp(mode, "symlink-parent")) { path = "/tmp/link/file.txt"; linked_path = "/tmp/link"; rejected = 1; }
    if (!strcmp(mode, "symlink-proc")) { path = "/tmp/proc-alias"; linked_path = path; rejected = 1; }
    if (!strcmp(mode, "symlink-dev")) { path = "/tmp/dev-alias"; linked_path = path; rejected = 1; }

    editor_open(ed, path);
    printf("open_status=%s\nrows=%d\n", ed->status, ed->line_count);
    if (rejected) {
        CHECK(strncmp(ed->status, "Opened:", 7) != 0);
        CHECK(state_preserved(ed, before));
        CHECK(same(undo_copy, ed->undo[0].text, sizeof(undo_copy)));
        CHECK(writes == 0);
        // Saving after a failed load must still target the previous document,
        // even after a normal edit. A rejection cannot merely disable one save.
        editor_win_key(&win, 0x1B); editor_win_key(&win, '!');
        editor_save(ed);
        CHECK(!strcmp(saved_path, "/mnt/original.txt"));
        CHECK(!strncmp(ed->status, "Saved:", 6));
        goto done;
    }
    CHECK(!strncmp(ed->status, "Opened:", 7) || !strncmp(ed->status, "Empty:", 6));
    CHECK(ed->undo_n == 0 && !ed->modified && !ed->selection_active);
    CHECK(ed->cursor_x == 0 && ed->cursor_y == 0 && ed->scroll_x == 0 && ed->scroll_y == 0);
    if (!strcmp(mode, "relative")) {
        CHECK(!strcmp(ed->filename, "/mnt/home/test/source.txt"));
        strcpy(cwd, "/elsewhere");
    }
    if (!strcmp(mode, "edit-undo")) {
        editor_win_key(&win, 0x01);
        CHECK(ed->selection_active && ed->cursor_y == ed->line_count - 1);
        editor_win_key(&win, 'X');
        CHECK(ed->line_count == 1 && !strcmp(ed->lines[0], "X") && ed->undo_n == 1);
        editor_win_key(&win, '\n');
        CHECK(ed->line_count == 2 && ed->undo_n == 2);
        editor_win_key(&win, 0x1A); editor_win_key(&win, 0x1A);
        CHECK(ed->selection_active && ed->undo_n == 0);
    }
    if (!strcmp(mode, "edit-newline")) {
        editor_win_key(&win, 0x01); editor_win_key(&win, 'X');
        editor_win_key(&win, '\n'); editor_win_key(&win, 'Y');
    }
    if (!strcmp(mode, "delete-all")) {
        editor_win_key(&win, 0x01); editor_win_key(&win, KEY_DEL);
        CHECK(ed->line_count == 1 && !ed->lines[0][0]);
    }
    if (!strcmp(mode, "edit-line-limit")) {
        editor_win_key(&win, KEY_END); editor_win_key(&win, 'X');
        CHECK(strstr(ed->status, "Line limit:") != NULL);
        CHECK(strlen(ed->lines[0]) == EDITOR_LINE_LEN - 1);
    }
    if (!strcmp(mode, "edit-row-limit")) {
        editor_win_key(&win, '\n');
        CHECK(strstr(ed->status, "Document limit:") != NULL);
        CHECK(ed->line_count == EDITOR_MAX_LINES);
    }
    if (!strcmp(mode, "undo-oom")) {
        no_alloc = 1; editor_win_key(&win, 'X'); no_alloc = 0;
        CHECK(ed->undo_n == 0 && ed->modified);
    }
    if (!strcmp(mode, "save-oom")) no_alloc = 1;
    if (!strcmp(mode, "short-write")) write_mode = 1;
    if (!strcmp(mode, "write-error")) write_mode = 2;
    if (!strcmp(mode, "symlink-before-save")) linked_path = ed->filename;
    if (!strcmp(mode, "default-path") || !strcmp(mode, "default-long")) {
        ed->filename[0] = 0;
        if (!strcmp(mode, "default-long")) {
            g_login_home[0] = '/'; for (int i = 1; i < 63; i++) g_login_home[i] = 'h';
            g_login_home[63] = 0;
        }
    }
    editor_win_key(&win, 0x13);
#ifdef SAVE_INPUT
    if (!strcmp(mode, "default-path") || !strcmp(mode, "default-long")) {
        CHECK(ed->save_stage == SAVE_INPUT && writes == 0 && !ed->filename[0]);
        const char* name = "chosen.txt";
        while (*name) editor_win_key(&win, *name++);
        editor_win_key(&win, 13);
        if (!strcmp(mode, "default-long")) {
            CHECK(ed->save_stage == SAVE_INPUT && writes == 0);
            goto done;
        }
        CHECK(ed->save_stage == SAVE_CONFIRM && writes == 0);
        editor_win_key(&win, 13);
        CHECK(!strcmp(saved_path, "/mnt/home/test/chosen.txt"));
    }
#endif
    printf("save_status=%s\nmodified=%d\nwrites=%d\n", ed->status, ed->modified, writes);
    if (!strcmp(mode, "save-oom") || !strcmp(mode, "default-long") || !strcmp(mode, "symlink-before-save")) {
        CHECK(writes == 0 && strncmp(ed->status, "Saved:", 6));
        if (!strcmp(mode, "save-oom")) CHECK(!strncmp(ed->status, "Save failed:", 12));
    } else if (write_mode) {
        CHECK(writes == 1 && ed->modified && strncmp(ed->status, "Save failed:", 12) == 0);
    } else {
        CHECK(writes == 1 && !ed->modified && !strncmp(ed->status, "Saved:", 6));
        CHECK(!strcmp(saved_path, ed->filename));
#ifndef SAVE_INPUT
        if (!strcmp(mode, "default-path")) CHECK(!strcmp(saved_path, "/mnt/home/test/untitled.txt"));
#endif
        if (!strcmp(mode, "relative")) CHECK(!strcmp(saved_path, "/mnt/home/test/source.txt"));
    }
done:
    no_alloc = 0;
    editor_win_close(&win); kfree(ed); free(before); free(input_data); close(output_fd);
    if (handles || bad_fd || live_allocs || guard_error || closes != opens - no_open) {
        printf("cleanup handles=%d bad_fd=%d allocs=%d guard=%d opens=%d closes=%d\n",
               handles, bad_fd, live_allocs, guard_error, opens, closes); failed = 1;
    }
    return failed ? 1 : 0;
}
'''


def function_source(source, name):
    """Keep path semantics identical to the real VFS, without linking its devices."""
    marker = f"{name}("
    start = source.index(marker)
    start = source.rfind("\n", 0, start) + 1
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/gui/apps/editor_win.c")
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    evidence = args.evidence or Path(tempfile.mkdtemp(prefix="editor-io-"))
    evidence.mkdir(parents=True, exist_ok=True)
    print(f"Evidence: {evidence}", flush=True)
    vfs = (ROOT / "kernel/fs/vfs.c").read_text()
    paths = function_source(vfs, "join_mount_relative") + "\n" + function_source(vfs, "vfs_realpath")
    core = (ROOT / "kernel/core/kernel.c").read_text()
    clipboard = core[core.index("#define CLIP_MAX"):core.index("// KAT: set/get round-trip")]
    harness = HARNESS.replace("@SOURCE@", source.as_posix()).replace("@PATH_FUNCTIONS@", paths)
    harness = harness.replace("@CLIPBOARD@", clipboard)
    (evidence / "harness.c").write_text(harness)
    command = ["gcc", "-std=gnu99", "-O0", "-ffreestanding", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", "-no-pie", "-Wl,--gc-sections",
               "-I", str(ROOT / "kernel/gui/apps")]
    if "int crlf;" in source.with_suffix(".h").read_text():
        command += ["-DLOSSLESS_FIELDS"]
    command += [str(evidence / "harness.c"), "-o", str(evidence / "editor")]
    built = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (evidence / "build.log").write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError(built.stdout + built.stderr)

    cases = []
    def add(name, data, mode="roundtrip", expected=None):
        if expected is None:
            expected = b"previous\r\nke!ep" if mode in {
                "reject", "missing", "directory", "fstat-error", "null-data", "prefix", "read-error",
                "open-oom", "metadata-change", "no-resolve", "resolve-error", "size-error", "no-resolve-callback", "no-size-callback",
                "path-64", "raw-path-limit", "cwd-path-limit", "proc", "dev", "empty-path", "ram-oversize",
                "symlink-file", "symlink-parent", "symlink-proc", "symlink-dev"
            } else data
        cases.append((name, data, mode, expected))
    for name, data in {
        "empty": b"", "no-final-lf": b"hello", "lf": b"hello\n", "crlf": b"first\r\nsecond\r\n",
        "crlf-no-final": b"first\r\nsecond", "blank-lf": b"\n\n", "blank-crlf": b"\r\n\r\n",
        "bytes": b"tab\tutf8:\xc3\xa9\n", "line-254": b"x" * 254, "line-255": b"x" * 255,
        "max-lf": b"\n".join([b"x" * 255] * 512),
        "max-crlf": b"\r\n".join([b"x" * 255] * 512),
    }.items():
        add(name, data)
    for count in (511, 512, 513):
        for eol in (b"\n", b"\r\n"):
            for final in (False, True):
                data = eol.join([b"line"] * count) + (eol if final else b"")
                add(f"rows-{count}-{'crlf' if len(eol) == 2 else 'lf'}-final-{int(final)}", data,
                    "roundtrip" if count + int(final) <= 512 else "reject")
    for name, data in {
        "line-256": b"x" * 256, "line-256-after-valid": b"good\n" + b"x" * 256,
        "nul": b"before\x00after", "bare-cr": b"hello\rworld", "terminal-cr": b"hello\r",
        "mixed-lf-first": b"one\ntwo\r\n", "mixed-crlf-first": b"one\r\ntwo\n",
        "oversize": b"x" * 131583,
    }.items():
        add(name, data, "reject")
    base = b"first\r\nsecond\r\n"
    for mode in ("missing", "directory", "fstat-error", "null-data", "prefix", "read-error", "open-oom",
                 "metadata-change", "no-resolve", "resolve-error", "size-error", "no-resolve-callback", "no-size-callback", "path-63",
                 "path-64", "raw-path-limit", "cwd-path-limit", "relative", "proc", "dev", "empty-path", "ram",
                 "symlink-file", "symlink-parent", "symlink-proc", "symlink-dev"):
        add(mode, base, mode)
    add("empty-null", b"", "empty-null")
    add("ram-oversize", b"x" * 131583, "ram-oversize")
    for name, data in (("lf", b"a\nb\n"), ("crlf", base), ("no-final", b"a\nb"), ("empty", b"")):
        # Ctrl+A on an empty document is an empty selection, so use nonempty edit/undo cases.
        if data:
            add("edit-undo-" + name, data, "edit-undo")
        add("delete-all-" + name, data, "delete-all", b"")
    add("edit-newline-crlf", base, "edit-newline", b"X\r\nY")
    add("edit-newline-lf", b"a\nb", "edit-newline", b"X\nY")
    add("edit-line-limit", b"x" * 255, "edit-line-limit")
    add("edit-row-limit", b"\n".join([b"x"] * 512), "edit-row-limit")
    add("undo-oom", base, "undo-oom", b"X" + base)
    add("save-oom", base, "save-oom", b"")
    add("short-write", base, "short-write", base[:-1])
    add("write-error", base, "write-error", b"")
    add("default-path", base, "default-path")
    add("default-long", base, "default-long", b"")
    add("symlink-before-save", base, "symlink-before-save", b"")

    results = []
    for name, data, mode, expected in cases:
        input_path, output_path = evidence / f"{name}.input", evidence / f"{name}.output"
        input_path.write_bytes(data)
        process = subprocess.run([str(evidence / "editor"), str(input_path), str(output_path), mode],
                                 capture_output=True, text=True, timeout=10)
        (evidence / f"{name}.log").write_text(process.stdout + process.stderr)
        actual = output_path.read_bytes() if output_path.exists() else None
        ok = process.returncode == 0 and actual == expected
        results.append({"name": name, "passed": ok, "returncode": process.returncode,
                        "input_bytes": len(data), "expected_bytes": len(expected),
                        "actual_bytes": None if actual is None else len(actual)})
        print(f"{'PASS' if ok else 'FAIL'} {name}", flush=True)
        if not ok:
            print(process.stdout + process.stderr, flush=True)
    report = {"source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "method": "Host execution of actual editor functions; VFS/allocator faults simulated; real VFS path normalizer",
              "passed": sum(r["passed"] for r in results), "total": len(results), "results": results}
    (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['passed']}/{report['total']} passed", flush=True)
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
