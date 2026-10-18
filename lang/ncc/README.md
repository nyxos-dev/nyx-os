# ncc — the N bootstrap compiler

Single-file, dependency-free C99 program that compiles N (see the
[language spec](../docs/spec-n.md), versioned in lockstep with this compiler)
to freestanding C targeting the NyxOS runtime. "Bootstrap" means: it exists to get N off the ground and to be simple
enough to port *into* NyxOS (milestone M2) — clarity beats cleverness
throughout.

## Build

Any C99 compiler works; no libraries beyond the C standard library:

```bash
gcc -O2 -Wall -Wextra -o ncc ncc.c
```

(That command is warning-clean; keep it that way.)

## Usage

```bash
ncc input.n -o output.c    # transpile
ncc input.n                # ... or write the C to stdout
ncc input.n --tokens       # dump the token stream: one "kind line" per token
ncc input.n --ast          # dump the parsed tree, postorder, one node per line
```

Since NyxOS 6.5.195, file output and all three stdout modes check the
stream error state and close/flush result before reporting success. A
reported output error exits with status 1 and identifies the destination;
an earlier buffered-write failure is retained even if closing succeeds.
`python3 tools/test_n_output.py` (from the repository root, on Linux/WSL)
covers short writes, sticky errors, close/flush failures and `/dev/full`
for the bootstrap N/N++ drivers. This does not provide atomic replacement
or detect storage errors that the underlying filesystem fails to report.
The self-hosted N emitters still need equivalent output checks.

Target verification on 6.5.195 installed `ncc` with `xbm` in QEMU: writing
to a RAM directory failed with status 1 after opening, without `OK`.
Workbench still compiled and ran through the in-OS C compiler; generated
C, its 301-byte trace, tokens and AST matched host references byte for
byte. Close/flush fault injection is host evidence, not a target disk-full
or power-loss test.

`--tokens` exists for differential testing: it prints the lexer's exact
output (token-kind number and source line, `T_EOF` included) so an
independent lexer — the planned `lex.n`, N's lexer written in N — can be
held to it byte for byte over any source file. See
[docs/selfhost.md](../docs/selfhost.md), "The mountain after the toy".

On success `ncc` prints a one-line summary to stderr and exits 0. On the first
error it prints `file:line: message` and exits 1.

## Compiling the output for NyxOS

The generated C includes `"nyxrt.h"` (the runtime header in `user/`) and is
compiled with the standard NyxOS user-space flags, then linked with the
standard `crt0` and the runtime:

```bash
gcc -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone \
    -I ../../user -c output.c ../../user/nyxrt.c
nasm -f elf64 ../../user/crt0.asm -o crt0.o
ld -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 \
   -o program.elf crt0.o nyxrt.o output.o
```

`-mno-red-zone` is mandatory (NyxOS interrupt handlers may run on the user
stack region). Since v0.2 the emitted C is **strict C99 with no GNU
extensions** (verified with `-std=c99 -pedantic-errors` in the test
pipeline) — this matters because TinyCC is the in-OS compiler that will
build N programs inside NyxOS at milestone M2/M3, and tcc does not support
extensions like `__auto_type`.

## Testing without booting the OS

The x86_64 `syscall` *instruction* ABI is identical on Linux and NyxOS — only
the syscall numbers differ. [`host/nyxrt.h`](host/nyxrt.h) exploits that: it
is a drop-in replacement runtime header whose `__nyx_syscall6` maps NyxOS
numbers to Linux ones (write→write, getpid→39, exit→60). Compile the generated
C hosted, against the shim, and run it natively:

```bash
mkdir -p t && cp host/nyxrt.h ../../user/nyxrt.c output.c t/ && cd t
gcc -O2 -Wall -o prog output.c nyxrt.c && ./prog
```

This executes the *real* generated code — real syscall instruction, real
formatting runtime — and is the standard way to behaviorally test compiler
changes quickly. Full in-OS verification (boot QEMU, `exec` the ELF) remains
the release gate.

## Source tour

`ncc.c` is organized top-to-bottom in four sections:

| Section | What it does | Key entry points |
|---|---|---|
| Lexer | Tokens, comments, literals; string interpolation via a brace-depth mode stack that emits `HEAD/MID/TAIL` runs | `next_token`, `scan_string_body` |
| Parser | Recursive descent, one-token lookahead; precedence ladder for expressions | `parse_program`, `parse_expr`, `parse_block` |
| Inference | Minimal per-function symbol table (block-scoped via save/restore); types `:=` bindings, dispatches interpolation, enforces `mut` | `infer_type`, `vars_find` |
| Codegen | Emits strict-C99 per the spec's §7 contract; interpolations lowered to hoisted, bounds-checked buffers | `gen_program`, `gen_preludes` |
| Driver | CLI, file I/O | `main` |

Two invariants worth knowing before editing:

1. **Interpolation buffers are hoisted** into the enclosing block, never
   inside a statement-expression — the `nyx_str` must outlive the statement
   using it (spec §7.3; a dangling-pointer bug taught us this).
2. **The `}` disambiguation** between "close a block" and "close an
   interpolation" is the lexer's brace-depth stack (`istack`): an
   interpolation closes only when depth returns to its opening level, so
   nested braces inside interpolated expressions stay correct.

## Porting to NyxOS (milestone M2)

`ncc.c` is already in-OS-portable at the source level: under TinyCC (which
always defines `__TINYC__`, and NyxOS is the only tcc environment we target)
it includes the single NyxOS libc header `libc.h` instead of the hosted
standard headers — the same pattern the self-hosted coreutils use. The NyxOS
libc provides everything it needs: buffered `FILE*` stdio with
`fopen/fread/fseek/ftell`, `fprintf/vfprintf` + `stderr`, and `calloc`.
Two portability rules are load-bearing:

1. **`%ld`, never `%lld`** — the NyxOS formatter implements `%l[dux]` but not
   `%ll`. Both targets are LP64, so `long` is 64-bit everywhere and `%ld` is
   width-correct on host and OS alike.
2. **No GNU extensions anywhere** — neither in `ncc.c` itself nor in the C it
   emits (the strict-C99 gate in the test pipeline enforces the latter).

The target in-OS build command is:

```
cc /mnt/ncc.c -I/usr/src/nyx -o /mnt/bin/ncc
```

(`/usr/src/nyx` ships `libc.h`/`syscall.h` in the initramfs; the `cc` builtin
passes `-I` through to tcc.) The header path is verified continuously on the
dev machine by compiling with `-nostdinc` against `user/tcc/nyxshim` +
`user/libc.h` — the exact set the in-OS tcc resolves.

**M2 and M3 are verified**: inside a booted NyxOS, that exact `cc` command
compiled `ncc`, the resulting in-OS `ncc` transpiled `lang/examples/hello.n`,
`cc` built the output against `nyxrt.c`, and the binary ran and printed its
pid — the full N pipeline with no dev machine involved. The runtime-side
portability that enabled it (tcc-safe fixed-width types, the explicit-`movq`
syscall asm, the `environ`/libc.o ownership rule) lives in `user/nyxrt.h` /
`user/nyxrt.c` with comments explaining each rule.

**`ncc` is an installable NyxOS package.** The `user/pkg/ncc/` recipe ships in
the initramfs package repository, so on any NyxOS:

```
xbm install ncc        # compiles ncc from source with the in-OS cc -> /mnt/bin/ncc
ncc program.n -o program.c
cc program.c /mnt/nyxrt.c -I/mnt -o /mnt/bin/program
```

(verified in-OS: the xbm-installed `ncc` transpiled `hello.n` and the result
ran, printing its pid). Maintenance note: `user/pkg/ncc/ncc.c` is a
**byte-identical copy** of `lang/ncc/ncc.c` — xbm recipes can only build
sources inside their own package directory (a path-traversal guard), so the
copy is by design. When changing the compiler, refresh it with
`cp lang/ncc/ncc.c user/pkg/ncc/ncc.c`; the canonical source is always
`lang/ncc/`.

**The compiler also exists in N.** `lang/selfhost/gen.n` is a byte-faithful
reimplementation of ncc's whole pipeline — lexer, parser, checker, code
generator — written in the language itself and verified to emit exactly the
C ncc emits, for every example and for the selfhost sources themselves,
its own included (on the host and inside NyxOS). The full account is in
[../docs/selfhost.md](../docs/selfhost.md). `ncc.c` remains the reference
implementation and the one this document describes.

That N implementation is installable too: `xbm install ngen` builds it
in-OS from `user/pkg/ngen/` (ncc's emitted C for `gen.n`, one
translation unit with the runtime appended — regenerated and
byte-verified by the suite on every change), and `ngen file.n` then
transpiles like `ncc file.n -o` — same C, byte for byte.
