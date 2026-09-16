/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/arch/cpu/percpu.h
 * Purpose: Declares the per-processor data area — the block of state each
 *          processor owns outright and no other processor reads — the register
 *          through which the executing processor finds its own, and the nested
 *          interrupt-disable discipline that every critical section in the
 *          kernel is built upon.
 * Key definitions: PerCpu, PER_CPU_MAXIMUM, PerCpuInitialise, PerCpuCurrent,
 *          PerCpuIndex, PerCpuAt, PerCpuOnlineCount, PerCpuIsEstablished,
 *          PerCpuPushInterruptState, PerCpuPopInterruptState,
 *          PerCpuSaveInterruptState, PerCpuLoadInterruptState,
 *          PerCpuCriticalDepth, PerCpuReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 3.4.4: in 64-bit mode the FS and GS segment bases are held in
 *     IA32_FS_BASE and IA32_GS_BASE and are added to the effective address of an
 *     access that carries the corresponding segment override. The base is not
 *     truncated to 32 bits and segmentation is otherwise disabled, so a segment
 *     override in 64-bit mode is a base register and nothing else.
 *   - Intel SDM, Volume 2B, "SWAPGS": exchanges GS.base with the contents of
 *     IA32_KERNEL_GS_BASE. It is valid only at privilege level 0, which is what
 *     makes the value it produces one a user program cannot have chosen.
 *   - Intel SDM, Volume 2A, "CPUID": leaf 1 returns the initial APIC identifier
 *     of the executing logical processor in bits 31:24 of EBX. It is available
 *     before the local controller has been mapped or enabled, which is why the
 *     area is identified from it rather than from the controller.
 *   - Intel SDM, Volume 3A, Section 8.1.2.2: the LOCK prefix is unnecessary for
 *     an access a single processor makes to its own storage. Nothing in this
 *     area is written by any processor but its owner, and that is the whole
 *     reason it exists.
 *   - docs/design/CONCURRENCY.md, Sections 3 and 4: what the area holds, why the
 *     first two fields are where they are, and why the interrupt-disable is
 *     counted rather than saved and restored by the caller.
 *
 * Why this is reached through GS and not through an array index.
 *
 *   Every alternative requires the processor to establish which processor it is
 *   before it can reach its own state, and the cheapest honest answer to that
 *   question — a read of the local controller's identifier register — is an
 *   uncached load from a memory-mapped device. A spinlock acquire performs this
 *   lookup, and a spinlock acquire must not cost a device access.
 *
 *   GS.base answers it in one instruction, because the answer was written there
 *   once when the processor started. The cost is that the register must hold the
 *   area whenever kernel code runs, which is an invariant the entry paths are
 *   responsible for: docs/design/CONCURRENCY.md, Section 3.2, states it and names
 *   the four places that maintain it.
 */

#ifndef OXYS_ARCH_CPU_PERCPU_H
#define OXYS_ARCH_CPU_PERCPU_H

#include <oxys/types.h>

/*
 * The greatest number of processors an area is reserved for.
 *
 * It matches ACPI_PROCESSOR_MAXIMUM, the bound upon the processors the MADT
 * parse will record, so that a machine whose firmware declares more processors
 * than this kernel admits is refused in one place rather than two. The areas are
 * statically allocated: they are reached before a heap exists upon the processor
 * that owns them, and an allocation that failed at that moment could not be
 * reported through a channel that itself needs an area.
 */
#define PER_CPU_MAXIMUM 64U

/*
 * The state one processor owns.
 *
 * The first two fields are addressed by number from kernel/arch/x86_64/syscall/syscall_entry.asm
 * and the third from the inline assembly of PerCpuCurrent below. All three
 * offsets are asserted in kernel/arch/x86_64/cpu/percpu.c, because a field inserted above
 * them would leave the assembly reading the wrong quadword and the very next
 * instruction of the system-call entry path loads RSP from the first of them —
 * which is a kernel executing upon an address privilege level 3 chose.
 *
 * Nothing here is guarded by a lock and nothing here needs to be. Every field is
 * written by the processor that owns the area and by no other; the counters
 * below are therefore ordinary additions and not atomic ones, and a report that
 * reads another processor's area may read a torn value, which is a cost paid
 * knowingly for a diagnostic and by nothing that acts upon what it reads.
 */
typedef struct PerCpu
{
    /* Offset 0: the top of this processor's kernel stack. The SYSCALL entry path
     * loads RSP from it, having no stack of its own until it has. */
    uint64_t kernel_stack;

    /* Offset 8: where the caller's RSP is put while there is nowhere else to put
     * it — no register may be destroyed and no memory addressed, at that moment,
     * except through GS. */
    uint64_t user_stack;

    /* Offset 16: the address of this structure. A GS-relative access can read a
     * field of the area but cannot produce its address, there being no
     * instruction that loads the segment base; so the base is stored within the
     * area and read like any other field. */
    struct PerCpu *self;

    /* The kernel's own dense numbering, 0 upward in the order the processors
     * were registered. The bootstrap processor is always 0. It is not the APIC
     * identifier: that is the firmware's numbering and it is neither dense nor
     * guaranteed to begin at zero. */
    uint32_t index;

    /* The identifier the local controller answers to, taken from CPUID leaf 1
     * when the area is established. */
    uint32_t apic_identifier;

    /* Whether this is the processor the firmware chose to boot the machine. */
    bool bootstrap;

    /* Whether the processor is executing kernel code. The bootstrap processor is
     * online from the moment its area is established; the others became so at
     * sub-task 6.14. */
    bool online;

    /*
     * The nesting depth of the interrupt-disable, and the flag state that was in
     * force when the outermost one was entered.
     *
     * See PerCpuPushInterruptState below for why the depth is counted here
     * rather than returned to the caller.
     */
    uint32_t critical_depth;
    bool interrupts_were_enabled;

    /* The number of spinlocks this processor holds. It is asserted to be zero
     * where the kernel intends to sleep, and it is what tells a diagnostic that
     * a processor stopped inside a critical section. */
    uint32_t locks_held;

    /* Accounting, all of it written by this processor alone. */
    uint64_t locks_acquired;
    uint64_t lock_contentions;
    uint64_t ipis_received;
    uint64_t shootdowns_serviced;
} PerCpu;

/*
 * Establishes the area of the executing processor and puts its address into
 * GS.base.
 *
 * It is called before any lock may be taken, which in practice means immediately
 * after the diagnostic channels exist and before the frame allocator is built:
 * a spinlock acquire reaches the area, and an area that does not yet exist would
 * be a null dereference in the one routine that must never fail.
 *
 * The bootstrap processor's area is index 0. Sub-task 6.14 calls this once upon
 * each application processor as it starts, and each receives the next index.
 *
 * Returns false where every area is already in use, which is a machine with more
 * processors than PER_CPU_MAXIMUM.
 */
bool PerCpuInitialise(void);

/*
 * Puts the executing processor's area back into GS.base, having found it by the
 * identifier CPUID reports rather than through the register being repaired.
 *
 * It exists because loading GS from a descriptor destroys GS.base. Intel SDM,
 * Volume 3A, Section 3.4.4, provides that in 64-bit mode the FS and GS bases are
 * not ignored, and that a segment load sets the hidden base from the descriptor
 * — which for a flat data descriptor is zero. Every reload of the segment
 * registers therefore drops the area, and the one place that reloads them,
 * GdtInitialise, calls this immediately afterwards.
 *
 * The defect this was written against was not hypothetical: the area was
 * established before the frame allocator, GdtInitialise ran some hundreds of
 * lines later, and the first per-processor access after it faulted upon address
 * sixteen — the self pointer's offset, read through a base of zero.
 *
 * Returns false where no area has been established for the executing processor,
 * which is the state an application processor is in before PerCpuInitialise has
 * run upon it; nothing is written in that case, and PerCpuInitialise sets the
 * base itself when it does run.
 */
bool PerCpuEstablishSegmentBase(void);

/*
 * The area belonging to the executing processor.
 *
 * This is the whole of the mechanism: one load, of the third quadword of the
 * area, through a segment base only privilege level 0 can have written. It is
 * valid wherever kernel code runs, which docs/design/CONCURRENCY.md, Section 3.2,
 * defines; it is not valid before PerCpuInitialise has run, and every caller of
 * it runs after that by construction.
 *
 * The offset is written as a number because there is no way to interpolate
 * offsetof into an assembly template without a compiler-specific operand
 * modifier. It is asserted against the structure in kernel/arch/x86_64/cpu/percpu.c.
 */
static inline PerCpu *PerCpuCurrent(void)
{
    PerCpu *area;

    __asm__ __volatile__("movq %%gs:16, %0" : "=r"(area));

    return area;
}

/* The dense index of the executing processor. */
static inline uint32_t PerCpuIndex(void)
{
    return PerCpuCurrent()->index;
}

/*
 * The area of a numbered processor, or NULL where the number names none.
 *
 * Reading another processor's area is legitimate for a diagnostic and for
 * nothing else: every field is written by its owner without a lock, so a value
 * read here may be one that is being changed as it is read.
 */
PerCpu *PerCpuAt(uint32_t index);

/* The number of areas established, which is the number of processors executing
 * kernel code. It was 1 until sub-task 6.14 started the application processors;
 * it is now one more than SmpProcessorsStarted reports. */
uint32_t PerCpuOnlineCount(void);

/* Whether an area has been established for the executing processor. It exists so
 * that a diagnostic raised before PerCpuInitialise — a triple fault in the frame
 * allocator, say — does not reach through GS.base for a structure that is not
 * there. */
bool PerCpuIsEstablished(void);

/*
 * Enters a critical section: clears the interrupt flag, and records the state it
 * had if this is the outermost such section.
 *
 * The state is kept here rather than being returned to the caller because the
 * caller is usually a spinlock acquire, and a lock that returned the flag state
 * would have to be released with the value the acquire produced. Two nested
 * locks released in the wrong order would then restore the flags in the wrong
 * order and re-enable interrupts inside a section that had disabled them — a
 * fault that shows itself as a deadlock in an interrupt handler minutes later
 * and nowhere near the code that caused it. Counting the depth here makes the
 * pairing structural: the flag is restored when the last section is left,
 * whichever of them that turns out to be.
 */
void PerCpuPushInterruptState(void);

/*
 * Leaves a critical section, restoring the interrupt flag where this was the
 * outermost one.
 *
 * It panics rather than proceeding upon two conditions: a pop with no matching
 * push, which is a release without an acquire; and a pop performed with
 * interrupts already enabled, which means something inside the section executed
 * STI and the section was therefore not critical at all. Both are silent in
 * every other respect, and both produce corruption at some later moment that has
 * no connection to them.
 */
void PerCpuPopInterruptState(void);

/*
 * Declares that this processor holds no critical section, and enables
 * interrupts.
 *
 * It is for the entry path of a thread that has never run, and for nothing else.
 * The counted disable belongs to the processor rather than to the thread, so a
 * thread started by the scheduler of sub-task 6.15 begins with the depth its
 * predecessor left — masked, and never pre-empted again. See the note upon the
 * definition; the failure it prevents is a machine that hangs silently the first
 * time a thread is scheduled.
 */
void PerCpuResetInterruptState(void);

/*
 * Reads out, and puts back, the counted interrupt state — the depth and the
 * recorded flag — so that a thread switch of sub-task 8.6 can carry them with
 * the thread rather than leave them with the processor.
 *
 * The counted disable belongs to the processor, and until 8.6 that was
 * harmless: the scheduler switched kernel threads that never slept inside a
 * section, and a thread that had never run was given a fresh count by
 * PerCpuResetInterruptState. A user thread that *sleeps* — inside `wait`, or
 * upon a pipe — sleeps from inside its own push, and is resumed by whichever
 * thread happens to push next. Were the state left with the processor, the
 * sleeper's pop would restore the flag the *other* thread recorded: the idle
 * thread's push records that interrupts were enabled, and a program resumed
 * from it would leave its system call with interrupts enabled in the kernel,
 * where the exit path's SWAPGS and SYSRET assume they are not. Saving the
 * state into the outgoing thread and loading the incoming thread's makes each
 * thread's pop answer its own push, whoever ran in between.
 *
 * Neither touches the interrupt flag itself. The flag at a switch is the
 * caller's business, and the flag a resumed thread wants is restored by its own
 * pop, from the state these two carry.
 */
void PerCpuSaveInterruptState(uint32_t *depth, bool *interrupts_were_enabled);
void PerCpuLoadInterruptState(uint32_t depth, bool interrupts_were_enabled);

/* The nesting depth of the executing processor's critical section, and the
 * number of spinlocks it holds. Both exist for the self-test and the report. */
uint32_t PerCpuCriticalDepth(void);
uint32_t PerCpuLocksHeld(void);

/* Emits a summary of every established area upon the console and the serial
 * port. */
void PerCpuReport(void);

#endif /* OXYS_ARCH_CPU_PERCPU_H */
