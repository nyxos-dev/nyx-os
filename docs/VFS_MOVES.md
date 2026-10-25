# File moves and failure reporting

Development version 6.5.261 makes `vfs_rename` return `0` on success and `-1`
on refusal or an incomplete move. `SYS_RENAME`, the kernel `mv` command, the
installed `/mv.elf`, and File Manager use that result. An existing destination
is no longer treated as evidence that a failed move succeeded.

## Contract

- Moving an existing file to the same normalized path does nothing. Missing
  sources still fail. The checked normalizer shared with syscall path copying
  collapses `.`, `..` and repeated slashes, and rejects overflow instead of
  silently truncating. Move components must fit the 63-byte name limit.
- A different existing destination is refused, preserving both files. This
  conservative policy also applies to mounted files; it is not POSIX atomic
  replacement semantics. Two paths resolving to the same inode through the
  same mount entry are a no-op.
- RAM moves refuse missing/non-directory parents, name collisions, a full target
  directory and moving a directory below itself. The target insertion succeeds
  before detaching the source. A 63-byte final component is explicitly terminated.
- A move involving a mount still uses copy followed by unlink. A failed copy
  never triggers source unlink. A failed unlink returns an error even if a
  destination copy exists. RAM copy creation now rejects data-buffer allocation
  failure instead of publishing an empty file as a successful copy.
- `/mv.elf` uses the existing rename syscall instead of its separate copy loop,
  which previously ignored read and close errors. It accepts exactly `SRC DST`
  and treats `DST` as the exact target path. The kernel shell additionally
  expands an existing directory destination to `DST/basename(SRC)` and rejects
  expansion that would truncate the path.
- File Manager retains a cut clipboard after failure. Paste, drag-move and
  rename show the returned result; refreshing the directory preserves that
  message. The existing copy-paste naming policy and keyboard translation are
  separate work.

## Host verification

Run `python3 tools/test_vfs_moves.py --mutations` and
`python3 tools/test_move_callers.py` on Linux or WSL. The first executes the real
normalizer, tree lookup, rename and copy bodies against a RAM tree and recording
mount callbacks, with allocator and I/O failure injection. Its 264 checks cover
aliases, missing sources, occupied targets, read/short-read, allocation,
write/short-write, unlink errors, cross-boundary moves, capacity, cycles and name
bounds, including refusal when the move-path allocation fails. Four mutations that restore unsafe behaviour are detected.

The 21 caller checks exercise the actual syscall branch, installed-command source,
cut/paste body and refresh/status helper. They verify failure return values,
clipboard retention, destination construction and visible status preservation.
AddressSanitizer and UndefinedBehaviorSanitizer run both suites. CI runs the
unmodified suites; mutation checks are an additional host qualification.

The suite also compiles the actual VFS and EXT2 objects using their Makefile
recipes and checks frame budgets of 256 bytes for `vfs_rename` and 128 bytes for
`ext2_sync_superblock`. These individual bounds are regression guards, not a
proof of the entire call chain. Move paths use a checked temporary allocation;
the dedicated superblock buffer is serialized by the existing EXT2 lock and
does not alias a caller's live block buffer.

Existing syscall-path, RAM replacement, mounted-path, growth and storage-error
regressions remain applicable. Mocked storage failures do not prove physical
device recovery.

## Target and GUI verification

A four-CPU QEMU/TCG VM with 5 GiB configured RAM and a disposable EXT2 overlay
checks 36 syscall/file-content assertions across RAM and mounted paths. The
installed `mv` returns success for an existing same-path source, failure for an
occupied target or missing source, and success for a new target. The installed
`libctest` includes 32 consecutive mounted moves with exact readback, plus no-op
and collision cases; its complete suite passes.

The final image retains all six live process stack canaries after a mounted
move. GCC reports 192 bytes for the move frame and 32 bytes for superblock sync;
these are individual frames, not total stack consumption. The build has no
compiler warnings, passes 230 kernel self-tests and reaches login with four CPUs.

File Manager GUI checks cover refused paste with the cut clipboard retained,
in-place paste, successful paste into another directory, and rename refusal and
success. After the VM closes, independent EXT2 inspection confirms exact bytes
for the moved files and collision target, the moved source's absence, and two
unchanged control files. This is a disposable-image test, not hardware or
power-loss qualification.

## Limits

Mounted moves are not atomic. A failed write may leave a partial destination,
and a failed unlink may leave both copies. Inspect and resolve that destination
before retrying: the move deliberately refuses an occupied target. These checks
do not lock an entire move against concurrent writers or provide crash/power-loss
recovery. Mounted directory relocation, complete symlink semantics, metadata
preservation and physical storage qualification remain open.

File Manager copy-paste still needs a collision-safe generated-name policy and
broader path-boundary coverage. This increment does not enable its deferred
cooked-key shortcuts. See [DAILY_USE.md](DAILY_USE.md),
[VFS_REPLACEMENT.md](VFS_REPLACEMENT.md) and [VFS_WRITE_BOUNDS.md](VFS_WRITE_BOUNDS.md)
for the wider data-integrity acceptance criteria.
