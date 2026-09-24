/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/smp/ipi.c
 * Purpose: Implements the inter-processor interrupt layer: the composition of a
 *          command for each of the three audiences a sender may address, the
 *          accounting of what was sent and what arrived, and the handler by
 *          which a panicking processor stops the others.
 * Key functions: IpiInitialise, IpiSendToOthers, IpiSendToSelf,
 *          IpiSendToProcessor, IpiHaltOtherProcessors, IpiIsAvailable,
 *          IpiReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 10.6: issuing interprocessor interrupts.
 *   - Intel SDM, Volume 3A, Section 10.6.1 and Figure 10-12: the command
 *     register's fields, and the four destination shorthands.
 *   - Intel SDM, Volume 3A, Section 10.6.2.1: the destination field in xAPIC
 *     mode.
 *   - Intel SDM, Volume 3A, Section 10.8.5: every handler entered by the fixed
 *     delivery mode must write the end-of-interrupt register before returning.
 *   - Intel SDM, Volume 2B, "HLT": the instruction halts the processor until an
 *     interrupt, a debug exception, a non-maskable interrupt or a reset.
 *   - docs/design/CONCURRENCY.md.
 *
 * Why every send here carries the assert level and the edge trigger.
 *
 *   Intel SDM, Volume 3A, Section 10.6.1, and Table 10-4 with it, provide that
 *   for every delivery mode but INIT the level and trigger fields are ignored,
 *   and that the level-triggered de-assert form exists only for the INIT
 *   sequence of sub-task 6.14. The bits are nevertheless set explicitly rather
 *   than left zero: a field that is ignored is a field whose value the reader
 *   cannot infer from the code, and the two ignored bits here are the two that
 *   would silently turn a fixed interrupt into something else if the delivery
 *   mode were ever mistyped.
 */

#include <oxys/arch/smp/ipi.h>
#include <oxys/dev/lapic.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/arch/interrupt/interrupts.h>
#include <oxys/kernel.h>

/* Accounting. Written by the sender and by the handlers, all of it diagnostic. */
static uint64_t IpiSent;
static uint64_t IpiReceived;
static uint64_t IpiFailedSends;
static uint64_t IpiHaltRequests;
static bool IpiHandlersRegistered;

/*
 * The command every send composes, less its audience.
 *
 * The fixed delivery mode, because the vector is what carries the meaning; the
 * physical destination mode, because this kernel programmes no logical
 * destination and the identifiers it addresses are the ones the controllers
 * answer to; the assert level and the edge trigger, for the reason the file
 * header gives.
 */
static uint32_t IpiCommandFor(uint8_t vector)
{
    return ((uint32_t)vector & LAPIC_ICR_VECTOR_MASK) |
           LAPIC_ICR_DELIVERY_FIXED |
           LAPIC_ICR_DESTINATION_PHYSICAL |
           LAPIC_ICR_LEVEL_ASSERT;
}

/*
 * Receives IPI_VECTOR_HALT.
 *
 * The processor stops here and does not return to what it was doing. That is the
 * point: it is stopped because another processor has decided the machine is no
 * longer to be trusted, and the most useful thing it can do is stop changing the
 * state that is about to be reported.
 *
 * The end-of-interrupt is written first although the handler does not return.
 * The interrupt would otherwise remain in service at this processor's
 * controller, and a machine that is later resumed by a debugger — or examined by
 * one — would show an in-service register that says an interrupt is still being
 * handled, which is a fact about this handler and not about the failure.
 */
static void IpiHandleHalt(TrapFrame *frame)
{
    (void)frame;

    ++IpiReceived;
    ++PerCpuCurrent()->ipis_received;

    LocalApicSignalEndOfInterrupt();

    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

void IpiInitialise(void)
{
    InterruptRegisterHandler(IPI_VECTOR_HALT, IpiHandleHalt,
                             "interprocessor halt");

    IpiHandlersRegistered = true;
}

bool IpiIsAvailable(void)
{
    return IpiHandlersRegistered && LocalApicIsEnabled();
}

/*
 * The one place a send is actually made.
 *
 * The three public forms differ only in the shorthand and the destination, so
 * they differ only in their arguments here. The counters are kept in one place
 * for the same reason: a count that is incremented in three places is a count
 * that is wrong in one of them.
 */
static bool IpiSend(uint8_t destination, uint8_t vector, uint32_t shorthand)
{
    if (!IpiIsAvailable())
    {
        return false;
    }

    if (!LocalApicSendCommand(destination, IpiCommandFor(vector) | shorthand))
    {
        ++IpiFailedSends;
        return false;
    }

    ++IpiSent;

    return true;
}

bool IpiSendToOthers(uint8_t vector)
{
    /*
     * The destination is ignored where a shorthand is given, per Intel SDM,
     * Volume 3A, Section 10.6.1, and zero is passed rather than some plausible
     * identifier so that a reader is not left wondering which processor it names.
     */
    return IpiSend(0U, vector, LAPIC_ICR_SHORTHAND_ALL_BUT_SELF);
}

bool IpiSendToSelf(uint8_t vector)
{
    return IpiSend(0U, vector, LAPIC_ICR_SHORTHAND_SELF);
}

bool IpiSendToProcessor(uint8_t apic_identifier, uint8_t vector)
{
    return IpiSend(apic_identifier, vector, LAPIC_ICR_SHORTHAND_NONE);
}

void IpiHaltOtherProcessors(void)
{
    /*
     * A machine with one processor started has nobody to tell, and the send is
     * skipped rather than made and ignored. The distinction matters to the
     * report: a halt request counted against a machine that has one processor
     * would suggest something was stopped.
     */
    if (PerCpuOnlineCount() <= 1U)
    {
        return;
    }

    ++IpiHaltRequests;

    (void)IpiSendToOthers(IPI_VECTOR_HALT);
}

uint64_t IpiSentCount(void)
{
    return IpiSent;
}

uint64_t IpiReceivedCount(void)
{
    return IpiReceived;
}

uint64_t IpiFailedSendCount(void)
{
    return IpiFailedSends;
}

uint64_t IpiHaltRequestCount(void)
{
    return IpiHaltRequests;
}

/*
 * Counts a received interrupt on behalf of a handler that belongs to another
 * subsystem.
 *
 * The shootdown handler is the memory manager's, but the count of what this
 * layer carried belongs to this layer. Exposing the increment rather than the
 * counter keeps the one writable quantity in the file that owns it.
 */
void IpiNoteReceived(void)
{
    ++IpiReceived;
    ++PerCpuCurrent()->ipis_received;
}

void IpiReport(void)
{
    KernelWriteString("Interprocessor interrupts: ");

    if (!IpiIsAvailable())
    {
        KernelWriteString("unavailable, the local controller not being enabled.\n");
        return;
    }

    KernelWriteString("shootdown vector ");
    KernelWriteDecimal((uint64_t)IPI_VECTOR_SHOOTDOWN);
    KernelWriteString(", halt vector ");
    KernelWriteDecimal((uint64_t)IPI_VECTOR_HALT);
    KernelWriteString("; sent ");
    KernelWriteDecimal(IpiSent);
    KernelWriteString(", received ");
    KernelWriteDecimal(IpiReceived);
    KernelWriteString(", refused ");
    KernelWriteDecimal(IpiFailedSends);
    KernelWriteString(", halt requests ");
    KernelWriteDecimal(IpiHaltRequests);
    KernelWriteString(".\n");
}
