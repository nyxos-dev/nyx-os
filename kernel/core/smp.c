#include "kernel.h"
#include "../drivers/misc/apic.h"
#include "smp.h"
#include "spinlock.h"

cpu_info_t cpu_info[MAX_CPUS];
uint32_t cpu_count = 1;

extern uint8_t _binary_trampoline_bin_start[];
extern uint8_t _binary_trampoline_bin_end[];

// AP interrupt stubs (isr_stubs.asm)
extern void ap_timer_stub(void);
extern void ap_spurious_stub(void);
extern void tlb_ipi_stub(void);

// AP-local periodic scheduler tick. This reload is uncalibrated: divide-by-16
// with 62500 counts measures about 1000 Hz in the QEMU 10.2.1 TCG test VM.
// The BSP's PIT is initialized after smp_init and is not a calibration source
// here. Hardware LAPIC frequencies can differ; calibration remains open.
#define AP_TIMER_INITCNT 62500

// The AP real-mode trampoline is copied to this physical page; a Startup-IPI's
// 8-bit vector is the START PAGE number, so vector = addr >> 12 (0x8000 -> 0x08).
#define AP_TRAMPOLINE_ADDR 0x8000
#define AP_SIPI_VECTOR     (AP_TRAMPOLINE_ADDR >> 12)

// Which CPU is executing this code? Resolved from the initial APIC id in
// CPUID(1).EBX[31:24] — an instruction, not a memory access, so it is valid
// before the LAPIC mapping is reachable and safe to call from an interrupt.
// The eventual answer is a GS-based per-CPU pointer, which is also what per-CPU
// user_cr3/user_rsp will need; a linear scan over at most MAX_CPUS entries is
// enough while the only per-CPU field is a counter.
cpu_info_t* cpu_self(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    uint32_t id = (ebx >> 24) & 0xFF;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        if (cpu_info[i].apic_id_self == id) return &cpu_info[i];
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        if (cpu_info[i].apic_id == id) return &cpu_info[i];
    return &cpu_info[0];
}

// The application processors' entire interrupt hot path: count our own tick and
// acknowledge our own LAPIC. Both are strictly CPU-local — cpu_info[] is indexed
// per CPU, and the 0xFEE00000 MMIO window is decoded by whichever core issues the
// access. Nothing else belongs here: the kernel has no spinlocks yet, so printf,
// kmalloc or the scheduler would race the BSP.
void ap_timer_tick(void) {
    cpu_self()->ticks++;
    apic_eoi();
}

/* ------------------------------------------------------------------------- */
/*  TLB shootdown (v5.8.94)                                                    */
/* ------------------------------------------------------------------------- */
/* Changing a page table entry only invalidates the TLB of the core that did it.
 * Every OTHER core that has the old translation cached keeps using it — reading
 * or writing a page that, as far as the page tables are concerned, is gone. The
 * fix is to interrupt those cores and make them invalidate it too.
 *
 * DEADLOCK IS THE WHOLE DESIGN PROBLEM HERE, and it has two shapes:
 *
 *  1) Two cores shooting down at once. The sender holds a lock and waits for the
 *     others to acknowledge. If the second core spun on that lock with
 *     INTERRUPTS DISABLED it could never service the first core's IPI, and both
 *     would wait forever. So this lock is taken with plain spin_lock() — NOT
 *     spin_lock_irqsave — and the acknowledgement wait runs with interrupts on.
 *     A core waiting its turn stays interruptible, which is what breaks the
 *     cycle.
 *
 *  2) Shooting down while holding a lock other cores spin on with interrupts
 *     off. page_lock and kmalloc_lock are exactly that. THE RULE: never call
 *     tlb_shootdown() while holding one. Callers are safe today because
 *     alloc_page/free_page/kmalloc take and release those internally and are
 *     never still holding one when they return to the caller doing the unmap.
 */
static spinlock_t shootdown_lock = SPINLOCK_INIT;
static volatile uint64_t shootdown_addr;
static volatile uint32_t shootdown_pending;   // bitmask of CPUs yet to acknowledge

uint64_t tlb_shootdowns = 0;                  // observability: how many we've sent

// Invalidate `vaddr` on THIS core and on every other online core, returning
// only once they have all confirmed. See the deadlock note above before adding
// a caller.
void tlb_shootdown(uint64_t vaddr) {
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");   // always ours
    if (cpu_count < 2 || !lapic) return;

    // Callers reach us with interrupts DISABLED: syscalls run under SF_MASK
    // (which clears IF) and the fault handlers sit behind interrupt gates. That
    // is fatal here — a core that cannot take an interrupt cannot acknowledge
    // anyone else's shootdown, which is deadlock shape (1) wearing a disguise.
    // So enable interrupts for the duration and put the caller's IF back exactly
    // as it was on the way out. We are never called from an IRQ handler, only
    // from syscall/fault context, where being interruptible is already normal.
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; sti" : "=r"(rflags) :: "memory");

    spin_lock(&shootdown_lock);        // deliberately NOT irqsave — see above
    uint32_t mask = 0;
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++)
        if (cpu_info[i].running && i != cpu_self()->cpu_number) mask |= (1u << i);

    if (mask) {
        shootdown_addr = vaddr;
        __asm__ volatile("" ::: "memory");
        shootdown_pending = mask;
        __asm__ volatile("mfence" ::: "memory");
        apic_send_ipi_all_but_self(IPI_TLB_VECTOR);

        // Bounded wait: a core that never answers must not hang the machine —
        // better a possibly-stale TLB on one core than a dead system.
        //
        // The bound is deliberately modest. An acknowledgement is microseconds;
        // the reason to time out at all is a core that CANNOT answer, e.g. one
        // sitting with interrupts disabled. That is not hypothetical — it is
        // exactly what `tlbtest` does on purpose — and with a huge bound the
        // sender burns seconds spinning for an answer that will never come.
        int spins = 0;
        while (shootdown_pending && ++spins < 2000000)
            __asm__ volatile("pause" ::: "memory");
        if (shootdown_pending) {
            // Say so ONCE. A silently degraded TLB is the kind of thing that
            // turns into an unexplainable bug three versions later.
            static int warned = 0;
            if (!warned) {
                warned = 1;
                printf("[SMP] WARNING: TLB shootdown timed out (cpus 0x%x did not ack);"
                       " their TLB may be stale. Reported once.\n", shootdown_pending);
            }
            shootdown_pending = 0;
        }
    }
    tlb_shootdowns++;
    spin_unlock(&shootdown_lock);
    __asm__ volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
}

// The IPI handler. Runs on every core except the sender.
void tlb_shootdown_ipi(void) {
    __asm__ volatile("invlpg (%0)" :: "r"(shootdown_addr) : "memory");
    uint32_t bit = 1u << cpu_self()->cpu_number;
    __asm__ volatile("lock andl %1, %0"
                     : "+m"(shootdown_pending) : "r"(~bit) : "memory");
    apic_eoi();
}

volatile int smp_work_active = 0;

// Off by default — see create_user_process(). Turning it on is what makes newly
// spawned user processes land on application processors.
volatile int smp_user_balance = 0;

// This core's scheduler. Called from ap_timer_stub with the interrupted thread's
// RSP already parked in sc_saved_rsp; it answers in sc_next_rsp.
//
// Round-robin over the tasks PINNED to this CPU (p->sched_cpu == our number).
// Pinning is the entire mutual-exclusion argument: no other core will even look
// at these tasks, so nothing here can race the BSP over who runs what. The lock
// covers the table scan and selection/FPU publication against a concurrent
// creator or collector. The IRQ stub acknowledges physical handoff afterwards.
//
// sched_cur == NULL means "the idle context this core booted into", whose stack
// pointer we park in idle_rsp so there is always somewhere to go back to when no
// pinned thread is runnable. Every task here is a kernel thread sharing the
// kernel address space, so there is no CR3 to switch and no ring to cross.
void ap_scheduler_tick(void) {
    cpu_info_t* me = cpu_self();
    me->ticks++;
    apic_eoi();

    process_t* cur = (process_t*)me->sched_cur;
    if (cur) cur->stack = (void*)me->sc_saved_rsp;    // remember where to resume it
    else     me->idle_rsp = me->sc_saved_rsp;         // ...or where idle left off

    extern process_t* process_table[MAX_PROCESSES];
    extern int process_count;
    extern spinlock_t sched_lock;

    process_t* pick = NULL;

    // This core in a critical section: keep running whatever it is, exactly as
    // the BSP does. Expressed as "pick = cur" rather than an early return
    // because sc_next_rsp MUST be written before we hand control back to the
    // stub — returning early would resume the previous tick's stale pointer.
    if (me->preempt_count > 0) {
        me->sc_next_rsp = me->sc_saved_rsp;
        me->sc_next_cr3 = kernel_pml4_phys;   // AP tasks are kernel-side today
        return;
    }

    uint64_t fl = spin_lock_irqsave(&sched_lock);
    int n = process_count;
    for (int i = 1; i <= n; i++) {
        int idx = (me->sched_scan + i) % n;
        process_t* p = process_table[idx];
        if (!p || p->state != PROC_RUN) continue;
        if (p->sched_cpu != (int32_t)me->cpu_number) continue;
        if (!p->stack) continue;
        // The BSP skips these and so must we: a user process that is not
        // sched_managed (init.elf, the blocking-exec target) is deliberately
        // never round-robined — its saved frame was never built to be resumed
        // by a scheduler. Running one anyway means iretq'ing into whatever
        // happens to be there, which is the #GP at RIP 0x10049f.
        if (p->page_directory != NULL && !p->sched_managed) continue;
        pick = p;
        me->sched_scan = idx;
        break;
    }
    me->sched_cur = pick;
    if (pick) {
        // sched_target() is the BSP's battle-tested aiming logic, and every slot
        // it writes (next_rsp/next_cr3/kernel_rsp, and the TSS RSP0) is per-CPU
        // as of stages 2/3a — so it is correct verbatim on an AP. Reusing it,
        // rather than a second copy, is what makes user processes work here: all
        // the ring/CR3 subtleties earlier Heisenbugs taught us come along.
        sched_target(pick);
    } else {
        me->sc_next_rsp = me->idle_rsp;      // back to the boot idle loop
        me->sc_next_cr3 = kernel_pml4_phys;  // ...which is always kernel-side
    }
    spin_unlock_irqrestore(&sched_lock, fl);
}

// The per-AP worker. It counts only while smp_work_active, and halts otherwise,
// so an idle machine pays nothing for having these threads parked on its cores.
//
// It deliberately counts into cpu_self()->work_ops rather than a slot chosen by
// its own id: the counter therefore records which core ACTUALLY executed it. If
// pinning were broken and the BSP ran this thread, the work would show up in the
// BSP's row — the test can't be fooled by the thread simply reporting where it
// was supposed to be.
// It also takes over the allocator hammer: once a worker exists, the scheduler
// always picks it over the idle context, so smpstress would otherwise stop
// seeing any AP participation.
// TLB-test rendezvous. The BSP bumps tlb_test_req; CPU 1 reads the watched
// address and echoes the sequence back. Only one core answers, so the value
// reported is unambiguously one core's view through its own TLB.
volatile int      tlb_test_req = 0;
volatile int      tlb_test_ack = 0;
volatile uint64_t tlb_test_va  = 0;
volatile uint64_t tlb_test_v1 = 0, tlb_test_v2 = 0, tlb_test_v3 = 0;

// The three-phase protocol CPU 1 runs for cmd_tlbtest.
//
// Phase 1->2 is taken with INTERRUPTS OFF, and that is the entire point. This
// core reloads CR3 on every timer tick (a context switch), and a CR3 reload
// flushes the TLB — so with interrupts on, a stale entry evaporates by itself
// within a millisecond and the test can never observe one. Holding interrupts
// off across the remap is what creates a window where staleness is real and
// therefore measurable. Interrupts come back on before phase 3, because the
// shootdown IPI cannot be delivered to a core that is not listening.
static void tlb_test_participate(void) {
    long guard;
    __asm__ volatile("cli");
    tlb_test_v1 = *(volatile uint64_t*)tlb_test_va;   // caches the translation
    tlb_test_ack = 1;
    guard = 0;
    while (tlb_test_req == 1 && ++guard < 100000000) __asm__ volatile("pause");
    tlb_test_v2 = *(volatile uint64_t*)tlb_test_va;   // remapped, nobody told us
    tlb_test_ack = 2;
    __asm__ volatile("sti");                          // now we can hear the IPI
    guard = 0;
    while (tlb_test_req == 2 && ++guard < 100000000) __asm__ volatile("pause");
    tlb_test_v3 = *(volatile uint64_t*)tlb_test_va;   // after the shootdown
    tlb_test_ack = 3;
}

static void ap_worker(void) {
    for (;;) {
        cpu_info_t* me = cpu_self();
        if (tlb_test_req == 1 && tlb_test_ack == 0 && me->cpu_number == 1 && tlb_test_va) {
            tlb_test_participate();
            continue;
        }
        if (smp_stress_active)    smp_stress_iteration(me);
        else if (smp_work_active) me->work_ops++;
        else __asm__ volatile("hlt");
    }
}

// One worker per application processor, created on the BSP once the process
// table exists. Nothing pins work to CPU 0 — the BSP keeps its own scheduler.
void smp_start_ap_threads(void) {
    for (uint32_t i = 1; i < cpu_count && i < MAX_CPUS; i++) {
        if (!cpu_info[i].running) continue;
        char name[32];
        strncpy(name, "apworker0", sizeof(name) - 1);
        name[8] = (char)('0' + i);
        if (!create_kernel_thread_on_cpu(name, (void*)ap_worker, (int)i))
            printf("[SMP] could not create worker for CPU %u\n", i);
    }
    printf("[SMP] %u AP worker thread(s) pinned\n", cpu_count > 1 ? cpu_count - 1 : 0);
}

// Point this CPU's GS base at its own cpu_info slot, so syscall_entry can reach
// the ABI-fixed prefix with no free register (isr_stubs.asm explains why this is
// IA32_GS_BASE and not the usual swapgs/IA32_KERNEL_GS_BASE pairing).
//
// The HIGHER-HALF alias is essential: `syscall` arrives still on the USER CR3,
// which maps the kernel only through the PML4[511] mirror, never at its low link
// address. Must be called AFTER any reload of the GS selector — loading a
// segment selector in long mode zeroes that segment's base.
#define IA32_GS_BASE 0xC0000101   /* 0xC0000100 is FS_BASE, 0xC0000102 is KERNEL_GS_BASE */

void cpu_install_gs_base(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;
    write_msr(IA32_GS_BASE, (uint64_t)&cpu_info[cpu_id] + KERNEL_BASE);
}

volatile int smp_stress_active = 0;

// One round of the cross-CPU allocator hammer. This is how the spinlocks get
// PROVEN rather than merely written: without it the locks are never contended,
// because only the BSP ever allocates. Every core runs this concurrently — the
// APs from their idle loop, the BSP from cmd_smpstress — so alloc_page,
// free_page, kmalloc and kfree really are entered from four cores at once.
//
// Each operation is verified, and anything wrong lands in this core's own
// stress_bad counter (never a shared one, and never a printf — an AP has no
// business in either).
void smp_stress_iteration(cpu_info_t* me) {
    uint64_t tag = 0xA5A5A5A500000000ULL | (uint64_t)me->apic_id_self;

    void* p = alloc_page();
    if (!p) {
        me->stress_bad++;                  // 4 KB at a time, paired — never expected
    } else {
        // The failure this catches: the same frame handed to two cores at once.
        // Write our own tag through the identity map, hold it a moment, and it
        // must still be ours when we read it back.
        volatile uint64_t* q = (volatile uint64_t*)p;
        q[0] = tag;
        for (volatile int d = 0; d < 64; d++) { }
        if (q[0] != tag) me->stress_bad++;
        free_page(p);
    }

    void* k = kmalloc(96);
    if (!k) {
        me->stress_bad++;
    } else {
        // Same idea one layer up: a torn slab/heap freelist shows as a block
        // whose ends no longer hold what this core just wrote into them.
        volatile uint8_t* b = (volatile uint8_t*)k;
        uint8_t a = (uint8_t)me->apic_id_self, z = (uint8_t)~me->apic_id_self;
        b[0] = a; b[95] = z;
        for (volatile int d = 0; d < 64; d++) { }
        if (b[0] != a || b[95] != z) me->stress_bad++;
        kfree(k);
    }
    me->stress_ops++;
}

// Bring this AP's Local APIC online and start its periodic timer. Every register
// touched here is CPU-local, so none of it disturbs the BSP's LAPIC.
static void ap_lapic_init(void) {
    if (!lapic) return;

    // The MSR enable bit is per-CPU too, and an AP comes out of INIT with its
    // LAPIC hardware-disabled.
    uint64_t base = read_msr(IA32_APIC_BASE_MSR);
    if (!(base & APIC_BASE_ENABLE))
        write_msr(IA32_APIC_BASE_MSR, base | APIC_BASE_ENABLE);

    lapic[LAPIC_SVR / 4] = SVR_ENABLE | AP_SPURIOUS_VECTOR;   // software-enable
    lapic[LAPIC_TPR / 4] = 0;                                 // accept every priority

    // Mask the LVT sources we have no handler for. An unmasked LINT0/LINT1,
    // thermal or error interrupt would vector somewhere with no gate installed.
    lapic[LAPIC_LVT_THERMAL / 4] = LVT_MASKED;
    lapic[LAPIC_LVT_PERFMON / 4] = LVT_MASKED;
    lapic[LAPIC_LVT_LINT0 / 4]   = LVT_MASKED;
    lapic[LAPIC_LVT_LINT1 / 4]   = LVT_MASKED;
    lapic[LAPIC_LVT_ERROR / 4]   = LVT_MASKED;

    lapic[LAPIC_TIMER_DIV / 4] = LAPIC_TIMER_DIV16;
    lapic[LAPIC_LVT_TIMER / 4] = AP_TIMER_VECTOR | LVT_TIMER_PERIODIC;
    lapic[LAPIC_TIMER_INITCNT / 4] = AP_TIMER_INITCNT;        // writing this starts it
}

static int detect_aps_via_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    if (eax < 1) return 0;

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    int logical_count = (ebx >> 16) & 0xFF;
    int bsp_apic_id = (ebx >> 24) & 0xFF;

    if (logical_count <= 1) return 0;

    cpu_info[0].apic_id = (uint32_t)bsp_apic_id;
    cpu_info[0].cpu_number = 0;
    cpu_info[0].started = 1;
    cpu_info[0].running = 1;

    int ap_found = 0;

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0xB), "c"(0));
    if (eax && ecx) {
        int apic_ids[MAX_CPUS], apic_count = 0;
        for (int sub = 0; sub < 2; sub++) {
            __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0xB), "c"(sub));
            if (!eax && !ebx) break;
            if ((ecx & 0xFF) == 1) {
                int total = (ebx & 0xFFFF);
                if (total > MAX_CPUS) total = MAX_CPUS;
                for (int id = 0; id < total; id++) {
                    int dup = 0;
                    for (int j = 0; j < apic_count; j++)
                        if (apic_ids[j] == id) { dup = 1; break; }
                    if (!dup) apic_ids[apic_count++] = id;
                }
            }
        }
        if (apic_count > 1) {
            for (int idx = 1; idx < apic_count && idx < MAX_CPUS; idx++) {
                cpu_info[idx].apic_id = (uint32_t)apic_ids[idx];
                cpu_info[idx].cpu_number = idx;
                cpu_info[idx].started = 0;
                cpu_info[idx].running = 0;
            }
            return apic_count;
        }
    }

    ap_found = logical_count;
    if (ap_found > MAX_CPUS) ap_found = MAX_CPUS;
    for (int idx = 1; idx < ap_found; idx++) {
        cpu_info[idx].apic_id = (uint32_t)(bsp_apic_id + idx);
        cpu_info[idx].cpu_number = idx;
        cpu_info[idx].started = 0;
        cpu_info[idx].running = 0;
    }
    return ap_found > 1 ? ap_found : 0;
}

void smp_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_info[i].apic_id = 0xFF;
        cpu_info[i].apic_id_self = 0xFFFFFFFF;
        cpu_info[i].cpu_number = i;
        cpu_info[i].started = 0;
        cpu_info[i].running = 0;
        cpu_info[i].ticks = 0;
    }

    // The gates the APs will vector through must exist BEFORE any AP starts —
    // the first LAPIC timer can fire while we are still in this loop sending
    // SIPIs to the next core.
    idt_set_gate(AP_TIMER_VECTOR,    (uint64_t)ap_timer_stub    + KERNEL_BASE, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_VECTOR,     (uint64_t)tlb_ipi_stub     + KERNEL_BASE, 0x08, 0x8E);
    idt_set_gate(AP_SPURIOUS_VECTOR, (uint64_t)ap_spurious_stub + KERNEL_BASE, 0x08, 0x8E);

    // The BSP is always CPU 0 and online — record it up front so `cpus`/nyxfetch
    // see it even on a single-core machine (where detection returns early).
    cpu_info[0].apic_id = apic_get_id();
    cpu_info[0].apic_id_self = apic_get_id();
    cpu_info[0].cpu_number = 0;
    cpu_info[0].started = 1;
    cpu_info[0].running = 1;

    int ap_count = detect_aps_via_cpuid();
    if (ap_count <= 1) {
        printf("[SMP] Single CPU.\n");
        return;
    }

    printf("[SMP] %d AP(s) found via CPUID\n", ap_count - 1);

    uint32_t tramp_size = (uint32_t)(uint64_t)_binary_trampoline_bin_end -
                          (uint32_t)(uint64_t)_binary_trampoline_bin_start;
    if (tramp_size > 0x1000) tramp_size = 0x1000;
    __builtin_memcpy((void*)AP_TRAMPOLINE_ADDR, _binary_trampoline_bin_start, tramp_size);

    for (int i = 1; i < ap_count && i < MAX_CPUS; i++) {
        uint32_t apic_id = cpu_info[i].apic_id;
        if (apic_id == 0xFF) continue;

        void* stack = kmalloc(AP_STACK_SIZE);
        if (!stack) continue;
        cpu_info[i].stack_base = (uint64_t)stack;
        cpu_info[i].stack_top = (uint64_t)stack + AP_STACK_SIZE;

        extern uint64_t kernel_pml4_phys;
        uint32_t data_off = tramp_size - 32;
        volatile uint64_t* td_data = (volatile uint64_t*)(uintptr_t)(AP_TRAMPOLINE_ADDR + data_off);
        td_data[0] = kernel_pml4_phys;
        td_data[1] = cpu_info[i].stack_top;
        volatile uint32_t* cpu_id_ptr = (volatile uint32_t*)((uint8_t*)td_data + 16);
        cpu_id_ptr[0] = (uint32_t)i;
        volatile uint64_t* entry_ptr = (volatile uint64_t*)((uint8_t*)td_data + 24);
        entry_ptr[0] = (uint64_t)ap_main;

        if (!lapic) { printf("[SMP] No LAPIC, skipping AP %d\n", i); continue; }

        // Bring the AP up with the architectural INIT–SIPI sequence, reusing the
        // apic.c ICR helper (named ICR_* delivery-mode constants + a delivery-status
        // wait) rather than poking the LAPIC with magic numbers. The io_wait() delays
        // stay HERE, not inside apic_send_init_ipi(): under single-threaded TCG only an
        // outb (port 0x80) forces a VCPU exit that lets QEMU actually deliver the IPI
        // and run the AP, so the settle must be the io_wait kind and sit between steps.

        // INIT assert (level-triggered), then a ~10 ms settle.
        apic_send_ipi(apic_id, ICR_INIT | ICR_PHYSICAL | ICR_LEVEL | ICR_ASSERT);
        for (volatile int d = 0; d < 500; d++) io_wait();

        // INIT de-assert to complete the level-triggered message.
        apic_send_ipi(apic_id, ICR_INIT | ICR_PHYSICAL | ICR_LEVEL);
        for (volatile int d = 0; d < 500; d++) io_wait();

        // SIPI (vector = the trampoline's start page); poll for the AP to signal in.
        apic_send_ipi(apic_id, ICR_STARTUP | ICR_PHYSICAL | (AP_SIPI_VECTOR & 0xFF));
        for (int retry = 0; retry < 200 && !cpu_info[i].started; retry++) {
            for (volatile int d = 0; d < 50; d++) io_wait();
        }

        if (cpu_info[i].started) {
            cpu_count++;
            printf("[SMP] AP %d (APIC %d) started\n", i, apic_id);
        } else {
            printf("[SMP] AP %d (APIC %d) FAILED to start\n", i, apic_id);
        }
    }

    printf("[SMP] %d CPU(s) online\n", cpu_count);
}

void ap_main(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;

    // Proof-of-execution: read our OWN initial APIC id via CPUID(1).EBX[31:24].
    // CPUID is an instruction (no memory access), so it's safe here even though
    // the AP's page tables (the plain kernel PML4) don't map the LAPIC MMIO —
    // reading `lapic[...]` here #PF'd with no IDT set up → triple fault.
    uint32_t ceax, cebx, cecx, cedx;
    __asm__ volatile("cpuid" : "=a"(ceax), "=b"(cebx), "=c"(cecx), "=d"(cedx) : "a"(1));
    cpu_info[cpu_id].apic_id_self = (cebx >> 24) & 0xFF;

    cpu_info[cpu_id].started = 1;
    cpu_info[cpu_id].running = 1;

    // CR4 is per-CPU: match the BSP's SMEP/SMAP so every core enforces the same
    // ring-0 isolation (a no-op on CPUs that don't advertise them). The silent
    // variant — an AP must not call printf, which is shared and unlocked.
    cpu_apply_smep_smap();
    // CR0/CR4 FPU+SSE are also per-CPU: without this an AP would #UD on the
    // fxsave/fxrstor that sched_target()->fpu_switch() issues once a userspace
    // (SSE) task is scheduled onto it (#40).
    cpu_enable_sse_fpu();

    // ORDER MATTERS HERE. The trampoline left us on its own throwaway GDT and
    // with NO IDT at all, so until the next two calls any fault is a triple
    // fault — which is exactly how a stray LAPIC read used to kill the AP with
    // no diagnostic. Descriptor tables first, LAPIC second, `sti` last.
    gdt_load_on_ap();
    idt_load_on_ap();
    load_tss_for_cpu(cpu_id);      // our own TSS — the ring3 -> ring0 landing pad
    cpu_install_gs_base(cpu_id);   // after the trampoline's `mov gs, ax` zeroed the base
    setup_syscall_msrs();          // STAR/LSTAR/SF_MASK are per-CPU; ready for stage 3
    ap_lapic_init();

    // Kernel idle loop. This core now genuinely executes and services its own
    // timer interrupts, which is what a climbing cpu_info[].ticks proves. It
    // stays OUT of the scheduler on purpose: current_idx and saved_rsp/next_rsp
    // are still unsynchronised globals, so running processes here would corrupt
    // the BSP. That is the next stage.
    //
    // The ALLOCATORS, though, are locked as of v5.8.91 — so an AP may now enter
    // them, and smp_stress_active is what sends it in to prove the locks hold
    // under real contention.
    //
    // `sti; hlt` is the race-free idiom: sti's one-instruction shadow means an
    // interrupt can't slip in between enabling and halting. Halting also stops
    // this core from starving the BSP under single-threaded TCG.
    cpu_info_t* me = &cpu_info[cpu_id];
    for (;;) {
        if (smp_stress_active) smp_stress_iteration(me);
        else __asm__ volatile("sti; hlt");
    }
}
