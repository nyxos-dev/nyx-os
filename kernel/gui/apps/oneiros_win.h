/*
 * oneiros_win.h - Oneiros, the NyxOS AI, as a desktop app.
 *
 * A windowed front-end for the from-scratch generative model that ships in NyxOS
 * (the same compact char-level RMSNorm transformer the `nyxgen` command runs).
 * Type a seed, press Dream, and watch it stream NyxOS-style C, generated live in
 * the compositor. Every neuron is home-grown; the model weights load from the
 * initramfs (/usr/pkg/nyxgen/model.bin). See github.com/nyxos-dev/oneiros.
 */
#ifndef ONEIROS_WIN_H_INCLUDED
#define ONEIROS_WIN_H_INCLUDED

#include "../core/compositor.h"

#define ONEIROS_WIN_W 580
#define ONEIROS_WIN_H 470

typedef struct oneiros_win oneiros_win_t;

oneiros_win_t* oneiros_create_ctx(void);
void oneiros_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void oneiros_win_click(window_t* win, int mx, int my, int btn);
void oneiros_win_key(window_t* win, int key);
int  oneiros_win_tick(window_t* win);
void oneiros_win_close(window_t* win);

#endif /* ONEIROS_WIN_H_INCLUDED */
