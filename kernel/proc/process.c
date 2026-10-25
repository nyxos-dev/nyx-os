#include "../core/kernel.h"
#include "../core/spinlock.h"

extern process_t* process_table[MAX_PROCESSES];
extern int process_count;
static uint64_t next_pid = 1;

// Total context switches since boot — bumped once per scheduler dispatch at the single
// switch chokepoint sched_target(), on the BSP and every AP. Exposed as /proc/stat's
// `ctxt`. Incremented with an atomic add because sched_target runs concurrently on
// several cores under smpbalance; a plain ++ would drop counts there.
volatile uint64_t g_ctx_switches = 0;
uint64_t total_procs_created(void) { return next_pid - 1; }   // /proc/stat `processes`

int current_idx = 0;

// Guards the SHAPE of process_table — the entries array and process_count —
// against a core scanning it while another appends or swap-removes. It does NOT
// guard the contents of a process_t: each task is owned by the one CPU pinned to
// it (p->sched_cpu), so nothing else writes its stack or state.
spinlock_t sched_lock = SPINLOCK_INIT;

void init_process(void) {
    memset_asm(process_table, 0, sizeof(process_table));
    process_t* init = (process_t*)kmalloc(sizeof(process_t));
    if (init) {
        memset_asm(init, 0, sizeof(process_t));
        init->pid = next_pid++;
        init->ppid = 0;
        init->state = 1;
        strncpy(init->comm, "init", 31);
        // page_directory left NULL -- scheduler uses kernel_pml4_phys (physical addr)
        process_table[process_count++] = init;
    }
}

// Set up a kernel stack for a new process.
// Stack layout (from low to high addr) matches switch_context restore:
//   r15, r14, r13, r12, r11, r10, r9, r8,
//   rbp, rdi, rsi, rdx, rcx, rbx, rax, rip_old
// Then after SAVE_REGS (old iret frame):
//   r15, r14, r13, r12, r11, r10, r9, r8,
//   rbp, rdi, rsi, rdx, rcx, rbx, rax,
//   int_no(32), error(0), rip, cs, rflags, rsp, ss
// ---- kernel task-stack canary (stack-smash tripwire) -----------------------------------------
// Each task's kernel stack is a kmalloc(4096) block that grows DOWN from stack_mem+4096. The
// lowest 8 bytes (stack_mem[0..7]) are the overflow-danger end: real usage peaks ~2.9 KB from the
// top (scratchpad/stackscan.sh), so they're never touched unless a frame nearly overruns the 4 KB
// stack. We stamp a magic there at allocation; `stackcheck` (stack_canary_sweep) later scans every
// task to detect a smash - detect-and-report defence-in-depth (no paging / no scheduler-path
// change) complementing the v6.4.233-234 off-stack-buffer work.
#define STACK_CANARY 0x4E7853746B437921ULL   /* "NxStkCy!" */

static void stack_canary_place(void* stack_mem) {
    if (stack_mem) *(volatile uint64_t*)stack_mem = STACK_CANARY;
}
static int stack_canary_intact(void* stack_mem) {
    return stack_mem && *(volatile uint64_t*)stack_mem == STACK_CANARY;
}
// kmalloc a 4 KB task stack with a canary already stamped at its low (overflow) end.
static void* alloc_task_stack(void) {
    void* m = kmalloc(4096);
    stack_canary_place(m);
    return m;
}
// Scan every live task's kernel-stack canary. Returns the count of SMASHED stacks (0 = all intact),
// logging each. kernel_stack == stack_mem+4096, so the canary lives at kernel_stack-4096.
int stack_canary_sweep(void) {
    int smashed = 0;
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (!p || !p->kernel_stack) continue;
        if (!stack_canary_intact((void*)((uintptr_t)p->kernel_stack - 4096))) {
            smashed++;
            printf("[STACKGUARD] pid %d: kernel stack canary SMASHED\n", p->pid);
        }
    }
    return smashed;
}
// KAT: stamp a canary, confirm intact, corrupt a byte, confirm the check fails, restore.
int stack_canary_selftest(void) {
    uint8_t buf[16];
    stack_canary_place(buf);
    if (!stack_canary_intact(buf)) return 1;
    buf[0] ^= 0xFF;
    if (stack_canary_intact(buf)) return 2;
    stack_canary_place(buf);
    if (!stack_canary_intact(buf)) return 3;
    return 0;
}

static int init_task_stack(process_t* proc, void* entry_point) {
    void* stack_mem = alloc_task_stack();
    if (!stack_mem) return -1;
    uint64_t* sp = (uint64_t*)((uintptr_t)stack_mem + 4096);

    // iretq frame for kernel process (ring 0). In long mode iretq ALWAYS pops
    // SS:RSP, even without a privilege change, so SS must be the kernel *data*
    // selector (0x10) — a writable data segment. It used to say 0x08 (the kernel
    // *code* selector); loading a code segment into SS #GP's (error=SS selector).
    // That bug stayed latent until preemptive scheduling actually iretq'd into one
    // of these frames.
    *--sp = KERNEL_DS;         // ss = kernel data (0x10)
    *--sp = (uint64_t)(uintptr_t)stack_mem + 4096; // rsp
    *--sp = 0x202;             // rflags (IF set)
    *--sp = KERNEL_CS;         // cs = kernel code (0x08)
    *--sp = (uint64_t)(uintptr_t)entry_point; // rip

    *--sp = 0;                 // error code
    *--sp = 32;                // int number (irq0)

    // SAVE_REGS: 15 zeros
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;

    proc->stack = (void*)((uintptr_t)sp);
    proc->kernel_stack = (void*)((uintptr_t)stack_mem + 4096);
    return 0;
}

static int init_user_task_stack(process_t* proc, void* entry_point, void* user_stack_top) {
    void* stack_mem = alloc_task_stack();
    if (!stack_mem) return -1;
    uint64_t* sp = (uint64_t*)((uintptr_t)stack_mem + 4096);

    // iretq frame for user process (ring 3)
    // When iretq detects CS.DPL != current CPL, it pops SS:RSP too (5 values)
    *--sp = USER_DS;            // ss = user data (ring 3)
    *--sp = (uint64_t)(uintptr_t)user_stack_top; // rsp = user stack
    *--sp = 0x202;              // rflags (IF set)
    *--sp = USER_CS;            // cs = user code (ring 3, L-bit=1)
    *--sp = (uint64_t)(uintptr_t)entry_point; // rip

    *--sp = 0;                  // error code
    *--sp = 32;                 // int number (irq0)

    // SAVE_REGS: 15 zeros
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;

    // Store the saved kernel-stack RSP as its HIGHER-HALF alias. When we iretq into
    // this process we first switch CR3 to its user page directory, where the only
    // mapping of kernel memory is the PML4[511] higher-half mirror — the low
    // identity address would #PF on the first stack access (RESTORE_REGS). This
    // keeps user procs consistent with the value the scheduler later saves (the
    // TSS RSP0 is also a higher-half alias). Kernel threads stay low (init_task_stack).
    proc->stack = (void*)((uintptr_t)sp + KERNEL_BASE);
    proc->kernel_stack = (void*)((uintptr_t)stack_mem + 4096);
    return 0;
}

process_t* create_process(const char* name, void* entry, uint64_t flags) {
    (void)flags;
    if (process_count >= MAX_PROCESSES) return NULL;
    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    if (!p) return NULL;
    memset_asm(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    p->ppid = 0;
    p->state = 1;
    strncpy(p->comm, name, 31);
    p->comm[31] = '\0';
    if (entry) {
        if (init_task_stack(p, entry) < 0) {
            kfree(p);
            return NULL;
        }
    }
    uint64_t fl = spin_lock_irqsave(&sched_lock);
    process_table[process_count++] = p;
    spin_unlock_irqrestore(&sched_lock, fl);
    return p;
}

// Create a kernel thread that only the given CPU will ever run. Pinning is the
// whole mutual-exclusion story for AP scheduling: no other core looks at a task
// whose sched_cpu isn't its own, so two cores can never pick the same one.
process_t* create_kernel_thread_on_cpu(const char* name, void* entry, int cpu) {
    process_t* p = create_process(name, entry, 0);
    if (p) p->sched_cpu = (int32_t)cpu;
    return p;
}

process_t* create_user_process(const char* name, void* entry, void* user_stack, uint64_t* page_dir) {
    if (process_count >= MAX_PROCESSES) return NULL;
    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    if (!p) return NULL;
    memset_asm(p, 0, sizeof(process_t));
    console_capture_inherit(p);
    p->pid = next_pid++;
    p->state = 1;
    p->page_directory = page_dir;
    strncpy(p->comm, name, 31);
    p->comm[31] = '\0';

    // Inherit the spawning context's cwd (the kernel shell's global current_dir,
    // which the compositor sets per-terminal before running a command) so a tool
    // launched after `cd /home/nyx` starts there, not at "/". fork() copies the
    // parent's cwd in do_fork(); this covers the spawn/exec-from-shell path.
    // Spread user processes across cores when balancing is on. Off by default:
    // the default placement (CPU 0) is exactly the behaviour every earlier
    // version had, so enabling SMP costs nothing until it is asked for. Each
    // process owns its page tables, so moving one to another core needs no TLB
    // coordination — unlike a thread group, which do_clone keeps together.
    if (smp_user_balance && cpu_count > 1) {
        static uint32_t rr = 0;
        p->sched_cpu = (int32_t)(rr++ % cpu_count);
    }

    const char* cwd0 = vfs_getcwd();
    if (cwd0 && cwd0[0]) {
        strncpy(p->cwd, cwd0, sizeof(p->cwd) - 1);
        p->cwd[sizeof(p->cwd) - 1] = '\0';
    }

    if (!user_stack) {
        void* stack_page = alloc_page();
        if (!stack_page) { kfree(p); return NULL; }
        uint64_t stack_virt = USER_STACK_TOP;            // top page (holds the frame)
        map_page_dir(page_dir, stack_page, (void*)stack_virt, 0x7 | PAGE_NX);
        for (int sp = 1; sp < USER_STACK_INIT_PAGES; sp++) {  // rest demand-grow (vm_handle_fault)
            void* pg = alloc_page();
            if (!pg) { kfree(p); return NULL; }
            map_page_dir(page_dir, pg, (void*)(stack_virt - (uint64_t)sp * 4096), 0x7 | PAGE_NX);
        }
        // Empty SysV entry frame (crt0 reads argc/argv from [rsp] on every launch).
        uint64_t* stk = (uint64_t*)((uint8_t*)stack_page + 4096 - 32);
        stk[0] = 0; stk[1] = 0; stk[2] = 0; stk[3] = 0;   // argc=0, NULLs, pad
        user_stack = (void*)(stack_virt + 4096 - 32);
    }

    if (init_user_task_stack(p, entry, user_stack) < 0) {
        kfree(p);
        return NULL;
    }
    process_table[process_count++] = p;
    return p;
}

// Build the child's kernel stack for fork(): a scheduler-resumable frame that
// iretq's straight back to ring 3 at the parent's post-syscall RIP, carrying the
// parent's register state — except rax = 0, so fork() returns 0 in the child.
// `frame` is the parent's saved syscall register frame (isr_stubs' syscall_frame_ptr):
// 15 GPRs r15..rax at frame[0..14], then RFLAGS at frame[15], RIP at frame[16].
// The layout mirrors init_user_task_stack (SAVE_REGS block below an iretq frame),
// so the scheduler resumes it through the same irq_common RESTORE_REGS/iretq path.
// (parameter is `u_rsp`, not `user_rsp`: that name is now the per-CPU accessor.)
static int init_forked_task_stack(process_t* proc, uint64_t* frame, uint64_t u_rsp) {
    void* stack_mem = alloc_task_stack();
    if (!stack_mem) return -1;
    uint64_t* sp = (uint64_t*)((uintptr_t)stack_mem + 4096);

    // iretq frame back to ring 3 (identical shape to init_user_task_stack).
    *--sp = USER_DS;                 // ss
    *--sp = u_rsp;                   // rsp = parent's user stack (COW-shared)
    *--sp = frame[15] | 0x202;       // rflags (parent's, IF + reserved bit forced on)
    *--sp = USER_CS;                 // cs
    *--sp = frame[16];               // rip = the instruction after the parent's syscall

    *--sp = 0;                       // error code
    *--sp = 32;                      // int number (irq0)

    // SAVE_REGS block. RESTORE_REGS pops r15 first (lowest addr) and rax last
    // (highest), so push rax first, r15 last. The child's fork() return value is 0.
    *--sp = 0;                       // rax = 0  (fork returns 0 in the child)
    *--sp = frame[13];               // rbx
    *--sp = frame[12];               // rcx
    *--sp = frame[11];               // rdx
    *--sp = frame[10];               // rsi
    *--sp = frame[9];                // rdi
    *--sp = frame[8];                // rbp
    *--sp = frame[7];                // r8
    *--sp = frame[6];                // r9
    *--sp = frame[5];                // r10
    *--sp = frame[4];                // r11
    *--sp = frame[3];                // r12
    *--sp = frame[2];                // r13
    *--sp = frame[1];                // r14
    *--sp = frame[0];                // r15

    // Saved kernel RSP as a higher-half alias (see init_user_task_stack's note).
    proc->stack = (void*)((uintptr_t)sp + KERNEL_BASE);
    proc->kernel_stack = (void*)((uintptr_t)stack_mem + 4096);
    return 0;
}

// SYS_FORK. Clone the calling ring-3 process: a COW copy of its address space and
// a child that resumes at the same point with fork()==0, while the parent gets the
// child's pid back. Runs inside the syscall (interrupts masked, kernel CR3), so
// current_idx and the parent's page tables are stable. Returns the child pid, or
// -1 on failure (out of memory / process slots / called from a kernel thread).
int do_fork(void) {
    extern void free_page_directory(uint64_t* pml4);

    process_t* parent = get_current_process();
    if (!parent || !parent->page_directory) return -1;   // only a ring-3 proc can fork
    if (process_count >= MAX_PROCESSES) return -1;
    uint64_t* frame = (uint64_t*)syscall_frame_ptr;
    if (!frame) return -1;

    uint64_t* child_pml4 = clone_page_directory_cow((uint64_t*)parent->page_directory);
    if (!child_pml4) return -1;

    process_t* child = (process_t*)kmalloc(sizeof(process_t));
    if (!child) { free_page_directory(child_pml4); return -1; }
    memset_asm(child, 0, sizeof(process_t));
    child->pid = next_pid++;
    child->ppid = parent->pid;
    console_capture_inherit(child);
    child->state = PROC_RUN;
    child->page_directory = child_pml4;
    child->heap_start    = parent->heap_start;       // ...same lazy-sbrk window
    child->program_break = parent->program_break;   // heap inherited (COW-shared)
    strncpy(child->comm, parent->comm, 31);
    child->comm[31] = '\0';
    // Inherit signal dispositions + trampoline (POSIX fork); the child starts with
    // no pending or in-flight signals (sig_pending/sig_active/sig_saved memset to 0).
    for (int i = 0; i < NSIG; i++) child->sig_handlers[i] = parent->sig_handlers[i];
    child->sig_mask = parent->sig_mask;
    child->sig_trampoline = parent->sig_trampoline;
    // Inherit mmap regions: clone_page_directory_cow shares the already-faulted
    // mmap pages COW, and copying the VMAs lets the child demand-fault any pages
    // the parent hadn't touched yet.
    for (int i = 0; i < PROC_MAX_VMAS; i++) child->mmap_vmas[i] = parent->mmap_vmas[i];
    child->mmap_next = parent->mmap_next;
    // A file-backed VMA owns its snapshot buffer — give the child its own copy so
    // munmap/exit in either process doesn't double-free (or corrupt) the other's.
    for (int i = 0; i < PROC_MAX_VMAS; i++) {
        vma_t* cv = &child->mmap_vmas[i];
        if (cv->used && cv->file_buf) {
            uint8_t* nb = (uint8_t*)kmalloc(cv->file_size);
            if (nb) { memcpy_asm(nb, cv->file_buf, cv->file_size); cv->file_buf = nb; }
            else    { cv->file_buf = 0; cv->file_size = 0; }   // degrade to zero-fill
        }
    }
    // Inherit the working directory (relative paths in the child resolve against it).
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
    // Inherit the parent's PIPE fds (with a refcount bump) so the classic
    // pipe();fork() pattern works. VFS fds are deliberately NOT inherited — their
    // handles aren't reference-counted, so sharing would risk a double close; the
    // child gets those empty (stdio still works: SYS_WRITE fd 1/2 hits the console).
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (parent->ufd_inuse[i] && (parent->ufd_handle[i] & UFD_PIPE_FLAG)) {
            child->ufd_inuse[i]  = 1;
            child->ufd_handle[i] = parent->ufd_handle[i];
            child->ufd_offset[i] = 0;
            pipe_incref(UFD_PIPE_ID(parent->ufd_handle[i]),
                        UFD_PIPE_IS_WRITE(parent->ufd_handle[i]));
        }
    }

    if (init_forked_task_stack(child, frame, user_rsp) < 0) {
        // The child already holds resources copied/refcounted above: its own kmalloc'd
        // file-backed VMA snapshots (file_buf) and a pipe_incref on every inherited pipe
        // end. free_page_directory + kfree(child) alone would leak both. Undo them the
        // same way reap and do_clone's failure path do, before freeing the process_t.
        close_proc_fds(child);      // drop the inherited pipe-end refcounts
        mmap_free_bufs(child);      // free the per-child file_buf copies
        free_page_directory(child_pml4);
        kfree(child);
        return -1;
    }

    // Both are now real scheduled processes. The parent may have been the unmanaged
    // blocking-exec / auto-exec target; once it forks it must round-robin as well.
    parent->sched_managed = 1;
    child->sched_managed = 1;
    child->sched_weight = parent->sched_weight;
    process_table[process_count++] = child;
    sched_enable();
    return (int)child->pid;
}

// Build a THREAD's kernel stack: like init_user_task_stack, but the ring-3 context
// starts at `entry` on the caller-supplied `user_stack_top` with RDI = arg, so the
// thread enters as fn(arg) under the SysV ABI. The 15-GPR SAVE_REGS block is pushed
// high->low (rax,rbx,rcx,rdx,rsi,rdi,rbp,r8..r15), so RDI is the 6th push.
static int init_thread_task_stack(process_t* proc, void* entry, void* user_stack_top,
                                  uint64_t arg) {
    void* stack_mem = alloc_task_stack();
    if (!stack_mem) return -1;
    uint64_t* sp = (uint64_t*)((uintptr_t)stack_mem + 4096);

    *--sp = USER_DS;                                   // ss
    *--sp = (uint64_t)(uintptr_t)user_stack_top;       // rsp = the thread's own stack
    *--sp = 0x202;                                     // rflags (IF set)
    *--sp = USER_CS;                                   // cs
    *--sp = (uint64_t)(uintptr_t)entry;                // rip = fn
    *--sp = 0;                                         // error code
    *--sp = 32;                                        // int number (irq0)

    *--sp = 0;      // rax
    *--sp = 0;      // rbx
    *--sp = 0;      // rcx
    *--sp = 0;      // rdx
    *--sp = 0;      // rsi
    *--sp = arg;    // rdi <- fn's first argument
    *--sp = 0;      // rbp
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;        // r8..r11
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;        // r12..r15

    // Higher-half alias, same reasoning as init_user_task_stack (we resume on the
    // thread's own CR3, where only the PML4[511] kernel mirror maps kernel memory).
    proc->stack = (void*)((uintptr_t)sp + KERNEL_BASE);
    proc->kernel_stack = (void*)((uintptr_t)stack_mem + 4096);
    return 0;
}

// With CLONE_VM, several process_t entries share ONE address space (the same PML4).
// Only the LAST task out may release it, so every free_page_directory site asks this
// first: does any OTHER live entry still point at this PML4?
int addr_space_shared(void* pml4, process_t* except) {
    if (!pml4) return 0;
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (p && p != except && p->page_directory == pml4) return 1;
    }
    return 0;
}

// The task owning this thread group's SHARED heap/VMA state. Threads carry the leader's
// pid in `tgid` (a pid, not a pointer, so an exited leader can never dangle); resolving
// by lookup falls back to self if the leader is already gone.
process_t* tg_leader(process_t* p) {
    if (!p || !p->tgid || p->tgid == p->pid) return p;
    process_t* l = find_process(p->tgid);
    return l ? l : p;
}

// Hand a dying task's SHARED thread-group state (heap + mmap, including file_buf
// OWNERSHIP so the dying leader won't free it out from under them, AND the shared fd
// table) to a surviving member, re-pointing the rest at that heir — the group stays
// coherent even when the main thread exits first. Returns 1 if it handed off to a live
// heir (the caller must then NOT close the fds — they belong to the group now), or 0 if
// there was no live survivor (this was the LAST member out, so the caller closes the fds
// normally: `if (!tg_reassign_leader(x)) close_proc_fds(x);`). This is what gives the
// group exit_group semantics — the shared descriptors are torn down only when the final
// member leaves, never yanked out from under a still-running worker (issue #73). Must run
// BEFORE close_proc_fds at every exit/reap site, else close clears the table first and
// the heir inherits nothing.
int tg_reassign_leader(process_t* dying) {
    if (!dying) return 0;
    process_t* heir = NULL;
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        // A LIVE sibling only: never hand the group to a thread that is itself a zombie
        // (also on its way out). If every other member is already dead, there is no heir
        // — return 0 so the caller closes the shared fds (the group is fully exiting).
        if (p && p != dying && p->tgid == dying->pid && p->state != PROC_ZOMBIE) { heir = p; break; }
    }
    if (!heir) return 0;                     // no live survivor: caller closes the fds
    // heap_start FIRST — same reason as elf.c, and this is the worst site for it:
    // the heir is a LIVE thread, already running and already resolvable through
    // tg_leader(), and this runs precisely while a thread group is churning. The
    // heir's own heap_start may still be 0, so publishing program_break first
    // exposes the window [0, brk) — which spans the program's .text — to every
    // sibling that faults in that instant. Hardening only; see the HONEST SCOPE
    // note in elf.c. The intermittent [fault] survived this change.
    heir->heap_start    = dying->heap_start;
    heir->program_break = dying->program_break;
    for (int i = 0; i < PROC_MAX_VMAS; i++) {
        heir->mmap_vmas[i] = dying->mmap_vmas[i];   // heir takes the buffers...
        dying->mmap_vmas[i].file_buf  = 0;          // ...so mmap_free_bufs must skip them
        dying->mmap_vmas[i].file_size = 0;
    }
    heir->mmap_next = dying->mmap_next;
    // The fd table moves as well, and is CLEARED in the dying leader so its
    // close_proc_fds() won't close descriptors the surviving group is still using.
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        heir->ufd_inuse[i]  = dying->ufd_inuse[i];
        heir->ufd_handle[i] = dying->ufd_handle[i];
        heir->ufd_offset[i] = dying->ufd_offset[i];
        dying->ufd_inuse[i] = 0;
    }
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (p && p != dying && p->tgid == dying->pid) p->tgid = heir->pid;
    }
    heir->tgid = 0;                          // the heir leads the group now
    return 1;                                // handed off — caller must NOT close the fds
}

// clone(fn, stack, arg, CLONE_VM) — create a THREAD: a scheduled ring-3 task that
// SHARES the caller's address space instead of getting a COW copy, running fn(arg) on
// the caller-supplied stack with its own kernel stack. Returns the new tid (pid), or
// -1. Without CLONE_VM we refuse (fork() is the separate-address-space primitive).
int do_clone(uint64_t fn, uint64_t stack, uint64_t arg, uint64_t flags) {
    process_t* self = get_current_process();
    if (!self || !self->page_directory) return -1;      // ring-3 tasks only
    if (!(flags & CLONE_VM)) return -1;                  // threads only for now
    if (!fn || !stack) return -1;
    if (process_count >= MAX_PROCESSES) return -1;

    process_t* t = (process_t*)kmalloc(sizeof(process_t));
    if (!t) return -1;
    memset_asm(t, 0, sizeof(process_t));
    t->pid   = next_pid++;
    t->ppid  = self->pid;
    console_capture_inherit(t);
    t->state = PROC_RUN;
    t->page_directory = self->page_directory;   // <- THE thread property: shared VM
    // Join the creator's thread group (a thread cloning a thread still points at the one
    // leader). The heap/VMA fields below are only a fallback snapshot: every sbrk/mmap/VMA
    // lookup resolves through tg_leader(), so the WHOLE group shares one heap and one
    // mmap table — that's what makes malloc/mmap coherent across threads.
    t->tgid           = tg_leader(self)->pid;
    // Where this thread runs. Spreading a thread group across cores needs two
    // things, and v5.8.96 only had one: the page tables coherent (TLB
    // shootdown), AND every task able to identify itself on the core it is
    // actually running on (get_current_process, fixed in v5.8.97 — until then a
    // thread on an AP blocked the BSP's task instead of its own and the machine
    // wedged). Opt-in, so the default placement stays exactly as it always was.
    // Spreading a thread group needs the page tables coherent (TLB shootdown),
    // every task able to identify itself on its own core (v5.8.97), and the
    // BSP not writing its frame into an AP's task (the affinity guard in
    // irq_scheduler_tick). Opt-in, so the default placement never changes.
    // Spreading a thread group needs all of: coherent page tables (TLB
    // shootdown), every task able to identify itself on its own core (v5.8.97),
    // the BSP not writing its frame into an AP's task, and the AP scheduler
    // honouring the same "never round-robin this" filter the BSP uses.
    // Opt-in, so the default placement never changes.
    // STILL PINNED — see scratchpad/GP-iretq-hunt.md for the full handoff.
    // Four hypotheses were reasoned from the source and all four were wrong or
    // insufficient. What is known: the panic is the `iretq` closing irq_common
    // (RIP 0x10049f), error code 0x0 (NOT selector-related, so a non-canonical
    // RIP or bad RFLAGS in the popped frame). Whoever writes that frame has not
    // been identified, and finding them needs measurement, not more reading.
    t->sched_cpu      = tg_leader(self)->sched_cpu;
    t->program_break  = self->program_break;
    t->heap_start     = self->heap_start;
    strncpy(t->comm, self->comm, 31); t->comm[31] = '\0';
    for (int i = 0; i < NSIG; i++) t->sig_handlers[i] = self->sig_handlers[i];
    t->sig_mask       = self->sig_mask;
    t->sig_trampoline = self->sig_trampoline;
    // Copy the VMA table so the thread can demand-fault mappings that already exist in
    // the shared space, but hand it NO file_buf ownership: those snapshot buffers belong
    // to the creator, and a sibling's munmap/exit must not double-free them.
    for (int i = 0; i < PROC_MAX_VMAS; i++) {
        t->mmap_vmas[i] = self->mmap_vmas[i];
        t->mmap_vmas[i].file_buf = 0;
        t->mmap_vmas[i].file_size = 0;
    }
    t->mmap_next = self->mmap_next;
    strncpy(t->cwd, self->cwd, sizeof(t->cwd) - 1);
    t->cwd[sizeof(t->cwd) - 1] = '\0';
    // NO fd copy (unlike fork): the thread group SHARES one fd table, which every fd
    // syscall reaches through tg_leader() (syscall.c fd_owner()). So a thread's own
    // table stays empty — that is exactly what makes close_proc_fds() at its exit a
    // no-op, leaving the group's fds open for its siblings, and it also means no
    // pipe_incref here (there is only ever the one reference the leader holds).

    if (init_thread_task_stack(t, (void*)fn, (void*)stack, arg) < 0) { kfree(t); return -1; }

    self->sched_managed = 1;
    t->sched_managed    = 1;
    t->sched_weight     = self->sched_weight;
    process_table[process_count++] = t;
    sched_enable();
    return (int)t->pid;
}

// futex(uaddr, op, val) — the sleep/wake primitive threads build mutexes on.
//   FUTEX_WAIT: block while *uaddr == val (returns at once if it already differs, which
//               is what makes the lock's compare-and-sleep race-free).
//   FUTEX_WAKE: wake up to `val` waiters, returning how many were woken.
// The queue is keyed by the word's PHYSICAL address (user_v2p), so every task mapping
// that page agrees on the key. The word itself is read through the identity map — the
// same supervisor path copy_to_user uses, so this stays SMEP/SMAP-clean.
int do_futex(uint64_t uaddr, int op, uint32_t val) {
    process_t* self = get_current_process();
    if (!self || !self->page_directory) return -1;
    if (uaddr & 3) return -1;                       // futex words must be 4-byte aligned
    uint64_t key = user_v2p(uaddr);
    if (!key) return -1;                            // unmapped user word

    if (op == FUTEX_WAKE) {
        int woken = 0;
        for (int i = 0; i < process_count && woken < (int)val; i++) {
            process_t* p = process_table[i];
            if (p && p->state == PROC_BLOCKED && p->futex_key == key) {
                p->futex_key = 0;
                p->state     = PROC_RUN;
                woken++;
            }
        }
        return woken;
    }
    if (op != FUTEX_WAIT) return -1;

    // Compare-and-block. We are inside a syscall with interrupts masked, so the compare
    // and the park are atomic w.r.t. a waker on this core — no lost wakeup. Parking
    // sti's, so save/restore the shared user_cr3/user_rsp globals around it exactly like
    // do_waitpid, or another task's syscall would clobber our return path.
    uint64_t saved_cr3 = user_cr3, s_ursp = user_rsp;   /* s_ursp: saved_rsp is now a per-CPU accessor */
    for (;;) {
        __asm__ volatile("cli");
        if (*(volatile uint32_t*)key != val) break;  // changed; return with interrupts masked
        self->blocked_in_kernel = 1;                // resume us on the KERNEL CR3
        self->state     = PROC_BLOCKED;
        self->futex_key = key;
        __asm__ volatile("sti; hlt");
        __asm__ volatile("cli");
        self->blocked_in_kernel = 0;
        if (self->futex_key == 0) break;            // a real FUTEX_WAKE released us
    }
    self->futex_key = 0;
    self->state     = PROC_RUN;
    // Keep per-CPU return context intact until the syscall assembly consumes it.
    user_cr3 = saved_cr3;
    user_rsp = s_ursp;
    return 0;
}

// SYS_EXECVE core: replace the calling process's image with the ELF in [data,size).
// Set p->comm to the basename of `path`, dropping any leading directories and a
// trailing ".elf" — so `ps` and `/proc/<pid>/status` show "spin"/"ps"/"init"
// rather than the "elf" that elf_load hardcodes for every image.
void proc_set_comm(process_t* p, const char* path) {
    if (!p || !path) return;
    const char* base = path;
    for (const char* q = path; *q; q++) if (*q == '/') base = q + 1;
    char tmp[32];
    int i = 0;
    for (; base[i] && i < 31; i++) tmp[i] = base[i];
    tmp[i] = '\0';
    if (i >= 4 && tmp[i-4] == '.' && tmp[i-3] == 'e' && tmp[i-2] == 'l' && tmp[i-1] == 'f')
        tmp[i-4] = '\0';                       // drop the ".elf" suffix
    if (tmp[0]) { strncpy(p->comm, tmp, 31); p->comm[31] = '\0'; }
}

// Loads the program into a fresh address space and swaps it in for the current
// process — same pid, same fds — then builds a SysV entry stack from kargv/kenvp
// ([argc][argv pointers][NULL][envp pointers][NULL] above the strings) and rewrites
// this syscall's saved user frame so the syscall "returns" into the new program's
// entry. kargv[0..argc-1] and kenvp[0..envc-1] are KERNEL-side strings (already
// copied out of the old address space by the SYS_EXECVE case — the old image is
// gone by the time we build), so the child inherits its parent's environment.
// `path` is the exec'd file's path (for comm/cmdline; may be NULL).
// Returns -1 on failure before the commit point (caller left intact); on success it
// "returns" into the new image. Runs inside the syscall (interrupts masked).
// Build a fresh SysV argv+envp frame at the top of `pd`'s stack page and return the
// ring-3 RSP that points at the argc frame. This is the spawn-side counterpart to
// do_execve's frame builder: it writes into a NEW address space's stack (so the kernel
// shell's `exec <file> a b c` can forward argv without replacing the caller). The child
// inherits the SHELL's environment (HOME + any `export`ed vars) via shell_env_snapshot,
// matching do_execve's inherit-the-parent-env behaviour — programs used to get an empty
// environment here, so getenv("HOME") returned NULL when launched from the shell.
// `stack_top` is elf_load_image's value; with no argv AND no env this leaves the stack
// untouched (identical to a no-argv launch). Returns the adjusted RSP.
extern int shell_env_snapshot(char*** out);          // kernel.c — the shell env table

uint64_t build_argv_stack(uint64_t* pd, uint64_t stack_top, char* const* kargv, int argc) {
    if (argc < 0) argc = 0;
    if (argc > 0 && !kargv) argc = 0;
    char** kenvp = 0;
    int envc = shell_env_snapshot(&kenvp);              // inherit the shell's environment
    if (envc < 0 || !kenvp) envc = 0;
    if (envc > 16) envc = 16;
    if (argc == 0 && envc == 0) return stack_top;       // nothing to seed -> keep the empty frame
    if (argc > 32) argc = 32;

    // copy_to_user() translates through the global user_cr3; aim it at the target
    // address space, and mask interrupts across the copy loop so a scheduler tick
    // can't swap user_cr3 out from under us (do_execve runs inside a masked syscall;
    // this path runs from the kernel shell with IF=1, so we must guard it).
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    uint64_t saved_cr3 = user_cr3;
    user_cr3 = (uint64_t)pd;

    uint64_t sp = (stack_top + 0xFFF) & ~0xFFFULL;      // raw top of the stack page
    uint64_t uargv[33];
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = strlen(kargv[i]) + 1;
        sp -= len;
        copy_to_user(sp, kargv[i], len);
        uargv[i] = sp;
    }
    uargv[argc] = 0;                                     // argv terminator
    uint64_t uenvp[17];                                  // envp strings below the argv strings
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = strlen(kenvp[i]) + 1;
        sp -= len;
        copy_to_user(sp, kenvp[i], len);
        uenvp[i] = sp;
    }
    uenvp[envc] = 0;                                     // envp terminator
    sp &= ~0xFULL;
    int qwords = 1 + (argc + 1) + (envc + 1);            // argc + argv[]+NULL + envp[]+NULL
    if (qwords & 1) qwords++;                            // keep entry RSP 16-byte aligned
    sp -= (uint64_t)qwords * 8;
    uint64_t argc64 = (uint64_t)argc, off = 0;
    copy_to_user(sp + off, &argc64, 8);                         off += 8;
    copy_to_user(sp + off, uargv, ((uint64_t)argc + 1) * 8);    off += ((uint64_t)argc + 1) * 8;
    copy_to_user(sp + off, uenvp, ((uint64_t)envc + 1) * 8);    // envp[] + NULL

    user_cr3 = saved_cr3;
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    return sp;
}

int do_execve(const uint8_t* data, uint32_t size, char* const* kargv, int argc,
              char* const* kenvp, int envc, const char* path) {
    process_t* self = get_current_process();
    if (!self || !self->page_directory) return -1;
    if (argc < 0) argc = 0;
    if (envc < 0) envc = 0;

    uint64_t* pd; uint64_t entry, stack_top, brk;
    if (elf_load_image(data, size, &pd, &entry, &stack_top, &brk) != 0) return -1;

    // Name the process after the new image, and record its argv as the cmdline
    // (space-joined) — both surfaced by ps / /proc. Done post-commit so a failed
    // load leaves the old identity intact.
    proc_set_comm(self, path);
    {
        int c = 0;
        for (int i = 0; i < argc && c < (int)sizeof(self->cmdline) - 1; i++) {
            if (i) self->cmdline[c++] = ' ';
            for (const char* s = kargv[i]; *s && c < (int)sizeof(self->cmdline) - 1; s++)
                self->cmdline[c++] = *s;
        }
        if (c == 0 && path) {                  // no argv -> fall back to the path
            for (const char* s = path; *s && c < (int)sizeof(self->cmdline) - 1; s++)
                self->cmdline[c++] = *s;
        }
        self->cmdline[c] = '\0';
    }

    // Commit: swap in the new address space and free the old one. We run on the
    // kernel CR3, so freeing the old user pd is safe (its COW pages' refcounts drop;
    // anything still shared with a relative survives).
    uint64_t* old_pd = (uint64_t*)self->page_directory;
    self->page_directory = pd;
    // heap_start BEFORE program_break — see elf.c, including its HONEST SCOPE
    // note: the reverse order briefly publishes the window [0, brk), which spans
    // the new image's own .text. Hardening only; it did not fix the intermittent
    // [fault] and that fault remains unexplained.
    self->heap_start = brk;      // fresh image -> fresh lazy heap window
    self->program_break = brk;
    // Reset signal state: the new image invalidates every handler address, so all
    // caught signals revert to SIG_DFL (POSIX). Clear pending/in-flight state too.
    for (int i = 0; i < NSIG; i++) self->sig_handlers[i] = SIG_DFL;
    self->sig_pending = 0; self->sig_active = 0; self->sig_mask = 0; self->sig_trampoline = 0;
    // Drop mmap regions: their pages are freed with old_pd, and the fresh image
    // starts with an empty mmap space (free any file-backed snapshot buffers first).
    for (int i = 0; i < PROC_MAX_VMAS; i++) {
        if (self->mmap_vmas[i].file_buf) { kfree(self->mmap_vmas[i].file_buf); self->mmap_vmas[i].file_buf = 0; }
        self->mmap_vmas[i].used = 0;
    }
    self->mmap_next = 0;
    self->tty_raw = 0;           // new image gets the canonical stdin discipline
    self->tty_pending_len = 0;
    // Only release the old space if no sibling CLONE_VM thread is still running in it.
    if (old_pd && !addr_space_shared(old_pd, self)) free_page_directory(old_pd);

    // Build the argv+envp frame on the NEW stack. copy_to_user translates through
    // user_cr3, so point it at the new pd first; the writes land in the fresh
    // stack page via the identity map. Strings go at the very top (overwriting
    // elf_load_image's empty frame), the qword frame below them, in SysV order:
    //   [sp] = argc, [sp+8..] = argv[0..argc-1], NULL, envp[0..envc-1], NULL.
    // crt0 reads argc/argv from [rsp] and derives environ = &argv[argc+1].
    user_cr3 = (uint64_t)pd;
    uint64_t sp = (stack_top + 0xFFF) & ~0xFFFULL;      // raw top of the stack page
    if (argc > 32) argc = 32;
    if (envc > 16) envc = 16;
    uint64_t uargv[33];                                  // user VAs of the argv strings (max 32)
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = strlen(kargv[i]) + 1;
        sp -= len;
        copy_to_user(sp, kargv[i], len);
        uargv[i] = sp;
    }
    uargv[argc] = 0;                                     // argv terminator
    uint64_t uenvp[17];                                  // user VAs of the envp strings (max 16)
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = strlen(kenvp[i]) + 1;
        sp -= len;
        copy_to_user(sp, kenvp[i], len);
        uenvp[i] = sp;
    }
    uenvp[envc] = 0;                                     // envp terminator
    sp &= ~0xFULL;                                       // align, then the qword frame:
    int qwords = 1 + (argc + 1) + (envc + 1);            // argc + argv[]+NULL + envp[]+NULL
    if (qwords & 1) qwords++;                            // keep entry RSP 16-byte aligned
    sp -= (uint64_t)qwords * 8;
    uint64_t argc64 = (uint64_t)argc;
    uint64_t off = 0;
    copy_to_user(sp + off, &argc64, 8);                          off += 8;
    copy_to_user(sp + off, uargv, ((uint64_t)argc + 1) * 8);     off += ((uint64_t)argc + 1) * 8;
    copy_to_user(sp + off, uenvp, ((uint64_t)envc + 1) * 8);     // envp[] + NULL

    // Rewrite the saved syscall frame: [0..14]=GPRs (r15..rax), [15]=RFLAGS, [16]=RIP.
    // Zero the GPRs (rdi/rsi = argc/argv as a courtesy for register-based entry
    // code; crt0 reads them from the stack), install the new entry + clean RFLAGS;
    // user_rsp/user_cr3 (read by the asm return path) point at the new stack/space.
    uint64_t* frame = (uint64_t*)syscall_frame_ptr;
    for (int i = 0; i < 15; i++) frame[i] = 0;
    frame[9]  = argc64;           // rdi = argc
    frame[10] = sp + 8;           // rsi = argv
    frame[15] = 0x202;            // RFLAGS: IF + reserved bit 1
    frame[16] = entry;            // new RIP
    user_rsp  = sp;               // new ring-3 stack, pointing at the argc frame
    return 0;
}

// The setjmp/longjmp blocking-exec launcher (`switch_to_user_process`,
// `g_user_proc`, `return_from_user_process`, the `switch_to_user_trampoline`
// trampoline, and `ku_setjmp`/`ku_longjmp`) was removed in v5.7.9: `exec` now runs
// foreground jobs via spawn_user_path()+kwait() on the preemptive scheduler, and
// syscalls resolve the current process through current_idx (get_current_process).

// Free everything owned by an exited user process: its page directory (all user
// pages + tables), the kernel/context stack (init_user_task_stack kmalloc'd
// kernel_stack-4096 as the base), the process_t, and its process-table slot.
// Only call once the process is no longer executing (e.g. a zombie reaped from a
// kernel thread, or a killed process).
// Close any file descriptors a process left open (frees the internal VFS handles
// and flushes mount-backed writes). Called when a process is reaped so fds don't
// leak across exec/spawn cycles — a well-behaved process closes its own, but a
// crashed or careless one shouldn't exhaust the VFS.
// Non-static: also called from SYS_EXIT so a process's fds (esp. pipe write ends)
// close when it exits, not only when it is reaped — otherwise a parent reading the
// exiting child's pipe never sees EOF (the zombie still holds the write end) and
// deadlocks. Idempotent: it clears ufd_inuse, so the reap-time call is a no-op.
void close_proc_fds(process_t* proc) {
    if (!proc) return;
    extern int vfs_close(int fd);
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (proc->ufd_inuse[i]) {
            int h = proc->ufd_handle[i];
            if (h & UFD_PIPE_FLAG)      pipe_close_end(UFD_PIPE_ID(h), UFD_PIPE_IS_WRITE(h));
            else if (h & UFD_SOCK_FLAG) nsock_close(UFD_SOCK_ID(h));
            else                        vfs_close(h);
            proc->ufd_inuse[i] = 0;
        }
    }
}

void reap_user_process(process_t* proc) {
    if (!proc) return;
    extern void free_page_directory(uint64_t* pml4);
    for (int i = 0; i < process_count; i++) {
        if (process_table[i] == proc) {
            process_table[i] = process_table[--process_count];
            break;
        }
    }
    // Hand the shared thread-group state (heap/mmap AND the fd table) to a surviving
    // member FIRST; only if this was the last member out (no live heir) do we actually
    // close the fds — otherwise the close would tear shared pipe/socket/file descriptors
    // out from under a still-running worker, and clear the table before the handoff could
    // copy it (issue #73).
    if (!tg_reassign_leader(proc)) close_proc_fds(proc);
    mmap_free_bufs(proc);
    if (proc->page_directory && !addr_space_shared(proc->page_directory, proc))
        free_page_directory((uint64_t*)proc->page_directory);
    if (proc->kernel_stack) kfree((void*)((uintptr_t)proc->kernel_stack - 4096));
    sched_forget(proc);            // no core may keep a pointer to freed memory
    kfree(proc);
}

void destroy_process(uint64_t pid) {
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (p && p->pid == pid) {
            if (p->sched_managed && p->state != PROC_ZOMBIE) {
                // A live scheduled process: don't free it out from under the
                // scheduler (it may be a saved ring-3 context awaiting its next
                // slice). Mark it a zombie so the scheduler stops running it, wake
                // anyone kwait()-ing on it, and let reap_zombies() free it safely.
                p->exit_code = -1;             // killed
                p->state = PROC_ZOMBIE;
                wake_waiters(p);
            } else {
                // Not scheduler-managed (a registered placeholder that never ran) —
                // safe to free right here (reap_user_process removes the slot and
                // frees the page directory + kernel-stack kmalloc base).
                reap_user_process(p);
            }
            return;
        }
    }
}

process_t* find_process(uint64_t pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i] && process_table[i]->pid == pid)
            return process_table[i];
    }
    return NULL;
}

// "Which process is running right now?" — the question every syscall asks first.
//
// This was BSP-only and silently wrong for anything running on an application
// processor: `current_idx` is written solely by irq_scheduler_tick, the BSP's
// scheduler, so a task on an AP resolved to whatever the BSP happened to be
// running. It cost two increments to find, because the failure is quiet — a
// thread calling futex_wait would block the BSP's task instead of itself, and
// the machine just wedged with no fault and no panic. Work that never asks who
// it is (spin.elf writing to serial) looked perfectly healthy throughout.
//
// Each AP tracks its own current task in cpu_info[].sched_cur; NULL there means
// the core is in its idle context, which is exactly "no current process".
process_t* get_current_process(void) {
    cpu_info_t* me = cpu_self();
    if (me->cpu_number != 0) return (process_t*)me->sched_cur;
    if (process_count > 0 && current_idx < process_count)
        return process_table[current_idx];
    return NULL;
}

// saved_rsp/next_rsp/next_cr3 used to be globals here. They are per-CPU slots as
// of v5.8.92 (kernel.h maps these names onto cpu_self()), because a second core
// scheduling would otherwise have overwritten the BSP's switch mid-flight.

// Preemptive round-robin scheduling over KERNEL threads (page_directory==NULL).
// Wiring: irq_common (isr_stubs.asm) saves the interrupted context, stores its
// RSP in saved_rsp, calls this, then loads next_rsp/next_cr3 and iretq's into the
// chosen thread. We only switch between kernel threads that share
// kernel_pml4_phys, so next_cr3 is always the kernel PML4 — no ring or
// address-space transition happens here. The kernel is built -mno-sse/-mno-mmx,
// so the 15 GPRs SAVE_REGS pushes are the complete thread state; nothing else to
// save. Enabled at runtime via sched_enable() (see the `mtdemo` command); dormant
// by default, in which case every tick simply keeps the current context — exactly
// the old cooperative behaviour.
static volatile int sched_enabled = 0;
static process_t* idle_proc = NULL;

void sched_enable(void)     { sched_enabled = 1; }
void sched_disable(void)    { sched_enabled = 0; }
int  sched_is_enabled(void) { return sched_enabled; }

// Preemption critical sections. While nonzero, the scheduler keeps the current
// thread — used to make the heap (kmalloc/kfree) safe against being preempted
// mid-update by another thread that also touches it.
// Per-CPU as of v5.8.99 (was one global shared by every core — see smp.h).
//
// NOTE ON WHAT THIS DOES AND DOES NOT PROMISE: it keeps THIS core from being
// preempted, and that is all it ever meant. Call sites that were using it as a
// mutual-exclusion *lock* (net.c, pipe.c, reap_zombies) are only safe while the
// state they guard is touched by one core — true in the default configuration,
// not true once user processes run on APs. Those need real spinlocks; that is a
// separate, labelled piece of work, not something this change quietly fixes.
void preempt_disable(void) { cpu_self()->preempt_count++; }
void preempt_enable(void)  {
    cpu_info_t* me = cpu_self();
    if (me->preempt_count > 0) me->preempt_count--;
}

// Forget a process that is about to be freed. sched_cur is a RAW POINTER held by
// every core, and irq_scheduler_tick dereferences it to choose the resume CR3 —
// so a freed process_t left in any core's slot is a use-after-free. The reap
// paths guard `i == current_idx` (an index) and that does not cover this.
//
// Both callers already hold off preemption on THIS core, and a core whose slot
// still names a dying process is by definition not running it any more, so
// nulling remote slots is safe: the owner re-fills its slot the next time it
// picks, and irq_scheduler_tick treats NULL as "fall back to current_idx".
void sched_forget(process_t* p) {
    if (!p) return;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpu_info[i].sched_cur == (void*)p) cpu_info[i].sched_cur = NULL;
        // #40: a freed task must not be fxsave'd into later — drop it as any core's FPU
        // owner (its XMM state dies with it; the next userspace switch-in restores its own).
        if (cpu_info[i].fpu_owner == (void*)p) cpu_info[i].fpu_owner = NULL;
    }
}

// --- #40 FPU/SSE context save ---------------------------------------------------------
// A clean fxsave template (x87 fninit'd, XMM at CPU-reset zeros, MXCSR default), captured
// once at boot by enable_sse_fpu(); each task's first switch-in fxrstor's this so no XMM
// state leaks between tasks. 16-byte aligned for fxsave/fxrstor.
uint8_t g_fpu_initial[512] __attribute__((aligned(16)));

// fxsave/fxrstor need a 16-byte-aligned pointer; process_t is only 8-aligned (kmalloc), so
// align into the 16-byte slack reserved in process_t.fpu_area[512+16].
static inline uint8_t* fpu_area_of(process_t* p) {
    return (uint8_t*)(((uintptr_t)p->fpu_area + 15u) & ~(uintptr_t)15u);
}

// Switch the live FPU/SSE register file from this core's current owner to `next`. Only
// userspace tasks carry FP state (kernel threads are -mno-sse and never touch XMM), so a
// switch INTO a kernel thread is a no-op: the outgoing task's state stays live in the
// registers (nothing clobbers it) and is banked only when the next userspace task arrives.
// Runs at the single switch chokepoint sched_target() — the BSP's irq_scheduler_tick and
// every AP's ap_scheduler_tick. fpu_owner is cleared by sched_forget() when a task is freed.
static void fpu_switch(process_t* next) {
    if (!next || !next->page_directory) return;          // kernel thread: nothing to manage
    cpu_info_t* me = cpu_self();
    process_t* prev = (process_t*)me->fpu_owner;
    if (prev == next) return;                            // registers already hold next's state
    if (prev) __asm__ volatile("fxsave (%0)" :: "r"(fpu_area_of(prev)) : "memory");  // bank outgoing
    if (next->fpu_ready)
        __asm__ volatile("fxrstor (%0)" :: "r"(fpu_area_of(next)) : "memory");
    else {
        __asm__ volatile("fxrstor (%0)" :: "r"(g_fpu_initial) : "memory");           // first run: clean
        next->fpu_ready = 1;
    }
    me->fpu_owner = (void*)next;
}

// Point next_rsp/next_cr3 at process p (about to be resumed). Kernel threads run
// in the kernel address space; user processes get their own CR3, and the TSS
// RSP0 must be their kernel stack so their next ring3→ring0 entry lands safely.
void sched_target(process_t* p) {
    __atomic_fetch_add(&g_ctx_switches, 1, __ATOMIC_RELAXED);   // one dispatch = one context switch
    next_rsp = (uint64_t)p->stack;
    if (p->page_directory) {
        // A process interrupted in ring 0 (mid-syscall) resumes there, whose
        // -mcmodel=large code lives at low link addresses only mapped in the kernel
        // CR3 — so resume it there, not on its user CR3 (its own kernel stack, a
        // higher-half alias, is mapped in both). It switches back to user CR3 itself
        // when the syscall returns. We read the ring from p's saved irq_common frame
        // (CS at offset 144 == index 18) rather than blocked_in_kernel, so a proc
        // preempted while busy-polling in a syscall (never a blocked_in_kernel yield)
        // is also handled — same rule as the resume-cur path in irq_scheduler_tick.
        uint64_t p_cs = p->stack ? ((volatile uint64_t*)p->stack)[18] : 0x08;
        next_cr3 = ((p_cs & 3) == 3) ? (uint64_t)p->page_directory
                                     : (uint64_t)kernel_pml4_phys;
        // Per-process syscall stack: point BOTH the TSS RSP0 (ring3→ring0 faults)
        // and kernel_rsp (the `syscall` instruction's stack) at this process's own
        // kernel stack. That makes a syscall re-entrant across a context switch — a
        // process blocked mid-syscall keeps its frame on its own stack instead of
        // the old single shared syscall stack that another process's syscall would
        // clobber. This is what lets waitpid() and friends truly block.
        uint64_t kstk = (uint64_t)(uintptr_t)p->kernel_stack + KERNEL_BASE;
        tss_set_stack(kstk);
        kernel_rsp = kstk;

    } else {
        next_cr3 = (uint64_t)kernel_pml4_phys;
    }
    fpu_switch(p);   // #40: bank the outgoing task's XMM/x87, restore p's (userspace only)
}

// ---- CPU-utilization accounting ---------------------------------------------------------
// Sampled once per BSP scheduler tick (the 1000 Hz PIT, 1:1 with tick_count): if the task we
// interrupted is the idle process the core was idle this tick, otherwise it was doing work.
// Over a fixed window the busy fraction IS the CPU utilization; we latch it at each window
// boundary so readers get a stable recent value without racing the accumulator. The window is
// 250 ticks (~250 ms), fast enough to feed a live graph yet long enough to be steady. The
// accumulator is a PURE state machine so a KAT can pin the arithmetic without real timing.
#define PERF_CPU_WINDOW 250u

typedef struct { uint32_t busy, count, latched; } perf_cpu_t;

// Advance one tick. was_idle != 0 when the interrupted context was the idle task. Returns 1 on
// the tick that closes a window (latched refreshed), else 0.
static int perf_cpu_tick(perf_cpu_t* s, int was_idle) {
    if (!was_idle) s->busy++;
    if (++s->count >= PERF_CPU_WINDOW) {
        s->latched = (s->busy * 100u) / s->count;   // count == WINDOW, so 0..100
        s->busy = 0;
        s->count = 0;
        return 1;
    }
    return 0;
}

static perf_cpu_t g_perf_cpu;

// Most recent completed window's BSP utilization, 0..100 (0 until the first window closes).
uint32_t perf_cpu_percent(void) { return g_perf_cpu.latched; }

void irq_scheduler_tick(void) {
    // Performance accounting first, before the wake loop below flips any sleeper's state, so
    // utilization covers every 1000 Hz tick. The interrupted task is process_table[current_idx].
    // The core did real WORK this tick only if that task was actually runnable (PROC_RUN) and
    // not the idle process. Everything else counts as idle: the idle task, and — crucially — a
    // task halted inside a blocking call. NyxOS parks a sleeper/waiter with PROC_BLOCKED and
    // `sti;hlt` in ITS OWN context (see sleep(), kwait, futex), so current_idx still points at
    // it while the CPU is physically halted; counting only `== idle_proc` as idle therefore
    // over-reported ~100% whenever anything (e.g. the compositor's sleep(5) idle-yield) was
    // parked rather than the idle task being scheduled.
    {
        process_t* interrupted = (current_idx >= 0 && current_idx < process_count)
                                     ? process_table[current_idx] : NULL;
        // Busy = a real runnable task was executing AND the CPU wasn't parked in an idle hlt.
        int was_busy = !g_cpu_idle && interrupted && interrupted != idle_proc &&
                       interrupted->state == PROC_RUN;
        perf_cpu_tick(&g_perf_cpu, !was_busy);
    }

    // Timer wait queue: wake any sleeper whose deadline has arrived. Sleepers are
    // PROC_BLOCKED with a non-zero wake_tick (kwait() blockers use wake_tick==0 and
    // are woken by wake_waiters instead, so the two don't collide).
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (p && p->state == PROC_BLOCKED && p->wake_tick != 0 &&
            (int32_t)(tick_count - p->wake_tick) >= 0) {
            p->wake_tick = 0;
            p->state = PROC_RUN;
        }
    }

    // alarm(2) timers: post SIGALRM to any process whose deadline has passed.
    // signal_raise wakes it if it's blocked, so delivery runs on its syscall return.
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (p && p->alarm_tick != 0 && (int32_t)(tick_count - p->alarm_tick) >= 0) {
            p->alarm_tick = 0;
            signal_raise(p, SIGALRM);
        }
    }

    // Default: resume the thread we interrupted, preserving its address space (a
    // ring-3 process keeps its own CR3 if it's the only thing runnable).
    process_t* cur = (current_idx >= 0 && current_idx < process_count)
                         ? process_table[current_idx] : NULL;
    next_rsp = saved_rsp;
    // Resume CR3 depends on WHERE cur was interrupted, read from its just-saved
    // irq_common frame (CS is at frame offset 144 == index 18). Ring 3 = userspace,
    // resume on its own CR3; ring 0 = it was in a syscall — even one merely
    // busy-polling (which never sets blocked_in_kernel) — whose low -mcmodel=large
    // code is only mapped in the kernel CR3, so resume there. THIS is the IRQ-load
    // Heisenbug fix: a userspace program busy-polling the network inside a syscall
    // used to resume on its user CR3 and #PF on the unmapped low kernel .text.
    uint64_t cur_cs = saved_rsp ? ((volatile uint64_t*)saved_rsp)[18] : 0x08;

    // The CR3 must come from THIS core's own record of what it is running, never
    // from process_table[current_idx].
    //
    // current_idx is an INDEX, and the reap paths (reap_zombies, do_waitpid)
    // swap-remove from process_table — on any core, and in do_waitpid's case
    // without even taking sched_lock. That compaction silently redirects the
    // index at a different process. The comment 30 lines below already knew this
    // ("current_idx can come to point at an AP-pinned task... which can hold
    // anything") and guarded `cur->stack` against it — but the CR3 decision here
    // used the same untrustworthy `cur` and was left unguarded.
    //
    // When the index lands on a KERNEL thread, page_directory is NULL, this
    // ternary falls to kernel_pml4_phys, and irq_common iretq's the *interrupted*
    // ring-3 frame (CS still 0x1b) onto the kernel's address space. The process
    // then executes with a page table its code does not appear in, and the first
    // instruction fetch dies with CR2 == RIP.
    //
    // HONEST SCOPE: that is a real hole and this closes it, but it is NOT the fix
    // for P0.1 — the intermittent ring-3 [fault] reproduced on the FIRST round
    // with this change in place. P0.1 still shows a ring-3 task on the kernel
    // CR3, so some OTHER path produces the same end state. Do not read this
    // comment as "the CR3 bug is solved"; see the P0.1 entry in the blockers.
    //
    // sched_cur is a raw pointer, which cannot be invalidated by table compaction
    // the way an index can — but it CAN dangle if the process is freed, which is
    // why every free path calls sched_forget() above.
    process_t* run = (process_t*)cpu_self()->sched_cur;
    if (!run) run = cur;                 // first tick after boot, before we ever picked
    next_cr3 = (run && run->page_directory && (cur_cs & 3) == 3)
                   ? (uint64_t)run->page_directory
                   : (uint64_t)kernel_pml4_phys;   // ring-0 (mid-syscall) -> kernel CR3

    // Dormant, in a heap/critical section, or nothing else to schedule.
    // OUR count, not the machine's: an AP inside a critical section used to stop
    // the BSP from scheduling at all.
    if (!sched_enabled || cpu_self()->preempt_count > 0 || process_count == 0) return;
    if (!cur) return;

    // Weighted round-robin: keep running the current thread until its time quantum
    // is spent, so a high-weight proc (the compositor, SCHED_WEIGHT_GUI) runs
    // several ticks in a row and gets a bigger CPU share than background jobs
    // (weight 1). Only while it's still runnable — a blocked/exited cur falls
    // through to pick someone else.
    if (cur->state == PROC_RUN && cur->sched_quantum > 1) {
        cur->sched_quantum--;
        return;                          // defaults above resume cur
    }

    // Remember where to resume the outgoing thread.
    // Remember where to resume the outgoing thread — but ONLY if it is really
    // ours. current_idx can come to point at an AP-pinned task without this
    // scheduler ever choosing one: the reap paths swap-remove and then patch
    // `current_idx` to the moved slot, which can hold anything. Writing our
    // frame into that task's stack hands an AP a context whose CS/SS belong to
    // us, and the AP's iretq takes a #GP (err 0x8) — the panic pstorm hit at
    // RIP 0x10049f once processes were spread across cores.
    if (cur->sched_cpu == 0) cur->stack = (void*)saved_rsp;

    // Round-robin to the next runnable thread. We schedule kernel threads and
    // spawned (sched_managed) user processes; we skip idle unless nothing else
    // runs, NULL-stack placeholders (e.g. `init` — switching to it would iretq off
    // a null RSP), unmanaged user procs (an image still being prepared), and
    // non-RUN states (parked/zombie).
    for (int i = 1; i <= process_count; i++) {
        int idx = (current_idx + i) % process_count;
        process_t* p = process_table[idx];
        if (!p || p->state != PROC_RUN) continue;
        if (p == idle_proc) continue;
        if (p->stack == NULL) continue;
        if (p->sched_cpu != 0) continue;      // pinned to an AP — that core's job
        if (p->page_directory != NULL && !p->sched_managed) continue;
        current_idx = idx;
        // current_idx stays as the round-robin SCAN CURSOR (a stale cursor only
        // costs a skipped turn). sched_cur is the separate, per-core record of
        // WHICH PROCESS IS RUNNING — the job the index was quietly doing as well
        // and could not do safely. The APs have kept it since stage 3a; the BSP
        // never did, which is why get_current_process() had to special-case cpu 0.
        cpu_self()->sched_cur = p;
        p->sched_quantum = p->sched_weight ? p->sched_weight : 1;   // start its turn
        sched_target(p);
        return;
    }
    // Nobody else runnable — keep the current thread, refreshing its quantum.
    if (cur->state == PROC_RUN)
        cur->sched_quantum = cur->sched_weight ? cur->sched_weight : 1;
}

void schedule(void) {
}

// Free every ZOMBIE process (a spawned user proc that exited): its user address
// space, kernel stack, process_t, and table slot. MUST run in a normal kernel
// thread (the compositor's background-task slot), never in the IRQ path — it
// calls kfree/free_page_directory. preempt_disable keeps the scheduler from
// switching mid-free, and a zombie is never the current thread (it yielded on
// exit), so freeing its stack/CR3 is safe.
void reap_zombies(void) {
    extern void free_page_directory(uint64_t* pml4);
    preempt_disable();
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (!p || p->state != PROC_ZOMBIE) continue;
        if (i == current_idx) continue;                 // never free the running proc
        // Leave a zombie whose parent is a live user process: that parent will
        // waitpid() it and collect the exit status (do_waitpid reaps it there).
        // Only orphans (parent gone) and background jobs (kernel-thread parent,
        // page_directory==NULL — the shell's `spawn`) are auto-reaped here.
        process_t* par = find_process(p->ppid);
        if (par && par->page_directory && par->state != PROC_ZOMBIE) continue;
        if (!tg_reassign_leader(p)) close_proc_fds(p);  // hand shared fds to a live thread first; close only if last member (#73)
        mmap_free_bufs(p);
        if (p->page_directory && !addr_space_shared(p->page_directory, p))
            free_page_directory((uint64_t*)p->page_directory);
        if (p->kernel_stack) kfree((void*)((uintptr_t)p->kernel_stack - 4096));
        // Swap-remove, keeping current_idx pointing at the same live proc if the
        // one we moved happened to be the current thread. Under sched_lock: an AP
        // may be scanning this array right now.
        uint64_t sfl = spin_lock_irqsave(&sched_lock);
        int last = --process_count;
        process_table[i] = process_table[last];
        process_table[last] = NULL;
        if (current_idx == last) current_idx = i;
        sched_forget(p);           // no core may keep a pointer to freed memory
        spin_unlock_irqrestore(&sched_lock, sfl);
        kfree(p);
        i--;                                            // re-examine the swapped-in slot
    }
    preempt_enable();
}

// SYS_WAITPID core — a TRUE blocking wait, now that per-process syscall stacks
// (sched_target) make it safe to block mid-syscall. Wait for a child of the caller
// (`wpid > 0`: that exact pid; `wpid <= 0`: any child) to exit, then collect and
// reap it. Returns the reaped child's pid with its exit code in *out_code, or -1 if
// there is no such child. Parks the caller with PROC_BLOCKED + `sti;hlt`, woken by
// the child's SYS_EXIT (wake_waiters), exactly like kwait().
//
// Two subtleties of blocking inside a syscall: (1) `blocked_in_kernel` makes the
// scheduler resume us on the KERNEL CR3 (we're in ring 0, and -mcmodel=large kernel
// code lives at low link addresses only mapped there), so we run do_waitpid on the
// kernel CR3 throughout — needed for free_page_directory's page-table walk and the
// caller's copy_to_user of the status. (2) Other processes run their own syscalls
// while we sleep, overwriting the shared user_cr3/user_rsp globals — so we save them
// on entry and restore them before returning, so the asm syscall-return path iretq's
// back into THIS process. Interrupts stay masked from the decision to return until
// that iretq re-enables them.
int do_waitpid(int wpid, int* out_code, int options) {
    extern void free_page_directory(uint64_t* pml4);
    process_t* self = get_current_process();
    if (!self) return -1;
    uint64_t saved_cr3 = user_cr3, saved_ursp = user_rsp;
    int result;
    for (;;) {
        __asm__ volatile("cli");
        self->blocked_in_kernel = 0;                     // past any block; interrupts are off
        int zi = -1, si = -1, any = 0;
        for (int i = 0; i < process_count; i++) {
            process_t* p = process_table[i];
            if (!p || p->ppid != self->pid) continue;    // only our own children
            if (wpid > 0 && (int)p->pid != wpid) continue;
            any = 1;
            if (p->state == PROC_ZOMBIE) { zi = i; break; }   // an exit takes priority over a stop
            if ((options & WUNTRACED) && p->state == PROC_STOPPED && !p->stopped_reported)
                si = i;                                  // a not-yet-reported stop (Ctrl-Z)
        }
        if (!any) { result = -1; break; }                // ECHILD (interrupts stay off)
        if (zi < 0 && si >= 0) {                          // a child stopped — report, don't reap
            process_t* ch = process_table[si];
            ch->stopped_reported = 1;                     // report each stop exactly once
            if (out_code) *out_code = WSTOPPED | (int)ch->stop_sig;
            result = (int)ch->pid;
            break;                                        // interrupts stay off until return
        }
        if (zi >= 0) {                                    // a child is ready — collect + reap it
            process_t* child = process_table[zi];
            if (out_code) *out_code = child->exit_code;
            result = (int)child->pid;
            if (!tg_reassign_leader(child)) close_proc_fds(child);  // hand shared fds to a live thread first; close only if last member (#73)
            mmap_free_bufs(child);
            if (child->page_directory && !addr_space_shared(child->page_directory, child))
                free_page_directory((uint64_t*)child->page_directory);
            if (child->kernel_stack) kfree((void*)((uintptr_t)child->kernel_stack - 4096));
            int last = --process_count;                   // swap-remove, keeping current_idx valid
            process_table[zi] = process_table[last];
            process_table[last] = NULL;
            if (current_idx == last) current_idx = zi;
            kfree(child);
            break;
        }
        // A child exists but hasn't exited. WNOHANG callers (shell reaping `&` jobs)
        // don't want to block — report "none ready yet" with 0 and return at once.
        if (options & WNOHANG) { result = 0; break; }     // interrupts stay off until return
        // Otherwise park until one does. The scheduler runs the child (on its own
        // kernel stack), whose SYS_EXIT wakes us via wake_waiters.
        self->blocked_in_kernel = 1;                      // resume us on the kernel CR3
        self->state = PROC_BLOCKED;
        self->waiting_for = (wpid > 0) ? (uint32_t)wpid : 0;
        __asm__ volatile("sti; hlt");
        // Resumed on the kernel CR3 — loop, clear the flag, re-check (interrupts off).
    }
    user_cr3 = saved_cr3;                                 // restore OUR syscall globals for the
    user_rsp = saved_ursp;                                // asm return path (still on kernel CR3)
    return result;
}

// Block the calling thread until child `pid` becomes a zombie, then return its
// exit code. This is the foreground-`exec` primitive: the shell (compositor)
// thread parks here in PROC_BLOCKED, the scheduler runs the child, and the
// child's SYS_EXIT wakes us via wake_waiters(). The zombie is freed afterwards by
// reap_zombies() (it can't run while we're parked, so we still see the zombie).
int kwait(uint32_t pid) {
    process_t* self = get_current_process();
    if (!self) return -1;
    for (;;) {
        // Check-and-block must be atomic w.r.t. the child exiting: with interrupts
        // off the scheduler can't run, so the child can't zombie-and-wake between
        // our check and marking ourselves blocked (no lost wakeup, single core).
        __asm__ volatile("cli");
        process_t* child = find_process(pid);
        if (!child) { __asm__ volatile("sti"); return -1; }
        if (child->state == PROC_ZOMBIE) {
            int code = child->exit_code;
            __asm__ volatile("sti");
            return code;
        }
        self->state = PROC_BLOCKED;
        self->waiting_for = pid;
        // sti+hlt is atomic (sti delays interrupts one instruction): enable, then
        // halt until the timer preempts us. The scheduler parks us (BLOCKED) and
        // runs the child; when it exits we're set back to PROC_RUN and resumed here.
        __asm__ volatile("sti; hlt");
        // Resumed — loop and re-check (we're PROC_RUN again).
    }
}

// Wake a parent blocked in kwait() on this exiting child. Called from SYS_EXIT
// with interrupts masked (so the state change is atomic w.r.t. the parent).
void wake_waiters(process_t* child) {
    if (!child || !child->ppid) return;
    process_t* parent = find_process(child->ppid);
    if (parent && parent->state == PROC_BLOCKED &&
        (parent->waiting_for == child->pid || parent->waiting_for == 0)) {
        parent->waiting_for = 0;
        parent->state = PROC_RUN;
    }
}

// Block the caller until ALL of its children (procs with ppid == our pid) have
// exited — the `wait` shell command with no argument. Same block/wake machinery as
// kwait(), but woken by ANY child's exit (wake_waiters matches waiting_for==0).
// Zombies are counted as done and freed by reap_zombies() once we return.
void kwait_all(void) {
    process_t* self = get_current_process();
    if (!self) return;
    for (;;) {
        // Atomic re-check + block (interrupts off ⇒ no child can exit-and-wake in
        // the gap ⇒ no lost wakeup on a single core).
        __asm__ volatile("cli");
        int pending = 0;
        for (int i = 0; i < process_count; i++) {
            process_t* p = process_table[i];
            if (p && p != self && p->ppid == self->pid && p->state != PROC_ZOMBIE) {
                pending = 1;
                break;
            }
        }
        if (!pending) { __asm__ volatile("sti"); return; }
        self->state = PROC_BLOCKED;
        self->waiting_for = 0;               // wake on any child's exit
        __asm__ volatile("sti; hlt");
    }
}

static void idle_task(void) {
    while (1) {
        __asm__ volatile("hlt");
    }
}

static int idle_created = 0;

void ensure_idle_process(void) {
    if (idle_created) return;
    idle_created = 1;
    idle_proc = create_process("idle", (void*)idle_task, 0);
}

static void task_blink(void) {
    // (was a serial heartbeat that spammed '.' — removed)
}

static int task_uptime_counter = 0;
static void task_uptime(void) {
    task_uptime_counter++;
    if (task_uptime_counter >= 50) {
        task_uptime_counter = 0;
    }
}

typedef struct {
    char name[16];
    void (*func)(void);
    int active;
} background_task_t;

static background_task_t bg_tasks[8];
static int bg_task_count = 0;
static int bg_task_cur = 0;

void register_background_task(const char* name, void (*func)(void)) {
    if (bg_task_count >= 8) return;
    strncpy(bg_tasks[bg_task_count].name, name, 15);
    bg_tasks[bg_task_count].func = func;
    bg_tasks[bg_task_count].active = 1;
    bg_task_count++;
}

void run_background_tasks(void) {
    if (bg_task_count == 0) return;
    bg_task_cur = (bg_task_cur + 1) % bg_task_count;
    if (bg_tasks[bg_task_cur].active)
        bg_tasks[bg_task_cur].func();
}

void init_background_tasks(void) {
    register_background_task("blink", task_blink);
    register_background_task("uptime", task_uptime);
    register_background_task("reap", reap_zombies);   // frees exited spawned procs
}

// --- Multitasking self-test (the `mtdemo` shell command) --------------------
// Two kernel threads that spin forever, each bumping its own counter and emitting
// a ~1 Hz serial heartbeat. Turning them on proves the preemptive scheduler
// time-slices them alongside the GUI compositor (which runs as the main "init"
// thread). Safe by construction: each thread touches only its own counter and the
// serial port, never GUI or shared kernel state.
volatile uint64_t mtdemo_a_count = 0;
volatile uint64_t mtdemo_b_count = 0;
static int mtdemo_started = 0;
static process_t* mtdemo_a_proc = NULL;
static process_t* mtdemo_b_proc = NULL;

// Each demo thread spins for MTDEMO_BEATS ~1 Hz heartbeats, then retires itself by
// setting state=0 so the scheduler stops picking it — this hands the CPU back to
// the compositor and the desktop returns to full speed. A retired thread never
// runs past the hlt loop (the scheduler skips state!=1), so it stays parked.
#define MTDEMO_BEATS 3

static void mtdemo_thread_a(void) {
    uint32_t last = tick_count; int beats = 0;
    for (;;) {
        mtdemo_a_count++;
        uint32_t t = tick_count;
        if (t - last >= 1000) {
            last = t;
            serial_puts("[mtdemo] thread A alive\n");
            if (++beats >= MTDEMO_BEATS) break;
        }
    }
    if (mtdemo_a_proc) mtdemo_a_proc->state = 0;   // retire
    for (;;) __asm__ volatile("hlt");
}

static void mtdemo_thread_b(void) {
    uint32_t last = tick_count; int beats = 0;
    for (;;) {
        mtdemo_b_count++;
        uint32_t t = tick_count;
        if (t - last >= 1000) {
            last = t;
            serial_puts("[mtdemo] thread B alive\n");
            if (++beats >= MTDEMO_BEATS) break;
        }
    }
    if (mtdemo_b_proc) mtdemo_b_proc->state = 0;   // retire
    for (;;) __asm__ volatile("hlt");
}

// Create the two demo threads and switch preemption on. Single-run per boot: the
// threads retire themselves after their heartbeats and park, so there's nothing to
// meaningfully restart. Returns 0 on a fresh start, 1 if already run this session,
// -1 if the threads couldn't be created.
int mtdemo_start(void) {
    if (mtdemo_started) return 1;
    mtdemo_a_proc = create_process("mtdemoA", (void*)mtdemo_thread_a, 0);
    mtdemo_b_proc = create_process("mtdemoB", (void*)mtdemo_thread_b, 0);
    if (!mtdemo_a_proc || !mtdemo_b_proc) return -1;
    mtdemo_b_proc->sched_weight = 3;   // B gets 3x the CPU of A — shows weighted RR
    mtdemo_started = 1;
    sched_enable();
    return 0;
}

// KAT: pins the CPU-utilization accumulator (perf_cpu_tick / perf_cpu_percent). Drives a fresh
// state machine with known idle/busy tick patterns over exact windows and checks the latched
// percent, plus the mid-window contract (nothing latches until a window closes). The busy
// fraction of a window must equal the utilization, floored to an integer. 0 = PASS, else the
// failing case number.
int perf_cpu_selftest(void) {
    perf_cpu_t s = {0, 0, 0};
    // 1. a full window entirely busy -> 100%, and the boundary tick reports the close
    for (uint32_t i = 0; i < PERF_CPU_WINDOW - 1; i++)
        if (perf_cpu_tick(&s, 0) != 0) return 1;          // no early latch mid-window
    if (perf_cpu_tick(&s, 0) != 1) return 2;              // closing tick signals
    if (s.latched != 100) return 3;
    if (s.count != 0 || s.busy != 0) return 4;            // window reset for the next round

    // 2. a full window entirely idle -> 0%
    for (uint32_t i = 0; i < PERF_CPU_WINDOW; i++) perf_cpu_tick(&s, 1);
    if (s.latched != 0) return 5;

    // 3. exactly half busy -> 50% (WINDOW is even)
    for (uint32_t i = 0; i < PERF_CPU_WINDOW; i++) perf_cpu_tick(&s, (i & 1) ? 1 : 0);
    if (s.latched != 50) return 6;

    // 4. one quarter busy -> 25%
    for (uint32_t i = 0; i < PERF_CPU_WINDOW; i++) perf_cpu_tick(&s, (i % 4 == 0) ? 0 : 1);
    if (s.latched != 25) return 7;

    // 5. mid-window, the latched value still reflects the LAST closed window, not the partial
    uint32_t prev = s.latched;
    for (uint32_t i = 0; i < PERF_CPU_WINDOW / 2; i++) perf_cpu_tick(&s, 0);
    if (s.latched != prev) return 8;                      // half a window in -> unchanged

    return 0;
}
