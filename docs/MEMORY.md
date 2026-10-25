# NyxOS Memory Management

How NyxOS manages physical RAM, the kernel heap, and each process's virtual
address space. Every structure and constant below is grounded in the source: the
physical allocator and kernel heap live in
[`kernel/mm/memory.c`](../kernel/mm/memory.c), the page tables in
[`kernel/mm/paging.c`](../kernel/mm/paging.c), the `mmap` family in
[`kernel/mm/mmap.c`](../kernel/mm/mmap.c), and the constants in
[`kernel/core/kernel.h`](../kernel/core/kernel.h).

This complements [ARCHITECTURE.md](ARCHITECTURE.md) (the whole-system view),
[PROCESS.md](PROCESS.md) (the tasks that own the address spaces), and
[SECURITY.md](SECURITY.md) (NX/SMEP and W^X as security properties).

---

## 1. The three layers

Memory in NyxOS is managed in three cooperating layers:

| Layer | Unit | Who allocates | Source |
|-------|------|---------------|--------|
| **Physical page allocator** | one 4 KiB frame | `alloc_page` / `free_page` | `memory.c` |
| **Kernel heap** | bytes (slab + free-list) | `kmalloc` / `kfree` | `memory.c` |
| **Virtual memory** | per-process page tables + VMAs | the `#PF` handler + `mmap` | `paging.c`, `mmap.c` |

Slab caches request frames from the page allocator. Larger kernel allocations use
a separate, statically reserved 16 MiB heap in `kernel/mm/heap.c`. Virtual memory
maps frames from the page allocator into each address space. Exhausting the fixed
heap and exhausting the physical frame pool are different conditions.

`PAGE_SIZE` is **4096** bytes everywhere.

---

## 2. The physical page allocator

Physical RAM is tracked by a **bitmap**, one bit per 4 KiB frame (`1` = free):

```c
#define MAX_PAGES ((uint32_t)(PHYS_IDENTITY_MAX_BYTES / PAGE_SIZE))
static uint32_t page_bitmap[MAX_PAGES / 32];        // 32 KiB bitmap
```

`alloc_page()` scans the bitmap for a free frame, clears its bit, and returns the
physical address; `free_page()` sets the bit back. The map of usable RAM comes
from the multiboot memory map at boot (`init_memory`), and the low BIOS/MMIO hole
is reserved out so it is never handed to a caller (`reserve_low_pages`). Firmware
holes remain reserved. Since development version 6.5.249, the shared
`PHYS_IDENTITY_MAX_BYTES` limit is **1 GiB**: metadata covers the existing runtime
identity map. Detected RAM above this bound is reported separately and is not
available to allocations. The early assembly bootstrap still maps 128 MiB.

### Reference counts (copy-on-write)

Each frame has a **reference count**, indexed by physical page number:

```c
static uint16_t page_refcount[MAX_PAGES];
```

`alloc_page()` sets it to `1`. When `fork()` shares a page copy-on-write, both
address spaces point their PTEs at the *same* frame and `page_incref()` bumps the
count. `free_page()` decrements it and only returns the frame to the bitmap when
the **last** reference drops. A refcount of `0` means reserved or already free;
`free_page()` rejects it without modifying the pool. The 16-bit table occupies
512 KiB at the 1 GiB limit and supports counts above 255. Some frames — the
shared-libc master pages, mapped read-only
into every process — is pinned and never returned regardless of refcount.

### Locking

The allocator state is shared across CPUs, so it is guarded by `page_lock`, always
taken with interrupts off (a page-fault handler allocates, so re-entering on the
same core would deadlock). `init_memory` runs before any AP exists and stays
unlocked.

---

## 3. The kernel heap

`kmalloc`/`kfree` serve byte-sized kernel allocations from two tiers behind one
lock (`kmalloc_lock`, always taken *before* `page_lock` so the ordering is
deadlock-free):

- a **slab** allocator for small fixed-size classes — each cache hands out
  same-sized objects cut from a page it got via `alloc_page()`; freed objects go
  back on the cache's free list for fast reuse;
- a **free-list heap** for larger requests, which `kmalloc` falls through to when
  no slab class fits.

The large-allocation heap is a static array in the kernel's low identity-mapped
image, reserved from the physical allocator:

```c
#define HEAP_SIZE (16 * 1024 * 1024)
uint8_t heap[HEAP_SIZE] __attribute__((section(".bss.heap")));
```

`kfree` poisons a freed block so a double-free is caught rather than silently
corrupting the free list (pinned by the `kfree` self-test).

---

## 4. Paging and the address-space layout

NyxOS runs on x86-64 4-level paging (PML4 → PDPT → PD → PT, 4 KiB leaves). A leaf
PTE carries the standard flags — `PAGE_PRESENT`, `PAGE_WRITABLE`, `PAGE_USER`, and
`PAGE_NX` (the no-execute bit, bit 63). Physical addresses are masked to bits
51:12, which deliberately drops the NX bit so an NX page's frame still resolves
correctly. The kernel image is linked at low physical addresses starting at
1 MiB and uses supervisor identity mappings. A higher-half alias starts at
`KERNEL_BASE = 0xFFFFFF8000000000`. User memory occupies the low canonical half,
with supervisor-only mappings protected by page permissions.

Each user address space is laid out so its regions can never collide:

| Region | Range | Notes |
|--------|-------|-------|
| User minimum | `USER_SPACE_MIN` = `0x1000` | page 0 stays unmapped (NULL-deref traps) |
| Program image + heap | from the ELF load address up | `SYS_SBRK` caps the exclusive break at `SHARED_LIBC_BASE` = 768 MiB |
| Shared libc | from `0x30000000` | reserved above the program heap |
| `mmap` region | `[MMAP_BASE, MMAP_MAX)` = **[4 GiB, 112 TiB)** | above the heap cap, below the stack |
| User stack | near `USER_SPACE_END` = **128 TiB** | grows down from the top of the user range |

```c
#define USER_SPACE_MIN 0x1000ULL
#define USER_SPACE_END 0x0000800000000000ULL   // 128 TiB
#define MMAP_BASE      0x100000000ULL           // 4 GiB
#define MMAP_MAX       0x0000700000000000ULL    // 112 TiB
```

A user page fault is serviced by `vm_handle_fault` (`paging.c`): a fault inside a
known region materialises a zeroed frame and maps it with that region's
protection; a fault outside any region is a genuine segfault and the process is
killed.

---

## 5. `mmap`, `mprotect`, and `munmap`

A process reserves virtual memory with `mmap()`; the pages are **demand-zero** —
`do_mmap` only records a **VMA** (a `{start, end, prot}` record in the thread
group's shared table) and bumps a pointer. No physical page is allocated until the
program first touches the region, at which point the fault handler allocates and
maps one zeroed frame.

- **Anonymous `MAP_PRIVATE`** is the core case: fresh demand-zero memory.
- **File-backed** mappings take a private snapshot of the file into a kernel
  buffer at `mmap` time, so a demand-faulted page copies from that snapshot
  (byte 0 of the mapping is `file[offset]`). Shared writeback (`MAP_SHARED`) is
  future work.

`do_mprotect(addr, len, prot)` changes protection on a sub-range. A **partial**
`mprotect` **splits** the overlapping VMA (a back or front carve adds one record,
a middle carve adds two) so that pages *outside* the changed range keep their
original protection even if they have not faulted in yet — the split is what the
`mprotecttest` user program checks. `do_munmap` unmaps a range, frees the present
frames (refcount-aware, so a COW-shared frame is only released at its last
reference), and drops or splits the VMA.

### W^X

Both `do_mmap` and `do_mprotect` enforce **W^X**: no mapping may be simultaneously
writable and executable. A `prot` that sets both `PROT_WRITE` and `PROT_EXEC` is
rejected (`MAP_FAILED` / `-1`) before any state changes:

```c
static inline int prot_wx_conflict(int prot) {
    return (prot & PROT_WRITE) && (prot & PROT_EXEC);
}
```

This closes the classic "write shellcode into a writable page, then jump to it"
primitive. Nothing in NyxOS needs W+X — the in-OS `cc` compiles to a file and
`exec()`s it (the ELF loader maps `.text` R+X and `.data` R+W|NX, never both)
rather than JITing. The `wx` self-test pins the rule.

---

## 6. Copy-on-write fork

`fork()` does not copy the parent's pages. Instead it clones the page tables so
both address spaces point at the *same* physical frames, marks the shared
writable pages **read-only**, and increments each frame's refcount. The first
write by either side takes a protection fault; the handler allocates a private
copy, points that process's PTE at it, and drops the shared frame's refcount. Only
pages that are actually written are ever duplicated — the rest stay shared for the
life of both processes. This is why the page refcount in §2 exists.

---

## 7. Protection and hardening

The memory system is also a security boundary (see [SECURITY.md](SECURITY.md)):

- **NX** — every non-executable mapping sets `PAGE_NX`, so data pages (heap,
  stack, `.data`) cannot be executed.
- **SMEP** — the CPU is configured to fault if ring 0 executes a user page.
- **W^X** — §5: no page is ever both writable and executable.
- **User/supervisor split** — only `PAGE_USER` pages are reachable from ring 3;
  the higher-half kernel is invisible to user code.
- **Guard / NULL trap** — page 0 is never mapped, so a NULL dereference faults.

---

## 8. Self-tests

The memory subsystem is covered by several KATs in the boot self-test battery
(run with the `selftest` multiboot command line; see [TESTING.md](TESTING.md)):

| KAT | What it pins |
|-----|--------------|
| `pagealloc` | refcounts, reserved/interior-pointer rejection, recovery and upper-frame access |
| `slab` | slab cache distinctness, integrity, reuse, and routing |
| `kfree` | double-free poisoning |
| `wx` | the W^X reject rule for `mmap`/`mprotect` |

When at least 256 free frames exist above 512 MiB, `pagealloc` holds lower frames,
writes and verifies 256 upper frames, exercises their reference counts and checks
exact recovery. It prints `[PMM-SPAN] PASS`; smaller configurations explicitly
print `SKIP`. CI runs the battery with 1536 MiB and requires the upper-frame pass.

`exec /vmtest.elf --span` requires at least 640 MiB of free managed memory. It
materialises 600 MiB, checks zero and data markers at both ends of every page,
forks a child that writes three COW probes, verifies the parent's markers and
unmaps the region. Page-table storage is reclaimed when the program exits. This
extends the existing `vmtest` address-boundary checks; it does not replace them.
The host `tools/test_page_pool.py` exhausts and recovers allocator metadata for
contiguous RAM and a reserved-hole layout under sanitizers. Its locks are mocked
and it does not dereference physical frames; real access is tested in QEMU.

---

## Summary

NyxOS layers a bitmap physical allocator (with per-frame refcounts for COW) under
a slab + free-list kernel heap, and maps it all through x86-64 4-level paging into
per-process address spaces described by demand-zero VMAs. Fork is copy-on-write,
protection is NX + SMEP + W^X, and each layer is guarded by a KAT.
