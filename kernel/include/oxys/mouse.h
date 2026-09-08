/*
 * File: kernel/include/oxys/mouse.h
 * Purpose: Declares the interface of the PS/2 mouse driver: the initialisation
 *          of the device upon the second port of the 8042 controller, the
 *          decoding of its movement packets, the pointer position it maintains
 *          from them, and the circular buffer through which movements and button
 *          transitions reach the rest of the kernel.
 * Key definitions: MouseEvent, MOUSE_IRQ, MOUSE_BUTTON_LEFT,
 *          MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE, MOUSE_BUFFER_CAPACITY,
 *          MouseInitialise, MouseIsPresent, MouseHasWheel, MouseProcessByte,
 *          MouseSetBounds, MouseSetPosition, MouseReadEvent, MouseHasEvent,
 *          MouseFlush, MouseX, MouseY, MouseButtons, MousePacketLengthInUse, MousePacketCount,
 *          MouseByteCount, MouseFramingErrorCount, MouseMovementOverflowCount,
 *          MouseOverflowCount, MouseReport.
 * References:
 *   - IBM Personal System/2 auxiliary device: the mouse is attached to the
 *     second port of the 8042 controller, which signals upon the interrupt
 *     controller's IR12 line — a line of the slave controller, so its cascade
 *     upon the master's IR2 must be unmasked with it.
 *   - The PS/2 auxiliary device command set: 0xFF resets the device, which
 *     acknowledges with 0xFA, reports 0xAA if its self-test passed, and then
 *     sends its device identifier; 0xF6 restores the default parameters; 0xF5
 *     and 0xF4 disable and enable data reporting; 0xF3 sets the sample rate,
 *     taking the rate as a second byte; 0xF2 reads the device identifier; 0xE8
 *     sets the resolution, taking it as a second byte; 0xE6 sets scaling to one
 *     to one.
 *   - The same, the standard three-byte movement packet: the first byte carries
 *     the left, right and middle buttons in bits 0, 1 and 2, a bit always set in
 *     bit 3, the signs of the two movements in bits 4 and 5, and their overflow
 *     indications in bits 6 and 7; the second and third bytes carry the
 *     magnitudes of the horizontal and vertical movements, each forming a
 *     nine-bit two's complement quantity with its sign bit in the first byte.
 *     Vertical movement is positive upward.
 *   - The same, the wheel extension: a device that is given the sample rates
 *     200, 100 and 80 in succession and then answers 0x03 to the identifier
 *     command sends four-byte packets, the fourth byte carrying the wheel
 *     movement in its low four bits as a two's complement quantity.
 *   - docs/devices/MOUSE.md: the design, and every assertion made upon it.
 *
 * Why this driver keeps the pointer position.
 *
 * A mouse reports movement and not position; the position is the running sum of
 * the movements, and something must keep it. It is kept here, next to the
 * decoder, for the same reason the keyboard driver keeps the modifier state
 * rather than handing every consumer the make and break codes: a sum is only
 * correct if exactly one thing performs it. Were the position kept by a consumer
 * instead, a consumer that missed an event — because the buffer overflowed, or
 * because it was not reading at that moment — would not lose one movement but
 * would be permanently displaced by it, and would remain so for the rest of the
 * machine's life with nothing to correct it against.
 *
 * The bounds within which the position is confined are not this driver's to
 * know, a mouse having no idea what it is pointing at. They are told to it by
 * whoever knows the display, through MouseSetBounds. Until they are, they are
 * one pixel by one pixel and the position is therefore the origin: a driver that
 * defaulted to some plausible display size would be reporting a position it had
 * invented, and a caller could not tell that from one it had measured.
 *
 * What this driver does not do is draw. The pointer's picture, the pixels it
 * stands upon and the restoring of them is <oxys/cursor.h>, which is graphics
 * and not a device.
 */

#ifndef OXYS_MOUSE_H
#define OXYS_MOUSE_H

#include <oxys/types.h>

/* The request line upon which the second port signals. */
#define MOUSE_IRQ UINT8_C(12)

/* The buttons, as they appear in the first byte of a packet and in an event. */
#define MOUSE_BUTTON_LEFT   UINT8_C(0x01)
#define MOUSE_BUTTON_RIGHT  UINT8_C(0x02)
#define MOUSE_BUTTON_MIDDLE UINT8_C(0x04)

/*
 * The capacity of the circular buffer, in events. A power of two, so that an
 * index may be reduced to a subscript by a mask rather than a division, the
 * reduction being performed within an interrupt handler.
 *
 * It is smaller than the keyboard's for a reason rather than by inattention. A
 * lost keystroke is a character the operator typed and meant; a lost movement is
 * a fraction of a hand's motion, and the position it would have contributed to
 * is carried in the next event regardless, because the position is a sum kept
 * here and not by the reader.
 */
#define MOUSE_BUFFER_CAPACITY 64U

/*
 * One decoded packet.
 *
 * Both the movement and the position it produced are carried. The position is
 * what a consumer drawing a pointer wants and is the authoritative value; the
 * movement is what a consumer that scrolls or drags wants, and it cannot be
 * recovered from two positions once the position has met a bound and stopped.
 *
 * `buttons` is the state after this packet and `changed` names the buttons whose
 * state this packet altered, so that a depression and a release are both
 * recoverable from one event without the reader keeping the previous state.
 */
typedef struct MouseEvent
{
    /* The pointer position after this movement, within the bounds in force. */
    int32_t x;
    int32_t y;

    /*
     * The movement itself, in the screen's sense: x increases to the right and y
     * increases downward. The device's vertical sense is the opposite of this
     * and is inverted by the decoder, at the one place that knows the device.
     */
    int16_t delta_x;
    int16_t delta_y;

    /* The wheel movement, positive away from the operator; always zero upon a
     * device without a wheel. */
    int8_t delta_wheel;

    /* The buttons held after this packet, and those this packet altered. */
    uint8_t buttons;
    uint8_t changed;
} MouseEvent;

/*
 * Initialises the mouse upon the second port of the controller, claims its
 * request line and unmasks it.
 *
 * Returns false where the controller has no usable second port or no device
 * answers upon it, in which case nothing is claimed and no line is unmasked. A
 * missing mouse is not a fault.
 *
 * Ps2Initialise must have run, and IrqInitialise before that. This driver does
 * not configure the controller: it is shared with the keyboard, and a driver
 * that reset it would silence a keyboard already reporting.
 */
bool MouseInitialise(void);

/* Reports whether initialisation found a working mouse. */
bool MouseIsPresent(void);

/* Whether the device answered the wheel interrogation and therefore sends
 * four-byte packets. */
bool MouseHasWheel(void);

/*
 * Decodes one byte of a packet, appending an event to the buffer where the byte
 * completes one.
 *
 * This is the entry point the interrupt handler calls, and it is exposed for the
 * same reason KeyboardProcessScancode is: the decoding of a movement packet is
 * not a property of the 8042, and a byte arriving by any other route decodes
 * identically. The boot-time self-test drives it directly, which is what permits
 * the decoder, the sign extension, the framing and the clamping to be asserted
 * upon a machine with no mouse and nobody moving it.
 */
void MouseProcessByte(uint8_t byte);

/*
 * Confines the pointer to a rectangle of the given size, whose origin is the top
 * left, and clamps the present position into it.
 *
 * A width or height below one is refused, an empty rectangle leaving no position
 * the pointer could legally occupy.
 */
void MouseSetBounds(int32_t width, int32_t height);

/* Places the pointer, clamped to the bounds in force. Produces no event: this is
 * the kernel moving the pointer, not the operator, and a consumer that treated
 * it as a movement would report a hand motion nobody made. */
void MouseSetPosition(int32_t x, int32_t y);

/* Removes the oldest event from the buffer. Returns false, leaving the argument
 * untouched, if the buffer is empty. */
bool MouseReadEvent(MouseEvent *event);

/* Reports whether the buffer holds at least one event. */
bool MouseHasEvent(void);

/* Discards every buffered event, and abandons any partly received packet. The
 * position and the button state are retained: they are where the pointer is and
 * what is held, neither of which a flush changes. */
void MouseFlush(void);

/* The pointer position, and the buttons presently held. */
int32_t MouseX(void);
int32_t MouseY(void);
uint8_t MouseButtons(void);

/*
 * The length of the packet the device is presently sending, in bytes: three, or
 * four where the interrogation found a wheel.
 *
 * The self-test needs it, and needs it rather than assuming it: a test that
 * composed three-byte packets against a device negotiated to four would feed the
 * decoder two thirds of a packet at a time and would assert upon whatever fell
 * out. It is also what the report prints, which is how a reader can tell a
 * device with no wheel from an interrogation that failed.
 */
uint8_t MousePacketLengthInUse(void);

/* The number of bytes received and the number of packets they completed. */
uint64_t MouseByteCount(void);
uint64_t MousePacketCount(void);

/*
 * The number of bytes discarded because they could not be the first byte of a
 * packet, and the number of packets whose movement the device declared to have
 * overflowed.
 *
 * Both are counted rather than merely handled, because both are silent: a driver
 * that has lost the framing decodes button states and movements from bytes taken
 * two at a time from adjacent packets, and every one of them is a plausible
 * value.
 */
uint64_t MouseFramingErrorCount(void);
uint64_t MouseMovementOverflowCount(void);

/* The number of events discarded because the buffer was full. */
uint64_t MouseOverflowCount(void);

/* Emits a summary of the mouse's state upon both output devices. */
void MouseReport(void);

#endif /* OXYS_MOUSE_H */
