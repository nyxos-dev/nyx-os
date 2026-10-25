#ifndef FILEMAN_WIN_H
#define FILEMAN_WIN_H

#include "../../core/kernel.h"
#include "../core/compositor.h"

#define FILEMAN_MAX_ENTRIES 512

// Sort columns for the clickable list headers.
#define FM_SORT_NAME 0
#define FM_SORT_SIZE 1

typedef struct {
    char cwd[256];
    char entries[FILEMAN_MAX_ENTRIES][64];
    int entry_types[FILEMAN_MAX_ENTRIES];
    uint32_t entry_sizes[FILEMAN_MAX_ENTRIES];   // Valid only when entry_size_known is set.
    uint8_t entry_size_known[FILEMAN_MAX_ENTRIES];
    int entry_count;
    int scroll_offset;
    char status[128];
    int sel_index;
    int input_mode;  // 0=none, 1=filename, 2=dirname, 3=rename
    char input_buf[64];
    int input_pos;
    uint32_t input_cursor_tick;
    // Context menu
    int ctx_open;
    int ctx_x, ctx_y;
    int ctx_hover;
    // Clipboard
    char clipboard_path[256];
    int clipboard_mode; // 0=none, 1=copy, 2=cut
    // Search
    int search_active;
    char search_pattern[64];
    int search_indices[FILEMAN_MAX_ENTRIES];
    int search_count;
    // Mouse state (for drag-and-drop)
    int mouse_down;
    int drag_active;
    int drag_file_idx;
    int drag_start_x, drag_start_y;
    int drag_cur_x, drag_cur_y;
    int drag_mode; // 0=none, 1=move (cut), 2=copy
    // Double-click detection (open a file in the Text Editor)
    uint32_t last_click_tick;
    int last_click_idx;
    // Properties modal (shown by the context-menu "Properties" item; any click closes it)
    int props_open;
    char props_name[64];
    char props_path[320];
    uint32_t props_size;
    int props_size_known;
    int props_is_dir;
    // Directory summary (computed on refresh) for the status-bar totals — dir/file
    // counts and the summed byte size of the files in the current directory.
    int summ_dirs;
    int summ_files;
    uint64_t summ_bytes;
    int summ_unknown;
    // Column sort: click a header to sort by it, click the active header again to
    // flip direction. Directories always group first; the key/direction order the
    // files within. sort_key: FM_SORT_NAME / FM_SORT_SIZE. sort_desc: 0=ascending.
    int sort_key;
    int sort_desc;
} fileman_win_t;

fileman_win_t* fileman_create_ctx(void);
void fileman_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void fileman_win_click(window_t* win, int mx, int my, int btn);
void fileman_win_key(window_t* win, int key);
void fileman_win_mousemove(window_t* win, int mx, int my, int btns);
void fileman_refresh(fileman_win_t* fm);
void fileman_new_folder(fileman_win_t* fm);
void fileman_new_file(fileman_win_t* fm);
int  fileman_nav_selftest(void);   // KAT (#76): sel_index real-index <-> display-position mapping under a search filter

#endif
