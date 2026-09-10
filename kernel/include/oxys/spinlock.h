/*
 * File: kernel/include/oxys/spinlock.h
 * Purpose: Declares the kernel's mutual-exclusion primitive: a ticket spinlock
 *          that admits its waiters in the order they arrived, that masks
 *          interrupts for as long as it is held, and that reports the two
 *          misuses which are otherwise silent — acquiring a lock one already
 *          holds, and holding one for longer than any correct section could.
 * Key definitions: Spinlock, SPINLOCK_INITIALISER, SpinlockInitialise,
 *          SpinlockAcquire, SpinlockRelease, SpinlockTryAcquire,
 *          SpinlockIsHeld, SpinlockIsHeldByThisProcessor, SpinlockName,
 *          SpinlockAcquisitionCount, SpinlockContentionCount, SpinlockReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 8.1.2.2 (Bus Locking): the LOCK prefix makes the read-modify-write
 *     of the destination operand atomic with respect to every other processor,
 *     and is honoured for XADD among others.
 *   - Intel SDM, Volume 2A, "XADD": exchanges the destination with the source
 *     and stores their sum in the destination, which is fetch-and-add when the
 *     LOCK prefix is applied.
 *   - Intel SDM, Volume 3A, Section 8.2.2: the x86 memory-ordering model. Loads
 *     are not reordered with other loads, stores are not reordered with other
 *     stores, and stores are not reordered with older loads. An acquire and a
 *     release therefore need no fence instruction; what they need is that the
 *     compiler does not move accesses across them, which is what the memory
 *     clobber upon the inline assembly expresses.
 *   - Intel SDM, Volume 2B, "PAUSE": improves the performance of a spin-wait
 *     loop and reduces the power it consumes, and de-pipelines the loop so that
 *     the processor leaving it does not pay the memory-order violation penalty
 *     the speculated reads would otherwise incur.
 *   - docs/design/CONCURRENCY.md, Section 2: why the lock is a ticket lock, why
 *     it masks interrupts unconditionally, and what each of its two panics
 *     catches.
 *
 * Why a ticket lock rather than a test-and-set.
 *
 *   A test-and-set lock hands the lock to whichever waiter's access happens to
 *   win, which upon a machine with an unfair interconnect can be the same waiter
 *   repeatedly while another waits without bound. That is a defect that does not
 *   appear at all until there are several processors and a contended structure,
 *   and then appears as a machine that is inexplicably slow rather than as a
 *   machine that is wrong. A ticket lock admits its waiters in arrival order by
 *   construction, and costs one extra sixteen-bit field to do it.
 *
 *   The second reason is diagnostic. The difference between the two counters is
 *   the number of processors waiting, which is a quantity a report can print and
 *   a self-test can assert upon. A test-and-set lock holds one bit and can say
 *   nothing about itself.
 */

#ifndef OXYS_SPINLOCK_H
#define OXYS_SPINLOCK_H

#include <oxys/types.h>

/* The value the owner field holds when the lock is free. It is not a valid
 * processor index, PER_CPU_MAXIMUM being 64. */
#define SPINLOCK_NO_OWNER UINT32_C(0xFFFFFFFF)

/*
 * The lock.
 *
 * `ticket` is the number the next arrival takes; `serving` is the number
 * presently admitted. A lock is free when the two are equal. Both are sixteen
 * bits, which bounds the number of waiters that may be outstanding at 65535 —
 * comfortably above PER_CPU_MAXIMUM, and chosen so that the pair occupies one
 * aligned doubleword and a reader of the lock's state sees both halves of it.
 *
 * `owner` is the index of the processor holding the lock, written after the lock
 * is taken and cleared before it is given up. It exists for the recursive-
 * acquire check, which is the one misuse that can be detected at the moment it
 * is committed rather than by its consequences.
 *
 * The counters are read by the report and by the self-test. They are written
 * only under the lock, save `contentions`, which is written by a processor that
 * does not hold it yet; a torn increment there costs a diagnostic and nothing
 * else.
 */
typedef struct Spinlock
{
    volatile uint16_t ticket;
    volatile uint16_t serving;
    volatile uint32_t owner;
    const char *name;
    uint64_t acquisitions;
    uint64_t contentions;
} Spinlock;

/*
 * Static initialisation, for a lock that must be usable before any code has run
 * to initialise it — the frame allocator's, which is taken before the kernel has
 * an initialiser to call.
 */
#define SPINLOCK_INITIALISER(lock_name) \
    { 0U, 0U, SPINLOCK_NO_OWNER, (lock_name), 0U, 0U }

/*
 * Prepares a lock. The name is retained by address and must therefore be a
 * string literal or something else that outlives the lock; every caller passes a
 * literal, and the name exists so that a report and a panic can say which lock
 * rather than which address.
 */
void SpinlockInitialise(Spinlock *lock, const char *name);

/*
 * Takes the lock, waiting for it if another processor holds it.
 *
 * Interrupts are masked before the lock is taken and remain masked until it is
 * released, unconditionally and whether or not any handler touches the structure
 * the lock governs. The alternative — masking only where a handler is known to
 * be a party to the lock — requires every caller to know what every handler
 * touches, and gets it wrong once: a handler that takes a lock the interrupted
 * code already holds spins for it upon the processor that is the only one able
 * to release it, which is a machine that stops with no record of why. The cost
 * of masking always is interrupt latency for the duration of a critical section,
 * and a critical section in this kernel is a few dozen instructions.
 *
 * The masking is counted, not saved and restored by the caller;
 * PerCpuPushInterruptState says why.
 *
 * Panics where the executing processor already holds this lock. That is a
 * deadlock against oneself, and the processor would otherwise spin for a ticket
 * that only it can serve.
 */
void SpinlockAcquire(Spinlock *lock);

/*
 * Gives the lock up and, where this was the outermost critical section, restores
 * the interrupt flag to what it held before.
 *
 * Panics where the executing processor is not the holder. A release by a
 * processor that does not hold the lock admits a second one into the section,
 * and the structure the lock governs is then modified by two processors while
 * both believe they are alone.
 */
void SpinlockRelease(Spinlock *lock);

/*
 * Takes the lock if it is free, and reports whether it did.
 *
 * Where it succeeds the caller holds the lock exactly as if it had called
 * SpinlockAcquire, interrupts included, and must release it. Where it fails
 * nothing has changed and interrupts are as they were.
 *
 * It exists for the paths that must not wait: a report drawn while another
 * processor holds the structure being reported should print what it can and say
 * that it could not read the rest, rather than stopping the machine that is
 * trying to explain itself.
 */
bool SpinlockTryAcquire(Spinlock *lock);

/* Whether the lock is held by anybody, and whether it is held by the executing
 * processor. The first is a diagnostic and is true only for the instant it is
 * read; the second is the one an assertion may rely upon, a processor being able
 * to answer for itself. */
bool SpinlockIsHeld(const Spinlock *lock);
bool SpinlockIsHeldByThisProcessor(const Spinlock *lock);

/* The lock's name, its acquisitions, and the number of those that had to wait. */
const char *SpinlockName(const Spinlock *lock);
uint64_t SpinlockAcquisitionCount(const Spinlock *lock);
uint64_t SpinlockContentionCount(const Spinlock *lock);

/* The number of waiters presently queued, being the difference between the two
 * counters. Zero means the lock is free or held without contention. */
uint16_t SpinlockWaiterCount(const Spinlock *lock);

/* Emits the state of one lock upon the console and the serial port. */
void SpinlockReport(const Spinlock *lock);

#endif /* OXYS_SPINLOCK_H */
