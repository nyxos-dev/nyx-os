# NyxOS Filesystem & VFS

How NyxOS names, stores, and serves files. The virtual filesystem lives in
[`kernel/fs/vfs.c`](../kernel/fs/vfs.c), the on-disk driver in
[`kernel/fs/ext2.c`](../kernel/fs/ext2.c). This complements
[ARCHITECTURE.md](ARCHITECTURE.md) (whole-system view) and
[PROCESS.md](PROCESS.md) (per-process cwd and fd tables).

---

## 1. One namespace, several backends

Everything hangs off a single root `/`. Four kinds of thing live in that tree:

```
/                       ramdisk (in-memory tree of vfs_node_t)
├── bin, etc, home, …   ramdisk files and directories
├── dev/                synthetic device nodes  (null, zero, random)
├── proc/               generated kernel state   (meminfo, uptime, <pid>/…)
└── mnt/                a real EXT2 disk, auto-mounted at boot
```

The VFS resolves a path, decides which backend owns it, and dispatches. The
ramdisk is the default; `/mnt` is backed by a real EXT2 partition; `/dev` and
`/proc` are computed on the fly.

---

## 2. The node pool

Every ramdisk file or directory — and every transient mirror of a mounted-FS
file — is a `vfs_node_t` drawn from a fixed pool:

```c
#define MAX_INODES   512   // total node pool
#define MAX_CHILDREN 128   // entries per directory
```

Key `vfs_node_t` fields ([vfs.c](../kernel/fs/vfs.c)):

| Field | Meaning |
|-------|---------|
| `name`, `type` | name and 0=file / 1=dir |
| `size`, `data` | length and the in-memory bytes (ramdisk files) |
| `parent`, `children[]`, `child_count` | the tree structure |
| `open_refs` | how many live fds point here — the pool must not recycle a node anyone still holds |
| `orphaned` | unlinked from the tree while still open; freed at the last close |
| `mount_backed`, `mpath`, `mount_ent`, `dirty` | a transient node mirroring an EXT2 file, and whether it has unflushed writes |
| `dev_type`, `proc_type`, `proc_pid` | `/dev` special or generated `/proc` node |

---

## 3. File descriptors

A VFS "fd" **is a pointer to a `vfs_node_t`** — there is no separate open-file
table at the VFS layer. `open_refs` reference-counts it: +1 on `open`, +1 per
`dup`, −1 per `close`, and the node is returned to the pool only at zero. This is
what stops the pool from handing the same memory to a new file while an old fd is
still live. Ring-3 processes never see these raw pointers: the syscall layer wraps
them in small per-process integer fds (see [PROCESS.md](PROCESS.md) §8 and
[SECURITY.md](SECURITY.md)).

The public API ([kernel.h](../kernel/core/kernel.h)):

```c
int vfs_open(path, flags, mode);   // flags: O_CREAT (1), O_TRUNC (2). O_APPEND (4) is defined
                                   // but not honored — vfs_write REPLACES the whole file (no
                                   // per-fd position); use vfs_pwrite to append or stream.
int vfs_read(fd, buf, count);      int vfs_write(fd, buf, count);
int vfs_pread(fd, buf, count, off);int vfs_pwrite(fd, buf, count, off);
int vfs_close(fd);
int vfs_stat(path, *size, *is_dir);int vfs_isdir(path);
int vfs_mkdir(path, mode);         int vfs_unlink(path);
int vfs_chdir(path);               const char* vfs_getcwd(void);
int vfs_mount(mount_point, fs_type, fs_data);
```

---

## 4. Path resolution and the working directory

Each process carries an absolute, normalised `cwd` (see [PROCESS.md](PROCESS.md)).
A relative path is joined onto it and the `.`/`..` segments are collapsed by
`join_mount_relative` — the canonicaliser pinned by the `pathnorm` self-test and
exposed to userland as [`realpath`](../kernel/fs/vfs.c) (`vfs_realpath`). Two
subtleties worth knowing:

- **`vfs_abs` normalises only relative-under-mount paths**; an already-absolute
  path with `.`/`..` is returned unchanged. Use `vfs_realpath` when you need a path
  with `..` in it fully collapsed.
- **`..` can never escape root** — stacked `..` past `/` stays at `/`. This
  containment is a property the `pathnorm` KAT verifies explicitly.

Because a mounted directory has no ramdisk node, `mount_dir_stub()` builds a chain
of lightweight stub nodes so `getcwd()` and `cd ..` keep working while the CWD is
somewhere under `/mnt`.

---

## 5. The EXT2 driver

`/mnt` is a **real, read/write EXT2 filesystem** on a disk partition, auto-mounted
at boot (`mount_table` holds up to `MAX_MOUNT_POINTS = 8` mounts). The driver
([ext2.c](../kernel/fs/ext2.c)) implements the parts a working Unix FS needs:

- **Variable block size** — 1 KiB, 2 KiB, or 4 KiB (`1024 << s_log_block_size`),
  read from the superblock.
- **Direct, singly-indirect, and doubly-indirect** block maps, so files grow well
  past the 12 direct blocks; sparse files (holes) are handled.
- **Superblock, group descriptors, inode tables, and block/inode bitmaps** for
  allocation, with a small set of reusable per-purpose block buffers (a general
  block, a bitmap block, and two auxiliary blocks — the doubly-indirect walk needs
  two live at once).
- **RTC timestamps** on inodes (`ext2_now`), so `mtime`/`dtime` are real and
  `e2fsck` doesn't flag "deleted inode has zero dtime".

**Write batching (v6.4.10).** Offset writes through `vfs_pwrite` (including
userspace `write`) mark a mounted file's mirror node
`dirty`; the whole file is flushed to disk once, at the **last close**. The
earlier flush-on-every-`write()` re-wrote the entire file per 4 KiB chunk — O(n²)
I/O that made the ~496 KiB in-OS `tcc` object take minutes. Now it is linear.
The kernel's whole-file `vfs_write` and `vfs_write_file` paths persist immediately.

### I/O failures (6.5.255)

ATA checks both command-error and device-fault status after write/flush completion.
EXT2 installs sector-cache lines only after successful transfers. A failed read,
write or device flush invalidates the cache and blocks further writes through that
EXT2 mount. Reads remain available when the device can satisfy them. A fresh mount
resets this guard; it does not repair the filesystem. Recover the underlying device
and check the filesystem offline before resuming writes.

Errors propagate through data blocks, inode tables, allocation bitmaps, group
descriptors, directories and the superblock. `ext2_resolve` and mount `resolve`
return a positive inode, zero for absence, or `-1` for an error. `get_size` returns
the byte count, including zero for an empty file, or `-1`. Mounted open and copy
require a complete readable image; a failed read cannot become an empty file.
Account lookup errors do not select fallback credentials, and failed account
reads do not replace the existing account file during user creation.

A successful userspace `write` can still mean only that bytes reached the VFS
buffer. Applications must check the **last `close`**: it returns `-1` if the
filesystem write or flush fails. Close consumes that descriptor and releases its
buffer even on error; retrying the closed descriptor is invalid. A failed immediate
`vfs_write` returns `-1` and keeps its open node dirty. The kernel `echo` and `write`
commands report write failures. Mnemosyne keeps its document marked as modified
when a save fails, allowing the user to copy or save the text elsewhere.

This is error reporting and containment, not atomic replacement or journalling.
Some sectors or allocation metadata may already have changed before an error;
partial files, leaks and a required filesystem repair remain possible. A successful
same-session readback is not proof of power-loss durability. RAM rescue is temporary.

---

## 6. Special filesystems

**`/dev`** — synthetic device nodes whose reads/writes bypass `node->data`:

| Node | Behaviour |
|------|-----------|
| `/dev/null` | reads return EOF; writes are discarded |
| `/dev/zero` | reads return endless zero bytes |
| `/dev/random`, `/dev/urandom` | reads return cryptographically-secure random bytes from the kernel CSPRNG (`csprng_bytes`); `urandom` is an alias. (This replaced an earlier tick-seeded xorshift64, whose output was predictable.) |

**`/proc`** — kernel state generated on read (nothing is stored):
`meminfo`, `uptime`, `version`, `cpuinfo`, `mounts`, and a per-process
`/proc/<pid>/` directory with `status`, `cmdline`, and `maps`. `proc_sync()`
creates and removes the per-pid directories to track the live process table.

---

## 7. The command surface

| Command | Purpose |
|---------|---------|
| `ls`, `cd`, `pwd`, `tree` | browse the tree |
| `cat`, `head`, `tail`, `wc` | read file contents |
| `touch`, `mkdir`, `rm`, `cp`, `mv` | create / remove / copy / move |
| `find`, `grep` | search by name / content |
| `stat`, `file`, `du`, `df` | metadata, type, disk usage, free space |
| `realpath` | canonical absolute path |
| `mount`, `disks`, `lsblk`, `ext2ls`, `ext2cat` | mounts and raw EXT2 inspection |

Plus the userland coreutils in [`user/`](../user) (`ls`, `cp`, `dd`, `od`, `xxd`, …).

---

## 8. Limits and non-goals

- **`MAX_INODES = 512`** ramdisk nodes and **128** children per directory — fixed
  pools, no dynamic growth. Sustained in-OS compilation used to exhaust the node
  pool (issue #66, since resolved); the 256→512 bump gave it headroom. The pools
  are still bounded, so a pathological workload could in principle refill them.
- **No `truncate`/resize primitive** — files grow by writing; there is no VFS call
  to shrink or set an exact size yet.
- **No symlinks and no permission enforcement** — the mode argument is accepted but
  access is not gated on it; `..` containment (not per-file ACLs) is the security
  boundary here. See [SECURITY.md](SECURITY.md).
- **At most 8 mounts**, and only one EXT2 partition is auto-mounted at boot.

NyxOS is an experimental system; the filesystem favours a small, auditable core
over completeness.
