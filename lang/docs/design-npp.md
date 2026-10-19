# N++ — Design Document

**Status:** P1–P5 staged into `ncc` (complete at bootstrap scale); **the front-end era is open — §6 is the plan** · **Base language:** [N](spec-n.md) · **Compiler:** `nppc`

N++ is to N what C++ was to C: a superset that keeps the base language intact
and adds the abstraction and safety layer on top. The contract:

> **Every valid N program is a valid N++ program, with identical behavior.**

N stays the language of the metal — kernel modules, drivers, tools where you
want to see every instruction. N++ is the language of applications: same
syscall access, same zero-hidden-runtime philosophy, but with types that make
whole bug classes unrepresentable.

---

## 1. Why a second language instead of growing N?

Because the two audiences pull in opposite directions. A driver author wants
`raw *u8` and no opinions; an application author wants `Result` and `match`
and cannot leak a window handle. Splitting lets N stay *permanently simple*
(the spec fits in one document, the compiler in one file — a feature in
itself, and the path to self-hosting), while N++ absorbs all the complexity
budget. The C/C++ split proved the model; we keep the good part (shared core,
gradual adoption per-file) and drop the bad (N++ will not fork the base
semantics — it compiles *through* N's pipeline, not beside it).

Files choose their language by extension: `.n` is compiled as N by `ncc`;
`.npp` is compiled as N++ by `n++`. The two link freely against each other.

## 2. Feature pillars

### 2.1 Data types: `struct`, `enum`, `match`

```npp
struct Rect { w: u32, h: u32 }

impl Rect {
    fn area(self) -> u32 = self.w * self.h
}

enum Shape {
    Circle(r: f64),
    Rect(w: f64, h: f64),
    Empty,
}

match shape {
    Circle(r)  => area_circle(r),
    Rect(w, h) => w * h,
    Empty      => 0.0,
}
```

Sum types with exhaustive `match` are the foundation everything else builds
on (`Result` is just an enum). Lowering: structs map to C structs; enums to
tagged unions; `match` to switch on the tag with bound locals.

### 2.2 Errors: `Result<T, E>` and `?`

```npp
fn read_config() -> Result<Config, Errno> {
    fd := fs.open("/etc/nyx.conf", .Read)?;   // ? propagates the error
    defer fs.close(fd);
    Config.parse(fd)
}
```

Kernel returns negative on error; the N++ standard bindings convert that
convention to `Result` once, at the boundary, so application code never
checks sign bits. `?` lowers to the early-return pattern; `defer` runs
LIFO at scope exit (including early returns — lowered via cleanup labels).

### 2.3 Safety: checked pointers and capabilities

- **`#[user] *T`** — a pointer that the compiler proves lies in the canonical
  user half `[0x1000, 0x0000_8000_0000_0000)` before it crosses a syscall
  boundary. This turns NyxOS's `user_ptr_ok()` kernel check into a
  compile-time guarantee. `raw *T` remains the explicit opt-out.
- **`PageFlags`** — a typed bitset for page mappings that rejects
  `WRITABLE | EXEC` combinations at compile time: W^X by construction,
  matching the kernel's NX discipline.
- **Capabilities** — a module declares its height (`ring0` / `ring3`) and
  capability set (`syscall`, `mmio`, `ports`, …); the checker rejects calls
  that exceed it. Kernel modules and user programs share one language with
  different licenses.

### 2.4 Ownership (opt-in, not Rust-globally)

Default N++ is value/pointer semantics like N. Types can opt into ownership:

```npp
own struct File { fd: i32 }        // moved, not copied; must be consumed
```

Dropping an unconsumed `own` value is a compile error unless the type
declares a destructor — enough to make handle leaks (files, windows, sockets)
unrepresentable, without imposing borrow-checking on all code. This is
deliberately far short of Rust: ownership is a per-type contract, not a
global discipline. (Shipped in the bootstrap: `#[drop(fn)]` wires an
ordinary consuming function as the destructor — spec §4.6.)

### 2.5 OS integration: GUI and tasks

The compositor and scheduler get language-level bindings. This section is
the **binding-surface draft**: concrete enough that the kernel side can be
built toward it, honest about what runs today.

#### What the kernel has, and why it does not cross the boundary

NyxOS windows today are **kernel-side constructs**: `window_create()` takes
a *draw callback*, and every app (terminal, editor, games) lives inside the
kernel, painting through `window_draw_fn` and receiving input through
`on_key`/`on_click`/`on_tick` function pointers (`kernel/gui/core/
compositor.h`). Function pointers cannot cross a syscall boundary, so a
user-space window needs the model **inverted**:

- the *process* owns its pixels — a plain buffer in user memory;
- the *kernel* composites them — a present call blits the buffer into the
  window's client area;
- input arrives as **data, not callbacks** — the process polls a
  per-window event queue whenever it likes.

The precedent already exists in-tree: `SYS_FBINFO`/`SYS_FBPRESENT`/
`SYS_GETKEYEVENT` (54–56) do exactly this for fullscreen programs (DOOM).
Windowed is the same idea with an id and a queue.

#### Proposed syscall surface (57–60)

| # | Call | Signature (kernel view) | Notes |
|---|---|---|---|
| 57 | `win_create` | `(w, h, title_ptr, title_len) -> id` (or −1) | client-area size; title copied at the crossing |
| 58 | `win_destroy` | `(id) -> 0/-1` | idempotent close |
| 59 | `win_present` | `(id, buf, w, h) -> 0/-1` | full client-area blit of a `u32` XRGB buffer; kernel validates the user range (`user_ptr_ok`) and clips |
| 60 | `win_poll_event` | `(id, ev_ptr) -> 1/0/-1` | copies one event out, or 0 if the queue is empty |

The event record is four `i64` words — `{ kind, a, b, c }` — with kinds:
`1` key (a = keycode), `2` click (a = x, b = y, c = button), `3` move
(a = x, b = y, c = buttons), `4` close-requested, `5` resize (a = w,
b = h). A fixed small queue per window (say 32 events, oldest dropped)
keeps the kernel side allocation-free.

Deliberate non-goals for v1: no damage rectangles (present is whole-area),
no shared memory mapping (present copies — correctness first), no
callbacks, no vsync contract beyond "present blits when called".

#### The N-side binding — the P5 pieces meet

The handle model is the `own` story arriving at its destination: a window
is an `own struct` and its destructor is `win_destroy` — leaking a window
becomes a compile error, dropping one closes it.

```n
#[caps(syscall)]
extern syscall {
    fn win_create(w: i64, h: i64, t: #[user] *u8, tl: isize) -> i64 = 57
    fn win_destroy(id: i64) -> i64                                  = 58
    fn win_present(id: i64, buf: #[user] *u32, w: i64, h: i64) -> i64 = 59
    fn win_poll_event(id: i64, ev: #[user] *i64) -> i64             = 60
}

#[drop(close_window)]
own struct Window { id: i64 }

#[caps(syscall)]
fn close_window(win: Window) {
    win_destroy(win.id);
}
```

An event loop in **current N syntax** (this compiles today against the
extern block above — `while`, indexing, `#[user]` crossings and `own`
all exist):

```n
fn event_loop(win: Window, fb: *u32, w: i64, h: i64) {
    mut running := true;
    while running {
        ev := alloc_words(4);
        while win_poll_event(win.id, ev as #[user] *i64) > 0 {
            if ev[0] == 4 {                 // close requested
                running = false;
            }
            if ev[0] == 2 {                 // click: paint a dot
                fb[ev[2] * w + ev[1]] = 0xFFFFFF;
            }
        }
        win_present(win.id, fb as #[user] *u32, w, h);
    }
    close_window(win);      // a held param is the callee's manual duty (sink rule)
}
```

The auto-close reads best at the birth site. Note how the capability
gate composes with the handle: only the audited `open_window` wrapper
touches the syscall — `main` holds no capability, cannot call
`win_create` directly (the checker refuses), and still cannot leak the
window:

```n
#[caps(syscall)]
fn open_window(w: i64, h: i64, title: str) -> Window {
    Window{ id: win_create(w, h, title.ptr as #[user] *u8, title.len as isize) }
}

fn main() -> i64 {
    win := open_window(220, 120, "hello");
    put("window {win.id} open\n");          // field reads peek, no move
    0
}                                           // ← win was LIVE: #[drop] closes it
```

The `gui.Window.new(...)? / win.on(Event.Click, fn(e) …)` sugar stays
`n++`-front-end territory (closures, `Result`); the binding above is the
floor it lowers to.

#### Staging — honest

| Piece | Status |
|---|---|
| N language surface (extern block, `own` + `#[drop]` handle, event loop) | **compiles today** — every construct shipped v0.12–v0.19 |
| Kernel syscalls 57–60 | **do not exist** — the ask above |
| Compositor support | needs a *user-buffer window* variant: a `window_t` whose draw callback blits from the owning process's presented buffer, plus a per-window event queue filled where `on_key`/`on_click` fire today |
| P5 gate ("a windowed N++ app on the NyxOS desktop") | **unblocked — 57–60 landed (v6.4.354), and nwin.n runs** |

Structured concurrency (`task` blocks whose children cannot outlive them)
maps onto NyxOS's preemptive scheduler — design follows once the kernel's
thread-spawn surface for user space stabilizes.

## 3. Compilation strategy

```
file.npp ──n++ front-end──► typed AST ──lower──► N-level IR ──► C ──cc/tcc──► ELF
```

The `n++` front-end (checker + lowering) reuses `ncc`'s lexer/parser
foundation, extends it with the constructs above, type-checks, then *lowers
to the same C surface `ncc` targets* — same runtime, same crt0, same
Makefile rules. This keeps one backend to maintain and means every N++
feature is debuggable by reading the generated C.

*(This sketch predates self-hosting. §6 revises the target: the
front-end now lowers to **N source**, and the verified N pipeline —
ncc or the selfhost toolbox — carries it the rest of the way.)*

## 4. Staged roadmap

| Stage | Contents | Gate |
|---|---|---|
| P1 | ✅ shipped in `ncc` v0.2–v0.4: inference (`i64` literals, typed interpolation, `mut`) + the expression-level checker (names, arity, argument/operand/return/assignment types) | all `lang/examples` still compile bit-identically ✅ |
| P2 | ✅ **complete**: `struct` (v0.5) · `defer` (v0.6) · `enum` + `match` (v0.7) · `impl` methods (v0.8, static dispatch) | structs.n, defer.n, enums.n, methods.n |
| P3 | ✅ **complete** (bootstrap side): match-as-expression (v0.9) · `?` over structural Ok/Err result enums (v0.10 — same-type pass-through, cross-type Err rewrap, defers honored) · fs bindings sketch (`fsio.n`: the negative-return→Result boundary conversion of §2.2, `?` chains, deferred close). Generic `Result<T, E>` and the idiomatic `.npp` rewrite move to the `n++` front-end (P5 era) | `fsio.n` runs the §2.2 shape end-to-end |
| P4 | ✅ **complete** (bootstrap side): `#[user]` pointer flavor (v0.12 — hard no-implicit-conversion boundary, `as` as the audited crossing) · `pageflags` W^X bitset (v0.13 — total compile-time W^X proof, live-mmap verified) · capabilities (v0.14 — `#[caps(syscall)]` gates extern blocks, direct callers must hold the cap, wrapper = audited boundary). The canonical-half range proof, further capability names (`mmio`, `ports`), and ring heights move to the `n++` front-end and kernel-side modules | a kernel-module example checked at `ring0` |
| P5 | 🔨 **started**: `own` structs shipped (v0.17 — move-not-copy, must-consume; leaks/double-use/discards are compile errors), the tracking is **branch-aware** (v0.18 — `if`/`else` arms may consume, both exits must agree, a `return`-ending arm is exempt; moves stay refused in loops, `while` conditions, and match arms), and **destructors shipped** (v0.19 — `#[drop(fn)]` wires an ordinary consuming fn; live values auto-drop at scope end, defers first then drops LIFO, held params never auto-drop, no drop flags). The own-types contract of §2.4 is complete. And the GUI bindings are REAL: the kernel implemented the drafted surface verbatim in v6.4.354 (syscalls 57–60, the 4×i64 event encoding — [#77](https://github.com/kazah-png/nyx-os/issues/77) closed), and [nwin.n](../examples/nwin.n) is the running program — an `own`+`#[drop]` Window handle over a real kernel resource, the gradient-and-square frame presented from a process-owned buffer, events polled, gracefully skipping where no desktop composites it. **P5 complete at bootstrap scale** | a windowed N++ app on the NyxOS desktop — the N bootstrap ran first |

P1 is the enabling investment: everything later depends on the checker
existing. It also immediately improves plain N (better errors from `ncc`).

## 5. Open questions

Recorded here so decisions are made deliberately, not by accident:

1. **Generics monomorphization vs erasure** — **decided (§6.2): monomorphization**
   (C++-style, zero-cost, larger binaries; N has no runtime to erase into).
2. **String ownership** — `str` is a view; N++ needs an owning string for
   builders. Candidate: `String` as an `own struct` over the future
   user-space allocator.
3. **Closure representation** — **decided (§6.2): explicit capture struct +
   top-level function**, the classic lambda-lifting lowering; a fat-pointer
   calling convention only if dynamic dispatch is ever needed.
4. **`n++` binary name on VFS** — `n++` is a valid NyxOS filename today; if
   shell parsing ever conflicts, fallback name is `npc`.

## 6. The n++ front-end era — the staged plan

The bootstrap staging (§4) is finished: every P-row shipped inside `ncc`,
and N is self-hosted with a byte-verified, installable toolchain
(`docs/selfhost.md`). What remains of the N++ vision is exactly what a
*separate front-end* exists for — generics, closures, modules — and this
section is its plan, written the way `selfhost.md` was written for M5:
shape first, then rungs, then the verification each rung must survive.

### 6.1 The compiler shape — lower to N source

Three shapes were on the table:

1. **`nppc` lowers `.npp` to `.n` source** — the front-end handles the
   new constructs (instantiate generics, lift closures, resolve modules)
   and emits plain N; the existing pipeline (`ncc`, or the self-hosted
   toolbox) carries it to C and the OS. **Chosen.**
2. Lowering straight to C — one less hop, but it forks the backend: two
   emitters to keep honest, and the lowered program bypasses every
   verified stage the last months built.
3. Growing `ncc` further — no new dialect, but generics and modules are
   front-end-sized complexity; the single-file bootstrap stays simple
   precisely because they live elsewhere (§1's whole argument).

Shape 1 wins on the strength the project already paid for: the lowered
`.n` is **checked by ncc's own type/ownership/capability checker** (a
free soundness net under the new front-end — a lowering bug that
produces ill-typed N is caught, loudly, by a compiler that is itself
held byte-faithful by the selfhost differentials), it is readable the
way §3 wanted the C to be readable, and every `.npp` program
automatically exercises the verified ladder end to end. The cost — one
more textual hop — buys the entire trust chain.

### 6.2 The feature ladder

**Monomorphized generics + `Result<T, E>`** (the flagship). Generic
`fn` and `struct`/`enum` declarations are templates the front-end
instantiates on use; each instantiation becomes a plain N item with a
mangled name (`__g_<name>_<type>…`, stable and readable), and
`Result<T, E>` becomes the declared-pair result enums v0.10 already
compiles — the structural bridge is shipped, the front-end only
automates the declaring. Duplicate instantiations dedupe by mangled
name. No erasure: N has no runtime to erase into. Shipped as M6.3a–f
(§6.4); [`result.npp`](../examples/result.npp) is the flagship end to
end.

**Closures** — lambda lifting. A closure literal becomes an explicit
capture `struct` plus a top-level function taking it as its first
parameter; the checker computes the capture set (by value; an `own`
capture moves and the struct inherits must-consume). What today is
written as the nwin event loop's manual dispatch becomes handler
fields over exactly that lowering — shipped as
[`nwinui.npp`](../examples/nwinui.npp): `Button { …, on_click: Fn(i64,
i64) }`, a `Ui` with `on_key: Fn(i64) -> bool`, and a `dispatch` that
routes the polled events (a library on top of M6.4c1–c2, no lowering
of its own; suite [10w]).

*Shipped as M6.4a–c4 (§6.4): the capture struct lives on the bump heap
and the closure carries its address, so a closure outlives its frame.
An `own` capture was an N question before it was an nppc one: the
environment has to be an `own` struct holding an `own` field — N
v0.25's rule ([spec-n.md §4.6](spec-n.md), "Own fields inside own
structs") — and it has to be reachable from the closure value, which for
the heap environment means through a pointer, and N refuses pointers to
own values so that an owner has exactly one binding. Of the two shapes
on the table — (i) an own environment passed **by value**, the closure
value itself an own struct consumed by its one call; (ii) an own heap
box in N, a bigger N rung of its own — (i) was chosen for c4: a
`:=`-bound lambda capturing an own local becomes a call-once closure,
and the checker N already has does the rest (a second call is a use
after move, an uncalled closure drops through its captures). Passing
such a closure where an `Fn(…)` is expected needed (ii) after all —
and N v0.26 ([spec-n.md §4.6](spec-n.md), "Behind a raw pointer")
gave it at the smallest size: a pointer to an own type may exist as a
local born from a cast, through which a value is stored (moved in),
peeked, or taken out into a fresh owner. On that, an owning closure is
an ordinary own struct: `#[drop(fo_drop)] own struct FnOnce_<sig> {
env: addr, call: fn(addr, A…) -> R, fin: fn(addr) }` with a per-lambda
finaliser that takes the environment out and lets its captures drop,
`call_once(f, x)` running the body and then the drop, N's checker
giving must-consume, one owner, auto-drop and "a second call is a use
after move" for free — [`ownbox.n`](../examples/ownbox.n) is that
closure written by hand, and the `FnOnce(A) -> R` slot type that
lowers to it is M6.4c6 — shipped:
[`fnonce.npp`](../examples/fnonce.npp).*

**Modules / `use`** — file-level units. A module is a file; `use`
imports its public items (`pub` marks them); resolution is a topological
walk with cyclic imports refused; the front-end concatenates the
instantiated, resolved program into the single `.n` unit the pipeline
already consumes. No link-time machinery — the flat model the OS
toolchain already trusts.

### 6.3 The verification story — differentials from day one

Every rung lands the way the N rungs landed: worked examples + negative
tests in the same commit, and a differential fence in the suite from the
first day — `nppc` lowers an `.npp` example to `.n`, then **both** `ncc`
and the self-hosted toolbox compile it and their C must agree
byte-for-byte (the [8d]-class guarantee extending up one layer). The
lowered `.n` is committed beside its source where it aids review, and
regenerated-and-compared by the suite where it does not (the [1c]
pattern). `nppc` itself starts as hosted C99 beside `ncc.c` — one file,
same toolchain discipline — and the self-hosting question is deferred
until the dialect stabilizes (the N ladder showed the way; it can be
climbed again when it is worth climbing).

**And the third way — the OS itself.** The lowered N of the earlier corpus
of twenty-five N++ programs — with `modlib` and `modutil` as modules their
users inline — has been compiled and run
inside NyxOS by the in-OS `ncc` — built from source there by
`xbm install ncc`, itself compiled by the in-OS TinyCC — with the
lowered `.n` files and `nyxrt.{c,h}` on the `/mnt` disk:
`ncc /mnt/X.n -o /mnt/X_gen.c`, then
`cc /mnt/X_gen.c /mnt/nyxrt.c -I/mnt -o /mnt/bin/nX`, then `nX`. Every
run printed on the serial port exactly what its host run prints (the
host runs are what the suite pins with exact traces) and exited clean.
The four closure rungs' traces, for the record:

| example | in-OS output (one line per `/`) |
|---|---|
| closure | `1 4 9` / `inc(41) = 42` / `twice(+1, 40) = 42` / `neg(5) = -5` |
| capture | `42` / `12 15` / `bump: 42` |
| fnonce | `calling` / `body: fd 3` / `closing 3` / `r = 42` / `closing 5` / `s = 6` / `closing 4` |
| gfnonce | `closing 3` / `a = 42` / `s = hi` / `keeper 7` / `closing 7` / `b = 5` / `keeper 8` / `closing 8` / `w = seed` / `guard 100 sees 9` / `closing 9` / `c = 101` / `guard 200 sees 2` / `d = 201` |

The other twenty-one — `hello` (its pid aside), `box`, `genfn`, `gcast`,
`ginfer`, `genum`, `result`, `rinfer`, `modmain`, `modreexp`, `gnest`,
`gfield`, `gclosure`, `closurety`, `gfnclosure`, `gstructfn`,
`gstructnest`, `owncap`, `nwinui`, `gdrop` and `gowncap` — matched the
same way: 25 of 25, through 25 in-OS `ncc` runs and 26 TinyCC compiles
(the compiler itself first), 76 clean exits, no panic, the whole hook
done 12 seconds after boot. Nothing in `nppc`'s output is special to the host — environments
on the sbrk heap, function values, `#[drop]` own structs and their
finalisers, generic instantiations, modules already inlined — and the
in-OS TinyCC compiled all of it as the plain C99 `ncc` emits, with no
change to any compiler. (The batches of 2026-09-07, 2026-09-08 and
2026-09-10, at commits 531668d, e92af82, 2f46bbf and b122342: a
temporary boot hook ran the commands before the login
screen with the results on serial, and was never committed; the recipe
is a clean archive of `master` in `/tmp`, the kernel and ISO built
there, the `.n` files injected into a copy of the ext2 image with
`debugfs`, QEMU headless, each in-OS output diffed against the
program's host run.)

### 6.4 Milestones

| Stage | Contents | Gate |
|---|---|---|
| M6.1 | This plan | ✅ this section |
| M6.2 | ✅ `nppc` skeleton shipped ([`../nppc/nppc.c`](../nppc/nppc.c)): the read side is ncc’s lexer whole (same tokens, escapes, interpolation stack, attributes, diagnostics — accept and refuse exactly as the N pipeline would), lowering is the identity on the N subset, [`hello.npp`](../examples/hello.npp) is the founding contract as a file, and the suite’s stage [10] is the day-one fence. The parse tree arrives with M6.3’s first real transform | ✅ hello.npp lowers byte-identically and `ncc` + the self-hosted `ngen` agree on its C |
| M6.3a | ✅ **generic structs** monomorphize ([`box.npp`](../examples/box.npp)): `struct Box<T>` + explicit uses `Box<i64>` / `Box<u8>` lower to concrete `__g_Box_<T>` structs (type parameter substituted, uses rewritten, everything else spliced through verbatim) via a token-span rewrite — no full parser needed, no inference. Suite stage [10b] | ✅ box.npp monomorphizes and the lowered N compiles identically under `ncc` and `ngen` |
| M6.3b | ✅ **generic functions** ([`genfn.npp`](../examples/genfn.npp)): `fn id<T>(x: T) -> T` + explicit calls `id<i64>(...)` lower to concrete `__g_id_<T>` functions (signature type slots substituted, body spliced through verbatim, calls rewritten). Type parameters must sit in a handled type slot (`:` / `->` / `*`); a use elsewhere is refused for now. Suite stage [10c] | ✅ genfn.npp monomorphizes and the lowered N compiles identically under `ncc` and `ngen` |
| M6.3c | ✅ **body type slots** ([`gcast.npp`](../examples/gcast.npp)): a cast to a type parameter (`x as T`) inside a generic function body substitutes like the signature does — the one type slot N bodies have, since locals are always inferred (`x := e`). Suite stage [10d] | ✅ gcast.npp monomorphizes and the lowered N compiles identically under `ncc` and `ngen` |
| M6.3d | ✅ **call-site inference** ([`ginfer.npp`](../examples/ginfer.npp)): `id(41)` infers `T = i64` from the literal argument and lowers to the same `__g_id_i64` as the explicit `id<i64>(41)` (dedup to one instantiation). Literal-driven for now (int / string / bool); a value nppc cannot read asks for the explicit form. Suite stage [10e] | ✅ ginfer.npp: inferred and explicit calls share one instantiation; the lowered N agrees under `ncc` and `ngen` |
| M6.3e | ✅ **generic enums** ([`genum.npp`](../examples/genum.npp)): `enum Maybe<T>` + explicit uses `Maybe<i64>` (type positions and construction `Maybe<i64>.Some{...}` / `Maybe<i64>.None`) lower to a concrete `__g_Maybe_i64` (payload type slots substituted); `match` arms need no rewrite. Reuses the fn span-splice. Suite stage [10f]. **The flagship end to end** ([`result.npp`](../examples/result.npp)): one `enum Result<T, E>`, two instantiations (`__g_Result_i64_i64`, `__g_Result_str_i64`), and N's structural `?` composing with them unchanged — same-type pass-through and the cross-instantiation Err rewrap. Stage [10g] runs it on the host and checks the output | ✅ genum.npp monomorphizes and the lowered N compiles identically under `ncc` and `ngen`; result.npp lowers, agrees, and runs correctly |
| M6.3f | ✅ **construction-site inference** ([`rinfer.npp`](../examples/rinfer.npp)): `Result.Ok{ v: n }` / `Maybe.None` with no `<...>` take their type arguments from the enclosing function's declared return type (`-> Result<i64, i64>`) and lower to the instantiation that return type already names — one definition each. Inference never guesses: a bare construction in a function returning something else is refused with the explicit form spelled out (inside a generic function it is a nested use — M6.3g). Suite stage [10h] | ✅ rinfer.npp: inferred constructions share the return type's instantiation; the lowered N agrees under `ncc` and `ngen` and runs correctly; 3 negatives refused |
| M6.3g | ✅ **generics inside generics** ([`gnest.npp`](../examples/gnest.npp)): a template may use another template with its own type parameters — `fn wrap<T>(x: T) -> Box<T> { Box<T>{ val: x } }`, `fn some<T>(x: T) -> Maybe<T> { Maybe.Some{ v: x } }`. A use inside a template is recorded on the template instead of instantiated at once; each concrete instantiation of the template then instantiates, to a fixpoint, the inner generics it needs with its arguments substituted, and the template's emit rewrites the uses (explicit, bare construction, or inferred call) to the concrete names. A type parameter may sit in a generic's argument list; one anywhere the lowering does not rewrite is still refused. Suite stage [10l] | ✅ gnest.npp: `wrap<i64>` / `wrap<str>` / `some<i64>` bring `__g_Box_i64` / `__g_Box_str` / `__g_Maybe_i64` into being; `ncc` and `ngen` agree; host output correct; a nested use with a concrete argument and an inferred call inside a template run; 1 negative refused |
| M6.3h | ✅ **generic struct fields of generic type** ([`gfield.npp`](../examples/gfield.npp)): `struct Pair<T> { a: Box<T>, b: T }`, `struct Two<A, B> { x: Box<A>, y: Box<B> }` — a field's generic type is a nested use of the struct, so each concrete `Pair<i64>` or `Two<i64, str>` instantiates the boxes it needs (nothing else has to mention `Box<i64>`) and names them concretely. The generic a field names is resolved once every generic is known (an enum declared later is fine); a non-generic type with arguments, or a wrong arity, is refused. Generics now compose across structs, functions, and enums alike — nothing pending. (A struct field of *enum* type by value needed N v0.24, which emits type layouts in declaration order; `struct Opt<T> { m: Maybe<T> }` instantiates like any other field now.) Suite stage [10m] | ✅ gfield.npp lowers to `struct __g_Pair_i64 { a: __g_Box_i64, b: i64 }` and `__g_Two_i64_str`; `ncc` and `ngen` agree; host output correct; `Pair<i64>` alone brings `Box<i64>` into being once; 2 negatives refused |
| M6.4a | ✅ **non-capturing lambdas** ([`closure.npp`](../examples/closure.npp)): `fn(x: i64) -> i64 { x * x }` in expression position — a call argument, a struct-literal field, a binding's right-hand side — is lifted to a top-level `fn __c_N(x: i64) -> i64 { x * x }` placed after the item before the enclosing one, and the expression becomes the name `__c_N`: a function value, typed by N v0.24's function types (the N rung the scout asked for landed first: `fn(T) -> U` types in ncc and all four self-hosted modules). A lambda is told from a function type by its body: `fn (…) [-> T] {` in a value position. The pass is a text splice to a fixpoint, innermost first, so lambdas nest. A lambda names its parameters and the program's items only: one that names a local of the enclosing function is refused (`lambda captures 'n' … pass it as a parameter`), and one inside a generic template waits for M6.4b. A `:=`-bound lambda needs its function type declared somewhere (N's binding rule). Suite stage [10o] | ✅ closure.npp lowers with `__c_0`…`__c_3`; `ncc` and `ngen` agree; host output correct; nested lambdas lift inner-first and run; a capture and a lambda in a generic body are refused |
| M6.4b | ✅ **lambdas inside generic templates** ([`gclosure.npp`](../examples/gclosure.npp)): a lambda in the body of `fn boxed<T>` that names `T` lifts to a template of its own, `fn __c_N<T>(v: T) -> Box<T> { … }`, over exactly the type parameters it mentions, and the expression becomes the use `__c_N<T>` — a nested generic use (M6.3g) instantiated once per concrete instantiation of the enclosing function and rewritten to `__g___c_N_i64`, which N passes as a function value. A lambda naming no type parameter lifts plain. The lifted template is declared before its user and the re-lex shows it to the generic pass as any other; the one addition there is a new handled type slot — a type parameter inside a function type's parameter list, `f: fn(T) -> Box<T>`, substitutes like one after `:` (function types postdate M6.3). Suite stage [10p] | ✅ gclosure.npp lowers with `__g___c_0_i64` / `__g___c_0_str` and a plain `__c_1`; `ncc` and `ngen` agree; host output correct; a capture inside a generic body refused |
| M6.4c1 | ✅ **the closure type `Fn(A, B) -> R`** ([`closurety.npp`](../examples/closurety.npp)): N's `fn(…)` is a bare function with nowhere to keep an environment, so closures get a type of their own. Each distinct `Fn` signature lowers to one N struct, `struct __Fn_i64__i64 { env: addr, call: fn(addr, i64) -> i64 }` (the spelling with its punctuation as underscores), declared once ahead of the first struct or function; every `Fn` type slot — parameter, struct field, return type — becomes the struct name; a call through a closure (a parameter, a local bound from a call returning `Fn`, or the `Fn` field of a struct-typed parameter or local) becomes `f.call(f.env, a)`; a lambda in a closure slot takes `_env: addr` first and becomes `__Fn_i64__i64{ env: 0, call: __c_N }`; a named function there gets an adapter. The environment is 0 at this rung. Suite stage [10q] | ✅ closurety.npp lowers with one struct, rewritten calls through a parameter, a field and locals, four lambdas and one adapter; `ncc` and `ngen` agree; host output correct; `Fn()` and `Fn(Fn())` run; `Fn` over a type parameter and a capture refused |
| M6.4c2 | ✅ **captures by value** ([`capture.npp`](../examples/capture.npp)): a lambda in a closure slot may name the enclosing function's locals; each is copied into an environment struct `__E_N` at the closure's birth by a generated maker (`__mk_E_N`: a bump allocation through `sys_sbrk` — declared by nppc when the program does not — 16 bytes per field, never freed) whose address the closure carries as `env`, and the lifted function reads them back as `__e.name` (a captured closure is called through its `call` field), so a closure outlives its frame: `adder(5)` returns one. A captured local's type must be evident to a token scan — a declared parameter, a literal, a call to a known function, a struct literal, or a name so typed — or the capture is refused with the fix named. Captures in a plain `fn(...)` slot stay refused (no environment there); inside generic templates they came with c3. Suite stage [10r] | ✅ capture.npp lowers with `__E_0` / `__mk_E_0` / `__e.k`, `sys_sbrk` declared once; `ncc` and `ngen` agree; host output correct; three refusals (type not evident, plain fn slot, generic body) |
| M6.4c3 | ✅ **`Fn` inside generic templates** ([`gfnclosure.npp`](../examples/gfnclosure.npp)): an `Fn` that names a type parameter of its template — `fn apply<T>(f: Fn(T) -> T, x: T)` — is left as written by the closure pass, the generic pass instantiates the template, and the closure pass runs a second time over the result, where `Fn(i64) -> i64` inside `__g_apply_i64` is concrete: the same `__Fn_i64__i64` struct a plain function uses (each signature declared once, by whichever pass meets it first), its calls rewritten, a closure made outside passed straight in. A lambda in such a slot lifts as a generic `__c_N<T>` (M6.4b) with its closure value spelled by the slot's type — `Fn(T) -> T{ env: 0, call: __c_N<T> }` — until the second pass names the struct; the slot's type is the callee's parameter type with a generic call's explicit arguments standing in for its type parameters. A capturing lambda in a template rides on the same machinery: its environment struct and maker are templates over the type parameters the captured types mention (`struct __E_N<T>`, `#[caps(syscall)] fn __mk_E_N<T>`), instantiated with the function around them. (A generic struct's fields of function or closure type came next, c3b.) Suite stage [10s] | ✅ gfnclosure.npp lowers with one `__Fn_i64__i64`, `__g_apply_i64(f: __Fn_i64__i64, x: i64)`, a kept `x: str` through `__g___mk_E_0_str`; `ncc` and `ngen` agree; host output correct; a closure returned by a template at two types and a named function passed to a generic call run |
| M6.4c3b | ✅ **function and closure types as fields of generic structs** ([`gstructfn.npp`](../examples/gstructfn.npp)): `struct Pair<T> { first: T, second: Fn(T) -> T }`, `struct Op<T> { name: str, f: fn(T) -> T }` — the generic pass keeps such a field as its type's token span and renders it per instantiation with the parameters substituted (`second: Fn(i64) -> i64`, which the second closure pass names `__Fn_i64__i64`; `f: fn(i64) -> i64`); a lambda in a `Pair<T>{ … }` literal inside a template lifts as a generic closure, one in a concrete `Pair<i64>{ … }` literal lifts plain, and a call through the field is rewritten like any closure-field call. (A generic use inside such a field's type came next, c3c.) Suite stage [10t] | ✅ gstructfn.npp lowers with `__g_Pair_i64 { second: __Fn_i64__i64 }` and `__g_Op_i64 { f: fn(i64) -> i64 }`; `ncc` and `ngen` agree; host output correct; 1 negative refused |
| M6.4c3c | ✅ **a generic use inside a function-typed field of a generic struct** ([`gstructnest.npp`](../examples/gstructnest.npp)): `struct Op<T> { f: fn(T) -> Box<T> }`, `struct Ap<T> { run: Fn(Box<T>) -> T }` — the use inside the field's type is a nested generic use of the struct (M6.3g's records, already collected for a struct body), instantiated with it and rendered as its concrete name (`f: fn(i64) -> __g_Box_i64`). Two rules fell out of it: a closure signature spells a generic type inside by its instantiation's name (`Fn(Box<i64>) -> i64` is `__Fn___g_Box_i64__i64`), so the closure made in plain code and the field it lands in — met by different closure passes — name one struct; and the signature structs are declared after the last struct or enum a signature names by value, since N lays types out in declaration order and a struct field of function type spells its types out. A wrong arity or a non-generic name inside such a field is refused. Suite stage [10u] | ✅ gstructnest.npp lowers with `__g_Op_i64 { f: fn(i64) -> __g_Box_i64 }` and `__g_Ap_i64 { run: __Fn___g_Box_i64__i64 }`, the closure struct declared once after `__g_Box_i64`; `ncc` and `ngen` agree; host output correct; a closure over a generic in plain code and a two-argument generic inside one run; 2 negatives refused |
| M6.4c4 | ✅ **`own` captures as call-once closures** ([`owncap.npp`](../examples/owncap.npp)): the environment inherits must-consume (design §6.2). It took an N rung first — v0.25, "Own fields inside own structs" (spec §4.6: an `own struct` may hold `own` fields; construction moves; a field never moves out alone; a held container's end drops its fields in reverse declaration order, its own `#[drop]` included), in `ncc`, `check.n` (a 75-row corpus) and `gen.n`, with [`ownnest.n`](../examples/ownnest.n) in every differential — and then the shape §6.2 chose: a `:=`-bound lambda that captures an `own` local lowers to `own struct __E_N` (the environment, by value, own fields included), `own struct __O_N { env: __E_N }` (the closure), a lifted `fn __c_N(__self: __O_N, …)` that holds the closure and reads `__self.env.f`, the birth `h := __O_N{ env: __E_N{ f: f } }` (moving `f` in) and every call `h(x)` rewritten to `__c_N(h, x)`, which moves `h` in — so the captures drop when the call's body ends, a second call is a use after move, and an uncalled closure drops at scope end, all judged by N's own checker. Limits: an own capture in an `Fn(…)` slot is refused with the fix named (that environment is by address), and, then, this `:=`-bound form could not be born inside a generic template — closed by M6.4c4b. The `win.on(Event.Click, fn(e) …)` handler sugar over [nwin.n](../examples/nwin.n)'s polled event loop is library design on top of closures rather than lowering, and follows a real N++ window module. Suite stage [10v] | ✅ owncap.npp lowers with `own struct __E_0` / `__O_0`, `__c_0(__self: __O_0, n: i64)`, `report := __O_0{ env: __E_0{ tag: tag, f: f } }`, `r := __c_0(report, 39)`; `ncc` and `ngen` agree; host output shows the capture closing after the call and an uncalled closure closing at scope end; an own capture in an Fn slot refused by nppc; a second call refused by N |
| M6.4c4b | ✅ **the call-once closure born inside a generic template** ([`gowncap.npp`](../examples/gowncap.npp)): the `:=`-bound form of c4 lifts as templates when it sits inside a generic function — `own struct __E_N<T>`, `own struct __O_N<T> { env: __E_N<T> }` and `fn __c_N<T>(__self: __O_N<T>, …)`, over the type parameters the lambda or one of its captures names — with the birth `__O_N<T>{ env: __E_N<T>{ … } }` and every call `__c_N<T>(h, …)` nested generic uses the generic pass instantiates with the enclosing function, as it already did for a template's `Fn` (M6.4b) and `FnOnce` (M6.4c6b) lambdas; the own struct templates ride M6.4c6c. A lambda naming no parameter lifts plain and is called from the template like any function. nppc's last "not supported yet" is gone; an own capture in an `Fn` slot inside a template stays refused with the fix named | ✅ gowncap.npp lowers with `own struct __g___E_0_i64` / `__g___O_0_i64` (and `_str`), `fn __g___c_0_i64(__self: __g___O_0_i64, n: i64)`, the instantiated call `__g___c_0_str(once, 1)`; `ncc` and `ngen` agree; the host trace closes each File as its call ends (`seed 5 fd 3` / `closing 3` / `a = 4` / …); the Fn-slot refusal stands; every other `.npp` byte-identical; the in-OS batch runs it too (25 of 25); stage [10v2] |
| M6.4 showcase | ✅ **handlers as fields** ([`nwinui.npp`](../examples/nwinui.npp)): [nwin.n](../examples/nwin.n)'s polled event loop through closures — widgets with `on_click: Fn(i64, i64)`, a `Ui` with `on_key: Fn(i64) -> bool`, `dispatch(ui, kind, a, b)` routing a click to the button whose rectangle holds it and a key to the handler, each button's lambda capturing its label and corner by value. A library over M6.4c1–c2 with no lowering of its own: one struct per signature, one environment with its maker, the field calls rewritten; on the host the loop reads a scripted event table where nwin.n polls the window. Suite stage [10w] | ✅ nwinui.npp lowers with `__Fn_i64_i64_`, `__Fn_i64__bool`, `__mk_E_0(x, y, label)` and the field calls rewritten; `ncc` and `ngen` agree; the host trace over the scripted events is exact; a capturing lambda in a plain `fn` field refused by nppc |
| M6.4c5 | ✅ **closure calls through indexed reads**: a name that points at a struct — a parameter `bs: *Button`, a local `bs := ui.buttons` (typed from the `Ui` field), or an `as *Button` cast — has the `Fn` fields of the element it indexes called in place: `bs[i].on_click(a, b)` → `bs[i].on_click.call(bs[i].on_click.env, a, b)`, the index text copied verbatim. [`nwinui.npp`](../examples/nwinui.npp)'s `dispatch` now calls the widget it reads out of the table; its `fire` helper is gone. Suite stage [10w] | ✅ nwinui.npp lowers with the indexed call rewritten and no `fire`; every other `.npp` lowers byte-identically to the previous nppc; `ncc` and `ngen` agree; the host trace is unchanged |
| M6.4c6 | ✅ **owning closures — the `FnOnce(A) -> R` slot type** ([`fnonce.npp`](../examples/fnonce.npp)), on N v0.26 ("Behind a raw pointer": a pointer to an own type as a local born from a cast; store moves in, peek moves nothing, take gives a fresh owner — [`ownbox.n`](../examples/ownbox.n)). The lowering is the hand-written closure in ownbox.n: one `#[drop(__fo_drop_<sig>)] own struct __FnOnce_<sig> { env: addr, call: fn(addr, A…) -> R, fin: fn(addr) }` per signature, the lambda's environment an `own struct __E_N` stored through `__mk_E_N` (its own captures moved in), a per-lambda finaliser `__fin_E_N(_env: addr)` that takes the environment out and lets it drop, `__call_once_<sig>(f, x)` running `f.call(f.env, x)` and then the drop, every `f(x)` on a `FnOnce` name rewritten to it. N then gives must-consume, one owner, auto-drop of a closure never called, and "use of 'f' after move" on a second call — an own capture may finally be passed where a closure is expected. Limits: a `FnOnce` field is refused (its holder would need to be own); inside a generic template since M6.4c6b. Suite stage [10x] | ✅ fnonce.npp lowers with `#[drop(__fo_drop_FnOnce_i64__i64)] own struct __FnOnce_i64__i64`, `__call_once_FnOnce_i64__i64`, `own struct __E_1` + `__fin_E_1`, the body peeking `__p[0].f.fd`, `__call_once_…(f, x)` / `(v, 1)`; `ncc` and `ngen` agree; the host trace shows the capture closing after the one call and an uncalled closure closing at scope end; a FnOnce field refused by nppc, a second call refused by N; every other `.npp` byte-identical |
| M6.4c6b | ✅ **FnOnce inside generic templates** ([`gfnonce.npp`](../examples/gfnonce.npp)): a `FnOnce(T) -> T` over a template's parameter lowers like an `Fn` one (c3) — the template's lambda lifts as a template (`__c_N<T>`, `__E_N<T>` + `__mk_E_N<T>`, `__fin_E_N<T>` when the environment owns), instantiated with the template around it; the owning struct is one per concrete signature, declared by the closure pass after the generic pass, which is also where calls through a FnOnce typed over T are rewritten (the earlier pass leaves them alone while the signature names a type parameter). An own capture flows through a generic consumer. Refused: a FnOnce field in a generic struct (judged before instantiation), an own capture born inside a template. Suite stage [10y] | ✅ gfnonce.npp lowers with `__FnOnce_i64__i64` + `__FnOnce_str__str`, `__g_consume_str(f: __FnOnce_str__str, …)`, `__g___mk_E_0_str`, the calls consumed after instantiation, no template signature leaking; `ncc` and `ngen` agree; the host trace shows the File closing after the generic consumer's one call; both refusals name the fix; every other `.npp` byte-identical |
| M6.4c6c | ✅ **own struct templates** — the generic pass carries `own struct S<T>` (the keyword travels with the declaration; every instantiation is an own struct), so a FnOnce lambda born inside a template may capture an own local: its environment is `own struct __E_N<T>`, instantiated with its finaliser per T ([`gfnonce.npp`](../examples/gfnonce.npp)'s `keeper<T>` owns a File per instantiation). Limit then: a capture typed `T` when T is instantiated with an own type (the environment came out plain; N refused the own field inside it) — closed by c6d; a `#[drop]` on a struct template — carried since M6.4d | ✅ gfnonce.npp lowers with `own struct __g___E_0_i64` / `_str` and `__g___fin_E_0_i64` / `_str`; `ncc` and `ngen` agree; the host trace closes each keeper's File after its one call; the former own-capture refusal retired; every other `.npp` byte-identical |
| M6.4c6d | ✅ **an instantiation that receives an own type is own** — a struct template instantiated with an own struct as the type argument of a by-value field (or holding a nested use of an own template) is emitted as `own struct`, N's rule that an own value lives only inside an own container; a lambda's lifted items are templates over the type parameters a capture's type names, not only the ones its signature names; and every capturing FnOnce lambda gets the real finaliser (taking a plain environment out is harmless, and only that shape drops an own one). So a capture typed `T` is own at an own T: [`gfnonce.npp`](../examples/gfnonce.npp)'s `guard<T>(v: T, tag: i64)` — its lambda `fn(x: i64) -> i64` names no T — peeks `v.fd` at its one call; `guard<File>` owns and closes its File, `guard<Handle>` stays plain; `Pair<T> { a: T, b: T }` at File drops both fields at scope end. Returning the capture itself is refused by N as a field move out of an own value. Remaining then: a `#[drop]` on a struct template — carried by M6.4d | ✅ gfnonce.npp lowers with `own struct __g___E_1_File` / `struct __g___E_1_Handle`, `__g___fin_E_1_File` / `_Handle`, the maker `__g___mk_E_1_File(tag: i64, v: File)`; `ncc` and `ngen` agree; the host trace closes the guarded File after its one call; a `Pair<File>` comes out own and drops both Files; every other `.npp` byte-identical |
| M6.4d | ✅ **a destructor on a struct template** ([`gdrop.npp`](../examples/gdrop.npp)): `#[drop(f)]` in front of `own struct S<T>` travels with the template — the attribute is cut out with the declaration, every instantiation is emitted as `#[drop(__g_f_X)] own struct __g_S_X`, and the generic drop function `f<T>` (required to take exactly the template, `fn f<T>(v: S<T>)`, over the same type parameters) is instantiated alongside each instantiation of S whether or not the program names it — an implicit instantiation the nested-use fixpoint adds, so `__g_release_i64` exists for `Guard<i64>` as `__g_Box_i64` exists for a `Box<T>` field. N's rules then apply unchanged: a local reaching its scope end unconsumed is released in reverse birth order, its own fields dropping after the destructor's body (v0.25); a held parameter never auto-drops (the sink rule). Refused with the fix named: `#[drop]` on a plain struct template; a drop function that is not generic, or does not take the template. Suite stage [10z] | ✅ gdrop.npp lowers with `#[drop(__g_release_i64)] own struct __g_Guard_i64` (and `_str`, `_File`) plus `fn __g_release_i64(g: __g_Guard_i64)` for each; `ncc` and `ngen` agree; the host trace releases the three guards in reverse birth order, the `Guard<File>` closing its File right after its release; the three refusals name the fix; every other `.npp` byte-identical; the in-OS batch runs it too (24 of 24) |
| M6.5a | ✅ **modules** ([`modmain.npp`](../examples/modmain.npp) + [`modlib.npp`](../examples/modlib.npp)): a module is a file; a top-level `use "file.npp";` (path relative to the using file) inlines that file's resolved text in its place — once per program (a repeated `use` reduces to a marker comment), cyclic uses refused, a missing file refused — and the generic pass then runs over the whole program, so a template declared in one file instantiates and dedups from another. `pub` marks exports and is dropped from the lowering. The lowered `.n` keeps `// use "…" (inlined by nppc)` / `// end of "…"` markers, so the flat unit stays reviewable; diagnostics after a `use` count lines of the combined text. Suite stage [10i] | ✅ modmain.npp lowers to one flat unit; `ncc` and `ngen` agree on it; host output correct; missing file and cycle refused; a repeated use inlines once |
| M6.5b | ✅ **visibility**: an item — a top-level `fn`, `struct`, or `enum` — is visible in the file that declares it, and `pub` makes it visible program-wide; a name-shaped reference (a call, a construction or generic use, a type slot, a type argument, an `impl` type) from another file to a non-`pub` item is refused with the file named (`'is_even' is private to modlib.npp (mark it pub to use it here)`), the main file's own items included. A module's private items are renamed `__m_<stem>_<name>` at their declaration and at every reference inside the module — a text splice before the generic pass, so a private generic, or a private type as a type argument, works unchanged — and two modules' private `helper`s stay two functions. Each module may declare its own `extern syscall` block (ncc accepts the repeat). [`modlib.npp`](../examples/modlib.npp) keeps `is_even` private. Suite stage [10j] | ✅ modmain.npp lowers with `__m_modlib_is_even`; `ncc` and `ngen` agree; host output correct; a private call from the main file and a module's reference to a main-file item are refused; two modules with a private `helper` each (plus the main file's own) compile and run; a private generic over a private type instantiates |
| M6.5c | ✅ **`pub` scoped to importers**: an exported item is visible to its own file and to the files that `use` its module directly. A use-graph — one edge per `use`, files by id — is recorded during resolution, and a name-shaped reference from a file that does not use the item's module is refused with the fix spelled out (`'bee' is declared by bb.npp, which main.npp does not use (add use "bb.npp";)`): a transitive reach (main uses A, A uses B, main names B's item) and a sibling reach (A names B's item, B used only by main) are both refused. A diamond (A and B both use C) inlines C once and both see it; a file using a module directly as well as through another still sees it. Suite stage [10k] | ✅ transitive and sibling references refused; the diamond program compiles identically under `ncc` and `ngen` and runs; direct-and-through use runs |
| M6.5d | ✅ **re-exports** ([`modutil.npp`](../examples/modutil.npp) + [`modreexp.npp`](../examples/modreexp.npp)): `pub use "file.npp";` inlines the file exactly as `use` does and makes its exports part of the using module's own — a file that uses that module sees them without naming the re-exported one, along any chain of `pub use`s; a plain `use` inside the chain exports nothing onward and the reach is refused with the fix named. The use-graph carries the flag per edge. Modules: nothing pending. Suite stage [10n] | ✅ modreexp.npp reaches put / Maybe<T> / first_even through modutil's `pub use`; `ncc` and `ngen` agree; host output correct; a plain use inside a pub-use chain is refused; a two-level pub-use chain reaches |
| M6.5e | ✅ **own structs across modules**: an exported own struct travels like any item, destructor included — `pub` may stand before `own` and a `#[drop(f)]` attribute (`pub #[drop(f)] own struct S<T>`), the item's visibility is read through them, and the attribute counts as a reference to `f`: a private drop function of the same module is renamed inside the attribute with the module's other privates (`#[drop(__m_lib_release)]`); a drop function of another module cannot arise, since it must name the struct's type and its file would have to use the struct's file and vice versa — a cycle, refused as such. Found while probing M6.4d across files: `pub own struct` in a module was refused outright ("pub must precede fn, struct, enum, or impl"), an exported template with a destructor read as private, and a renamed private drop function left its attribute naming the old name | ✅ the four pub/private combinations of an exported `Guard<T>` and its `release<T>` lower and run from the main file (`made 7` / `release 7`); a non-generic `pub #[drop(close_box)] own struct Box` with a private `close_box` too; every other `.npp` byte-identical; stage [10z] |
| M6.6 | `nppc` self-describes | an `.npp` program written in the dialect's own idioms exercises every rung at once |

The rungs are sized like M5's were: one honest increment each, docs in
the same commit, nothing claimed that a fence does not hold.

### Indexed callback receiver evaluation (NyxOS 6.5.216)

The M6.4c5 implementation originally copied an indexed receiver into both
`.call` and `.env` accesses. An index such as `next(counter)` therefore ran
twice. The current lowering passes the closure value once to a concrete
`__invoke_Fn_...` helper, which reads both fields from its local parameter.
The helper adds no environment allocation and leaves `FnOnce` ownership intact.

`nppc/tests/test_indexed_calls.py` checks seven cases through `ncheck`, identical
`ncc`/`ngen` C, and both host executions; the preceding compiler evaluates the
index twice in all seven cases. NyxOS 6.5.216 also executes seven host-compiled
ELFs with exact serial results and 184 bytes of extracted EXT2 output. The
26-example corpus, Workbench cleanup and module tests pass. This does not add
index bounds checking, arbitrary receiver type inference, `FnMut`, or general
argument evaluation-order semantics. N++ lowering still ran on the host.

### 6.5 M6.6 integration checkpoint

[`workbench.npp`](../examples/workbench.npp) and its
[`workbench/`](../examples/workbench/) modules are a bounded first checkpoint
toward M6.6. This is a small request processor: decode two ASCII digit bytes,
add them, produce a label or an error, publish through a one-shot handler and
cancel an unused handler. It is not a compiler or a self-hosting claim.

| Composition | Exercised by the program |
|---|---|
| Modules and re-exports | One `use` of `api.npp` exposes the public API. Decode and encode both re-export the shared common module through equivalent paths, forming a diamond. |
| Private identities | `decode/util.npp` and `encode/util.npp` deliberately share a basename, a private `Token` type and a private `helper` with different behavior. The exported generic `Guard<T>` has a private generic destructor. |
| Generics | Structs, enums and functions; literal call inference; inferred Result construction; a generic cast; nested `Box<T>` fields and uses; function/closure fields whose signatures name `Box<T>`. |
| Reusable closures | A field captures a bias by value; a returned `Fn` survives its maker and is called twice through a generic consumer. A noncapturing lambda also passes through `apply<str>`. |
| Owning closures | A generic `FnOnce` maker captures an own generic guard containing an own lease. One handler is consumed and one abandoned; both release their captures once. A generic `:=` closure exercises the by-value own form. |
| Errors and cleanup | Generic Result variants, statement and value `match`, same-type and cross-type `?`; defers and destructors on ordinary and error returns. The writer uses a capability-gated syscall with a checked user pointer. |

The runnable fence is
[`test_workbench.py`](../nppc/tests/test_workbench.py):

```sh
python3 lang/nppc/tests/test_workbench.py --examples
```

On Linux x86_64 or WSL it bootstraps the tools from their checked-in sources,
lowers the program twice to verify deterministic output, and requires `ncc`
and the self-hosted `ngen` to emit identical C from that N. Both C outputs
compile as freestanding C99 and run with the Linux host shim. An exact output
oracle checks success, both error paths, and resource lifetime/order, while
the self-hosted checker independently accepts the lowered program. The
optional corpus pass repeats the differential and execution comparison on
the twenty-five preceding N++ entry points. `CC` selects the C compiler.

The first integrated run exposed a missing cleanup path in N: `?` returned
an Err after defers without dropping live locals or held parameters' own
fields. It also failed to reject destructor-less locals whose only consume
was after `?`. `ncc` and the N checker/emitter now handle that implicit exit
like an explicit return. They snapshot ownership after evaluating the
operand, apply defer/drop/leak handling for Err, and restore that state for
Ok. The fence includes all three accepted statement forms, same and
rewrapped Result types, reverse drop order, held fields, values moved into
the operand or assignment target, shadowing payload bindings, and refused
leaks. An assignment target's moves count only on Ok, and a payload's source
name is bound after Err cleanup so it cannot hide an outer owner. Running
the fence with an earlier `--ncc` fails
because `release 11` is missing. The packaged `ncc` source and generated
`ngen` translation unit follow the repaired sources.

The generated ELF also ran inside NyxOS 6.5.190 under QEMU using the target
runtime and `crt0` build documented in the ncc README. Its 301-byte trace
matched the oracle both on the console and through file redirection, with
exit status 0 and no kernel fault. In a further NyxOS 6.5.191 QEMU check,
freshly installed `ncc` and `ngen` packages compiled the lowered 7,852-byte
N source inside the OS. Both generated C files matched the host reference;
the in-OS `cc` linked both, and both executions returned 0 with the exact
301-byte trace and no kernel fault. N++ to N lowering remained a host step.

**M6.6 remains open.** This checkpoint does not exercise every historical
sub-rung (for example nested lambdas and indexed closure fields retain their
separate examples). `FnMut` is not implemented; the example uses the actual `Fn` and
`FnOnce` types. Mutable borrowed captures and `FnOnce` fields are not added.
The existing bump-allocated closure environments are not reclaimed, and
module path identity is lexical rather than symlink-aware. The separate
privacy and ownership refusal cases hold these boundaries without expanding
the language surface.
