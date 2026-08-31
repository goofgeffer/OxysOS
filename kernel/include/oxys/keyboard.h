/*
 * File: kernel/include/oxys/keyboard.h
 * Purpose: Declares the interface of the PS/2 keyboard driver: the initialisation
 *          of the 8042 controller, the decoded key event, the modifier state, and
 *          the circular buffer through which keystrokes reach the rest of the
 *          kernel.
 * Key definitions: KeyEvent, KEYBOARD_MODIFIER_SHIFT, KEYBOARD_MODIFIER_CONTROL,
 *          KEYBOARD_MODIFIER_ALT, KEYBOARD_MODIFIER_CAPS_LOCK, KEYBOARD_IRQ,
 *          KEYBOARD_BUFFER_CAPACITY, KeyboardInitialise, KeyboardIsPresent,
 *          KeyboardProcessScancode, KeyboardReadEvent, KeyboardReadCharacter,
 *          KeyboardHasEvent, KeyboardFlush, KeyboardModifiers,
 *          KeyboardScancodeCount, KeyboardEventCount, KeyboardOverflowCount,
 *          KeyboardReport.
 * References:
 *   - IBM Personal Computer AT technical reference, the 8042 keyboard
 *     controller: the data port at 0x60 and the status and command port at 0x64;
 *     status bit 0 denotes that the output buffer holds a byte for the processor
 *     and bit 1 that the input buffer still holds one for the controller; the
 *     keyboard is attached to the interrupt controller's IR1 line.
 *   - IBM Personal Computer AT technical reference, scan code set 1: a make code
 *     is the key's code and the corresponding break code is that code with bit 7
 *     set; a code prefixed by 0xE0 denotes a key added after the original
 *     84-key layout.
 *   - The 8042 controller and PS/2 device command sets: 0x20 reads the
 *     controller configuration byte and 0x60 writes it; 0xAD and 0xAE disable
 *     and enable the first port; 0xA7 disables the second; 0xAA is the
 *     controller self-test, answered by 0x55; 0xAB tests the first port,
 *     answered by 0x00. Of the device commands, 0xFF resets a device and 0xF4
 *     enables scanning; a device answers 0xFA to acknowledge and 0xFE to request
 *     that the command be sent again.
 */

#ifndef OXYS_KEYBOARD_H
#define OXYS_KEYBOARD_H

#include <oxys/types.h>

/* The request line upon which the keyboard controller signals. */
#define KEYBOARD_IRQ UINT8_C(1)

/*
 * The modifier keys whose state is tracked. The shift, control and alternate
 * flags follow the key: they are set while it is held and cleared when it is
 * released. The capitals lock flag is a latch, toggled by each depression of the
 * key and unaffected by its release.
 */
#define KEYBOARD_MODIFIER_SHIFT     UINT8_C(0x01)
#define KEYBOARD_MODIFIER_CONTROL   UINT8_C(0x02)
#define KEYBOARD_MODIFIER_ALT       UINT8_C(0x04)
#define KEYBOARD_MODIFIER_CAPS_LOCK UINT8_C(0x08)

/*
 * The capacity of the circular buffer, in events. It is a power of two so that
 * an index may be reduced to a subscript by a bitwise mask rather than by a
 * division, the reduction being performed within an interrupt handler.
 */
#define KEYBOARD_BUFFER_CAPACITY 128U

/*
 * One decoded keystroke.
 *
 * Both depressions and releases are recorded, and the scancode is retained
 * alongside the character. A consumer that wants text ignores the releases and
 * reads the character, which KeyboardReadCharacter does on its behalf; a
 * consumer that wants keys, such as the window system of Phase 9, needs the
 * releases and the codes of the keys that produce no character at all.
 */
typedef struct KeyEvent
{
    /* The scancode, with the break bit removed and the extended prefix consumed. */
    uint8_t scancode;

    /*
     * The character the key produces under the modifiers in force, or zero where
     * it produces none, as for a function key or a modifier itself.
     */
    char character;

    /* The modifiers in force at the instant the event was decoded. */
    uint8_t modifiers;

    /* True for a depression, false for a release. */
    bool pressed;

    /* True if the code was prefixed by 0xE0. */
    bool extended;
} KeyEvent;

/*
 * Initialises the 8042 controller and the keyboard attached to its first port,
 * claims the keyboard's request line and unmasks it.
 *
 * Returns false if no working controller or keyboard was found, in which case
 * nothing is claimed and no line is unmasked. A missing keyboard is not a fault:
 * every wait upon the controller is bounded, so a machine that has none proceeds
 * unimpeded rather than blocking upon a status flag that will never change.
 *
 * The interrupt controller must have been initialised by PicInitialise before
 * this is called.
 */
bool KeyboardInitialise(void);

/* Reports whether initialisation found a working keyboard. */
bool KeyboardIsPresent(void);

/*
 * Decodes one scancode, updating the modifier state and appending an event to
 * the buffer where the code completes one.
 *
 * This is the entry point the interrupt handler calls, and it is exposed because
 * the decoding of scan code set 1 is not a property of the 8042: a scancode
 * arriving by any other route decodes identically. The boot-time self-test
 * drives it directly, which is what permits the decoder to be tested upon a
 * machine at which nobody is typing.
 */
void KeyboardProcessScancode(uint8_t scancode);

/*
 * Removes the oldest event from the buffer. Returns false, leaving the argument
 * untouched, if the buffer is empty.
 */
bool KeyboardReadEvent(KeyEvent *event);

/*
 * Removes events until one is found that is a depression and that produces a
 * character, and yields that character. Returns false if the buffer is exhausted
 * without finding one.
 */
bool KeyboardReadCharacter(char *character);

/* Reports whether the buffer holds at least one event. */
bool KeyboardHasEvent(void);

/* Discards every buffered event and clears the transient modifier state. */
void KeyboardFlush(void);

/* The modifiers presently in force. */
uint8_t KeyboardModifiers(void);

/* The number of scancodes decoded, and the number of events they produced. */
uint64_t KeyboardScancodeCount(void);
uint64_t KeyboardEventCount(void);

/* The number of events discarded because the buffer was full. */
uint64_t KeyboardOverflowCount(void);

/* Emits a summary of the keyboard's state upon both output devices. */
void KeyboardReport(void);

#endif /* OXYS_KEYBOARD_H */
