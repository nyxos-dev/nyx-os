; isr_stubs.asm - x86_64 interrupt stubs for NyxOS
default rel
BITS 64

; Higher-half kernel offset for accessing variables from user page tables
%define KERNEL_BASE 0xFFFFFF8000000000

extern kernel_pml4_phys
extern irq_eoi

; Byte offsets into cpu_info_t's ABI-fixed prefix (smp.h). Repeated near
; syscall_entry, where the full rationale for reaching them through GS lives.
%define CPU_USER_RSP    0
%define CPU_KERNEL_RSP  8
%define CPU_USER_CR3    16
%define CPU_FRAME_PTR   24
%define CPU_SAVED_RSP   32
%define CPU_NEXT_RSP    40
%define CPU_NEXT_CR3    48
%define CPU_SCHED_CUR   128
%define CPU_SCHED_ACTIVE 160

; Save all registers (15 pushes).
;
; The `cld` is a SECURITY requirement, not tidiness. RFLAGS.DF is part of ring 3's
; state and survives into the handler: a user program can execute `std` and then
; simply wait for a timer interrupt. Every rep-string in the kernel would then run
; BACKWARDS — and memcpy_asm/memset_asm in stubs.asm are exactly `rep movsb` /
; `rep stosb`, so essentially every copy and clear in the kernel would walk down
; through memory instead of up. The SysV ABI also requires DF clear on entry to
; any C function, so compiler-emitted string ops assume it too.
;
; The `syscall` path is already covered by IA32_FMASK (syscall.c clears IF and
; DF); interrupts and exceptions were not covered by anything. Putting it in this
; macro covers every entry path that saves state — irq_common, isr_common and
; ap_timer_stub — in one place, so a new stub cannot forget it.
%macro SAVE_REGS 0
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

; ISR with no error code
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

extern isr_handler
isr_common:
    ; Save interrupted registers FIRST — the CR3 switch below clobbers rax, and a
    ; handler that returns (e.g. demand paging) would otherwise resume with a
    ; corrupted rax (same class of bug as the old irq_common). Exceptions that
    ; panic don't care, but this keeps the path correct for recoverable faults.
    SAVE_REGS
    ; Remember the FAULTING CR3. r15 is callee-saved, so the C isr_handler
    ; preserves it across the call. A recoverable fault (demand paging / COW)
    ; must iretq back on the address space that faulted — for a ring-3 fault
    ; that's the process's own CR3, not the kernel CR3 we switch to just below.
    ; Restored right before RESTORE_REGS/iretq (RESTORE_REGS then reloads the
    ; user's saved r15 off the stack).
    mov r15, cr3
    ; Switch to kernel page tables in case we came from user mode (rax is now
    ; saved scratch, restored by RESTORE_REGS).
    mov rax, KERNEL_BASE + kernel_pml4_phys
    mov rax, [rax]
    test rax, rax
    jz .isr_bad
    test rax, 0xFFF
    jnz .isr_bad
    mov cr3, rax
    jmp .isr_done
.isr_bad:
    mov dx, 0x3FD
.w1: in al, dx; test al, 0x20; jz .w1
    mov dx, 0x3F8; mov al, 'I'; out dx, al
    mov dx, 0x3FD
.w2: in al, dx; test al, 0x20; jz .w2
    mov dx, 0x3F8; mov al, 'C'; out dx, al
    mov dx, 0x3FD
.w3: in al, dx; test al, 0x20; jz .w3
    mov dx, 0x3F8; mov al, '3'; out dx, al
.isr_halt: cli; hlt; jmp .isr_halt
.isr_done:
    ; Stack: [RSP+0]=R15, ..., [RSP+112]=RAX
    ; After SAVE_REGS: [RSP+120]=int_no, [RSP+128]=error, [RSP+136]=RIP, [RSP+144]=CS, [RSP+152]=RFLAGS, ...
    mov rdi, [rsp + 120]     ; int_no
    mov rsi, [rsp + 136]     ; faulting RIP
    mov rdx, [rsp + 128]     ; error code
    mov rcx, [rsp + 144]     ; CS (ring is CS & 3)
    mov r8, rsp              ; 5th arg: exception frame base (GPRs..int/err/RIP/CS/RFLAGS/RSP/SS)
    ; 6th arg: the CR3 THAT FAULTED. The handler cannot read this for itself —
    ; we switched to the kernel's tables above, so a read_cr3() in C reports the
    ; KERNEL's CR3 on every single ring-3 fault, unconditionally. A diagnostic
    ; built on that measures the handler, not the faulting task, and will happily
    ; report "wrong address space" for a perfectly ordinary user bug. It did:
    ; five P0.1 hypotheses were chased against that artefact.
    mov r9, r15
    call isr_handler         ; may rewrite the frame to divert a ring-3 fault into a signal handler
    mov cr3, r15             ; restore the faulting address space before returning
    RESTORE_REGS
    add rsp, 16               ; pop error code + int number
    iretq

; Syscall entry via syscall instruction
; RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args (Linux convention)
; RCX = return RIP (set by syscall), R11 = saved RFLAGS (set by syscall)
global syscall_entry
extern syscall_handler
extern signal_dispatch

; The CPU_* offsets (defined at the top of this file) name slots in cpu_info_t's
; ABI-fixed prefix. They used to be plain globals right here — one set for the
; whole machine, which is fine on one CPU and catastrophic the moment a second
; core takes a syscall: both would stash their user CR3 and RSP over each other.
; A compile-time check in smp.h fails the build if the offsets ever drift.
;
;   CPU_FRAME_PTR = base of the saved user register frame on the kernel stack:
;   [+0..112]=GPRs (r15..rax), [+120]=RFLAGS, [+128]=RIP. do_fork() reads it to
;   clone the caller's ring-3 context into the child (with rax=0 for the child).

section .text
; Entry: rax=syscall#, rdi/rsi/rdx/r10/r8/r9=args, rcx=user RIP, r11=user RFLAGS,
; rsp=USER stack, CR3=USER. Interrupts are masked (SF_MASK clears IF).
; LSTAR is the higher-half alias of this label, so RIP is in the higher half and
; every `default rel` memory access resolves to the higher-half alias of our
; .data — which the user CR3 maps via PML4[511]. That lets us stash RSP and save
; all user GPRs onto the kernel stack with NO scratch register, before touching
; CR3. (The previous version clobbered RAX/RBX during the CR3 switch *before*
; SAVE_REGS, corrupting the syscall number and the user's RBX/RSP.)
syscall_entry:
    ; GS is how we reach per-CPU state with NO free register — the one thing the
    ; hardware gives us here. Each core's IA32_GS_BASE holds the HIGHER-HALF alias
    ; of its own cpu_info slot (cpu_install_gs_base), so these accesses resolve
    ; under the user CR3 too, through the PML4[511] mirror.
    ;
    ; NO swapgs, deliberately. The textbook discipline is to swap GS on every
    ; ring transition, and it is wrong for this kernel: irq_common enters from a
    ; process in ring 0 and can iretq into a DIFFERENT process in ring 3, so the
    ; swaps would not pair — the next `syscall` would then compute gs:0 and write
    ; to address 0. Instead GS_BASE simply stays pointed at the per-CPU block in
    ; both rings. That is safe here precisely because NyxOS gives userspace no GS
    ; base of its own (no TLS, no arch_prctl, CR4.FSGSBASE off), and ring 3 still
    ; cannot read the block: the linear address it computes lands on a supervisor
    ; page and faults.
    ;
    ; IF USERSPACE EVER GETS A GS BASE (TLS), this must become a real swapgs —
    ; and then irq_common and isr_common need the paired swaps too.
    mov [gs:CPU_USER_RSP], rsp   ; stash user RSP
    mov rsp, [gs:CPU_KERNEL_RSP] ; switch to this CPU's kernel stack
    push rcx                     ; user return RIP
    push r11                     ; user RFLAGS
    SAVE_REGS                    ; save all 15 user GPRs intact — nothing clobbered yet

    ; All user state is now on the (higher-half) kernel stack; rax/rbx are free.
    mov rax, cr3
    mov [gs:CPU_USER_CR3], rax   ; save user CR3
    mov rax, [kernel_pml4_phys]
    mov cr3, rax                 ; -> kernel CR3 (higher-half stack still mapped via 511)

    ; Marshal saved user regs into SysV C argument registers.
    ; Stack: [RSP+0..112]=GPRs (r15..rax), [RSP+120]=RFLAGS, [RSP+128]=RIP
    mov rdi, [rsp + 112]         ; RAX = syscall number
    mov rsi, [rsp + 72]          ; RDI = arg1
    mov rdx, [rsp + 80]          ; RSI = arg2
    mov rcx, [rsp + 88]          ; RDX = arg3
    mov r8,  [rsp + 40]          ; R10 = arg4
    mov r9,  [rsp + 56]          ; R8  = arg5
    mov [gs:CPU_FRAME_PTR], rsp  ; expose the saved user frame to do_fork() (frame base)
    ; arg6 (user R9, at frame+48) is the 7th integer arg -> passed on the stack per
    ; SysV. Push it so the callee finds it just above the return address, then pop it
    ; back after the call so rsp is the frame base again for the RAX-slot write below.
    ; (Alignment isn't required: the kernel is built -mno-sse, so no movaps.)
    push qword [rsp + 48]        ; arg6
    call syscall_handler
    add rsp, 8                   ; drop arg6 -> rsp back at the frame base
    mov [rsp + 112], rax         ; return value -> saved RAX slot

    ; Deliver a pending signal (still on the kernel CR3; user_cr3 valid for the user
    ; stack write). May rewrite the frame (RIP->handler, RDI->signo) + user_rsp, or
    ; not return at all for a default-terminate signal. rsp = the saved user frame.
    mov rdi, rsp
    call signal_dispatch

    ; ATOMIC RETURN WINDOW — this cli is the fix for P0.1, and it is load-bearing.
    ;
    ; Below, we switch CR3 to the user space and THEN run ~20 instructions before
    ; the iretq that actually drops to ring 3. Across that window the task is still
    ; CPL 0 (kernel code) but already on the USER CR3. If it is preempted there, the
    ; scheduler saves a frame whose CS says ring 0 (0x08 — true, we ARE in ring 0),
    ; and irq_scheduler_tick's rule "a ring-0 frame resumes on the kernel CR3, since
    ; the syscall switches to user itself before its iretq" then resumes us on the
    ; KERNEL CR3 — but we have ALREADY done our switch and will not repeat it. The
    ; `push 0x1B / iretq` below then returns to ring 3 on the kernel CR3, and the
    ; first user instruction fetch faults (CR2 == RIP). That is P0.1 exactly:
    ; intermittent, always in `threads`, load-dependent, recoverable.
    ;
    ; Why the window is reachable at all: syscall ENTRY masks IF via SF_MASK, so a
    ; simple syscall runs cli'd from entry to iretq and cannot be preempted here.
    ; But BLOCKING syscalls sti to yield (futex_wait, do_waitpid, poll, pipe read)
    ; and return with IF=1 — so the exit path runs preemptibly. `threads`
    ; synchronises on futex constantly, which is why the fault is always in it.
    ;
    ; cli makes the CR3-switch..iretq window non-preemptible; iretq then restores
    ; the user RFLAGS (IF=1) atomically as it enters ring 3. One instruction, and it
    ; covers every syscall regardless of what it left IF as. The ENTRY side needs no
    ; equivalent: its pre-switch window already runs with IF=0 from SF_MASK.
    cli

    ; Return: back to user CR3 first (higher-half stack stays mapped), then restore.
    mov rax, [gs:CPU_USER_CR3]
    mov cr3, rax
    RESTORE_REGS                 ; restore user GPRs (RAX = return value)
    pop r11                      ; user RFLAGS
    pop rcx                      ; user RIP
    ; Return to ring 3 via iretq (NASM's bare `sysret` is the 32-bit form — it
    ; drops to compat mode and truncates RSP to 32 bits; iretq is how the scheduler
    ; also enters ring 3, with no STAR/GDT selector-ordering constraints). Build the
    ; iret frame: SS, RSP, RFLAGS, CS, RIP.
    push 0x23                    ; user SS (USER_DS, RPL 3)
    push qword [gs:CPU_USER_RSP] ; user RSP
    push r11                     ; RFLAGS
    push 0x1B                    ; user CS (USER_CS, RPL 3)
    push rcx                     ; user RIP
    iretq                        ; (no swapgs — GS_BASE stays per-CPU; see entry)

; IRQ stubs (mapped to INT 32-47)
%macro IRQ 2
global irq%1
irq%1:
    push 0
    push %2
    jmp irq_common
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

extern irq_handler
extern irq_scheduler_tick

irq_common:
    ; Save ALL interrupted registers FIRST, before touching rax/rbx. The CR3
    ; switch below used rax/rbx as scratch *before* SAVE_REGS, so on return the
    ; interrupted code got a corrupted RBX (= KERNEL_BASE) / RAX and, if it then
    ; computed a call/jump from them, jumped to KERNEL_BASE+3 → #UD. This was the
    ; intermittent GUI/keyboard crash and the timer/scheduler blocker.
    SAVE_REGS
    ; Switch to kernel page tables for C code. rax/rbx are now free scratch — they
    ; are saved on the stack and restored by RESTORE_REGS before iretq.
    mov rax, kernel_pml4_phys
    mov rbx, KERNEL_BASE
    add rax, rbx
    mov rax, [rax]
    test rax, rax
    jz .irq_bad
    test rax, 0xFFF
    jnz .irq_bad
    mov cr3, rax
    jmp .irq_done
.irq_bad:
    mov dx, 0x3FD
.w4: in al, dx; test al, 0x20; jz .w4
    mov dx, 0x3F8; mov al, 'K'; out dx, al
    mov dx, 0x3FD
.w5: in al, dx; test al, 0x20; jz .w5
    mov dx, 0x3F8; mov al, '3'; out dx, al
.irq_halt: cli; hlt; jmp .irq_halt
.irq_done:
    ; Stack: [RSP+0]=R15..[RSP+112]=RAX, [RSP+120]=int_no, [RSP+128]=error, [RSP+136]=RIP
    mov rdi, [rsp + 120]     ; int_no
    call irq_handler
    ; Send EOI (handles APIC or legacy PIC)
    mov rdi, [rsp + 120]     ; int_no
    call irq_eoi
    ; Scheduler hand-off. These three slots are PER-CPU (v5.8.92) — one shared set
    ; described a single machine-wide switch, which is exactly wrong once a second
    ; core schedules. GS already holds this core's block, and its base is the
    ; higher-half alias, so these resolve under a user CR3 too via PML4[511] —
    ; which is what the old `mov rax, sym / add KERNEL_BASE` dance was doing by hand.
    mov [gs:CPU_SAVED_RSP], rsp
    call irq_scheduler_tick
    ; Set RSP before switching CR3 — ensures a valid stack during the CR3 switch
    mov rsp, [gs:CPU_NEXT_RSP]
    ; Validate next_cr3 before switching
    mov rax, [gs:CPU_NEXT_CR3]
    test rax, rax
    jz .bad_cr3
    test rax, 0xFFF
    jnz .bad_cr3
    mov cr3, rax
    jmp .restore_done
.bad_cr3:
    mov dx, 0x3FD
.b1: in al, dx; test al, 0x20; jz .b1
    mov dx, 0x3F8; mov al, 'B'; out dx, al
    mov dx, 0x3FD
.b2: in al, dx; test al, 0x20; jz .b2
    mov dx, 0x3F8; mov al, '3'; out dx, al
.b3: cli; hlt; jmp .b3
.restore_done:
    ; Retire the outgoing stack only after both stack and address-space handoff.
    mov rax, [gs:CPU_SCHED_CUR]
    mov [gs:CPU_SCHED_ACTIVE], rax
    RESTORE_REGS
    add rsp, 16
    iretq

; ---------------------------------------------------------------------------
; Application-processor LAPIC timer (vector AP_TIMER_VECTOR).
;
; As of v5.8.92 this is a REAL context-switching ISR, not just a tick counter:
; it saves the full register set, hands its RSP to this core's own scheduler,
; and resumes whatever that scheduler chose. Each core's saved/next slots live
; in its own cpu_info block, so two cores describe two independent switches.
;
; Still deliberately NOT irq_common. That path does CR3 work and drives the
; BSP's process table; an AP here runs only kernel threads, which all share the
; kernel address space — so there is no CR3 to switch and no ring to cross.
; Keeping the two paths apart is what lets APs schedule without touching any of
; the delicate ring-3 machinery.
;
; The pushed frame matches init_task_stack()'s layout exactly (error, int_no,
; then 15 GPRs), which is what makes a thread built there resumable here.
; ---------------------------------------------------------------------------
extern ap_scheduler_tick

global ap_timer_stub
ap_timer_stub:
    push 0                       ; error code (none for this vector)
    push 0x40                    ; int_no = AP_TIMER_VECTOR
    SAVE_REGS
    ; As of v5.8.93 an AP can be running a USER process, so it may have been
    ; interrupted on a user CR3 — under which the kernel's low, -mcmodel=large
    ; code is not mapped at all. Switch to the kernel tables before any call,
    ; exactly as irq_common does. (The stack we are on is either a kernel
    ; thread's, or the process's kernel stack via TSS RSP0 — a higher-half
    ; alias, mapped in both address spaces.)
    mov rax, kernel_pml4_phys
    mov rbx, KERNEL_BASE
    add rax, rbx
    mov rax, [rax]
    mov cr3, rax
    mov [gs:CPU_SAVED_RSP], rsp  ; where the interrupted task resumes
    call ap_scheduler_tick       ; counts the tick, EOIs, picks what runs next
    mov rsp, [gs:CPU_NEXT_RSP]   ; set RSP before CR3, so the stack stays valid
    mov rax, [gs:CPU_NEXT_CR3]
    test rax, rax
    jz .ap_bad_cr3
    test rax, 0xFFF
    jnz .ap_bad_cr3
    mov cr3, rax
    mov rax, [gs:CPU_SCHED_CUR]
    mov [gs:CPU_SCHED_ACTIVE], rax
    RESTORE_REGS
    add rsp, 16                  ; drop int_no + error
    iretq
.ap_bad_cr3:
    cli
    hlt
    jmp .ap_bad_cr3

; ---------------------------------------------------------------------------
; TLB shootdown IPI (vector IPI_TLB_VECTOR).
;
; Arrives on any core, at any moment, including one running a USER process — so
; it must switch to the kernel tables before calling C (whose low
; -mcmodel=large code is unmapped under a user CR3) and put the interrupted
; CR3 back before returning. The old CR3 rides on the stack, and the 11 pushes
; leave RSP 16-byte aligned at the call, as the ABI requires.
;
; No context switch: this handler must be short and must not touch the
; scheduler. It invalidates one address, acknowledges, and leaves.
; ---------------------------------------------------------------------------
extern tlb_shootdown_ipi

global tlb_ipi_stub
tlb_ipi_stub:
    cld                          ; see SAVE_REGS: ring 3 may have left DF set
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbx                     ; RBX is callee-saved and clobbered just below
    mov rax, cr3
    push rax                     ; interrupted CR3 (11 pushes -> RSP 16-aligned)
    mov rax, kernel_pml4_phys
    mov rbx, KERNEL_BASE
    add rax, rbx
    mov rax, [rax]
    mov cr3, rax
    call tlb_shootdown_ipi
    pop rax                      ; interrupted CR3
    mov cr3, rax
    pop rbx
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

; Spurious LAPIC interrupt. It takes no EOI by design — the only job here is to
; exist, so a spurious vector can't fault an AP into a triple fault.
global ap_spurious_stub
ap_spurious_stub:
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
