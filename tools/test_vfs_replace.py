#!/usr/bin/env python3
"""Fault-inject real RAM replacement and inode-pool functions on the host.

Run on Linux/WSL with Python 3 and GCC. --source accepts an older vfs.c to
check regression sensitivity. Path lookup and mounts are controlled fixtures;
this does not qualify disk durability or concurrent writers.
"""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_NAME 64
#define MAX_PATH 256
#define MAX_INODES 512
#define MAX_CHILDREN 128
#define memset_asm memset
@NODE@
static vfs_node_t nodes[MAX_INODES], *free_nodes[MAX_INODES];
static uint32_t node_count;
static int free_node_count, node_pool_lock;
static uint64_t spin_lock_irqsave(int* lock) { (void)lock; return 0; }
static void spin_unlock_irqrestore(int* lock, uint64_t flags) { (void)lock; (void)flags; }
@POOL@
static int checks, failures, fail_alloc, live, allocs, frees;
static vfs_node_t *found, *parent_result;
static void* kmalloc(size_t n) {
    allocs++;
    if (fail_alloc) return NULL;
    void* p = malloc(n);
    if (!p) abort();
    live++;
    return p;
}
static void kfree(void* p) { if (p) { live--; frees++; free(p); } }
static const char* vfs_abs(const char* p, char* b, int n) { (void)b; (void)n; return p; }
static vfs_node_t* resolve_path(const char* p) { (void)p; return found; }
static vfs_node_t* resolve_parent(const char* p, char* name) {
    (void)p; strcpy(name, "new.txt"); return parent_result;
}
typedef struct { const char* mount_point; int (*write_file)(const char*, const void*, uint32_t); } mount_entry_t;
static mount_entry_t* mounted;
static mount_entry_t* vfs_find_mount(const char* p) { (void)p; return mounted; }
static int mount_calls, mount_rc;
static const char *mount_path;
static const void* mount_buf;
static uint32_t mount_len;
static int disk_write(const char* p, const void* b, uint32_t n) {
    mount_calls++; mount_path = p; mount_buf = b; mount_len = n; return mount_rc;
}
@WRITE@
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
static void reset(void) {
    for (unsigned i = 0; i < node_count; i++) {
        if (nodes[i].data) { kfree(nodes[i].data); nodes[i].data = NULL; }
    }
    CHECK(live == 0);
    memset(nodes, 0, sizeof(nodes));
    node_count = 0; free_node_count = 0;
    found = parent_result = NULL; mounted = NULL;
    fail_alloc = allocs = frees = mount_calls = 0;
}
static vfs_node_t* old_file(void) {
    vfs_node_t* n = alloc_node();
    n->data = kmalloc(4); memcpy(n->data, "KEEP", 4); n->size = 4;
    n->open_refs = 2; found = n; return n;
}
int main(void) {
    reset();
    vfs_node_t* n = old_file();
    uint8_t* prior = n->data;
    fail_alloc = 1;
    CHECK(vfs_write_file("/old", "X", 0x80000000u) == -1);
    CHECK(n->data == prior && n->size == 4 && allocs == 1 && frees == 0);
    CHECK(vfs_write_file("/old", "NEW", 3) == -1);
    CHECK(n->data == prior && n->size == 4);
    CHECK(frees == 0 && live == 1 && n->open_refs == 2);
    // Stop before dereferencing a buffer freed by a vulnerable baseline.
    if (failures) return 1;
    CHECK(memcmp(n->data, "KEEP", 4) == 0);
    CHECK(free_node_count == 0 && node_count == 1);
    fail_alloc = 0;
    CHECK(vfs_write_file("/old", n->data + 1, 3) == 3);
    CHECK(found == n && n->size == 3 && memcmp(n->data, "EEP", 3) == 0);
    CHECK(live == 1 && frees == 1 && n->open_refs == 2 && n->node_id == 0);
    CHECK(vfs_write_file("/old", "longer value", 12) == 12);
    CHECK(n->size == 12 && memcmp(n->data, "longer value", 12) == 0);
    fail_alloc = 1; int before = allocs;
    CHECK(vfs_write_file("/old", NULL, 0) == 0);
    CHECK(n->size == 0 && n->data == NULL && live == 0 && allocs == before);
    CHECK(vfs_write_file("/old", NULL, 0) == 0);
    CHECK(live == 0 && allocs == before);
    reset(); n = old_file(); n->mode = 0444; before = allocs;
    CHECK(vfs_write_file("/old", "NEW", 3) == -1);
    CHECK(vfs_write_file("/old", NULL, 0) == -1);
    CHECK(allocs == before && frees == 0 && n->size == 4 && memcmp(n->data, "KEEP", 4) == 0);
    n->mode = 0644; n->type = 1;
    CHECK(vfs_write_file("/old", "NEW", 3) == -1);
    CHECK(allocs == before && frees == 0 && n->size == 4);
    reset();
    CHECK(vfs_write_file("/missing/new", "X", 1) == -1);
    CHECK(node_count == 0 && live == 0);
    parent_result = alloc_node();
    CHECK(vfs_write_file("/file/new", "X", 1) == -1);
    CHECK(node_count == 1 && live == 0);
    parent_result->type = 1; fail_alloc = 1;
    for (int i = 0; i < MAX_INODES * 2; i++) {
        CHECK(vfs_write_file("/dir/new.txt", "X", 1) == -1);
        CHECK(parent_result->child_count == 0 && live == 0);
    }
    CHECK(node_count == 2 && free_node_count == 1);
    fail_alloc = 0;
    CHECK(vfs_write_file("/dir/new.txt", "OK", 2) == 2);
    CHECK(parent_result->child_count == 1 && free_node_count == 0 && live == 1);
    n = parent_result->children[0];
    CHECK(n->parent == parent_result && n->size == 2 && n->mode == 0644 && n->type == 0);
    CHECK(strcmp(n->name, "new.txt") == 0 && memcmp(n->data, "OK", 2) == 0);
    reset(); parent_result = alloc_node(); parent_result->type = 1;
    fail_alloc = 1;
    CHECK(vfs_write_file("/dir/empty", NULL, 0) == 0);
    CHECK(parent_result->child_count == 1 && allocs == 0 && live == 0);
    CHECK(parent_result->children[0]->size == 0 && parent_result->children[0]->data == NULL);
    reset(); parent_result = alloc_node(); parent_result->type = 1;
    parent_result->child_count = MAX_CHILDREN;
    for (int i = 0; i < MAX_CHILDREN; i++) parent_result->children[i] = parent_result;
    for (int i = 0; i < 10; i++) {
        CHECK(vfs_write_file("/full/new.txt", "NEW", 3) == -1);
        CHECK(vfs_write_file("/full/empty", NULL, 0) == -1);
        CHECK(live == 0 && parent_result->child_count == MAX_CHILDREN && free_node_count == 1);
    }
    CHECK(node_count == 2);
    for (int i = 0; i < MAX_CHILDREN; i++) CHECK(parent_result->children[i] == parent_result);
    reset(); parent_result = alloc_node(); parent_result->type = 1;
    while (alloc_node()) {}
    CHECK(vfs_write_file("/dir/new.txt", "X", 1) == -1);
    CHECK(parent_result->child_count == 0 && allocs == 0 && live == 0);
    reset();
    mount_entry_t mount = {"/mnt", disk_write}; mounted = &mount;
    const char* payload = "disk";
    int results[] = {-1, 0, 2, 4};
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        mount_rc = results[i];
        CHECK(vfs_write_file("/mnt/file", payload, 4) == mount_rc);
        CHECK(strcmp(mount_path, "/file") == 0 && mount_buf == payload && mount_len == 4);
        CHECK(allocs == 0 && node_count == 0);
    }
    CHECK(mount_calls == 4);
    CHECK(vfs_write_file("/mnt/file", payload, 0x80000000u) == -1);
    CHECK(mount_calls == 4);
    CHECK(vfs_write_file("/mnt", payload, 4) == 4);
    CHECK(strcmp(mount_path, "/") == 0);
    reset();
    printf("RAM VFS replacement: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/fs/vfs.c")
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index("typedef struct vfs_node {")
    end = source.index("} vfs_node_t;", start) + len("} vfs_node_t;")
    code = HARNESS.replace("@NODE@", source[start:end])
    code = "\n".join(re.findall(r"^#define VFS_MAX_FILE_SIZE[^\n]*", source, re.M)) + "\n" + code
    code = code.replace("@POOL@", "\n".join(function(source, signature) for signature in (
        "static vfs_node_t* alloc_node(void)", "static void free_node(vfs_node_t* n)",
        "static int vfs_append_child(vfs_node_t* parent, vfs_node_t* child)")))
    code = code.replace("@WRITE@", function(source, "int vfs_write_file("))
    with tempfile.TemporaryDirectory(prefix="nyx-vfs-replace-") as tmp:
        path = Path(tmp)
        (path / "test.c").write_text(code)
        subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
