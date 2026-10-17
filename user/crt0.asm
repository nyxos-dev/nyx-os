; CRT0 for NyxOS x86_64 user programs
; Calls main(argc, argv) then exit()
BITS 64

global _start
extern main
extern environ

section .text
_start:
    ; SysV-style entry stack, built by the kernel for every user launch:
    ;   [rsp]   = argc
    ;   [rsp+8] = argv[0..argc-1], NULL, envp[0..], NULL
    ; elf_load_image writes an empty frame (argc=0, both NULLs) for plain spawns;
    ; execve() builds the real one from the caller's argv + envp.
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp+8]        ; argv
    ; envp = &argv[argc+1] = rsp + 8 + (argc+1)*8  (past argv's NULL terminator)
    lea rdx, [rdi+1]        ; argc + 1
    lea rdx, [rsp + rdx*8 + 8]  ; -> envp
    mov [abs environ], rdx  ; publish it as the libc `environ` global (getenv reads it); `abs` = explicit absolute (silences NASM's implicit-ABS deprecation; user binaries are fixed-VA / non-PIE)
    call main              ; main(argc, argv, envp) — extra arg ignored by 2-arg mains

    ; exit(rax)
    mov rdi, rax
    mov rax, 0      ; SYS_EXIT
    syscall
    hlt

; Signal-return trampoline. The kernel (signal_dispatch) pushes this address as the
; handler's return address; when the handler `ret`s, control lands here and
; SYS_SIGRETURN restores the interrupted context — so this does not return. The
; libc signal() wrapper hands the kernel this address as the sa_restorer.
global __sigreturn
__sigreturn:
    mov rax, 18     ; SYS_SIGRETURN
    syscall
    hlt             ; unreachable: the kernel iretq's back to the interrupted RIP

section .note.GNU-stack noalloc noexec nowrite progbits
