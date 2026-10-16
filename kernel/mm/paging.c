#include "../core/kernel.h"

#define IDENTITY_MAP_MB 64

// PML4 indices
#define PML4_IDENTITY 0
#define PML4_HIGHER   511       // 0xFFFFFF8000000000 — matches KERNEL_BASE

// Page-frame address bits [51:12] of a PTE/table entry. Masking with this (NOT `& ~0xFFF`, which
// keeps bit 63 = NX plus any reserved 63:52) is what turns an entry back into a clean physical
// address: `& ~0xFFF` on an NX leaf leaves bit 63 set, so the "physical address" comes out wrong.
#define PHYS_ADDR_MASK 0x000FFFFFFFFFF000ULL

static uint64_t* current_pml4 = NULL;
static uint64_t* kernel_pml4 = NULL;

// Get physical address (virtual -> physical) using kernel page tables
void* get_phys_addr(void* virtual_addr) {
    uint64_t addr = (uint64_t)virtual_addr;
    int pml4_idx = (addr >> 39) & 0x1FF;
    int pdpt_idx = (addr >> 30) & 0x1FF;
    int pd_idx   = (addr >> 21) & 0x1FF;
    int pt_idx   = (addr >> 12) & 0x1FF;
    int offset   = addr & 0xFFF;

    if (!kernel_pml4) return NULL;
    uint64_t pml4e = kernel_pml4[pml4_idx];
    if (!(pml4e & PAGE_PRESENT)) return NULL;

    uint64_t* pdpt = (uint64_t*)(pml4e & PHYS_ADDR_MASK);
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PAGE_PRESENT)) return NULL;

    // 1GB huge page?
    if (pdpte & PAGE_HUGE) {
        return (void*)((pdpte & PHYS_ADDR_MASK & ~((1ULL << 30) - 1)) + (addr & ((1ULL << 30) - 1)));
    }

    uint64_t* pd = (uint64_t*)(pdpte & PHYS_ADDR_MASK);
    uint64_t pde = pd[pd_idx];
    if (!(pde & PAGE_PRESENT)) return NULL;

    // 2MB huge page?
    if (pde & PAGE_HUGE) {
        return (void*)((pde & PHYS_ADDR_MASK & ~((1ULL << 21) - 1)) + (addr & ((1ULL << 21) - 1)));
    }

    uint64_t* pt = (uint64_t*)(pde & PHYS_ADDR_MASK);
    uint64_t pte = pt[pt_idx];
    if (!(pte & PAGE_PRESENT)) return NULL;

    return (void*)((pte & PHYS_ADDR_MASK) + offset);   // mask [51:12] drops NX/reserved so NX pages resolve correctly
}

// Extract the physical base of a present intermediate page-table entry with the FULL
// address mask (bits 51:12) — NOT `& ~0xFFF`, which leaves the NX bit (63) and any
// reserved high bits in place, so a corrupted entry becomes a non-canonical pointer
// that #GPs opaquely on the next dereference. A well-formed intermediate entry never
// has bits 63:52 set; if any are, the table is corrupt (the still-open "-1 writer")
// — panic here with the offending value/vaddr/pid instead of crashing one deref later.
#define PT_ADDR_BITS PHYS_ADDR_MASK      // alias of the canonical page-frame mask (bits [51:12])
static uint64_t* pt_next(uint64_t entry, const char* level, uint64_t vaddr, int idx) {
    if (entry & 0xFFF0000000000000ULL) {
        process_t* p = get_current_process();
        printf("\n[MAPCORRUPT] %s=0x%lx (reserved bits set) vaddr=0x%lx idx=%d pid=%u comm=%s\n",
               level, entry, vaddr, idx, p ? (unsigned)p->pid : 0, p ? p->comm : "?");
        kernel_panic("page-table corruption: %s=0x%lx (vaddr 0x%lx)", level, entry, vaddr);
    }
    return (uint64_t*)(entry & PT_ADDR_BITS);
}

// Map a page in a specific PML4
static void map_pml4(uint64_t* pml4, void* phys, void* virt, uint64_t flags) {
    uint64_t vaddr = (uint64_t)virt;
    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    int pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t pml4e = pml4[pml4_idx];
    uint64_t* pdpt;
    if (!(pml4e & PAGE_PRESENT)) {
        pdpt = (uint64_t*)alloc_page();
        if (!pdpt) return;
        memset_asm(pdpt, 0, 4096);
        pml4[pml4_idx] = (uint64_t)pdpt | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER ? PAGE_USER : 0);
    } else {
        pdpt = pt_next(pml4e, "pml4e", vaddr, pml4_idx);
    }

    uint64_t pdpte = pdpt[pdpt_idx];
    uint64_t* pd;
    if (!(pdpte & PAGE_PRESENT)) {
        pd = (uint64_t*)alloc_page();
        if (!pd) return;
        memset_asm(pd, 0, 4096);
        pdpt[pdpt_idx] = (uint64_t)pd | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER ? PAGE_USER : 0);
    } else {
        pd = pt_next(pdpte, "pdpte", vaddr, pdpt_idx);
    }

    uint64_t pde = pd[pd_idx];
    uint64_t* pt;
    if (!(pde & PAGE_PRESENT)) {
        pt = (uint64_t*)alloc_page();
        if (!pt) return;
        memset_asm(pt, 0, 4096);
        pd[pd_idx] = (uint64_t)pt | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER ? PAGE_USER : 0);
    } else {
        pt = pt_next(pde, "pde", vaddr, pd_idx);
    }

    uint64_t old = pt[pt_idx];

    pt[pt_idx] = (uint64_t)phys | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER ? PAGE_USER : 0) | (flags & PAGE_NX);

    // Two different situations, and conflating them would be expensive:
    //
    //  - REPLACING a live translation. Other cores may hold the old one cached
    //    and would go on using it, so they must be told. (This is the case my
    //    own tlbtest hit: its remap was invisible to CPU 1 until a shootdown.)
    //
    //  - Creating one where there was none. A remote core holding the old
    //    not-present state simply faults and re-walks, so a broadcast buys
    //    nothing — and this is the demand-fault path, so putting an IPI here
    //    would tax every page fault in the system.
    if (old & PAGE_PRESENT) tlb_shootdown(vaddr);
    else __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void map_page(void* phys, void* virt, uint64_t flags) {
    map_pml4(kernel_pml4, phys, virt, flags);
}

void map_page_dir(uint64_t* pml4, void* phys, void* virt, uint64_t flags) {
    map_pml4(pml4, phys, virt, flags);
}

void unmap_page(void* virt) {
    uint64_t vaddr = (uint64_t)virt;
    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    int pt_idx   = (vaddr >> 12) & 0x1FF;

    if (!kernel_pml4) return;
    uint64_t pml4e = kernel_pml4[pml4_idx];
    if (!(pml4e & PAGE_PRESENT)) return;

    uint64_t* pdpt = (uint64_t*)(pml4e & PHYS_ADDR_MASK);
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PAGE_PRESENT)) return;

    uint64_t* pd = (uint64_t*)(pdpte & PHYS_ADDR_MASK);
    uint64_t pde = pd[pd_idx];
    if (!(pde & PAGE_PRESENT)) return;

    uint64_t* pt = (uint64_t*)(pde & PHYS_ADDR_MASK);
    pt[pt_idx] = 0;
    // Removal from the KERNEL page tables, which every core shares — the case
    // v5.8.95 missed while wiring up vm_free_range/vm_protect_range/vm_unmap.
    // Its one caller (vbe.c, at mode-set) runs on the BSP holding no allocator
    // lock, so the shootdown rule is satisfied.
    tlb_shootdown(vaddr);
}

uint64_t* get_kernel_page_directory(void) {
    return kernel_pml4;
}

// Physical address of kernel PML4 for assembly page-table switching
uint64_t kernel_pml4_phys = 0;

void switch_page_directory(uint64_t* pml4) {
    if (!pml4) pml4 = kernel_pml4;
    current_pml4 = pml4;
    write_cr3((uint64_t)pml4);
}

uint64_t* alloc_page_directory(void) {
    uint64_t* pml4 = (uint64_t*)alloc_page();
    if (!pml4) return NULL;
    memset_asm(pml4, 0, PAGE_SIZE);

    // Clone kernel PML4 entries for the higher half only (PML4[256..511])
    // Identity mapping (PML4[0]) is NOT shared — user processes use higher half
    for (int i = PML4_HIGHER; i < 512; i++) {
        if (kernel_pml4[i] & PAGE_PRESENT) {
            pml4[i] = kernel_pml4[i];
        }
    }
    return pml4;
}

void* clone_page_directory(void) {
    return (void*)alloc_page_directory();
}

// Free a user page directory created by alloc_page_directory: releases every
// leaf frame and intermediate table for the user half (PML4[0..510]) and the
// PML4 page itself. PML4[511] is the shared kernel mirror and is left untouched.
// Physical addresses are masked to bits 51:12 to drop the NX bit and flags.
#define PT_PHYS_MASK PHYS_ADDR_MASK      // alias of the canonical page-frame mask (bits [51:12])
void free_page_directory(uint64_t* pml4) {
    if (!pml4) return;
    for (int i = 0; i < PML4_HIGHER; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(pml4[i] & PT_PHYS_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT) || (pdpt[j] & PAGE_HUGE)) continue;
            uint64_t* pd = (uint64_t*)(pdpt[j] & PT_PHYS_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PAGE_PRESENT) || (pd[k] & PAGE_HUGE)) continue;
                uint64_t* pt = (uint64_t*)(pd[k] & PT_PHYS_MASK);
                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PAGE_PRESENT)
                        free_page((void*)(pt[l] & PT_PHYS_MASK));
                }
                free_page(pt);
            }
            free_page(pd);
        }
        free_page(pdpt);
    }
    free_page(pml4);
}

// Identity map using 2MB huge pages for speed
// ============================================================
// Demand paging + copy-on-write, serviced from the #PF handler.
// PTEs carry two OS-available marker bits (ignored by the CPU):
//   PTE_DEMAND — page not present; allocate a zeroed page on first touch.
//   PTE_COW    — page present but read-only; copy it privately on write.
// Backing pages come from alloc_page() (low physical, so identity-accessible).
// ============================================================
#define PTE_DEMAND     (1ULL << 9)
#define PTE_COW        (1ULL << 10)
#define PTE_ADDR_MASK  PHYS_ADDR_MASK          // alias of the canonical page-frame mask (bits [51:12])

static uint64_t vm_demand_faults = 0;
static uint64_t vm_cow_faults = 0;

// Walk `pml4` to the leaf PTE for `virt`, creating intermediate tables when
// `create`. Returns a pointer to the PT entry, or NULL. Page tables are reached
// through their identity mapping (they live in the low, identity-mapped 64 MB).
static uint64_t* pte_ptr(uint64_t* pml4, uint64_t virt, int create) {
    int idx[4] = { (int)((virt >> 39) & 0x1FF), (int)((virt >> 30) & 0x1FF),
                   (int)((virt >> 21) & 0x1FF), (int)((virt >> 12) & 0x1FF) };
    uint64_t* tbl = pml4;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t e = tbl[idx[lvl]];
        if (e & PAGE_HUGE) return NULL;         // don't touch huge mappings
        if (!(e & PAGE_PRESENT)) {
            if (!create) return NULL;
            uint64_t* nt = (uint64_t*)alloc_page();
            if (!nt) return NULL;
            memset_asm(nt, 0, PAGE_SIZE);
            // Intermediate tables must be USER-accessible: a ring-3 translation
            // requires PAGE_USER at EVERY level, and the leaf PTE gates the actual
            // permission. Without USER here, a forked child's user pages (whose
            // tables are built through this walker) sit behind supervisor-only
            // intermediate entries, so ring 3 can't even fetch its own code
            // (#PF present+user+instr-fetch). Kernel leaves stay supervisor via
            // their own missing USER bit. Mirrors map_pml4's user mapping path.
            tbl[idx[lvl]] = (uint64_t)nt | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
            tbl = nt;
        } else {
            tbl = (uint64_t*)(e & ~0xFFFULL);
        }
    }
    return &tbl[idx[3]];
}

// Map a READ-ONLY user page (present, user, not writable; NX unless exec). Used
// for the shared libc: the same physical frame is mapped into many processes, so
// it must NOT be writable (map_page_dir would force PAGE_WRITABLE). The caller
// page_incref()s the frame so process teardown's refcount-aware free() balances.
void map_page_ro(uint64_t* pml4, void* phys, void* virt, int exec) {
    uint64_t* pte = pte_ptr(pml4, (uint64_t)virt, 1);
    if (!pte) return;
    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (!exec) f |= PAGE_NX;
    *pte = ((uint64_t)phys & PTE_ADDR_MASK) | f;
}

// Identity-map `bytes` of device MMIO at physical `phys` into the kernel address
// space (virt == phys), UNCACHEABLE (PCD) + writable + NX. Device BARs — an NVMe
// or AHCI controller's registers — sit far above boot.asm's 128 MB identity map,
// so a driver must map its register window before dereferencing it; and MMIO must
// be uncached, or the CPU would cache device-register reads/writes and the device
// would never see them. Rounds to whole pages. Kernel-only (leaf lacks PAGE_USER).
#define PAGE_PCD (1ULL << 4)
void map_mmio_range(uint64_t phys, uint64_t bytes) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end   = (phys + bytes + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        uint64_t* pte = pte_ptr(kernel_pml4, p, 1);
        if (!pte) return;
        *pte = (p & PTE_ADDR_MASK) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX | PAGE_PCD;
        __asm__ volatile("invlpg (%0)" :: "r"(p) : "memory");
    }
}

// Walk a process's user address space, coalescing runs of present pages with the
// same permissions into regions (for /proc/<pid>/maps). A COW page counts as
// writable (it becomes writable on the next write). Returns the region count.
int vm_collect_regions(uint64_t* pml4, vm_region_t* out, int max) {
    if (!pml4 || !out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < PML4_HIGHER; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(pml4[i] & PT_PHYS_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT) || (pdpt[j] & PAGE_HUGE)) continue;
            uint64_t* pd = (uint64_t*)(pdpt[j] & PT_PHYS_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PAGE_PRESENT) || (pd[k] & PAGE_HUGE)) continue;
                uint64_t* pt = (uint64_t*)(pd[k] & PT_PHYS_MASK);
                for (int l = 0; l < 512; l++) {
                    uint64_t e = pt[l];
                    if (!(e & PAGE_PRESENT)) continue;
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30)
                                | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    int w = ((e & PAGE_WRITABLE) || (e & PTE_COW)) ? 1 : 0;
                    int x = (e & PAGE_NX) ? 0 : 1;
                    if (n > 0 && out[n-1].end == va && out[n-1].writable == w && out[n-1].exec == x) {
                        out[n-1].end = va + 4096;                    // extend the run
                    } else {
                        if (n >= max) return n;
                        out[n].start = va; out[n].end = va + 4096;
                        out[n].writable = w; out[n].exec = x;
                        n++;
                    }
                }
            }
        }
    }
    return n;
}

// Called from the #PF handler. Returns 1 if the fault was a demand/COW page it
// resolved (retry the instruction), 0 for a genuine fault (let it panic).
int vm_handle_fault(uint64_t cr2, uint64_t err) {
    // Pick the faulting address space. A user-mode fault (err bit2 == 1) belongs
    // to the running process — the scheduler tracks that by current_idx, not the
    // global current_pml4 (which only follows explicit switch_page_directory
    // calls, so it lags behind ring-3 processes the scheduler dispatched via
    // next_cr3). Kernel-mode faults (the cowtest self-test) use current_pml4.
    // This is what makes COW work for a forked child running in its own CR3.
    process_t* p = get_current_process();
    uint64_t* pml4 = ((err & 0x4) && p && p->page_directory)
                     ? (uint64_t*)p->page_directory : current_pml4;
    if (!pml4) return 0;

    // Lazy sbrk: a USER not-present fault (err bit2 set, bit0 clear) whose address
    // lies in the process's heap window [heap_start, program_break) — SYS_SBRK just
    // moved the break without allocating, so materialise a fresh zeroed page now.
    // This runs BEFORE the pte_ptr walk below because a brand-new heap page has no
    // page-table entry (nor intermediate tables) yet; map_page_dir creates them.
    // The heap window belongs to the THREAD GROUP: a page sbrk'd by one thread must
    // fault in for any sibling that touches it, so test the leader's window, not ours.
    // The window is read from ANOTHER process_t (the thread-group leader), reached
    // via find_process(), which walks the process table with no lock. So it can be
    // observed mid-update or belong to a slot being recycled. Sanity-check it
    // before trusting it: a heap that starts below user space, or an inverted
    // window, is never legitimate — and a window starting at 0 swallows the
    // program's own .text, which is exactly how a zeroed NX page ended up mapped
    // over live code. Rejecting it here costs one comparison and can only ever
    // STOP this path firing on a nonsense window; it never enables it anywhere it
    // did not already fire.
    //
    // HONEST SCOPE: hardening, NOT the fix for the v5.9.0 intermittent [fault].
    // With this guard in place the fault still reproduced (NX violation on the
    // process's own .text, CR2 == RIP, err 0x15), so the zeroed-NX page that
    // lands over user code does NOT come through this path. Still unexplained;
    // see the ordering note in elf.c and P0.1 in the release blockers.
    process_t* mm = tg_leader(p);
    if ((err & 0x4) && !(err & 0x1) && p && p->page_directory && mm &&
        mm->heap_start >= USER_SPACE_MIN && mm->program_break > mm->heap_start &&
        cr2 >= mm->heap_start && cr2 < mm->program_break) {
        void* page = alloc_page();
        if (!page) return 0;
        memset_asm((void*)(uint64_t)page, 0, PAGE_SIZE);
        map_page_dir(pml4, page, (void*)(cr2 & ~0xFFFULL), 0x7 | PAGE_NX);  // P|W|U, NX
        invlpg((void*)cr2);
        vm_demand_faults++;
        return 1;
    }

    // Anonymous mmap: a USER not-present fault inside one of the process's mmap
    // regions gets a fresh zeroed page with the region's prot. Like the heap block,
    // this precedes the pte_ptr walk below (a new mmap page has no PTE yet). We set
    // the PTE directly rather than via map_page_dir (which forces WRITABLE) so a
    // PROT_READ / non-exec mapping is honored: writable only if PROT_WRITE, NX
    // unless PROT_EXEC. pte_ptr(create=1) builds the intermediate tables (USER).
    if ((err & 0x4) && !(err & 0x1) && p && p->page_directory) {
        vma_t* v = vma_find(p, cr2);
        if (v) {
            uint64_t* mpte = pte_ptr(pml4, cr2, 1);
            if (!mpte) return 0;
            void* page = alloc_page();
            if (!page) return 0;
            memset_asm((void*)(uint64_t)page, 0, PAGE_SIZE);
            if (v->file_buf) {                     // file-backed: copy this page's slice
                uint64_t off = (cr2 & ~0xFFFULL) - v->start;
                if (off < v->file_size) {
                    uint32_t n = PAGE_SIZE;
                    if (off + n > v->file_size) n = v->file_size - (uint32_t)off;
                    memcpy((void*)(uint64_t)page, v->file_buf + off, n);
                }
            }
            uint64_t f = PAGE_PRESENT | PAGE_USER;
            if (v->prot & PROT_WRITE) f |= PAGE_WRITABLE;
            if (!(v->prot & PROT_EXEC)) f |= PAGE_NX;
            *mpte = ((uint64_t)page & PTE_ADDR_MASK) | f;
            invlpg((void*)cr2);
            vm_demand_faults++;
            return 1;
        }
    }

    // Stack auto-grow: a USER not-present fault (err bit2 set, bit0 clear) inside the
    // stack window [USER_STACK_LOW, USER_STACK_TOP + PAGE_SIZE), below the committed
    // pages — materialise a fresh zeroed RW+NX page so the stack grows on demand up to
    // the USER_STACK_LOW floor. Precedes the pte_ptr walk: a grown page has no PTE yet.
    if ((err & 0x4) && !(err & 0x1) && p && p->page_directory &&
        cr2 >= USER_STACK_LOW && cr2 < USER_STACK_TOP + PAGE_SIZE) {
        void* page = alloc_page();
        if (!page) return 0;
        memset_asm((void*)(uint64_t)page, 0, PAGE_SIZE);
        map_page_dir(pml4, page, (void*)(cr2 & ~0xFFFULL), 0x7 | PAGE_NX);   // P|W|U|NX
        invlpg((void*)cr2);
        vm_demand_faults++;
        return 1;
    }
    // Stack overflow: a USER fault in the guard page just below the stack floor. Report
    // it clearly, then return 0 so the #PF handler delivers SIGSEGV (we never map it —
    // growing here would silently run the stack into whatever lies below).
    if ((err & 0x4) && p && cr2 >= USER_STACK_LOW - PAGE_SIZE && cr2 < USER_STACK_LOW) {
        printf("[stack overflow] pid=%u comm=%s hit the stack guard page (cr2=0x%lx)\n",
               (unsigned)p->pid, p->comm, cr2);   // comm is a char[32] — always non-NULL
        return 0;
    }

    uint64_t* pte = pte_ptr(pml4, cr2, 0);
    if (!pte) return 0;
    uint64_t e = *pte;

    // Demand paging: a not-present fault (err bit0 == 0) on a DEMAND-marked page.
    if (!(err & 0x1) && (e & PTE_DEMAND)) {
        void* page = alloc_page();
        if (!page) return 0;
        memset_asm((void*)(uint64_t)page, 0, PAGE_SIZE);
        *pte = ((uint64_t)page & PTE_ADDR_MASK) | PAGE_PRESENT | PAGE_WRITABLE
             | (e & PAGE_USER) | (e & PAGE_NX);
        invlpg((void*)cr2);
        vm_demand_faults++;
        return 1;
    }

    // Copy-on-write: a write (err bit1) protection fault (err bit0 == 1) on a
    // present, COW-marked page — give the writer a private, writable copy. If we
    // are the sole remaining owner (refcount 1, the other side already copied or
    // exited) there is nothing to protect: just clear COW and re-enable writes in
    // place, saving an allocation and a page copy.
    if ((err & 0x1) && (err & 0x2) && (e & PAGE_PRESENT) && (e & PTE_COW)) {
        uint64_t old = e & PTE_ADDR_MASK;
        if (page_get_refcount((void*)old) <= 1) {
            *pte = old | PAGE_PRESENT | PAGE_WRITABLE | (e & PAGE_USER) | (e & PAGE_NX);
        } else {
            void* newp = alloc_page();
            if (!newp) return 0;
            memcpy((void*)(uint64_t)newp, (void*)old, PAGE_SIZE);
            free_page((void*)old);          // drop our reference to the shared page
            *pte = ((uint64_t)newp & PTE_ADDR_MASK) | PAGE_PRESENT | PAGE_WRITABLE
                 | (e & PAGE_USER) | (e & PAGE_NX);
        }
        // COW REPLACES an existing translation: the old one pointed at a shared,
        // read-only frame. Any other core in this SAME address space (i.e. a
        // sibling thread) holding it cached would keep READING the old frame
        // after we privatised the page — so this must be broadcast, not just
        // invalidated locally. Reached with interrupts off, which tlb_shootdown
        // handles by enabling them for its duration and restoring IF after.
        tlb_shootdown(cr2);
        vm_cow_faults++;
        return 1;
    }
    return 0;
}

// munmap helper: unmap and free every present page in [start, end) of `pml4`
// (both page-aligned). free_page is refcount-aware, so a page still shared COW with
// a forked relative just drops a reference here. pte_ptr(create=0) never allocates,
// so ranges with no backing (never-faulted mmap pages) are skipped cheaply.
void vm_free_range(uint64_t* pml4, uint64_t start, uint64_t end) {
    if (!pml4) return;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* pte = pte_ptr(pml4, va, 0);
        if (pte && (*pte & PAGE_PRESENT)) {
            free_page((void*)(*pte & PTE_ADDR_MASK));
            *pte = 0;
            // A mapping going AWAY is the case that needs every core told: one
            // still holding this translation would keep reading a frame that has
            // already gone back to the allocator. Safe to call here — free_page
            // released page_lock before returning, and the shootdown rule is
            // never to hold it (or kmalloc_lock) across this.
            tlb_shootdown(va);
        }
    }
}

// mprotect helper: rewrite the flag bits of every PRESENT page in [start, end),
// keeping each page's physical frame. Writable iff PROT_WRITE; NX unless PROT_EXEC.
// Not-present pages are skipped — the VMA's updated prot covers them when they fault.
void vm_protect_range(uint64_t* pml4, uint64_t start, uint64_t end, int prot) {
    if (!pml4) return;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* pte = pte_ptr(pml4, va, 0);
        if (pte && (*pte & PAGE_PRESENT)) {
            uint64_t e = *pte;
            uint64_t f = PAGE_PRESENT | PAGE_USER;
            if (prot & PROT_WRITE) f |= PAGE_WRITABLE;
            if (!(prot & PROT_EXEC)) f |= PAGE_NX;

            // A COW page must STAY copy-on-write. Rebuilding the entry from the
            // frame address alone dropped PTE_COW and, for PROT_WRITE, granted
            // PAGE_WRITABLE outright — so after fork(), an mprotect(PROT_WRITE)
            // on shared memory let this process write the frame the OTHER one is
            // still reading. No fault, no copy: silent cross-process corruption,
            // the exact isolation fork() exists to provide.
            //
            // Keeping the marker and withholding the write bit means the first
            // write still faults into the COW handler, which privatises the page
            // and only then applies the writability this call asked for.
            if (e & PTE_COW) {
                f &= ~PAGE_WRITABLE;
                f |= PTE_COW;
            }
            // Demand-mapped entries keep their marker for the same reason.
            if (e & PTE_DEMAND) f |= PTE_DEMAND;

            *pte = (e & PTE_ADDR_MASK) | f;
            // mprotect can only ever RESTRICT what another core already has
            // cached (a writable entry becoming read-only), so it must be
            // broadcast for the new protection to actually bind.
            tlb_shootdown(va);
        }
    }
}

// fork(): build a new address space that shares the caller's user pages
// copy-on-write. alloc_page_directory() already shares the higher-half kernel
// mapping (PML4[511]); here we walk the parent's user half (PML4[0..510]) and,
// for every present leaf page, bump its refcount and install the same mapping in
// the child. Writable pages are downgraded to read-only + PTE_COW in BOTH the
// parent and the child, so the first writer on either side takes a private copy
// via vm_handle_fault; read-only pages (code/rodata) are shared as-is. The
// parent's live TLB is refreshed by the CR3 reload on syscall return. Returns the
// child PML4 (physical), or NULL on allocation failure.
uint64_t* clone_page_directory_cow(uint64_t* parent) {
    if (!parent) return NULL;
    uint64_t* child = alloc_page_directory();
    if (!child) return NULL;

    for (int i = 0; i < PML4_HIGHER; i++) {
        if (!(parent[i] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(parent[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT) || (pdpt[j] & PAGE_HUGE)) continue;
            uint64_t* pd = (uint64_t*)(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PAGE_PRESENT) || (pd[k] & PAGE_HUGE)) continue;
                uint64_t* pt = (uint64_t*)(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    uint64_t e = pt[l];
                    if (!(e & PAGE_PRESENT)) continue;
                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30)
                                  | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    uint64_t* cpte = pte_ptr(child, virt, 1);
                    if (!cpte) { free_page_directory(child); return NULL; }
                    if (e & PAGE_WRITABLE) {
                        uint64_t cow = (e & ~PAGE_WRITABLE) | PTE_COW;
                        pt[l] = cow;        // parent: RO + COW
                        *cpte = cow;        // child:  RO + COW (same frame)
                    } else {
                        *cpte = e;          // read-only page: shared as-is
                    }
                    page_incref((void*)(e & PTE_ADDR_MASK));
                }
            }
        }
    }
    return child;
}

// --- setup helpers (used by the `cowtest` self-test) ---
int vm_map_demand(uint64_t virt) {
    uint64_t* pte = pte_ptr(current_pml4, virt, 1);
    if (!pte) return -1;
    *pte = PTE_DEMAND;                 // not present, allocate on first touch
    invlpg((void*)virt);
    return 0;
}
int vm_map_cow(uint64_t virt, uint64_t phys) {
    uint64_t* pte = pte_ptr(current_pml4, virt, 1);
    if (!pte) return -1;
    *pte = (phys & PTE_ADDR_MASK) | PAGE_PRESENT | PTE_COW;  // present, read-only
    // Mapping a page COW adds a reference to it: the writer's copy-out drops this
    // one (refcount>1 -> private copy), leaving the frame valid for its other
    // holder. Without this a COW page with a single alloc reference would be seen
    // as sole-owner and written in place, corrupting the shared original.
    page_incref((void*)(phys & PTE_ADDR_MASK));
    invlpg((void*)virt);
    return 0;
}
void vm_unmap(uint64_t virt) {
    uint64_t* pte = pte_ptr(current_pml4, virt, 0);
    if (pte) { *pte = 0; tlb_shootdown(virt); }   // removal: tell every core
}
uint64_t vm_stat_demand(void) { return vm_demand_faults; }
uint64_t vm_stat_cow(void) { return vm_cow_faults; }

// Number of 2 MiB entries in the boot identity map. Firmware RAM is a sum
// of regions, whereas the allocator's indexes span their holes as well.
static int identity_map_entries(uint64_t detected_bytes, uint32_t managed_pages) {
    uint64_t bytes = (uint64_t)managed_pages * PAGE_SIZE;
    if (detected_bytes > bytes) bytes = detected_bytes;
    if (bytes < ((uint64_t)IDENTITY_MAP_MB << 20)) bytes = (uint64_t)IDENTITY_MAP_MB << 20;
    if (bytes > (1ULL << 30)) bytes = 1ULL << 30;       // one page directory
    return (int)((bytes + 0x1FFFFF) >> 21);
}

void init_paging(void) {
    printf("[PAGING] Allocating PML4 table...\n");
    kernel_pml4 = (uint64_t*)alloc_page();
    if (!kernel_pml4) kernel_panic("No PML4 page");
    memset_asm(kernel_pml4, 0, PAGE_SIZE);

    // Allocate one PDP table (covers up to 512 GB)
    uint64_t* pdpt = (uint64_t*)alloc_page();
    if (!pdpt) kernel_panic("No PDPT page");
    memset_asm(pdpt, 0, PAGE_SIZE);

    // Allocate one Page Directory with 2MB huge-page entries
    uint64_t* pd = (uint64_t*)alloc_page();
    if (!pd) kernel_panic("No PD page");
    memset_asm(pd, 0, PAGE_SIZE);

    // Identity-map ALL of physical RAM (not a fixed 64 MB): the kernel allocates
    // page-table pages and copy-on-write target pages with alloc_page() and then
    // touches them through their IDENTITY (low) virtual address, so every page
    // alloc_page() can hand out MUST be identity-mapped. When only 64 MB was mapped,
    // heavy memory use (a large program + fork COW + a pipeline) eventually allocated
    // a page above 64 MB and the kernel faulted dereferencing its unmapped identity
    // address — the intermittent `cmd | less`-class crash. One PD spans 512*2MB = 1 GB;
    // cap there (more RAM would need extra PDs). Cover at least the whole managed
    // span even when reserved holes make the sum of detected RAM smaller.
    int num_pages = identity_map_entries(memory_total, get_total_pages());
    printf("[PAGING] Identity-mapping %d MB (RAM %llu MB)...\n", num_pages * 2, memory_total >> 20);
    for (int i = 0; i < num_pages; i++) {
        pd[i] = ((uint64_t)i * 0x200000) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
    }

    pdpt[0] = (uint64_t)pd | PAGE_PRESENT | PAGE_WRITABLE;
    kernel_pml4[PML4_IDENTITY] = (uint64_t)pdpt | PAGE_PRESENT | PAGE_WRITABLE;

    // Mirror identity mapping in higher half (PML4[256] = PML4[0])
    kernel_pml4[PML4_HIGHER] = kernel_pml4[PML4_IDENTITY];

    // Expose physical address for assembly page-table switching
    kernel_pml4_phys = (uint64_t)kernel_pml4;

    printf("[PAGING] Loading CR3 with 0x%lx\n", (uint64_t)kernel_pml4);
    write_cr3((uint64_t)kernel_pml4);

    // Enable NXE (No-Execute) in EFER
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | EFER_NXE);
    printf("[PAGING] NXE enabled.\n");

    // Enable CR0.WP: without it, ring-0 code may write to read-only pages without
    // faulting — copy-on-write needs those supervisor writes to trap.
    write_cr0(read_cr0() | (1ULL << 16));
    printf("[PAGING] CR0.WP enabled.\n");

    // SMEP disabled for -cpu qemu64 compatibility
    // uint64_t cr4_save = read_cr4();
    // write_cr4(cr4_save | CR4_SMEP);
    // printf("[PAGING] SMEP enabled.\n");

    printf("[PAGING] Enabled successfully.\n");
    current_pml4 = kernel_pml4;
}
