#include "../../core/kernel.h"
#include "compositor.h"
#include "wallpaper_win.h"
#include "../apps/terminal_win.h"
#include "../../drivers/video/font.h"
#include "theme.h"                 // THEME_ACCENT — the runtime UI accent, for cohesive highlights

// The live desktop widget's state lives in compositor.c; the picker's "Panel:" row reads it to
// show the current corner and writes it (+ save_nyx_config) when a button is clicked (v6.5.42).
extern int g_widget_on;            // 1 = the CPU/RAM panel is shown
extern int g_widget_pos;           // 0=bottom-right 1=bottom-left 2=top-right 3=top-left

// The 11 wallpaper base colors. Index 0 is the NyxOS brand purple (morado) — the
// default. draw_background() (compositor.c) builds a vertical gradient from a darker
// to a lighter shade of whichever base color is selected here.
static const struct { uint8_t r, g, b; const char* name; } palette[WALLPAPER_COUNT] = {
    { 130,  90, 210, "Morado"   },   // 0 = brand purple (default)
    {  60, 110, 210, "Azul"     },
    {  40, 160, 175, "Turquesa" },
    {  55, 165,  95, "Verde"    },
    { 140, 165,  60, "Lima"     },
    { 220, 185,  70, "Oro"      },
    { 225, 130,  50, "Naranja"  },
    { 205,  70,  70, "Rojo"     },
    { 215,  95, 175, "Rosa"     },
    {  95, 105, 130, "Pizarra"  },
    {  45,  50,  70, "Carbon"   },
};

// The wallpaper render styles (the "scene" the compositor paints from the base
// color). "Nightfall" — the moon-and-stars scene — is the default: NyxOS is named
// for Nyx, the goddess of night, so the night sky IS the brand identity. The clean
// gradient and a flat solid stay one click away in the Wallpaper app.
static const char* style_names[WP_STYLE_COUNT] = { "Limpio", "Nightfall", "Plano", "Estrellas", "Meteoros", "Aurora", "Nebula", "Luces", "Ondas", "Astral", "Lluvia", "Cordillera" };

static int g_wallpaper = 0;                    // selected base color; default = morado
static int g_style     = WP_STYLE_NIGHTFALL;   // selected render style; default = moon + stars

// Arbitrary base-color override — nyx.conf `accent = #RRGGBB` picks any color, not just the 11
// presets. Off by default (the palette index wins); set via wallpaper_set_color_rgb and cleared
// whenever a preset index is chosen, so every named-accent path stays byte-identical.
static int      g_wp_override     = 0;
static uint32_t g_wp_override_rgb = 0;

uint32_t wallpaper_base_color(void) {
    if (g_wp_override) return g_wp_override_rgb;
    return fb_rgb(palette[g_wallpaper].r, palette[g_wallpaper].g, palette[g_wallpaper].b);
}

// rgb of ANY palette color by index (not just the selected one) — for the nyx.conf
// `border` key, which can outline focused windows in any of the 11 accent colors.
uint32_t wallpaper_color_rgb(int idx) {
    if (idx < 0 || idx >= WALLPAPER_COUNT) idx = 0;
    return fb_rgb(palette[idx].r, palette[idx].g, palette[idx].b);
}

int wallpaper_style(void) {
    return g_style;
}

// Setters + name lookups so /etc/nyx.conf (the rice config) can pick the wallpaper by name
// (v6.5.23). Out-of-range is ignored; an unknown name returns -1 so the caller keeps the default.
void wallpaper_set_style(int style) { if (style >= 0 && style < WP_STYLE_COUNT) g_style = style; }
void wallpaper_set_color(int idx)   { if (idx   >= 0 && idx   < WALLPAPER_COUNT) { g_wallpaper = idx; g_wp_override = 0; } }
// nyx.conf `accent = #RRGGBB`: set the base color to an arbitrary rgb (tints wallpaper + UI).
void wallpaper_set_color_rgb(uint32_t rgb) { g_wp_override = 1; g_wp_override_rgb = rgb; }
int wallpaper_color(void)           { return g_wallpaper; }   // current base-color index
int      wallpaper_is_rgb_override(void) { return g_wp_override; }     // nyx.conf `accent = #RRGGBB` active?
uint32_t wallpaper_override_rgb(void)    { return g_wp_override_rgb; } // its rgb (valid when the above is 1)
// Index -> name, for writing /etc/nyx.conf back from the GUI (save_nyx_config). Out-of-range
// falls back to the default names so a bad index never yields a NULL into snprintf.
const char* wallpaper_style_name(int i) { return (i >= 0 && i < WP_STYLE_COUNT)  ? style_names[i] : "Nightfall"; }
const char* wallpaper_color_name(int i) { return (i >= 0 && i < WALLPAPER_COUNT) ? palette[i].name : "Morado"; }
int wallpaper_style_from_name(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < WP_STYLE_COUNT; i++)
        if (strcmp(style_names[i], name) == 0) return i;
    return -1;
}
int wallpaper_color_from_name(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < WALLPAPER_COUNT; i++)
        if (strcmp(palette[i].name, name) == 0) return i;
    return -1;
}

// Style-button grid geometry (top of the content; shared by draw + click). Twelve
// styles now, laid out 4 per row over 3 rows (the 3rd row is Ondas/Astral/Lluvia/Cordillera).
#define WP_STYLE_COLS 4
#define WP_STYLE_OX   16
#define WP_STYLE_OY   34
#define WP_STYLE_BW   78
#define WP_STYLE_BH   28
#define WP_STYLE_BGAP 6
#define WP_STYLE_RGAP 6
// x/y of style button i (col-major within a row, then wrap to the next row).
#define WP_STYLE_X(i) (WP_STYLE_OX + ((i) % WP_STYLE_COLS) * (WP_STYLE_BW + WP_STYLE_BGAP))
#define WP_STYLE_Y(i) (WP_STYLE_OY + ((i) / WP_STYLE_COLS) * (WP_STYLE_BH + WP_STYLE_RGAP))

// Color-swatch grid geometry (below the two style rows; shared by draw + click).
#define WP_COLS   4
#define WP_SW     74     // swatch width
#define WP_SH     54     // swatch height
#define WP_GAP    12
#define WP_OX     16     // grid origin x within the content
#define WP_OY     158    // grid origin y within the content (below the 3-row style grid)
#define WP_ROW_H  (WP_SH + 22 + WP_GAP)   // swatch + label + gap

// "Panel:" widget-corner control row (below the color grid). Five buttons: Off + the four
// corners; picking a corner also turns the panel on. wg_pos[i] = the g_widget_pos value the
// button selects, or -1 for Off. The current choice is highlighted in the runtime accent.
#define WP_WG_OY   420
#define WP_WG_BW   58
#define WP_WG_BH   24
#define WP_WG_GAP  5
#define WP_WG_BX   64                       // first button x (after the "Panel:" label)
#define WP_WG_X(i) (WP_WG_BX + (i) * (WP_WG_BW + WP_WG_GAP))
static const char* wg_labels[5] = { "Off", "BR", "BL", "TR", "TL" };
static const int   wg_pos[5]    = {  -1,    0,    1,    2,    3  };

#define WP_TERM_OY   456
#define WP_TERM_BX   96
#define WP_TERM_BW   104
#define WP_TERM_BH   26
#define WP_TERM_GAP  8
#define WP_TERM_X(i) (WP_TERM_BX + (i) * (WP_TERM_BW + WP_TERM_GAP))
static const char* term_labels[2] = { "Classic", "Nightfall" };
static const char* term_names[2] = { "classic", "nightfall" };

// "Reset to defaults" button (below the Terminal row): restores the seeded rice —
// Nightfall wallpaper, Morado accent, panel on in the bottom-right — and persists it.
#define WP_DEC_OY   490
#define WP_DEC_X(i) WP_TERM_X(i)
static const char* decoration_labels[2] = { "Classic", "Flat" };
static const char* decoration_names[2] = { "classic", "flat" };

#define WP_RST_OY  526
#define WP_RST_X   16
#define WP_RST_W   168
#define WP_RST_H   26

// Fill a disc clipped to the rect [clx,cly,clw,clh) — a tiny moon for the preview
// swatches (bg_fill_circle lives in compositor.c and isn't visible here).
static void wp_fill_disc_clip(int cx, int cy, int r, uint32_t c, int clx, int cly, int clw, int clh) {
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < cly || yy >= cly + clh) continue;
        int dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        int x0 = cx - dx, x1 = cx + dx;
        if (x0 < clx) x0 = clx;
        if (x1 >= clx + clw) x1 = clx + clw - 1;
        if (x1 >= x0) fb_fill_rect(x0, yy, x1 - x0 + 1, 1, c);
    }
}

// Paint a faithful MINIATURE of the wallpaper scene into (x,y,w,h): the same
// 45%..115% vertical gradient of the base hue that draw_background() paints, plus
// — for the night styles (Nightfall / Estrellas / Meteoros) — a small moon in the
// upper-right and a scatter of tiny stars, and for Meteoros a short meteor streak.
// Flat is a solid fill; Limpio is just the gradient. This turns each colour swatch
// into a LIVE PREVIEW of what that hue looks like in the currently-selected style.
static void wallpaper_draw_preview(int x, int y, int w, int h, int br, int bg, int bb, int style) {
    if (style == WP_STYLE_FLAT) {
        fb_fill_rect(x, y, w, h, fb_rgb((uint8_t)br, (uint8_t)bg, (uint8_t)bb));
        return;
    }
    int hd = (h > 1) ? h : 1;
    for (int i = 0; i < h; i++) {
        int pct = 45 + i * 70 / hd;                         // 45% (top) .. 115% (bottom)
        int r = br * pct / 100; if (r > 255) r = 255;
        int g = bg * pct / 100; if (g > 255) g = 255;
        int b = bb * pct / 100; if (b > 255) b = 255;
        fb_fill_rect(x, y + i, w, 1, fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }
    if (style != WP_STYLE_NIGHTFALL && style != WP_STYLE_STARFIELD &&
        style != WP_STYLE_SHOOTINGSTAR && style != WP_STYLE_AURORA &&
        style != WP_STYLE_NEBULA && style != WP_STYLE_LUCES && style != WP_STYLE_ONDAS &&
        style != WP_STYLE_CONSTELACIONES && style != WP_STYLE_LLUVIA &&
        style != WP_STYLE_CORDILLERA)
        return;                                             // Limpio: gradient only.

    // Moon (upper-right) with a small halo, then deterministic tiny stars that keep
    // clear of the moon — the same LCG draw_background() uses, so previews are stable.
    int mx = x + w - 15, my = y + 13, mr = 6;
    wp_fill_disc_clip(mx, my, mr + 3, fb_rgb(88, 78, 126),   x, y, w, h);
    wp_fill_disc_clip(mx, my, mr + 1, fb_rgb(150, 138, 196), x, y, w, h);
    wp_fill_disc_clip(mx, my, mr,     fb_rgb(214, 202, 244), x, y, w, h);
    uint32_t seed = 0x9E3779B1u;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245u + 12345u; int sx = x + (int)((seed >> 9) % (uint32_t)w);
        seed = seed * 1103515245u + 12345u; int sy = y + (int)((seed >> 9) % (uint32_t)(h * 3 / 4));
        int ex = sx - mx, ey = sy - my;
        if (ex * ex + ey * ey < (mr + 3) * (mr + 3)) continue;
        int shade = (int)((seed >> 20) & 3);
        uint32_t sc = (shade == 0) ? fb_rgb(150, 142, 186)
                    : (shade == 1) ? fb_rgb(190, 182, 220)
                                   : fb_rgb(224, 218, 248);
        fb_fill_rect(sx, sy, 1, 1, sc);
    }
    if (style == WP_STYLE_SHOOTINGSTAR) {                    // a short meteor streak
        int hx = x + w / 3, hy = y + h / 2;
        for (int s = 0; s <= 10; s++) {
            int tx = hx + 2 * s, ty = hy - s;
            if (tx < x || tx >= x + w || ty < y || ty >= y + h) continue;
            int lum = 240 - s * 18; if (lum < 60) lum = 60;
            int sz = (s < 2) ? 2 : 1;
            fb_fill_rect(tx, ty, sz, sz, fb_rgb((uint8_t)(lum * 92 / 100), (uint8_t)(lum * 96 / 100), (uint8_t)lum));
        }
    }
    if (style == WP_STYLE_AURORA) {                          // a soft green-violet aurora band
        int cyb = y + h * 42 / 100;
        for (int px = 0; px < w; px++) {
            int wave = (px * 6) & 31;                        // cheap triangle undulation
            int crest = cyb + (wave < 16 ? wave : 32 - wave) - 8;
            for (int dy = -6; dy <= 6; dy++) {
                int yy = crest + dy; if (yy < y || yy >= y + h) continue;
                int ady = dy < 0 ? -dy : dy;
                int lum = 90 - ady * 14; if (lum <= 0) continue;
                int cr = (px & 8) ? 120 : 90, cg = 220, cb = (px & 8) ? 235 : 170;
                fb_fill_rect(x + px, yy, 1, 1,
                             fb_rgb((uint8_t)(cr * lum / 100 + 30), (uint8_t)(cg * lum / 100 + 20),
                                    (uint8_t)(cb * lum / 100 + 30)));
            }
        }
    }
    if (style == WP_STYLE_NEBULA) {                          // two soft purple gas blobs
        int cx0[2] = { x + w / 3, x + 2 * w / 3 }, cy0[2] = { y + h * 40 / 100, y + h * 55 / 100 };
        int rad[2] = { w / 4, w / 5 };
        for (int b = 0; b < 2; b++) {
            for (int dy = -rad[b]; dy <= rad[b]; dy++) {
                int yy = cy0[b] + dy; if (yy < y || yy >= y + h) continue;
                for (int dx = -rad[b]; dx <= rad[b]; dx++) {
                    int xx = cx0[b] + dx; if (xx < x || xx >= x + w) continue;
                    int d2 = dx * dx + dy * dy, r2 = rad[b] * rad[b];
                    if (d2 >= r2) continue;
                    int lum = 46 * (r2 - d2) / r2;           // soft falloff to the edge
                    int nr = b ? 120 : 165, ng = 74, nb = b ? 235 : 195;
                    fb_fill_rect(xx, yy, 1, 1,
                                 fb_rgb((uint8_t)(60 + nr * lum / 100), (uint8_t)(50 + ng * lum / 100),
                                        (uint8_t)(90 + nb * lum / 100)));
                }
            }
        }
    }
    if (style == WP_STYLE_LUCES) {                           // a few soft lilac orbs (fireflies)
        int ox[5] = { x + w/5, x + w/2, x + 3*w/4, x + w/3, x + 4*w/5 };
        int oy[5] = { y + h*3/4, y + h/2, y + h*3/5, y + h*2/5, y + h*4/5 };
        for (int o = 0; o < 5; o++) {
            for (int dy = -2; dy <= 2; dy++) {
                int yy = oy[o] + dy; if (yy < y || yy >= y + h) continue;
                for (int dx = -2; dx <= 2; dx++) {
                    int xx = ox[o] + dx; if (xx < x || xx >= x + w) continue;
                    int d2 = dx*dx + dy*dy; if (d2 > 4) continue;
                    int lum = (4 - d2) * 60 / 4;             // soft falloff
                    fb_fill_rect(xx, yy, 1, 1,
                                 fb_rgb((uint8_t)(120 + lum), (uint8_t)(110 + lum), (uint8_t)(150 + lum)));
                }
            }
        }
    }
    if (style == WP_STYLE_ONDAS) {                           // two moonlight ripples round the moon
        for (int ri = 0; ri < 2; ri++) {
            int rr = 13 + ri * 10, lum = 150 - ri * 45;
            for (int py = y; py < y + h; py++) {
                int dyv = py - my;
                for (int px = x; px < x + w; px++) {
                    int dxv = px - mx, d2 = dxv * dxv + dyv * dyv;
                    if (d2 >= (rr - 1) * (rr - 1) && d2 <= (rr + 1) * (rr + 1))
                        fb_fill_rect(px, py, 1, 1, fb_rgb((uint8_t)lum, (uint8_t)(lum - 10), (uint8_t)(lum + 30)));
                }
            }
        }
    }
    if (style == WP_STYLE_CONSTELACIONES) {                  // a tiny 3-star constellation
        int vx[3] = { x + w / 4, x + w / 2, x + 2 * w / 3 };
        int vy[3] = { y + h / 2, y + h * 2 / 5, y + h * 3 / 5 };
        for (int s = 0; s < 2; s++)                          // faint connecting threads
            for (int p = 0; p <= 8; p++) {
                int lx = vx[s] + (vx[s + 1] - vx[s]) * p / 8;
                int ly = vy[s] + (vy[s + 1] - vy[s]) * p / 8;
                if (lx >= x && lx < x + w && ly >= y && ly < y + h) fb_fill_rect(lx, ly, 1, 1, fb_rgb(150, 142, 200));
            }
        for (int s = 0; s < 3; s++)                          // bright vertex stars
            if (vx[s] >= x && vx[s] < x + w && vy[s] >= y && vy[s] < y + h)
                fb_fill_rect(vx[s], vy[s], 2, 2, fb_rgb(216, 210, 246));
    }
    if (style == WP_STYLE_LLUVIA) {                          // a few falling lilac drops
        int dx5[5] = { x + w/6, x + w/2, x + 3*w/4, x + w/3, x + 5*w/6 };
        int dy5[5] = { y + h/3, y + h*3/5, y + h/4, y + h*4/5, y + h/2 };
        for (int d = 0; d < 5; d++) {
            for (int s = 0; s <= 5; s++) {                   // head at bottom, fading up the streak
                int ty = dy5[d] - s; if (ty < y || ty >= y + h || dx5[d] < x || dx5[d] >= x + w) continue;
                int lum = 230 - s * 30; if (lum < 60) lum = 60;
                fb_fill_rect(dx5[d], ty, 1, (s < 2) ? 2 : 1,
                             fb_rgb((uint8_t)(lum * 90 / 100), (uint8_t)(lum * 86 / 100), (uint8_t)lum));
            }
        }
    }
    if (style == WP_STYLE_CORDILLERA) {                      // a tiny two-layer mountain horizon
        int by[2] = { y + h * 70 / 100, y + h * 82 / 100 };
        uint32_t rc[2] = { fb_rgb(54, 44, 84), fb_rgb(18, 14, 30) };
        for (int r = 0; r < 2; r++)
            for (int px = 0; px < w; px++) {
                int wv = (px * (r ? 9 : 5)) & 15;            // 0..15 saw per column
                int crest = by[r] + (wv < 8 ? wv : 16 - wv) - 4;   // +/-4 triangle ridge
                for (int yy = crest; yy < y + h; yy++)
                    if (yy >= y) fb_fill_rect(x + px, yy, 1, 1, rc[r]);
            }
    }
}

void wallpaper_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    (void)win;
    const uint32_t bg = fb_rgb(28, 28, 34);
    fb_fill_rect(cx, cy, cw, ch, bg);

    // --- Style row: pick how the desktop is painted -------------------------
    font_draw_string(cx + 12, cy + 14, "Estilo:", fb_rgb(230, 230, 240), bg);
    for (int i = 0; i < WP_STYLE_COUNT; i++) {
        int x = cx + WP_STYLE_X(i);
        int y = cy + WP_STYLE_Y(i);
        int sel = (i == g_style);
        uint32_t fill = sel ? THEME_ACCENT : fb_rgb(48, 48, 58);   // selected = the runtime accent, not a fixed purple
        fb_fill_rect(x, y, WP_STYLE_BW, WP_STYLE_BH, fill);
        uint32_t fr = sel ? fb_rgb(255, 255, 255) : fb_rgb(80, 80, 92);
        fb_fill_rect(x, y, WP_STYLE_BW, 1, fr);                    // top
        fb_fill_rect(x, y + WP_STYLE_BH - 1, WP_STYLE_BW, 1, fr);  // bottom
        fb_fill_rect(x, y, 1, WP_STYLE_BH, fr);                    // left
        fb_fill_rect(x + WP_STYLE_BW - 1, y, 1, WP_STYLE_BH, fr);  // right
        int nlen = (int)strlen(style_names[i]) * FONT_WIDTH;
        font_draw_string(x + (WP_STYLE_BW - nlen) / 2, y + (WP_STYLE_BH - FONT_HEIGHT) / 2,
                         style_names[i], sel ? fb_rgb(255, 255, 255) : fb_rgb(200, 200, 210), fill);
    }

    // --- Color grid: pick the base hue --------------------------------------
    font_draw_string(cx + 12, cy + 140, "Color:", fb_rgb(230, 230, 240), bg);
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        int col = i % WP_COLS, row = i / WP_COLS;
        int x = cx + WP_OX + col * (WP_SW + WP_GAP);
        int y = cy + WP_OY + row * WP_ROW_H;
        // The selected swatch gets a white frame.
        if (i == g_wallpaper) fb_fill_rect(x - 3, y - 3, WP_SW + 6, WP_SH + 6, fb_rgb(255, 255, 255));
        // Live preview: the actual scene in this hue under the current style, not a flat block.
        wallpaper_draw_preview(x, y, WP_SW, WP_SH, palette[i].r, palette[i].g, palette[i].b, g_style);
        int nlen = (int)strlen(palette[i].name) * FONT_WIDTH;
        font_draw_string(x + (WP_SW - nlen) / 2, y + WP_SH + 4, palette[i].name,
                         i == g_wallpaper ? fb_rgb(255, 255, 255) : fb_rgb(175, 175, 185), bg);
    }

    // --- Panel row: where the live CPU/RAM widget sits (or Off) ---------------
    font_draw_string(cx + 12, cy + WP_WG_OY + (WP_WG_BH - FONT_HEIGHT) / 2, "Panel:", fb_rgb(230, 230, 240), bg);
    for (int i = 0; i < 5; i++) {
        int x = cx + WP_WG_X(i), y = cy + WP_WG_OY;
        int sel = (wg_pos[i] < 0) ? !g_widget_on : (g_widget_on && g_widget_pos == wg_pos[i]);
        uint32_t fill = sel ? THEME_ACCENT : fb_rgb(48, 48, 58);
        fb_fill_rect(x, y, WP_WG_BW, WP_WG_BH, fill);
        uint32_t fr = sel ? fb_rgb(255, 255, 255) : fb_rgb(80, 80, 92);
        fb_fill_rect(x, y, WP_WG_BW, 1, fr);
        fb_fill_rect(x, y + WP_WG_BH - 1, WP_WG_BW, 1, fr);
        fb_fill_rect(x, y, 1, WP_WG_BH, fr);
        fb_fill_rect(x + WP_WG_BW - 1, y, 1, WP_WG_BH, fr);
        int nlen = (int)strlen(wg_labels[i]) * FONT_WIDTH;
        font_draw_string(x + (WP_WG_BW - nlen) / 2, y + (WP_WG_BH - FONT_HEIGHT) / 2, wg_labels[i],
                         sel ? fb_rgb(255, 255, 255) : fb_rgb(200, 200, 210), fill);
    }

    font_draw_string(cx + 12, cy + WP_TERM_OY + (WP_TERM_BH - FONT_HEIGHT) / 2,
                     "Terminal:", fb_rgb(230, 230, 240), bg);
    for (int i = 0; i < 2; i++) {
        int x = cx + WP_TERM_X(i), y = cy + WP_TERM_OY;
        int sel = strcmp(terminal_palette_name(), term_names[i]) == 0;
        uint32_t fill = sel ? THEME_ACCENT : fb_rgb(48, 48, 58);
        uint32_t fr = sel ? fb_rgb(255, 255, 255) : fb_rgb(80, 80, 92);
        fb_fill_rect(x, y, WP_TERM_BW, WP_TERM_BH, fill);
        fb_fill_rect(x, y, WP_TERM_BW, 1, fr);
        fb_fill_rect(x, y + WP_TERM_BH - 1, WP_TERM_BW, 1, fr);
        fb_fill_rect(x, y, 1, WP_TERM_BH, fr);
        fb_fill_rect(x + WP_TERM_BW - 1, y, 1, WP_TERM_BH, fr);
        int nlen = (int)strlen(term_labels[i]) * FONT_WIDTH;
        font_draw_string(x + (WP_TERM_BW - nlen) / 2, y + (WP_TERM_BH - FONT_HEIGHT) / 2,
                         term_labels[i], sel ? fb_rgb(255, 255, 255) : fb_rgb(200, 200, 210), fill);
    }

    font_draw_string(cx + 12, cy + WP_DEC_OY + (WP_TERM_BH - FONT_HEIGHT) / 2,
                     "Windows:", THEME_TEXT, bg);
    for (int i = 0; i < 2; i++) {
        int x = cx + WP_DEC_X(i), y = cy + WP_DEC_OY;
        int sel = !strcmp(desktop_decoration_name(), decoration_names[i]);
        uint32_t fill = sel ? THEME_ACCENT : fb_rgb(48, 48, 58);
        fb_fill_rect(x, y, WP_TERM_BW, WP_TERM_BH, fill);
        int text_width = (int)strlen(decoration_labels[i]) * FONT_WIDTH;
        font_draw_string(x + (WP_TERM_BW - text_width) / 2, y + (WP_TERM_BH - FONT_HEIGHT) / 2,
                         decoration_labels[i], THEME_TEXT, fill);
    }

    // --- Reset button: one click back to the seeded defaults ------------------
    {
        int x = cx + WP_RST_X, y = cy + WP_RST_OY;
        fb_fill_rect(x, y, WP_RST_W, WP_RST_H, fb_rgb(58, 48, 62));
        uint32_t fr = fb_rgb(96, 84, 104);
        fb_fill_rect(x, y, WP_RST_W, 1, fr);
        fb_fill_rect(x, y + WP_RST_H - 1, WP_RST_W, 1, fr);
        fb_fill_rect(x, y, 1, WP_RST_H, fr);
        fb_fill_rect(x + WP_RST_W - 1, y, 1, WP_RST_H, fr);
        const char* lbl = "Reset to defaults";
        int nlen = (int)strlen(lbl) * FONT_WIDTH;
        font_draw_string(x + (WP_RST_W - nlen) / 2, y + (WP_RST_H - FONT_HEIGHT) / 2, lbl,
                         fb_rgb(225, 225, 235), fb_rgb(58, 48, 62));
    }
}

void wallpaper_win_click(window_t* win, int mx, int my, int btn) {
    int cx = win->x, cy = win->y + TITLE_H;   // content origin (matches draw)

    // Style grid hit-test (4 per row, 2 rows).
    for (int i = 0; i < WP_STYLE_COUNT; i++) {
        int x = cx + WP_STYLE_X(i);
        int y = cy + WP_STYLE_Y(i);
        if (mx >= x && mx < x + WP_STYLE_BW && my >= y && my < y + WP_STYLE_BH) {
            wallpaper_set_style(i);
            save_nyx_config();
            return;
        }
    }

    // Color grid hit-test. Picking a color sets the wallpaper base AND retints the whole UI
    // accent (title bars / taskbar / menus), matching nyx.conf's `accent` — so the picker is
    // a live THEME switcher, not just a wallpaper tint. Then persist the choice.
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        int col = i % WP_COLS, row = i / WP_COLS;
        int x = cx + WP_OX + col * (WP_SW + WP_GAP);
        int y = cy + WP_OY + row * WP_ROW_H;
        if (mx >= x && mx < x + WP_SW && my >= y && my < y + WP_SH) {
            wallpaper_set_color(i);
            theme_set_accent(wallpaper_base_color());
            save_nyx_config();
            return;
        }
    }

    // Panel row hit-test: Off, or one of the four corners (which also turns the panel on).
    for (int i = 0; i < 5; i++) {
        int x = cx + WP_WG_X(i), y = cy + WP_WG_OY;
        if (mx >= x && mx < x + WP_WG_BW && my >= y && my < y + WP_WG_BH) {
            if (wg_pos[i] < 0) g_widget_on = 0;
            else { g_widget_on = 1; g_widget_pos = wg_pos[i]; }
            save_nyx_config();                // persist; the compositor repaints on this click
            return;
        }
    }

    if (btn == 1) for (int i = 0; i < 2; i++) {
        int x = cx + WP_TERM_X(i), y = cy + WP_TERM_OY;
        if (mx >= x && mx < x + WP_TERM_BW && my >= y && my < y + WP_TERM_BH) {
            terminal_set_palette(term_names[i]);
            save_nyx_config();
            return;
        }
    }

    if (btn == 1) for (int i = 0; i < 2; i++) {
        int x = cx + WP_DEC_X(i), y = cy + WP_DEC_OY;
        if (mx >= x && mx < x + WP_TERM_BW && my >= y && my < y + WP_TERM_BH) {
            desktop_set_decoration(decoration_names[i]);
            save_nyx_config();
            return;
        }
    }

    // Reset button: back to the seeded defaults (Nightfall / Morado / panel on, bottom-right).
    {
        int x = cx + WP_RST_X, y = cy + WP_RST_OY;
        if (mx >= x && mx < x + WP_RST_W && my >= y && my < y + WP_RST_H) {
            reset_nyx_config();
            save_nyx_config();
            return;
        }
    }
}
