/*
 * File: kernel/test/verify_smp.c
 * Purpose: Asserts the work of sub-task 6.13: the per-processor data area and
 *          the segment base it is reached through, the ticket spinlock and the
 *          counted interrupt-disable beneath it, the inter-processor interrupt,
 *          and the translation-lookaside-buffer shootdown built upon that.
 * Key functions: KernelVerifyPerCpu, KernelVerifySpinlock, KernelVerifyIpi,
 *          KernelVerifyShootdown.
 * References:
 *   - docs/design/CONCURRENCY.md, Section 8: the assertions, each paired with
 *     the silent failure it exists to catch.
 *   - Intel SDM, Volume 3A, Sections 4.10.4.1, 4.10.5, 8.1.2.2, 10.6 and 10.6.1.
 *   - docs/design/ARCHITECTURE.md, Section 4.1: sub-task 6.13 was placed before
 *     6.14 upon the argument that everything in it can be exercised upon one
 *     processor. These are the assertions that argument has to be true for.
 *
 * The class of failure these are written against.
 *
 *   A lock that does not lock behaves exactly like a lock that does, upon a
 *   machine with one processor — which is every machine this kernel has yet run
 *   upon. Nothing observable distinguishes them until a second processor exists,
 *   and by then the failure presents as corruption somewhere else entirely. So
 *   what is asserted here is not "the machine still works": it is that each
 *   mechanism's *internal* state moves as it must — the tickets advance, the
 *   depth counts, the interrupt arrives, the address the handler was given is
 *   the address it invalidated — because that state is the only thing a single
 *   processor can be made to show.
 *
 *   The one thing that can be shown end to end is the shootdown, and it is,
 *   because an interrupt a processor sends to itself is delivered like any
 *   other. KernelVerifyShootdown deliberately makes a mapping stale in the way
 *   the kernel's own routines never would, and then requires the handler to be
 *   what repairs it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/percpu.h>
#include <oxys/spinlock.h>
#include <oxys/ipi.h>
#include <oxys/shootdown.h>
#include <oxys/lapic.h>
#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/vmm.h>
#include <oxys/memory.h>
#include <oxys/msr.h>
#include <oxys/cpu.h>
#include <oxys/tss.h>

/* The lock the spinlock assertions operate upon. It is a lock of this file's
 * own: asserting upon one the kernel is actually using would mean asserting upon
 * counters that anything else may have moved. */
static Spinlock VerifySpinlock = SPINLOCK_INITIALISER("self-test");

/*
 * The number of iterations the wait for a self-delivered interrupt is given.
 *
 * It is a fixed spin and not a wait upon the timer, for the reason
 * kernel/test/verify_apic.c gives for the same choice: a bound expressed in
 * ticks is a bound that does not expire upon a machine whose timer has stopped,
 * which is one of the conditions these tests exist to detect.
 */
#define VERIFY_DELIVERY_SPIN 2000000U

/*
 * Asserts the per-processor area.
 *
 * Every assertion is made against what the hardware reports rather than against
 * what this kernel believes it wrote, because the mechanism is a segment base
 * and a segment base that is wrong produces a structure at the wrong address
 * rather than an error. A per-processor area reached through a base of zero is a
 * structure at address sixteen: on this kernel's map that is not present, so the
 * first access faults — but it would not fault upon a kernel that had identity
 * mapped low memory, and there it would silently read and write whatever was
 * there.
 */
void KernelVerifyPerCpu(void)
{
    bool succeeded = true;
    PerCpu *area;
    uint64_t gs_base;

    if (!PerCpuIsEstablished())
    {
        KernelWriteString("Per-processor area self-test: no area has been "
                          "established. FAILED.\n");
        return;
    }

    area = PerCpuCurrent();

    /*
     * The self pointer is what makes the area reachable, and it is the one field
     * whose corruption would be invisible: an area whose self pointer named
     * another area would give every processor another's state, and every
     * assertion below would still pass.
     */
    if (area->self != area)
    {
        KernelWriteString("  The area's self pointer does not name the area.\n");
        succeeded = false;
    }

    /*
     * GS.base must hold the area while kernel code is executing. This is the
     * invariant of docs/design/CONCURRENCY.md, Section 3.2, read back from the
     * register rather than assumed; the model-specific register is the only
     * place the value actually lives.
     */
    gs_base = ReadMsr(IA32_GS_BASE);

    if (gs_base != (uint64_t)(uintptr_t)area)
    {
        KernelWriteString("  GS.base does not hold the per-processor area while "
                          "the kernel is executing.\n");
        succeeded = false;
    }

    /*
     * And IA32_KERNEL_GS_BASE must hold what a user program would have, which is
     * zero. A machine with the area in both registers works until the first
     * SWAPGS, after which the kernel is running with a user's value and the user
     * with the kernel's — and the kernel's is a pointer to the stack the entry
     * path switches to.
     */
    if (ReadMsr(IA32_KERNEL_GS_BASE) != 0U)
    {
        KernelWriteString("  IA32_KERNEL_GS_BASE holds something while the kernel "
                          "is executing; SWAPGS would exchange it in.\n");
        succeeded = false;
    }

    /*
     * The bootstrap processor is index 0 and the machine has one processor
     * online until sub-task 6.14. Both are stated so that the run in which 6.14
     * changes them is the run that says so.
     */
    if (area->index != 0U)
    {
        KernelWriteString("  The initialising processor is not index 0.\n");
        succeeded = false;
    }

    if (!area->bootstrap)
    {
        KernelWriteString("  The initialising processor is not the bootstrap "
                          "processor, which cannot be true before sub-task 6.14.\n");
        succeeded = false;
    }

    if (!area->online)
    {
        KernelWriteString("  The executing processor's area is not marked online.\n");
        succeeded = false;
    }

    if (PerCpuOnlineCount() != 1U)
    {
        KernelWriteString("  More than one processor is online, which cannot be "
                          "true before sub-task 6.14.\n");
        succeeded = false;
    }

    /*
     * The area's identifier must be the one the controller answers to. The area
     * takes it from CPUID leaf 1 before the controller has been mapped, and the
     * controller reports it from its own identifier register; a disagreement
     * would mean an interrupt addressed to a processor by the number its area
     * carries would be delivered to a different one.
     */
    if (LocalApicIsEnabled() &&
        area->apic_identifier != (uint32_t)LocalApicIdentifier())
    {
        KernelWriteString("  The area's APIC identifier disagrees with the one the "
                          "local controller reports.\n");
        succeeded = false;
    }

    /*
     * The kernel stack the system-call entry path will switch to. It is the
     * first field of the area and the entry path loads RSP from it without
     * checking, so a zero here is a kernel executing upon address zero the
     * moment a program makes a call.
     */
    if (area->kernel_stack == 0U)
    {
        KernelWriteString("  The area names no kernel stack.\n");
        succeeded = false;
    }
    else if (area->kernel_stack != TssKernelStack())
    {
        KernelWriteString("  The area's kernel stack is not the one the task state "
                          "segment names.\n");
        succeeded = false;
    }

    /*
     * Nothing is held at the moment this runs. A depth that is not zero here
     * means some earlier section was entered and never left, and every interrupt
     * from that point onwards has been masked by an owner nothing records.
     */
    if (PerCpuCriticalDepth() != 0U)
    {
        KernelWriteString("  A critical section is outstanding that nothing left.\n");
        succeeded = false;
    }

    if (PerCpuLocksHeld() != 0U)
    {
        KernelWriteString("  A spinlock is held that nothing released.\n");
        succeeded = false;
    }

    /* An index nothing has been established for must produce nothing rather than
     * the storage that happens to lie there. */
    if (PerCpuAt(PerCpuOnlineCount()) != NULL)
    {
        KernelWriteString("  An area was returned for a processor that has none.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Per-processor area self-test passed.\n"
                                : "Per-processor area self-test FAILED.\n");
}

/*
 * Asserts the spinlock and the interrupt discipline beneath it.
 *
 * Upon one processor a lock is never contended, so what is asserted is the state
 * the acquire and the release leave behind: the tickets, the owner, the counted
 * depth and the interrupt flag. Those are what a second processor will depend
 * upon, and they are wrong or right today exactly as they will be then.
 */
void KernelVerifySpinlock(void)
{
    bool succeeded = true;
    const bool interrupts_before = InterruptsAreEnabled();
    uint16_t ticket_before;
    uint64_t acquisitions_before;

    SpinlockInitialise(&VerifySpinlock, "self-test");

    /* A lock nobody has taken is free, unowned and has no waiters. */
    if (SpinlockIsHeld(&VerifySpinlock) || SpinlockWaiterCount(&VerifySpinlock) != 0U)
    {
        KernelWriteString("  A freshly initialised lock reports itself held.\n");
        succeeded = false;
    }

    if (SpinlockIsHeldByThisProcessor(&VerifySpinlock))
    {
        KernelWriteString("  A freshly initialised lock reports this processor as "
                          "its owner.\n");
        succeeded = false;
    }

    ticket_before = SpinlockWaiterCount(&VerifySpinlock);
    acquisitions_before = SpinlockAcquisitionCount(&VerifySpinlock);

    /* --- The uncontended acquire. --- */

    SpinlockAcquire(&VerifySpinlock);

    if (!SpinlockIsHeld(&VerifySpinlock))
    {
        KernelWriteString("  A lock that has been acquired does not report itself "
                          "held.\n");
        succeeded = false;
    }

    if (!SpinlockIsHeldByThisProcessor(&VerifySpinlock))
    {
        KernelWriteString("  A lock this processor holds does not name it as the "
                          "owner. The recursive-acquire check would not fire.\n");
        succeeded = false;
    }

    /*
     * The mask, which is the half of the acquire that is easy to omit and
     * impossible to notice. A lock taken without it is a lock an interrupt
     * handler may take behind, upon the one processor able to release it.
     */
    if (InterruptsAreEnabled())
    {
        KernelWriteString("  A lock was acquired without masking interrupts.\n");
        succeeded = false;
    }

    if (PerCpuCriticalDepth() != 1U)
    {
        KernelWriteString("  An acquire did not enter exactly one critical "
                          "section.\n");
        succeeded = false;
    }

    if (PerCpuLocksHeld() != 1U)
    {
        KernelWriteString("  An acquire did not record the lock as held.\n");
        succeeded = false;
    }

    /*
     * A held lock has issued one more ticket than it has served. This is the
     * assertion that a test-and-set lock could not make of itself, and it is
     * what the ordering guarantee rests upon.
     */
    if (SpinlockWaiterCount(&VerifySpinlock) != (uint16_t)(ticket_before + 1U))
    {
        KernelWriteString("  A held lock does not stand one ticket ahead of its "
                          "serving number.\n");
        succeeded = false;
    }

    /* --- A nested critical section, which the release must not end. --- */

    PerCpuPushInterruptState();

    if (PerCpuCriticalDepth() != 2U)
    {
        KernelWriteString("  A nested critical section did not deepen the count.\n");
        succeeded = false;
    }

    PerCpuPopInterruptState();

    if (PerCpuCriticalDepth() != 1U)
    {
        KernelWriteString("  Leaving a nested section did not restore the count.\n");
        succeeded = false;
    }

    /*
     * And the flag is still clear. This is the assertion that catches the design
     * in which each section saves and restores the flags for itself: there, the
     * inner pop restores the state from before the *inner* push, which is
     * "masked", and the code happens to work; but the same design with the pushes
     * in the other order re-enables interrupts inside a section that had disabled
     * them, and nothing says so.
     */
    if (InterruptsAreEnabled())
    {
        KernelWriteString("  Leaving a nested section re-enabled interrupts inside "
                          "the outer one.\n");
        succeeded = false;
    }

    /* --- The release. --- */

    SpinlockRelease(&VerifySpinlock);

    if (SpinlockIsHeld(&VerifySpinlock))
    {
        KernelWriteString("  A released lock still reports itself held.\n");
        succeeded = false;
    }

    if (SpinlockIsHeldByThisProcessor(&VerifySpinlock))
    {
        KernelWriteString("  A released lock still names an owner. The next "
                          "acquire would be refused as recursive.\n");
        succeeded = false;
    }

    if (PerCpuCriticalDepth() != 0U || PerCpuLocksHeld() != 0U)
    {
        KernelWriteString("  A release did not leave the critical section.\n");
        succeeded = false;
    }

    /*
     * The flag is restored to what it was before the acquire, and not to some
     * fixed value. A release that unconditionally set the flag would work
     * perfectly at every call site reached with interrupts on, and would enable
     * interrupts in the middle of the boot sequence at every other one.
     */
    if (InterruptsAreEnabled() != interrupts_before)
    {
        KernelWriteString("  A release did not restore the interrupt flag to the "
                          "state the acquire found it in.\n");
        succeeded = false;
    }

    if (SpinlockAcquisitionCount(&VerifySpinlock) != acquisitions_before + 1U)
    {
        KernelWriteString("  The acquisition was not counted.\n");
        succeeded = false;
    }

    /*
     * An uncontended acquire is not a contention. The counter is what a later
     * phase will use to decide whether a structure needs a finer lock, and one
     * that counted every acquire would say every lock was contended.
     */
    if (SpinlockContentionCount(&VerifySpinlock) != 0U)
    {
        KernelWriteString("  An uncontended acquire was counted as a contention.\n");
        succeeded = false;
    }

    /* --- The conditional acquire. --- */

    if (!SpinlockTryAcquire(&VerifySpinlock))
    {
        KernelWriteString("  A conditional acquire of a free lock failed.\n");
        succeeded = false;
    }
    else
    {
        if (!SpinlockIsHeldByThisProcessor(&VerifySpinlock))
        {
            KernelWriteString("  A conditional acquire that succeeded did not take "
                              "the lock.\n");
            succeeded = false;
        }

        /*
         * A second conditional acquire must fail rather than deadlock. Upon one
         * processor this is the only way the refusal path can be reached at all,
         * and it is the path a report will take when it declines to wait for a
         * structure another processor is changing.
         */
        if (SpinlockTryAcquire(&VerifySpinlock))
        {
            KernelWriteString("  A conditional acquire of a held lock succeeded.\n");
            succeeded = false;
        }

        if (PerCpuCriticalDepth() != 1U)
        {
            KernelWriteString("  A conditional acquire that failed did not leave "
                              "the interrupt state as it found it.\n");
            succeeded = false;
        }

        SpinlockRelease(&VerifySpinlock);
    }

    if (InterruptsAreEnabled() != interrupts_before)
    {
        KernelWriteString("  The conditional acquire left the interrupt flag "
                          "changed.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Spinlock self-test passed.\n"
                                : "Spinlock self-test FAILED.\n");
}

/*
 * Asserts the inter-processor interrupt.
 *
 * There is one processor, so the only audience an interrupt can be sent to is
 * this one — which is the audience that proves the mechanism, the delivery being
 * the same in either case per Intel SDM, Volume 3A, Section 10.6.1. The
 * shootdown vector is used rather than a vector invented for the test, because a
 * test that exercises its own vector proves the controller sends *something* and
 * nothing about the path the kernel actually uses.
 */
void KernelVerifyIpi(void)
{
    bool succeeded = true;
    const bool interrupts_before = InterruptsAreEnabled();
    uint64_t received_before;
    uint64_t sent_before;

    if (!IpiIsAvailable())
    {
        KernelWriteString("Interprocessor interrupt self-test: unavailable, the "
                          "local controller not being enabled.\n");
        return;
    }

    /*
     * The command register must be idle before anything is sent. One that is not
     * means a previous send was never accepted, and every send after it would be
     * discarded silently — the register being written while the controller is
     * still carrying the last message.
     */
    if (!LocalApicCommandIsIdle())
    {
        KernelWriteString("  The interrupt command register is not idle before "
                          "anything has been sent.\n");
        succeeded = false;
    }

    received_before = IpiReceivedCount();
    sent_before = IpiSentCount();

    /*
     * The interrupt is sent with the flag clear and the flag is then set, rather
     * than the other way about. The controller holds the request until the
     * processor accepts one, so this establishes that a request raised while
     * interrupts are masked is delivered afterwards rather than lost — which is
     * the condition every shootdown sent to a processor inside a critical
     * section will be delivered under.
     */
    __asm__ __volatile__("cli" : : : "memory");

    if (!IpiSendToSelf(IPI_VECTOR_SHOOTDOWN))
    {
        KernelWriteString("  The local controller refused an interrupt addressed "
                          "to this processor.\n");
        succeeded = false;
    }

    if (IpiReceivedCount() != received_before)
    {
        KernelWriteString("  An interrupt was delivered while the flag was clear.\n");
        succeeded = false;
    }

    __asm__ __volatile__("sti" : : : "memory");

    for (volatile uint32_t spin = 0U;
         spin < VERIFY_DELIVERY_SPIN && IpiReceivedCount() == received_before;
         ++spin)
    {
        /* Deliberately empty: time is allowed to pass with interrupts on. */
    }

    __asm__ __volatile__("cli" : : : "memory");

    if (IpiReceivedCount() == received_before)
    {
        KernelWriteString("  An interrupt this processor sent to itself was never "
                          "delivered.\n");
        succeeded = false;
    }

    if (IpiSentCount() != sent_before + 1U)
    {
        KernelWriteString("  The send was not counted.\n");
        succeeded = false;
    }

    if (IpiFailedSendCount() != 0U)
    {
        KernelWriteString("  The local controller refused a send.\n");
        succeeded = false;
    }

    /*
     * The handler must have signalled the end of the interrupt. It cannot be
     * read back directly without disturbing the in-service register, so what is
     * asserted is the consequence: a second interrupt of the same vector is
     * accepted. One that was left in service would block every vector of its
     * priority class and above, which at 0xFD is everything but the two the
     * controller keeps for itself.
     */
    received_before = IpiReceivedCount();

    __asm__ __volatile__("sti" : : : "memory");

    (void)IpiSendToSelf(IPI_VECTOR_SHOOTDOWN);

    for (volatile uint32_t spin = 0U;
         spin < VERIFY_DELIVERY_SPIN && IpiReceivedCount() == received_before;
         ++spin)
    {
        /* Deliberately empty. */
    }

    __asm__ __volatile__("cli" : : : "memory");

    if (IpiReceivedCount() == received_before)
    {
        KernelWriteString("  A second interrupt of the same vector was not "
                          "delivered; the first was left in service.\n");
        succeeded = false;
    }

    if (LocalApicErrorCount() != 0U)
    {
        KernelWriteString("  The local controller reported an error while sending. "
                          "Its error status was ");
        KernelWriteHexadecimal((uint64_t)LocalApicLastErrorStatus());
        KernelWriteString(".\n");
        succeeded = false;
    }

    if (LocalApicCommandTimeoutCount() != 0U)
    {
        KernelWriteString("  A send was abandoned because the delivery status did "
                          "not clear.\n");
        succeeded = false;
    }

    if (interrupts_before)
    {
        __asm__ __volatile__("sti" : : : "memory");
    }

    KernelWriteString(succeeded ? "Interprocessor interrupt self-test passed.\n"
                                : "Interprocessor interrupt self-test FAILED.\n");
}

/*
 * Walks the active hierarchy to the page-table entry that translates an address,
 * and returns a pointer to it, or NULL where the address is not mapped by a
 * 4 KiB page.
 *
 * The self-test needs to change a mapping *without* invalidating it, which is
 * exactly what none of the kernel's own routines will do — PagingMapKernelPage
 * invalidates, and the whole point of it doing so is the point being tested. So
 * the entry is reached directly. This is the one place in the kernel outside
 * kernel/mm/ that walks the hierarchy, and it is a self-test doing deliberately
 * what a defect would do accidentally.
 */
static uint64_t *KernelVerifyLeafEntry(VirtualAddress address)
{
    PhysicalAddress table = PagingActiveRoot();
    const size_t indices[4] = {
        (size_t)((address >> 39) & 0x1FFU),
        (size_t)((address >> 30) & 0x1FFU),
        (size_t)((address >> 21) & 0x1FFU),
        (size_t)((address >> 12) & 0x1FFU),
    };

    for (size_t level = 0U; level < 3U; ++level)
    {
        const uint64_t entry = PagingTableEntries(table)[indices[level]];

        if ((entry & PAGE_ENTRY_PRESENT) == 0U ||
            (entry & PAGE_ENTRY_LARGE) != 0U)
        {
            return NULL;
        }

        table = (PhysicalAddress)(entry & PAGE_ENTRY_ADDRESS_MASK);
    }

    return &PagingTableEntries(table)[indices[3]];
}

/*
 * Asserts the translation-lookaside-buffer shootdown.
 *
 * This is the one mechanism of sub-task 6.13 that can be shown to work end to
 * end upon a single processor, and it is shown the hard way: a mapping is
 * changed behind the processor's back, so that the translation it holds is
 * genuinely stale, and the shootdown handler is then required to be what
 * repairs it.
 *
 * The final read is the assertion, and it is sound whether or not the processor
 * had in fact cached anything: a shootdown that works produces the new value,
 * and there is no arrangement of a broken one that also does. Whether staleness
 * was observable beforehand is reported rather than asserted, the architecture
 * nowhere requiring a processor to cache a translation it has used.
 */
void KernelVerifyShootdown(void)
{
    bool succeeded = true;
    const bool interrupts_before = InterruptsAreEnabled();
    uint64_t serviced_before;
    uint64_t requests_before;
    PhysicalAddress second_frame;
    volatile uint64_t *window;
    uint64_t *entry;
    uint64_t original_entry;
    uint64_t observed_before_shootdown;
    uint64_t observed_after_shootdown;

    if (!IpiIsAvailable())
    {
        KernelWriteString("TLB shootdown self-test: unavailable, the local "
                          "controller not being enabled.\n");
        return;
    }

    /*
     * The window is an ordinary page of the kernel arena, mapped by the ordinary
     * routine over a frame the arena owns. Nothing about it is special, which is
     * the point: what is asserted below must hold for the mappings the kernel
     * actually makes and not for an arrangement built to be asserted upon.
     */
    window = (volatile uint64_t *)KernelPagesAllocate(1U);

    if (window == NULL)
    {
        KernelWriteString("TLB shootdown self-test: no page is available. "
                          "FAILED.\n");
        return;
    }

    second_frame = FrameAllocate();

    if (second_frame == FRAME_ALLOCATION_FAILED)
    {
        KernelWriteString("TLB shootdown self-test: no frame is available. "
                          "FAILED.\n");
        KernelPagesFree((void *)(uintptr_t)window, 1U);
        return;
    }

    *window = UINT64_C(0x0111111111111111);

    /* The second frame is filled through the direct map, so that its contents
     * differ and the window is not the thing that wrote them. */
    *(volatile uint64_t *)(uintptr_t)PhysicalToDirect(second_frame) =
        UINT64_C(0x0222222222222222);

    /* The window is read, which is what puts the translation of it in the buffer
     * this test is about. */
    if (*window != UINT64_C(0x0111111111111111))
    {
        KernelWriteString("  The window does not read back what was written "
                          "through it.\n");
        succeeded = false;
    }

    entry = KernelVerifyLeafEntry((VirtualAddress)(uintptr_t)window);

    if (entry == NULL)
    {
        KernelWriteString("TLB shootdown self-test: the window is not mapped by a "
                          "4 KiB page. FAILED.\n");
        KernelPagesFree((void *)(uintptr_t)window, 1U);
        FrameFree(second_frame);
        return;
    }

    /*
     * The mapping is moved to the second frame, and nothing is invalidated. From
     * here until the shootdown the processor may be holding a translation that
     * names a frame the page tables no longer do — which is precisely the state
     * a mapping changed upon another processor leaves this one in.
     *
     * The entry is saved so that it can be put back exactly. The arena's
     * accounting believes this page names the frame it was given, and freeing it
     * while it names another would return the wrong frame to the allocator and
     * leak the right one.
     */
    original_entry = *entry;
    *entry = (uint64_t)second_frame | PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE;

    observed_before_shootdown = *window;

    serviced_before = ShootdownServiceCount();
    requests_before = ShootdownRequestCount();

    /*
     * The request needs the flag set, the acknowledgement being made by a
     * handler upon this very processor.
     */
    __asm__ __volatile__("sti" : : : "memory");

    if (!ShootdownToSelf((VirtualAddress)(uintptr_t)window))
    {
        KernelWriteString("  A shootdown addressed to this processor was not "
                          "acknowledged.\n");
        succeeded = false;
    }

    __asm__ __volatile__("cli" : : : "memory");

    observed_after_shootdown = *window;

    /* --- What the handler must have done. --- */

    if (ShootdownServiceCount() != serviced_before + 1U)
    {
        KernelWriteString("  The shootdown handler did not run exactly once.\n");
        succeeded = false;
    }

    if (ShootdownRequestCount() != requests_before + 1U)
    {
        KernelWriteString("  The request was not counted.\n");
        succeeded = false;
    }

    if (ShootdownLastAddress() != (VirtualAddress)(uintptr_t)window)
    {
        KernelWriteString("  The address the handler was given is not the address "
                          "the request named.\n");
        succeeded = false;
    }

    if (ShootdownAbandonedCount() != 0U)
    {
        KernelWriteString("  A shootdown was abandoned for want of an "
                          "acknowledgement.\n");
        succeeded = false;
    }

    if (PerCpuCurrent()->shootdowns_serviced == 0U)
    {
        KernelWriteString("  The service was not recorded against the processor "
                          "that performed it.\n");
        succeeded = false;
    }

    /* --- The assertion the whole test exists for. --- */

    if (observed_after_shootdown != UINT64_C(0x0222222222222222))
    {
        KernelWriteString("  After the shootdown the window still reads through "
                          "the mapping that was replaced.\n");
        succeeded = false;
    }

    /*
     * Reported and not asserted. A processor is nowhere required to have cached
     * the translation, so an equality here means the test proved less than it
     * hoped rather than that anything is wrong — and the run in which it changes
     * is a run worth being able to see.
     */
    KernelWriteString("  The stale translation was ");
    KernelWriteString(observed_before_shootdown == UINT64_C(0x0111111111111111)
                          ? "observable before the shootdown.\n"
                          : "not observable; the processor had not cached it.\n");

    /*
     * The entry is put back to exactly what it held, and invalidated properly
     * this time — the translation in the buffer now names the second frame, and
     * that frame is about to be given away.
     */
    *entry = original_entry;
    PagingInvalidatePage((VirtualAddress)(uintptr_t)window);

    KernelPagesFree((void *)(uintptr_t)window, 1U);
    FrameFree(second_frame);

    if (interrupts_before)
    {
        __asm__ __volatile__("sti" : : : "memory");
    }

    KernelWriteString(succeeded ? "TLB shootdown self-test passed.\n"
                                : "TLB shootdown self-test FAILED.\n");
}
