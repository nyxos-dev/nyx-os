#ifndef EDITOR_WIN_H
#define EDITOR_WIN_H

#include "../../core/kernel.h"
#include "../core/compositor.h"

#define EDITOR_MAX_LINES 512
#define EDITOR_LINE_LEN 256
#define EDITOR_UNDO_N   16          // undo depth (snapshots kept before mutating edits)

// One undo snapshot: a heap copy of the used lines (line_count * EDITOR_LINE_LEN bytes) + cursor.
typedef struct { char* text; int line_count, cx, cy, anchor_x, anchor_y, selected, crlf; } editor_undo_t;

typedef struct {
    char lines[EDITOR_MAX_LINES][EDITOR_LINE_LEN];
    int line_count;
    int crlf;                     // separators between rows: 0 = LF (new files), 1 = CRLF
    int cursor_x, cursor_y;
    int scroll_x, scroll_y;
    int selection_x, selection_y;   // fixed anchor; the cursor is the moving end
    int selection_active;
    int selecting;                 // left-button drag started in the text area
    char filename[64];
    int modified;
    int close_pending, close_discard, close_failed;
    char status[64];
    uint32_t cursor_tick;
    int  find_active;          // Ctrl+F find mode: capturing a search pattern
    char find_pat[64];         // the pattern typed in find mode
    int  find_len;
    int  repl_active;          // Ctrl+R replace: 0=off, 1=typing find text, 2=typing replacement
    char repl_find[64];        // the text to find
    int  repl_find_len;
    char repl_with[64];        // the replacement text
    int  repl_with_len;
    int  goto_active;          // Ctrl+G goto-line mode: capturing a target line number
    char goto_buf[12];         // the digits typed in goto mode
    int  goto_len;
    editor_undo_t undo[EDITOR_UNDO_N];   // Ctrl+Z undo stack (undo[undo_n-1] = most recent)
    int  undo_n;
} editor_win_t;

editor_win_t* editor_create_ctx(void);
void editor_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void editor_win_click(window_t* win, int mx, int my, int btn);
void editor_win_pressed(window_t* win, int mx, int my, int btn);
void editor_win_mousemove(window_t* win, int mx, int my, int btns);
void editor_win_key(window_t* win, int key);
void editor_win_close(window_t* win);   // on_close: free the undo snapshots before the ctx is kfree'd
int editor_win_can_close(window_t* win);
void editor_load_file(editor_win_t* ed, const char* path);  // load `path` into this editor
int  editor_find_selftest(void);                            // KAT for the Ctrl+F search core (editor_win.c)
int  editor_replace_selftest(void);                         // KAT for the Ctrl+R replace core (editor_win.c)
// Resolve a Ctrl+G goto-line entry to a 0-based line index, clamped to [0, line_count-1];
// returns -1 for an empty/non-numeric entry (no jump). Pure; pinned by editor_goto_selftest.
int  editor_goto_target(const char* buf, int line_count);
int  editor_goto_selftest(void);                            // KAT for the Ctrl+G goto-line core (editor_win.c)
int  editor_selection_selftest(void);                       // selection delete, bounds and cursor viewport

#endif
