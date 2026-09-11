; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: MIT
; ==============================================================================
; File: libc/syscall/invoke.asm
;
; Purpose:
;   The instruction. Everything else in this library's system-call support is C
;   standing upon these four routines, each of which moves a caller's arguments
;   into the registers the kernel reads them from, executes SYSCALL, and returns
;   what the kernel put in RAX.
;
; Key routines:
;   OxysSyscallInvoke0 - a call taking no argument.
;   OxysSyscallInvoke1 - a call taking one.
;   OxysSyscallInvoke2 - a call taking two.
;   OxysSyscallInvoke3 - a call taking three, which is every call this kernel
;                        presently has that takes more than two.
;
; References:
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
;     "SYSCALL": the instruction loads RIP from IA32_LSTAR and CS and SS from
;     IA32_STAR, saves the address of the following instruction in RCX and RFLAGS
;     in R11, and clears in RFLAGS every bit set in IA32_FMASK. RCX and R11 are
;     therefore destroyed by the instruction itself and no value may be left in
;     either across it.
;   - System V Application Binary Interface, AMD64 supplement, Section 3.2.3: the
;     first six integer arguments arrive in RDI, RSI, RDX, RCX, R8 and R9, and an
;     integer result is returned in RAX.
;   - kernel/abi/oxys/syscall_abi.h: the kernel reads the call number from RAX
;     and the arguments from RDI, RSI, RDX, R10, R8 and R9 — the fourth register
;     differing from the C convention because SYSCALL has taken RCX.
;   - docs/design/LIBC.md, Section 8: why this is a translation unit of assembly
;     rather than inline assembly inside the C wrappers.
;
; The shift, and why it is what these routines are for.
;
;   A caller writes OxysSyscallInvoke3(number, a, b, c), so the compiler places
;   the number in RDI and the three arguments in RSI, RDX and RCX. The kernel
;   wants the number in RAX and the three arguments in RDI, RSI and RDX. Every
;   routine below is therefore that shift by one place and nothing else, written
;   so that each register is read before it is overwritten — RCX in particular,
;   which must be consumed before SYSCALL destroys it.
;
; Why there are four of these and not one.
;
;   One routine taking the maximum number of arguments would have to move six
;   registers whatever the call, and would take its seventh argument — the sixth
;   of the call — from the stack, since the shift pushes the last one off the end
;   of the six the convention provides. Four routines, one per arity, move only
;   what there is to move and touch no memory at all. That last property is the
;   one this file is arranged around: see below.
;
; Nothing here refers to memory, and that is load-bearing.
;
;   These routines contain no memory operand, no relative displacement, no
;   absolute address and no relocation. The bytes the assembler emits therefore
;   mean the same thing wherever they are placed, which is what allows
;   kernel/test/verify_wrappers.c to copy them out of the kernel image, into a
;   program's own address space, and execute them there at privilege level 3 —
;   asserting the code this library actually ships rather than a reconstruction
;   of it. A single instruction reaching a variable would end that, so nothing
;   here may acquire one.
;
;   The aliases beside each entry point exist for the same test. A symbol
;   declared to C as a function and then read as bytes would be two
;   contradictory declarations of one name; the test reads the aliases, which are
;   declared as bytes and nothing else, and the two declarations never meet.
; ==============================================================================

bits 64

section .text

global OxysSyscallInvoke0
global OxysSyscallInvoke1
global OxysSyscallInvoke2
global OxysSyscallInvoke3

; The boundaries and the entry offsets, for the self-test that copies this block.
global OxysSyscallInvokeBegin
global OxysSyscallInvokeEnd
global OxysSyscallInvokeBytes0
global OxysSyscallInvokeBytes1
global OxysSyscallInvokeBytes2
global OxysSyscallInvokeBytes3

OxysSyscallInvokeBegin:

; ------------------------------------------------------------------------------
; int64_t OxysSyscallInvoke0(uint64_t number)
;
; RDI holds the number and becomes RAX. Nothing else moves.
; ------------------------------------------------------------------------------
OxysSyscallInvoke0:
OxysSyscallInvokeBytes0:
    mov     rax, rdi
    syscall
    ret

; ------------------------------------------------------------------------------
; int64_t OxysSyscallInvoke1(uint64_t number, uint64_t first)
;
; RSI must reach RDI, so RDI is read into RAX first. Written the other way round
; the number would be overwritten before it was taken.
; ------------------------------------------------------------------------------
OxysSyscallInvoke1:
OxysSyscallInvokeBytes1:
    mov     rax, rdi
    mov     rdi, rsi
    syscall
    ret

; ------------------------------------------------------------------------------
; int64_t OxysSyscallInvoke2(uint64_t number, uint64_t first, uint64_t second)
; ------------------------------------------------------------------------------
OxysSyscallInvoke2:
OxysSyscallInvokeBytes2:
    mov     rax, rdi
    mov     rdi, rsi
    mov     rsi, rdx
    syscall
    ret

; ------------------------------------------------------------------------------
; int64_t OxysSyscallInvoke3(uint64_t number, uint64_t first, uint64_t second,
;                            uint64_t third)
;
; The third argument arrives in RCX, which SYSCALL destroys. It is moved into RDX
; before the instruction executes, which is the whole reason the moves below run
; in this order and not another.
; ------------------------------------------------------------------------------
OxysSyscallInvoke3:
OxysSyscallInvokeBytes3:
    mov     rax, rdi
    mov     rdi, rsi
    mov     rsi, rdx
    mov     rdx, rcx
    syscall
    ret

OxysSyscallInvokeEnd:
