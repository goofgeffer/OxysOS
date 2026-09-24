/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/terminal/terminal.c
 * Purpose: Asserts the terminal input path of sub-task 8.1 — that a key event
 *          becomes the bytes a terminal would send, that the bytes are
 *          delivered in the order they were queued, and that the queue's bound
 *          is honoured — by driving the keyboard decoder with scancodes upon a
 *          machine at which nobody is typing.
 * Key functions: KernelVerifyTerminal.
 * References:
 *   - docs/design/SHELL.md: the table pairing every property
 *     asserted below with the silent failure the assertion exists to catch.
 *   - kernel/include/oxys/terminal/terminal.h: what is promised of the stream.
 *   - kernel/test/dev/devices.c: the keyboard's own self-test, whose technique
 *     of driving the decoder directly is borrowed here, and which asserts that
 *     a scancode becomes the right event; this asserts that the event becomes
 *     the right bytes.
 *   - IBM Personal Computer AT technical reference, scan code set 1: the codes
 *     below, and 0xE0 as the extended prefix and bit 7 as the break bit.
 *
 * What is not asserted, and why.
 *
 *   That a character received upon the serial line reaches the queue. The
 *   serial self-test cannot make a character arrive, for the reason its own
 *   file records, and neither can this one; the path from the receive buffer
 *   to the queue is four lines of TerminalPoll and is exercised by every boot
 *   at which somebody types upon the serial line, which is every boot under
 *   `make verify` that anybody watches. It is the one thing here that a person
 *   sees and no assertion does.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <oxys/dev/keyboard.h>
#include <oxys/terminal/terminal.h>

static bool VerifyTerminalSucceeded;

static void VerifyTerminalRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyTerminalSucceeded = false;
    }
}

/* Scan code set 1: the codes used below. The break code is the make code with
 * bit 7 set; an extended key is prefixed by 0xE0. */
#define VERIFY_TERMINAL_PREFIX     UINT8_C(0xE0)
#define VERIFY_TERMINAL_BREAK      UINT8_C(0x80)
#define VERIFY_TERMINAL_CONTROL    UINT8_C(0x1D)
#define VERIFY_TERMINAL_SHIFT      UINT8_C(0x2A)
#define VERIFY_TERMINAL_A          UINT8_C(0x1E)
#define VERIFY_TERMINAL_ONE        UINT8_C(0x02)
#define VERIFY_TERMINAL_ENTER      UINT8_C(0x1C)
#define VERIFY_TERMINAL_BACKSPACE  UINT8_C(0x0E)
#define VERIFY_TERMINAL_F1         UINT8_C(0x3B)
#define VERIFY_TERMINAL_HOME       UINT8_C(0x47)
#define VERIFY_TERMINAL_UP         UINT8_C(0x48)
#define VERIFY_TERMINAL_LEFT       UINT8_C(0x4B)
#define VERIFY_TERMINAL_RIGHT      UINT8_C(0x4D)
#define VERIFY_TERMINAL_END        UINT8_C(0x4F)
#define VERIFY_TERMINAL_DOWN       UINT8_C(0x50)
#define VERIFY_TERMINAL_DELETE     UINT8_C(0x53)

/* Presses and releases an ordinary key. */
static void VerifyTerminalTap(uint8_t code)
{
    KeyboardProcessScancode(code);
    KeyboardProcessScancode((uint8_t)(code | VERIFY_TERMINAL_BREAK));
}

/* Presses and releases an extended key. */
static void VerifyTerminalTapExtended(uint8_t code)
{
    KeyboardProcessScancode(VERIFY_TERMINAL_PREFIX);
    KeyboardProcessScancode(code);
    KeyboardProcessScancode(VERIFY_TERMINAL_PREFIX);
    KeyboardProcessScancode((uint8_t)(code | VERIFY_TERMINAL_BREAK));
}

/* Reads everything queued and compares it against `expected`, length first. */
static bool VerifyTerminalDelivers(const char *expected)
{
    char buffer[64];
    size_t length = 0U;
    size_t read;

    while (expected[length] != '\0')
    {
        ++length;
    }

    read = TerminalRead(buffer, sizeof buffer);

    if (read != length)
    {
        return false;
    }

    for (size_t index = 0U; index < length; ++index)
    {
        if (buffer[index] != expected[index])
        {
            return false;
        }
    }

    return true;
}

static void VerifyTerminalKeys(void)
{
    /* An ordinary key becomes its character; a release becomes nothing. */
    VerifyTerminalTap(VERIFY_TERMINAL_A);
    VerifyTerminalRequire(VerifyTerminalDelivers("a"),
                          "a key that produces a character did not become that byte alone");

    /* Enter is a line feed, and backspace is BS: what the keyboard driver's
     * table says, restated here because the editor depends upon both. */
    VerifyTerminalTap(VERIFY_TERMINAL_ENTER);
    VerifyTerminalTap(VERIFY_TERMINAL_BACKSPACE);
    VerifyTerminalRequire(VerifyTerminalDelivers("\n\b"),
                          "enter and backspace did not become LF and BS");

    /* Control with a letter is the control character; with a digit it is the
     * digit; with shift as well it is still the control character, because a
     * terminal sends the same byte for control-A and control-shift-A. */
    KeyboardProcessScancode(VERIFY_TERMINAL_CONTROL);
    VerifyTerminalTap(VERIFY_TERMINAL_A);
    VerifyTerminalTap(VERIFY_TERMINAL_ONE);
    KeyboardProcessScancode(VERIFY_TERMINAL_SHIFT);
    VerifyTerminalTap(VERIFY_TERMINAL_A);
    KeyboardProcessScancode((uint8_t)(VERIFY_TERMINAL_SHIFT | VERIFY_TERMINAL_BREAK));
    KeyboardProcessScancode((uint8_t)(VERIFY_TERMINAL_CONTROL | VERIFY_TERMINAL_BREAK));
    VerifyTerminalRequire(VerifyTerminalDelivers("\x01" "1" "\x01"),
                          "control with a letter, a digit, and a shifted letter did not "
                          "become control-A, 1 and control-A");

    /* Control released: the letter is a letter again. A translation that kept
     * the modifier after its release would turn every later keystroke into a
     * control character, silently. */
    VerifyTerminalTap(VERIFY_TERMINAL_A);
    VerifyTerminalRequire(VerifyTerminalDelivers("a"),
                          "a letter after control was released was still a control character");

    /* The seven extended keys become the seven sequences, in order. */
    {
        const uint64_t translated = TerminalKeysTranslated();

        VerifyTerminalTapExtended(VERIFY_TERMINAL_UP);
        VerifyTerminalTapExtended(VERIFY_TERMINAL_DOWN);
        VerifyTerminalTapExtended(VERIFY_TERMINAL_RIGHT);
        VerifyTerminalTapExtended(VERIFY_TERMINAL_LEFT);
        VerifyTerminalTapExtended(VERIFY_TERMINAL_HOME);
        VerifyTerminalTapExtended(VERIFY_TERMINAL_END);
        VerifyTerminalTapExtended(VERIFY_TERMINAL_DELETE);
        VerifyTerminalRequire(
            VerifyTerminalDelivers("\x1B[A\x1B[B\x1B[C\x1B[D\x1B[H\x1B[F\x1B[3~"),
            "the cursor, home, end and delete keys did not become their control "
            "sequences in order");
        VerifyTerminalRequire(TerminalKeysTranslated() == translated + 7U,
                              "seven translated keys were not counted as seven");
    }

    /* A key with no character and no sequence — a function key — is nothing.
     * The alternative, delivering some byte for it, would put a byte the editor
     * did not ask for into a line the person did not see. */
    VerifyTerminalTap(VERIFY_TERMINAL_F1);
    VerifyTerminalRequire(!TerminalHasInput(),
                          "a key with no character and no sequence produced a byte");

    /* And an extended key with a character position but no sequence — the
     * extended 0x1C, the keypad's enter — is nothing too, rather than the
     * main enter's character read from the wrong table. */
    VerifyTerminalTapExtended(VERIFY_TERMINAL_ENTER);
    VerifyTerminalRequire(!TerminalHasInput(),
                          "an extended key sharing an ordinary key's code produced that "
                          "key's character");
}

static void VerifyTerminalQueue(void)
{
    static char pattern[TERMINAL_QUEUE_CAPACITY + 8U];
    char buffer[64];
    const uint64_t discarded = TerminalBytesDiscarded();

    /* Injected bytes come out in order and in the counts asked for, a read of
     * fewer than are queued leaving the rest. */
    TerminalInject("abcdef", 6U);
    VerifyTerminalRequire(TerminalBytesQueued() == 6U, "six injected bytes were not six queued");
    VerifyTerminalRequire((TerminalRead(buffer, 4U) == 4U) && (buffer[0] == 'a') &&
                              (buffer[3] == 'd'),
                          "a read of four of six did not deliver the first four");
    VerifyTerminalRequire(VerifyTerminalDelivers("ef"),
                          "the two bytes a short read left were not the last two");

    /* A read of an empty queue delivers nothing and waits for nothing. */
    VerifyTerminalRequire(TerminalRead(buffer, sizeof buffer) == 0U,
                          "a read of an empty queue delivered something");

    /* The bound. Eight bytes beyond the capacity are discarded and counted,
     * and the first bytes — not the last — are what survive. */
    for (size_t index = 0U; index < sizeof pattern; ++index)
    {
        pattern[index] = (char)('A' + (index % 26U));
    }

    TerminalInject(pattern, sizeof pattern);
    VerifyTerminalRequire(TerminalBytesQueued() == TERMINAL_QUEUE_CAPACITY,
                          "the queue did not fill to its capacity and stop");
    VerifyTerminalRequire(TerminalBytesDiscarded() == discarded + 8U,
                          "the bytes beyond the capacity were not counted as discarded");

    {
        size_t total = 0U;
        bool ordered = true;

        for (;;)
        {
            const size_t read = TerminalRead(buffer, sizeof buffer);

            if (read == 0U)
            {
                break;
            }

            for (size_t index = 0U; index < read; ++index)
            {
                if (buffer[index] != pattern[total + index])
                {
                    ordered = false;
                }
            }

            total += read;
        }

        VerifyTerminalRequire((total == TERMINAL_QUEUE_CAPACITY) && ordered,
                              "a full queue did not deliver its first capacity's worth of "
                              "bytes in order");
    }

    /* Flushing empties the queue and the devices beneath it. */
    TerminalInject("xyz", 3U);
    VerifyTerminalTap(VERIFY_TERMINAL_A);
    TerminalFlush();
    VerifyTerminalRequire(!TerminalHasInput() && (TerminalBytesQueued() == 0U),
                          "a flush left a byte queued or a key event pending");
}

void KernelVerifyTerminal(void)
{
    VerifyTerminalSucceeded = true;

    KernelWriteString("Terminal: asserting the byte stream a program reads as its "
                      "standard input.\n");

    /* Anything a person typed during the boot is discarded first, so that it is
     * not read back as the answer to an assertion below. */
    TerminalFlush();

    VerifyTerminalKeys();
    VerifyTerminalQueue();

    TerminalFlush();

    if (VerifyTerminalSucceeded)
    {
        KernelWriteString("Terminal self-test passed: characters, control characters and "
                          "the seven translated keys arrive as a terminal would send them, "
                          "in order, and the queue's bound holds.\n");
    }
    else
    {
        KernelWriteString("Terminal self-test FAILED.\n");
    }
}
