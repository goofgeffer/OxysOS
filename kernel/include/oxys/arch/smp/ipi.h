/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/arch/smp/ipi.h
 * Purpose: Declares the inter-processor interrupt layer: the vectors this
 *          kernel reserves for one processor to address another, the three ways
 *          an audience is named, and the handler by which a panicking processor
 *          stops the others before it prints.
 * Key definitions: IPI_VECTOR_SHOOTDOWN, IPI_VECTOR_HALT, IpiInitialise,
 *          IpiSendToOthers, IpiSendToSelf, IpiSendToProcessor,
 *          IpiHaltOtherProcessors, IpiIsAvailable, IpiSentCount,
 *          IpiReceivedCount, IpiReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 10.6 (Issuing Interprocessor Interrupts): an interrupt is sent by
 *     writing the interrupt command register of the sending processor's local
 *     controller, and is received by the target exactly as an interrupt from a
 *     device would be — the same vector, the same gate, the same
 *     end-of-interrupt.
 *   - Intel SDM, Volume 3A, Section 10.6.1: the destination shorthands, of which
 *     "all excluding self" and "self" are the two used here.
 *   - Intel SDM, Volume 3A, Section 10.5.2: vectors 16 to 255 are valid; a
 *     vector below 16 is refused by the controller and recorded as an error.
 *   - Intel SDM, Volume 3A, Section 10.8.5: a fixed-delivery interrupt is in
 *     service until its handler writes the end-of-interrupt register, so a
 *     handler here must signal exactly as a device handler does.
 *   - docs/design/CONCURRENCY.md, Section 5.
 *
 * Why the vectors are the highest available.
 *
 *   The local controller's own two vectors are 0xFF and 0xFE, chosen there
 *   because 0xFF's low four bits are hardwired upon some processors. These sit
 *   immediately below them and above every vector a device can be routed to,
 *   which tops out at 47. Intel SDM, Volume 3A, Section 10.8.3, makes a vector's
 *   number its priority, so an interrupt one processor sends to another is
 *   served ahead of any device request the target may also be holding — which is
 *   what a shootdown requires, the sender being stopped until the target answers.
 */

#ifndef OXYS_ARCH_SMP_IPI_H
#define OXYS_ARCH_SMP_IPI_H

#include <oxys/types.h>
#include <oxys/arch/interrupt/interrupts.h>

/*
 * The translation-lookaside-buffer shootdown, and the general halt.
 *
 * The shootdown is the reason this layer exists: docs/design/CONCURRENCY.md,
 * Section 6, describes what it carries. The halt is what a panicking processor
 * sends before it prints, so that the machine being explained stops changing
 * while the explanation is written.
 */
#define IPI_VECTOR_SHOOTDOWN UINT8_C(0xFD)
#define IPI_VECTOR_HALT      UINT8_C(0xFC)

/*
 * Registers the handler for IPI_VECTOR_HALT and makes this layer available.
 *
 * It is called after the local controller has been enabled and before anything
 * may send: a vector delivered to a processor that has registered no handler for
 * it is counted as unhandled and discarded, so an interrupt sent before this
 * would be received by nobody and a sender waiting for its effect would wait out
 * its bound.
 *
 * IPI_VECTOR_SHOOTDOWN is registered by ShootdownInitialise instead, the handler
 * belonging to the memory manager and not to this layer. The division is the
 * same one the interrupt request layer makes: this layer carries interrupts
 * between processors and knows nothing of what any of them means.
 */
void IpiInitialise(void);

/* Whether an interrupt may be sent at all, being whether the local controller is
 * enabled. Every send below returns false where it is not, and every caller
 * treats that as "there is nobody to tell", which upon a machine with one
 * processor is the truth. */
bool IpiIsAvailable(void);

/*
 * Sends `vector` to every processor except the executing one.
 *
 * This is the shootdown's audience and the halt's. The shorthand is used rather
 * than a list of identifiers because the set is exactly what the shorthand
 * names, and because a list would have to be walked with the local controller's
 * command register held between entries — during which another processor's send
 * would find it busy.
 */
bool IpiSendToOthers(uint8_t vector);

/*
 * Sends `vector` to the executing processor.
 *
 * It exists because this kernel has started one processor and must nevertheless
 * demonstrate that the mechanism works. Intel SDM, Volume 3A, Section 10.6.1,
 * gives the self shorthand as a delivery like any other: the same vector, the
 * same gate, the same end-of-interrupt. The self-test of sub-task 6.13 uses it
 * to make the shootdown handler run and to observe the invalidation it performs,
 * which is the whole of what an application processor does with it since 6.14.
 *
 * The interrupt is delivered when the processor next accepts one, so a caller
 * that waits for its effect must have interrupts enabled. A caller that does not
 * — inside a spinlock, say — would wait for a handler that cannot run.
 */
bool IpiSendToSelf(uint8_t vector);

/* Sends `vector` to the one processor whose local controller answers to
 * `apic_identifier`. No shorthand: the identifier goes into the destination
 * field. */
bool IpiSendToProcessor(uint8_t apic_identifier, uint8_t vector);

/*
 * Stops every other processor.
 *
 * Called by KernelPanic before it prints. A machine that has failed keeps
 * running upon its other processors, and each of them goes on writing to the
 * structures whose state the report is about — so the report describes a machine
 * that no longer exists by the time it is read. This does not stop them
 * gracefully and is not meant to: they clear the interrupt flag and halt.
 *
 * It returns rather than waiting for them, because the caller is a panic and a
 * panic that waited could be stopped by the very processor it is trying to stop.
 */
void IpiHaltOtherProcessors(void);

/* The number of interrupts this kernel has sent through this layer, the number
 * its handlers have received, and the number of sends the controller refused. */
uint64_t IpiSentCount(void);
uint64_t IpiReceivedCount(void);
uint64_t IpiFailedSendCount(void);

/* The number of processors halted by IpiHaltOtherProcessors, which is a count of
 * the requests made and not of the processors that obeyed. */
uint64_t IpiHaltRequestCount(void);

/*
 * Counts one received interrupt, on behalf of a handler this layer does not own.
 *
 * The shootdown handler belongs to the memory manager, but the count of what
 * this layer carried belongs here; exposing the increment rather than the
 * counter keeps the one writable quantity in the file that owns it.
 */
void IpiNoteReceived(void);

/* Emits a summary upon the console and the serial port. */
void IpiReport(void);

#endif /* OXYS_ARCH_SMP_IPI_H */
