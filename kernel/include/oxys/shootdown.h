/*
 * File: kernel/include/oxys/shootdown.h
 * Purpose: Declares the translation-lookaside-buffer shootdown: the mechanism by
 *          which a processor that has changed a paging-structure entry causes
 *          every other processor to discard the translation it may have cached
 *          from the old one.
 * Key definitions: ShootdownInitialise, ShootdownBroadcast, ShootdownToSelf,
 *          ShootdownRequestCount, ShootdownServiceCount,
 *          ShootdownAbandonedCount, ShootdownReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.10.4.1: software must invalidate a translation whenever it
 *     changes a paging-structure entry the processor may have cached, and INVLPG
 *     discards the entries for one linear address.
 *   - Intel SDM, Volume 3A, Section 4.10.5 ("Propagation of Paging-Structure
 *     Changes to Multiple Processors"): the invalidation reaches the executing
 *     processor alone. Where several processors may have cached a translation,
 *     software must cause each of them to perform the invalidation itself, which
 *     it does by interrupting them; the manual names the procedure "TLB
 *     shootdown".
 *   - Intel SDM, Volume 3A, Section 4.10.4.4: the delayed invalidation is only
 *     safe while no processor may use the stale translation, which is what the
 *     wait for an acknowledgement below establishes.
 *   - docs/design/CONCURRENCY.md, Section 6.
 *
 * What the mechanism does not do.
 *
 *   It carries one linear address. A change that affects a range is announced one
 *   page at a time, and a change to an entire address space — a process that has
 *   ended, say — is not announced at all, because the paging structures it used
 *   are not freed until nothing refers to them. Both are recorded as limitations
 *   in docs/design/CONCURRENCY.md rather than implemented ahead of the sub-task
 *   that needs them: a range shootdown is worth its complexity when there are
 *   processors to send it to, and there was one until sub-task 6.14 provided them.
 */

#ifndef OXYS_SHOOTDOWN_H
#define OXYS_SHOOTDOWN_H

#include <oxys/types.h>

/*
 * Registers the handler for IPI_VECTOR_SHOOTDOWN.
 *
 * It is called once the inter-processor interrupt layer exists, and before any
 * paging-structure entry is changed with more than one processor running. Until
 * it has run every broadcast below returns at once, there being no handler to
 * answer one.
 */
void ShootdownInitialise(void);

/*
 * Causes every processor but this one to discard its cached translation of
 * `address`.
 *
 * The caller has already invalidated the address here; this is the other
 * processors' half. It returns as soon as there is nothing to do — which is the
 * case upon a machine with one processor started, and is therefore the case
 * throughout sub-task 6.13 — and otherwise sends the interrupt and waits for
 * each target to acknowledge.
 *
 * The wait is what makes the operation correct rather than hopeful. Intel SDM,
 * Volume 3A, Section 4.10.4.4, permits an invalidation to be deferred only while
 * no processor can use the stale translation; returning before the targets have
 * invalidated would leave exactly that window, and the caller would go on to
 * reuse the frame the old entry still points at.
 *
 * Returns false where the send was refused or an acknowledgement did not arrive
 * within a bound. A caller cannot recover from that — the other processors may
 * be holding a translation of a page it is about to give away — and the failure
 * is fatal; it is reported rather than returned silently.
 */
bool ShootdownBroadcast(VirtualAddress address);

/*
 * The same request, directed at the executing processor.
 *
 * It exists because this kernel has started one processor and must nevertheless
 * demonstrate that the mechanism works end to end:
 * `docs/design/ARCHITECTURE.md`, Section 4.1, records that as the condition upon
 * which sub-task 6.13 was placed before 6.14. The interrupt goes out through the
 * local controller, arrives through the same gate an application processor's
 * would, runs the same handler, invalidates the same address and acknowledges by
 * the same counter.
 *
 * It requires the interrupt flag to be set, the acknowledgement being made by a
 * handler upon the processor that is waiting for it. A caller with interrupts
 * masked would wait out the bound and report a failure that is its own.
 */
bool ShootdownToSelf(VirtualAddress address);

/* The number of requests made, the number of pages this processor has
 * invalidated on another's behalf, and the number of requests abandoned because
 * an acknowledgement did not arrive. */
uint64_t ShootdownRequestCount(void);
uint64_t ShootdownServiceCount(void);
uint64_t ShootdownAbandonedCount(void);

/* The address most recently announced, for the self-test and the report. */
VirtualAddress ShootdownLastAddress(void);

/* Emits a summary upon the console and the serial port. */
void ShootdownReport(void);

#endif /* OXYS_SHOOTDOWN_H */
