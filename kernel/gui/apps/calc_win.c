#include "../core/theme.h"
#include "../../core/kernel.h"
#include "../core/compositor.h"
#include "calc_win.h"
#include "../../drivers/video/font.h"

static const char* btn_labels[16] = {
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "C", "0", "=", "+"
};

// Maps 4x4 grid position to button ID (0-9=digit, 10='=',11='+',12='-',13='*',14='/',15='C')
static const uint8_t pos_to_btn[16] = {
    7, 8, 9, 14,
    4, 5, 6, 13,
    1, 2, 3, 12,
    15, 0, 10, 11
};

static void update_display(calc_win_t* calc) {
    if (calc->error) snprintf(calc->display, sizeof(calc->display), "ERR");
    else snprintf(calc->display, sizeof(calc->display), "%ld", (long long)calc->current_val);
}

calc_win_t* calc_create_ctx(void) {
    calc_win_t* calc = (calc_win_t*)kmalloc(sizeof(calc_win_t));
    if (!calc) return NULL;
    memset_asm(calc, 0, sizeof(calc_win_t));
    calc->op = 0;
    calc->new_input = 1;
    update_display(calc);
    return calc;
}

static void calc_set_error(calc_win_t* calc) {
    calc->current_val = 0;
    calc->mem_val = 0;
    calc->op = 0;
    calc->new_input = 1;
    calc->error = 1;
    update_display(calc);
}

static int calc_do_op(calc_win_t* calc) {
    int64_t result = calc->current_val;
    int failed = 0;
    if (calc->op == '+') failed = __builtin_add_overflow(calc->mem_val, calc->current_val, &result);
    else if (calc->op == '-') failed = __builtin_sub_overflow(calc->mem_val, calc->current_val, &result);
    else if (calc->op == '*') failed = __builtin_mul_overflow(calc->mem_val, calc->current_val, &result);
    else if (calc->op == '/') {
        const int64_t min_value = -9223372036854775807LL - 1;
        failed = calc->current_val == 0 || (calc->mem_val == min_value && calc->current_val == -1);
        if (!failed) result = calc->mem_val / calc->current_val;
    }
    if (failed) { calc_set_error(calc); return 0; }
    calc->mem_val = result;
    calc->current_val = result;
    return 1;
}

static void calc_handle_btn(calc_win_t* calc, int btn_id) {
    if (calc->error) {
        if ((btn_id < 0 || btn_id > 9) && btn_id != 15) return;
        calc->error = 0;
    }
    switch (btn_id) {
        case 0 ... 9:
            if (calc->new_input) {
                calc->current_val = btn_id;
                calc->new_input = 0;
            } else {
                int64_t scaled, value;
                if (__builtin_mul_overflow(calc->current_val, (int64_t)10, &scaled) ||
                    __builtin_add_overflow(scaled, (int64_t)btn_id, &value)) {
                    calc_set_error(calc);
                    return;
                }
                calc->current_val = value;
            }
            break;
        case 10:
            if (!calc_do_op(calc)) return;
            calc->op = 0;
            calc->new_input = 1;
            break;
        case 11:
            if (!calc_do_op(calc)) return;
            calc->op = '+';
            calc->new_input = 1;
            break;
        case 12:
            if (!calc_do_op(calc)) return;
            calc->op = '-';
            calc->new_input = 1;
            break;
        case 13:
            if (!calc_do_op(calc)) return;
            calc->op = '*';
            calc->new_input = 1;
            break;
        case 14:
            if (!calc_do_op(calc)) return;
            calc->op = '/';
            calc->new_input = 1;
            break;
        case 15:
            calc->current_val = 0;
            calc->mem_val = 0;
            calc->op = 0;
            calc->new_input = 1;
            break;
        case 16:   // backspace (keyboard only): drop the last entered digit while typing
            if (!calc->new_input) calc->current_val /= 10;   // integer divide truncates toward 0
            break;
        default:
            return;
    }
    update_display(calc);
    return;
}

void calc_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    (void)cx; (void)cy; (void)cw; (void)ch;
    calc_win_t* calc = (calc_win_t*)win->reserved;
    if (!calc) return;

    int x0 = cx + CALC_MARGIN;
    int y0 = cy + CALC_MARGIN;

    fb_fill_rect(cx, cy, cw, ch, fb_rgb(40, 40, 50));
    uint32_t disp_w = CALC_COLS * (CALC_BTN_W + CALC_GAP) - CALC_GAP;
    fb_fill_rect(x0, y0, disp_w, CALC_DISP_H, fb_rgb(210, 220, 190));
    fb_fill_rect(x0, y0, disp_w, 1, THEME_BORDER);
    fb_fill_rect(x0, y0 + CALC_DISP_H - 1, disp_w, 1, THEME_BORDER);
    fb_fill_rect(x0, y0, 1, CALC_DISP_H, THEME_BORDER);
    fb_fill_rect(x0 + disp_w - 1, y0, 1, CALC_DISP_H, THEME_BORDER);

    int text_x = x0 + 6;
    int text_y = y0 + (CALC_DISP_H - 16) / 2;
    uint32_t text_color = fb_rgb(10, 10, 10);
    int len = strlen(calc->display);
    int text_w = len * 8;
    if (text_w > (int)disp_w - 12) {
        text_x = x0 + disp_w - text_w - 6;
    }
    font_draw_string(text_x, text_y, calc->display, text_color, fb_rgb(210, 220, 190));

    int bx = x0;
    int by = y0 + CALC_DISP_H + CALC_GAP;
    for (int i = 0; i < 16; i++) {
        int col = i % CALC_COLS;
        int row = i / CALC_COLS;
        int bix = bx + col * (CALC_BTN_W + CALC_GAP);
        int biy = by + row * (CALC_BTN_H + CALC_GAP);

        uint32_t btn_color;
        if (btn_labels[i][0] >= '0' && btn_labels[i][0] <= '9') {
            btn_color = fb_rgb(60, 60, 80);
        } else if (btn_labels[i][0] == 'C') {
            btn_color = fb_rgb(160, 40, 40);
        } else if (btn_labels[i][0] == '=') {
            btn_color = fb_rgb(40, 100, 160);
        } else {
            btn_color = fb_rgb(50, 50, 70);
        }
        fb_fill_rect(bix, biy, CALC_BTN_W, CALC_BTN_H, btn_color);
        fb_fill_rect(bix, biy, CALC_BTN_W, 1, fb_rgb(120, 120, 140));
        fb_fill_rect(bix, biy + CALC_BTN_H - 1, CALC_BTN_W, 1, fb_rgb(80, 80, 100));
        fb_fill_rect(bix, biy, 1, CALC_BTN_H, fb_rgb(120, 120, 140));
        fb_fill_rect(bix + CALC_BTN_W - 1, biy, 1, CALC_BTN_H, fb_rgb(80, 80, 100));

        int tlen = strlen(btn_labels[i]);
        int tx = bix + (CALC_BTN_W - tlen * 8) / 2;
        int ty = biy + (CALC_BTN_H - 16) / 2;
        font_draw_string(tx, ty, btn_labels[i], fb_rgb(230, 230, 230), btn_color);
    }
}

void calc_win_click(window_t* win, int mx, int my, int btn) {
    if (btn != 1) return;
    calc_win_t* calc = (calc_win_t*)win->reserved;
    if (!calc) return;

    int x0 = win->x + CALC_MARGIN;
    int y0 = WIN_CLIENT_Y(win) + CALC_MARGIN;   /* was missing TITLE_H */
    int by_start = y0 + CALC_DISP_H + CALC_GAP;

    for (int i = 0; i < 16; i++) {
        int col = i % CALC_COLS;
        int row = i / CALC_COLS;
        int bix = x0 + col * (CALC_BTN_W + CALC_GAP);
        int biy = by_start + row * (CALC_BTN_H + CALC_GAP);

        if (mx >= bix && mx < bix + CALC_BTN_W && my >= biy && my < biy + CALC_BTN_H) {
            calc_handle_btn(calc, pos_to_btn[i]);
            break;
        }
    }
}

void calc_win_key(window_t* win, int key) {
    calc_win_t* calc = (calc_win_t*)win->reserved;
    if (!calc) return;

    if (key >= '0' && key <= '9') {
        calc_handle_btn(calc, key - '0');
    } else if (key == '+') {
        calc_handle_btn(calc, 11);
    } else if (key == '-') {
        calc_handle_btn(calc, 12);
    } else if (key == '*') {
        calc_handle_btn(calc, 13);
    } else if (key == '/') {
        calc_handle_btn(calc, 14);
    } else if (key == '=' || key == '\r' || key == '\n') {
        calc_handle_btn(calc, 10);
    } else if (key == 'c' || key == 'C') {
        calc_handle_btn(calc, 15);
    } else if (key == '\b' || key == 0x7F) {
        calc_handle_btn(calc, 16);   // backspace: correct the last digit without clearing
    }
}
