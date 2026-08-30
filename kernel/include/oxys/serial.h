/*
 * File: kernel/include/oxys/serial.h
 * Purpose: Declares the interface of the early polled COM1 serial output driver,
 *          which provides diagnostic output independent of the display hardware.
 * Key definitions: SERIAL_COM1_PORT, SerialInitialise, SerialPutCharacter,
 *          SerialWriteString.
 * References:
 *   - National Semiconductor PC16550D Universal Asynchronous Receiver/Transmitter
 *     datasheet: register map at offsets 0 to 7 from the base address, the
 *     divisor latch access bit (bit 7 of the line control register), and the
 *     transmitter holding register empty flag (bit 5 of the line status
 *     register).
 *   - IBM Personal Computer AT technical reference: the first serial adapter is
 *     conventionally decoded at I/O base address 0x03F8.
 *
 * Note: this is the minimal polled subset required for Phase 1 diagnostics. The
 * formal interrupt-driven driver is scheduled for Phase 4, sub-task 4.1.
 */

#ifndef OXYS_SERIAL_H
#define OXYS_SERIAL_H

#include <oxys/types.h>

/* The conventional I/O base address of the first serial adapter. */
#define SERIAL_COM1_PORT UINT16_C(0x03F8)

/*
 * Configures the specified serial adapter for 115200 baud, eight data bits, no
 * parity and one stop bit, with interrupts disabled and the first-in-first-out
 * buffers enabled.
 *
 * Returns true if the adapter responded correctly to a loopback test, and false
 * otherwise. A false return indicates that no adapter is present, in which case
 * subsequent writes are discarded harmlessly.
 */
bool SerialInitialise(uint16_t port);

/*
 * Transmits a single character, waiting until the transmitter holding register
 * is empty. The line feed character is preceded by a carriage return so that the
 * output is legible upon a terminal that does not perform that translation.
 */
void SerialPutCharacter(char character);

/*
 * Transmits a null-terminated string by repeated application of
 * SerialPutCharacter.
 */
void SerialWriteString(const char *string);

#endif /* OXYS_SERIAL_H */
