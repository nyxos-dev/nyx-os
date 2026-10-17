# nppc — the N++ front-end compiler

`nppc` compiles N++ (`.npp`) by lowering it to **N source**; the verified N
pipeline — `ncc`, or the self-hosted toolbox (`nlex`/`nparse`/`ngen`) —
carries the lowered program to C and into NyxOS. The shape decision and the
staged plan live in [`../docs/design-npp.md`](../docs/design-npp.md) §6: the
lowered `.n` is checked by ncc's own type/ownership/capability checker, so
the front-end always works above a soundness net that is itself held
byte-faithful by the selfhost differentials.

```
file.npp ──nppc──► file.n ──ncc / toolbox──► C ──cc/tcc──► ELF
```

The right-hand end of that pipeline is real, not a diagram: the earlier
corpus of twenty-five programs in [../examples](../examples) has had its
lowered N compiled and run inside NyxOS by the in-OS `ncc`
(`xbm install ncc`, then `ncc /mnt/X.n -o /mnt/X_gen.c`,
`cc /mnt/X_gen.c /mnt/nyxrt.c -I/mnt -o /mnt/bin/nX`, `nX`), each
printing exactly what its host run prints — see *verified three ways* in
[../README.md](../README.md) and the transcript in
[../docs/design-npp.md §6.3](../docs/design-npp.md).

## Build and use

```
gcc -O2 -Wall -Wextra -o nppc lang/nppc/nppc.c
./nppc program.npp -o program.n
```

One file, C99, no dependencies — the `ncc` discipline.

The focused module regression runs on Linux x86_64 (including WSL), using
Python 3 and a C99 compiler:

```sh
python3 lang/nppc/tests/test_modules.py
```

It compiles and runs modules with colliding basenames and sanitized stems,
checking their private functions, types, generic destructors, nested/diamond imports,
equivalent paths and located cycle errors. Set `CC` to select another compiler.

The bounded M6.6 integration checkpoint adds a small multi-module request
processor, [`workbench.npp`](../examples/workbench.npp). It parses two digit
bytes, combines generic value transformations, publishes a result through an
owning closure, and cancels another. Its Linux/WSL test builds `nppc`, `ncc`,
and the N-written `ngen` and `ncheck`; compares the emitted C byte-for-byte;
and compiles and executes both outputs against an exact cleanup trace:

```sh
python3 lang/nppc/tests/test_workbench.py
python3 lang/nppc/tests/test_workbench.py --examples
```

`--examples` also checks the twenty-five earlier N++ programs through both
emitters and compares their host execution (normalizing only hello's pid).
The generated workbench ELF has also run inside NyxOS 6.5.190 under QEMU:
its 301-byte trace matched the oracle on the console and through file
redirection, with exit status 0. A further QEMU check on NyxOS 6.5.191
installed `ncc` and `ngen` from their packages, used both inside NyxOS to
compile the lowered 7,852-byte N source, and linked both outputs with the
in-OS `cc`. Both emitted C files matched the host reference byte-for-byte;
both executables returned 0 with the exact 301-byte trace. N++ to N lowering
still ran on the host for this check.
M6.6 remains open: this is a documented combination of implemented
features, not an N++ compiler written in N++, nor a claim to exercise every
milestone. See the coverage and limits in
[design §6.5](../docs/design-npp.md#65-m66-integration-checkpoint).

The checkpoint caught an N ownership bug: the Err return of `?` ran defers
but skipped local destructors, held parameters' own fields, and the leak
check. Both compiler implementations now apply ordinary return cleanup to
that branch, preserving the post-operand ownership state for the continuing
Ok branch. The test covers binding, assignment and expression forms, equal
and different Result types, moved values, held fields and destructor-less
leaks, including moves in assignment targets and bindings that shadow an
outer owner. `--ncc /path/to/older/ncc` demonstrates the original missing
`release 11` in the workbench trace. Private access, an own capture in `Fn`, and a
second call of `FnOnce` are also required to fail.

## What this rung is (M6.2), plainly

The accepted dialect is **exactly the N subset**, and lowering is the
**identity**: the input is written out byte-for-byte. What makes the
skeleton real is the read side — the whole file is lexed with a faithful
copy of ncc's lexer (same tokens, escapes, interpolation mode stack,
attributes, and diagnostics), so everything `nppc` accepts is something
the N pipeline lexes identically, and everything it refuses would have
been refused there too. This holds the founding contract mechanically:

> Every valid N program is a valid N++ program, with identical behavior.

[`../examples/hello.npp`](../examples/hello.npp) is that contract as a
file — `hello.n`'s content under the `.npp` extension — and the suite's
stage [10] is the day-one differential from design §6.3: nppc lowers it,
the output must equal the `.n` byte-for-byte, and then **both** `ncc` and
the self-hosted `ngen` compile the lowered program and their C must agree.

Each M6.3+ rung (monomorphized generics, closures, modules) replaces a
slice of the identity with a real transform — new syntax lands in this
lexer and lowering first, never in `ncc`.

## Generics (M6.3), plainly

`nppc` monomorphizes. A generic `struct`, `fn`, or `enum` is a template;
each distinct use becomes one plain N item with a mangled name
(`Box<i64>` → `__g_Box_i64`, `Result<str, i64>` → `__g_Result_str_i64`),
every use is rewritten to that name, and everything else — comments,
spacing, the bodies themselves — is spliced through verbatim. It is a
token-span rewrite over the lexed file, no parse tree yet, and the
lowered `.n` is then held to the N checker like any hand-written N. A
concrete item is emitted where its template is declared, so a type a
template will be instantiated with is declared before the template, as
any struct used by value would be. A struct template is `own` at an
instantiation that receives an own struct as the type argument of a
by-value field, or holds a nested use of an own template — `Pair<T> {
a: T, b: T }` at `File` is `own struct __g_Pair_File`, and drops both
Files at scope end — which is N's rule that an own value lives only
inside an own container, decided per instantiation (M6.4c6d); an `own
struct` template is own at every instantiation (M6.4c6c).

| Rung | Example | What lowers |
|---|---|---|
| M6.3a | [`box.npp`](../examples/box.npp) | generic structs, explicit type arguments |
| M6.3b | [`genfn.npp`](../examples/genfn.npp) | generic functions: signature type slots substituted, body verbatim |
| M6.3c | [`gcast.npp`](../examples/gcast.npp) | `x as T` inside a generic body — the one type slot N bodies have |
| M6.3d | [`ginfer.npp`](../examples/ginfer.npp) | call-site inference: `id(41)` is `id<i64>(41)` (literal arguments) |
| M6.3e | [`genum.npp`](../examples/genum.npp), [`result.npp`](../examples/result.npp) | generic enums; `Result<T, E>` composing with N's structural `?` |
| M6.3f | [`rinfer.npp`](../examples/rinfer.npp) | construction-site inference: `Result.Ok{ ... }` from the enclosing return type |
| M6.3g | [`gnest.npp`](../examples/gnest.npp) | generics inside generics: `wrap<T> -> Box<T>` instantiates `Box` per `wrap` instantiation |
| M6.3h | [`gfield.npp`](../examples/gfield.npp) | generic struct fields of generic type: `Pair<T> { a: Box<T> }` instantiates `Box` per `Pair` instantiation |

Inference never guesses. A call whose argument `nppc` cannot type, or a
bare construction in a function that does not return that enum, is
refused with the explicit form spelled out in the diagnostic. A template
may use other templates with its own parameters (`wrap<T> -> Box<T>`):
the use is recorded on the template and instantiated, to a fixpoint,
once per concrete instantiation of it, with the arguments substituted —
in a function's signature or body, an enum's payload, or a struct's
field (`Pair<T> { a: Box<T> }`) alike. Generics compose; nothing is
pending on that front.

## Modules (M6.5), plainly

A module is a file. A top-level `use "lib.npp";` inlines that file's
text in its place — the path is relative to the using file — and the
program the generic pass sees is the flat result, so a template declared
in one file and instantiated in another monomorphizes and dedups exactly
as it would in one file. The rules are the ones design §6.2 names:

- **once per program** — a second `use` of the same file, from anywhere,
  leaves only a marker comment. Paths are normalized lexically before opening
  or resolving them: `./lib.npp`, `dir/../lib.npp` and `lib.npp` identify the
  same module, and repeated `/` separators collapse. The initial source path
  follows the same rule. Leading `..` components remain in relative paths;
  absolute paths cannot climb above `/`. This is a lexical path contract:
  it does not resolve symlinks, fold filename case, or equate relative paths
  with absolute paths through the host's working directory;
- **cycles refused** — a file that names one still being resolved is an
  error at the offending `use`;
- **an item is visible in its file; `pub` makes it visible to the files
  that `use` its module.** A top-level `fn`, `struct`, or `enum` without
  `pub` is private to the file that declares it — the main file's own
  items included — and a reference to it from any other file is refused
  with the file named: `'is_even' is private to modlib.npp (mark it pub
  to use it here)`. An exported item is visible to the files that `use`
  its module *directly*: a file that reaches the module only through
  another `use`, or a sibling that never uses it, is refused with the fix
  spelled out: `'bee' is declared by bb.npp, which main.npp does not use
  (add use "bb.npp";)`. A diamond — two modules using the same third —
  inlines it once and both see it. `pub use "lib.npp";` re-exports: the
  used module's exports become part of this module's own, so a file that
  uses this module sees them too, along any chain of `pub use`s; a plain
  `use` inside the chain exports nothing onward. "Reference" means a
  name-shaped use:
  a call `x(`, a construction or generic use `x.` / `x{` / `x<`, a type
  slot, a type argument, an `impl` type. A field or method after `.`, or
  a binding or parameter name, is not one.
- **private items never collide.** Each non-`pub` item of a module is
  renamed `__m_<id>_<stem>_<name>` (`__m_1_modlib_is_even`) at its declaration
  and at every reference inside the module, so two modules' private
  `helper`s are two functions in the lowered N. The file id is assigned in
  import order and keeps names distinct even when modules have the same
  basename or names such as `a-b.npp` and `a_b.npp`. The renaming is a text
  splice done before the generic pass, which then sees the mangled names
  as ordinary ones — a private generic, or a private type used as a type
  argument, needs nothing special. The main file's items keep their
  names, and `pub` itself is dropped from the lowering (N has no
  visibility). Each module may declare its own `extern syscall` block.

The lowered `.n` keeps `// use "lib.npp" (inlined by nppc)` and
`// end of "lib.npp"` markers around each inlined file, so the flat unit
stays reviewable. Diagnostics after a `use` count lines of the combined
text. [`../examples/modmain.npp`](../examples/modmain.npp) and
[`../examples/modlib.npp`](../examples/modlib.npp) are the two-file
program the suite's stages [10i]–[10n] hold — lowered, agreed on by
`ncc` and `ngen`, run on the host, with a missing file, a cycle, a
private call, and an unreached export refused;
[`../examples/modreexp.npp`](../examples/modreexp.npp) reaches modlib
through [`../examples/modutil.npp`](../examples/modutil.npp)'s `pub use`.
An own struct crosses a module boundary like any item, destructor
included (M6.5e): `pub #[drop(f)] own struct S<T>` exports the template
with its attribute (`pub` may stand before `own` and the attribute), a
`#[drop(f)]` counts as a reference to `f` — a private drop function of
the same module is renamed inside the attribute with the rest of the
module's privates (the drop function always lives beside its struct: it
names the struct's type, so no other file could declare it without a
`use` cycle) — so an exported `Guard<T>` released by a private
`release<T>` instantiates from the main file exactly as it would from
its own. Nothing is pending on the module front.

## Closures (M6.4), plainly

A lambda is a function literal where a value goes:

```
each(fn(x: i64) -> i64 { x * x }, 1, 2, 3);
op := Op{ name: "inc", run: fn(x: i64) -> i64 { x + 1 } };
```

`fn ( params ) [-> type] { body }` in an expression position — a call
argument, a struct-literal field, the right-hand side of a binding, a
return value. nppc lifts each one to a top-level function named `__c_N`
(numbered in the order they are lifted) placed right after the item
before the one the lambda appears in, and the expression becomes that
name — a function value, which N v0.24's function types then type
exactly like a named function passed by name. The body is spliced
through verbatim; the lowered N of the first line above is
`fn __c_0(x: i64) -> i64 { x * x }` followed by `each(__c_0, 1, 2, 3)`.
A lambda is told from a function type by its body: `fn(i64) -> i64` in
a type slot has none.

The rules at this rung (M6.4a):

- **a lambda in a closure slot captures by value (M6.4c2).** Where an
  `Fn` type is expected (below), a lambda may name the enclosing
  function's locals. Each is copied into an environment struct `__E_N`
  when the closure is born — a generated maker `__mk_E_N` puts the copy
  on the bump heap (`sys_sbrk`, 16 bytes per field, never freed; nppc
  declares the syscall when the program does not) and the closure
  carries its address as `env` — and the lifted function reads them back
  as `__e.name` (a captured closure is called through its `call` field),
  so the closure may outlive its frame: `adder(5)` returns one. A
  captured local's type must be evident to a token scan — a declared
  parameter, a literal (`i64`, `str`, `bool`), a call to a known
  function, a struct literal (a generic one too: `b := Box<i64>{ … }`
  types `b` as `Box<i64>`, which the generic pass then concretes), or a
  name so typed; anything else is
  refused: `cannot capture 'n': its type is not evident — bind it with a
  literal, a call or a struct literal, or pass it as a parameter`. An
  `own` local cannot be captured here — this environment lives behind a
  pointer, which N refuses for own values; it can be captured by a
  `:=`-bound lambda, the call-once closure below (M6.4c4).
- **elsewhere, lambdas capture nothing.** In a plain `fn(...)` slot or a
  `:=` binding there is no environment to keep a capture in — the one
  exception being a `:=`-bound lambda that captures an `own` local
  (M6.4c4, below): naming any other local of the enclosing function
  there is refused with the fix named.
  (A local a token scan cannot see, such as a `match` arm's bind, is
  refused by N instead, as an undeclared variable in the lifted
  function.) Inside a generic template a capture works the same way: the
  environment struct and its maker come out as templates over the type
  parameters the captured types mention (`keep<T>` keeps an `x: T` in
  `__E_N<T>`, made by `__mk_E_N<T>`), instantiated with the function
  around them (M6.4c3).
- **lambdas nest.** The pass runs to a fixpoint, innermost first, so a
  lambda inside a lambda is lifted before the one around it copies its
  text.
- **inside a generic template, a lambda is generic too (M6.4b).** A
  lambda in the body of `fn boxed<T>` that names `T` lifts to a template
  of its own — `fn __c_N<T>(v: T) -> Box<T> { … }`, over exactly the type
  parameters it mentions — and the expression becomes the use `__c_N<T>`:
  a nested generic use like `Box<T>`, instantiated once per concrete
  instantiation of the enclosing function and rewritten to the concrete
  name (`__g___c_N_i64`), which N passes as a function value. A lambda
  that names no type parameter lifts as a plain function. (A type
  parameter inside a function type, `f: fn(T) -> Box<T>`, is a handled
  type slot of the generic pass since this rung.)
- **a `:=`-bound lambda needs a declared function type.** N binds a
  function value only when its type is declared somewhere in the program
  (a parameter, a field, or a return type of that signature) — the rule
  N's spec §3.4 states; the lowering adds nothing to it.

**The closure type `Fn` (M6.4c1).** N's `fn(i64) -> i64` is a bare
function: it has nowhere to keep an environment, so a closure needs a
type of its own. `Fn(i64) -> i64` is that type — it may appear in any
type slot (a parameter, a struct field, a return type) — and nppc lowers
each distinct signature to one N struct:

```
struct __Fn_i64__i64 {
    env: addr,
    call: fn(addr, i64) -> i64,
}
```

declared once, ahead of the program's first struct or function — or,
when the signature names a struct or enum by value, right after the
last one it names, since N lays types out in declaration order; the
struct's name is the signature's spelling with its punctuation as
underscores and a generic type inside spelled as its instantiation's
name (`Fn()` is `__Fn__`, `Fn(*u8, Box<i64>) -> bool` is
`__Fn_pu8___g_Box_i64__bool` — the same name whichever pass meets it). A call through a closure — a parameter of
`Fn` type, a local bound from a call returning one, the `Fn` field of
a struct-typed parameter or local, or that field of the element a
pointer-to-struct name indexes, `bs[i].f(a)` (M6.4c5) — becomes
`f.call(f.env, a)`; on a `FnOnce` name it becomes `__call_once_…(f, a)`,
which consumes the closure (M6.4c6, below). A lambda
written where a closure is expected takes the environment as its first
parameter and the expression becomes the closure value
`__Fn_i64__i64{ env: 0, call: __c_N }`; a named function passed there is
wrapped in an adapter of the same shape (`fn __c_N(_env: addr, a0: i64)
-> i64 { dbl(a0) }`). The environment is 0 when the closure captures
nothing; a capturing one carries its environment struct's address (the
capture bullet above).
[`../examples/closurety.npp`](../examples/closurety.npp) is the worked
example, held by stage [10q];
[`../examples/capture.npp`](../examples/capture.npp) captures, stage
[10r].

**`Fn` inside generic templates (M6.4c3).** An `Fn` that names a type
parameter of the template around it — `fn apply<T>(f: Fn(T) -> T, x: T)`
— is left as written while the generic pass instantiates the template,
and the closure pass runs once more over the result: `Fn(i64) -> i64`
inside `__g_apply_i64` is concrete then, so it becomes the same
`__Fn_i64__i64` a plain function uses (declared once, whichever pass
meets it first), its calls are rewritten, and a closure made outside
(`inc := adder(1); twice<i64>(inc, 40)`) passes straight in. A lambda in
such a slot lifts as a generic `__c_N<T>` and its closure value is
spelled with the slot's type until that second pass names the struct —
`Fn(T) -> T{ env: 0, call: __c_N<T> }` — where the slot's type is the
callee's parameter type with the call's explicit arguments standing in
for the callee's type parameters (`apply<i64>(fn(v: i64) -> i64 { … },
40)` reads `Fn(i64) -> i64`, and a lambda in `main` stays plain). A
named function passed to a generic call gets its adapter in that second
pass.
[`../examples/gfnclosure.npp`](../examples/gfnclosure.npp) is the worked
example, held by stage [10s].

**Function and closure types as fields of generic structs (M6.4c3b).**
A generic struct's field may be a function type or a closure type that
names the struct's type parameters — `struct Pair<T> { first: T,
second: Fn(T) -> T }`, `struct Op<T> { f: fn(T) -> T }`. The generic
pass keeps such a field as the type's token span and spells it out per
instantiation with the parameters substituted, so `__g_Pair_i64` reads
`second: Fn(i64) -> i64` and the closure pass names it `__Fn_i64__i64`
like any other slot; a lambda in a `Pair<T>{ … }` literal inside a
template lifts as a generic closure, one in a concrete `Pair<i64>{ … }`
literal lifts plain, and a call through the field, `p.second(0)`, is
rewritten like a call through any closure field.
[`../examples/gstructfn.npp`](../examples/gstructfn.npp) is the worked
example, held by stage [10t]. Such a field's type may name another
generic over the struct's parameters (M6.4c3c) — `struct Op<T> { f:
fn(T) -> Box<T> }`, `struct Ap<T> { run: Fn(Box<T>) -> T }`: the use is
a nested generic use of the struct, instantiated with it (`Op<i64>`
brings `Box<i64>` into being and reads `f: fn(i64) -> __g_Box_i64`), and
a closure type that names a generic spells it by that concrete name, so
the closure made in `main` and the field it lands in agree on their
struct. A use with the wrong arity, or of a name that is not generic, is
refused as anywhere else.
[`../examples/gstructnest.npp`](../examples/gstructnest.npp) is the
worked example, held by stage [10u].

**Own captures — call-once closures (M6.4c4).** A capture by value puts
its copy behind the environment's pointer, and N lets no `own` value
sit behind a pointer: an own handle captured that way would have two
owners. So an own capture takes the other shape N v0.25 allows — an own
container by value — and pays for it with a call count. A lambda bound
with `:=` that captures an `own` local becomes a **call-once closure**:
its environment is `own struct __E_N { f: File, … }`, holding every
capture (own fields included) by value; the closure value is `own
struct __O_N { env: __E_N }`; the lifted function holds the closure,
`fn __c_N(__self: __O_N, n: i64) -> i64`, and reads its captures as
`__self.env.f`; the binding is the struct literal `h := __O_N{ env:
__E_N{ f: f } }` — which moves `f` in — and every later call `h(39)` in
that function becomes `__c_N(h, 39)`, which moves `h` in. Everything
else N polices for free: when the lifted function's body ends, its held
closure drops through its fields, so the captured owns are consumed
right after the call ("the environment inherits must-consume"); a
second `h(…)` is a use after move; a closure never called drops at
scope end through its captures; the body may peek at an own capture but
not move it out (v0.25's rule on fields). The limit: such a closure
cannot be passed where an `Fn(…)` is expected (that slot's environment
is by address, non-owning — the capture is refused with the fix named).
Born inside a generic template, this `:=`-bound form lifts as templates
too (M6.4c4b): `own struct __E_N<T>`, `own struct __O_N<T> { env:
__E_N<T> }` and `fn __c_N<T>(__self: __O_N<T>, …)`, over the type
parameters the lambda or one of its captures names, with the birth
`__O_N<T>{ env: __E_N<T>{ … } }` and every call `__c_N<T>(h, …)`
nested generic uses the generic pass instantiates with the enclosing
function — exactly as it does for a template's `Fn` and `FnOnce`
lambdas; a lambda that names no parameter lifts plain and is called
from the template like any function.
[`../examples/gowncap.npp`](../examples/gowncap.npp) is the worked
example (`keep<T>` captures its `seed: T` and a File born beside it; the
File closes as each call ends), held by stage [10v2].
[`../examples/owncap.npp`](../examples/owncap.npp) is the worked
example, held by stage [10v].

**Handlers as fields — the nwin sugar.** What the closure rungs were
for: [`../examples/nwinui.npp`](../examples/nwinui.npp) is
[nwin.n](../examples/nwin.n)'s polled event loop expressed through
closures, and it needs nothing beyond M6.4c1–c2. A widget is a struct
with a handler field, `Button { label, x, y, w, h, on_click: Fn(i64,
i64) }`; the `Ui` keeps a table of them (sbrk'd, 64 bytes each — N lays
a Button out as the str, four i64, and the closure's address and
function pointer) and a key handler `on_key: Fn(i64) -> bool` whose
answer is whether the loop goes on; `dispatch(ui, kind, a, b)` routes a
click to the button whose rectangle holds it and a key to the handler.
Each button's lambda captures its label and its corner by value, so the
handler prints the click relative to the button — no callback registry,
no switch on names, no shared mutable state. It lowers to exactly the
shapes above: one struct per signature (`__Fn_i64_i64_`,
`__Fn_i64__bool`), one `__E_0 { x, y, label }` with its maker, two
lifted functions, and the field calls rewritten — `b.on_click(x, y)`
becomes `b.on_click.call(b.on_click.env, x, y)`; the widget structs are
plain N structs, and the same `dispatch` would sit in nwin.n's loop as
`running = dispatch(ui, ev[0], ev[1], ev[2]);`. The call reaches the
widget where it lies: `dispatch` reads `bs := ui.buttons` and calls
`bs[i].on_click(a, b)` on the button whose rectangle holds the click —
nppc types `bs` as a pointer to Buttons from the `Ui` field it was read
from (M6.4c5, below), so the indexed call is rewritten like any other.
On the host, where the window syscalls do
not exist, the program feeds dispatch a scripted table of events and
prints the trace that stage [10w] checks; the natural mistake — a
capturing lambda in a plain `fn(i64, i64)` field — is refused with the
M6.4c2 text.

**Closure calls through indexed reads (M6.4c5).** A name that points
at a struct joins the names whose `Fn` fields are called through
`.call`: a parameter `bs: *Button`, a local `bs := ui.buttons` whose
type is the `*Button` field of a struct-typed name, or a local cast
`bs := sys_sbrk(n) as *Button`. Where such a name is indexed and a
closure field called — `bs[i].on_click(a, b)` — the call becomes
`bs[i].on_click.call(bs[i].on_click.env, a, b)`, the index copied
verbatim (brackets balanced, so `bs[idx[k]]` works too). The scan is
the token scan the other shapes use: it types nothing it cannot see
(an index on a `*i64`, a field chain before the index, `ui.buttons[i]`)
and leaves those calls alone for N to judge. nwinui.npp's `dispatch`
is the worked case, held by stage [10w].

**Owning closures — `FnOnce` (M6.4c6).** `FnOnce(A…) -> R` is the
closure type that owns: a lambda in a FnOnce slot (a parameter, a
local bound from a call, a return type) may capture `own` locals, the
closure value is itself an own value — must-consume, one owner — and
its one call consumes it. It lowers to the shape
[`../examples/ownbox.n`](../examples/ownbox.n) writes by hand, on N
v0.26's "own values behind a raw pointer": one own struct per
signature, `#[drop(__fo_drop_FnOnce_i64__i64)] own struct
__FnOnce_i64__i64 { env: addr, call: fn(addr, i64) -> i64, fin: fn(addr) }`,
its drop function calling the finaliser, and
`__call_once_FnOnce_i64__i64(f, a0)` running `f.call(f.env, a0)` and
then the drop — all declared with the `__Fn_` block. A capturing lambda
there gets an **own environment**, `own struct __E_N` (the own
captures moved in by `__mk_E_N` at the closure's birth, as the maker's
arguments), a finaliser `__fin_E_N(_env: addr)` that takes the
environment back out of the heap — `__p := _env as *__E_N; __e :=
__p[0];` — so its captures drop as `__e` ends, and a lifted body that
only **peeks** through the pointer (`__p[0].f.fd`, never a take: a take
would end the captures after the first call). A lambda capturing
nothing gets `env: 0` and a no-op finaliser; so does a named function
passed where a FnOnce is expected, through the usual adapter. Every
call `f(x)` on a FnOnce parameter or local becomes
`__call_once_…(f, x)`, which moves `f` in: N then supplies the rest —
a second call is *use of 'f' after move*, a FnOnce never called drops
at scope end through its finaliser, and a FnOnce handed to an `Fn`
slot is a type mismatch. The limit at this rung: a `FnOnce` **field**
is refused (*a FnOnce field needs an own struct holding it (N v0.25) —
pass the closure as a parameter instead (M6.4c6)*), in a generic struct
too — judged before the template is ever instantiated. The own-capture
refusal in an `Fn` slot now names both fixes: *bind the lambda with :=
and call it once, or make the slot FnOnce(...)*.
[`../examples/fnonce.npp`](../examples/fnonce.npp) is the worked
example, held by stage [10x].

**FnOnce inside generic templates (M6.4c6b).** A `FnOnce(T) -> T`
over a template's type parameter lowers the way an `Fn` one does
(M6.4c3): the template's lambda lifts as a template of its own —
`__c_N<T>`, its environment `__E_N<T>` with the maker `__mk_E_N<T>`,
and, when the environment owns something, a finaliser `__fin_E_N<T>` —
instantiated with the template that uses them, so `keeper<i64>` and
`keeper<str>` each get their own copies; the owning struct is one per
CONCRETE signature (`__FnOnce_i64__i64`, `__FnOnce_str__str`),
declared by the closure pass that runs after the generic pass, which is
also where a call through a FnOnce parameter or local typed over T is
rewritten into `__call_once_…` — the closure pass before it leaves
`f(x)` alone while the signature still names a type parameter, because
the struct it would name does not exist yet. An `own` capture flows
through a generic consumer unchanged (`consume<i64>` handed a closure
that owns a File closes it after the one call). An `own` capture in a
lambda born INSIDE a template works too (M6.4c6c): the generic pass
carries `own struct` templates — the keyword travels with the
declaration and every instantiation is an own struct — so such a
lambda's environment is `own struct __E_N<T>`, instantiated as
`own struct __g___E_N_i64` with its finaliser `__g___fin_E_N_i64`;
`keeper<T>` in the example owns a File per instantiation and closes it
after the one call. A capture typed `T` itself may be own at some
instantiation (M6.4c6d): nppc cannot see that at the lambda — `T` is
not an own struct of the program — so the environment template comes
out plain, and the generic pass decides per instantiation: a struct
template instantiated with an own struct as the type argument of a
by-value field (or holding a nested use of an own template) is emitted
as `own struct`, N's rule that an own value lives only inside an own
container. Two details make it hold. The lifted items are templates
over the type parameters a capture's TYPE names, not only the ones the
lambda's own signature names — `guard<T>(v: T, tag: i64)`'s lambda is
`fn(x: i64) -> i64` and names no T, yet its environment is `__E_N<T>`.
And every capturing FnOnce lambda gets the real finaliser
`__fin_E_N<T>`: taking a plain environment out is harmless, and only
that shape drops an own one, so the no-op finaliser is kept for the
lambda that captures nothing and for a named function adapted into the
slot. `guard<File>` in the example thus owns its File and closes it
after the one call, while `guard<Handle>` stays a plain struct; the
body may peek a field of the capture (`v.fd`), and returning the
capture itself is refused by N (*cannot move field 'v' out of the
pointee '__p[0]' — take it as a whole first, e := __p[0] (v0.26)*) —
the environment still owns it.

**A destructor on a template (M6.4d).** `#[drop(f)]` in front of
`own struct S<T>` travels with the template: the attribute is cut out
with the declaration, every instantiation is emitted as
`#[drop(__g_f_X)] own struct __g_S_X`, and the generic drop function
`f<T>` — which must take exactly the template, `fn f<T>(v: S<T>)`,
over the same type parameters — is instantiated alongside each
instantiation of S whether or not the program ever names it: an
implicit instantiation the nested-use fixpoint adds. N then applies
its own rules unchanged — a local that reaches its scope end
unconsumed is released, in reverse birth order, its own fields
dropping after the destructor's body (v0.25), and a held parameter
never auto-drops (the sink rule). A `#[drop]` on a plain struct
template, and one whose drop function is not generic or does not take
the template, are refused with the fix spelled out.
[`../examples/gdrop.npp`](../examples/gdrop.npp) is the worked example
— three guards, one of them owning a File — held by stage [10z].
[`../examples/gfnonce.npp`](../examples/gfnonce.npp) is the worked
example, held by stage [10y].

[`../examples/closure.npp`](../examples/closure.npp) is the worked
example: four lambdas — an argument, a struct field, another argument,
a binding — lifted to `__c_0`…`__c_3`, held by the suite's stage [10o]
(lowered, agreed on by `ncc` and `ngen`, run on the host; nested lambdas
run; a capture refused).
[`../examples/gclosure.npp`](../examples/gclosure.npp) is the generic
case, stage [10p]: a lambda in `boxed<T>` lifts to `__c_0<T>` and comes
out as `__g___c_0_i64` and `__g___c_0_str`; one in `count<T>` that names
no type parameter lifts plain.
