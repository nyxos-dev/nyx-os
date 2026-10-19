# User-space GUI — migration audit & plan

**Status: planning / groundwork.** This document audits where the NyxOS GUI runs today,
what user-space plumbing already exists, and a phased plan to move GUI *applications* out
of the kernel and into ring-3 processes. It is a living plan, not a finished migration.

## The problem

Today the **built-in desktop applications run in ring 0**. The compositor (Hemera), the window manager, the
desktop, and every app — terminal (Erebus), browser (Selene), editor (Mnemosyne), paint,
the games, the file manager, the task manager, the wallpaper picker — live under
`kernel/gui/` and are compiled straight into `nyx-kernel.bin`. The `user/*.c` programs,
including the `wintest` GUI demonstrator, coreutils and `xbm` ports, are ring-3 ELFs.

For a daily-driver-grade OS that is real technical debt:

- **No fault isolation.** A bug in any GUI app faults in kernel context and panics the
  *whole* machine, instead of just closing one window.
- **No memory protection** between GUI components — they share the kernel address space.
- **Full privilege.** Every GUI app runs with kernel privileges; there is no boundary.

The goal is the daily-driver shape: a display server plus GUI **clients that run in ring 3**,
talking to the kernel over a small syscall ABI — so an app crash is contained.

## What already exists (the good news)

NyxOS already has a working **ring-3 windowing ABI**. A user-space program can open a
composited desktop window, blit pixels to it, and receive input — all without kernel
privileges. This is the foundation the migration builds on; it is not a greenfield.

**Syscalls** (see [SYSCALLS.md](SYSCALLS.md), `kernel/core/syscall.c`, `user/syscall.h`):

| # | syscall | what it does |
|---|---------|--------------|
| 54 | `SYS_FBINFO` | screen width/height/bpp (for a fullscreen app) |
| 55 | `SYS_FBPRESENT` | blit a 32bpp buffer to the whole screen, scaled (fullscreen app) |
| 56 | `SYS_GETKEYEVENT` | next raw key make/break event |
| 57 | `SYS_WIN_CREATE` | open a `w×h` composited desktop window → id |
| 58 | `SYS_WIN_DESTROY` | destroy a window (idempotent) |
| 59 | `SYS_WIN_PRESENT` | blit a `w×h` XRGB buffer as the whole client area |
| 60 | `SYS_WIN_POLL_EVENT` | pop one input event, non-blocking |
| 61 | `SYS_WIN_PRESENT_RECT` | copy a rectangular client-area update |
| 62 | `SYS_WIN_SET_TITLE` | update the bound window title |
| 63 | `SYS_WIN_RESIZE` | resize the client area and discard the old backing |
| 64 | `SYS_FONT_GLYPH` | copy one shared 8x16 font glyph |

**Kernel model** — `kernel/gui/core/userwin.c`: a fixed registry of user windows, each owning
its backing buffer and a per-window **event ring** (`UWE_KEY`, `UWE_CLICK`, `UWE_MOVE`, with
client-relative coordinates). `uwin_create` binds a real compositor window whose draw
callback blits the app's last-presented buffer, and whose key/click/move callbacks push
events into that window's ring. Keyboard **and** mouse both reach the client. There is a
known-answer test, `uwin_selftest` (registered in the battery as `userwin`), that exercises
the ring, the registry, present size-checking, event round-trip, release and slot reuse —
so the model is verified headlessly.

**Proof it works end-to-end** — `user/wintest.c`: a ring-3 ELF that `win_create`s a window,
animates it with `win_present`, and pumps `win_poll_event` until closed. This is the
existing demonstration that the whole path composites correctly from user space.

So: **fullscreen** ring-3 graphics (DOOM uses `SYS_FBINFO`/`SYS_FBPRESENT`/`SYS_GETKEYEVENT`)
**and windowed** ring-3 graphics (`wintest` uses `SYS_WIN_*`) already run today.

## The gap

The apps themselves have not been migrated — they are still in-kernel C using the compositor's
internal API directly (`window_create`, `fb_*`, `font_*`), not ring-3 clients using `SYS_WIN_*`.
Moving one means rewriting it as a standalone ELF that renders into its own buffer and presents
it. The client ABI is also still minimal for rich apps; likely additions as real apps are ported:

- ~~**Partial present.**~~ **DONE (v6.5.56):** `win_present_rect(id, buf, x, y, w, h)`
  (`SYS_WIN_PRESENT_RECT`, 61) updates a sub-rectangle of the client area, so a client sends only
  what changed instead of the whole window. (The compositor still blits the whole backing on
  redraw — a compositor-side partial-blit is a later optimization.)
- **Title and resize are available.** App-initiated resizing uses `win_resize`;
  frame drag-resizing queues `UWE_RESIZE`. Window-move and focus events are still absent.
- **Drawing helpers exist** in `user/uwin.h`: clipped primitives, shared-font text,
  button drawing and event-state helpers. This is not yet a full widget toolkit
  with text editing, menus, layout and accessibility.
- **Ownership and lifetime need work.** The registry uses reusable slot IDs without
  owner identities or generation counters. Process exit does not currently release
  those slots. Migration must address other-process access, stale handles, thread
  groups, exit/fault cleanup and synchronization before claiming app isolation.
- The **compositor/display-server stays in the kernel** under this plan (see below); fully
  moving Hemera itself to user space is a separate, much larger step.

## Plan (phased, incremental)

The pragmatic first target is **app fault isolation** — the biggest daily-driver win — while
leaving the compositor in the kernel acting as an in-kernel display server that ring-3 clients
drive through `SYS_WIN_*`.

1. **Audit & document** (this file). ✅ groundwork.
2. **Pick the smallest self-contained app** and port it to a ring-3 ELF client on top of the
   existing `SYS_WIN_*` ABI (e.g. a simple viewer/toy), launched like any other ELF; verify it
   composites and takes input, with the original in-kernel version kept until parity is proven.
3. **Close ABI gaps the port exposes** — including ownership, lifetime and focus events —
   each behind a syscall with its own KAT (extend `uwin_selftest`).
4. **Extend the existing ring-3 UI helpers** into reusable widgets as real clients need them.
5. **Port the remaining apps one at a time**, boot-verifying 0-fault at each step, then retire
   the in-kernel version once the ring-3 one reaches parity.
6. **(Stretch, much later)** move the compositor itself toward user space.

Each step is a small, independently verifiable increment — no big-bang rewrite of a working OS.

## Window close context ownership (6.5.220)

The compositor owns dynamically allocated application contexts in
`window_t.reserved`: `window_destroy` invokes `on_close`, then frees any context
pointer the callback leaves behind. User windows instead store a pointer into
the static registry. Their close callback now clears `reserved` before releasing
the backing buffer, so the compositor never passes the static registry to
`kfree`. Both app-initiated destruction and compositor-driven close use this path.

`tools/test_userwin_close.py --compare-ref c66a346` compiles the actual
`window_destroy` function with the user-window implementation and host allocation
stubs under ASan/UBSan. It exercises repeated app/compositor closes, checks that
only the backing buffer and window are released, and checks repeated destroys.
The baseline triggers an AddressSanitizer invalid-free report for static storage.
In the kernel, `kfree` writes a freed marker to the presumed allocation header
before checking its original magic; passing static storage is therefore invalid
even if it does not immediately cause a guest panic. This fix does not implement
process-exit cleanup or concurrent window ownership.

The corrected build passes 961 close checks, the existing identity/headless
regressions, 230 QEMU kernel tests and four-CPU login, with no GCC14 warnings.
A ring-3 guest launched using `spawn` detects GUI close, creates a replacement,
rejects present through the old handle, destroys the replacement twice and
exits 0. Terminal commands and dragging remain usable afterward.

For interactive window tests, launch the client with `spawn`. The current
foreground `exec` wait loop calls `compositor_redraw_now`, which only repaints;
it does not run the desktop input/event loop. A waiting client can therefore
remain visible without receiving a title-bar close until the foreground wait
ends. This input limitation is separate from the static-context fix.

## Window identities (6.5.219)

Window IDs are opaque nonnegative `int` handles, not registry indices. A
successful allocation consumes the next handle, independently of which of
the eight slots it occupies. Closing a window invalidates its handle; a
replacement in the same slot receives a different one. Destroy remains
idempotent for a dead handle. Present, rectangle present, poll, retitle and
resize reject that handle without affecting the replacement's buffer or events.

Handles stop at `INT_MAX`. After the final handle is allocated, further creates
fail until reboot, even if slots are free. This deliberate failure avoids
wrapping into a previously issued identity. Placement on the desktop still uses
the small slot index, so a long creation history does not move windows off-screen.

`tools/test_userwin_handles.py --compare-ref e3e9f7b` runs 3489 ASan/UBSan
checks, including repeated headless/graphical slot reuse, all stale-handle
operations, full-table replacement and the final two handle values. The old
implementation fails the identity-reuse regression. The existing 585-check
headless/desktop suite and kernel user-window KAT also pass on the host.

The final GCC14 build is warning-free, passes 230 QEMU kernel tests and reaches
login with four CPUs. In a separate single-CPU headless VM, a ring-3 test runs
128 create/destroy/replacement cycles across two executions and a full-table
replacement case; stale calls are rejected and the live window remains usable.
In the graphical VM, two packaged `wintest.elf` runs receive handles 0 and 1,
render at the same visible position, present 160 frames each and exit 0.
The terminal continues to run `echo` and `mem` afterward. A separate GUI check
visibly moves the terminal from (80,80) to (180,160) and runs another command.

This is not process ownership or automatic cleanup. Another process that knows
a live handle can still use it; process exit still leaves windows behind.
Registry, backing-buffer and compositor synchronization remain open work;
these sequential tests do not establish SMP or GUI race safety.

## Text-mode window creation (6.5.218)

Without framebuffer dimensions, `uwin_create` now returns a headless registry slot
without calling `window_create`. Present, poll, resize, retitle and explicit destroy
remain available; the slot has no visible frame and receives no desktop input.
Clients must still explicitly destroy it. A missing compositor window is not an
application-isolation or automatic-cleanup guarantee.

Before this check, a ring-3 `win_create(8, 8, "owner")` on a QEMU boot with
`-vga none` caused a ring-0 division-by-zero panic in the background rendering
reached through `redraw_all`. The test uses the actual syscall, not a model of it.
The host regression `tools/test_userwin_headless.py --compare-ref 1d8849c`
checks absent/partially absent dimensions, repeated headless operations, desktop
binding callbacks, compositor allocation failure and the existing user-window KAT
under ASan/UBSan (585 checks plus the KAT).

With the check in place, the same text-mode VM completes the window syscalls.
The 6.5.218 audit reproduced three limits: a fork child can
retitle/present/poll/destroy its parent's window; a released ID can destroy a new
window that reused its slot; and eight programs that exit without destroying
their windows exhaust the eight-slot registry. Explicit destruction recovers the
slots. The stale-ID case is addressed by 6.5.219 above; ownership and exit cleanup
remain unfixed. These are single-CPU VM results, not concurrency or fault-cleanup tests.

The final kernel builds without compiler/linker warnings, passes 230 QEMU
self-tests and reaches login with four CPUs. With a framebuffer, the packaged
`wintest.elf` creates a desktop window, presents 160 frames, explicitly destroys
the window and exits 0; Erebus continues to execute `echo` and `mem` afterward.

## Pointers

- Syscall ABI: [SYSCALLS.md](SYSCALLS.md) (rows 54–64).
- Kernel model + KAT: `kernel/gui/core/userwin.{c,h}` (`uwin_selftest`, battery entry `userwin`).
- Working ring-3 client: `user/wintest.c`.
- In-kernel GUI (what migrates): `kernel/gui/core/` (compositor) and `kernel/gui/apps/`.
