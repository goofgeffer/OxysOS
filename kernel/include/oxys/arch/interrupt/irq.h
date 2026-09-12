/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/arch/interrupt/irq.h
 * Purpose: Declares the interrupt request layer, which is the one interface a
 *          device driver claims a request line through, whichever controller is
 *          presently delivering it: the registration of a handler, the masking
 *          and permitting of a line, the routing of a request to the driver that
 *          claimed it, and the adoption of the APIC in place of the 8259A pair.
 * Key definitions: IRQ_VECTOR_BASE, IRQ_LINE_COUNT, IrqController,
 *          IrqInitialise, IrqAdoptApic, IrqInstallHandler, IrqRemoveHandler,
 *          IrqRegisteredHandler, IrqMaskLine, IrqUnmaskLine, IrqLineIsMasked,
 *          IrqActiveController, IrqRequestCount, IrqSpuriousCount,
 *          IrqUnclaimedCount, IrqReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 6.2: vectors 0 to 31 are reserved to the
 *     architecture-defined exceptions, which is why request line n is presented
 *     as vector 32 + n and not as vector n.
 *   - ACPI Specification 6.5, Section 5.2.12.4: upon a machine supporting both
 *     interrupt models the first sixteen global system interrupts carry the
 *     8259A request lines, except where an Interrupt Source Override says
 *     otherwise. That is what allows one line number to mean the same device
 *     under either controller.
 *   - docs/design/INTERRUPTS.md, Section 10: why this layer exists, what it owns
 *     and what it deliberately does not.
 *
 * Why a device driver names a line and not a controller.
 *
 *   Until sub-task 6.12 every driver called PicInstallHandler and PicUnmaskLine,
 *   which named the 8259A in the source of drivers that have nothing to do with
 *   it. That was accurate while the 8259A was the only controller. The moment it
 *   is retired those calls become a statement that is no longer true, and each
 *   driver would acquire a decision — which controller am I upon? — that is not
 *   its business and that four drivers would answer four times.
 *
 *   The line number is the durable fact. IR1 is the keyboard whether the request
 *   arrives from a 8259A upon vector 33 or from an I/O APIC input carrying global
 *   system interrupt 1 upon vector 33. This layer holds the one place that knows
 *   which controller is answering.
 */

#ifndef OXYS_ARCH_INTERRUPT_IRQ_H
#define OXYS_ARCH_INTERRUPT_IRQ_H

#include <oxys/types.h>
#include <oxys/arch/interrupt/interrupts.h>

/*
 * The vector at which the request lines begin, and how many lines this layer
 * carries.
 *
 * Thirty-two is the first vector Intel SDM, Volume 3A, Section 6.2, leaves
 * available. Sixteen lines is what the cascaded 8259A pair provides, and it is
 * also the range that ACPI 6.5, Section 5.2.12.4, guarantees the I/O APIC
 * carries the same devices upon; an input above that has no 8259A counterpart
 * and no driver in this kernel, and is recorded as a limitation rather than
 * given a number here.
 */
#define IRQ_VECTOR_BASE UINT8_C(32)
#define IRQ_LINE_COUNT  16U

/* The vector upon which a request line is presented. */
#define IRQ_VECTOR_FOR_LINE(line) ((uint8_t)(IRQ_VECTOR_BASE + (line)))

/* Which controller is presently delivering requests. */
typedef enum IrqController
{
    /* Nothing has been initialised; no request can be delivered. */
    IRQ_CONTROLLER_NONE = 0,
    /* The cascaded pair of 8259A controllers of sub-task 3.5. */
    IRQ_CONTROLLER_8259A,
    /* The I/O APIC and the Local APIC of sub-task 6.12. */
    IRQ_CONTROLLER_APIC
} IrqController;

/*
 * Initialises the 8259A pair and installs the routing handler for the sixteen
 * vectors it presents. Every line is left masked and unclaimed.
 *
 * The routing handler is installed for all sixteen rather than for the lines a
 * driver claims, so that a request upon an unclaimed line is acknowledged rather
 * than left standing in the controller's in-service register, where it would
 * withhold every request of lower priority for the remainder of the machine's
 * life.
 *
 * The interrupt descriptor table must have been loaded and the stubs installed.
 */
void IrqInitialise(void);

/*
 * Retires the 8259A pair in favour of the I/O APIC and the Local APIC, carrying
 * every claimed line across.
 *
 * Each claimed line is resolved to the global system interrupt that carries it,
 * programmed into the redirection table with the vector it already had and the
 * polarity and trigger mode the ACPI tables declare, and left masked or
 * unmasked exactly as it was. A driver observes nothing: it keeps the same line
 * number, the same vector and the same handler.
 *
 * Must be called with the interrupt flag clear. Between the masking of the
 * 8259A and the programming of the redirection table there is no controller
 * that will deliver a device's request, and a request raised in that interval
 * would be lost.
 *
 * LocalApicInitialise and IoApicInitialise must both have succeeded. Returns
 * false, leaving the 8259A in charge, where either did not.
 */
bool IrqAdoptApic(void);

/*
 * Registers the handler to be entered when the given request line is presented,
 * replacing any handler previously registered for it.
 *
 * The handler is entered with the completion of the request not yet signalled,
 * and must not signal it: this layer does so upon the handler's return, at
 * whichever controller delivered the request. See docs/design/INTERRUPTS.md,
 * Section 10.3.
 *
 * name: a short description used in diagnostic output. The string is not copied
 *     and must therefore have static storage duration.
 */
void IrqInstallHandler(uint8_t line, InterruptHandler handler, const char *name);

/* Removes the handler registered for a request line, if any, and masks the line:
 * a line whose driver has gone is a line nothing can service. */
void IrqRemoveHandler(uint8_t line);

/* The handler registered for a request line, or NULL if none is registered. */
InterruptHandler IrqRegisteredHandler(uint8_t line);

/* The name the driver claiming a line supplied, or NULL. */
const char *IrqLineName(uint8_t line);

/* Withholds and permits the delivery of a request line at whichever controller
 * is presently answering. */
void IrqMaskLine(uint8_t line);
void IrqUnmaskLine(uint8_t line);

/* Whether a request line is presently withheld. */
bool IrqLineIsMasked(uint8_t line);

/* Which controller is presently delivering requests, and its name. */
IrqController IrqActiveController(void);
const char *IrqControllerName(IrqController controller);

/*
 * The global system interrupt a line is carried by while the APIC is answering.
 * Equal to the line number save where the firmware declared an override. Zero
 * where no APIC is in use.
 */
uint32_t IrqGlobalInterruptForLine(uint8_t line);

/* The number of requests routed, the number found spurious, and the number
 * presented upon a line no driver had claimed. */
uint64_t IrqRequestCount(void);
uint64_t IrqSpuriousCount(void);
uint64_t IrqUnclaimedCount(void);

/* Emits a summary of the lines and their claimants upon the console and the
 * serial port. */
void IrqReport(void);

#endif /* OXYS_ARCH_INTERRUPT_IRQ_H */
