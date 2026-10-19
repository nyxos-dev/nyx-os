#ifndef CALC_WIN_HDR
#define CALC_WIN_HDR

#define CALC_BTN_H 36
#define CALC_BTN_W 40
#define CALC_GAP 4
#define CALC_DISP_H 32
#define CALC_MARGIN 8
#define CALC_COLS 4
#define CALC_WIN_W (CALC_COLS * (CALC_BTN_W + CALC_GAP) - CALC_GAP + 2 * CALC_MARGIN)
#define CALC_WIN_H (CALC_MARGIN + CALC_DISP_H + CALC_GAP + 4 * (CALC_BTN_H + CALC_GAP) + CALC_MARGIN)
#define CALC_WIN_CLIENT_H (CALC_WIN_H)

#include "../../core/kernel.h"
#include "../core/compositor.h"

typedef struct {
    int64_t current_val;
    int64_t mem_val;
    char op;
    int new_input;
    int error;
    char display[32];
} calc_win_t;

calc_win_t* calc_create_ctx(void);
void calc_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void calc_win_click(window_t* win, int mx, int my, int btn);
void calc_win_key(window_t* win, int key);

#endif
