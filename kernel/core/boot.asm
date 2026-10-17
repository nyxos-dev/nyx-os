; NyxOS boot.asm - Multiboot2 → Long Mode entry (x86_64)
BITS 32
DEFAULT ABS          ; make the (already-default) absolute addressing explicit

section .multiboot
align 8
; Multiboot v1 header (kept for compatibility, but v2 is primary)
    dd 0x1BADB002
    dd 0x00000003
    dd -(0x1BADB002 + 3)
    times 5 dd 0

align 8
; Multiboot v2 header
mb2_start:
    dd 0xE85250D6          ; magic
    dd 0                   ; architecture (0 = i386)
    dd mb2_end - mb2_start ; header length
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start)) ; checksum
    ; Framebuffer request tag (type 5) — asks the bootloader (GRUB) to set a linear
    ; graphics mode and pass its address/pitch back via the type-8 tag. This is what
    ; makes NyxOS work on REAL hardware + UEFI (GOP): no more poking the QEMU-only
    ; Bochs VBE. 1024x768x32 has a clean pitch (4096 = width*4) on essentially all HW.
    align 8
    dw 5                   ; type = framebuffer
    dw 0                   ; flags (0 = required)
    dd 20                  ; size
    dd 1024                ; width
    dd 768                 ; height
    dd 32                  ; depth
    ; End tag
    align 8
    dw 0                   ; type
    dw 0                   ; flags
    dd 8                   ; size
mb2_end:

section .bss
align 4096
; Kernel stack FIRST, and large: the GUI compositor runs on it with deep redraw
; call chains plus interrupt frames. It was 16 KB and sat directly below the page
; tables, so an overflow silently corrupted them (→ crashes at bad addresses).
; 128 KB + the page tables placed *after* it keeps overflow away from the tables.
stack_bottom:
    resb 131072           ; 128 KB kernel stack
stack_top:
align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table:
    resb 8192             ; 2 PDs for 128 MB (64 2MB-pages per PD)

section .text
global _start
extern kernel_main

_start:
    cli
    mov [boot_magic], eax
    mov [boot_info], ebx

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Clear page tables
    mov edi, pml4_table
    xor eax, eax
    mov ecx, 1024
    rep stosd
    mov edi, pdpt_table
    mov ecx, 1024
    rep stosd
    mov edi, pd_table
    mov ecx, 2048          ; 2 PDs
    rep stosd

    ; PML4[0] → PDPT
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax

    ; PDPT[0] → PD
    mov eax, pd_table
    or eax, 3
    mov [pdpt_table], eax

    ; Mirror PML4[0] to PML4[511] for higher-half kernel mapping
    mov eax, [pml4_table]
    mov [pml4_table + 511*8], eax

    ; Identity map 0–128 MB using 2 MB pages
    mov edi, pd_table
    mov ecx, 64
    mov eax, 0x83
.map_loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    dec ecx
    jnz .map_loop

    ; Load CR3
    mov eax, pml4_table
    mov cr3, eax

    ; Enable Long Mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Load 64-bit GDT and far jump to 64-bit code
    lgdt [gdtr64]
    jmp 0x08:.long_mode

BITS 64
.long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top
    cld
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    mov edi, [boot_magic]
    mov esi, [boot_info]
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .data
boot_magic: dd 0
boot_info:  dd 0

align 16
gdtr64:
    dw gdt64_end - gdt64 - 1
    dq gdt64

align 16
gdt64:
    dq 0x0000000000000000 ; null
    dq 0x00209A0000000000 ; kernel code 64
    dq 0x0000920000000000 ; kernel data
    dq 0x0020FA0000000000 ; user code 64
    dq 0x0000F20000000000 ; user data
gdt64_end:

section .note.GNU-stack noalloc noexec nowrite progbits
