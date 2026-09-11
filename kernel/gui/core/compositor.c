#include "../../core/kernel.h"
#include "../../core/caldate.h"
#include "../../core/datefmt.h"
#include "compositor.h"
#include "theme.h"
#include "../../drivers/video/font.h"
#include "../apps/terminal_win.h"
#include "../games/pong_win.h"
#include "../games/voxel_win.h"
#include "../games/fire_win.h"
#include "../games/matrix_win.h"
#include "../games/lava_win.h"
#include "../games/mandel_win.h"
#include "../games/julia_win.h"
#include "../games/particles_win.h"
#include "../games/rotor_win.h"
#include "../games/fill_win.h"
#include "../games/life_win.h"
#include "../games/blobs_win.h"
#include "../apps/fileman_win.h"
#include "../apps/paint_win.h"
#include "../apps/taskman_win.h"
#include "../apps/editor_win.h"
#include "../apps/imageview_win.h"
#include "../apps/soundtest_win.h"
#include "../apps/calc_win.h"
#include "../games/minesweeper_win.h"
#include "../games/snake_win.h"
#include "../games/tetris_win.h"
#include "../games/games_win.h"
#include "../apps/selene_win.h"
#include "../apps/oneiros_win.h"
#include "wallpaper_win.h"
#include "../../drivers/misc/rtc.h"
#include "../../auth/login.h"
#include "../../auth/auth.h"

static window_t* windows[MAX_WINDOWS];
static int window_count = 0;
static int next_id = 100;
static int focused_id = 0;
static int drag_id = 0;
static int resize_id = 0;
static int quit = 0;
static int compositor_active = 0;
static int current_workspace = 0;

static uint32_t taskbar_bg, taskbar_fg, taskbar_hl;
static uint32_t desktop_bg, title_active, title_inactive;

static int start_menu_open = 0;
static int user_menu_open = 0;      // the taskbar user badge's popup menu
int compositor_logout_requested = 0; // set by the user menu; boot loop re-shows login
static int ctx_menu_open = 0;
static int ctx_menu_x = 0, ctx_menu_y = 0;
static int cal_popup_open = 0;      // the taskbar clock's calendar popup
static int g_clock_12h = 0;         // taskbar clock: 0 = 24-hour (default), 1 = 12-hour AM/PM — nyx.conf `clock`
static int g_gaps      = 0;         // WM gaps (px) inset around + between snapped/maximized tiles — nyx.conf `gaps` (0 = classic tiling, off)
static uint32_t g_border_color = 0; // focused-window outline override — nyx.conf `border` (0 = follow the UI accent)
static uint32_t g_border_inactive = 0; // unfocused-window outline override — nyx.conf `border_inactive` (0 = neutral bevel)
static int g_panel_tint = 0;        // taskbar tint toward the wallpaper colour, 0-100% — nyx.conf `panel_tint` (0 = off)
static int g_shadows   = 1;         // window + start-menu drop shadows — nyx.conf `shadow` (off = flat, no shadows)
static int g_title_center = 0;      // title-bar text alignment — nyx.conf `title_align` (0 = left default, 1 = center)
static int g_wallpaper_dim = 0;     // wallpaper darken percent 0..80 — nyx.conf `wallpaper_dim` (0 = off; mutes the background so windows/icons pop)
static int net_popup_open = 0;      // the system tray's network-status popup
static int spk_popup_open = 0;      // the system tray's sound/volume popup
static int g_volume = 75;           // master volume 0..100 (persisted while running)
static int g_muted  = 0;            // mute toggle
static int mouse_x = 0, mouse_y = 0;
static int mouse_z = 0;          // previous wheel total (see mouse_get_z); delta = new - this
static uint8_t mouse_btns = 0;
// Set by redraw_all() whenever it recomposites the back buffer; the event loop
// clears it after fb_present(). This means *any* redraw_all() caller — including
// menu/drag handlers that call it directly (not via the loop's `redraw` flag) —
// gets its finished frame published, without every caller having to remember to.
static int frame_dirty = 0;

#define CURSOR_W 12
#define CURSOR_H 16
static const uint8_t cursor_data[CURSOR_H][CURSOR_W] = {
    {1,1,0,0,0,0,0,0,0,0,0,0},{1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},{1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},{1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},{1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},{1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},{1,2,2,2,2,2,1,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},{1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},{0,0,0,0,0,0,1,2,2,1,0,0},
};
// I-beam: a white (2) vertical bar with a black (1) outline + top/bottom serifs, so the
// pointer reads as a text caret over Terminal/Editor content. Same 0/1/2 encoding + 12x16
// box as the arrow, so it shares draw_cursor's save/restore path (only the bitmap swaps).
static const uint8_t ibeam_data[CURSOR_H][CURSOR_W] = {
    {0,0,0,1,1,1,1,1,1,0,0,0},{0,0,0,1,2,2,2,2,1,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,1,2,2,2,2,1,0,0,0},{0,0,0,1,1,1,1,1,1,0,0,0},
};
// Directional resize cursors (same 0=transparent / 1=black-outline / 2=white-fill 12x16
// box as the arrow, so they share draw_cursor's save/restore path). Double-headed arrows
// generated to be symmetric; picked by pick_cursor_shape when the pointer is on a resize
// border. H = ↔ (left/right edges), V = ↕ (top/bottom), NWSE = ⤡ (↖/↘ corners),
// NESW = ⤢ (↗/↙ corners).
static const uint8_t cursor_resize_h[CURSOR_H][CURSOR_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},{0,0,1,1,2,1,1,2,1,1,0,0},
    {0,1,1,2,1,1,1,1,2,1,1,0},{1,1,2,1,1,1,1,1,1,2,1,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},{1,2,2,2,2,2,2,2,2,2,2,1},
    {1,1,2,1,1,1,1,1,1,2,1,1},{0,1,1,2,1,1,1,1,2,1,1,0},
    {0,0,1,1,2,1,1,2,1,1,0,0},{0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0},
};
static const uint8_t cursor_resize_v[CURSOR_H][CURSOR_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,1,1,2,2,1,1,0,0,0},{0,0,0,1,2,2,2,2,1,0,0,0},
    {0,0,1,1,2,2,2,2,1,1,0,0},{0,0,1,2,1,2,2,1,2,1,0,0},
    {0,0,1,1,1,2,2,1,1,1,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},{0,0,1,1,1,2,2,1,1,1,0,0},
    {0,0,1,2,1,2,2,1,2,1,0,0},{0,0,1,1,2,2,2,2,1,1,0,0},
    {0,0,0,1,2,2,2,2,1,0,0,0},{0,0,0,1,1,2,2,1,1,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0},
};
static const uint8_t cursor_resize_nwse[CURSOR_H][CURSOR_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},{1,1,1,1,1,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},{1,2,2,1,1,0,0,0,0,0,0,0},
    {1,1,2,2,1,1,0,0,0,0,0,0},{0,1,1,1,2,1,1,0,0,0,0,0},
    {0,0,0,1,1,2,1,1,0,0,0,0},{0,0,0,0,1,1,2,1,1,0,0,0},
    {0,0,0,0,0,1,1,2,1,1,0,0},{0,0,0,0,0,0,1,1,2,1,1,1},
    {0,0,0,0,0,0,0,1,1,2,2,1},{0,0,0,0,0,0,0,1,1,2,2,1},
    {0,0,0,0,0,0,0,1,2,2,2,1},{0,0,0,0,0,0,0,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0},
};
static const uint8_t cursor_resize_nesw[CURSOR_H][CURSOR_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,2,2,2,1},{0,0,0,0,0,0,0,1,1,2,2,1},
    {0,0,0,0,0,0,1,1,2,2,1,1},{0,0,0,0,0,1,1,2,1,1,1,0},
    {0,0,0,0,1,1,2,1,1,0,0,0},{0,0,0,1,1,2,1,1,0,0,0,0},
    {0,0,1,1,2,1,1,0,0,0,0,0},{1,1,1,2,1,1,0,0,0,0,0,0},
    {1,2,2,1,1,0,0,0,0,0,0,0},{1,2,2,1,1,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},{1,1,1,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0},
};
static int pick_cursor_shape(int mx, int my);   // defined after window_hit (below)
static int resize_hit(window_t* win, int mx, int my, int* dir);  // defined below pick
static uint32_t cursor_bg_buf[CURSOR_H * CURSOR_W];
static int cursor_saved = 0;

static inline uint32_t win_client_x(window_t* w) { return w->x; }
static inline uint32_t win_client_y(window_t* w) { return w->y + TITLE_H; }
static inline uint32_t win_total_h(window_t* w) { return w->h + TITLE_H; }

static void compositor_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    if (x >= fw || y >= fh) return;
    ((uint32_t*)fb_get_addr())[y * fw + x] = color;
}

static void save_cursor_bg(int mx, int my) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    for (int y = 0; y < CURSOR_H && my + y < (int)fh; y++)
        for (int x = 0; x < CURSOR_W && mx + x < (int)fw; x++)
            cursor_bg_buf[y * CURSOR_W + x] = ((uint32_t*)fb_get_addr())[(my + y) * fw + (mx + x)];
    cursor_saved = 1;
}

static void restore_cursor_bg(int mx, int my) {
    if (!cursor_saved) return;
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    for (int y = 0; y < CURSOR_H && my + y < (int)fh; y++)
        for (int x = 0; x < CURSOR_W && mx + x < (int)fw; x++)
            ((uint32_t*)fb_get_addr())[(my + y) * fw + (mx + x)] = cursor_bg_buf[y * CURSOR_W + x];
}

static void draw_cursor(int mx, int my) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    if (fw == 0 || fh == 0) return;
    uint32_t* fb = (uint32_t*)fb_get_addr();
    if (!fb) return;

    // Context cursor: I-beam over a text window's client area, a directional double-arrow
    // over a resize border, the arrow otherwise. The saved/restored 12x16 box is the same
    // for every shape, so only the bitmap changes.
    const uint8_t (*glyph)[CURSOR_W];
    switch (pick_cursor_shape(mx, my)) {
        case CURSOR_IBEAM:        glyph = ibeam_data;         break;
        case CURSOR_RESIZE_H:     glyph = cursor_resize_h;    break;
        case CURSOR_RESIZE_V:     glyph = cursor_resize_v;    break;
        case CURSOR_RESIZE_NWSE:  glyph = cursor_resize_nwse; break;
        case CURSOR_RESIZE_NESW:  glyph = cursor_resize_nesw; break;
        default:                  glyph = cursor_data;        break;
    }
    for (int y = 0; y < CURSOR_H && my + y < (int)fh; y++) {
        for (int x = 0; x < CURSOR_W && mx + x < (int)fw; x++) {
            uint8_t p = glyph[y][x];
            if (p == 0) continue;
            if (p == 2)
                fb[(my + y) * fw + (mx + x)] = 0xFFFFFF;
            else
                fb[(my + y) * fw + (mx + x)] = 0x000000;
        }
    }
}

// Title-bar window controls. Square by design — but the corners are softened
// (WINBTN_R) so they sit with the rounded title bar, and the fills carry the
// brand identity instead of the old flat primary colours: MAXIMISE takes the
// NyxOS purple accent, CLOSE the universal red, MINIMISE a neutral slate. The
// cut corners let the title-bar gradient show through, so the buttons read as
// soft chips rather than hard blocks. Glyphs are unchanged (see the note below
// draw_x_button on the shared inner box).
#define WINBTN_R      3
#define WINBTN_CLOSE  fb_rgb(214,  72,  72)   /* red — destructive           */
#define WINBTN_MIN    fb_rgb( 92,  98, 116)   /* neutral slate               */
#define WINBTN_MAX    fb_rgb(180, 150, 240)   /* light lavender — brand purple,
                                                 kept lighter than the focused
                                                 (accent) title bar so it stays
                                                 visible instead of blending in */

static void draw_x_button(int x, int y, int size, uint32_t color) {
    fb_fill_round_rect(x, y, size, size, WINBTN_R, WINBTN_CLOSE);
    int pad = 4;
    for (int i = 0; i < size - pad * 2; i++) {
        compositor_draw_pixel(x + pad + i, y + pad + i, color);
        compositor_draw_pixel(x + pad + i + 1, y + pad + i, color);
        compositor_draw_pixel(x + size - pad - 1 - i, y + pad + i, color);
        compositor_draw_pixel(x + size - pad - 1 - i - 1, y + pad + i, color);
    }
}

// All three glyphs share the SAME inner box — [x+P, x+E] x [y+P, y+E] with P=4 — so the
// dash, the square and the X read as one consistent set on every title bar. (The old
// max button drew its top/bottom edges out to x+size-1 while the verticals stopped at
// x+size-4, so the square had two stubs poking out the right — a few px off on every
// window. The dash was also 2px wider than the X. Fixed by centring all three the same.)
static void draw_min_button(int x, int y, int size, uint32_t color) {
    fb_fill_round_rect(x, y, size, size, WINBTN_R, WINBTN_MIN);
    int p = 4;
    for (int i = 0; i < size - 2 * p; i++)
        compositor_draw_pixel(x + p + i, y + size / 2, color);      // centred dash
}

static void draw_max_button(int x, int y, int size, uint32_t color) {
    fb_fill_round_rect(x, y, size, size, WINBTN_R, WINBTN_MAX);     // brand purple (light) = identity
    int p = 4, e = size - 1 - p;                                     // inner square [p, e]
    for (int i = 0; i <= e - p; i++) {
        compositor_draw_pixel(x + p + i, y + p,     color);         // top edge
        compositor_draw_pixel(x + p + i, y + e,     color);         // bottom edge
        compositor_draw_pixel(x + p,     y + p + i, color);         // left edge
        compositor_draw_pixel(x + e,     y + p + i, color);         // right edge
    }
}

// Corner-rounding radius for window title bars.
#define WIN_RADIUS 7
#define WIN_RADIUS_MAX 14                   // rice clamp for the nyx.conf `rounding` key
// Runtime corner radius (px). Default WIN_RADIUS; the nyx.conf `rounding` key overrides it
// (0 = fully square windows + start menu). win_radius() still shrinks it per window so a
// minimum-size window never over-rounds. Set by apply_nyx_config().
static int g_corner_radius = WIN_RADIUS;

// Rounded-corner primitives (fb_corner_inset / fb_fill_round_rect /
// fb_stroke_round_rect) and the integer sqrt now live in fb.c, shared with the
// login screen. This file keeps only the window-specific pieces below.

// The radius a given window can actually use: shrink it so a minimum-size window
// (120x80) never rounds so hard the two corners meet.
static int win_radius(window_t* win) {
    int r = g_corner_radius;
    if (r > (int)win->w / 2) r = (int)win->w / 2;
    if (r > TITLE_H)         r = TITLE_H;             // rounding lives in the title bar
    return r < 0 ? 0 : r;
}

// One row of a vertical gradient: linear interpolation from `top` (i=0) to
// `bottom` (i=n-1). Split out from fb_fill_vgrad so the title bar can drive the
// fill row by row and inset the rounded top corners.
static uint32_t vgrad_row(uint32_t top, uint32_t bottom, int i, int n) {
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int dr = (int)((bottom >> 16) & 0xFF) - tr;
    int dg = (int)((bottom >>  8) & 0xFF) - tg;
    int db = (int)( bottom        & 0xFF) - tb;
    int den = n > 1 ? n - 1 : 1;
    return fb_rgb(tr + dr * i / den, tg + dg * i / den, tb + db * i / den);
}

// Shift a colour a percentage toward white / black. Proportional (not a flat
// per-channel add) so the hue is preserved as it lightens or darkens.
static uint32_t col_lighten(uint32_t c, int pct) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return fb_rgb(r + (255 - r) * pct / 100,
                  g + (255 - g) * pct / 100,
                  b + (255 - b) * pct / 100);
}
static uint32_t col_darken(uint32_t c, int pct) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return fb_rgb(r - r * pct / 100, g - g * pct / 100, b - b * pct / 100);
}
// Alpha-mix two colours per channel: `a` percent of `fg` over `bg` (0 = all bg, 100 = all
// fg). The reusable blend primitive — used now by the `panel_tint` frosted taskbar, and the
// groundwork for future true (fb-read-back) window/panel translucency.
static uint32_t col_blend(uint32_t fg, uint32_t bg, int a) {
    if (a < 0) a = 0; else if (a > 100) a = 100;
    int fr = (fg >> 16) & 0xFF, fgn = (fg >> 8) & 0xFF, fbn = fg & 0xFF;
    int br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bbn = bg & 0xFF;
    return fb_rgb((fr * a + br * (100 - a)) / 100,
                  (fgn * a + bgn * (100 - a)) / 100,
                  (fbn * a + bbn * (100 - a)) / 100);
}

// KAT for col_blend: endpoints (all-fg / all-bg), a half mix, a quarter mix, and the
// identity of blending equal colours. 0 = pass.
int col_blend_selftest(void) {
    uint32_t red = fb_rgb(255, 0, 0), blue = fb_rgb(0, 0, 255);
    if (col_blend(red, blue, 100) != red)  return 1;
    if (col_blend(red, blue, 0)   != blue) return 2;
    if (col_blend(red, blue, 50)  != fb_rgb(127, 0, 127)) return 3;
    if (col_blend(fb_rgb(255,255,255), fb_rgb(0,0,0), 25) != fb_rgb(63, 63, 63)) return 4;
    if (col_blend(fb_rgb(128,128,128), fb_rgb(128,128,128), 40) != fb_rgb(128,128,128)) return 5;
    return 0;
}

// The taskbar background, tinted toward the wallpaper's colour by nyx.conf `panel_tint`
// (0 = the plain theme colour, the default; 100 = fully the wallpaper colour) — a frosted
// panel that picks up the desktop hue. Two known colours, so no fb read-back and no
// per-frame drift. Every taskbar_bg draw site routes through here to stay coherent.
static uint32_t taskbar_bg_effective(void) {
    return g_panel_tint ? col_blend(wallpaper_base_color(), taskbar_bg, g_panel_tint) : taskbar_bg;
}

// The runtime UI accent (declared in theme.h; THEME_ACCENT/THEME_ACCENT_DIM read
// these). Default = brand purple "Morado" {130,90,210}; apply_nyx_config() sets it
// from /etc/nyx.conf so a single `accent = <name>` retints the whole desktop chrome.
uint32_t g_theme_accent     = 0;   // set at desktop start (theme_set_accent); 0 until then
uint32_t g_theme_accent_dim = 0;
// Set the UI accent and derive the darker "pressed / frame-lo" shade from it, so every
// scheme keeps the same light→dark relationship the purple had (28% ≈ the old 130,90,210
// → 92,64,150 pair). Pure aside from the two globals it writes.
void theme_set_accent(uint32_t rgb) {
    g_theme_accent     = rgb;
    g_theme_accent_dim = col_darken(rgb, 28);
    // The focused title-bar fill is cached in title_active (set once at compositor_init,
    // before the config is read); keep it in sync so the whole bar — not just the accent
    // frame border, which reads THEME_ACCENT live — follows the runtime accent.
    title_active = rgb;   // == THEME_TITLE_ACTIVE (THEME_ACCENT); title_inactive stays a fixed grey
}

// A button filled with the same top-lit vertical gradient the title bars use,
// centred on `base` (±amt), with a transparent-background label so the text sits
// directly on the gradient. Used for the taskbar's Menu button and window tabs.
static void draw_grad_button(int x, int y, int w, int h, uint32_t base, uint32_t fg,
                             const char* label, int amt) {
    fb_fill_vgrad(x, y, w, h, col_lighten(base, amt), col_darken(base, amt));
    if (label) {
        int tw = (int)strlen(label) * FONT_WIDTH;
        font_draw_string_trans(x + (w - tw) / 2, y + (h - FONT_HEIGHT) / 2, label, fg);
    }
}

// Fit `title` into `avail_px` pixels of title-bar text space, writing the possibly
// truncated result to out[0..outsz-1]. The whole title is copied when it fits; else
// it is cut to the widest prefix that still leaves room for a trailing "..." ellipsis
// (or, when even "..." will not fit, the widest bare prefix). Pure + unit-testable —
// keeps a long title (or a normal one on a narrow/snapped window) from running under
// the min/max/close buttons. FONT_WIDTH px per glyph.
static void fit_title(const char* title, int avail_px, char* out, int outsz) {
    int max_chars = (avail_px > 0) ? avail_px / FONT_WIDTH : 0;
    if (max_chars > outsz - 1) max_chars = outsz - 1;
    int tlen = (int)strlen(title);
    int i = 0;
    if (tlen <= max_chars) {                         // fits whole
        for (; i < tlen; i++) out[i] = title[i];
    } else if (max_chars >= 4) {                      // prefix + "..."
        int keep = max_chars - 3;
        for (; i < keep; i++) out[i] = title[i];
        out[i++] = '.'; out[i++] = '.'; out[i++] = '.';
    } else {                                          // too narrow for an ellipsis
        for (; i < max_chars; i++) out[i] = title[i];
    }
    out[i] = '\0';
}

static int title_text_x(int left, int right, int textw, int center);   // fwd (defined with its KAT below)
static void draw_titlebar(window_t* win) {
    uint32_t base = win->focused ? title_active : title_inactive;
    // A subtle top-lit vertical gradient gives the bar some dimension to sit with
    // the drop shadows. It is centred on `base` (lighten top, darken bottom by the
    // same amount), so the mid rows — where the title text sits — are ~base; the
    // text is drawn with a transparent background over it, so no flat panel breaks
    // the gradient. Focused bars get a touch more sheen than inactive ones.
    int amt = win->focused ? 16 : 8;
    uint32_t gtop = col_lighten(base, amt), gbot = col_darken(base, amt);
    // Draw the gradient row by row so the top two corners can be rounded: the
    // first R rows are inset by the arc, leaving the corner pixels unpainted for
    // the desktop/lower window to show through. Only the TOP is rounded — the
    // bottom of the title bar meets the (square) body, and app draw callbacks
    // fill the body edge-to-edge, so rounding there would just be squared off.
    int R = win_radius(win);
    for (uint32_t row = 0; row < TITLE_H; row++) {
        int inset = (row < (uint32_t)R) ? fb_corner_inset((int)row, R) : 0;
        fb_fill_rect(win->x + inset, win->y + row, win->w - 2 * inset, 1,
                     vgrad_row(gtop, gbot, (int)row, TITLE_H));
    }

    // Left edge of the leftmost button present, so the title can be truncated to end
    // a few pixels before it instead of running underneath (walks the same right-to-
    // left button layout the drawing below uses).
    int bl = win->x + (int)win->w - CLOSE_W - 2;
    int leftmost_btn = win->x + (int)win->w - 2;     // no buttons -> the inner right edge
    if (win->has_close) { leftmost_btn = bl; bl -= CLOSE_W + 2; }
    if (win->has_max)   { leftmost_btn = bl; bl -= CLOSE_W + 2; }
    if (win->has_min)   { leftmost_btn = bl; }
    int text_x = win->x + 4;
    char tbuf[MAX_TITLE + 4];
    fit_title(win->title, leftmost_btn - 6 - text_x, tbuf, (int)sizeof(tbuf));

    int y_off = win->y + (TITLE_H - FONT_HEIGHT) / 2;
    int tx = title_text_x(text_x, leftmost_btn - 6, (int)strlen(tbuf) * FONT_WIDTH, g_title_center);
    font_draw_string_trans(tx, y_off, tbuf, THEME_TITLE_TEXT);

    int bx = win->x + win->w - CLOSE_W - 2;
    if (win->has_close) {
        draw_x_button(bx, win->y + 3, CLOSE_W, THEME_TITLE_TEXT);
        bx -= CLOSE_W + 2;
    }
    if (win->has_max) {
        draw_max_button(bx, win->y + 3, CLOSE_W, THEME_TITLE_TEXT);
        bx -= CLOSE_W + 2;
    }
    if (win->has_min) {
        draw_min_button(bx, win->y + 3, CLOSE_W, THEME_TITLE_TEXT);
    }
}

static void draw_window_frame(window_t* win) {
    // The frame carries the focus state too, not just the title bar: a focused
    // window is outlined in the accent so its border and title strip read as one
    // continuous edge. Previously this bevel was identical on every window, so a
    // window buried under others gave no focus cue at all once its title bar was
    // covered.
    // Focused windows outline in the accent (or the nyx.conf `border` color if set), so
    // the border + title strip read as one edge; unfocused windows keep the neutral bevel
    // unless nyx.conf `border_inactive` overrides it (the classic focused/unfocused rice pair).
    uint32_t acc_hi = g_border_color ? g_border_color               : THEME_ACCENT;
    uint32_t acc_lo = g_border_color ? col_darken(g_border_color, 28) : THEME_ACCENT_DIM;
    uint32_t ina_hi = g_border_inactive ? g_border_inactive               : THEME_FRAME_HI;
    uint32_t ina_lo = g_border_inactive ? col_darken(g_border_inactive, 28) : THEME_FRAME_LO;
    uint32_t hi = win->focused ? acc_hi : ina_hi;
    uint32_t lo = win->focused ? acc_lo : ina_lo;
    int x = win->x, y = win->y, w = (int)win->w, H = (int)win_total_h(win);
    int R = win_radius(win);

    // Straight edges run BETWEEN the rounded corners on all four sides now (the
    // body's bottom is rounded by the clip in redraw_all, so the frame traces that
    // arc to match the top).
    fb_fill_rect(x + R, y - 1, w - 2 * R, 1, hi);        // top
    fb_fill_rect(x + R, y + H, w - 2 * R, 1, lo);        // bottom
    fb_fill_rect(x - 1, y + R, 1, H - 2 * R, hi);        // left
    fb_fill_rect(x + w, y + R, 1, H - 2 * R, lo);        // right

    // Bottom corner arcs (lo), mirroring the top: one pixel outside the curve the
    // clip leaves in the body at the bottom.
    for (int row = 0; row < R; row++) {
        int inset = fb_corner_inset(row, R);
        fb_put_pixel((uint32_t)(x + inset - 1), (uint32_t)(y + H - 1 - row), lo);   // bottom-left
        fb_put_pixel((uint32_t)(x + w - inset), (uint32_t)(y + H - 1 - row), lo);   // bottom-right
    }

    // Top corner arcs, one pixel outside the title bar's rounded corners so the
    // border hugs the same curve the fill leaves.
    for (int row = 0; row < R; row++) {
        int inset = fb_corner_inset(row, R);
        fb_put_pixel((uint32_t)(x + inset - 1),  (uint32_t)(y + row), hi);   // top-left
        fb_put_pixel((uint32_t)(x + w - inset),  (uint32_t)(y + row), hi);   // top-right
    }
}

static int titlebar_hit(window_t* win, int mx, int my) {
    return mx >= win->x && mx < win->x + (int)win->w
        && my >= win->y && my < win->y + TITLE_H;
}

static int close_hit(window_t* win, int mx, int my) {
    if (!win->has_close) return 0;
    int bx = win->x + win->w - CLOSE_W - 2;
    return mx >= bx && mx < bx + CLOSE_W
        && my >= win->y + 3 && my < win->y + 3 + CLOSE_W;
}

static int max_hit(window_t* win, int mx, int my) {
    if (!win->has_max) return 0;
    int bx = win->x + win->w - CLOSE_W - 2 - (win->has_close ? CLOSE_W + 2 : 0);
    return mx >= bx && mx < bx + CLOSE_W
        && my >= win->y + 3 && my < win->y + 3 + CLOSE_W;
}

// --- title-bar double-click -> maximize/restore ---
#define DBLCLICK_MS 400
static uint32_t last_tb_click_ms = 0;
static int      last_tb_click_id = -1;
// Two presses on the SAME window's title bar within DBLCLICK_MS milliseconds are a
// double-click. Unsigned subtraction so a wrap of the ms counter is handled. Pure +
// unit-testable — the standard "double-click the title bar to (un)maximize" gesture.
static int is_dblclick(uint32_t last_ms, uint32_t now_ms, int same_win) {
    return same_win && (uint32_t)(now_ms - last_ms) <= DBLCLICK_MS;
}

static int min_hit(window_t* win, int mx, int my) {
    if (!win->has_min) return 0;
    int bx = win->x + win->w - CLOSE_W - 2 - (win->has_close ? CLOSE_W + 2 : 0) - (win->has_max ? CLOSE_W + 2 : 0);
    return mx >= bx && mx < bx + CLOSE_W
        && my >= win->y + 3 && my < win->y + 3 + CLOSE_W;
}

static int window_hit(window_t* win, int mx, int my) {
    return mx >= win->x && mx < win->x + (int)win->w
        && my >= win->y && my < win->y + (int)win_total_h(win);
}

// The directional double-arrow that signals a resize in `dir` (a RESIZE_* value from
// resize_hit): left/right -> ↔, top/bottom -> ↕, the two ↖/↘ corners -> \, ↗/↙ -> /.
static int cursor_for_resize_dir(int dir) {
    switch (dir) {
        case RESIZE_LEFT:
        case RESIZE_RIGHT:        return CURSOR_RESIZE_H;
        case RESIZE_TOP:
        case RESIZE_BOTTOM:       return CURSOR_RESIZE_V;
        case RESIZE_LEFT_TOP:
        case RESIZE_CORNER:       return CURSOR_RESIZE_NWSE;   // CORNER = right-bottom
        case RESIZE_RIGHT_TOP:
        case RESIZE_LEFT_BOTTOM:  return CURSOR_RESIZE_NESW;
        default:                  return CURSOR_ARROW;
    }
}

// Which cursor shape belongs at (mx,my): the topmost window under the pointer decides.
// On its resize border the pointer becomes the matching directional double-arrow (exactly
// where a drag-resize would start — same resize_hit the mouse-down path uses); else over
// its CLIENT area it takes that window's cursor_shape (I-beam for text windows); over the
// title bar or the bare desktop it stays the arrow.
static int pick_cursor_shape(int mx, int my) {
    window_t* top = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = windows[i];
        if (!w || !w->visible) continue;
        if (w->workspace != current_workspace || w->state == WSTATE_MINIMIZED) continue;
        if (!window_hit(w, mx, my)) continue;
        if (!top || w->z_order > top->z_order) top = w;
    }
    if (!top) return CURSOR_ARROW;
    // The title-bar strip is a DRAG zone that the mouse-down path checks before resize_hit,
    // so a top-edge/top-corner resize is intercepted there — don't promise a resize cursor
    // the click can't deliver. Below the strip, a resize border wins over the client cursor.
    int dir;
    if (!titlebar_hit(top, mx, my) && resize_hit(top, mx, my, &dir))
        return cursor_for_resize_dir(dir);                 // resize border (left/right/bottom)
    if (my >= top->y + TITLE_H) return top->cursor_shape;   // client area
    return CURSOR_ARROW;                                    // title bar
}

// KAT: the pointer shape is picked from the topmost window under it — I-beam over a text
// window's client area, arrow over its title bar / the bare desktop / a non-text window.
// Stages a throwaway window in the (idle, pre-run) compositor table and restores it.
int cursor_pick_selftest(void) {
    static window_t fake;
    memset_asm(&fake, 0, sizeof fake);
    fake.x = 100; fake.y = 100; fake.w = 200; fake.h = 150;
    fake.visible = 1; fake.state = WSTATE_NORMAL; fake.z_order = 7;
    fake.workspace = current_workspace;
    fake.cursor_shape = CURSOR_IBEAM;
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!windows[i]) { slot = i; break; }
    if (slot < 0) return 5;
    windows[slot] = &fake;
    int rc = 0;
    if      (pick_cursor_shape(150, 100 + TITLE_H + 30) != CURSOR_IBEAM) rc = 10; // client -> I-beam
    else if (pick_cursor_shape(150, 100 + 5)            != CURSOR_ARROW) rc = 11; // title bar -> arrow
    else if (pick_cursor_shape(400, 400)                != CURSOR_ARROW) rc = 12; // off-window -> arrow
    else {                                                                        // arrow window keeps arrow
        fake.cursor_shape = CURSOR_ARROW;
        if (pick_cursor_shape(150, 100 + TITLE_H + 30)  != CURSOR_ARROW) rc = 13;
    }
    windows[slot] = NULL;
    return rc;
}

// KAT: over a window's resize border the pointer becomes the matching directional arrow,
// exactly where a drag-resize would start. Same staged-window trick as cursor_pick_selftest.
// Window at (100,100) 200x150: rx=300, by=100+150+TITLE_H=272, client_top=122, margin=6.
int cursor_resize_selftest(void) {
    static window_t fake;
    memset_asm(&fake, 0, sizeof fake);
    fake.x = 100; fake.y = 100; fake.w = 200; fake.h = 150;
    fake.visible = 1; fake.state = WSTATE_NORMAL; fake.z_order = 7;
    fake.workspace = current_workspace;
    fake.cursor_shape = CURSOR_IBEAM;   // a text window, so resize must win over the I-beam
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!windows[i]) { slot = i; break; }
    if (slot < 0) return 5;
    windows[slot] = &fake;
    int rc = 0;
    // Reachable resize borders (below the title-bar strip) -> the right directional arrow.
    if      (pick_cursor_shape(297, 200) != CURSOR_RESIZE_H)    rc = 20; // right edge  -> ↔
    else if (pick_cursor_shape(103, 200) != CURSOR_RESIZE_H)    rc = 21; // left edge   -> ↔
    else if (pick_cursor_shape(200, 269) != CURSOR_RESIZE_V)    rc = 22; // bottom edge -> ↕
    else if (pick_cursor_shape(297, 269) != CURSOR_RESIZE_NWSE) rc = 23; // ↘ corner -> NWSE
    else if (pick_cursor_shape(103, 269) != CURSOR_RESIZE_NESW) rc = 24; // ↙ corner -> NESW
    // Resize beats the I-beam at the border but NOT in the deep client area.
    else if (pick_cursor_shape(200, 200) != CURSOR_IBEAM)       rc = 25; // interior    -> I-beam
    // The title-bar strip is a drag zone: no resize cursor even on its left edge / top band.
    else if (pick_cursor_shape(103, 110) != CURSOR_ARROW)       rc = 26; // titlebar L edge -> arrow
    else if (pick_cursor_shape(200, 102) != CURSOR_ARROW)       rc = 27; // top band (drag) -> arrow
    else if (pick_cursor_shape(400, 400) != CURSOR_ARROW)       rc = 28; // off-window  -> arrow
    // The dir->shape mapping is exhaustive and correct.
    else if (cursor_for_resize_dir(RESIZE_TOP)         != CURSOR_RESIZE_V)    rc = 30;
    else if (cursor_for_resize_dir(RESIZE_LEFT_TOP)    != CURSOR_RESIZE_NWSE) rc = 31;
    else if (cursor_for_resize_dir(RESIZE_RIGHT_TOP)   != CURSOR_RESIZE_NESW) rc = 32;
    else if (cursor_for_resize_dir(RESIZE_NONE)        != CURSOR_ARROW)       rc = 33;
    windows[slot] = NULL;
    return rc;
}

static int resize_hit(window_t* win, int mx, int my, int* dir) {
    int rx = win->x + win->w, by = win->y + win_total_h(win);
    int lx = win->x, ty = win->y;
    int client_top = win->y + TITLE_H;

    int on_left   = mx >= lx - RESIZE_MARGIN && mx < lx + RESIZE_MARGIN;
    int on_right  = mx >= rx - RESIZE_MARGIN && mx < rx + RESIZE_MARGIN;
    int on_top    = my >= ty - RESIZE_MARGIN && my < ty + RESIZE_MARGIN;
    int on_bottom = my >= by - RESIZE_MARGIN && my < by + RESIZE_MARGIN;
    int in_client = my >= client_top && my < by;
    int in_vrange = my >= ty && my < by;

    if (on_top && on_left)    { *dir = RESIZE_LEFT_TOP;     return 1; }
    if (on_top && on_right)   { *dir = RESIZE_RIGHT_TOP;    return 1; }
    if (on_bottom && on_left) { *dir = RESIZE_LEFT_BOTTOM;  return 1; }
    if (on_bottom && on_right){ *dir = RESIZE_CORNER;       return 1; }
    if (on_left && in_vrange) { *dir = RESIZE_LEFT;         return 1; }
    if (on_right && in_client){ *dir = RESIZE_RIGHT;        return 1; }
    if (on_top && in_vrange)  { *dir = RESIZE_TOP;          return 1; }
    if (on_bottom && in_vrange){ *dir = RESIZE_BOTTOM;      return 1; }
    return 0;
}

static int start_hit(int mx, int my) {
    return mx >= 2 && mx < 2 + 80 && my >= (int)(fb_get_height() - TASKBAR_H) && my < (int)fb_get_height();
}

static int systray_x(void);   // defined below; the hit-test needs draw_taskbar()'s exact right edge
static int taskbar_mod_x(void); // left edge of the CPU/RAM module — the button strip stops here

static int taskbar_win_hit(int mx, int my, int* id) {
    uint32_t fh = fb_get_height();
    // Clamp buttons at the SAME right edge draw_taskbar() uses — taskbar_mod_x() - 4, which
    // reserves the CPU/RAM module + system tray + user badge + clock. Keeping this identical
    // to the drawn limit is what stops bx/bw drift (clicks landing on the wrong window).
    int right_limit = taskbar_mod_x() - 4;
    if (right_limit < 90) right_limit = 90;
    int bx = 90;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i] || !windows[i]->visible) continue;
        // Must match the window-selection predicate in draw_taskbar() exactly,
        // otherwise the button layout (bx) drifts out of sync and clicks land on
        // the wrong window. draw_taskbar() also lists minimized windows from other
        // workspaces (so they can be restored), so this hit-test must too.
        if (windows[i]->workspace != current_workspace && windows[i]->state != WSTATE_MINIMIZED) continue;
        int bw = 150;
        if (bx + bw > right_limit) bw = right_limit - bx;
        if (bw < 40) break;
        if (mx >= bx && mx < bx + bw && my >= (int)(fh - TASKBAR_H + 4) && my < (int)fh - 4) {
            *id = windows[i]->id;
            return 1;
        }
        bx += bw + 2;
    }
    return 0;
}

static int start_menu_hit(int mx, int my) {
    if (!start_menu_open) return 0;
    uint32_t fh = fb_get_height();
    int sm_x = 2;
    int sm_y = (int)fh - TASKBAR_H - START_H;
    return mx >= sm_x && mx < sm_x + START_W && my >= sm_y && my < sm_y + START_H;
}

/* THE start-menu geometry. draw_start_menu() lays the entries out from these and
 * so does the hit test — they used to derive it separately and disagreed on
 * every one of the three numbers:
 *
 *   - the first entry starts START_ITEM_Y (28) below the menu top, under the
 *     green header, but the hit test subtracted 8. Twenty pixels of skew means
 *     the lower two thirds of every entry selected the one BELOW it: clicking
 *     "Terminal" opened Settings.
 *   - the menu has START_ITEM_N entries and the test rejected `>= 12`, so the
 *     thirteenth — Minesweeper — could not be launched from the menu at all.
 *   - C integer division truncates toward ZERO, so a click on the header gave
 *     (my - sm_y - 8) / 28 == 0 rather than something negative, and the `< 0`
 *     guard never fired: clicking the "NyxOS Menu" title launched File Manager.
 *
 * Same lesson as the TITLE_H family in compositor.h — the fix is one definition
 * both sides read, not two derivations that have to be kept in agreement. */
#define START_HDR_H   24    /* accent brand header band */
#define START_ITEM_Y  28    /* first entry's top, relative to the menu top */
#define START_ITEM_H  28    /* entry pitch: 26 px body + 1 px separator + 1 */
#define START_ITEM_N  14    /* entries in the menu (indices 0..13) */

// The Start-menu entries — file scope so the draw, the hit test, and the
// type-to-search filter share ONE list. The index IS do_start_menu_action()'s arg.
static const char* const start_items[START_ITEM_N] = {
    "File Manager", "Text Editor", "Image Viewer", "Terminal",
    "Settings", "Nyx Monitor", "Selene",
    "Paint", "Sound Test", "About", "Shutdown", "Calculator",
    "Games", "Oneiros",
};
// Type-to-search (Windows-style): while the menu is open, typed letters narrow the
// list to entries containing the query (case-insensitive substring). The buffer is
// kept already-lowercased.
static char start_filter[24];
static int  start_filter_len = 0;
// Keyboard navigation of the (filtered) menu: start_sel is the selected row within the
// filtered list; start_nav is set once the user types or arrows, so the selection
// highlight only appears during keyboard use (mouse-only stays hover-driven).
static int  start_sel = 0;
static int  start_nav = 0;
static int  start_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int  start_item_matches(const char* label) {
    if (start_filter_len == 0) return 1;
    for (int i = 0; label[i]; i++) {
        int j = 0;
        while (start_filter[j] && label[i + j] && start_lc(label[i + j]) == start_filter[j]) j++;
        if (j == start_filter_len) return 1;
    }
    return 0;
}
// Fill out[] with the real indices of the entries matching the filter (all, in order,
// when the filter is empty). Returns the count. Shared by draw + hit-test + Enter.
static int start_menu_filtered(int* out) {
    int c = 0;
    for (int i = 0; i < START_ITEM_N; i++)
        if (start_item_matches(start_items[i])) out[c++] = i;
    return c;
}

static int start_menu_item_hit(int mx, int my, int* idx) {
    if (!start_menu_open) return 0;
    uint32_t fh = fb_get_height();
    int sm_x = 2;
    int sm_y = (int)fh - TASKBAR_H - START_H;
    if (mx < sm_x || mx >= sm_x + START_W || my < sm_y || my >= sm_y + START_H) return 0;
    int rel = my - sm_y - START_ITEM_Y;
    if (rel < 0) return 0;                 // the header, not an entry
    int row = rel / START_ITEM_H;
    int fi[START_ITEM_N];
    int fc = start_menu_filtered(fi);      // rows are the FILTERED entries, packed
    if (row >= fc) return 0;               // below the last matching entry
    *idx = fi[row];                        // map the visible row to its real index
    return 1;
}

// Is any non-loopback interface configured with an IP? Drives the tray's live network icon.
static int systray_online(void) {
    for (int i = 0; i < 8; i++) {
        const char* nm = net_interfaces[i].name;
        if (nm[0] && net_interfaces[i].ip != 0 &&
            !(nm[0] == 'l' && nm[1] == 'o' && nm[2] == 0))
            return 1;
    }
    return 0;
}

// A Windows-like taskbar system tray: a grouped status area (network + speaker) that
// sits just left of the user badge. The network icon is LIVE — four accent signal bars
// when an interface holds an IP, dim bars with a small red mark when offline.
#define SYSTRAY_W 48
#define TASKBAR_MOD_W 148   // live CPU%/RAM% status module, just left of the tray
static void draw_systray(int x, int tb_y) {
    int cy = tb_y + TASKBAR_H / 2;
    fb_fill_rect(x, tb_y + 4, SYSTRAY_W, TASKBAR_H - 8, col_darken(taskbar_bg_effective(), 14));  // inset panel

    // network: four ascending signal bars
    int online = systray_online();
    uint32_t on_c = fb_rgb(150, 110, 235), off_c = fb_rgb(70, 70, 80);
    int nx = x + 8, base = cy + 6;
    for (int b = 0; b < 4; b++) {
        int bh = 3 + b * 3;                                    // 3,6,9,12
        fb_fill_rect(nx + b * 4, base - bh, 3, bh, online ? on_c : off_c);
    }
    if (!online) fb_fill_rect(nx + 12, base - 12, 3, 3, fb_rgb(210, 80, 80));  // offline mark

    // speaker: a small back + a cone flaring right + two sound waves
    int sx = x + 27; uint32_t sp = fb_rgb(190, 190, 205);
    fb_fill_rect(sx, cy - 2, 3, 5, sp);                       // back/body
    for (int c = 0; c < 5; c++)                               // cone widens to the right
        fb_fill_rect(sx + 3 + c, cy - (1 + c), 1, 2 + 2 * c, sp);
    fb_fill_rect(sx + 10, cy - 3, 2, 7, sp);                  // near sound wave
    fb_fill_rect(sx + 13, cy - 5, 2, 11, sp);                 // far sound wave
    if (g_muted)                                             // a red diagonal slash when muted
        for (int i = 0; i < 16; i++) fb_fill_rect(sx + i, cy + 7 - i, 2, 2, fb_rgb(215, 80, 80));
}

// x of the system-tray inset panel — the shared source of truth for both the taskbar
// layout and the tray's click hit-testing (they must not drift).
static int systray_x(void) {
    uint32_t fw = fb_get_width();
    int av_s = TASKBAR_H - 14;
    int ublock_w = av_s + 6 + (int)strlen(g_login_user) * FONT_WIDTH + 10;
    return (int)(fw - CLOCK_W - 8) - ublock_w - SYSTRAY_W;
}

// Left edge of the live CPU/RAM status module (it sits just left of the system tray). The
// window-button strip stops here, so draw + hit-test both derive their right edge from it.
static int taskbar_mod_x(void) { return systray_x() - TASKBAR_MOD_W; }

// Is (mx,my) on the tray's network icon (its left ~half, the four signal bars)?
static int systray_net_hit(int mx, int my) {
    int tb_y = (int)fb_get_height() - TASKBAR_H;
    int sx = systray_x();
    return mx >= sx && mx < sx + 25 && my >= tb_y + 4 && my < tb_y + TASKBAR_H - 4;
}

// Is (mx,my) on the tray's speaker icon (its right ~half)?
static int systray_spk_hit(int mx, int my) {
    int tb_y = (int)fb_get_height() - TASKBAR_H;
    int sx = systray_x();
    return mx >= sx + 25 && mx < sx + SYSTRAY_W && my >= tb_y + 4 && my < tb_y + TASKBAR_H - 4;
}

// Push the current volume/mute to the Sound Blaster master mixer (register 0x22: high
// nibble = left, low nibble = right, 0..15 each). Harmless port writes if no SB16 exists.
extern int sb16_set_mixer(uint8_t reg, uint8_t value);
static void apply_volume(void) {
    int v = g_muted ? 0 : g_volume;                        // 0..100
    int nib = v * 15 / 100;                                // 0..15 per channel
    sb16_set_mixer(0x22, (uint8_t)((nib << 4) | nib));
}

static void draw_taskbar(void) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    int tb_y = fh - TASKBAR_H;

    // Subtle top-lit gradient bar with a 1px highlight along its top edge, to sit
    // with the windows' gradient title bars rather than reading as a flat slab.
    fb_fill_vgrad(0, tb_y, fw, TASKBAR_H, col_lighten(taskbar_bg_effective(), 12), col_darken(taskbar_bg_effective(), 10));
    fb_fill_rect(0, tb_y, fw, 1, col_lighten(taskbar_bg_effective(), 34));

    // The Menu button is the brand launcher, so it always wears the accent (dimmed
    // when the menu is closed, full-strength when open) instead of blending in.
    draw_grad_button(2, tb_y + 4, 80, TASKBAR_H - 8,
                     start_menu_open ? THEME_ACCENT : col_darken(THEME_ACCENT, 22),
                     fb_rgb(255, 255, 255), "Menu", 16);

    // Reserve room for the logged-in user badge (avatar + name), left of the clock,
    // and the system tray (network + speaker), left of the badge.
    int av_s = TASKBAR_H - 14;
    int ublock_w = av_s + 6 + (int)strlen(g_login_user) * FONT_WIDTH + 10;
    int tray_x = systray_x();
    int right_limit = taskbar_mod_x() - 4;   // stop the window buttons before the CPU/RAM module
    if (right_limit < 90) right_limit = 90;

    int bx = 90;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i] || !windows[i]->visible) continue;
        if (windows[i]->workspace != current_workspace && windows[i]->state != WSTATE_MINIMIZED) continue;
        int bw = 150;
        if (bx + bw > right_limit) bw = right_limit - bx;
        if (bw < 40) break;
        // Focused window tab wears the accent gradient (matching its title bar);
        // an unfocused one gets a quiet bar gradient; a minimized one a flatter,
        // greyer fill so it reads as dormant.
        int bh = TASKBAR_H - 8;
        if (windows[i]->state == WSTATE_MINIMIZED)
            fb_fill_vgrad(bx, tb_y + 4, bw, bh, fb_rgb(58, 58, 64), fb_rgb(44, 44, 50));
        else if (windows[i]->focused)
            fb_fill_vgrad(bx, tb_y + 4, bw, bh, col_lighten(taskbar_hl, 16), col_darken(taskbar_hl, 16));
        else
            fb_fill_vgrad(bx, tb_y + 4, bw, bh, col_lighten(taskbar_bg_effective(), 10), col_darken(taskbar_bg_effective(), 10));
        if (windows[i]->title[0])
            font_draw_string_trans(bx + 4, tb_y + (TASKBAR_H - FONT_HEIGHT) / 2,
                                   windows[i]->title, fb_rgb(230, 230, 235));
        // Running-app indicator hugging the button's bottom edge, like Windows 11: a
        // wide bright accent pill for the focused window, a short dim one for the other
        // open windows, and nothing for a minimized (dormant) one.
        if (windows[i]->state != WSTATE_MINIMIZED) {
            int ind_w = windows[i]->focused ? bw - 16 : 12;
            uint32_t ind_c = windows[i]->focused ? fb_rgb(150, 110, 235) : col_darken(THEME_ACCENT, 30);
            fb_fill_rect(bx + (bw - ind_w) / 2, tb_y + TASKBAR_H - 6, ind_w, 2, ind_c);
        }
        bx += bw + 2;
    }

    // Live CPU%/RAM% status module — a rice-style bar readout, refreshed each second with
    // the clock. Same honest sources as `top` / Nyx Monitor (perf_cpu_percent + mem_pool_kb).
    {
        int modx = taskbar_mod_x();
        uint32_t cpu = perf_cpu_percent();
        uint32_t mu = 0, mt = 0; mem_pool_kb(&mu, 0, &mt);
        uint32_t rampct = mt ? (uint32_t)(((uint64_t)mu * 100) / mt) : 0;
        if (cpu > 100) cpu = 100;
        if (rampct > 100) rampct = 100;
        char modbuf[40];
        snprintf(modbuf, sizeof(modbuf), "CPU %u%% RAM %u%%", cpu, rampct);
        fb_fill_rect(modx, tb_y + 4, TASKBAR_MOD_W - 4, TASKBAR_H - 8, fb_rgb(30, 30, 35));
        font_draw_string(modx + 8, tb_y + (TASKBAR_H - FONT_HEIGHT) / 2, modbuf,
                         fb_rgb(170, 150, 225), fb_rgb(30, 30, 35));   // accent-tinted gauge text
    }

    // System tray (network + speaker status), just left of the user badge.
    draw_systray(tray_x, tb_y);

    // Logged-in user badge: profile picture + username.
    int ubx = (int)(fw - CLOCK_W - 8) - ublock_w + 4;
    draw_avatar(ubx, tb_y + 7, av_s, g_login_avatar, 0);
    font_draw_string_trans(ubx + av_s + 6, tb_y + (TASKBAR_H - FONT_HEIGHT) / 2,
                           g_login_user, fb_rgb(215, 215, 235));

    rtc_time_t rt;
    rtc_read_time(&rt);
    char timebuf[32];
    // 24-hour by default; nyx.conf `clock = 12h` switches to a 12-hour AM/PM face.
    const char* clkfmt = g_clock_12h ? "%a %I:%M %p  %d/%m" : "%a %H:%M  %d/%m";
    date_format(timebuf, sizeof(timebuf), clkfmt, &rt);   // weekday computed by date_format (Sakamoto)
    fb_fill_rect(fw - CLOCK_W - 4, tb_y + 4, CLOCK_W, TASKBAR_H - 8, fb_rgb(30,30,35));
    font_draw_string(fw - CLOCK_W - 2 + (CLOCK_W - strlen(timebuf) * FONT_WIDTH) / 2,
                     tb_y + (TASKBAR_H - FONT_HEIGHT) / 2, timebuf, fb_rgb(180,180,200), fb_rgb(30,30,35));
}

// ---- Taskbar clock -> calendar popup (like Windows: click the clock for a month view) ----
#define CAL_CELL_W 26
#define CAL_CELL_H 18
#define CAL_PAD     8
#define CAL_HDR    22                                   // month/year header bar height
#define CAL_W      (7 * CAL_CELL_W + 2 * CAL_PAD)
#define CAL_H      (CAL_HDR + 7 * CAL_CELL_H + 2 * CAL_PAD)   // header + weekday row + up to 6 weeks

static int cal_popup_x(void) { return (int)fb_get_width()  - CAL_W - 4; }         // right edge, over the clock
static int cal_popup_y(void) { return (int)fb_get_height() - TASKBAR_H - CAL_H - 4; }  // just above the taskbar

// Is (mx,my) on the taskbar clock rectangle?
static int clock_hit(int mx, int my) {
    int fw = (int)fb_get_width(), tb_y = (int)fb_get_height() - TASKBAR_H;
    return mx >= fw - CLOCK_W - 4 && mx < fw - 4 && my >= tb_y + 4 && my < tb_y + TASKBAR_H - 4;
}
static int cal_popup_hit(int mx, int my) {
    if (!cal_popup_open) return 0;
    int x = cal_popup_x(), y = cal_popup_y();
    return mx >= x && mx < x + CAL_W && my >= y && my < y + CAL_H;
}

static void draw_cal_popup(void) {
    if (!cal_popup_open) return;
    rtc_time_t rt; rtc_read_time(&rt);
    int Y = (int)rt.year, M = (int)rt.month, today = (int)rt.day;
    if (M < 1 || M > 12) M = 1;

    static const unsigned char DIM[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int dim = DIM[M - 1];
    if (M == 2 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0)) dim = 29;   // leap February
    int first = day_of_week(Y, M, 1);                    // 0=Sun .. 6=Sat — the column day 1 starts in
    if (first < 0 || first > 6) first = 0;

    int x = cal_popup_x(), y = cal_popup_y();

    // Panel body + a light/dark bevel border (matches the window frames).
    fb_fill_rect(x, y, CAL_W, CAL_H, THEME_WINDOW_BG);
    fb_fill_rect(x, y, CAL_W, 1, THEME_ACCENT);
    fb_fill_rect(x, y + CAL_H - 1, CAL_W, 1, col_darken(THEME_WINDOW_BG, 30));
    fb_fill_rect(x, y, 1, CAL_H, col_lighten(THEME_WINDOW_BG, 10));
    fb_fill_rect(x + CAL_W - 1, y, 1, CAL_H, col_darken(THEME_WINDOW_BG, 30));

    // Header: "Month YYYY" centred on an accent bar (same look as the start menu).
    fb_fill_vgrad(x + 1, y + 1, CAL_W - 2, CAL_HDR - 1, THEME_ACCENT, col_darken(THEME_ACCENT, 22));
    rtc_time_t ht = rt; ht.month = (uint8_t)M; ht.year = (uint16_t)Y;   // clamped month/year for "%B %Y"
    char hdr[32]; date_format(hdr, sizeof(hdr), "%B %Y", &ht);
    font_draw_string_trans(x + (CAL_W - (int)strlen(hdr) * FONT_WIDTH) / 2,
                           y + (CAL_HDR - FONT_HEIGHT) / 2, hdr, THEME_ON_ACCENT);

    // Weekday header row.
    static const char* const WH[7] = { "Su","Mo","Tu","We","Th","Fr","Sa" };
    int gx = x + CAL_PAD, gy = y + CAL_HDR + 2;
    for (int c = 0; c < 7; c++)
        font_draw_string_trans(gx + c * CAL_CELL_W + (CAL_CELL_W - 2 * FONT_WIDTH) / 2,
                               gy + (CAL_CELL_H - FONT_HEIGHT) / 2, WH[c], fb_rgb(150,150,170));

    // Day grid: today gets an accent tile.
    int row = 0, col = first;
    for (int d = 1; d <= dim; d++) {
        int cellx = gx + col * CAL_CELL_W;
        int celly = gy + CAL_CELL_H + row * CAL_CELL_H;         // +1 row past the weekday header
        if (d == today)
            fb_fill_rect(cellx + 1, celly, CAL_CELL_W - 2, CAL_CELL_H - 1, THEME_ACCENT);
        char db[4]; snprintf(db, sizeof(db), "%d", d);
        uint32_t fg = (d == today) ? THEME_ON_ACCENT : fb_rgb(210,210,225);
        font_draw_string_trans(cellx + (CAL_CELL_W - (int)strlen(db) * FONT_WIDTH) / 2,
                               celly + (CAL_CELL_H - FONT_HEIGHT) / 2, db, fg);
        if (++col > 6) { col = 0; row++; }
    }
}

// ---- System-tray network icon -> network-status popup (like Windows' network flyout) ----
#define NET_W   200
#define NET_HDR  22
#define NET_ROW  16
#define NET_PAD   8
#define NET_H   (NET_HDR + 4 * NET_ROW + 2 * NET_PAD)     // header + up to 4 info lines

static int net_popup_x(void) { return (int)fb_get_width()  - NET_W - 4; }
static int net_popup_y(void) { return (int)fb_get_height() - TASKBAR_H - NET_H - 4; }

static int net_popup_hit(int mx, int my) {
    if (!net_popup_open) return 0;
    int x = net_popup_x(), y = net_popup_y();
    return mx >= x && mx < x + NET_W && my >= y && my < y + NET_H;
}

// A small flyout showing the first live (non-loopback) interface's IP / mask / gateway,
// or "Not connected" when the machine is offline. Same panel styling as the calendar.
static void draw_net_popup(void) {
    if (!net_popup_open) return;
    int x = net_popup_x(), y = net_popup_y();

    fb_fill_rect(x, y, NET_W, NET_H, THEME_WINDOW_BG);
    fb_fill_rect(x, y, NET_W, 1, THEME_ACCENT);
    fb_fill_rect(x, y + NET_H - 1, NET_W, 1, col_darken(THEME_WINDOW_BG, 30));
    fb_fill_rect(x, y, 1, NET_H, col_lighten(THEME_WINDOW_BG, 10));
    fb_fill_rect(x + NET_W - 1, y, 1, NET_H, col_darken(THEME_WINDOW_BG, 30));

    fb_fill_vgrad(x + 1, y + 1, NET_W - 2, NET_HDR - 1, THEME_ACCENT, col_darken(THEME_ACCENT, 22));
    font_draw_string_trans(x + NET_PAD, y + (NET_HDR - FONT_HEIGHT) / 2, "Network", THEME_ON_ACCENT);

    int tx = x + NET_PAD, ty = y + NET_HDR + NET_PAD, line = 0, shown = 0;
    char buf[48];
    for (int i = 0; i < 8 && !shown; i++) {
        net_iface_t* nf = &net_interfaces[i];
        if (!nf->name[0] || nf->ip == 0) continue;
        if (nf->name[0] == 'l' && nf->name[1] == 'o' && nf->name[2] == 0) continue;   // skip loopback
        font_draw_string_trans(tx, ty + line++ * NET_ROW, nf->name, fb_rgb(150, 110, 235));
        snprintf(buf, sizeof(buf), "IP    %d.%d.%d.%d", IP4_OCTETS(nf->ip));
        font_draw_string_trans(tx, ty + line++ * NET_ROW, buf, fb_rgb(215, 215, 225));
        snprintf(buf, sizeof(buf), "Mask  %d.%d.%d.%d", IP4_OCTETS(nf->netmask));
        font_draw_string_trans(tx, ty + line++ * NET_ROW, buf, fb_rgb(215, 215, 225));
        snprintf(buf, sizeof(buf), "GW    %d.%d.%d.%d", IP4_OCTETS(nf->gateway));
        font_draw_string_trans(tx, ty + line++ * NET_ROW, buf, fb_rgb(215, 215, 225));
        shown = 1;
    }
    if (!shown)
        font_draw_string_trans(tx, ty, "Not connected", fb_rgb(210, 120, 120));
}

// ---- System-tray speaker icon -> sound/volume popup (like Windows' volume flyout) ----
#define SPK_W       200
#define SPK_HDR      22
#define SPK_PAD       8
#define SPK_TRACK_H  10
#define SPK_MUTE_W   96
#define SPK_MUTE_H   20
#define SPK_H       (SPK_HDR + 66)                     // header + slider + mute button + padding

static int spk_popup_x(void) { return (int)fb_get_width()  - SPK_W - 4; }
static int spk_popup_y(void) { return (int)fb_get_height() - TASKBAR_H - SPK_H - 4; }
static int spk_track_x(void) { return spk_popup_x() + SPK_PAD; }
static int spk_track_y(void) { return spk_popup_y() + SPK_HDR + SPK_PAD + 12; }
static int spk_track_w(void) { return SPK_W - 2 * SPK_PAD; }
static int spk_mute_x(void)  { return spk_popup_x() + SPK_PAD; }
static int spk_mute_y(void)  { return spk_popup_y() + SPK_H - SPK_PAD - SPK_MUTE_H; }

static int spk_popup_hit(int mx, int my) {
    if (!spk_popup_open) return 0;
    int x = spk_popup_x(), y = spk_popup_y();
    return mx >= x && mx < x + SPK_W && my >= y && my < y + SPK_H;
}

// The sound flyout: a draggable master-volume track (click to set) and a mute toggle.
static void draw_spk_popup(void) {
    if (!spk_popup_open) return;
    int x = spk_popup_x(), y = spk_popup_y();

    fb_fill_rect(x, y, SPK_W, SPK_H, THEME_WINDOW_BG);
    fb_fill_rect(x, y, SPK_W, 1, THEME_ACCENT);
    fb_fill_rect(x, y + SPK_H - 1, SPK_W, 1, col_darken(THEME_WINDOW_BG, 30));
    fb_fill_rect(x, y, 1, SPK_H, col_lighten(THEME_WINDOW_BG, 10));
    fb_fill_rect(x + SPK_W - 1, y, 1, SPK_H, col_darken(THEME_WINDOW_BG, 30));

    fb_fill_vgrad(x + 1, y + 1, SPK_W - 2, SPK_HDR - 1, THEME_ACCENT, col_darken(THEME_ACCENT, 22));
    font_draw_string_trans(x + SPK_PAD, y + (SPK_HDR - FONT_HEIGHT) / 2, "Sound", THEME_ON_ACCENT);
    // volume % (or "muted") on the right of the header
    char pct[8];
    if (g_muted) { snprintf(pct, sizeof(pct), "muted"); }
    else { snprintf(pct, sizeof(pct), "%d", g_volume); uint32_t l = 0; while (pct[l]) l++;
           if (l < sizeof(pct) - 1) { pct[l] = '%'; pct[l + 1] = 0; } }
    font_draw_string_trans(x + SPK_W - SPK_PAD - (int)strlen(pct) * FONT_WIDTH,
                           y + (SPK_HDR - FONT_HEIGHT) / 2, pct, THEME_ON_ACCENT);

    // slider track + accent fill + knob
    int tx = spk_track_x(), ty = spk_track_y(), tw = spk_track_w();
    fb_fill_rect(tx, ty, tw, SPK_TRACK_H, col_darken(THEME_WINDOW_BG, 20));
    int fill = g_muted ? 0 : (g_volume * tw / 100);
    if (fill > 0) fb_fill_rect(tx, ty, fill, SPK_TRACK_H, THEME_ACCENT);
    int kx = tx + fill; if (kx > tx + tw - 4) kx = tx + tw - 4; if (kx < tx) kx = tx;
    fb_fill_rect(kx, ty - 3, 4, SPK_TRACK_H + 6, fb_rgb(222, 222, 236));

    // mute toggle button
    int bx = spk_mute_x(), by = spk_mute_y();
    uint32_t bfill = g_muted ? fb_rgb(150, 60, 60) : col_darken(THEME_WINDOW_BG, 12);
    fb_fill_rect(bx, by, SPK_MUTE_W, SPK_MUTE_H, bfill);
    fb_fill_rect(bx, by, SPK_MUTE_W, 1, col_lighten(bfill, 24));
    const char* ml = g_muted ? "Unmute" : "Mute";
    font_draw_string_trans(bx + (SPK_MUTE_W - (int)strlen(ml) * FONT_WIDTH) / 2,
                           by + (SPK_MUTE_H - FONT_HEIGHT) / 2, ml, fb_rgb(230, 230, 240));
}

static void draw_start_menu(void) {
    if (!start_menu_open) return;
    uint32_t fh = fb_get_height();
    int sm_x = 2, sm_y = fh - TASKBAR_H - START_H;

    int R = g_corner_radius;   // follow the nyx.conf `rounding` knob (0 = square menu top)

    // Rounded-top body. The bottom sits flush on the taskbar, so it stays square
    // there — the menu reads as growing out of the bar. Corner-outside pixels are
    // left unpainted (same technique as the window title bars), showing whatever
    // is behind the menu through the curve.
    for (int row = 0; row < START_H; row++) {
        int inset = (row < R) ? fb_corner_inset(row, R) : 0;
        fb_fill_rect(sm_x + inset, sm_y + row, START_W - 2 * inset, 1, THEME_WINDOW_BG);
    }

    // Brand header: accent gradient (matching the taskbar Menu button and title
    // bars), rounded to the same top corners, with a transparent label so the
    // gradient shows through the text.
    uint32_t hg_t = col_lighten(THEME_ACCENT, 14), hg_b = col_darken(THEME_ACCENT, 12);
    for (int row = 0; row < START_HDR_H; row++) {
        int inset = (row < R) ? fb_corner_inset(row, R) : 0;
        fb_fill_rect(sm_x + inset, sm_y + row, START_W - 2 * inset, 1,
                     vgrad_row(hg_t, hg_b, row, START_HDR_H));
    }
    if (start_filter_len > 0) {                 // type-to-search: show the live query
        char hdr[32];
        int p = 0; hdr[p++] = '>'; hdr[p++] = ' ';
        for (int i = 0; i < start_filter_len && p < (int)sizeof(hdr) - 2; i++) hdr[p++] = start_filter[i];
        hdr[p++] = '_'; hdr[p] = '\0';
        font_draw_string_trans(sm_x + 8, sm_y + 4, hdr, THEME_ON_ACCENT);
    } else {
        font_draw_string_trans(sm_x + 8, sm_y + 4, "NyxOS Menu", THEME_ON_ACCENT);
    }

    // Index order here IS the argument do_start_menu_action() switches on, and
    // start_menu_item_hit() computes that index from the same START_ITEM_*
    // constants this loop lays the entries out with.
    // The entry under the pointer is highlighted with the accent, like a hovered
    // menu item (the event loop recomposites on pointer move while the menu is
    // open, so this tracks the cursor). hov is a REAL item index.
    int hov = -1, hi;
    if (start_menu_item_hit(mouse_x, mouse_y, &hi)) hov = hi;

    // A small colour "chip" carrying each entry's initial gives every item an
    // icon-like glyph — the emblems in draw_icon_at are ~56 px, too big for a 26 px
    // row — so the menu reads like a modern app launcher, not a plain text list.
    const uint32_t chip_cols[START_ITEM_N] = {
        fb_rgb(80,140,215),  fb_rgb(230,170,60),  fb_rgb(70,175,175), fb_rgb(60,150,90),
        fb_rgb(130,135,150), fb_rgb(210,95,80),   fb_rgb(150,120,235),
        fb_rgb(210,90,150),  fb_rgb(90,170,235),  fb_rgb(110,110,200), fb_rgb(200,70,70), fb_rgb(90,180,120),
        fb_rgb(170,100,210), fb_rgb(190,110,235),
    };

    // Only the entries matching the type-to-search filter are shown, packed from the
    // top: visible row r draws start_items[fidx[r]].
    int fidx[START_ITEM_N];
    int fcount = start_menu_filtered(fidx);
    for (int r = 0; r < fcount; r++) {
        int i = fidx[r];
        int sel = (i == hov) || (start_nav && r == start_sel);   // hover OR keyboard selection
        int iy = sm_y + START_ITEM_Y + r * START_ITEM_H;
        if ((uint32_t)(iy + START_ITEM_H) > fh - TASKBAR_H) break;
        if (sel)
            fb_fill_vgrad(sm_x + 4, iy, START_W - 8, START_ITEM_H - 2,
                          col_lighten(THEME_ACCENT, 12), col_darken(THEME_ACCENT, 14));
        else
            fb_fill_rect(sm_x + 4, iy, START_W - 8, START_ITEM_H - 2, THEME_WINDOW_BG);

        // icon chip: the app's tint + its initial letter, vertically centred in the row
        int cw = 18, cx = sm_x + 7, cy = iy + (START_ITEM_H - 2 - cw) / 2;
        fb_fill_rect(cx, cy, cw, cw, chip_cols[i]);
        fb_fill_rect(cx, cy, cw, 1, col_lighten(chip_cols[i], 40));   // top highlight
        char ini[2] = { start_items[i][0], '\0' };
        font_draw_string_trans(cx + (cw - FONT_WIDTH) / 2, cy + (cw - FONT_HEIGHT) / 2, ini, fb_rgb(255, 255, 255));

        // label, to the right of the chip
        if (sel) font_draw_string_trans(sm_x + 30, iy + 5, start_items[i], THEME_ON_ACCENT);
        else     font_draw_string(sm_x + 30, iy + 5, start_items[i], THEME_TEXT, THEME_WINDOW_BG);

        fb_fill_rect(sm_x + 4, iy + START_ITEM_H - 1, START_W - 8, 1, THEME_ROW_DIV);
    }
    if (fcount == 0)   // no entry matches the current query
        font_draw_string(sm_x + 12, sm_y + START_ITEM_Y + 5, "(no matches)", fb_rgb(150,150,160), THEME_WINDOW_BG);

    // Border: rounded top corners to match the body, straight sides, square bottom
    // where it meets the taskbar.
    fb_fill_rect(sm_x + R, sm_y, START_W - 2 * R, 1, THEME_BORDER);              // top
    fb_fill_rect(sm_x, sm_y + START_H - 1, START_W, 1, THEME_BORDER);           // bottom
    fb_fill_rect(sm_x, sm_y + R, 1, START_H - R, THEME_BORDER);                 // left
    fb_fill_rect(sm_x + START_W - 1, sm_y + R, 1, START_H - R, THEME_BORDER);   // right
    for (int row = 0; row < R; row++) {
        int inset = fb_corner_inset(row, R);
        fb_put_pixel((uint32_t)(sm_x + inset),              (uint32_t)(sm_y + row), THEME_BORDER);
        fb_put_pixel((uint32_t)(sm_x + START_W - 1 - inset), (uint32_t)(sm_y + row), THEME_BORDER);
    }
}

// ---- Taskbar user-badge popup menu (change profile picture / log out) ----
#define USERMENU_W   150
#define USERMENU_ITH 26
#define USERMENU_HDR 24
#define USERMENU_N   2
static const char* usermenu_items[] = { "Change picture", "Log out" };

static void user_menu_rect(int* rx, int* ry, int* rh) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    int h = USERMENU_HDR + USERMENU_N * USERMENU_ITH + 2;
    int x = (int)(fw - CLOCK_W - 8) - USERMENU_W;
    if (x < 2) x = 2;
    *rx = x; *ry = (int)(fh - TASKBAR_H) - h; if (rh) *rh = h;
}

static int user_menu_item_hit(int mx, int my, int* idx);   // defined below
static int ctx_menu_item_hit(int mx, int my, int* idx);    // defined below

static void draw_user_menu(void) {
    if (!user_menu_open) return;
    int x, y, h; user_menu_rect(&x, &y, &h);
    int R = 6;
    fb_fill_round_rect(x, y, USERMENU_W, h, R, THEME_WINDOW_BG);

    // header: accent gradient (rounded to the body's top corners) carrying the
    // logged-in user's avatar + name, with a transparent label.
    uint32_t hg_t = col_lighten(THEME_ACCENT, 14), hg_b = col_darken(THEME_ACCENT, 12);
    for (int row = 0; row < USERMENU_HDR; row++) {
        int inset = (row < R) ? fb_corner_inset(row, R) : 0;
        fb_fill_rect(x + inset, y + row, USERMENU_W - 2 * inset, 1,
                     vgrad_row(hg_t, hg_b, row, USERMENU_HDR));
    }
    draw_avatar(x + 5, y + 3, USERMENU_HDR - 6, g_login_avatar, 0);
    font_draw_string_trans(x + USERMENU_HDR + 6, y + (USERMENU_HDR - FONT_HEIGHT) / 2,
                           g_login_user, THEME_ON_ACCENT);

    int hov = -1, hi;
    if (user_menu_item_hit(mouse_x, mouse_y, &hi)) hov = hi;
    for (int i = 0; i < USERMENU_N; i++) {
        int iy = y + USERMENU_HDR + i * USERMENU_ITH;
        if (i == hov) {
            fb_fill_vgrad(x + 3, iy, USERMENU_W - 6, USERMENU_ITH - 1,
                          col_lighten(THEME_ACCENT, 12), col_darken(THEME_ACCENT, 14));
            font_draw_string_trans(x + 12, iy + (USERMENU_ITH - FONT_HEIGHT) / 2,
                                   usermenu_items[i], THEME_ON_ACCENT);
        } else {
            font_draw_string(x + 12, iy + (USERMENU_ITH - FONT_HEIGHT) / 2,
                             usermenu_items[i], THEME_TEXT, THEME_WINDOW_BG);
        }
    }
    fb_stroke_round_rect(x, y, USERMENU_W, h, R, THEME_BORDER);
}

// The taskbar user badge (avatar + name, left of the clock) is clickable; this
// mirrors the geometry draw_taskbar uses for it.
static int badge_hit(int mx, int my) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    int tb_y = (int)fh - TASKBAR_H;
    int av_s = TASKBAR_H - 14;
    int ublock_w = av_s + 6 + (int)strlen(g_login_user) * FONT_WIDTH + 10;
    int ubx = (int)(fw - CLOCK_W - 8) - ublock_w + 4;
    return my >= tb_y && mx >= ubx - 4 && mx < (int)(fw - CLOCK_W - 8);
}

static int user_menu_item_hit(int mx, int my, int* idx) {
    if (!user_menu_open) return 0;
    int x, y; user_menu_rect(&x, &y, 0);
    for (int i = 0; i < USERMENU_N; i++) {
        int iy = y + USERMENU_HDR + i * USERMENU_ITH;
        if (mx >= x && mx < x + USERMENU_W && my >= iy && my < iy + USERMENU_ITH) { *idx = i; return 1; }
    }
    return 0;
}

static int user_menu_area_hit(int mx, int my) {
    if (!user_menu_open) return 0;
    int x, y, h; user_menu_rect(&x, &y, &h);
    return mx >= x && mx < x + USERMENU_W && my >= y && my < y + h;
}

static void do_user_menu_action(int idx) {
    if (idx == 0) {            // Change picture: cycle to the next avatar + persist
        g_login_avatar = (g_login_avatar + 1) % AVATAR_COUNT;
        auth_set_avatar(g_login_user, g_login_avatar);
    } else if (idx == 1) {     // Log out: unwind to the login screen (handled in the boot loop)
        compositor_logout_requested = 1;
        quit = 1;
    }
}

#define CTX_MENU_W 160
#define CTX_MENU_H 124
#define CTX_MENU_N 5
static const char* ctx_menu_items[] = {
    "New Folder", "New File", "Refresh", "Settings", "Wallpaper"
};

// Forward declarations
static void redraw_all(void);
static void notify_render(uint32_t now);        // desktop toasts, drawn on top
static int  notify_active_count(uint32_t now);  // live-toast count (loop repaint gate)
static void draw_snap_preview(void);
static void do_start_menu_action(int idx);
static void settings_win_click(window_t* win, int mx, int my, int btn);

static void draw_ctx_menu(void) {
    if (!ctx_menu_open) return;
    int x = ctx_menu_x, y = ctx_menu_y;
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    if (x + CTX_MENU_W > (int)fw) x = (int)fw - CTX_MENU_W - 2;
    if (y + CTX_MENU_H > (int)fh - TASKBAR_H) y = (int)fh - TASKBAR_H - CTX_MENU_H - 2;
    int R = 6;
    fb_fill_round_rect(x, y, CTX_MENU_W, CTX_MENU_H, R, THEME_WINDOW_BG);

    int hov = -1, hi;
    if (ctx_menu_item_hit(mouse_x, mouse_y, &hi)) hov = hi;
    for (int i = 0; i < CTX_MENU_N; i++) {
        int iy = y + 4 + i * 24;
        if (i == hov) {
            fb_fill_vgrad(x + 4, iy, CTX_MENU_W - 8, 22,
                          col_lighten(THEME_ACCENT, 12), col_darken(THEME_ACCENT, 14));
            font_draw_string_trans(x + 12, iy + 3, ctx_menu_items[i], THEME_ON_ACCENT);
        } else {
            font_draw_string(x + 12, iy + 3, ctx_menu_items[i], THEME_TEXT, THEME_WINDOW_BG);
        }
    }
    fb_stroke_round_rect(x, y, CTX_MENU_W, CTX_MENU_H, R, THEME_BORDER);
}

static int ctx_menu_hit(int mx, int my) {
    if (!ctx_menu_open) return 0;
    int x = ctx_menu_x, y = ctx_menu_y;
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    if (x + CTX_MENU_W > (int)fw) x = (int)fw - CTX_MENU_W - 2;
    if (y + CTX_MENU_H > (int)fh - TASKBAR_H) y = (int)fh - TASKBAR_H - CTX_MENU_H - 2;
    return mx >= x && mx < x + CTX_MENU_W && my >= y && my < y + CTX_MENU_H;
}

static int ctx_menu_item_hit(int mx, int my, int* idx) {
    if (!ctx_menu_open) return 0;
    int x = ctx_menu_x, y = ctx_menu_y;
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    if (x + CTX_MENU_W > (int)fw) x = (int)fw - CTX_MENU_W - 2;
    if (y + CTX_MENU_H > (int)fh - TASKBAR_H) y = (int)fh - TASKBAR_H - CTX_MENU_H - 2;
    if (mx < x || mx >= x + CTX_MENU_W || my < y || my >= y + CTX_MENU_H) return 0;
    *idx = (my - y - 4) / 24;
    if (*idx < 0 || *idx >= CTX_MENU_N) return 0;
    return 1;
}

static void do_ctx_menu_action(int idx) {
    ctx_menu_open = 0;
    switch (idx) {
        case 0: // New Folder
            {
                // Find an existing file manager window or create one
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (windows[i] && strcmp(windows[i]->title, "File Manager") == 0) {
                        fileman_new_folder((fileman_win_t*)windows[i]->reserved);
                        redraw_all();
                        return;
                    }
                }
                // Create File Manager which will show the dialog
                window_t* fwin = window_create(100, 100, 550, 380, "File Manager", fileman_win_draw);
                if (fwin) {
                    fwin->reserved = fileman_create_ctx();
                    if (fwin->reserved) {
                        fwin->on_click = fileman_win_click;
                        fwin->on_key = fileman_win_key;
                        fwin->on_mousemove = fileman_win_mousemove;
                        fileman_new_folder((fileman_win_t*)fwin->reserved);
                    }
                }
            }
            break;
        case 1: // New File
            {
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (windows[i] && strcmp(windows[i]->title, "File Manager") == 0) {
                        fileman_new_file((fileman_win_t*)windows[i]->reserved);
                        redraw_all();
                        return;
                    }
                }
                window_t* fwin = window_create(100, 100, 550, 380, "File Manager", fileman_win_draw);
                if (fwin) {
                    fwin->reserved = fileman_create_ctx();
                    if (fwin->reserved) {
                        fwin->on_click = fileman_win_click;
                        fwin->on_key = fileman_win_key;
                        fwin->on_mousemove = fileman_win_mousemove;
                        fileman_new_file((fileman_win_t*)fwin->reserved);
                    }
                }
            }
            break;
        case 2: // Refresh
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (windows[i] && strcmp(windows[i]->title, "File Manager") == 0 && windows[i]->reserved) {
                    fileman_refresh((fileman_win_t*)windows[i]->reserved);
                }
            }
            break;
        case 3: // Settings
            do_start_menu_action(4);
            break;
        case 4: // Wallpaper
            {
                window_t* wwin = window_create(180, 96, 400, 520, "Wallpaper", wallpaper_win_draw);
                if (wwin) wwin->on_click = wallpaper_win_click;
            }
            break;
    }
    redraw_all();
}

static void draw_workspace_indicator(void) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    int y = fh - TASKBAR_H - 10;
    for (int i = 0; i < WORKSPACE_COUNT; i++) {
        uint32_t c = (i == current_workspace) ? THEME_INDICATOR_ON : THEME_INDICATOR_OFF;
        fb_fill_rect(fw - CLOCK_W - 4 + i * 18, y, 14, 6, c);
    }
}

// The desktop background: a smooth vertical gradient built from the wallpaper base
// color the user picked (default = the brand purple). Each band scales the base's
// brightness from ~45% at the top to ~115% at the bottom (clamped), which reads as a
// soft top-to-bottom glow in whatever hue is selected. The Wallpaper app changes the
// base color (wallpaper_base_color) and the compositor repaints this on the next frame.
// Filled circle by horizontal spans (clipped to the framebuffer). Uses fb_isqrt
// (the kernel has no libm) — the same integer sqrt the rounded corners rely on.
static void bg_fill_circle(int cx, int cy, int rad, uint32_t col) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    for (int dy = -rad; dy <= rad; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= (int)fh) continue;
        int dx = (int)fb_isqrt((uint32_t)(rad * rad - dy * dy));
        int x0 = cx - dx, x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= (int)fw) x1 = (int)fw - 1;
        if (x1 >= x0) fb_fill_rect(x0, yy, x1 - x0 + 1, 1, col);
    }
}

// The desktop background, painted first each frame behind the icons and windows.
// The wallpaper base color + style (see the Wallpaper app) decide the look:
// CLEAN = a soft vertical gradient of the hue (the v6 default); NIGHTFALL = that
// gradient overlaid with a glowing moon and a deterministic star field (the classic
// scene); FLAT = a single solid fill. The star layout is seeded by a fixed constant
// so it is identical on every repaint (no flicker).
// Integer sine for the aurora curtains: phase 0..255 (a full turn) -> -1024..1024.
// Parabolic approximation — no float, no table — two mirrored parabolas per turn.
static int wp_isin(int ph) {
    ph &= 255;
    int sign = 1;
    if (ph >= 128) { ph -= 128; sign = -1; }   // second half mirrors + negates
    int y = ph * (128 - ph);                    // 0..4096 parabola, peak at ph=64
    return sign * (y / 4);                       // -1024..1024
}

// One expanding moonlight ripple: a thin ring (integer midpoint circle) centered on
// the moon; each plotted pixel blends from the LOCAL sky gradient toward a soft lilac
// by `inten` (0..100), so the ring melts into the night with no hard edge. Off-screen
// octant points are clipped. Used by the ONDAS wallpaper style.
static void bg_ripple_ring(int cx, int cy, int r, int fw, int fh,
                           int br, int bgc, int bb, int inten) {
    if (r < 1 || inten <= 0) return;
    int x = r, y = 0, err = 0;
    while (x >= y) {
        int pts[8][2] = {
            { cx + x, cy + y }, { cx + y, cy + x }, { cx - y, cy + x }, { cx - x, cy + y },
            { cx - x, cy - y }, { cx - y, cy - x }, { cx + y, cy - x }, { cx + x, cy - y },
        };
        for (int p = 0; p < 8; p++) {
            int px = pts[p][0], py = pts[p][1];
            if (px < 0 || px >= fw - 1 || py < 0 || py >= fh - 1) continue;
            int pct = 45 + py * 70 / fh;
            int skr = br * pct / 100, skg = bgc * pct / 100, skb = bb * pct / 100;
            int rr = skr + (214 - skr) * inten / 100;
            int gg = skg + (200 - skg) * inten / 100;
            int bl = skb + (248 - skb) * inten / 100;
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bl > 255) bl = 255;
            fb_fill_rect(px, py, 2, 2, fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bl));
        }
        y++; err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0) { x--; err += 1 - 2 * x; }
    }
}

// A faint constellation edge: an integer Bresenham line whose every pixel blends from
// the LOCAL sky gradient toward a soft lilac by `inten` (0..100), so the line reads as a
// delicate thread between stars rather than a hard stroke. Used by the CONSTELACIONES
// (Astral) wallpaper style.
static void bg_star_line(int x0, int y0, int x1, int y1, int fw, int fh,
                         int br, int bgc, int bb, int inten) {
    int dx = (x1 > x0) ? x1 - x0 : x0 - x1, sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1), sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < fw && y0 >= 0 && y0 < fh) {
            int pct = 45 + y0 * 70 / fh;
            int skr = br * pct / 100, skg = bgc * pct / 100, skb = bb * pct / 100;
            int rr = skr + (206 - skr) * inten / 100;
            int gg = skg + (196 - skg) * inten / 100;
            int bl = skb + (240 - skb) * inten / 100;
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bl > 255) bl = 255;
            fb_fill_rect(x0, y0, 1, 1, fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bl));
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Does the current wallpaper style animate (twinkle / meteor / aurora …) — i.e. does the
// desktop repaint every frame on its own? The drag partial-present falls back to a full
// present for these, since an animated background changes OUTSIDE the dragged window's rect.
static int wallpaper_animated(void) {
    int s = wallpaper_style();
    return s == WP_STYLE_STARFIELD || s == WP_STYLE_SHOOTINGSTAR || s == WP_STYLE_AURORA ||
           s == WP_STYLE_NEBULA || s == WP_STYLE_LUCES || s == WP_STYLE_ONDAS ||
           s == WP_STYLE_LLUVIA;
}

static void draw_background(void) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    uint32_t base = wallpaper_base_color();
    int br = (int)((base >> 16) & 0xFF), bg = (int)((base >> 8) & 0xFF), bb = (int)(base & 0xFF);
    int style = wallpaper_style();

    // WP_STYLE_FLAT: a single solid fill — the cleanest possible desktop.
    if (style == WP_STYLE_FLAT) {
        fb_fill_rect(0, 0, fw, fh, base);
        return;
    }

    // The vertical gradient is shared by the CLEAN and NIGHTFALL styles.
    uint32_t steps = 96;
    uint32_t band_h = fh / steps + 1;
    for (uint32_t i = 0; i < steps; i++) {
        int pct = 45 + (int)(i * 70 / steps);      // 45% (top) .. 115% (bottom)
        int r = br * pct / 100; if (r > 255) r = 255;
        int g = bg * pct / 100; if (g > 255) g = 255;
        int b = bb * pct / 100; if (b > 255) b = 255;
        fb_fill_rect(0, i * band_h, fw, band_h, fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }

    // WP_STYLE_CLEAN stops here: just the gradient. NIGHTFALL, STARFIELD and
    // SHOOTINGSTAR all go on to paint the moon + star field (STARFIELD additionally
    // twinkles; SHOOTINGSTAR keeps the calm sky and adds a periodic meteor below).
    if (style != WP_STYLE_NIGHTFALL && style != WP_STYLE_STARFIELD &&
        style != WP_STYLE_SHOOTINGSTAR && style != WP_STYLE_AURORA &&
        style != WP_STYLE_NEBULA && style != WP_STYLE_LUCES &&
        style != WP_STYLE_ONDAS && style != WP_STYLE_CONSTELACIONES &&
        style != WP_STYLE_LLUVIA && style != WP_STYLE_CORDILLERA) return;

    // Moon in the upper-right, with a soft halo (concentric rings fading inward).
    int mx = (int)fw - 130, my = 96, mr = 40;
    bg_fill_circle(mx, my, mr + 14, fb_rgb(70,  60,  104));
    bg_fill_circle(mx, my, mr + 8,  fb_rgb(104, 92,  150));
    bg_fill_circle(mx, my, mr + 3,  fb_rgb(150, 138, 196));
    bg_fill_circle(mx, my, mr,      fb_rgb(214, 202, 244));
    // Faint craters for a little character.
    bg_fill_circle(mx - 12, my - 8,  6, fb_rgb(196, 184, 228));
    bg_fill_circle(mx + 10, my + 12, 8, fb_rgb(198, 186, 230));
    bg_fill_circle(mx + 4,  my - 16, 4, fb_rgb(200, 188, 232));

    // Stars — deterministic LCG, upper ~3/4 of the screen, kept clear of the moon.
    // Positions are fixed (seeded by a constant) so they never jump; only the
    // brightness moves. STARFIELD twinkles: each star's luminance rides a slow
    // triangle wave phased by its own seed, sampled from get_ticks() (~2.5 s period),
    // so the compositor's periodic repaint makes the sky shimmer. NIGHTFALL keeps
    // the three fixed shades and never forces a repaint (a still, quiet night).
    int animate = (style == WP_STYLE_STARFIELD);
    uint32_t t = animate ? get_ticks() : 0;
    uint32_t seed = 0x9E3779B1u;
    uint32_t star_zone = fh * 3 / 4;
    for (int i = 0; i < 110; i++) {
        seed = seed * 1103515245u + 12345u;
        int sx = (int)((seed >> 9) % fw);
        seed = seed * 1103515245u + 12345u;
        int sy = (int)((seed >> 9) % star_zone);
        int ex = sx - mx, ey = sy - my;
        if (ex * ex + ey * ey < (mr + 18) * (mr + 18)) continue;   // don't scatter over the moon
        uint32_t sc; int sz;
        if (animate) {
            int off = (int)((seed >> 12) & 63);
            int ph  = ((int)(t / 40) + off) & 63;                  // 0..63 cycle (~2.5 s)
            int tri = (ph < 32) ? ph : (64 - ph);                  // 0..32 triangle
            int lum = 70 + tri * 6; if (lum > 255) lum = 255;      // dim -> bright pulse
            sc = fb_rgb((uint8_t)(lum * 88 / 100), (uint8_t)(lum * 84 / 100), (uint8_t)lum);
            sz = (tri > 24) ? 2 : 1;                               // brightest moments swell to 2px
        } else {
            int shade = (int)((seed >> 20) & 3);
            sc = (shade == 0) ? fb_rgb(120, 112, 156)
               : (shade == 1) ? fb_rgb(168, 158, 202)
                              : fb_rgb(214, 208, 240);
            sz = (shade >= 2) ? 2 : 1;
        }
        fb_fill_rect(sx, sy, sz, sz, sc);
    }

    // SHOOTINGSTAR: a periodic meteor streaks across the otherwise calm sky. Integer
    // only — its timing and path come from get_ticks(), and each meteor's launch
    // point is varied by an integer hash of the period index so they don't all trace
    // the same line. The compositor forces a faster repaint while this style is
    // selected (below), so the streak reads as smooth motion.
    if (style == WP_STYLE_SHOOTINGSTAR) {
        uint32_t tms = get_ticks();
        const uint32_t PERIOD = 3600, STREAK = 900;     // one ~0.9 s streak every ~3.6 s
        uint32_t ph = tms % PERIOD;
        if (ph < STREAK) {
            uint32_t h = (tms / PERIOD) * 2654435761u;  // hash the meteor index -> varied path
            int startx = (int)(fw / 2) + (int)(h % (fw / 3));          // launch in the upper right
            int starty = 24 + (int)((h >> 9) % (star_zone / 3));
            int span   = (int)fw * 3 / 5;                             // travel distance
            int prog   = (int)(ph * 256u / STREAK);                   // 0..256 along the streak
            int hx     = startx - span * prog / 256;                  // move down-LEFT...
            int hy     = starty + (span / 2) * prog / 256;            // ...and gently downward
            // A dense, continuous streak: the head leads at (hx,hy) and the tail trails
            // up-right (opposite the 2:1 down-left motion) as a fading line ~44 px long.
            for (int s = 0; s <= 44; s++) {
                int tx = hx + 2 * s;                                  // 2:1 diagonal, adjacent points
                int ty = hy - s;
                if (tx < 0 || tx >= (int)fw || ty < 0 || ty >= (int)fh) continue;
                int lum = 255 - s * 5; if (lum < 30) lum = 30;       // bright head -> faded tail
                int sz = (s < 2) ? 3 : (s < 12 ? 2 : 1);             // fat glowing head, thinning tail
                fb_fill_rect(tx, ty, sz, sz,
                             fb_rgb((uint8_t)(lum * 92 / 100), (uint8_t)(lum * 96 / 100), (uint8_t)lum));
            }
        }
    }

    // AURORA: slow drifting curtains of green-violet light over the mid sky. Integer
    // only — each curtain's crest y rides a sum of two slow sines sampled from
    // get_ticks(), and the glow blends from the LOCAL sky gradient toward the curtain
    // hue by an intensity that fades vertically from a bright core to nothing, so it
    // layers over the gradient + stars with no hard edges (blending from the sky, not
    // a fixed base, keeps the faded margins invisible). Green/teal cores with a violet
    // curtain keep the Nyx night palette.
    if (style == WP_STYLE_AURORA) {
        int drift = (int)(get_ticks() / 24);            // slow horizontal phase advance
        struct { int basey; int amp; int half; int ph; int cr, cg, cb; } cur[3] = {
            { (int)fh * 30 / 100, 26, 44,   0,  90, 225, 150 },   // green, higher/thin
            { (int)fh * 42 / 100, 34, 58, 100, 130, 150, 235 },   // violet, lower/taller
            { (int)fh * 36 / 100, 20, 34, 200,  90, 215, 205 },   // teal, mid
        };
        for (int k = 0; k < 3; k++) {
            for (int x = 0; x < (int)fw; x += 3) {
                int a1 = wp_isin((x * 3 + drift + cur[k].ph) & 255);
                int a2 = wp_isin((x * 7 - drift / 2) & 255);
                int crest = cur[k].basey + cur[k].amp * a1 / 1024 + cur[k].amp * a2 / 2048;
                int half = cur[k].half;
                for (int dy = -half; dy <= half; dy += 2) {
                    int yy = crest + dy;
                    if (yy < 0 || yy >= (int)fh) continue;
                    int ady = dy < 0 ? -dy : dy;
                    int fade = 100 - ady * 100 / half;           // 100 core -> 0 edge
                    if (fade <= 4) continue;
                    int sh = 78 + wp_isin((x * 5 + drift * 2 + cur[k].ph) & 255) * 22 / 1024;
                    int lum = fade * sh / 100;                    // 0..100 glow strength
                    int pct = 45 + yy * 70 / (int)fh;            // local sky gradient %
                    int skr = br * pct / 100, skg = bg * pct / 100, skb = bb * pct / 100;
                    int rr = skr + (cur[k].cr - skr) * lum / 100;
                    int gg = skg + (cur[k].cg - skg) * lum / 100;
                    int bb2 = skb + (cur[k].cb - skb) * lum / 100;
                    if (rr > 255) rr = 255;
                    if (gg > 255) gg = 255;
                    if (bb2 > 255) bb2 = 255;
                    fb_fill_rect(x, yy, 3, 2, fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bb2));
                }
            }
        }
    }

    // NEBULA: soft, slow-drifting clouds of purple-magenta gas over the upper sky. A
    // cheap LOW-frequency plasma field (sums of wp_isin) picks out big blobs; only the
    // field's upper range glows, so most of the sky (and the stars) shows through and
    // the clouds read as diffuse nebulosity rather than a hard plasma. Each lit pixel
    // blends from the LOCAL sky gradient toward a purple/blue nebula hue that itself
    // drifts — keeping the Nyx night palette.
    if (style == WP_STYLE_NEBULA) {
        uint32_t tms = get_ticks();
        int t1 = (int)(tms / 60), t2 = (int)(tms / 90), t3 = (int)(tms / 45);
        int zone = (int)fh * 62 / 100;
        for (int y = 0; y < zone; y += 2) {
            int pct = 45 + y * 70 / (int)fh;
            int skr = br * pct / 100, skg = bg * pct / 100, skb = bb * pct / 100;
            for (int x = 0; x < (int)fw; x += 3) {
                int f = wp_isin((x / 3 + t1) & 255)
                      + wp_isin((y / 2 - t2) & 255)
                      + wp_isin(((x + y) / 4 + t3) & 255);
                int glow = f - 1100;                          // only the upper field glows
                if (glow <= 0) continue;                       // sky + stars show through
                int inten = glow * 48 / 1972;                  // 0..~48 soft max
                if (inten > 48) inten = 48;
                int hsel = wp_isin((x / 3 - y / 2 + t2) & 255);  // drifts the hue purple<->blue
                int nr = 150 + hsel / 22, ng = 74, nb = 208 - hsel / 26;
                int rr = skr + (nr - skr) * inten / 100;
                int gg = skg + (ng - skg) * inten / 100;
                int bb2 = skb + (nb - skb) * inten / 100;
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb2 > 255) bb2 = 255;
                if (rr < 0) rr = 0;
                if (gg < 0) gg = 0;
                if (bb2 < 0) bb2 = 0;
                fb_fill_rect(x, y, 3, 2, fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bb2));
            }
        }
    }

    // LUCES: soft lilac orbs — "fireflies" — drift slowly UP the night sky, each on
    // its own lane with a gentle horizontal sway and a slow brightness pulse. Every
    // orb's position is a pure function of get_ticks() + its fixed LCG seed, so a
    // repaint at a given instant is identical (no flicker); the motion comes from the
    // compositor's periodic repaint advancing the clock. Each orb is a small disc that
    // blends from the LOCAL sky gradient toward a lilac core with a soft radial
    // falloff, so it glows over the gradient + stars with no hard edge.
    if (style == WP_STYLE_LUCES) {
        uint32_t tms = get_ticks();
        uint32_t seed2 = 0x1234567u;
        int span = (int)fh + 48;                        // rise distance before wrap
        for (int i = 0; i < 16; i++) {
            seed2 = seed2 * 1103515245u + 12345u; int lane  = (int)((seed2 >> 9) % fw);
            seed2 = seed2 * 1103515245u + 12345u; int phase = (int)((seed2 >> 9) & 255);
            seed2 = seed2 * 1103515245u + 12345u; int spd   = 34 + (int)((seed2 >> 9) % 34);   // ms per px up
            seed2 = seed2 * 1103515245u + 12345u; int amp   = 16 + (int)((seed2 >> 9) % 40);   // sway px
            int yy = (int)fh + 24 - (int)((tms / (uint32_t)spd + (uint32_t)phase * 4u) % (uint32_t)span);
            int xx = lane + wp_isin((int)(tms / 40) + phase) * amp / 1024;
            if (xx < 0 || xx >= (int)fw || yy < 0 || yy >= (int)fh) continue;
            int ex = xx - mx, ey = yy - my;
            if (ex * ex + ey * ey < (mr + 16) * (mr + 16)) continue;   // keep clear of the moon
            int pp = ((int)(tms / 50) + phase) & 63;
            int tri = (pp < 32) ? pp : (64 - pp);        // 0..32 pulse
            int glow = 34 + tri * 2;                     // 34..98 peak intensity
            int pct = 45 + yy * 70 / (int)fh;            // local sky gradient %
            int skr = br * pct / 100, skg = bg * pct / 100, skb = bb * pct / 100;
            for (int dy = -4; dy <= 4; dy++) {
                int py = yy + dy; if (py < 0 || py >= (int)fh) continue;
                for (int dx = -4; dx <= 4; dx++) {
                    int d2 = dx * dx + dy * dy; if (d2 > 16) continue;
                    int px = xx + dx; if (px < 0 || px >= (int)fw) continue;
                    int inten = (16 - d2) * glow / 16;   // soft radial falloff, 0..glow
                    int rr = skr + (214 - skr) * inten / 100;
                    int gg = skg + (194 - skg) * inten / 100;
                    int bb2 = skb + (248 - skb) * inten / 100;
                    if (rr > 255) rr = 255;
                    if (gg > 255) gg = 255;
                    if (bb2 > 255) bb2 = 255;
                    fb_fill_rect(px, py, 1, 1, fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bb2));
                }
            }
        }
    }

    // ONDAS: slow concentric ripples of moonlight expand out from the moon, like light
    // on still water. NR rings ride the same clock at even radius offsets; each fades
    // as it grows, so a few soft lilac rings always hang around the moon.
    if (style == WP_STYLE_ONDAS) {
        uint32_t tms = get_ticks();
        int maxr = (int)fw;                          // a ring reaches the far side, then wraps
        const int NR = 4;
        int step = maxr / NR;
        int base2 = (int)(tms / 5) % step;            // shared outward phase (px)
        for (int k = 0; k < NR; k++) {
            int r = base2 + k * step;
            if (r < mr + 10) continue;               // keep clear of the moon disc
            int inten = 42 * (maxr - r) / maxr;      // bright when young, fades as it grows
            bg_ripple_ring(mx, my, r, (int)fw, (int)fh, br, bg, bb, inten);
        }
    }

    // CONSTELACIONES ("Astral"): a star map — a handful of constellations, each a short
    // chain of bright anchor stars joined by faint lilac threads, laid over the calm sky.
    // Deterministic (a fixed LCG seed) so the map never jumps, and each thread blends
    // from the local sky so it reads as delicate light, not a hard stroke. A still sky.
    if (style == WP_STYLE_CONSTELACIONES) {
        uint32_t cs = 0x2545F491u;
        for (int c = 0; c < 7; c++) {
            cs = cs * 1103515245u + 12345u;
            int npts = 3 + (int)((cs >> 7) % 3);            // 3..5 stars in this constellation
            int px = (int)fw / 2, py = (int)star_zone / 2;
            for (int tries = 0; tries < 10; tries++) {      // anchor it clear of the moon
                cs = cs * 1103515245u + 12345u; px = 30 + (int)((cs >> 9) % (fw - 60));
                cs = cs * 1103515245u + 12345u; py = 34 + (int)((cs >> 9) % (star_zone - 50));
                int ex = px - mx, ey = py - my;
                if (ex * ex + ey * ey > (mr + 55) * (mr + 55)) break;
            }
            fb_fill_rect(px, py, 2, 2, fb_rgb(216, 210, 246));   // first anchor star
            for (int k = 1; k < npts; k++) {
                cs = cs * 1103515245u + 12345u; int nx = px + (int)((cs >> 10) % 66) - 33;
                cs = cs * 1103515245u + 12345u; int ny = py + (int)((cs >> 10) % 56) - 28;
                if (nx < 10) nx = 10;
                if (nx > (int)fw - 10) nx = (int)fw - 10;
                if (ny < 10) ny = 10;
                if (ny > (int)star_zone) ny = (int)star_zone;
                bg_star_line(px, py, nx, ny, (int)fw, (int)fh, br, bg, bb, 34);   // faint thread
                fb_fill_rect(nx, ny, 2, 2, fb_rgb(210, 204, 242));               // vertex star
                px = nx; py = ny;
            }
        }
    }

    // LLUVIA: a soft rain of luminous lilac drops falling through the night. Each drop
    // is a vertical streak whose bright head falls at its own steady speed and whose
    // tail fades upward (a drop's motion blur trails behind it). Position is a pure
    // function of get_ticks() + the drop's fixed LCG seed, so a repaint at any instant
    // is identical (no flicker); the fall comes from the compositor advancing the clock.
    // A faint wind sway rides wp_isin. Each streak pixel blends from the LOCAL sky
    // gradient toward a soft lilac, so the rain glows over the gradient + stars with no
    // hard edge and melts back into the night — keeping the Nyx palette. The long
    // streaks overlap frame-to-frame so the motion reads smooth. Drops skip the moon.
    if (style == WP_STYLE_LLUVIA) {
        uint32_t tms = get_ticks();
        uint32_t rs = 0x51ED2701u;
        int span = (int)fh + 40;                         // fall distance before a drop wraps
        for (int i = 0; i < 34; i++) {
            rs = rs * 1103515245u + 12345u; int lane  = (int)((rs >> 9) % fw);
            rs = rs * 1103515245u + 12345u; int phase = (int)((rs >> 9) % (uint32_t)span);
            rs = rs * 1103515245u + 12345u; int spd   = 5 + (int)((rs >> 9) % 5);   // ms per px down
            rs = rs * 1103515245u + 12345u; int len   = 16 + (int)((rs >> 9) % 14); // streak length px
            rs = rs * 1103515245u + 12345u; int amp   = 2 + (int)((rs >> 9) % 5);   // wind sway px
            int hy = (int)((tms / (uint32_t)spd + (uint32_t)phase) % (uint32_t)span);   // head y from top
            int hx = lane + wp_isin((int)(tms / 60) + phase) * amp / 1024;
            for (int s = 0; s <= len; s++) {
                int py = hy - s;                          // tail trails UPWARD, opposite the fall
                if (py < 0 || py >= (int)fh) continue;
                if (hx < 0 || hx >= (int)fw) continue;
                int ex = hx - mx, ey = py - my;
                if (ex * ex + ey * ey < (mr + 12) * (mr + 12)) continue;   // keep clear of the moon
                int inten = 70 * (len - s) / len;         // bright head -> faded tail (0..70)
                int pct = 45 + py * 70 / (int)fh;         // local sky gradient %
                int skr = br * pct / 100, skg = bg * pct / 100, skb = bb * pct / 100;
                int rr = skr + (214 - skr) * inten / 100;
                int gg = skg + (202 - skg) * inten / 100;
                int bl = skb + (244 - skb) * inten / 100;
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bl > 255) bl = 255;
                fb_fill_rect(hx, py, (s < 3) ? 2 : 1, 1, fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bl));
            }
        }
    }

    // CORDILLERA: a still range of night mountains along the horizon. Three layered
    // silhouettes — back (highest, lilac-dark) to front (lowest, near-black) — each a
    // ridge whose crest rides a sum of two STATIC sines (wp_isin) and is filled straight
    // down to the bottom of the screen, the front layers overpainting the back for depth.
    // No get_ticks(): a calm landscape under the moon and stars, purple-tinted darks
    // keeping the Nyx night identity. Some low stars fall behind the ridges (occluded).
    if (style == WP_STYLE_CORDILLERA) {
        struct { int basey, a1, f1, a2, f2, ph, r, g, b; } ridge[3] = {
            { (int)fh * 60 / 100, 30, 3, 13, 7,  40, 60, 48, 92 },   // back: highest, lilac-dark
            { (int)fh * 70 / 100, 40, 2, 18, 5, 150, 38, 30, 60 },   // mid
            { (int)fh * 80 / 100, 32, 4, 22, 9, 210, 16, 13, 28 },   // front: lowest, near-black
        };
        for (int k = 0; k < 3; k++) {
            uint32_t col = fb_rgb((uint8_t)ridge[k].r, (uint8_t)ridge[k].g, (uint8_t)ridge[k].b);
            for (int x = 0; x < (int)fw; x++) {
                int crest = ridge[k].basey
                          + ridge[k].a1 * wp_isin((x * ridge[k].f1 + ridge[k].ph) & 255) / 1024
                          + ridge[k].a2 * wp_isin((x * ridge[k].f2) & 255) / 1024;
                if (crest < 0) crest = 0;
                if (crest < (int)fh) fb_fill_rect(x, crest, 1, (int)fh - crest, col);
            }
        }
    }
}

static void init_desktop_icons(void);
static void draw_desktop_icons(void);
static void draw_desktop_widget(void);   // conky-style live CPU/RAM panel on the wallpaper
static void settings_draw_fn(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);

// Drop-shadow geometry. OFFSET is how far down-right the shadow sits; RADIUS is
// how far it feathers out past the window edge; CORE is the darkening (0..255) of
// the innermost ring, fading linearly to nothing at the outermost.
#define SHADOW_OFFSET 6
#define SHADOW_RADIUS 12
#define SHADOW_CORE   100

// Cast a soft drop shadow for one window onto whatever has already been drawn
// beneath it. Called from the back-to-front composite loop right before the
// window itself, so the shadow lands on the desktop and any lower windows, and
// the window is then painted opaque on top — leaving only the soft band past the
// bottom and right edges visible, which is the drop-shadow look.
//
// Drawn as concentric 1px OUTLINES growing outward from the offset window rect,
// darkest (SHADOW_CORE) nearest the window and fading to zero at RADIUS. Outlines
// rather than filled rects means each pixel is darkened exactly once (a clean
// gradient, no compounding) and the cost is perimeter, not area — a handful of
// thin strips per window instead of RADIUS full-window fills.
static void draw_window_shadow(window_t* win) {
    if (!g_shadows) return;                      // nyx.conf `shadow = off` -> flat, shadowless windows
    // A maximized window fills the usable area, so its shadow would be entirely
    // behind it or off-screen: pure wasted work. Snapped/normal windows all cast.
    if (win->state == WSTATE_MAXIMIZED) return;
    int rx = win->x + SHADOW_OFFSET;
    // Start the shadow BELOW the rounded title-bar top, not at the window top.
    // The top corners are cut away (see draw_titlebar), so a shadow drawn up there
    // shows through the cutout as a stray dark step at the top-right; beginning it
    // at the title/body junction keeps it out of the rounded region entirely while
    // leaving the visible drop (the body's right edge and bottom) untouched.
    int ry = win->y + TITLE_H;
    int rw = (int)win->w;
    int rh = (int)win_total_h(win) + SHADOW_OFFSET - TITLE_H;   // reach to below the window bottom
    for (int r = SHADOW_RADIUS; r >= 1; r--) {
        uint8_t shade = (uint8_t)(SHADOW_CORE * (SHADOW_RADIUS - r + 1) / SHADOW_RADIUS);
        int x = rx - r, y = ry - r, w = rw + 2 * r, h = rh + 2 * r;
        fb_darken_rect(x,         y,         w, 1, shade);   // top edge
        fb_darken_rect(x,         y + h - 1, w, 1, shade);   // bottom edge
        fb_darken_rect(x,         y + 1,     1, h - 2, shade);   // left edge
        fb_darken_rect(x + w - 1, y + 1,     1, h - 2, shade);   // right edge
    }
}

// Cast a soft shadow under the open Start menu so it floats above the desktop and
// any windows behind it — the same shadow language as windows (SHADOW_* macros),
// for a consistent modern-launcher look. Called from redraw_all right after the
// taskbar and just before the menu body paints, so only the feathered halo past
// the menu's top/left/right edges shows and the body then covers the interior.
// The bottom edge is omitted on purpose: the menu sits flush on the taskbar
// (already drawn), reading as growing out of it, so a shadow there would just
// smudge the bar — the side bands stop at the taskbar top for the same reason.
static void draw_start_menu_shadow(void) {
    if (!start_menu_open || !g_shadows) return;   // nyx.conf `shadow = off` -> no menu shadow either
    int fh = (int)fb_get_height();
    int sm_x = 2, sm_y = fh - TASKBAR_H - START_H;
    int bar_top = fh - TASKBAR_H;                 // side bands stop here, off the taskbar
    for (int r = SHADOW_RADIUS; r >= 1; r--) {
        uint8_t shade = (uint8_t)(SHADOW_CORE * (SHADOW_RADIUS - r + 1) / SHADOW_RADIUS);
        int y = sm_y - r;
        fb_darken_rect(sm_x - r, y, START_W + 2 * r, 1, shade);                   // top edge
        fb_darken_rect(sm_x - r,               y + 1, 1, bar_top - (y + 1), shade); // left edge
        fb_darken_rect(sm_x + START_W + r - 1, y + 1, 1, bar_top - (y + 1), shade); // right edge
    }
}

static void redraw_all(void) {
    draw_background();
    // nyx.conf `wallpaper_dim`: mute the freshly-painted wallpaper by g_wallpaper_dim percent
    // (BEFORE widgets/icons/windows, so only the background dims). Off (0) = a no-op.
    if (g_wallpaper_dim > 0)
        fb_darken_rect(0, 0, (int)fb_get_width(), (int)fb_get_height(),
                       (uint8_t)(g_wallpaper_dim * 255 / 100));
    draw_desktop_widget();   // on the wallpaper, behind windows
    draw_desktop_icons();

    window_t* sorted[MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->visible && windows[i]->state != WSTATE_MINIMIZED
            && windows[i]->workspace == current_workspace)
            sorted[n++] = windows[i];

    for (int i = 1; i < n; i++) {
        window_t* key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->z_order > key->z_order) { sorted[j+1] = sorted[j]; j--; }
        sorted[j+1] = key;
    }

    for (int i = 0; i < n; i++) {
        window_t* win = sorted[i];
        draw_window_shadow(win);      // cast onto lower windows/desktop, before this window paints
        // Clip the body + app content to the window's rounded-bottom shape, so an
        // app that fills its client rect edge-to-edge can't square off the corners
        // the frame rounds. Frame + title bar draw AFTER, unclipped: the frame
        // traces the arc, and the title bar rounds the top itself.
        int wr = win_radius(win);
        fb_set_round_clip(win->x, win->y, (int)win->w, (int)win_total_h(win), wr);
        fb_fill_rect(win->x, win->y + TITLE_H, win->w, win->h, fb_rgb(35,35,40));
        if (win->draw)
            win->draw(win, win->x, win->y + TITLE_H, win->w, win->h);
        fb_clear_clip();
        draw_window_frame(win);
        draw_titlebar(win);
    }

    draw_snap_preview();          // Aero-snap target hint, on top of the windows
    draw_workspace_indicator();
    draw_taskbar();
    draw_start_menu_shadow();     // soft halo cast under the menu, before its body paints
    draw_start_menu();
    draw_user_menu();
    draw_ctx_menu();
    draw_cal_popup();
    draw_net_popup();
    draw_spk_popup();
    notify_render(get_ticks());   // desktop toasts sit on top of everything
    frame_dirty = 1;      // a fresh frame is in the back buffer, awaiting fb_present()
}

// Public one-shot recomposite. Used by the foreground-exec wait loop (cmd_exec)
// to keep the desktop repainting — so a running TUI (e.g. top) is visible live —
// while the compositor thread is parked in that loop instead of its own event
// loop. Pure draw: touches no input state, safe to call re-entrantly from a
// command handler that the compositor's key dispatch invoked.
void compositor_redraw_now(void) {
    redraw_all();
    fb_present();     // this path runs outside the event loop, so publish here
    frame_dirty = 0;
}

// Dirty-rect groundwork (FLUIDEZ): is any window ABOVE `a` (higher z, so it draws
// on top in redraw_all's sorted pass) overlapping a's footprint — INCLUDING the
// drop-shadow margin, since a higher window's shadow can fall onto a? If so, a
// clipped repaint of `a` alone would paint over that window, so the caller must
// fall back to a full redraw_all. Only visible, non-minimized windows on the
// current workspace count.
static int window_occluded_above(window_t* a) {
    const int M = SHADOW_OFFSET + SHADOW_RADIUS + 2;
    int ax0 = a->x - M, ay0 = a->y - M;
    int ax1 = a->x + (int)a->w + M, ay1 = a->y + (int)win_total_h(a) + M;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* b = windows[i];
        if (!b || b == a || !b->visible || b->state == WSTATE_MINIMIZED
            || b->workspace != current_workspace || b->z_order <= a->z_order) continue;
        int bx0 = b->x - M, by0 = b->y - M;
        int bx1 = b->x + (int)b->w + M, by1 = b->y + (int)win_total_h(b) + M;
        if (ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1) return 1;   // overlap
    }
    return 0;
}

// Dirty-rect FIRST SLICE (FLUIDEZ): repaint just ONE window into the persistent
// back buffer, WITHOUT touching the wallpaper, its (unmoved) drop shadow, the other
// windows, or the taskbar — they stay valid from the last full redraw_all. This is
// exactly redraw_all's per-window block for `win`, minus draw_window_shadow (the
// window isn't moving, so its shadow pixels are already correct). The caller must
// guarantee nothing else changed this frame and `win` is un-occluded (see
// window_occluded_above) so the result is pixel-identical to a full recomposite.
static void redraw_window_only(window_t* win) {
    int wr = win_radius(win);
    fb_set_round_clip(win->x, win->y, (int)win->w, (int)win_total_h(win), wr);
    fb_fill_rect(win->x, win->y + TITLE_H, win->w, win->h, fb_rgb(35,35,40));
    if (win->draw)
        win->draw(win, win->x, win->y + TITLE_H, win->w, win->h);
    fb_clear_clip();
    draw_window_frame(win);
    draw_titlebar(win);
    frame_dirty = 1;      // a fresh (partial) frame is in the back buffer, awaiting present
}

// ---------------------------------------------------------------------------
// Desktop notifications / toasts (v6.4.247). notify_push() drops a short-lived
// purple Nyx card in the top-right corner; the compositor keeps repainting
// while any toast is alive (see compositor_run) so it fades out on its own. The
// ring logic (alive/expire/capacity/eviction) takes an injected clock so it is
// KAT'd with no framebuffer (same idea as the auth-lockout clock injection).
// ---------------------------------------------------------------------------
#define NOTIFY_MAX     4
#define NOTIFY_TTL_MS  4000u
#define NOTIFY_TITLE   48
#define NOTIFY_BODY    96

typedef struct {
    int      used;
    uint32_t born;                 // get_ticks() ms when pushed
    char     title[NOTIFY_TITLE];
    char     body[NOTIFY_BODY];
} toast_t;

static toast_t toasts[NOTIFY_MAX];

static int toast_alive(const toast_t* t, uint32_t now) {
    return t->used && (uint32_t)(now - t->born) < NOTIFY_TTL_MS;
}

static int notify_active_count(uint32_t now) {
    int c = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) if (toast_alive(&toasts[i], now)) c++;
    return c;
}

// Push a toast at time `now`: reuse a dead slot, else evict the oldest live one.
static void notify_push_at(const char* title, const char* body, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < NOTIFY_MAX; i++) if (!toast_alive(&toasts[i], now)) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < NOTIFY_MAX; i++)
            if ((uint32_t)(now - toasts[i].born) > (uint32_t)(now - toasts[slot].born)) slot = i;
    }
    toasts[slot].used = 1;
    toasts[slot].born = now;
    strncpy(toasts[slot].title, title ? title : "", NOTIFY_TITLE - 1);
    toasts[slot].title[NOTIFY_TITLE - 1] = '\0';
    strncpy(toasts[slot].body, body ? body : "", NOTIFY_BODY - 1);
    toasts[slot].body[NOTIFY_BODY - 1] = '\0';
}

void notify_push(const char* title, const char* body) {
    notify_push_at(title, body, get_ticks());
    compositor_redraw_now();       // show it immediately (safe re-entrantly)
}

// Draw the live toasts stacked top-right; reap any that have expired.
static void notify_render(uint32_t now) {
    const int tw = 264, th = 46, pad = 12, gap = 8;
    int maxc = (tw - 24) / FONT_WIDTH;
    if (maxc > 38) maxc = 38;
    int x = (int)fb_get_width() - tw - pad;
    int y = 40;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!toast_alive(&toasts[i], now)) { toasts[i].used = 0; continue; }
        fb_fill_rect(x, y, tw, th, fb_rgb(40, 32, 58));            // Nyx purple card
        fb_fill_rect(x, y, 4, th, fb_rgb(150, 110, 235));         // accent bar
        char line[40];
        int n = 0; while (n < maxc && toasts[i].title[n]) { line[n] = toasts[i].title[n]; n++; } line[n] = '\0';
        font_draw_string_trans(x + 12, y + 6, line, fb_rgb(235, 230, 245));
        n = 0; while (n < maxc && toasts[i].body[n]) { line[n] = toasts[i].body[n]; n++; } line[n] = '\0';
        font_draw_string_trans(x + 12, y + 8 + FONT_HEIGHT, line, fb_rgb(182, 176, 200));
        y += th + gap;
    }
}

int notify_selftest(void) {
    for (int i = 0; i < NOTIFY_MAX; i++) toasts[i].used = 0;

    uint32_t t0 = 1000;
    notify_push_at("a", "1", t0);
    if (notify_active_count(t0) != 1) return 1;
    if (notify_active_count(t0 + NOTIFY_TTL_MS - 1) != 1) return 2;   // alive just before TTL
    if (notify_active_count(t0 + NOTIFY_TTL_MS) != 0)     return 3;   // expired at TTL

    for (int i = 0; i < NOTIFY_MAX; i++) notify_push_at("x", "y", t0);
    if (notify_active_count(t0) != NOTIFY_MAX) return 4;             // all slots filled
    notify_push_at("new", "z", t0 + 10);                            // full -> evict oldest
    if (notify_active_count(t0 + 10) != NOTIFY_MAX) return 5;        // still capped, no overflow

    for (int i = 0; i < NOTIFY_MAX; i++) toasts[i].used = 0;         // leave a clean slate
    return 0;
}

static window_t* find_window(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->id == id) return windows[i];
    return NULL;
}

// Open a Text Editor window, optionally pre-loaded with a file. Shared by the
// start-menu / desktop "Text Editor" action (path = NULL → blank document) and
// the file manager, which passes the path of a file the user opened. Returns the
// new window, or NULL if the window pool is full / allocation failed.
window_t* compositor_open_editor(const char* path) {
    window_t* ewin = window_create(150, 120, 600, 400, EDITOR_NAME, editor_win_draw);
    if (!ewin) return NULL;
    ewin->cursor_shape = CURSOR_IBEAM;   // Mnemosyne: text area -> I-beam pointer
    ewin->reserved = editor_create_ctx();
    if (!ewin->reserved) { window_destroy(ewin->id); return NULL; }
    ewin->on_click = editor_win_click;
    ewin->on_key = editor_win_key;
    ewin->on_close = editor_win_close;   // free the undo snapshots when the window closes
    if (path && path[0]) editor_load_file((editor_win_t*)ewin->reserved, path);
    return ewin;
}

// ---- DOOM windowed rendering (v5.9.37) -------------------------------------------
// DOOM normally drives the whole framebuffer via SYS_FBPRESENT (fullscreen). To run it
// in a WINDOW instead, launch_doom_windowed() opens a plain compositor window and the
// SYS_FBPRESENT handler is rerouted (doom_window_present) to copy each frame into
// doom_frame rather than blitting fullscreen — so fb_fullscreen_active() stays false and
// the compositor keeps publishing the desktop. The window's draw callback scales that
// frame into its client area. DOOM runs FOREGROUND (gui_launch_elf); run_foreground_elf's
// redraw pump repaints the window, and Ctrl-C exits (SIGINT to the foreground pid).
static uint32_t doom_frame[320 * 200];
static int      doom_win_id = -1;

// Called from the SYS_FBPRESENT handler. Returns 1 if a DOOM window is up (frame stored,
// caller must NOT go fullscreen); 0 to fall back to the fullscreen blit.
int doom_window_present(const uint32_t* src, uint32_t w, uint32_t h) {
    if (doom_win_id < 0 || !src || w != 320 || h != 200) return 0;
    for (int i = 0; i < 320 * 200; i++) doom_frame[i] = src[i];
    return 1;
}

// Draw callback: nearest-neighbour scale the 320x200 frame into the window client.
static void doom_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    (void)win;
    if (cw == 0 || ch == 0) return;
    for (uint32_t y = 0; y < ch; y++) {
        const uint32_t* srow = doom_frame + (uint64_t)(y * 200 / ch) * 320;
        for (uint32_t x = 0; x < cw; x++)
            fb_put_pixel((uint32_t)(cx + (int)x), (uint32_t)(cy + (int)y), srow[x * 320 / cw]);
    }
}

// Open a DOOM window and run the game inside it (foreground). Blocks until DOOM exits,
// then tears the window down. Used by both the desktop icon and the `doom` command.
void launch_doom_windowed(void) {
    if (doom_win_id >= 0) return;                            // already playing
    const int W = 640, H = 400;                              // 2x the native 320x200
    int px = ((int)fb_get_width()  - W) / 2;                 if (px < 0) px = 0;
    int py = ((int)fb_get_height() - H - TITLE_H) / 2;       if (py < 0) py = 0;
    window_t* w = window_create(px, py, W, H, "DOOM  -  Ctrl-C to quit", doom_win_draw);
    if (!w) { gui_launch_elf("/doom.elf"); return; }         // no slot: fall back to fullscreen
    doom_win_id = w->id;
    for (int i = 0; i < 320 * 200; i++) doom_frame[i] = 0;   // black until the first frame
    gui_launch_elf("/doom.elf");                             // foreground: pump repaints us
    doom_win_id = -1;
    window_destroy(w->id);
}

// Open a Pong window. In-kernel game: the compositor's game-tick animates it via on_tick,
// the player's paddle follows the mouse. Used by the `pong` command and the Games folder.
void launch_pong(void) {
    int px = ((int)fb_get_width()  - PONG_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - PONG_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, PONG_W, PONG_H, "Pong", pong_win_draw);
    if (!w) return;
    w->reserved = pong_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_key       = pong_win_key;
    w->on_mousemove = pong_win_mousemove;
    w->on_tick      = pong_win_tick;
}

// Open the Nyx Voxels window — an interactive isometric voxel scene (a step toward
// the voxel-sandbox north star). A per-window ctx holds the mutable heightmap;
// left-click places a cube, right-click breaks the top one (voxel_win_click), and
// a dirty-gated tick repaints after an edit.
void launch_voxel(void) {
    int px = ((int)fb_get_width()  - VOXEL_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - VOXEL_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, VOXEL_W, VOXEL_H, "Nyx Voxels", voxel_win_draw);
    if (!w) return;
    w->reserved = voxel_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_click = voxel_win_click;
    w->on_key   = voxel_win_key;
    w->on_tick  = voxel_win_tick;
}

// Open the Nyx Fire window — the classic animated "doom fire" effect (a P4 visual
// perf demo, and one of the /fire //matrix //lava effects from the user backlog).
// A per-window ctx holds the heat grid; the game tick advances the flames.
void launch_fire(void) {
    int px = ((int)fb_get_width()  - FIRE_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - FIRE_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, FIRE_WIN_W, FIRE_WIN_H, "Nyx Fire", fire_win_draw);
    if (!w) return;
    w->reserved = fire_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = fire_win_tick;
}

// Open the Nyx Matrix window — the animated green "code rain" effect (a P4 visual
// perf demo, and one of the /fire //matrix //lava effects from the user backlog).
void launch_matrix(void) {
    int px = ((int)fb_get_width()  - MATRIX_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - MATRIX_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, MATRIX_WIN_W, MATRIX_WIN_H, "Nyx Matrix", matrix_win_draw);
    if (!w) return;
    w->reserved = matrix_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = matrix_win_tick;
}

// Open the Nyx Lava window — the animated plasma / "lava" effect (a P4 visual perf
// demo, and the third of the /fire //matrix //lava effects from the user backlog).
void launch_lava(void) {
    int px = ((int)fb_get_width()  - LAVA_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - LAVA_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, LAVA_WIN_W, LAVA_WIN_H, "Nyx Lava", lava_win_draw);
    if (!w) return;
    w->reserved = lava_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = lava_win_tick;
}

// Open "Nyx Flex" — a four-window showcase tiled across the screen quadrants: Nyx Matrix
// (top-left), Nyx Lava (top-right), an Erebus terminal running nyxfetch (bottom-left),
// and DOOM (bottom-right). The first three are passive animated tiles that open at once;
// DOOM, like launch_doom_windowed, runs in the FOREGROUND and BLOCKS until the player
// quits (Ctrl-C), so it is opened LAST, after the other three are already on screen. A
// "show off the whole OS at a glance" command (user-requested).
void launch_nyxflex(void) {
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    int hw = sw / 2, hh = sh / 2;
    // Centre a window of size (w, h)+title inside the quadrant whose top-left is (qx, qy).
    #define NF_POS(qx, qy, w, h, ox, oy) do { \
        (ox) = (qx) + (hw - (int)(w)) / 2;             if ((ox) < (qx)) (ox) = (qx); \
        (oy) = (qy) + (hh - (int)(h) - TITLE_H) / 2;   if ((oy) < (qy)) (oy) = (qy); } while (0)
    int mx, my, lx, ly, tx, ty;
    // Q1 top-left — Nyx Matrix
    NF_POS(0, 0, MATRIX_WIN_W, MATRIX_WIN_H, mx, my);
    window_t* mw = window_create(mx, my, MATRIX_WIN_W, MATRIX_WIN_H, "Nyx Matrix", matrix_win_draw);
    if (mw) { mw->reserved = matrix_create_ctx(); if (mw->reserved) mw->on_tick = matrix_win_tick; else window_destroy(mw->id); }
    // Q2 top-right — Nyx Lava
    NF_POS(hw, 0, LAVA_WIN_W, LAVA_WIN_H, lx, ly);
    window_t* lw = window_create(lx, ly, LAVA_WIN_W, LAVA_WIN_H, "Nyx Lava", lava_win_draw);
    if (lw) { lw->reserved = lava_create_ctx(); if (lw->reserved) lw->on_tick = lava_win_tick; else window_destroy(lw->id); }
    // Q3 bottom-left — Erebus terminal, auto-running nyxfetch (fed to its key handler)
    NF_POS(0, hh, 640, 400, tx, ty);
    window_t* tw = window_create(tx, ty, 640, 400, "Terminal", terminal_win_draw);
    if (tw) {
        tw->cursor_shape = CURSOR_IBEAM;   // Erebus: text area -> I-beam pointer
        tw->reserved = terminal_create_ctx();
        if (tw->reserved) {
            tw->on_key = terminal_win_key;
            for (const char* c = "nyxfetch\n"; *c; c++) terminal_win_key(tw, (unsigned char)*c);
        } else window_destroy(tw->id);
    }
    // Q4 bottom-right — DOOM (foreground; blocks until quit, so it goes last)
    if (doom_win_id < 0) {
        const int DW = 640, DH = 400;
        int dpx = hw + (hw - DW) / 2;             if (dpx < hw) dpx = hw;
        int dpy = hh + (hh - DH - TITLE_H) / 2;   if (dpy < hh) dpy = hh;
        window_t* dw = window_create(dpx, dpy, DW, DH, "DOOM  -  Ctrl-C to quit", doom_win_draw);
        if (dw) {
            doom_win_id = dw->id;
            for (int i = 0; i < 320 * 200; i++) doom_frame[i] = 0;
            gui_launch_elf("/doom.elf");           // foreground: pump repaints all four windows
            doom_win_id = -1;
            window_destroy(dw->id);
        }
    }
    #undef NF_POS
}

// Open the Nyx Fractal window — a fixed-point Mandelbrot renderer that auto-zooms,
// a P4 rendering-performance-testing demo with a live benchmark HUD (frame-time /
// render FPS / iteration cap / zoom), sibling to the /fire //matrix //lava effects.
void launch_mandel(void) {
    int px = ((int)fb_get_width()  - MANDEL_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - MANDEL_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, MANDEL_WIN_W, MANDEL_WIN_H, "Nyx Fractal", mandel_win_draw);
    if (!w) return;
    w->reserved = mandel_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = mandel_win_tick;
}

// Open the Nyx Julia window — a fixed-point Julia-set renderer whose constant c
// orbits so the set morphs, a P4 rendering-performance-testing demo with the same
// benchmark HUD as Nyx Fractal.
void launch_julia(void) {
    int px = ((int)fb_get_width()  - JULIA_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - JULIA_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, JULIA_WIN_W, JULIA_WIN_H, "Nyx Julia", julia_win_draw);
    if (!w) return;
    w->reserved = julia_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = julia_win_tick;
}

void launch_particles(void) {
    int px = ((int)fb_get_width()  - PART_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - PART_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, PART_WIN_W, PART_WIN_H, "Nyx Particles", particles_win_draw);
    if (!w) return;
    w->reserved = particles_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = particles_win_tick;
    w->on_key  = particles_win_key;
}

void launch_rotor(void) {
    int px = ((int)fb_get_width()  - ROTOR_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - ROTOR_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, ROTOR_WIN_W, ROTOR_WIN_H, "Nyx Rotor", rotor_win_draw);
    if (!w) return;
    w->reserved = rotor_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = rotor_win_tick;
    w->on_key  = rotor_win_key;
}

void launch_fill(void) {
    int px = ((int)fb_get_width()  - FILL_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - FILL_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, FILL_WIN_W, FILL_WIN_H, "Nyx Fill", fill_win_draw);
    if (!w) return;
    w->reserved = fill_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = fill_win_tick;
    w->on_key  = fill_win_key;
}

// Open the Nyx Life window - Conway's Game of Life as a compute/render perf testbed
// (the cellular-automaton sibling of Nyx Fill and Nyx Fractal). A per-window ctx holds
// the two grid buffers; the game tick advances `speed` generations and repaints.
void launch_life(void) {
    int px = ((int)fb_get_width()  - LIFE_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - LIFE_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, LIFE_WIN_W, LIFE_WIN_H, "Nyx Life", life_win_draw);
    if (!w) return;
    w->reserved = life_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = life_win_tick;
    w->on_key  = life_win_key;
}

// Open the Nyx Blobs window - a metaballs render-perf demo (the organic scalar-field
// sibling of Nyx Fractal / Nyx Fill). A per-window ctx holds the drifting blobs; the
// game tick moves them and repaints the field.
void launch_blobs(void) {
    int px = ((int)fb_get_width()  - BLOBS_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - BLOBS_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, BLOBS_WIN_W, BLOBS_WIN_H, "Nyx Blobs", blobs_win_draw);
    if (!w) return;
    w->reserved = blobs_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_tick = blobs_win_tick;
    w->on_key  = blobs_win_key;
}

// Open a Snake window. In-kernel game like Pong: the compositor's game-tick steps it via
// on_tick, arrows / WASD steer. Used by the `snake` command and the Games folder.
void launch_snake(void) {
    int px = ((int)fb_get_width()  - SNAKE_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - SNAKE_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, SNAKE_W, SNAKE_H, "Snake", snake_win_draw);
    if (!w) return;
    w->reserved = snake_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_key  = snake_win_key;
    w->on_tick = snake_win_tick;
}

// Open a Tetris window. In-kernel game like Pong/Snake: the compositor's game-tick
// drives gravity via on_tick, arrows move/rotate. Used by `tetris` and the Games folder.
void launch_tetris(void) {
    int px = ((int)fb_get_width()  - TET_W) / 2;               if (px < 0) px = 0;
    int py = ((int)fb_get_height() - TET_H - TITLE_H) / 2;     if (py < 0) py = 0;
    window_t* w = window_create(px, py, TET_W, TET_H, "Tetris", tetris_win_draw);
    if (!w) return;
    w->reserved = tetris_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_key  = tetris_win_key;
    w->on_tick = tetris_win_tick;
}

// Open the Minesweeper window. Extracted from do_start_menu_action's case 12 so the
// Games folder can launch it too (one definition, two callers).
void launch_minesweeper(void) {
    window_t* mwin = window_create(320, 180, MS_WIN_W, MS_WIN_H,
                                   "Minesweeper", minesweeper_win_draw);
    if (!mwin) return;
    mwin->reserved = minesweeper_create_ctx();
    if (mwin->reserved) {
        mwin->on_click = minesweeper_win_click;
        mwin->on_key   = minesweeper_win_key;
    }
}

// Open Selene, the NyxOS web browser (selene_win.c). Fetches its default page right
// away (selene_first_load) so it opens showing a real site rather than a blank view.
void launch_selene(void) {
    int px = ((int)fb_get_width()  - SELENE_W) / 2;            if (px < 0) px = 0;
    int py = ((int)fb_get_height() - SELENE_H - TITLE_H) / 2;  if (py < 0) py = 0;
    window_t* w = window_create(px, py, SELENE_W, SELENE_H, "Selene", selene_win_draw);
    if (!w) return;
    w->reserved = selene_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_key   = selene_win_key;
    w->on_click = selene_win_click;
    w->on_tick  = selene_win_tick;   // cooperative loader: streams in one image per tick, UI stays live
    selene_first_load(w);      // fetches the main page (blocks briefly); images stream in via the tick
}

// Open the Image Viewer, optionally on a file. Mirrors launch_selene: a centred window wired for
// keyboard + the animated-GIF tick + close cleanup, then loads `path` (PNG/BMP/GIF/JPEG) if given.
void launch_imageview(const char* path) {
    int px = ((int)fb_get_width()  - 540) / 2;            if (px < 0) px = 0;
    int py = ((int)fb_get_height() - 420 - TITLE_H) / 2;  if (py < 0) py = 0;
    window_t* w = window_create(px, py, 540, 420, "Image Viewer", imageview_win_draw);
    if (!w) return;
    w->reserved = imageview_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_key   = imageview_win_key;
    w->on_tick  = imageview_win_tick;    // animated-GIF playback
    w->on_close = imageview_win_close;   // free decoded pixels / GIF frames on close
    if (path && path[0])
        imageview_open_file((imageview_win_t*)w->reserved, path);
}

// Open the File Manager, optionally at a starting directory (so `files /mnt` lands there). Mirrors the
// desktop-icon path; lets the File Manager be launched from the shell like the other apps.
void launch_fileman(const char* path) {
    window_t* w = window_create(100, 100, 550, 380, "File Manager", fileman_win_draw);
    if (!w) return;
    w->reserved = fileman_create_ctx();
    if (!w->reserved) { window_destroy(w->id); return; }
    w->on_click     = fileman_win_click;
    w->on_key       = fileman_win_key;
    w->on_mousemove = fileman_win_mousemove;
    fileman_win_t* fm = (fileman_win_t*)w->reserved;
    if (path && path[0]) {
        strncpy(fm->cwd, path, sizeof(fm->cwd) - 1);
        fm->cwd[sizeof(fm->cwd) - 1] = 0;
    }
    fileman_refresh(fm);
}

// Open the "Games" desktop folder — a window listing Minesweeper, DOOM and Pong as
// clickable emblems (games_win.c). Its click handler calls the three launchers above.
void launch_games_folder(void) {
    int px = ((int)fb_get_width()  - GAMES_WIN_W) / 2;              if (px < 0) px = 0;
    int py = ((int)fb_get_height() - GAMES_WIN_H - TITLE_H) / 2;    if (py < 0) py = 0;
    window_t* w = window_create(px, py, GAMES_WIN_W, GAMES_WIN_H, "Games", games_win_draw);
    if (!w) return;
    w->on_click = games_win_click;
}

static void do_start_menu_action(int idx) {
    start_menu_open = 0;
    redraw_all();
    switch (idx) {
        case 0: // File Manager
            {
                window_t* fwin = window_create(100, 100, 550, 380, "File Manager", fileman_win_draw);
                if (fwin) {
                    fwin->reserved = fileman_create_ctx();
                    if (fwin->reserved) {
                        fwin->on_click = fileman_win_click;
                        fwin->on_key = fileman_win_key;
                        fwin->on_mousemove = fileman_win_mousemove;
                        fileman_refresh((fileman_win_t*)fwin->reserved);
                    }
                }
            }
            break;
        case 1: // Text Editor
            compositor_open_editor(NULL);
            break;
        case 2: // Image Viewer
            {
                window_t* iwin = window_create(200, 140, 540, 420, "Image Viewer", imageview_win_draw);
                if (iwin) {
                    iwin->reserved = imageview_create_ctx();
                    if (iwin->reserved) {
                        iwin->on_key = imageview_win_key;
                        iwin->on_tick = imageview_win_tick;    // drive animated-GIF playback
                        iwin->on_close = imageview_win_close;  // free decoded pixels / GIF frames
                    }
                }
            }
            break;
        case 3: // Terminal
            {
                window_t* twin = window_create(80, 80, 640, 400, "Terminal", terminal_win_draw);
                if (twin) {
                    twin->cursor_shape = CURSOR_IBEAM;   // Erebus: text area -> I-beam pointer
                    twin->reserved = terminal_create_ctx();
                    twin->on_key = terminal_win_key;
                }
            }
            break;
        case 4: // Settings
            {
                window_t* swin = window_create(160, 100, 500, 340, "Settings", settings_draw_fn);
                if (swin) {
                    int* tab = kmalloc(sizeof(int));
                    if (tab) { *tab = 0; swin->reserved = tab; }
                    swin->on_click = settings_win_click;
                }
            }
            break;
        case 5: // Nyx Monitor (performance + process monitor)
            {
                window_t* twin = window_create(100, 70, 520, 430, "Nyx Monitor", taskman_win_draw);
                if (twin) {
                    twin->reserved = taskman_create_ctx();
                    if (twin->reserved) {
                        twin->on_key = taskman_win_key;
                        twin->on_click = taskman_win_click;
                        twin->on_tick = taskman_win_tick;   // ~4 Hz live CPU/RAM graph sampling
                    }
                }
            }
            break;
        case 6: // Selene — the NyxOS web browser (also on a desktop icon, case 15)
            launch_selene();
            break;
        case 7: // Paint
            {
                window_t* pwin = window_create(80, 60, 580, 480, "Paint", paint_win_draw);
                if (pwin) {
                    pwin->reserved = paint_create_ctx();
                    if (pwin->reserved) {
                        pwin->on_click = paint_win_click;
                        pwin->on_key = paint_win_key;
                        pwin->on_pressed = paint_win_pressed;
                    }
                }
            }
            break;
        case 8: // Sound Test
            {
                window_t* sndwin = window_create(200, 150, 220, 340, "Sound Test", soundtest_win_draw);
                if (sndwin) {
                    sndwin->reserved = soundtest_create_ctx();
                    if (sndwin->reserved) {
                        sndwin->on_click = soundtest_win_click;
                    }
                }
            }
            break;
        case 9: // About
            // Centred on the 1024x768 design grid — (1024-300)/2, (768-200)/2 —
            // NOT on the live framebuffer. Deriving it from fb_get_width() would
            // have the scale applied twice and land the window left of centre.
            window_create(362, 284, 300, 200, "About NyxOS", NULL);
            break;
        case 10: // Shutdown
            quit = 1;
            break;
        case 11: // Calculator
            {
                window_t* cwin = window_create(300, 200, CALC_WIN_W, CALC_WIN_H,
                                               "Calculator", calc_win_draw);
                if (cwin) {
                    cwin->reserved = calc_create_ctx();
                    if (cwin->reserved) {
                        cwin->on_click = calc_win_click;
                        cwin->on_key = calc_win_key;
                    }
                }
            }
            break;
        case 12: // Games — folder of the built-in games (DOOM, Pong, Snake, Tetris, Minesweeper)
            launch_games_folder();
            break;
        case 13: // Oneiros — the NyxOS AI, live in a window (github.com/nyxos-dev/oneiros).
                 // The same from-scratch model the `nyxgen` command runs, streaming into
                 // the compositor: type a seed, press Dream, watch it generate NyxOS-style C.
            {
                window_t* owin = window_create(150, 90, ONEIROS_WIN_W, ONEIROS_WIN_H, "Oneiros", oneiros_win_draw);
                if (owin) {
                    owin->reserved = oneiros_create_ctx();
                    if (owin->reserved) {
                        owin->on_click = oneiros_win_click;
                        owin->on_key   = oneiros_win_key;
                        owin->on_tick  = oneiros_win_tick;   // ~30 Hz: stream a few chars per tick
                        owin->on_close = oneiros_win_close;  // free the model + scratch
                    }
                }
            }
            break;
        case 14: // Games folder — a desktop shelf of the built-in games
            launch_games_folder();
            break;
        case 15: // Selene — the NyxOS web browser
            launch_selene();
            break;
        case 16: // DOOM — the ring-3 game, run in a window (also reachable from the Games folder).
            launch_doom_windowed();
            break;
    }
    redraw_all();
}

void compositor_init(void) {
    // Free any windows from a previous session (so logout -> re-login starts with a
    // clean desktop instead of leaking the old user's windows + their contexts).
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i]) {
            if (windows[i]->reserved) kfree(windows[i]->reserved);
            kfree(windows[i]);
            windows[i] = NULL;
        }
    }
    window_count = 0; next_id = 100; focused_id = 0; drag_id = 0; resize_id = 0; quit = 0;
    current_workspace = 0; start_menu_open = 0; user_menu_open = 0; cal_popup_open = 0; net_popup_open = 0; spk_popup_open = 0; cursor_saved = 0;
    apply_volume();   // sync the SB16 master mixer to the current volume/mute at startup

    taskbar_bg = THEME_TASKBAR_BG;
    taskbar_fg = THEME_TASKBAR_FG;
    taskbar_hl = THEME_TASKBAR_HL;   // active Menu button + focused window = brand accent
    desktop_bg = fb_rgb(30,35,50);
    title_active = THEME_TITLE_ACTIVE;      // was an off-brand blue (60,130,200)
    title_inactive = THEME_TITLE_INACTIVE;
    init_desktop_icons();
}

// Every window_create() call site writes its geometry as literals — 640x400 for
// the Terminal, 580x480 for Paint. Those numbers were all authored while looking
// at 1024x768, so that is the grid they are expressed in. State it once here
// instead of leaving it as folklore in nineteen call sites.
#define DESIGN_W 1024
#define DESIGN_H  768

// Geometry scales are Q16.16. The extra precision is not fussiness — see
// scale_u below for what Q8.8 actually cost.
#define SCALE_ONE 65536u

// Scale from the design grid to the live framebuffer, aspect-preserving and
// SHRINK-ONLY. Shrinking is the half that matters: at 640x480 the Terminal's
// authored 640x400 is the entire screen width and most of its height, so v5.9.5's
// clamp — which could only cap it at the screen edge — turned "a window" into
// "the desktop". Scaled instead, it lands at 400x250 and 640x480 gets the same
// layout as 1024x768, just smaller.
//
// Growing is deliberately NOT done. On a 1280x1024 screen the right answer is
// more room for windows, not one bigger window, and blowing up fixed-pixel
// content (the font, the calculator's button grid) would only make it blurrier.
static uint32_t design_scale(void) {
    uint32_t sx = (fb_get_width()  * SCALE_ONE) / DESIGN_W;
    uint32_t sy = (fb_get_height() * SCALE_ONE) / DESIGN_H;
    uint32_t s  = sx < sy ? sx : sy;
    return s > SCALE_ONE ? SCALE_ONE : s;
}

// Apply a scale, rounding to nearest instead of truncating.
//
// Both halves of that matter, because these scales get applied AGAIN when the
// resolution changes back, so any systematic bias compounds every time the user
// toggles modes. Measured on the real thing at Q8.8: 1024 -> 640 -> 1024 brought
// the Terminal back one pixel narrower, every single round trip. The culprit was
// the scale itself — 1024/640 is 1.6, which Q8.8 can only hold as 409/256 =
// 1.5977, so each round trip quietly multiplied the window by 0.9985.
//
// Q16.16 holds it as 104857/65536 = 1.59999, and with round-to-nearest the size
// round-trips exactly: 640 -> 400 -> 640, measured identical over three trips.
//
// POSITION can still move one pixel, once. A window at x=300 maps to 187.5 on
// the smaller screen, and no amount of precision decides that tie for us — it
// lands on 188 and comes back as 301. That is lost information, not lost
// precision. What matters is that it then stays there: 301 -> 188 -> 301 is a
// fixed point, so the shift happens on the first trip and never again (verified
// over three consecutive 1024 -> 640 -> 1024 cycles).
static inline int scale_i(int v, uint32_t s) {
    return (int)(((int64_t)v * (int)s + (SCALE_ONE / 2)) >> 16);
}
static inline uint32_t scale_u(uint32_t v, uint32_t s) {
    return (uint32_t)(((uint64_t)v * s + (SCALE_ONE / 2)) >> 16);
}

// Clamp a window INTO the current framebuffer. Shared by window_create (born
// on-screen) and windows_reflow (stays on-screen across a mode change) so the
// two paths cannot drift apart.
//
// Order matters: cap the SIZE against the usable area first, then move the
// window back inside, then floor at the origin — repositioning before capping
// would just push an oversized window off the opposite edge.
static void window_clamp(window_t* win) {
    int fw = (int)fb_get_width();
    int fh = (int)fb_get_height();
    int usable_h = fh - TASKBAR_H - TITLE_H;
    if ((int)win->w > fw) win->w = (uint32_t)fw;
    if ((int)win->h > usable_h && usable_h > 0) win->h = (uint32_t)usable_h;
    if (win->x + (int)win->w > fw) win->x = fw - (int)win->w;
    if (win->y + (int)win->h + TITLE_H > fh - TASKBAR_H)
        win->y = fh - TASKBAR_H - (int)win->h - TITLE_H;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;
}

// Re-flow every open window when the screen size changes underneath it.
//
// Both of the geometry guards so far only run at BIRTH — v5.9.5 clamps a window
// as it is created, and the scaling above sizes it as it is created. Nothing had
// ever revisited a window that was already open when the mode changed, and the
// most visible casualty was the Settings window itself: switching 1024x768 ->
// 640x480 from its own Display tab left the window being clicked hanging 20px
// off the right edge and 18px under the taskbar.
//
// Scale by the OLD->NEW screen ratio rather than re-deriving from the design
// grid, so a window the user has since dragged or resized keeps the arrangement
// they chose instead of snapping back to wherever it was authored.
// Defined further down beside window_snap; forward-declared because the re-flow
// below needs to re-derive snapped windows onto the new framebuffer.
static void apply_snap_geom(window_t* win);
static int  win_is_snapped(int state);

static void windows_reflow(uint32_t old_fw, uint32_t old_fh) {
    if (!old_fw || !old_fh) return;
    uint32_t sx = (fb_get_width()  * SCALE_ONE) / old_fw;
    uint32_t sy = (fb_get_height() * SCALE_ONE) / old_fh;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* win = windows[i];
        if (!win) continue;

        // normal_* has to travel too. It is where un-maximising lands, so leaving
        // it on the old screen's coordinates would stage a window that looks fine
        // while maximised and jumps off-screen the moment it is restored.
        win->normal_x = scale_i(win->normal_x, sx);
        win->normal_y = scale_i(win->normal_y, sy);
        win->normal_w = scale_u(win->normal_w, sx);
        win->normal_h = scale_u(win->normal_h, sy);
        if (win->normal_w < MIN_WIN_W) win->normal_w = MIN_WIN_W;
        if (win->normal_h < MIN_WIN_H) win->normal_h = MIN_WIN_H;

        // Maximised and snapped windows are defined by the SCREEN, not by their
        // history: re-derive their rect from the new framebuffer instead of
        // scaling it, or rounding would leave a seam at the edge (maximised) or
        // down the middle (snapped). normal_* was already scaled above, so
        // restoring one of these later still lands where the user left it.
        // (Since v5.9.7 Alt+Up maximises and Alt+Left/Right snap from the
        // keyboard, so both of these branches are now headlessly exercised.)
        if (win->state == WSTATE_MAXIMIZED || win_is_snapped(win->state)) {
            apply_snap_geom(win);   // re-derive maximize AND snap from the shared gap-aware path
            continue;
        }

        win->x = scale_i(win->x, sx);
        win->y = scale_i(win->y, sy);
        win->w = scale_u(win->w, sx);
        win->h = scale_u(win->h, sy);
        if (win->w < MIN_WIN_W) win->w = MIN_WIN_W;
        if (win->h < MIN_WIN_H) win->h = MIN_WIN_H;
        window_clamp(win);
    }
}

// Switch the display mode and bring the whole desktop across with it.
//
// One function because there are now two ways in — the Settings Display tab and
// the `setres` shell command — and a mode switch that forgot half its follow-up
// work would be a bug you could only see at one resolution. It also gives the
// re-flow a seam a script can drive, instead of the change being reachable only
// by clicking a button inside the very window the re-flow has to rescue.
void display_set_mode(uint32_t w, uint32_t h) {
    uint32_t old_fw = fb_get_width(), old_fh = fb_get_height();
    if (w == old_fw && h == old_fh) return;

    printf("[DISPLAY] Switching to %ux%u...\n", w, h);
    vbe_set_mode(w, h, 32);
    fb_init(w, h, 32, (void*)0xE0000000);

    // Re-flow the desktop icons for the new width. Without this the layout kept
    // the OLD screen's coordinates, so shrinking the resolution stranded icons
    // off the right edge with no way to reach them. A full re-flow (rather than
    // clamping each icon back inside) is what desktops conventionally do on a
    // mode change, and it cannot leave two icons stacked on one another the way
    // clamping to the edge would.
    init_desktop_icons();
    windows_reflow(old_fw, old_fh);

    printf("[DISPLAY] Now %ux%u, %d window(s) re-flowed\n",
           fb_get_width(), fb_get_height(), window_count);
}

// Copy `title` into win->title with truncation to MAX_TITLE-1 and NUL-termination.
// ONE definition shared by window_create and window_set_title so the two can never
// drift on the truncation rule. No redraw — callers that want it on screen do that.
static void window_store_title(window_t* win, const char* title) {
    if (!title) title = "";
    int sl = strlen(title);
    if (sl >= MAX_TITLE) sl = MAX_TITLE - 1;
    memcpy(win->title, title, sl);
    win->title[sl] = '\0';
}

window_t* window_create(int x, int y, uint32_t w, uint32_t h, const char* title, window_draw_fn draw) {
    if (window_count >= MAX_WINDOWS) return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i]) { slot = i; break; }
    if (slot < 0) return NULL;

    window_t* win = (window_t*)kmalloc(sizeof(window_t));
    if (!win) return NULL;
    // kmalloc does not zero. Every field this function does NOT assign was heap
    // garbage — including the on_pressed / on_key callback pointers, which the
    // compositor guards with `if (win->on_pressed)` before calling. A non-NULL
    // garbage value passes that guard and is then CALLED, so opening a window
    // could jump to an arbitrary address. Zero first, assign after.
    memset_asm(win, 0, sizeof(window_t));

    // Map the authored geometry onto this screen BEFORE the minimum-size floor
    // and the clamp. Order is the whole point: once everything has shrunk to fit,
    // the clamp usually has nothing left to do, so it goes back to being the
    // safety net it was meant to be instead of the thing that decides the layout.
    //
    // Position scales with size, or a window shrunk to 62% of its width would
    // still start at its full-size x and crowd the right edge.
    {
        uint32_t s = design_scale();
        if (s < SCALE_ONE) {
            w = scale_u(w, s); h = scale_u(h, s);
            x = scale_i(x, s); y = scale_i(y, s);
        }
    }
    win->w = w < MIN_WIN_W ? MIN_WIN_W : w;
    win->h = h < MIN_WIN_H ? MIN_WIN_H : h;
    win->x = x; win->y = y;

    // Backstop for whatever the scale did not save: a window born partly
    // off-screen is not just ugly, it can be unreachable, because the title bar
    // is the drag handle — if that is what went off the edge there is no way to
    // pull the window back. Clamping here rather than at each call site means no
    // future caller can reintroduce it either.
    window_clamp(win);
    // normal_* is what un-maximising restores to, so it must record the CLAMPED
    // geometry — copying the caller's raw request would send the window straight
    // back off-screen the first time it was maximised and restored.
    // (Read these from win->, not from the local x/y: window_clamp adjusts the
    // window, so the locals still hold the caller's unclamped request.)
    win->normal_x = win->x; win->normal_y = win->y;
    win->normal_w = win->w; win->normal_h = win->h;
    win->z_order = window_count;
    win->visible = 1; win->state = WSTATE_NORMAL;
    win->dragging = 0; win->resizing = 0;
    win->focused = 0; win->id = next_id++;
    win->workspace = current_workspace;
    win->has_close = 1; win->has_min = 1; win->has_max = 1;
    win->draw = draw;
    win->on_key = NULL;
    win->on_click = NULL;
    win->on_mousemove = NULL;
    win->reserved = NULL;
    window_store_title(win, title);
    windows[slot] = win;
    window_count++;
    window_focus(win->id);
    return win;
}

// Change a window's title-bar text after creation. find_window rejects an unknown id
// (returns -1) BEFORE any redraw, so the KAT can exercise that path headless; the
// success path copies via the shared helper and recomposites so the title bar AND the
// taskbar button (both read win->title) pick up the new text.
int window_set_title(int id, const char* title) {
    window_t* win = find_window(id);
    if (!win) return -1;
    window_store_title(win, title);
    redraw_all();
    return 0;
}

// KAT: window_set_title's copy logic (via the shared window_store_title) truncates and
// NUL-terminates like window_create, and the id lookup rejects an unknown window. The
// copy is checked on a throwaway window_t (no redraw); the reject path returns before any
// recomposite, so the whole test runs headless in the idle compositor.
int title_set_selftest(void) {
    static window_t fake;
    memset_asm(&fake, 0, sizeof fake);
    int rc = 0;
    window_store_title(&fake, "hello");
    if (strcmp(fake.title, "hello") != 0) rc = 1;
    else { window_store_title(&fake, 0); if (fake.title[0] != '\0') rc = 2; }   // NULL -> ""
    if (!rc) {                                                                  // over-long truncates
        static char longt[MAX_TITLE + 16];
        for (int i = 0; i < MAX_TITLE + 15; i++) longt[i] = 'A';
        longt[MAX_TITLE + 15] = '\0';
        window_store_title(&fake, longt);
        if ((int)strlen(fake.title) != MAX_TITLE - 1) rc = 3;
        else if (fake.title[MAX_TITLE - 1] != '\0')   rc = 4;
    }
    if (!rc && window_set_title(-31337, "x") != -1) rc = 5;                     // unknown id -> -1
    return rc;
}

static void focus_next_window(void) {
    // Find the highest-z-order visible, non-minimized window
    window_t* best = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i] || !windows[i]->visible) continue;
        if (windows[i]->state == WSTATE_MINIMIZED) continue;
        if (windows[i]->workspace != current_workspace) continue;
        if (!best || windows[i]->z_order > best->z_order)
            best = windows[i];
    }
    if (best) {
        window_focus(best->id);
    } else {
        focused_id = 0;
    }
}

// Alt+Tab: focus the next window in a stable, repeatable cycle.
//
// Deliberately NOT focus_next_window()'s highest-z-order pick. That is the right
// choice on destroy (fall back to whatever is on top), but as a cycler it would
// only ever toggle the top two windows, because focusing one bumps its z-order
// above the other. Walking the array in SLOT order instead — slots are stable
// for a window's whole life — makes repeated Alt+Tab visit every eligible window
// in a fixed rotation and come back round, which is what a cycler should do.
static void window_cycle_focus(void) {
    int start = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->id == focused_id) { start = i; break; }
    // start == -1 (nothing focused) makes the first probe land on slot 0.
    for (int step = 1; step <= MAX_WINDOWS; step++) {
        int i = (start + step) % MAX_WINDOWS;
        window_t* w = windows[i];
        if (!w || !w->visible || w->state == WSTATE_MINIMIZED) continue;
        if (w->workspace != current_workspace) continue;
        window_focus(w->id);
        return;
    }
}

void window_destroy(int id) {
    window_t* win = find_window(id);
    if (!win) return;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] == win) { windows[i] = NULL; break; }
    window_count--;
    if (win->on_close)          // let the app release ctx sub-allocations while `reserved` is still valid
        win->on_close(win);
    if (win->reserved)
        kfree(win->reserved);
    kfree(win);
    if (focused_id == id)
        focus_next_window();
}

void window_focus(int id) {
    window_t* win = find_window(id);
    if (!win || !win->visible || win->state == WSTATE_MINIMIZED) return;
    win->focused = 1;
    win->z_order = window_count + 10;
    focused_id = id;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->id != id)
            windows[i]->focused = 0;
}

void window_move(int id, int x, int y) {
    window_t* win = find_window(id);
    if (!win) return;
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    // Allow off-screen during resize (clamp is done in window_resize)
    if (!win->resizing) {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + (int)win->w > (int)fw) x = (int)fw - (int)win->w;
        if (y + (int)win_total_h(win) > (int)fh - (int)TASKBAR_H)
            y = (int)fh - (int)TASKBAR_H - (int)win_total_h(win);
    }
    win->x = x; win->y = y;
}

void window_resize(int id, uint32_t w, uint32_t h) {
    window_t* win = find_window(id);
    if (!win) return;
    win->w = w < MIN_WIN_W ? MIN_WIN_W : w;
    win->h = h < MIN_WIN_H ? MIN_WIN_H : h;
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    if (win->x + win->w > fw) win->w = fw - win->x;
    if (win->y + win_total_h(win) > fh - TASKBAR_H) win->h = fh - TASKBAR_H - win->y - TITLE_H;
}

void window_minimize(int id) {
    window_t* win = find_window(id);
    if (!win) return;
    win->state = WSTATE_MINIMIZED;
    win->visible = 1;
    win->focused = 0;
    if (focused_id == id) focused_id = 0;
}

// Snapshot the restore-to geometry — but ONLY from the normal state. If the
// window is already maximized or snapped, normal_* already holds the true
// pre-arrangement rect; overwriting it with the current full/half-screen one
// would make "restore" restore to the arrangement instead of the original.
static void save_normal_geom(window_t* win) {
    if (win->state == WSTATE_NORMAL) {
        win->normal_x = win->x; win->normal_y = win->y;
        win->normal_w = win->w; win->normal_h = win->h;
    }
}

// Fill a snapped window's rect for the CURRENT framebuffer. Shared by window_snap
// The snap states form a 2-axis grid: a horizontal side (left/right) and a
// vertical zone (full height, top half, or bottom half). These four accessors
// keep the axis logic in one place so the chord handler and the geometry code
// agree on what each state means.
static int win_is_snapped(int s) {
    return s == WSTATE_SNAP_LEFT || s == WSTATE_SNAP_RIGHT ||
           s == WSTATE_SNAP_TL   || s == WSTATE_SNAP_TR   ||
           s == WSTATE_SNAP_BL   || s == WSTATE_SNAP_BR;
}
static int win_snap_left(int s) {
    return s == WSTATE_SNAP_LEFT || s == WSTATE_SNAP_TL || s == WSTATE_SNAP_BL;
}
// Vertical zone: 0 = full height (a half), 1 = top quarter, 2 = bottom quarter.
static int win_snap_vzone(int s) {
    if (s == WSTATE_SNAP_TL || s == WSTATE_SNAP_TR) return 1;
    if (s == WSTATE_SNAP_BL || s == WSTATE_SNAP_BR) return 2;
    return 0;
}
static int win_snap_state(int left, int vzone) {
    if (vzone == 1) return left ? WSTATE_SNAP_TL : WSTATE_SNAP_TR;
    if (vzone == 2) return left ? WSTATE_SNAP_BL : WSTATE_SNAP_BR;
    return left ? WSTATE_SNAP_LEFT : WSTATE_SNAP_RIGHT;
}

// Aero-snap zone for a title-bar drag: map the cursor position (during/at the end
// of a drag) and the framebuffer size to the state a drop there should apply,
// Windows-11-style — within SNAP_EDGE px of the left/right screen edge snaps to
// that half, and the top/bottom third of that edge snaps to the corresponding
// quarter; within SNAP_EDGE px of the top edge (and not a side) maximizes;
// anywhere else is WSTATE_NORMAL (no snap — the window is left where it was
// dropped). Pure function of its inputs, so it is unit-tested on the host.
#define SNAP_EDGE 16
static int snap_zone_for_cursor(int mx, int my, int fw, int fh) {
    int near_l = mx <= SNAP_EDGE;
    int near_r = mx >= fw - 1 - SNAP_EDGE;
    if (near_l || near_r) {                          // a side edge takes the corners too
        int left  = near_l;
        int third = fh / 3;
        int vzone = (my <= third) ? 1 : (my >= fh - third) ? 2 : 0;
        return win_snap_state(left, vzone);
    }
    if (my <= SNAP_EDGE) return WSTATE_MAXIMIZED;    // top edge (not a corner) -> maximize
    return WSTATE_NORMAL;
}

// Outer on-screen footprint a window occupies when snapped to `zone` (WSTATE_MAXIMIZED
// or a WSTATE_SNAP_*), on an fw x fh framebuffer with a bottom taskbar of height tb.
// Pure geometry, the ONE source of truth for every snapped/maximized rect: the drag
// preview, the keyboard snap, the drop, the maximize, and the mode-change reflow all
// read it, so none can drift. Returns 1 and fills the out params for a snap/maximize
// zone, 0 otherwise (leaving them untouched).
//
// `g` is the WM gap: it insets the usable area by an outer margin on all four sides and
// leaves exactly one inner gap of `g` between adjacent tiles — the classic ricing
// "gaps". g = 0 is byte-identical to seamless tiling (halves/quarters meet, no crack),
// so the feature is a pure opt-in with no change to the default look. The far edge takes
// any odd pixel, so two tiles never overlap or leave a one-pixel seam.
static int snap_zone_rect(int zone, int fw, int fh, int tb, int g,
                          int* rx, int* ry, int* rw, int* rh) {
    if (g < 0) g = 0;
    int uw = fw - 2 * g;                 // usable width  after the outer margin
    int uh = (fh - tb) - 2 * g;          // usable height after the outer margin
    if (uw < 1) uw = 1;
    if (uh < 1) uh = 1;
    if (zone == WSTATE_MAXIMIZED) { *rx = g; *ry = g; *rw = uw; *rh = uh; return 1; }
    if (!win_is_snapped(zone)) return 0;
    int left = win_snap_left(zone);
    int vz   = win_snap_vzone(zone);
    int lw = (uw - g) / 2;               // left tile width (one inner gap between halves)
    if (left) { *rx = g;          *rw = lw; }
    else      { *rx = g + lw + g; *rw = uw - g - lw; }           // right takes the remainder
    int th = (uh - g) / 2;               // top tile height (one inner gap between quarters)
    if (vz == 1)      { *ry = g;          *rh = th; }            // top quarter
    else if (vz == 2) { *ry = g + th + g; *rh = uh - g - th; }   // bottom quarter
    else              { *ry = g;          *rh = uh; }            // full-height half
    return 1;
}

// KAT: the pure snap/maximize geometry, with and without gaps. Pins that g = 0 tiles the
// screen seamlessly (halves/quarters meet and cover to the edge) and that g > 0 insets
// every zone by one outer margin and leaves exactly one inner gap between adjacent tiles,
// the far edge taking the odd pixel. Guards the shared call sites from ever drifting. 0 = pass.
int snap_gap_selftest(void) {
    int x, y, w, h, fw = 1024, fh = 768, tb = 36, usable = fh - tb;   // usable = 732
    // ---- g = 0: seamless tiling, identical to the pre-gap geometry ----
    if (!snap_zone_rect(WSTATE_MAXIMIZED, fw, fh, tb, 0, &x, &y, &w, &h)) return 1;
    if (x || y || w != fw || h != usable) return 2;
    snap_zone_rect(WSTATE_SNAP_LEFT,  fw, fh, tb, 0, &x, &y, &w, &h);
    if (x != 0 || y != 0 || h != usable) return 3;
    int lw = w;
    snap_zone_rect(WSTATE_SNAP_RIGHT, fw, fh, tb, 0, &x, &y, &w, &h);
    if (x != lw || x + w != fw || h != usable) return 4;          // halves meet, cover to edge
    snap_zone_rect(WSTATE_SNAP_TL, fw, fh, tb, 0, &x, &y, &w, &h);
    int th = h;
    snap_zone_rect(WSTATE_SNAP_BL, fw, fh, tb, 0, &x, &y, &w, &h);
    if (y != th || y + h != usable) return 5;                     // quarters meet, cover to edge
    // ---- g = 12: one outer margin every side, one inner gap between tiles ----
    int g = 12;
    snap_zone_rect(WSTATE_MAXIMIZED, fw, fh, tb, g, &x, &y, &w, &h);
    if (x != g || y != g || w != fw - 2 * g || h != usable - 2 * g) return 6;
    snap_zone_rect(WSTATE_SNAP_LEFT,  fw, fh, tb, g, &x, &y, &w, &h);
    if (x != g || y != g || h != usable - 2 * g) return 7;
    int Lx = x, Lw = w;
    snap_zone_rect(WSTATE_SNAP_RIGHT, fw, fh, tb, g, &x, &y, &w, &h);
    if (x != Lx + Lw + g) return 8;                               // exactly one inner gap
    if (x + w != fw - g) return 9;                                // right ends at the margin
    snap_zone_rect(WSTATE_SNAP_TL, fw, fh, tb, g, &x, &y, &w, &h);
    int Ty = y, Th = h;
    snap_zone_rect(WSTATE_SNAP_BL, fw, fh, tb, g, &x, &y, &w, &h);
    if (y != Ty + Th + g) return 10;                             // one inner gap, vertical
    if (y + h != (fh - tb) - g) return 11;                       // bottom ends at the margin
    return 0;
}

// While a title-bar drag is in progress, trace a 3px accent border around the zone an
// Aero-snap would drop the window into (the exact rect window_snap_to will use), so the
// snap is predictable — the Windows drag-to-edge snap hint. Nothing is drawn when the
// window is not being dragged or the cursor is not in a snap zone. Called from
// redraw_all after the windows, so the hint sits on top.
static void draw_snap_preview(void) {
    if (!drag_id) return;
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    int zone = snap_zone_for_cursor(mouse_x, mouse_y, fw, fh);
    int rx, ry, rw, rh;
    if (!snap_zone_rect(zone, fw, fh, TASKBAR_H, g_gaps, &rx, &ry, &rw, &rh)) return;
    uint32_t c = fb_rgb(150, 110, 235);
    const int T = 3;                                  // border thickness
    fb_fill_rect(rx, ry, rw, T, c);                   // top
    fb_fill_rect(rx, ry + rh - T, rw, T, c);          // bottom
    fb_fill_rect(rx, ry, T, rh, c);                   // left
    fb_fill_rect(rx + rw - T, ry, T, rh, c);          // right
}

// Place `win` on the rect its state maps to — a snap zone OR maximize — for the CURRENT
// framebuffer and the active gap (g_gaps). Every path routes through here (the keyboard
// snap, the drag drop, the maximize, and the mode-change reflow) so no two can drift and
// one gap value governs them all. win->h drops TITLE_H because the struct's h is the
// content height below the title bar. A non-snapped, non-maximized window is untouched.
static void apply_snap_geom(window_t* win) {
    int rx, ry, rw, rh;
    if (!snap_zone_rect(win->state, (int)fb_get_width(), (int)fb_get_height(),
                        TASKBAR_H, g_gaps, &rx, &ry, &rw, &rh)) return;
    win->x = rx; win->y = ry;
    win->w = (uint32_t)rw;
    win->h = (uint32_t)(rh > (int)TITLE_H ? rh - (int)TITLE_H : 1);
}

void window_maximize(int id) {
    window_t* win = find_window(id);
    if (!win) return;
    if (win->state == WSTATE_MAXIMIZED) { window_restore(id); return; }
    save_normal_geom(win);   // no-op if coming from a snap, preserving the original
    win->state = WSTATE_MAXIMIZED;
    apply_snap_geom(win);    // re-derive the rect (honoring the active gap) from ONE place
}

// Alt+Left / Alt+Right: set the horizontal side of the snap.
//  - from normal/maximized: snap to the full-height half on that side;
//  - from a snap already on that side: toggle back to the pre-snap geometry
//    (this is what makes Alt+Left double as a spring-loaded un-snap);
//  - from a snap on the OTHER side: flip sides, keeping the vertical zone, so a
//    top-right quarter slides to a top-left quarter.
void window_snap(int id, int left) {
    window_t* win = find_window(id);
    if (!win) return;
    if (win_is_snapped(win->state)) {
        if (win_snap_left(win->state) == left) { window_restore(id); return; }
        win->state = win_snap_state(left, win_snap_vzone(win->state));
    } else {
        save_normal_geom(win);
        win->state = win_snap_state(left, 0);   // full-height half
    }
    apply_snap_geom(win);
}

// Alt+Up / Alt+Down when the window is already snapped: slide the vertical zone
// along a single ladder  max <- top <- half <- bottom <- normal. up=1 climbs
// toward maximize, up=0 descends toward the un-snapped window. The ends spill
// out of the snap grid: climbing past "top" maximizes, descending past "bottom"
// restores. Returns nothing; the caller only reaches here when win_is_snapped.
static void window_snap_vslide(int id, int up) {
    window_t* win = find_window(id);
    if (!win) return;
    int left = win_snap_left(win->state);
    int vz   = win_snap_vzone(win->state);   // 0 half, 1 top, 2 bottom
    if (up) {
        if (vz == 0)      { win->state = win_snap_state(left, 1); apply_snap_geom(win); } // half -> top
        else if (vz == 2) { win->state = win_snap_state(left, 0); apply_snap_geom(win); } // bottom -> half
        else                window_maximize(win->id);                                     // top -> maximize
    } else {
        if (vz == 1)      { win->state = win_snap_state(left, 0); apply_snap_geom(win); } // top -> half
        else if (vz == 0) { win->state = win_snap_state(left, 2); apply_snap_geom(win); } // half -> bottom
        else                window_restore(win->id);                                      // bottom -> un-snap
    }
}

void window_restore(int id) {
    window_t* win = find_window(id);
    if (!win) return;
    win->x = win->normal_x; win->y = win->normal_y;
    win->w = win->normal_w; win->h = win->normal_h;
    win->state = WSTATE_NORMAL;
}

// Drop-snap: SET a window outright to `state` (from a title-bar drag onto a screen
// edge), unlike window_snap's Alt+arrow toggling. `state` is WSTATE_MAXIMIZED or a
// WSTATE_SNAP_*; anything else is ignored. save_normal_geom (a no-op unless the
// window is currently NORMAL) preserves the pre-snap rect for a later restore, and
// the layout is re-derived by the same apply_snap_geom the keyboard path uses.
static void window_snap_to(int id, int state) {
    window_t* win = find_window(id);
    if (!win) return;
    if (state == WSTATE_MAXIMIZED) {
        if (win->state != WSTATE_MAXIMIZED) window_maximize(id);   // saves normal + maximizes
        return;
    }
    if (!win_is_snapped(state)) return;
    save_normal_geom(win);
    win->state = state;
    apply_snap_geom(win);
}

void window_set_workspace(int id, int ws) {
    window_t* win = find_window(id);
    if (!win) return;
    win->workspace = ws;
}

int window_get_count(void) { return window_count; }

int window_get_ids(int* ids, int max) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS && n < max; i++)
        if (windows[i]) ids[n++] = windows[i]->id;
    return n;
}

static void icon_disc(int ccx, int ccy, int r, uint32_t col);  // defined below (moon emblems)

// A single, subtle first-run greeting card — replaces the old three stacked boot
// windows (a keyboard-shortcut list, an About box, and a Terminal). The minimal
// desktop (v6.4.48+) opens to a clean wallpaper; this one centered card just says
// hello, shows the version, and points at the Start menu. The user can close it
// for a pristine desktop. Nyx identity: a night sky with the crescent moon.
static void welcome_draw_fn(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    (void)win;
    // cy already sits below the title bar (draw is called with y + TITLE_H), so
    // the usable client height is ch - TITLE_H. A SOLID night fill (not a gradient)
    // keeps the crescent carve and the scaled wordmark seam-free.
    int H = (int)ch - TITLE_H;
    if (H < 1) H = 1;
    uint32_t sky = fb_rgb(24, 20, 44);   // Nyx night — deep purple
    fb_fill_rect(cx, cy, cw, (uint32_t)H, sky);

    // A fixed scatter of stars (constant positions so they don't shimmer on each
    // redraw), guarded so a design-scaled small card never draws past the client.
    static const int star[][2] = {
        {40,26},{300,40},{92,150},{330,120},{20,90},{355,70},{250,22},{132,30},{206,158}
    };
    for (unsigned i = 0; i < sizeof(star)/sizeof(star[0]); i++)
        if (star[i][0] < (int)cw - 2 && star[i][1] < H - 2)
            fb_fill_rect(cx + star[i][0], cy + star[i][1], 2, 2, fb_rgb(200,200,230));

    // The crescent moon (left): a pale disc with a sky-coloured disc carved out of
    // its upper-right — the same trick as the Selene emblem, seam-free since the
    // carve colour is exactly the solid background.
    int mcx = cx + 74, mcy = cy + H / 2, R = 44;
    icon_disc(mcx, mcy, R, fb_rgb(232, 230, 248));
    icon_disc(mcx + 19, mcy - 11, R - 3, sky);

    // Text column (right).
    int tx = cx + 150;
    font_draw_string_scaled(tx, cy + 26, "NyxOS", fb_rgb(206, 186, 255), sky, 2);
    char vbuf[48];
    snprintf(vbuf, sizeof(vbuf), "Version %s", KERNEL_VERSION);
    font_draw_string_trans(tx + 2, cy + 66,  vbuf,                        fb_rgb(176, 176, 200));
    font_draw_string_trans(tx + 2, cy + 92,  "Goddess of the night",     fb_rgb(150, 150, 178));
    font_draw_string_trans(tx + 2, cy + 128, "Open Start to launch apps", fb_rgb(150, 194, 255));
}

enum { SETTINGS_TAB_INFO, SETTINGS_TAB_DISPLAY, SETTINGS_TAB_KEYBOARD };

/* Settings layout — the button-row Y offsets (content-relative) are shared
 * between the draw fn and the click hit-test so they can never drift again: the
 * click handler used cy+48 while draw placed the buttons at cy+84 / cy+88, so
 * every Resolution and Keyboard-layout button click missed and did nothing. */
#define SETTINGS_RES_BTN_Y  84   /* Display tab: Resolution buttons row           */
#define SETTINGS_KBD_BTN_Y  88   /* Keyboard tab: US / ES layout buttons row       */
#define SETTINGS_BTN_H      26

// Resolution modes offered in Settings -> Display. ONE table, used by BOTH the
// draw fn (a 4-column grid) and the click hit-test, so the button geometry can
// never drift. Adding a mode here adds a button AND makes it clickable.
typedef struct { const char* label; uint16_t w, h; } res_mode_t;
static const res_mode_t g_res_modes[] = {
    {"1024x768", 1024, 768},  {"1280x720", 1280, 720},
    {"1280x1024", 1280, 1024},{"1366x768", 1366, 768},
    {"1440x900", 1440, 900},  {"1600x900", 1600, 900},
    {"1680x1050", 1680, 1050},{"1920x1080", 1920, 1080},
};
#define RES_MODE_N   (int)(sizeof(g_res_modes) / sizeof(g_res_modes[0]))
#define RES_COLS     4
#define RES_BTN_W    110
#define RES_BTN_GAP  8
#define RES_ROW_GAP  6

static void settings_draw_fn(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    (void)cw; (void)ch;
    int* tab = win->reserved ? (int*)win->reserved : NULL;
    int cur_tab = tab ? *tab : 0;
    uint32_t char_h = FONT_HEIGHT;
    fb_fill_rect(cx, cy, cw, ch, fb_rgb(30,30,35));

    // Tab bar
    int tab_h = 28;
    const char* tabs[] = {"Info", "Display", "Keyboard"};
    for (int i = 0; i < 3; i++) {
        int tx = cx + 4 + i * 90;
        uint32_t tbg = (i == cur_tab) ? fb_rgb(50,60,80) : fb_rgb(45,45,50);
        fb_fill_rect(tx, cy + 2, 86, tab_h - 2, tbg);
        font_draw_string(tx + (86 - strlen(tabs[i]) * FONT_WIDTH) / 2,
                         cy + 2 + (tab_h - 2 - char_h) / 2, tabs[i], fb_rgb(220,220,220), tbg);
    }
    fb_fill_rect(cx, cy + tab_h, cw, 1, fb_rgb(70,70,80));

    int y = cy + tab_h + 12;
    char buf[128];

    if (cur_tab == SETTINGS_TAB_INFO) {
        font_draw_string(cx + 10, y, "System Information", fb_rgb(100,200,100), fb_rgb(30,30,35));
        y += 24;
        snprintf(buf, sizeof(buf), "Kernel: %s %s (%s)", KERNEL_NAME, KERNEL_VERSION, KERNEL_CODENAME);
        font_draw_string(cx + 10, y, buf, fb_rgb(200,200,200), fb_rgb(30,30,35)); y += 18;
        uint32_t mi_used_kb, mi_free_kb, mi_total_kb;
        mem_pool_kb(&mi_used_kb, &mi_free_kb, &mi_total_kb);   // same source as top / nyxfetch / Nyx Monitor
        snprintf(buf, sizeof(buf), "Memory: %u MB used / %u MB (%u%% used, %u MB free)",
            mi_used_kb / 1024, mi_total_kb / 1024,
            mi_total_kb ? (uint32_t)(((uint64_t)mi_used_kb * 100) / mi_total_kb) : 0, mi_free_kb / 1024);
        font_draw_string(cx + 10, y, buf, fb_rgb(200,200,200), fb_rgb(30,30,35)); y += 18;
        snprintf(buf, sizeof(buf), "Uptime: %u sec", get_uptime_seconds());
        font_draw_string(cx + 10, y, buf, fb_rgb(200,200,200), fb_rgb(30,30,35)); y += 18;
        snprintf(buf, sizeof(buf), "Heap: %d KB", KERNEL_HEAP_SIZE / 1024);
        font_draw_string(cx + 10, y, buf, fb_rgb(200,200,200), fb_rgb(30,30,35)); y += 18;
        snprintf(buf, sizeof(buf), "Windows: %d / %d", window_count, MAX_WINDOWS);
        font_draw_string(cx + 10, y, buf, fb_rgb(200,200,200), fb_rgb(30,30,35)); y += 18;
        snprintf(buf, sizeof(buf), "Resolution: %u x %u", fb_get_width(), fb_get_height());
        font_draw_string(cx + 10, y, buf, fb_rgb(200,200,200), fb_rgb(30,30,35));
    } else if (cur_tab == SETTINGS_TAB_DISPLAY) {
        font_draw_string(cx + 10, y, "Display Settings", fb_rgb(100,200,100), fb_rgb(30,30,35));
        y += 24;
        font_draw_string(cx + 10, y, "Resolution:", fb_rgb(220,220,220), fb_rgb(30,30,35)); y += 20;

        // Resolution grid (RES_COLS columns). Row Y + geometry are shared with the
        // click hit-test (g_res_modes + the RES_*/SETTINGS_* constants); the active
        // mode is highlighted green.
        for (int i = 0; i < RES_MODE_N; i++) {
            int col = i % RES_COLS, row = i / RES_COLS;
            int bx = cx + 10 + col * (RES_BTN_W + RES_BTN_GAP);
            int by = cy + SETTINGS_RES_BTN_Y + row * (SETTINGS_BTN_H + RES_ROW_GAP);
            int cur = (fb_get_width() == g_res_modes[i].w && fb_get_height() == g_res_modes[i].h);
            uint32_t bg = cur ? fb_rgb(50,90,50) : fb_rgb(60,70,80);
            fb_fill_rect(bx, by, RES_BTN_W, SETTINGS_BTN_H, bg);
            font_draw_string(bx + (RES_BTN_W - (int)strlen(g_res_modes[i].label) * FONT_WIDTH) / 2,
                             by + (SETTINGS_BTN_H - (int)char_h) / 2, g_res_modes[i].label, fb_rgb(220,220,220), bg);
        }
        int rrows = (RES_MODE_N + RES_COLS - 1) / RES_COLS;
        y = cy + SETTINGS_RES_BTN_Y + rrows * (SETTINGS_BTN_H + RES_ROW_GAP) + 6;
        snprintf(buf, sizeof(buf), "Current: %u x %u @ 32bpp", fb_get_width(), fb_get_height());
        font_draw_string(cx + 10, y, buf, fb_rgb(160,160,160), fb_rgb(30,30,35));
    } else if (cur_tab == SETTINGS_TAB_KEYBOARD) {
        font_draw_string(cx + 10, y, "Keyboard Layout", fb_rgb(100,200,100), fb_rgb(30,30,35));
        y += 24;
        const char* layout_name = keyboard_layout == 0 ? "US (QWERTY)" : "Spanish (ES)";
        snprintf(buf, sizeof(buf), "Current: %s", layout_name);
        font_draw_string(cx + 10, y, buf, fb_rgb(220,220,220), fb_rgb(30,30,35));
        y = cy + SETTINGS_KBD_BTN_Y;   // authoritative row Y (shared with the hit-test)
        // US button
        uint32_t us_bg = (keyboard_layout == 0) ? fb_rgb(50,90,50) : fb_rgb(60,70,80);
        fb_fill_rect(cx + 10, y, 120, SETTINGS_BTN_H, us_bg);
        font_draw_string(cx + 10 + (120 - 2 * FONT_WIDTH) / 2, y + (SETTINGS_BTN_H - char_h) / 2, "US", fb_rgb(220,220,220), us_bg);
        // ES button
        uint32_t es_bg = (keyboard_layout == 1) ? fb_rgb(50,90,50) : fb_rgb(60,70,80);
        fb_fill_rect(cx + 140, y, 120, SETTINGS_BTN_H, es_bg);
        font_draw_string(cx + 140 + (120 - 2 * FONT_WIDTH) / 2, y + (SETTINGS_BTN_H - char_h) / 2, "ES", fb_rgb(220,220,220), es_bg);
    }
}

static void settings_win_click(window_t* win, int mx, int my, int btn) {
    (void)btn;
    int* tab = win->reserved ? (int*)win->reserved : NULL;
    if (!tab) return;
    int cx = win->x, cy = win->y + TITLE_H;

    // Tab click
    if (my >= cy + 2 && my < cy + 28) {
        int rel = mx - cx;
        for (int i = 0; i < 3; i++) {
            if (rel >= 4 + i * 90 && rel < 4 + i * 90 + 86) {
                *tab = i;
                return;
            }
        }
    }

    if (*tab == SETTINGS_TAB_DISPLAY) {
        // Resolution grid — geometry shared with the draw fn (g_res_modes + RES_*).
        // display_set_mode re-flows this very window; that is fine (it re-lays out on
        // the next paint).
        for (int i = 0; i < RES_MODE_N; i++) {
            int col = i % RES_COLS, row = i / RES_COLS;
            int bx = cx + 10 + col * (RES_BTN_W + RES_BTN_GAP);
            int by = cy + SETTINGS_RES_BTN_Y + row * (SETTINGS_BTN_H + RES_ROW_GAP);
            if (mx >= bx && mx < bx + RES_BTN_W && my >= by && my < by + SETTINGS_BTN_H) {
                display_set_mode(g_res_modes[i].w, g_res_modes[i].h);
                return;
            }
        }
    } else if (*tab == SETTINGS_TAB_KEYBOARD) {
        int y = cy + SETTINGS_KBD_BTN_Y;
        // US button
        if (mx >= cx + 10 && mx < cx + 130 && my >= y && my < y + SETTINGS_BTN_H) {
            if (keyboard_layout != 0) {
                set_keyboard_layout(0);
                printf("[SETTINGS] Keyboard layout: US\n");
            }
            return;
        }
        // ES button
        if (mx >= cx + 140 && mx < cx + 260 && my >= y && my < y + SETTINGS_BTN_H) {
            if (keyboard_layout != 1) {
                set_keyboard_layout(1);
                printf("[SETTINGS] Keyboard layout: ES\n");
            }
            return;
        }
    }
}

#define NUM_DESKTOP_ICONS 10
#define ICON_SIZE 64
#define ICON_PAD 12
static const char* desktop_icon_names[] = {
    "Files", "Terminal", "Editor", "Viewer", "Settings", "Paint", "Sounds", "Calc", "Games", "Selene"
};
// Action 14 is the "Games" folder (v5.9.40, gathers Minesweeper/DOOM/Pong/Snake/Tetris);
// action 15 is Selene, the web browser (v5.9.44). Both open an in-kernel window.
static int desktop_icon_actions[] = {0, 3, 1, 2, 4, 7, 8, 11, 14, 15};
static int desktop_icon_x[NUM_DESKTOP_ICONS];
static int desktop_icon_y[NUM_DESKTOP_ICONS];

// Lay the icons out as a GRID that wraps on the screen width, instead of the one
// fixed row this used to be. That row was `20 + i * 76` with no bound: the ninth
// icon ("Mines") spans x=628..692, so at the 640x480 mode Settings offers it was
// drawn past the right edge — invisible AND unclickable, since the hit test reads
// the same coordinates. Nothing shrank the row and nothing re-ran on a mode
// change, so there was no way to get it back.
//
// At 1024x768 this computes 13 columns, so all nine still sit in one row and the
// default desktop looks exactly as before; only the narrow modes actually wrap.
#define ICON_CELL_W (ICON_SIZE + ICON_PAD)
#define ICON_CELL_H (ICON_SIZE + 16 + ICON_PAD)   /* icon + label strip + pad */
#define ICON_MARGIN 20

static void init_desktop_icons(void) {
    int fw = (int)fb_get_width();
    int cols = (fw - ICON_MARGIN) / ICON_CELL_W;
    if (cols < 1) cols = 1;                       // absurdly narrow: single column
    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        desktop_icon_x[i] = ICON_MARGIN + (i % cols) * ICON_CELL_W;
        desktop_icon_y[i] = ICON_MARGIN + (i / cols) * ICON_CELL_H;
    }
}

// Minimal desktop (user, 2026-08-02): the app-launcher icons live on the Start
// menu now (every app is reachable there), so the desktop stays CLEAN by default.
// This flag gates BOTH the icon drawing and hit-testing; a future Settings toggle
// ("Show desktop icons") can flip it back on without touching the icon machinery.
static int g_desktop_icons_visible = 0;

static int desktop_icon_hit(int mx, int my) {
    if (!g_desktop_icons_visible) return -1;   // clean desktop: no icons to hit
    uint32_t fh = fb_get_height();
    if (my >= (int)(fh - 36)) return -1;  // taskbar area
    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        if (mx >= desktop_icon_x[i] && mx < desktop_icon_x[i] + ICON_SIZE &&
            my >= desktop_icon_y[i] && my < desktop_icon_y[i] + ICON_SIZE + 16)
            return i;
    }
    return -1;
}

// Drag-reorder desktop icons
static int drag_icon_idx = -1;
static int drag_icon_ofs_x = 0, drag_icon_ofs_y = 0;

// ---- per-app desktop-icon emblems (v5.9.41) ----
// Each draws a distinct little picture inside the icon plate at (x, y) (ICON_SIZE
// square), so every app reads at a glance — the same treatment the Games folder
// gave its game emblems, instead of the one generic colour-square-plus-three-bars
// every app used to share. Drawn with fb_fill_rect within roughly the inner box
// [x+8 .. x+56] x [y+8 .. y+52].

static void emblem_files(int x, int y) {              // blue folder with a page peeking out
    fb_fill_rect(x+24, y+12, 22, 30, fb_rgb(250,250,252));     // document
    for (int r = 0; r < 4; r++) fb_fill_rect(x+28, y+18 + r*5, 14, 2, fb_rgb(150,160,175));
    fb_fill_rect(x+12, y+18, 10, 6, fb_rgb(95,155,225));       // folder tab
    fb_fill_rect(x+10, y+23, 40, 25, fb_rgb(80,140,215));      // folder front
    fb_fill_rect(x+10, y+23, 40, 3,  fb_rgb(115,170,240));     // lip highlight
}

static void emblem_terminal(int x, int y) {           // console with a green prompt
    fb_fill_rect(x+8,  y+8,  48, 42, fb_rgb(18,22,28));        // screen
    fb_fill_rect(x+8,  y+8,  48, 7,  fb_rgb(44,50,60));        // title bar
    fb_fill_rect(x+12, y+10, 3,  3,  fb_rgb(230,90,80));       // window dot
    font_draw_string(x+11, y+22, ">", fb_rgb(90,235,130), fb_rgb(18,22,28));
    fb_fill_rect(x+23, y+30, 12, 3, fb_rgb(90,235,130));       // cursor line
}

static void emblem_editor(int x, int y) {             // a page + a pencil across it
    fb_fill_rect(x+14, y+8, 30, 42, fb_rgb(250,249,244));      // page
    for (int r = 0; r < 5; r++) fb_fill_rect(x+18, y+13 + r*7, 22, 2, fb_rgb(140,140,150));
    for (int t = 0; t < 22; t++) fb_fill_rect(x+16+t, y+44-t, 3, 3, fb_rgb(240,185,50)); // pencil body
    fb_fill_rect(x+36, y+22, 4, 4, fb_rgb(60,55,50));          // pencil tip
    fb_fill_rect(x+16, y+42, 4, 4, fb_rgb(235,120,120));       // eraser
}

static void emblem_viewer(int x, int y) {             // framed picture: sun over a mountain
    fb_fill_rect(x+8,  y+10, 48, 36, fb_rgb(242,242,246));     // frame
    fb_fill_rect(x+11, y+13, 42, 30, fb_rgb(120,190,235));     // sky
    fb_fill_rect(x+41, y+17, 8,  8,  fb_rgb(250,220,90));      // sun
    for (int yy = 0; yy < 16; yy++)                            // mountain
        fb_fill_rect(x+24 - yy, y+26 + yy, 2*yy + 1, 1, fb_rgb(70,150,90));
}

static void emblem_settings(int x, int y) {           // three sliders with knobs
    uint32_t track = fb_rgb(95,100,115), knob = fb_rgb(205,210,220);
    int kx[3] = { x+40, x+18, x+32 };
    for (int r = 0; r < 3; r++) {
        int yy = y+15 + r*12;
        fb_fill_rect(x+12, yy, 40, 3, track);
        fb_fill_rect(kx[r], yy-4, 7, 11, knob);
    }
}

static void emblem_paint(int x, int y) {              // palette with paint dots
    fb_fill_rect(x+13, y+15, 36, 28, fb_rgb(228,223,213));     // palette body
    fb_fill_rect(x+40, y+31, 8, 8, fb_rgb(55,60,75));          // thumb hole (plate colour)
    fb_fill_rect(x+18, y+20, 6, 6, fb_rgb(232,72,72));         // red
    fb_fill_rect(x+29, y+18, 6, 6, fb_rgb(72,150,235));        // blue
    fb_fill_rect(x+19, y+31, 6, 6, fb_rgb(242,202,62));        // yellow
    fb_fill_rect(x+30, y+31, 6, 6, fb_rgb(82,200,112));        // green
}

static void emblem_sounds(int x, int y) {             // speaker + sound waves
    uint32_t sp = fb_rgb(232,232,238);
    fb_fill_rect(x+13, y+24, 8, 12, sp);                       // speaker box
    for (int c = 0; c < 9; c++) {                              // cone
        int h = 12 + c*2;
        fb_fill_rect(x+21 + c, y+30 - h/2, 2, h, sp);
    }
    fb_fill_rect(x+38, y+26, 2, 8,  fb_rgb(120,200,255));      // waves
    fb_fill_rect(x+43, y+22, 2, 16, fb_rgb(120,200,255));
    fb_fill_rect(x+48, y+18, 2, 24, fb_rgb(120,200,255));
}

// A filled disc (integer span fill), for the moon emblems.
static void icon_disc(int ccx, int ccy, int r, uint32_t col) {
    for (int dy = -r; dy <= r; dy++) {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
        fb_fill_rect(ccx - w, ccy + dy, 2 * w + 1, 1, col);
    }
}

static void emblem_selene(int x, int y) {             // the browser: a crescent moon + a star
    fb_fill_rect(x + 8, y + 6, ICON_SIZE - 16, ICON_SIZE - 16, fb_rgb(38, 30, 66)); // night sky
    icon_disc(x + 30, y + 32, 14, fb_rgb(232, 230, 248));   // moon
    icon_disc(x + 36, y + 29, 12, fb_rgb(38, 30, 66));      // carve the crescent
    fb_fill_rect(x + 44, y + 16, 2, 2, fb_rgb(240, 240, 255)); // a star
    fb_fill_rect(x + 18, y + 44, 2, 2, fb_rgb(220, 220, 245)); // a star
}

static void emblem_calc(int x, int y) {               // calculator: LCD + button grid
    fb_fill_rect(x+12, y+8,  40, 44, fb_rgb(60,66,80));        // body
    fb_fill_rect(x+16, y+12, 32, 10, fb_rgb(160,220,180));     // green LCD
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            fb_fill_rect(x+16 + c*11, y+26 + r*8, 8, 6, fb_rgb(205,210,220));
}

static void draw_icon_at(int i) {
    int x = desktop_icon_x[i];
    int y = desktop_icon_y[i];
    int hl = (i == drag_icon_idx);
    fb_fill_rect(x, y, ICON_SIZE, ICON_SIZE, hl ? fb_rgb(65,70,95) : fb_rgb(45,50,65));
    fb_fill_rect(x+1, y+1, ICON_SIZE-2, ICON_SIZE-2, hl ? fb_rgb(75,80,105) : fb_rgb(55,60,75));
    uint32_t icon_color[] = {
        fb_rgb(70,160,230), fb_rgb(0,220,0), fb_rgb(230,60,60),
        fb_rgb(220,220,50), fb_rgb(100,220,100), fb_rgb(220,100,220)
    };
    // Each app gets a distinct emblem so the desktop reads at a glance. "Games" is a
    // folder (a container of games, not an app); the rest are little pictures of what
    // they do. Any unnamed app falls back to the old generic colour-square-plus-bars.
    const char* nm = desktop_icon_names[i];
    uint32_t icon_bg = icon_color[i % 6];
    if      (strcmp(nm, "Games")    == 0) {
        uint32_t body = fb_rgb(235,190,70), back = fb_rgb(200,150,40);
        fb_fill_rect(x+12, y+16, 22, 8, back);                       // tab
        fb_fill_rect(x+10, y+22, ICON_SIZE-20, ICON_SIZE-34, back);  // back panel
        fb_fill_rect(x+10, y+27, ICON_SIZE-20, ICON_SIZE-39, body);  // front flap
    }
    else if (strcmp(nm, "Files")    == 0) emblem_files(x, y);
    else if (strcmp(nm, "Terminal") == 0) emblem_terminal(x, y);
    else if (strcmp(nm, "Editor")   == 0) emblem_editor(x, y);
    else if (strcmp(nm, "Viewer")   == 0) emblem_viewer(x, y);
    else if (strcmp(nm, "Settings") == 0) emblem_settings(x, y);
    else if (strcmp(nm, "Paint")    == 0) emblem_paint(x, y);
    else if (strcmp(nm, "Sounds")   == 0) emblem_sounds(x, y);
    else if (strcmp(nm, "Calc")     == 0) emblem_calc(x, y);
    else if (strcmp(nm, "Selene")   == 0) emblem_selene(x, y);
    else {
        fb_fill_rect(x+8, y+6, ICON_SIZE-16, ICON_SIZE-16, icon_bg);
        fb_fill_rect(x+12, y+10, ICON_SIZE-24, 2, fb_rgb(255,255,255));  // highlight
        fb_fill_rect(x+16, y+18, ICON_SIZE-32, 3, fb_rgb(255,255,255));  // content lines
        fb_fill_rect(x+16, y+26, ICON_SIZE-32, 3, fb_rgb(255,255,255));
        fb_fill_rect(x+16, y+34, ICON_SIZE-32, 3, fb_rgb(255,255,255));
    }
    // Label background for readability
    int tw = strlen(desktop_icon_names[i]) * 8;
    int tx = x + (ICON_SIZE - tw) / 2;
    if (tx < 0) tx = 0;
    uint32_t lw = tw + 8;
    if (lw > ICON_SIZE + 8) lw = ICON_SIZE + 8;
    fb_fill_rect(tx - 2, y + ICON_SIZE + 1, lw, FONT_HEIGHT + 2, fb_rgb(20,25,35));
    font_draw_string(tx, y + ICON_SIZE + 3, desktop_icon_names[i], fb_rgb(230,230,230), fb_rgb(20,25,35));
}

// --- Live desktop widget (conky-style) --------------------------------------------------
// An always-on panel on the wallpaper (bottom-right, behind windows) with two stacked live
// history graphs — CPU% and RAM% — from the same honest sources as top / the taskbar module
// (perf_cpu_percent + mem_pool_kb). Both scroll every WGT_SAMPLE_MS (motion), refreshed by a
// partial present of just its rect when no window covers it. CPU uses THEME_ACCENT and RAM a
// lighter shade, so the widget follows the runtime colorscheme (v6.5.28). Toggle with
// `widget = on|off` in nyx.conf.
#define WGT_W        180
#define WGT_H        182          // three stacked graphs (CPU + RAM + NET)
#define WGT_MARGIN    16
#define WGT_N        160          // history samples == graph width in px
#define WGT_SAMPLE_MS 500u        // sample + scroll cadence
#define WGT_NET_REF  131072u      // bytes/s that fills the NET bar (128 KB/s); higher clamps to 100%
// Animation partial-present: publish up to this many simultaneously-animating windows' rects
// (a game + the live monitor + a GIF, or the Nyx Flex tiles) instead of a full-screen blit.
// Beyond it — or when the combined damage covers most of the screen — a full present is cheaper.
#define TICK_MAX_RECTS 4
int g_widget_on = 1;              // set from /etc/nyx.conf (apply_nyx_config); default on
int g_widget_pos = 0;             // 0=bottom-right (default) 1=bottom-left 2=top-right 3=top-left — nyx.conf `widget_pos`
static uint8_t wgt_cpu[WGT_N], wgt_ram[WGT_N], wgt_net[WGT_N];
static uint64_t wgt_net_last;         // net_total_bytes() at the previous sample
static unsigned long wgt_net_bps;     // current NET rate (bytes/s) for the caption

// nyx.conf name <-> index for the widget corner (rice: place the monitor where you like).
static const char* widget_pos_name(int p) {
    switch (p) { case 1: return "bottom-left"; case 2: return "top-right"; case 3: return "top-left"; default: return "bottom-right"; }
}
static int widget_pos_from_name(const char* s) {
    if (strcmp(s, "bottom-left")  == 0) return 1;
    if (strcmp(s, "top-right")    == 0) return 2;
    if (strcmp(s, "top-left")     == 0) return 3;
    if (strcmp(s, "bottom-right") == 0) return 0;
    return -1;   // unknown -> caller keeps the current value
}

static void wgt_geom(int* x, int* y) {
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    int left = (g_widget_pos == 1 || g_widget_pos == 3);   // BL / TL hug the left edge
    int top  = (g_widget_pos == 2 || g_widget_pos == 3);   // TR / TL hug the top edge
    *x = left ? WGT_MARGIN : (fw - WGT_W - WGT_MARGIN);
    *y = top  ? WGT_MARGIN : (fh - (int)TASKBAR_H - WGT_H - WGT_MARGIN);  // bottom clears the taskbar
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

static uint32_t wgt_ram_pct(void) {
    uint32_t mu = 0, mt = 0; mem_pool_kb(&mu, 0, &mt);
    return mt ? (uint32_t)(((uint64_t)mu * 100) / mt) : 0;
}

// Format a bytes/second rate as a short human string ("0 B/s", "12 KB/s", "3 MB/s") for the
// NET graph caption — integer units, no decimals. Pure; KAT'd by net_rate_selftest.
static void net_rate_str(unsigned long bps, char* out, int outsz) {
    if (bps < 1024UL)                snprintf(out, outsz, "%lu B/s", bps);
    else if (bps < 1024UL * 1024UL)  snprintf(out, outsz, "%lu KB/s", bps / 1024UL);
    else                             snprintf(out, outsz, "%lu MB/s", bps / (1024UL * 1024UL));
}

// KAT: the NET-rate caption formatter — B/s under 1 KiB, KB/s under 1 MiB (truncating), else
// MB/s. 0 = pass.
int net_rate_selftest(void) {
    char b[24];
    net_rate_str(0, b, sizeof b);        if (strcmp(b, "0 B/s")     != 0) return 1;
    net_rate_str(512, b, sizeof b);      if (strcmp(b, "512 B/s")   != 0) return 2;
    net_rate_str(1023, b, sizeof b);     if (strcmp(b, "1023 B/s")  != 0) return 3;
    net_rate_str(1024, b, sizeof b);     if (strcmp(b, "1 KB/s")    != 0) return 4;
    net_rate_str(1536, b, sizeof b);     if (strcmp(b, "1 KB/s")    != 0) return 5;   // truncates
    net_rate_str(1048575, b, sizeof b);  if (strcmp(b, "1023 KB/s") != 0) return 6;
    net_rate_str(1048576, b, sizeof b);  if (strcmp(b, "1 MB/s")    != 0) return 7;
    net_rate_str(5242880, b, sizeof b);  if (strcmp(b, "5 MB/s")    != 0) return 8;
    return 0;
}

// Push the current CPU%, RAM% and NET rate and scroll all three histories one sample left.
static void wgt_push_sample(void) {
    uint32_t c = perf_cpu_percent(); if (c > 100) c = 100;
    uint32_t r = wgt_ram_pct();      if (r > 100) r = 100;
    // NET: bytes since the last sample -> a per-second rate over the WGT_SAMPLE_MS window,
    // normalized against WGT_NET_REF for the 0..100 bar height. Counters only ever climb.
    uint64_t tot = net_total_bytes();
    uint64_t delta = (tot >= wgt_net_last) ? (tot - wgt_net_last) : 0;
    wgt_net_last = tot;
    wgt_net_bps = (unsigned long)(delta * 1000u / WGT_SAMPLE_MS);
    uint32_t np = (uint32_t)(wgt_net_bps * 100u / WGT_NET_REF); if (np > 100) np = 100;
    for (int i = 1; i < WGT_N; i++) { wgt_cpu[i-1] = wgt_cpu[i]; wgt_ram[i-1] = wgt_ram[i]; wgt_net[i-1] = wgt_net[i]; }
    wgt_cpu[WGT_N - 1] = (uint8_t)c;
    wgt_ram[WGT_N - 1] = (uint8_t)r;
    wgt_net[WGT_N - 1] = (uint8_t)np;
}

// Does any visible window overlap the widget's rect? (If so, skip its partial repaint — it
// is hidden behind the window, and a full recomposite redraws it when the window moves.)
static int widget_covered(void) {
    int wx, wy; wgt_geom(&wx, &wy);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = windows[i];
        if (!w || !w->visible || w->state == WSTATE_MINIMIZED || w->workspace != current_workspace) continue;
        int rx = w->x, ry = w->y, rw = (int)w->w, rh = (int)win_total_h(w);
        if (rx < wx + WGT_W && rx + rw > wx && ry < wy + WGT_H && ry + rh > wy) return 1;
    }
    return 0;
}

// Draw one labelled history graph: a dark inset, a 50% grid line, and WGT_N bars rising from
// the baseline in `color`, with a "LABEL nn%" caption above it.
static void wgt_draw_graph(int x, int y, const char* caption, const uint8_t* series, uint32_t color) {
    font_draw_string_trans(x, y, caption, fb_rgb(212, 206, 228));
    int gx = x, gy = y + 14, gw = WGT_N, gh = 38;
    fb_fill_rect(gx, gy, gw, gh, fb_rgb(14, 14, 18));
    fb_fill_rect(gx, gy + gh / 2, gw, 1, fb_rgb(42, 42, 52));   // 50% grid line
    for (int i = 0; i < WGT_N; i++) {
        int v = series[i];
        int bh = v * gh / 100; if (bh < 1 && v > 0) bh = 1; if (bh > gh) bh = gh;
        if (bh > 0) fb_fill_rect(gx + i, gy + gh - bh, 1, bh, color);
    }
}

static void draw_desktop_widget(void) {
    if (!g_widget_on) return;
    int x, y; wgt_geom(&x, &y);
    // Opaque panel + accent frame, so a partial repaint over its own rect is self-contained.
    fb_fill_rect(x, y, WGT_W, WGT_H, fb_rgb(24, 24, 30));
    fb_fill_rect(x, y, WGT_W, 1, THEME_ACCENT);
    fb_fill_rect(x, y + WGT_H - 1, WGT_W, 1, col_darken(THEME_ACCENT, 45));
    fb_fill_rect(x, y, 1, WGT_H, col_darken(THEME_ACCENT, 20));
    fb_fill_rect(x + WGT_W - 1, y, 1, WGT_H, col_darken(THEME_ACCENT, 45));
    uint32_t cpu = perf_cpu_percent(); if (cpu > 100) cpu = 100;
    // Two stacked live graphs: CPU in the accent, RAM in a lighter shade of it (both follow
    // the runtime colorscheme). Each scrolls every WGT_SAMPLE_MS → the motion the rice wants.
    char cap[24];
    snprintf(cap, sizeof cap, "CPU %u%%", cpu);
    wgt_draw_graph(x + 10, y + 6,   cap, wgt_cpu, THEME_ACCENT);
    snprintf(cap, sizeof cap, "RAM %u%%", wgt_ram_pct());
    wgt_draw_graph(x + 10, y + 64,  cap, wgt_ram, col_lighten(THEME_ACCENT, 35));
    char nr[16]; net_rate_str(wgt_net_bps, nr, sizeof nr);
    snprintf(cap, sizeof cap, "NET %s", nr);
    wgt_draw_graph(x + 10, y + 122, cap, wgt_net, col_lighten(THEME_ACCENT, 60));
}

static void draw_desktop_icons(void) {
    if (!g_desktop_icons_visible) return;  // minimal desktop — apps live in the Start menu
    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        if (i == drag_icon_idx) continue; // drawn last on top
        draw_icon_at(i);
    }
    if (drag_icon_idx >= 0) draw_icon_at(drag_icon_idx);
}

// nyx.conf `border` — the focused window's outline color. Default (or "accent") follows
// the UI accent, as the frame always has; a palette color name (Morado/Azul/Turquesa/…)
// outlines focused windows in a distinct color — the canonical rice "focused border color".
// Returns the rgb, or 0 meaning "follow the accent" (an unknown name falls back to that).
static uint32_t border_resolve(const char* name) {
    if (!name || strcmp(name, "accent") == 0) return 0;
    int idx = wallpaper_color_from_name(name);
    return idx >= 0 ? wallpaper_color_rgb(idx) : 0;
}

// KAT: the border-color resolver shared by the focused `border` and the unfocused
// `border_inactive` knobs — "accent"/unknown/NULL -> 0 (follow accent / neutral bevel), a
// real palette name -> that exact rgb. Also exercises the full nyx.conf parse chain for the
// focused+inactive pair (distinct keys, no prefix collision). 0 = pass.
static void border_config_str(uint32_t rgb, char* out, int cap);   // save_nyx_config's rgb->name reverse
int border_color_selftest(void) {
    if (border_resolve("accent") != 0) return 1;
    if (border_resolve(0) != 0) return 2;
    if (border_resolve("zzz") != 0) return 3;
    if (border_resolve("Morado")   != fb_rgb(130, 90, 210)) return 4;
    if (border_resolve("Turquesa") != fb_rgb(40, 160, 175)) return 5;
    if (border_resolve("Carbon")   != fb_rgb(45, 50, 70))   return 6;
    const char* cfg = "border = Turquesa\nborder_inactive = Carbon\n";
    char v[64];
    if (!nyxconf_get(cfg, "border", v, sizeof v)          || border_resolve(v) != fb_rgb(40, 160, 175)) return 7;
    if (!nyxconf_get(cfg, "border_inactive", v, sizeof v) || border_resolve(v) != fb_rgb(45, 50, 70))   return 8;
    // Persistence round-trip (save_nyx_config): border_resolve(border_config_str(rgb)) == rgb, so a
    // border color survives a GUI save->reload. 0 maps through "accent" back to 0.
    char rb[24];
    border_config_str(0, rb, sizeof rb);                          if (border_resolve(rb) != 0) return 9;
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        uint32_t rgb = wallpaper_color_rgb(i);
        border_config_str(rgb, rb, sizeof rb);
        if (border_resolve(rb) != rgb) return 10;
    }
    return 0;
}

// nyx.conf `accent` may name a preset OR give a raw #RRGGBB hex, so a rice can pick ANY accent
// color, not just the 11 presets. Parse exactly six hex digits with an optional leading '#'
// (values arrive space/CR-trimmed from nyxconf_get); on success store the rgb and return 1,
// else return 0 (the caller then tries the preset-name path). Pure.
static int parse_hex_color(const char* s, uint32_t* out) {
    if (!s) return 0;
    if (*s == '#') s++;
    int v[6];
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        if      (c >= '0' && c <= '9') v[i] = c - '0';
        else if (c >= 'a' && c <= 'f') v[i] = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v[i] = c - 'A' + 10;
        else return 0;                          // too short or a non-hex digit
    }
    if (s[6] != '\0') return 0;                 // trailing garbage / too long
    if (out) *out = fb_rgb((uint32_t)(v[0]*16 + v[1]),
                           (uint32_t)(v[2]*16 + v[3]),
                           (uint32_t)(v[4]*16 + v[5]));
    return 1;
}

// KAT: the #RRGGBB accent parser — accepts 6 hex digits with/without '#' (case-insensitive),
// rejects wrong length / non-hex / trailing garbage, and never touches *out on failure. 0 = pass.
int hex_color_selftest(void) {
    uint32_t c = 0;
    if (!parse_hex_color("#1E90FF", &c) || c != fb_rgb(0x1E, 0x90, 0xFF)) return 1;
    if (!parse_hex_color("1e90ff",  &c) || c != fb_rgb(0x1E, 0x90, 0xFF)) return 2;  // no '#', lowercase
    if (!parse_hex_color("#000000", &c) || c != fb_rgb(0, 0, 0))          return 3;
    if (!parse_hex_color("#FFFFFF", &c) || c != fb_rgb(255, 255, 255))    return 4;
    if (parse_hex_color("#12345",  &c))  return 5;   // too short
    if (parse_hex_color("#1234567", &c)) return 6;   // too long
    if (parse_hex_color("#12345g",  &c)) return 7;   // non-hex digit
    if (parse_hex_color("Morado",   &c)) return 8;   // a preset name is not hex
    if (parse_hex_color(0,          &c)) return 9;   // NULL
    c = 0xABCD;                                      // failure must leave *out untouched
    if (parse_hex_color("nope!!", &c) || c != 0xABCD) return 10;
    return 0;
}

// Format the `accent` value save_nyx_config writes back to /etc/nyx.conf: a hand-set (or
// override) color has no palette name, so persist it as a lowercase #RRGGBB that parse_hex_color
// reads back verbatim — otherwise saving from the GUI clobbered a #hex accent with the stale
// index's name ("Morado"). A named color is written by name. Pure. `rgb` is an fb_rgb() value.
static void accent_config_str(int is_override, uint32_t rgb, const char* name, char* out, int outsz) {
    if (is_override)
        snprintf(out, outsz, "#%02x%02x%02x", (unsigned)((rgb >> 16) & 0xFF),
                 (unsigned)((rgb >> 8) & 0xFF), (unsigned)(rgb & 0xFF));
    else
        snprintf(out, outsz, "%s", name ? name : "Morado");
}

// KAT: the accent config-string round-trips through parse_hex_color (a #hex written by save must
// re-parse to the SAME rgb), and a named color is written by name. 0 = pass.
int accent_config_selftest(void) {
    char b[16]; uint32_t rgb;
    accent_config_str(1, fb_rgb(0x1E, 0x90, 0xFF), "Morado", b, sizeof b);
    if (strcmp(b, "#1e90ff") != 0) return 1;
    if (!parse_hex_color(b, &rgb) || rgb != fb_rgb(0x1E, 0x90, 0xFF)) return 2;   // round-trips
    accent_config_str(0, 0, "Turquesa", b, sizeof b);
    if (strcmp(b, "Turquesa") != 0) return 3;                                     // named -> name
    accent_config_str(1, fb_rgb(0, 0, 0), "x", b, sizeof b);
    if (strcmp(b, "#000000") != 0) return 4;
    accent_config_str(1, fb_rgb(255, 255, 255), "x", b, sizeof b);
    if (strcmp(b, "#ffffff") != 0) return 5;
    return 0;
}

// nyx.conf `rounding` — the window/menu corner radius in px, clamped to [0, WIN_RADIUS_MAX].
// 0 = fully square windows (a classic rice choice); a leading non-digit or NULL -> 0. Pure
// (does not set g_corner_radius) so the KAT can check the parse in isolation.
static int rounding_resolve(const char* val) {
    int r = 0;
    if (val) for (const char* p = val; *p >= '0' && *p <= '9'; p++) r = r * 10 + (*p - '0');
    if (r > WIN_RADIUS_MAX) r = WIN_RADIUS_MAX;
    return r < 0 ? 0 : r;
}

// KAT: the `rounding` parser clamps to [0, WIN_RADIUS_MAX] and maps NULL / non-numeric to 0.
int rounding_selftest(void) {
    if (rounding_resolve("7")   != 7)              return 1;
    if (rounding_resolve("0")   != 0)              return 2;   // square windows
    if (rounding_resolve("999") != WIN_RADIUS_MAX) return 3;   // clamped
    if (rounding_resolve(0)     != 0)              return 4;   // NULL -> 0
    if (rounding_resolve("abc") != 0)              return 5;   // non-numeric -> 0
    if (rounding_resolve("12")  != 12)             return 6;
    return 0;
}

// nyx.conf `wallpaper_dim` — darken the wallpaper by this PERCENT (0..80). 0 = off (no dim);
// higher values mute the background so windows/icons/text stand out (an r/unixporn staple).
// Clamped to 80 so the desktop never goes fully black; NULL / non-numeric -> 0. Pure (does not
// set g_wallpaper_dim) so the KAT can check the parse in isolation.
#define WALLPAPER_DIM_MAX 80
static int wallpaper_dim_resolve(const char* val) {
    int d = 0;
    if (val) for (const char* p = val; *p >= '0' && *p <= '9'; p++) d = d * 10 + (*p - '0');
    if (d > WALLPAPER_DIM_MAX) d = WALLPAPER_DIM_MAX;
    return d < 0 ? 0 : d;
}

// KAT: the `wallpaper_dim` parser clamps to [0, WALLPAPER_DIM_MAX] and maps NULL / non-numeric to 0.
int wallpaper_dim_selftest(void) {
    if (wallpaper_dim_resolve("0")   != 0)                 return 1;   // off
    if (wallpaper_dim_resolve("40")  != 40)                return 2;
    if (wallpaper_dim_resolve("80")  != WALLPAPER_DIM_MAX) return 3;
    if (wallpaper_dim_resolve("100") != WALLPAPER_DIM_MAX) return 4;   // clamped
    if (wallpaper_dim_resolve(0)     != 0)                 return 5;   // NULL -> 0
    if (wallpaper_dim_resolve("abc") != 0)                 return 6;   // non-numeric -> 0
    return 0;
}

// nyx.conf boolean-OFF parse: any of off/0/false/no reads as "disabled"; anything else
// (including NULL) is "enabled". Shared by toggle keys like `shadow`.
static int conf_is_off(const char* val) {
    return val && (strcmp(val, "off")   == 0 || strcmp(val, "0")  == 0 ||
                   strcmp(val, "false") == 0 || strcmp(val, "no") == 0);
}

// x for the title text in the span [left, right): left-aligned, or centered when `center` and the
// (already-fit) text is narrower than the span. Never left of `left`, never past the buttons.
static int title_text_x(int left, int right, int textw, int center) {
    if (!center) return left;
    int span = right - left;
    if (textw >= span || span <= 0) return left;
    return left + (span - textw) / 2;
}

// KAT: the title-text x placement — left mode is width-independent; centered splits the slack,
// clamps to `left` when the text is as wide as / wider than the span, and on a degenerate span.
int titlebar_selftest(void) {
    if (title_text_x(10, 200, 40, 0)  != 10)  return 1;   // left mode -> always `left`
    if (title_text_x(10, 200, 40, 1)  != 85)  return 2;   // centered: 10 + (190-40)/2
    if (title_text_x(10, 200, 300, 1) != 10)  return 3;   // wider than span -> clamp left
    if (title_text_x(10, 200, 190, 1) != 10)  return 4;   // exact fit -> left
    if (title_text_x(100, 100, 0, 1)  != 100) return 5;   // degenerate span -> left
    return 0;
}

// KAT: conf_is_off recognises exactly off/0/false/no as disabled; everything else (incl. NULL,
// and case variants like "Off") is enabled.
int shadow_selftest(void) {
    if (!conf_is_off("off"))   return 1;
    if (!conf_is_off("0"))     return 2;
    if (!conf_is_off("false")) return 3;
    if (!conf_is_off("no"))    return 4;
    if (conf_is_off("on"))     return 5;
    if (conf_is_off("1"))      return 6;
    if (conf_is_off("yes"))    return 7;
    if (conf_is_off(0))        return 8;   // NULL -> enabled
    if (conf_is_off("Off"))    return 9;   // case-sensitive -> enabled
    return 0;
}

// nyx.conf `scheme` — one-word colorscheme presets. Each preset names a coordinated
// (wallpaper style, accent color) pair, so a single keyword themes the whole desktop —
// the "swappable colorschemes" the rice north star asks for. Returns 1 and sets *wp and
// *accent to the preset's wallpaper-style + accent-color NAMES (fed straight into the same
// validated wallpaper_*_from_name lookups the wallpaper/accent keys use), or 0 for an
// unknown name (the caller then keeps whatever the other keys set).
static int scheme_lookup(const char* name, const char** wp, const char** accent) {
    static const struct { const char* name; const char* wp; const char* accent; } S[] = {
        { "nightfall", "Nightfall",  "Morado"   },   // signature default: moon + brand purple
        { "aurora",    "Aurora",     "Turquesa" },
        { "ember",     "Meteoros",   "Naranja"  },
        { "nebula",    "Nebula",     "Rosa"     },
        { "abyss",     "Astral",     "Azul"     },
        { "forest",    "Cordillera", "Verde"    },
        { "mono",      "Plano",      "Pizarra"  },
        { "clean",     "Limpio",     "Morado"   },   // minimal: just the brand gradient
        { "starfield", "Estrellas",  "Azul"     },   // deep-blue twinkling night sky
        { "fireflies", "Luces",      "Lima"     },   // green-gold meadow with drifting orbs
        { "ripple",    "Ondas",      "Turquesa" },   // moonlit water ripples
        { "rain",      "Lluvia",     "Carbon"   },   // stormy dark falling rain
    };
    for (int i = 0; i < (int)(sizeof(S) / sizeof(S[0])); i++)
        if (strcmp(name, S[i].name) == 0) { *wp = S[i].wp; *accent = S[i].accent; return 1; }
    return 0;
}

// KAT: the colorscheme presets. Every preset must resolve to a REAL wallpaper style AND a
// REAL accent color (a typo'd preset would silently no-op), two specific mappings are pinned,
// and an unknown name must be rejected. 0 = pass.
int scheme_selftest(void) {
    const char *wp, *ac;
    if (!scheme_lookup("nightfall", &wp, &ac)) return 1;
    if (strcmp(wp, "Nightfall") || strcmp(ac, "Morado")) return 2;
    if (!scheme_lookup("aurora", &wp, &ac)) return 3;
    if (strcmp(wp, "Aurora") || strcmp(ac, "Turquesa")) return 4;
    static const char* names[] = { "nightfall", "aurora", "ember", "nebula", "abyss", "forest", "mono",
                                    "clean", "starfield", "fireflies", "ripple", "rain" };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (!scheme_lookup(names[i], &wp, &ac)) return 5;
        if (wallpaper_style_from_name(wp) < 0) return 6;   // a real wallpaper style
        if (wallpaper_color_from_name(ac) < 0) return 7;   // a real accent color
    }
    if (scheme_lookup("bogus", &wp, &ac)) return 8;        // unknown -> rejected
    return 0;
}

// Rice config foundation (v6.5.23): read /etc/nyx.conf at desktop start and apply the
// desktop settings it names. A missing file or unknown value just leaves the defaults, so
// the desktop always comes up. Currently drives the wallpaper style + accent color; future
// keys (font, taskbar…) hang off the same parser.
static void apply_nyx_config(void) {
    theme_set_accent(fb_rgb(130, 90, 210));     // Morado default UI accent (may be overridden below)
    int fd = vfs_open("/etc/nyx.conf", 0, 0);   // O_RDONLY; -1 if absent
    if (fd < 0) return;
    static char buf[2048];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char val[64];
    if (nyxconf_get(buf, "wallpaper", val, sizeof val)) {
        int s = wallpaper_style_from_name(val);
        if (s >= 0) wallpaper_set_style(s);
    }
    if (nyxconf_get(buf, "accent", val, sizeof val)) {
        uint32_t hex;
        if (parse_hex_color(val, &hex)) {               // accent = #RRGGBB -> ANY color (wallpaper + chrome)
            wallpaper_set_color_rgb(hex);
            theme_set_accent(hex);
        } else {
            int c = wallpaper_color_from_name(val);
            if (c >= 0) {
                wallpaper_set_color(c);                 // the wallpaper base color…
                theme_set_accent(wallpaper_base_color()); // …AND the whole UI chrome, cohesively
            }
        }
    }
    if (nyxconf_get(buf, "scheme", val, sizeof val)) {
        const char *swp, *sac;                      // a one-word preset overrides wallpaper+accent
        if (scheme_lookup(val, &swp, &sac)) {
            int s = wallpaper_style_from_name(swp);
            int c = wallpaper_color_from_name(sac);
            if (s >= 0) wallpaper_set_style(s);
            if (c >= 0) { wallpaper_set_color(c); theme_set_accent(wallpaper_base_color()); }
        }
    }
    if (nyxconf_get(buf, "widget", val, sizeof val)) {
        // live desktop CPU/RAM widget: any of off/0/false/no disables it (default on)
        g_widget_on = !(strcmp(val, "off") == 0 || strcmp(val, "0") == 0 ||
                        strcmp(val, "false") == 0 || strcmp(val, "no") == 0);
    }
    if (nyxconf_get(buf, "widget_pos", val, sizeof val)) {
        int p = widget_pos_from_name(val);          // which corner the monitor widget hugs
        if (p >= 0) g_widget_pos = p;               // unknown name -> keep the default corner
    }
    if (nyxconf_get(buf, "clock", val, sizeof val)) {
        g_clock_12h = (strcmp(val, "12h") == 0 || strcmp(val, "12") == 0);   // else 24-hour
    }
    if (nyxconf_get(buf, "icons", val, sizeof val)) {
        // desktop app icons (Files/Terminal/…/Selene): opt-IN (default off = clean desktop)
        g_desktop_icons_visible = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0 ||
                                   strcmp(val, "true") == 0 || strcmp(val, "yes") == 0);
    }
    if (nyxconf_get(buf, "gaps", val, sizeof val)) {
        int gp = 0;                                  // px between tiles + around the screen
        for (const char* p = val; *p >= '0' && *p <= '9'; p++) gp = gp * 10 + (*p - '0');
        if (gp > 64) gp = 64;                        // clamp to a sane rice range
        g_gaps = gp;
    }
    if (nyxconf_get(buf, "border", val, sizeof val)) {
        g_border_color = border_resolve(val);        // focused-window outline; 0 = follow accent
    }
    if (nyxconf_get(buf, "border_inactive", val, sizeof val)) {
        g_border_inactive = border_resolve(val);     // unfocused-window outline; 0 = neutral bevel
    }
    if (nyxconf_get(buf, "panel_tint", val, sizeof val)) {
        int t = 0;                                   // % the taskbar tints toward the wallpaper
        for (const char* p = val; *p >= '0' && *p <= '9'; p++) t = t * 10 + (*p - '0');
        if (t > 100) t = 100;
        g_panel_tint = t;
    }
    if (nyxconf_get(buf, "rounding", val, sizeof val)) {
        g_corner_radius = rounding_resolve(val);      // window/menu corner radius; 0 = square
    }
    if (nyxconf_get(buf, "shadow", val, sizeof val)) {
        g_shadows = !conf_is_off(val);                // window/menu drop shadows; off = flat
    }
    if (nyxconf_get(buf, "title_align", val, sizeof val)) {
        g_title_center = (strcmp(val, "center") == 0);  // title-bar text: center, else left (default)
    }
    if (nyxconf_get(buf, "wallpaper_dim", val, sizeof val)) {
        g_wallpaper_dim = wallpaper_dim_resolve(val);   // darken the wallpaper 0-80% (0 = off)
    }
}

// Write the current theme (wallpaper style + accent color + widget state) back to
// /etc/nyx.conf, so a change made in the GUI (the Wallpaper theme picker) persists across
// reboots — apply_nyx_config reads it at desktop start. The inverse of apply_nyx_config.
// Reverse of border_resolve for save_nyx_config: 0 -> "accent", else the palette name whose
// rgb matches (a border only ever holds 0 or one of the 11 palette colors, so this round-trips
// exactly). An unrecognised rgb falls back to "accent" so the write is always valid.
static void border_config_str(uint32_t rgb, char* out, int cap) {
    if (rgb != 0)
        for (int i = 0; i < WALLPAPER_COUNT; i++)
            if (wallpaper_color_rgb(i) == rgb) { snprintf(out, cap, "%s", wallpaper_color_name(i)); return; }
    snprintf(out, cap, "accent");
}

void save_nyx_config(void) {
    char buf[640];
    char acc[16];   // a #RRGGBB accent has no palette name — persist it so the GUI save round-trips
    accent_config_str(wallpaper_is_rgb_override(), wallpaper_override_rgb(),
                      wallpaper_color_name(wallpaper_color()), acc, sizeof acc);
    char bf[24], bi[24];                              // border / border_inactive as palette names
    border_config_str(g_border_color, bf, sizeof bf);
    border_config_str(g_border_inactive, bi, sizeof bi);
    // Persist EVERY knob apply_nyx_config reads (not just the GUI-picker ones), so a theme
    // change in the GUI no longer O_TRUNCs away a hand-set rice config (gaps/border/rounding/
    // shadow/title_align/panel_tint/wallpaper_dim). `scheme` is intentionally omitted — it is a
    // one-word alias that resolves into wallpaper+accent, which are written explicitly here.
    int n = snprintf(buf, sizeof buf,
        "# NyxOS desktop config -- rice it here (also editable from the Wallpaper picker).\n"
        "wallpaper = %s\n"
        "accent = %s\n"
        "widget = %s\n"
        "widget_pos = %s\n"
        "clock = %s\n"
        "gaps = %d\n"
        "icons = %s\n"
        "border = %s\n"
        "border_inactive = %s\n"
        "panel_tint = %d\n"
        "rounding = %d\n"
        "shadow = %s\n"
        "title_align = %s\n"
        "wallpaper_dim = %d\n",
        wallpaper_style_name(wallpaper_style()),
        acc,
        g_widget_on ? "on" : "off",
        widget_pos_name(g_widget_pos),
        g_clock_12h ? "12h" : "24h",
        g_gaps,
        g_desktop_icons_visible ? "on" : "off",
        bf, bi,
        g_panel_tint,
        g_corner_radius,
        g_shadows ? "on" : "off",
        g_title_center ? "center" : "left",
        g_wallpaper_dim);
    if (n <= 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;   // snprintf returns the intended length; clamp to what fit
    int fd = vfs_open("/etc/nyx.conf", O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { vfs_write(fd, buf, (size_t)n); vfs_close(fd); }
}

static void draw_welcome_windows(void) {
    // Minimal desktop (v6.4.48+): open to a clean wallpaper with ONE subtle,
    // centered greeting card instead of the old three stacked boot windows (a
    // keyboard-shortcut list, an About box, and a full Terminal). Every app —
    // including the Terminal — launches from the Start menu, so nothing is lost;
    // the user can close this card for a pristine desktop.
    const int ww = 380, wh = 210;
    int x = ((int)fb_get_width()  - ww) / 2;
    int y = ((int)fb_get_height() - wh) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    window_create(x, y, ww, wh, "Welcome to NyxOS", welcome_draw_fn);
}

// ---- Screensaver: an idle-triggered night scene (Nyx: a crescent moon over a drifting,
// twinkling starfield with the wall-clock time), dismissed by any key or mouse activity.
#define SS_IDLE_MS  60000u      // idle time (ms; tick is 1000 Hz) before it kicks in
static int      screensaver_active = 0;
static uint32_t ss_last_activity   = 0;

static void draw_screensaver(uint32_t t) {
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    fb_fill_vgrad(0, 0, fw, fh, fb_rgb(20, 14, 34), fb_rgb(4, 2, 8));   // deep night sky

    // A fixed LCG-seeded starfield (same positions each frame) that drifts slowly right
    // and twinkles via a per-star phase — pure function of t, so any repaint is coherent.
    uint32_t s = 0x1234567u;
    for (int i = 0; i < 150; i++) {
        s = s * 1103515245u + 12345u; int bx = (int)((s >> 9) % (uint32_t)fw);
        s = s * 1103515245u + 12345u; int by = (int)((s >> 9) % (uint32_t)fh);
        int phase = (int)((s >> 3) & 255);
        int saw   = (int)((t / 8 + (uint32_t)phase) & 255);
        int b     = 80 + (saw < 128 ? saw : 255 - saw) / 2;             // 80..143 triangle
        int px    = (bx + (int)(t / 60)) % fw;                          // gentle drift
        uint8_t c = (uint8_t)b, cb = (uint8_t)(b + 40 > 255 ? 255 : b + 40);
        int big   = (b > 130);
        fb_fill_rect(px, by, big ? 2 : 1, big ? 2 : 1, fb_rgb(c, c, cb));
    }

    // A crescent moon (a lilac disc with a sky-coloured disc offset to carve it), bobbing.
    int r = 46, mcx = fw * 3 / 4, mcy = fh / 4 + (int)((t / 24) % 44) - 22;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;                    // outside the disc
            int sdx = dx - 22, sdy = dy - 8;
            if (sdx * sdx + sdy * sdy <= r * r) continue;               // inside the shadow disc
            int px = mcx + dx, py = mcy + dy;
            if (px < 0 || px >= fw || py < 0 || py >= fh) continue;
            fb_fill_rect(px, py, 1, 1, fb_rgb(228, 224, 248));
        }

    // The wall-clock time, centred and dim (reuses date_format from the taskbar clock).
    rtc_time_t rt; rtc_read_time(&rt);
    char tb[16]; date_format(tb, sizeof(tb), "%H:%M", &rt);
    int tw = (int)strlen(tb) * FONT_WIDTH;
    font_draw_string_trans((fw - tw) / 2, fh * 5 / 8, tb, fb_rgb(165, 155, 200));
}

void compositor_run(void) {
    compositor_active = 1;
    // Double buffering: compose each frame off-screen and blit it in one shot
    // (fb_present) so the screen never shows the half-drawn background between
    // elements — this is what removes the flicker on click / drag / repaint.
    fb_enable_backbuffer();
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    quit = 0;
    mouse_x = fw / 2; mouse_y = fh / 2;
    mouse_set_pos(mouse_x, mouse_y);
    uint8_t prev_btns = 0;
    ss_last_activity = get_ticks(); screensaver_active = 0;   // start the idle timer fresh

    apply_nyx_config();          // rice: /etc/nyx.conf picks the wallpaper/accent before first paint
    draw_welcome_windows();
    redraw_all();
    save_cursor_bg(mouse_x, mouse_y);
    draw_cursor(mouse_x, mouse_y);
    fb_present();
    frame_dirty = 0;
    int redraw = 0;
    uint32_t clock_tick = 0;
    // Window-drag partial present: the drag handler records the damaged region (old ∪ new
    // window footprint, incl. drop shadow) each move so the present publishes just that rect
    // instead of the whole 3 MB screen. Reset every iteration.
    int drag_dmg_valid = 0, ddx = 0, ddy = 0, ddw = 0, ddh = 0;
    extern volatile int kbd_head, kbd_tail;
    while (!quit) {
        drag_dmg_valid = 0;   // recomputed by the drag handler each iteration
        // Idle-yield: when nothing is happening — no key queued, no mouse button
        // held, the pointer hasn't moved, and no drag/resize/menu is active — sleep
        // briefly instead of busy-polling, so background jobs (or the CPU itself)
        // get the time. Any activity skips the sleep, so interactivity is unchanged;
        // the wakeup is within ~5ms (imperceptible). sleep() blocks via the scheduler
        // when it's running, or idles (hlt) to the tick otherwise.
        if (kbd_head == kbd_tail && mouse_get_buttons() == 0 &&
            mouse_get_x() == mouse_x && mouse_get_y() == mouse_y &&
            mouse_get_z() == mouse_z &&
            !drag_id && !resize_id && drag_icon_idx < 0 &&
            !ctx_menu_open && !start_menu_open) {
            sleep(5);
        }

        kernel_poll_net();
        run_background_tasks();

        int k = getkey_poll();

        // MouseKeys — drive the pointer from the keyboard, essential on hardware with no
        // working mouse/touchpad (e.g. an 8" UMPC with one USB port). Hold Alt: W/A/S/D move
        // the cursor, Space left-clicks, C right-clicks. The key is consumed so it never leaks
        // to a focused app; movement clamps to the (possibly rotated) logical screen.
        if (k && is_alt_pressed()) {
            int nx = mouse_get_x(), ny = mouse_get_y(), hit = 1;
            const int STEP = 16;
            if      (k == 'w' || k == 'W') ny -= STEP;
            else if (k == 's' || k == 'S') ny += STEP;
            else if (k == 'a' || k == 'A') nx -= STEP;
            else if (k == 'd' || k == 'D') nx += STEP;
            else if (k == ' ' || k == 'q' || k == 'Q') mouse_kbd_click(0);
            else if (k == 'e' || k == 'E' || k == 'c' || k == 'C') mouse_kbd_click(1);
            else hit = 0;
            if (hit) {
                if (nx < 0) nx = 0; else if (nx >= (int)fw) nx = (int)fw - 1;
                if (ny < 0) ny = 0; else if (ny >= (int)fh) ny = (int)fh - 1;
                mouse_set_pos(nx, ny);
                k = 0;
            }
        }

        int mx = mouse_get_x();
        int my = mouse_get_y();
        uint8_t btns = mouse_get_buttons();

        // Screensaver: after SS_IDLE_MS of no input it takes over the screen with the night
        // scene; any key or mouse activity wakes it + restores the desktop (that input is
        // swallowed — it only dismisses). It owns the frame either way, so it `continue`s.
        {
            uint32_t ss_now = get_ticks();
            int ss_input = (k > 0) || (mx != mouse_x) || (my != mouse_y) ||
                           (btns != prev_btns) || (mouse_get_z() != mouse_z);
            if (ss_input) ss_last_activity = ss_now;
            if (screensaver_active) {
                if (ss_input) {                        // wake: repaint the desktop, swallow input
                    screensaver_active = 0;
                    mouse_x = mx; mouse_y = my; mouse_z = mouse_get_z(); prev_btns = btns;
                    redraw_all();
                    save_cursor_bg(mouse_x, mouse_y);
                    draw_cursor(mouse_x, mouse_y);
                    fb_present();
                } else {                               // still idle: animate one frame
                    draw_screensaver(ss_now);
                    fb_present();
                }
                continue;
            }
            if ((ss_now - ss_last_activity) > SS_IDLE_MS) { screensaver_active = 1; continue; }
        }

        restore_cursor_bg(mouse_x, mouse_y);

        if (mx < 0) mx = 0;
        if (mx >= (int)fw) mx = fw - 1;
        if (my < 0) my = 0;
        if (my >= (int)fh) my = fh - 1;

        // Dispatch mouse-move to focused window
        // Did a hover handler run this frame? If so, the focused window may have painted
        // into the back buffer without setting `redraw`, so the finished frame must be
        // published in full — the cursor-only fast present below would miss it.
        int mm_dispatched = 0;
        if (mx != mouse_x || my != mouse_y) {
            window_t* fwin = NULL;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (windows[i] && windows[i]->id == focused_id) {
                    fwin = windows[i];
                    break;
                }
            }
            if (fwin && fwin->on_mousemove) {
                fwin->on_mousemove(fwin, mx, my, btns);
                mm_dispatched = 1;
            }
        }

        // Dispatch mouse-wheel notches to the focused window as synthetic scroll keys
        // (the terminal turns them into scrollback movement). One key per notch.
        int wz = mouse_get_z();
        if (wz != mouse_z) {
            int notches = wz - mouse_z;
            int steps = notches < 0 ? -notches : notches;
            if (steps > 8) steps = 8;                    // clamp a fast flick
            window_t* fwin = find_window(focused_id);
            if (fwin && fwin->on_key) {
                int keycode = (notches > 0) ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
                for (int s = 0; s < steps; s++) fwin->on_key(fwin, keycode);
                redraw = 1;
            }
            mouse_z = wz;
        }

        if (drag_id && !(btns & 1)) {                    // dropped a title-bar drag
            int zone = snap_zone_for_cursor(mx, my, (int)fw, (int)fh);
            if (zone != WSTATE_NORMAL) window_snap_to(drag_id, zone);   // Aero-snap on drop
            drag_id = 0; redraw = 1;
        }

        if (resize_id && !(btns & 1)) {
            // Drag-resize released: notify the window of its FINAL client size (once, not per
            // pixel) so a ring-3 client can realloc + re-present. win->w/win->h are the client
            // dims (title bar is separate — see the .67 TITLE_H fix).
            window_t* rw = find_window(resize_id);
            if (rw && rw->on_resize) rw->on_resize(rw, (int)rw->w, (int)rw->h);
            resize_id = 0; redraw = 1;
        }

        if (drag_icon_idx >= 0 && !(btns & 1)) {
            int cx = desktop_icon_x[drag_icon_idx] + ICON_SIZE / 2;
            int cy = desktop_icon_y[drag_icon_idx] + ICON_SIZE / 2;
            int nearest = 0, near_dist = 999999;
            for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
                int sx = 20 + i * (ICON_SIZE + ICON_PAD) + ICON_SIZE / 2;
                int dx = cx - sx, dy = cy - (20 + ICON_SIZE / 2);
                int d = dx * dx + dy * dy;
                if (d < near_dist) { near_dist = d; nearest = i; }
            }
            int old_idx = drag_icon_idx;
            int moved = (nearest != old_idx);
            if (moved) {
                const char* name = desktop_icon_names[old_idx];
                int action = desktop_icon_actions[old_idx];
                if (nearest < old_idx) {
                    for (int i = old_idx; i > nearest; i--) {
                        desktop_icon_names[i] = desktop_icon_names[i-1];
                        desktop_icon_actions[i] = desktop_icon_actions[i-1];
                    }
                } else {
                    for (int i = old_idx; i < nearest; i++) {
                        desktop_icon_names[i] = desktop_icon_names[i+1];
                        desktop_icon_actions[i] = desktop_icon_actions[i+1];
                    }
                }
                desktop_icon_names[nearest] = name;
                desktop_icon_actions[nearest] = action;
            }
            for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
                desktop_icon_x[i] = 20 + i * (ICON_SIZE + ICON_PAD);
                desktop_icon_y[i] = 20;
            }
            if (!moved) do_start_menu_action(desktop_icon_actions[old_idx]);
            drag_icon_idx = -1;
            redraw = 1;
            goto done_click;
        }

        // Right-click: desktop context menu
        if ((btns & 2) && !(btns & 1)) {
            // Check if we hit desktop (not on a window, not on taskbar)
            int on_desktop = 1;
            if (my >= (int)(fh - TASKBAR_H)) on_desktop = 0; // taskbar (fh: the function-level framebuffer height)
            window_t* sorted_win[MAX_WINDOWS];
            int nw = 0;
            for (int i = 0; i < MAX_WINDOWS; i++)
                if (windows[i] && windows[i]->visible && windows[i]->state != WSTATE_MINIMIZED
                    && windows[i]->workspace == current_workspace)
                    sorted_win[nw++] = windows[i];
            // Order topmost-first (descending z-order), like the left-click path, so
            // an overlapped right-click is routed to the window on top instead of
            // whichever one happens to sit earliest in the array. Without this a
            // right-click (e.g. flagging in Minesweeper, or the File Manager context
            // menu) leaks to a lower window when the two overlap.
            for (int i = 0; i < nw; i++)
                for (int j = i + 1; j < nw; j++)
                    if (sorted_win[i]->z_order < sorted_win[j]->z_order) {
                        window_t* t = sorted_win[i]; sorted_win[i] = sorted_win[j]; sorted_win[j] = t;
                    }
            for (int i = 0; i < nw; i++)
                if (window_hit(sorted_win[i], mx, my)) { on_desktop = 0; break; }
            if (on_desktop) {
                if (ctx_menu_hit(mx, my)) {
                    int idx;
                    if (ctx_menu_item_hit(mx, my, &idx)) {
                        do_ctx_menu_action(idx);
                        redraw = 1;
                    }
                } else {
                    ctx_menu_open = 1;
                    ctx_menu_x = mx; ctx_menu_y = my;
                    redraw = 1;
                }
            } else {
                // Right-click on a window → route to window's on_click
                ctx_menu_open = 0;
                for (int i = 0; i < nw; i++)
                    if (window_hit(sorted_win[i], mx, my)) {
                        if (sorted_win[i]->on_click)
                            sorted_win[i]->on_click(sorted_win[i], mx, my, 2);
                        break;
                    }
            }
            goto done_click;
        }

        if (btns & 1) {
            if (drag_icon_idx >= 0) {
                int nx = mx - drag_icon_ofs_x;
                int ny = my - drag_icon_ofs_y;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                desktop_icon_x[drag_icon_idx] = nx;
                desktop_icon_y[drag_icon_idx] = ny;
                redraw = 1;
                goto done_click;
            } else if (drag_id) {
                window_t* win = find_window(drag_id);
                if (win) {
                    // Record the old footprint, move, record the new one → the damaged region
                    // is their union (+ a margin covering the frame + drop shadow). The present
                    // section blits just this rect instead of the whole screen.
                    int oax = win->x, oay = win->y, oaw = (int)win->w, oah = (int)win_total_h(win);
                    window_move(drag_id, mx - win->drag_off_x, my - win->drag_off_y);
                    int nbx = win->x, nby = win->y, nbw = (int)win->w, nbh = (int)win_total_h(win);
                    const int M = SHADOW_OFFSET + SHADOW_RADIUS + 2;
                    int x0 = (oax < nbx ? oax : nbx) - M;
                    int y0 = (oay < nby ? oay : nby) - M;
                    int x1 = ((oax + oaw) > (nbx + nbw) ? (oax + oaw) : (nbx + nbw)) + M;
                    int y1 = ((oay + oah) > (nby + nbh) ? (oay + oah) : (nby + nbh)) + M;
                    ddx = x0; ddy = y0; ddw = x1 - x0; ddh = y1 - y0; drag_dmg_valid = 1;
                    redraw = 1;
                }
            } else if (resize_id) {
                window_t* win = find_window(resize_id);
                if (win) {
                    // Footprint BEFORE this step (for the partial-present damage rect below).
                    int oax = win->x, oay = win->y, oaw = (int)win->w, oah = (int)win_total_h(win);
                    int dx = mx - win->resize_start_x;
                    int dy = my - win->resize_start_y;
                    int nx = win->x, ny = win->y;
                    uint32_t nw = win->resize_start_w, nh = win->resize_start_h;
                    switch (win->resize_dir) {
                        case RESIZE_RIGHT:
                        case RESIZE_RIGHT_TOP:
                        case RESIZE_CORNER:
                            nw = win->resize_start_w + dx; break;
                        case RESIZE_LEFT:
                        case RESIZE_LEFT_TOP:
                        case RESIZE_LEFT_BOTTOM:
                            nw = (int)win->resize_start_w - dx;
                            nx = win->resize_start_x + dx;
                            break;
                    }
                    switch (win->resize_dir) {
                        case RESIZE_BOTTOM:
                        case RESIZE_CORNER:
                        case RESIZE_LEFT_BOTTOM:
                            nh = win->resize_start_h + dy; break;
                        case RESIZE_TOP:
                        case RESIZE_LEFT_TOP:
                        case RESIZE_RIGHT_TOP:
                            nh = (int)win->resize_start_h - dy;
                            ny = win->resize_start_y + dy;
                            break;
                    }
                    if ((int)nw < MIN_WIN_W) { nw = MIN_WIN_W; nx = win->x; }
                    if ((int)nh < MIN_WIN_H) { nh = MIN_WIN_H; ny = win->y; }
                    win->x = nx; win->y = ny;
                    window_resize(resize_id, nw, nh);
                    // Damage = old ∪ new footprint (+ shadow margin). The union covers both a
                    // grow (new bigger) and a shrink (old bigger → the vacated area repaints),
                    // so the present publishes just this rect instead of the whole screen.
                    int nbx = win->x, nby = win->y, nbw = (int)win->w, nbh = (int)win_total_h(win);
                    const int M = SHADOW_OFFSET + SHADOW_RADIUS + 2;
                    int x0 = (oax < nbx ? oax : nbx) - M;
                    int y0 = (oay < nby ? oay : nby) - M;
                    int x1 = ((oax + oaw) > (nbx + nbw) ? (oax + oaw) : (nbx + nbw)) + M;
                    int y1 = ((oay + oah) > (nby + nbh) ? (oay + oah) : (nby + nbh)) + M;
                    ddx = x0; ddy = y0; ddw = x1 - x0; ddh = y1 - y0; drag_dmg_valid = 1;
                    redraw = 1;
                }
            } else {
                window_t* hit = NULL;
                window_t* sorted[MAX_WINDOWS];
                int n = 0;
                for (int i = 0; i < MAX_WINDOWS; i++)
                    if (windows[i] && windows[i]->visible && windows[i]->state != WSTATE_MINIMIZED
                        && windows[i]->workspace == current_workspace)
                        sorted[n++] = windows[i];
                for (int i = 0; i < n; i++)
                    for (int j = i + 1; j < n; j++)
                        if (sorted[i]->z_order < sorted[j]->z_order) {
                            window_t* t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
                        }

                // Close context menu on any left-click
                if (ctx_menu_open) {
                    int idx;
                    if (ctx_menu_item_hit(mx, my, &idx)) {
                        do_ctx_menu_action(idx);
                        ctx_menu_open = 0;
                        redraw = 1;
                    } else {
                        ctx_menu_open = 0;
                    }
                }

                /* This whole left-button block runs under `if (btns & 1)`, which
                 * is LEVEL-triggered: it re-runs on every compositor iteration
                 * for as long as the button is down. That is right for dragging
                 * and resizing, which must keep acting while held. It is wrong
                 * for every menu transition below, and the two combined made the
                 * Start menu impossible to open with a normal click:
                 *
                 *   iteration 1  press on "Menu"  -> start_hit toggles it OPEN
                 *   iteration 2  button still down, pointer still on the taskbar
                 *                at y=750, which is OUTSIDE the menu rect
                 *                (y 332..732) -> the click-outside branch runs
                 *                and closes it again.
                 *
                 * So the menu opened and shut inside a single press, and whether
                 * anything survived came down to how long the button was held.
                 * Opening, closing and selecting are all EDGE events; only the
                 * drag paths are level events. Found by driving the real mouse —
                 * the headless keyboard-only suite could never have seen it. */
                int pressed = !(prev_btns & 1);

                if (user_menu_open) {
                    int idx;
                    if (user_menu_item_hit(mx, my, &idx)) {
                        if (pressed) {
                            do_user_menu_action(idx);
                            user_menu_open = 0;
                            redraw = 1;
                        }
                    } else if (pressed && !user_menu_area_hit(mx, my)) {
                        user_menu_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (cal_popup_open) {
                    if (pressed && !cal_popup_hit(mx, my)) {   // click away closes the calendar
                        cal_popup_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (net_popup_open) {
                    if (pressed && !net_popup_hit(mx, my)) {   // click away closes the network flyout
                        net_popup_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (spk_popup_open) {
                    if (pressed) {
                        int tx = spk_track_x(), ty = spk_track_y(), tw = spk_track_w();
                        int bx = spk_mute_x(), by = spk_mute_y();
                        if (mx >= tx - 4 && mx <= tx + tw + 4 && my >= ty - 5 && my <= ty + SPK_TRACK_H + 5) {
                            int v = (mx - tx) * 100 / (tw > 0 ? tw : 1);   // click the track to set volume
                            if (v < 0) v = 0;
                            if (v > 100) v = 100;
                            g_volume = v; g_muted = 0; apply_volume(); redraw = 1;
                        } else if (mx >= bx && mx < bx + SPK_MUTE_W && my >= by && my < by + SPK_MUTE_H) {
                            g_muted = !g_muted; apply_volume(); redraw = 1;    // toggle mute
                        } else if (!spk_popup_hit(mx, my)) {
                            spk_popup_open = 0; redraw = 1;                    // click away closes
                        }
                    }
                    goto done_click;
                }

                if (systray_net_hit(mx, my)) {
                    if (pressed) {
                        net_popup_open = !net_popup_open;       // click the tray network icon
                        start_menu_open = 0; user_menu_open = 0; cal_popup_open = 0; spk_popup_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (systray_spk_hit(mx, my)) {
                    if (pressed) {
                        spk_popup_open = !spk_popup_open;       // click the tray speaker icon
                        start_menu_open = 0; user_menu_open = 0; cal_popup_open = 0; net_popup_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (badge_hit(mx, my)) {
                    if (pressed) {
                        user_menu_open = !user_menu_open;
                        start_menu_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (start_menu_open) {
                    int idx;
                    if (start_menu_item_hit(mx, my, &idx)) {
                        if (pressed) {
                            do_start_menu_action(idx);
                            redraw = 1;
                        }
                    } else if (pressed && !start_menu_hit(mx, my)) {
                        start_menu_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (start_hit(mx, my)) {
                    if (pressed) {
                        start_menu_open = !start_menu_open;
                        start_filter_len = 0; start_filter[0] = 0;   // fresh query each open
                        start_sel = 0; start_nav = 0;                // no keyboard selection yet
                        redraw = 1;
                    }
                    goto done_click;
                }

                if (clock_hit(mx, my)) {
                    if (pressed) {
                        cal_popup_open = !cal_popup_open;      // click the clock to open the calendar
                        start_menu_open = 0;
                        user_menu_open = 0;
                        net_popup_open = 0;
                        spk_popup_open = 0;
                        redraw = 1;
                    }
                    goto done_click;
                }

                int tb_win_id;
                if (taskbar_win_hit(mx, my, &tb_win_id)) {
                    window_t* w = find_window(tb_win_id);
                    if (w) {
                        if (w->state == WSTATE_MINIMIZED) {
                            w->state = WSTATE_NORMAL;
                            w->workspace = current_workspace;
                            window_focus(tb_win_id);
                        } else if (w->focused) {
                            window_minimize(tb_win_id);
                        } else {
                            window_focus(tb_win_id);
                        }
                        redraw = 1;
                    }
                    goto done_click;
                }

                {
                    int icon_idx = desktop_icon_hit(mx, my);
                    if (icon_idx >= 0) {
                        drag_icon_idx = icon_idx;
                        drag_icon_ofs_x = mx - desktop_icon_x[icon_idx];
                        drag_icon_ofs_y = my - desktop_icon_y[icon_idx];
                        redraw = 1;
                        goto done_click;
                    }
                }

                for (int i = 0; i < n; i++) {
                    if (window_hit(sorted[i], mx, my)) {
                        hit = sorted[i]; break;
                    }
                }

                /* Window actions are EDGE-triggered: focus, close, min/max and
                 * starting a drag/resize fire only on the PRESS, never on the held
                 * frames that follow. Before this gate the block re-ran every
                 * compositor frame while the button was down, so the instant a top
                 * window was closed (or slid out from under the pointer mid-drag)
                 * the next frame re-hit-tested, focused the window now underneath
                 * and delivered the click to IT — the "close/move goes to the app
                 * below" bug (issue #44). Drag/resize CONTINUATION is handled above
                 * via the persistent drag_id/resize_id, so nothing here has to run
                 * while held except feeding a held press to the window that already
                 * OWNS it (paint-style drag) — and that goes to the focused window
                 * under the pointer only, never a freshly re-picked one. */
                if (hit && pressed) {
                    window_focus(hit->id);
                    redraw = 1;

                    if (close_hit(hit, mx, my)) {
                        window_destroy(hit->id);
                    } else if (max_hit(hit, mx, my)) {
                        if (hit->state == WSTATE_MAXIMIZED) window_restore(hit->id);
                        else window_maximize(hit->id);
                    } else if (min_hit(hit, mx, my)) {
                        window_minimize(hit->id);
                    } else if (titlebar_hit(hit, mx, my)) {
                        uint32_t now = get_ticks();
                        if (is_dblclick(last_tb_click_ms, now, hit->id == last_tb_click_id)) {
                            window_maximize(hit->id);   // double-click toggles maximize/restore
                            last_tb_click_id = -1;      // consume, so a 3rd click starts fresh
                        } else {
                            hit->dragging = 1;
                            hit->drag_off_x = mx - hit->x;
                            hit->drag_off_y = my - hit->y;
                            drag_id = hit->id;
                            last_tb_click_ms = now;
                            last_tb_click_id = hit->id;
                        }
                    } else {
                        int rdir;
                        if (resize_hit(hit, mx, my, &rdir)) {
                            hit->resizing = 1;
                            hit->resize_dir = rdir;
                            hit->resize_start_x = mx;
                            hit->resize_start_y = my;
                            hit->resize_start_w = hit->w;
                            hit->resize_start_h = hit->h;
                            resize_id = hit->id;
                        } else {
                            if (hit->on_click) {
                                hit->on_click(hit, mx, my, 1);
                                redraw = 1;
                            }
                            if (hit->on_pressed) {
                                hit->on_pressed(hit, mx, my, 1);
                                redraw = 1;
                            }
                        }
                    }
                } else if (!pressed && !drag_id && !resize_id) {
                    /* Button HELD (not a fresh press) and no drag/resize active:
                     * deliver the continuous press only to the focused window that
                     * still contains the pointer, in its CONTENT area (below the
                     * title bar). Never re-pick — re-picking is what leaked the
                     * click to lower windows once the top one closed/moved. */
                    window_t* fwin = find_window(focused_id);
                    if (fwin && fwin->on_pressed && window_hit(fwin, mx, my)
                        && my >= fwin->y + TITLE_H) {
                        fwin->on_pressed(fwin, mx, my, 1);
                        redraw = 1;
                    }
                }
            }
        } else {
            // Type-to-search in an open Start menu: keys narrow (or launch) the list
            // instead of going to a window. Only active while the menu is open, so
            // ordinary typing in apps is untouched.
            if (start_menu_open && k > 0) {
                int fi[START_ITEM_N]; int fc = start_menu_filtered(fi);
                if (k == '\n' || k == '\r') {                 // Enter: launch the SELECTED match
                    int idx = (start_nav && start_sel < fc) ? fi[start_sel] : (fc > 0 ? fi[0] : -1);
                    start_filter_len = 0; start_filter[0] = 0; start_sel = 0; start_nav = 0;
                    if (idx >= 0) do_start_menu_action(idx);    // (also closes the menu)
                    else          start_menu_open = 0;
                    redraw = 1; goto done_click;
                }
                if (k == 27) {                                // Escape: close
                    start_menu_open = 0; start_filter_len = 0; start_filter[0] = 0;
                    start_sel = 0; start_nav = 0;
                    redraw = 1; goto done_click;
                }
                if (k == KEY_UP || k == KEY_DOWN) {           // arrow keys: move the selection (wraps)
                    start_nav = 1;
                    if (fc > 0) {
                        if (k == KEY_UP) start_sel = (start_sel <= 0) ? fc - 1 : start_sel - 1;
                        else             start_sel = (start_sel >= fc - 1) ? 0 : start_sel + 1;
                    }
                    redraw = 1; goto done_click;
                }
                if (k == '\b' || k == 127) {                  // Backspace: edit the query
                    if (start_filter_len > 0) start_filter[--start_filter_len] = 0;
                    start_sel = 0; start_nav = 1;             // reset to the new top match
                    redraw = 1; goto done_click;
                }
                if (k >= 32 && k < 127 && start_filter_len < (int)sizeof(start_filter) - 1) {
                    start_filter[start_filter_len++] = (char)start_lc(k);
                    start_filter[start_filter_len] = 0;
                    start_sel = 0; start_nav = 1;             // highlight the new top match
                    redraw = 1; goto done_click;
                }
            }
            // Ctrl+1..4: workspace switching
            if (is_ctrl_pressed() && k >= '1' && k <= '4') {
                current_workspace = k - '1';
                redraw = 1;
                goto done_click;
            }

            // Window-management chords. Checked BEFORE the key is routed to the
            // focused window, and each ends in `goto done_click`, so a maximised
            // Terminal never also receives the Tab/arrow as input. Alt+Tab works
            // even with nothing focused (it picks the first window); the rest
            // need a target, so they no-op when focused_id is 0.
            if (is_alt_pressed()) {
                if (k == '\t') {                       // Alt+Tab: cycle focus
                    window_cycle_focus();
                    redraw = 1;
                    goto done_click;
                }
                window_t* awin = find_window(focused_id);
                if (awin) {
                    if (k == KEY_F4) {                 // Alt+F4: close
                        window_destroy(awin->id);
                        redraw = 1;
                        goto done_click;
                    }
                    if (k == KEY_UP) {                 // Alt+Up: climb the ladder
                        // While snapped, Up refines the vertical zone (half -> top
                        // quarter -> maximise). Otherwise it just maximises.
                        if (win_is_snapped(awin->state))
                            window_snap_vslide(awin->id, 1);
                        else if (awin->state != WSTATE_MAXIMIZED)
                            window_maximize(awin->id);
                        redraw = 1;
                        goto done_click;
                    }
                    if (k == KEY_DOWN) {               // Alt+Down: descend the ladder
                        // Snapped: Down refines the vertical zone (top -> half ->
                        // bottom -> un-snap). Maximised: restore. Plain: minimise.
                        // (A focused window is never minimised, so the else really
                        // is a plain normal window.)
                        if (win_is_snapped(awin->state))
                            window_snap_vslide(awin->id, 0);
                        else if (awin->state == WSTATE_MAXIMIZED)
                            window_restore(awin->id);
                        else
                            window_minimize(awin->id);
                        redraw = 1;
                        goto done_click;
                    }
                    if (k == KEY_LEFT) {               // Alt+Left: snap/flip left (toggle off if already left)
                        window_snap(awin->id, 1);
                        redraw = 1;
                        goto done_click;
                    }
                    if (k == KEY_RIGHT) {              // Alt+Right: snap/flip right (toggle off if already right)
                        window_snap(awin->id, 0);
                        redraw = 1;
                        goto done_click;
                    }
                }
            }

            // Route all keys to focused window's key handler
            window_t* fwin = find_window(focused_id);
            if (fwin && fwin->on_key && k > 0) {
                fwin->on_key(fwin, k);
                redraw = 1;
            }
        }
        prev_btns = btns;

done_click:

        int old_cx = mouse_x, old_cy = mouse_y;   // where the cursor started this frame
        int moved = (mx != mouse_x || my != mouse_y);
        mouse_x = mx; mouse_y = my;
        mouse_btns = btns;

        // While any menu is open, a bare pointer move must recomposite so the
        // hovered-entry highlight tracks the cursor (a move alone only re-blits the
        // existing frame + cursor, which would leave the highlight frozen).
        if (moved && (start_menu_open || ctx_menu_open || user_menu_open)) redraw = 1;

        uint32_t now = get_ticks();
        int taskbar_only = 0;
        int widget_only = 0;
        if (now - clock_tick > 1000) {
            clock_tick = now;
            // The clock + CPU/RAM module tick every second. Don't recomposite the WHOLE
            // screen for that — flag a taskbar-only refresh (repaint just the strip +
            // fb_present_rect it below). If a full frame is already happening this
            // iteration, frame_dirty wins and the taskbar refreshes with it.
            taskbar_only = 1;
        }

        // Periodic tick (~30 fps): drive any window that registered an on_tick (the games'
        // animation, Selene's cooperative image loader). Each handler returns 1 only when it
        // changed something, so an idle Selene window doesn't force a 30fps recomposite.
        static uint32_t game_tick_ms = 0;
        // Snapshot whether anything EARLIER this frame (a click, key, drag, resize) already asked
        // for a full redraw, and count how many windows the tick actually changes. When the SOLE
        // dirty source turns out to be exactly one animating window, the present below publishes
        // just that window's rect (see tick_partial) instead of the whole ~3 MB screen.
        int redraw_pre_tick = redraw;
        int tick_changed = 0, tick_idx[TICK_MAX_RECTS];
        if (now - game_tick_ms >= 33) {
            game_tick_ms = now;
            for (int gi = 0; gi < MAX_WINDOWS; gi++)
                if (windows[gi] && windows[gi]->on_tick && windows[gi]->visible) {
                    if (windows[gi]->on_tick(windows[gi])) {
                        redraw = 1;
                        if (tick_changed < TICK_MAX_RECTS) tick_idx[tick_changed] = gi;
                        tick_changed++;   // total; only the first TICK_MAX_RECTS are recorded
                    }
                }
        }

        // Animated wallpaper: Starfield twinkles (~8 fps is plenty) and Meteoros
        // streaks a shooting star (needs a faster ~14 fps for smooth motion). Force a
        // desktop repaint while one of them is selected; gated on the style, so every
        // other wallpaper still lets the desktop idle at rest (no needless recomposite).
        static uint32_t wp_anim_ms = 0;
        int wp_st = wallpaper_style();
        uint32_t wp_iv = (wp_st == WP_STYLE_SHOOTINGSTAR || wp_st == WP_STYLE_LLUVIA) ? 70u : 120u;
        if (wallpaper_animated() && now - wp_anim_ms >= wp_iv) {   // one animated-style list (drag partial uses it too)
            wp_anim_ms = now;
            redraw = 1;
        }

        // Keep repainting (~10 fps) while a notification toast is on screen so it
        // self-dismisses even on a still wallpaper; gated on a live toast, so an
        // idle desktop with none still rests. When the last toast expires, force
        // one final repaint so the desktop erases it instead of leaving it painted.
        static uint32_t notify_anim_ms = 0;
        static int notify_was_active = 0;
        int notify_now_active = notify_active_count(now) > 0;
        int toast_just_expired = (!notify_now_active && notify_was_active);  // this frame erases a toast → area outside a window changes → force a full present
        if ((notify_now_active && now - notify_anim_ms >= 100u) ||
            (!notify_now_active && notify_was_active)) {
            notify_anim_ms = now;
            redraw = 1;
        }
        notify_was_active = notify_now_active;

        // Live desktop widget: sample CPU% and scroll the graph every WGT_SAMPLE_MS. If a
        // full recomposite is already happening (redraw) it repaints with the frame; else,
        // when the widget isn't covered by a window, flag a widget-only partial repaint (its
        // ~180x76 rect, not the whole screen) so the graph animates without a full present.
        static uint32_t wgt_ms = 0;
        if (g_widget_on && now - wgt_ms >= WGT_SAMPLE_MS) {
            wgt_ms = now;
            wgt_push_sample();
            if (!redraw && !widget_covered()) widget_only = 1;
        }

        if (redraw) {
            // Dirty-rect clip-redraw (FLUIDEZ): when the SOLE dirty source this frame is the on_tick
            // of 1..TICK_MAX_RECTS animating windows (games, video, live graphs) and nothing outside
            // them could have changed, repaint ONLY those windows instead of the whole scene — the
            // back buffer is persistent, so the wallpaper (~8.8M cyc), their unmoved shadows, every
            // other window and the taskbar stay valid from the last full frame. .69 did the single
            // window; .71 generalises to the multi-window case (a game + the Nyx Monitor, Nyx Flex
            // tiles). Each ticked window must be un-occluded (window_occluded_above) — which also
            // means the ticked windows can't overlap EACH OTHER (an overlap puts one above the other
            // → that one occludes it → fall back), so paint order doesn't matter. The guard is a
            // strict SUBSET of the tick_partial PRESENT conditions below, so the publish shrinks to
            // match; if ANY ticked window fails the guard, one full redraw_all covers everything.
            window_t* clipwins[TICK_MAX_RECTS];
            int nclip = 0, clip_ok = 0;
            if (redraw_pre_tick == 0 && tick_changed >= 1 && tick_changed <= TICK_MAX_RECTS
                && !taskbar_only && !widget_only && !drag_id && !resize_id
                && !fb_fullscreen_active() && !wallpaper_animated()
                && notify_active_count(now) == 0 && !toast_just_expired
                && !start_menu_open && !ctx_menu_open && !user_menu_open) {
                clip_ok = 1;
                for (int t = 0; t < tick_changed; t++) {
                    window_t* tw = windows[tick_idx[t]];
                    if (!tw || !tw->visible || tw->state == WSTATE_MINIMIZED
                        || tw->workspace != current_workspace || window_occluded_above(tw)) {
                        clip_ok = 0; break;
                    }
                    clipwins[nclip++] = tw;
                }
            }
            if (clip_ok && nclip >= 1) {
                for (int t = 0; t < nclip; t++) redraw_window_only(clipwins[t]);
            } else {
                redraw_all();
            }
            redraw = 0;
        } else {
            if (taskbar_only) draw_taskbar();   // idle clock/module refresh: taskbar strip only
            if (widget_only)  draw_desktop_widget();
        }

        // A fullscreen userspace app (SYS_FBPRESENT) owns the screen and fb_present()
        // yields to it. When it stops presenting (exits) and ownership lapses, force
        // one recomposite so the desktop returns rather than leaving the app's last
        // frame frozen on screen.
        static int comp_was_fs = 0;
        int comp_fs = fb_fullscreen_active();
        if (comp_was_fs && !comp_fs) redraw_all();
        comp_was_fs = comp_fs;

        save_cursor_bg(mouse_x, mouse_y);
        draw_cursor(mouse_x, mouse_y);
        // Publish the finished frame. A recomposite (or a hover handler that may have
        // painted) needs the whole screen. But when ONLY the pointer moved, blit just its
        // old + new 12x16 rects instead of the entire ~3 MB back buffer — a full fb_present
        // to move the cursor one step was the single biggest per-frame cost (measured
        // ~3.4 ms vs microseconds for the two small rects), so pointer motion is now free.
        if (frame_dirty || (moved && mm_dispatched)) {
            // Window drag OR resize: publish just the affected window's damaged region (old ∪
            // new footprint) instead of a full ~3 MB blit. Fall back to a full present whenever
            // something OUTSIDE that rect could also have changed — a drop-snap hint at the
            // screen edge (drags only; a resize never shows one), an animated wallpaper, a live
            // toast, or an open menu — so nothing is left stale. redraw_all still rebuilt the
            // whole back buffer; only the PUBLISH shrinks.
            int snap_showing = drag_id &&
                snap_zone_for_cursor(mouse_x, mouse_y, (int)fw, (int)fh) != WSTATE_NORMAL;
            int drag_partial = drag_dmg_valid && (drag_id || resize_id) && frame_dirty
                && !fb_fullscreen_active() && !snap_showing
                && !wallpaper_animated() && notify_active_count(now) == 0
                && !start_menu_open && !ctx_menu_open && !user_menu_open;

            // Single-window animation partial present: when the ONLY dirty source this frame is
            // exactly one window's on_tick — a game, an animated GIF, the live CPU/RAM graph — and
            // nothing else could have changed outside it (no earlier click/key/drag/resize, no
            // animated wallpaper, no live-or-expiring toast, no open menu), publish just that
            // window's footprint (frame + drop shadow) rather than the whole ~3 MB screen.
            // redraw_all still rebuilt the whole back buffer; only the PUBLISH shrinks — the same
            // lever as the drag (.32) / resize (.35) paths, now covering the last frequent full
            // present. on_tick handlers animate CONTENT inside a fixed window (never move/resize
            // it), so the current footprint IS the whole damage region — no old∪new union needed.
            // Generalised from one window (.39) to up to TICK_MAX_RECTS: collect each ticked
            // window's footprint into a rect list. The blit cost is ~linear in pixels, so N rects
            // beat a full present as long as their COMBINED area stays well under the whole screen
            // — guard on that (else 3-4 near-fullscreen windows would blit MORE than one full frame).
            int tick_partial = 0, tprn = 0;
            int tprx[TICK_MAX_RECTS], tpry[TICK_MAX_RECTS], tprw[TICK_MAX_RECTS], tprh[TICK_MAX_RECTS];
            if (!drag_partial && frame_dirty && redraw_pre_tick == 0
                && tick_changed >= 1 && tick_changed <= TICK_MAX_RECTS
                && !drag_id && !resize_id && !taskbar_only && !widget_only
                && !fb_fullscreen_active() && !wallpaper_animated()
                && notify_active_count(now) == 0 && !toast_just_expired
                && !start_menu_open && !ctx_menu_open && !user_menu_open) {
                const int M = SHADOW_OFFSET + SHADOW_RADIUS + 2;
                long area = 0;
                for (int t = 0; t < tick_changed; t++) {
                    window_t* tw = windows[tick_idx[t]];
                    if (!tw || !tw->visible || tw->state == WSTATE_MINIMIZED) continue;
                    int x0 = tw->x - M, y0 = tw->y - M;
                    int x1 = tw->x + (int)tw->w + M, y1 = tw->y + (int)win_total_h(tw) + M;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 > (int)fw) x1 = (int)fw;
                    if (y1 > (int)fh) y1 = (int)fh;
                    if (x1 - x0 > 0 && y1 - y0 > 0) {
                        tprx[tprn] = x0; tpry[tprn] = y0; tprw[tprn] = x1 - x0; tprh[tprn] = y1 - y0;
                        area += (long)(x1 - x0) * (y1 - y0);
                        tprn++;
                    }
                }
                // Only publish rects when they cover clearly less than the whole screen (85%).
                if (tprn >= 1 && area < (long)fw * (long)fh * 85 / 100) tick_partial = 1;
            }

            if (drag_partial) {
                fb_present_rect(ddx, ddy, ddw, ddh);
                if (moved) {   // cursor may sit outside the window rect near a clamped edge
                    fb_present_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
                    fb_present_rect(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
                }
            } else if (tick_partial) {
                for (int t = 0; t < tprn; t++) fb_present_rect(tprx[t], tpry[t], tprw[t], tprh[t]);
                if (moved) {   // the pointer may have moved this frame too — publish its rects
                    fb_present_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
                    fb_present_rect(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
                }
            } else {
                fb_present();
            }
            frame_dirty = 0;
        } else if (taskbar_only || widget_only) {
            // publish just the taskbar strip and/or the widget rect (each far smaller than
            // the full screen), plus the cursor's rects if it also moved this tick
            if (taskbar_only) fb_present_rect(0, (int)(fh - TASKBAR_H), (int)fw, TASKBAR_H);
            if (widget_only) { int wx, wy; wgt_geom(&wx, &wy); fb_present_rect(wx, wy, WGT_W, WGT_H); }
            if (moved) {
                fb_present_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
                fb_present_rect(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
            }
        } else if (moved) {
            fb_present_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);   // erase the old cursor
            fb_present_rect(mouse_x, mouse_y, CURSOR_W, CURSOR_H); // draw the new one
        }
        for (int d = 0; d < 100000; d++) __asm__ volatile("pause");
    }

    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i]) { kfree(windows[i]); windows[i] = NULL; }
    window_count = 0;
    init_screen();
    clear_screen();
    compositor_active = 0;
}

int compositor_is_running(void) {
    return compositor_active;
}

void compositor_quit(void) {
    quit = 1;
}
