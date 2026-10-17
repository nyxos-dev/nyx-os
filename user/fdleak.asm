; fdleak.asm - per-process fd demo for NyxOS x86_64
; Opens a file and exits WITHOUT closing it, to show that the kernel force-closes
; a process's leaked fds when it's reaped (close_proc_fds), and that each process
; gets its own fd table (so running this repeatedly never exhausts fds).
;   syscall 3 (SYS_OPEN):  rdi=path, rsi=flags, rdx=mode  -> rax=fd
;   syscall 2 (SYS_PRINT): rdi=NUL-terminated string
;   syscall 0 (SYS_EXIT):  rdi=exit code
BITS 64

global _start

section .text
_start:
    mov rax, 3                  ; SYS_OPEN
    mov rdi, path
    xor rsi, rsi                ; flags = 0 (read)
    xor rdx, rdx                ; mode
    syscall                     ; rax = fd (we intentionally leak it)

    mov rax, 2                  ; SYS_PRINT
    mov rdi, msg
    syscall

    mov rax, 0                  ; SYS_EXIT — note: no SYS_CLOSE first
    xor rdi, rdi
    syscall

.hang:                          ; unreachable
    hlt
    jmp .hang

section .data
path: db "/init.elf", 0
msg:  db "[fdleak] opened /init.elf and exiting WITHOUT closing it", 10, 0

section .note.GNU-stack noalloc noexec nowrite progbits
