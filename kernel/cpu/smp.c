/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/cpu/smp.c
 * Purpose: Implements the bring-up of the application processors: the placement
 *          and mapping of the real-mode trampoline, the preparation each
 *          processor is given before it is started, the INIT-startup-startup
 *          sequence that starts it, the bounded wait for it to answer, and what
 *          it does once it has.
 * Key functions: SmpInitialise, SmpApplicationProcessorEntry,
 *          SmpProcessorsStarted, SmpProcessorsRefused, SmpStartupFailureCount,
 *          SmpIsMultiprocessor, SmpDeclinedReason, SmpReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 8.4.4.1 ("Typical BSP Initialization Sequence"): send an INIT
 *     inter-processor interrupt, wait ten milliseconds, send a startup
 *     interrupt, wait two hundred microseconds, and where the processor has not
 *     answered, send a second startup interrupt.
 *   - Intel SDM, Volume 3A, Section 8.4.3: the startup interrupt carries a
 *     vector VV and the processor that answers it begins executing at physical
 *     address 000VV000H, in real mode.
 *   - Intel SDM, Volume 3A, Section 8.4.4.1: every device capable of delivering
 *     an interrupt must be inhibited between the INIT and the last startup
 *     interrupt of a sequence.
 *   - Intel SDM, Volume 3A, Section 10.6.1 and Figure 10-12: the interrupt
 *     command register; delivery mode 101 is INIT and 110 is Start-Up; bit 12
 *     is the delivery status and is read-only; the destination field is bits
 *     31:24 of the high half in xAPIC mode.
 *   - Intel SDM, Volume 3A, Section 4.10.4.4: an invalidation may be deferred
 *     only while no processor can use the stale translation, which is why the
 *     trampoline's mapping is removed through the shootdown of sub-task 6.13
 *     rather than by clearing an entry.
 *   - ACPI Specification 6.5, Section 5.2.12.2 and Table 5.23: the Processor
 *     Local APIC structure, and the two flags by which a processor is usable.
 *   - Multiboot2 Specification 2.0, Section 3.6.8: the memory map, from which
 *     the trampoline page is proved available before anything is copied to it.
 *   - docs/design/SMP.md: the design and the reasoning.
 *
 * The one rule this file is built around.
 *
 *   **A starting processor allocates nothing, maps nothing and claims nothing.**
 *   Every resource it will need — its stack, its double-fault stack, its task
 *   state segment descriptor — is prepared by the bootstrap processor before
 *   the startup interrupt is sent, and handed over through the parameter block
 *   or found by index. The reason is the state of this kernel at this sub-task:
 *   the frame allocator, the kernel arena and the heap are all unsynchronised,
 *   and docs/design/CONCURRENCY.md, Section 10, limitation 1, says so. A
 *   processor that allocated its own stack would be the second processor in an
 *   allocator that admits one, at the one moment when a corruption would be
 *   indistinguishable from a processor that simply failed to start.
 *
 *   The rule also decides what a started processor may do afterwards, which is
 *   very little: it answers inter-processor interrupts and halts. Sub-task 6.15
 *   is what gives it work, and the locks the structures above need are what it
 *   must bring with it.
 *
 * Concurrency. The counters below are written by the bootstrap processor alone,
 * which is the only processor that runs this file's bring-up half; the entry
 * point runs upon the starting processor and writes nothing here but through
 * the per-processor area, which is its own. The preparation table is written by
 * the bootstrap processor before a processor is started and read by that
 * processor afterwards, and the startup interrupt between the two is the
 * ordering: Intel SDM, Volume 3A, Section 8.2.2, provides that stores are not
 * reordered with other stores, so everything written before the command
 * register is visible to a processor that woke because of it.
 */

#include <oxys/smp.h>
#include <oxys/percpu.h>
#include <oxys/spinlock.h>
#include <oxys/ipi.h>
#include <oxys/shootdown.h>
#include <oxys/lapic.h>
#include <oxys/acpi.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>
#include <oxys/gdt.h>
#include <oxys/idt.h>
#include <oxys/tss.h>
#include <oxys/syscall.h>
#include <oxys/pit.h>
#include <oxys/cpu.h>
#include <oxys/framebuffer.h>
#include <oxys/sched.h>
#include <oxys/memory.h>
#include <oxys/verify.h>
#include <oxys/kernel.h>

/* The two ends of the assembled trampoline, embedded by
 * kernel/cpu/smp_trampoline.asm. */
extern const uint8_t SmpTrampolineImageStart[];
extern const uint8_t SmpTrampolineImageEnd[];

/*
 * The delays the protocol prescribes, and the bound upon the wait for an answer.
 *
 * The first two are Intel SDM, Volume 3A, Section 8.4.4.1, verbatim. The third
 * is this kernel's own: a processor that has answered the startup interrupt
 * reaches 64-bit mode within a few thousand instructions, so a hundred
 * milliseconds is four orders of magnitude of margin and is short enough that a
 * machine declaring a processor it does not have still finishes booting.
 */
#define SMP_INIT_DELAY_MICROSECONDS    UINT32_C(10000)
#define SMP_STARTUP_DELAY_MICROSECONDS UINT32_C(200)
#define SMP_ANSWER_WAIT_MICROSECONDS   UINT32_C(100000)

/* How often the wait for an answer looks, in microseconds. Short enough that
 * the wait costs little when a processor answers at once, long enough that the
 * loop is not itself the thing consuming the bus. */
#define SMP_ANSWER_POLL_MICROSECONDS UINT32_C(200)

/*
 * What the bootstrap processor prepares for each processor it is about to start.
 *
 * The stacks are held here rather than passed through the parameter block
 * because only one of them can be: the block hands over the stack the entry
 * point begins upon, and the entry point needs the other two facts — the same
 * stack again, to give the task state segment, and the double-fault stack — at
 * a point where the block has already been overwritten for the next processor.
 * The index is the key, and the index is what the block carries.
 */
typedef struct SmpPreparation
{
    uint64_t stack_top;
    uint64_t double_fault_stack_top;
    bool prepared;
} SmpPreparation;

static SmpPreparation SmpPreparations[PER_CPU_MAXIMUM];

/*
 * What each started processor recorded about itself before it parked.
 *
 * Each entry is written by the processor it belongs to and by no other, so it
 * needs no lock; it is read by the bootstrap processor afterwards, and the
 * ordering is the same one the whole of this file relies upon — the processor
 * has brought its area online by then, and the count that says so is written
 * after everything a reader would look at.
 */
static SmpProcessorRecord SmpRecords[PER_CPU_MAXIMUM];

/* The accounting, written by the bootstrap processor alone. */
static uint32_t SmpStarted;
static uint32_t SmpRefused;
static uint32_t SmpFailures;
static const char *SmpDeclined;
static bool SmpTrampolineMapped;

/*
 * The parameter block, addressed through the page the trampoline was copied to.
 *
 * It is volatile because the acknowledgement field is written by another
 * processor and read here in a loop, and a compiler entitled to assume no other
 * writer would hoist the read out of the loop and spin upon a register.
 */
static volatile SmpTrampolineParameters *SmpParameters(void)
{
    return (volatile SmpTrampolineParameters *)(uintptr_t)(
        SMP_TRAMPOLINE_ADDRESS + SMP_TRAMPOLINE_PARAMETER_OFFSET);
}

/*
 * Whether the firmware's memory map declares the trampoline page available, and
 * whether anything this kernel already owns stands within it.
 *
 * The page is fixed rather than allocated, for the reason
 * kernel/include/oxys/smp.h gives, so this is the assertion that the fixture is
 * sound upon *this* machine. It is not a formality: a firmware that reserved the
 * page — for a memory-mapped device, or for something it means to be called back
 * through — would have this kernel copy a trampoline over it and then start
 * processors into whatever remained. That is a machine that fails in the
 * firmware's code with this kernel's name upon the screen.
 */
static bool SmpTrampolinePageIsUsable(void)
{
    const PhysicalAddress start = SMP_TRAMPOLINE_ADDRESS;
    const PhysicalAddress end = SMP_TRAMPOLINE_ADDRESS + PAGE_SIZE;
    bool declared_available = false;

    for (size_t index = 0U; index < KernelBootInformation.memory_region_count;
         ++index)
    {
        const BootMemoryRegion *const region =
            &KernelBootInformation.memory_regions[index];
        const PhysicalAddress region_end = region->base_address + region->length;

        if ((region->type == BOOT_MEMORY_USABLE) &&
            (region->base_address <= start) && (region_end >= end))
        {
            declared_available = true;
            break;
        }
    }

    if (!declared_available)
    {
        return false;
    }

    /*
     * The kernel image and the boot information structure both lie within
     * regions the map calls available — Multiboot2 Specification, Section 3.6.8,
     * says as much in terms — so "available" is necessary and not sufficient.
     * kernel/mm/pmm.c excludes both from the allocator for exactly this reason,
     * and the same two exclusions are applied here because this page is not
     * obtained from the allocator.
     */
    if ((start < KernelBootInformation.kernel_physical_end) &&
        (end > KernelBootInformation.kernel_physical_start))
    {
        return false;
    }

    if ((start < KernelBootInformation.boot_information_end) &&
        (end > KernelBootInformation.boot_information_start))
    {
        return false;
    }

    return true;
}

/*
 * Maps the trampoline page into the kernel hierarchy at its own address.
 *
 * The mapping is an identity mapping and is the only one this kernel has: the
 * one boot/boot.asm established was removed the instant sub-task 2.3 wrote CR3,
 * and PagingReport announces its absence upon every boot. It comes back for the
 * duration of the bring-up and no longer, because the instruction after the one
 * that enables paging upon a starting processor executes at the address it
 * already had — which is this page — and an unmapped instruction pointer at that
 * moment is a triple fault with nothing reported.
 *
 * It is a hazard for as long as it stands, and the hazard is stated rather than
 * accepted quietly: a stray low pointer dereferenced by the kernel while it
 * stands reads and writes real memory instead of faulting. The window is the
 * bring-up alone, the bring-up is a few milliseconds of a boot, and the removal
 * below is what closes it.
 */
static void SmpMapTrampolinePage(void)
{
    PagingMapKernelPage(SMP_TRAMPOLINE_ADDRESS, SMP_TRAMPOLINE_ADDRESS,
                        PAGE_ENTRY_WRITABLE);
    SmpTrampolineMapped = true;
}

/*
 * Removes it again — which is the first shootdown this kernel performs that has
 * anybody to send to.
 *
 * PagingUnmapKernelPage invalidates through ShootdownBroadcast, so every
 * processor started above is interrupted, discards its translation of this page
 * and acknowledges before this returns. Until this sub-task that broadcast
 * returned at once for want of an audience; from here it is the mechanism
 * sub-task 6.13 built, doing the thing it was built for.
 *
 * The order is deliberate. The mapping is removed after every processor has
 * come online and therefore after every processor has left this page for the
 * higher half — a processor still executing here would lose the ground beneath
 * it. A processor that never answered has not been started at all and cannot be
 * executing anywhere.
 */
static void SmpUnmapTrampolinePage(void)
{
    if (!SmpTrampolineMapped)
    {
        return;
    }

    PagingUnmapKernelPage(SMP_TRAMPOLINE_ADDRESS);
    SmpTrampolineMapped = false;
}

/*
 * Copies the assembled image into the page and proves that it arrived where the
 * C believes it did.
 *
 * The magic is the proof. boot/trampoline.asm lays the parameter block out in
 * assembly and kernel/include/oxys/smp.h lays a structure over it, and neither
 * translation unit can see the other: the only thing that can catch a
 * disagreement between them is a value the assembler wrote and the C reads back
 * through the structure. Without it, a field inserted or resized on one side
 * would send a processor to whatever address the entry-point field had come to
 * overlap — a jump into arbitrary memory, upon a processor with no diagnostic
 * channel of its own, which is as close to unreportable as this kernel gets.
 */
static bool SmpPlaceTrampoline(void)
{
    const size_t length = (size_t)(SmpTrampolineImageEnd - SmpTrampolineImageStart);
    uint8_t *const destination = (uint8_t *)(uintptr_t)SMP_TRAMPOLINE_ADDRESS;

    if ((length == 0U) || (length > PAGE_SIZE))
    {
        SmpDeclined = "the trampoline image does not fit the page reserved for it";
        return false;
    }

    for (size_t index = 0U; index < length; ++index)
    {
        destination[index] = SmpTrampolineImageStart[index];
    }

    if (SmpParameters()->magic != SMP_TRAMPOLINE_MAGIC)
    {
        SmpDeclined = "the trampoline's parameter block is not where the kernel "
                      "expects it";
        return false;
    }

    return true;
}

/*
 * Obtains the stacks a processor will run upon.
 *
 * They come from the kernel arena rather than from `.bss` because there are as
 * many of them as there are processors and the machine says how many; and they
 * come from the bootstrap processor rather than from the processor that will use
 * them because of the rule this file is built around. A guard page is not
 * requested: KernelPagesAllocate does not offer one, and the stack a processor
 * idles upon overflows only if something running upon it recurses, which upon a
 * processor with nothing to run it cannot.
 */
static bool SmpPrepareProcessor(uint32_t index)
{
    void *stack;
    void *double_fault_stack;

    if (index >= PER_CPU_MAXIMUM)
    {
        return false;
    }

    if (SmpPreparations[index].prepared)
    {
        return true;
    }

    stack = KernelPagesAllocate(SMP_PROCESSOR_STACK_PAGES);
    if (stack == NULL)
    {
        return false;
    }

    double_fault_stack = KernelPagesAllocate(SMP_PROCESSOR_STACK_PAGES);
    if (double_fault_stack == NULL)
    {
        KernelPagesFree(stack, SMP_PROCESSOR_STACK_PAGES);
        return false;
    }

    /* The stack grows downward, so the pointer handed over is one past the last
     * byte of the allocation, as it is everywhere else in this kernel. */
    SmpPreparations[index].stack_top =
        (uint64_t)(uintptr_t)stack + ((uint64_t)SMP_PROCESSOR_STACK_PAGES * PAGE_SIZE);
    SmpPreparations[index].double_fault_stack_top =
        (uint64_t)(uintptr_t)double_fault_stack +
        ((uint64_t)SMP_PROCESSOR_STACK_PAGES * PAGE_SIZE);
    SmpPreparations[index].prepared = true;

    return true;
}

/*
 * The startup sequence of Intel SDM, Volume 3A, Section 8.4.4.1.
 *
 * Every send is checked. LocalApicSendCommand waits out a command still pending
 * in the register and returns false where that wait expired, and a startup
 * sequence with one of its steps missing is worse than one not attempted: an
 * INIT that arrived without the startup interrupt that should have followed it
 * leaves a processor held in reset, which is a processor the firmware declared
 * and this kernel then removed from the machine.
 *
 * The INIT is sent with the level-assert and level-trigger bits set, which is
 * the encoding the manual's own algorithm uses — its 000C4500H less the
 * destination shorthand, this being addressed to one processor rather than
 * broadcast. The startup interrupts carry the trampoline's page number as their
 * vector.
 *
 * The second startup interrupt is sent only where the first was not answered.
 * The manual prescribes it unconditionally, and prescribing it is right for a
 * processor that may be slow; sending it to one that has already started would
 * be sending a startup interrupt to a processor executing kernel code, which
 * Section 8.4.4.1 does not define. The acknowledgement written by the trampoline
 * is what distinguishes the two, and it is written before the trampoline jumps
 * into the kernel precisely so that it can be.
 */
static bool SmpStartProcessor(uint8_t apic_identifier)
{
    const uint32_t init_command = LAPIC_ICR_DELIVERY_INIT |
                                  LAPIC_ICR_LEVEL_ASSERT |
                                  LAPIC_ICR_TRIGGER_LEVEL |
                                  LAPIC_ICR_SHORTHAND_NONE |
                                  LAPIC_ICR_DESTINATION_PHYSICAL;
    const uint32_t startup_command = LAPIC_ICR_DELIVERY_STARTUP |
                                     LAPIC_ICR_LEVEL_ASSERT |
                                     LAPIC_ICR_SHORTHAND_NONE |
                                     LAPIC_ICR_DESTINATION_PHYSICAL |
                                     (uint32_t)SMP_TRAMPOLINE_VECTOR;
    uint32_t waited;

    SmpParameters()->acknowledged = 0U;

    if (!LocalApicSendCommand(apic_identifier, init_command))
    {
        return false;
    }

    if (!PitBusyWaitMicroseconds(SMP_INIT_DELAY_MICROSECONDS))
    {
        return false;
    }

    if (!LocalApicSendCommand(apic_identifier, startup_command))
    {
        return false;
    }

    if (!PitBusyWaitMicroseconds(SMP_STARTUP_DELAY_MICROSECONDS))
    {
        return false;
    }

    if (SmpParameters()->acknowledged == 0U)
    {
        if (!LocalApicSendCommand(apic_identifier, startup_command))
        {
            return false;
        }
    }

    /*
     * The wait is for the trampoline's acknowledgement and not for the
     * per-processor area, because the two answer different questions. This one
     * says the processor answered the interrupt and reached 64-bit mode upon
     * this kernel's hierarchy; the area says it got through the kernel's own
     * initialisation afterwards. A processor that satisfies the first and not
     * the second failed somewhere this kernel wrote, and that is worth being
     * able to tell apart from one that never woke.
     */
    for (waited = 0U; waited < SMP_ANSWER_WAIT_MICROSECONDS;
         waited += SMP_ANSWER_POLL_MICROSECONDS)
    {
        if (SmpParameters()->acknowledged != 0U)
        {
            return true;
        }

        if (!PitBusyWaitMicroseconds(SMP_ANSWER_POLL_MICROSECONDS))
        {
            return false;
        }
    }

    return SmpParameters()->acknowledged != 0U;
}

/* Waits for a started processor to finish the kernel's half of its
 * initialisation, which it announces by bringing its area online. */
static bool SmpWaitForOnline(uint32_t expected_count)
{
    for (uint32_t waited = 0U; waited < SMP_ANSWER_WAIT_MICROSECONDS;
         waited += SMP_ANSWER_POLL_MICROSECONDS)
    {
        if (PerCpuOnlineCount() >= expected_count)
        {
            return true;
        }

        if (!PitBusyWaitMicroseconds(SMP_ANSWER_POLL_MICROSECONDS))
        {
            return false;
        }
    }

    return PerCpuOnlineCount() >= expected_count;
}

/* Writes a decimal number into a buffer and returns the number of characters,
 * so that a whole line may be composed before any of it is written. */
static size_t SmpFormatDecimal(char *buffer, size_t capacity, uint64_t value)
{
    char digits[21];
    size_t count = 0U;
    size_t written = 0U;

    do
    {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (count < sizeof digits));

    while ((count > 0U) && (written < capacity))
    {
        buffer[written++] = digits[--count];
    }

    return written;
}

/*
 * A started processor announces itself in one call.
 *
 * The line is composed before any of it is written, because the diagnostic lock
 * of kernel/kernel.c governs a call and the thing that must not interleave is a
 * line. Two processors arriving at once would otherwise produce "Processor
 * Processor 1 online.2 online." — which is not merely untidy: the count of
 * processors is read out of this log by a person, and a log that has to be
 * reassembled before it can be read is one whose figures cannot be trusted.
 */
static void SmpAnnounceArrival(uint32_t index, uint32_t apic_identifier)
{
    char line[96];
    size_t length = 0U;
    static const char prefix[] = "  Processor ";
    static const char middle[] = " is online, local controller identifier ";
    static const char suffix[] = ".\n";

    for (size_t at = 0U; (prefix[at] != '\0') && (length < sizeof line - 1U); ++at)
    {
        line[length++] = prefix[at];
    }

    length += SmpFormatDecimal(&line[length], (sizeof line - 1U) - length, index);

    for (size_t at = 0U; (middle[at] != '\0') && (length < sizeof line - 1U); ++at)
    {
        line[length++] = middle[at];
    }

    length +=
        SmpFormatDecimal(&line[length], (sizeof line - 1U) - length, apic_identifier);

    for (size_t at = 0U; (suffix[at] != '\0') && (length < sizeof line - 1U); ++at)
    {
        line[length++] = suffix[at];
    }

    line[length] = '\0';

    /*
     * One call, and therefore no lock of this file's own. KernelWriteString's
     * critical section is a call, so a line written by one call cannot be
     * interleaved with another; a second lock here would protect nothing and
     * would be a second thing to take in the right order.
     */
    KernelWriteString(line);
}

_Noreturn void SmpApplicationProcessorEntry(uint64_t index)
{
    const uint32_t expected = (uint32_t)index;

    /*
     * The order below is the order the dependencies impose, and each step is
     * something the trampoline could not do.
     *
     * The descriptor tables come first. The trampoline loaded a table of its own
     * from the page it was copied to, and that page is about to be unmapped; a
     * processor holding a selector into it would fault upon the next segment
     * load with the table gone. The kernel's table is loaded before anything
     * else can require one.
     */
    GdtLoadOnThisProcessor();
    IdtLoadOnThisProcessor();

    /*
     * Then the area, which is what every lock in this kernel reaches through.
     * Nothing above this line may take one, and nothing above it does.
     */
    if (!PerCpuInitialise())
    {
        /*
         * More processors than this kernel reserves areas for. It cannot be
         * reported from here — there is no area, so no lock, so no safe way to
         * write to the diagnostic channel — so the processor stops and the
         * bootstrap processor reports it as one that did not come online.
         */
        for (;;)
        {
            __asm__ __volatile__("cli; hlt");
        }
    }

    /*
     * The index the bootstrap processor prepared for, against the index actually
     * claimed. They agree because the processors are started one at a time and
     * each is waited for; they would disagree if that ever ceased to be true,
     * and the consequence would be a processor running upon a stack another
     * processor was also given. It is worth a panic rather than a report: there
     * is no correct way to continue with two processors upon one stack.
     */
    if (PerCpuIndex() != expected)
    {
        KernelPanic("A starting processor claimed an area other than the one "
                    "prepared for it.");
    }

    /*
     * The task state segment, upon the stacks prepared above. It matters here
     * and not only at sub-task 6.15: the double-fault gate names an entry of the
     * interrupt stack table, and a processor whose task register is null cannot
     * supply one — so a double fault upon a processor without a segment is a
     * triple fault, which is a reset with nothing written anywhere. This project
     * has met one of those; docs/design/INTERRUPTS.md, Section 5, is the
     * account.
     */
    TssInitialiseProcessor(expected, SmpPreparations[expected].stack_top,
                           SmpPreparations[expected].double_fault_stack_top);

    /*
     * The system-call configuration, which is four model-specific registers and
     * every one of them per processor. This processor will not execute SYSCALL
     * until there is a scheduler to give it a user thread, but IA32_KERNEL_GS_BASE
     * is written here too, and the interrupt entry path exchanges GS against it
     * upon every interrupt from privilege level 3 — so a processor without it
     * would be one interrupt away from running with a segment base of zero.
     */
    (void)SyscallInitialise();
    SyscallSetKernelStack(SmpPreparations[expected].stack_top);

    /*
     * And its own local controller, without which it accepts no interrupt at
     * all — including the shootdown its fellows are about to send it, and whose
     * absence would present as a machine that hangs in PagingUnmapKernelPage.
     */
    if (!LocalApicInitialiseThisProcessor())
    {
        KernelPanic("A starting processor could not enable its local controller.");
    }

    /*
     * The processor writes down what it actually holds, before it parks and
     * while it is the only thing that can read it.
     *
     * Every value here is taken with the instruction that reads the register —
     * STR, SGDT, SIDT and the control registers — rather than from what this
     * kernel believes it loaded, because what is being asserted is that the load
     * reached this processor. A processor that skipped one of the steps above
     * runs perfectly well until the one moment that step exists for, and by then
     * there is nothing left to report with.
     */
    SmpRecords[expected].task_register = (uint64_t)TssTaskRegister();
    SmpRecords[expected].gdt_base = GdtBase();
    SmpRecords[expected].gdt_limit = (uint64_t)GdtLimit();
    SmpRecords[expected].idt_base = IdtBase();
    SmpRecords[expected].cr0 = ReadCr0();
    SmpRecords[expected].cr3 = ReadCr3();
    SmpRecords[expected].cr4 = ReadCr4();
    SmpRecords[expected].recorded = true;

    /*
     * And the page attribute table, which is per processor and which the
     * mapping of the framebuffer depends upon.
     *
     * The mapping the bootstrap processor made carries the page-attribute-table
     * flag, and that flag selects entry 4 of *this* processor's IA32_PAT — which
     * a reset leaves holding write-back. Without this write the announcement
     * below would be the first of many writes to one physical page under a
     * memory type no other processor is using, which Intel SDM, Volume 3A,
     * Section 11.12.4, declines to define the behaviour of. It stands
     * immediately before the first diagnostic for that reason.
     */
    FramebufferEstablishWriteCombiningOnThisProcessor();

    SmpAnnounceArrival(PerCpuIndex(), PerCpuCurrent()->apic_identifier);

    /*
     * And into the scheduler, which is where this processor's working life
     * begins.
     *
     * Sub-task 6.14 ended here with a bare halt loop, there being nothing to
     * run. Sub-task 6.15 gives this processor a run queue of its own, a timer of
     * its own that takes a thread back when its quantum expires, and an idle
     * thread — which is this very execution, described. SchedulerEnterIdle
     * adopts it and does not return.
     *
     * It stands after the announcement rather than before it, so that a
     * processor which reached this point is reported as online whatever the
     * scheduler then makes of it: a processor that could not claim an idle
     * thread has still come up, and the two facts are worth telling apart.
     */
    SchedulerEnterIdle();
}

void SmpInitialise(void)
{
    size_t declared;

    SmpDeclined = NULL;

    if (!AcpiIsAvailable())
    {
        SmpDeclined = "the firmware declared no processors: there is no MADT";
        return;
    }

    declared = AcpiProcessorCount();

    if (AcpiUsableProcessorCount() <= 1U)
    {
        SmpDeclined = "this machine has one usable processor";
        return;
    }

    if (!LocalApicIsEnabled())
    {
        SmpDeclined = "the local controller is not enabled, so nothing can be sent";
        return;
    }

    /*
     * CR3 is a 32-bit register until paging is enabled, and the trampoline is in
     * 32-bit protected mode when it writes it. A hierarchy above four gibibytes
     * could not be named there at all, and half of its address would be. The
     * kernel's root is obtained from FrameAllocateBelow and has never been
     * anywhere near that boundary, so this is a refusal that has never fired and
     * is written so that it fires rather than truncates if it ever can.
     */
    if (PagingKernelRoot() >= UINT64_C(0x100000000))
    {
        SmpDeclined = "the kernel paging hierarchy lies above four gibibytes, "
                      "which the trampoline cannot name";
        return;
    }

    if (!PitIsRunning())
    {
        SmpDeclined = "the interval timer is not running, so the delays the "
                      "startup protocol prescribes cannot be measured";
        return;
    }

    if (!SmpTrampolinePageIsUsable())
    {
        SmpDeclined = "the firmware does not offer the low page the trampoline "
                      "requires";
        return;
    }

    SmpMapTrampolinePage();

    if (!SmpPlaceTrampoline())
    {
        SmpUnmapTrampolinePage();
        return;
    }

    /*
     * Interrupts are masked for the whole of the bring-up.
     *
     * Intel SDM, Volume 3A, Section 8.4.4.1, requires every device capable of
     * delivering an interrupt to be inhibited between an INIT and the last
     * startup interrupt of a sequence, and this kernel's interrupt flag is the
     * blunt instrument that achieves it. It is also what the delays require:
     * they are measured by polling the interval timer's counter, and a handler
     * running in the middle of one would extend it rather than corrupt it, but
     * a handler that wrote to the diagnostic channel would take a lock this
     * processor might be holding.
     *
     * The critical section is entered through the per-processor area rather than
     * with a bare CLI so that the flag is restored to whatever it was, and so
     * that the depth is counted; the kernel arrives here with interrupts
     * enabled, and leaves with them enabled.
     */
    PerCpuPushInterruptState();

    for (size_t index = 0U; index < declared; ++index)
    {
        const AcpiProcessor *const processor = AcpiProcessorAt(index);
        uint32_t next;

        /*
         * Usable is enabled, or online capable — ACPI Specification 6.5,
         * Table 5.23: a structure with both flags clear describes a processor
         * that is not present at all, and its contents must be ignored.
         */
        if ((processor == NULL) ||
            (!processor->enabled && !processor->online_capable))
        {
            continue;
        }

        /* The bootstrap processor is already running and must never be sent an
         * INIT: Section 8.4.4.1's sequence is addressed to a processor that has
         * not started, and an INIT to one that has is a reset of the machine
         * this kernel is running upon. */
        if (processor->apic_id == (uint32_t)LocalApicIdentifier())
        {
            continue;
        }

        /*
         * An identifier beyond eight bits cannot be named in the destination
         * field, which is bits 31:24 of the command register's high half in
         * xAPIC mode. Such an identifier comes from a Processor Local x2APIC
         * structure, and reaching it requires the x2APIC mode this kernel does
         * not enter; it is declined rather than truncated, a truncated
         * identifier naming a different processor rather than none.
         */
        if (processor->apic_id > UINT32_C(0xFF))
        {
            ++SmpRefused;
            continue;
        }

        next = PerCpuOnlineCount();

        if (next >= PER_CPU_MAXIMUM)
        {
            ++SmpRefused;
            continue;
        }

        if (!SmpPrepareProcessor(next))
        {
            ++SmpRefused;
            continue;
        }

        SmpParameters()->page_table = (uint64_t)PagingKernelRoot();
        SmpParameters()->entry_point =
            (uint64_t)(uintptr_t)&SmpApplicationProcessorEntry;
        SmpParameters()->stack_top = SmpPreparations[next].stack_top;
        SmpParameters()->argument = (uint64_t)next;

        if (!SmpStartProcessor((uint8_t)processor->apic_id))
        {
            ++SmpFailures;
            continue;
        }

        if (!SmpWaitForOnline(next + 1U))
        {
            ++SmpFailures;
            continue;
        }

        ++SmpStarted;
    }

    PerCpuPopInterruptState();

    /*
     * The mapping goes last, and its removal is announced to every processor
     * just started. It is the first shootdown this kernel has performed that
     * had anybody to send it to.
     */
    SmpUnmapTrampolinePage();
}

const SmpProcessorRecord *SmpRecordAt(uint32_t index)
{
    if ((index >= PER_CPU_MAXIMUM) || !SmpRecords[index].recorded)
    {
        return NULL;
    }

    return &SmpRecords[index];
}

uint32_t SmpProcessorsStarted(void)
{
    return SmpStarted;
}

uint32_t SmpProcessorsRefused(void)
{
    return SmpRefused;
}

uint32_t SmpStartupFailureCount(void)
{
    return SmpFailures;
}

bool SmpIsMultiprocessor(void)
{
    return PerCpuOnlineCount() > 1U;
}

const char *SmpDeclinedReason(void)
{
    return SmpDeclined;
}

void SmpReport(void)
{
    KernelWriteString("Application processors: ");

    if (SmpDeclined != NULL)
    {
        KernelWriteString("none started, ");
        KernelWriteString(SmpDeclined);
        KernelWriteString(".\n");
        return;
    }

    KernelWriteDecimal((uint64_t)SmpStarted);
    KernelWriteString(" started, ");
    KernelWriteDecimal((uint64_t)SmpFailures);
    KernelWriteString(" did not answer, ");
    KernelWriteDecimal((uint64_t)SmpRefused);
    KernelWriteString(" declined; ");
    KernelWriteDecimal((uint64_t)PerCpuOnlineCount());
    KernelWriteString(" processor(s) online in all.\n");

    KernelWriteString("  Trampoline at ");
    KernelWriteHexadecimal(SMP_TRAMPOLINE_ADDRESS);
    KernelWriteString(", startup vector ");
    KernelWriteHexadecimal((uint64_t)SMP_TRAMPOLINE_VECTOR);
    KernelWriteString(", ");
    KernelWriteDecimal(
        (uint64_t)(SmpTrampolineImageEnd - SmpTrampolineImageStart));
    KernelWriteString(" bytes; its identity mapping is ");
    KernelWriteString(SmpTrampolineMapped ? "STILL PRESENT (unexpected)"
                                          : "removed");
    KernelWriteString(".\n");
}
