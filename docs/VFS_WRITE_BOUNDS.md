# VFS write and seek bounds

Development version 6.5.198 rejects file writes that cannot be represented by the
VFS's signed `int` size and byte-count results. `VFS_MAX_FILE_SIZE` is
2,147,483,647 bytes. This is an interface bound, not a promise that a file of that
size fits in NyxOS's memory; the existing heap and physical-memory limits remain.

Previously, `vfs_pwrite` checked `offset + count` but added its 512-byte growth
reserve in 32-bit arithmetic without checking that addition. For example, one
byte at offset `0xfffffffe` requested a 511-byte allocation before attempting a
copy near 4 GiB. The host reproducer intercepted allocation and forced failure,
so it established the incorrect size without performing the invalid access.

`vfs_pwrite` now validates the count and final position before allocating or
changing a regular file. Capacity growth saturates at the interface bound;
doubling the old capacity cannot wrap. Ordinary small writes still grow
geometrically and zero-fill sparse gaps. A zero-byte positional write returns
zero without allocation, size changes or dirtying the file, even at an extreme
offset. Invalid handles and directories still fail. Device writes continue to
discard accepted data and do not allocate a file buffer.

`vfs_write` and `vfs_write_file` also reject counts above the signed result bound.
This prevents whole-file allocation arithmetic from wrapping or a successful
large write from returning a negative byte count. `vfs_write_file` applies this
count check before dispatching a mounted callback. Its RAM replacement
preservation from 6.5.197 remains in place; see [VFS_REPLACEMENT.md](VFS_REPLACEMENT.md).

## Verification

`python3 tools/test_vfs_growth.py` executes the real write and growth functions
with AddressSanitizer and UndefinedBehaviorSanitizer. Its 8,302 assertions cover
boundary arithmetic, allocation refusal, preservation of existing bytes, sparse
gap contents, zero writes, devices and 64 successive 4 KiB writes. Most assertions
check individual gap bytes; this is not a claim of thousands of distinct scenarios.
The older 6.5.197 source fails the boundary regression with allocation intercepted.
The replacement runner also passes 2,267 assertions, including the new count guard.

The warning-free GCC 14 kernel passes all 228 QEMU selftests; `vfspwrite` now checks
extreme positions and zero-byte writes on a live inode. A four-CPU boot reaches
login. A separate one-CPU, 512 MiB QEMU run executes a ring-3 program against RAM
and a private EXT2 disk. Five extreme positions reject one-byte writes, failed
writes preserve the descriptor position and original `KEEP` data, and zero writes
preserve size. A subsequent ordinary sparse write succeeds, its gap reads as zero,
and close/reopen retains the file. Extracting the disk image yields exactly
`KEEP`, 8,188 zero bytes and `END` (8,195 bytes total).

A separate QEMU integration run installs and verifies the embedded `ncc`, `nlex`,
`nparse` and `ngen` packages, checks 18 installed-driver cases, generates Workbench C
with both `ncc` and `ngen`, and compiles both outputs using the in-OS C compiler.
Both programs exit successfully and produce the exact 301-byte reference trace.
This checks the changed write path under a real toolchain workload; it adds no
new N or N++ language feature.

## Remaining limits

These checks do not establish concurrent-writer safety, atomic mounted writes,
physical storage reliability or power-loss recovery. The 6.5.198 write change did
not repair `lseek`; the 6.5.200 follow-up below adds its range checks. The write
path rejects unrepresentable resulting writes. These changes do not expand memory capacity,
provide sparse physical allocation or qualify a complete daily-use session.

## Seek positions (6.5.200)

Descriptor positions are stored as `uint32_t`, independently of the smaller signed
write-result bound. `SYS_LSEEK` accepts positions from **0 through 4,294,967,295**.
Seeking beyond EOF changes only the position: it does not allocate, extend the file
or make a subsequent out-of-range write valid.

The syscall validates the signed relative offset before adding it to the base for
`SEEK_SET`, `SEEK_CUR` or `SEEK_END`. Negative or unrepresentable results return
`-1` and leave the previous position unchanged. Invalid `whence` values, including
high bits supplied through a raw syscall, also fail without moving. `SEEK_END`
uses the unsigned VFS size directly, avoiding the old conversion through signed
`int`. Existing pipe/socket refusal is unchanged.

Previously, seeking to 4 GiB succeeded with position zero after truncation to
32 bits. Adding a large positive signed offset to the current position or size
could overflow `int64_t`. The host baseline reproduces both the wrong position and
a signed-overflow diagnostic from UndefinedBehaviorSanitizer.

`python3 tools/test_lseek.py` executes the real syscall branch with mocked
descriptor lookup and VFS size, comparing the result against 128-bit arithmetic.
All **1,162 assertions** pass with address/undefined-behavior sanitizers. Coverage
includes both signed extremes, the full unsigned position range, every origin,
invalid origins, missing descriptors/offsets, pipes/sockets, unchanged rejected
positions and unsigned `SEEK_END` sizes. Large size metadata is simulated; no
multi-gigabyte file is allocated for this test.

The final GCC 14 build has no warnings, passes 229 QEMU kernel tests including
`seek-offset`, and reaches login with four CPUs. In a separate one-CPU, 512 MiB
QEMU run, a ring-3 program tests failed and successful seeks on RAM and a disposable
EXT2 disk, relative writes, close/reopen and pipe refusal. Disk extraction matches
`KEXY`, eight zero bytes and `END` exactly (15 bytes). The existing in-OS libc suite
reports `ALL PASS`, including buffered `fseek`/`ftell`. All test VMs were closed.
There are no GUI or hardware validation claims for this increment.

`vfs_fsize` returns zero for some invalid/non-file states and has no separate error
channel; this change does not resolve that ambiguity. Concurrent descriptor or
file changes, descriptor alias semantics, device seek policy and storage durability
remain separate work. Source: `kernel/core/syscall.c`, `kernel/fs/vfs.c` and
`user/syscall.h`.
