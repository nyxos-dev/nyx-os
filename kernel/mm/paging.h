// ============================================================
// paging.h - NyxOS virtual-memory / paging public interface
// ============================================================
// The public contract of the paging subsystem (mm/paging.c + the #PF handler):
// page-table entry flags, the mapping primitives, the address-space (PML4)
// lifecycle, and the demand-paging / copy-on-write entry points. Split out of the
// god-header core/kernel.h as the next step of the incremental modular-header
// direction that began with types.h (v6.4.128) — a translation unit that only
// needs paging can eventually include this instead of all 1200+ lines of
// kernel.h. Self-contained: it depends only on the base integer types, so it
// pulls in types.h (NOT the whole kernel.h) and never forms a circular include.
// kernel.h includes it, so every existing includer is unaffected for now.
#ifndef NYX_PAGING_H
#define NYX_PAGING_H

#include "../core/types.h"

// One runtime identity-map page directory; every managed frame must fit here.
#define PHYS_IDENTITY_MAX_BYTES (1ULL << 30)

// Page-table entry flags.
#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63)

// Mapping primitives + kernel/user address-space (PML4) lifecycle.
void init_paging(void);
void* get_phys_addr(void* virtual_addr);
void map_page(void* phys_addr, void* virt_addr, uint64_t flags);
void map_mmio_range(uint64_t phys, uint64_t bytes);  // paging.c: identity-map device MMIO, uncached (PCD)+NX
void map_page_ro(uint64_t* pml4, void* phys, void* virt, int exec);  // paging.c: RO user page
void unmap_page(void* virt_addr);
void* clone_page_directory(void);
uint64_t* clone_page_directory_cow(uint64_t* parent_pml4);  // fork: COW-share the user half
uint64_t* alloc_page_directory(void);
uint64_t* get_kernel_page_directory(void);
void switch_page_directory(uint64_t* pd);
void map_page_dir(uint64_t* pd, void* phys, void* virt, uint64_t flags);
void free_page_directory(uint64_t* pml4);  // free a user address space (COW-refcount aware)
void vm_protect_range(uint64_t* pml4, uint64_t start, uint64_t end, int prot);  // paging.c

// Demand paging + copy-on-write (serviced from the #PF handler).
int vm_handle_fault(uint64_t cr2, uint64_t err);
int vm_map_demand(uint64_t virt);             // mark virt as allocate-on-first-touch
int vm_map_cow(uint64_t virt, uint64_t phys); // map virt -> phys read-only, copy on write
void vm_unmap(uint64_t virt);
uint64_t vm_stat_demand(void);
uint64_t vm_stat_cow(void);

#endif // NYX_PAGING_H
