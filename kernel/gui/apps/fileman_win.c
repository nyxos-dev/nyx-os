#include "../core/theme.h"
#include "../../core/kernel.h"
#include "../core/compositor.h"
#include "fileman_win.h"
#include "../../drivers/video/font.h"
#include "../../auth/login.h"

#define TOOLBAR_H 28
#define BTN_SPACE 6
#define HEADER_H  18
#define SCROLL_W  12
#define CTX_W     160
#define CTX_ITEM_H 20

fileman_win_t* fileman_create_ctx(void) {
    fileman_win_t* fm = (fileman_win_t*)kmalloc(sizeof(fileman_win_t));
    if (!fm) return NULL;
    memset_asm(fm, 0, sizeof(fileman_win_t));
    strncpy(fm->cwd, g_login_home[0] ? g_login_home : "/", sizeof(fm->cwd));
    fm->cwd[sizeof(fm->cwd) - 1] = '\0';
    fm->sel_index = -1;
    fm->input_mode = 0;
    fm->input_cursor_tick = get_ticks();
    fm->mouse_down = 0;
    fm->drag_active = 0;
    fm->drag_file_idx = -1;
    fm->drag_mode = 0;
    fm->last_click_idx = -1;
    fm->search_active = 0;
    fm->search_pattern[0] = '\0';
    fm->search_count = 0;
    snprintf(fm->status, sizeof(fm->status), "Ready");
    return fm;
}

static int str_contains(const char* s, const char* sub) {
    if (!sub || !*sub) return 1;
    while (*s) {
        const char* a = s;
        const char* b = sub;
        while (*a && *b && (*a == *b || (*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) == (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b))) {
            a++; b++;
        }
        if (!*b) return 1;
        s++;
    }
    return 0;
}

static void fileman_apply_search(fileman_win_t* fm) {
    fm->search_count = 0;
    if (!fm->search_active || !fm->search_pattern[0]) {
        for (int i = 0; i < fm->entry_count; i++)
            fm->search_indices[fm->search_count++] = i;
        return;
    }
    for (int i = 0; i < fm->entry_count; i++) {
        if (str_contains(fm->entries[i], fm->search_pattern))
            fm->search_indices[fm->search_count++] = i;
    }
    if (fm->sel_index >= 0) {
        // Keep selection if still visible
        int found = 0;
        for (int i = 0; i < fm->search_count; i++)
            if (fm->search_indices[i] == fm->sel_index) { found = 1; break; }
        if (!found) fm->sel_index = -1;
    }
}

// The list is shown either straight or filtered; `search_indices[i]` is the real entries[]
// index at DISPLAY position i (identity when no filter is active). `sel_index` and
// `drag_file_idx` are ALWAYS real entries[] indices; these map between a real index and the
// display position (row) that keyboard navigation + scrolling operate in. Keeping those two
// conventions from colliding is issue #76: the nav code used to compare sel_index (a real
// index) against a position bound and then re-index search_indices[] with it as a position.
static int fm_disp_count(fileman_win_t* fm) {
    return fm->search_active ? fm->search_count : fm->entry_count;
}
static int fm_pos_to_real(fileman_win_t* fm, int pos) {   // display row -> entries[] index
    if (pos < 0) return -1;
    return fm->search_active ? fm->search_indices[pos] : pos;
}
static int fm_sel_to_pos(fileman_win_t* fm) {             // sel_index (real) -> display row, or -1
    if (fm->sel_index < 0) return -1;
    if (!fm->search_active) return fm->sel_index;
    for (int i = 0; i < fm->search_count; i++)
        if (fm->search_indices[i] == fm->sel_index) return i;
    return -1;
}

// KAT (issue #76): the real-index <-> display-position mapping stays consistent under a
// search filter, so arrow-key navigation lands on the right entry (not a real index reused
// as a position). Uses a throwaway fileman state; no windows / VFS touched.
int fileman_nav_selftest(void) {
    static fileman_win_t fm;
    memset_asm(&fm, 0, sizeof fm);
    fm.entry_count = 8;
    fm.search_active = 1; fm.search_count = 3;       // filtered view shows entries 2,5,7
    fm.search_indices[0] = 2; fm.search_indices[1] = 5; fm.search_indices[2] = 7;
    if (fm_pos_to_real(&fm, 0) != 2) return 10;
    if (fm_pos_to_real(&fm, 2) != 7) return 11;
    fm.sel_index = 5; if (fm_sel_to_pos(&fm) != 1) return 12;   // real 5 -> pos 1
    fm.sel_index = 7; if (fm_sel_to_pos(&fm) != 2) return 13;
    fm.sel_index = 4; if (fm_sel_to_pos(&fm) != -1) return 14;  // 4 not in the filter
    fm.sel_index = 5;                                            // DOWN: pos 1 -> 2 -> real 7
    { int pos = fm_sel_to_pos(&fm); if (pos < fm_disp_count(&fm) - 1) pos++; if (fm_pos_to_real(&fm, pos) != 7) return 15; }
    { int pos = fm_sel_to_pos(&fm); if (pos > 0) pos--; if (fm_pos_to_real(&fm, pos) != 2) return 16; } // UP: pos 1 -> 0 -> real 2
    fm.search_active = 0; fm.sel_index = 3;                     // no filter: identity mapping
    if (fm_sel_to_pos(&fm) != 3 || fm_pos_to_real(&fm, 4) != 4) return 17;
    return 0;
}

// Case-insensitive name compare: <0 if x<y, 0 if equal, >0 if x>y. Shorter name
// first on a common prefix.
static int fm_namecmp(const char* x, const char* y) {
    for (; *x && *y; x++, y++) {
        char cx = (*x >= 'A' && *x <= 'Z') ? (char)(*x + 32) : *x;
        char cy = (*y >= 'A' && *y <= 'Z') ? (char)(*y + 32) : *y;
        if (cx != cy) return (int)cx - (int)cy;
    }
    return (*x ? 1 : 0) - (*y ? 1 : 0);
}

// Order two entries for the listing. Directories always group before files (the
// standard "folders first" behaviour). Within a group, order by the active sort
// column: Name (case-insensitive) or Size, honouring the asc/desc direction;
// Size ties break by name so the order stays stable.
static int fm_entry_less(fileman_win_t* fm, int a, int b) {
    int da = fm->entry_types[a] ? 0 : 1;   // dirs sort before files
    int db = fm->entry_types[b] ? 0 : 1;
    if (da != db) return da < db;
    int cmp;
    if (fm->sort_key == FM_SORT_SIZE && !fm->entry_types[a]) {
        // Size is only meaningful for files (dirs carry 0); tie-break by name.
        if (fm->entry_sizes[a] != fm->entry_sizes[b])
            cmp = (fm->entry_sizes[a] < fm->entry_sizes[b]) ? -1 : 1;
        else
            cmp = fm_namecmp(fm->entries[a], fm->entries[b]);
    } else {
        cmp = fm_namecmp(fm->entries[a], fm->entries[b]);
    }
    if (fm->sort_desc) cmp = -cmp;
    return cmp < 0;
}

// Selection sort of the parallel entry arrays (name/type/size in lockstep).
// n <= FILEMAN_MAX_ENTRIES and this runs only on refresh, so O(n^2) is fine.
static void fileman_sort(fileman_win_t* fm) {
    for (int i = 0; i < fm->entry_count - 1; i++) {
        int m = i;
        for (int j = i + 1; j < fm->entry_count; j++)
            if (fm_entry_less(fm, j, m)) m = j;
        if (m != i) {
            char tn[64];
            strncpy(tn, fm->entries[i], sizeof(tn)); tn[sizeof(tn)-1] = '\0';
            strncpy(fm->entries[i], fm->entries[m], 64); fm->entries[i][63] = '\0';
            strncpy(fm->entries[m], tn, 64); fm->entries[m][63] = '\0';
            int tt = fm->entry_types[i]; fm->entry_types[i] = fm->entry_types[m]; fm->entry_types[m] = tt;
            uint32_t ts = fm->entry_sizes[i]; fm->entry_sizes[i] = fm->entry_sizes[m]; fm->entry_sizes[m] = ts;
        }
    }
}

void fileman_refresh(fileman_win_t* fm) {
    fm->entry_count = 0;
    fm->scroll_offset = 0;
    int fd = vfs_open(fm->cwd, 0, 0);
    if (fd < 0) {
        snprintf(fm->status, sizeof(fm->status), "Cannot open: %s", fm->cwd);
        return;
    }
    int skipped_names = 0;
    dirent_t* de = vfs_readdir(fd);
    while (de && fm->entry_count < FILEMAN_MAX_ENTRIES) {
        if (strlen(de->name) >= sizeof(fm->entries[0])) {
            skipped_names++;
            de = vfs_readdir(fd);
            continue;
        }
        strncpy(fm->entries[fm->entry_count], de->name, 63);
        fm->entries[fm->entry_count][63] = '\0';
        fm->entry_types[fm->entry_count] = de->type;
        // Stat files for the Size column; directories carry no byte size (drawn "<DIR>").
        fm->entry_sizes[fm->entry_count] = 0;
        if (!de->type) {
            char epath[320];
            if (strcmp(fm->cwd, "/") == 0) snprintf(epath, sizeof(epath), "/%s", de->name);
            else                            snprintf(epath, sizeof(epath), "%s/%s", fm->cwd, de->name);
            uint32_t esz = 0; int eisd = 0;
            if (vfs_stat(epath, &esz, &eisd) == 0) fm->entry_sizes[fm->entry_count] = esz;
        }
        fm->entry_count++;
        de = vfs_readdir(fd);
    }
    vfs_close(fd);
    fileman_sort(fm);        // directories first, then case-insensitive A-Z
    fm->sel_index = -1;      // a fresh read invalidates any prior selection index
    // Directory totals for the status bar: count dirs vs files and sum the file
    // bytes, skipping the "." / ".." self/parent links so they don't inflate the count.
    fm->summ_dirs = 0; fm->summ_files = 0; fm->summ_bytes = 0;
    for (int i = 0; i < fm->entry_count; i++) {
        const char* nm = fm->entries[i];
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;                                   // skip "." and ".."
        if (fm->entry_types[i]) fm->summ_dirs++;
        else { fm->summ_files++; fm->summ_bytes += fm->entry_sizes[i]; }
    }
    snprintf(fm->status, sizeof(fm->status), "%d entries", fm->entry_count);
    fileman_apply_search(fm);
    if (skipped_names) snprintf(fm->status, sizeof(fm->status), "Names over 63 bytes hidden: %d", skipped_names);
}

static void fileman_refresh_status(fileman_win_t* fm) {
    char status[sizeof(fm->status)];
    memcpy(status, fm->status, sizeof(status));
    fileman_refresh(fm);
    memcpy(fm->status, status, sizeof(status));
}

// Format a byte count as a compact human-readable string (B / KB / MB) for the
// Size column. Integer math only (the kernel printf has no %f).
static void fileman_fmt_size(uint32_t b, char* out, int n) {
    if (b < 1024u)               snprintf(out, n, "%u B",  b);
    else if (b < 1024u * 1024u)  snprintf(out, n, "%u KB", (b + 512u) / 1024u);
    else                         snprintf(out, n, "%u MB", (b + 512u * 1024u) / (1024u * 1024u));
}

static int fileman_join_path(fileman_win_t* fm, const char* dir, const char* name,
                             char* path, int maxlen) {
    size_t prefix = strcmp(dir, "/") == 0 ? 0 : strlen(dir);
    if (maxlen < 2 || prefix + 1 + strlen(name) >= (size_t)maxlen) {
        snprintf(fm->status, sizeof(fm->status), "Path too long");
        return -1;
    }
    if (!prefix) snprintf(path, maxlen, "/%s", name);
    else snprintf(path, maxlen, "%s/%s", dir, name);
    return 0;
}

static int fileman_get_path(fileman_win_t* fm, const char* name, char* path, int maxlen) {
    return fileman_join_path(fm, fm->cwd, name, path, maxlen);
}

static void fileman_cd(fileman_win_t* fm, const char* dir, window_t* win) {
    (void)win;
    char newpath[256];
    if (strcmp(dir, "..") == 0) {
        if (strcmp(fm->cwd, "/") == 0) return;
        char* last_slash = NULL;
        for (int i = 0; fm->cwd[i]; i++)
            if (fm->cwd[i] == '/') last_slash = &fm->cwd[i];
        if (last_slash && last_slash > fm->cwd) {
            *last_slash = '\0';
            strncpy(newpath, fm->cwd, sizeof(newpath));
            *last_slash = '/';
        } else {
            strncpy(newpath, "/", sizeof(newpath));
        }
    } else if (dir[0] == '/') {
        if (strlen(dir) >= sizeof(newpath)) {
            snprintf(fm->status, sizeof(fm->status), "Path too long");
            return;
        }
        strcpy(newpath, dir);
    } else if (fileman_get_path(fm, dir, newpath, sizeof(newpath)) < 0) {
        return;
    }
    newpath[255] = '\0';
    int fd = vfs_open(newpath, 0, 0);
    if (fd >= 0) {
        vfs_close(fd);
        strncpy(fm->cwd, newpath, sizeof(fm->cwd));
        fm->cwd[255] = '\0';
        fileman_refresh(fm);
    } else {
        snprintf(fm->status, sizeof(fm->status), "Cannot enter: %s", newpath);
    }
}



// 1 if `name` ends with a known image extension (case-insensitive) — decides which app opens it.
static int fileman_is_image(const char* name) {
    static const char* const exts[] = { ".png", ".gif", ".jpg", ".jpeg", ".bmp", 0 };
    int n = 0; while (name[n]) n++;
    for (int e = 0; exts[e]; e++) {
        int m = 0; while (exts[e][m]) m++;
        if (n < m) continue;
        int match = 1;
        for (int k = 0; k < m; k++) {
            char a = name[n - m + k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);   // ASCII tolower
            if (a != exts[e][k]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

// Open a file the user activated (double-click / Enter): image files go to the Image Viewer,
// everything else to the Text Editor. `name` is the display name for the status line.
static void fileman_activate_file(fileman_win_t* fm, const char* path, const char* name) {
    if (fileman_is_image(name)) {
        launch_imageview(path);
        snprintf(fm->status, sizeof(fm->status), "Opened in Image Viewer: %s", name);
    } else if (compositor_open_editor(path)) {
        snprintf(fm->status, sizeof(fm->status), "Opened in editor: %s", name);
    } else {
        snprintf(fm->status, sizeof(fm->status), "Cannot open editor");
    }
}

static void fileman_delete(fileman_win_t* fm) {
    if (fm->sel_index < 0 || fm->sel_index >= fm->entry_count) {
        snprintf(fm->status, sizeof(fm->status), "No file selected");
        return;
    }
    char path[256];
    if (fileman_get_path(fm, fm->entries[fm->sel_index], path, sizeof(path)) < 0) return;
    if (vfs_unlink(path) == 0) {
        snprintf(fm->status, sizeof(fm->status), "Deleted: %s", fm->entries[fm->sel_index]);
        fm->sel_index = -1;
        fileman_refresh(fm);
    } else {
        snprintf(fm->status, sizeof(fm->status), "Cannot delete: %s", fm->entries[fm->sel_index]);
    }
}

void fileman_new_folder(fileman_win_t* fm) {
    fm->input_mode = 2;
    memset_asm(fm->input_buf, 0, sizeof(fm->input_buf));
    fm->input_pos = 0;
    fm->input_cursor_tick = get_ticks();
    snprintf(fm->status, sizeof(fm->status), "Enter folder name:");
}

void fileman_new_file(fileman_win_t* fm); // forward

static void ctx_rename(fileman_win_t* fm) {
    if (fm->sel_index < 0 || fm->sel_index >= fm->entry_count) return;
    strncpy(fm->input_buf, fm->entries[fm->sel_index], sizeof(fm->input_buf) - 1);
    fm->input_pos = strlen(fm->input_buf);
    fm->input_mode = 3;
    fm->input_cursor_tick = get_ticks();
    snprintf(fm->status, sizeof(fm->status), "Rename:");
}

static void ctx_copy(fileman_win_t* fm) {
    if (fm->sel_index < 0 || fm->sel_index >= fm->entry_count) return;
    if (fileman_get_path(fm, fm->entries[fm->sel_index], fm->clipboard_path, sizeof(fm->clipboard_path)) < 0) return;
    fm->clipboard_mode = 1;
    snprintf(fm->status, sizeof(fm->status), "Copied: %s", fm->entries[fm->sel_index]);
}

static void ctx_cut(fileman_win_t* fm) {
    if (fm->sel_index < 0 || fm->sel_index >= fm->entry_count) return;
    if (fileman_get_path(fm, fm->entries[fm->sel_index], fm->clipboard_path, sizeof(fm->clipboard_path)) < 0) return;
    fm->clipboard_mode = 2;
    snprintf(fm->status, sizeof(fm->status), "Cut: %s", fm->entries[fm->sel_index]);
}

static void ctx_paste(fileman_win_t* fm) {
    if (fm->clipboard_mode == 0 || !fm->clipboard_path[0]) {
        snprintf(fm->status, sizeof(fm->status), "Clipboard is empty");
        return;
    }
    const char* basename = fm->clipboard_path;
    for (int i = 0; fm->clipboard_path[i]; i++)
        if (fm->clipboard_path[i] == '/') basename = &fm->clipboard_path[i] + 1;
    char dst[256];
    if (fileman_get_path(fm, basename, dst, sizeof(dst)) < 0) return;
    if (fm->clipboard_mode == 1) {
        int result = 1;
        for (int attempt = 0; attempt < 100 && result == 1; attempt++) {
            if (attempt) {
                char name[64];
                int suffix = attempt == 1 ? 5 : attempt < 10 ? 6 : 7;
                if (strlen(basename) + suffix >= sizeof(name)) {
                    snprintf(fm->status, sizeof(fm->status), "Copy name too long");
                    return;
                }
                if (attempt == 1) snprintf(name, sizeof(name), "%s_copy", basename);
                else snprintf(name, sizeof(name), "%s_copy%d", basename, attempt);
                if (fileman_get_path(fm, name, dst, sizeof(dst)) < 0) return;
            }
            result = vfs_cp_new(fm->clipboard_path, dst);
        }
        if (result == 0) snprintf(fm->status, sizeof(fm->status), "Pasted: %s", dst);
        else if (result == 1) snprintf(fm->status, sizeof(fm->status), "Copy names exhausted");
        else snprintf(fm->status, sizeof(fm->status), "Paste failed");
    } else if (fm->clipboard_mode == 2) {
        if (vfs_rename(fm->clipboard_path, dst) == 0) {
            snprintf(fm->status, sizeof(fm->status), "Moved: %s", basename);
            fm->clipboard_mode = 0;
            fm->clipboard_path[0] = '\0';
        } else {
            snprintf(fm->status, sizeof(fm->status), "Move failed; cut retained");
        }
    }
    fileman_refresh_status(fm);
}

// Snapshot the selected entry's details into the props_* fields and open the
// Properties modal. Read from the snapshot (not sel_index) so the panel is stable.
static void fileman_show_properties(fileman_win_t* fm) {
    if (fm->sel_index < 0 || fm->sel_index >= fm->entry_count) {
        snprintf(fm->status, sizeof(fm->status), "No file selected");
        return;
    }
    int i = fm->sel_index;
    strncpy(fm->props_name, fm->entries[i], sizeof(fm->props_name) - 1);
    fm->props_name[sizeof(fm->props_name) - 1] = '\0';
    fm->props_is_dir = fm->entry_types[i];
    fm->props_size = fm->entry_sizes[i];
    if (fileman_get_path(fm, fm->entries[i], fm->props_path, sizeof(fm->props_path)) < 0) return;
    fm->props_open = 1;
}

static const char* ctx_items[] = {"Rename", "Copy", "Cut", "Paste", "---", "Delete", "New File", "New Folder", "---", "Properties"};
#define CTX_ITEMS_COUNT (sizeof(ctx_items) / sizeof(ctx_items[0]))

static void ctx_do_action(fileman_win_t* fm, int idx) {
    fm->ctx_open = 0;
    switch (idx) {
        case 0: ctx_rename(fm); break;
        case 1: ctx_copy(fm); break;
        case 2: ctx_cut(fm); break;
        case 3: ctx_paste(fm); break;
        case 5: fileman_delete(fm); break;
        case 6: fileman_new_file(fm); break;
        case 7: fileman_new_folder(fm); break;
        case 9: fileman_show_properties(fm); break;
    }
}

void fileman_new_file(fileman_win_t* fm) {
    fm->input_mode = 1;
    memset_asm(fm->input_buf, 0, sizeof(fm->input_buf));
    fm->input_pos = 0;
    fm->input_cursor_tick = get_ticks();
    snprintf(fm->status, sizeof(fm->status), "Enter file name:");
}



// Per-row entry icons for the file list: a folder glyph for directories and a
// document glyph for files, so the two read apart at a glance (a modern file-
// manager touch replacing the old "/" / " " text prefix). Sized to sit inside a
// FONT_HEIGHT (16px) row and drawn opaque, so they show over a selected row's
// highlight too. The folder blue matches the directory name colour for cohesion.
static void fm_draw_folder_icon(int x, int y, int rowh) {
    const int iw = 13, ih = 10;
    int iy = y + (rowh - ih) / 2;
    uint32_t body = fb_rgb(95,165,235), lite = fb_rgb(150,200,248), dark = fb_rgb(60,110,180);
    fb_fill_rect(x, iy, 6, 2, dark);                  // back tab
    fb_fill_rect(x, iy + 2, iw, ih - 2, body);        // body
    fb_fill_rect(x, iy + 2, iw, 1, lite);             // top highlight
    fb_fill_rect(x, iy + ih - 1, iw, 1, dark);        // bottom shade
}

// Case-insensitive extension compare; returns 0 when equal.
static int fm_extcmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

// Tint the document icon by file type, so a listing reads like a modern file
// manager: executables green, objects amber, source blue, images pink, text/data
// their own greys — everything else a neutral page.
static uint32_t fm_file_accent(const char* name) {
    int n = 0; while (name[n]) n++;
    const char* ext = 0;
    for (int i = n - 1; i >= 0 && name[i] != '/'; i--)
        if (name[i] == '.') { ext = name + i + 1; break; }
    if (!ext || !ext[0]) return fb_rgb(208,213,223);
    if (!fm_extcmp(ext, "elf")) return fb_rgb(150,205,140);                 // executable
    if (!fm_extcmp(ext, "o"))   return fb_rgb(228,190,130);                 // object
    if (!fm_extcmp(ext, "c")  || !fm_extcmp(ext, "h")   || !fm_extcmp(ext, "asm") ||
        !fm_extcmp(ext, "nyx")|| !fm_extcmp(ext, "sh")  || !fm_extcmp(ext, "py"))
        return fb_rgb(150,180,235);                                        // source
    if (!fm_extcmp(ext, "png")|| !fm_extcmp(ext, "bmp") || !fm_extcmp(ext, "gif") ||
        !fm_extcmp(ext, "jpg")|| !fm_extcmp(ext, "jpeg")|| !fm_extcmp(ext, "ppm"))
        return fb_rgb(212,160,225);                                        // image
    if (!fm_extcmp(ext, "gz") || !fm_extcmp(ext, "tar") || !fm_extcmp(ext, "img") ||
        !fm_extcmp(ext, "iso")|| !fm_extcmp(ext, "wav"))
        return fb_rgb(220,205,140);                                        // archive/data
    return fb_rgb(208,213,223);                                            // text / other
}

static void fm_draw_file_icon(int x, int y, int rowh, uint32_t page) {
    const int iw = 10, ih = 12;
    int ix = x + 2, iy = y + (rowh - ih) / 2;
    uint32_t edge = fb_rgb(108,114,128), fold = fb_rgb(86,92,106);   // dark, so lines read on any tint
    fb_fill_rect(ix, iy, iw, ih, page);               // page body (type-tinted)
    fb_fill_rect(ix, iy, iw, 1, fb_rgb(245,247,250)); // top highlight
    fb_fill_rect(ix + iw - 3, iy, 3, 3, fold);        // folded top-right corner
    fb_fill_rect(ix + 2, iy + 4, iw - 4, 1, edge);    // faint text lines
    fb_fill_rect(ix + 2, iy + 6, iw - 4, 1, edge);
    fb_fill_rect(ix + 2, iy + 8, iw - 5, 1, edge);
    fb_fill_rect(ix, iy + ih - 1, iw, 1, edge);       // bottom edge
}

// A small sort-direction triangle built from stacked bars (no glyph needed):
// apex up = ascending, apex down = descending. Marks the active sort column.
static void fm_draw_sort_arrow(int x, int y, int up, uint32_t col) {
    for (int r = 0; r < 4; r++) {
        int wr = up ? (2 * r + 1) : (7 - 2 * r);   // 1,3,5,7 (up) or 7,5,3,1 (down)
        fb_fill_rect(x + (7 - wr) / 2, y + r, wr, 1, col);
    }
}

// Type-column labels come from the shared kernel classifier (kernel/fs/filetype.c).
extern const char* filetype_label(const char* name, int is_dir);

void fileman_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    fileman_win_t* fm = (fileman_win_t*)win->reserved;
    if (!fm) return;
    uint32_t char_h = FONT_HEIGHT;

    fb_fill_rect(cx, cy, cw, ch, fb_rgb(30,30,35));

    // Toolbar
    int tb_y = cy;
    fb_fill_rect(cx, tb_y, cw, TOOLBAR_H, fb_rgb(50,55,65));
    fb_fill_rect(cx, tb_y + TOOLBAR_H - 1, cw, 1, fb_rgb(70,70,80));

    int bx = cx + 4;
    fb_fill_rect(bx, tb_y + 2, 80, TOOLBAR_H - 4, THEME_BUTTON);
    font_draw_string(bx + (80 - strlen("New File") * FONT_WIDTH) / 2, tb_y + (TOOLBAR_H - FONT_HEIGHT) / 2, "New File", THEME_TEXT, THEME_BUTTON);
    bx += 84;
    fb_fill_rect(bx, tb_y + 2, 80, TOOLBAR_H - 4, THEME_BUTTON);
    font_draw_string(bx + (80 - strlen("New Dir") * FONT_WIDTH) / 2, tb_y + (TOOLBAR_H - FONT_HEIGHT) / 2, "New Dir", THEME_TEXT, THEME_BUTTON);
    bx += 84;
    fb_fill_rect(bx, tb_y + 2, 80, TOOLBAR_H - 4, fb_rgb(80,50,50));
    font_draw_string(bx + (80 - strlen("Delete") * FONT_WIDTH) / 2, tb_y + (TOOLBAR_H - FONT_HEIGHT) / 2, "Delete", THEME_TEXT, fb_rgb(80,50,50));
    bx += 84;
    fb_fill_rect(bx, tb_y + 2, 80, TOOLBAR_H - 4, THEME_BUTTON);
    font_draw_string(bx + (80 - strlen("Go Up") * FONT_WIDTH) / 2, tb_y + (TOOLBAR_H - FONT_HEIGHT) / 2, "Go Up", THEME_TEXT, THEME_BUTTON);
    bx += 84;
    fb_fill_rect(bx, tb_y + 2, 80, TOOLBAR_H - 4, THEME_BUTTON);
    font_draw_string(bx + (80 - strlen("Refresh") * FONT_WIDTH) / 2, tb_y + (TOOLBAR_H - FONT_HEIGHT) / 2, "Refresh", THEME_TEXT, THEME_BUTTON);
    bx += 84;
    fb_fill_rect(bx, tb_y + 2, 60, TOOLBAR_H - 4, fm->search_active ? fb_rgb(80,70,40) : THEME_BUTTON);
    font_draw_string(bx + (60 - strlen("Find") * FONT_WIDTH) / 2, tb_y + (TOOLBAR_H - FONT_HEIGHT) / 2, "Find", THEME_TEXT, fm->search_active ? fb_rgb(80,70,40) : THEME_BUTTON);

    // Directory path bar
    int path_y = tb_y + TOOLBAR_H;
    fb_fill_rect(cx, path_y, cw, HEADER_H, THEME_PANEL_HEADER);
    font_draw_string(cx + 4, path_y + (HEADER_H - char_h) / 2, fm->cwd, fb_rgb(200,200,255), THEME_PANEL_HEADER);

    // Search bar (visible when search_active)
    int search_bar_h = fm->search_active ? HEADER_H : 0;
    if (fm->search_active) {
        int sby = path_y + HEADER_H;
        fb_fill_rect(cx, sby, cw, (uint32_t)search_bar_h, fb_rgb(60,50,30));
        char search_disp[128];
        snprintf(search_disp, sizeof(search_disp), "Search: %s_ (%d/%d)", fm->search_pattern,
            fm->search_count, fm->entry_count);
        font_draw_string(cx + 4, sby + (HEADER_H - char_h) / 2, search_disp,
            fb_rgb(255,255,150), fb_rgb(60,50,30));
    }

    // Header row — clickable column headers (click to sort by that column, click
    // the active column again to flip direction). The active header is tinted the
    // Nyx accent purple and carries a direction arrow; the inactive one is white.
    int list_header_y = path_y + HEADER_H + search_bar_h;
    uint32_t hdr_bg = fb_rgb(50,55,65);
    uint32_t hdr_active = fb_rgb(200,180,255);   // Nyx purple accent
    fb_fill_rect(cx, list_header_y, cw, char_h + 2, hdr_bg);
    // Type column: a name-based type label between Name and Size (Windows-Explorer
    // style). Fixed widths are reserved on the right; Name takes the remaining space
    // and is truncated to fit. Hidden on a very narrow window.
    int fm_list_w   = (int)cw - SCROLL_W;
    int fm_size_w   = 72, fm_type_w = 100;
    int fm_type_x   = cx + fm_list_w - fm_size_w - fm_type_w;
    int fm_name_x   = cx + 4 + 16;
    int fm_show_type = (fm_type_x > fm_name_x + 40);
    uint32_t name_col = (fm->sort_key == FM_SORT_NAME) ? hdr_active : fb_rgb(255,255,255);
    uint32_t size_col = (fm->sort_key == FM_SORT_SIZE) ? hdr_active : fb_rgb(255,255,255);
    font_draw_string(cx + 4, list_header_y + 1, "Name", name_col, hdr_bg);
    if (fm->sort_key == FM_SORT_NAME)
        fm_draw_sort_arrow(cx + 4 + (int)strlen("Name") * FONT_WIDTH + 4, list_header_y + 7, !fm->sort_desc, name_col);
    if (fm_show_type)
        font_draw_string(fm_type_x, list_header_y + 1, "Type", fb_rgb(255,255,255), hdr_bg);
    {
        const char* size_hdr = "Size";
        int shx = cx + ((int)cw - SCROLL_W) - 8 - (int)strlen(size_hdr) * FONT_WIDTH;
        if (fm->sort_key == FM_SORT_SIZE)
            fm_draw_sort_arrow(shx - 11, list_header_y + 7, !fm->sort_desc, size_col);
        font_draw_string(shx, list_header_y + 1, size_hdr, size_col, hdr_bg);
    }

    // File listing
    int list_y = list_header_y + char_h + 4;
    int avail_h = (int)(cy + ch - list_y - HEADER_H - 4);
    int max_rows = avail_h / (int)char_h;
    if (max_rows < 1) max_rows = 1;
    int list_w = (int)cw - SCROLL_W;

    int disp_count = fm->search_active ? fm->search_count : fm->entry_count;
    for (int i = fm->scroll_offset; i < disp_count && (i - fm->scroll_offset) < max_rows; i++) {
        int row = i - fm->scroll_offset;
        int ei = fm->search_active ? fm->search_indices[i] : i;
        int ey = list_y + row * char_h;
        uint32_t bg = (ei == fm->sel_index) ? THEME_SELECTION : fb_rgb(30,30,35);
        fb_fill_rect(cx + 2, ey, (uint32_t)(list_w - 4), char_h, bg);

        if (fm->entry_types[ei]) fm_draw_folder_icon(cx + 4, ey, (int)char_h);
        else                     fm_draw_file_icon(cx + 4, ey, (int)char_h, fm_file_accent(fm->entries[ei]));
        uint32_t fg = fm->entry_types[ei] ? fb_rgb(100,200,255) : THEME_TEXT;
        const char* nm = fm->entries[ei];
        char nmbuf[80];
        if (fm_show_type) {                                  // truncate the name to leave room for Type
            int maxch = (fm_type_x - 6 - fm_name_x) / FONT_WIDTH;
            if (maxch < 3) maxch = 3;
            if ((int)strlen(nm) > maxch) {
                int k = maxch - 1;
                if (k > (int)sizeof(nmbuf) - 2) k = (int)sizeof(nmbuf) - 2;
                for (int j = 0; j < k; j++) nmbuf[j] = nm[j];
                nmbuf[k] = '~'; nmbuf[k + 1] = '\0';
                nm = nmbuf;
            }
        }
        font_draw_string(fm_name_x, ey, nm, fg, bg);
        if (fm_show_type)
            font_draw_string(fm_type_x, ey, filetype_label(fm->entries[ei], fm->entry_types[ei]),
                             fb_rgb(150,155,170), bg);
        // Right-aligned Size column: byte count for files, "<DIR>" for folders.
        char szbuf[16];
        if (fm->entry_types[ei]) snprintf(szbuf, sizeof(szbuf), "<DIR>");
        else                     fileman_fmt_size(fm->entry_sizes[ei], szbuf, sizeof(szbuf));
        int szx = cx + list_w - 8 - (int)strlen(szbuf) * FONT_WIDTH;
        font_draw_string(szx, ey, szbuf, fm->entry_types[ei] ? fb_rgb(120,150,180) : fb_rgb(170,170,180), bg);
    }

    // Scrollbar
    int sb_x = cx + list_w;
    int sb_y = list_y;
    int sb_h = avail_h;
    fb_fill_rect(sb_x, sb_y, SCROLL_W, (uint32_t)sb_h, fb_rgb(40,40,45));

    if (disp_count > max_rows) {
        int thumb_top = (fm->scroll_offset * sb_h) / disp_count;
        int thumb_h = (max_rows * sb_h) / disp_count;
        if (thumb_h < 8) thumb_h = 8;
        if (thumb_top + thumb_h > sb_h) thumb_top = sb_h - thumb_h;
        fb_fill_rect(sb_x, sb_y + thumb_top, SCROLL_W, (uint32_t)thumb_h, fb_rgb(100,100,110));
        fb_fill_rect(sb_x, sb_y + thumb_top, SCROLL_W, 1, fb_rgb(140,140,150));
        fb_fill_rect(sb_x, sb_y + thumb_top + thumb_h - 1, SCROLL_W, 1, fb_rgb(70,70,80));
    }

    // Status bar
    int status_y = cy + ch - HEADER_H;
    fb_fill_rect(cx, status_y, cw, HEADER_H, THEME_PANEL_HEADER);

    if (fm->input_mode) {
        // Show input prompt
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "%s %s_", fm->status, fm->input_buf);
        font_draw_string(cx + 4, status_y + (HEADER_H - char_h) / 2, prompt, fb_rgb(255,255,100), THEME_PANEL_HEADER);
    } else {
        font_draw_string(cx + 4, status_y + (HEADER_H - char_h) / 2, fm->status, fb_rgb(180,180,180), THEME_PANEL_HEADER);
        // Right-aligned directory totals (a modern file-manager status bar): dir/file
        // counts and the summed file size, drawn only when there is room so it never
        // collides with a long left-hand status/preview message.
        char summ[48], szbuf[16];
        fileman_fmt_size(fm->summ_bytes, szbuf, sizeof(szbuf));
        snprintf(summ, sizeof(summ), "%d dirs, %d files, %s", fm->summ_dirs, fm->summ_files, szbuf);
        int sx = cx + (int)cw - 6 - (int)strlen(summ) * FONT_WIDTH;
        int left_end = cx + 4 + (int)strlen(fm->status) * FONT_WIDTH + 10;
        if (sx > left_end)
            font_draw_string(sx, status_y + (HEADER_H - char_h) / 2, summ, fb_rgb(150,170,200), THEME_PANEL_HEADER);
    }

    // Drag ghost (drag_file_idx is a real entries[] index — set + consumed as one; #76)
    if (fm->drag_active && fm->drag_file_idx >= 0 && fm->drag_file_idx < fm->entry_count) {
        int gx = fm->drag_cur_x + 8;
        int gy = fm->drag_cur_y + 8;
        int gw = 180, gh = FONT_HEIGHT + 6;
        if (gx + gw > (int)fb_get_width()) gx = (int)fb_get_width() - gw;
        if (gy + gh > (int)fb_get_height()) gy = (int)fb_get_height() - gh;
        fb_fill_rect(gx, gy, (uint32_t)gw, (uint32_t)gh, THEME_SELECTION);
        fb_fill_rect(gx, gy, (uint32_t)gw, 1, fb_rgb(140,180,255));
        fb_fill_rect(gx, gy + gh - 1, (uint32_t)gw, 1, fb_rgb(140,180,255));
        fb_fill_rect(gx, gy, 1, (uint32_t)gh, fb_rgb(140,180,255));
        fb_fill_rect(gx + gw - 1, gy, 1, (uint32_t)gh, fb_rgb(140,180,255));
        char label[72];
        int drag_ei = fm->drag_file_idx;   // already a real entries[] index (#76)
        snprintf(label, sizeof(label), "%c %s  [%s]",
            (drag_ei >= 0 && drag_ei < fm->entry_count && fm->entry_types[drag_ei]) ? '/' : ' ',
            drag_ei >= 0 && drag_ei < fm->entry_count ? fm->entries[drag_ei] : "?",
            fm->drag_mode == 2 ? "COPY" : "MOVE");
        font_draw_string(gx + 4, gy + 3, label, fb_rgb(255,255,255), THEME_SELECTION);
    }

    // Context menu
    if (fm->ctx_open) {
        int cmx = fm->ctx_x, cmy = fm->ctx_y;
        int cmw = CTX_W, cmh = CTX_ITEM_H * CTX_ITEMS_COUNT + 4;
        fb_fill_rect(cmx, cmy, cmw, cmh, fb_rgb(50,50,55));
        fb_fill_rect(cmx, cmy, cmw, 1, fb_rgb(100,100,110));
        fb_fill_rect(cmx, cmy + cmh - 1, cmw, 1, fb_rgb(100,100,110));
        fb_fill_rect(cmx, cmy, 1, cmh, fb_rgb(100,100,110));
        fb_fill_rect(cmx + cmw - 1, cmy, 1, cmh, fb_rgb(100,100,110));
        for (uint32_t i = 0; i < CTX_ITEMS_COUNT; i++) {
            int iy = cmy + 2 + i * CTX_ITEM_H;
            uint32_t bg = ((int)i == fm->ctx_hover) ? fb_rgb(70,90,130) : fb_rgb(50,50,55);
            if (ctx_items[i][0] == '-' && ctx_items[i][1] == '-') {
                bg = fb_rgb(50,50,55);
                fb_fill_rect(cmx + 4, iy + CTX_ITEM_H / 2, cmw - 8, 1, fb_rgb(80,80,90));
                continue;
            }
            fb_fill_rect(cmx + 2, iy, cmw - 4, CTX_ITEM_H, bg);
            // Disable paste if clipboard empty
            if (i == 3 && (fm->clipboard_mode == 0 || !fm->clipboard_path[0]))
                font_draw_string(cmx + 10, iy + 2, ctx_items[i], THEME_BORDER, bg);
            else
                font_draw_string(cmx + 10, iy + 2, ctx_items[i], THEME_TEXT, bg);
        }
    }

    // Properties modal — drawn last so it overlays everything; any click closes it (fileman_win_click).
    if (fm->props_open) {
        int pw = 320, ph = 130;
        int px = cx + ((int)cw - pw) / 2;
        int py = cy + ((int)ch - ph) / 2;
        if (px < cx + 4) px = cx + 4;
        if (py < cy + 4) py = cy + 4;
        fb_fill_rect(px, py, pw, ph, fb_rgb(45, 45, 55));
        fb_fill_rect(px, py, pw, 1, fb_rgb(120, 120, 140));
        fb_fill_rect(px, py + ph - 1, pw, 1, fb_rgb(120, 120, 140));
        fb_fill_rect(px, py, 1, ph, fb_rgb(120, 120, 140));
        fb_fill_rect(px + pw - 1, py, 1, ph, fb_rgb(120, 120, 140));
        fb_fill_rect(px, py, pw, char_h + 6, THEME_PANEL_HEADER);
        font_draw_string(px + 8, py + 3, "Properties", fb_rgb(255, 255, 255), THEME_PANEL_HEADER);
        int ly = py + char_h + 12;
        char line[400];
        snprintf(line, sizeof(line), "Name: %s", fm->props_name);
        font_draw_string(px + 10, ly, line, fb_rgb(220, 220, 230), fb_rgb(45, 45, 55)); ly += char_h + 4;
        snprintf(line, sizeof(line), "Type: %s", fm->props_is_dir ? "Directory" : "File");
        font_draw_string(px + 10, ly, line, fb_rgb(220, 220, 230), fb_rgb(45, 45, 55)); ly += char_h + 4;
        if (fm->props_is_dir) {
            snprintf(line, sizeof(line), "Size: --");
        } else {
            char sb[16]; fileman_fmt_size(fm->props_size, sb, sizeof(sb));
            snprintf(line, sizeof(line), "Size: %s (%u bytes)", sb, fm->props_size);
        }
        font_draw_string(px + 10, ly, line, fb_rgb(220, 220, 230), fb_rgb(45, 45, 55)); ly += char_h + 4;
        snprintf(line, sizeof(line), "Path: %s", fm->props_path);
        font_draw_string(px + 10, ly, line, fb_rgb(220, 220, 230), fb_rgb(45, 45, 55));
        font_draw_string(px + 10, py + ph - char_h - 6, "(click to close)", fb_rgb(150, 150, 160), fb_rgb(45, 45, 55));
    }
}

void fileman_win_click(window_t* win, int mx, int my, int btn) {
    fileman_win_t* fm = (fileman_win_t*)win->reserved;
    if (!fm) return;
    if (fm->props_open) { fm->props_open = 0; return; }   // any click dismisses the Properties panel
    int cx = WIN_CLIENT_X(win), cy = WIN_CLIENT_Y(win);
    uint32_t cw = win->w, ch = win->h;
    uint32_t char_h = FONT_HEIGHT;

    // During drag, ignore click events (drop handled in mousemove on button release)
    if (fm->drag_active) {
        return;
    }

    if (fm->entry_count == 0) fileman_refresh(fm);

    // Context menu handling (left-click closes menu or executes action)
    if (btn == 1 && fm->ctx_open) {
        int cmx = fm->ctx_x, cmy = fm->ctx_y;
        int cmw = CTX_W, cmh = CTX_ITEM_H * CTX_ITEMS_COUNT + 4;
        if (mx >= cmx && mx < cmx + cmw && my >= cmy && my < cmy + cmh) {
            int idx = (my - cmy - 2) / CTX_ITEM_H;
            if (idx >= 0 && idx < (int)CTX_ITEMS_COUNT && ctx_items[idx][0] != '-') {
                ctx_do_action(fm, idx);
            }
        }
        fm->ctx_open = 0;
        return;
    }

    int search_bar_h = fm->search_active ? HEADER_H : 0;
    int disp_count = fm->search_active ? fm->search_count : fm->entry_count;

    // Right-click: show context menu
    if (btn == 2) {
        fm->ctx_hover = -1;

        // Check if click is in file list area
        int list_header_y = cy + TOOLBAR_H + HEADER_H + search_bar_h;
        int list_y = list_header_y + char_h + 4;
        int avail_h = (int)(ch - TOOLBAR_H - HEADER_H - search_bar_h - char_h - 4 - HEADER_H - 4);
        int max_rows = avail_h / (int)char_h;
        if (max_rows < 1) max_rows = 1;

        if (my >= list_y && my < list_y + max_rows * (int)char_h) {
            int idx = (my - list_y) / (int)char_h + fm->scroll_offset;
            if (idx >= 0 && idx < disp_count) {
                int real_idx = fm->search_active ? fm->search_indices[idx] : idx;
                fm->sel_index = real_idx;
            }
        }

        fm->ctx_open = 1;
        fm->ctx_x = mx;
        fm->ctx_y = my;
        // Keep menu within window bounds
        int cmw = CTX_W, cmh = CTX_ITEM_H * CTX_ITEMS_COUNT + 4;
        if (fm->ctx_x + cmw > cx + (int)cw) fm->ctx_x = cx + (int)cw - cmw;
        if (fm->ctx_y + cmh > cy + (int)ch) fm->ctx_y = cy + (int)ch - cmh;
        return;
    }

    // Left-click: normal handling
    // Toolbar clicks
    int tb_y = cy;
    if (my >= tb_y && my < tb_y + TOOLBAR_H) {
        int rel_x = mx - cx;
        if (rel_x >= 4 && rel_x < 84) { fileman_new_file(fm); return; }
        if (rel_x >= 88 && rel_x < 168) { fileman_new_folder(fm); return; }
        if (rel_x >= 172 && rel_x < 252) { fileman_delete(fm); return; }
        if (rel_x >= 256 && rel_x < 336) { fileman_cd(fm, "..", win); return; }
        if (rel_x >= 340 && rel_x < 420) { fileman_refresh(fm); return; }
        // Find/Search button
        if (rel_x >= 424 && rel_x < 484) {
            if (fm->search_active) {
                fm->search_active = 0;
                fm->search_pattern[0] = '\0';
                fm->scroll_offset = 0;
                fileman_apply_search(fm);
                snprintf(fm->status, sizeof(fm->status), "Search closed");
            } else {
                fm->search_active = 1;
                fm->search_pattern[0] = '\0';
                fm->scroll_offset = 0;
                fileman_apply_search(fm);
                snprintf(fm->status, sizeof(fm->status), "Type to search (Esc to close)");
            }
            return;
        }
    }

    // File list area
    int list_header_y = cy + TOOLBAR_H + HEADER_H + search_bar_h;
    int list_w = (int)cw - SCROLL_W;

    // Column-header click: sort by that column; clicking the active column flips
    // ascending/descending. The right ~96px (over the Size values) is the Size
    // zone, the rest of the header is the Name zone. Re-sort and drop the stale
    // selection index (it points into the pre-sort order).
    if (btn == 1 && my >= list_header_y && my < list_header_y + (int)char_h + 2) {
        int want = (mx >= cx + list_w - 96) ? FM_SORT_SIZE : FM_SORT_NAME;
        if (fm->sort_key == want) fm->sort_desc = !fm->sort_desc;
        else { fm->sort_key = want; fm->sort_desc = 0; }
        fileman_sort(fm);
        fm->sel_index = -1;
        fileman_apply_search(fm);
        snprintf(fm->status, sizeof(fm->status), "Sorted by %s, %s",
            want == FM_SORT_SIZE ? "size" : "name", fm->sort_desc ? "descending" : "ascending");
        return;
    }

    int list_y = list_header_y + char_h + 4;
    int avail_h = (int)(ch - TOOLBAR_H - HEADER_H - search_bar_h - char_h - 4 - HEADER_H - 4);
    int max_rows = avail_h / (int)char_h;
    if (max_rows < 1) max_rows = 1;

    // Scrollbar click
    int sb_x = cx + list_w;
    if (mx >= sb_x && mx < sb_x + SCROLL_W && my >= list_y && my < list_y + avail_h) {
        if (disp_count > max_rows) {
            int rel_y = my - list_y;
            fm->scroll_offset = (rel_y * disp_count) / avail_h;
            if (fm->scroll_offset > disp_count - max_rows)
                fm->scroll_offset = disp_count - max_rows;
            if (fm->scroll_offset < 0) fm->scroll_offset = 0;
        }
        return;
    }

    if (my >= list_y && my < list_y + max_rows * (int)char_h) {
        int idx = (my - list_y) / (int)char_h + fm->scroll_offset;
        if (idx >= 0 && idx < disp_count) {
            int real_idx = fm->search_active ? fm->search_indices[idx] : idx;
            fm->sel_index = real_idx;
            if (fm->entry_types[real_idx]) {
                fm->last_click_idx = -1;
                fileman_cd(fm, fm->entries[real_idx], win);
            } else {
                char path[256];
                if (fileman_get_path(fm, fm->entries[real_idx], path, sizeof(path)) < 0) return;
                uint32_t now = get_ticks();
                if (fm->last_click_idx == real_idx && (now - fm->last_click_tick) < 400) {
                    // Second click on the same file within the double-click window: open it
                    // (images -> Image Viewer, everything else -> Text Editor).
                    fileman_activate_file(fm, path, fm->entries[real_idx]);
                    fm->last_click_idx = -1;   // don't re-fire on a third click
                } else {
                    // First click: select. An image just gets a hint; a text file previews its
                    // first bytes (dumping a binary image's bytes into the status bar is garbage).
                    if (fileman_is_image(fm->entries[real_idx])) {
                        snprintf(fm->status, sizeof(fm->status), "%s (image) - double-click to view", fm->entries[real_idx]);
                    } else {
                        int fd = vfs_open(path, 0, 0);
                        if (fd >= 0) {
                            char buf[512];
                            int n = vfs_read(fd, buf, sizeof(buf)-1);
                            vfs_close(fd);
                            if (n > 0) {
                                buf[n] = '\0';
                                snprintf(fm->status, sizeof(fm->status), "%s (%d bytes) - double-click to edit: %.170s", fm->entries[real_idx], n, buf);
                            } else {
                                snprintf(fm->status, sizeof(fm->status), "%s (empty) - double-click to edit", fm->entries[real_idx]);
                            }
                        }
                    }
                    fm->last_click_tick = now;
                    fm->last_click_idx = real_idx;
                }
            }
        }
    }
}

// Map a y coordinate to the entry index at that list row (or -1 if outside the
// list). Uses the same geometry as draw/click, so drop-target detection matches
// what the user sees.
static int fileman_row_at(fileman_win_t* fm, window_t* win, int my) {
    int cy = WIN_CLIENT_Y(win);
    uint32_t char_h = FONT_HEIGHT;
    int search_bar_h = fm->search_active ? HEADER_H : 0;
    int list_header_y = cy + TOOLBAR_H + HEADER_H + search_bar_h;
    int list_y = list_header_y + (int)char_h + 4;
    int avail_h = (int)(win->h - TOOLBAR_H - HEADER_H - search_bar_h - (int)char_h - 4 - HEADER_H - 4);
    int max_rows = avail_h / (int)char_h;
    if (max_rows < 1) max_rows = 1;
    int disp_count = fm->search_active ? fm->search_count : fm->entry_count;
    if (my < list_y || my >= list_y + max_rows * (int)char_h) return -1;
    int idx = (my - list_y) / (int)char_h + fm->scroll_offset;
    if (idx < 0 || idx >= disp_count) return -1;
    return fm->search_active ? fm->search_indices[idx] : idx;
}

void fileman_win_mousemove(window_t* win, int mx, int my, int btns) {
    fileman_win_t* fm = (fileman_win_t*)win->reserved;
    if (!fm) return;

    int search_bar_h = fm->search_active ? HEADER_H : 0;
    int disp_count = fm->search_active ? fm->search_count : fm->entry_count;

    // Track button state
    if (btns & 1) {
        if (!fm->mouse_down) {
            // Left button just pressed (first mousemove with btn down)
            fm->mouse_down = 1;
            fm->drag_start_x = mx;
            fm->drag_start_y = my;
            // Determine which file was under cursor at press time
            int cy = WIN_CLIENT_Y(win);
            uint32_t char_h = FONT_HEIGHT;
            int list_header_y = cy + TOOLBAR_H + HEADER_H + search_bar_h;
            int list_y = list_header_y + char_h + 4;
            int avail_h = (int)(win->h - TOOLBAR_H - HEADER_H - search_bar_h - char_h - 4 - HEADER_H - 4);
            int max_rows = avail_h / (int)char_h;
            if (max_rows < 1) max_rows = 1;
            if (my >= list_y && my < list_y + max_rows * (int)char_h) {
                int idx = (my - list_y) / (int)char_h + fm->scroll_offset;
                if (idx >= 0 && idx < disp_count)
                    fm->drag_file_idx = fm->search_active ? fm->search_indices[idx] : idx;
                else
                    fm->drag_file_idx = -1;
            } else {
                fm->drag_file_idx = -1;
            }
        }
        // Check for drag activation (distance threshold)
        if (fm->mouse_down && !fm->drag_active && fm->drag_file_idx >= 0) {
            int dx = mx - fm->drag_start_x;
            int dy = my - fm->drag_start_y;
            if (dx * dx + dy * dy > 36) { // 6px threshold
                fm->drag_active = 1;
                fm->drag_mode = is_ctrl_pressed() ? 2 : 1; // Ctrl = copy, else move
                fm->drag_cur_x = mx;
                fm->drag_cur_y = my;
            }
        }
    } else {
        // Button released — perform the drop.
        if (fm->drag_active) {
            int src_idx = (fm->drag_file_idx >= 0 && fm->drag_file_idx < fm->entry_count)
                ? fm->drag_file_idx : -1;
            if (src_idx >= 0) {
                char src[256];
                if (fileman_get_path(fm, fm->entries[src_idx], src, sizeof(src)) < 0) goto drop_done;
                const char* basename = src;
                for (int i = 0; src[i]; i++)
                    if (src[i] == '/') basename = &src[i] + 1;

                // Destination directory: if dropped onto a folder row, move/copy the
                // file INTO that folder; otherwise it stays in the current directory.
                char dstdir[256];
                int tgt = fileman_row_at(fm, win, my);
                if (tgt >= 0 && tgt != src_idx && fm->entry_types[tgt]) {
                    if (fileman_get_path(fm, fm->entries[tgt], dstdir, sizeof(dstdir)) < 0) goto drop_done;
                } else {
                    strncpy(dstdir, fm->cwd, sizeof(dstdir));
                    dstdir[sizeof(dstdir) - 1] = '\0';
                }
                char dst[256];
                if (fileman_join_path(fm, dstdir, basename, dst, sizeof(dst)) < 0)
                    goto drop_done;

                if (strcmp(src, dst) == 0) {
                    snprintf(fm->status, sizeof(fm->status), "Dropped in place");
                } else if (fm->drag_mode == 2) {          // Ctrl-drag → copy
                    if (vfs_cp_new(src, dst) == 0)
                        snprintf(fm->status, sizeof(fm->status), "Copied %s -> %s", basename, dstdir);
                    else
                        snprintf(fm->status, sizeof(fm->status), "Copy failed (name exists?)");
                } else {                                   // plain drag → move
                    if (vfs_rename(src, dst) == 0)
                        snprintf(fm->status, sizeof(fm->status), "Moved %s -> %s", basename, dstdir);
                    else
                        snprintf(fm->status, sizeof(fm->status), "Move failed (name exists?)");
                }
                fileman_refresh_status(fm);
            }
        }
drop_done:
        fm->mouse_down = 0;
        fm->drag_active = 0;
        fm->drag_file_idx = -1;
    }

    if (fm->drag_active) {
        fm->drag_cur_x = mx;
        fm->drag_cur_y = my;
    }

    // Context menu hover
    if (fm->ctx_open) {
        int cmx = fm->ctx_x, cmy = fm->ctx_y;
        int cmw = CTX_W, cmh = CTX_ITEM_H * CTX_ITEMS_COUNT + 4;
        if (mx >= cmx && mx < cmx + cmw && my >= cmy && my < cmy + cmh) {
            int idx = (my - cmy - 2) / CTX_ITEM_H;
            if (idx >= 0 && idx < (int)CTX_ITEMS_COUNT)
                fm->ctx_hover = idx;
            else
                fm->ctx_hover = -1;
        } else {
            fm->ctx_hover = -1;
        }
    }
}

void fileman_win_key(window_t* win, int key) {
    fileman_win_t* fm = (fileman_win_t*)win->reserved;
    if (!fm) return;

    if (!fm->input_mode &&
        (window_key_modifiers() & (KEY_MOD_CTRL | KEY_MOD_ALT)) == KEY_MOD_CTRL) {
        // Cooked Ctrl letters arrive as bytes 1..26, even after Ctrl is released.
        int shortcut = key;
        if (shortcut >= 1 && shortcut <= 26) shortcut += 'a' - 1;
        if (shortcut >= 'A' && shortcut <= 'Z') shortcut += 'a' - 'A';
        if (shortcut == 'f') {
            // Toggle search
            if (fm->search_active) {
                fm->search_active = 0;
                fm->search_pattern[0] = '\0';
                fm->scroll_offset = 0;
                fileman_apply_search(fm);
                snprintf(fm->status, sizeof(fm->status), "Search closed");
            } else {
                fm->search_active = 1;
                fm->input_mode = 0; // Ensure not in input mode
                fm->search_pattern[0] = '\0';
                fm->scroll_offset = 0;
                fileman_apply_search(fm);
                snprintf(fm->status, sizeof(fm->status), "Type search pattern (Esc to close)");
            }
            return;
        }
        if (shortcut == 'c') {
            ctx_copy(fm);
            return;
        }
        if (shortcut == 'x') {
            ctx_cut(fm);
            return;
        }
        if (shortcut == 'v') {
            ctx_paste(fm);
            return;
        }
        if (shortcut == 'a') {
            if (fm_disp_count(fm) > 0) {
                fm->sel_index = fm_pos_to_real(fm, 0);
                fm->scroll_offset = 0;
                snprintf(fm->status, sizeof(fm->status), "Selected: %s", fm->entries[fm->sel_index]);
            }
            return;
        }
    }

    // Search input mode
    if (fm->search_active && !fm->input_mode) {
        if (key == 0x1B) {
            fm->search_active = 0;
            fm->search_pattern[0] = '\0';
            fm->scroll_offset = 0;
            fileman_apply_search(fm);
            snprintf(fm->status, sizeof(fm->status), "Search closed");
            return;
        }
        if (key == '\b') {
            int len = strlen(fm->search_pattern);
            if (len > 0) {
                fm->search_pattern[len - 1] = '\0';
                fm->scroll_offset = 0;
                fileman_apply_search(fm);
            }
            return;
        }
        if (key >= 0x20 && key <= 0x7E) {
            int len = strlen(fm->search_pattern);
            if (len < 62) {
                fm->search_pattern[len] = (char)key;
                fm->search_pattern[len + 1] = '\0';
                fm->scroll_offset = 0;
                fileman_apply_search(fm);
            }
            return;
        }
    }

    // Navigation keys when not in input mode
    if (!fm->input_mode) {
        int disp_count = fm->search_active ? fm->search_count : fm->entry_count;
        if (disp_count == 0) return;

        // Calculate visible rows (same as in draw)
        int char_h = FONT_HEIGHT;
        uint32_t ch = win->h;
        int search_bar_h = fm->search_active ? HEADER_H : 0;
        int avail_h = (int)(ch - TOOLBAR_H - HEADER_H - search_bar_h - char_h - 4 - HEADER_H - 4);
        int max_rows = avail_h / char_h;
        if (max_rows < 1) max_rows = 1;

        if (key == KEY_DOWN) {
            int pos = fm_sel_to_pos(fm);
            if (pos < 0) pos = 0;                       // no selection yet -> first row
            else if (pos < disp_count - 1) pos++;
            fm->sel_index = fm_pos_to_real(fm, pos);
            if (pos >= fm->scroll_offset + max_rows)    // scroll_offset is a POSITION, so compare with pos
                fm->scroll_offset = pos - max_rows + 1;
            return;
        }
        if (key == KEY_UP) {
            int pos = fm_sel_to_pos(fm);
            if (pos < 0) pos = 0;
            else if (pos > 0) pos--;
            fm->sel_index = fm_pos_to_real(fm, pos);
            if (pos < fm->scroll_offset)
                fm->scroll_offset = pos;
            return;
        }
        if (key == KEY_PGDN) {
            fm->scroll_offset += max_rows;
            if (fm->scroll_offset >= disp_count)
                fm->scroll_offset = disp_count - 1;
            if (fm->scroll_offset < 0) fm->scroll_offset = 0;
            fm->sel_index = fm_pos_to_real(fm, fm->scroll_offset);
            return;
        }
        if (key == KEY_PGUP) {
            fm->scroll_offset -= max_rows;
            if (fm->scroll_offset < 0) fm->scroll_offset = 0;
            fm->sel_index = fm_pos_to_real(fm, fm->scroll_offset);
            return;
        }
        if (key == KEY_HOME) {
            fm->scroll_offset = 0;
            fm->sel_index = fm_pos_to_real(fm, 0);
            return;
        }
        if (key == KEY_END) {
            int pos = disp_count - 1;
            fm->sel_index = fm_pos_to_real(fm, pos);
            fm->scroll_offset = pos - max_rows + 1;      // scroll by POSITION, not the real index
            if (fm->scroll_offset < 0) fm->scroll_offset = 0;
            return;
        }
        if (key == '\n' && fm->sel_index >= 0) {
            if (fm->entry_types[fm->sel_index]) {
                fileman_cd(fm, fm->entries[fm->sel_index], win);
            } else {
                // Enter on a file opens it (image -> Image Viewer, else Text Editor), like double-click.
                char path[256];
                if (fileman_get_path(fm, fm->entries[fm->sel_index], path, sizeof(path)) < 0) return;
                fileman_activate_file(fm, path, fm->entries[fm->sel_index]);
            }
            return;
        }
        return;
    }

    if (key == '\n') {
        // Commit input
        if (fm->input_pos > 0) {
            char path[256];
            if (fileman_get_path(fm, fm->input_buf, path, sizeof(path)) < 0) return;
            if (fm->input_mode == 3) {
                // Rename. The selection was validated when the rename STARTED,
                // but it can be cleared while the user is still typing the new
                // name — fileman_refresh() drops it when a search filter no
                // longer matches, and deleting resets it to -1. Committing then
                // indexed entries[-1], reading 64 bytes from before the array
                // and handing them to vfs_rename as a path. Re-check here, the
                // same way every other consumer of sel_index already does.
                if (fm->sel_index < 0 || fm->sel_index >= fm->entry_count) {
                    snprintf(fm->status, sizeof(fm->status), "Rename cancelled: selection lost");
                    fm->input_mode = 0;
                    fm->input_pos = 0;
                    fm->input_buf[0] = '\0';
                    return;
                }
                char old_path[256];
                if (fileman_get_path(fm, fm->entries[fm->sel_index], old_path, sizeof(old_path)) < 0) return;
                if (vfs_rename(old_path, path) == 0)
                    snprintf(fm->status, sizeof(fm->status), "Renamed to: %s", fm->input_buf);
                else
                    snprintf(fm->status, sizeof(fm->status), "Rename failed (name exists?)");
                fileman_refresh_status(fm);
            } else if (fm->input_mode == 2) {
                // Create directory
                if (vfs_mkdir(path, 0) == 0) {
                    snprintf(fm->status, sizeof(fm->status), "Created folder: %s", fm->input_buf);
                } else {
                    snprintf(fm->status, sizeof(fm->status), "Cannot create folder: %s", fm->input_buf);
                }
                fileman_refresh(fm);
            } else {
                // Create file (empty)
                int fd = vfs_open(path, 1, 0);
                if (fd >= 0) {
                    vfs_close(fd);
                    snprintf(fm->status, sizeof(fm->status), "Created file: %s", fm->input_buf);
                } else {
                    snprintf(fm->status, sizeof(fm->status), "Cannot create file: %s", fm->input_buf);
                }
                fileman_refresh(fm);
            }
        }
        fm->input_mode = 0;
        fm->input_buf[0] = '\0';
        return;
    }

    if (key == 0x1B) {
        // Cancel
        fm->input_mode = 0;
        fm->input_buf[0] = '\0';
        snprintf(fm->status, sizeof(fm->status), "Cancelled");
        return;
    }

    if (key == '\b') {
        if (fm->input_pos > 0) {
            fm->input_pos--;
            fm->input_buf[fm->input_pos] = '\0';
        }
        return;
    }

    // Only printable characters
    if (key >= 0x20 && key <= 0x7E && fm->input_pos < 63) {
        fm->input_buf[fm->input_pos++] = (char)key;
        fm->input_buf[fm->input_pos] = '\0';
    }
}
