#!/usr/bin/env python3
"""Exercise actual editor key handling and shared clipboard with allocation faults."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_editor_io import HARNESS, ROOT, function_source

CASES = r'''
#undef CHECK
static int checks;
extern int fflush(void* stream);
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fflush(NULL); return 1; } } while (0)
static void reset_editor(editor_win_t* ed) {
    no_alloc = fail_at = alloc_calls = 0;
    editor_clear_undo(ed);
    memset_asm(ed, 0, sizeof(*ed));
    ed->line_count = 1;
    strcpy(ed->filename, "/mnt/document.txt");
    clipboard_clear();
}
static void fixture(editor_win_t* ed) {
    reset_editor(ed);
    strcpy(ed->lines[0], "alpha"); strcpy(ed->lines[1], "beta"); strcpy(ed->lines[2], "omega");
    ed->line_count = 3;
    ed->selection_active = 1; ed->selection_x = 2; ed->selection_y = 0;
    ed->cursor_x = 2; ed->cursor_y = 1;
}
static int document_equal(editor_win_t* a, editor_win_t* b) {
    return a->line_count == b->line_count && a->crlf == b->crlf &&
           same(a->lines, b->lines, sizeof(a->lines)) && a->modified == b->modified &&
           a->cursor_x == b->cursor_x && a->cursor_y == b->cursor_y &&
           a->selection_x == b->selection_x && a->selection_y == b->selection_y &&
           a->selection_active == b->selection_active && a->undo_n == b->undo_n &&
           same(a->undo, b->undo, sizeof(a->undo)) && !strcmp(a->filename, b->filename);
}
int main(void) {
    editor_win_t* ed = editor_create_ctx();
    editor_win_t* before = malloc(sizeof(*before));
    CHECK(ed && before);
    window_t win; memset_asm(&win, 0, sizeof(win)); win.reserved = ed; win.w = 640; win.h = 400;
    char clip[CLIPBOARD_MAX];
    fixture(ed); memcpy_asm(before, ed, sizeof(*ed));
    editor_win_key(&win, 3);
    CHECK(clipboard_get(clip, sizeof(clip)) == 6 && same(clip, "pha\nbe", 6));
    CHECK(document_equal(ed, before));
    ed->selection_x = 2; ed->selection_y = 1; ed->cursor_x = 2; ed->cursor_y = 0;
    editor_win_key(&win, 3);
    CHECK(clipboard_get(clip, sizeof(clip)) == 6 && same(clip, "pha\nbe", 6));
    fixture(ed); ed->crlf = 1;
    editor_win_key(&win, 3);
    CHECK(clipboard_get(clip, sizeof(clip)) == 7 && same(clip, "pha\r\nbe", 7));
    editor_win_key(&win, 24);
    CHECK(ed->line_count == 2 && !strcmp(ed->lines[0], "alta") && !strcmp(ed->lines[1], "omega"));
    CHECK(ed->undo_n == 1 && ed->cursor_x == 2 && ed->cursor_y == 0 && !ed->selection_active);
    editor_win_key(&win, 26);
    CHECK(ed->line_count == 3 && !strcmp(ed->lines[0], "alpha") && !strcmp(ed->lines[1], "beta"));
    CHECK(ed->selection_active && ed->selection_x == 2 && ed->cursor_y == 1 && ed->crlf == 1);
    fixture(ed); clipboard_set("X\r\nY", 4); editor_win_key(&win, 22);
    CHECK(ed->line_count == 3 && !strcmp(ed->lines[0], "alX") && !strcmp(ed->lines[1], "Yta"));
    CHECK(!strcmp(ed->lines[2], "omega") && ed->cursor_x == 1 && ed->cursor_y == 1 && ed->crlf == 0);
    CHECK(ed->undo_n == 1 && !ed->selection_active);
    editor_win_key(&win, 26);
    CHECK(!strcmp(ed->lines[0], "alpha") && !strcmp(ed->lines[1], "beta") && ed->selection_active);
    fixture(ed); ed->crlf = 1; clipboard_set("X\nY\r\n", 5); editor_win_key(&win, 22);
    CHECK(ed->line_count == 4 && ed->crlf == 1 && !strcmp(ed->lines[0], "alX"));
    CHECK(!strcmp(ed->lines[1], "Y") && !strcmp(ed->lines[2], "ta") && ed->cursor_x == 0 && ed->cursor_y == 2);
    reset_editor(ed); strcpy(ed->lines[0], "ab"); ed->cursor_x = 1;
    clipboard_set("XY", 2); editor_win_key(&win, 22);
    CHECK(!strcmp(ed->lines[0], "aXYb") && ed->cursor_x == 3 && ed->undo_n == 1);
    editor_win_key(&win, 26); CHECK(!strcmp(ed->lines[0], "ab") && ed->cursor_x == 1);
    fixture(ed); clipboard_clear(); memcpy_asm(before, ed, sizeof(*ed)); editor_win_key(&win, 22);
    CHECK(document_equal(ed, before));
    ed->selection_active = 0; clipboard_set("keep", 4); memcpy_asm(before, ed, sizeof(*ed));
    editor_win_key(&win, 3); editor_win_key(&win, 24);
    CHECK(document_equal(ed, before) && clipboard_get(clip, sizeof(clip)) == 4 && same(clip, "keep", 4));
    const char* invalid[] = {"a\0b", "bare\rcr", "end\r"};
    unsigned lengths[] = {3, 7, 4};
    for (int i = 0; i < 3; i++) {
        fixture(ed); clipboard_set(invalid[i], lengths[i]); memcpy_asm(before, ed, sizeof(*ed));
        editor_win_key(&win, 22); CHECK(document_equal(ed, before));
        CHECK(!strncmp(ed->status, "Paste rejected:", 15));
    }
    for (int failure = 1; failure <= 3; failure++) {
        fixture(ed); editor_push_undo(ed); clipboard_set("new", 3);
        memcpy_asm(before, ed, sizeof(*ed)); alloc_calls = 0; fail_at = failure;
        editor_win_key(&win, 22); CHECK(document_equal(ed, before));
        CHECK(live_allocs == 2 && !guard_error);
    }
    for (int failure = 1; failure <= 2; failure++) {
        fixture(ed); editor_push_undo(ed); clipboard_set("keep", 4);
        memcpy_asm(before, ed, sizeof(*ed)); alloc_calls = 0; fail_at = failure;
        editor_win_key(&win, 24); CHECK(document_equal(ed, before));
        CHECK(clipboard_get(clip, sizeof(clip)) == 4 && same(clip, "keep", 4) && live_allocs == 2);
    }
    fixture(ed); clipboard_set("keep", 4); memcpy_asm(before, ed, sizeof(*ed)); no_alloc = 1;
    editor_win_key(&win, 3); CHECK(document_equal(ed, before) && clipboard_len() == 4);
    reset_editor(ed); memset_asm(ed->lines[0], 'a', 254); clipboard_set("b", 1); ed->cursor_x = 254;
    editor_win_key(&win, 22); CHECK(strlen(ed->lines[0]) == 255 && ed->lines[0][254] == 'b');
    memcpy_asm(before, ed, sizeof(*ed)); editor_win_key(&win, 22); CHECK(document_equal(ed, before));
    ed->cursor_x = 0; memcpy_asm(before, ed, sizeof(*ed)); editor_win_key(&win, 22);
    CHECK(document_equal(ed, before));
    reset_editor(ed); ed->line_count = 511; clipboard_set("\n", 1); editor_win_key(&win, 22);
    CHECK(ed->line_count == 512 && ed->undo_n == 1);
    memcpy_asm(before, ed, sizeof(*ed)); editor_win_key(&win, 22); CHECK(document_equal(ed, before));
    reset_editor(ed); ed->line_count = 17;
    for (int y = 0; y < 16; y++) memset_asm(ed->lines[y], 'x', 255);
    editor_win_key(&win, 1); editor_win_key(&win, 3);
    CHECK(clipboard_len() == CLIPBOARD_MAX && clipboard_get(clip, sizeof(clip)) == CLIPBOARD_MAX);
    CHECK(clip[4095] == '\n' && ed->undo_n == 0);
    editor_win_key(&win, 24); CHECK(ed->line_count == 1 && !ed->lines[0][0] && ed->undo_n == 1);
    editor_win_key(&win, 22); CHECK(ed->line_count == 17 && strlen(ed->lines[15]) == 255 && !ed->lines[16][0]);
    CHECK(ed->undo_n == 2);
    editor_win_key(&win, 1); ed->lines[16][0] = 'Z'; ed->lines[16][1] = 0; ed->cursor_x = 1;
    clipboard_set("keep", 4); memcpy_asm(before, ed, sizeof(*ed));
    editor_win_key(&win, 3); editor_win_key(&win, 24);
    CHECK(document_equal(ed, before) && clipboard_len() == 4);
    reset_editor(ed); ed->line_count = 2;
    memset_asm(ed->lines[0], 'a', 255); memset_asm(ed->lines[1], 'b', 255);
    ed->selection_active = 1; ed->selection_x = 200; ed->selection_y = 0; ed->cursor_x = 50; ed->cursor_y = 1;
    clipboard_set("keep", 4); memcpy_asm(before, ed, sizeof(*ed)); editor_win_key(&win, 24);
    CHECK(document_equal(ed, before) && clipboard_len() == 4);
    clipboard_set("\n", 1); editor_win_key(&win, 22);
    CHECK(ed->line_count == 2 && strlen(ed->lines[0]) == 200 && strlen(ed->lines[1]) == 205);
    fixture(ed); ed->find_active = 1; clipboard_set("keep", 4); memcpy_asm(before, ed, sizeof(*ed));
    editor_win_key(&win, 3); editor_win_key(&win, 24); editor_win_key(&win, 22);
    CHECK(document_equal(ed, before) && clipboard_len() == 4);
    reset_editor(ed); clipboard_set("x", 1);
    for (int i = 0; i < EDITOR_UNDO_N + 2; i++) editor_win_key(&win, 22);
    CHECK(ed->undo_n == EDITOR_UNDO_N && strlen(ed->lines[0]) == EDITOR_UNDO_N + 2);
    for (int i = 0; i < EDITOR_UNDO_N; i++) editor_win_key(&win, 26);
    CHECK(!strcmp(ed->lines[0], "xx") && ed->undo_n == 0);
    reset_editor(ed); kfree(ed); free(before);
    CHECK(live_allocs == 0 && !guard_error);
    printf("Editor clipboard: %d checks passed\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/gui/apps/editor_win.c")
    args = parser.parse_args()
    vfs = (ROOT / "kernel/fs/vfs.c").read_text()
    paths = function_source(vfs, "join_mount_relative") + "\n" + function_source(vfs, "vfs_realpath")
    core = (ROOT / "kernel/core/kernel.c").read_text()
    clip = core[core.index("#define CLIP_MAX"):core.index("// KAT: set/get round-trip")]
    harness = HARNESS.replace("@SOURCE@", args.source.resolve().as_posix()).replace("@PATH_FUNCTIONS@", paths).replace("@CLIPBOARD@", clip)
    harness = harness.replace("int main(int argc, char** argv)", "int io_fixture_main(int argc, char** argv)")
    harness = harness.replace("void* kmalloc(size_t size) {", "static int fail_at, alloc_calls;\nvoid* kmalloc(size_t size) {\n    if (++alloc_calls == fail_at) return NULL;")
    with tempfile.TemporaryDirectory(prefix="nyx-editor-clip-") as tmp:
        path = Path(tmp)
        (path / "test.c").write_text(harness + CASES)
        subprocess.run(["gcc", "-std=gnu99", "-O0", "-ffreestanding", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-no-pie", "-Wl,--gc-sections",
                        "-fsanitize=address,undefined", "-DLOSSLESS_FIELDS", "-I", str(ROOT / "kernel/gui/apps"),
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
