/*
 * File: kernel/mm/shootdown.c
 * Purpose: Implements the translation-lookaside-buffer shootdown: the
 *          publication of the address whose translation has become stale, the
 *          interrupt that tells the other processors to discard it, the
 *          acknowledgement each makes, and the bounded wait for all of them.
 * Key functions: ShootdownInitialise, ShootdownBroadcast, ShootdownToSelf,
 *          ShootdownReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 4.10.4.1: INVLPG discards the cached
 *     translations of one linear address upon the executing processor.
 *   - Intel SDM, Volume 3A, Section 4.10.5: the invalidation reaches the
 *     executing processor alone, so a change to a paging-structure entry that
 *     several processors may have cached must be announced to each of them,
 *     which software does by interrupting them. The manual names the procedure
 *     "TLB shootdown".
 *   - Intel SDM, Volume 3A, Section 4.10.4.4: an invalidation may be deferred
 *     only while no processor can use the stale translation.
 *   - Intel SDM, Volume 3A, Section 8.1.2.2: the LOCK prefix, by which the
 *     acknowledgements of several processors are counted without a lock.
 *   - Intel SDM, Volume 3A, Section 10.8.5: the end-of-interrupt every handler
 *     entered by the fixed delivery mode must write.
 *   - docs/design/CONCURRENCY.md, Section 6.
 *
 * Why one request is in flight at a time.
 *
 *   The address being announced is a single global, and the lock below is what
 *   makes it one. The alternative — a request block per sending processor, which
 *   the handler would scan — removes the serialisation and is what this will
 *   become if shootdowns are ever measured to be the thing a workload waits for.
 *   It is not built now for the reason docs/devices/APIC.md, Section 7, gives for
 *   not having built this: a mechanism with nothing to use it is a mechanism
 *   nothing has ever shown to be right. There is one processor started, and the
 *   simple form is the one whose correctness can be argued in a paragraph.
 */

#include <oxys/shootdown.h>
#include <oxys/spinlock.h>
#include <oxys/percpu.h>
#include <oxys/paging.h>
#include <oxys/ipi.h>
#include <oxys/interrupts.h>
#include <oxys/lapic.h>
#include <oxys/cpu.h>
#include <oxys/kernel.h>

/*
 * The number of spins after which an acknowledgement is given up on.
 *
 * A processor that accepts the interrupt invalidates one address and decrements
 * a counter, which is a handful of instructions; the delay is whatever it was
 * doing with interrupts masked when the interrupt arrived, and a critical
 * section in this kernel is a few dozen instructions. This bound is some
 * hundreds of millions of iterations and is a diagnostic, not a tolerance.
 */
#define SHOOTDOWN_WAIT_LIMIT UINT64_C(100000000)

/*
 * The published request.
 *
 * `ShootdownAddress` is what every target invalidates; `ShootdownPending` is the
 * number of them that have not yet said they did. The lock serialises the
 * publication so that a second sender cannot overwrite an address the first is
 * still waiting on.
 */
static Spinlock ShootdownLock = SPINLOCK_INITIALISER("TLB shootdown");
static volatile VirtualAddress ShootdownAddress;
static volatile uint32_t ShootdownPending;

/* Accounting. The first two are written by handlers upon arbitrary processors
 * and are therefore incremented with the LOCK prefix; the rest are written by a
 * sender, under the lock. */
static volatile uint64_t ShootdownServiced;
static uint64_t ShootdownRequests;
static uint64_t ShootdownAbandoned;
static bool ShootdownRegistered;

/*
 * Adds one to a counter that processors other than this one also write.
 *
 * An ordinary increment is a load, an addition and a store, and two processors
 * performing it together lose one of the two additions. Intel SDM, Volume 3A,
 * Section 8.1.2.2, gives the LOCK prefix the guarantee that makes the sequence
 * indivisible.
 */
static void ShootdownCountAtomically(volatile uint64_t *counter)
{
    __asm__ __volatile__("lock addq $1, %0" : "+m"(*counter) : : "memory", "cc");
}

/* Subtracts one from the outstanding acknowledgements, indivisibly. */
static void ShootdownAcknowledge(void)
{
    __asm__ __volatile__("lock subl $1, %0" : "+m"(ShootdownPending) : : "memory", "cc");
}

/*
 * Receives IPI_VECTOR_SHOOTDOWN.
 *
 * The order of the three actions is the whole of the protocol. The address is
 * read first, because the acknowledgement is what permits the sender to publish
 * another and a read afterwards could read that one. The invalidation is
 * performed second. The acknowledgement is made third and last, because it is
 * the sender's evidence that the translation is gone — made before the
 * invalidation it would be a promise rather than a report, and the sender would
 * proceed to reuse a frame this processor could still reach.
 *
 * The end-of-interrupt comes after all three, as it must: Intel SDM, Volume 3A,
 * Section 10.8.5, requires it of every handler entered by the fixed delivery
 * mode, and issuing it earlier would admit a second interrupt of this vector
 * into the middle of the first.
 */
static void ShootdownHandle(TrapFrame *frame)
{
    const VirtualAddress address = ShootdownAddress;

    (void)frame;

    IpiNoteReceived();

    PagingInvalidateLocalPage(address);

    ShootdownCountAtomically(&ShootdownServiced);
    ++PerCpuCurrent()->shootdowns_serviced;

    ShootdownAcknowledge();

    LocalApicSignalEndOfInterrupt();
}

void ShootdownInitialise(void)
{
    SpinlockInitialise(&ShootdownLock, "TLB shootdown");

    InterruptRegisterHandler(IPI_VECTOR_SHOOTDOWN, ShootdownHandle,
                             "TLB shootdown");

    ShootdownRegistered = true;
}

/*
 * Waits for every target to acknowledge, and reports whether they did.
 *
 * PAUSE for the reason a spinlock uses it. The counter is volatile, so it is
 * re-read upon each iteration rather than held in a register — without which
 * this loop would be compiled into one that tests a value that cannot change.
 */
static bool ShootdownWait(void)
{
    for (uint64_t spins = 0U; spins < SHOOTDOWN_WAIT_LIMIT; ++spins)
    {
        if (ShootdownPending == 0U)
        {
            return true;
        }

        __asm__ __volatile__("pause" : : : "memory");
    }

    return false;
}

/*
 * Reports a shootdown that was not completed.
 *
 * It is separated from its callers because both must say the same thing: which
 * address was being announced, and how many processors never answered. A
 * shootdown that failed is not a condition any caller can carry on from, and
 * what is salvaged here is the explanation.
 */
static void ShootdownReportFailure(VirtualAddress address)
{
    ++ShootdownAbandoned;

    KernelWriteString("\nTLB shootdown: no acknowledgement for the address ");
    KernelWriteHexadecimal(address);
    KernelWriteString(".\n  Processors still to answer: ");
    KernelWriteDecimal((uint64_t)ShootdownPending);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)PerCpuOnlineCount());
    KernelWriteString(" online.\n");
}

bool ShootdownBroadcast(VirtualAddress address)
{
    uint32_t targets;
    bool acknowledged;

    /*
     * Nothing to announce.
     *
     * Upon a machine with one processor started — which is every machine this
     * kernel has yet run upon, sub-task 6.14 not having been done — the
     * invalidation the caller has already performed is the whole of the
     * operation. The test is made before the lock is taken so that the ordinary
     * path costs one comparison: PagingInvalidate is on the path of every map,
     * unmap and copy-on-write fault, and a lock taken there for nobody's benefit
     * would be paid for by all of them.
     */
    if (!ShootdownRegistered || PerCpuOnlineCount() <= 1U)
    {
        return true;
    }

    if (!IpiIsAvailable())
    {
        return false;
    }

    SpinlockAcquire(&ShootdownLock);

    targets = PerCpuOnlineCount() - 1U;

    ShootdownAddress = address;
    ShootdownPending = targets;
    ++ShootdownRequests;

    if (!IpiSendToOthers(IPI_VECTOR_SHOOTDOWN))
    {
        ShootdownPending = 0U;
        ShootdownReportFailure(address);
        SpinlockRelease(&ShootdownLock);
        return false;
    }

    /*
     * The wait is performed with the lock held and interrupts masked, and both
     * are correct here. The acknowledgements come from other processors, whose
     * interrupts are their own affair; nothing this processor could be
     * interrupted to do would advance the count, and being interrupted while
     * holding the lock would only lengthen the interval another sender waits for
     * it.
     */
    acknowledged = ShootdownWait();

    if (!acknowledged)
    {
        ShootdownReportFailure(address);
    }

    SpinlockRelease(&ShootdownLock);

    return acknowledged;
}

bool ShootdownToSelf(VirtualAddress address)
{
    bool acknowledged;

    if (!ShootdownRegistered || !IpiIsAvailable())
    {
        return false;
    }

    /*
     * Refused where a second processor is running, and the refusal is what makes
     * the rest of this function correct.
     *
     * The interrupt is delivered to the processor that sent it, so the sender
     * must have interrupts enabled to receive it — which means it cannot hold
     * ShootdownLock, whose acquisition masks them. The published address is
     * therefore unprotected for the duration of the wait. That is safe upon a
     * machine with one processor, there being nobody to overwrite it, and it is
     * not safe upon any other; so the condition is enforced rather than assumed.
     *
     * The restriction costs nothing, because the caller is the self-test and its
     * purpose is exactly the single-processor case: docs/design/ARCHITECTURE.md,
     * Section 4.1, records that sub-task 6.13 was placed before 6.14 on the
     * understanding that everything in it could be exercised upon one processor,
     * and this is the sentence that has to be true for that to hold.
     */
    if (PerCpuOnlineCount() != 1U)
    {
        return false;
    }

    /*
     * And it needs the flag set for the same reason. A caller inside a critical
     * section would send an interrupt that is held in the request register until
     * the section ends, and would then wait out the bound for an acknowledgement
     * from a handler that cannot run.
     */
    if (!InterruptsAreEnabled())
    {
        return false;
    }

    ShootdownAddress = address;
    ShootdownPending = 1U;
    ++ShootdownRequests;

    if (!IpiSendToSelf(IPI_VECTOR_SHOOTDOWN))
    {
        ShootdownPending = 0U;
        ShootdownReportFailure(address);
        return false;
    }

    acknowledged = ShootdownWait();

    if (!acknowledged)
    {
        ShootdownReportFailure(address);
    }

    return acknowledged;
}

uint64_t ShootdownRequestCount(void)
{
    return ShootdownRequests;
}

uint64_t ShootdownServiceCount(void)
{
    return ShootdownServiced;
}

uint64_t ShootdownAbandonedCount(void)
{
    return ShootdownAbandoned;
}

VirtualAddress ShootdownLastAddress(void)
{
    return ShootdownAddress;
}

void ShootdownReport(void)
{
    KernelWriteString("TLB shootdown: ");

    if (!ShootdownRegistered)
    {
        KernelWriteString("no handler registered.\n");
        return;
    }

    KernelWriteString("vector ");
    KernelWriteDecimal((uint64_t)IPI_VECTOR_SHOOTDOWN);
    KernelWriteString(", requests ");
    KernelWriteDecimal(ShootdownRequests);
    KernelWriteString(", serviced ");
    KernelWriteDecimal(ShootdownServiced);
    KernelWriteString(", abandoned ");
    KernelWriteDecimal(ShootdownAbandoned);
    KernelWriteString(", last address ");
    KernelWriteHexadecimal(ShootdownAddress);
    KernelWriteString(".\n");

    SpinlockReport(&ShootdownLock);
}
