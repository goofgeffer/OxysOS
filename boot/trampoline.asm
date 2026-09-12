; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
; ==============================================================================
; File: boot/trampoline.asm
;
; Purpose:
;   The real-mode trampoline an application processor begins executing when it
;   answers a startup inter-processor interrupt. It carries that processor from
;   the 16-bit real mode a reset leaves it in, through 32-bit protected mode,
;   into 64-bit long mode upon the kernel's own paging hierarchy, and hands it
;   to the C entry point of kernel/arch/x86_64/smp/smp.c.
;
; Key routines and data:
;   SmpTrampolineStart       - The first instruction the processor executes.
;   SmpTrampolineParameters  - The block the bootstrap processor fills in.
;   SmpTrampolineReal        - Real mode, with CS normalised to zero.
;   SmpTrampolineProtected   - 32-bit protected mode.
;   SmpTrampolineLongMode    - 64-bit mode, which jumps to the kernel.
;   SmpTrampolineGdt         - The table that carries it across the two modes.
;
; References:
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
;     Section 8.4.3 ("MP Initialization Protocol Algorithm for MP Systems") and
;     Section 8.4.4.1: the startup inter-processor interrupt carries a vector
;     VV, and the processor that answers it begins executing at physical address
;     000VV000H in real mode, with CS = VV00H and IP = 0000H.
;   - Intel SDM, Volume 3A, Section 9.1.4 and Table 9-1: the processor state
;     after a reset — CR0 = 60000010H, so protection is off; the interrupt flag
;     is clear; and the segment registers are undefined save CS, which is set
;     from the startup vector.
;   - Intel SDM, Volume 3A, Section 9.9.1 ("Switching to Protected Mode"): the
;     descriptor table must be loaded before CR0.PE is set, and the far jump
;     that follows is what loads CS with a protected-mode descriptor and flushes
;     the instruction that was prefetched under the old mode.
;   - Intel SDM, Volume 3A, Section 4.1.2 and Table 4-14: the order in which
;     long mode is entered — CR4.PAE, then IA32_EFER.LME, then CR0.PG.
;   - Intel SDM, Volume 3A, Section 3.4.5 and Figure 3-8: the segment descriptor,
;     including the L flag (bit 21 of the upper doubleword) that designates a
;     64-bit code segment and the D flag that must be clear where L is set.
;   - Intel SDM, Volume 3A, Section 2.5: CR0.WP, which is per processor and must
;     therefore be set here as boot/boot.asm's successor set it upon the
;     bootstrap processor.
;   - docs/design/SMP.md, Sections 3 and 4.
;
; Why this is a flat binary and not a linked section.
;
;   The processor begins in real mode, where an address is a segment and an
;   offset and nothing above the first mebibyte can be reached. The code must
;   therefore execute from a low physical page, and the addresses it names must
;   be that page's — not the higher-half addresses the kernel is linked at.
;   NASM's `org` directive states that origin, and `org` is available only in
;   the flat binary format. The image is assembled separately, embedded in the
;   kernel by kernel/arch/x86_64/smp/smp_trampoline.asm, and copied to the page named below
;   by SmpPlaceTrampoline before the first processor is started.
;
;   The origin is a constant rather than an argument because a real-mode near
;   reference is an offset from a segment base, and a relocatable trampoline
;   would have to compute every one of them at run time from a base it was
;   handed. Fixing the page and asserting that the firmware calls it usable is
;   the cheaper of the two, and the assertion is the part that matters: see
;   SmpTrampolinePageIsUsable in kernel/arch/x86_64/smp/smp.c.
;
; Why the first instruction is a far jump.
;
;   The processor arrives with CS = VV00H and IP = 0, so a label assembled at
;   origin 8000H would be reached at offset 8000H from a base that is already
;   8000H. The far jump to 0000H:SmpTrampolineReal normalises CS to zero, after
;   which every offset in this file means what `org` says it means and the whole
;   of the first mebibyte is addressable through a data segment of zero.
; ==============================================================================

; ------------------------------------------------------------------------------
; The origin. It must equal SMP_TRAMPOLINE_ADDRESS in kernel/include/oxys/smp.h,
; and the magic value below is what proves that it does: the bootstrap processor
; reads the magic back out of the copied page through the C structure, and a
; disagreement between this file's layout and that structure's is a wrong field
; read from a page the processor is about to execute.
; ------------------------------------------------------------------------------

SMP_TRAMPOLINE_ORIGIN       equ 0x8000
SMP_TRAMPOLINE_MAGIC        equ 0x504D5358  ; "XSMP", little-endian.

CR0_PROTECTION_BIT          equ 0
CR0_WRITE_PROTECT_BIT       equ 16
CR0_PAGING_BIT              equ 31
CR4_PAE_BIT                 equ 5
IA32_EFER_MSR               equ 0xC0000080
IA32_EFER_LME_BIT           equ 8

TRAMPOLINE_CODE32_SELECTOR  equ 0x08
TRAMPOLINE_DATA_SELECTOR    equ 0x10
TRAMPOLINE_CODE64_SELECTOR  equ 0x18

bits 16
org SMP_TRAMPOLINE_ORIGIN

; ------------------------------------------------------------------------------
; SmpTrampolineStart
;
; The first instruction executed by a processor answering a startup
; inter-processor interrupt. Interrupts are already masked by the reset state;
; the CLI is defensive, and the direction flag is established because the System
; V ABI the C entry point is compiled against requires it forward.
; ------------------------------------------------------------------------------
SmpTrampolineStart:
    cli
    cld
    jmp     0x0000:SmpTrampolineReal

; ------------------------------------------------------------------------------
; SmpTrampolineParameters
;
; The block the bootstrap processor fills in before it sends the startup
; interrupt. It is laid over by SmpTrampolineParameters in
; kernel/include/oxys/smp.h, and the two layouts are checked against each other
; at run time by way of the magic value rather than being trusted to agree.
;
; It stands here, immediately after the entry jump, so that its offsets are
; small constants both this file and that header can state. The jump above is a
; two-byte short displacement, so nothing here is executed.
; ------------------------------------------------------------------------------
align 8
SmpTrampolineParameters:
.magic:         dd  SMP_TRAMPOLINE_MAGIC    ; +0x00
.reserved:      dd  0                       ; +0x04, for the alignment below.
.page_table:    dq  0                       ; +0x08  CR3 for the starting processor.
.entry_point:   dq  0                       ; +0x10  The 64-bit C entry point.
.stack_top:     dq  0                       ; +0x18  The stack that entry runs upon.
.argument:      dq  0                       ; +0x20  Passed to the entry in RDI.
.acknowledged:  dq  0                       ; +0x28  Written by the starting processor.

; ------------------------------------------------------------------------------
; SmpTrampolineReal
;
; Real mode, with CS zero. The data segments are made zero to match, so that
; every `org`-relative reference below addresses the byte it names.
;
; No stack is established and none is used: there is no call and no push in this
; file before RSP is loaded from the parameter block in 64-bit mode. A stack in
; low memory would be a second page to find, to map and to prove usable, for the
; sake of a handful of instructions that need none.
; ------------------------------------------------------------------------------
SmpTrampolineReal:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; The descriptor table must be in force before protection is enabled;
    ; Intel SDM, Volume 3A, Section 9.9.1.
    lgdt    [SmpTrampolineGdtDescriptor]

    mov     eax, cr0
    or      eax, 1 << CR0_PROTECTION_BIT
    mov     cr0, eax

    ; The far jump loads CS from the table just installed and discards whatever
    ; was prefetched under the previous mode.
    jmp     TRAMPOLINE_CODE32_SELECTOR:SmpTrampolineProtected

; ------------------------------------------------------------------------------
; SmpTrampolineProtected
;
; 32-bit protected mode, paging disabled. It performs exactly the sequence
; boot/boot.asm performs for the bootstrap processor, differing in one respect:
; the paging hierarchy is not built here but taken from the parameter block,
; because the kernel already has one and a second would have to be kept in step
; with it.
; ------------------------------------------------------------------------------
bits 32
SmpTrampolineProtected:
    mov     ax, TRAMPOLINE_DATA_SELECTOR
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; Physical address extension, which long mode requires.
    mov     eax, cr4
    or      eax, 1 << CR4_PAE_BIT
    mov     cr4, eax

    ; The kernel's own hierarchy. Only the low doubleword is loaded: CR3 is a
    ; 32-bit register until paging is enabled in long mode, and the bootstrap
    ; processor refuses to start anybody at all if the root lies above four
    ; gibibytes — see SmpInitialise, which says why that refusal is a report and
    ; not an omission.
    mov     eax, [SmpTrampolineParameters.page_table]
    mov     cr3, eax

    mov     ecx, IA32_EFER_MSR
    rdmsr
    or      eax, 1 << IA32_EFER_LME_BIT
    wrmsr

    ; Paging, and with it long mode; and write protection in the same store.
    ;
    ; CR0.WP is per processor. Without it a write by privilege level 0 to a page
    ; marked read-only succeeds, which would leave this processor able to write
    ; the kernel's text while the bootstrap processor could not — a difference
    ; between processors that nothing in the kernel would ever report.
    mov     eax, cr0
    or      eax, (1 << CR0_PAGING_BIT) | (1 << CR0_WRITE_PROTECT_BIT)
    mov     cr0, eax

    ; The instruction after the store above executes with paging enabled and at
    ; the address it already had, so this page must be mapped in the hierarchy
    ; just loaded. SmpMapTrampolinePage establishes exactly that mapping, and
    ; SmpUnmapTrampolinePage removes it once every processor is past this point.
    jmp     TRAMPOLINE_CODE64_SELECTOR:SmpTrampolineLongMode

; ------------------------------------------------------------------------------
; SmpTrampolineLongMode
;
; 64-bit mode. The kernel's own descriptor table, interrupt descriptor table and
; per-processor area are established by the C entry point rather than here: they
; are the kernel's structures, and composing any of them in assembly would be a
; second implementation of something the kernel already has.
; ------------------------------------------------------------------------------
bits 64
SmpTrampolineLongMode:
    mov     ax, TRAMPOLINE_DATA_SELECTOR
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; The acknowledgement is written before the jump and not after it.
    ;
    ; It says "this processor reached 64-bit mode upon the kernel's hierarchy",
    ; which is the last thing this file can speak for. Everything after the jump
    ; is the kernel's, and the kernel reports its own progress through the
    ; per-processor area. A flag written by the C entry point instead would make
    ; a processor that got here and then faulted indistinguishable from one that
    ; never answered the startup interrupt at all — and those two have entirely
    ; different causes.
    mov     qword [SmpTrampolineParameters.acknowledged], 1

    mov     rsp, [SmpTrampolineParameters.stack_top]

    ; Terminate the frame-pointer chain, as boot/boot.asm does at the equivalent
    ; point, so that an unwinder recognises this frame as the outermost.
    xor     rbp, rbp

    ; The first integer argument, per the System V AMD64 calling convention.
    mov     rdi, [SmpTrampolineParameters.argument]

    mov     rax, [SmpTrampolineParameters.entry_point]
    jmp     rax

    ; The entry point does not return. Were it to, the processor is halted here
    ; rather than falling into the descriptor table below it.
.Halt:
    cli
    hlt
    jmp     .Halt

; ------------------------------------------------------------------------------
; SmpTrampolineGdt
;
; The table that carries the processor across the two mode changes. It is the
; kernel's table in miniature and is discarded the moment the C entry point
; loads the real one: what it must contain is a 32-bit code and data pair for
; the protected-mode step and a 64-bit code descriptor for the far jump that
; enters long mode, and nothing else.
;
; The base and limit of the 64-bit code descriptor are ignored by the processor,
; and are given the flat values the others carry so that the three descriptors
; differ only where they mean to.
; ------------------------------------------------------------------------------
align 8
SmpTrampolineGdt:
    dq  0x0000000000000000  ; 0x00: the mandatory null descriptor.
    dq  0x00CF9A000000FFFF  ; 0x08: 32-bit code, DPL 0, D set, L clear.
    dq  0x00CF92000000FFFF  ; 0x10: data, DPL 0, writable.
    dq  0x00AF9A000000FFFF  ; 0x18: 64-bit code, DPL 0, L set, D clear.
SmpTrampolineGdtEnd:

SmpTrampolineGdtDescriptor:
    dw  SmpTrampolineGdtEnd - SmpTrampolineGdt - 1
    dd  SmpTrampolineGdt    ; A 32-bit linear base; the table is below 1 MiB.

; ------------------------------------------------------------------------------
; The image must fit within the one 4 KiB page that is copied, mapped and proved
; usable. The assertion is here rather than in the C, because here it is a
; failure to build and there it would be a failure to boot.
; ------------------------------------------------------------------------------
SmpTrampolineEnd:

%if (SmpTrampolineEnd - SmpTrampolineStart) > 4096
    %error "The trampoline exceeds the single page that is copied to low memory."
%endif
