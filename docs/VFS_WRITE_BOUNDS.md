# VFS write bounds

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
physical storage reliability or power-loss recovery. General `lseek` range and
signed-arithmetic handling remain a separate audit; the repaired write path rejects
unrepresentable resulting writes. This increment does not expand memory capacity,
provide sparse physical allocation or qualify a complete daily-use session.
