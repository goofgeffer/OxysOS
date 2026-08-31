/*
 * File: kernel/include/oxys/pic.h
 * Purpose: Declares the interface of the 8259A programmable interrupt controller
 *          driver: the remapping of the two cascaded controllers clear of the
 *          architecture-defined exception vectors, the masking of individual
 *          interrupt request lines, the routing of a request to a device
 *          handler, and the end-of-interrupt signalling that the controller
 *          requires before it will present a further request of equal or lower
 *          priority.
 * Key definitions: PIC_MASTER_VECTOR_BASE, PIC_SLAVE_VECTOR_BASE, PIC_IRQ_COUNT,
 *          PicInitialise, PicInstallHandler, PicRemoveHandler,
 *          PicRegisteredHandler, PicMaskLine, PicUnmaskLine, PicLineIsMasked,
 *          PicMaskValue, PicInServiceRegister, PicRequestRegister,
 *          PicRequestCount, PicSpuriousCount, PicUnclaimedCount, PicDisable,
 *          PicReport.
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
 */

#ifndef OXYS_PIC_H
#define OXYS_PIC_H

#include <oxys/types.h>
#include <oxys/interrupts.h>

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
 * bits of the vector being supplied by the request level.
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
 * PIC_SLAVE_VECTOR_BASE, installs the routing handler for the sixteen vectors so
 * produced, and masks every request line, the cascade included.
 *
 * Every line is left masked because a device whose driver does not yet exist
 * would otherwise raise a request that nothing could service or silence, and the
 * controller withholds every request of equal or lower priority until the one in
 * service is acknowledged. A driver unmasks its own line when it is ready.
 *
 * The interrupt descriptor table must have been loaded and the stubs installed
 * before this is called.
 */
void PicInitialise(void);

/*
 * Registers the handler to be entered when the given request line is presented,
 * replacing any handler previously registered for it.
 *
 * The handler is entered with the end-of-interrupt not yet signalled, and must
 * not signal it: the routing layer does so upon the handler's return, for the
 * reason recorded in the header of drivers/pic/pic.c.
 *
 * name: a short description used in diagnostic output. The string is not copied
 *     and must therefore have static storage duration.
 */
void PicInstallHandler(uint8_t irq, InterruptHandler handler, const char *name);

/* Removes the handler registered for a request line, if any. */
void PicRemoveHandler(uint8_t irq);

/* The handler registered for a request line, or NULL if none is registered. */
InterruptHandler PicRegisteredHandler(uint8_t irq);

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

/* The number of requests dispatched, and the number found to be spurious. */
uint64_t PicRequestCount(void);
uint64_t PicSpuriousCount(void);

/* The number of times a request line with no registered handler was presented. */
uint64_t PicUnclaimedCount(void);

/*
 * Masks every line of both controllers. Used in Phase 6, sub-task 6.7, when the
 * Local APIC and the I/O APIC supersede this device.
 */
void PicDisable(void);

/* Emits a summary of the controllers' state upon the console and the serial port. */
void PicReport(void);

#endif /* OXYS_PIC_H */
