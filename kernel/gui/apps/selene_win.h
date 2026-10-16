#ifndef SELENE_WIN_H
#define SELENE_WIN_H
#include "../core/compositor.h"

// Selene - the NyxOS web browser (introduced v5.9.44). A GUI window with a URL bar and a
// rendered (HTML-tag-stripped, word-wrapped) view of a page fetched over HTTP or HTTPS
// (TLS 1.2, since v5.9.56). It ensures connectivity (DHCP) on load, resolves DNS and fetches
// the real page. Named for Selene, the Greek moon goddess -
// on-theme with Nyx (night). Launched by the `selene` command and a desktop icon.
#define SELENE_W 720
#define SELENE_H 520

void* selene_create_ctx(void);
void  selene_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void  selene_win_key(window_t* win, int key);
void  selene_win_click(window_t* win, int mx, int my, int btn);  // follow a link / Back / focus URL
int   selene_win_tick(window_t* win);     // ~30fps: cooperatively load one visible image per frame
void  selene_first_load(window_t* win);   // render the local start page without a network wait
void  selene_win_close(window_t* win);    // free every tab before the compositor frees its manager
int   selene_start_selftest(void);       // local startup + tab cleanup, no network

// HTML-forms self-test: parse a known form + build its GET submission URL. 0 if all pass.
int   selene_form_selftest(void);

// Back/Forward history-stack self-test (no network): 0 if all pass.
int   selene_nav_selftest(void);

#endif
