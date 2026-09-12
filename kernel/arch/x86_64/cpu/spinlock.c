/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/cpu/spinlock.c
 * Purpose: Implements the ticket spinlock: the locked fetch-and-add that issues
 *          a ticket, the bounded wait for it to be served, the release that
 *          admits the next arrival, and the two checks that turn the silent
 *          misuses of a lock into a report.
 * Key functions: SpinlockInitialise, SpinlockAcquire, SpinlockRelease,
 *          SpinlockTryAcquire, SpinlockIsHeld, SpinlockIsHeldByThisProcessor,
 *          SpinlockWaiterCount, SpinlockReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 8.1.2.2: the LOCK prefix and the operations
 *     it may be applied to, XADD and CMPXCHG among them.
 *   - Intel SDM, Volume 2A, "XADD" and "CMPXCHG": fetch-and-add, and
 *     compare-and-exchange with its ZF result.
 *   - Intel SDM, Volume 3A, Section 8.2.2: loads are not reordered with loads
 *     and stores are not reordered with stores, so the acquire and the release
 *     below require no fence.
 *   - Intel SDM, Volume 3A, Section 8.2.3.4: a load may be reordered with an
 *     older store to a different location. This is why the release stores rather
 *     than exchanges, and why nothing here depends upon a store being visible
 *     before a subsequent load of another address.
 *   - Intel SDM, Volume 2B, "PAUSE".
 *   - docs/design/CONCURRENCY.md, Section 2.
 *
 * The bound upon the wait, and why there is one.
 *
 *   A spinlock that waits without bound turns every deadlock into a machine that
 *   stops with a blank screen and no record of what it was doing. The bound
 *   below is enormous — some hundreds of millions of iterations of a two-
 *   instruction loop — because it must not fire for a lock that is merely
 *   contended, however heavily; what it fires for is a lock whose holder is
 *   never going to release it. When it fires the machine panics and names the
 *   lock, the holder and the waiter, which is the whole of what a person needs.
 *
 *   It is a diagnostic and not a recovery. There is no correct way to proceed
 *   past a lock that will not be released; what is recovered is the explanation.
 */

#include <oxys/spinlock.h>
#include <oxys/percpu.h>
#include <oxys/kernel.h>

/*
 * The number of spins after which the wait is declared hopeless.
 *
 * At one PAUSE per iteration upon a machine of the era this kernel targets, this
 * is of the order of a second. A critical section in this kernel is a few dozen
 * instructions, so a wait that reaches this number is not a wait.
 */
#define SPINLOCK_SPIN_LIMIT UINT64_C(100000000)

/*
 * Issues a ticket: returns the value the counter held and leaves it one greater.
 *
 * This is the only operation in the acquire that must be atomic. Two processors
 * arriving together must receive different tickets, and an ordinary increment
 * would give them the same one — after which both would be served, both would
 * enter the section, and the structure the lock governs would be modified by two
 * processors each believing itself alone. Intel SDM, Volume 3A, Section 8.1.2.2,
 * gives the LOCK prefix that guarantee.
 *
 * The condition-code clobber is declared because XADD writes the flags.
 */
static uint16_t SpinlockIssueTicket(Spinlock *lock)
{
    uint16_t issued = 1U;

    __asm__ __volatile__("lock xaddw %0, %1"
                         : "+r"(issued), "+m"(lock->ticket)
                         :
                         : "memory", "cc");

    return issued;
}

/*
 * Takes the lock if and only if it is free, and reports whether it did.
 *
 * The serving number is read first and the ticket counter second, and the ticket
 * counter is then advanced by a compare-and-exchange that fails if it has moved
 * since. That ordering is what makes the result trustworthy without reading both
 * counters at once:
 *
 *   - If the exchange succeeds, no ticket had been issued between the two reads,
 *     because the counter still held what was read. The ticket taken is
 *     therefore the one the serving number named, and the caller is served the
 *     moment it holds it.
 *   - The serving number cannot have advanced past that value in the interval
 *     either, since advancing it requires the holder of that very ticket to
 *     release — and that ticket had not been issued to anybody.
 *   - If the exchange fails, another processor took a ticket first and nothing
 *     has been changed by this one.
 *
 * A plain comparison of the two counters would not do: between the two reads
 * another processor may take and release the lock, after which the values
 * compared belong to different moments and their equality means nothing.
 */
static bool SpinlockClaimIfFree(Spinlock *lock)
{
    const uint16_t serving = lock->serving;
    uint16_t expected = lock->ticket;
    const uint16_t desired = (uint16_t)(expected + 1U);
    uint8_t succeeded = 0U;

    if (expected != serving)
    {
        return false;
    }

    __asm__ __volatile__("lock cmpxchgw %3, %1\n\t"
                         "sete %0"
                         : "=q"(succeeded), "+m"(lock->ticket), "+a"(expected)
                         : "r"(desired)
                         : "memory", "cc");

    return succeeded != 0U;
}

/*
 * Waits, cheaply.
 *
 * PAUSE tells the processor that this is a spin-wait: it de-pipelines the loop,
 * so that the reads speculated ahead of the one that finally succeeds do not
 * cost a memory-order violation when the loop is left, and it lowers the power
 * the wait draws. Upon a processor that does not implement it the instruction is
 * a no-operation, so no test is needed.
 */
static void SpinlockPause(void)
{
    __asm__ __volatile__("pause" : : : "memory");
}

void SpinlockInitialise(Spinlock *lock, const char *name)
{
    lock->ticket = 0U;
    lock->serving = 0U;
    lock->owner = SPINLOCK_NO_OWNER;
    lock->name = name;
    lock->acquisitions = 0U;
    lock->contentions = 0U;
}

/*
 * Records that the lock has been taken.
 *
 * Everything here is written by the processor that now holds the lock, under the
 * lock, and is therefore an ordinary store. The owner is written first because
 * it is what the recursive-acquire check reads, and a lock held with no owner
 * recorded is a lock that check cannot see.
 */
static void SpinlockNoteAcquired(Spinlock *lock)
{
    PerCpu *const area = PerCpuCurrent();

    lock->owner = area->index;
    ++lock->acquisitions;
    ++area->locks_acquired;
    ++area->locks_held;
}

void SpinlockAcquire(Spinlock *lock)
{
    uint16_t ticket;
    uint64_t spins = 0U;

    /*
     * The mask comes first, and before the ticket is issued.
     *
     * Between issuing a ticket and being served, this processor holds a claim
     * upon the lock that only it can relinquish. An interrupt delivered in that
     * window runs a handler which may take the same lock, and that handler would
     * wait behind a ticket held by the code it interrupted — upon the one
     * processor able to advance it. Masking before the ticket is issued closes
     * the window rather than narrowing it.
     */
    PerCpuPushInterruptState();

    /*
     * The recursive acquire, caught at the moment it is committed.
     *
     * Without this the processor issues itself a second ticket and waits for a
     * serving number that only it could advance, which is a machine that stops
     * inside a lock whose name nothing records. The check is exact rather than
     * heuristic: the owner is written under the lock and cleared under it, so a
     * lock owned by this processor is a lock this processor holds.
     */
    if (lock->owner == PerCpuIndex())
    {
        KernelWriteString("\nSpinlock: an acquisition of a lock already held by "
                          "the acquiring processor.\n");
        SpinlockReport(lock);
        KernelPanic("A processor acquired a spinlock it already held.");
    }

    ticket = SpinlockIssueTicket(lock);

    if (lock->serving != ticket)
    {
        ++lock->contentions;
        ++PerCpuCurrent()->lock_contentions;
    }

    while (lock->serving != ticket)
    {
        SpinlockPause();

        ++spins;

        if (spins >= SPINLOCK_SPIN_LIMIT)
        {
            /*
             * The report is made before the panic, because the panic's message
             * is one string and what is wanted here is four numbers: which lock,
             * who holds it, who is waiting, and how many are behind them.
             */
            KernelWriteString("\nSpinlock: a wait exceeded its bound.\n");
            SpinlockReport(lock);
            KernelWriteString("  The waiting processor is ");
            KernelWriteDecimal((uint64_t)PerCpuIndex());
            KernelWriteString(", holding ticket ");
            KernelWriteDecimal((uint64_t)ticket);
            KernelWriteString(".\n");

            KernelPanic("A spinlock was held beyond any bound a correct section "
                        "could require.");
        }
    }

    SpinlockNoteAcquired(lock);
}

bool SpinlockTryAcquire(Spinlock *lock)
{
    PerCpuPushInterruptState();

    if (lock->owner == PerCpuIndex())
    {
        PerCpuPopInterruptState();
        return false;
    }

    if (!SpinlockClaimIfFree(lock))
    {
        PerCpuPopInterruptState();
        return false;
    }

    SpinlockNoteAcquired(lock);

    return true;
}

void SpinlockRelease(Spinlock *lock)
{
    PerCpu *const area = PerCpuCurrent();

    /*
     * A release by a processor that does not hold the lock admits a second
     * processor into a section the first is still inside. It is caught here
     * because it cannot be caught anywhere else: the structure the lock governs
     * has no way to know how many processors are modifying it.
     */
    if (lock->owner != area->index)
    {
        KernelWriteString("\nSpinlock: a release by a processor that does not hold it.\n");
        SpinlockReport(lock);
        KernelPanic("A spinlock was released by a processor that did not hold it.");
    }

    if (area->locks_held > 0U)
    {
        --area->locks_held;
    }

    lock->owner = SPINLOCK_NO_OWNER;

    /*
     * The release is an ordinary store, and the order of the two stores above is
     * the whole of the protocol.
     *
     * Intel SDM, Volume 3A, Section 8.2.2, provides that stores are not
     * reordered with other stores, so the clearing of the owner is visible to
     * every processor before the serving number advances. A processor admitted
     * by the new serving number therefore never sees this lock still owned, and
     * the recursive-acquire check in SpinlockAcquire cannot fire spuriously
     * against a stale owner.
     *
     * The increment is not locked, and needs no lock: only the holder writes
     * this field, and there is one holder.
     */
    lock->serving = (uint16_t)(lock->serving + 1U);

    PerCpuPopInterruptState();
}

bool SpinlockIsHeld(const Spinlock *lock)
{
    return lock->ticket != lock->serving;
}

bool SpinlockIsHeldByThisProcessor(const Spinlock *lock)
{
    return lock->owner == PerCpuIndex();
}

const char *SpinlockName(const Spinlock *lock)
{
    return lock->name != NULL ? lock->name : "unnamed";
}

uint64_t SpinlockAcquisitionCount(const Spinlock *lock)
{
    return lock->acquisitions;
}

uint64_t SpinlockContentionCount(const Spinlock *lock)
{
    return lock->contentions;
}

uint16_t SpinlockWaiterCount(const Spinlock *lock)
{
    /* The subtraction is modular and is meant to be: the counters wrap
     * independently, and their difference is the queue length however many times
     * either has gone round. */
    return (uint16_t)(lock->ticket - lock->serving);
}

void SpinlockReport(const Spinlock *lock)
{
    KernelWriteString("  Spinlock \"");
    KernelWriteString(SpinlockName(lock));
    KernelWriteString("\": ticket ");
    KernelWriteDecimal((uint64_t)lock->ticket);
    KernelWriteString(", serving ");
    KernelWriteDecimal((uint64_t)lock->serving);
    KernelWriteString(", waiters ");
    KernelWriteDecimal((uint64_t)SpinlockWaiterCount(lock));
    KernelWriteString(", owner ");

    if (lock->owner == SPINLOCK_NO_OWNER)
    {
        KernelWriteString("none");
    }
    else
    {
        KernelWriteDecimal((uint64_t)lock->owner);
    }

    KernelWriteString(", acquisitions ");
    KernelWriteDecimal(lock->acquisitions);
    KernelWriteString(", contended ");
    KernelWriteDecimal(lock->contentions);
    KernelWriteString(".\n");
}
