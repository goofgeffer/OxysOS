; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
; ==============================================================================
; File: kernel/arch/x86_64/cpu/gdt.asm
;
; Purpose:
;   Loads the kernel global descriptor table and reloads every segment register,
;   including CS, which cannot be assigned by an ordinary instruction.
;
; Key routines:
;   GdtLoadAndReloadSegments - loads the table and reloads the selectors.
;
; References:
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2A,
;     "LGDT/LIDT": loads the descriptor table register.
;   - Intel SDM, Volume 3A, Section 3.4.3: the CS register cannot be loaded by a
;     MOV instruction. It is changed by a far transfer, of which a far return is
;     the form usable here, the target being expressible as a 64-bit address.
;   - Intel SDM, Volume 2A, "RET": the far form pops the instruction pointer and
;     then the code segment selector, so the selector must be pushed first.
;   - Intel SDM, Volume 3A, Section 6.8.3: loading SS inhibits interrupts and
;     certain debug exceptions until after the following instruction, so the
;     stack pointer may be changed safely in the pair.
;   - System V ABI, AMD64 supplement, Section 3.2.3: the first three integer
;     arguments arrive in RDI, RSI and RDX.
; ==============================================================================

section .text
bits 64

global GdtLoadAndReloadSegments

; ------------------------------------------------------------------------------
; GdtLoadAndReloadSegments(const GdtRegister *descriptor,
;                          uint16_t code_selector,
;                          uint16_t data_selector)
;
; RDI holds the address of the ten-byte LGDT operand, RSI the code selector and
; RDX the data selector.
; ------------------------------------------------------------------------------
GdtLoadAndReloadSegments:
    lgdt    [rdi]

    ; Reload the data segment registers. In 64-bit mode their base and limit are
    ; ignored, but a selector referring to the retired table would remain in the
    ; register, and would be consulted upon a transition that does not ignore it.
    ;
    ; GS is the exception to "their base is ignored", and the load below destroys
    ; it. Intel SDM, Volume 3A, Section 3.4.4, provides that FS.base and GS.base
    ; are *not* ignored in 64-bit mode, and that loading the segment register from
    ; a descriptor sets the hidden base to the descriptor's — which for the flat
    ; data descriptor of this table is zero. The per-processor area of sub-task
    ; 6.13 lives at that base, so it is re-established by the caller in
    ; kernel/arch/x86_64/cpu/gdt.c the instant this routine returns; see the note there.
    mov     ax, dx
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; Reload CS by a far return. The selector is pushed first because the far
    ; return pops the instruction pointer before it.
    lea     rax, [rel .ReloadedCodeSegment]
    push    rsi
    push    rax
    o64 retf

.ReloadedCodeSegment:
    ret
