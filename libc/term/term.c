/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/term/term.c
 * Purpose: The character grid of sub-task 9.6 — what a terminal emulator's
 *          display holds and where its cursor stands, given the bytes a program
 *          writes to it. It draws nothing and reads nothing.
 * Key functions: TermInitialise, TermResize, TermWriteByte, TermWrite, TermRow,
 *          TermRowChanged, TermRowDrawn, TermScroll.
 * References:
 *   - libc/include/term.h: what this is, and why it is here rather than in
 *     /bin/terminal.
 *   - docs/design/TERMINAL.md, Section 3: the five control characters acted
 *     upon, and what is deliberately not.
 *   - docs/design/CONSOLE.md, Section 7: the kernel console's backspace, which
 *     crosses to the row above; this does the same, because the line editor of
 *     sub-task 8.1 was written against that console and erases a line by
 *     backspacing over it.
 *
 * Concurrency. None; see the header.
 */

#include <term.h>

/* Marks a row as owed to whoever draws. Out of range is ignored rather than
 * refused: the movements below clamp, and a mark that had to be checked twice
 * would be a check in every one of them. */
static void TermTouch(TermScreen *screen, uint32_t row)
{
    if (row < screen->rows)
    {
        screen->changed[row] = true;
    }
}

/* Fills one row with spaces and terminates it. */
static void TermClearRow(TermScreen *screen, uint32_t row)
{
    for (uint32_t column = 0U; column < screen->columns; ++column)
    {
        screen->cell[row][column] = ' ';
    }

    screen->cell[row][screen->columns] = '\0';
    TermTouch(screen, row);
}

/*
 * Moves every row up by one and empties the last.
 *
 * Every row is marked changed, and that is not laziness: a scroll moves the
 * text of the whole grid, so every row's content differs from what was drawn
 * there. A terminal that marked only the last would draw a screen one line out
 * of date and stay that way until each row happened to be written to.
 */
static void TermScroll(TermScreen *screen)
{
    for (uint32_t row = 1U; row < screen->rows; ++row)
    {
        for (uint32_t column = 0U; column <= screen->columns; ++column)
        {
            screen->cell[row - 1U][column] = screen->cell[row][column];
        }

        TermTouch(screen, row - 1U);
    }

    TermClearRow(screen, screen->rows - 1U);
    ++screen->scrolls;
}

/* The cursor to the start of the next row, scrolling where it was upon the
 * last. The column is not changed, which is what separates this from a line
 * feed: the callers below decide. */
static void TermDown(TermScreen *screen)
{
    if ((screen->cursor_row + 1U) < screen->rows)
    {
        ++screen->cursor_row;

        return;
    }

    TermScroll(screen);
}

bool TermInitialise(TermScreen *screen, uint32_t columns, uint32_t rows)
{
    if ((screen == NULL) || (columns == 0U) || (rows == 0U) ||
        (columns > TERM_COLUMNS_MAXIMUM) || (rows > TERM_ROWS_MAXIMUM))
    {
        return false;
    }

    screen->columns = columns;
    screen->rows = rows;
    screen->cursor_column = 0U;
    screen->cursor_row = 0U;
    screen->scrolls = 0U;
    screen->written = 0U;

    for (uint32_t row = 0U; row < rows; ++row)
    {
        TermClearRow(screen, row);
    }

    return true;
}

bool TermResize(TermScreen *screen, uint32_t columns, uint32_t rows)
{
    uint32_t dropped;

    if ((screen == NULL) || (columns == 0U) || (rows == 0U) ||
        (columns > TERM_COLUMNS_MAXIMUM) || (rows > TERM_ROWS_MAXIMUM))
    {
        return false;
    }

    /* The rows above the cursor's that no longer fit, taken from the top. */
    dropped = ((screen->cursor_row + 1U) > rows) ? ((screen->cursor_row + 1U) - rows) : 0U;

    /*
     * In place, top to bottom: row `row` is written from row `row + dropped`,
     * which is never above it, so every row is read before it is overwritten.
     * A cell beyond the old grid in either direction is a space.
     */
    for (uint32_t row = 0U; row < rows; ++row)
    {
        const uint32_t source = row + dropped;

        for (uint32_t column = 0U; column < columns; ++column)
        {
            screen->cell[row][column] = ((source < screen->rows) && (column < screen->columns))
                                            ? screen->cell[source][column]
                                            : ' ';
        }

        screen->cell[row][columns] = '\0';
        screen->changed[row] = true;
    }

    screen->columns = columns;
    screen->rows = rows;
    screen->cursor_row -= dropped;

    if (screen->cursor_column >= columns)
    {
        screen->cursor_column = columns - 1U;
    }

    return true;
}

void TermWriteByte(TermScreen *screen, char byte)
{
    if ((screen == NULL) || (screen->columns == 0U))
    {
        return;
    }

    ++screen->written;

    switch (byte)
    {
    case '\r':
        /* The cursor to the left margin, and nothing erased. */
        screen->cursor_column = 0U;
        TermTouch(screen, screen->cursor_row);

        return;

    case '\n':
        /*
         * Down **and** to the left margin, which is what the kernel's console
         * does with a line feed and therefore what every program in this system
         * has been written against. A line feed that moved down alone would put
         * the second line of every message under the end of the first.
         */
        TermTouch(screen, screen->cursor_row);
        screen->cursor_column = 0U;
        TermDown(screen);
        TermTouch(screen, screen->cursor_row);

        return;

    case '\b':
        /*
         * One position left, crossing to the end of the row above where it
         * stands at the left margin. The crossing is what the line editor
         * needs: it erases by writing a space and backspacing, and a line it
         * had wrapped could not otherwise be erased. It stops at the top left,
         * there being nothing above it to cross into.
         */
        if (screen->cursor_column > 0U)
        {
            --screen->cursor_column;
        }
        else if (screen->cursor_row > 0U)
        {
            --screen->cursor_row;
            screen->cursor_column = screen->columns - 1U;
        }

        TermTouch(screen, screen->cursor_row);

        return;

    case '\f':
        /*
         * A new page: every row blanked and the cursor to the top left — what
         * the kernel's console and its VGA text driver do with a form feed,
         * and what the shell's `clear` writes. Until 2026-09-24 the grid
         * dropped it with every other control byte, so `clear` in a terminal
         * window did nothing and said nothing. Every row is marked changed by
         * the clearing, which is what makes the emulator draw the empty page.
         */
        for (uint32_t row = 0U; row < screen->rows; ++row)
        {
            TermClearRow(screen, row);
        }

        screen->cursor_column = 0U;
        screen->cursor_row = 0U;

        return;

    case '\t':
        /* To the next multiple of eight, and no further than the last column.
         * Nothing in this system writes a tab to a terminal; it is acted upon
         * because a tab left to fall through would be drawn as a hole. */
        do
        {
            screen->cell[screen->cursor_row][screen->cursor_column] = ' ';
            ++screen->cursor_column;
        } while ((screen->cursor_column < screen->columns) &&
                 ((screen->cursor_column % 8U) != 0U));

        TermTouch(screen, screen->cursor_row);

        if (screen->cursor_column >= screen->columns)
        {
            screen->cursor_column = 0U;
            TermDown(screen);
            TermTouch(screen, screen->cursor_row);
        }

        return;

    default:
        break;
    }

    /*
     * Everything else that is not a printable character is dropped. A control
     * byte drawn as a glyph is a screen a person cannot read, and this grid has
     * no escape sequences to be in the middle of — TERMINAL.md, Section 3,
     * records what that costs and what would be needed to pay it.
     */
    if ((byte < 0x20) || (byte == 0x7F))
    {
        return;
    }

    screen->cell[screen->cursor_row][screen->cursor_column] = byte;
    TermTouch(screen, screen->cursor_row);
    ++screen->cursor_column;

    /* The wrap is at the moment of writing past the edge and not at the moment
     * of reaching it, so that a line exactly as wide as the grid leaves the
     * cursor upon its own last column and a backspace still lands in it. */
    if (screen->cursor_column >= screen->columns)
    {
        screen->cursor_column = 0U;
        TermDown(screen);
        TermTouch(screen, screen->cursor_row);
    }
}

void TermWrite(TermScreen *screen, const char *bytes, size_t count)
{
    if ((screen == NULL) || (bytes == NULL))
    {
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        TermWriteByte(screen, bytes[index]);
    }
}

const char *TermRow(const TermScreen *screen, uint32_t row)
{
    if ((screen == NULL) || (row >= screen->rows))
    {
        return NULL;
    }

    return screen->cell[row];
}

bool TermRowChanged(const TermScreen *screen, uint32_t row)
{
    return (screen != NULL) && (row < screen->rows) && screen->changed[row];
}

void TermRowDrawn(TermScreen *screen, uint32_t row)
{
    if ((screen != NULL) && (row < screen->rows))
    {
        screen->changed[row] = false;
    }
}

uint32_t TermColumns(const TermScreen *screen)
{
    return (screen == NULL) ? 0U : screen->columns;
}

uint32_t TermRows(const TermScreen *screen)
{
    return (screen == NULL) ? 0U : screen->rows;
}

uint32_t TermCursorColumn(const TermScreen *screen)
{
    return (screen == NULL) ? 0U : screen->cursor_column;
}

uint32_t TermCursorRow(const TermScreen *screen)
{
    return (screen == NULL) ? 0U : screen->cursor_row;
}

uint64_t TermScrolls(const TermScreen *screen)
{
    return (screen == NULL) ? 0U : screen->scrolls;
}

uint64_t TermBytesWritten(const TermScreen *screen)
{
    return (screen == NULL) ? 0U : screen->written;
}
