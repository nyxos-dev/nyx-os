#include "../../core/kernel.h"

static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_bpp = 0;
// Hardware row stride in PIXELS. Equals fb_width for the QEMU Bochs LFB (pitch = width*4), but a
// bootloader/GOP framebuffer on real hardware can have a PADDED pitch (pitch > width*4), so the
// blit to fb_hw must step rows by this, not by fb_width. Internal drawing + the back buffer stay
// width-contiguous; only the fb_hw boundary (fb_present, the debug banner) uses fb_hw_stride.
static uint32_t fb_hw_stride = 0;
// fb_addr is the CURRENT draw target that every fb_* primitive (and the
// compositor's cursor code, via fb_get_addr) writes to. By default it is the
// hardware LFB (fb_hw) so early, one-shot rendering — bootsplash, the login
// screen — paints straight to the screen with no need to "present". Once the
// compositor calls fb_enable_backbuffer(), fb_addr is redirected to an
// off-screen buffer (fb_back) and the compositor blits a whole finished frame
// to fb_hw in one shot via fb_present() — this is what kills the flicker (the
// user never sees the half-drawn background between elements).
static void* fb_addr = NULL;
static void* fb_hw   = NULL;   // real hardware linear framebuffer
static void* fb_back = NULL;   // off-screen back buffer (NULL = draw direct)
static int   fb_back_wanted = 0;   // re-establish the back buffer after a mode change

int  fb_enable_backbuffer(void);   // fwd

// Display rotation (0/90/180/270 degrees clockwise), for panels physically mounted
// rotated — e.g. a portrait 8" UMPC panel that scans out a landscape LFB, so an
// unrotated desktop appears sideways. When nonzero, the WHOLE GUI renders into fb_back
// at the swapped logical size (fb_width/fb_height) and fb_present() is the single point
// that rotates it onto the hardware LFB (fb_hw_w x fb_hw_h). Enabled via the "rotate=" boot
// cmdline. Default 0 → every path below stays byte-identical to an unrotated build.
static int      fb_rot  = 0;
static uint32_t fb_hw_w = 0;   // hardware framebuffer dims (== fb_width/height when unrotated)
static uint32_t fb_hw_h = 0;

void fb_set_rotation(int deg) {
    int d = ((deg % 360) + 360) % 360;
    fb_rot = (d / 90) * 90;    // snap to 0/90/180/270
}

// Fullscreen userspace ownership (v5.9.29 — the DOOM-milestone graphics enabler).
// A program that calls SYS_FBPRESENT is driving the whole screen; while it keeps
// presenting, the compositor's fb_present() yields so the desktop doesn't overwrite
// the app's frames. Self-releasing: fb_fs_last is the tick of the last present, and
// ownership lapses FB_FS_TIMEOUT_MS after the app stops presenting (i.e. exits), at
// which point the desktop returns — no reap hook or explicit release needed.
#define FB_FS_TIMEOUT_MS 500
static volatile uint32_t fb_fs_last = 0;
int fb_fullscreen_active(void) {
    return fb_fs_last != 0 && (get_ticks() - fb_fs_last) < FB_FS_TIMEOUT_MS;
}

void fb_init(uint32_t width, uint32_t height, uint32_t bpp, void* addr) {
    fb_hw_w = width;                        // remember the true hardware dimensions
    fb_hw_h = height;
    fb_bpp = bpp;
    fb_hw_stride = width;                   // Bochs LFB: pitch == width*4 (no padding)
    fb_hw = addr;
    if (fb_rot == 90 || fb_rot == 270) {    // logical screen = the panel's upright (swapped) size
        fb_width = height; fb_height = width;
    } else {
        fb_width = width;  fb_height = height;
    }
    // A mode change invalidates any back buffer sized for the old resolution.
    if (fb_back) { kfree(fb_back); fb_back = NULL; }
    if (fb_rot != 0) {
        // Rotation REQUIRES the back buffer: all drawing goes to logical-space fb_back and
        // fb_present() rotates it. Drawing straight to fb_hw would be unrotated wrong-stride
        // garbage, so force it on (and keep it on — fb_use_lfb_direct becomes a no-op).
        fb_back_wanted = 1;
        fb_addr = addr;
        fb_enable_backbuffer();             // repoints fb_addr -> fb_back
    } else {
        fb_addr = addr;                     // default: draw straight to the LFB
        if (fb_back_wanted) fb_enable_backbuffer();   // resize the back buffer to match
    }
}

// Like fb_init, but for a bootloader/GOP framebuffer whose hardware row stride (pitch) may be
// wider than width*4 — the real-hardware / UEFI path. `stride_px` = pitch / 4.
void fb_init_ex(uint32_t width, uint32_t height, uint32_t bpp, void* addr, uint32_t stride_px) {
    fb_init(width, height, bpp, addr);
    fb_hw_stride = stride_px ? stride_px : width;
}

// Early "we have the framebuffer" signal, painted STRAIGHT to the hardware LFB before any
// compositor exists: a Nyx-purple bar with a white border across the top. On a real machine it
// proves graphics came up the instant they do — and because it steps rows by fb_hw_stride, a
// WRONG pitch shows as a visible diagonal/skew, which is itself the diagnostic.
void fb_debug_banner(void) {
    if (!fb_hw) return;
    uint32_t W = fb_hw_w ? fb_hw_w : fb_width;    // hardware space (== fb_width/height unrotated)
    uint32_t H = fb_hw_h ? fb_hw_h : fb_height;
    uint32_t bar = H < 48 ? H : 48;
    for (uint32_t y = 0; y < bar; y++) {
        uint32_t* row = (uint32_t*)fb_hw + (size_t)y * fb_hw_stride;
        uint32_t col = (y < 6 || y >= bar - 6) ? 0x00FFFFFF : 0x00825AD2;  // white border / Nyx purple
        for (uint32_t x = 0; x < W; x++) row[x] = col;
    }
}

// Turn on double buffering: allocate a back buffer the size of the framebuffer
// and redirect all drawing to it. Returns 1 on success (or if already on), 0 if
// it couldn't allocate / the mode isn't a 32bpp LFB (callers then just keep
// drawing direct). After this, drawing is invisible until fb_present().
int fb_enable_backbuffer(void) {
    fb_back_wanted = 1;
    if (fb_back) {
        // Re-arming with a buffer that already exists. This used to `return 1`
        // and nothing else — leaving fb_addr wherever it happened to point.
        // After a logout that is the HARDWARE framebuffer (fb_use_lfb_direct
        // sent the login screen there), so the second desktop of a session
        // drew every element straight onto the visible screen — the flicker
        // double buffering exists to prevent — while fb_present kept blitting
        // this buffer, still holding the PREVIOUS user's frozen desktop, on
        // top of it every frame. Repoint, and clear it: nothing from the
        // session that just logged out may survive into the next one.
        fb_addr = fb_back;
        memset_asm(fb_back, 0, (size_t)fb_width * fb_height * 4);
        return 1;
    }
    if (!fb_hw || fb_bpp != 32) return 0;
    size_t bytes = (size_t)fb_width * fb_height * 4;
    void* buf = kmalloc(bytes);
    if (!buf) { printf("[FB] back buffer alloc failed (%u KB) — direct rendering\n",
                        (uint32_t)(bytes / 1024)); return 0; }
    fb_back = buf;
    fb_addr = fb_back;
    printf("[FB] double buffering on — back buffer %ux%u (%u KB)\n",
           fb_width, fb_height, (uint32_t)(bytes / 1024));
    return 1;
}

// Blit the finished back buffer to the hardware framebuffer in one pass. No-op
// when double buffering isn't enabled (drawing already went straight to the LFB).
void fb_present(void) {
    if (fb_fullscreen_active()) return;   // a userspace app owns the screen — don't clobber it
    if (!fb_back || !fb_hw) return;
    // Publishing only makes sense while drawing is actually GOING to the back
    // buffer. Once fb_use_lfb_direct() has repointed drawing at the hardware
    // (logout, panic), the back buffer holds nothing but a stale frame from
    // before — and a present from a straggling caller would paint it over the
    // login screen or the panic report. Tie the publish to the invariant that
    // makes it meaningful rather than trusting every caller to know.
    if (fb_addr != fb_back) return;
    if (fb_rot == 0) {
        if (fb_hw_stride == fb_width) {
            memcpy_asm(fb_hw, fb_back, (size_t)fb_width * fb_height * 4);   // contiguous fast path
        } else {
            // padded hardware rows (GOP pitch > width): publish row by row
            for (uint32_t y = 0; y < fb_height; y++)
                memcpy_asm((uint8_t*)fb_hw + (size_t)y * fb_hw_stride * 4,
                           (uint32_t*)fb_back + (size_t)y * fb_width, (size_t)fb_width * 4);
        }
        return;
    }
    // Rotated publish: scatter each logical back-buffer pixel to its hardware position.
    // fb_back is fb_width x fb_height (logical/upright); fb_hw is fb_hw_w x fb_hw_h (stride
    // fb_hw_stride). 90 = clockwise, 270 = counter-clockwise, 180 = flip.
    const uint32_t* src = (const uint32_t*)fb_back;
    uint32_t* hw = (uint32_t*)fb_hw;
    for (uint32_t ly = 0; ly < fb_height; ly++) {
        const uint32_t* srow = src + (size_t)ly * fb_width;
        for (uint32_t lx = 0; lx < fb_width; lx++) {
            uint32_t hx, hy;
            if (fb_rot == 90)       { hx = fb_hw_w - 1 - ly; hy = lx; }
            else if (fb_rot == 270) { hx = ly;               hy = fb_hw_h - 1 - lx; }
            else /* 180 */          { hx = fb_hw_w - 1 - lx; hy = fb_hw_h - 1 - ly; }
            hw[(size_t)hy * fb_hw_stride + hx] = srow[lx];
        }
    }
}

// SYS_FBINFO: hand the screen geometry to a userspace program so it can size its
// render buffer (DOOM's DG_Init reads this).
void fb_query(uint32_t* w, uint32_t* h, uint32_t* bpp) {
    if (w)   *w   = fb_width;
    if (h)   *h   = fb_height;
    if (bpp) *bpp = fb_bpp;
}

// Publish ONLY a sub-rectangle of the back buffer to hardware. The compositor uses this to
// move the cursor without blitting the whole ~3 MB screen every pointer step (a full
// fb_present was the single biggest per-frame cost). Clamps to the framebuffer; the rare
// rotated path falls back to a full present (correctness over speed).
void fb_present_rect(int x, int y, int w, int h) {
    if (fb_fullscreen_active()) return;
    if (!fb_back || !fb_hw) return;
    if (fb_addr != fb_back) return;
    if (fb_rot != 0) { fb_present(); return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0 || x >= (int)fb_width || y >= (int)fb_height) return;
    if (x + w > (int)fb_width)  w = (int)fb_width  - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    for (int row = 0; row < h; row++) {
        uint32_t fy = (uint32_t)(y + row);
        memcpy_asm((uint8_t*)fb_hw + ((size_t)fy * fb_hw_stride + (uint32_t)x) * 4,
                   (const uint32_t*)fb_back + (size_t)fy * fb_width + (uint32_t)x,
                   (size_t)w * 4);
    }
}

// SYS_FBPRESENT: blit a 32bpp source buffer (sw x sh) straight to the hardware
// framebuffer, nearest-neighbour scaled to the full screen, and take fullscreen
// ownership so the compositor yields. `src` is a KERNEL buffer — the syscall handler
// copies the user buffer in first (validated) so this never dereferences user VAs.
// Nearest-neighbour keeps it a single integer-only pass with no per-pixel division
// in the inner loop (the source x/y steps are precomputed per column/row could be,
// but the div is cheap enough here and keeps it obviously correct).
void fb_present_kbuf(const uint32_t* src, uint32_t sw, uint32_t sh) {
    if (!fb_hw || fb_bpp != 32 || !src || sw == 0 || sh == 0) return;
    uint32_t* dst = (uint32_t*)fb_hw;
    // Precompute the source column for every destination column ONCE (fb_width divides),
    // so the inner loop is a table lookup instead of a 64-bit divide PER PIXEL
    // (fb_width*fb_height divides — ~786K/frame at 1024x768). Big win for the fullscreen
    // present path (DOOM and any SYS_FBPRESENT app). Sized to the widest supported mode;
    // wider would just fall back to a per-pixel divide for the overflow columns.
    static uint32_t xmap[2048];
    uint32_t vw = fb_width <= 2048 ? fb_width : 2048;
    for (uint32_t x = 0; x < vw; x++) xmap[x] = (uint32_t)((uint64_t)x * sw / fb_width);
    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * sh / fb_height);
        const uint32_t* srow = src + (uint64_t)sy * sw;
        uint32_t* drow = dst + (uint64_t)y * fb_width;
        uint32_t x = 0;
        for (; x < vw; x++) drow[x] = srow[xmap[x]];
        for (; x < fb_width; x++) drow[x] = srow[(uint32_t)((uint64_t)x * sw / fb_width)]; // >2048 fallback
    }
    uint32_t t = get_ticks();
    fb_fs_last = t ? t : 1;                 // nonzero => fb_fullscreen_active() sees it
}

// Repoint all drawing straight at the hardware framebuffer. The panic screen
// uses this: the compositor's present loop is dead by the time we panic, so
// anything drawn to the back buffer would never be published — draw direct so
// it's on the visible screen immediately (no fb_present needed).
void fb_use_lfb_direct(void) {
    if (fb_rot != 0) return;   // rotated: keep drawing to fb_back; fb_present does the rotation
    if (fb_hw) fb_addr = fb_hw;
}

static int clip_pixel_ok(int x, int y);   // defined with the clip machinery below

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_addr || x >= fb_width || y >= fb_height) return;
    if (!clip_pixel_ok((int)x, (int)y)) return;   // honour the round/region clip (round-rect corners, stars)
    if (fb_bpp == 32) {
        ((uint32_t*)fb_addr)[y * fb_width + x] = color;
    } else if (fb_bpp == 24) {
        uint8_t* p = (uint8_t*)fb_addr + (y * fb_width + x) * 3;
        p[0] = color & 0xFF;
        p[1] = (color >> 8) & 0xFF;
        p[2] = (color >> 16) & 0xFF;
    } else if (fb_bpp == 8) {
        ((uint8_t*)fb_addr)[y * fb_width + x] = (uint8_t)color;
    }
}

// ---- Rounded-bottom clip region -------------------------------------------
// When enabled, fb_fill_rect / fb_blit mask their writes to a rectangle whose two
// BOTTOM corners are rounded to radius clip_r (the top stays square — a window's
// top is rounded by its title bar instead). The compositor turns it on around a
// window's body + app-content draws so that content can't square off the rounded
// bottom the frame traces. Off by default, and the clip-off path in each
// primitive is kept byte-identical to before.
static int clip_on = 0;
static int clip_x0, clip_y0, clip_x1, clip_y1, clip_r;

void fb_set_round_clip(int x, int y, int w, int h, int r) {
    clip_on = 1;
    clip_x0 = x; clip_y0 = y;
    clip_x1 = x + w - 1; clip_y1 = y + h - 1;
    clip_r = r;
}
void fb_clear_clip(void) { clip_on = 0; }

// A SECOND, "outer" rectangular clip that INTERSECTS the round clip above — the
// groundwork for dirty-rect region redraws (repaint the desktop composited but
// bounded to a damage rect). It composes with the per-window round clip rather
// than replacing it: set the region once, then the normal draw sequence (which
// sets/clears its own round clip per window) also stays inside the region. Only
// the primitives that honour clip_span (fb_fill_rect, fb_blit) respect it so far;
// extending the rest (fonts/darken/round-rect) is a later slice. NOT yet wired to
// any redraw path — v6.5.75 adds the mechanism + KAT in isolation.
static int region_on = 0;
static int region_x0, region_y0, region_x1, region_y1;

void fb_set_region_clip(int x, int y, int w, int h) {
    region_on = 1;
    region_x0 = x; region_y0 = y;
    region_x1 = x + w - 1; region_y1 = y + h - 1;
}
void fb_clear_region_clip(void) { region_on = 0; }

// Pure span math (no globals) so it can be unit-tested: the allowed horizontal
// span [*lo,*hi) for row `py`, given an optional round clip (rounded rect, arc
// radius `rr`) intersected with an optional region rect, all clamped to [0,fbw).
// Empty (lo>=hi) when the row is outside either clip or they don't overlap in x.
static void clip_compute_span(int py, int fbw,
        int round_on, int rx0, int ry0, int rx1, int ry1, int rr,
        int reg_on,   int gx0, int gy0, int gx1, int gy1,
        int* lo, int* hi) {
    int l = 0, r = fbw;                             // start with the whole row
    if (round_on) {
        if (py < ry0 || py > ry1) { *lo = *hi = 0; return; }
        int cl = rx0, cr = rx1 + 1;
        int from_bottom = ry1 - py;
        if (from_bottom < rr) {
            int inset = fb_corner_inset(from_bottom, rr);   // arc inset near the rounded bottom
            cl += inset; cr -= inset;
        }
        if (cl > l) l = cl;
        if (cr < r) r = cr;
    }
    if (reg_on) {
        if (py < gy0 || py > gy1) { *lo = *hi = 0; return; }
        if (gx0     > l) l = gx0;
        if (gx1 + 1 < r) r = gx1 + 1;
    }
    if (l < 0) l = 0;
    if (r > fbw) r = fbw;
    if (r < l) r = l;                               // empty (disjoint) -> lo==hi
    *lo = l; *hi = r;
}

// Allowed horizontal span [*lo,*hi) for row `py` under the active clip(s): the
// round clip and/or the region clip, whichever are on (see clip_compute_span).
static void clip_span(int py, int* lo, int* hi) {
    clip_compute_span(py, (int)fb_width,
                       clip_on, clip_x0, clip_y0, clip_x1, clip_y1, clip_r,
                       region_on, region_x0, region_y0, region_x1, region_y1,
                       lo, hi);
}

// Is pixel (x,y) inside the active clip(s)? Used by single-pixel writers (fb_put_pixel)
// so they respect the round/region clip like the rect fillers do. No clip active -> always ok.
static int clip_pixel_ok(int x, int y) {
    if (!clip_on && !region_on) return 1;
    int lo, hi; clip_span(y, &lo, &hi);
    return x >= lo && x < hi;
}

// Public clip probes for direct-write blitters (font glyphs) that bypass fb_put_pixel for
// speed: check fb_clip_active() ONCE per glyph and, only when a clip is up, gate each pixel
// with fb_pixel_visible(). With no clip the caller keeps its tight direct-write loop.
int fb_clip_active(void)          { return clip_on || region_on; }
int fb_pixel_visible(int x, int y) { return clip_pixel_ok(x, y); }

// KAT: the pure region/round span intersection. rr=0 keeps the round clip a plain
// rect (no arc) so the geometry is exact. 0 = pass, else the failing case number.
int region_clip_selftest(void) {
    int lo, hi;
    // 1. region only, row inside -> [gx0, gx1+1], clamped to fbw.
    clip_compute_span(50, 200, 0,0,0,0,0,0, 1, 10,40,60,80, &lo,&hi);
    if (lo != 10 || hi != 61) return 1;
    // 2. region only, row ABOVE the region -> empty.
    clip_compute_span(10, 200, 0,0,0,0,0,0, 1, 10,40,60,80, &lo,&hi);
    if (lo != hi) return 2;
    // 3. round only (rr=0), row inside -> [rx0, rx1+1].
    clip_compute_span(50, 200, 1, 5,40,100,80,0, 0,0,0,0,0, &lo,&hi);
    if (lo != 5 || hi != 101) return 3;
    // 4. BOTH: intersection of x-ranges (round [5,100] ∩ region [10,60]) -> [10,61).
    clip_compute_span(50, 200, 1, 5,40,100,80,0, 1, 10,40,60,80, &lo,&hi);
    if (lo != 10 || hi != 61) return 4;
    // 5. BOTH but x-disjoint (round right of region) -> empty.
    clip_compute_span(50, 200, 1, 120,40,150,80,0, 1, 10,40,60,80, &lo,&hi);
    if (lo != hi) return 5;
    // 6. BOTH, row inside round but OUTSIDE region -> empty.
    clip_compute_span(90, 200, 1, 5,40,100,120,0, 1, 10,40,60,80, &lo,&hi);
    if (lo != hi) return 6;
    // 7. region wider than the screen -> clamped to [0, fbw).
    clip_compute_span(50, 200, 0,0,0,0,0,0, 1, -20,40,999,80, &lo,&hi);
    if (lo != 0 || hi != 200) return 7;
    return 0;
}

// KAT: memset32_asm fills EXACTLY `count` 32bpp pixels — no overrun/underrun, and
// count==0 is a no-op. This is the fb fill/blit fast-path primitive (rep stosd), so
// a stray write past the run would corrupt whatever sits after it. 0 = pass.
int memset32_selftest(void) {
    static uint32_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0xA5A5A5A5u;          // sentinel guards
    memset32_asm(&buf[8], 0x11223344u, 32);                     // fill [8,40)
    for (int i = 0;  i < 8;  i++) if (buf[i] != 0xA5A5A5A5u) return 1;   // underrun
    for (int i = 8;  i < 40; i++) if (buf[i] != 0x11223344u) return 2;   // body
    for (int i = 40; i < 64; i++) if (buf[i] != 0xA5A5A5A5u) return 3;   // overrun
    memset32_asm(&buf[40], 0xDEADBEEFu, 0);                     // count==0 writes nothing
    if (buf[40] != 0xA5A5A5A5u) return 4;
    memset32_asm(&buf[40], 0xDEADBEEFu, 1);                     // exactly one dword
    if (buf[40] != 0xDEADBEEFu || buf[41] != 0xA5A5A5A5u) return 5;
    return 0;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_addr) return;
    if (x >= fb_width || y >= fb_height) return;
    // Subtract after validating the origin: x+w/y+h can wrap for dimensions
    // converted from negative layout space and bypass the framebuffer bound.
    if (w > fb_width - x) w = fb_width - x;
    if (h > fb_height - y) h = fb_height - y;
    if (!w || !h) return;

    if (fb_bpp == 32) {
        if (!clip_on && !region_on) {
            // A solid row is a run of identical pixels: one rep-stosd per row beats
            // the scalar per-pixel store loop ~2x (measured), and this fills the
            // desktop background + every window body/taskbar each recomposite.
            uint32_t* ptr = (uint32_t*)fb_addr + y * fb_width + x;
            for (uint32_t row = 0; row < h; row++) {
                memset32_asm(ptr, color, w);
                ptr += fb_width;
            }
        } else {
            for (uint32_t row = 0; row < h; row++) {
                int py = (int)(y + row), lo, hi;
                clip_span(py, &lo, &hi);
                int a = (int)x, b = (int)(x + w);
                if (a < lo) a = lo;
                if (b > hi) b = hi;
                if (a >= b) continue;
                uint32_t* ptr = (uint32_t*)fb_addr + (uint32_t)py * fb_width + (uint32_t)a;
                memset32_asm(ptr, color, (size_t)(b - a));
            }
        }
    } else if (fb_bpp == 8) {
        uint8_t* ptr = (uint8_t*)fb_addr + y * fb_width + x;
        for (uint32_t row = 0; row < h; row++) {
            for (uint32_t col = 0; col < w; col++)
                ptr[col] = (uint8_t)color;
            ptr += fb_width;
        }
    }
}

void fb_blit(const void* src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
             uint32_t dx, uint32_t dy) {
    if (!fb_addr || !src) return;
    if (fb_bpp == 32) {
        // Clip to the framebuffer. This function had NO bounds check at all, so
        // any caller blitting near the right or bottom edge — a window dragged
        // there, or Paint's 512x384 canvas — wrote straight past the end of the
        // buffer into whatever the allocator had placed after it.
        //
        // dx/dy are unsigned, so an off-screen negative coordinate arrives here
        // as a huge value and is caught by the same test.
        if (dx >= fb_width || dy >= fb_height) return;   // entirely off-screen
        uint32_t src_stride = w;                         // rows are ORIGINAL w apart
        uint32_t max_w = fb_width - dx;
        uint32_t max_h = fb_height - dy;
        if (w > max_w) w = max_w;                        // narrow the copy...
        if (h > max_h) h = max_h;                        // ...but not the stride

        uint32_t* dst = (uint32_t*)fb_addr + dy * fb_width + dx;
        uint32_t* src32 = (uint32_t*)src + sy * src_stride + sx;
        for (uint32_t row = 0; row < h; row++) {
            if (!clip_on && !region_on) {
                memcpy_asm(dst, src32, (size_t)w * 4);   // contiguous row copy (rep movsb)
            } else {
                int lo, hi;
                clip_span((int)(dy + row), &lo, &hi);
                int a = (int)dx, b = (int)(dx + w);
                if (a < lo) a = lo;
                if (b > hi) b = hi;
                for (int c = a - (int)dx; c < b - (int)dx; c++)
                    dst[c] = src32[c];
            }
            dst += fb_width;
            src32 += src_stride;
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb_width, fb_height, color);
}

// Fill a rectangle with a vertical gradient: `top` at the first row, `bottom` at
// the last, linearly interpolated per row. 32bpp only; falls back to a flat
// `top` fill otherwise. Colours are the 32bpp BGRX form fb_rgb() produces, so the
// channels sit at <<16 (R), <<8 (G), <<0 (B) with the alpha byte forced opaque.
void fb_fill_vgrad(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t top, uint32_t bottom) {
    if (fb_bpp != 32 || h == 0) { fb_fill_rect(x, y, w, h, top); return; }
    int tr = (int)((top >> 16) & 0xFF), tg = (int)((top >> 8) & 0xFF), tb = (int)(top & 0xFF);
    int dr = (int)((bottom >> 16) & 0xFF) - tr;
    int dg = (int)((bottom >>  8) & 0xFF) - tg;
    int db = (int)( bottom        & 0xFF) - tb;
    int den = (h > 1) ? (int)(h - 1) : 1;
    for (uint32_t row = 0; row < h; row++) {
        int r = tr + dr * (int)row / den;
        int g = tg + dg * (int)row / den;
        int b = tb + db * (int)row / den;
        fb_fill_rect(x, y + row, w, 1,
                     (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
    }
}

// Darken a rectangle in place by mixing its pixels toward black. `shade` is the
// black overlay's alpha, 0..255 (0 = no change, 255 = solid black). Reads the
// current draw target, so it composites over whatever was already drawn there —
// this is what makes window drop shadows fall onto the desktop and lower windows.
// 32bpp only; a no-op otherwise (shadows are a cosmetic layer, not correctness).
// Coordinates are signed and clipped, so a shadow feathered past the top-left
// origin (x-2, y-2) is fine.
void fb_darken_rect(int x, int y, int w, int h, uint8_t shade) {
    if (!fb_addr || fb_bpp != 32 || shade == 0 || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)fb_width || y >= (int)fb_height) return;
    if (x + w > (int)fb_width)  w = (int)fb_width  - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t keep = 255 - shade;   // how much of the original survives, 0..255
    // Per-call blend table: keep is constant for the whole rect, so precompute
    // v*keep/255 for every channel value ONCE (256 divides) instead of three real
    // `div`s per pixel (-Os emits an actual div, not a magic-multiply). Output is
    // byte-identical to the per-pixel divide; measured ~5x on shadow-sized rects,
    // and shadows blend every window's feathered border each recomposite.
    uint8_t tab[256];
    for (int v = 0; v < 256; v++) tab[v] = (uint8_t)((uint32_t)v * keep / 255);
    int clipped = (clip_on || region_on);      // honour the round/region clip per row
    for (int row = 0; row < h; row++) {
        int py = y + row, a = x, b = x + w;
        if (clipped) { int lo, hi; clip_span(py, &lo, &hi); if (a < lo) a = lo; if (b > hi) b = hi; }
        if (a >= b) continue;
        uint32_t* ptr = (uint32_t*)fb_addr + (uint32_t)py * fb_width + (uint32_t)a;
        for (int col = 0; col < b - a; col++) {
            uint32_t p = ptr[col];
            ptr[col] = (p & 0xFF000000) | ((uint32_t)tab[(p >> 16) & 0xFF] << 16)
                     | ((uint32_t)tab[(p >> 8) & 0xFF] << 8) | (uint32_t)tab[p & 0xFF];
        }
    }
}

// KAT: the darken blend table equals the exact per-channel v*keep/255, with the
// right endpoints — keep=255 (shade 0) is identity, keep=0 (shade 255) is all
// black, keep=127 (shade 128) halves. 0 = pass, else the failing case number.
int darken_blend_selftest(void) {
    uint8_t tab[256];
    for (int v = 0; v < 256; v++) tab[v] = (uint8_t)((uint32_t)v * 255 / 255);   // keep=255
    if (tab[0] != 0 || tab[128] != 128 || tab[255] != 255) return 1;
    for (int v = 0; v < 256; v++) tab[v] = (uint8_t)((uint32_t)v * 0 / 255);     // keep=0
    if (tab[0] != 0 || tab[255] != 0) return 2;
    for (int v = 0; v < 256; v++) tab[v] = (uint8_t)((uint32_t)v * 127 / 255);   // keep=127
    if (tab[255] != 127 || tab[0] != 0 || tab[254] != 126) return 3;             // 254*127/255=126.5->126
    uint32_t p = 0xFF40A0F0u, got = (p & 0xFF000000)
        | ((uint32_t)tab[0x40] << 16) | ((uint32_t)tab[0xA0] << 8) | (uint32_t)tab[0xF0];
    if (got != (0xFF000000u | ((0x40u*127/255) << 16) | ((0xA0u*127/255) << 8) | (0xF0u*127/255))) return 4;
    return 0;
}

// Integer square root (no libm in the kernel). Shared by the rounded-corner
// helpers below and the desktop/login circles.
uint32_t fb_isqrt(uint32_t x) {
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

// Horizontal inset of a rounded corner of radius R at vertical distance `d` from
// the rounded end (d = 0 is the very edge row, fully cut; d = R-1 is nearly
// square). Pixels nearer the corner than this inset lie outside the arc and are
// simply not painted, so whatever was drawn beneath shows through — there is no
// saved backing store to carve against, so the only clean option is to never
// touch those pixels. Callers that round only some corners (a title bar's top)
// use this directly, row by row; the two helpers below use it for all four.
int fb_corner_inset(int d, int R) {
    if (R <= 0 || d >= R) return 0;
    int dy = R - d;
    return R - (int)fb_isqrt((uint32_t)(R * R - dy * dy));
}

// Fill a rectangle with all four corners rounded to radius R.
void fb_fill_round_rect(int x, int y, int w, int h, int R, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (R * 2 > w) R = w / 2;
    if (R * 2 > h) R = h / 2;
    for (int row = 0; row < h; row++) {
        int d = -1;
        if (row < R)            d = row;
        else if (row >= h - R)  d = h - 1 - row;
        int in = (d >= 0) ? fb_corner_inset(d, R) : 0;
        fb_fill_rect(x + in, y + row, w - 2 * in, 1, col);
    }
}

// 1px rounded outline matching fb_fill_round_rect's shape.
void fb_stroke_round_rect(int x, int y, int w, int h, int R, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (R * 2 > w) R = w / 2;
    if (R * 2 > h) R = h / 2;
    fb_fill_rect(x + R, y,         w - 2 * R, 1, col);
    fb_fill_rect(x + R, y + h - 1, w - 2 * R, 1, col);
    fb_fill_rect(x,         y + R, 1, h - 2 * R, col);
    fb_fill_rect(x + w - 1, y + R, 1, h - 2 * R, col);
    for (int row = 0; row < R; row++) {
        int in = fb_corner_inset(row, R);
        fb_put_pixel((uint32_t)(x + in),         (uint32_t)(y + row),         col);
        fb_put_pixel((uint32_t)(x + w - 1 - in), (uint32_t)(y + row),         col);
        fb_put_pixel((uint32_t)(x + in),         (uint32_t)(y + h - 1 - row), col);
        fb_put_pixel((uint32_t)(x + w - 1 - in), (uint32_t)(y + h - 1 - row), col);
    }
}

uint32_t fb_get_width(void) { return fb_width; }
uint32_t fb_get_height(void) { return fb_height; }
void* fb_get_addr(void) { return fb_addr; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    // VBE 32-bit LFB is BGRX (byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = X)
    if (fb_bpp == 32) return (0xFF << 24) | (r << 16) | (g << 8) | b;
    return (r << 16) | (g << 8) | b;
}
