/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/pic/pic.c
 * Purpose: Implements the driver for the pair of cascaded Intel 8259A
 *          programmable interrupt controllers of the IBM Personal Computer AT:
 *          their remapping clear of the architecture-defined exception vectors,
 *          the masking of individual request lines, the detection of a spurious
 *          request, the end-of-interrupt signalling the device requires, and the
 *          silencing of the pair when the APIC supersedes it.
 * Key functions: PicInitialise, PicMaskLine, PicUnmaskLine, PicLineIsMasked,
 *          PicMaskValue, PicInServiceRegister, PicRequestRegister,
 *          PicRequestIsSpurious, PicSendEndOfInterrupt, PicIsInitialised,
 *          PicDisable, PicReport.
 * References:
 *   - Intel 8259A Programmable Interrupt Controller datasheet (order number
 *     231468-003), section "INITIALIZATION COMMAND WORDS (ICWS)": a write to the
 *     command port with D4 set is interpreted as ICW1 and begins the
 *     initialisation sequence; ICW1 bit D0 (IC4) states that ICW4 will follow and
 *     bit D1 (SNGL) distinguishes a single controller from a cascaded pair; ICW2
 *     supplies bits 7 to 3 of the vector, the low three bits being supplied by
 *     the request level; ICW3 names the lines bearing slaves at the master and
 *     the cascade identity at the slave; ICW4 bit D0 selects the 8086 mode.
 *   - 8259A datasheet, same section: issuing ICW1 resets the edge sense circuit,
 *     clears the interrupt mask register, assigns IR7 the lowest priority, sets
 *     the slave mode address to seven, clears the special mask mode and sets the
 *     status read to the interrupt request register. The clearing of the mask
 *     register is why every line is masked explicitly after the sequence and not
 *     before it.
 *   - 8259A datasheet, section "OPERATION COMMAND WORDS (OCWS)": OCW1 is the
 *     interrupt mask register at the data port, a set bit withholding the
 *     corresponding line; OCW2 with R=0, SL=0 and EOI=1 is the non-specific
 *     end-of-interrupt, which resets the highest priority bit set in the
 *     in-service register; OCW3 with RR set selects the register subsequently
 *     read at the command port, RIS choosing the in-service register when set and
 *     the interrupt request register when clear.
 *   - IBM Personal Computer AT technical reference: the master controller is
 *     decoded at ports 0x20 and 0x21 and the slave at 0xA0 and 0xA1; the slave's
 *     output is attached to the master's IR2 input, whence IR2 is unavailable as
 *     an ordinary request line; IR0 is the interval timer and IR1 the keyboard.
 *     The firmware programmes the master to vectors 8 to 15 and the slave to
 *     0x70 to 0x77; the controller itself holds no vector base until ICW2 is
 *     written, so those values are a property of the firmware and not of the
 *     device, and they are the state in which the kernel receives the machine.
 *   - Intel SDM, Volume 3A, Section 6.2: vectors 0 to 31 are reserved to the
 *     architecture-defined exceptions; 32 to 255 are available.
 *
 * Why this module owns the end-of-interrupt, and why it does not own the routing.
 *
 *   The controller withholds every request of equal or lower priority until the
 *   bit standing in its in-service register is reset. A driver that neglected to
 *   signal completion would therefore silence its own device permanently, and,
 *   the timer being the highest priority line, would in most cases silence the
 *   whole machine. The signalling is a property of the controller rather than of
 *   any device, so PicSendEndOfInterrupt is written here, once, and a device
 *   driver neither may nor need call it.
 *
 *   The same reasoning governs the cascade. A request of the slave controller
 *   stands in the in-service register of both, the master having accepted it upon
 *   IR2, so both must be signalled; sending only the slave's would leave the
 *   master withholding every line of priority below IR2.
 *
 *   The table of device handlers is the other half of that argument and comes
 *   out the other way. Which driver claims IR1 is a property of the machine and
 *   not of this device: from sub-task 6.12 the same line is delivered by an I/O
 *   APIC upon a machine where this pair has been retired, and a table held here
 *   would have to be read out and carried across, or duplicated. It is therefore
 *   held in kernel/cpu/irq.c, which calls into this file for the two operations
 *   above and for the mask registers.
 *
 * Concurrency. The mask registers are unsynchronised. Until the interrupt flag is
 * set there is one flow of control and the question does not arise; from sub-task
 * 6.13 there is a spinlock the read-modify-write of a mask register requires,
 * since an interrupt handler and an application processor may both enter it. It
 * has not been applied here, and it remains unnecessary for two reasons at once:
 * a processor started by sub-task 6.14 is parked and reaches nothing here, and
 * since sub-task 6.12 this pair is masked and retired in favour of the APIC, so
 * the only writer left is the retirement itself. Sub-task 6.15 is what would make
 * the register contended if anything still drove it.
 */

#include <oxys/pic.h>
#include <oxys/io.h>
#include <oxys/kernel.h>

/* The command and data ports of the two controllers. */
#define PIC_MASTER_COMMAND UINT16_C(0x0020)
#define PIC_MASTER_DATA    UINT16_C(0x0021)
#define PIC_SLAVE_COMMAND  UINT16_C(0x00A0)
#define PIC_SLAVE_DATA     UINT16_C(0x00A1)

/*
 * Initialisation command word 1. Bit 4 identifies the word and begins the
 * sequence; bit 0 states that ICW4 will follow. Bit 1 is left clear, denoting a
 * cascaded rather than a single controller, and bit 3 is left clear, selecting
 * edge-triggered inputs, which is the wiring of the AT.
 */
#define PIC_ICW1_ICW4_FOLLOWS UINT8_C(0x01)
#define PIC_ICW1_INITIALISE   UINT8_C(0x10)

/*
 * Initialisation command word 3. At the master a set bit denotes a request line
 * bearing a slave; at the slave the low three bits carry its cascade identity,
 * which must equal the master line it is attached to.
 */
#define PIC_ICW3_MASTER_SLAVE_ON_IR2 UINT8_C(0x04)
#define PIC_ICW3_SLAVE_IDENTITY_TWO  UINT8_C(0x02)

/* Initialisation command word 4. Bit 0 selects the 8086 mode, in which the
 * controller presents an eight-bit vector rather than a CALL instruction. */
#define PIC_ICW4_8086_MODE UINT8_C(0x01)

/* Operation command word 2: the non-specific end-of-interrupt, R=0, SL=0, EOI=1. */
#define PIC_OCW2_END_OF_INTERRUPT UINT8_C(0x20)

/*
 * Operation command word 3. Bit 3 identifies the word and bit 1 (RR) states that
 * a register is to be read at the command port; bit 0 (RIS) selects the
 * in-service register when set and the interrupt request register when clear.
 */
#define PIC_OCW3_READ_REQUEST_REGISTER    UINT8_C(0x0A)
#define PIC_OCW3_READ_IN_SERVICE_REGISTER UINT8_C(0x0B)

/* Every line masked, both controllers. */
#define PIC_MASK_ALL UINT8_C(0xFF)

/* Whether PicInitialise has run and PicDisable has not. Reported, and consulted
 * by PicReport so that a summary taken before initialisation, or after the
 * device was retired, cannot be mistaken for one taken while it was answering. */
static bool PicInitialised;

/* Whether PicDisable has retired the pair in favour of the APIC. */
static bool PicRetired;

/*
 * Reads one of the two status registers of both controllers through OCW3.
 *
 * The word written selects the register; the subsequent read of the same port
 * returns it. The selection persists in the controller, which is why the request
 * register is selected explicitly rather than relied upon as the state ICW1
 * leaves behind: an intervening read of the in-service register would otherwise
 * make this function return the wrong register.
 */
static uint16_t PicReadStatusRegister(uint8_t command)
{
    uint8_t master;
    uint8_t slave;

    PortWriteByte(PIC_MASTER_COMMAND, command);
    PortWriteByte(PIC_SLAVE_COMMAND, command);

    master = PortReadByte(PIC_MASTER_COMMAND);
    slave = PortReadByte(PIC_SLAVE_COMMAND);

    return (uint16_t)(((uint16_t)slave << 8) | (uint16_t)master);
}

uint16_t PicInServiceRegister(void)
{
    return PicReadStatusRegister(PIC_OCW3_READ_IN_SERVICE_REGISTER);
}

uint16_t PicRequestRegister(void)
{
    return PicReadStatusRegister(PIC_OCW3_READ_REQUEST_REGISTER);
}

uint16_t PicMaskValue(void)
{
    const uint8_t master = PortReadByte(PIC_MASTER_DATA);
    const uint8_t slave = PortReadByte(PIC_SLAVE_DATA);

    return (uint16_t)(((uint16_t)slave << 8) | (uint16_t)master);
}

bool PicLineIsMasked(uint8_t irq)
{
    if (irq >= PIC_IRQ_COUNT)
    {
        return true;
    }

    return (PicMaskValue() & (uint16_t)(UINT16_C(1) << irq)) != 0U;
}

void PicMaskLine(uint8_t irq)
{
    if (irq >= PIC_IRQ_COUNT)
    {
        return;
    }

    if (irq < 8U)
    {
        const uint8_t mask = PortReadByte(PIC_MASTER_DATA);

        PortWriteByte(PIC_MASTER_DATA, (uint8_t)(mask | (uint8_t)(1U << irq)));
    }
    else
    {
        const uint8_t mask = PortReadByte(PIC_SLAVE_DATA);

        PortWriteByte(PIC_SLAVE_DATA, (uint8_t)(mask | (uint8_t)(1U << (irq - 8U))));
    }
}

void PicUnmaskLine(uint8_t irq)
{
    if (irq >= PIC_IRQ_COUNT)
    {
        return;
    }

    if (irq < 8U)
    {
        const uint8_t mask = PortReadByte(PIC_MASTER_DATA);

        PortWriteByte(PIC_MASTER_DATA, (uint8_t)(mask & (uint8_t)~(1U << irq)));
    }
    else
    {
        const uint8_t mask = PortReadByte(PIC_SLAVE_DATA);

        PortWriteByte(PIC_SLAVE_DATA, (uint8_t)(mask & (uint8_t)~(1U << (irq - 8U))));

        /*
         * A request of the slave reaches the processor only by way of the
         * master's IR2. Unmasking a slave line while the cascade remained masked
         * would silently accomplish nothing, and the defect would present as a
         * device that never interrupts.
         */
        PicUnmaskLine(PIC_CASCADE_IRQ);
    }
}

/*
 * Signals the completion of a request.
 *
 * A request of the slave controller stands in the in-service register of both
 * controllers, the master having accepted it upon its cascade input, so the
 * slave is signalled first and the master afterwards. The order is immaterial to
 * the hardware but the slave is taken first for consistency with the order in
 * which the two accepted the request.
 */
void PicSendEndOfInterrupt(uint8_t irq)
{
    if (irq >= 8U)
    {
        PortWriteByte(PIC_SLAVE_COMMAND, PIC_OCW2_END_OF_INTERRUPT);
    }

    PortWriteByte(PIC_MASTER_COMMAND, PIC_OCW2_END_OF_INTERRUPT);
}

/*
 * Determines whether a request upon the lowest priority line of either
 * controller is spurious.
 *
 * The 8259A presents its lowest priority line, IR7, when a request it had begun
 * to accept is withdrawn before the vector is read, most often because of noise
 * upon the line or because an earlier handler signalled completion at the wrong
 * moment. No line is genuinely in service, so the bit is absent from the
 * in-service register, and that absence is the only means of distinguishing the
 * case.
 *
 * A spurious request must not be acknowledged, there being nothing to
 * acknowledge; an end-of-interrupt sent in that state would reset the bit of
 * whatever line was genuinely in service and lose a real interrupt. The one
 * exception is the slave's IR15: the master did accept the cascade request and
 * does hold a bit in its own in-service register, so the master alone is
 * signalled.
 */
bool PicRequestIsSpurious(uint8_t irq)
{
    uint16_t in_service;

    if (irq != PIC_MASTER_SPURIOUS_IRQ && irq != PIC_SLAVE_SPURIOUS_IRQ)
    {
        return false;
    }

    in_service = PicInServiceRegister();

    if ((in_service & (uint16_t)(UINT16_C(1) << irq)) != 0U)
    {
        return false;
    }

    if (irq == PIC_SLAVE_SPURIOUS_IRQ)
    {
        /* The master accepted the cascade request and must be released. */
        PortWriteByte(PIC_MASTER_COMMAND, PIC_OCW2_END_OF_INTERRUPT);
    }

    return true;
}

void PicInitialise(void)
{
    /*
     * The initialisation sequence must be issued without interruption, each word
     * being written to the port the controller expects next. IoWait separates the
     * writes because the controller of an original AT requires a settling
     * interval between them that a modern processor would otherwise not provide.
     */
    PortWriteByte(PIC_MASTER_COMMAND,
                  (uint8_t)(PIC_ICW1_INITIALISE | PIC_ICW1_ICW4_FOLLOWS));
    IoWait();
    PortWriteByte(PIC_SLAVE_COMMAND,
                  (uint8_t)(PIC_ICW1_INITIALISE | PIC_ICW1_ICW4_FOLLOWS));
    IoWait();

    /* ICW2: the vector base of each controller. */
    PortWriteByte(PIC_MASTER_DATA, PIC_MASTER_VECTOR_BASE);
    IoWait();
    PortWriteByte(PIC_SLAVE_DATA, PIC_SLAVE_VECTOR_BASE);
    IoWait();

    /* ICW3: the cascade wiring, expressed differently at each controller. */
    PortWriteByte(PIC_MASTER_DATA, PIC_ICW3_MASTER_SLAVE_ON_IR2);
    IoWait();
    PortWriteByte(PIC_SLAVE_DATA, PIC_ICW3_SLAVE_IDENTITY_TWO);
    IoWait();

    /* ICW4: the 8086 mode. */
    PortWriteByte(PIC_MASTER_DATA, PIC_ICW4_8086_MODE);
    IoWait();
    PortWriteByte(PIC_SLAVE_DATA, PIC_ICW4_8086_MODE);
    IoWait();

    /*
     * ICW1 cleared both mask registers, so every line is presently permitted.
     * Mask them all before anything can be delivered. A device whose driver does
     * not exist would otherwise present a request that nothing could service, and
     * the controller would withhold every request of lower priority thereafter.
     */
    PortWriteByte(PIC_MASTER_DATA, PIC_MASK_ALL);
    PortWriteByte(PIC_SLAVE_DATA, PIC_MASK_ALL);

    PicInitialised = true;
}

void PicDisable(void)
{
    PortWriteByte(PIC_MASTER_DATA, PIC_MASK_ALL);
    PortWriteByte(PIC_SLAVE_DATA, PIC_MASK_ALL);

    /*
     * The device is retired, not merely quiescent, so the flag that governs the
     * report is cleared with it and a second flag records why. Were the first
     * left set, PicReport would continue to describe a controller that is
     * remapped and answering after the APIC had superseded it, which is
     * precisely the sort of stale diagnostic that sends an investigation in the
     * wrong direction.
     */
    PicInitialised = false;
    PicRetired = true;
}

bool PicIsInitialised(void)
{
    return PicInitialised;
}

void PicReport(void)
{
    KernelWriteString("8259A: ");

    if (!PicInitialised)
    {
        /*
         * The two cases are distinguished because they mean opposite things. A
         * pair that was never initialised is a kernel that has not got that far;
         * a pair that was retired is a machine running upon its APIC, which is
         * the intended state and not a defect. A single "not initialised" would
         * have sent a reader of the log looking for the wrong fault.
         */
        KernelWriteString(PicRetired ? "retired in favour of the APIC; mask "
                                     : "not initialised; mask ");
        KernelWriteHexadecimal((uint64_t)PicMaskValue());
        KernelWriteString(".\n");
        return;
    }

    KernelWriteString("remapped to vectors ");
    KernelWriteDecimal((uint64_t)PIC_MASTER_VECTOR_BASE);
    KernelWriteString(" to ");
    KernelWriteDecimal((uint64_t)PIC_MASTER_VECTOR_BASE + PIC_IRQ_COUNT - 1U);
    KernelWriteString(", mask ");
    KernelWriteHexadecimal((uint64_t)PicMaskValue());
    KernelWriteString(", in service ");
    KernelWriteHexadecimal((uint64_t)PicInServiceRegister());
    KernelWriteString(".\n");

    /*
     * Which lines are claimed, and by what, is reported by IrqReport: the
     * claimants are recorded there and not here, for the reason given in the
     * header of this file. This report is confined to what only this device can
     * answer — the vectors it presents, and the state of its registers.
     */
}
