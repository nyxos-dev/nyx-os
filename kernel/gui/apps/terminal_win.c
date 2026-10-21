#include "../../core/kernel.h"
#include "../../core/ansi.h"
#include "../core/compositor.h"
#include "terminal_win.h"
#include "../../drivers/video/font.h"
#include "../../auth/login.h"

// ---- command-history ring (Up/Down recall, v6.4.326) ---------------------------------
// A small fixed ring of past command lines. term_hist_get/add are pure (no window state)
// so they can be unit-tested off-target by term_hist_selftest; the Up/Down key handling
// (term_hist_nav) layers the fresh-line stash on top. Newest entry is the last added.
static int th_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
const char* term_hist_get(const term_hist_t* h, int back) {
    if (!h || back < 0 || back >= h->count) return 0;     // 0..count-1 only; 0 ptr otherwise
    int idx = ((h->head - 1 - back) % TERM_HIST_MAX + TERM_HIST_MAX) % TERM_HIST_MAX;
    return h->line[idx];
}
void term_hist_add(term_hist_t* h, const char* line) {
    if (!h || !line || line[0] == '\0') return;           // never record a blank line
    const char* newest = term_hist_get(h, 0);
    if (newest && th_streq(newest, line)) return;         // ignore an exact repeat (ignoredups)
    char* dst = h->line[h->head];
    int i = 0;
    for (; line[i] && i < TERM_INPUT_MAX - 1; i++) dst[i] = line[i];
    dst[i] = '\0';
    h->head = (h->head + 1) % TERM_HIST_MAX;
    if (h->count < TERM_HIST_MAX) h->count++;             // ring saturates, never overflows
}
// KAT: newest-first ordering, blank/dup skipping, ring wrap-around eviction, range guards.
int term_hist_selftest(void) {
    static term_hist_t h;                 // static: term_hist_t is ~4 KB — too big for the kernel stack
    memset_asm(&h, 0, sizeof h); h.nav = -1;

    if (term_hist_get(&h, 0) != 0) return 1;              // empty ring recalls nothing
    term_hist_add(&h, "");                                 // a blank line is ignored
    if (h.count != 0) return 2;

    term_hist_add(&h, "one"); term_hist_add(&h, "two"); term_hist_add(&h, "three");
    if (h.count != 3) return 3;
    if (!th_streq(term_hist_get(&h, 0), "three")) return 4;   // back=0 is the newest
    if (!th_streq(term_hist_get(&h, 1), "two"))   return 5;
    if (!th_streq(term_hist_get(&h, 2), "one"))   return 6;
    if (term_hist_get(&h, 3)  != 0) return 7;                 // past the oldest
    if (term_hist_get(&h, -1) != 0) return 8;                 // negative guard

    term_hist_add(&h, "three");                              // exact repeat of newest -> skipped
    if (h.count != 3) return 9;
    term_hist_add(&h, "four");                               // a different line is recorded
    if (h.count != 4 || !th_streq(term_hist_get(&h, 0), "four")) return 10;

    static const char hx[] = "0123456789abcdef";             // overflow the ring with MAX distinct lines
    for (int i = 0; i < TERM_HIST_MAX; i++) { char b[4]; b[0]='h'; b[1]=hx[i]; b[2]='\0'; term_hist_add(&h, b); }
    if (h.count != TERM_HIST_MAX) return 11;                 // saturated at the cap, never beyond
    { char last[4];   last[0]='h';   last[1]=hx[TERM_HIST_MAX-1]; last[2]='\0';
      if (!th_streq(term_hist_get(&h, 0), last)) return 12; }              // newest = last pushed
    { char oldest[4]; oldest[0]='h'; oldest[1]=hx[0];            oldest[2]='\0';
      if (!th_streq(term_hist_get(&h, TERM_HIST_MAX - 1), oldest)) return 13; }  // oldest survivor
    if (term_hist_get(&h, TERM_HIST_MAX) != 0) return 14;    // exactly cap entries live
    return 0;
}

// True if `hay` contains `needle` as a substring (empty needle → true). Pure, local.
static int th_contains(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] == needle[k]) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

// Reverse incremental history search: the 'back' index (0 = newest) of the most recent entry
// at or older than start_back whose text contains `query`, or -1 if none. Empty query matches
// any entry. Walking start_back forward (older) gives "next older match". Pure — KAT-able.
int term_hist_rsearch(const term_hist_t* h, const char* query, int start_back) {
    if (!h || h->count == 0) return -1;
    if (start_back < 0) start_back = 0;
    for (int b = start_back; b < h->count; b++) {
        const char* e = term_hist_get(h, b);
        if (e && th_contains(e, query)) return b;
    }
    return -1;
}

// KAT: newest-first matching, next-older advance, no-match, empty query, substring, and the
// (find, then continue from idx+1) advance the Ctrl+R key path uses.
int term_rsearch_selftest(void) {
    static term_hist_t h;                 // static: ~4 KB, too big for the kernel stack
    memset_asm(&h, 0, sizeof h); h.nav = -1;
    term_hist_add(&h, "echo one");
    term_hist_add(&h, "ls -la");
    term_hist_add(&h, "echo two");
    term_hist_add(&h, "cat file");        // back: 0=cat file 1=echo two 2=ls -la 3=echo one
    if (term_hist_rsearch(&h, "echo", 0) != 1) return 1;   // newest 'echo' = "echo two" (back 1)
    if (term_hist_rsearch(&h, "echo", 2) != 3) return 2;   // next older from back 2 = "echo one" (back 3)
    if (term_hist_rsearch(&h, "echo", 4) != -1) return 3;  // past the oldest
    if (term_hist_rsearch(&h, "cat",  0) != 0) return 4;   // newest overall
    if (term_hist_rsearch(&h, "zzz",  0) != -1) return 5;  // no match anywhere
    if (term_hist_rsearch(&h, "",     0) != 0) return 6;   // empty query -> newest
    if (term_hist_rsearch(&h, "la",   0) != 2) return 7;   // substring inside "ls -la"
    if (term_hist_rsearch(&h, "echo", -3) != 1) return 8;  // negative start clamps to 0
    int b = term_hist_rsearch(&h, "echo", 0);              // repeated-Ctrl+R advance semantics
    if (term_hist_rsearch(&h, "echo", b + 1) != 3) return 9;
    return 0;
}

static void term_add_line(terminal_win_t* term, const char* text, uint8_t color) {
    if (term->line_count >= TERM_LINES) {
        for (int i = 1; i < TERM_LINES; i++) {
            memcpy(term->lines[i-1], term->lines[i], TERM_COLS);
            memcpy(term->colors[i-1], term->colors[i], TERM_COLS);
        }
        term->line_count = TERM_LINES - 1;
    }
    int idx = term->line_count;
memset_asm(term->lines[idx], ' ', TERM_COLS);
    for (int i = 0; i < TERM_COLS-1 && text[i]; i++)
        term->lines[idx][i] = text[i];
    memset_asm(term->colors[idx], color, TERM_COLS);
    term->line_count++;
    if (term->scroll_offset > 0) term->scroll_offset++;
}

static int terminal_nightfall;

void terminal_set_palette(const char* name) {
    if (!name) return;
    if (!strcmp(name, "classic")) terminal_nightfall = 0;
    else if (!strcmp(name, "nightfall")) terminal_nightfall = 1;
}

const char* terminal_palette_name(void) {
    return terminal_nightfall ? "nightfall" : "classic";
}

// Preserve VGA/ANSI indices in stored cells; only their displayed RGB changes.
static uint32_t vga_to_rgb(uint8_t idx) {
    static const uint8_t nightfall[16][3] = {
        {24, 24, 37}, {137, 180, 250}, {166, 227, 161}, {148, 226, 213},
        {243, 139, 168}, {203, 166, 247}, {249, 226, 175}, {205, 214, 244},
        {108, 112, 134}, {180, 208, 255}, {195, 245, 190}, {180, 245, 235},
        {255, 177, 195}, {226, 199, 255}, {255, 240, 205}, {239, 241, 255}
    };
    if (terminal_nightfall) {
        const uint8_t* rgb = nightfall[idx & 0x0F];
        return fb_rgb(rgb[0], rgb[1], rgb[2]);
    }
    switch (idx & 0x0F) {
        case 0:  return fb_rgb(0,0,0);
        case 1:  return fb_rgb(0,0,170);
        case 2:  return fb_rgb(0,170,0);
        case 3:  return fb_rgb(0,170,170);
        case 4:  return fb_rgb(170,0,0);
        case 5:  return fb_rgb(170,0,170);
        case 6:  return fb_rgb(170,85,0);
        case 7:  return fb_rgb(170,170,170);
        case 8:  return fb_rgb(85,85,85);
        case 9:  return fb_rgb(85,85,255);
        case 10: return fb_rgb(85,255,85);
        case 11: return fb_rgb(85,255,255);
        case 12: return fb_rgb(255,85,85);
        case 13: return fb_rgb(255,85,255);
        case 14: return fb_rgb(255,255,85);
        default: return fb_rgb(255,255,255);
    }
}

// ANSI SGR color -> VGA attribute nibble. Index by (ansi_code - 30):
//   blk  red  grn  yel  blu  mag  cyn  wht
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

// Apply the collected SGR params to the terminal's current color (fg = low nibble,
// bg = high nibble). 0 resets, 1 = bold (bright fg), 7 = reverse (swap fg/bg),
// 30-37/90-97 set fg, 40-47/100-107 set bg.
static void term_apply_sgr(terminal_win_t* t, const int* p, int n) {
    if (n <= 0) { t->cur_color = TERM_DEFAULT_COLOR; return; }
    for (int i = 0; i < n; i++) {
        int v = p[i];
        if (v == 0)                    t->cur_color = TERM_DEFAULT_COLOR;
        else if (v == 1)               t->cur_color |= 0x08;                                   // bold -> bright fg
        else if (v == 7)               t->cur_color = (uint8_t)(((t->cur_color & 0x0F) << 4) | ((t->cur_color >> 4) & 0x0F));
        else if (v >= 30 && v <= 37)   t->cur_color = (uint8_t)((t->cur_color & 0xF0) |  ansi2vga[v - 30]);
        else if (v >= 90 && v <= 97)   t->cur_color = (uint8_t)((t->cur_color & 0xF0) | (ansi2vga[v - 90] | 0x08));
        else if (v >= 40 && v <= 47)   t->cur_color = (uint8_t)((t->cur_color & 0x0F) | (ansi2vga[v - 40] << 4));
        else if (v >= 100 && v <= 107) t->cur_color = (uint8_t)((t->cur_color & 0x0F) | ((ansi2vga[v - 100] | 0x08) << 4));
    }
}

// Append a scrollback line carrying per-char colors (from the capture buffers),
// so a colorized command (e.g. `ls --color`) keeps its colors in history.
static void term_add_line_c(terminal_win_t* term, const char* text, const uint8_t* colorbuf, int len) {
    if (term->line_count >= TERM_LINES) {
        for (int i = 1; i < TERM_LINES; i++) {
            memcpy(term->lines[i-1], term->lines[i], TERM_COLS);
            memcpy(term->colors[i-1], term->colors[i], TERM_COLS);
        }
        term->line_count = TERM_LINES - 1;
    }
    int idx = term->line_count;
    memset_asm(term->lines[idx], ' ', TERM_COLS);
    memset_asm(term->colors[idx], TERM_DEFAULT_COLOR, TERM_COLS);
    for (int i = 0; i < TERM_COLS - 1 && i < len; i++) {
        term->lines[idx][i] = text[i];
        term->colors[idx][i] = colorbuf[i];
    }
    term->line_count++;
    if (term->scroll_offset > 0) term->scroll_offset++;
}

// Rebuild this terminal's prompt from its own current directory. Activating the
// shell's CWD first means the prompt shows the real absolute path (e.g. after a
// `cd`) and each Terminal window tracks its directory independently.
static void term_set_prompt(terminal_win_t* term) {
    vfs_setcwd_node(term->cwd);
    snprintf(term->prompt, sizeof(term->prompt), "nyx:%s$ ", vfs_getcwd());
    term->prompt_len = strlen(term->prompt);
}

terminal_win_t* terminal_create_ctx(void) {
    terminal_win_t* term = (terminal_win_t*)kmalloc(sizeof(terminal_win_t));
    if (!term) return NULL;
    memset_asm(term, 0, sizeof(terminal_win_t));
    term->session_id = console_session_open();
    if (!term->session_id) { kfree(term); return NULL; }
    term->hist.nav = -1;                  // start editing a fresh line, not navigating history
    term->cwd = login_home_node();        // each terminal starts in the user's home dir
    term_set_prompt(term);
    term->visible_rows = 20;
    char banner[48];
    snprintf(banner, sizeof(banner), "%s Terminal v%s", TERMINAL_NAME, TERMINAL_VERSION);
    term_add_line(term, banner, VGA_LIGHT_GREEN | (VGA_BLACK << 4));
    term_add_line(term, "Type 'help' for available commands.", VGA_LIGHT_CYAN | (VGA_BLACK << 4));
    term_add_line(term, "", VGA_LIGHT_GREY | (VGA_BLACK << 4));
    return term;
}

static terminal_win_t* capture_term = NULL;

// ANSI/CSI escape parser for captured TUI output. States: 0 normal, 1 saw ESC,
// 2 inside a CSI collecting numeric params. Recognized finals:
//   H / f  cursor position ESC[row;colH (1-based; default 1,1)  -> screen mode
//   J      ESC[2J clear the screen                              -> screen mode
//   K      ESC[K clear from the cursor to end of line
// The first cursor/clear sequence flips the terminal into screen_mode, where the
// lines[] grid becomes a fixed cell screen addressed by (out_row,out_col) with a
// drawn block cursor — this is what a full-screen editor draws onto.

// Enter/refresh screen mode: blank the addressable grid, home the output cursor.
static void term_screen_clear(terminal_win_t* t) {
    for (int r = 0; r < TERM_SCREEN_ROWS; r++) {
        memset_asm(t->lines[r], ' ', TERM_COLS);
        memset_asm(t->colors[r], VGA_LIGHT_GREY | (VGA_BLACK << 4), TERM_COLS);
    }
    t->line_count = TERM_SCREEN_ROWS;
    t->scroll_offset = 0;
    t->output_len = 0;
    t->out_row = t->out_col = 0;
    t->screen_mode = 1;
}

// Reset back to scrollback mode (called when a command finishes).
void terminal_capture_reset(terminal_win_t* t) {
    if (t) { t->esc_state = 0; t->cur_color = TERM_DEFAULT_COLOR; if (t->screen_mode) { t->screen_mode = 0; t->line_count = 0; } }
}

static int terminal_output_putchar(terminal_win_t* t, int c) {

    if (t->esc_state == 1) {                        // saw ESC: a CSI opens with '['
        if (c == '[') { t->esc_state = 2; t->esc_p[0] = t->esc_p[1] = 0; t->esc_np = 0; }
        else t->esc_state = 0;
        return c;
    }
    if (t->esc_state == 2) {                         // inside CSI
        if (c >= '0' && c <= '9') {               // accumulate a numeric param (saturating)
            int i = t->esc_np < 2 ? t->esc_np : 1;
            t->esc_p[i] = csi_param_accum(t->esc_p[i], c - '0');
            return c;
        }
        if (c == ';') { if (t->esc_np < 2) t->esc_np++; return c; }
        if (c == 'H' || c == 'f') {               // cursor position (1-based -> 0-based)
            if (!t->screen_mode) term_screen_clear(t);
            t->out_row = t->esc_p[0] > 0 ? t->esc_p[0] - 1 : 0;
            t->out_col = t->esc_p[1] > 0 ? t->esc_p[1] - 1 : 0;
        } else if (c == 'J') {                    // erase display
            if (t->esc_p[0] == 3) {                  // ESC[3J: wipe scrollback, return to the prompt
                t->line_count = 0;                    // (userspace `clear` — composes in the shell)
                t->scroll_offset = 0;
                t->output_len = 0;
                t->out_row = t->out_col = 0;
                t->screen_mode = 0;
            } else {                                   // ESC[2J / ESC[J: full-screen clear (TUI)
                term_screen_clear(t);
            }
        } else if (c == 'K') {                    // clear to end of line
            if (t->screen_mode && t->out_row >= 0 && t->out_row < TERM_SCREEN_ROWS)
                for (int x = t->out_col; x >= 0 && x < TERM_COLS; x++) {
                    t->lines[t->out_row][x] = ' ';
                    t->colors[t->out_row][x] = t->cur_color;
                }
        } else if (c == 'm') {                    // SGR: set color (fg/bg)
            term_apply_sgr(t, t->esc_p, (t->esc_np + 1 > 2) ? 2 : t->esc_np + 1);
        }
        t->esc_state = 0;                            // any other final is swallowed
        return c;
    }
    if (c == 0x1B) { t->esc_state = 1; return c; }   // ESC begins a sequence

    if (t->screen_mode) {                         // cursor-addressed cell writes
        if (c == '\n') { t->out_row++; t->out_col = 0; }
        else if (c == '\r') { t->out_col = 0; }
        else if (c == '\b' || c == 0x7F) { if (t->out_col > 0) t->out_col--; }
        else if (c >= 0x20) {
            if (t->out_row < 0) t->out_row = 0;
            if (t->out_row >= TERM_SCREEN_ROWS) t->out_row = TERM_SCREEN_ROWS - 1;
            if (t->out_col >= 0 && t->out_col < TERM_COLS) {
                t->lines[t->out_row][t->out_col] = (char)c;
                t->colors[t->out_row][t->out_col] = t->cur_color;
                t->out_col++;
            }
        }
        return c;
    }

    // Normal (scrollback) mode.
    if (c == '\n') {
        t->output_buf[t->output_len] = '\0';
        if (t->output_len > 0)
            term_add_line_c(t, t->output_buf, t->out_color, t->output_len);
        t->output_len = 0;
    } else if (c == '\b' || c == 0x7F) {
        // Destructive backspace on the pending line — makes the kernel echo's
        // "\b \b" and the shell line editor's redraws render correctly.
        if (t->output_len > 0) t->output_len--;
    } else if (c == '\r') {
        t->output_len = 0;                        // carriage return: rebuild the line
    } else if (c >= 0x20 && t->output_len < TERM_OUTPUT_MAX - 1) {
        t->out_color[t->output_len] = t->cur_color;   // per-char color (ls --color)
        t->output_buf[t->output_len++] = c;
    }
    return c;
}
int terminal_capture_putchar(int c) {
    if (!capture_term || !capture_term->capturing) return c;
    return terminal_output_putchar(capture_term, c);
}

static void terminal_flush_pending(terminal_win_t* t) {
    if (t->output_len && !t->screen_mode) {
        term_add_line_c(t, t->output_buf, t->out_color, t->output_len);
        t->output_len = 0;
    }
}

int terminal_win_tick(window_t* win) {
    terminal_win_t* t = win ? (terminal_win_t*)win->reserved : NULL;
    if (!t || t->capturing || !t->session_id) return 0;
    char pending[512];
    uint32_t omitted;
    int n = console_session_read(t->session_id, pending, sizeof(pending), &omitted);
    for (int i = 0; i < n; i++) terminal_output_putchar(t, (unsigned char)pending[i]);
    if (omitted) {
        terminal_capture_reset(t);
        terminal_flush_pending(t);
        char message[80];
        snprintf(message, sizeof(message), "[terminal: %u bytes omitted; full output on serial]", omitted);
        term_add_line(t, message, VGA_LIGHT_RED | (VGA_BLACK << 4));
    }
    return n || omitted;
}

void terminal_win_close(window_t* win) {
    terminal_win_t* t = win ? (terminal_win_t*)win->reserved : NULL;
    if (t) { console_session_close(t->session_id); t->session_id = 0; }
}

int term_clear_selftest(void) {
    terminal_win_t* t = (terminal_win_t*)kmalloc(sizeof(*t));
    if (!t) return 1;
    memset_asm(t, 0, sizeof(*t));
    terminal_win_t* saved = capture_term;
    capture_term = t;
    t->esc_state = 0;
    t->capturing = 1;
    t->cur_color = TERM_DEFAULT_COLOR;
    t->input[0] = 'x';
    t->input_len = 1;
    term_hist_add(&t->hist, "clear");
    const char* text = "old line\npending" TERM_CLEAR_SEQUENCE;
    for (const char* p = text; *p; p++) terminal_capture_putchar(*p);
    int result = 0;
    if (t->line_count || t->output_len || t->scroll_offset || t->screen_mode) result = 2;
    text = "new\n";
    for (const char* p = text; *p; p++) terminal_capture_putchar(*p);
    if (t->line_count != 1 || strncmp(t->lines[0], "new", 3) != 0 || t->lines[0][3] != ' ') result = 3;
    if (t->input_len != 1 || t->input[0] != 'x' || t->hist.count != 1) result = 4;
    capture_term = saved;
    kfree(t);
    return result;
}

void terminal_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    terminal_win_t* term = (terminal_win_t*)win->reserved;
    if (!term) return;
    terminal_win_tick(win);

    uint32_t canvas = vga_to_rgb(VGA_BLACK);
    uint32_t input_fg = terminal_nightfall ? vga_to_rgb(VGA_LIGHT_GREEN) : fb_rgb(0,255,0);
    fb_fill_rect(cx, cy, cw, ch, canvas);

    uint32_t char_w = FONT_WIDTH;
    uint32_t char_h = FONT_HEIGHT;
    int cols = cw / char_w;
    if (cols > TERM_COLS) cols = TERM_COLS;
    int rows = ch / char_h;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    term->visible_rows = rows;

    // Screen mode (full-screen TUI): the lines[] grid IS the screen, drawn across
    // all `rows`. Normal mode RESERVES the last visible row for the live input line,
    // so the scrollback fills the `rows - 1` rows above it — and start_line is
    // derived from that same count so the NEWEST scrollback line lands just above
    // the input. (Before, the loop drew all `rows` lines, so the input row painted
    // over the newest output line — the "terminal overwrites itself" bug — and
    // start_line's off-by-one hid a second line above it.)
    int sb_rows = term->screen_mode ? rows : rows - 1;
    if (sb_rows < 1) sb_rows = 1;
    int total_lines = term->line_count + (!term->screen_mode && term->output_len > 0);
    int start_line = term->screen_mode ? 0
                     : total_lines - sb_rows - term->scroll_offset;
    if (start_line < 0) start_line = 0;

    for (int r = 0; r < sb_rows && start_line + r < total_lines; r++) {
        int li = start_line + r;
        int y = cy + r * char_h;
        for (int c = 0; c < cols; c++) {
            int pending_line = li == term->line_count;
            char ch_char = pending_line ? (c < term->output_len ? term->output_buf[c] : ' ') : term->lines[li][c];
            if (!ch_char) ch_char = ' ';
            uint8_t col = pending_line ? (c < term->output_len ? term->out_color[c] : TERM_DEFAULT_COLOR) : term->colors[li][c];
            uint32_t fg = vga_to_rgb(col & 0x0F);
            uint32_t bg = vga_to_rgb((col >> 4) & 0x0F);   // background from the high nibble
            // Block cursor in screen mode: invert the cell at (out_row,out_col).
            if (term->screen_mode && li == term->out_row && c == term->out_col)
                font_draw_char(cx + c * char_w, y, ch_char, bg, fg);
            else
                font_draw_char(cx + c * char_w, y, ch_char, fg, bg);
        }
    }

    int input_row = rows - 1;
    if (!term->screen_mode && input_row >= 0) {
        int y = cy + input_row * char_h;
        char full_line[TERM_COLS + TERM_INPUT_MAX];
        int fl = 0;
        for (int i = 0; i < term->prompt_len && fl < TERM_COLS + TERM_INPUT_MAX - 1; i++)
            full_line[fl++] = term->prompt[i];
        for (int i = 0; i < term->input_len && fl < TERM_COLS + TERM_INPUT_MAX - 1; i++)
            full_line[fl++] = term->input[i];
        if (term->input_len < term->cursor_pos) term->cursor_pos = term->input_len;
        int cursor_draw = term->cursor_pos + term->prompt_len;
        full_line[fl] = '\0';

        for (int c = 0; c < cols && full_line[c]; c++) {
            uint32_t fg = (c == cursor_draw) ? canvas : input_fg;
            uint32_t bg = (c == cursor_draw) ? input_fg : canvas;
            font_draw_char(cx + c * char_w, y, full_line[c], fg, bg);
        }
    }

    // Scrollbar on the right edge when there's more history than fits (skip in TUI
    // screen mode). Thumb height ~ visible/total; its position tracks how far up the
    // view is scrolled, and it turns purple while scrolled off the live tail.
    if (!term->screen_mode && term->line_count > rows) {
        int total = term->line_count;
        int bx = cx + cw - 3;
        fb_fill_rect(bx, cy, 3, ch, fb_rgb(35, 35, 45));            // track
        int th = (int)((uint64_t)ch * rows / total);
        if (th < 10) th = 10;
        if (th > (int)ch) th = ch;
        int max_start = total - rows;
        if (max_start < 1) max_start = 1;
        int start = start_line < 0 ? 0 : (start_line > max_start ? max_start : start_line);
        int ty = cy + (int)((uint64_t)(ch - th) * start / max_start);
        uint32_t thumb = term->scroll_offset > 0 ? fb_rgb(150, 130, 220)   // scrolled: purple
                                                 : fb_rgb(90, 90, 110);     // at the tail: dim
        fb_fill_rect(bx, ty, 3, th, thumb);
    }
}

// Scroll the scrollback VIEW by `delta` lines (positive = toward older output). No-op
// in TUI screen mode. Clamped so you can't scroll below the live tail (offset 0) or
// past the oldest kept line.
static void term_scroll(terminal_win_t* t, int delta) {
    if (t->screen_mode) return;
    int max_off = t->line_count - t->visible_rows;
    if (max_off < 0) max_off = 0;
    t->scroll_offset += delta;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    if (t->scroll_offset > max_off) t->scroll_offset = max_off;
}

// ---------------------------------------------------------------------------
// Clipboard cut/paste for the input line (v6.4.245). Ctrl+X cuts the whole
// input line to the system clipboard; Ctrl+V pastes the clipboard back in at
// the cursor. term_input_paste is the pure, bounded insert Ctrl+V uses (and the
// term-paste KAT exercises): it drops non-printable bytes — a pasted '\n' must
// not submit or corrupt the line — and never grows the line past `cap`.
int term_input_paste(char* input, int* input_len, int* cursor_pos, int cap,
                     const char* clip, int clip_len) {
    int inserted = 0;
    for (int i = 0; i < clip_len; i++) {
        unsigned char uch = (unsigned char)clip[i];
        if (uch < 0x20 || uch >= 0x7F) continue;   // skip control / non-ASCII bytes
        if (*input_len >= cap - 1) break;          // input line is full
        for (int j = *input_len; j > *cursor_pos; j--)
            input[j] = input[j - 1];
        input[*cursor_pos] = (char)uch;
        (*input_len)++;
        (*cursor_pos)++;
        inserted++;
    }
    return inserted;
}

int term_paste_selftest(void) {
    char buf[TERM_INPUT_MAX];
    int len, cur;

    // 1) paste into an empty line
    buf[0] = '\0'; len = 0; cur = 0;
    if (term_input_paste(buf, &len, &cur, TERM_INPUT_MAX, "echo hi", 7) != 7) return 1;
    if (len != 7 || cur != 7) return 2;
    buf[len] = '\0';
    if (strcmp(buf, "echo hi") != 0) return 3;

    // 2) paste at a mid-line cursor (right after "echo")
    cur = 4;
    if (term_input_paste(buf, &len, &cur, TERM_INPUT_MAX, "X", 1) != 1) return 4;
    buf[len] = '\0';
    if (len != 8 || cur != 5 || strcmp(buf, "echoX hi") != 0) return 5;

    // 3) non-printable bytes are dropped (a pasted tab/newline never enters the line)
    len = 0; cur = 0; buf[0] = '\0';
    if (term_input_paste(buf, &len, &cur, TERM_INPUT_MAX, "a\tb\nc", 5) != 3) return 6;
    buf[len] = '\0';
    if (len != 3 || strcmp(buf, "abc") != 0) return 7;

    // 4) truncation at cap: a source longer than the line fills to TERM_INPUT_MAX-1
    len = 0; cur = 0;
    char big[TERM_INPUT_MAX];
    for (int i = 0; i < TERM_INPUT_MAX; i++) big[i] = 'z';
    if (term_input_paste(buf, &len, &cur, TERM_INPUT_MAX, big, TERM_INPUT_MAX)
            != TERM_INPUT_MAX - 1) return 8;
    if (len != TERM_INPUT_MAX - 1 || cur != TERM_INPUT_MAX - 1) return 9;
    if (term_input_paste(buf, &len, &cur, TERM_INPUT_MAX, "q", 1) != 0) return 10;   // full: refuses more

    return 0;
}

// Readline-style line kills (v6.5.21): Ctrl+U cuts the text BEFORE the cursor, Ctrl+K the text
// AFTER it, each to the system clipboard. Pure + bounded so the term-kill KAT drives them:
// they copy the removed run into `cut` (NUL-terminated, capped at cap-1), update the line, and
// return how many chars were removed. Ctrl+U slides the surviving tail down to index 0 and
// homes the cursor; Ctrl+K just truncates at the cursor.
int term_kill_to_start(char* input, int* input_len, int* cursor_pos, char* cut, int cap) {
    int span = *cursor_pos;                          // chars before the cursor -> removed
    int n = (span < cap) ? span : cap - 1;           // chars copied into cut (clamped)
    for (int i = 0; i < n; i++) cut[i] = input[i];
    cut[n] = '\0';
    int tail = *input_len - span;                    // slide [cursor,len) down to the start
    for (int i = 0; i < tail; i++) input[i] = input[span + i];
    *input_len = tail;
    *cursor_pos = 0;
    return span;
}
int term_kill_to_end(char* input, int* input_len, int cursor_pos, char* cut, int cap) {
    int span = *input_len - cursor_pos;              // chars from the cursor to EOL -> removed
    if (span < 0) span = 0;
    int n = (span < cap) ? span : cap - 1;
    for (int i = 0; i < n; i++) cut[i] = input[cursor_pos + i];
    cut[n] = '\0';
    *input_len = cursor_pos;
    return span;
}

int term_kill_selftest(void) {
    char buf[TERM_INPUT_MAX], cut[TERM_INPUT_MAX];
    int len, cur, n;
    // Ctrl+K: kill from mid-line to EOL -> line keeps the prefix, cut holds the suffix
    strcpy(buf, "hello world"); len = 11; cur = 6;
    n = term_kill_to_end(buf, &len, cur, cut, sizeof cut); buf[len] = '\0';
    if (n != 5 || len != 6 || strcmp(cut, "world") || strcmp(buf, "hello ")) return 1;
    // Ctrl+U: kill from mid-line to start -> tail slides down, cursor homes, cut holds the prefix
    strcpy(buf, "hello world"); len = 11; cur = 6;
    n = term_kill_to_start(buf, &len, &cur, cut, sizeof cut); buf[len] = '\0';
    if (n != 6 || len != 5 || cur != 0 || strcmp(cut, "hello ") || strcmp(buf, "world")) return 2;
    // Ctrl+U at column 0 -> no-op, empty cut
    strcpy(buf, "abc"); len = 3; cur = 0;
    n = term_kill_to_start(buf, &len, &cur, cut, sizeof cut); buf[len] = '\0';
    if (n != 0 || len != 3 || cur != 0 || cut[0] || strcmp(buf, "abc")) return 3;
    // Ctrl+K at EOL -> no-op, empty cut
    strcpy(buf, "abc"); len = 3; cur = 3;
    n = term_kill_to_end(buf, &len, cur, cut, sizeof cut);
    if (n != 0 || len != 3 || cut[0]) return 4;
    // Ctrl+U with the cursor at EOL kills the whole line
    strcpy(buf, "full"); len = 4; cur = 4;
    n = term_kill_to_start(buf, &len, &cur, cut, sizeof cut);
    if (n != 4 || len != 0 || cur != 0 || strcmp(cut, "full")) return 5;
    return 0;
}

// Replace the input line with `s` (bounded) and park the cursor at its end.
static void term_load_input(terminal_win_t* t, const char* s, int len) {
    if (len > TERM_INPUT_MAX - 1) len = TERM_INPUT_MAX - 1;
    for (int i = 0; i < len; i++) t->input[i] = s[i];
    t->input_len  = len;
    t->cursor_pos = len;
}

// Up/Down history recall. dir = +1 walks OLDER (Up), -1 walks NEWER (Down). The fresh
// line the user was typing is stashed the moment recall begins and restored when they
// come back down past the newest entry (nav returns to -1).
static void term_hist_nav(terminal_win_t* t, int dir) {
    term_hist_t* h = &t->hist;
    if (h->count == 0) return;                       // nothing recorded yet
    int nn = h->nav + dir;
    if (nn < -1) nn = -1;                            // no newer than the fresh line
    if (nn > h->count - 1) nn = h->count - 1;        // clamp at the oldest entry
    if (nn == h->nav) return;                         // already at that edge — no change
    if (h->nav == -1) {                               // leaving the fresh line: save it
        int n = t->input_len; if (n > TERM_INPUT_MAX - 1) n = TERM_INPUT_MAX - 1;
        for (int i = 0; i < n; i++) h->stash[i] = t->input[i];
        h->stash_len = n;
    }
    h->nav = nn;
    if (nn == -1) { term_load_input(t, h->stash, h->stash_len); return; }   // back to fresh line
    const char* e = term_hist_get(h, nn);
    if (e) { int L = 0; while (e[L]) L++; term_load_input(t, e, L); }
}

// --- Ctrl+R reverse-incremental history search (v6.4.339) -----------------------------
// Recompute the match for the current query, searching older from `from_back`, and reflect
// it on the input line + the search prompt. A failing search keeps the last match shown.
static void term_rsearch_apply(terminal_win_t* t, int from_back) {
    int b = term_hist_rsearch(&t->hist, t->rsearch_q, from_back);
    if (b >= 0) {
        t->rsearch_idx = b;
        const char* e = term_hist_get(&t->hist, b);
        if (e) { int L = 0; while (e[L]) L++; term_load_input(t, e, L); }
    }
    snprintf(t->prompt, sizeof(t->prompt), "%s(r-search)`%s': ",
             b < 0 ? "failing " : "", t->rsearch_q);
    t->prompt_len = (int)strlen(t->prompt);
}

// Enter reverse-i-search: stash the input line + prompt, then show the (empty-query) search.
static void term_rsearch_begin(terminal_win_t* t) {
    t->rsearch_active = 1;
    t->rsearch_len = 0; t->rsearch_q[0] = '\0';
    t->rsearch_idx = -1;
    int n = t->input_len; if (n > TERM_INPUT_MAX - 1) n = TERM_INPUT_MAX - 1;
    for (int i = 0; i < n; i++) t->rsearch_saved[i] = t->input[i];
    t->rsearch_saved_len = n;
    int pn = t->prompt_len; if (pn > 63) pn = 63;
    for (int i = 0; i < pn; i++) t->rsearch_prompt_save[i] = t->prompt[i];
    t->rsearch_prompt_save[pn] = '\0';
    t->rsearch_prompt_save_len = pn;
    term_rsearch_apply(t, 0);
}

// Leave search: restore the real prompt; on cancel also restore the pre-search input line.
static void term_rsearch_end(terminal_win_t* t, int accept) {
    for (int i = 0; i < t->rsearch_prompt_save_len; i++) t->prompt[i] = t->rsearch_prompt_save[i];
    t->prompt[t->rsearch_prompt_save_len] = '\0';
    t->prompt_len = t->rsearch_prompt_save_len;
    t->rsearch_active = 0;
    if (!accept) term_load_input(t, t->rsearch_saved, t->rsearch_saved_len);
    // accept: the matched command is already on the input line — a subsequent Enter runs it.
}

// Every key while searching flows through here.
static void term_rsearch_key(terminal_win_t* t, int key) {
    if (key == 0x1B) { term_rsearch_end(t, 0); return; }                  // Esc  — cancel
    if (key == '\r' || key == '\n') { term_rsearch_end(t, 1); return; }   // Enter — accept onto the line
    if (key == 0x12) {                                                    // Ctrl+R — next older match
        term_rsearch_apply(t, (t->rsearch_idx < 0 ? 0 : t->rsearch_idx + 1));
        return;
    }
    if (key == '\b' || key == 0x7F) {                                     // edit the query
        if (t->rsearch_len > 0) t->rsearch_q[--t->rsearch_len] = '\0';
        term_rsearch_apply(t, 0);
        return;
    }
    if (key >= 0x20 && key <= 0x7E && t->rsearch_len < TERM_INPUT_MAX - 1) {
        t->rsearch_q[t->rsearch_len++] = (char)key;
        t->rsearch_q[t->rsearch_len] = '\0';
        term_rsearch_apply(t, 0);                                         // narrow from the newest
    }
    // swallow everything else while searching
}

void terminal_win_key(window_t* win, int key) {
    terminal_win_t* term = (terminal_win_t*)win->reserved;
    if (!term) return;

    // Ctrl+R reverse-i-search owns every key while active.
    if (term->rsearch_active) { term_rsearch_key(term, key); return; }

    // Scrollback navigation — moves the VIEW only (not the input line) and never snaps
    // back to the tail. A page is one screenful minus a row of overlap; a wheel notch
    // is a few lines. PgUp/PgDn come from the keyboard, WHEEL_* from the compositor.
    int page = term->visible_rows > 3 ? term->visible_rows - 2 : 1;
    switch (key) {
        case KEY_PGUP:       term_scroll(term,  page); return;
        case KEY_PGDN:       term_scroll(term, -page); return;
        case KEY_WHEEL_UP:   term_scroll(term,  3);    return;
        case KEY_WHEEL_DOWN: term_scroll(term, -3);    return;
    }

    // Any other key snaps back to the live tail — typing/Enter always jumps to bottom.
    term->scroll_offset = 0;

    char c = (char)(key < 0x80 ? key : 0);

    // Extended keycodes (arrows, etc.)
    if (key >= 0x80) {
        switch (key) {
            case KEY_UP:                 // recall an older command
                term_hist_nav(term, +1);
                break;
            case KEY_DOWN:               // walk back toward the fresh line
                term_hist_nav(term, -1);
                break;
            case KEY_LEFT:
                if (term->cursor_pos > 0) term->cursor_pos--;
                break;
            case KEY_RIGHT:
                if (term->cursor_pos < term->input_len) term->cursor_pos++;
                break;
            case KEY_HOME:
                term->cursor_pos = 0;
                break;
            case KEY_END:
                term->cursor_pos = term->input_len;
                break;
            case KEY_DEL:
                if (term->cursor_pos < term->input_len) {
                    for (int i = term->cursor_pos; i < term->input_len - 1; i++)
                        term->input[i] = term->input[i + 1];
                    term->input_len--;
                }
                break;
        }
        return;
    }

    if (c == 0x12) {                    // Ctrl+R — enter reverse-i-search
        term_rsearch_begin(term);
        return;
    }

    if (c == 0x03) {                    // no foreground job: cancel the input line
        term->input_len = 0;
        term->cursor_pos = 0;
        term->input[0] = 0;
        return;
    }

    // Foreground Ctrl+C/Z remain signals; idle terminal cut/paste uses X/V.
    if (c == 0x18) {                    // Ctrl+X — cut the whole input line
        if (term->input_len > 0) {
            clipboard_set(term->input, (uint32_t)term->input_len);
            term->input_len = 0;
            term->cursor_pos = 0;
        }
        return;
    }
    if (c == 0x16) {                    // Ctrl+V — paste the clipboard at the cursor
        char cb[TERM_INPUT_MAX];
        uint32_t n = clipboard_get(cb, sizeof(cb));
        term_input_paste(term->input, &term->input_len, &term->cursor_pos,
                         TERM_INPUT_MAX, cb, (int)n);
        return;
    }

    // Readline line-editing chords: motion (Ctrl+A/E) + kill-to-clipboard (Ctrl+U/K).
    if (c == 0x01) { term->cursor_pos = 0; return; }                  // Ctrl+A — start of line
    if (c == 0x05) { term->cursor_pos = term->input_len; return; }    // Ctrl+E — end of line
    if (c == 0x15) {                                                  // Ctrl+U — kill to line start
        char cut[TERM_INPUT_MAX];
        int n = term_kill_to_start(term->input, &term->input_len, &term->cursor_pos, cut, sizeof cut);
        if (n > 0) clipboard_set(cut, (uint32_t)n);
        return;
    }
    if (c == 0x0B) {                                                  // Ctrl+K — kill to line end
        char cut[TERM_INPUT_MAX];
        int n = term_kill_to_end(term->input, &term->input_len, term->cursor_pos, cut, sizeof cut);
        if (n > 0) clipboard_set(cut, (uint32_t)n);
        return;
    }

    if (c == '\b' || c == 0x7F) {
        if (term->cursor_pos > 0) {
            for (int i = term->cursor_pos - 1; i < term->input_len - 1; i++)
                term->input[i] = term->input[i + 1];
            term->input_len--;
            term->cursor_pos--;
        }
        return;
    }

    if (c == '\t') {
        if (term->input_len > 0) {
            int space_pos = -1;
            for (int i = 0; i < term->input_len; i++) {
                if (term->input[i] == ' ') { space_pos = i; break; }
            }
            if (space_pos < 0) {
                char completed[64];
                int match_count = 0;
                command_complete(term->input, completed, sizeof(completed), &match_count);
                if (match_count == 1) {
                    int clen = strlen(completed);
                    term->input_len = clen;
                    memcpy(term->input, completed, clen);
                    term->cursor_pos = clen;
                } else if (match_count > 1) {
                    char matches[256];
                    command_list_matches(term->input, matches, sizeof(matches));
                    term_add_line(term, matches, VGA_LIGHT_CYAN | (VGA_BLACK << 4));
                    if (strlen(completed) > (size_t)term->input_len) {
                        int clen = strlen(completed);
                        term->input_len = clen;
                        memcpy(term->input, completed, clen);
                        term->cursor_pos = clen;
                    }
                }
            }
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        terminal_win_tick(win);
        terminal_flush_pending(term);
        terminal_capture_reset(term);
        term->input[term->input_len] = '\0';
        char full_line[TERM_COLS + TERM_INPUT_MAX];
        int fl = 0;
        for (int i = 0; i < term->prompt_len; i++)
            full_line[fl++] = term->prompt[i];
        for (int i = 0; i < term->input_len; i++)
            full_line[fl++] = term->input[i];
        full_line[fl] = '\0';
        term_add_line(term, full_line, VGA_LIGHT_GREEN | (VGA_BLACK << 4));

        term_hist_add(&term->hist, term->input);    // record for Up/Down recall (skips blank + dup)
        term->hist.nav = -1;                         // Enter ends any history navigation

        if (term->input_len > 0) {
            capture_term = term;
            term->capturing = 1;
            term->output_len = 0;
            term->cur_color = TERM_DEFAULT_COLOR;   // fresh color state per command
            vfs_setcwd_node(term->cwd);       // run in THIS shell's directory
            set_putchar_hook_session(terminal_capture_putchar, term->session_id);
            execute_command(term->input);
            set_putchar_hook(NULL);
            term->capturing = 0;
            int was_screen = term->screen_mode;
            if (term->output_len > 0 && !was_screen) {   // flush a trailing (unterminated) line
                term->output_buf[term->output_len] = '\0';
                term_add_line_c(term, term->output_buf, term->out_color, term->output_len);
                term->output_len = 0;
            }
            terminal_capture_reset(term);      // leave TUI screen mode -> clean scrollback
            term->cwd = vfs_getcwd_node();     // a `cd` may have moved us
            term_set_prompt(term);             // reflect the new path in the prompt
            capture_term = NULL;
        }

        term->input_len = 0;
        term->cursor_pos = 0;
        return;
    }

    if (c >= 0x20 && c < 0x7F) {
        if (term->input_len < TERM_INPUT_MAX - 1) {
            for (int i = term->input_len; i > term->cursor_pos; i--)
                term->input[i] = term->input[i - 1];
            term->input[term->cursor_pos] = c;
            term->input_len++;
            term->cursor_pos++;
        }
    }
}
