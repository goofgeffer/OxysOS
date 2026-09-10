/*
 * File: kernel/include/oxys/ps2.h
 * Purpose: Declares the interface of the 8042 keyboard controller itself, as
 *          distinct from the devices attached to it: the initialisation and
 *          self-test of the controller, the discovery and testing of its two
 *          device ports, the configuration byte they share, and the bounded
 *          exchange of bytes with a device upon either port.
 * Key definitions: PS2_PORT_FIRST, PS2_PORT_SECOND, PS2_ACKNOWLEDGE,
 *          PS2_RESEND, PS2_SELF_TEST_PASSED, Ps2Initialise, Ps2IsPresent,
 *          Ps2PortIsUsable, Ps2EnablePort, Ps2SetPortInterrupt, Ps2ReadData,
 *          Ps2ReadDataFrom, Ps2ReadPending, Ps2SendDeviceCommand, Ps2SendDeviceByte,
 *          Ps2DrainOutputBuffer, Ps2ByteIsFromSecondPort, Ps2Configuration,
 *          Ps2Report.
 * References:
 *   - IBM Personal Computer AT technical reference, the 8042 keyboard
 *     controller: the data port at 0x60, and the status register read at 0x64
 *     with the command register written at the same address. Status bit 0 is set
 *     while the output buffer holds a byte for the processor and bit 1 while the
 *     input buffer still holds one for the controller, so a byte may be read
 *     only when bit 0 is set and written only when bit 1 is clear.
 *   - The same, the auxiliary device: status bit 5 is set when the byte in the
 *     output buffer came from the second port rather than the first. The first
 *     port is attached to the interrupt controller's IR1 line and the second to
 *     IR12.
 *   - The 8042 controller command set: 0x20 reads the configuration byte and
 *     0x60 writes it; 0xAD and 0xAE disable and enable the first port, 0xA7 and
 *     0xA8 the second; 0xAA is the controller self-test and answers 0x55 upon
 *     success; 0xAB tests the first port and 0xA9 the second, each answering
 *     0x00 upon success; 0xD4 directs the byte written next to the second port
 *     rather than the first.
 *   - The same, the configuration byte: bit 0 enables the interrupt of the first
 *     port, bit 1 that of the second, bit 4 disables the first port's clock when
 *     set, bit 5 disables the second port's clock when set, and bit 6 enables
 *     the translation of scan code set 2 into set 1.
 *   - The PS/2 device command set: a device answers 0xFA to acknowledge a
 *     command and 0xFE to ask that it be sent again; 0xFF resets a device, which
 *     acknowledges and then reports 0xAA if its own self-test passed.
 *   - docs/devices/KEYBOARD.md, Section 2, and docs/devices/MOUSE.md, Section 2:
 *     the controller as the two drivers see it.
 *
 * Why the controller is a module and not part of the keyboard driver.
 *
 * Until sub-task 6.5 it was part of the keyboard driver, and that was correct
 * while there was one device: the 8042 and the keyboard are reached through the
 * same pair of ports, and separating them would have been a distinction without
 * a reader.
 *
 * A second device makes the distinction load-bearing, because the configuration
 * byte is not the keyboard's and is not the mouse's. It is one byte governing
 * both ports, and it is read, modified and written whole. Two drivers each
 * keeping their own idea of it would each write back the other's bits as they
 * last saw them: the mouse driver, enabling its own interrupt, would restore the
 * translation bit to whatever it had been when the mouse driver first looked,
 * and the keyboard would afterwards deliver scan code set 2 while decoding it as
 * set 1 — which is not a failure to work but a failure to be right, the two sets
 * overlapping without agreeing.
 *
 * The keyboard driver's initialisation also disables the second port
 * deliberately, so that nothing arrives from it while the controller is being
 * configured. A mouse driver that did not know this would find its port shut and
 * would have no way to learn why. One owner, holding the byte and both ports,
 * removes both faults rather than documenting them.
 *
 * Concurrency. Every routine here is called during initialisation, before the
 * interrupt flag is set, save Ps2ByteIsFromSecondPort and the reading of the
 * data port, which the two interrupt handlers call. The read-modify-write of the
 * configuration byte requires the spinlock sub-task 6.13 built, which is not yet
 * taken; a handler's read of the data port needs none, the controller
 * holding one byte and the two handlers being woken by different lines.
 */

#ifndef OXYS_PS2_H
#define OXYS_PS2_H

#include <oxys/types.h>

/*
 * The two device ports, named rather than numbered at each site.
 *
 * These are indices into this module's own tables and are not written to any
 * register. The controller distinguishes the ports by which command is used,
 * not by a port number, which is why a caller may not invent a third.
 */
#define PS2_PORT_FIRST  0U
#define PS2_PORT_SECOND 1U
#define PS2_PORT_COUNT  2U

/* The answers a device gives. A command is acknowledged by 0xFA; 0xFE is a
 * request that it be sent again; 0xAA is reported by a device whose own
 * self-test, following a reset, passed. */
#define PS2_ACKNOWLEDGE      UINT8_C(0xFA)
#define PS2_RESEND           UINT8_C(0xFE)
#define PS2_SELF_TEST_PASSED UINT8_C(0xAA)

/*
 * Initialises the controller: disables both ports, discards whatever the
 * firmware left in the output buffer, runs the controller's self-test, tests
 * each port, and determines whether the second port exists at all.
 *
 * Returns false where no working controller was found, in which case no port is
 * usable and nothing further may be attempted. A machine with no PS/2
 * controller decodes these ports as a constant, so every wait here is bounded
 * and such a machine proceeds unimpeded rather than blocking upon a status flag
 * that will never change.
 *
 * No port is enabled and no interrupt is permitted: those are the business of
 * whichever driver claims the port, and are done only once that driver has
 * established that its device answers.
 */
bool Ps2Initialise(void);

/* Reports whether initialisation found a working controller. */
bool Ps2IsPresent(void);

/*
 * Reports whether the given port exists and passed its test.
 *
 * The second port's existence is discovered rather than assumed. A controller
 * with one port ignores the command that would enable a second, and the clock
 * bit that command should have cleared is therefore found still set; that is
 * the test, and it is the only one available, no register reporting the number
 * of ports directly.
 */
bool Ps2PortIsUsable(uint8_t port);

/* Enables the port, permitting its device to send. Returns false if the port is
 * unusable or the controller did not accept the command. */
bool Ps2EnablePort(uint8_t port);

/*
 * Permits or withholds the port's interrupt, by the corresponding bit of the
 * configuration byte, leaving every other bit as it stands.
 *
 * A driver calls this last, once its device has answered and its handler is
 * registered, so that a byte arriving between the two cannot be recorded as an
 * unclaimed request and lost.
 */
bool Ps2SetPortInterrupt(uint8_t port, bool enabled);

/* Reads a byte from the data port, having waited for one to appear. Returns
 * false if the bound was reached, which is how a device's silence is
 * discovered. */
bool Ps2ReadData(uint8_t *value);

/*
 * Reads a byte, and reports which port it came from.
 *
 * This exists because a byte read from the data port is not necessarily the
 * byte the reader is waiting for. Both devices deliver through the same
 * one-byte buffer, and a mouse whose data reporting is enabled will interleave
 * its packets with the keyboard's scancodes. During initialisation the ports are
 * enabled one at a time so the question does not arise; from an interrupt
 * handler it does, and the handler that finds a byte belonging to the other
 * device must leave it alone rather than consume it.
 */
bool Ps2ReadDataFrom(uint8_t *value, uint8_t *port);

/*
 * Takes the byte the controller is presently holding, if it is holding one, and
 * reports which port it came from. Returns false at once where the output buffer
 * is empty.
 *
 * This is the routine an interrupt handler calls, and the difference from
 * Ps2ReadDataFrom above is the whole of its purpose: that one waits, bounded, for
 * a byte to appear, which is right during initialisation and wrong within a
 * handler. A handler is entered because a byte arrived, so a handler that found
 * none must return immediately; waiting a hundred thousand iterations for a byte
 * that another handler has already taken would hold the processor for no reason,
 * with the interrupt flag clear.
 */
bool Ps2ReadPending(uint8_t *value, uint8_t *port);

/*
 * Whether the byte presently in the output buffer came from the second port.
 *
 * This is read before the data port, not after: reading the data port clears the
 * status bit along with the buffer, so a handler that read first would have
 * nothing left to ask.
 */
bool Ps2ByteIsFromSecondPort(void);

/*
 * Sends a command to the device upon the given port and awaits its
 * acknowledgement, retrying while the device asks for the command to be sent
 * again.
 *
 * The retry is bounded. A device that asked without end would otherwise hold the
 * processor for ever, and a device that cannot be commanded is better reported
 * as absent than allowed to stop the machine.
 */
bool Ps2SendDeviceCommand(uint8_t port, uint8_t command);

/*
 * Sends a byte that is the argument of a command already acknowledged — a sample
 * rate, a resolution — and awaits the acknowledgement of the argument in turn.
 *
 * It is separate from the command above only for the reader's sake: a device
 * acknowledges the two identically, and naming the argument as an argument keeps
 * a caller from having to remember that the second 0xF3 in a sequence is a
 * number and not a command.
 */
bool Ps2SendDeviceByte(uint8_t port, uint8_t value);

/*
 * Discards every byte the controller is presently holding.
 *
 * The firmware has been using these devices and may have left a keystroke, or
 * the tail of a command exchange, in the output buffer. Such a byte would be
 * decoded as a scancode or as the first byte of a movement packet, and would
 * appear as an event nobody caused.
 */
void Ps2DrainOutputBuffer(void);

/* The configuration byte as this module last wrote it. Exposed for the report
 * and for the self-test, which asserts that the bits the two drivers depend upon
 * are the ones actually in force. */
uint8_t Ps2Configuration(void);

/* Emits a summary of the controller's state upon both output devices. */
void Ps2Report(void);

#endif /* OXYS_PS2_H */
