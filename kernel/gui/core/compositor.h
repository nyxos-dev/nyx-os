#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "../../core/kernel.h"

#define MAX_WINDOWS        32
#define TITLE_H            22

/* THE client-area origin. A window's (x, y) is the top-left of its TITLE BAR,
 * and its content starts TITLE_H below that. The draw callbacks are handed the
 * client origin already computed, but every hit-test handler worked it out for
 * itself from win->y — and simply forgot the title bar. That is why clicks in
 * Paint, the Text Editor and Sound Test all landed exactly TITLE_H = 22 px above
 * what was drawn: four separate symptoms of one missing term.
 *
 * Exported here so there is ONE definition of where a window's content begins,
 * rather than each module re-deriving it and being right or wrong on its own. */
#define WIN_CLIENT_X(w) ((int)(w)->x)
#define WIN_CLIENT_Y(w) ((int)(w)->y + TITLE_H)
#define CLOSE_W            16
#define MAX_TITLE          48
#define MIN_WIN_W          120
#define MIN_WIN_H          80
#define RESIZE_MARGIN      6
#define WORKSPACE_COUNT    4
#define TASKBAR_H          36
#define START_W            160
#define START_H            428   /* 14 entries: START_ITEM_Y + 14*START_ITEM_H + pad */
#define CLOCK_W            160   /* fits "Www HH:MM  DD/MM" (24h) and the wider "Www II:MM PP  DD/MM" (12h nyx.conf clock) */

enum {
    RESIZE_NONE,
    RESIZE_RIGHT,
    RESIZE_BOTTOM,
    RESIZE_CORNER,
    RESIZE_LEFT,
    RESIZE_TOP,
    RESIZE_LEFT_TOP,
    RESIZE_RIGHT_TOP,
    RESIZE_LEFT_BOTTOM,
};

enum {
    WSTATE_NORMAL,
    WSTATE_MINIMIZED,
    WSTATE_MAXIMIZED,
    WSTATE_SNAP_LEFT,   // tiled to the left half of the usable area
    WSTATE_SNAP_RIGHT,  // tiled to the right half
    WSTATE_SNAP_TL,     // top-left quarter
    WSTATE_SNAP_TR,     // top-right quarter
    WSTATE_SNAP_BL,     // bottom-left quarter
    WSTATE_SNAP_BR,     // bottom-right quarter
};

// Cursor shape the pointer takes over a window's CLIENT area. Default 0 = arrow;
// text windows (Terminal, Editor) set CURSOR_IBEAM so the pointer becomes an I-beam
// over their content, the way every desktop signals "you can select/place text here".
// The RESIZE_* shapes are picked live by pick_cursor_shape() when the pointer is over a
// window's resize border (see resize_hit): a directional double-arrow that shows which
// way a drag would grow the window — H over left/right edges, V over top/bottom,
// NWSE (\) over the ↖/↘ corners, NESW (/) over the ↗/↙ corners.
enum { CURSOR_ARROW, CURSOR_IBEAM,
       CURSOR_RESIZE_H, CURSOR_RESIZE_V, CURSOR_RESIZE_NWSE, CURSOR_RESIZE_NESW };

typedef struct window window_t;

typedef void (*window_draw_fn)(window_t* win, int clip_x, int clip_y, uint32_t clip_w, uint32_t clip_h);

struct window {
    int x, y;
    uint32_t w, h;
    int normal_x, normal_y;
    uint32_t normal_w, normal_h;
    char title[MAX_TITLE];
    int z_order;
    int visible;
    int state;
    int dragging;
    int drag_off_x, drag_off_y;
    int resizing;
    int resize_dir;
    int resize_start_x, resize_start_y;
    uint32_t resize_start_w, resize_start_h;
    int focused;
    int id;
    int cursor_shape;   // CURSOR_ARROW (default) / CURSOR_IBEAM — pointer over the client area
    int workspace;
    int has_close;
    int has_min;
    int has_max;
    window_draw_fn draw;
    void (*on_key)(struct window* win, int key);
    void (*on_click)(struct window* win, int mx, int my, int btn);
    void (*on_pressed)(struct window* win, int mx, int my, int btn);
    void (*on_mousemove)(struct window* win, int mx, int my, int btns);
    void (*on_resize)(struct window* win, int w, int h);   // client area resized (fired on drag-release); w,h = new CLIENT size
    int  (*on_tick)(struct window* win);   // periodic ~30fps tick; returns 1 if it changed something needing a redraw (0 = idle, no repaint)
    void (*on_close)(struct window* win);  // optional: called by window_destroy BEFORE freeing `reserved`, so an app can release ctx sub-allocations
    void* reserved;
};

void compositor_init(void);
window_t* window_create(int x, int y, uint32_t w, uint32_t h, const char* title, window_draw_fn draw);
// Replace a window's title-bar text after creation (a ring-3 client via SYS_WIN_SET_TITLE,
// or an in-kernel app). Truncated to MAX_TITLE-1; redraws so the bar + taskbar refresh.
// Returns 0, or -1 for an unknown id.
int window_set_title(int id, const char* title);
// Change the screen mode and re-flow icons + open windows onto it. Callers must
// pass a mode the hardware actually supports — vbe_set_mode validates nothing.
void display_set_mode(uint32_t w, uint32_t h);
void theme_set_accent(uint32_t rgb);   // set the runtime UI accent (the wallpaper picker + nyx.conf)
void save_nyx_config(void);            // persist the current theme to /etc/nyx.conf
void window_destroy(int id);
void window_focus(int id);
void window_move(int id, int x, int y);
void window_resize(int id, uint32_t w, uint32_t h);
void window_minimize(int id);
void window_maximize(int id);
void window_restore(int id);
void window_snap(int id, int left);   // tile the window to the left (1) or right (0) half
void window_set_workspace(int id, int ws);
int  window_get_count(void);
int  window_get_ids(int* ids, int max);
void compositor_run(void);
void compositor_quit(void);
int compositor_is_running(void);
window_t* compositor_open_editor(const char* path);  // open Text Editor, optionally with a file
int cursor_pick_selftest(void);   // KAT: pointer shape picked from the window under it (I-beam over text)
int cursor_resize_selftest(void); // KAT: resize-edge dir -> directional cursor mapping
int snap_gap_selftest(void);      // KAT: snap/maximize tiling geometry, with and without WM gaps
int scheme_selftest(void);        // KAT: nyx.conf colorscheme presets resolve to real wallpaper+accent names
int border_color_selftest(void);  // KAT: nyx.conf `border` focused-window outline color resolves to real palette rgb
int hex_color_selftest(void);      // KAT: nyx.conf `accent = #RRGGBB` hex color parser
int accent_config_selftest(void);  // KAT: save_nyx_config accent string round-trips (#hex or name)
int net_rate_selftest(void);       // KAT: the desktop NET-graph rate caption formatter (B/KB/MB per s)
int col_blend_selftest(void);     // KAT: the col_blend alpha-mix primitive (panel_tint / future translucency)
int title_set_selftest(void);     // KAT: window_set_title copy/truncation + bad-id reject (SYS_WIN_SET_TITLE path)
int rounding_selftest(void);      // KAT: nyx.conf `rounding` corner-radius parse + clamp (0 = square windows)
int shadow_selftest(void);        // KAT: nyx.conf boolean-off parse (`shadow` toggle: off/0/false/no)
int titlebar_selftest(void);      // KAT: title-bar text x placement (nyx.conf `title_align` left/center)
int wallpaper_dim_selftest(void); // KAT: nyx.conf `wallpaper_dim` percent parse + clamp (0..80)
extern int compositor_logout_requested;              // user menu "Log out" -> boot loop re-shows login

#endif
