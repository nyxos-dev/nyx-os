# lang/selfhost — ncc, rewritten in N

The module ladder toward M5 (`ncc` self-hosted in N), named in
[docs/selfhost.md](../docs/selfhost.md) under "The mountain after the
toy": **`lex.n` → `parse.n` → `check.n` → `gen.n`** — each of ncc's
passes rewritten as a real N program, compiled by ncc, and held to
ncc's actual behavior **differentially**.

## Input paths and bounded reads

All four drivers accept a source path as their first argument:
`nlex file.n`, `nparse file.n`, `ncheck file.n`, `ngen file.n`. Relative
paths and quoted paths containing spaces work. Further arguments remain
ignored for compatibility. An explicit path that cannot be opened returns
1 and names the tool and path; it never falls back to another file.

With no argument, each driver keeps its `/tmp/n_<tool>_target.n` convention
(`lex`, `parse`, `check`, or `gen`). If that fallback cannot be opened, the
historical skip message and exit status 0 remain unchanged. This includes
ngen's historical `(no check target - skipped)` wording. Empty files still
produce no output and return 0.

The accepted source limit is **262144 bytes (256 KiB), inclusive**, for all
four tools. Reads are capped to the remaining capacity and accept short
reads; at the exact limit a one-byte EOF probe uses an existing byte of the
buffer. An extra byte, a read error (even after receiving valid data), or a
failed initial buffer allocation closes the descriptor and returns 1,
without parsing or emitting partial compiler output. Input diagnostics use
stdout, like the tools' existing diagnostics. Lex/parse retain their 256 KiB
allocation; check/gen retain 512 KiB, leaving the second half available for
the synthesized names appended after the source. Check/gen therefore enforce
a smaller source limit than their previous nominal 512 KiB buffer size;
their old read loop was unbounded and left no guaranteed space for names.
This bounds source input;
it does not enlarge or audit the parser's other fixed arenas and tables.

The reproducible Linux x86_64/WSL fence is:

```sh
python3 lang/selfhost/tests/test_driver_io.py --corpus
```

It builds all four N sources through ncc, checks explicit and fallback
paths, empty files, boundary sizes, short reads, and failures after data or
at allocation. A temporary instrumented host shim asserts read-buffer
bounds and descriptor closure; the checked-in runtime is unchanged.
`--corpus` compares tokens, AST, diagnostics and generated C with ncc over
the N examples and all four selfhost sources, plus the checker rejection
corpus. Comparisons are byte-for-byte. Use `CC` to select the C compiler or
`--sources /path/to/older/selfhost` to demonstrate the pre-fix failures.

## lex.n — the lexer (rung 1: the stream)

`lex.n` is ncc's `next_token`, in N: the whitespace/comment skip
(nested block comments, depth-counted), the 22-keyword table, integers
(hex, `_` separators), strings with escapes and the **interpolation
brace stack** (16 deep, `T_INTERP_R` on a hole's close), the full
operator switch, and the three attributes — every kind number ncc's
own, `T_EOF` included.

**The contract**: `lex.n` reads `/tmp/n_lex_target.n` and prints one
`kind line` pair per token. Its output must be **byte-identical** to
`ncc --tokens` on the same file. Verified over the whole examples
directory *and over lex.n itself*:

```bash
gcc -O2 -o ncc lang/ncc/ncc.c
ncc lang/selfhost/lex.n -o lex.c && gcc -O2 -o nlex lex.c user/nyxrt.c -I lang/ncc/host
cp <source>.n /tmp/n_lex_target.n
diff <(ncc <source>.n --tokens) <(./nlex)     # empty = proven
```

23 sources, all byte-identical — the suite's stage `[8]` runs exactly
this loop and fails the build on any mismatch.

## parse.n — the parser (rung 1: the shape)

`parse.n` is the chain's second link, and the chain ACCRETES: the
file *contains* its lexer — lex.n's functions carried forward, the
way every link of the toy chain carried its predecessors (N has no
modules; ncc itself is one file) — wearing a two-token window (CUR +
NEXT, ncc's own lookahead plus the peek that tells `x := e` from an
expression statement). Its contract is `ncc --ast`: the POSTORDER
tree dump, byte for byte. The trick that made rung 1 land in one
piece: **recursive descent IS a postorder walk**, so the parser
prints as it parses and stores no tree at all — the tree
materializes when `check.n` needs one, not before.

**Coverage after rung 2** (the claimed targets, enforced by the
suite): everything but `impl` — struct and enum DECLARATIONS
(`#[drop]`/`own` included), extern blocks, functions, every
statement form (`match` in all four positions — statement, `:=`,
`=`, `return` — and `for` joined let/assign/return/expr, `while`,
`if`-`else`(-`if`), `defer`, `break`/`continue`), `?` propagation,
block tails, and the whole expression ladder at ncc's exact
precedence. **Verified byte-identical over the ENTIRE corpus — 24 of 24
sources: every example, lex.n, and parse.n PARSING ITSELF** (the
rung-1 circle, closed) **— including the whole toy compiler
nparse.n, 150K of source, 15,082 dump lines, exact.** `impl` joined
last: methods dump as `M` lines after the F section (ncc's METHODS
table, mirrored by a fifth pass; `self` leads, holds no
capabilities), and with it the parser's coverage ledger is EMPTY. Two designs made rung 2 fit:
the S 9 anchor line grew match-assign's lhs child and `aop` field
(both sides in one step, as always), and the dump's category
grouping — all D lines, then V/U, then X, then F — comes from
**four skip-passes over the file** rather than buffered text: each
pass prints one category and brace-counts past the rest, which is
safe because a string's braces never tokenize. Nothing is out: the
anchor grew its `M` lines (both sides in one step, as always) and
the last source fell.

**And parse.n is proven inside NyxOS too** — rung 2 included: the
in-OS run matched matchexpr.n's 84-line tree (declarations, `match`
in its expression positions, the extended `S 9` line) as well as
hello.n's, both dumped by the on-target build over serial. Rung 1's
first proof: the in-OS ncc (compiled
by the in-OS TinyCC) built it on target and its tree dump for
hello.n matched the C parser's, all 22 lines over serial. That run
earned its keep twice over — the first pass MISMATCHED, and the
culprit was the *C anchor*, not the N module: the dump printers used
`%lld`, which NyxOS's printf does not speak (`%l[dux]` only — the
project's own documented portability rule). The N side was right
both times; the differential caught the reference lying. Fixed to
`%ld`, both dumps agree on both worlds. **And the same
differential holds inside NyxOS**: the in-OS `ncc` (compiled by the
in-OS TinyCC) built `lex.n` on target, and both dumps — the C lexer's
and the N lexer's, over hello.n (73 tokens) and countdown.n (124) —
came back identical over serial. The lexer is proven on the machine
it exists for.

## check.n — the checker (rung 1 landed)

The third module's contract lives in
[tests/bad/](tests/bad/): sixty-six wrong programs and a manifest of
ncc's exact first-error lines. `check.n` must reproduce them —
location and wording — for every file it claims; the corpus doubles
as ncc's own regression battery, so the reference and its rewrite
keep each other honest.

The checker accepts `ncheck file.n`, including relative paths and quoted
paths containing spaces. With no argument it still reads
`/tmp/n_check_target.n`; an unavailable fallback keeps the existing skip
message and successful exit. An unavailable explicit path returns 1, as
described above. Language diagnostics remain filename-free so the
differential contract below is unchanged.

To build and exercise the real checker on Linux x86_64 (including WSL):

```sh
python3 lang/selfhost/tests/test_check_cli.py
```

This compiles `check.n` through ncc and the host runtime, compares valid
and invalid argv targets against ncc, and checks both legacy fallback
outcomes. It temporarily creates `/tmp/n_check_target.n` exclusively and
refuses to overwrite an existing fixture.

**Rung 1 — the tree + the first errors (landed)**: the parser's code
accretes one more time, transformed — every parse function now
returns a NODE INDEX instead of printing, and the tree the toy
proved in miniature materializes at real scale: one parallel-array
arena (kind / children / line / span / next-link, eight words a
node), child lists in a flat side arena, and the five category
passes collapsed back to ncc's own single pass (errors have no dump
order to buy; declarations land in tables). On top of the tree: a
merged extern+fn table (ncc's fn_lookup order), a scoped name table
with ncc's VARS discipline — params seeded immutable, innermost
shadows, blocks truncate on exit — and a checker walk that mirrors
ncc's own sequence (assignment checks lhs, then rhs, then `mut`;
calls resolve, then arity, then arguments). The first error prints
as `line: message` and stops, exactly as ncc dies on its first.

Claimed and byte-exact — the manifest rows with the filename column
stripped (the normalization rule lives in
[tests/bad/README.md](tests/bad/README.md)):

   bad_undecl       2: undeclared variable 'y'
   bad_mut          3: cannot assign to immutable 'x' (declare it with 'mut')
   bad_unknown_fn   2: unknown function 'launch'
   bad_arity        3: 'write' takes 3 argument(s), got 1
   bad_for_mut      3: cannot assign to immutable 'i' (declare it with 'mut')
   bad_struct_mut   4: cannot assign to immutable 'r' (declare it with 'mut')
   bad_caps         7: 'write' requires the syscall capability — ...
   bad_cname        1: function name 'double' is a C keyword — ...
   bad_defer_nested 4: defer is only allowed in the function's outermost block
   bad_argty        6: argument 1 to 'put': expected str, got i64
   bad_userptr      6: argument 2 to 'write': expected #[user] *u8, got *u8
   bad_assign_ty    3: cannot assign i64 to a str target
   bad_index_valty  4: cannot assign str to a u8 target
   bad_index_write  3: cannot write through a str index — ...
   bad_ret_void     2: 'return' with a value in a function with no return type
   bad_struct_unknown 2: unknown struct 'Point'
   bad_struct_missing 3: literal for 'Rect' must initialize field 'h' exactly once
   bad_struct_field 4: struct 'Rect' has no field 'z'
   bad_ret_ty       2: return type mismatch: expected i64, got str
   bad_variant_payload 3: variant 'Shape.Circle' carries a payload — ...
   bad_match_nonenum 3: match subject must be an enum value (got i64)
   bad_match_missing 4: match must cover variant 'Empty' exactly once
   bad_match_binds  5: arm 'Circle' binds 2 name(s), but the payload has 1 field(s)
   bad_mexpr_types  5: match arms disagree: arm 'B' yields str, expected i64
   bad_mexpr_void   5: match arm 'A' must yield a value
   bad_try_operand  4: operand of '?' must be a result enum (...), got Maybe
   bad_try_ret      4: '?' propagates by returning, so the function must (...)
   bad_try_errty    5: cannot propagate R.Err (str) as S.Err (i64)
   bad_try_bind_void 4: R.Ok carries no payload to bind — use `expr?;`
   bad_method       7: Rect has no method 'grow'
   bad_method_arity 7: method 'Rect.scale' takes 1 argument(s), got 0
   bad_fmt_unknown  5: unknown format spec ':q' — the format specs are ...
   bad_fmt_nowidth  5: unknown format spec ':z' — ...
   bad_fmt_trail    5: unknown format spec ':w4q' — ...
   bad_fmt_str      5: format specs apply to integers — a str interpolates as text
   bad_fmt_width_str 5: format specs apply to integers — ...
   bad_wx           2: W^X violation: a mapping cannot be both writable and executable
   bad_pf_castin    2: cannot cast into pageflags — build it from the PROT_* constants ...
   bad_pf_op        2: pageflags compose only with '|' ... (got pageflags + pageflags)
   bad_pf_param     2: compose pageflags from the PROT_* constants — ...
   bad_binop_str    3: operator '+' cannot be applied to str values ...
   bad_ptr_arith    3: operator '+': pointers only support comparison ...
   bad_own_useafter 6: use of 'f' after move
   bad_own_mut      4: own bindings are immutable — ownership transfers by move (v0.17)
   bad_own_discard  5: own result discarded — bind it so someone owns it
   bad_own_loop     7: own value 'f' cannot move inside a loop — ...
   bad_own_while_cond 5: own value 'f' cannot move inside a loop — ...
   bad_own_match_move 8: own value 'f' cannot move inside a match arm — ...
   bad_own_branch   6: own value 'f' is consumed in only one branch of this if — ...
   bad_own_branch_noelse 6: own value 'f' is consumed in only one branch ...
   bad_drop_branch  7: own value 'p' is consumed in only one branch ...
   bad_own_leak     3: unconsumed own value 'f' at the end of the function — ...
   bad_caps_item    2: #[caps(syscall)] applies to a fn or an extern syscall block
   bad_drop_nonown  2: expected 'own' after #[drop(...)] — ...
   bad_drop_unknown    #[drop(nosuch)] on 'Page': unknown function 'nosuch'
   bad_drop_sig        drop function 'free_it' must take exactly one Page parameter
   bad_drop_ret        drop function 'free_it' must not return a value — ...
   bad_own_nest        own type in field 'Holder.f' — own values cannot nest ...
   bad_for_range    2: for range bounds must be integers (got str .. str)
   bad_index_ty     3: cannot index a i64 value (only pointers and str)
   bad_index_idx    4: index must be an integer (got str)
   bad_user_nonptr  1: #[user] applies only to pointer types (got i64)
   bad_user_raw     1: #[user] and raw are mutually exclusive — ...
   bad_noreturn        not every path through 'pick' returns a i64 — ...
   bad_noreturn_loop   not every path through 'spin' returns a i64 — ...
   bad_noreturn_method not every path through method 'Rect.area' returns a i64 — ...

plus ALL TWENTY-TWO examples as POSITIVE targets — the complete
directory checks clean, silence enforced, nparse.n's 150K included. Coverage, stated honestly: extern
blocks and functions parse for real (full bodies, the whole
expression ladder, for-loop variables seeded in their body scope);
struct/enum/impl items brace-skip and `match` refuses — their
manifest rows arrive with the decl-table and match rungs. The host
shim's sbrk arena grew 1M → 8M (ncc/host/nyxrt.h): the checker's
node arena was the first thing to outgrow it, and the differential
reported the overflow as six segfaults before it could pass for an N
bug. Suite stage [8c] holds all eight targets.

**Rung 2 — the structural family (landed)**: three more rows, no
types needed, each a small judgment over machinery rung 1 already
built. The **capability gate** reads the caps bit the fn table
already carries — and mirrors ncc's shape exactly: `xfn_caps` is
XFNS-only, so fn-to-fn calls never gate, and the check sits between
existence and arity (a call that is wrong twice reports the
capability first, ncc's order — probed differentially). The
**C-keyword name refusal** is parse-time, at the function's name,
because ncc's `pexp` fires it before the program is even whole; the
24-entry keyword table lands in kwlook's length-gated shape. The
**defer depth judgment** is the first thing the defer case does —
before its expression resolves (ncc's S_DEFER order) — with
BLOCK_DEPTH tracked exactly as ncc tracks it: incremented per block,
1 meaning the function's own. Nine rows claimed, all byte-exact;
positive silences unchanged.

**Rung 3 opens — the type family**: `infer_type` arrives in N.
`Ty` was hiding in plain sight — the parser's `struct T` (ptrs,
is_user, name-span) is ncc's `Ty` exactly, with an empty span as
ncc's NULL name (void). Synthesized names (an int literal is `i64`
with no source text to point at) use the INTERN TRICK: the six fixed
names are appended after the source in the same buffer, so spans
stay uniform and the lexer never sees them. The name table carries
each binding's inferred type, the fn table carries full parameter
records and return types, and `cinfer` transliterates ncc's
`infer_type` case for case — literals, paths, calls, str's `.ptr`
and `.len`, unary and binary (comparisons to bool, arithmetic by the
left operand), casts as the authoritative source, indexing down one
pointer level. `tcompat` is ncc's `ty_compat` in order: the
`#[user]` boundary first (so the byte-pointer wildcards cannot
smuggle across), exact name+depth, integer inter-conversion, then
the `*u8`/`*void` escapes. Five rows landed with it — argument
types (including the `#[user]` mismatch, rendered `#[user] *u8`
exactly), assignment targets, and the str-index write refusal —
and ncc's own function table had to grow from 64 to 128 on the way:
the checker became the biggest N program yet written. Struct
fields, enum semantics and method returns still fall back softly
(their tables are the next rungs); the claimed rows never reach
them.

**THE CORPUS IS COMPLETE: ALL 66 MANIFEST ROWS BYTE-EXACT, ALL 22
EXAMPLES SILENT.** The last family was the missing-return analysis
— ncc's `block_guarantees`, transliterated whole: a path is covered
by a tail expression, a `return` (or a `return match`), an `if`
whose BOTH arms guarantee, an exhaustive match statement whose
every arm guarantees, or a call to a `never` function; loops never
guarantee, and `while true` is deliberately not special-cased. The
check runs where ncc runs it — per function and per method, after
the parameters seed and before the body walks — and speaks in the
FILE-level shape the declarations rung built. With those three
rows, every wrong program in the corpus is refused with ncc's
exact first error, and every example the language ships checks in
enforced silence. The negative corpus and the checker now hold
each other completely.

**The stragglers (landed): EVERY LINE-LEVEL ROW IS NOW HELD.**
Five rows scattered outside the big families — the E_INDEX read
judgments (only pointers and str have elements; the index counts in
integers), the counted-for's bounds rule (both ends integers, both
types rendered), and parse_type's two `#[user]` refusals (pointer
types only; never together with `raw`), each at ncc's site and in
ncc's order. One find rode along: the index node had never printed
before, so nothing had noticed it carried no line — it does now,
stamped at its `[`. Sixty-three rows; every error in the corpus
that carries a line number is byte-exact, and the three
not-every-path-returns rows — FILE-level, on the contract the
declarations rung built — are all that stand before completeness.

**Rung 10c — the declarations (landed): THE OWN FAMILY IS
COMPLETE, and FILE-level messages joined the contract.** The item
loop now walks ncc's exact order — caps, then fn/extern, then the
caps-on-anything-else refusal (a row of its own), then `#[drop]`,
which DEMANDS `own` next (another row) — and a new declaration pass
runs between the parse and the body checks, where ncc runs it:
`validate_drops` first (the destructor must exist, take exactly one
value of the struct's own type, and return nothing — the lexer had
already isolated the function name inside `#[drop(...)]`, so the
table lookup was a span away), then the v0.17 containment walks
(no own type may hide in a struct field, an enum variant, or cross
a syscall; no impl on an own type). Four of the six new rows have
NO line number — ncc prints them FILE-level — and the manifest
contract absorbed them without touching the harness: stripping the
filename column from such a row leaves the message with a leading
space, so check.n prints exactly that shape (the two-shape contract
is documented in [tests/bad/README.md](tests/bad/README.md)).
Fifty-eight rows; the whole own/move/drop dialect — seventeen rows
across four rungs — is now held byte-exact, and only the
not-every-path-returns family stands between the corpus and
completeness.

**Rung 10b — the leak scan (landed): OWNERSHIP IS NOW A CLOSED
LEDGER.** The other half of must-consume: at every `return` (drops
drain, then the whole frame must be clean — "at this return") and
at every scope close (this block's own births — "the end of the
function" or "the end of this block", split on the block depth,
reported at the LAST statement's line, ncc's exact anchor), any own
binding still LIVE is refused with the corpus's leak row. The v0.19
auto-drop rides in front of every scan: a LIVE binding whose type
carries a `#[drop]` destructor drops instead of leaking — the
struct table has held the attribute's span since 10a, so "has a
destructor" was one slot read away — which is precisely what keeps
own.n silent (its `use_page` leaks-by-design into an auto-drop, and
its loop body births a page each iteration and drops it at the
body's close). One row, and the accounting property is total: an
own value now provably goes SOMEWHERE — moved, returned, dropped,
or refused at compile time.

**Rung 10a — own states and consume points (landed): NINE ROWS,
AND THE BRANCH MERGE CAME EARLY.** The deepest family opened: the
struct table records `own` (and holds the `#[drop]` span for the
declarations rung), every binding carries an ownership state —
NONE, LIVE at an own birth (a consumption obligation), HELD for an
own parameter (the callee is the owner of record), MOVED after a
consume — and ncc's `own_move_expr` transliterates as `cmove` at
the five consume points: call arguments (each after its type
check), a `let` init, an assignment's rhs, a `return`, and the
block tail (at line 0, ncc's own oddity). The v0.18 gates ride
LOOP_DEPTH — covering while BODIES and while CONDITIONS, which
re-evaluate — and MATCH_DEPTH; moved values refuse further use at
the path itself, `own` + `mut` refuses at the binding, and an own
result nobody binds is refused at the statement (whose line ncc
stamps AFTER the semicolon — the parser now does too). Then own.n
itself forced the ladder's hand: its legal flows use v0.18
branch-aware consumption (both arms move, or a returning arm is
exempt), so the S_IF machinery came forward from the flow rung —
pre-if states snapshot, each arm checks from the same start, and
the exits must agree at the merge unless an arm returns. That
carried the branch-agreement rows with it: nine rows this rung,
fifty-one total, and own.n stays the silence that proves the legal
side of every one of them.

**Rung 9 — pageflags and the operator rules (landed): THE EXAMPLES
LEDGER CLOSES.** The four PROT_* constants predeclare (checked
before name resolution, ncc's order), every pageflags binding
tracks its statically-known flag set (`pmask`, -1 for a parameter's
opaque flags), and `cpfmask` folds constants, bindings and `|`
compositions exactly as ncc's `pf_mask` does — which makes W^X a
total compile-time proof, transliterated: compose only with `|`,
only from statically-known sets, and never writable and executable
together. Casts never go INTO pageflags, and only integers come
out. The binary-operator rules landed with it in ncc's arm order —
pageflags, then str (no `+` on strings), then pointers (comparison
only), then integers. Six rows — and with pageflags.n silent,
**every example the language ships is now an enforced silence: 22
of 22.** The checker holds the whole examples directory to zero
output and forty-two manifest rows to the byte.

**Rung 8 — the format specs (landed)**: the parser stops dropping
`:spec` — each hole records whether a spec rode it, and the spec
GRAMMAR validates at parse time, ncc's site and walk exactly
(`[wN|zN][x|X] | x | X`; no digits after w/z, a zero or absurd
width, or trailing garbage all print the one long teaching message
verbatim). At check time every hole must interpolate at all (str or
integer), and a spec'd hole must be an integer — a str interpolates
as text. Five rows. The silence sweep then paid twice over: seven
more examples check clean (ntokens, ncalc, nemit, nstack, nwin,
own, fsio) and so does nparse.n — the 150K toy compiler, ~15,000
nodes through the checker's arenas without a squeak — plus lex.n
itself. Twenty-one of the twenty-two examples are enforced
silences now; pageflags.n alone waits for its family (PROT_READ is
a predeclared constant the checker does not yet know).

**Rung 7 — methods (landed)**: pimpl accretes back — the last
brace-skipped item parses for real — and each method lands in a
method table keyed on (impl type, name), with `self` excluded from
the params exactly as ncc's Method records exclude it. The E_CALL
method arm completes: the receiver resolves and its TYPE keys the
lookup (a missing method renders the receiver through ty_str, so
`Rect has no method 'grow'` prints the type bare), then arity, then
per-argument types against the declared params. Method BODIES check
before function bodies — ncc's gen_program order — with `self`
seeded first as an immutable binding of the impl type (its name
interned like the synthesized type names) and no capabilities
(v0.14). cinfer's method-return fallback became the real lookup, so
a method call's result types everything downstream. Two rows, and
methods.n — the dispatch machinery's own example — is the
thirteenth silence.

**Rung 6 — `?` propagation (landed)**: the try flags the parser had
been consuming and dropping now ride the nodes, and try statements
route whole through ncc's gen_try sequence, in its order: the
operand must be a result enum (exactly the variants Ok and Err,
each carrying at most one payload field — `enum_is_result`
transliterated), the enclosing function must return one, DIFFERENT
result enums must agree on their Err payloads (count, then type),
and the binding forms need an Ok payload — which then types the
binding: `x := e?;` binds the UNWRAPPED payload type, so everything
downstream of a try sees what ncc sees. Four rows landed, and
results.n — the example that had to stay off the positive list —
checks in silence as the twelfth.

**Rung 5 — the enum table, and match comes back (landed)**: penum
accretes back like pstruct did, and `match` — refused since the tree
rung — parses again in all four positions (statement, `:=`, `=`,
`return`), building nodes whose arms carry variant spans, bind
spans, and body/value nodes. The checker transliterates ncc's whole
S_MATCH sequence in order: the subject must be an enum, every
declared variant covered exactly once, no stray arm names, each
arm's binds counted against its variant's payload — then, for the
value forms, every arm checks WITH ITS BINDS IN SCOPE (typed from
the payload fields, ncc's positional rule), the first arm fixes the
result type, later arms must agree, and the `:=`/`=`/`return`
targets get their own statement's checks against the match's result.
Variant references (`Shape.Empty`) judge before the base could
misresolve as a variable, payload-carrying variants point at the
constructor, and enum literals check their payload fields with
slit's exact discipline. Seven rows landed at once — and eight more
examples joined the silences (enums.n and matchexpr.n among them:
the whole match machinery, exercised by real programs that must
stay quiet). results.n stays off the list honestly: it uses `?`,
and the try rung has not been climbed.

**Rung 4 — the struct table (landed)**: pstruct accretes back from
the brace-skip, transformed — fields land in a struct table (name
span + field records, the same span+T shape params use) instead of a
D line. Struct literals now carry their field NAMES as well as their
values, and the checker runs ncc's E_SLIT sequence in ncc's order:
known struct, every declared field exactly once, no extra names,
then each value resolved and type-checked against its declared
field. Field reads run ncc's three arms — str's `.ptr`/`.len`, the
struct-table lookup, and the `has no fields` refusal — and `cinfer`
returns real field types now, so a struct field feeds every
downstream judgment (probed: a str field passed to an i64 parameter
reports the argument mismatch, ncc-identical). structs.n joins the
positive targets: a real example exercising the table must stay
silent. Method calls check their receiver, not their field node —
ncc's own shape; dispatch is a later rung.

**The return judgments (row 15)**: `CUR_RET` rides the checker's
state (set per function from the recorded return type), and the
return case mirrors ncc's three branches in order — a value in a
no-return function (the claimed row), the value/return-type
mismatch, and a bare `return` in a value-returning function — with
`never` counting as no-type, exactly as ncc's `is_never` does.

**Proven inside NyxOS (batches V45–V52)**: the on-target ledger
now holds the GENERATOR too. V52 built gen.n inside NyxOS (the
in-OS ncc compiled its ~4400 lines, tcc compiled the C) and ran it
on hello.n and structs.n; the generator's emitted C crossed the
serial port line by line and matched the reference compiler's
output for the same fixed source path — every line of both
programs, 24 and 40 lines, identical (the single normalization:
the provenance comment's path is rewritten on the reference side,
since the harness host cannot park a file at the OS's `/mnt`).
Twelve first-error differentials stand alongside the two generator
fences, zero panics. The fence's design history is itself a
finding: the shell's `>` turned out to redirect builtin output
only — an exec'd binary's stdout goes to the console — reported
upstream as issue #88.

**Proven inside NyxOS (batches V45–V47)**: V47 added the struct and
enum/match families to the on-target ledger — four two-sided
differentials now (caps, argument types, struct-literal fields,
match cover), every first-error line matched byte-for-byte inside
the OS, with the in-OS ncc compiling the 85-function checker each
time. Zero panics; the census on its exact baseline every run.

**Proven inside NyxOS (batches V45 + V46)**: the in-OS ncc+tcc
pipeline compiled check.n on target twice over. V45 ran it against
bad_mut.n; V46 ran it against a row from EACH new family —
bad_caps.n (structural) and bad_argty.n (types) — and both first
error lines matched the in-OS ncc's own, location and wording, with
only the filename column normalized away (two-sided on-target
differentials: reference and rewrite both ran inside the OS). V46's
in-OS ncc also compiled the 75-function checker itself — the 64→128
function-table growth, exercised by the operating system's own
compiler. V45 additionally closed the parser's last on-target gap
(methods.n's 53-line impl/M dump, byte-identical). Zero panics both
times; the VFS census held its exact baseline (219/512 live,
high-water 221) — the node pool recycles.

**Rung 2 — lexemes (landed)**: the stream carries substance now, on
both sides of the differential at once. `ncc --tokens` and `lex.n`
both print, per token: identifiers and `#[drop]` names **verbatim**
(`7 11 write`), integers as their **parsed value** — hex and `_`
separators normalized, so `0x811C9DC5` prints `2166136261` and the N
side's fold must match ncc's exactly — and string segments as their
**processed byte count** (`3 17 #18`: bodies hold control bytes, so
they are counted, not printed; the count still pins the escape rules,
since one miscounted escape shifts every line after it). Still 23/23
byte-identical — **and proven inside NyxOS with the lexemes on**:
the in-OS run matched hello.n's 73 enriched lines and countdown.n's
124, both dumps over serial. Errors print kind `-1` and stop; a `-1`
against a valid source means the lexer is wrong, and the differential
would already have caught it. Sources are capped at 256K (nparse.n,
the largest, is 150K).

## gen.n — the generator (rung 1: first C, byte for byte)

The fourth and final module. `gen.n` accretes `check.n` whole —
generation assumes checked input, exactly as ncc's single pass does —
and adds the emitters. Its differential needs NO normalization at
all: copy the source to the fixed path (`/tmp/n_gen_target.n`), run
`ncc` and `gen.n`'s hosted build over the same file, and `cmp` the
two C outputs. Byte-identical, provenance comment included.

Three accretion changes feed the emitters: the lexer keeps each
string's RAW body start beside its decoded count (the emitter
re-walks the body, translating N's escapes to C's — the same
spellings, brace escapes decoding to bare braces), `pinterp` keeps
EVERY fragment (text and holes, 4-word records) instead of holes
only, and `pextern` keeps the syscall number the wrappers embed.
The emitters then reproduce ncc's discipline: the header comment,
the `static inline` syscall wrappers over `__nyx_syscall6`, the
forward prototypes, `base_ctype`'s primitive map, and per function
the numbered interpolation preludes (`__b0`/`__s0`, ncc's IID
counter, assigned post-order), the statement forms, and the
tail-return. Emission runs only after the whole check phase ends
clean, over the same trees and tables, re-inferring with `cinfer`.

**Rung 2 holds FOURTEEN examples byte-exact** — rung 1's three plus
structs, forloop, caps, bytes, userptr, ncalc, nemit, nstack, nwin,
pageflags and **nparse.n, the 150K toy compiler, byte-identical C
end to end**. The rung added the struct and enum layout walks
(C-typedef structs; tagged unions with the numbered tag comment),
the three literal forms (compound-literal structs, tagged enum
values, tag-only variant references), the counted `for` with its
hoisted `__fsN`/`__feN` bounds on the shared IID counter,
`break`/`continue`, the `else if` wrap (ncc's parser puts a chained
`if` inside a synthesized block — the emitter now does too), the
format-spec lowerings (`__nyx_fmt_hex`/`__nyx_fmt_num` — fragment
records grew to carry the parsed width/zero/hex values), and the
unary `!` (which had never had a rendering: the operator table was
built for binary diagnostics). `cmp`-verified by suite stage [8d].

**Rung 3 holds EIGHTEEN: the match switch and methods landed.**
Both match forms emit ncc's exact lowering — the statement form's
subject-once temp (`__mN`), the tag switch with case numbers in
declared-variant order, binds as typed locals initialized from the
payload fields; and the expression forms' zero-initialized
`__mresN` temp (the switch is exhaustive — the store is
deliberately dead), consumed after the switch so an arm bind may
shadow the target. Methods came in the same climb: the `static
Type_m(Type self, ...)` prototypes after the function prototypes,
the definitions BEFORE the function definitions (ncc's order), and
the call-site rewrite `recv.m(a)` → `Type_m(recv, a)` on the
receiver's inferred type. enums, matchexpr, ntokens and methods
joined the byte-exact list.

**Rung 4 holds TWENTY-ONE: defer and `?` landed together.** The
defer registry mirrors ncc's — sixteen expression slots, reset per
function, LIFO at every exit, with each deferred expression's
PRELUDES emitted at the exit itself so the temporary numbering
interleaves exactly as the reference's does. The braced forms came
with it: a valued `return` under live defers computes into `__ret`
first (a defer cannot change what gets returned), a returning tail
does the same inside the block's own braces, and a void function
falling off its end runs the defers last. The `?` lowering is
`gen_try` transliterated: the operand lands in `__tN` once, the Err
arm returns at once — the same value when the enums match, the
payload rewrapped through `__eN` when they differ — defers before
the return, and the Ok payload binds, assigns, or discards per the
statement form. defer.n, results.n and fsio.n all byte-exact.

**Rung 5 closes it: ALL TWENTY-TWO — THE GENERATOR HOLDS THE
WHOLE DIRECTORY.** The last construct was the own auto-drops, and
they asked for something no earlier rung had needed: the emission
walk REPLAYS the ownership state machine — parameters arrive HELD,
births go LIVE, every consume point transfers (through a silent
mover: the checker already proved every move legal, so the emit
phase's transfers carry no diagnostics), and the `if`/`else`
branch discipline is replayed too — snapshot, restore between
arms, merge with the returning-arm exemption — because a drop's
placement depends on which arm's exit state flows on. With the
states correct, `gdrops` emits each destructor call exactly where
ncc's `own_drops_emit` does: reverse birth order, after the defers
at returns, at the scope's close otherwise, and a pending drop
forces the braced `__ret` form just as a live defer does. Every
example the language ships now emits byte-identical C — and the
generator crossed ncc's function ceiling doing it (FNS 128→256,
the second capacity the selfhost ladder has outgrown).

Batch V53 carried the match switch to the target the same hour:
matchexpr.n's 111 lines — the full `__mN`/`__mresN` lowering —
emitted inside NyxOS, identical to the reference beside it.

Batch V54 put the own replay itself on target: own.n's 142
generated lines — auto-drops placed by the replayed state machine —
emitted inside NyxOS, identical to the reference beside it.

**And then the summit. Pointed at the four selfhost sources
themselves, the generator emits every one of them byte-identically
to ncc — lex.n (578 lines of C), parse.n (2220), check.n (4913),
and its OWN source, gen.n (6624).** The survey's first run told an
honest story: lex and parse passed immediately, but check.n was
refused with a bogus `undeclared variable` — not a logic gap but a
capacity one. The accreted parser's node arena held 16K nodes and
check.n alone parses to ~22K; the overflow quietly stomped the
arenas behind it. The selfhost sources are the first inputs big
enough to find that edge — which is precisely what a summit survey
is for. With the arenas sized for self-hosting (64K nodes, and the
source buffer at 512K), all four fell on the first re-run. The
suite's [8e] stage now holds the whole claim: the checker is
silent on its own sources, and the generator reproduces ncc's C
for the entire ladder, itself included.

**Batch V55 speaks that sentence inside NyxOS.** The in-OS `cc`
(TinyCC) compiles ncc, ncc compiles gen.n, and the resulting
`ngen` -- running as a NyxOS user process -- emits the C of its own
source: all 6,624 lines over the serial line, byte-identical to
what ncc emits for gen.n on the host. Self-hosting's full circle,
closed where the language lives: the compiler that compiles the
compiler is itself compiled inside the operating system it was
built for, and can reproduce itself there exactly.

**What a self-hosted compiler owes its language came due the same
week.** ncc grew v0.23 — program arguments — and the ladder had to
follow or let the byte-identity claim quietly expire. It followed
in two matched steps that are now precedent: the changed `main`
prologue (every program's emitted C moved) landed in gen.n in the
SAME commit as ncc's, so every baseline shifted together and
regenerated identically; the new `arg_count()`/`arg(i)` builtins
followed one rung behind, as identical checker arms in check.n and
gen.n with ncc's exact wording, and only then did `args.n` join
every differential list. An emitted-shape change rides in
lockstep; a new construct follows when its mirror exists.

Then the ladder used the feature it had just learned. `ngen
file.n` names its target from argv — the fixed harness path
survives as the no-argument fallback, the provenance comment names
the real input exactly as ncc names its own — and `nlex` and
`nparse` grew the same two-line mouth. The suite holds all three
as tools now: invoked with real paths against ncc on the same
paths, byte-identical with zero normalization, because every path
in the comparison is the true one.

**And the toolbox installs.** `xbm install nlex`, `nparse`,
`ngen`: three packages cut from one cloth — each tool's emitted C
with the runtime appended, one translation unit, a local header, a
three-line recipe — and none of it hand-written: the suite's [1c]
guard regenerates every package from its N source on each run and
byte-compares, so a package can never drift from the source it
claims to be. Batches V56 and V57 close the story on target: a
booted NyxOS installs all three from its package manager and runs
them — token dump, syntax tree, generated C, the self-emission
itself — off the installed binaries, byte-identical to the
reference, every fence green. The ladder that began as a lexer
held to `ncc --tokens` ends as an installable compiler toolchain,
written in N, verified byte-for-byte, living inside the operating
system it was built for.
