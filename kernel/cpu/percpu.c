/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/cpu/percpu.c
 * Purpose: Implements the per-processor data areas: their static allocation,
 *          the establishment of the executing processor's own and the segment
 *          base through which it is reached, and the counted interrupt-disable
 *          that every critical section in the kernel is built upon.
 * Key functions: PerCpuInitialise, PerCpuAt, PerCpuOnlineCount,
 *          PerCpuIsEstablished, PerCpuPushInterruptState,
 *          PerCpuPopInterruptState, PerCpuResetInterruptState, PerCpuReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 3.4.4: the FS and GS bases in 64-bit mode.
 *   - Intel SDM, Volume 3A, Table 2-1: IA32_GS_BASE at 0xC0000101 and
 *     IA32_KERNEL_GS_BASE at 0xC0000102.
 *   - Intel SDM, Volume 2A, "CPUID": leaf 1, EBX bits 31:24, the initial APIC
 *     identifier of the executing logical processor.
 *   - Intel SDM, Volume 2B, "CLI" and "STI": the interrupt flag, and that STI's
 *     effect is delayed by one instruction.
 *   - docs/design/CONCURRENCY.md, Sections 3 and 4.
 *
 * Why the areas are a static array and not an allocation.
 *
 *   The area of a processor is reached by the first spinlock that processor
 *   takes, and upon the bootstrap processor that happens before the frame
 *   allocator has been built. There is nothing to allocate from at that moment,
 *   and a failure would have to be reported through a path that itself takes a
 *   lock. Sixty-four areas of some eighty bytes is five kilobytes of the kernel
 *   image, which is the whole cost of never having to answer the question.
 */

#include <oxys/percpu.h>
#include <oxys/msr.h>
#include <oxys/cpu.h>
#include <oxys/lapic.h>
#include <oxys/kernel.h>

#include <stddef.h>

/*
 * The areas, and the count of those in use.
 *
 * The array is written only by PerCpuInitialise, which runs once upon each
 * processor as it starts, and the count only there. Sub-task 6.14 calls it from
 * an application processor, at which point two processors could in principle
 * claim the same index; the claim is therefore made by a locked exchange upon
 * the count rather than by an ordinary increment. It cannot use a spinlock: a
 * spinlock acquire reaches the area this function is in the middle of creating.
 */
static PerCpu PerCpuAreas[PER_CPU_MAXIMUM];
static volatile uint32_t PerCpuEstablishedCount;

/*
 * The assembly of kernel/cpu/syscall_entry.asm addresses the first two fields by
 * number, and PerCpuCurrent the third, none of them having sight of this
 * structure. A field inserted above them would put the caller's stack pointer
 * where the kernel stack belongs, and the instruction after the one that reads
 * it loads RSP — which is a kernel running upon an address privilege level 3
 * chose.
 */
_Static_assert(offsetof(PerCpu, kernel_stack) == 0U,
               "The system-call entry path reads the kernel stack at offset 0.");
_Static_assert(offsetof(PerCpu, user_stack) == 8U,
               "The system-call entry path writes the caller's stack at offset 8.");
_Static_assert(offsetof(PerCpu, self) == 16U,
               "PerCpuCurrent reads the self pointer at offset 16.");

/*
 * The initial APIC identifier of the executing processor.
 *
 * CPUID is used rather than the local controller's identifier register because
 * the area is established before the controller has been mapped — the frame
 * allocator, and therefore the kernel arena the controller's register page is
 * mapped into, does not exist yet. Intel SDM, Volume 2A, records that leaf 1
 * returns this value in EBX bits 31:24 and that it is assigned at reset, so it
 * is available at every moment after one.
 *
 * The instruction is written out here rather than taken from <cpuid.h>, which is
 * a compiler-supplied header rather than a standard one; see the same reasoning
 * in drivers/apic/lapic.c. RBX is named as an output rather than clobbered
 * because it is otherwise the compiler's base pointer under some models.
 */
static uint32_t PerCpuInitialApicIdentifier(void)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;

    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(UINT32_C(1)), "c"(UINT32_C(0)));

    return ebx >> 24;
}

/*
 * Claims the next free index.
 *
 * A locked exchange-and-add, because at sub-task 6.14 two application processors
 * may execute this at once and an ordinary increment would hand both the same
 * area. Intel SDM, Volume 3A, Section 8.1.2.2, provides that the LOCK prefix
 * makes the read-modify-write of the destination atomic with respect to every
 * other processor.
 *
 * The returned value is the index claimed. A value at or above PER_CPU_MAXIMUM
 * means the machine has more processors than this kernel reserves areas for; the
 * count is left as it is rather than being wound back, so that a second caller
 * in the same condition is refused as well.
 */
static uint32_t PerCpuClaimIndex(void)
{
    uint32_t claimed = 1U;

    __asm__ __volatile__("lock xaddl %0, %1"
                         : "+r"(claimed), "+m"(PerCpuEstablishedCount)
                         :
                         : "memory", "cc");

    return claimed;
}

bool PerCpuInitialise(void)
{
    const uint32_t index = PerCpuClaimIndex();
    PerCpu *area;

    if (index >= PER_CPU_MAXIMUM)
    {
        return false;
    }

    area = &PerCpuAreas[index];

    area->self = area;
    area->index = index;
    area->apic_identifier = PerCpuInitialApicIdentifier();

    /*
     * Bit 8 of IA32_APIC_BASE is the bootstrap processor flag, per Intel SDM,
     * Volume 3A, Figure 10-5. It is read here rather than taken from the local
     * controller driver because the driver has not been initialised at this
     * point and reads nothing until it has; the register itself is readable from
     * reset.
     */
    area->bootstrap = (ReadMsr(IA32_APIC_BASE) & LAPIC_BASE_BOOTSTRAP_PROCESSOR) != 0U;

    /*
     * The stack is not known here.
     *
     * The task state segment has not been built when the bootstrap processor's
     * area is established, so the kernel stack is written by SyscallInitialise
     * once it is, and thereafter by every context switch. Zero until then is
     * correct and is also safe: nothing reads the field before privilege level 3
     * exists, and there is no privilege level 3 until sub-task 6.10's descent.
     */
    area->kernel_stack = 0U;
    area->user_stack = 0U;

    area->critical_depth = 0U;
    area->interrupts_were_enabled = false;
    area->locks_held = 0U;

    /*
     * GS.base, and not IA32_KERNEL_GS_BASE.
     *
     * The invariant is that GS.base holds the area whenever kernel code is
     * executing and IA32_KERNEL_GS_BASE holds it whenever a user program is, the
     * two being exchanged by SWAPGS at each boundary. This function runs in the
     * kernel, so the area goes into GS.base; the other register is cleared for
     * the same reason a user program's value is zero — nothing has given it one.
     */
    WriteMsr(IA32_GS_BASE, (uint64_t)(uintptr_t)area);
    WriteMsr(IA32_KERNEL_GS_BASE, 0U);

    /* Written last. A processor is online once everything a reader of its area
     * would look at has been written, and not before. */
    area->online = true;

    return true;
}

/*
 * Finds the area belonging to a processor by the identifier its controller
 * answers to.
 *
 * It is the only way to reach one's own area without GS.base, and therefore the
 * only way to repair GS.base. The identifier is the right key because it is
 * assigned by the hardware at reset and is unique among the processors of a
 * machine; the dense index is this kernel's own numbering and could not be
 * recovered from anything the processor knows about itself.
 */
static PerCpu *PerCpuFindByApicIdentifier(uint32_t identifier)
{
    const uint32_t count = PerCpuOnlineCount();

    for (uint32_t index = 0U; index < count; ++index)
    {
        if (PerCpuAreas[index].online &&
            PerCpuAreas[index].apic_identifier == identifier)
        {
            return &PerCpuAreas[index];
        }
    }

    return NULL;
}

bool PerCpuEstablishSegmentBase(void)
{
    PerCpu *const area = PerCpuFindByApicIdentifier(PerCpuInitialApicIdentifier());

    if (area == NULL)
    {
        return false;
    }

    WriteMsr(IA32_GS_BASE, (uint64_t)(uintptr_t)area);
    WriteMsr(IA32_KERNEL_GS_BASE, 0U);

    return true;
}

PerCpu *PerCpuAt(uint32_t index)
{
    if (index >= PER_CPU_MAXIMUM || index >= PerCpuEstablishedCount)
    {
        return NULL;
    }

    return &PerCpuAreas[index];
}

uint32_t PerCpuOnlineCount(void)
{
    const uint32_t established = PerCpuEstablishedCount;

    return established > PER_CPU_MAXIMUM ? PER_CPU_MAXIMUM : established;
}

bool PerCpuIsEstablished(void)
{
    return PerCpuEstablishedCount != 0U;
}

void PerCpuPushInterruptState(void)
{
    const bool were_enabled = InterruptsAreEnabled();
    PerCpu *area;

    /*
     * The flag is read before it is cleared, and the area reached after.
     *
     * Reading first is the whole point: what is being recorded is the state the
     * caller was in, and clearing first would record the state this function
     * produced. Reaching the area afterwards is deliberate too — with interrupts
     * still enabled a handler could run between the load of GS.base and the use
     * of what it produced, and while that cannot presently move the flow of
     * control to another processor, it will once there is a scheduler, and the
     * ordering costs nothing now.
     */
    __asm__ __volatile__("cli" : : : "memory");

    area = PerCpuCurrent();

    if (area->critical_depth == 0U)
    {
        area->interrupts_were_enabled = were_enabled;
    }

    ++area->critical_depth;
}

void PerCpuPopInterruptState(void)
{
    PerCpu *const area = PerCpuCurrent();

    /*
     * Interrupts enabled here mean something within the section executed STI,
     * and the section was therefore not the critical section it claimed to be.
     * The damage is done by then; what this catches is the code that did it,
     * rather than the corruption it produces somewhere else later.
     */
    if (InterruptsAreEnabled())
    {
        KernelPanic("A critical section was left with interrupts already enabled.");
    }

    if (area->critical_depth == 0U)
    {
        KernelPanic("A critical section was left that had not been entered.");
    }

    --area->critical_depth;

    if (area->critical_depth == 0U && area->interrupts_were_enabled)
    {
        __asm__ __volatile__("sti" : : : "memory");
    }
}

/*
 * Declares that the executing processor holds no critical section, and enables
 * interrupts.
 *
 * **It exists for exactly one caller and would be a defect anywhere else.** The
 * counted interrupt-disable is a property of the processor, not of the thread,
 * and the scheduler of sub-task 6.15 switches threads from inside one: it masks
 * interrupts, chooses, and switches. A thread that is *resumed* by that switch
 * carries on inside its own PerCpuPushInterruptState and executes the matching
 * pop, so the count comes out even. A thread that has **never run** has no such
 * pop: it begins at a prepared frame, and would begin it with the depth and the
 * flag of the thread that gave it the processor — running with interrupts masked
 * for ever, taking no timer tick, and never being pre-empted again.
 *
 * That failure was met and is why this exists. It presents as a machine that
 * hangs the first time the scheduler starts a thread, with no fault and nothing
 * in the log.
 *
 * The count comes out even across the machine because every push still has a
 * pop somewhere: the pushing thread executes its own when it is next resumed,
 * against whatever depth the resuming processor then holds.
 */
void PerCpuResetInterruptState(void)
{
    PerCpu *const area = PerCpuCurrent();

    if (area == NULL)
    {
        return;
    }

    area->critical_depth = 0U;
    area->interrupts_were_enabled = true;

    __asm__ __volatile__("sti" : : : "memory");
}

uint32_t PerCpuCriticalDepth(void)
{
    return PerCpuCurrent()->critical_depth;
}

uint32_t PerCpuLocksHeld(void)
{
    return PerCpuCurrent()->locks_held;
}

void PerCpuReport(void)
{
    const uint32_t count = PerCpuOnlineCount();

    KernelWriteString("Per-processor areas: ");
    KernelWriteDecimal((uint64_t)count);
    KernelWriteString(count == 1U ? " processor online.\n" : " processors online.\n");

    for (uint32_t index = 0U; index < count; ++index)
    {
        const PerCpu *const area = &PerCpuAreas[index];

        KernelWriteString("  Processor ");
        KernelWriteDecimal((uint64_t)area->index);
        KernelWriteString(": APIC identifier ");
        KernelWriteDecimal((uint64_t)area->apic_identifier);
        KernelWriteString(area->bootstrap ? " (bootstrap)" : " (application)");
        KernelWriteString(", kernel stack ");
        KernelWriteHexadecimal(area->kernel_stack);
        KernelWriteString(", critical depth ");
        KernelWriteDecimal((uint64_t)area->critical_depth);
        KernelWriteString(", locks held ");
        KernelWriteDecimal((uint64_t)area->locks_held);
        KernelWriteString(".\n");

        KernelWriteString("  Processor ");
        KernelWriteDecimal((uint64_t)area->index);
        KernelWriteString(": acquisitions ");
        KernelWriteDecimal(area->locks_acquired);
        KernelWriteString(", contentions ");
        KernelWriteDecimal(area->lock_contentions);
        KernelWriteString(", interprocessor interrupts received ");
        KernelWriteDecimal(area->ipis_received);
        KernelWriteString(", shootdowns serviced ");
        KernelWriteDecimal(area->shootdowns_serviced);
        KernelWriteString(".\n");
    }
}
