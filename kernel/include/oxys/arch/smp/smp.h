/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/arch/smp/smp.h
 * Purpose: Declares the bring-up of the application processors — the low page
 *          the real-mode trampoline is copied to, the block of parameters the
 *          bootstrap processor fills in before it starts one, the startup
 *          sequence itself, and what a started processor does until there is a
 *          scheduler to give it work.
 * Key definitions: SMP_TRAMPOLINE_ADDRESS, SMP_TRAMPOLINE_VECTOR,
 *          SMP_TRAMPOLINE_MAGIC, SmpTrampolineParameters, SmpInitialise,
 *          SmpApplicationProcessorEntry, SmpProcessorsStarted,
 *          SmpProcessorsRefused, SmpIsMultiprocessor, SmpStartupFailureCount,
 *          SmpReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 8.4.4.1 ("Typical BSP Initialization Sequence"): the bootstrap
 *     processor starts an application processor by an INIT inter-processor
 *     interrupt, a wait of ten milliseconds, a startup inter-processor
 *     interrupt, a wait of two hundred microseconds, and a second startup
 *     interrupt where the first was not answered.
 *   - Intel SDM, Volume 3A, Section 8.4.3: the startup interrupt carries a
 *     vector VV, and the processor that answers it begins executing in real
 *     mode at physical address 000VV000H.
 *   - Intel SDM, Volume 3A, Section 10.6.1 and Figure 10-12: the interrupt
 *     command register, its delivery modes 101 (INIT) and 110 (Start-Up), and
 *     the destination field of bits 31:24 of the high half.
 *   - Intel SDM, Volume 3A, Section 8.4.4.1, the note upon interrupt
 *     inhibition: every device capable of delivering an interrupt must be
 *     inhibited between the INIT and the last startup interrupt of a sequence.
 *   - ACPI Specification 6.5, Section 5.2.12.2 and Table 5.23: which declared
 *     processors are usable, and therefore which this brings up.
 *   - docs/design/SMP.md: the design, the ordering, and what each self-test
 *     assertion exists to catch.
 *
 * What this sub-task adds, and what it deliberately does not.
 *
 *   It starts every processor the firmware declares usable, carries each into
 *   64-bit mode upon the kernel's own paging hierarchy, gives it the kernel's
 *   descriptor tables, a task state segment of its own and a per-processor area
 *   of its own, and then parks it in a loop that halts until it is interrupted.
 *
 *   It gives none of them anything to run. There is no scheduler until sub-task
 *   6.15 and no run queue for a processor to take work from, so a started
 *   processor's whole contribution is that it answers inter-processor
 *   interrupts — which is not nothing: it is what makes the shootdown of
 *   sub-task 6.13 a real broadcast to real targets rather than a mechanism
 *   exercised upon its own sender.
 */

#ifndef OXYS_ARCH_SMP_SMP_H
#define OXYS_ARCH_SMP_SMP_H

#include <oxys/types.h>

/*
 * The physical page the trampoline is copied to, and the startup vector that
 * names it.
 *
 * The vector is the page number: Intel SDM, Volume 3A, Section 8.4.3, gives the
 * address a processor begins at as 000VV000H for a startup interrupt carrying
 * vector VV, so the two constants are one fact written twice and the static
 * assertion below is what keeps them one.
 *
 * The page is 0x8000 because the whole of it must be below the first mebibyte,
 * which is all a processor in real mode can address, and because that mebibyte
 * is not this kernel's to allocate from: kernel/mm/pmm.c reserves it in its
 * entirety, so no frame within it is ever issued and a fixed page cannot
 * collide with one that was. What it can collide with is something the firmware
 * left there, and that is not assumed away — SmpInitialise refuses to start any
 * processor at all unless the Multiboot2 memory map declares this page
 * available and it lies clear of the kernel image and of the boot information
 * structure.
 *
 * 0x8000 in particular stands above the interrupt vector table at 0, the BIOS
 * data area at 0x400, and the sector a legacy boot loader is read to at 0x7C00.
 */
#define SMP_TRAMPOLINE_ADDRESS UINT64_C(0x8000)
#define SMP_TRAMPOLINE_VECTOR  UINT8_C(0x08)

_Static_assert(((uint64_t)SMP_TRAMPOLINE_VECTOR << 12) == SMP_TRAMPOLINE_ADDRESS,
               "The startup vector must name the page the trampoline is copied to.");
_Static_assert(SMP_TRAMPOLINE_ADDRESS < UINT64_C(0x100000),
               "A processor answering a startup interrupt is in real mode and "
               "cannot address anything above the first mebibyte.");

/*
 * The offset of the parameter block within that page, and the value that proves
 * the block is where this header says it is.
 *
 * boot/trampoline.asm lays the block out in assembly and this header lays a C
 * structure over it, and neither can see the other. The magic is written by the
 * assembler as the block's first field and read back through the structure
 * after the image has been copied: a disagreement between the two layouts shows
 * itself there, at the one moment it can still be reported, rather than as a
 * processor jumping to whatever the entry-point field happened to overlap.
 */
#define SMP_TRAMPOLINE_PARAMETER_OFFSET UINT64_C(0x08)
#define SMP_TRAMPOLINE_MAGIC            UINT32_C(0x504D5358) /* "XSMP". */

/*
 * The block the bootstrap processor fills in before each start.
 *
 * It is packed, and the reason is the one PROJECT_GUIDELINES.md, Section 8,
 * requires to be stated: the layout is not this compiler's to choose. It is
 * fixed by boot/trampoline.asm, which addresses each field by a displacement
 * the assembler computed, and a padding byte inserted here would move every
 * field after it out from under the instruction that reads it. The fields are
 * nevertheless ordered and sized so that each is naturally aligned, so the
 * attribute changes no offset and exists to prevent one being introduced.
 */
typedef struct __attribute__((packed)) SmpTrampolineParameters
{
    uint32_t magic;         /* Written by the assembler; verified, never set. */
    uint32_t reserved;      /* Aligns the quadwords that follow. */
    uint64_t page_table;    /* The value loaded into CR3. */
    uint64_t entry_point;   /* SmpApplicationProcessorEntry, as a linear address. */
    uint64_t stack_top;     /* The stack that entry point begins upon. */
    uint64_t argument;      /* Passed to it in RDI: the index it must claim. */
    uint64_t acknowledged;  /* Set by the starting processor in 64-bit mode. */
} SmpTrampolineParameters;

_Static_assert(sizeof(SmpTrampolineParameters) == 48U,
               "The parameter block must match the layout of boot/trampoline.asm.");

/*
 * The stack a started processor runs upon, in pages of 4 KiB.
 *
 * It serves three purposes at once, sub-task 6.15 having divided none of them:
 * it is the
 * stack the trampoline hands the C entry point, it is what the task state
 * segment names as the stack of a transition to privilege level 0, and it is
 * what the processor idles upon. Four pages is what a kernel stack is given
 * everywhere else in this kernel; see TSS_KERNEL_STACK_SIZE.
 */
#define SMP_PROCESSOR_STACK_PAGES 4U

/*
 * What a started processor records about itself, once it has finished setting
 * itself up and before it parks.
 *
 * It exists because of a gap a negative test found. Every other assertion about
 * a started processor is made from outside it — that its area is online, that
 * its identifier is unique, that it answered a shootdown — and none of those
 * touches the registers that are per processor rather than per machine. A
 * processor that never loaded a task state segment passed all of them: it ran,
 * it answered interrupts, and it would have taken a triple fault the first time
 * it double-faulted, because a gate naming an interrupt stack table entry cannot
 * be delivered upon a processor whose task register is null.
 *
 * These are values only the processor itself can read, taken with the
 * instructions that read them — STR, SGDT, SIDT and the control registers — and
 * not what this kernel believes it told the processor to load. The distinction
 * is the whole point: what is being asserted is that the load happened.
 */
typedef struct SmpProcessorRecord
{
    uint64_t task_register;  /* STR: the selector, or zero if LTR never ran. */
    uint64_t gdt_base;       /* SGDT, which must name the kernel's own table. */
    uint64_t gdt_limit;
    uint64_t idt_base;       /* SIDT, likewise. */
    uint64_t cr0;            /* Paging and write protection, both per processor. */
    uint64_t cr3;            /* The kernel hierarchy, and no other. */
    uint64_t cr4;
    bool recorded;
} SmpProcessorRecord;

/* The record of a numbered processor, or NULL where it made none. The bootstrap
 * processor makes none: everything here can be read from it directly, and a copy
 * would be a second thing that could disagree. */
const SmpProcessorRecord *SmpRecordAt(uint32_t index);

/*
 * Starts every processor the firmware declares usable, and returns when each has
 * either come online or been given up upon.
 *
 * It must be called after the local controller is enabled, after the
 * inter-processor interrupt layer and the shootdown exist, after the descriptor
 * tables are built, after the kernel arena can allocate — a started processor is
 * given a stack, and it is given one rather than taking one, this kernel's
 * allocators not yet being safe against two processors at once — and after the
 * interval timer is programmed, the delays the protocol prescribes being
 * measured by polling its counter.
 *
 * It is safe upon a machine with one processor, where it starts nobody and says
 * so.
 */
void SmpInitialise(void);

/*
 * The C entry point of a started processor, entered from boot/trampoline.asm in
 * 64-bit mode upon the kernel's paging hierarchy, with a stack and with nothing
 * else established.
 *
 * It does not return. It is declared here rather than kept private because the
 * trampoline is handed its address, and an address taken of a static function
 * would be an address of something the linker was free to have discarded.
 */
_Noreturn void SmpApplicationProcessorEntry(uint64_t index);

/* The number of processors started, not counting the bootstrap processor; the
 * number the firmware declared but this kernel declined to start; and the number
 * that were started and never answered. */
uint32_t SmpProcessorsStarted(void);
uint32_t SmpProcessorsRefused(void);
uint32_t SmpStartupFailureCount(void);

/* Whether more than one processor is executing kernel code. */
bool SmpIsMultiprocessor(void);

/* Why no processor was started, where none was, as a sentence; NULL where the
 * bring-up was attempted. It is the report's whole explanation upon a machine
 * that has one processor or a page this kernel would not use. */
const char *SmpDeclinedReason(void);

/* Emits a summary upon the console and the serial port. */
void SmpReport(void);

#endif /* OXYS_ARCH_SMP_SMP_H */
