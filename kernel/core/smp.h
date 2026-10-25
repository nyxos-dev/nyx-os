#ifndef SMP_H
#define SMP_H

#define MAX_CPUS 8
#define AP_STACK_SIZE 8192

// Interrupt vectors owned by the application processors. The LAPIC timer is a
// per-CPU device, so this vector only ever fires on the core it was programmed
// on — the BSP keeps its own timer masked and stays on the PIT's IRQ0.
#define AP_TIMER_VECTOR     0x40
#define IPI_TLB_VECTOR      0x41   // "invalidate this address on your core too"
#define AP_SPURIOUS_VECTOR  0xFF

// The per-CPU block. Anything a core may touch while another core runs belongs
// here, indexed by CPU, never in a kernel global. Today that is just the tick
// counter; the scheduler cursor and user_cr3/user_rsp move here when APs start
// running user processes.
typedef struct {
    /* ---- ABI-FIXED PREFIX ---------------------------------------------------
     * syscall_entry (isr_stubs.asm) reaches these four through GS, using the
     * CPU_* byte offsets defined there. Their positions are a contract with the
     * assembler, not an implementation detail: the compile-time check below
     * fails the build if anyone reorders or inserts ahead of them. New fields
     * go BELOW this block.
     *
     * They live per-CPU rather than per-process because syscall_entry has no
     * free register to find the current process with — swapgs is the only
     * register-free anchor the hardware offers. See kernel.h for what that does
     * and does not fix.
     * ------------------------------------------------------------------------ */
    uint64_t sc_user_rsp;   // +0   user RSP stashed on entry, reloaded for iretq
    uint64_t sc_kernel_rsp; // +8   kernel stack this CPU enters syscalls on
    uint64_t sc_user_cr3;   // +16  user CR3 saved on entry, restored on exit
    uint64_t sc_frame_ptr;  // +24  base of the saved user register frame
    uint64_t sc_saved_rsp;  // +32  RSP of the context this CPU just interrupted
    uint64_t sc_next_rsp;   // +40  RSP of the context this CPU will resume
    uint64_t sc_next_cr3;   // +48  CR3 to resume under (BSP only; APs stay kernel)

    uint32_t apic_id;       // APIC id the BSP assigned/expects for this CPU
    uint32_t apic_id_self;  // APIC id the CPU read from its OWN Local APIC (proof)
    uint32_t cpu_number;
    uint64_t stack_base;
    uint64_t stack_top;
    volatile int started;
    volatile int running;
    volatile uint64_t ticks; // THIS core's LAPIC-timer ticks (APs only; BSP uses tick_count)
    volatile uint64_t stress_ops;  // allocator-hammer iterations this core completed
    volatile uint64_t stress_bad;  // integrity failures this core observed
    volatile uint64_t work_ops;    // iterations this CPU's pinned worker thread ran

    // This CPU's scheduler cursor. sched_cur is the process_t it is currently
    // running, or NULL meaning "the idle context this core booted into" — whose
    // stack pointer is then parked in idle_rsp so we can get back to it. Typed
    // void* only to keep this header free of process.h ordering constraints.
    void*    sched_cur;
    uint64_t idle_rsp;
    int      sched_scan;           // rotating index for round-robin fairness

    // Preemption critical sections, PER CPU (v5.8.99). It was one global, which
    // was wrong in both directions once a second core scheduled: the BSP's
    // preempt_disable() meant nothing to an AP, and an AP raising it stopped the
    // BSP's scheduler outright. "Don't preempt me" is inherently a statement
    // about one core, so it belongs here.
    volatile int preempt_count;

    // #40 FPU/SSE context save: the process whose XMM/x87 state is currently live in
    // THIS core's registers (NULL = none yet, or the last owner was freed). sched_target's
    // fpu_switch() fxsave's it out and fxrstor's the incoming task in. void* to keep this
    // header free of process.h ordering constraints; only ever set to a process_t*.
    void*    fpu_owner;
    // Written by IRQ stubs AFTER switching RSP and CR3 (also initialized by the
    // explicit boot-stack adoption). sched_cur can name the incoming task earlier.
    void*    sched_active;
} cpu_info_t;

/* Pin the assembler contract. If this line fails to compile, someone moved a
 * field in the ABI-fixed prefix and the CPU_* offsets in isr_stubs.asm no longer
 * match — which would silently corrupt every syscall rather than fault. */
typedef char cpu_abi_prefix_offsets_are_pinned[
    (__builtin_offsetof(cpu_info_t, sc_user_rsp)   ==  0 &&
     __builtin_offsetof(cpu_info_t, sc_kernel_rsp) ==  8 &&
     __builtin_offsetof(cpu_info_t, sc_user_cr3)   == 16 &&
     __builtin_offsetof(cpu_info_t, sc_frame_ptr)  == 24 &&
     __builtin_offsetof(cpu_info_t, sc_saved_rsp)  == 32 &&
     __builtin_offsetof(cpu_info_t, sc_next_rsp)   == 40 &&
     __builtin_offsetof(cpu_info_t, sc_next_cr3)   == 48) ? 1 : -1];

typedef char cpu_scheduler_handoff_offsets_are_pinned[
    (__builtin_offsetof(cpu_info_t, sched_cur) == 128 &&
     __builtin_offsetof(cpu_info_t, sched_active) == 160) ? 1 : -1];

extern cpu_info_t cpu_info[MAX_CPUS];
extern uint32_t cpu_count;

/* Point this CPU's IA32_KERNEL_GS_BASE at its own block, so `swapgs` at syscall
 * entry lands on it. Called by the BSP and by every AP. */
void cpu_install_gs_base(uint32_t cpu_id);

void smp_init(void);
void ap_main(uint32_t cpu_id);
cpu_info_t* cpu_self(void);
void ap_timer_tick(void);

// Cross-CPU allocator stress. `smp_stress_active` is what pulls the APs out of
// their idle loop and into smp_stress_iteration(); the BSP runs the same
// iteration itself, so every core contends for the same locks.
extern volatile int smp_stress_active;
void smp_stress_iteration(cpu_info_t* me);

// Per-CPU scheduling of kernel threads pinned to an AP (v5.8.92). Called from
// ap_timer_stub, which is a real context-switching ISR for these cores.
void ap_scheduler_tick(void);
void smp_start_ap_threads(void);
extern volatile int smp_work_active;
extern volatile int smp_user_balance;   // spread new user processes over the APs

// TLB shootdown. Read the deadlock note in smp.c before adding a caller.
void tlb_shootdown(uint64_t vaddr);
void tlb_shootdown_ipi(void);
extern uint64_t tlb_shootdowns;

// tlbtest rendezvous (see cmd_tlbtest): CPU 1 reads tlb_test_va on request.
extern volatile int      tlb_test_req;
extern volatile int      tlb_test_ack;
extern volatile uint64_t tlb_test_va;
extern volatile uint64_t tlb_test_v1, tlb_test_v2, tlb_test_v3;

#endif
