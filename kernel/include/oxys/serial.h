/*
 * File: kernel/include/oxys/serial.h
 * Purpose: Declares the interface of the interrupt-driven 16550 serial driver,
 *          which provides the diagnostic channel that is independent of the
 *          display hardware, together with the line parameters, the buffering
 *          discipline and the accounting that the boot-time self-test reads.
 * Key definitions: SERIAL_COM1_PORT, SERIAL_COM1_IRQ, SerialParity,
 *          SerialStopBits, SerialConfiguration, SerialInitialise,
 *          SerialConfigure, SerialActivateInterrupts, SerialPutCharacter,
 *          SerialWriteString, SerialFlush, SerialReadCharacter,
 *          SerialLoopbackTest, SerialReport.
 * References:
 *   - National Semiconductor PC16550D Universal Asynchronous Receiver/Transmitter
 *     datasheet, Table 2 ("Register Addresses"): the registers lie at offsets 0
 *     to 7 from the base address, the divisor latches replacing the data and
 *     interrupt enable registers while the divisor latch access bit is set.
 *   - PC16550D datasheet, Table 1 ("Summary of Registers"): the bit assignments
 *     of the line control, interrupt enable, line status and modem control
 *     registers.
 *   - PC16550D datasheet, Table 5 ("Interrupt Control Functions"): the four
 *     interrupt sources, their priority, and the action that resets each.
 *   - PC16550D datasheet, Section "Programmable Baud Generator": the divisor is
 *     the reference oscillator frequency divided by sixteen times the desired
 *     signalling rate.
 *   - IBM Personal Computer AT technical reference: the first serial adapter is
 *     decoded at I/O base address 0x03F8 and raises IRQ4; its 1.8432 MHz
 *     reference oscillator yields 115200 baud at a divisor of one.
 */

#ifndef OXYS_SERIAL_H
#define OXYS_SERIAL_H

#include <oxys/types.h>

/* The conventional I/O base addresses of the first two serial adapters. */
#define SERIAL_COM1_PORT UINT16_C(0x03F8)
#define SERIAL_COM2_PORT UINT16_C(0x02F8)

/*
 * The request lines of those adapters upon the PC/AT. The first and third
 * adapters share IRQ4 and the second and fourth IRQ3, which is why a machine
 * carrying four adapters cannot use them all at once without sharing.
 */
#define SERIAL_COM1_IRQ UINT8_C(4)
#define SERIAL_COM2_IRQ UINT8_C(3)

/*
 * The signalling rate obtained at a divisor of one, being the reference
 * oscillator of 1843200 Hz divided by the sixteen clocks the receiver takes for
 * each bit. Every other rate is this quantity divided by the divisor, so it is
 * also the greatest rate the adapter can produce.
 */
#define SERIAL_MAXIMUM_BAUD_RATE UINT32_C(115200)

/* The parity schemes the line control register can express. */
typedef enum SerialParity
{
    SERIAL_PARITY_NONE  = 0,
    SERIAL_PARITY_ODD   = 1,
    SERIAL_PARITY_EVEN  = 2,
    SERIAL_PARITY_MARK  = 3,
    SERIAL_PARITY_SPACE = 4
} SerialParity;

/*
 * The number of stop bits. The adapter transmits one and a half rather than two
 * when the word length is five bits, which is a property of the hardware and not
 * a separate selection.
 */
typedef enum SerialStopBits
{
    SERIAL_STOP_BITS_ONE = 0,
    SERIAL_STOP_BITS_TWO = 1
} SerialStopBits;

/*
 * A complete description of the line parameters. The signalling rate is stated
 * in baud rather than as a divisor, the divisor being an artefact of the
 * adapter's oscillator that no caller should have to know.
 */
typedef struct SerialConfiguration
{
    uint32_t baud_rate;
    uint8_t data_bits; /* Five to eight inclusive. */
    SerialParity parity;
    SerialStopBits stop_bits;
} SerialConfiguration;

/*
 * Configures the specified adapter with the default line parameters of 115200
 * baud, eight data bits, no parity and one stop bit, and determines whether an
 * adapter is present by a loopback test.
 *
 * The driver begins in the polled mode, because it must serve the diagnostics of
 * the earliest initialisation, at which point no interrupt controller exists.
 *
 * Returns false if no adapter responded, in which case every subsequent write is
 * discarded harmlessly and the machine proceeds without a serial channel.
 */
bool SerialInitialise(uint16_t port);

/*
 * Applies a new set of line parameters to the adapter in use. Any output already
 * queued is transmitted first, since it was composed for the parameters in force
 * when it was written and would otherwise be received as noise.
 *
 * Returns false, leaving the parameters unaltered, if no adapter is in use or if
 * the configuration cannot be expressed: a signalling rate that does not divide
 * the oscillator to a divisor within one to 65535, or a word length outside five
 * to eight bits.
 */
bool SerialConfigure(const SerialConfiguration *configuration);

/*
 * Promotes the driver from the polled mode to the interrupt-driven one, claiming
 * the adapter's request line and unmasking it. It must be called after
 * PicInitialise, and has no effect if no adapter is in use.
 */
void SerialActivateInterrupts(void);

/* True if an adapter answered at initialisation and is therefore in use. */
bool SerialIsPresent(void);

/* True once SerialActivateInterrupts has claimed the request line. */
bool SerialInterruptsActive(void);

/*
 * True while the adapter is asked to interrupt upon its transmitter falling
 * empty. That request is made only while characters are waiting: the condition
 * is a level and not an event, so an adapter with nothing to send would present
 * an interrupt that no service could dismiss. The accessor exists so that the
 * self-test may assert the withdrawal, an interrupt storm being a failure that
 * stops the machine without ever reporting anything.
 */
bool SerialTransmitInterruptEnabled(void);

/*
 * Transmits a known sequence with the adapter in local loopback and confirms
 * that it returns unaltered and in order, restoring the modem control register
 * and clearing both first-in-first-out buffers afterwards. Any output already
 * queued is transmitted first, so nothing composed for the line is diverted into
 * the loopback.
 *
 * The test is conducted by polling, and the adapter's interrupts are silenced
 * for its duration. Local loopback disconnects OUT2 from its pin, and upon the
 * PC/AT that is the signal gating the adapter's interrupt onto IRQ4; a test that
 * waited for an interrupt might therefore wait for one that could not arrive.
 *
 * Returns false if no adapter is in use, if a character did not return, or if
 * the sequence returned altered or out of order.
 */
bool SerialLoopbackTest(void);

/*
 * Transmits a single character. The line feed is preceded by a carriage return,
 * so that the output is legible upon a terminal that performs no such
 * translation of its own.
 *
 * Once interrupts are active the character is placed in the transmit buffer and
 * the caller returns without waiting for the line. A caller that fills the
 * buffer waits for room rather than losing the character, a diagnostic channel
 * that discards its output being worse than a slow one.
 */
void SerialPutCharacter(char character);

/*
 * Transmits a null-terminated string by repeated application of
 * SerialPutCharacter.
 */
void SerialWriteString(const char *string);

/*
 * Returns once every queued character has been presented to the line. It is used
 * before the machine stops or changes the line parameters, and by the output
 * path itself whenever it must fall back upon polling.
 */
void SerialFlush(void);

/* True if a received character is waiting to be read. */
bool SerialHasInput(void);

/*
 * Removes the oldest received character from the receive buffer. Returns false,
 * leaving the argument untouched, if no character is waiting.
 */
bool SerialReadCharacter(char *character);

/* Discards every character in both buffers, and any character in the adapter. */
void SerialFlushBuffers(void);

/* The line parameters presently in force. */
const SerialConfiguration *SerialCurrentConfiguration(void);

/*
 * The signalling rate the adapter actually produces, which is the oscillator
 * divided by the divisor selected and is therefore seldom exactly the rate
 * requested. It is reported so that a discrepancy is visible rather than latent.
 */
uint32_t SerialRealisedBaudRate(void);

/* The divisor presently loaded into the latches. */
uint16_t SerialDivisor(void);

/* Accounting, read by the boot-time self-test and by SerialReport. */
uint64_t SerialCharactersTransmitted(void);
uint64_t SerialCharactersReceived(void);
uint64_t SerialInterruptCount(void);
uint64_t SerialLineErrorCount(void);
uint64_t SerialReceiveOverrunCount(void);
uint64_t SerialTransmitWaitCount(void);

/* Writes a description of the adapter and its accounting to the console. */
void SerialReport(void);

#endif /* OXYS_SERIAL_H */
