/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/term/keys.c
 * Purpose: Turns one key, as a window receives it, into the bytes a terminal
 *          would send for it — the other pure half of sub-task 9.6's emulator,
 *          and the half that decides what the shell beneath it reads.
 * Key functions: TermKeyBytes, TermSequenceFor.
 * References:
 *   - ECMA-48, 5th edition (1991), Sections 8.3.18 (CUB), 8.3.19 (CUD),
 *     8.3.20 (CUF) and 8.3.22 (CUU): the final bytes D, B, C and A that the
 *     cursor keys send after CSI.
 *   - XTerm Control Sequences (Dickey), "PC-Style Function Keys": Home and End
 *     are CSI H and CSI F, and Delete is CSI 3 ~.
 *   - IBM Personal Computer AT technical reference, scan code set 1: the
 *     extended scancodes translated below, each prefixed by 0xE0 upon the wire
 *     and delivered without the prefix, with `extended` saying it was there.
 *   - kernel/terminal/terminal.c: the kernel's translation of the same keys for
 *     the terminal it assembles, which this agrees with byte for byte.
 *
 * Why this is written a second time rather than shared with the kernel's.
 *
 *   The kernel is LGPL-3.0-or-later and this library is MIT, and a program
 *   under this licence may not link the kernel's — the wall the disc of
 *   sub-task 9.2 was redrawn behind, and the font of 9.5 stands behind still.
 *   So the table is written again. **It must agree with the kernel's**, because
 *   the line editor of sub-task 8.1 parses one dialect and the same shell runs
 *   at a window and at a serial line; where they differ, the cursor keys work
 *   in one place and not in the other. The kernel's self-test asserts this copy
 *   against the same sequences, which is what makes the agreement checkable
 *   rather than remembered.
 */

#include <term.h>

/* The extended scancodes given a control sequence, scan code set 1. */
#define TERM_SCANCODE_HOME   UINT8_C(0x47)
#define TERM_SCANCODE_UP     UINT8_C(0x48)
#define TERM_SCANCODE_LEFT   UINT8_C(0x4B)
#define TERM_SCANCODE_RIGHT  UINT8_C(0x4D)
#define TERM_SCANCODE_END    UINT8_C(0x4F)
#define TERM_SCANCODE_DOWN   UINT8_C(0x50)
#define TERM_SCANCODE_DELETE UINT8_C(0x53)

const char *TermSequenceFor(uint8_t scancode)
{
    switch (scancode)
    {
    case TERM_SCANCODE_UP:
        return "\x1B[A";
    case TERM_SCANCODE_DOWN:
        return "\x1B[B";
    case TERM_SCANCODE_RIGHT:
        return "\x1B[C";
    case TERM_SCANCODE_LEFT:
        return "\x1B[D";
    case TERM_SCANCODE_HOME:
        return "\x1B[H";
    case TERM_SCANCODE_END:
        return "\x1B[F";
    case TERM_SCANCODE_DELETE:
        return "\x1B[3~";
    default:
        return NULL;
    }
}

size_t TermKeyBytes(char character, uint8_t scancode, uint8_t modifiers, bool extended,
                    char *bytes, size_t capacity)
{
    size_t count = 0U;

    if ((bytes == NULL) || (capacity == 0U))
    {
        return 0U;
    }

    /*
     * A letter with control held becomes the control character a terminal
     * would send — the letter's code with its two high bits cleared, so that
     * control-A is byte 1 whether the letter arrived as `a` or, with shift or
     * capitals lock, as `A`. Nothing else is altered by control: a terminal
     * sends control-1 as `1`, and so does this.
     */
    if (character != '\0')
    {
        char byte = character;

        if ((modifiers & TERM_MODIFIER_CONTROL) != 0U)
        {
            const bool lower = (byte >= 'a') && (byte <= 'z');
            const bool upper = (byte >= 'A') && (byte <= 'Z');

            if (lower || upper)
            {
                byte = (char)(byte & 0x1F);
            }
        }

        bytes[0] = byte;

        return 1U;
    }

    /*
     * A key that produces no character contributes bytes only where it is one
     * of the seven a terminal has a sequence for. Every other — a shift, a
     * function key, a capitals lock — contributes nothing, which is what a
     * terminal does with a key it has no code for and is not the same as
     * contributing a zero byte.
     */
    if (extended)
    {
        const char *const sequence = TermSequenceFor(scancode);

        if (sequence == NULL)
        {
            return 0U;
        }

        while ((sequence[count] != '\0') && (count < capacity))
        {
            bytes[count] = sequence[count];
            ++count;
        }
    }

    return count;
}
