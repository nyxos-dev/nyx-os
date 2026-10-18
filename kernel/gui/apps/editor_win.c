#include "../core/theme.h"
#include "../../core/kernel.h"
#include "../core/compositor.h"
#include "editor_win.h"
#include "../../drivers/video/font.h"
#include "../../auth/login.h"   // g_login_home: default new saves into the persistent home

static void editor_clear_undo(editor_win_t* ed);   // defined below; a new file drops the undo stack

#define TOOLBAR_H 26
#define STATUS_H 18
#define BTN_W 56
#define BTN_H 20
#define TEXT_LEFT 36
#define TEXT_RIGHT 2

static void editor_clamp_cursor(editor_win_t* ed) {
    if (ed->cursor_y < 0) ed->cursor_y = 0;
    if (ed->cursor_y >= ed->line_count) ed->cursor_y = ed->line_count - 1;
    int len = (int)strlen(ed->lines[ed->cursor_y]);
    if (ed->cursor_x < 0) ed->cursor_x = 0;
    if (ed->cursor_x > len) ed->cursor_x = len;
}

static int editor_rows(const window_t* win) {
    int n = ((int)win->h - TOOLBAR_H - STATUS_H) / FONT_HEIGHT;
    return n > 0 ? n : 1;
}

static int editor_cols(const window_t* win) {
    int n = ((int)win->w - TEXT_LEFT - TEXT_RIGHT) / FONT_WIDTH;
    return n > 0 ? n : 1;
}

static void editor_clear_selection(editor_win_t* ed) {
    ed->selection_active = 0;
    ed->selecting = 0;
}

// Half-open [start,end), in document order, independent of drag direction.
static int editor_selection_bounds(const editor_win_t* ed, int* sx, int* sy, int* ex, int* ey) {
    if (!ed->selection_active) return 0;
    *sx = ed->selection_x; *sy = ed->selection_y;
    *ex = ed->cursor_x; *ey = ed->cursor_y;
    if (*sy > *ey || (*sy == *ey && *sx > *ex)) {
        int x = *sx, y = *sy;
        *sx = *ex; *sy = *ey; *ex = x; *ey = y;
    }
    return *sy != *ey || *sx != *ex;
}

static int editor_delete_selection(editor_win_t* ed) {
    int sx, sy, ex, ey;
    if (!editor_selection_bounds(ed, &sx, &sy, &ex, &ey)) return 0;
    int tail = (int)strlen(ed->lines[ey]) - ex;
    if (sx + tail >= EDITOR_LINE_LEN) {
        snprintf(ed->status, sizeof(ed->status), "Cannot join lines beyond %d columns", EDITOR_LINE_LEN - 1);
        return 0;
    }
    // Forward copy is safe even for a selection within one line (dst <= src).
    for (int i = 0; i <= tail; i++) ed->lines[sy][sx + i] = ed->lines[ey][ex + i];
    int removed = ey - sy;
    for (int y = sy + 1; y < ed->line_count - removed; y++)
        memcpy_asm(ed->lines[y], ed->lines[y + removed], EDITOR_LINE_LEN);
    ed->line_count -= removed;
    ed->cursor_x = sx; ed->cursor_y = sy;
    ed->modified = 1;
    editor_clear_selection(ed);
    return 1;
}

editor_win_t* editor_create_ctx(void) {
    editor_win_t* ed = (editor_win_t*)kmalloc(sizeof(editor_win_t));
    if (!ed) return NULL;
    memset_asm(ed, 0, sizeof(editor_win_t));
    ed->line_count = 1;
    ed->cursor_tick = get_ticks();
    snprintf(ed->status, sizeof(ed->status), "New file");
    return ed;
}

// Every separator creates a row, including an empty final row. 512 rows of
// 255 bytes separated by CRLF are the largest image this model can represent.
#define EDITOR_MAX_BYTES (EDITOR_MAX_LINES * (EDITOR_LINE_LEN - 1) + (EDITOR_MAX_LINES - 1) * 2)

// vfs_realpath uses a fixed MAX_PATH scratch buffer and can truncate. Bound its
// unnormalised input first, then require the complete absolute name to fit the
// document. Absolute names without VFS symlinks keep Save independent of CWD.
static int editor_path(editor_win_t* ed, const char* path, char out[MAX_PATH]) {
    if (!path || !*path) {
        snprintf(ed->status, sizeof(ed->status), "File error: empty path");
        return 0;
    }
    size_t len = strlen(path);
    if (len >= MAX_PATH || (path[0] != '/' && strlen(vfs_getcwd()) + 1 + len >= MAX_PATH)) {
        snprintf(ed->status, sizeof(ed->status), "File error: path too long");
        return 0;
    }
    vfs_realpath(path, out, MAX_PATH);
    if (strlen(out) >= sizeof(ed->filename)) {
        snprintf(ed->status, sizeof(ed->status), "File error: path exceeds %d bytes", (int)sizeof(ed->filename) - 1);
        return 0;
    }
    // VFS resolves relative symlink targets against the current CWD. Its
    // lexical realpath does not resolve them, so reject any linked component
    // on both Open and Save rather than risking a different target later.
    for (int i = 1; ; i++) {
        if (out[i] && out[i] != '/') continue;
        char end = out[i], target[1];
        out[i] = 0;
        int linked = vfs_readlink(out, target, sizeof(target)) >= 0;
        out[i] = end;
        if (linked) {
            snprintf(ed->status, sizeof(ed->status), "File error: symbolic links are unsupported");
            return 0;
        }
        if (!end) break;
    }
    // /proc and /dev expose generated/stream data rather than the regular file
    // image used below. Their fsize/fdata pair cannot prove a complete read.
    if ((!strncmp(out, "/proc", 5) && (!out[5] || out[5] == '/')) ||
        (!strncmp(out, "/dev", 4) && (!out[4] || out[4] == '/'))) {
        snprintf(ed->status, sizeof(ed->status), "File error: regular files only");
        return 0;
    }
    return 1;
}

static void editor_save(editor_win_t* ed) {
    // New files retain the persistent-home default, but a name that cannot be
    // stored intact must never be written using a silently shortened path.
    char defbuf[128], filename[MAX_PATH];
    snprintf(defbuf, sizeof(defbuf), "%s/untitled.txt", g_login_home[0] ? g_login_home : "/home");
    if (!editor_path(ed, ed->filename[0] ? ed->filename : defbuf, filename)) return;
    int bytes = (ed->line_count - 1) * (ed->crlf ? 2 : 1);
    for (int i = 0; i < ed->line_count; i++) bytes += (int)strlen(ed->lines[i]);
    // Even an empty file gets a valid allocation for VFS; all text stays on the
    // heap because a full document is much larger than the kernel task stack.
    char* buf = (char*)kmalloc((size_t)bytes + 1);
    if (!buf) {
        snprintf(ed->status, sizeof(ed->status), "Save failed: out of memory");
        return;
    }
    int pos = 0;
    for (int i = 0; i < ed->line_count; i++) {
        int len = (int)strlen(ed->lines[i]);
        memcpy_asm(buf + pos, ed->lines[i], len);
        pos += len;
        if (i + 1 < ed->line_count) {
            if (ed->crlf) buf[pos++] = '\r';
            buf[pos++] = '\n';
        }
    }
    buf[pos] = '\0';
    // A short or failed write must stay visible. VFS does not offer an atomic
    // replace here, so a physical write failure may already have changed disk.
    int written = vfs_write_file(filename, buf, (uint32_t)pos);
    if (written == pos) {
        ed->modified = 0;
        strcpy(ed->filename, filename);
        snprintf(ed->status, sizeof(ed->status), "Saved: %s (%d bytes)", filename, pos);
    } else {
        ed->modified = 1;     // disk may now contain only a prefix of this buffer
        snprintf(ed->status, sizeof(ed->status), "Save failed: wrote %d of %d bytes", written, pos);
    }
    kfree(buf);
}

static void editor_open(editor_win_t* ed, const char* path) {
    char filename[MAX_PATH];
    if (!editor_path(ed, path, filename)) return;
    mount_entry_t* mount = vfs_find_mount(filename);
    const char* sub = mount ? filename + strlen(mount->mount_point) : NULL;
    uint32_t expected = 0;
    if (mount) {
        if (!*sub) sub = "/";
        if (!mount->resolve || !mount->get_size || !mount->resolve(sub)) {
            snprintf(ed->status, sizeof(ed->status), "Open failed: cannot verify backing file");
            return;
        }
        expected = mount->get_size(sub);
        if (expected > EDITOR_MAX_BYTES) {
            snprintf(ed->status, sizeof(ed->status), "Open failed: document exceeds editor capacity");
            return;
        }
    }
    int fd = vfs_open(filename, 0, 0);
    if (fd < 0) {
        snprintf(ed->status, sizeof(ed->status), "Open failed: file unavailable");
        return;
    }
    uint32_t size = 0;
    int isdir = 0;
    uint8_t* data = vfs_fdata(fd);
    // Mounted open may return a prefix, or size=0 after OOM/read failure.
    // fstat alone sees that prefix too; compare with independent backing size.
    if (vfs_fstat(fd, &size, &isdir) != 0 || isdir || (size && !data) ||
        (mount && (size != expected || !mount->resolve(sub) || size != mount->get_size(sub)))) {
        snprintf(ed->status, sizeof(ed->status), "Open failed: incomplete or invalid file image");
        vfs_close(fd);
        return;
    }
    if (size > EDITOR_MAX_BYTES) {
        snprintf(ed->status, sizeof(ed->status), "Open failed: document exceeds editor capacity");
        vfs_close(fd);
        return;
    }
    // Validate without touching the current text, filename, selection or undo.
    // NUL, bare CR and mixed LF/CRLF cannot round-trip through this text model.
    int line = 0, col = 0, eol = 0;
    const char* error = NULL;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t c = data[i];
        if (!c) { error = "NUL bytes are unsupported"; break; }
        if (c == '\r' || c == '\n') {
            int kind = c == '\r' ? 2 : 1;
            if (kind == 2 && (i + 1 >= size || data[++i] != '\n')) {
                error = "bare CR is unsupported"; break;
            }
            if (eol && eol != kind) { error = "mixed LF/CRLF is unsupported"; break; }
            eol = kind;
            if (++line >= EDITOR_MAX_LINES) { error = "more than 512 rows (final empty row counts)"; break; }
            col = 0;
        } else if (++col >= EDITOR_LINE_LEN) {
            error = "line exceeds 255 bytes"; break;
        }
    }
    if (error) {
        snprintf(ed->status, sizeof(ed->status), "Open failed: %s", error);
        vfs_close(fd);
        return;
    }
    // Commit only the validated image. No allocations or fallible reads remain.
    editor_clear_undo(ed);
    editor_clear_selection(ed);
    ed->line_count = line + 1;
    ed->crlf = eol == 2;
    line = 0; col = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (data[i] == '\r') continue;
        if (data[i] == '\n') { ed->lines[line++][col] = '\0'; col = 0; }
        else ed->lines[line][col++] = (char)data[i];
    }
    ed->lines[line][col] = '\0';
    strcpy(ed->filename, filename);
    vfs_close(fd);
    ed->cursor_x = 0; ed->cursor_y = 0;
    ed->scroll_x = 0; ed->scroll_y = 0;
    ed->find_active = 0; ed->repl_active = 0; ed->goto_active = 0;
    ed->modified = 0;
    snprintf(ed->status, sizeof(ed->status), "Opened: %s (%u bytes)", filename, size);
}

// Public entry point: load `path` into this editor context. Used by the file
// manager to open a file the user picked; wraps the internal loader.
void editor_load_file(editor_win_t* ed, const char* path) {
    if (ed && path && path[0]) editor_open(ed, path);
}

static void editor_insert_char(editor_win_t* ed, char c) {
    int y = ed->cursor_y;
    if (y >= ed->line_count) return;
    int len = strlen(ed->lines[y]);
    if (len < EDITOR_LINE_LEN - 1) {
        for (int i = len; i >= ed->cursor_x; i--)
            ed->lines[y][i + 1] = ed->lines[y][i];
        ed->lines[y][ed->cursor_x] = c;
        if (ed->cursor_x < EDITOR_LINE_LEN - 1) ed->cursor_x++;
        ed->modified = 1;
    } else {
        snprintf(ed->status, sizeof(ed->status), "Line limit: %d columns; press Enter", EDITOR_LINE_LEN - 1);
    }
}

static void editor_newline(editor_win_t* ed) {
    if (ed->line_count >= EDITOR_MAX_LINES) {
        snprintf(ed->status, sizeof(ed->status), "Document limit: %d lines", EDITOR_MAX_LINES);
        return;
    }
    int y = ed->cursor_y;
    for (int i = ed->line_count; i > y + 1; i--)
        strncpy(ed->lines[i], ed->lines[i - 1], EDITOR_LINE_LEN - 1);
    int rest = strlen(ed->lines[y] + ed->cursor_x);
    memcpy_asm(ed->lines[y + 1], ed->lines[y] + ed->cursor_x, rest + 1);
    ed->lines[y][ed->cursor_x] = '\0';
    ed->line_count++;
    ed->cursor_y++;
    ed->cursor_x = 0;
    ed->modified = 1;
}

static void editor_backspace(editor_win_t* ed) {
    if (ed->cursor_x > 0) {
        int len = strlen(ed->lines[ed->cursor_y]);
        for (int i = ed->cursor_x - 1; i < len; i++)
            ed->lines[ed->cursor_y][i] = ed->lines[ed->cursor_y][i + 1];
        ed->cursor_x--;
        ed->modified = 1;
    } else if (ed->cursor_y > 0) {
        int prev_len = strlen(ed->lines[ed->cursor_y - 1]);
        int cur_len = strlen(ed->lines[ed->cursor_y]);
        if (prev_len + cur_len < EDITOR_LINE_LEN) {
            memcpy_asm(ed->lines[ed->cursor_y - 1] + prev_len, ed->lines[ed->cursor_y], cur_len + 1);
            for (int i = ed->cursor_y; i < ed->line_count - 1; i++)
                strncpy(ed->lines[i], ed->lines[i + 1], EDITOR_LINE_LEN - 1);
            ed->line_count--;
            ed->cursor_y--;
            ed->cursor_x = prev_len;
            ed->modified = 1;
        }
    }
}

static void editor_delete(editor_win_t* ed) {
    int len = strlen(ed->lines[ed->cursor_y]);
    if (ed->cursor_x < len) {
        for (int i = ed->cursor_x; i < len; i++)
            ed->lines[ed->cursor_y][i] = ed->lines[ed->cursor_y][i + 1];
        ed->modified = 1;
    } else if (ed->cursor_y < ed->line_count - 1) {
        int next_len = strlen(ed->lines[ed->cursor_y + 1]);
        if (len + next_len < EDITOR_LINE_LEN) {
            memcpy_asm(ed->lines[ed->cursor_y] + len, ed->lines[ed->cursor_y + 1], next_len + 1);
            for (int i = ed->cursor_y + 1; i < ed->line_count - 1; i++)
                strncpy(ed->lines[i], ed->lines[i + 1], EDITOR_LINE_LEN - 1);
            ed->line_count--;
            ed->modified = 1;
        }
    }
}

static void editor_adjust_scroll(editor_win_t* ed, window_t* win) {
    editor_clamp_cursor(ed);
    int max_rows = editor_rows(win);
    int max_cols = editor_cols(win);
    if (ed->cursor_y < ed->scroll_y) ed->scroll_y = ed->cursor_y;
    if (ed->cursor_y >= ed->scroll_y + max_rows) ed->scroll_y = ed->cursor_y - max_rows + 1;
    if (ed->cursor_x < ed->scroll_x) ed->scroll_x = ed->cursor_x;
    if (ed->cursor_x >= ed->scroll_x + max_cols) ed->scroll_x = ed->cursor_x - max_cols + 1;
    if (ed->scroll_y < 0) ed->scroll_y = 0;
    if (ed->scroll_x < 0) ed->scroll_x = 0;
}

// Regression cases for selection edits and the shared viewport geometry.
// The document is heap-backed here too: kernel task stacks are only 4 KiB.
int editor_selection_selftest(void) {
    editor_win_t* ed = (editor_win_t*)kmalloc(sizeof(*ed));
    if (!ed) return 1;
    memset_asm(ed, 0, sizeof(*ed));
    int result = 0;
    window_t win = {0};
    win.w = 120; win.h = 80;
#define ED_CHECK(cond, code) do { if (!(cond)) { result = (code); goto done; } } while (0)
    ed->line_count = 3;
    strcpy(ed->lines[0], "alpha"); strcpy(ed->lines[1], "beta"); strcpy(ed->lines[2], "gamma");
    ed->selection_active = 1; ed->selection_x = 2; ed->selection_y = 0;
    ed->cursor_x = 2; ed->cursor_y = 2;
    ED_CHECK(editor_delete_selection(ed) && ed->line_count == 1 && !strcmp(ed->lines[0], "almma"), 2);
    ED_CHECK(ed->cursor_x == 2 && ed->cursor_y == 0 && !ed->selection_active, 3);

    strcpy(ed->lines[0], "abcdef");
    ed->selection_active = 1; ed->selection_x = 5; ed->selection_y = 0;
    ed->cursor_x = 2;
    ED_CHECK(editor_delete_selection(ed) && !strcmp(ed->lines[0], "abf"), 4);
    ed->selection_active = 1; ed->selection_x = 0;
    ed->cursor_x = 3;
    ED_CHECK(editor_delete_selection(ed) && ed->line_count == 1 && !ed->lines[0][0], 5);

    // Reject an overlong join atomically; never truncate the unselected suffix.
    ed->line_count = 2;
    for (int i = 0; i < 200; i++) { ed->lines[0][i] = 'a'; ed->lines[1][i] = 'b'; }
    ed->lines[0][200] = 0; ed->lines[1][200] = 0;
    ed->selection_active = 1; ed->selection_x = 150; ed->selection_y = 0;
    ed->cursor_x = 50; ed->cursor_y = 1;
    ED_CHECK(!editor_delete_selection(ed) && ed->line_count == 2 &&
             strlen(ed->lines[0]) == 200 && strlen(ed->lines[1]) == 200, 6);
    editor_clear_selection(ed);
    strcpy(ed->lines[1], "x"); ed->cursor_x = 100;
    editor_adjust_scroll(ed, &win);
    ED_CHECK(ed->cursor_x == 1 && ed->scroll_x == 0, 7);
    ed->cursor_y = 0; ed->cursor_x = 30;
    editor_adjust_scroll(ed, &win);
    ED_CHECK(editor_cols(&win) == 10 && ed->scroll_x == 21, 8);
    win.w = 600;
    ed->cursor_x = 0;
    editor_adjust_scroll(ed, &win);
    ED_CHECK(ed->scroll_x == 0 && editor_rows(&win) == 2, 9);
done:
#undef ED_CHECK
    kfree(ed);
    return result;
}

void editor_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    editor_win_t* ed = (editor_win_t*)win->reserved;
    if (!ed) return;
    editor_adjust_scroll(ed, win);   // keep the cursor visible after resizing too

    // Toolbar
    fb_fill_rect(cx, cy, cw, TOOLBAR_H, THEME_ROW_DIV);
    // Open button
    fb_fill_rect(cx + 4, cy + 3, BTN_W, BTN_H, THEME_SELECTION);
    font_draw_string(cx + 4 + (BTN_W - 3*FONT_WIDTH)/2, cy + 3 + (BTN_H - FONT_HEIGHT)/2, "Open", fb_rgb(255,255,255), THEME_SELECTION);
    // Save button
    fb_fill_rect(cx + 64, cy + 3, BTN_W, BTN_H, THEME_SELECTION);
    font_draw_string(cx + 64 + (BTN_W - 4*FONT_WIDTH)/2, cy + 3 + (BTN_H - FONT_HEIGHT)/2, "Save", fb_rgb(255,255,255), THEME_SELECTION);
    // Filename
    char name_buf[56];
    snprintf(name_buf, sizeof(name_buf), "%s%s",
             ed->filename[0] ? ed->filename : "untitled",
             ed->modified ? " *" : "");
    uint32_t name_x = cx + 128;
    uint32_t name_y = cy + (TOOLBAR_H - FONT_HEIGHT) / 2;
    font_draw_string(name_x, name_y, name_buf, fb_rgb(200,200,220), THEME_ROW_DIV);

    // Text area
    int text_area_y = cy + TOOLBAR_H;
    int avail_w = (int)cw - 4;
    int avail_h = (int)ch - TOOLBAR_H - STATUS_H;
    int max_cols = ((int)cw - TEXT_LEFT - TEXT_RIGHT) / (int)FONT_WIDTH;
    int max_rows = avail_h / (int)FONT_HEIGHT;
    if (max_cols < 1) max_cols = 1;
    if (max_rows < 1) max_rows = 1;

    fb_fill_rect(cx, text_area_y, cw, avail_h, fb_rgb(40,42,48));

    for (int r = 0; r < max_rows; r++) {
        int idx = r + ed->scroll_y;
        if (idx >= ed->line_count) break;
        int draw_x = cx + TEXT_LEFT;
        int draw_y = text_area_y + r * (int)FONT_HEIGHT;
        uint32_t line_bg = (idx == ed->cursor_y) ? fb_rgb(55,60,70) : fb_rgb(40,42,48);
        fb_fill_rect(cx + 2, draw_y, (uint32_t)avail_w, FONT_HEIGHT, line_bg);
        // Line number
        char ln[12];
        snprintf(ln, sizeof(ln), "%d", idx + 1);
        font_draw_string(cx + 2, draw_y, ln, fb_rgb(100,120,140), line_bg);
        // Draw only visible cells. Moving an entire string left allowed its
        // hidden prefix to overwrite the gutter (and unsigned x to wrap).
        int sx, sy, ex, ey;
        int selected = editor_selection_bounds(ed, &sx, &sy, &ex, &ey);
        int len = (int)strlen(ed->lines[idx]);
        for (int c = 0; c < max_cols; c++) {
            int col = ed->scroll_x + c;
            if (col > len) break;
            int mark = selected && idx >= sy && idx <= ey &&
                       (idx > sy || col >= sx) && (idx < ey || col < ex);
            if (col == len && !mark) break;
            font_draw_char((uint32_t)(draw_x + c * FONT_WIDTH), (uint32_t)draw_y,
                           col < len ? (unsigned char)ed->lines[idx][col] : ' ',
                           mark ? fb_rgb(255,255,255) : THEME_TEXT,
                           mark ? THEME_SELECTION : line_bg);
        }
    }

    // Cursor (blink)
    if ((get_ticks() - ed->cursor_tick) < 500) {
        int cur_x = cx + TEXT_LEFT + (ed->cursor_x - ed->scroll_x) * (int)FONT_WIDTH;
        int cur_y = text_area_y + (ed->cursor_y - ed->scroll_y) * (int)FONT_HEIGHT;
        if (cur_y >= text_area_y && cur_y < text_area_y + avail_h &&
            cur_x >= cx + TEXT_LEFT && cur_x < cx + TEXT_LEFT + max_cols * FONT_WIDTH)
            fb_fill_rect((uint32_t)cur_x, (uint32_t)cur_y, 2, FONT_HEIGHT, fb_rgb(255,255,255));
    }

    // Status bar: the message on the left, and the cursor position (with a `*` when the buffer
    // has unsaved edits) right-aligned — the Ln/Col readout every real editor shows.
    int status_y = cy + (int)ch - STATUS_H;
    fb_fill_rect(cx, status_y, cw, STATUS_H, THEME_WINDOW_BG);
    font_draw_string(cx + 4, (uint32_t)status_y + 2, ed->status, fb_rgb(180,200,220), THEME_WINDOW_BG);

    char pos[40];
    snprintf(pos, sizeof(pos), "%s Ln %d, Col %d", ed->modified ? "*" : " ",
             ed->cursor_y + 1, ed->cursor_x + 1);
    int pw = (int)strlen(pos) * FONT_WIDTH;
    int px = cx + (int)cw - 6 - pw;
    if (px < cx + 4) px = cx + 4;
    font_draw_string((uint32_t)px, (uint32_t)status_y + 2, pos, fb_rgb(150,185,215), THEME_WINDOW_BG);
}

static void editor_point_cursor(editor_win_t* ed, window_t* win, int mx, int my) {
    int tx = WIN_CLIENT_X(win) + TEXT_LEFT;
    int ty = WIN_CLIENT_Y(win) + TOOLBAR_H;
    int row = my < ty ? 0 : (my - ty) / FONT_HEIGHT;
    int col = mx < tx ? 0 : (mx - tx) / FONT_WIDTH;
    int rows = editor_rows(win), cols = editor_cols(win);
    if (row >= rows) row = rows - 1;
    if (col >= cols) col = cols - 1;
    ed->cursor_y = ed->scroll_y + row;
    ed->cursor_x = ed->scroll_x + col;
    editor_clamp_cursor(ed);
    ed->cursor_tick = get_ticks();
}

void editor_win_click(window_t* win, int mx, int my, int btn) {
    editor_win_t* ed = (editor_win_t*)win->reserved;
    if (!ed || btn != 1) return;
    ed->selecting = 0;
    // Toolbar click
    if (my >= WIN_CLIENT_Y(win) && my < WIN_CLIENT_Y(win) + TOOLBAR_H) {
        int rx = mx - WIN_CLIENT_X(win);
        if (rx >= 4 && rx < 4 + BTN_W) {
            // Open
            if (!ed->filename[0]) {
                // Try common paths
                const char* paths[] = {"/home/test.txt", "/test.txt", "/shell.txt", "/README.txt"};
                int found = 0;
                for (int i = 0; i < 4; i++) {
                    int fd = vfs_open(paths[i], 0, 0);
                    if (fd >= 0) {
                        vfs_close(fd);
                        editor_open(ed, paths[i]);
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    snprintf(ed->status, sizeof(ed->status), "No file found. Type path and press Ctrl+O");
            } else {
                editor_open(ed, ed->filename);
            }
            return;
        }
        if (rx >= 64 && rx < 64 + BTN_W) {
            editor_save(ed);
            return;
        }
    }
    // A fresh text press sets the anchor; held presses extend the selection.
    int text_area_y = WIN_CLIENT_Y(win) + TOOLBAR_H;   /* was missing TITLE_H too */
    int avail_h = (int)win->h - TOOLBAR_H - STATUS_H;
    if (my >= text_area_y && my < text_area_y + avail_h &&
        mx >= WIN_CLIENT_X(win) + TEXT_LEFT && mx < WIN_CLIENT_X(win) + (int)win->w) {
        editor_point_cursor(ed, win, mx, my);
        ed->selection_x = ed->cursor_x; ed->selection_y = ed->cursor_y;
        ed->selection_active = 1;
        ed->selecting = 1;
        ed->status[0] = 0;
    }
}

void editor_win_pressed(window_t* win, int mx, int my, int btn) {
    editor_win_t* ed = (editor_win_t*)win->reserved;
    if (!ed || btn != 1 || !ed->selecting) return;
    int tx = WIN_CLIENT_X(win) + TEXT_LEFT;
    int ty = WIN_CLIENT_Y(win) + TOOLBAR_H;
    if (mx < tx && ed->scroll_x > 0) ed->scroll_x--;
    if (mx >= tx + editor_cols(win) * FONT_WIDTH && ed->scroll_x < EDITOR_LINE_LEN - 1) ed->scroll_x++;
    if (my < ty && ed->scroll_y > 0) ed->scroll_y--;
    if (my >= ty + editor_rows(win) * FONT_HEIGHT && ed->scroll_y < ed->line_count - 1) ed->scroll_y++;
    editor_point_cursor(ed, win, mx, my);
    editor_adjust_scroll(ed, win);
}

void editor_win_mousemove(window_t* win, int mx, int my, int btns) {
    (void)mx; (void)my;
    editor_win_t* ed = (editor_win_t*)win->reserved;
    if (ed && !(btns & 1)) ed->selecting = 0;
}

// First index >= `start` where `pat` occurs in NUL-terminated `s`, or -1.
static int editor_line_find(const char* s, int start, const char* pat) {
    int slen = (int)strlen(s), plen = (int)strlen(pat);
    if (start < 0) start = 0;
    for (int i = start; i + plen <= slen; i++) {
        int k = 0;
        while (k < plen && s[i + k] == pat[k]) k++;
        if (k == plen) return i;
    }
    return -1;
}

// Search `lines[0..count)` for `pat`, starting at (from_y, from_x) and wrapping once to
// the top so every position is visited exactly once. On a match sets *my/*mx and returns
// 1; returns 0 if `pat` is empty or not found anywhere. Pure — pinned by editor_find_selftest.
int editor_find_in(char lines[][EDITOR_LINE_LEN], int count, int from_y, int from_x,
                   const char* pat, int* my, int* mx) {
    if (!pat || !pat[0] || count <= 0) return 0;
    if (from_y < 0 || from_y >= count) from_y = 0;
    for (int i = 0; i <= count; i++) {
        int y = (from_y + i) % count;
        int start = (i == 0) ? from_x : 0;
        int hit = editor_line_find(lines[y], start, pat);
        if (hit >= 0) { if (my) *my = y; if (mx) *mx = hit; return 1; }
        if (i == count) break;                     // completed the wrap back onto from_y
    }
    return 0;
}

// KAT: forward search, next-after-cursor, a later line, not-found, and the wrap-around.
int editor_find_selftest(void) {
    static char L[4][EDITOR_LINE_LEN];
    strcpy(L[0], "the quick brown fox");
    strcpy(L[1], "jumps over the lazy");
    strcpy(L[2], "dog and the cat");
    L[3][0] = '\0';
    int my = 0, mx = 0;
    if (!editor_find_in(L, 4, 0, 0, "the",   &my, &mx) || my != 0 || mx != 0)  return 1;
    if (!editor_find_in(L, 4, 0, 1, "the",   &my, &mx) || my != 1 || mx != 11) return 2;
    if (!editor_find_in(L, 4, 0, 0, "cat",   &my, &mx) || my != 2 || mx != 12) return 3;
    if ( editor_find_in(L, 4, 0, 0, "zebra", &my, &mx))                         return 4;
    if (!editor_find_in(L, 4, 2, 0, "quick", &my, &mx) || my != 0 || mx != 4)  return 5;
    return 0;
}

// Replace every non-overlapping occurrence of `pat` with `with` in NUL-terminated `line`
// (capacity EDITOR_LINE_LEN). Returns the number of replacements made. Builds the result in
// a scratch buffer and never overflows: if the growing result would exceed the buffer it
// stops replacing and copies the remaining input raw. The replacement is not re-scanned, so
// a `with` that contains `pat` can't loop. Pure — pinned by editor_replace_selftest.
static int editor_line_replace(char* line, const char* pat, const char* with) {
    int plen = (int)strlen(pat);
    if (plen == 0) return 0;
    int wlen = (int)strlen(with), llen = (int)strlen(line);
    char out[EDITOR_LINE_LEN];
    int oi = 0, i = 0, n = 0;
    while (i < llen) {
        int k = 0;
        while (k < plen && line[i + k] == pat[k]) k++;
        if (k == plen) {
            if (oi + wlen >= EDITOR_LINE_LEN) break;   // would overflow — stop replacing
            memcpy_asm(out + oi, with, wlen);
            oi += wlen; i += plen; n++;
        } else {
            if (oi + 1 >= EDITOR_LINE_LEN) break;
            out[oi++] = line[i++];
        }
    }
    while (i < llen && oi + 1 < EDITOR_LINE_LEN) out[oi++] = line[i++];  // raw tail if we broke early
    out[oi] = '\0';
    if (n > 0) memcpy_asm(line, out, oi + 1);
    return n;
}

// Replace `pat` with `with` across every line of `lines[0..count)`. Returns the total number
// of replacements. Empty pattern or empty file → 0, no change. Pure — KAT-able off-target.
static int editor_replace_all(char lines[][EDITOR_LINE_LEN], int count,
                              const char* pat, const char* with) {
    if (!pat || !pat[0] || count <= 0) return 0;
    int total = 0;
    for (int y = 0; y < count; y++) total += editor_line_replace(lines[y], pat, with);
    return total;
}

// KAT: multi-line replace, same-length / growing / shrinking replacements, empty pattern,
// no-match, and a replacement that contains the pattern (must not re-scan or loop).
int editor_replace_selftest(void) {
    static char L[4][EDITOR_LINE_LEN];
    strcpy(L[0], "the cat sat on the mat");
    strcpy(L[1], "cat cat cat");
    strcpy(L[2], "no match here");
    L[3][0] = '\0';
    if (editor_replace_all(L, 4, "cat", "dog") != 4)         return 1;   // 1 + 3 across two lines
    if (strcmp(L[0], "the dog sat on the mat"))              return 2;
    if (strcmp(L[1], "dog dog dog"))                         return 3;
    if (strcmp(L[2], "no match here"))                       return 4;   // untouched
    if (editor_replace_all(L, 4, "dog", "puppy") != 4)       return 5;   // grow 3->5
    if (strcmp(L[1], "puppy puppy puppy"))                   return 6;
    if (editor_replace_all(L, 4, "puppy", "x") != 4)         return 7;   // shrink 5->1
    if (strcmp(L[1], "x x x"))                               return 8;
    if (editor_replace_all(L, 4, "", "z") != 0)              return 9;   // empty pattern: no-op
    if (editor_replace_all(L, 4, "zzz", "q") != 0)           return 10;  // not present: no-op
    if (editor_replace_all(L, 4, "x", "xx") != 4)            return 11;  // 1 in L0 + 3 in L1
    if (strcmp(L[1], "xx xx xx"))                            return 12;  // replacement holds pattern, no loop
    return 0;
}

// Ctrl+G goto-line core: parse a decimal (1-based) line number and clamp it to a valid 0-based
// cursor row. Empty or non-numeric -> -1 (no jump). Saturates so a huge entry can't overflow.
int editor_goto_target(const char* buf, int line_count) {
    if (!buf || !buf[0] || line_count < 1) return -1;
    long n = 0;
    for (const char* p = buf; *p; p++) {
        if (*p < '0' || *p > '9') return -1;             // any non-digit cancels the jump
        n = n * 10 + (*p - '0');
        if (n > 1000000000L) n = 1000000000L;            // saturate well below overflow
    }
    if (n < 1) n = 1;                                    // "0" -> first line
    if (n > line_count) n = line_count;                  // past EOF -> last line
    return (int)(n - 1);                                 // 1-based entry -> 0-based row
}

// KAT for the goto-line core: in-range, both clamps, and the empty/non-numeric cancels.
int editor_goto_selftest(void) {
    if (editor_goto_target("50", 100)         != 49) return 1;
    if (editor_goto_target("1", 100)          != 0)  return 2;
    if (editor_goto_target("100", 100)        != 99) return 3;
    if (editor_goto_target("999", 100)        != 99) return 4;   // past EOF -> last line
    if (editor_goto_target("0", 100)          != 0)  return 5;   // 0 -> first line
    if (editor_goto_target("", 100)           != -1) return 6;   // empty -> no jump
    if (editor_goto_target("12x", 100)        != -1) return 7;   // non-numeric -> no jump
    if (editor_goto_target("3", 1)            != 0)  return 8;   // single-line file
    if (editor_goto_target("2000000000", 100) != 99) return 9;   // saturating huge -> last line
    return 0;
}

// Push the current buffer onto the undo stack before a mutating edit. Snapshots only the used
// lines (line_count * EDITOR_LINE_LEN, contiguous in lines[]) so a small file costs little; drops
// the oldest snapshot when the stack is full. A failed kmalloc just skips undo for this edit.
static int editor_push_undo(editor_win_t* ed) {
    int bytes = ed->line_count * EDITOR_LINE_LEN;
    char* snap = (char*)kmalloc(bytes);
    if (!snap) return 0;
    memcpy_asm(snap, ed->lines, bytes);
    if (ed->undo_n == EDITOR_UNDO_N) {                       // full: free + shift out the oldest
        kfree(ed->undo[0].text);
        for (int i = 1; i < EDITOR_UNDO_N; i++) ed->undo[i - 1] = ed->undo[i];
        ed->undo_n--;
    }
    ed->undo[ed->undo_n].text = snap;
    ed->undo[ed->undo_n].line_count = ed->line_count;
    ed->undo[ed->undo_n].cx = ed->cursor_x;
    ed->undo[ed->undo_n].cy = ed->cursor_y;
    ed->undo[ed->undo_n].anchor_x = ed->selection_x;
    ed->undo[ed->undo_n].anchor_y = ed->selection_y;
    ed->undo[ed->undo_n].selected = ed->selection_active;
    ed->undo[ed->undo_n].crlf = ed->crlf;
    ed->undo_n++;
    return 1;
}

static void editor_copy_selection(editor_win_t* ed, int cut) {
    int sx, sy, ex, ey;
    if (!editor_selection_bounds(ed, &sx, &sy, &ex, &ey)) {
        snprintf(ed->status, sizeof(ed->status), "Select text first");
        return;
    }
    uint32_t size = 0;
    for (int y = sy; y <= ey; y++) {
        int first = y == sy ? sx : 0;
        int last = y == ey ? ex : (int)strlen(ed->lines[y]);
        size += (uint32_t)(last - first);
        if (y < ey) size += ed->crlf ? 2 : 1;
    }
    if (size > CLIPBOARD_MAX) {
        snprintf(ed->status, sizeof(ed->status), "Selection exceeds %d clipboard bytes", CLIPBOARD_MAX);
        return;
    }
    if (cut && sx + (int)strlen(ed->lines[ey]) - ex >= EDITOR_LINE_LEN) {
        snprintf(ed->status, sizeof(ed->status), "Cut would exceed %d columns", EDITOR_LINE_LEN - 1);
        return;
    }
    char* text = (char*)kmalloc(size);
    if (!text) { snprintf(ed->status, sizeof(ed->status), "Clipboard: out of memory"); return; }
    uint32_t pos = 0;
    for (int y = sy; y <= ey; y++) {
        int first = y == sy ? sx : 0;
        int last = y == ey ? ex : (int)strlen(ed->lines[y]);
        for (int x = first; x < last; x++) text[pos++] = ed->lines[y][x];
        if (y < ey) {
            if (ed->crlf) text[pos++] = '\r';
            text[pos++] = '\n';
        }
    }
    if (cut && !editor_push_undo(ed)) {
        kfree(text);
        snprintf(ed->status, sizeof(ed->status), "Cut cancelled: cannot save undo");
        return;
    }
    clipboard_set(text, size);
    kfree(text);
    if (cut) editor_delete_selection(ed);
    snprintf(ed->status, sizeof(ed->status), "%s %d bytes", cut ? "Cut" : "Copied", (int)size);
}

static void editor_paste(editor_win_t* ed) {
    if (!clipboard_len()) { snprintf(ed->status, sizeof(ed->status), "Clipboard is empty"); return; }
    char* text = (char*)kmalloc(CLIPBOARD_MAX);
    if (!text) { snprintf(ed->status, sizeof(ed->status), "Paste: out of memory"); return; }
    uint32_t size = clipboard_get(text, CLIPBOARD_MAX);
    if (!size) { kfree(text); return; }
    int sx, sy, ex, ey;
    if (!editor_selection_bounds(ed, &sx, &sy, &ex, &ey)) {
        sx = ex = ed->cursor_x; sy = ey = ed->cursor_y;
    }
    int rows = 0, col = sx;
    const char* error = NULL;
    for (uint32_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!c) { error = "NUL bytes are unsupported"; break; }
        if (c == '\r') {
            if (i + 1 >= size || text[++i] != '\n') { error = "bare CR is unsupported"; break; }
            c = '\n';
        }
        if (c == '\n') { rows++; col = 0; }
        else if (++col >= EDITOR_LINE_LEN) { error = "line exceeds 255 bytes"; break; }
    }
    int count = ed->line_count - (ey - sy) + rows;
    int tail = (int)strlen(ed->lines[ey]) - ex;
    if (!error && col + tail >= EDITOR_LINE_LEN) error = "joined line exceeds 255 bytes";
    if (!error && count > EDITOR_MAX_LINES) error = "document exceeds 512 rows";
    if (error) {
        kfree(text);
        snprintf(ed->status, sizeof(ed->status), "Paste rejected: %s", error);
        return;
    }
    // Stage the complete result so validation/OOM never consumes the selection.
    char (*lines)[EDITOR_LINE_LEN] = kmalloc((size_t)count * EDITOR_LINE_LEN);
    if (!lines) { kfree(text); snprintf(ed->status, sizeof(ed->status), "Paste: out of memory"); return; }
    memset_asm(lines, 0, (size_t)count * EDITOR_LINE_LEN);
    for (int y = 0; y < sy; y++) memcpy_asm(lines[y], ed->lines[y], EDITOR_LINE_LEN);
    memcpy_asm(lines[sy], ed->lines[sy], (size_t)sx);
    int y = sy, x = sx;
    for (uint32_t i = 0; i < size; i++) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n') { y++; x = 0; }
        else lines[y][x++] = text[i];
    }
    memcpy_asm(lines[y] + x, ed->lines[ey] + ex, (size_t)tail + 1);
    for (int r = ey + 1; r < ed->line_count; r++)
        memcpy_asm(lines[y + r - ey], ed->lines[r], EDITOR_LINE_LEN);
    if (!editor_push_undo(ed)) {
        kfree(lines); kfree(text);
        snprintf(ed->status, sizeof(ed->status), "Paste cancelled: cannot save undo");
        return;
    }
    memcpy_asm(ed->lines, lines, (size_t)count * EDITOR_LINE_LEN);
    ed->line_count = count;
    ed->cursor_x = x; ed->cursor_y = y;
    ed->modified = 1;
    editor_clear_selection(ed);
    kfree(lines); kfree(text);
    snprintf(ed->status, sizeof(ed->status), "Pasted %d bytes", (int)size);
}

// Ctrl+Z: restore (and drop) the most recent snapshot.
static void editor_undo_pop(editor_win_t* ed) {
    if (ed->undo_n == 0) { snprintf(ed->status, sizeof(ed->status), "Nothing to undo"); return; }
    editor_undo_t* u = &ed->undo[--ed->undo_n];
    ed->line_count = (u->line_count < 1) ? 1 : u->line_count;
    ed->crlf = u->crlf;
    memcpy_asm(ed->lines, u->text, u->line_count * EDITOR_LINE_LEN);
    ed->cursor_y = u->cy; ed->cursor_x = u->cx;
    ed->selection_x = u->anchor_x; ed->selection_y = u->anchor_y;
    ed->selection_active = u->selected;
    ed->selecting = 0;
    if (ed->cursor_y >= ed->line_count) ed->cursor_y = ed->line_count - 1;
    if (ed->cursor_y < 0) ed->cursor_y = 0;
    int llen = strlen(ed->lines[ed->cursor_y]);
    if (ed->cursor_x > llen) ed->cursor_x = llen;
    kfree(u->text); u->text = 0;
    ed->modified = 1;
    snprintf(ed->status, sizeof(ed->status), "Undo (%d left)", ed->undo_n);
}

// Free every retained snapshot (on window close, and when a new file replaces the buffer so a
// stale snapshot can't be restored over unrelated content).
static void editor_clear_undo(editor_win_t* ed) {
    for (int i = 0; i < ed->undo_n; i++) if (ed->undo[i].text) { kfree(ed->undo[i].text); ed->undo[i].text = 0; }
    ed->undo_n = 0;
}
void editor_win_close(window_t* win) {
    editor_win_t* ed = (editor_win_t*)win->reserved;
    if (ed) editor_clear_undo(ed);
}

void editor_win_key(window_t* win, int key) {
    editor_win_t* ed = (editor_win_t*)win->reserved;
    if (!ed) return;

    editor_clamp_cursor(ed);
    ed->selecting = 0;
    ed->cursor_tick = get_ticks();

    // Ctrl+F — enter incremental find mode (type a pattern, Enter = jump to next match,
    // Enter again = keep advancing, Esc = back to editing).
    if (key == 0x06 && !ed->repl_active && !ed->goto_active) {
        editor_clear_selection(ed);
        ed->find_active = 1; ed->find_len = 0; ed->find_pat[0] = '\0';
        snprintf(ed->status, sizeof(ed->status), "Find: ");
        return;
    }
    if (ed->find_active) {
        if (key == 0x1B) {                          // Esc — leave find mode
            ed->find_active = 0;
            ed->status[0] = 0;                       // clear the "Find:" prompt (Ln/Col shows in the bar)
            return;
        }
        if (key == '\r' || key == '\n') {           // Enter — jump to the next match
            if (ed->find_len == 0) return;
            int my, mx;
            if (editor_find_in(ed->lines, ed->line_count, ed->cursor_y, ed->cursor_x + 1,
                               ed->find_pat, &my, &mx)) {
                ed->cursor_y = my; ed->cursor_x = mx;
                editor_adjust_scroll(ed, win);
                snprintf(ed->status, sizeof(ed->status), "Found '%s' (Ln %d)", ed->find_pat, my + 1);
            } else {
                snprintf(ed->status, sizeof(ed->status), "Not found: %s", ed->find_pat);
            }
            return;
        }
        if (key == '\b') {                          // edit the pattern
            if (ed->find_len > 0) ed->find_pat[--ed->find_len] = '\0';
            snprintf(ed->status, sizeof(ed->status), "Find: %s", ed->find_pat);
            return;
        }
        if (key >= 0x20 && key <= 0x7E && ed->find_len < (int)sizeof(ed->find_pat) - 1) {
            ed->find_pat[ed->find_len++] = (char)key;
            ed->find_pat[ed->find_len] = '\0';
            snprintf(ed->status, sizeof(ed->status), "Find: %s", ed->find_pat);
        }
        return;                                     // swallow every other key while finding
    }

    // Ctrl+G — go to a line number. Pairs with the line-number gutter: type digits, Enter jumps
    // (clamped to the file), Esc cancels. Handy for a long file instead of scrolling by hand.
    if (key == 0x07 && !ed->repl_active) {
        editor_clear_selection(ed);
        ed->goto_active = 1; ed->goto_len = 0; ed->goto_buf[0] = '\0';
        snprintf(ed->status, sizeof(ed->status), "Go to line: ");
        return;
    }
    if (ed->goto_active) {
        if (key == 0x1B) {                          // Esc — cancel
            ed->goto_active = 0; ed->status[0] = 0;
            return;
        }
        if (key == '\r' || key == '\n') {           // Enter — jump
            int row = editor_goto_target(ed->goto_buf, ed->line_count);
            ed->goto_active = 0;
            if (row < 0) { snprintf(ed->status, sizeof(ed->status), "Go to line: cancelled"); return; }
            ed->cursor_y = row;
            int llen = strlen(ed->lines[row]);
            if (ed->cursor_x > llen) ed->cursor_x = llen;
            editor_adjust_scroll(ed, win);
            snprintf(ed->status, sizeof(ed->status), "Ln %d", row + 1);
            return;
        }
        if (key == '\b') {                          // edit the number
            if (ed->goto_len > 0) ed->goto_buf[--ed->goto_len] = '\0';
            snprintf(ed->status, sizeof(ed->status), "Go to line: %s", ed->goto_buf);
            return;
        }
        if (key >= '0' && key <= '9' && ed->goto_len < (int)sizeof(ed->goto_buf) - 1) {
            ed->goto_buf[ed->goto_len++] = (char)key;
            ed->goto_buf[ed->goto_len] = '\0';
            snprintf(ed->status, sizeof(ed->status), "Go to line: %s", ed->goto_buf);
        }
        return;                                     // swallow every other key while in goto mode
    }

    // Ctrl+R — find-and-replace. Phase 1: type the text to find, Enter. Phase 2: type the
    // replacement, Enter = replace every occurrence across the file. Esc cancels either phase.
    if (key == 0x12) {
        editor_clear_selection(ed);
        ed->repl_active = 1;
        ed->repl_find_len = 0; ed->repl_find[0] = '\0';
        ed->repl_with_len = 0; ed->repl_with[0] = '\0';
        snprintf(ed->status, sizeof(ed->status), "Replace - find: ");
        return;
    }
    if (ed->repl_active) {
        if (key == 0x1B) {                          // Esc — cancel
            ed->repl_active = 0;
            ed->status[0] = 0;                       // clear the "Replace:" prompt
            return;
        }
        if (key == '\r' || key == '\n') {
            if (ed->repl_active == 1) {             // finished the find text → ask for replacement
                if (ed->repl_find_len == 0) return; // need something to find
                ed->repl_active = 2;
                snprintf(ed->status, sizeof(ed->status), "Replace '%s' with: ", ed->repl_find);
                return;
            }
            editor_push_undo(ed);                   // so Ctrl+Z reverts the whole replace
            int n = editor_replace_all(ed->lines, ed->line_count, ed->repl_find, ed->repl_with);
            if (n > 0) ed->modified = 1;
            if (ed->cursor_y >= ed->line_count) ed->cursor_y = ed->line_count - 1;   // clamp after edits
            int llen = strlen(ed->lines[ed->cursor_y]);
            if (ed->cursor_x > llen) ed->cursor_x = llen;
            ed->repl_active = 0;
            snprintf(ed->status, sizeof(ed->status), "Replaced %d occurrence%s of '%s'",
                     n, n == 1 ? "" : "s", ed->repl_find);
            return;
        }
        if (key == '\b') {                          // edit whichever field is active
            if (ed->repl_active == 1) {
                if (ed->repl_find_len > 0) ed->repl_find[--ed->repl_find_len] = '\0';
                snprintf(ed->status, sizeof(ed->status), "Replace - find: %s", ed->repl_find);
            } else {
                if (ed->repl_with_len > 0) ed->repl_with[--ed->repl_with_len] = '\0';
                snprintf(ed->status, sizeof(ed->status), "Replace '%s' with: %s", ed->repl_find, ed->repl_with);
            }
            return;
        }
        if (key >= 0x20 && key <= 0x7E) {
            if (ed->repl_active == 1 && ed->repl_find_len < (int)sizeof(ed->repl_find) - 1) {
                ed->repl_find[ed->repl_find_len++] = (char)key; ed->repl_find[ed->repl_find_len] = '\0';
                snprintf(ed->status, sizeof(ed->status), "Replace - find: %s", ed->repl_find);
            } else if (ed->repl_active == 2 && ed->repl_with_len < (int)sizeof(ed->repl_with) - 1) {
                ed->repl_with[ed->repl_with_len++] = (char)key; ed->repl_with[ed->repl_with_len] = '\0';
                snprintf(ed->status, sizeof(ed->status), "Replace '%s' with: %s", ed->repl_find, ed->repl_with);
            }
        }
        return;                                     // swallow every other key while replacing
    }

    if (key == 0x13) { // Ctrl+S
        editor_save(ed);
        return;
    }

    if (key == 0x03 || key == 0x18 || key == 0x16) {
        if (key == 0x16) editor_paste(ed);
        else editor_copy_selection(ed, key == 0x18);
        editor_adjust_scroll(ed, win);
        return;
    }

    if (key == 0x01) { // Ctrl+A — the entire document, including intervening newlines
        ed->selection_x = 0; ed->selection_y = 0;
        ed->cursor_y = ed->line_count - 1;
        ed->cursor_x = (int)strlen(ed->lines[ed->cursor_y]);
        ed->selection_active = 1;
        editor_adjust_scroll(ed, win);
        snprintf(ed->status, sizeof(ed->status), "All text selected");
        return;
    }
    if (key == 0x1B) {
        editor_clear_selection(ed);
        return;
    }
    if (key == 0x0F) { // Ctrl+O
        // Open: if filename ends in .txt or similar, open it
        if (ed->filename[0]) {
            editor_open(ed, ed->filename);
        } else {
            snprintf(ed->status, sizeof(ed->status), "Save file first to set filename, then Ctrl+O to reopen");
        }
        return;
    }

    if (key == 0x1A) { // Ctrl+Z — undo the last edit
        editor_undo_pop(ed);
        editor_adjust_scroll(ed, win);
        return;
    }

    int sx, sy, ex, ey;
    int selected = editor_selection_bounds(ed, &sx, &sy, &ex, &ey);
    int editing = (key >= 0x20 && key <= 0x7E) || key == '\r' || key == '\n' ||
                  key == '\b' || key == KEY_DEL;
    if (editing) {
        editor_push_undo(ed);           // replacement + insertion is one undo step
        if (selected && !editor_delete_selection(ed)) return;
        if (selected) ed->status[0] = 0;
        editor_clear_selection(ed);
    }
    if (key >= 0x20 && key <= 0x7E) {
        editor_insert_char(ed, (char)key);
    } else if (key == '\r' || key == '\n') {
        editor_newline(ed);
    } else if (key == '\b') {
        if (!selected) editor_backspace(ed);
    } else if (key == KEY_DEL) {
        if (!selected) editor_delete(ed);
    } else if (key == KEY_LEFT) {
        if (selected) { ed->cursor_x = sx; ed->cursor_y = sy; }
        else if (ed->cursor_x > 0) ed->cursor_x--;
        else if (ed->cursor_y > 0) { ed->cursor_y--; ed->cursor_x = strlen(ed->lines[ed->cursor_y]); }
    } else if (key == KEY_RIGHT) {
        int len = strlen(ed->lines[ed->cursor_y]);
        if (selected) { ed->cursor_x = ex; ed->cursor_y = ey; }
        else if (ed->cursor_x < len) ed->cursor_x++;
        else if (ed->cursor_y < ed->line_count - 1) { ed->cursor_y++; ed->cursor_x = 0; }
    } else if (key == KEY_UP) {
        if (ed->cursor_y > 0) ed->cursor_y--;
    } else if (key == KEY_DOWN) {
        if (ed->cursor_y < ed->line_count - 1) ed->cursor_y++;
    } else if (key == KEY_HOME) {
        ed->cursor_x = 0;
    } else if (key == KEY_END) {
        ed->cursor_x = strlen(ed->lines[ed->cursor_y]);
    } else if (key == KEY_PGUP) {
        int rows = editor_rows(win);
        ed->cursor_y -= rows;
        if (ed->cursor_y < 0) ed->cursor_y = 0;
    } else if (key == KEY_PGDN) {
        int rows = editor_rows(win);
        ed->cursor_y += rows;
        if (ed->cursor_y >= ed->line_count) ed->cursor_y = ed->line_count - 1;
    }

    if (key >= KEY_UP && key <= KEY_PGDN) {
        editor_clear_selection(ed);
        if (selected) ed->status[0] = 0;
    }
    editor_adjust_scroll(ed, win);
    // (The live Ln/Col is drawn in the status bar every frame, so we no longer overwrite the
    // status MESSAGE on each keystroke — "Saved: ..." / "Opened: ..." now persists.)
}
