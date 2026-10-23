# NyxOS Testing

How NyxOS proves it still works: an **in-kernel known-answer self-test battery** that runs
inside a real boot, plus the CI that gates every push on it. The tests live in
[`kernel/core/kernel.c`](../kernel/core/kernel.c) (`run_selftests`) and the individual
`*_selftest()` functions scattered through the subsystem sources. This complements
[ARCHITECTURE.md](ARCHITECTURE.md) (whole-system view), [CRYPTO.md](CRYPTO.md) and
[NETWORK.md](NETWORK.md) (the subsystems most of the tests cover).

---

## 1. Why known-answer tests

Most of NyxOS is small, pure, self-contained logic: a hash, a base-N codec, a DEFLATE
inflater, an image decoder, a `cut`/`comm`/`join` text core. Code like that has a *right
answer* for a given input — so the cheapest, strongest way to keep it correct is a
**known-answer test (KAT)**: feed a fixed input, compare the output to a value computed
independently (usually cross-checked against the reference tool on the host — GNU
coreutils, Python `hashlib`/`zlib`, OpenSSL — before the vector is frozen into the kernel).

A KAT that once passed and later fails means a real regression, pinned to one line. The
battery is run on **every** boot in CI, so a change that breaks SHA-512 or the GIF LZW
decoder cannot merge. This is the discipline that keeps a hobby OS honest as it grows.

Wherever it is practical, the *pure logic* is split from the I/O so it can be tested
off-target. For example [`kernel/core/cut.c`](../kernel/core/cut.c) exposes
`cut_parse_list` / `cut_line` (no file access) and `cmd_cut` only wraps them; the KAT
exercises the pure core directly. The same shape is used for `comm`, `join`, `xargs`, the
image decoders, the crypto primitives, and more.

---

## 2. The self-test battery

`run_selftests()` in [`kernel/core/kernel.c`](../kernel/core/kernel.c) holds one table:

```c
struct { const char* name; int (*fn)(void); } t[] = {
    {"sha512",  sha512_selftest},   {"aes_gcm", aes_gcm_selftest},
    {"inflate", inflate_selftest},  {"png",     png_selftest},
    {"cut",     cut_selftest},      {"comm",    comm_selftest},
    /* ... 184 entries and counting ... */
};
```

Each `*_selftest()` returns **0 on pass**, or a non-zero marker identifying the failing
vector. The runner calls every entry, prints one line per test, and finishes with a
machine-readable summary bracketed by two sentinels:

```
SELFTEST-BEGIN
[SELFTEST] sha512       PASS
[SELFTEST] aes_gcm      PASS
...
[SELFTEST] join         PASS
SELFTEST-SUMMARY passed=211 failed=0 total=211
SELFTEST-END
```

The `t[]` array in `kernel.c` is the authoritative, always-current list. Broadly the
battery covers:

| Area | Examples |
|------|----------|
| **Crypto** | SHA-1/256/512, SHA-3, MD5, BLAKE2s, HMAC, PBKDF2, HKDF, AES-GCM/CBC/CTR/KW, ChaCha20-Poly1305, SipHash, CMAC, Curve25519, Ed25519, P-256/384, RSA, the CSPRNG, constant-time compare |
| **TLS / PKI** | TLS 1.2 PRF, key schedule, record layer, ServerKeyExchange, DER, X.509 chain verification |
| **Encodings** | Base16/32/58/64/85, bech32, URL, UTF-8, UUID, ANSI CSI |
| **Checksums** | CRC-16, CRC-32, CRC-32C, Fletcher, FNV, MurmurHash, Adler-32 (in the inflate path) |
| **Compression** | raw DEFLATE, zlib, gzip (round-tripped through the encoder) |
| **Images** | PNG (decode + encode), BMP, GIF (incl. LZW), JPEG, PPM, format sniffing + malformed-input rejection |
| **Networking** | TCP checksum + window math, IPv4/IPv6, `ipcalc`, DNS (build + parse), ARP input parsing, the ICMP echo gate, DHCP options, HTTP response parsing |
| **Text / data** | `cut`, `comm`, `join`, `xargs`, `sed`, `patch`, `fmt`, `pr`, `shuf`, `tsort`, `calc`, `gcd`/`lcm`, JSON (+ query), glob, CSV, INI, the `nyx.conf` desktop-config parser, `strings`, semver, date formatting, path normalization, shell `$VAR` expansion |
| **Disk / boot** | mkfs.ext2 layout, FAT formatter, MBR/GPT partition tables, PCI/NVMe enumeration + Wi-Fi radio identification (Intel AX200 family), the GRUB boot image, the real-hardware framebuffer-source pick (multiboot2 GOP framebuffer vs QEMU Bochs VBE) |
| **Kernel** | page-allocator refcount / COW, W^X mappings, slab/`kfree`, stack canaries, signal delivery, the pipe ring buffer, the ELF loader + dynamic linker, the CPU-utilization accountant, wall-clock uptime (civil→epoch), the software 3D rasterizer + `mat4`, the keyboard + PS/2-mouse packet decode, the ring-3 syscall pointer/length boundary + cwd path resolution (`..` can't escape root), the offset-aware file write behind `pwrite`/`>>` (geometric growth + zero-filled sparse gaps, so a file never reads back stale heap), the CMOS/RTC clock decode (BCD/binary, 12h/24h), the #PF error-code decode for the fault dump, the thread-group fd handoff when a leader exits before its workers (exit_group semantics), the context-cursor shape picked from the window under the pointer (I-beam over text areas, directional double-arrows over the resize borders), the file-manager selection mapping between a real entry index and its display position under a search filter, TOTP / account-lockout |

---

## 3. Running the battery

The battery is gated behind the `selftest` **multiboot command line** — a normal boot
shows the login screen instead. The release build ships a dedicated *self-test ISO* whose
GRUB entry appends `selftest`; booting it runs the tests and halts.

Locally, with QEMU:

```bash
# build the ISO, then boot the self-test entry headless
./build.ps1
qemu-system-x86_64 -cdrom NyxOS.iso -serial file:serial.log -display none -no-reboot
# (select the "self-test" GRUB entry, or use the selftest ISO)
grep SELFTEST-SUMMARY serial.log     # -> passed=N failed=0 total=N
```

A run is **good** only if `SELFTEST-END` is reached, a `SELFTEST-SUMMARY` line is present,
and it reports `failed=0`. Any `FAIL`, a missing summary, or a kernel fault
(`#PF`/`#GP`/`PANIC`) is a failure.

---

## 4. Adding a test

Three steps:

1. **Write** a `int foo_selftest(void)` next to the code it covers. Return `0` for pass, or
   a small non-zero number for the vector that failed (so a regression says *which* case
   broke). Keep the vectors fixed and cross-checked against a reference implementation.

   ```c
   int foo_selftest(void) {
       if (foo("ab") != 0x1234) return 1;   // vector 1
       if (foo("")   != 0)      return 2;   // vector 2 (empty input)
       return 0;
   }
   ```

2. **Declare** it — add the prototype to the subsystem header (if it has one), or `extern`
   it near the top of [`kernel/core/kernel.c`](../kernel/core/kernel.c) alongside the
   other selftest prototypes.

3. **Register** it in the `t[]` table inside `run_selftests()`:

   ```c
   {"foo", foo_selftest},
   ```

Rebuild, boot the self-test entry, and confirm `[SELFTEST] foo PASS` with the summary
still `failed=0`. That is the whole loop.

---

## 5. Continuous integration

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on every push:

- **build** — compiles the kernel warning-free and builds both the normal and the self-test
  ISO.
- **smoke-boot** — boots the normal ISO in QEMU until the login screen is reached, catching a
  boot that faults or hangs before the desktop.
- **selftests** — boots the self-test ISO (cmdline `selftest`) against a blank ext2 disk,
  captures the serial log, and **fails the job** unless it sees `SELFTEST-END` with a
  `SELFTEST-SUMMARY … failed=0` and no kernel fault.

Alongside these, CodeQL scans the sources, a `v*` tag builds a release ISO, and the Pages
workflow publishes this documentation. The self-test job is the gate that matters for
correctness: if the battery is red, the change does not ship.

Both QEMU jobs install required packages with `--no-install-recommends`. They use
BIOS boot, raw ext2 disks and `-display none`; optional GUI, firmware and storage
packages are not part of this test configuration. This reduces dependency downloads
that can exhaust the job timeout before a boot test starts. Required dependencies
remain installed, and the boot and self-test assertions are unchanged.

A job canceled during package installation provides no boot result. Inspect the
failed step separately from kernel output and confirm a successful descendant
containing the change before marking a canceled run covered.

---

*The list of tests grows with the OS. When you touch a subsystem with a pure, checkable
core and it has no KAT yet, add one — it is the cheapest insurance NyxOS has.*
