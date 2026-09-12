/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/keyboard/keyboard.c
 * Purpose: Implements the PS/2 keyboard driver: the initialisation of the
 *          keyboard upon the first port of the 8042 controller, the translation
 *          of scan code set 1 into characters, the tracking of the modifier
 *          keys, and the circular buffer by which keystrokes are delivered from
 *          the interrupt handler to the rest of the kernel.
 * Key functions: KeyboardInitialise, KeyboardIsPresent, KeyboardProcessScancode,
 *          KeyboardReadEvent, KeyboardReadCharacter, KeyboardHasEvent,
 *          KeyboardFlush, KeyboardModifiers, KeyboardScancodeCount,
 *          KeyboardEventCount, KeyboardOverflowCount, KeyboardReport.
 * References:
 *   - IBM Personal Computer AT technical reference, scan code set 1: a make code
 *     is the key's own code, and the break code is that code with bit 7 set; a
 *     code prefixed by 0xE0 denotes one of the keys added after the original
 *     84-key layout. The keyboard is attached to the interrupt controller's IR1
 *     line.
 *   - The PS/2 device command set: 0xFF resets a device, which answers 0xFA and
 *     then 0xAA upon a successful self-test, and 0xF4 enables scanning; a device
 *     answers 0xFA to acknowledge a command and 0xFE to ask that it be sent
 *     again. The controller commands, the status register and the configuration
 *     byte are cited in <oxys/dev/ps2.h>, which owns them.
 *   - docs/devices/KEYBOARD.md, Sections 2 and 3: the controller and the
 *     keyboard upon it, and why they are different devices.
 *
 * Where the controller went.
 *
 *   Until sub-task 6.5 this file also drove the 8042 itself: the status waits,
 *   the self-test, the configuration byte. It no longer does. The mouse of that
 *   sub-task sits upon the same controller's second port, and the configuration
 *   byte governs both ports and is written whole; two drivers each keeping their
 *   own idea of it would each write back the other's bits as they last saw them.
 *   The controller therefore has one owner, drivers/ps2/ps2.c, and this file is
 *   a driver for the keyboard alone.
 *
 *   The translation of scan code set 2 into set 1 went with it, and belongs
 *   there: it is the controller that translates, and the keyboard sends set 2
 *   whatever this kernel does. What is decoded below is set 1 because
 *   Ps2Initialise establishes the translation rather than assuming it.
 *
 * Concurrency. The circular buffer has a single producer, the interrupt handler,
 * and a single consumer. The producer advances the write index alone and the
 * consumer the read index alone, and each reads the other's index without
 * modifying it, so the arrangement is correct without a lock upon one processor.
 * The spinlock of sub-task 6.13 exists and has not been applied here. With
 * several consumers possible it is the consumer's side that requires it; the
 * producer's side does not, there being one keyboard and therefore one producer.
 */

#include <oxys/dev/keyboard.h>
#include <oxys/dev/ps2.h>
#include <oxys/dev/mouse.h>
#include <oxys/arch/interrupt/irq.h>
#include <oxys/arch/interrupt/interrupts.h>
#include <oxys/kernel.h>

/*
 * The device commands this driver issues, and the answer a reset ends with.
 *
 * The acknowledgement and the resend request are not here: every PS/2 device
 * uses them alike, so they belong to the controller module and are declared by
 * <oxys/dev/ps2.h>.
 */
#define KEYBOARD_DEVICE_RESET           UINT8_C(0xFF)
#define KEYBOARD_DEVICE_ENABLE_SCANNING UINT8_C(0xF4)

/* The prefix denoting an extended scancode, and the bit denoting a release. */
#define KEYBOARD_EXTENDED_PREFIX UINT8_C(0xE0)
#define KEYBOARD_BREAK_BIT       UINT8_C(0x80)

/* The scancodes of the keys whose state is tracked rather than reported. */
#define KEYBOARD_SCANCODE_LEFT_CONTROL UINT8_C(0x1D)
#define KEYBOARD_SCANCODE_LEFT_SHIFT   UINT8_C(0x2A)
#define KEYBOARD_SCANCODE_RIGHT_SHIFT  UINT8_C(0x36)
#define KEYBOARD_SCANCODE_LEFT_ALT     UINT8_C(0x38)
#define KEYBOARD_SCANCODE_CAPS_LOCK    UINT8_C(0x3A)

/* Whether a working keyboard was found upon the controller's first port. */
static bool KeyboardPresent;

/* The modifiers presently in force, and whether the next code is extended. */
static uint8_t KeyboardModifierState;
static bool KeyboardExtendedPending;

/*
 * The circular buffer, and the free-running indices into it.
 *
 * The indices are not wrapped to the capacity; they increase without bound and
 * are masked when used. This removes the ambiguity that afflicts the usual
 * arrangement, in which equal indices denote a buffer that is either empty or
 * full and a further datum is needed to say which. Their difference is the
 * occupancy directly, and the unsigned arithmetic remains correct across the
 * wrap of the index itself.
 */
static KeyEvent KeyboardBuffer[KEYBOARD_BUFFER_CAPACITY];
static volatile uint32_t KeyboardWriteIndex;
static volatile uint32_t KeyboardReadIndex;

/*
 * The capacity must be a power of two, an index being reduced to a subscript by
 * a bitwise mask. The requirement is load-bearing rather than an optimisation: a
 * capacity of, say, one hundred would leave the remainder operator correct but
 * would make the mask address only the first sixty-four entries, silently
 * corrupting the buffer. The assertion converts that into a build failure.
 */
_Static_assert((KEYBOARD_BUFFER_CAPACITY & (KEYBOARD_BUFFER_CAPACITY - 1U)) == 0U,
               "The keyboard buffer capacity must be a power of two.");

/* Accounting. */
static uint64_t KeyboardScancodesDecoded;
static uint64_t KeyboardEventsProduced;
static uint64_t KeyboardEventsDiscarded;

/*
 * Scan code set 1, unshifted. The index is the make code with the break bit
 * removed. A zero denotes a key that produces no character, whether because it
 * is a modifier, a function key, or unassigned.
 */
static const char KeyboardCharacters[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6',  /* 0x00 */
    '7',  '8', '9', '0', '-', '=', '\b', '\t', /* 0x08 */
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  /* 0x10 */
    'o',  'p', '[', ']', '\n', 0,   'a', 's',  /* 0x18 */
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',  /* 0x20 */
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', /* 0x28 */
    'b',  'n', 'm', ',', '.', '/', 0,   '*',  /* 0x30 */
    0,    ' ', 0,   0,   0,   0,   0,   0,    /* 0x38 */
    0,    0,   0,   0,   0,   0,   0,   '7',  /* 0x40 */
    '8',  '9', '-', '4', '5', '6', '+', '1',  /* 0x48 */
    '2',  '3', '0', '.', 0,   0,   0,   0,    /* 0x50 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x58 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x60 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x68 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x70 */
    0,    0,   0,   0,   0,   0,   0,   0     /* 0x78 */
};

/*
 * Scan code set 1, with a shift key held. The keypad is deliberately identical
 * to the unshifted table: upon a real keyboard the shift key interacts with the
 * number lock latch to produce the cursor movements, which this driver does not
 * yet distinguish, and yielding the digit is the more useful of the two
 * simplifications available.
 */
static const char KeyboardShiftedCharacters[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^',  /* 0x00 */
    '&',  '*', '(', ')', '_', '+', '\b', '\t', /* 0x08 */
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',  /* 0x10 */
    'O',  'P', '{', '}', '\n', 0,   'A', 'S',  /* 0x18 */
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',  /* 0x20 */
    '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',  /* 0x28 */
    'B',  'N', 'M', '<', '>', '?', 0,   '*',  /* 0x30 */
    0,    ' ', 0,   0,   0,   0,   0,   0,    /* 0x38 */
    0,    0,   0,   0,   0,   0,   0,   '7',  /* 0x40 */
    '8',  '9', '-', '4', '5', '6', '+', '1',  /* 0x48 */
    '2',  '3', '0', '.', 0,   0,   0,   0,    /* 0x50 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x58 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x60 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x68 */
    0,    0,   0,   0,   0,   0,   0,   0,    /* 0x70 */
    0,    0,   0,   0,   0,   0,   0,   0     /* 0x78 */
};

/*
 * Appends an event to the circular buffer.
 *
 * A buffer that is full discards the new event rather than the oldest. The
 * oldest are the characters typed first, and for a line of input the beginning
 * matters more than the end; discarding from the front would also mean that a
 * burst of keystrokes silently rewrote the text a consumer had not yet read.
 * The discard is counted, so that the loss is visible rather than merely
 * suffered.
 */
static void KeyboardAppendEvent(const KeyEvent *event)
{
    if ((uint32_t)(KeyboardWriteIndex - KeyboardReadIndex) >=
        (uint32_t)KEYBOARD_BUFFER_CAPACITY)
    {
        ++KeyboardEventsDiscarded;
        return;
    }

    KeyboardBuffer[KeyboardWriteIndex & (KEYBOARD_BUFFER_CAPACITY - 1U)] = *event;

    /*
     * The index is advanced only after the event has been written. A consumer
     * observing the advance is thereby guaranteed that the event beneath it is
     * complete.
     */
    ++KeyboardWriteIndex;
    ++KeyboardEventsProduced;
}

/*
 * Applies a scancode to the modifier state, and reports whether it was a
 * modifier and therefore requires no further treatment.
 */
static bool KeyboardApplyModifier(uint8_t scancode, bool pressed, bool extended)
{
    uint8_t flag;

    switch (scancode)
    {
    case KEYBOARD_SCANCODE_LEFT_SHIFT:
    case KEYBOARD_SCANCODE_RIGHT_SHIFT:
        /* The extended prefix is not used by either shift key; a code that bore
         * one is some other key and is not treated as a shift. */
        if (extended)
        {
            return false;
        }
        flag = KEYBOARD_MODIFIER_SHIFT;
        break;

    case KEYBOARD_SCANCODE_LEFT_CONTROL:
        /* With the prefix this is the right control key, which sets the same
         * flag; the two are not distinguished. */
        flag = KEYBOARD_MODIFIER_CONTROL;
        break;

    case KEYBOARD_SCANCODE_LEFT_ALT:
        flag = KEYBOARD_MODIFIER_ALT;
        break;

    case KEYBOARD_SCANCODE_CAPS_LOCK:
        /*
         * A latch rather than a state that follows the key. It is toggled upon
         * depression alone; toggling upon the release as well would return it to
         * where it began and the key would appear to do nothing.
         */
        if (pressed && !extended)
        {
            KeyboardModifierState ^= KEYBOARD_MODIFIER_CAPS_LOCK;
        }
        return !extended;

    default:
        return false;
    }

    if (pressed)
    {
        KeyboardModifierState |= flag;
    }
    else
    {
        KeyboardModifierState &= (uint8_t)~flag;
    }

    return true;
}

/*
 * Selects the character a scancode yields under the modifiers in force.
 *
 * Shift and capitals lock combine differently according to the key, and the
 * difference is not a refinement. Capitals lock alters the letters alone; it
 * does not turn the digit 1 into an exclamation mark. For a letter the two
 * therefore combine as an exclusive disjunction, so that shift with the lock
 * engaged yields a lower-case letter, and for every other key the lock is
 * disregarded.
 */
static char KeyboardCharacterFor(uint8_t scancode, uint8_t modifiers)
{
    const bool shift_held = (modifiers & KEYBOARD_MODIFIER_SHIFT) != 0U;
    const bool caps_engaged = (modifiers & KEYBOARD_MODIFIER_CAPS_LOCK) != 0U;
    const char unshifted = KeyboardCharacters[scancode];
    const bool is_letter = (unshifted >= 'a') && (unshifted <= 'z');
    bool use_shifted;

    if (is_letter)
    {
        use_shifted = (shift_held != caps_engaged);
    }
    else
    {
        use_shifted = shift_held;
    }

    return use_shifted ? KeyboardShiftedCharacters[scancode] : unshifted;
}

void KeyboardProcessScancode(uint8_t scancode)
{
    KeyEvent event;
    uint8_t code;
    bool pressed;
    bool extended;

    ++KeyboardScancodesDecoded;

    /*
     * The prefix is not itself a key. It is recorded and the next code is
     * interpreted in its light; two prefixes in succession are treated as one,
     * which costs nothing and avoids an ambiguous state.
     */
    if (scancode == KEYBOARD_EXTENDED_PREFIX)
    {
        KeyboardExtendedPending = true;
        return;
    }

    extended = KeyboardExtendedPending;
    KeyboardExtendedPending = false;

    /* Bit 7 distinguishes a release from a depression; the remaining seven bits
     * are the key's own code in either case. */
    pressed = (scancode & KEYBOARD_BREAK_BIT) == 0U;
    code = (uint8_t)(scancode & (uint8_t)~KEYBOARD_BREAK_BIT);

    if (KeyboardApplyModifier(code, pressed, extended))
    {
        /*
         * A modifier alters the state and produces no event of its own. A
         * consumer wanting to observe the modifier keys themselves would need
         * this to change; nothing does at present, and reporting them would
         * oblige every consumer of characters to filter them out.
         */
        return;
    }

    event.scancode = code;
    event.pressed = pressed;
    event.extended = extended;
    event.modifiers = KeyboardModifierState;

    /*
     * An extended code shares its number with an ordinary key — the extended
     * 0x1C is the keypad's enter and the ordinary 0x1C the main one — so the
     * character tables, which are indexed by the number alone, must not be
     * consulted for it. The extended keys that do produce characters are the
     * keypad's enter and solidus, and they are left to a later phase rather than
     * given a table of their own for two entries.
     */
    event.character = extended ? '\0' : KeyboardCharacterFor(code, KeyboardModifierState);

    KeyboardAppendEvent(&event);
}

/*
 * Receives the keyboard's request line.
 *
 * Exactly one byte is read. The controller raises the request once per byte it
 * has to offer, so a handler that drained the buffer in a loop would consume
 * bytes whose requests were still to be delivered, and those requests would then
 * find nothing to read. The end-of-interrupt is signalled by the routing layer
 * of drivers/pic/pic.c upon this handler's return.
 */
static void KeyboardHandleInterrupt(TrapFrame *frame)
{
    uint8_t scancode;
    uint8_t port;

    (void)frame;

    if (!Ps2ReadPending(&scancode, &port))
    {
        return;
    }

    /*
     * A byte from the second port is the mouse's, and is left to the mouse's
     * handler — but it has already been taken from the controller by the read
     * above, which cannot be undone: the output buffer holds one byte and
     * reading it is what empties it.
     *
     * It is therefore handed across rather than discarded. This is not a
     * hypothetical: both devices deliver through the one buffer, and the
     * controller raises IR1 and IR12 for bytes that queue behind one another, so
     * a movement packet arriving while a keystroke is being serviced is
     * routinely presented to whichever handler runs next. Discarding it would
     * lose one byte of a three-byte packet, and a packet decoder that has lost a
     * byte does not merely miss one movement; it is out of step with every
     * packet after it until the framing bit brings it back.
     */
    if (port == PS2_PORT_SECOND)
    {
        MouseProcessByte(scancode);
        return;
    }

    KeyboardProcessScancode(scancode);
}

bool KeyboardInitialise(void)
{
    uint8_t answer;

    KeyboardPresent = false;

    /*
     * The controller is not initialised here. It is one device shared with the
     * mouse, it is established by Ps2Initialise before either driver runs, and
     * this driver refuses to proceed rather than reconfiguring it: a keyboard
     * driver that reset the controller would silence a mouse already reporting.
     */
    if (!Ps2PortIsUsable(PS2_PORT_FIRST))
    {
        return false;
    }

    if (!Ps2EnablePort(PS2_PORT_FIRST))
    {
        return false;
    }

    /*
     * Reset the keyboard. It acknowledges, then reports the result of its own
     * self-test as a second byte. The second byte is read but not insisted
     * upon: some emulated keyboards omit it, and a keyboard that answered the
     * reset at all is working well enough to proceed with.
     */
    if (!Ps2SendDeviceCommand(PS2_PORT_FIRST, KEYBOARD_DEVICE_RESET))
    {
        return false;
    }

    if (Ps2ReadData(&answer) && answer != PS2_SELF_TEST_PASSED)
    {
        return false;
    }

    if (!Ps2SendDeviceCommand(PS2_PORT_FIRST, KEYBOARD_DEVICE_ENABLE_SCANNING))
    {
        return false;
    }

    /* Anything the reset or the enabling left behind is not a keystroke. */
    Ps2DrainOutputBuffer();

    /* Permit the controller to raise this port's request line. */
    if (!Ps2SetPortInterrupt(PS2_PORT_FIRST, true))
    {
        return false;
    }

    KeyboardPresent = true;

    /*
     * The line is unmasked only after the handler is registered, so that a
     * keystroke arriving between the two cannot be recorded as an unclaimed
     * request and lost.
     */
    IrqInstallHandler(KEYBOARD_IRQ, KeyboardHandleInterrupt, "PS/2 keyboard");
    IrqUnmaskLine(KEYBOARD_IRQ);

    return true;
}

bool KeyboardIsPresent(void)
{
    return KeyboardPresent;
}

bool KeyboardHasEvent(void)
{
    return KeyboardWriteIndex != KeyboardReadIndex;
}

bool KeyboardReadEvent(KeyEvent *event)
{
    if (event == NULL || !KeyboardHasEvent())
    {
        return false;
    }

    *event = KeyboardBuffer[KeyboardReadIndex & (KEYBOARD_BUFFER_CAPACITY - 1U)];
    ++KeyboardReadIndex;

    return true;
}

bool KeyboardReadCharacter(char *character)
{
    KeyEvent event;

    if (character == NULL)
    {
        return false;
    }

    while (KeyboardReadEvent(&event))
    {
        if (event.pressed && event.character != '\0')
        {
            *character = event.character;
            return true;
        }
    }

    return false;
}

void KeyboardFlush(void)
{
    KeyboardReadIndex = KeyboardWriteIndex;

    /*
     * The capitals lock latch is deliberately retained. It reflects a state the
     * operator chose and, upon a real keyboard, a lamp that is still lit; the
     * transient modifiers are cleared because a key held across a flush cannot
     * be known still to be held.
     */
    KeyboardModifierState &= KEYBOARD_MODIFIER_CAPS_LOCK;
    KeyboardExtendedPending = false;
}

uint8_t KeyboardModifiers(void)
{
    return KeyboardModifierState;
}

uint64_t KeyboardScancodeCount(void)
{
    return KeyboardScancodesDecoded;
}

uint64_t KeyboardEventCount(void)
{
    return KeyboardEventsProduced;
}

uint64_t KeyboardOverflowCount(void)
{
    return KeyboardEventsDiscarded;
}

void KeyboardReport(void)
{
    KernelWriteString("PS/2 keyboard: ");

    if (!KeyboardPresent)
    {
        KernelWriteString("absent or unusable; no request line claimed.\n");
        return;
    }

    KernelWriteString("present, scan code set 1 by controller translation, line ");
    KernelWriteString(IrqLineIsMasked(KEYBOARD_IRQ) ? "masked" : "unmasked");
    KernelWriteString(".\n");

    KernelWriteString("PS/2 keyboard: scancodes ");
    KernelWriteDecimal(KeyboardScancodesDecoded);
    KernelWriteString(", events ");
    KernelWriteDecimal(KeyboardEventsProduced);
    KernelWriteString(", buffered ");
    KernelWriteDecimal((uint64_t)(uint32_t)(KeyboardWriteIndex - KeyboardReadIndex));
    KernelWriteString(", discarded ");
    KernelWriteDecimal(KeyboardEventsDiscarded);
    KernelWriteString(", modifiers ");
    KernelWriteHexadecimal((uint64_t)KeyboardModifierState);
    KernelWriteString(".\n");
}
