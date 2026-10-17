; stubs.asm - NyxOS x86_64 low-level assembly
default rel
BITS 64

global memcpy_asm, memset_asm, memset32_asm
global _gdt_flush, _idt_flush
global read_cr0, write_cr0, read_cr2, read_cr3, write_cr3, flush_tlb, invlpg
global enable_interrupts, disable_interrupts

; void memcpy_asm(void* dest, const void* src, size_t n)
memcpy_asm:
    mov rcx, rdx
    rep movsb
    ret

; void memset_asm(void* dest, uint8_t val, size_t n)
memset_asm:
    mov rcx, rdx
    mov al, sil
    rep stosb
    ret

; void memset32_asm(void* dest, uint32_t val, size_t count)
; Fill `count` 32-bit words at dest with val (rep stosd). The framebuffer fill
; primitive: a solid-colour rectangle is a run of identical pixels, and one
; `rep stosd` per row beats the scalar per-pixel store loop ~2x (measured).
memset32_asm:
    mov rcx, rdx        ; count (dwords)
    mov eax, esi        ; val
    rep stosd
    ret

; void _gdt_flush(uint64_t gdt_ptr_addr)
_gdt_flush:
    lgdt [rdi]
    ; Far return to reload CS via iretq
    push 0x10             ; SS = kernel data
    push rsp              ; RSP
    pushfq                ; RFLAGS
    push 0x08             ; CS = kernel code 64
    lea rax, [rel .flush]
    push rax              ; RIP
    iretq
.flush:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; void _idt_flush(uint64_t idt_ptr_addr)
_idt_flush:
    lidt [rdi]
    ret

enable_interrupts:
    sti
    ret

disable_interrupts:
    cli
    ret

read_cr0:
    mov rax, cr0
    ret

write_cr0:
    mov cr0, rdi
    ret

read_cr2:
    mov rax, cr2
    ret

read_cr3:
    mov rax, cr3
    ret

write_cr3:
    mov cr3, rdi
    ret

flush_tlb:
    mov rax, cr3
    mov cr3, rax
    ret

invlpg:
    invlpg [rdi]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
