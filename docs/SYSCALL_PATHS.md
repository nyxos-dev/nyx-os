# Checked syscall paths

Development version 6.5.215 rejects incomplete paths before filesystem access.
Previously `copy_path_from_user` accepted a raw string shortened to 127 bytes,
and `path_resolve` could shorten a joined path or silently omit components
after its 32-entry stack filled. A write or removal could consequently target
an existing prefix rather than the requested name.

The path copier now uses the checked string-copy helper introduced for exec.
The normalizer returns failure for raw/joined buffer overflow, component-stack
overflow or insufficient output space. Its caller propagates that failure
before invoking the filesystem. Failed normalization leaves its output buffer
unchanged; it does not provide a partial path for the caller to use.

| Boundary | Contract |
|---|---|
| Raw syscall path | At most 127 bytes plus the actual NUL terminator |
| Normalizer work buffer | 256 bytes; raw absolute path and raw cwd/path join must fit |
| Component stack | At most 32 live components, including the working directory |
| Normalized output | Must fit completely, including its terminator |
| Rejection | Existing generic syscall failure `-1`; no new errno ABI |

The shared helper covers `open`, `stat`, both operands of `rename`, `getdents`,
`chdir`, `mkdir`, `unlink` and `dlopen`. Existing `.`, `..`, repeated slash,
root containment and empty-path behavior remain. The bounds are conservative:
a raw join or intermediate component stack that overflows is rejected even
if later `..` components would shrink it. The change adds no dynamic allocation.

## Verification

```sh
python3 tools/test_syscall_paths.py --compare-ref 56b7a47
python3 tools/test_exec_input.py
```

The actual copying, normalization and existing normalization KAT functions
pass 67,421 host checks under ASan/UBSan with mapped user-memory stubs. The
preceding source fails 16,883 assertions. Checks include raw lengths, faults
at each source byte, joined lengths, stack limits, output sizes, unchanged
output on rejection and preserved normalization. Exec-copy regression still
passes 5,365 checks. The GCC 14 build has no warnings and passes the RTC/ELF
contracts, 230 kernel tests with one CPU, and four-CPU login.

A one-CPU, 512-MiB QEMU guest repeats direct syscall tests in RAM and on a
disposable EXT2 image. Raw 128-byte paths, 33-component resolved paths and
256-byte joined paths reject `open(O_TRUNC)`, `unlink`, both rename operand
positions and `stat`. Existing prefix files retain `KEEP`. A joined path of
255 bytes and an ordinary rename succeed. Offline extraction confirms four
exact `KEEP` files (16 bytes total) from the EXT2 image.

The first boot reader encountered a host log I/O error; the repeated boot
passes. Guest file checks also passed before an offline extraction hit the
host filename-length limit. Hash-named artifact files verify that same disk
image without repeating the guest run. All VMs are closed. These are host
and serial-guest results, not new GUI or physical-hardware qualification.

## Remaining limits

- Individual filesystem component-name bounds and other VFS/kernel-shell
  entry points are separate. This change does not establish a universal
  path-length or complete-input contract across every OS API.
- Shell PATH-search candidates, variable storage, intermediate expansion
  buffers and parser limits still need separate work; see [SHELL_ENTRY.md](SHELL_ENTRY.md).
- `execve` retains its independently checked input path and argument contract.
  Other uses of the legacy string copier are unchanged.
- Concurrent user-memory modification, filesystem races, backing-write
  failures, save atomicity and crash recovery are outside this increment.
  Retaining a full pathname does not make a save transaction atomic.

See [MOUNT_PATHS.md](MOUNT_PATHS.md) for mounted flush paths and
[DAILY_USE.md](DAILY_USE.md) for the broader acceptance plan.
