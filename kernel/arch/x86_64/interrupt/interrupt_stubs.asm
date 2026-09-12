; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
; ==============================================================================
; File: kernel/arch/x86_64/interrupt/interrupt_stubs.asm
;
; Purpose:
;   Provides one entry stub for each of the 256 interrupt vectors, together with
;   the common stub that they all reach. The stubs exist to normalise two
;   irregularities of the architecture so that the C dispatcher receives an
;   identical frame whatever the vector:
;
;     (a) Only some exceptions push an error code. A stub for a vector that does
;         not receives a zero in its place, so that the frame has the same shape
;         in every case.
;     (b) The processor does not record which vector was presented. Each stub
;         pushes its own vector number, which is the only way the dispatcher can
;         know what happened.
;
; Key routines and data:
;   InterruptStub0 .. InterruptStub255 - the per-vector entry points.
;   InterruptCommonStub                - saves the registers and calls the dispatcher.
;   InterruptStubTable                 - an array of the 256 stub addresses, for C.
;
; References:
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
;     Table 6-1: the vectors that push an error code are 8 (#DF), 10 (#TS),
;     11 (#NP), 12 (#SS), 13 (#GP), 14 (#PF) and 17 (#AC). Later revisions of the
;     architecture add 21 (#CP), and the AMD64 architecture adds 29 (#VC) and
;     30 (#SX). All ten are treated here as pushing an error code; refer to the
;     note below.
;   - Intel SDM, Volume 3A, Section 6.12.1 and Figure 6-4: in 64-bit mode the
;     processor pushes SS, RSP, RFLAGS, CS and RIP unconditionally, and aligns
;     RSP to a 16-byte boundary before doing so.
;   - Intel SDM, Volume 3A, Section 6.13: the error code is pushed last, nearest
;     the handler, and in 64-bit mode is padded to eight bytes.
;   - Intel SDM, Volume 2A, "IRET/IRETQ": returns from the handler, popping the
;     frame the processor pushed.
;   - Intel SDM, Volume 2B, "SWAPGS": exchanges GS.base with the contents of
;     IA32_KERNEL_GS_BASE. It is valid only at privilege level 0.
;   - Intel SDM, Volume 3A, Section 6.12.1: the CS the processor pushes carries
;     the requested privilege level of the interrupted code in its low two bits,
;     which is how a handler establishes where it was entered from.
;   - System V ABI, AMD64 supplement, Section 3.2.3: the first integer argument
;     is passed in RDI; Section 3.4.1 requires the direction flag to be clear
;     upon entry to a function.
;
; Note upon the segment base exchange, added at sub-task 6.13.
;   The kernel keeps its per-processor data area in GS.base while it executes and
;   in IA32_KERNEL_GS_BASE while a user program does; docs/design/CONCURRENCY.md,
;   Section 3.2, states the invariant. An interrupt that arrives while a user
;   program is running therefore enters kernel code with the program's value in
;   the register, and every spinlock the handler takes reaches for the area
;   through it. The exchange below restores the invariant on the way in and undoes
;   it on the way out, and is performed only where the saved CS says the interrupt
;   came from privilege level 3 — an interrupt taken in the kernel already has the
;   area there, and exchanging unconditionally would hand it away.
;
;   The test is made against the frame rather than against a register, because at
;   that moment no register can be trusted to hold anything: the interrupted code
;   owned all of them.
;
; Note upon vectors 21, 29 and 30.
;   The revision of Table 6-1 consulted lists vectors 21 to 31 as reserved. Later
;   revisions define 21 as #CP, the control protection exception, which pushes an
;   error code; the AMD64 architecture defines 29 as #VC and 30 as #SX, which do
;   likewise. They are treated here as pushing an error code, because the cost of
;   being wrong differs sharply between the two choices. If such an exception is
;   never raised, the treatment is immaterial. If one is raised on a processor
;   that pushes an error code and the stub pushed a further zero, every field of
;   the frame beyond that point would be displaced by eight bytes and the
;   diagnosis would be nonsense.
;
; Note upon software interrupts.
;   The INT n instruction never pushes an error code, whatever the vector. The
;   stubs for the ten vectors above therefore MUST NOT be reached by INT n: doing
;   so would leave the frame short by eight bytes. Nothing in this kernel invokes
;   them that way, and nothing should.
; ==============================================================================

section .text
bits 64

extern InterruptDispatch

; ------------------------------------------------------------------------------
; The per-vector stubs.
;
; A stub is deliberately minimal. It establishes the two fields the processor did
; not, and transfers to the common stub; nothing that could fault or that depends
; upon a register is performed before the registers have been saved.
; ------------------------------------------------------------------------------

; A vector for which the processor pushes no error code. A zero is supplied in
; its place so that the frame is uniform.
%macro InterruptStubNoErrorCode 1
global InterruptStub%1
InterruptStub%1:
    push    qword 0                     ; The absent error code.
    push    qword %1                    ; The vector number.
    jmp     InterruptCommonStub
%endmacro

; A vector for which the processor has already pushed an error code. Only the
; vector number is required.
%macro InterruptStubWithErrorCode 1
global InterruptStub%1
InterruptStub%1:
    push    qword %1                    ; The vector number.
    jmp     InterruptCommonStub
%endmacro

%assign VectorNumber 0
%rep 256
    %if VectorNumber == 8  || VectorNumber == 10 || VectorNumber == 11 || \
        VectorNumber == 12 || VectorNumber == 13 || VectorNumber == 14 || \
        VectorNumber == 17 || VectorNumber == 21 || VectorNumber == 29 || \
        VectorNumber == 30
        InterruptStubWithErrorCode VectorNumber
    %else
        InterruptStubNoErrorCode VectorNumber
    %endif
    %assign VectorNumber VectorNumber+1
%endrep

; ------------------------------------------------------------------------------
; InterruptCommonStub
;
; Saves every general-purpose register, calls the C dispatcher with a pointer to
; the completed frame, restores the registers and returns from the interrupt.
;
; The push order is the reverse of the declaration order of TrapFrame in
; kernel/include/oxys/interrupts.h. RAX is pushed first and therefore occupies
; the highest address of the saved set; R15 is pushed last and occupies the
; lowest, which is where RSP points when the dispatcher is called. The two files
; are one interface expressed in two languages and must be changed together.
;
; RSP is not among the registers pushed. The stack pointer of the interrupted
; code was recorded by the processor in the frame it pushed; pushing the register
; here would record the handler's own stack pointer instead, which is of no
; interest and would be mistaken for the other.
;
; Stack alignment. The processor aligns RSP to 16 bytes before pushing its
; five-quadword frame, whether or not an error code follows. Adding the vector
; and the error code brings the total to seven quadwords, and the fifteen
; general-purpose registers bring it to twenty-two, which is 176 bytes and a
; multiple of 16. RSP is therefore 16-byte aligned at the CALL, and the CALL
; pushes the eight bytes that the System V ABI expects to have been pushed upon
; entry to a function.
; ------------------------------------------------------------------------------
InterruptCommonStub:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; The System V ABI requires the direction flag to be clear upon entry to a
    ; function. The interrupted code may have set it, and the interrupt did not
    ; clear it.
    cld

    ; The per-processor area into GS.base, where the interrupt came from
    ; privilege level 3. The saved CS lies fifteen registers, the vector, the
    ; error code and RIP above the current stack pointer, which is 18 quadwords;
    ; TRAP_FRAME_CS_OFFSET in kernel/include/oxys/interrupts.h asserts the number
    ; against the structure the two files share.
    test    qword [rsp + 144], 3
    jz      .kernel_segment_base
    swapgs
.kernel_segment_base:

    ; The frame is complete and begins at the current stack pointer. Pass its
    ; address as the first argument.
    mov     rdi, rsp
    call    InterruptDispatch

    ; And back, if the frame says control is returning to privilege level 3. The
    ; frame is read again rather than a decision being remembered, because a
    ; handler is permitted to alter it — docs/design/INTERRUPTS.md, Section 7.2 —
    ; and the exchange must match the privilege level being returned to and not
    ; the one that was left.
    test    qword [rsp + 144], 3
    jz      .kernel_return
    swapgs
.kernel_return:

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    ; Discard the vector number and the error code, which the processor will not
    ; pop. IRETQ expects RSP to point at the saved RIP.
    add     rsp, 16

    iretq

; ------------------------------------------------------------------------------
; InterruptStubTable
;
; The addresses of the 256 stubs, in vector order, so that the C code may install
; the gates without naming each stub individually.
; ------------------------------------------------------------------------------

section .rodata
align 8
global InterruptStubTable
InterruptStubTable:
%assign VectorNumber 0
%rep 256
    dq      InterruptStub %+ VectorNumber
%assign VectorNumber VectorNumber+1
%endrep
