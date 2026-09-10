/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/mouse/mouse.c
 * Purpose: Implements the PS/2 mouse driver: the initialisation of the device
 *          upon the second port of the 8042 controller, the interrogation that
 *          establishes whether it has a wheel, the decoding and framing of its
 *          movement packets, the pointer position accumulated from them, and the
 *          circular buffer by which events are delivered from the interrupt
 *          handler to the rest of the kernel.
 * Key functions: MouseInitialise, MouseIsPresent, MouseHasWheel,
 *          MouseProcessByte, MouseSetBounds, MouseSetPosition, MouseReadEvent,
 *          MouseHasEvent, MouseFlush, MouseX, MouseY, MouseButtons,
 *          MousePacketLengthInUse, MouseReport.
 * References:
 *   - IBM Personal System/2 auxiliary device: the mouse upon the second port of
 *     the 8042, signalling upon the interrupt controller's IR12 line.
 *   - The PS/2 auxiliary device command set: 0xFF reset, 0xF6 set defaults, 0xF5
 *     and 0xF4 disable and enable data reporting, 0xF3 set sample rate followed
 *     by the rate, 0xF2 read device identifier, 0xE8 set resolution followed by
 *     the resolution, 0xE6 set scaling one to one.
 *   - The same, the movement packet: byte 0 bits 0 to 2 the left, right and
 *     middle buttons, bit 3 always set, bits 4 and 5 the signs of the horizontal
 *     and vertical movements, bits 6 and 7 their overflow indications; byte 1
 *     the horizontal magnitude and byte 2 the vertical, each completing a
 *     nine-bit two's complement quantity with its sign bit from byte 0. Vertical
 *     movement is positive upward.
 *   - The same, the wheel extension: the sample rates 200, 100 and 80 given in
 *     succession cause a device that has a wheel to answer 0x03 to the
 *     identifier command and to send four-byte packets thereafter, the fourth
 *     byte carrying the wheel movement in its low four bits.
 *   - docs/devices/MOUSE.md, Sections 3 to 6: the packet, the framing, the
 *     position, and what is asserted about each.
 *
 * Why the always-set bit is a framing test and not a curiosity.
 *
 *   The device sends a stream of bytes with nothing in it to say where a packet
 *   begins. A driver that has lost its place — because a byte was dropped, or
 *   because the firmware left one in the controller's buffer — does not stop
 *   working. It reads the second byte of one packet as the first byte of the
 *   next, and thereafter reports button states taken from a movement magnitude
 *   and movements taken from a button byte, indefinitely and with no error
 *   anywhere. Every value it produces is a value the device could have sent.
 *
 *   Bit 3 of the first byte is set in every packet the device sends. Refusing
 *   any byte that lacks it while a packet is being awaited is therefore a test
 *   that a mis-framed stream fails and a correct one cannot, and it recovers by
 *   itself: bytes are discarded, one at a time, until one arrives that could
 *   begin a packet. The discards are counted, so that a fault which the driver
 *   corrects is still visible to whoever reads the report.
 *
 * Concurrency. The circular buffer has a single producer, the interrupt handler,
 * and a single consumer, as the keyboard's has; the reasoning in
 * drivers/keyboard/keyboard.c applies unchanged. The pointer position and the
 * partly received packet are written by the producer alone and read by both;
 * the spinlock of sub-task 6.13 is what the consumer's side requires, and it has
 * not been applied here.
 */

#include <oxys/mouse.h>
#include <oxys/keyboard.h>
#include <oxys/ps2.h>
#include <oxys/irq.h>
#include <oxys/interrupts.h>
#include <oxys/kernel.h>

/* The device commands this driver issues. */
#define MOUSE_COMMAND_RESET              UINT8_C(0xFF)
#define MOUSE_COMMAND_SET_DEFAULTS       UINT8_C(0xF6)
#define MOUSE_COMMAND_ENABLE_REPORTING   UINT8_C(0xF4)
#define MOUSE_COMMAND_DISABLE_REPORTING  UINT8_C(0xF5)
#define MOUSE_COMMAND_SET_SAMPLE_RATE    UINT8_C(0xF3)
#define MOUSE_COMMAND_READ_IDENTIFIER    UINT8_C(0xF2)
#define MOUSE_COMMAND_SET_RESOLUTION     UINT8_C(0xE8)
#define MOUSE_COMMAND_SET_SCALING_LINEAR UINT8_C(0xE6)

/*
 * The identifiers a device reports.
 *
 * Zero is the plain three-button device. Three is the wheel device, and is
 * reported only after the interrogation below; a wheel device that has not been
 * asked reports zero and sends three-byte packets, which is what makes the
 * interrogation necessary rather than merely informative.
 */
#define MOUSE_IDENTIFIER_STANDARD UINT8_C(0x00)
#define MOUSE_IDENTIFIER_WHEEL    UINT8_C(0x03)

/* The sample rates that, given in this order, ask a device whether it has a
 * wheel. The sequence is the interrogation; no single command asks. */
#define MOUSE_KNOCK_FIRST  UINT8_C(200)
#define MOUSE_KNOCK_SECOND UINT8_C(100)
#define MOUSE_KNOCK_THIRD  UINT8_C(80)

/*
 * The parameters established after the interrogation.
 *
 * The rate is reports per second and the resolution is an exponent: 3 selects
 * eight counts to the millimetre, the finest the command defines. They are set
 * explicitly rather than left to the defaults because the defaults are the
 * firmware's, and a pointer that moves at a different speed upon two machines
 * for no reason the kernel recorded is a fault nobody can find.
 */
#define MOUSE_SAMPLE_RATE UINT8_C(100)
#define MOUSE_RESOLUTION  UINT8_C(3)

/* The bits of the first byte of a packet. */
#define MOUSE_PACKET_ALWAYS_SET   UINT8_C(0x08)
#define MOUSE_PACKET_X_SIGN       UINT8_C(0x10)
#define MOUSE_PACKET_Y_SIGN       UINT8_C(0x20)
#define MOUSE_PACKET_X_OVERFLOW   UINT8_C(0x40)
#define MOUSE_PACKET_Y_OVERFLOW   UINT8_C(0x80)
#define MOUSE_PACKET_BUTTON_MASK  UINT8_C(0x07)

/* The two packet lengths, and the greater of them, which is the buffer's size. */
#define MOUSE_PACKET_LENGTH_STANDARD 3U
#define MOUSE_PACKET_LENGTH_WHEEL    4U
#define MOUSE_PACKET_LENGTH_MAXIMUM  4U

/* Whether a working mouse was found, and whether it has a wheel. */
static bool MousePresent;
static bool MouseWheelPresent;

/* The packet presently being received, and how far into it we are. */
static uint8_t MousePacket[MOUSE_PACKET_LENGTH_MAXIMUM];
static uint8_t MousePacketOffset;
static uint8_t MousePacketLength = MOUSE_PACKET_LENGTH_STANDARD;

/*
 * The pointer position, and the rectangle it is confined to.
 *
 * The bounds begin at one by one rather than at some plausible display size, so
 * that a position read before MouseSetBounds has been called is the origin and
 * is obviously not a measurement. See <oxys/mouse.h>.
 */
static int32_t MousePositionX;
static int32_t MousePositionY;
static int32_t MouseBoundWidth = 1;
static int32_t MouseBoundHeight = 1;

/* The buttons presently held. */
static uint8_t MouseButtonState;

/* The circular buffer, and the free-running indices into it. The indices are
 * not wrapped to the capacity; they are masked when used, so that their
 * difference is the occupancy directly. The reasoning is the keyboard's. */
static MouseEvent MouseBuffer[MOUSE_BUFFER_CAPACITY];
static volatile uint32_t MouseWriteIndex;
static volatile uint32_t MouseReadIndex;

_Static_assert((MOUSE_BUFFER_CAPACITY & (MOUSE_BUFFER_CAPACITY - 1U)) == 0U,
               "The mouse buffer capacity must be a power of two.");

/* Accounting. */
static uint64_t MouseBytesReceived;
static uint64_t MousePacketsDecoded;
static uint64_t MouseFramingErrors;
static uint64_t MouseMovementOverflows;
static uint64_t MouseEventsDiscarded;

/*
 * Appends an event to the circular buffer.
 *
 * A buffer that is full discards the new event rather than the oldest, as the
 * keyboard's does, but the consequence differs and is worth stating. Discarding
 * the newest loses the most recent movement, and the position that movement
 * would have produced is lost with it — until the next event, which carries the
 * position afresh rather than a further increment, and so restores the reader to
 * the truth. That is the property the position is kept here for.
 */
static void MouseAppendEvent(const MouseEvent *event)
{
    if ((uint32_t)(MouseWriteIndex - MouseReadIndex) >= (uint32_t)MOUSE_BUFFER_CAPACITY)
    {
        ++MouseEventsDiscarded;
        return;
    }

    MouseBuffer[MouseWriteIndex & (MOUSE_BUFFER_CAPACITY - 1U)] = *event;

    /* The index advances only after the event is written, so that a consumer
     * observing the advance is guaranteed a complete event beneath it. */
    ++MouseWriteIndex;
}

/*
 * Extends the nine-bit two's complement movement of one axis.
 *
 * The magnitude is a whole byte and the sign is a bit of another, so the
 * quantity is nine bits and not eight: a movement of -1 is sent as a magnitude
 * of 255 with the sign bit set, and -256 as a magnitude of 0 with the sign bit
 * set. Subtracting 256 where the sign bit stands is therefore the extension, and
 * treating the byte as a signed char instead — the obvious mistake — turns that
 * -1 into -1 by accident and that -256 into 0, which is a movement quietly lost.
 */
static int32_t MouseExtendMovement(uint8_t magnitude, bool negative)
{
    return (int32_t)magnitude - (negative ? 256 : 0);
}

/* Confines a value to the closed interval, which is what a bound of `extent`
 * pixels means: the last legal coordinate is one less than the extent. */
static int32_t MouseClamp(int32_t value, int32_t extent)
{
    if (value < 0)
    {
        return 0;
    }

    if (value > (extent - 1))
    {
        return extent - 1;
    }

    return value;
}

/*
 * Extracts the wheel movement from the fourth byte.
 *
 * Only the low four bits carry it, and they are two's complement, so the range
 * is -8 to 7. The upper four bits are the fourth and fifth buttons upon a device
 * that has them, and are deliberately not decoded here: this driver interrogates
 * for the wheel alone, so a device reporting five buttons has not been put into
 * the mode where those bits mean that, and reading them would be reading bits
 * whose meaning was never established.
 */
static int8_t MouseExtractWheel(uint8_t byte)
{
    const uint8_t nibble = (uint8_t)(byte & UINT8_C(0x0F));

    return (int8_t)((nibble > UINT8_C(0x07)) ? ((int32_t)nibble - 16) : (int32_t)nibble);
}

/* Completes a packet: decodes it, advances the position and produces an event. */
static void MouseCompletePacket(void)
{
    MouseEvent event;
    const uint8_t flags = MousePacket[0];
    const uint8_t buttons = (uint8_t)(flags & MOUSE_PACKET_BUTTON_MASK);
    int32_t movement_x;
    int32_t movement_y;

    ++MousePacketsDecoded;

    /*
     * An overflow is the device saying that the movement was larger than nine
     * bits could express, which means the magnitude it sent is not the movement
     * but the low bits of it. The movement is therefore discarded rather than
     * used: a wrong distance in the right direction would jump the pointer
     * somewhere arbitrary, and standing still is the better failure. The buttons
     * in the same byte are unaffected and are kept.
     */
    if ((flags & (MOUSE_PACKET_X_OVERFLOW | MOUSE_PACKET_Y_OVERFLOW)) != 0U)
    {
        ++MouseMovementOverflows;
        movement_x = 0;
        movement_y = 0;
    }
    else
    {
        movement_x = MouseExtendMovement(MousePacket[1], (flags & MOUSE_PACKET_X_SIGN) != 0U);

        /*
         * The device measures vertical movement positive upward and the display
         * measures it positive downward. The negation is here, in the one place
         * that knows what the device meant; every consumer above receives the
         * screen's sense and none of them needs to know that a mouse disagrees.
         */
        movement_y = -MouseExtendMovement(MousePacket[2], (flags & MOUSE_PACKET_Y_SIGN) != 0U);
    }

    MousePositionX = MouseClamp(MousePositionX + movement_x, MouseBoundWidth);
    MousePositionY = MouseClamp(MousePositionY + movement_y, MouseBoundHeight);

    event.x = MousePositionX;
    event.y = MousePositionY;
    event.delta_x = (int16_t)movement_x;
    event.delta_y = (int16_t)movement_y;
    event.delta_wheel = (MousePacketLength == MOUSE_PACKET_LENGTH_WHEEL)
                            ? MouseExtractWheel(MousePacket[3])
                            : (int8_t)0;
    event.buttons = buttons;
    event.changed = (uint8_t)(buttons ^ MouseButtonState);

    MouseButtonState = buttons;

    MouseAppendEvent(&event);
}

void MouseProcessByte(uint8_t byte)
{
    ++MouseBytesReceived;

    /*
     * The framing test, applied to the first byte of every packet and to no
     * other. See the file header: a byte that cannot begin a packet is discarded
     * singly, which is what allows a stream that has lost its place to recover
     * without the driver being reset.
     */
    if ((MousePacketOffset == 0U) && ((byte & MOUSE_PACKET_ALWAYS_SET) == 0U))
    {
        ++MouseFramingErrors;
        return;
    }

    MousePacket[MousePacketOffset] = byte;
    ++MousePacketOffset;

    if (MousePacketOffset < MousePacketLength)
    {
        return;
    }

    MousePacketOffset = 0U;

    MouseCompletePacket();
}

/*
 * Receives the mouse's request line.
 *
 * Exactly one byte is read, for the reason the keyboard's handler gives: the
 * controller raises the request once per byte, so a handler that drained the
 * buffer would consume bytes whose requests were still to be delivered.
 *
 * A byte that came from the first port is the keyboard's and is handed to it.
 * The read cannot be undone — the output buffer holds one byte and reading it is
 * what empties it — so handing it across is the only alternative to losing it.
 */
static void MouseHandleInterrupt(TrapFrame *frame)
{
    uint8_t byte;
    uint8_t port;

    (void)frame;

    if (!Ps2ReadPending(&byte, &port))
    {
        return;
    }

    if (port == PS2_PORT_FIRST)
    {
        KeyboardProcessScancode(byte);
        return;
    }

    MouseProcessByte(byte);
}

/*
 * Asks the device whether it has a wheel, and reports what it answered.
 *
 * The interrogation is a sequence of sample-rate commands and not a question,
 * for the reason given beside the constants: no command asks. A device that does
 * not recognise the sequence simply accepts three sample rates and continues to
 * report itself as the standard device, which is why a failure here is not a
 * failure at all — it leaves a working three-byte mouse.
 */
static bool MouseInterrogateForWheel(void)
{
    uint8_t identifier;

    static const uint8_t knock[] = { MOUSE_KNOCK_FIRST, MOUSE_KNOCK_SECOND, MOUSE_KNOCK_THIRD };

    for (size_t index = 0U; index < (sizeof(knock) / sizeof(knock[0])); ++index)
    {
        if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_SET_SAMPLE_RATE) ||
            !Ps2SendDeviceByte(PS2_PORT_SECOND, knock[index]))
        {
            return false;
        }
    }

    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_READ_IDENTIFIER))
    {
        return false;
    }

    if (!Ps2ReadData(&identifier))
    {
        return false;
    }

    return identifier == MOUSE_IDENTIFIER_WHEEL;
}

bool MouseInitialise(void)
{
    uint8_t answer;

    MousePresent = false;
    MouseWheelPresent = false;
    MousePacketOffset = 0U;
    MousePacketLength = MOUSE_PACKET_LENGTH_STANDARD;
    MouseButtonState = 0U;
    MouseWriteIndex = 0U;
    MouseReadIndex = 0U;
    MouseBytesReceived = 0U;
    MousePacketsDecoded = 0U;
    MouseFramingErrors = 0U;
    MouseMovementOverflows = 0U;
    MouseEventsDiscarded = 0U;

    /*
     * The controller is not configured here. It is shared with the keyboard and
     * has one owner; this driver asks only whether the port it wants exists.
     */
    if (!Ps2PortIsUsable(PS2_PORT_SECOND))
    {
        return false;
    }

    if (!Ps2EnablePort(PS2_PORT_SECOND))
    {
        return false;
    }

    /*
     * Reset the device. It acknowledges, then reports its self-test result, then
     * its identifier. Both further bytes are read; the self-test result is
     * insisted upon, since a device that failed its own test is not one to take
     * movements from, while the identifier is discarded because the
     * interrogation below establishes it properly.
     */
    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_RESET))
    {
        return false;
    }

    if (!Ps2ReadData(&answer) || answer != PS2_SELF_TEST_PASSED)
    {
        return false;
    }

    (void)Ps2ReadData(&answer);

    /*
     * The defaults are restored before anything is chosen, so that whatever the
     * firmware left the device in is not carried forward into the parameters set
     * below. The reset above ought to have done this; a device that reset
     * incompletely is common enough that the guidelines' preference for
     * establishing rather than assuming applies.
     */
    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_SET_DEFAULTS))
    {
        return false;
    }

    MouseWheelPresent = MouseInterrogateForWheel();
    MousePacketLength =
        MouseWheelPresent ? MOUSE_PACKET_LENGTH_WHEEL : MOUSE_PACKET_LENGTH_STANDARD;

    /*
     * The interrogation left the sample rate at 80, whether or not it found a
     * wheel. The rate wanted is set afterwards for that reason, and not before.
     */
    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_SET_SAMPLE_RATE) ||
        !Ps2SendDeviceByte(PS2_PORT_SECOND, MOUSE_SAMPLE_RATE))
    {
        return false;
    }

    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_SET_RESOLUTION) ||
        !Ps2SendDeviceByte(PS2_PORT_SECOND, MOUSE_RESOLUTION))
    {
        return false;
    }

    /*
     * Linear scaling. The alternative the device offers applies a non-linear
     * acceleration of its own, which would make the movement in an event
     * something other than the distance the hand moved — and would put a policy
     * that belongs to whoever draws the pointer inside a device this kernel
     * cannot inspect.
     */
    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_SET_SCALING_LINEAR))
    {
        return false;
    }

    if (!Ps2SendDeviceCommand(PS2_PORT_SECOND, MOUSE_COMMAND_ENABLE_REPORTING))
    {
        return false;
    }

    /* Whatever the exchange above left behind is not a movement. */
    Ps2DrainOutputBuffer();

    if (!Ps2SetPortInterrupt(PS2_PORT_SECOND, true))
    {
        return false;
    }

    MousePresent = true;

    /*
     * The line is unmasked only after the handler is registered, as the
     * keyboard's is. IR12 belongs to the slave controller, whose cascade upon
     * the master's IR2 the interrupt controller driver unmasks with it.
     */
    IrqInstallHandler(MOUSE_IRQ, MouseHandleInterrupt, "PS/2 mouse");
    IrqUnmaskLine(MOUSE_IRQ);

    return true;
}

bool MouseIsPresent(void)
{
    return MousePresent;
}

bool MouseHasWheel(void)
{
    return MouseWheelPresent;
}

void MouseSetBounds(int32_t width, int32_t height)
{
    if ((width < 1) || (height < 1))
    {
        return;
    }

    MouseBoundWidth = width;
    MouseBoundHeight = height;

    /* The position is clamped immediately. A bound that has narrowed would
     * otherwise leave the pointer outside it until the next movement, and a
     * consumer drawing at that position would draw outside the display. */
    MousePositionX = MouseClamp(MousePositionX, MouseBoundWidth);
    MousePositionY = MouseClamp(MousePositionY, MouseBoundHeight);
}

void MouseSetPosition(int32_t x, int32_t y)
{
    MousePositionX = MouseClamp(x, MouseBoundWidth);
    MousePositionY = MouseClamp(y, MouseBoundHeight);
}

bool MouseHasEvent(void)
{
    return MouseWriteIndex != MouseReadIndex;
}

bool MouseReadEvent(MouseEvent *event)
{
    if (event == NULL || !MouseHasEvent())
    {
        return false;
    }

    *event = MouseBuffer[MouseReadIndex & (MOUSE_BUFFER_CAPACITY - 1U)];
    ++MouseReadIndex;

    return true;
}

void MouseFlush(void)
{
    MouseReadIndex = MouseWriteIndex;

    /*
     * The partly received packet is abandoned with the events. Retaining it
     * would mean the next byte to arrive was appended to a fragment whose
     * remaining bytes had been discarded, which is precisely the mis-framing the
     * always-set bit exists to prevent.
     */
    MousePacketOffset = 0U;
}

int32_t MouseX(void)
{
    return MousePositionX;
}

int32_t MouseY(void)
{
    return MousePositionY;
}

uint8_t MouseButtons(void)
{
    return MouseButtonState;
}

uint8_t MousePacketLengthInUse(void)
{
    return MousePacketLength;
}

uint64_t MouseByteCount(void)
{
    return MouseBytesReceived;
}

uint64_t MousePacketCount(void)
{
    return MousePacketsDecoded;
}

uint64_t MouseFramingErrorCount(void)
{
    return MouseFramingErrors;
}

uint64_t MouseMovementOverflowCount(void)
{
    return MouseMovementOverflows;
}

uint64_t MouseOverflowCount(void)
{
    return MouseEventsDiscarded;
}

void MouseReport(void)
{
    KernelWriteString("PS/2 mouse: ");

    if (!MousePresent)
    {
        KernelWriteString("absent or unusable; no request line claimed.\n");
        return;
    }

    KernelWriteString(MouseWheelPresent ? "present with a wheel, four-byte packets"
                                        : "present, three-byte packets");
    KernelWriteString(", line ");
    KernelWriteString(IrqLineIsMasked(MOUSE_IRQ) ? "masked" : "unmasked");
    KernelWriteString(".\n");

    KernelWriteString("PS/2 mouse: bytes ");
    KernelWriteDecimal(MouseBytesReceived);
    KernelWriteString(", packets ");
    KernelWriteDecimal(MousePacketsDecoded);
    KernelWriteString(", mis-framed ");
    KernelWriteDecimal(MouseFramingErrors);
    KernelWriteString(", overflowed ");
    KernelWriteDecimal(MouseMovementOverflows);
    KernelWriteString(", discarded ");
    KernelWriteDecimal(MouseEventsDiscarded);
    KernelWriteString(".\n");

    KernelWriteString("PS/2 mouse: pointer at ");
    KernelWriteDecimal((uint64_t)(uint32_t)MousePositionX);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)(uint32_t)MousePositionY);
    KernelWriteString(" within ");
    KernelWriteDecimal((uint64_t)(uint32_t)MouseBoundWidth);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)(uint32_t)MouseBoundHeight);
    KernelWriteString(", buttons ");
    KernelWriteHexadecimal((uint64_t)MouseButtonState);
    KernelWriteString(".\n");
}
