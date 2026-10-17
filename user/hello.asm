; Minimal ELF test program for NyxOS x86_64
; Uses syscall instruction:
;   syscall 2: print string (rdi = string pointer)
;   syscall 0: exit (rdi = exit code)
BITS 64

global _start

section .text

_start:
    mov rax, 2          ; syscall: print string
    mov rdi, hello_msg  ; arg1: string pointer
    syscall

    mov rax, 2
    mov rdi, second_msg
    syscall

    mov rax, 0          ; syscall: exit
    mov rdi, 42         ; exit code
    syscall

.loop:
    hlt
    jmp .loop

section .data

hello_msg:  db "Hello from x86_64 ELF on NyxOS!", 10, 0
second_msg: db "This is ring 3 user mode!", 10, 0

section .note.GNU-stack noalloc noexec nowrite progbits
