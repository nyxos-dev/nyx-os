#include "userwin.h"
#include "compositor.h"
#include "gfx_prims.h"

/* ---------------------------------------------------------------------------
 * Pure event ring: push at (head+count), pop from head, oldest dropped on full.
 * ------------------------------------------------------------------------- */

void uwin_evq_init(uwin_evq_t* q) { q->head = 0; q->count = 0; }

void uwin_evq_push(uwin_evq_t* q, int64_t kind, int64_t a, int64_t b, int64_t c) {
    if (q->count == USERWIN_EVQ_LEN) {                 /* full: drop the oldest */
        q->head = (q->head + 1) % USERWIN_EVQ_LEN;
        q->count--;
    }
    int slot = (q->head + q->count) % USERWIN_EVQ_LEN;
    q->ev[slot].kind = kind;
    q->ev[slot].a = a;
    q->ev[slot].b = b;
    q->ev[slot].c = c;
    q->count++;
}

int uwin_evq_pop(uwin_evq_t* q, uwin_event_t* out) {
    if (q->count == 0) return 0;
    *out = q->ev[q->head];
    q->head = (q->head + 1) % USERWIN_EVQ_LEN;
    q->count--;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Window registry. Each slot owns its backing buffer and event ring; the
 * compositor window (if any) points back here through win->reserved.
 * ------------------------------------------------------------------------- */

typedef struct {
    int        active;
    int        handle;   /* opaque per-boot identity, never reused */
    int        win_id;    /* compositor window id, or -1 (headless / not bound) */
    uint32_t   w, h;      /* client-area dimensions */
    uint32_t*  backing;   /* w*h XRGB, last presented; NULL until first present */
    uwin_evq_t evq;
} user_win_t;

static user_win_t g_uwin[USERWIN_MAX];
#define USERWIN_HANDLE_LIMIT 0x7fffffffU
static uint32_t g_next_uwin_handle;

static user_win_t* uwin_slot(int id) {
    if (id < 0) return 0;
    for (int i = 0; i < USERWIN_MAX; i++)
        if (g_uwin[i].active && g_uwin[i].handle == id) return &g_uwin[i];
    return 0;
}

/* Allocate + initialise a registry slot (no compositor window). Shared by
 * uwin_create and the KAT so the model is testable without a running desktop. */
static int uwin_alloc_slot(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > USERWIN_MAX_W || h > USERWIN_MAX_H) return -1;
    /* Fail closed at exhaustion rather than wrapping into an old identity. */
    if (g_next_uwin_handle > USERWIN_HANDLE_LIMIT) return -1;
    for (int i = 0; i < USERWIN_MAX; i++) {
        if (!g_uwin[i].active) {
            user_win_t* s = &g_uwin[i];
            s->w = w; s->h = h; s->backing = 0; s->win_id = -1;
            uwin_evq_init(&s->evq);
            s->handle = (int)g_next_uwin_handle++;
            s->active = 1;
            return s->handle;
        }
    }
    return -1;                                          /* table full */
}

/* Release a slot's resources exactly once — re-entrant-safe so the compositor's
 * on_close (fired during window_destroy) and an app-initiated uwin_destroy can
 * both run without a double free. */
static void uwin_release(user_win_t* s) {
    if (!s || !s->active) return;
    s->active = 0;                                      /* mark dead FIRST */
    if (s->backing) { kfree(s->backing); s->backing = 0; }
    s->win_id = -1;
}

/* ---- compositor callbacks ---- */

static void uwin_draw_cb(window_t* win, int clip_x, int clip_y, uint32_t clip_w, uint32_t clip_h) {
    (void)clip_x; (void)clip_y; (void)clip_w; (void)clip_h;
    user_win_t* s = (user_win_t*)win->reserved;
    if (!s || !s->active) return;
    if (s->backing)
        fb_blit(s->backing, 0, 0, s->w, s->h, WIN_CLIENT_X(win), WIN_CLIENT_Y(win));
    else                                                /* not presented yet */
        fb_fill_rect(WIN_CLIENT_X(win), WIN_CLIENT_Y(win), s->w, s->h, 0x00101018);
}

static void uwin_on_key(window_t* win, int key) {
    user_win_t* s = (user_win_t*)win->reserved;
    if (s && s->active) uwin_evq_push(&s->evq, UWE_KEY, key, 0, 0);
}

static void uwin_on_click(window_t* win, int mx, int my, int btn) {
    user_win_t* s = (user_win_t*)win->reserved;
    if (s && s->active)
        uwin_evq_push(&s->evq, UWE_CLICK, mx - WIN_CLIENT_X(win), my - WIN_CLIENT_Y(win), btn);
}

static void uwin_on_move(window_t* win, int mx, int my, int btns) {
    user_win_t* s = (user_win_t*)win->reserved;
    if (s && s->active)
        uwin_evq_push(&s->evq, UWE_MOVE, mx - WIN_CLIENT_X(win), my - WIN_CLIENT_Y(win), btns);
}

// The compositor fires this once when a USER drag-resize of this window's frame is released,
// with the new CLIENT dimensions. Adopt them into the registry, DROP the (now wrong-size) backing
// so the next present reallocs + uwin_present's size check demands the new size, and queue a
// UWE_RESIZE so the client re-presents at the new size. (Mirrors uwin_resize's app-initiated path.)
static void uwin_on_resize(window_t* win, int w, int h) {
    user_win_t* s = (user_win_t*)win->reserved;
    if (!s || !s->active || w <= 0 || h <= 0) return;
    if ((uint32_t)w == s->w && (uint32_t)h == s->h) return;       // no change
    s->w = (uint32_t)w; s->h = (uint32_t)h;
    if (s->backing) { kfree(s->backing); s->backing = 0; }
    uwin_evq_push(&s->evq, UWE_RESIZE, w, h, 0);
}

static void uwin_on_close_cb(window_t* win) {
    /* window_destroy calls this before freeing win. If the app initiated the
     * teardown, uwin_destroy already released the slot (no-op here); if the
     * compositor's close box fired it, this is where the slot is released and the
     * app learns of it via poll_event returning -1 (its window no longer exists). */
    uwin_release((user_win_t*)win->reserved);
}

/* ---- syscall entry points ---- */

int64_t uwin_create(uint32_t w, uint32_t h, const char* title) {
    int id = uwin_alloc_slot(w, h);
    if (id < 0) return -1;

    /* Text-mode boots have no drawable desktop; keep the headless registry slot. */
    if (!fb_get_width() || !fb_get_height()) return id;

    /* Bind a compositor window whose CLIENT area is w x h. window_create's h param IS the
     * client height (win_total_h adds TITLE_H for the title bar on top), so pass h directly —
     * NOT h + TITLE_H, which made the frame one title-bar too tall and left a TITLE_H strip of
     * bare body fill below the app's w x h backing. In-kernel windows pass the client height too. */
    user_win_t* s = uwin_slot(id);
    int slot_index = (int)(s - g_uwin);
    int px = 80 + slot_index * 24, py = 80 + slot_index * 24;
    window_t* win = window_create(px, py, w, h, title ? title : "app", uwin_draw_cb);
    if (win) {
        win->reserved     = s;
        win->on_key       = uwin_on_key;
        win->on_click     = uwin_on_click;
        win->on_mousemove = uwin_on_move;
        win->on_resize    = uwin_on_resize;
        win->on_close     = uwin_on_close_cb;
        s->win_id = win->id;
    }
    /* No compositor window (desktop not up / table full) still yields a usable
     * headless slot: present/poll work, it simply isn't drawn. */
    return id;
}

int64_t uwin_destroy(int id) {
    user_win_t* s = uwin_slot(id);
    if (!s) return 0;                                   /* idempotent */
    int win_id = s->win_id;
    uwin_release(s);                                    /* free backing, mark dead */
    if (win_id >= 0) window_destroy(win_id);            /* fires on_close_cb -> no-op */
    return 0;
}

int64_t uwin_present(int id, const uint32_t* px, uint32_t w, uint32_t h) {
    user_win_t* s = uwin_slot(id);
    if (!s || !px) return -1;
    if (w != s->w || h != s->h) return -1;              /* whole client-area blit only */
    if (!s->backing) {
        s->backing = (uint32_t*)kmalloc((size_t)s->w * s->h * 4);
        if (!s->backing) return -1;
    }
    uint32_t n = s->w * s->h;
    for (uint32_t i = 0; i < n; i++) s->backing[i] = px[i];
    return 0;
}

/* Damage-rect present: copy a rw*rh XRGB buffer into the sub-rectangle at (rx,ry) of the
 * client area, leaving the rest of the backing untouched — so a client can update just what
 * changed instead of re-sending the whole window. The rect must lie fully inside the client
 * area. If nothing has been presented yet the backing is allocated and cleared first. */
int64_t uwin_present_rect(int id, const uint32_t* px, int rx, int ry, int rw, int rh) {
    user_win_t* s = uwin_slot(id);
    if (!s || !px || rw <= 0 || rh <= 0) return -1;
    if (rx < 0 || ry < 0 ||
        (uint32_t)(rx + rw) > s->w || (uint32_t)(ry + rh) > s->h) return -1;   /* out of bounds */
    if (!s->backing) {
        s->backing = (uint32_t*)kmalloc((size_t)s->w * s->h * 4);
        if (!s->backing) return -1;
        for (uint32_t i = 0; i < s->w * s->h; i++) s->backing[i] = 0;           /* start opaque black */
    }
    for (int row = 0; row < rh; row++)
        for (int col = 0; col < rw; col++)
            s->backing[(uint32_t)(ry + row) * s->w + (uint32_t)(rx + col)] = px[row * rw + col];
    return 0;
}

int64_t uwin_poll_event(int id, uwin_event_t* out) {
    user_win_t* s = uwin_slot(id);
    if (!s || !out) return -1;
    return uwin_evq_pop(&s->evq, out) ? 1 : 0;
}

/* Retitle the compositor window bound to this slot, so a client can reflect state
 * (edited file, current URL, cwd) after creation. A headless slot (no desktop, or the
 * window table was full at create time) has no title bar: report success (0) so the
 * client need not special-case it. A bad/dead id is -1. */
int64_t uwin_set_title(int id, const char* title) {
    user_win_t* s = uwin_slot(id);
    if (!s) return -1;
    if (s->win_id >= 0) return window_set_title(s->win_id, title);
    return 0;                                           /* headless: nothing to draw */
}

/* App-initiated resize of the client area to w x h. The backing is dropped (the old-size
 * buffer no longer matches), so uwin_present's size check now demands the NEW dimensions and
 * the client must present a fresh w x h buffer. The bound compositor window's frame is resized
 * the same way uwin_create maps it (client h + TITLE_H). A no-op (same size) succeeds without
 * touching the backing. Bad id / zero / oversize -> -1. */
int64_t uwin_resize(int id, uint32_t w, uint32_t h) {
    user_win_t* s = uwin_slot(id);
    if (!s) return -1;
    if (w == 0 || h == 0 || w > USERWIN_MAX_W || h > USERWIN_MAX_H) return -1;
    if (w == s->w && h == s->h) return 0;               /* no-op: keep the backing */
    s->w = w; s->h = h;
    if (s->backing) { kfree(s->backing); s->backing = 0; }  /* next present reallocs at new size */
    if (s->win_id >= 0) window_resize(s->win_id, w, h);  /* client height; window_resize/win_total_h add TITLE_H */
    return 0;
}

/* ---------------------------------------------------------------------------
 * KAT — exercises the event ring, the registry (alloc / present / poll / release /
 * reuse / exhaustion) with no compositor window, entirely on kernel buffers.
 * ------------------------------------------------------------------------- */

int uwin_selftest(void) {
    uwin_event_t e;

    /* 1. event ring: empty, FIFO order, overflow drops oldest. (0 = PASS.) */
    uwin_evq_t q; uwin_evq_init(&q);
    if (uwin_evq_pop(&q, &e) != 0) return 1;
    for (int i = 0; i < 3; i++) uwin_evq_push(&q, UWE_KEY, i, 0, 0);
    if (uwin_evq_pop(&q, &e) != 1 || e.a != 0) return 2;
    if (uwin_evq_pop(&q, &e) != 1 || e.a != 1) return 3;
    uwin_evq_init(&q);
    for (int i = 0; i < USERWIN_EVQ_LEN + 5; i++) uwin_evq_push(&q, UWE_KEY, i, 0, 0);
    if (q.count != USERWIN_EVQ_LEN) return 4;            /* capped */
    if (uwin_evq_pop(&q, &e) != 1 || e.a != 5) return 5; /* oldest 5 dropped */

    /* 2. registry: alloc, present (size-checked), event round-trip, release. */
    int id = uwin_alloc_slot(64, 48);
    if (id < 0 || !uwin_slot(id)) return 6;
    static uint32_t buf[64 * 48];
    for (int i = 0; i < 64 * 48; i++) buf[i] = 0x00334455;
    if (uwin_present(id, buf, 64, 48) != 0) return 7;
    if (uwin_present(id, buf, 64, 47) != -1) return 8;  /* wrong size rejected */
    if (!uwin_slot(id)->backing || uwin_slot(id)->backing[10] != 0x00334455) return 9;
    uwin_evq_push(&uwin_slot(id)->evq, UWE_CLICK, 3, 4, 1);
    if (uwin_poll_event(id, &e) != 1 || e.kind != UWE_CLICK || e.a != 3 || e.c != 1) return 10;
    if (uwin_poll_event(id, &e) != 0) return 11;        /* drained */
    uwin_release(uwin_slot(id));
    if (uwin_slot(id)) return 12;                       /* now dead */
    if (uwin_present(id, buf, 64, 48) != -1) return 13; /* dead slot rejects */
    if (uwin_poll_event(id, &e) != -1) return 14;
    if (uwin_destroy(id) != 0) return 15;               /* idempotent on a free slot */

    /* 3. exhaustion + freed-id reuse. */
    int ids[USERWIN_MAX];
    for (int i = 0; i < USERWIN_MAX; i++) { ids[i] = uwin_alloc_slot(8, 8); if (ids[i] < 0) return 16; }
    if (uwin_alloc_slot(8, 8) != -1) return 17;         /* table full */
    uwin_release(uwin_slot(ids[3]));
    int replacement = uwin_alloc_slot(8, 8);
    if (replacement < 0 || replacement == ids[3] || uwin_slot(ids[3])) return 18;
    for (int i = 0; i < USERWIN_MAX; i++) uwin_release(&g_uwin[i]);

    /* 4. partial present: a sub-rect lands exactly, neighbours are untouched, and an
     * out-of-bounds rect / bad id is rejected. */
    int pid = uwin_alloc_slot(16, 12);
    if (pid < 0) return 19;
    static uint32_t pbuf[16 * 12];
    for (int i = 0; i < 16 * 12; i++) pbuf[i] = 0x00112233;
    if (uwin_present(pid, pbuf, 16, 12) != 0) return 20;         /* fill backing */
    static uint32_t r2[2 * 2];
    for (int i = 0; i < 4; i++) r2[i] = 0x00AABBCC;
    if (uwin_present_rect(pid, r2, 3, 4, 2, 2) != 0) return 21;  /* 2x2 at (x=3,y=4) */
    user_win_t* ps = uwin_slot(pid);
    if (!ps || !ps->backing) return 22;
    if (ps->backing[4 * 16 + 3] != 0x00AABBCC) return 23;        /* (3,4) updated */
    if (ps->backing[5 * 16 + 4] != 0x00AABBCC) return 24;        /* (4,5) updated */
    if (ps->backing[0]          != 0x00112233) return 25;        /* corner untouched */
    if (ps->backing[4 * 16 + 2] != 0x00112233) return 26;        /* just left of rect untouched */
    if (uwin_present_rect(pid, r2, 15, 4, 2, 2) != -1) return 27; /* x+w=17 > 16 -> reject */
    if (uwin_present_rect(pid, r2, 3, 11, 2, 2) != -1) return 28; /* y+h=13 > 12 -> reject */
    if (uwin_present_rect(-1, r2, 0, 0, 2, 2) != -1) return 29; /* bad id -> reject */
    if (ps->backing[4 * 16 + 3] != 0x00AABBCC) return 30;        /* rejects left the backing intact */

    /* 5. set_title: a headless slot (win_id == -1, no compositor window) is a no-op
     * success so a client need not special-case the desktop-down path; a bad id is -1. */
    if (uwin_set_title(pid, "retitled") != 0) return 31;         /* headless slot -> 0 */
    if (uwin_set_title(-1, "x")       != -1) return 32;        /* bad id -> -1 */
    uwin_release(ps);

    /* 6. resize: client dims update and the backing is DROPPED (so the next present reallocs
     * at the new size), a no-op resize keeps the backing, and bad dims / bad id are rejected. */
    int rid = uwin_alloc_slot(20, 10);
    if (rid < 0) return 33;
    static uint32_t rbuf0[20 * 10];
    for (int i = 0; i < 20 * 10; i++) rbuf0[i] = 0x00445566;
    if (uwin_present(rid, rbuf0, 20, 10) != 0) return 34;        /* backing at 20x10 */
    if (uwin_resize(rid, 32, 24) != 0) return 35;                /* grow */
    user_win_t* rs = uwin_slot(rid);
    if (!rs || rs->w != 32 || rs->h != 24 || rs->backing != 0) return 36;  /* dims updated, backing dropped */
    static uint32_t rbuf1[32 * 24];
    for (int i = 0; i < 32 * 24; i++) rbuf1[i] = 0x00778899;
    if (uwin_present(rid, rbuf1, 32, 24) != 0) return 37;        /* present at the NEW size works */
    if (uwin_resize(rid, 32, 24) != 0) return 38;                /* no-op resize -> 0 */
    if (uwin_slot(rid)->backing == 0) return 39;                 /* ...and it kept the backing */
    if (uwin_resize(rid, 0, 10) != -1) return 40;                /* zero dim rejected */
    if (uwin_resize(-1, 8, 8) != -1) return 41;                /* bad id rejected */

    /* 7. compositor-driven resize notify (a USER drag-resize): uwin_on_resize adopts the new
     * client dims, drops the backing, and queues UWE_RESIZE(w,h). Staged window bound to the slot
     * (static -> zero-initialised; only `reserved` matters to the callback). */
    static window_t fkw;                          /* zero-init; reserved set below */
    fkw.reserved = uwin_slot(rid);
    uwin_on_resize(&fkw, 48, 36);
    if (uwin_slot(rid)->w != 48 || uwin_slot(rid)->h != 36) return 42;   /* adopted new client dims */
    if (uwin_slot(rid)->backing != 0) return 43;                          /* backing dropped */
    uwin_event_t re;
    if (uwin_poll_event(rid, &re) != 1 || re.kind != UWE_RESIZE || re.a != 48 || re.b != 36) return 44;
    uwin_on_resize(&fkw, 48, 36);                                         /* same size -> no-op */
    if (uwin_poll_event(rid, &re) != 0) return 45;                        /* ...queues nothing */
    uwin_release(uwin_slot(rid));
    return 0;
}
