#ifndef TERMINAL_WIN_H
#define TERMINAL_WIN_H

#include "../../core/kernel.h"
#include "../core/compositor.h"

#define TERM_LINES      2000
#define TERM_COLS       80
#define TERM_INPUT_MAX  256
#define TERM_OUTPUT_MAX 4096
#define TERM_CLEAR_SEQUENCE "\x1b[2J\x1b[H\x1b[3J"
#define TERM_HIST_MAX   16     // command lines kept for Up/Down recall (ring buffer)

// Command-line history ring (Up/Down recall, v6.4.326). Newest entry is the one added
// last; term_hist_add skips blank lines and an exact repeat of the newest (bash-style
// ignoredups). `nav` is -1 while the user edits a fresh line, else the 0-based number
// of steps back into history currently shown; `stash` keeps that fresh line so coming
// back down past the newest entry restores what was typed.
typedef struct {
    char line[TERM_HIST_MAX][TERM_INPUT_MAX];
    int  count;                    // live entries (0..TERM_HIST_MAX)
    int  head;                     // ring index the next push writes to
    int  nav;                      // -1 = not navigating; else steps back from newest
    char stash[TERM_INPUT_MAX];    // the in-progress line saved when recall began
    int  stash_len;
} term_hist_t;

typedef struct {
    char lines[TERM_LINES][TERM_COLS];
    uint8_t colors[TERM_LINES][TERM_COLS];
    int line_count;
    int scroll_offset;
    char input[TERM_INPUT_MAX];
    int input_len;
    int cursor_pos;
    char prompt[64];
    int prompt_len;
    void* cwd;              // this shell's current directory (opaque vfs node)
    char output_buf[TERM_OUTPUT_MAX];
    int output_len;
    int capturing;
    uint64_t session_id;
    int esc_state, esc_p[2], esc_np;
    int visible_rows;
    // Cursor-addressed "screen mode" for full-screen TUIs (edit, top). Entered when
    // a captured program emits a CSI cursor sequence (ESC[H / ESC[2J); the lines[]
    // grid is then treated as a fixed screen drawn from row 0, with a block cursor
    // at (out_row,out_col). Reset when the command finishes.
    int screen_mode;
    int out_row, out_col;
    // ANSI SGR color state (v5.8.26). cur_color is the VGA attribute byte applied
    // to newly written cells (fg = low nibble, bg = high nibble); out_color mirrors
    // output_buf so a scrollback line keeps per-char color (e.g. `ls --color`).
    uint8_t cur_color;
    uint8_t out_color[TERM_OUTPUT_MAX];
    term_hist_t hist;       // Up/Down command history (v6.4.326)
    // Ctrl+R reverse-incremental history search (v6.4.339). While active, the prompt shows
    // "(r-search)`query': " and the input line holds the current match; typing narrows it,
    // Ctrl+R jumps to the next older match, Enter accepts onto the line, Esc cancels.
    int  rsearch_active;
    char rsearch_q[TERM_INPUT_MAX];        // the incremental query
    int  rsearch_len;
    int  rsearch_idx;                      // 'back' index of the current match (-1 = none)
    char rsearch_saved[TERM_INPUT_MAX];    // input line before search (restored on Esc)
    int  rsearch_saved_len;
    char rsearch_prompt_save[64];          // real prompt text, restored when search ends
    int  rsearch_prompt_save_len;
} terminal_win_t;

#define TERM_SCREEN_ROWS 45    // rows cleared/addressable in screen mode (>= any window)
#define TERM_DEFAULT_COLOR (VGA_LIGHT_GREY | (VGA_BLACK << 4))   // light-grey on black

terminal_win_t* terminal_create_ctx(void);
void terminal_set_palette(const char* name);
const char* terminal_palette_name(void);
void terminal_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void terminal_win_key(window_t* win, int key);
int terminal_win_tick(window_t* win);
void terminal_win_close(window_t* win);
int terminal_capture_putchar(int c);
void terminal_capture_reset(terminal_win_t* t);   // leave screen mode when a cmd ends
int term_input_paste(char* input, int* input_len, int* cursor_pos, int cap,
                     const char* clip, int clip_len);   // Ctrl+V insert (bounded)
int term_paste_selftest(void);
int term_clear_selftest(void);

// Command-history ring core (pure, unit-tested by term_hist_selftest).
void term_hist_add(term_hist_t* h, const char* line);     // record a command (skip blank/dup)
const char* term_hist_get(const term_hist_t* h, int back); // back=0 newest; 0 ptr if out of range
int  term_hist_selftest(void);
// Reverse-i-search core (pure): 'back' index (0=newest) of the most recent entry at or older
// than start_back whose text contains `query` (empty query matches any), or -1 if none.
int  term_hist_rsearch(const term_hist_t* h, const char* query, int start_back);
int  term_rsearch_selftest(void);

#endif
