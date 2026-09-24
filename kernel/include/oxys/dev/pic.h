/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/dev/pic.h
 * Purpose: Declares the interface of the 8259A programmable interrupt controller
 *          driver: the remapping of the two cascaded controllers clear of the
 *          architecture-defined exception vectors, the masking of individual
 *          interrupt request lines, the recognition of a spurious request, the
 *          end-of-interrupt signalling that the controller requires before it
 *          will present a further request of equal or lower priority, and the
 *          silencing of the pair when the APIC supersedes it.
 * Key definitions: PIC_MASTER_VECTOR_BASE, PIC_SLAVE_VECTOR_BASE, PIC_IRQ_COUNT,
 *          PicInitialise, PicMaskLine, PicUnmaskLine, PicLineIsMasked,
 *          PicMaskValue, PicInServiceRegister, PicRequestRegister,
 *          PicRequestIsSpurious, PicSendEndOfInterrupt, PicIsInitialised,
 *          PicDisable, PicReport.
 * References:
 *   - Intel 8259A Programmable Interrupt Controller datasheet (order number
 *     231468-003), section "INITIALIZATION COMMAND WORDS (ICWS)": the
 *     initialisation sequence ICW1, ICW2, ICW3 and ICW4, and the requirement
 *     that ICW1 be issued first, whereupon the edge sense circuit is reset, the
 *     interrupt mask register is cleared, IR7 is assigned the lowest priority,
 *     the special mask mode is cleared and the status read is set to the
 *     interrupt request register.
 *   - 8259A datasheet, section "OPERATION COMMAND WORDS (OCWS)": OCW1 is the
 *     interrupt mask register, reached at the data port; OCW2 carries the R, SL
 *     and EOI bits, of which the encoding R=0, SL=0, EOI=1 is the non-specific
 *     end-of-interrupt command; OCW3 carries the RR and RIS bits that select the
 *     interrupt request register or the in-service register for reading at the
 *     command port.
 *   - IBM Personal Computer AT technical reference: the master controller is
 *     decoded at I/O ports 0x20 and 0x21 and the slave at 0xA0 and 0xA1, the
 *     slave's output being attached to the master's IR2 input; and the firmware
 *     programmes the master to vectors 8 to 15 and the slave to 0x70 to 0x77,
 *     which is the state in which the kernel receives the machine.
 *   - Intel SDM, Volume 3A, Section 6.2: vectors 0 to 31 are reserved to the
 *     architecture-defined exceptions and vectors 32 to 255 are available, which
 *     is why the controllers must be remapped before any request line is
 *     unmasked.
 *   - ACPI Specification 6.5, Table 5.20: where the firmware declares that the
 *     machine also carries this pair, its vectors must be masked before the APIC
 *     is enabled. PicDisable is what performs that.
 *
 * What this driver is not.
 *
 *   It holds no handler table and routes nothing. A device driver claims a
 *   request line through <oxys/arch/interrupt/irq.h>, which owns the routing for whichever
 *   controller is answering and calls this one for the parts that are properties
 *   of this device: the mask registers, the spurious request, and the
 *   end-of-interrupt. The division was made at sub-task 6.12, when a second
 *   controller made "install a handler upon the 8259A" a statement a keyboard
 *   driver had no business making.
 */

#ifndef OXYS_DEV_PIC_H
#define OXYS_DEV_PIC_H

#include <oxys/types.h>

/*
 * The vectors to which the two controllers are remapped.
 *
 * The 8259A has no vector base of its own; it holds whatever ICW2 last supplied.
 * The firmware of the IBM Personal Computer AT and its successors programmes the
 * master to present vectors 8 to 15 and the slave 0x70 to 0x77, and that is the
 * state in which the kernel receives the machine. The first of those ranges is
 * precisely the one the architecture reserves for the double fault, the invalid
 * task state segment, the segment-not-present fault, the stack-segment fault,
 * the general protection fault and the page fault, so a timer interrupt would be
 * indistinguishable from a double fault.
 *
 * Thirty-two is the first vector Intel SDM, Volume 3A, Section 6.2, leaves
 * available, and the 8259A requires a base divisible by eight, the low three
 * bits of the vector being supplied by the request level. They are the same
 * vectors <oxys/arch/interrupt/irq.h> presents its lines upon, and deliberately: a line keeps
 * its vector across the change of controller.
 */
#define PIC_MASTER_VECTOR_BASE UINT8_C(32)
#define PIC_SLAVE_VECTOR_BASE  UINT8_C(40)

/* The number of interrupt request lines the cascaded pair provides. */
#define PIC_IRQ_COUNT          16U

/* The request line upon which the slave controller's output is cascaded. */
#define PIC_CASCADE_IRQ        UINT8_C(2)

/*
 * The two request lines upon which a spurious interrupt is delivered, being the
 * lowest priority line of each controller.
 */
#define PIC_MASTER_SPURIOUS_IRQ UINT8_C(7)
#define PIC_SLAVE_SPURIOUS_IRQ  UINT8_C(15)

/*
 * Remaps the two controllers to PIC_MASTER_VECTOR_BASE and
 * PIC_SLAVE_VECTOR_BASE and masks every request line, the cascade included.
 *
 * Every line is left masked because a device whose driver does not yet exist
 * would otherwise raise a request that nothing could service or silence, and the
 * controller withholds every request of equal or lower priority until the one in
 * service is acknowledged. A driver unmasks its own line when it is ready.
 *
 * It is called by IrqInitialise, which installs the routing handler for the
 * vectors this remapping produces; calling it alone would remap a controller
 * whose requests had nowhere to go.
 */
void PicInitialise(void);

/*
 * Withholds and permits the delivery of a request line by setting and clearing
 * its bit in the interrupt mask register of the controller that owns it.
 * Unmasking a line of the slave controller implicitly requires the cascade line
 * to be unmasked, which PicUnmaskLine performs.
 */
void PicMaskLine(uint8_t irq);
void PicUnmaskLine(uint8_t irq);

/* Reports whether a request line is presently masked. */
bool PicLineIsMasked(uint8_t irq);

/*
 * The combined interrupt mask register of both controllers, the master in the
 * low eight bits and the slave in the high eight. A set bit denotes a masked
 * line.
 */
uint16_t PicMaskValue(void);

/*
 * The combined in-service and interrupt request registers of both controllers,
 * read through OCW3, with the master in the low eight bits.
 */
uint16_t PicInServiceRegister(void);
uint16_t PicRequestRegister(void);

/*
 * Determines whether a request upon the lowest priority line of either
 * controller is spurious, and releases the master where the slave's is.
 *
 * A spurious request must not be acknowledged, and this is the only means of
 * recognising one; see the commentary upon the implementation. It is called by
 * the routing layer of kernel/arch/x86_64/interrupt/irq.c before a handler is entered, and by
 * nothing else.
 */
bool PicRequestIsSpurious(uint8_t irq);

/*
 * Signals the completion of a request at whichever controllers accepted it.
 *
 * Called by the routing layer upon the return of a device handler. A device
 * driver neither may nor need call it: the signalling is a property of this
 * controller rather than of any device, and forgetting it would silence not the
 * device but every line of lower priority. See docs/design/INTERRUPTS.md.
 */
void PicSendEndOfInterrupt(uint8_t irq);

/* Whether PicInitialise has run and PicDisable has not. */
bool PicIsInitialised(void);

/*
 * Masks every line of both controllers, retiring the device.
 *
 * Called by IrqAdoptApic at sub-task 6.12, before the redirection tables of the
 * I/O APIC are programmed. ACPI 6.5, Table 5.20, requires it of any machine that
 * declares both interrupt models: two controllers presenting one device upon one
 * vector would deliver every request twice.
 */
void PicDisable(void);

/* Emits a summary of the controllers' state upon the console and the serial port. */
void PicReport(void);

#endif /* OXYS_DEV_PIC_H */
