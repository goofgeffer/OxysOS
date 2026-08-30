; ==============================================================================
; File: kernel/cpu/interrupt_stubs.asm
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
;   - System V ABI, AMD64 supplement, Section 3.2.3: the first integer argument
;     is passed in RDI; Section 3.4.1 requires the direction flag to be clear
;     upon entry to a function.
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

    ; The frame is complete and begins at the current stack pointer. Pass its
    ; address as the first argument.
    mov     rdi, rsp
    call    InterruptDispatch

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
