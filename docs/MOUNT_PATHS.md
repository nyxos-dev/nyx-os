# Mounted file path preservation

Development version 6.5.213 fixes a data-loss bug in the VFS mount mirrors.
`vfs_open` previously stored the complete mount-relative path in a 64-byte
filename buffer. A later close flushed dirty data using only its first 63
bytes. Creating a longer file could leave the requested file empty and write
its contents to another name, including an existing shorter file.

The mirror now stores `mpath[MAX_PATH]`, matching the 256-byte VFS path buffer.
Both file and directory mirrors retain the complete validated path. Open
rejects null inputs and raw paths of 256 bytes or more. For relative opens
from a mounted working directory, it also rejects a raw `cwd + / + path`
of 256 bytes or more before normalization, allocation or mount callbacks.
This conservative check can reject a path whose `..` segments would make the
normalized result shorter. It does not silently substitute a shortened name.

The change adds 192 bytes to each of the 512 statically allocated VFS nodes:
96 KiB for the node pool. It introduces no additional per-open allocation.
This is a correctness tradeoff, not a memory-usage optimization. Component
names, userspace syscall copying and application input fields retain their
own bounds; a 255-byte VFS buffer does not mean every application accepts
255-byte paths.

## Verification

```sh
python3 tools/test_vfs_mount_paths.py --compare-ref d6d307c
python3 tools/test_vfs_replace.py
python3 tools/test_vfs_growth.py
```

The mount-path harness extracts the actual node layout, path joining, open
and flush functions, with recording mount callbacks. ASan/UBSan runs pass
2,417 checks; the previous revision fails 655 assertions. Cases cover
mount-relative lengths 1 through 251 under `/mnt`, file/directory mirrors,
existing/new files, rejected raw inputs and mounted joins at the 255/256-byte
boundary, and node-allocation failure. The existing replacement and growth
suites pass 2,267 and 8,302 checks respectively.

The GCC 14 kernel build has no warnings. Build contracts cover RTC behavior,
nine assembly objects and 103 user ELF stack declarations. QEMU passes all
230 kernel self-tests with one CPU and reaches login with four CPUs.

A separate ring-3 test program in a one-CPU, 512-MiB VM writes, closes,
reopens and updates nested files in RAM and on a disposable EXT2 image. The
mount-relative paths are 63, 64, 95, 100 and 127 bytes long. For paths beyond
63 bytes, a preexisting file at the old truncated prefix retains `KEEP`.
Offline extraction verifies five exact `UPDATE!` files, four exact `KEEP`
collision files and one unchanged `KEEP` survivor: 55 bytes total. The VM
is closed. This test bypasses shell expansion so that its separate argument
truncation cannot hide or misattribute the VFS result.

## Remaining limits

- The userspace shell still truncates some literal arguments to 95 bytes
  during glob expansion. This separate reproduced bug remains a priority.
- Other VFS entry points and syscall path-copy/normalization helpers are
  unchanged. These checks do not establish complete path rejection across
  all callers, long working directories or all filesystem operations.
- Mounted flushes still ignore backing write failures and lack atomic
  replacement, crash recovery and concurrent-writer protection. Preserving
  the path does not make save transactions reliable under every failure.
- There is no new GUI, hardware or long-duration workspace qualification
  in this increment. Host boundary tests and actual guest disk-byte tests
  have the distinct scopes described above.
