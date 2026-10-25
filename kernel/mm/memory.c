#include "../core/kernel.h"
#include "slab.h"
#include "../core/spinlock.h"

// The physical allocator's state — page_bitmap, page_refcount, free_pages,
// memory_used — is now reachable from any CPU, so it needs a real lock rather
// than preempt_disable(). Held for a bitmap scan at most, always with interrupts
// off (a #PF handler allocates, and re-entering on the same core would deadlock).
// init_memory/reserve_low_pages run before any AP exists and stay unlocked.
static spinlock_t page_lock = SPINLOCK_INIT;

// One lock for kmalloc/kfree, covering BOTH the slab caches and the heap free
// list. They are two allocators but one dependency graph — slab_new_page() calls
// alloc_page(), and kmalloc falls through from slab to heap — so a single lock
// keeps the ordering trivially deadlock-free (kmalloc_lock is always taken
// before page_lock, never the reverse).
static spinlock_t kmalloc_lock = SPINLOCK_INIT;

// Bitmap-based physical page allocator
// Each bit represents one 4KB page (1 = free, 0 = used)
// Covers the runtime identity map: up to 1 GiB, with a 32 KiB bitmap.

#define MAX_PAGES ((uint32_t)(PHYS_IDENTITY_MAX_BYTES / PAGE_SIZE))
#define BITMAP_WORDS (MAX_PAGES / 32)
static uint32_t page_bitmap[BITMAP_WORDS];
static uint32_t total_pages = 0;
static uint32_t free_pages = 0;

// Per-page reference count, indexed by physical page number. Copy-on-write
// (fork) shares one physical page between several address spaces; the refcount
// is how many PTEs point at it. alloc_page() sets it to 1; page_incref() bumps
// it when a page is shared (COW clone); free_page() decrements and only returns
// the frame to the bitmap when the last reference drops. A refcount of 0 for a
// page that was never tracked (reserved at init, or pre-refcount allocations)
// means reserved or already free; free_page rejects it without changing the pool.
// uint16 (not uint8): a shared frame's reference count can reach the number of address
// spaces mapping it, and MAX_PROCESSES is 512 — past a uint8's 255 ceiling. A saturating
// uint8 under-counted a widely-shared COW frame and freed it early (use-after-free); 16
// bits holds every reachable count with room to spare (the incref still saturates safely
// at 0xFFFF, now unreachable). Costs 2*MAX_PAGES bytes of BSS (512 KiB at 1 GiB).
static uint16_t page_refcount[MAX_PAGES];

// Frames that must NEVER return to the allocator, whatever the refcount says: the
// shared-libc master pages (loaded once at boot, mapped read-only into every
// process). A per-process libc mapping that wasn't matched by an incref would
// otherwise drive the master reference to 0 in free_page and release the frame;
// alloc_page() then hands the still-mapped page back out (double-allocation) — the
// root of the pipeline memory corruption. Pinning the masters breaks that at the
// source. page_pin() is called from shared_libc.c as each master frame is loaded.
static uint8_t page_pinned[MAX_PAGES];
void page_pin(void* addr) {
    uint32_t page_idx = (uint32_t)(uintptr_t)addr / PAGE_SIZE;
    if (page_idx < MAX_PAGES) page_pinned[page_idx] = 1;
}

// Reserve pages [0, end_page): mark each used (clear its bitmap bit) and drop free_pages
// for every one that was free. Carves the kernel image + low 1MB out of the freed RAM.
static void reserve_low_pages(uint32_t end_page) {
    for (uint32_t i = 0; i < end_page && i < total_pages; i++) {
        if (page_bitmap[i / 32] & (1u << (i % 32))) {   // currently free -> reserve it
            page_bitmap[i / 32] &= ~(1u << (i % 32));
            free_pages--;
        }
    }
}

// Initialize the physical page allocator. When the bootloader provides a firmware
// memory map (multiboot2 type-6 tag, parsed in kernel_main), we mark EVERY page used
// and then free ONLY the ranges the firmware reports as available RAM (type 1) — so
// every hole (the low VGA/BIOS window 0xA0000-0xFFFFF, the block SeaBIOS relocates
// itself into near the top of RAM, ACPI tables, PCI MMIO) is reserved automatically,
// regardless of RAM size or where the holes sit. alloc_page can then never hand out a
// non-RAM frame — the root fix for the "pipeline corruption" Heisenbug, generalized:
// v5.8.83 reserved a hardcoded [0,_kernel_end), which covered the LOW firmware hole
// but not reserved regions ABOVE the kernel (safe only because pstorm never allocated
// that high). With no memory map we fall back to that v5.8.83 behavior.
void init_memory(uint64_t mem_size, const mb_mmap_entry_t* mmap, int mmap_count) {
    memory_total = mem_size;
    memory_used = 0;

    extern uint8_t _kernel_end[];
    uint32_t kernel_end_page = (uint32_t)(((uintptr_t)_kernel_end + PAGE_SIZE - 1) / PAGE_SIZE);

    if (mmap && mmap_count > 0) {
        memset_asm(page_bitmap, 0x00, sizeof(page_bitmap));   // all pages USED
        free_pages = 0;

        // The basic multiboot memory count only describes low RAM on machines
        // with a PCI hole. Sum available RAM from the full map, including regions
        // above 4 GiB, for detection. Keep it distinct from the capped pool: RAM
        // outside this allocator's address range is NOT allocated/used memory.
        uint64_t top = 0, detected = 0;
        const uint64_t pool_limit = (uint64_t)MAX_PAGES * PAGE_SIZE;
        for (int r = 0; r < mmap_count; r++) {
            if (mmap[r].type != 1 || mmap[r].len > ~(uint64_t)0 - mmap[r].base) continue;
            uint64_t end = mmap[r].base + mmap[r].len;
            if (end > top) top = end;
            if (mmap[r].len <= ~(uint64_t)0 - detected) detected += mmap[r].len;
        }
        if (detected) memory_total = detected;
        // Cap before narrowing: a high firmware region must not wrap a 32-bit
        // frame index and accidentally free unrelated low physical pages.
        total_pages = (uint32_t)((top < pool_limit ? top : pool_limit) / PAGE_SIZE);

        // Free every whole page inside an AVAILABLE (type 1) region.
        for (int r = 0; r < mmap_count; r++) {
            if (mmap[r].type != 1 || mmap[r].base >= pool_limit ||
                mmap[r].len > ~(uint64_t)0 - mmap[r].base) continue;
            uint64_t start = (mmap[r].base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);  // round up
            uint64_t end = mmap[r].base + mmap[r].len;
            if (end > pool_limit) end = pool_limit;
            end &= ~(uint64_t)(PAGE_SIZE - 1);           // round down
            for (uint64_t a = start; a < end; a += PAGE_SIZE) {
                uint32_t p = (uint32_t)(a / PAGE_SIZE);
                if (p >= total_pages) break;
                if (!(page_bitmap[p / 32] & (1u << (p % 32)))) {  // used -> free
                    page_bitmap[p / 32] |= (1u << (p % 32));
                    free_pages++;
                }
            }
        }
        // The firmware map lists RAM the kernel itself occupies as available (GRUB loaded
        // us into it), so carve it back out: page 0 (NULL guard), the whole low 1MB
        // (real-mode IVT/BDA + the SMP trampoline at 0x8000, copied in later by smp_init),
        // and the kernel image/BSS through _kernel_end. Reserve [0, max(1MB, _kernel_end)).
        uint32_t low_1mb = 0x100000 / PAGE_SIZE;
        reserve_low_pages(kernel_end_page > low_1mb ? kernel_end_page : low_1mb);

        printf("[MEM] mmap %d regions: %u pages free (%u MiB usable), detected %llu MiB, managed %u MiB\n",
               mmap_count, free_pages, free_pages / 256, memory_total / (1024 * 1024), total_pages / 256);
        return;
    }

    // Fallback: no firmware memory map. Reserve [0, _kernel_end) — including the whole
    // sub-1MB firmware hole (VGA MMIO + BIOS ROM) — and treat the rest up to mem_size as
    // usable. This is the v5.8.83 behavior; it can't see reserved regions above the
    // kernel, so it is only the safety net for a boot without a type-6 tag.
    memset_asm(page_bitmap, 0xFF, sizeof(page_bitmap));
    uint64_t pages = mem_size / PAGE_SIZE;
    total_pages = pages > MAX_PAGES ? MAX_PAGES : (uint32_t)pages;
    free_pages = total_pages;
    reserve_low_pages(kernel_end_page);
    printf("[MEM] no mmap: %d pages free (%d KB)\n", free_pages, free_pages * 4);
}

void* alloc_page(void) {
    uint64_t fl = spin_lock_irqsave(&page_lock);
    for (uint32_t i = 0; i < BITMAP_WORDS && i * 32 < total_pages; i++) {
        if (page_bitmap[i]) {
            uint32_t bit = __builtin_ctz(page_bitmap[i]);
            uint32_t page_idx = i * 32 + bit;
            if (page_idx >= total_pages) break;
            page_bitmap[i] &= ~(1u << bit);
            free_pages--;
            memory_used += PAGE_SIZE;
            page_refcount[page_idx] = 1;       // one owner until COW-shared
            spin_unlock_irqrestore(&page_lock, fl);
            return (void*)(uintptr_t)(page_idx * PAGE_SIZE);
        }
    }
    spin_unlock_irqrestore(&page_lock, fl);
    return NULL;
}

// Add a reference to an already-allocated physical page (used by the COW clone
// in fork: the child maps the parent's page instead of copying it).
void page_incref(void* addr) {
    uintptr_t a = (uintptr_t)addr;
    if (a & (PAGE_SIZE - 1)) return;                 // must be a page-aligned frame
    uint32_t page_idx = (uint32_t)(a / PAGE_SIZE);
    if (page_idx >= total_pages) return;
    uint64_t fl = spin_lock_irqsave(&page_lock);
    // Only an already-allocated frame can gain a reference. Incref'ing a free frame
    // (refcount 0) would leave it "referenced" yet still marked free in the bitmap,
    // so alloc_page could hand the same frame out a second time (issue #53). Saturate
    // at 0xFFFF rather than wrapping (unreachable at MAX_PROCESSES=512, but safe).
    if (page_refcount[page_idx] >= 1 && page_refcount[page_idx] < 0xFFFF)
        page_refcount[page_idx]++;
    spin_unlock_irqrestore(&page_lock, fl);
}

// Free-frame count. The strongest whole-machine invariant the SMP stress test
// has: every core's allocations are paired with a free, so this number must be
// identical before and after the run. A broken lock shows up as drift here even
// when no individual operation looked wrong.
uint32_t get_free_pages(void) { return free_pages; }

// Total frames under the allocator's control (RAM top rounded to pages, capped at MAX_PAGES).
// With get_free_pages() this gives the true "used = total - free" for the managed pool — which,
// unlike memory_used (dynamic alloc_page bytes only), also counts the reserved kernel and low
// memory, so it is the honest figure for a system memory gauge.
uint32_t get_total_pages(void) { return total_pages; }

// One honest source for every "memory usage" report (mem, top, nyxfetch, Nyx Monitor,
// /proc/meminfo, the desktop meminfo), so they all agree. Used = the managed pool minus what is
// free — this counts the reserved kernel + low memory, unlike the global `memory_used` (which
// tracks only dynamic alloc_page bytes and reads ~0, making every report say "0 MB used"). Any
// pointer may be NULL. Values are KiB (4 KiB per frame); callers /1024 for MiB.
void mem_pool_kb(uint32_t* used_kb, uint32_t* free_kb, uint32_t* total_kb) {
    uint32_t t = total_pages, f = free_pages;
    uint32_t u = t > f ? t - f : 0;
    if (used_kb)  *used_kb  = u * 4;
    if (free_kb)  *free_kb  = f * 4;
    if (total_kb) *total_kb = t * 4;
}

uint32_t page_get_refcount(void* addr) {
    uint32_t page_idx = (uint32_t)(uintptr_t)addr / PAGE_SIZE;
    if (page_idx >= total_pages) return 0;
    return page_refcount[page_idx];
}

void free_page(void* addr) {
    uintptr_t a = (uintptr_t)addr;
    // Reject a non-page-aligned address: alloc_page only ever hands out page-aligned
    // frames, so an interior pointer here is a caller bug. Silently flooring it (the
    // old behaviour) could release a DIFFERENT frame than intended (issue #54).
    if (a & (PAGE_SIZE - 1)) return;
    uint32_t page_idx = (uint32_t)(a / PAGE_SIZE);
    if (page_idx >= total_pages) return;
    uint64_t fl = spin_lock_irqsave(&page_lock);
    // Only an ALLOCATED frame (refcount >= 1) may be freed. A refcount of 0 means the
    // frame is already free (a double-free) or was never allocated (a reserved frame);
    // proceeding would push a bad frame into the pool and inflate free_pages /
    // memory_used, corrupting the allocator's accounting (issue #50).
    if (page_refcount[page_idx] == 0) { spin_unlock_irqrestore(&page_lock, fl); return; }
    // Shared page (COW): drop one reference, keep the frame for the others. The
    // read-decide-write on the refcount is exactly why this needs the lock: two
    // cores dropping the last two references could otherwise both see >1.
    if (page_refcount[page_idx] > 1) {
        page_refcount[page_idx]--;
    } else if (page_pinned[page_idx]) {
        // Pinned (shared-libc master) frames live for the whole OS lifetime: never
        // free one, and floor the refcount at 1 so a stray over-decrement can't
        // underflow it.
        page_refcount[page_idx] = 1;
    } else {
        page_refcount[page_idx] = 0;
        page_bitmap[page_idx / 32] |= 1u << (page_idx % 32);
        free_pages++;
        memory_used -= PAGE_SIZE;
    }
    spin_unlock_irqrestore(&page_lock, fl);
}

// The normal self-test uses little RAM. When upper frames exist, hold the lower
// ones long enough to prove real identity access and refcounts above 512 MiB.
static int page_span_selftest(void) {
    const uintptr_t boundary = 512ULL * 1024 * 1024;
    const uint32_t goal = 256;
    uint32_t available = 0;
    for (uint32_t i = (uint32_t)(boundary / PAGE_SIZE); i < total_pages; i++)
        if (page_bitmap[i / 32] & (1u << (i % 32))) available++;
    if (available < goal) {
        printf("[PMM-SPAN] SKIP: fewer than %u upper frames available\n", goal);
        return 0;
    }
    uint32_t before = get_free_pages(), count = 0, high = 0;
    uint64_t used_before = memory_used;
    volatile uint64_t* head = NULL;
    int rc = 0;
    while (high < goal) {
        volatile uint64_t* p = (volatile uint64_t*)alloc_page();
        if (!p) { rc = 21; break; }
        uintptr_t address = (uintptr_t)p;
        p[0] = (uintptr_t)head; p[1] = ~p[0];
        uint64_t pattern = address ^ 0x4e59585041474553ULL;
        p[2] = pattern ^ 2; p[PAGE_SIZE / 8 - 1] = pattern ^ (PAGE_SIZE / 8 - 1);
        if (address >= boundary) {
            for (uint32_t j = 2; j < PAGE_SIZE / 8; j++) p[j] = pattern ^ j;
            page_incref((void*)address);
            if (page_get_refcount((void*)address) != 2) rc = 22;
            free_page((void*)address);
            if (page_get_refcount((void*)address) != 1) rc = 22;
            high++;
        }
        head = p; count++;
    }
    uint32_t released = 0;
    while (head) {
        uintptr_t address = (uintptr_t)head, previous = (uintptr_t)head[0];
        if (head[1] != ~(uint64_t)previous || (previous && (previous >= address || previous % PAGE_SIZE))) {
            rc = 23; break;  // a broken chain must not become an arbitrary dereference
        }
        uint64_t pattern = address ^ 0x4e59585041474553ULL;
        if (head[2] != (pattern ^ 2) || head[PAGE_SIZE / 8 - 1] != (pattern ^ (PAGE_SIZE / 8 - 1))) rc = 24;
        if (address >= boundary)
            for (uint32_t j = 2; j < PAGE_SIZE / 8; j++)
                if (head[j] != (pattern ^ j)) rc = 24;
        free_page((void*)address); released++;
        head = (volatile uint64_t*)previous;
    }
    if (high != goal || released != count || get_free_pages() != before || memory_used != used_before) rc = 25;
    printf("[PMM-SPAN] %s: %u upper frames, %u held/released, free %u -> %u\n",
           rc ? "FAIL" : "PASS", high, count, before, get_free_pages());
    return rc;
}

// KAT for the physical page allocator's refcount / copy-on-write invariants — the
// foundation fork's COW pages and the shared-libc mapping rest on, and where issues
// #50 (double-free), #53 (incref a free frame), and #54 (free an interior pointer)
// were fixed. Exercises a real alloc/incref/free cycle, checks the free-page count
// returns EXACTLY to where it started (net-zero, the strongest whole-allocator
// invariant), and probes the three guards on reserved page 0 so no concurrent
// allocation can race them. Runs at the quiescent selftest point, like kfree_selftest.
// Returns 0 on pass, else the failing case number.
int page_alloc_selftest(void) {
    uint32_t free0 = get_free_pages();
    // --- COW refcount cycle on a real frame (held across the refcount checks) ---
    void* p = alloc_page();
    if (!p) return 1;
    if (page_get_refcount(p) != 1) return 2;                       // fresh frame: one owner
    if (get_free_pages() != free0 - 1) return 3;                   // one fewer free page
    page_incref(p);                                               // COW share -> second reference
    if (page_get_refcount(p) != 2) return 4;
    if (get_free_pages() != free0 - 1) return 5;                   // no extra frame consumed
    free_page((void*)((uintptr_t)p + 1));                          // interior pointer (#54): must be a no-op
    if (page_get_refcount(p) != 2 || get_free_pages() != free0 - 1) return 6;
    free_page(p);                                                 // drop one reference
    if (page_get_refcount(p) != 1) return 7;                       // frame kept for the other owner
    if (get_free_pages() != free0 - 1) return 8;
    free_page(p);                                                 // drop the last reference
    if (page_get_refcount(p) != 0) return 9;                       // frame returned to the pool
    if (get_free_pages() != free0) return 10;                      // accounting restored
    // --- guards on reserved page 0 (low 1 MB, refcount 0, never pooled: race-free) ---
    void* rsv = (void*)0;
    if (page_get_refcount(rsv) != 0) return 11;                    // precondition: reserved -> untracked
    free_page(rsv);                                               // free a free/reserved frame (#50): no-op
    if (page_get_refcount(rsv) != 0 || get_free_pages() != free0) return 12;
    page_incref(rsv);                                            // incref a free frame (#53): must NOT resurrect it
    if (page_get_refcount(rsv) != 0) return 13;
    // --- net-zero stress: many alloc/free pairs leave the count where it started ---
    void* v[32];
    for (int i = 0; i < 32; i++) { v[i] = alloc_page(); if (!v[i]) return 14; }
    for (int i = 0; i < 32; i++) free_page(v[i]);
    if (get_free_pages() != free0) return 15;
    // --- refcount above the old uint8 ceiling (255) --------------------------------
    // MAX_PROCESSES is 512, so a read-only COW frame (e.g. a program's .rodata that
    // forks into hundreds of children without a write) can be referenced by more than
    // 255 address spaces. A uint8 refcount saturated there and then freed the frame
    // 255 decrefs later — while the remaining owners still had it mapped (use-after-
    // free). Count exactly to 300 and back to prove the type now holds it.
    void* q = alloc_page();
    if (!q) return 16;                                    // fresh: refcount 1
    for (int i = 0; i < 299; i++) page_incref(q);         // 300 references in total
    if (page_get_refcount(q) != 300) return 17;           // a uint8 refcount capped at 255 here
    for (int i = 0; i < 299; i++) free_page(q);           // drop back toward the single owner
    if (page_get_refcount(q) != 1) return 18;             // still referenced: NOT freed early
    if (get_free_pages() != free0 - 1) return 19;         // exactly the one frame still out
    free_page(q);                                         // last reference -> back to the pool
    if (get_free_pages() != free0) return 20;             // net-zero restored
    return page_span_selftest();
}

// Small allocation header for slab/heap routing
typedef struct alloc_hdr {
    uint32_t magic;
    uint32_t size;
} alloc_hdr_t;

// Two magics so kfree knows the true origin. The slab can't serve every size
// <= SLAB_MAX_OBJ (its cache classes may not cover a size, and the header pushes
// some requests over), so kmalloc falls back to the heap — kfree must route to
// the matching allocator regardless of size.
#define ALLOC_MAGIC_SLAB 0x4E79584F // "NyXO"
#define ALLOC_MAGIC_HEAP 0x4E795848 // "NyXH"
// Stamped over a block's magic the instant it is freed (see kfree). A double-free — or
// any stale pointer whose block was already released — then fails the live-magic check
// and is dropped, instead of being re-routed to heap_free() and re-coalescing an
// already-free block (which corrupts the free list). "NyXF".
#define ALLOC_MAGIC_FREED 0x4E795846

void slab_init_all(void) {
    slab_init();
}

// kmalloc: use slab for small objects (<=512 bytes), heap for larger
void* kmalloc(size_t size) {
    // The slab/heap freelists are not reentrant. preempt_disable() used to hold
    // them together by stopping a context switch mid-update — which was enough
    // while one CPU ran everything, and is worth nothing against another core
    // that never consults it. The spinlock replaces it, and taking it with
    // interrupts off subsumes what preempt_disable was doing locally.
    uint64_t fl = spin_lock_irqsave(&kmalloc_lock);
    void* result = NULL;
    // Try the slab for small objects. slab_alloc returns NULL when no cache
    // class covers (size + header); in that case fall through to the heap
    // rather than failing the allocation (the old code returned NULL here,
    // so every 505..1016-byte kmalloc — e.g. VFS file writes — failed).
    if (size + sizeof(alloc_hdr_t) <= SLAB_MAX_OBJ) {
        void* ptr = slab_alloc((uint32_t)size + sizeof(alloc_hdr_t));
        if (ptr) {
            alloc_hdr_t* hdr = (alloc_hdr_t*)ptr;
            hdr->magic = ALLOC_MAGIC_SLAB;
            hdr->size = (uint32_t)size;
            result = (void*)(hdr + 1);
        }
    }
    if (!result) {
        extern void* heap_alloc(size_t);
        void* ptr = heap_alloc(size + sizeof(alloc_hdr_t));
        if (ptr) {
            alloc_hdr_t* hdr = (alloc_hdr_t*)ptr;
            hdr->magic = ALLOC_MAGIC_HEAP;
            hdr->size = (uint32_t)size;
            result = (void*)(hdr + 1);
        }
    }
    spin_unlock_irqrestore(&kmalloc_lock, fl);
    return result;
}

void* kmalloc_aligned(size_t size, uint32_t align) {
    (void)align;
    return kmalloc(size);
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint64_t fl = spin_lock_irqsave(&kmalloc_lock);
    alloc_hdr_t* hdr = ((alloc_hdr_t*)ptr) - 1;
    extern void heap_free(void*);
    // Read the origin magic, then immediately poison the header. heap_free() only flips
    // its block's used-bit — it never touches this alloc_hdr_t — so without poisoning, a
    // heap-backed block kept its ALLOC_MAGIC_HEAP after being freed: a second kfree of the
    // same pointer re-entered heap_free() and coalesced an already-free block, corrupting
    // the free list. (slab_free() happens to overwrite the header with its free-list link,
    // so the slab path was already covered; the heap path was not.) Stamping
    // ALLOC_MAGIC_FREED makes a double-free — or any stale pointer — fail the check below
    // and be dropped. hdr->size stays intact for the slab_free() argument evaluated first.
    uint32_t magic = hdr->magic;
    hdr->magic = ALLOC_MAGIC_FREED;
    if (magic == ALLOC_MAGIC_SLAB) {
        slab_free(hdr, hdr->size + sizeof(alloc_hdr_t));
    } else if (magic == ALLOC_MAGIC_HEAP) {
        heap_free(hdr);
    }
    // Neither live magic: a double-free (header now reads ALLOC_MAGIC_FREED), a pointer
    // that never came from kmalloc, or a corrupted header. Every pointer kfree can
    // legitimately see was stamped by kmalloc (slab_alloc/heap_alloc are only ever reached
    // through it), so an unknown header is invalid. Guessing a backend and calling
    // slab_free/heap_free would let it rewrite an unrelated block's metadata (issue #52) —
    // drop it instead of corrupting the heap.
    spin_unlock_irqrestore(&kmalloc_lock, fl);
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return NULL; }
    void* newp = kmalloc(size);
    // Allocation failed: keep the original block valid and return NULL, matching
    // standard realloc semantics so a transient OOM doesn't cost the caller its data.
    // The old code freed ptr unconditionally, destroying the only copy (issue #51).
    if (!newp) return NULL;
    alloc_hdr_t* hdr = ((alloc_hdr_t*)ptr) - 1;
    size_t old_size = hdr->size;
    memcpy_asm(newp, ptr, old_size < size ? old_size : size);
    kfree(ptr);
    return newp;
}

// KAT (`kfree` in the self-test battery): kfree() must poison a block's header the
// instant it is freed, so a double-free is caught rather than re-routed. Exercises the
// HEAP-backed path (a request whose size+header exceeds every slab class) — the one that
// previously kept its ALLOC_MAGIC_HEAP after free and so re-coalesced the free list on a
// second kfree. Returns 0 on pass, else the failing step.
int kfree_selftest(void) {
    size_t big = SLAB_MAX_OBJ;                     // + header exceeds the slab -> heap path
    void* p = kmalloc(big);
    if (!p) return 1;
    alloc_hdr_t* h = ((alloc_hdr_t*)p) - 1;
    if (h->magic != ALLOC_MAGIC_HEAP) return 2;    // confirm it really took the heap path
    kfree(p);
    if (h->magic != ALLOC_MAGIC_FREED) return 3;   // THE FIX: header poisoned on free
    kfree(p);                                      // double-free: must be dropped, not re-freed
    // The dropped double-free must not have corrupted the free list: two fresh heap
    // allocations still succeed and occupy distinct, non-overlapping regions.
    void* a = kmalloc(big);
    void* b = kmalloc(big);
    int rc = 0;
    if (!a || !b) rc = 4;
    else {
        uint8_t* pa = (uint8_t*)a; uint8_t* pb = (uint8_t*)b;
        if (!(pa + big <= pb || pb + big <= pa)) rc = 5;   // overlap => corrupted free list
    }
    if (a) kfree(a);
    if (b) kfree(b);
    return rc;
}

// KAT (`slab` in the self-test battery): exercises the slab allocator (kmalloc's backend for
// small objects) through kmalloc/kfree, so the allocator lock keeps it SMP/IRQ-safe. Verifies
// a page's worth of same-size objects are distinct + non-overlapping (no slot aliasing / free-
// list corruption), each block keeps its own bytes (no cross-slot bleed), freed slots are
// reclaimed (a second round of the same allocations still succeeds), and a request past the
// largest slab class (1024) still succeeds via the heap fallback. 0 on pass, else the step.
int slab_selftest(void) {
    enum { SN = 24 };
    void* v[SN];
    // 1. distinctness + no-overlap: kmalloc(48) -> the 64-byte slab class (~63 slots/page)
    for (int i = 0; i < SN; i++) { v[i] = kmalloc(48); if (!v[i]) return 1; }
    for (int i = 0; i < SN; i++)
        for (int j = i + 1; j < SN; j++) {
            uint64_t x = (uint64_t)v[i], y = (uint64_t)v[j], d = (x > y) ? x - y : y - x;
            if (d < 48) { for (int k = 0; k < SN; k++) kfree(v[k]); return 2; }   // aliased slots
        }
    // 2. write-integrity: a distinct pattern per block, verified with no cross-contamination
    for (int i = 0; i < SN; i++) { uint8_t* s = (uint8_t*)v[i]; for (int k = 0; k < 48; k++) s[k] = (uint8_t)(i * 5 + k); }
    for (int i = 0; i < SN; i++) { uint8_t* s = (uint8_t*)v[i];
        for (int k = 0; k < 48; k++) if (s[k] != (uint8_t)(i * 5 + k)) { for (int m = 0; m < SN; m++) kfree(v[m]); return 3; } }
    for (int i = 0; i < SN; i++) kfree(v[i]);
    // 3. reuse: the same count re-allocates after freeing (freed slots reclaimed, not leaked)
    for (int i = 0; i < SN; i++) { v[i] = kmalloc(48); if (!v[i]) { for (int m = 0; m < i; m++) kfree(v[m]); return 4; } }
    for (int i = 0; i < SN; i++) kfree(v[i]);
    // 4. size routing: a request past the largest slab class -> heap fallback still serves it
    void* big = kmalloc(SLAB_MAX_OBJ + 64); if (!big) return 5;
    { uint8_t* s = (uint8_t*)big; s[0] = 0x5a; s[SLAB_MAX_OBJ + 63] = 0xa5;
      if (s[0] != 0x5a || s[SLAB_MAX_OBJ + 63] != 0xa5) { kfree(big); return 6; } }
    kfree(big);
    return 0;
}

// Usable size of a kmalloc'd block: the request size stored in its header. Lets a
// grow-in-place buffer (vfs_pwrite) know how many bytes its current allocation holds
// without tracking a separate capacity field. NULL -> 0. Only valid for pointers
// returned by kmalloc (which always writes the header).
uint32_t ksize(void* ptr) {
    if (!ptr) return 0;
    return (((alloc_hdr_t*)ptr) - 1)->size;
}
