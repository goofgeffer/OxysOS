/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/cpu/tss.c
 * Purpose: Establishes the task state segment: the stacks it names, its
 *          descriptor within the global descriptor table, and the loading of the
 *          task register.
 * Key functions: TssInitialise, TssInitialiseProcessor, TssSetKernelStack,
 *          TssKernelStack, TssInterruptStack, TssAddress, TssLimit,
 *          TssTaskRegister, TssReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 8.7 and Figure 8-11: the 64-bit task state segment.
 *   - Intel SDM, Volume 3A, Section 6.14.4: an interrupt stack table entry is
 *     loaded unconditionally, whatever the privilege change, which is what makes
 *     it usable for a fault taken at privilege level 0.
 *   - Intel SDM, Volume 3A, Section 8.2.3: the sixteen-byte TSS descriptor, and
 *     its type of 9 while available and 11 once loaded.
 *   - Intel SDM, Volume 2A, "LTR": the operand is a selector for an available
 *     TSS descriptor, and the instruction marks that descriptor busy.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.2:
 *     the stack pointer is sixteen-byte aligned at a function's entry, which is
 *     why the stacks below are aligned and sized in multiples of sixteen.
 *
 * Design note. In 64-bit mode this structure holds no task state. Hardware task
 * switching does not exist there, and what remains of the segment is a table of
 * stack pointers the processor reads when it needs a stack it can trust: RSP0
 * upon a transfer from privilege level 3, and an interrupt stack table entry
 * upon a gate that names one. The name is the architecture's and is retained
 * because every manual uses it; nothing here switches a task.
 *
 * Concurrency. Each processor has a segment of its own, with its own stacks and
 * its own descriptor, because RSP0 names the stack of whatever is running upon
 * *that* processor and a shared segment would deliver two system calls upon one
 * stack. The task register is per-processor already, so what sub-task 6.14
 * duplicated is the storage and not the mechanism. No lock is required and none
 * would help: every segment is written by the processor that owns it, save the
 * one moment at which the bootstrap processor installs a descriptor for a
 * processor it is about to start — and that processor is not running yet.
 */

#include <oxys/tss.h>
#include <oxys/gdt.h>
#include <oxys/kernel.h>

/*
 * The segments, one to a processor.
 *
 * They are not const: the processor reads them, and this kernel writes RSP0 at
 * every privilege transition from sub-task 6.9. The array is aligned so that no
 * field crosses a cache line unnecessarily, the processor reading RSP0 upon
 * every entry from user mode.
 *
 * The array is statically sized to PER_CPU_MAXIMUM rather than to the number of
 * processors the firmware declares, because a descriptor for a processor must be
 * installed before that processor runs and the address it names must not move
 * afterwards — the processor holds it in a structure it reads without asking.
 * An array that grew would move every segment already in use.
 */
static TaskStateSegment TssSegments[PER_CPU_MAXIMUM] __attribute__((aligned(16)));

/*
 * The segment of the executing processor.
 *
 * The dense index of kernel/include/oxys/percpu.h is the key, and not the
 * identifier the local controller answers to: the index is this kernel's own
 * numbering, is dense from zero, and is therefore what an array is subscripted
 * by. The bootstrap processor is index 0 and is the only processor that ever
 * reaches this function before its area exists — TssInitialise runs long after
 * PerCpuInitialise, but a diagnostic raised in between would otherwise read
 * through a segment base that is not yet a base.
 */
static TaskStateSegment *TssCurrent(void)
{
    const uint32_t index = PerCpuIsEstablished() ? PerCpuIndex() : 0U;

    return &TssSegments[(index < PER_CPU_MAXIMUM) ? index : 0U];
}

/*
 * The stack the processor loads upon entering privilege level 0 from level 3.
 *
 * It is reserved in .bss rather than taken from the kernel heap because it must
 * exist before anything that could fail does, and because the address must not
 * change: the processor holds it in a structure it reads without asking.
 *
 * There is no guard page beneath it, and there will not be one. The thread
 * stacks of sub-task 6.9 come from the kernel arena and do carry a guard; this
 * stack is not one of them. It is reserved in `.bss` because it must exist
 * before any allocator does, and `.bss` has no page beneath it to unmap.
 *
 * What lies below it instead is the double-fault stack, so an overflow runs into
 * that, and the double fault that results is delivered upon a stack of its own
 * and is therefore reported. That is an accident rather than a design, and it is
 * recorded as a limitation in docs/design/PRIVILEGE.md.
 */
static uint8_t TssKernelStackStore[TSS_KERNEL_STACK_SIZE] __attribute__((aligned(16)));

/*
 * The stack upon which a double fault is delivered.
 *
 * It is separate from every other stack in the machine, and that separation is
 * the whole of its purpose: the commonest cause of a double fault is that the
 * stack in use was the thing that went wrong, and a processor that cannot push
 * an exception frame does not raise a third exception but shuts down.
 */
static uint8_t TssDoubleFaultStackStore[TSS_INTERRUPT_STACK_SIZE]
    __attribute__((aligned(16)));

/* The top of a stack reserved as an array: the stack grows downward, so the
 * pointer begins one past the last byte. */
static uint64_t TssStackTop(uint8_t *store, size_t size)
{
    return (uint64_t)(uintptr_t)&store[size];
}

void TssInitialise(void)
{
    /*
     * The bootstrap processor is index 0 and takes the two stacks reserved in
     * .bss above. It cannot take allocated ones: this runs before the process
     * table exists and, more to the point, the stacks must exist before anything
     * that could fail does. Every other processor is given allocated stacks by
     * the bootstrap processor before it is started; see SmpPrepareProcessor.
     */
    TssInitialiseProcessor(
        0U,
        TssStackTop(TssKernelStackStore, sizeof TssKernelStackStore),
        TssStackTop(TssDoubleFaultStackStore, sizeof TssDoubleFaultStackStore));
}

void TssInitialiseProcessor(uint32_t processor_index, uint64_t kernel_stack_top,
                            uint64_t double_fault_stack_top)
{
    TaskStateSegment *segment;

    if (processor_index >= PER_CPU_MAXIMUM)
    {
        KernelPanic("A task state segment was asked for beyond the reservation.");
    }

    segment = &TssSegments[processor_index];

    for (size_t index = 0U; index < sizeof *segment; ++index)
    {
        ((uint8_t *)segment)[index] = 0U;
    }

    segment->rsp0 = kernel_stack_top;
    segment->ist[TSS_IST_DOUBLE_FAULT - 1U] = double_fault_stack_top;

    /*
     * The map base is set beyond the limit, which the architecture defines as
     * the absence of an I/O permission bit map. Every I/O instruction executed
     * at a privilege level above IOPL then raises a general-protection
     * exception — which is what this kernel wants of a user program, the ports
     * being the kernel's to drive.
     */
    segment->io_map_base = (uint16_t)sizeof(TaskStateSegment);

    GdtInstallTaskStateSegment(processor_index, (uint64_t)(uintptr_t)segment,
                               TssLimit());

    /*
     * The task register is loaded last, the descriptor having to exist before a
     * selector for it can be accepted. LTR marks the descriptor busy, so its
     * type changes from 9 to 11 at this instruction and the self-test asserts
     * the changed value: that is the only evidence available that the processor
     * read the descriptor rather than merely that the selector was written.
     *
     * The selector is held in a uint16_t of its own rather than named directly,
     * so that `%0` expands to a 16-bit register.
     *
     * Intel SDM, Volume 2A, "LTR", defines the instruction upon r/m16 and upon
     * nothing else. `GDT_TSS_SELECTOR` is a UINT16_C constant, but it promotes
     * to int in the operand, so naming it expanded `%0` to `%eax` and emitted
     * `ltr %eax`. GNU as accepts that and assembles `0F 00 D8` — the correct
     * encoding, and the only one the instruction has — so the kernel this
     * produced was never wrong. But the source was relying upon the assembler
     * being lenient about a register name the instruction does not admit, which
     * clang's integrated assembler refuses outright with "invalid operand for
     * instruction". Both compilers now name a 16-bit register — GCC `ltr %ax`
     * and clang `ltrw %ax` — so the operand size is stated rather than inferred,
     * and the encoding is unchanged.
     */
    const uint16_t selector = GdtTaskStateSegmentSelector(processor_index);

    __asm__ __volatile__("ltr %0" : : "r"(selector) : "memory");
}

void TssSetKernelStack(uint64_t stack_top)
{
    TssCurrent()->rsp0 = stack_top;
}

uint64_t TssKernelStack(void)
{
    return TssCurrent()->rsp0;
}

uint64_t TssInterruptStack(unsigned int entry)
{
    if ((entry == 0U) || (entry > 7U))
    {
        return 0U;
    }

    return TssCurrent()->ist[entry - 1U];
}

uint16_t TssIoMapBase(void)
{
    return TssCurrent()->io_map_base;
}

const TaskStateSegment *TssAddress(void)
{
    return TssCurrent();
}

uint32_t TssLimit(void)
{
    /*
     * The limit is one less than the size, as every segment limit is. It must
     * cover the whole segment and no more: a limit short of the I/O map base
     * would place that field outside the segment, and a limit beyond the
     * structure would admit an I/O permission bit map that does not exist.
     */
    return (uint32_t)(sizeof(TaskStateSegment) - 1U);
}

uint16_t TssTaskRegister(void)
{
    uint16_t selector;

    __asm__ __volatile__("str %0" : "=r"(selector));

    return selector;
}

void TssReport(void)
{
    KernelWriteString("Task state segment: at ");
    KernelWriteHexadecimal((uint64_t)(uintptr_t)TssCurrent());
    KernelWriteString(", limit ");
    KernelWriteDecimal((uint64_t)TssLimit());
    KernelWriteString(", task register ");
    KernelWriteHexadecimal((uint64_t)TssTaskRegister());
    KernelWriteString(".\n");

    KernelWriteString("  RSP0 ");
    KernelWriteHexadecimal(TssCurrent()->rsp0);
    KernelWriteString(" (");
    KernelWriteDecimal((uint64_t)TSS_KERNEL_STACK_SIZE / 1024U);
    KernelWriteString(" KiB), IST");
    KernelWriteDecimal((uint64_t)TSS_IST_DOUBLE_FAULT);
    KernelWriteString(" ");
    KernelWriteHexadecimal(TssCurrent()->ist[TSS_IST_DOUBLE_FAULT - 1U]);
    KernelWriteString(" (double fault, ");
    KernelWriteDecimal((uint64_t)TSS_INTERRUPT_STACK_SIZE / 1024U);
    KernelWriteString(" KiB).\n");

    KernelWriteString("  I/O permission map base ");
    KernelWriteDecimal((uint64_t)TssCurrent()->io_map_base);
    KernelWriteString(", beyond the limit: no port is permitted to user mode.\n");
}
