; spin.asm - preemptive ring-3 demo for NyxOS x86_64
; Burns CPU in a tight ring-3 loop (so on a non-preemptive kernel it would freeze
; the whole system), printing a heartbeat each pass, then exits cleanly. Launched
; by the `usertest` shell command via spawn_user_path(), which marks it
; sched_managed so the timer round-robins it against the GUI and the other copy.
;   syscall 2 (SYS_PRINT): rdi = NUL-terminated string
;   syscall 0 (SYS_EXIT):  rdi = exit code
BITS 64

global _start

section .text
_start:
    mov r12, 10                 ; heartbeats before exiting (long enough to `kill` mid-run)

.outer:
    mov rax, 2                  ; SYS_PRINT
    mov rdi, msg
    syscall

    mov rcx, 0x6000000          ; ring-3 busy work between heartbeats
.spin:
    dec rcx
    jnz .spin

    dec r12
    jnz .outer

    mov rax, 0                  ; SYS_EXIT
    xor rdi, rdi                ; exit code 0
    syscall

.hang:                          ; unreachable — exit does not return
    hlt
    jmp .hang

section .data
msg: db "[spin] ring-3 process alive (preempted; GUI keeps running)", 10, 0

section .note.GNU-stack noalloc noexec nowrite progbits
