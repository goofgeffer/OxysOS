/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/term.h
 * Purpose: Declares the character grid of sub-task 9.6 — the half of a terminal
 *          emulator that decides what stands where, kept apart from the half
 *          that draws it and from the descriptors it reads, so that the whole of
 *          it can be asserted before there is a window to look at.
 * Key definitions: TermScreen, TERM_COLUMNS_MAXIMUM, TERM_ROWS_MAXIMUM, TermResize,
 *          TermInitialise, TermWrite, TermWriteByte, TermRow, TermRowChanged,
 *          TermRowDrawn, TermColumns, TermRows, TermCursorColumn, TermCursorRow,
 *          TermKeyBytes, TermSequenceFor, TERM_MODIFIER_CONTROL,
 *          TermScrolls, TermBytesWritten.
 * References:
 *   - docs/design/TERMINAL.md: what this interprets and what it
 *     deliberately does not, and why the list is as short as it is.
 *   - docs/design/CONSOLE.md: the kernel's console, which is
 *     the display the Phase 8 shell was written against. What that console does
 *     with a backspace, a carriage return and a line feed is what a program
 *     writing to this one expects, so this does the same thing — including the
 *     backspace that crosses to the row above, which the line editor of
 *     sub-task 8.1 relies upon and which nothing else here would need.
 *   - ECMA-48, 5th edition (1991), Sections 8.3.16 (CR), 8.3.74 (LF) and
 *     8.3.5 (BS): the three control characters this acts upon.
 *
 * Why this is in the C library and not in the program that draws it.
 *
 *   It is the seam of docs/design/LIBC.md applied a third time,
 *   after the line editor's editing and the configuration's parsing. A grid of
 *   characters with a cursor is pure memory: it reads nothing, writes nothing
 *   and calls nothing, so the kernel's self-test can drive it through a session
 *   of bytes and read back what stands where. `/bin/terminal` is then the part
 *   that cannot be asserted without a person — a window, a shell, and a hand at
 *   the keyboard — and it is as thin as that division can make it.
 *
 * What a caller must know about the bounds.
 *
 *   The grid is a fixed array and the size is chosen within it, so a terminal
 *   asks for the columns and rows it wants and is refused where it asks for
 *   more than this holds. Nothing here allocates, because a C library that
 *   called the heap in a program's drawing path would be a heap in the path of
 *   every keystroke.
 */

#ifndef OXYS_TERM_H
#define OXYS_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The bounds of the grid.
 *
 * Since 2026-09-23 a terminal made full is given the whole screen below the
 * panel, and VirtualBox's 640 by 480 drawn at the face's own size is then
 * seventy-nine columns and forty-three rows — past the forty this was. The
 * rows are sixty-four, which a screen of 1280 by 800 drawn at scale one would
 * fill with room over. The columns are bounded by something else: a row is
 * drawn with one `window_text`, which carries SYSCALL_WINDOW_TEXT_MAXIMUM
 * characters, so a row longer than that would be refused and never drawn, and
 * a hundred and twenty stays below it. A caller asking for more is refused
 * rather than quietly given fewer — a terminal that thought it had more columns
 * than it has would wrap its lines in the wrong places for ever.
 */
#define TERM_COLUMNS_MAXIMUM 120U
#define TERM_ROWS_MAXIMUM    64U

/*
 * One grid.
 *
 * `cell` holds one character per position and is padded with spaces, so that a
 * row is a string a caller may draw in one call rather than a run of characters
 * with holes in it. `changed` records the rows that have been written to since
 * the caller last said it had drawn them: a terminal redraws what changed and
 * not the screen, because each row drawn is a system call and a screen of forty
 * rows redrawn for one keystroke is forty.
 *
 * Concurrency. None. It is a program's own memory, touched by that program
 * alone, and this system has no threads within a process.
 */
typedef struct TermScreen
{
    char cell[TERM_ROWS_MAXIMUM][TERM_COLUMNS_MAXIMUM + 1U];
    bool changed[TERM_ROWS_MAXIMUM];
    uint32_t columns;
    uint32_t rows;
    uint32_t cursor_column;
    uint32_t cursor_row;
    uint64_t scrolls;
    uint64_t written;
} TermScreen;

/*
 * Prepares a grid of the size given, filled with spaces and with the cursor at
 * the top left. Every row is marked changed, a grid nothing has drawn yet being
 * a grid every row of which is owed. Refuses a size of zero or one beyond the
 * bounds above and leaves the grid untouched.
 */
bool TermInitialise(TermScreen *screen, uint32_t columns, uint32_t rows);

/*
 * Gives the grid a new size, of 2026-09-23, keeping its text: each row cut or
 * padded with spaces to the new width, and, where there are fewer rows, the
 * rows dropped from the top so that the cursor's row stays upon the grid —
 * the line a person is typing is the line that must survive. Every row is
 * marked changed. Refuses the sizes TermInitialise refuses, changing nothing.
 */
bool TermResize(TermScreen *screen, uint32_t columns, uint32_t rows);

/* Writes one byte, and a run of them. */
void TermWriteByte(TermScreen *screen, char byte);
void TermWrite(TermScreen *screen, const char *bytes, size_t count);

/*
 * The text of a row, as a terminated string of exactly `columns` characters.
 * Null where the row is beyond the grid.
 */
const char *TermRow(const TermScreen *screen, uint32_t row);

/* Whether a row has been written to since it was last said to be drawn, and the
 * saying. A caller draws the rows for which the first is true and calls the
 * second upon each; nothing here draws, and nothing here knows how. */
bool TermRowChanged(const TermScreen *screen, uint32_t row);
void TermRowDrawn(TermScreen *screen, uint32_t row);

/* The shape, the cursor, and what has happened. */
uint32_t TermColumns(const TermScreen *screen);
uint32_t TermRows(const TermScreen *screen);
uint32_t TermCursorColumn(const TermScreen *screen);
uint32_t TermCursorRow(const TermScreen *screen);
uint64_t TermScrolls(const TermScreen *screen);
uint64_t TermBytesWritten(const TermScreen *screen);

/*
 * The modifiers a key is held with, as the window event carries them. They are
 * the keyboard driver's bits — KEYBOARD_MODIFIER_* of
 * kernel/include/oxys/dev/keyboard.h — named again here because a program under
 * this licence may not include the kernel's headers, and asserted against the
 * kernel's by the self-test that drives both.
 */
#define TERM_MODIFIER_SHIFT      UINT8_C(0x01)
#define TERM_MODIFIER_CONTROL    UINT8_C(0x02)
#define TERM_MODIFIER_ALT        UINT8_C(0x04)
#define TERM_MODIFIER_CAPS_LOCK  UINT8_C(0x08)

/* The longest run of bytes one key becomes: CSI 3 ~ is four. */
#define TERM_KEY_BYTES_MAXIMUM 4U

/*
 * The bytes a key contributes to what the program beneath the terminal reads,
 * written into `bytes` and counted by the return. Zero for a key that sends
 * nothing — a shift, a function key — which is not the same as a key that sends
 * a zero byte.
 *
 * `character` is what the key produces under the modifiers held, or zero where
 * it produces none; `scancode` and `extended` are the key itself, for the seven
 * that have a control sequence rather than a character.
 */
size_t TermKeyBytes(char character, uint8_t scancode, uint8_t modifiers, bool extended,
                    char *bytes, size_t capacity);

/* The control sequence an extended scancode sends, or null where it sends
 * none. Exposed so that the self-test can compare this table with the kernel's
 * rather than only its effect. */
const char *TermSequenceFor(uint8_t scancode);

#endif /* OXYS_TERM_H */
