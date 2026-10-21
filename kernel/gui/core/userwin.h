#ifndef USERWIN_H
#define USERWIN_H

#include "../../core/kernel.h"

/*
 * User-space windows — the kernel half of syscalls 57-60 (win_create / destroy /
 * present / poll_event), issue #77. A ring-3 program creates a window, presents
 * an XRGB u32 client-area buffer, and polls input events out of a fixed per-window
 * ring. The compositor draws the window by blitting the last-presented buffer;
 * key / click / move arrive through the window's on_* callbacks and are queued here.
 *
 * The syscall layer (syscall.c) owns ALL user-VA validation and copying: every
 * function here takes KERNEL pointers only, which keeps the whole model unit-
 * testable without a ring-3 process (see uwin_selftest).
 *
 * Damage rects arrived in v6.5.56 (uwin_present_rect — update a sub-rectangle instead of
 * the whole client area). Still v1 non-goals: no shared mappings (present copies), no
 * callbacks into user code, no vsync.
 */

#define USERWIN_MAX      8       /* concurrent user windows */
#define USERWIN_EVQ_LEN  32      /* per-window event ring; oldest dropped when full */
#define USERWIN_MAX_W    2048
#define USERWIN_MAX_H    2048

/* Event kinds — the proposal's four-i64 record {kind, a, b, c}. */
enum {
    UWE_KEY    = 1,   /* a = keycode */
    UWE_CLICK  = 2,   /* a = x, b = y, c = button   (client-relative) */
    UWE_MOVE   = 3,   /* a = x, b = y, c = buttons  (client-relative) */
    UWE_CLOSE  = 4,   /* the window's close box was pressed */
    UWE_RESIZE = 5,   /* a = w, b = h */
};

typedef struct { int64_t kind, a, b, c; } uwin_event_t;

/* Pure event ring — head + count over a fixed slab; KAT-tested in isolation. */
typedef struct {
    uwin_event_t ev[USERWIN_EVQ_LEN];
    int head, count;
} uwin_evq_t;

void uwin_evq_init(uwin_evq_t* q);
void uwin_evq_push(uwin_evq_t* q, int64_t kind, int64_t a, int64_t b, int64_t c);
int  uwin_evq_pop(uwin_evq_t* q, uwin_event_t* out);   /* 1 = got one, 0 = empty */

/* Syscall entry points — KERNEL pointers only (syscall.c marshals user VAs).
 * IDs are opaque nonnegative int handles, never reused within a boot. Exhausting
 * the handle space makes create fail; IDs are not ownership capabilities. */
int64_t uwin_create(uint32_t w, uint32_t h, const char* title); /* id (>=0) or -1  */
int64_t uwin_destroy(int id);                                   /* 0 (idempotent)   */
/* Allocation-free preflight for syscall scratch growth. Present checks again;
 * these predicates do not pin a window or establish concurrent ownership. */
int uwin_can_present(int id, uint32_t w, uint32_t h);
int uwin_can_present_rect(int id, int rx, int ry, int rw, int rh);
int64_t uwin_present(int id, const uint32_t* px, uint32_t w, uint32_t h); /* 0 / -1 */
int64_t uwin_present_rect(int id, const uint32_t* px, int rx, int ry, int rw, int rh); /* dirty-rect update; 0 / -1 */
int64_t uwin_poll_event(int id, uwin_event_t* out);             /* 1 / 0 / -1       */
int64_t uwin_set_title(int id, const char* title);             /* retitle the bound window; 0 / -1 */
int64_t uwin_resize(int id, uint32_t w, uint32_t h);           /* app-initiated client resize; drops backing; 0 / -1 */

int uwin_selftest(void);   /* KAT */

#endif
