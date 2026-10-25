#!/usr/bin/env python3
"""Compile actual xbm/cc functions with controlled compiler and VFS failures.

The SHA-256 and ELF validation are the real kernel implementations. Run under
Linux/WSL; --source can select an isolated proposed kernel.c for pre-apply checks.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    import re
    match = re.search(r"^(?:static )?[^\n]*\b" + name + r"\([^;{]*\) \{", source, re.M)
    if not match:
        raise RuntimeError(f"Cannot extract function {name}")
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"


SHIM = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define KERNEL_H
#include "@ROOT@/kernel/crypto/sha256.c"
#include "@ROOT@/kernel/proc/elf.h"
typedef struct { char path[192]; uint8_t* data; uint32_t size; int exists, dir, readonly; } Node;
typedef struct { Node* node; uint8_t* data; uint32_t size; int snapshot; } Handle;
static Node nodes[128];
static Handle handles[128];
static int compiler_code, compiler_output, fault, stage_opens, publish_failed, stat_broken;
static uint32_t image_size;
static char last_output[192];
enum { NONE, NO_STAGE, NO_HASH, NO_IMAGE_DATA, IMAGE_CHANGED, STAGE_SHORT, OLD_IMAGE_SHORT, OLD_IMAGE_EMPTY,
       BACKUP_ERROR, MANIFEST_BACKUP_ERROR, SHORT_BINARY, CORRUPT_BINARY, ERROR_MANIFEST,
       SHORT_MANIFEST, CORRUPT_MANIFEST, RECOVERY_ERROR, RESTORE_EMPTY, RESTORE_SHORT };
static Node* node(const char* path) {
    for (int i = 0; i < 128; i++) if (nodes[i].exists && !strcmp(nodes[i].path, path)) return &nodes[i];
    return NULL;
}
typedef struct { const char* mount_point; uint32_t (*resolve)(const char*); uint32_t (*get_size)(const char*); } mount_entry_t;
static Node* mounted_node(const char* sub) { char path[192]; snprintf(path, sizeof(path), "/mnt%s", sub); return node(path); }
static uint32_t mounted_resolve(const char* sub) { return mounted_node(sub) ? 1 : 0; }
static uint32_t mounted_size(const char* sub) { Node* n = mounted_node(sub); return n ? n->size : 0; }
static mount_entry_t test_mount = {"/mnt", mounted_resolve, mounted_size};
static mount_entry_t* vfs_find_mount(const char* path) { return !strncmp(path, "/mnt/", 5) ? &test_mount : NULL; }
static Node* put(const char* path, const void* data, uint32_t size) {
    Node* n = node(path);
    if (!n) for (int i = 0; i < 128; i++) if (!nodes[i].exists) { n = &nodes[i]; break; }
    if (!n) abort();
    uint8_t* copy = malloc(size ? size : 1);
    if (!copy) abort();
    if (size) memcpy(copy, data, size);
    free(n->data); n->data = copy; n->size = size; n->exists = 1;
    snprintf(n->path, sizeof(n->path), "%s", path);
    return n;
}
static int is_stage(const char* path) { return strstr(path, ".xbm-") && strstr(path, ".new"); }
static int vfs_open(const char* path, int flags, int mode) {
    (void)mode;
    if (is_stage(path)) {
        stage_opens++;
        if ((fault == NO_STAGE && stage_opens == 1) || (fault == NO_HASH && stage_opens == 2)) return -1;
    }
    Node* n = node(path);
    if (!n && (flags & 1)) n = put(path, "", 0);
    if (!n) return -1;
    if (fault == IMAGE_CHANGED && is_stage(path) && stage_opens == 2 && n->size) {
        n->data[n->size - 1] ^= 1; fault = NONE;
    }
    for (int fd = 1; fd < 128; fd++) if (!handles[fd].node) {
        Handle* h = &handles[fd]; h->node = n; h->size = n->size;
        h->snapshot = !strncmp(path, "/mnt/", 5);
        if (h->snapshot) { h->data = malloc(n->size ? n->size : 1); if (n->size) memcpy(h->data, n->data, n->size); }
        if ((fault == STAGE_SHORT && is_stage(path)) ||
            ((fault == OLD_IMAGE_SHORT || fault == OLD_IMAGE_EMPTY) && !strcmp(path, "/mnt/bin/hello")) ||
            ((fault == RESTORE_EMPTY || fault == RESTORE_SHORT) && publish_failed && strstr(path, "/mnt/bin/.xbm-") && strstr(path, ".old"))) {
            h->size = fault == OLD_IMAGE_EMPTY || fault == RESTORE_EMPTY ? 0 : fault == STAGE_SHORT ? n->size - 1 : n->size / 2;
            if (!h->size) { free(h->data); h->data = NULL; }
        }
        return fd;
    }
    return -1;
}
static void vfs_close(int fd) { if (handles[fd].snapshot) free(handles[fd].data); memset(&handles[fd], 0, sizeof(handles[fd])); }
static uint8_t* vfs_fdata(int fd) {
    if (fault == NO_IMAGE_DATA && is_stage(handles[fd].node->path)) return NULL;
    return handles[fd].node->dir ? NULL : handles[fd].snapshot ? handles[fd].data : handles[fd].node->data;
}
static uint32_t vfs_fsize(int fd) { return handles[fd].node->dir ? 0 : handles[fd].snapshot ? handles[fd].size : handles[fd].node->size; }
static int vfs_fstat(int fd, uint32_t* size, int* dir) { if (size) *size = vfs_fsize(fd); if (dir) *dir = handles[fd].node->dir; return 0; }
static int vfs_stat(const char* path, uint32_t* size, int* dir) {
    if (stat_broken && !strncmp(path, "/mnt/", 5)) return -1;
    Node* n = node(path); if (!n) return -1;
    if (size) *size = n->size;
    if (dir) *dir = n->dir;
    return 0;
}
static int vfs_read(int fd, void* data, uint32_t size) {
    if (handles[fd].node->dir) return -1;
    if (size > vfs_fsize(fd)) size = vfs_fsize(fd);
    if (size) memcpy(data, vfs_fdata(fd), size);
    return (int)size;
}
static int vfs_unlink(const char* path) {
    Node* n = node(path); if (!n || n->dir) return -1;
    free(n->data); memset(n, 0, sizeof(*n)); return 0;
}
static int vfs_mkdir(const char* path, int mode) { (void)mode; Node* n = node(path); if (!n) n = put(path, "", 0); n->dir = 1; return 0; }
static int vfs_chmod(const char* path, uint16_t mode) { Node* n = node(path); if (!n) return -1; n->readonly = !(mode & 0200); return 0; }
static int vfs_write_file(const char* path, const void* data, uint32_t size) {
    Node* n = node(path); if (n && (n->dir || n->readonly)) return -1;
    if (fault == BACKUP_ERROR && strstr(path, "/mnt/bin/.xbm-") && strstr(path, ".old")) {
        fault = NONE; put(path, data, size / 2); return -1;
    }
    if (fault == MANIFEST_BACKUP_ERROR && strstr(path, "/usr/pkg/hello/.xbm-") && strstr(path, ".old")) {
        fault = NONE; put(path, data, size / 2); return -1;
    }
    if (!strcmp(path, "/mnt/bin/hello")) {
        if (fault == RESTORE_EMPTY || fault == RESTORE_SHORT) { publish_failed = 1; put(path, "", 0); return 0; }
        if (fault == SHORT_BINARY || fault == RECOVERY_ERROR) {
            if (fault != RECOVERY_ERROR) fault = NONE;
            put(path, data, size / 2); return (int)(size / 2);
        }
        if (fault == CORRUPT_BINARY) { fault = NONE; n = put(path, data, size); n->data[0] ^= 1; return (int)size; }
    }
    if (!strcmp(path, "/usr/pkg/hello/sha256")) {
        if (fault == ERROR_MANIFEST || fault == SHORT_MANIFEST) {
            int result = fault == ERROR_MANIFEST ? -1 : (int)(size / 2); fault = NONE;
            put(path, data, size / 2); return result;
        }
        if (fault == CORRUPT_MANIFEST) { fault = NONE; n = put(path, data, size); n->data[0] ^= 1; return (int)size; }
    }
    put(path, data, size); return (int)size;
}
static int kernel_printf(const char* fmt, ...) { (void)fmt; return 0; }
#define printf kernel_printf
#define kmalloc malloc
#define kfree free
#define PROC_ZOMBIE 2
#define PROC_STOPPED 4
#define SIGCONT 18
typedef struct { int state; } process_t;
uint32_t g_foreground_pid;
static process_t simulated_child;
static int child_exists, child_spawn, child_status, child_vanish, child_waits, child_sleeps, child_resumes;
static int spawn_user_path_args_redir(const char* path, char* const* argv, int argc,
                                      const char* out, int append, const char* err, int err_append) {
    (void)path; (void)argv; (void)argc; (void)out; (void)append; (void)err; (void)err_append; return child_spawn;
}
static process_t* find_process(uint32_t pid) { (void)pid; return child_exists ? &simulated_child : NULL; }
static void compositor_redraw_now(void) {}
#define compositor_foreground_tick compositor_redraw_now
static void signal_raise(process_t* proc, int signal) { if (signal != SIGCONT) abort(); child_resumes++; proc->state = 1; }
static void sleep(int milliseconds) { (void)milliseconds; child_sleeps++; simulated_child.state = PROC_ZOMBIE; if (child_vanish) child_exists = 0; }
static int kwait(uint32_t pid) { (void)pid; child_waits++; return child_status; }
static int tcc_self_ready(void) { return 1; }
static int xbm_fetch_url(const char* url, const char* path) { (void)url; (void)path; return -1; }
static int run_foreground_elf(const char* path, char** argv, int argc) {
    (void)path;
    const char* out = NULL;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "-o")) out = argv[++i];
    if (!out) abort();
    snprintf(last_output, sizeof(last_output), "%s", out);
    if (compiler_output) {
        uint8_t* image = calloc(1, image_size);
        if (!image) abort();
        elf64_hdr_t* hdr = (elf64_hdr_t*)image;
        memcpy(hdr->e_ident, "\177ELF\2\1", 6);
        hdr->e_type = ELF_EXEC; hdr->e_machine = EM_X86_64; hdr->e_phnum = 1;
        hdr->e_phoff = sizeof(*hdr); hdr->e_phentsize = sizeof(elf64_phdr_t);
        if (compiler_output == 2) hdr->e_ident[0] = 0;
        if (compiler_output == 3) hdr->e_phoff = image_size;
        put(out, image, compiler_output == 4 ? 0 : image_size); free(image);
    }
    return compiler_code;
}
'''

TESTS = r'''
#undef printf
static const char* current;
static int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", current, __LINE__, #condition); } } while (0)
static int bytes_are(const char* path, const char* bytes, size_t size) { Node* n = node(path); return n && !n->dir && n->size == size && !memcmp(n->data, bytes, size); }
static void reset(const char* name, int installed) {
    current = name;
    for (int fd = 1; fd < 128; fd++) if (handles[fd].node) vfs_close(fd);
    for (int i = 0; i < 128; i++) free(nodes[i].data);
    memset(nodes, 0, sizeof(nodes));
    compiler_code = 0; compiler_output = 1; image_size = 160; fault = NONE; stage_opens = 0; publish_failed = 0; stat_broken = 0;
    test_mount.resolve = mounted_resolve; test_mount.get_size = mounted_size;
    const char* recipe = "source: hello.c\nbin: hello\n";
    put("/usr/pkg/hello/recipe", recipe, (uint32_t)strlen(recipe));
    if (installed) { put("/mnt/bin/hello", "old executable", 14); put("/usr/pkg/hello/sha256", "old manifest", 12); }
}
static void previous(void) {
    CHECK(bytes_are("/mnt/bin/hello", "old executable", 14));
    CHECK(bytes_are("/usr/pkg/hello/sha256", "old manifest", 12));
}
static void no_temps(void) {
    for (int i = 0; i < 128; i++) if (nodes[i].exists) CHECK(!strstr(nodes[i].path, ".xbm-"));
}
static void success(void) {
    CHECK(xbm_install_one("hello", "xbm") == 0);
    Node* binary = node("/mnt/bin/hello"); Node* manifest = node("/usr/pkg/hello/sha256");
    CHECK(binary && manifest);
    if (binary && manifest) {
        CHECK(binary->size == image_size); CHECK(manifest->size == 64);
        sha256_ctx_t ctx; uint8_t digest[32]; char hex[65];
        sha256_init(&ctx); sha256_update(&ctx, binary->data, binary->size); sha256_final(&ctx, digest);
        pkg_hex_encode(digest, 32, hex); CHECK(manifest->size == 64 && !memcmp(manifest->data, hex, 64));
    }
    no_temps();
}
int main(int argc, char** argv) {
    if (argc > 1) {
        reset("temporary name aliases", !strcmp(argv[1], "source"));
        const char* binary = !strcmp(argv[1], "new") ? ".xbm-1.new" : !strcmp(argv[1], "old") ? ".xbm-1.old" : "hello";
        char recipe[128], output[192];
        snprintf(recipe, sizeof(recipe), "source: %s\nbin: %s\n", !strcmp(argv[1], "source") ? ".xbm-1.old" : "hello.c", binary);
        put("/usr/pkg/hello/recipe", recipe, (uint32_t)strlen(recipe));
        if (!strcmp(argv[1], "source")) put("/usr/pkg/hello/.xbm-1.old", "source stub", 11);
        CHECK(xbm_install_one("hello", "xbm") == 0);
        snprintf(output, sizeof(output), "/mnt/bin/%s", binary);
        CHECK(node(output) && node(output)->size == image_size);
        if (!strcmp(argv[1], "source")) CHECK(bytes_are("/usr/pkg/hello/.xbm-1.old", "source stub", 11));
        printf("xbm alias %s: %d checks, %d failures\n", argv[1], checks, failures);
        reset("cleanup", 0); for (int i = 0; i < 128; i++) free(nodes[i].data);
        return failures ? 1 : 0;
    }
#ifdef XBM_TRANSACTIONAL
    reset("preexisting temporary files are never reused or removed", 1);
    put("/mnt/bin/.xbm-1.new", "stale", 5); put("/mnt/bin/.xbm-2.old", "backup", 6);
    put("/usr/pkg/hello/.xbm-3.old", "manifest backup", 15); compiler_output = 0;
    CHECK(xbm_install_one("hello", "xbm") < 0); CHECK(!strcmp(last_output, "/mnt/bin/.xbm-4.new")); previous();
    CHECK(bytes_are("/mnt/bin/.xbm-1.new", "stale", 5)); CHECK(bytes_are("/mnt/bin/.xbm-2.old", "backup", 6));
    CHECK(bytes_are("/usr/pkg/hello/.xbm-3.old", "manifest backup", 15));
#endif
    reset("nonzero compiler without output rejects stale installed ELF", 1);
    compiler_code = 1; compiler_output = 0; CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("nonzero compiler with output still rejected", 1);
    compiler_code = 7; CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("compiler launch failure", 1);
    compiler_code = -9; compiler_output = 0; CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("zero compiler exit but missing output", 1);
    compiler_output = 0; CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    for (int output = 2; output <= 4; output++) {
        reset("invalid, truncated or empty ELF", 1); compiler_output = output;
        CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    }
    for (int error = NO_STAGE; error <= CORRUPT_MANIFEST; error++) {
        char name[64]; snprintf(name, sizeof(name), "controlled VFS failure %d", error); reset(name, 1); fault = error;
        CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    }
    reset("readonly manifest rollback", 1); node("/usr/pkg/hello/sha256")->readonly = 1;
    CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("mounted stat allocation failure cannot mean uninstalled", 1); stat_broken = 1; node("/usr/pkg/hello/sha256")->readonly = 1;
    CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("missing mounted existence callback fails closed", 1); test_mount.resolve = NULL;
    CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("missing mounted size callback fails closed", 1); test_mount.get_size = NULL;
    CHECK(xbm_install_one("hello", "xbm") < 0); previous(); no_temps();
    reset("directory destination rejected", 1); node("/mnt/bin/hello")->dir = 1;
    CHECK(xbm_install_one("hello", "xbm") < 0); CHECK(node("/mnt/bin/hello")->dir); CHECK(bytes_are("/usr/pkg/hello/sha256", "old manifest", 12)); no_temps();
    reset("directory manifest rejected", 1); node("/usr/pkg/hello/sha256")->dir = 1;
    CHECK(xbm_install_one("hello", "xbm") < 0); CHECK(node("/usr/pkg/hello/sha256")->dir); CHECK(bytes_are("/mnt/bin/hello", "old executable", 14)); no_temps();
    reset("new install compiler failure leaves no installed pair", 0); compiler_code = 1;
    CHECK(xbm_install_one("hello", "xbm") < 0); CHECK(!node("/mnt/bin/hello")); CHECK(!node("/usr/pkg/hello/sha256")); no_temps();
    reset("new install manifest failure removes partial files", 0); fault = ERROR_MANIFEST;
    CHECK(xbm_install_one("hello", "xbm") < 0); CHECK(!node("/mnt/bin/hello")); CHECK(!node("/usr/pkg/hello/sha256")); no_temps();
    reset("successful replacement", 1); success();
    reset("successful first install", 0); success();
    reset("full hash of ELF larger than former 256 KiB cap", 1); image_size = 300000; success();
#ifdef XBM_TRANSACTIONAL
    for (int recovery = RECOVERY_ERROR; recovery <= RESTORE_SHORT; recovery++) {
        reset("persistent recovery failure retains original backup bytes", 1); fault = recovery;
        CHECK(xbm_install_one("hello", "xbm") < 0);
        int found_bin = 0, found_manifest = 0;
        for (int i = 0; i < 128; i++) if (nodes[i].exists && strstr(nodes[i].path, ".xbm-") && strstr(nodes[i].path, ".old")) {
            if (bytes_are(nodes[i].path, "old executable", 14)) found_bin++;
            if (bytes_are(nodes[i].path, "old manifest", 12)) found_manifest++;
        }
        CHECK(found_bin == 1); CHECK(found_manifest == 1);
    }
    reset("VFS selftest", 0); CHECK(xbm_install_selftest() == 0);
    reset("cc returns actual child status", 0); compiler_code = 23;
    char* cc_argv[] = {"cc", "/test.c", "-o", "/test", NULL}; CHECK(cc_compile(4, cc_argv) == 23);
    CHECK(cc_compile(1, cc_argv) < 0);
#endif
    current = "foreground child status";
    for (int scenario = 0; scenario < 7; scenario++) {
        child_spawn = scenario == 6 ? -8 : 42; child_exists = scenario != 4;
        simulated_child.state = scenario == 3 ? PROC_STOPPED : (scenario == 2 || scenario == 5 ? 1 : PROC_ZOMBIE);
        child_vanish = scenario == 5; child_status = scenario == 1 ? 23 : 0;
        child_waits = child_sleeps = child_resumes = 0; g_foreground_pid = 0;
        int result = run_foreground_elf_redir("/test", NULL, 0, NULL, 0, NULL, 0);
        CHECK(result == (scenario == 6 ? -8 : (scenario == 4 || scenario == 5 ? -1 : child_status)));
        CHECK(g_foreground_pid == 0); CHECK(child_waits == (scenario < 4 ? 1 : 0));
        CHECK(child_resumes == (scenario == 3 ? 1 : 0));
        CHECK(child_sleeps == (scenario == 2 || scenario == 3 || scenario == 5 ? 1 : 0));
    }
    for (int fd = 1; fd < 128; fd++) CHECK(!handles[fd].node);
    reset("cleanup", 0);
    for (int i = 0; i < 128; i++) free(nodes[i].data);
    printf("xbm install: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/core/kernel.c")
    args = parser.parse_args()
    source = args.source.read_text()
    transactional = "static int cc_compile(" in source
    program = SHIM.replace("@ROOT@", str(ROOT))
    program += function((ROOT / "kernel/proc/elf.c").read_text(), "elf_validate")
    program += function(source, "run_foreground_elf_redir")
    if transactional:
        program += "#define XBM_TRANSACTIONAL 1\n" + function(source, "cc_compile")
    else:
        program += function(source, "cmd_cc")
        program += r'''
static void execute_command(const char* text) {
    char command[32], input[192], output[192];
    if (sscanf(text, "%31s %191s -o %191s", command, input, output) != 3) abort();
    char* argv[] = { command, input, "-o", output, NULL }; cmd_cc(4, argv);
}
'''
    for name in ("pkg_recipe_value", "pkg_valid_name", "pkg_hex_encode", "pkg_sha256_file"):
        if name == "pkg_sha256_file":
            if transactional:
                for helper in ("pkg_path_exists", "pkg_file_image", "pkg_sha256_file_checked"):
                    program += function(source, helper)
            else:
                program += "static uint8_t pkg_hash_buf[262144];\n"
        program += function(source, name)
    if transactional:
        start = source.index("static int xbm_file_matches(")
        end = source.index("// Build + install ONE package", start)
        program += source[start:end]
    program += function(source, "xbm_install_one") + TESTS
    with tempfile.TemporaryDirectory(prefix="nyx-xbm-test-") as work:
        work = Path(work)
        cfile, executable = work / "test.c", work / "test"
        cfile.write_text(program)
        subprocess.run(["gcc", "-std=gnu99", "-O1", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-fno-pie", "-no-pie", str(cfile), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        for alias in ("new", "old", "source"):
            subprocess.run([str(executable), alias], check=True)


if __name__ == "__main__":
    main()
