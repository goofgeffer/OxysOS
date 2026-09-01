/*
 * File: drivers/keyboard/keyboard.c
 * Purpose: Implements the PS/2 keyboard driver: the initialisation of the 8042
 *          controller and of the keyboard upon its first port, the translation
 *          of scan code set 1 into characters, the tracking of the modifier
 *          keys, and the circular buffer by which keystrokes are delivered from
 *          the interrupt handler to the rest of the kernel.
 * Key functions: KeyboardInitialise, KeyboardIsPresent, KeyboardProcessScancode,
 *          KeyboardReadEvent, KeyboardReadCharacter, KeyboardHasEvent,
 *          KeyboardFlush, KeyboardModifiers, KeyboardScancodeCount,
 *          KeyboardEventCount, KeyboardOverflowCount, KeyboardReport.
 * References:
 *   - IBM Personal Computer AT technical reference, the 8042 keyboard
 *     controller: the data port at 0x60, and the status register read at 0x64
 *     and the command register written at the same address; status bit 0 is set
 *     while the output buffer holds a byte for the processor, and bit 1 while
 *     the input buffer still holds one for the controller, so a byte may be read
 *     only when bit 0 is set and written only when bit 1 is clear. The keyboard
 *     is attached to the interrupt controller's IR1 line.
 *   - The 8042 controller and PS/2 device command sets: 0x20 reads the
 *     configuration byte and 0x60 writes it; 0xAD and 0xAE disable and enable
 *     the first port, 0xA7 disables the second; 0xAA is the controller self-test
 *     and answers 0x55 upon success; 0xAB tests the first port and answers 0x00
 *     upon success. Of the device commands, 0xFF resets a device, which answers
 *     0xFA and then 0xAA upon a successful self-test, and 0xF4 enables scanning;
 *     a device answers 0xFA to acknowledge a command and 0xFE to ask that it be
 *     sent again.
 *   - The same, the configuration byte: bit 0 enables the interrupt of the first
 *     port, bit 1 that of the second, bit 4 disables the first port's clock when
 *     set, and bit 6 enables the translation of scan code set 2 into set 1.
 *   - IBM Personal Computer AT technical reference, scan code set 1: a make code
 *     is the key's own code, and the break code is that code with bit 7 set; a
 *     code prefixed by 0xE0 denotes one of the keys added after the original
 *     84-key layout.
 *
 * Why the translation bit is set rather than assumed.
 *
 *   A PS/2 keyboard powers up in scan code set 2, not set 1. Set 1 is what the
 *   processor sees only because the 8042 translates on its behalf, that
 *   translation being governed by bit 6 of the configuration byte. The firmware
 *   ordinarily enables it, so a driver that merely assumed set 1 would work upon
 *   most machines and fail upon those where it did not — and would fail by
 *   delivering plausible characters that were simply the wrong ones, since the
 *   two sets overlap without agreeing.
 *
 *   This driver therefore sets the bit explicitly and keeps it set. The
 *   alternative, clearing it and decoding set 2 directly, is defensible and is
 *   what a driver supporting a USB-attached keyboard would eventually want; it
 *   is not what sub-task 3.7 of docs/project/PLAN.md specifies.
 *
 * Concurrency. The circular buffer has a single producer, the interrupt handler,
 * and a single consumer. The producer advances the write index alone and the
 * consumer the read index alone, and each reads the other's index without
 * modifying it, so the arrangement is correct without a lock upon one processor.
 * From sub-task 6.9, with several consumers possible, the consumer's side
 * requires the spinlock governing this device; the producer's side does not,
 * there being one keyboard and therefore one producer.
 */

#include <oxys/keyboard.h>
#include <oxys/pic.h>
#include <oxys/interrupts.h>
#include <oxys/io.h>
#include <oxys/kernel.h>

/* The data port, and the status and command port. */
#define KEYBOARD_DATA_PORT    UINT16_C(0x0060)
#define KEYBOARD_STATUS_PORT  UINT16_C(0x0064)
#define KEYBOARD_COMMAND_PORT UINT16_C(0x0064)

/* Status register bits. */
#define KEYBOARD_STATUS_OUTPUT_FULL UINT8_C(0x01)
#define KEYBOARD_STATUS_INPUT_FULL  UINT8_C(0x02)

/* Controller commands. */
#define KEYBOARD_COMMAND_READ_CONFIGURATION  UINT8_C(0x20)
#define KEYBOARD_COMMAND_WRITE_CONFIGURATION UINT8_C(0x60)
#define KEYBOARD_COMMAND_DISABLE_SECOND_PORT UINT8_C(0xA7)
#define KEYBOARD_COMMAND_SELF_TEST           UINT8_C(0xAA)
#define KEYBOARD_COMMAND_TEST_FIRST_PORT     UINT8_C(0xAB)
#define KEYBOARD_COMMAND_DISABLE_FIRST_PORT  UINT8_C(0xAD)
#define KEYBOARD_COMMAND_ENABLE_FIRST_PORT   UINT8_C(0xAE)

/* The answers those commands give upon success. */
#define KEYBOARD_SELF_TEST_PASSED UINT8_C(0x55)
#define KEYBOARD_PORT_TEST_PASSED UINT8_C(0x00)

/* Configuration byte bits. */
#define KEYBOARD_CONFIGURATION_FIRST_PORT_INTERRUPT UINT8_C(0x01)
#define KEYBOARD_CONFIGURATION_SECOND_PORT_INTERRUPT UINT8_C(0x02)
#define KEYBOARD_CONFIGURATION_FIRST_PORT_CLOCK_OFF UINT8_C(0x10)
#define KEYBOARD_CONFIGURATION_TRANSLATION          UINT8_C(0x40)

/* Device commands and answers. */
#define KEYBOARD_DEVICE_RESET           UINT8_C(0xFF)
#define KEYBOARD_DEVICE_ENABLE_SCANNING UINT8_C(0xF4)
#define KEYBOARD_DEVICE_ACKNOWLEDGE     UINT8_C(0xFA)
#define KEYBOARD_DEVICE_RESEND          UINT8_C(0xFE)
#define KEYBOARD_DEVICE_SELF_TEST_PASSED UINT8_C(0xAA)

/* The prefix denoting an extended scancode, and the bit denoting a release. */
#define KEYBOARD_EXTENDED_PREFIX UINT8_C(0xE0)
#define KEYBOARD_BREAK_BIT       UINT8_C(0x80)

/* The scancodes of the keys whose state is tracked rather than reported. */
#define KEYBOARD_SCANCODE_LEFT_CONTROL UINT8_C(0x1D)
#define KEYBOARD_SCANCODE_LEFT_SHIFT   UINT8_C(0x2A)
#define KEYBOARD_SCANCODE_RIGHT_SHIFT  UINT8_C(0x36)
#define KEYBOARD_SCANCODE_LEFT_ALT     UINT8_C(0x38)
#define KEYBOARD_SCANCODE_CAPS_LOCK    UINT8_C(0x3A)

/*
 * The bound upon every wait for the controller, in iterations.
 *
 * A bound is not a refinement here but a requirement. The convention recorded in
 * drivers/README.md is that a missing device must never cause the kernel to
 * block, and an unbounded wait upon a status flag is exactly how it would: a
 * machine with no PS/2 controller decodes the port as a constant, and the flag
 * awaited would never change for as long as the machine ran.
 */
#define KEYBOARD_WAIT_LIMIT 100000U

/*
 * The bound upon a drain of the controller's output buffer. It is not related to
 * the capacity of the event buffer and does not share its constant: the two
 * count different things, and a change to one must not silently alter the other.
 * A controller holding more than this many bytes is malfunctioning.
 */
#define KEYBOARD_DRAIN_LIMIT 32U

/* Whether a working controller and keyboard were found. */
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
 * Waits until the controller will accept a byte, which is to say until the input
 * buffer is empty. Returns false if the bound was reached.
 */
static bool KeyboardWaitToWrite(void)
{
    for (uint32_t attempt = 0U; attempt < KEYBOARD_WAIT_LIMIT; ++attempt)
    {
        if ((PortReadByte(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_INPUT_FULL) == 0U)
        {
            return true;
        }
    }

    return false;
}

/*
 * Waits until the controller has a byte for the processor. Returns false if the
 * bound was reached, which is how the absence of a device is discovered.
 */
static bool KeyboardWaitToRead(void)
{
    for (uint32_t attempt = 0U; attempt < KEYBOARD_WAIT_LIMIT; ++attempt)
    {
        if ((PortReadByte(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_OUTPUT_FULL) != 0U)
        {
            return true;
        }
    }

    return false;
}

/* Sends a command to the controller itself. */
static bool KeyboardSendControllerCommand(uint8_t command)
{
    if (!KeyboardWaitToWrite())
    {
        return false;
    }

    PortWriteByte(KEYBOARD_COMMAND_PORT, command);

    return true;
}

/* Writes a byte to the data port, whether as the argument of a controller
 * command or as a command to the device upon the first port. */
static bool KeyboardWriteData(uint8_t value)
{
    if (!KeyboardWaitToWrite())
    {
        return false;
    }

    PortWriteByte(KEYBOARD_DATA_PORT, value);

    return true;
}

/* Reads a byte from the data port, having waited for one to appear. */
static bool KeyboardReadData(uint8_t *value)
{
    if (!KeyboardWaitToRead())
    {
        return false;
    }

    *value = PortReadByte(KEYBOARD_DATA_PORT);

    return true;
}

/*
 * Discards every byte the controller is presently holding.
 *
 * The firmware has been using the keyboard, and may have left a keystroke or the
 * tail of a command exchange in the output buffer. Such a byte would be decoded
 * as a scancode and would appear as a keystroke nobody made.
 */
static void KeyboardDrainOutputBuffer(void)
{
    for (uint32_t attempt = 0U; attempt < KEYBOARD_DRAIN_LIMIT; ++attempt)
    {
        if ((PortReadByte(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_OUTPUT_FULL) == 0U)
        {
            return;
        }

        (void)PortReadByte(KEYBOARD_DATA_PORT);
    }
}

/*
 * Sends a command to the keyboard and awaits its acknowledgement, retrying while
 * the device asks for the command to be sent again.
 *
 * The retry is bounded. A device that asked without end would otherwise hold the
 * processor for ever, and a keyboard that cannot be commanded is better reported
 * as absent than allowed to stop the machine.
 */
static bool KeyboardSendDeviceCommand(uint8_t command)
{
    for (uint32_t attempt = 0U; attempt < 3U; ++attempt)
    {
        uint8_t answer;

        if (!KeyboardWriteData(command))
        {
            return false;
        }

        if (!KeyboardReadData(&answer))
        {
            return false;
        }

        if (answer == KEYBOARD_DEVICE_ACKNOWLEDGE)
        {
            return true;
        }

        if (answer != KEYBOARD_DEVICE_RESEND)
        {
            return false;
        }
    }

    return false;
}

/* Reads the controller configuration byte. */
static bool KeyboardReadConfiguration(uint8_t *configuration)
{
    if (!KeyboardSendControllerCommand(KEYBOARD_COMMAND_READ_CONFIGURATION))
    {
        return false;
    }

    return KeyboardReadData(configuration);
}

/* Writes the controller configuration byte. */
static bool KeyboardWriteConfiguration(uint8_t configuration)
{
    if (!KeyboardSendControllerCommand(KEYBOARD_COMMAND_WRITE_CONFIGURATION))
    {
        return false;
    }

    return KeyboardWriteData(configuration);
}

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
    (void)frame;

    if ((PortReadByte(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_OUTPUT_FULL) == 0U)
    {
        return;
    }

    KeyboardProcessScancode(PortReadByte(KEYBOARD_DATA_PORT));
}

bool KeyboardInitialise(void)
{
    uint8_t configuration;
    uint8_t answer;

    KeyboardPresent = false;

    /*
     * Both ports are disabled first, so that nothing arrives while the
     * controller is being reconfigured and no byte read below belongs to a
     * keystroke rather than to the exchange in progress.
     */
    (void)KeyboardSendControllerCommand(KEYBOARD_COMMAND_DISABLE_FIRST_PORT);
    (void)KeyboardSendControllerCommand(KEYBOARD_COMMAND_DISABLE_SECOND_PORT);

    KeyboardDrainOutputBuffer();

    if (!KeyboardReadConfiguration(&configuration))
    {
        return false;
    }

    /*
     * Silence both ports' interrupts for the duration, ensure the first port's
     * clock is running, and ensure the translation of set 2 into set 1 is in
     * force. The translation is the reason this driver may decode set 1 at all;
     * the keyboard itself is in set 2.
     */
    configuration &= (uint8_t)~(KEYBOARD_CONFIGURATION_FIRST_PORT_INTERRUPT |
                                KEYBOARD_CONFIGURATION_SECOND_PORT_INTERRUPT |
                                KEYBOARD_CONFIGURATION_FIRST_PORT_CLOCK_OFF);
    configuration |= KEYBOARD_CONFIGURATION_TRANSLATION;

    if (!KeyboardWriteConfiguration(configuration))
    {
        return false;
    }

    /* The controller's own self-test. */
    if (!KeyboardSendControllerCommand(KEYBOARD_COMMAND_SELF_TEST) ||
        !KeyboardReadData(&answer) || answer != KEYBOARD_SELF_TEST_PASSED)
    {
        return false;
    }

    /*
     * The self-test resets the controller upon some implementations, discarding
     * the configuration written above. It is therefore written again. Upon an
     * implementation that does not reset, this is merely redundant.
     */
    if (!KeyboardWriteConfiguration(configuration))
    {
        return false;
    }

    /* The first port's own test. */
    if (!KeyboardSendControllerCommand(KEYBOARD_COMMAND_TEST_FIRST_PORT) ||
        !KeyboardReadData(&answer) || answer != KEYBOARD_PORT_TEST_PASSED)
    {
        return false;
    }

    if (!KeyboardSendControllerCommand(KEYBOARD_COMMAND_ENABLE_FIRST_PORT))
    {
        return false;
    }

    /*
     * Reset the keyboard. It acknowledges, then reports the result of its own
     * self-test as a second byte. The second byte is read but not insisted
     * upon: some emulated keyboards omit it, and a keyboard that answered the
     * reset at all is working well enough to proceed with.
     */
    if (!KeyboardSendDeviceCommand(KEYBOARD_DEVICE_RESET))
    {
        return false;
    }

    if (KeyboardReadData(&answer) && answer != KEYBOARD_DEVICE_SELF_TEST_PASSED)
    {
        return false;
    }

    if (!KeyboardSendDeviceCommand(KEYBOARD_DEVICE_ENABLE_SCANNING))
    {
        return false;
    }

    /* Anything the reset or the enabling left behind is not a keystroke. */
    KeyboardDrainOutputBuffer();

    /* Permit the controller to raise its request line. */
    configuration |= KEYBOARD_CONFIGURATION_FIRST_PORT_INTERRUPT;

    if (!KeyboardWriteConfiguration(configuration))
    {
        return false;
    }

    KeyboardPresent = true;

    /*
     * The line is unmasked only after the handler is registered, so that a
     * keystroke arriving between the two cannot be recorded as an unclaimed
     * request and lost.
     */
    PicInstallHandler(KEYBOARD_IRQ, KeyboardHandleInterrupt, "PS/2 keyboard");
    PicUnmaskLine(KEYBOARD_IRQ);

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
    KernelWriteString(PicLineIsMasked(KEYBOARD_IRQ) ? "masked" : "unmasked");
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
