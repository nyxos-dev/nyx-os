; switch.asm - x86_64 context switch for NyxOS
default rel
BITS 64

global switch_context
global create_task_stack

; (ku_setjmp/ku_longjmp removed in v5.7.9 with the setjmp/longjmp blocking-exec
; path — see process.c. switch_context/create_task_stack are kept below.)

; switch_context(uint64_t* old_rsp_ptr, uint64_t new_rsp)
; RDI = old_rsp_ptr, RSI = new_rsp (x86_64 ABI)
; CURRENTLY UNUSED — the preemptive scheduler switches through irq_common's
; saved_rsp/next_rsp instead. Kept because create_task_stack's frame layout is
; the reference the process.c stack builders are written against.
;
; It also USED TO BE BROKEN: it pushed 14 registers and popped 15, so resuming a
; context this function had saved consumed the return address as if it were RAX
; and `ret` jumped to whatever sat above it. It only ever appeared to work
; because the one frame it was fed came from create_task_stack, which lays out
; 15 slots. The `push rax` below restores the symmetry; do not remove it without
; removing a pop as well.
switch_context:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi           ; saved RSI = new_rsp
    push rdi           ; saved RDI = old_rsp_ptr
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    ; Pushed rax..r15, so from RSP: r15=+0 r14=+8 r13=+16 r12=+24 r11=+32
    ; r10=+40 r9=+48 r8=+56 rbp=+64 rdi=+72 rsi=+80 rdx=+88 rcx=+96 rbx=+104
    ; rax=+112. The saved ARGS are rdi (old_rsp_ptr) and rsi (new_rsp).
    ; These offsets were +96/+104, which are rcx and rbx — so this function
    ; read the wrong two registers as its arguments on top of the push/pop
    ; imbalance. Both are fixed; it remains unused.
    mov rax, [rsp + 72]        ; saved RDI = old_rsp_ptr
    mov [rax], rsp             ; *old_rsp_ptr = current RSP
    mov rsp, [rsp + 80]        ; saved RSI = new_rsp
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
    ret

; create_task_stack(uint64_t stack_top, uint64_t entry_point)
; RDI = stack_top, RSI = entry_point
create_task_stack:
    mov rax, rdi                ; stack_top
    mov rcx, rsi                ; entry_point
    mov [rax - 8], rcx          ; return address = entry_point
    mov qword [rax - 16], 0     ; r15
    mov qword [rax - 24], 0     ; r14
    mov qword [rax - 32], 0     ; r13
    mov qword [rax - 40], 0     ; r12
    mov qword [rax - 48], 0     ; r11
    mov qword [rax - 56], 0     ; r10
    mov qword [rax - 64], 0     ; r9
    mov qword [rax - 72], 0     ; r8
    mov qword [rax - 80], 0     ; rbp
    mov qword [rax - 88], 0     ; rdi
    mov qword [rax - 96], 0     ; rsi
    mov qword [rax - 104], 0    ; rdx
    mov qword [rax - 112], 0    ; rcx
    mov qword [rax - 120], 0    ; rbx
    mov qword [rax - 128], 0    ; rax
    lea rax, [rax - 128]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
