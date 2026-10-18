# NyxOS Architecture

*A developer's map of the codebase — where things live, how they fit together, and
how to add your own.*

NyxOS is a from-scratch, 64-bit (x86-64) hobby operating system written in freestanding
C with a little assembly. It boots on real Multiboot bootloaders (GRUB), comes up into a
purple, night-themed graphical desktop, and ships a Unix-flavoured userland — a shell,
~60 coreutils, a windowing compositor with apps and games, a TCP/IP + TLS 1.2 network
stack, image decoders, and an in-OS C toolchain that self-hosts. Everything wears the
same identity: **Nyx**, the Greek goddess of night — so the palette is purple/violet and
the mood is "Nightfall", never a generic clone of another OS.

This document is the orientation guide for someone reading or extending the source. It is
deliberately code-grounded: every subsystem entry points at the directory and the entry
functions you would actually open.

---

## At a glance

| Property | Value |
|---|---|
| Architecture | x86-64 (long mode) |
| Language | Freestanding C (`-std=gnu99`), plus x86 assembly for boot/context-switch |
| Boot | Multiboot v1 **and** v2 headers; GRUB loads it, then `boot.asm` enters long mode |
| Kernel model | Monolithic; drivers, FS, net, crypto, and GUI all run in ring 0 |
| Userland | Ring-3 ELF processes over a small syscall ABI and a custom libc |
| Concurrency | Preemptive multitasking, SMP (multiple CPUs) |
| Float policy | Built `-mno-sse -mno-red-zone`; **the kernel is integer-only** (no FPU/SSE in kernel code) |
| Toolchain | Cross-GCC + NASM on the host; an in-OS TinyCC port (`cc`) that self-hosts |
| Source size | ~186 kernel `.c`, ~183 kernel `.h`, ~105 userland `.c` |

Because the kernel is compiled `-mno-sse`, **all kernel math is integer / fixed-point** —
the render demos, the wallpapers, and the crypto all avoid floating point on purpose. Keep
that constraint in mind when adding code: a stray `double` will not link cleanly in a
freestanding target (and pulls in libgcc helpers the build deliberately excludes).

---

## Repository layout

```
nyx-os/
├── kernel/            The kernel and (under user/) the userland it bundles
│   ├── core/          Boot, kernel_main, the shell/command dispatch, syscalls, self-tests
│   ├── mm/            Physical page allocator, paging, copy-on-write, kernel heap, sbrk
│   ├── proc/          Tasks, scheduler, fork/execve/waitpid, SMP, pipes, signals, /proc
│   ├── fs/            VFS, initramfs, ext2, the node pool + fd handle table
│   ├── drivers/
│   │   ├── video/     VBE/LFB framebuffer, fonts
│   │   ├── input/     PS/2 keyboard + mouse
│   │   ├── net/       RTL8139 NIC (polled + IRQ)
│   │   ├── audio/     Sound Blaster 16
│   │   └── misc/      PIT, RTC, PCI, serial, …
│   ├── net/           ARP/IP/ICMP/UDP/TCP, DHCP, DNS, sockets
│   ├── crypto/        SHA-2, AES-GCM, RSA, EC (P-256/P-384), X25519, CSPRNG, DER/X.509
│   │   └── tls/       TLS 1.2 record layer + handshake
│   ├── image/         PNG, BMP, GIF, JPEG decoders (+ reject-hardening)
│   ├── gui/
│   │   ├── core/      Compositor, window manager, wallpapers, login, desktop
│   │   ├── apps/      Terminal, file manager, editor, paint, calc, image viewer, Selene browser…
│   │   └── games/     DOOM, Pong, Snake, Tetris, Minesweeper + the "Nyx" perf/visual demos
│   ├── auth/          Users, login credentials
│   └── makefile       Builds the kernel image and every userland ELF
├── user/              Userland: crt0, libc, the shell (sh), and ~60 coreutils
├── docs/              GitHub Pages site + this architecture guide
├── build.ps1          Host build entry point (Windows → WSL make → ISO)
└── README.md
```

Includes are **relative to each file's own folder**; the makefile's `vpath` keeps the
resulting `.o` files flat. When you add a source file, put it next to its peers and
include headers by their path relative to you (e.g. `../core/kernel.h`).

---

## Boot & init sequence

1. **GRUB → `kernel/core/boot.asm`.** The image carries both a Multiboot v1 header
   (`0x1BADB002`, for compatibility) and a Multiboot v2 header (`0xE85250D6`, primary).
   GRUB loads the 32-bit stub, which sets up paging, enters **long mode**, and jumps to C.
   *(This is why `qemu -kernel nyx-kernel.bin` fails — QEMU's multiboot path wants a
   32-bit image. Always boot the GRUB ISO with `-cdrom`.)*
2. **`kernel_main(magic, mboot_ptr)`** (in `kernel/core/kernel.c`) brings the machine up:
   parses the Multiboot tags (memory map, framebuffer), initializes the physical allocator
   and paging, the heap, interrupts/IDT, the PIT/RTC, PCI, drivers, the VFS + initramfs,
   the network stack, and the GUI. It also reads the kernel command line: the token
   `selftest` flips the boot into **CI self-test mode** (see below) instead of the desktop.
3. **Login → desktop.** Normally `kernel_main` shows the purple login screen
   (`nyx` / `nyx` by default), then hands off to `compositor_run()` which draws the
   Nightfall desktop and runs the window event loop. Logging out returns to the login
   screen without a reboot.

The serial log line `[LOGIN] Ready.` is the canonical "fully booted" marker used by the
automated tests.

---

## Subsystems

### Memory management — `kernel/mm/`
A physical page-frame allocator with **per-frame reference counts** (so copy-on-write can
share pages), 4-level paging, and a kernel heap. Fork uses **copy-on-write**: the child
shares the parent's pages read-only and each side faults in a private copy on first write.
Userland heaps grow with a **lazy `sbrk`** — `SYS_SBRK` only bumps the break; the pages are
faulted in on demand. The allocator is careful to hand out only real RAM frames (a historic
bug handed out the sub-1 MB BIOS/MMIO hole under pressure — see the page-allocator
hardening in the history).

### Processes & scheduling — `kernel/proc/`
Preemptive, SMP-capable multitasking. The classic trio is here: `do_fork` (COW),
`do_execve` (loads an ELF via `elf_load_image`), and a truly-blocking `do_waitpid`.
Each process has its own kernel syscall stack. Inter-process plumbing includes **pipes**
(with blocking reads and fork inheritance), **signals** with `setjmp`/`siglongjmp` fault
recovery, and a live **`/proc`** filesystem (per-pid status, `maps`, etc.). SMP-shared
tables are guarded by spinlocks (e.g. the `/proc` sync lock, the TCP connection-table
lock).

### Filesystems — `kernel/fs/`
A **VFS** sits over multiple backends: the **initramfs** (bundled at build time — this is
where the userland ELFs live), an **ext2** driver (read/write, used for the DOOM WAD and a
scratch disk), and the synthetic **`/proc`**. Open files are represented by a static node
pool; file descriptors are **1-based indices** into that pool (not raw pointers), which
keeps them small and 64-bit-safe.

### Drivers — `kernel/drivers/`
- **video/** — a linear-framebuffer (VBE/LFB) driver with a bitmap font; the GUI draws
  straight into it (double-buffered for the desktop, direct for login).
- **input/** — PS/2 keyboard (with a line discipline for `SYS_READ` on fd 0, control-key
  translation, and layout switching) and mouse.
- **net/** — the **RTL8139** NIC. Traffic is primarily **polled**; the RX/TX interrupt is
  also wired (a minimal handler that acks the ISR and flags work) as the first rung toward
  fully interrupt-driven networking (issue #62).
- **audio/** — Sound Blaster 16 playback.
- **misc/** — PIT timer, RTC clock, PCI enumeration, serial (the debug/CI channel).

### Networking — `kernel/net/`
A hand-written TCP/IP stack: ARP, IPv4, ICMP (ping), UDP, and **TCP** with a real
three-way open, checksum validation on RX, a retransmit **queue**, and a graceful close.
On top sit **DHCP** (lease a v4 address), **DNS** (with anti-spoofing checks), and a
BSD-ish **socket** layer. The stack is exercised over loopback in the self-tests. See
**`docs/NETWORK.md`** for the full walkthrough: the layer cake, the poll-driven model,
the `net_lock` concurrency model, the `nsock_*` socket API, and each protocol client.

### Cryptography & TLS — `kernel/crypto/` + `kernel/crypto/tls/`
Enough modern crypto to speak real TLS 1.2: **SHA-256/384/512**, **AES-GCM**, **RSA**,
elliptic curves **P-256 / P-384** and **X25519**, a **CSPRNG**, and **DER / X.509**
certificate parsing with a chain-verification trust model. The TLS layer has its own
record and handshake self-tests. All of it is integer bignum math (no floats, no libgcc).
See **`docs/CRYPTO.md`** for the full walkthrough: every primitive, the TLS 1.2
handshake step by step, and the key-pinned X.509 trust model (`X509_OK` / `INCOMPLETE`
/ `FORGED`, host + validity checks, strict mode).

### Image decoders — `kernel/image/`
From-scratch **PNG** (with per-chunk CRC-32 validation), **BMP**, **GIF** (including
animation), and **JPEG** decoders, plus a "reject" self-test for selected malformed
inputs. These fixed cases do not establish safety for every possible input.

Development version **6.5.206** computes BMP height magnitude in 64-bit signed
arithmetic before applying the existing dimension and pixel limits. This rejects
the minimum signed 32-bit height without undefined negation; supported top-down
and bottom-up images retain their row order. `bmptest` includes this malformed
height. `python3 tools/test_bmp_bounds.py` adds 251 ASan/UBSan host checks for exact
RGBA, signed dimension boundaries, truncation, failed allocation and recovery.
The same test detects the negation error in 6.5.205. This is a bounded BMP fix;
it does not add image formats, aggregate browser image budgets or asynchronous I/O.
The warning-free GCC 14 build passes 229 kernel tests in QEMU, including `bmptest`
5/5 with seven malformed inputs rejected, and reaches login with four CPUs.
Sanitizer and allocation-failure checks are host evidence; no new GUI, general
image-format fuzzing or physical-hardware qualification is claimed.

### GUI — `kernel/gui/`
- **core/** — the **compositor** and window manager: window stacking, dragging, focus, the
  taskbar, the login screen, and the animated **wallpaper** system (`draw_background()`
  renders one of several Nightfall styles, including animated ones).
- **apps/** — Terminal, File Manager, a text editor, Paint, a calculator, an image viewer,
  the **Selene** web browser (its own identity — TLS + HTML/CSS + animated GIF/JPEG), a
  sound tester, and a task manager.
- **games/** — DOOM (the real 1993 shareware, playable), Pong, Snake, Tetris, Minesweeper,
  and the **"Nyx" render/perf demos** (see below).

> The GUI currently runs **in the kernel** (ring 0). NyxOS already exposes a ring-3
> windowing ABI (`SYS_WIN_*`, proven end-to-end by `user/wintest.c`) as the groundwork for
> moving apps to user space; the audit + phased migration plan is in
> [USERSPACE_GUI.md](USERSPACE_GUI.md).

### Toolchain & packages
NyxOS carries a **TinyCC** port (`user/tcc/`) exposed in-OS as `cc`: it compiles C
*inside* the running OS and **self-hosts** (tcc compiles tcc). **`xbm`** is the package
manager — NyxOS's answer to pacman/apt — which builds a package from a recipe with `cc`
and installs the binary to `/mnt/bin` (run by bare name thereafter). The kernel also
implements **`dlopen`/`dlsym`** (dynamic-library loading) for userland.

---

## The shell & command dispatch

There are two shells. The **kernel command interpreter** (`execute_command` in
`kernel/core/kernel.c`) drives the Terminal app and understands ~110 built-in commands,
grouped into categories by `help`. The **userland shell** `user/sh.c` (`sh.elf`) is a real
ring-3 program with pipelines, redirection, script mode, and a REPL.

Command resolution in the kernel shell is the key wiring to understand:

```
execute_command("xxd /foo.png")
  → look up "xxd" in the builtin command table  (miss)
  → resolve_user_elf("xxd"): try /xxd.elf, /xxd, /mnt/bin/xxd  (hit: /xxd.elf)
  → run_foreground_elf("/xxd.elf", argv, argc)
```

So **any coreutil bundled in the initramfs runs by bare name** without needing a kernel
entry — the fallthrough handles it. Some common utilities (`cat`, `nl`, …) *also* have a
native builtin that shadows the ELF for speed; most (`cut`, `tac`, `cal`, `xxd`, …) are
pure ELFs relying on the fallthrough.

---

## Userland & libc — `user/`

Userland programs are ordinary ELF executables linked at `0x10000`, starting at `crt0.o`'s
`_start`, which sets up `argv`/`environ` and calls `main`. They link against **`libc.so`**
(a `--just-symbols` shared image, so there is no runtime dynamic linking cost) which
provides the familiar surface: `malloc`/`free`, `str*`/`mem*`, `printf`/`snprintf`, a
buffered `FILE*` stdio layer, `ctype`, `strtol`, `qsort`, `getenv`, and `setjmp`. Under
that, `syscall.h` wraps the raw kernel calls (`open`/`read`/`write`/`fork`/`execve`/…).

The ~60 coreutils (`cat`, `ls`, `grep`, `sort`, `wc`, `cut`, `nl`, `xxd`, `cal`, `find`,
`cp`, `mv`, `ps`, `top`, `edit`, …) are small, self-contained programs in this style —
read the short ones (`cat.c`, `nl.c`) first; they are the template for a new tool.

---

## Extending NyxOS — recipes

These are the reusable patterns the codebase is built on. Follow the peer that already
exists; the makefile and dispatch tables are the only "wiring" you touch.

### Add a coreutil (userland ELF)
1. Write `user/<name>.c` including `libc.h`; `main(argc, argv)` does the work.
2. In `kernel/makefile`: add `$(USER_DIR)/<name>.elf` to `USER_ELFS`, and add the two
   rules for `<name>.elf` (link) and `<name>.o` (compile) — copy the `cal.elf` pair.
3. Build. It is bundled into the initramfs and **runs by bare name** via the fallthrough.

### Add a kernel builtin command
1. Write `static void cmd_<name>(int argc, char** argv)` in `kernel/core/kernel.c`.
2. Add a forward declaration near the other `cmd_*` decls, an entry in the `commands[]`
   table (`{"<name>", cmd_<name>, "one-line help", false}`), a longer entry in the
   detailed-help array, and the name into the right `HC_*` help category.

### Add a "Nyx" perf/visual demo window
The demo family (`voxel`, `fractal`, `julia`, `particles`, `rotor` + the `fire`/`matrix`/
`lava` effects) all share one shape — a per-window context, an `on_tick` that animates, a
draw function, and a **benchmark HUD** (per-frame render time via `get_ticks()` + derived
FPS). To add one:
1. `kernel/gui/games/<name>_win.{c,h}` — `create_ctx`/`draw`/`tick`/`key`, all integer math.
2. `launch_<name>()` in `kernel/gui/core/compositor.c` (+ `#include` the header).
3. `cmd_<name>` + a `commands[]` entry + help in `kernel/core/kernel.c`, and a
   `launch_<name>` decl in `kernel/core/kernel.h`.
4. `<name>_win.o` into `OBJS_KERNEL` in the makefile.

### Add a wallpaper style
Add a `WP_STYLE_*` to `kernel/gui/core/wallpaper_win.h`, render it in `draw_background()`
in `compositor.c` (integer parabolic sine `wp_isin` for animation, blend from the local
sky gradient so faded edges are invisible), add it to the animated-repaint gating, and add
a picker button. Keep it in the Nightfall palette. See **`docs/WALLPAPERS.md`** for the full
wallpaper/background subsystem: the eight styles, the integer-animation pattern (`wp_isin`), the
repaint gate, and a step-by-step recipe.

### Add a CI self-test (KAT)
Only for **pure kernel logic**. Write `int <name>_selftest(void)` returning 0 on pass,
add an `extern` decl and a `{"<name>", <name>_selftest}` entry to the `t[]` array in
`run_selftests()` (`kernel/core/kernel.c`). It then runs in CI on every build.

---

## Build & run

The host build entry point is **`build.ps1`** (Windows PowerShell), which shells out to
**WSL** to run the cross-toolchain `make -C kernel`, producing `nyx-kernel.bin`, then wraps
it into a GRUB-bootable **`NyxOS.iso`** with `grub-mkrescue`.

```bash
./build.ps1            # compile kernel + all userland ELFs, build the ISO
```

> **Gotcha:** a bare `make` builds the kernel binary but **does not** rebuild `NyxOS.iso` —
> only `build.ps1` does. Boot the ISO, or QEMU will silently run a stale kernel.

Run it under QEMU (headless, serial to a log — the pattern the tests use):

```bash
qemu-system-x86_64 -cdrom NyxOS.iso -hda ext2-test.img -m 512 -boot d \
  -no-reboot -serial file:boot.log -vga std -display none
```

Add `-netdev user,id=n0 -device rtl8139,netdev=n0` to exercise the NIC.

---

## Continuous integration

GitHub Actions (`.github/workflows/`) builds the kernel, assembles the ISOs, smoke-boots
NyxOS in QEMU to the login prompt, and runs the **self-test battery** — a set of
**Known-Answer Tests** driven by booting with the `selftest` kernel command line. The
battery halts the machine after printing a `SELFTEST-SUMMARY` line so CI can read the
serial log. As of this writing there are **26 KATs**:

| Domain | Tests |
|---|---|
| Crypto | `sha512`, `pbkdf2`, `csprng`, `aes_gcm`, `curve25519`, `p256`, `p384`, `rsa`, `der`, `base64`, `x509` |
| TLS | `tls_prf`, `tls_keysched`, `tls_record`, `tls_ske_p384` |
| Images | `inflate`, `png`, `bmp`, `gif`, `jpeg`, `imgreject` |
| FS / Net / Misc | `ext2dir`, `tcpcksum`, `httpparse`, `mathx`, `crc32` |

Other workflows handle CodeQL analysis, tagged releases (an ISO on a `v*` tag), and
publishing the `docs/` Pages site.

---

## Testing & verification philosophy

- **Pure logic → a self-test.** Deterministic kernel logic gets a KAT in the battery so it
  is checked on every build forever.
- **New kernel logic mid-development → a temporary serial hook.** Drop a small block
  before the `run_selftests()` call in `kernel_main`, print a marker over serial, boot
  headless, read the log, then remove it. The serial channel is the ground truth.
- **Visual changes → screendump.** Boot under QEMU with a monitor socket, log in, and
  `screendump` the framebuffer to inspect the actual pixels.

The serial console is the backbone of all of this: kernel `printf` reaches it, so a headless
QEMU run with `-serial file:…` captures a complete, greppable boot transcript.

---

## Coding conventions

- **Language:** all code, comments, and commit messages are in **English**.
- **Freestanding constraints:** integer-only in the kernel (no floats / SSE), no libc, no
  libgcc — avoid constructs that pull in compiler runtime helpers (128-bit division, etc.).
- **Includes:** relative to the file's own directory; `.o` outputs stay flat via `vpath`.
- **Style:** match the surrounding file's naming and comment density. Comments explain the
  *why*, not the obvious *what*.
- **Identity:** NyxOS has its **own** name for everything (the browser is *Selene*, the
  package manager is *xbm*, the demos are *Nyx …*) — extend the identity, don't clone.

---

*NyxOS — a night-goddess OS, built from the metal up.*
