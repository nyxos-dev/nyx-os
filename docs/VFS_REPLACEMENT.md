# RAM file replacement

Development version 6.5.197 fixes data loss in the RAM branch of
`vfs_write_file`. Previously, replacing an existing file freed its old buffer
before allocating the new one. An allocation failure returned `-1` with a null
data pointer and the old nonzero size. Creating a new file could likewise leave
an empty directory entry after reporting failure.

The replacement now allocates and copies the complete new content before
replacing the existing buffer. A failed allocation preserves the old bytes,
size, inode identity and open references. The source may point into the old
buffer: copying finishes before that buffer is freed. A successful zero-length
replacement truncates the file without allocating memory.

New files enter the parent directory only after their content is ready. Failed
allocation returns the reserved inode to the pool. If the directory is full,
both the prepared buffer and reserved inode are released. Existing directory
entries remain intact. Read-only files and directory targets are still refused.

## Verification

Run `python3 tools/test_vfs_replace.py` on Linux or WSL. It executes the actual
replacement function, inode structure, pool allocation/recycling and bounded
directory insertion functions, with controlled path/mount fixtures and allocator
failure injection. AddressSanitizer and UndefinedBehaviorSanitizer check memory
accesses. The original 6.5.197 run's 2,263 checks include repeated allocation failures, inode exhaustion,
full directories, empty files, alias input, retained open references and unchanged
mounted-filesystem callback results. The regression fails against 6.5.196 before
dereferencing its freed buffer.

From 6.5.198, four additional checks cover the signed byte-count bound, bringing
this runner to 2,267 assertions. See [VFS_WRITE_BOUNDS.md](VFS_WRITE_BOUNDS.md).

The kernel's `vfs-replace` selftest exercises the real path lookup, allocator,
open descriptor, alias replacement, read-only refusal, truncation and readback.
Allocation failure injection belongs to the host test; the kernel selftest does
not simulate memory pressure. Existing editor, desktop configuration and package
tests remain useful caller checks but use a mocked VFS.

For 6.5.197, the warning-free GCC 14 build passes all 228 kernel selftests in
one-CPU, 512 MiB QEMU and reaches login with four CPUs. A diskless desktop trial
creates the home configuration by selecting blue, replaces it by selecting green,
and reads the new configuration through the terminal. Both saves display the
RAM-only notice. This is a bounded functional check, not an out-of-memory GUI test
or a long-session measurement.

## Limits

Preparing a replacement requires the old and new buffers to coexist briefly.
This can reject a write earlier under memory pressure, while preserving the
existing file. The managed physical-memory and kernel-heap limits are unchanged.

This change does not provide concurrent-writer synchronization, a filesystem
transaction, power-loss recovery or persistent RAM files. Mounted filesystem
callbacks retain their existing semantics; in particular, an EXT2 replacement
can still damage the old file on failure. Descriptor writes through `vfs_write`
and `vfs_pwrite` are separate paths. No general atomic-save guarantee follows
from this repair.
