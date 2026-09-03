/*
 * File: kernel/cpu/tss.c
 * Purpose: Establishes the task state segment: the stacks it names, its
 *          descriptor within the global descriptor table, and the loading of the
 *          task register.
 * Key functions: TssInitialise, TssSetKernelStack, TssKernelStack,
 *          TssInterruptStack, TssAddress, TssLimit, TssTaskRegister, TssReport.
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
 * Concurrency. There is one segment because there is one processor. From
 * sub-task 6.9 each processor requires a segment of its own, with its own stacks
 * and its own descriptor, because RSP0 names the stack of whatever is running
 * upon *that* processor; a shared segment would deliver two system calls upon
 * one stack. The task register is per-processor already, so what must be
 * duplicated is the storage and not the mechanism.
 */

#include <oxys/tss.h>
#include <oxys/gdt.h>
#include <oxys/kernel.h>

/*
 * The segment itself.
 *
 * It is not const: the processor reads it, and this kernel writes RSP0 at every
 * privilege transition from sub-task 6.4. It is aligned so that no field crosses
 * a cache line unnecessarily, the processor reading RSP0 upon every entry from
 * user mode.
 */
static TaskStateSegment Tss __attribute__((aligned(16)));

/*
 * The stack the processor loads upon entering privilege level 0 from level 3.
 *
 * It is reserved in .bss rather than taken from the kernel heap because it must
 * exist before anything that could fail does, and because the address must not
 * change: the processor holds it in a structure it reads without asking.
 *
 * There is no guard page beneath it. The kernel arena of sub-task 2.5 could
 * supply one, and sub-task 6.4 will need to when each thread has a stack of its
 * own and the stacks are numerous enough that an overflow becomes likely; until
 * then an overflow runs into the .bss below, which is the double-fault stack,
 * and the double fault that results is delivered upon a stack of its own and is
 * therefore reported. That is an accident rather than a design, and it is
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
    for (size_t index = 0U; index < sizeof Tss; ++index)
    {
        ((uint8_t *)&Tss)[index] = 0U;
    }

    Tss.rsp0 = TssStackTop(TssKernelStackStore, sizeof TssKernelStackStore);
    Tss.ist[TSS_IST_DOUBLE_FAULT - 1U] =
        TssStackTop(TssDoubleFaultStackStore, sizeof TssDoubleFaultStackStore);

    /*
     * The map base is set beyond the limit, which the architecture defines as
     * the absence of an I/O permission bit map. Every I/O instruction executed
     * at a privilege level above IOPL then raises a general-protection
     * exception — which is what this kernel wants of a user program, the ports
     * being the kernel's to drive.
     */
    Tss.io_map_base = (uint16_t)sizeof(TaskStateSegment);

    GdtInstallTaskStateSegment((uint64_t)(uintptr_t)&Tss, TssLimit());

    /*
     * The task register is loaded last, the descriptor having to exist before a
     * selector for it can be accepted. LTR marks the descriptor busy, so its
     * type changes from 9 to 11 at this instruction and the self-test asserts
     * the changed value: that is the only evidence available that the processor
     * read the descriptor rather than merely that the selector was written.
     */
    __asm__ __volatile__("ltr %0" : : "r"(GDT_TSS_SELECTOR) : "memory");
}

void TssSetKernelStack(uint64_t stack_top)
{
    Tss.rsp0 = stack_top;
}

uint64_t TssKernelStack(void)
{
    return Tss.rsp0;
}

uint64_t TssInterruptStack(unsigned int entry)
{
    if ((entry == 0U) || (entry > 7U))
    {
        return 0U;
    }

    return Tss.ist[entry - 1U];
}

uint16_t TssIoMapBase(void)
{
    return Tss.io_map_base;
}

const TaskStateSegment *TssAddress(void)
{
    return &Tss;
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
    KernelWriteHexadecimal((uint64_t)(uintptr_t)&Tss);
    KernelWriteString(", limit ");
    KernelWriteDecimal((uint64_t)TssLimit());
    KernelWriteString(", task register ");
    KernelWriteHexadecimal((uint64_t)TssTaskRegister());
    KernelWriteString(".\n");

    KernelWriteString("  RSP0 ");
    KernelWriteHexadecimal(Tss.rsp0);
    KernelWriteString(" (");
    KernelWriteDecimal((uint64_t)TSS_KERNEL_STACK_SIZE / 1024U);
    KernelWriteString(" KiB), IST");
    KernelWriteDecimal((uint64_t)TSS_IST_DOUBLE_FAULT);
    KernelWriteString(" ");
    KernelWriteHexadecimal(Tss.ist[TSS_IST_DOUBLE_FAULT - 1U]);
    KernelWriteString(" (double fault, ");
    KernelWriteDecimal((uint64_t)TSS_INTERRUPT_STACK_SIZE / 1024U);
    KernelWriteString(" KiB).\n");

    KernelWriteString("  I/O permission map base ");
    KernelWriteDecimal((uint64_t)Tss.io_map_base);
    KernelWriteString(", beyond the limit: no port is permitted to user mode.\n");
}
