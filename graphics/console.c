/*
 * File: graphics/console.c
 * Purpose: Implements the graphical console: a grid of character cells drawn
 *          upon the framebuffer, with the four control characters, a scroll
 *          performed by blitting the surface upon itself, and a buffer that
 *          replays what was written before the console existed.
 * Key functions: ConsoleInitialise, ConsoleWriteCharacter, ConsoleWriteString,
 *          ConsoleSetColour, ConsoleSetEraseLimit, ConsoleSuspend, ConsoleReport.
 * References:
 *   - ANSI X3.4-1986: LF, CR, HT and BS, and the meaning each is given.
 *   - docs/devices/DISPLAY.md, Sections 6 and 7: the same four characters as the
 *     text-mode driver implements them, and the erase limit.
 *   - docs/design/CONSOLE.md, Sections 2 and 3.
 *
 * Concurrency. There is no lock, and there is no second thread of control that
 * writes here: the interrupt handlers do not print, save through the panic path,
 * which does not return. From sub-task 6.14 that ceases to be true and this must
 * take the lock sub-task 6.13 built — the whole of a character, not one pixel of
 * it, being the thing that must not interleave.
 */

#include <oxys/console.h>
#include <oxys/compositor.h>
#include <oxys/font.h>
#include <oxys/graphics.h>
#include <oxys/framebuffer.h>
#include <oxys/kernel.h>

/* The control characters, named rather than written as numbers. */
#define CONSOLE_BACKSPACE      0x08
#define CONSOLE_TAB            0x09
#define CONSOLE_LINE_FEED      0x0A
#define CONSOLE_CARRIAGE_RETURN 0x0D

/* The tabulation interval, in columns. HT advances to the next multiple of
 * this, not by this; see ConsoleWriteCharacter. */
#define CONSOLE_TAB_INTERVAL 8U

static GraphicsSurface ConsoleSurface;
static bool ConsoleActive;

/* Whether the surface above is the compositor's back buffer rather than the
 * framebuffer itself, and therefore whether what is drawn must be reported as
 * changed before anybody will see it. */
static bool ConsoleComposited;
static bool ConsoleSuspended;

static uint32_t ConsoleColumnCount;
static uint32_t ConsoleRowCount;
static uint32_t ConsoleCursorColumn;
static uint32_t ConsoleCursorRow;

static uint32_t ConsoleForeground;
static uint32_t ConsoleBackground;

static uint32_t ConsoleLimitColumn;
static uint32_t ConsoleLimitRow;

static uint64_t ConsoleCharactersWritten;
static uint64_t ConsoleScrollCount;

/*
 * Where each row's text ended when the cursor last left it downward.
 *
 * A backspace standing in the first column of a row consumes the separator
 * between that row and the one above, and the cursor belongs after the text of
 * the row above — not at the right-hand edge of the display, which is where a
 * console with no such record has to put it. The text-mode driver answers the
 * same question by reading the characters back out of text memory; a console
 * drawn upon a framebuffer has no characters to read back, only pixels, so what
 * text memory would have told it is recorded as it goes.
 *
 * It is written when a row is left and read when a row is re-entered from below,
 * so it cannot go stale: a row whose text was shortened by an erasure is left
 * again before it can be re-entered again, and the leaving rewrites the entry.
 *
 * A row filled to its last column is the exception, and is why the value stored
 * may be the column count itself. Such a row did not end because a line feed was
 * written but because the text wrapped, and there is no separator between it and
 * the row below to consume; the cursor therefore stops upon the final character,
 * which the same backspace goes on to erase.
 */
static uint32_t ConsoleRowEnd[CONSOLE_MAXIMUM_ROWS];

/*
 * What was written before the console existed, and how much did not fit.
 *
 * The count of dropped bytes is kept and reported rather than the overflow being
 * ignored, because a replay that silently began part way through would look
 * exactly like a boot that began part way through.
 */
static char ConsoleEarlyBuffer[CONSOLE_EARLY_CAPACITY];
static size_t ConsoleEarlyLength;
static size_t ConsoleEarlyDropped;

/*
 * Moves every row up by one and clears the row exposed at the bottom.
 *
 * This is one blit of the surface upon itself, and it is the overlapping case
 * that sub-task 6.3 chose a copy direction for: the destination lies above the
 * source, so the rows are taken from the top and nothing reads a byte the copy
 * has already overwritten. A console scroll is the reason that was implemented,
 * and this is the whole of the payment.
 */
static void ConsoleScroll(void)
{
    const GraphicsRectangle body = { 0, (int32_t)FONT_HEIGHT,
                                     (int32_t)(ConsoleColumnCount * FONT_WIDTH),
                                     (int32_t)((ConsoleRowCount - 1U) * FONT_HEIGHT) };
    const GraphicsRectangle last = { 0, (int32_t)((ConsoleRowCount - 1U) * FONT_HEIGHT),
                                     (int32_t)(ConsoleColumnCount * FONT_WIDTH),
                                     (int32_t)FONT_HEIGHT };

    (void)GraphicsBlit(&ConsoleSurface, 0, 0, &ConsoleSurface, body);
    GraphicsFillRectangle(&ConsoleSurface, last, ConsoleBackground);

    /*
     * The erase limit moves up with the text it marks. A limit left where it was
     * would come to mark a different character after a scroll, and a backspace
     * would then be permitted to erase output it was meant to protect — or
     * refused where it should have been allowed.
     */
    if (ConsoleLimitRow > 0U)
    {
        --ConsoleLimitRow;
    }
    else
    {
        ConsoleLimitColumn = 0U;
    }

    /* The row lengths move with the rows they describe, for the same reason. */
    for (uint32_t row = 0U; (row + 1U) < ConsoleRowCount; ++row)
    {
        ConsoleRowEnd[row] = ConsoleRowEnd[row + 1U];
    }

    ConsoleRowEnd[ConsoleRowCount - 1U] = 0U;

    /*
     * A scroll moves every row, so every row must be carried out. This is the
     * one operation whose damage cannot be narrowed, and it is also the one this
     * sub-task made cheap: the blit now reads the back buffer, which is ordinary
     * cached memory, where before it read the framebuffer through a mapping in
     * which reads are uncached.
     */
    if (ConsoleComposited)
    {
        CompositorInvalidateAll();
    }

    ++ConsoleScrollCount;
}

/* Advances to the first column of the following row, scrolling upon the last. */
static void ConsoleNewLine(void)
{
    /*
     * Recorded before the column is reset, and recorded here rather than in the
     * two callers because this is the one place a row is left downward: an
     * explicit line feed arrives with the cursor after the text, and a wrap
     * arrives with it at the column count, which is the distinction a backspace
     * crossing back up needs to make.
     */
    ConsoleRowEnd[ConsoleCursorRow] = ConsoleCursorColumn;

    ConsoleCursorColumn = 0U;

    if ((ConsoleCursorRow + 1U) < ConsoleRowCount)
    {
        ++ConsoleCursorRow;
        return;
    }

    ConsoleScroll();
}

/* Advances one cell, wrapping to the next row at the right-hand edge. */
static void ConsoleAdvance(void)
{
    ++ConsoleCursorColumn;

    if (ConsoleCursorColumn >= ConsoleColumnCount)
    {
        ConsoleNewLine();
    }
}

bool ConsoleInitialise(void)
{
    /*
     * The back buffer where there is one, and the framebuffer where there is
     * not.
     *
     * A surface owns nothing and describes memory somebody else supplied, which
     * is exactly what allows this substitution to be one line: every primitive
     * below, every glyph and the scroll itself go wherever this points without
     * knowing which they got. That was the argument for the abstraction in
     * sub-task 6.3 and this is the sub-task that collects on it.
     */
    if (CompositorIsActive())
    {
        ConsoleSurface = *CompositorSurface();
        ConsoleComposited = true;
    }
    else if (GraphicsSurfaceFromFramebuffer(&ConsoleSurface))
    {
        ConsoleComposited = false;
    }
    else
    {
        return false;
    }

    ConsoleColumnCount = ConsoleSurface.width / FONT_WIDTH;
    ConsoleRowCount = ConsoleSurface.height / FONT_HEIGHT;

    /*
     * A framebuffer taller than the row lengths can describe is used as far as
     * they reach. Nothing hands this kernel a display of four thousand pixels,
     * and a console short of the foot of such a display is a better failure than
     * a backspace that reads a row length beyond the end of the record.
     */
    if (ConsoleRowCount > CONSOLE_MAXIMUM_ROWS)
    {
        ConsoleRowCount = CONSOLE_MAXIMUM_ROWS;
    }

    /*
     * A framebuffer smaller than one cell is refused rather than divided by.
     * Nothing produces one, and a console of zero columns would divide by zero
     * at the first tabulation.
     */
    if ((ConsoleColumnCount == 0U) || (ConsoleRowCount == 0U))
    {
        return false;
    }

    ConsoleForeground = FramebufferEncode(200U, 200U, 200U);
    ConsoleBackground = FramebufferEncode(0U, 0U, 0U);

    ConsoleCursorColumn = 0U;
    ConsoleCursorRow = 0U;
    ConsoleLimitColumn = 0U;
    ConsoleLimitRow = 0U;
    ConsoleCharactersWritten = 0U;
    ConsoleScrollCount = 0U;

    for (uint32_t row = 0U; row < ConsoleRowCount; ++row)
    {
        ConsoleRowEnd[row] = 0U;
    }

    GraphicsResetClip(&ConsoleSurface);
    GraphicsClear(&ConsoleSurface, ConsoleBackground);

    ConsoleActive = true;

    /*
     * Replay what was written before this point.
     *
     * This cannot re-enter the buffer, and the reason is the order of the two
     * statements above rather than a flag: ConsoleActive was set true before the
     * loop, and ConsoleWriteCharacter records into the buffer only while it is
     * false. Every character below therefore takes the drawing path, and nothing
     * is ever appended to the buffer again.
     */
    for (size_t index = 0U; index < ConsoleEarlyLength; ++index)
    {
        ConsoleWriteCharacter(ConsoleEarlyBuffer[index]);
    }

    return true;
}

bool ConsoleIsActive(void)
{
    return ConsoleActive;
}

void ConsoleSetColour(uint32_t foreground, uint32_t background)
{
    ConsoleForeground = foreground;
    ConsoleBackground = background;
}

uint32_t ConsoleColumns(void)
{
    return ConsoleActive ? ConsoleColumnCount : 0U;
}

uint32_t ConsoleRows(void)
{
    return ConsoleActive ? ConsoleRowCount : 0U;
}

uint32_t ConsoleColumn(void)
{
    return ConsoleCursorColumn;
}

uint32_t ConsoleRow(void)
{
    return ConsoleCursorRow;
}

void ConsoleSetEraseLimit(void)
{
    ConsoleLimitColumn = ConsoleCursorColumn;
    ConsoleLimitRow = ConsoleCursorRow;
}

void ConsoleSuspend(void)
{
    ConsoleSuspended = true;
}

/*
 * The position of a cell counted from the first cell of the first row, by which
 * two positions are compared in one operation rather than by a pair of tests
 * that must agree.
 */
static uint32_t ConsolePosition(uint32_t row, uint32_t column)
{
    return (row * ConsoleColumnCount) + column;
}

/*
 * Retreats the cursor by one position, crossing into the row above where it
 * stands in the first column.
 *
 * Crossing up lands after the text of the row above, which is where the next
 * character written upon that row would go — not at the right-hand edge of the
 * display. The edge is where this went before the row lengths were recorded, and
 * it was wrong in a way that made backspacing over a line separator useless: the
 * erasure the caller composes was written a hundred and fifty columns away from
 * the text, so a person saw the characters they were trying to delete stay
 * exactly where they were.
 *
 * The exception is a row filled to its last column. Such a row did not end
 * because a line feed was written but because the text wrapped, and there is no
 * separator to consume; the cursor stops upon the final character, which the
 * same backspace goes on to erase.
 *
 * The movement stops at the erase limit. Without one the console has no way to
 * tell a character the user typed from a character the kernel printed, and a
 * backspace crossing a row boundary would consume the boot log a character at a
 * time.
 */
static void ConsoleBackspace(void)
{
    const uint32_t limit = ConsolePosition(ConsoleLimitRow, ConsoleLimitColumn);

    if (ConsolePosition(ConsoleCursorRow, ConsoleCursorColumn) <= limit)
    {
        return;
    }

    if (ConsoleCursorColumn > 0U)
    {
        --ConsoleCursorColumn;
    }
    else if (ConsoleCursorRow > 0U)
    {
        const uint32_t end = ConsoleRowEnd[ConsoleCursorRow - 1U];

        --ConsoleCursorRow;
        ConsoleCursorColumn = (end < ConsoleColumnCount) ? end : (ConsoleColumnCount - 1U);
    }
    else
    {
        /* The first column of the first row; there is nowhere above to go. */
        return;
    }

    /*
     * A row above whose text ends before the limit would otherwise have carried
     * the cursor past it, the limit standing in the middle of a row that a
     * prompt shares with the input that follows it.
     */
    if (ConsolePosition(ConsoleCursorRow, ConsoleCursorColumn) < limit)
    {
        ConsoleCursorRow = ConsoleLimitRow;
        ConsoleCursorColumn = ConsoleLimitColumn;
    }
}

void ConsoleWriteCharacter(char character)
{
    const uint8_t code = (uint8_t)character;

    /*
     * Tested before the early buffer and before anything else, so that a
     * suspended console neither draws nor records. Recording would be worse than
     * useless: the buffer exists to be replayed upon a console that is about to
     * start, and this one never will.
     */
    if (ConsoleSuspended)
    {
        return;
    }

    /*
     * Before the console exists, everything is recorded for replay. The
     * framebuffer cannot be mapped until the arena exists, and by then the boot
     * log has begun; without this the screen would start part way through it.
     */
    if (!ConsoleActive)
    {
        if (ConsoleEarlyLength < CONSOLE_EARLY_CAPACITY)
        {
            ConsoleEarlyBuffer[ConsoleEarlyLength] = character;
            ++ConsoleEarlyLength;
        }
        else
        {
            ++ConsoleEarlyDropped;
        }

        return;
    }

    switch (code)
    {
    case CONSOLE_LINE_FEED:
        ConsoleNewLine();
        return;

    case CONSOLE_CARRIAGE_RETURN:
        ConsoleCursorColumn = 0U;
        return;

    case CONSOLE_TAB:
        /*
         * To the next multiple of eight columns, not onward by eight. The
         * distinction is the whole of what a tabulation is for: columns of text
         * separated by tabulations line up only if every one of them lands upon
         * the same grid, whatever the length of what preceded it.
         */
        {
            uint32_t next = ((ConsoleCursorColumn / CONSOLE_TAB_INTERVAL) + 1U) *
                            CONSOLE_TAB_INTERVAL;

            if (next >= ConsoleColumnCount)
            {
                ConsoleNewLine();
            }
            else
            {
                ConsoleCursorColumn = next;
            }
        }
        return;

    case CONSOLE_BACKSPACE:
        /*
         * One position backward, and it does not erase — the caller composes an
         * erasure from backspace, space, backspace, exactly as it must upon the
         * text-mode display and upon a serial terminal. The limit is what keeps
         * an echo loop from retreating over a prompt.
         */
        ConsoleBackspace();
        return;

    default:
        break;
    }

    /*
     * Everything else is drawn, the font substituting its replacement glyph for
     * any code it does not cover. A control character nothing meant to emit
     * therefore appears as a hollow box rather than vanishing, which is the
     * behaviour that makes such a character findable.
     *
     * The cell and its glyph are drawn together, in one pass. Filling the cell
     * and then drawing the glyph over it was the first arrangement and was
     * replaced: it wrote every pixel of the cell twice and clipped each of them
     * separately, and it was the greater part of what a character cost. See
     * docs/design/CONSOLE.md, Section 6.
     */
    FontDrawGlyphOpaque(&ConsoleSurface, (int32_t)(ConsoleCursorColumn * FONT_WIDTH),
                        (int32_t)(ConsoleCursorRow * FONT_HEIGHT), code, ConsoleForeground,
                        ConsoleBackground);

    if (ConsoleComposited)
    {
        const GraphicsRectangle cell = { (int32_t)(ConsoleCursorColumn * FONT_WIDTH),
                                         (int32_t)(ConsoleCursorRow * FONT_HEIGHT),
                                         FONT_WIDTH, FONT_HEIGHT };

        CompositorInvalidate(cell);
    }

    ++ConsoleCharactersWritten;
    ConsoleAdvance();
}

void ConsoleWriteString(const char *string)
{
    if (string == NULL)
    {
        return;
    }

    for (size_t index = 0U; string[index] != '\0'; ++index)
    {
        ConsoleWriteCharacter(string[index]);
    }
}

void ConsoleReport(void)
{
    if (!ConsoleActive)
    {
        /*
         * Two different things bring us here and the report must say which.
         *
         * There may be no framebuffer of pixels — the boot loader left the
         * adapter in a text mode, and the driver of sub-task 4.2 is displaying
         * the console. Or there is one and this was deliberately not started,
         * because the command line asked for the figures of sub-tasks 6.2 and
         * 6.3 and they cannot share the screen with a boot log.
         *
         * Reporting the first in both cases would have the kernel state that it
         * has no framebuffer upon a boot where it had just described one, three
         * lines earlier, in detail.
         */
        KernelWriteString(FramebufferIsGraphical()
                              ? "Console: not started; the command line asked for the "
                                "drawing figures, which own the screen instead.\n"
                              : "Console: none; the adapter is in a text mode, which the "
                                "display driver owns.\n");
        return;
    }

    KernelWriteString("Console: ");
    KernelWriteDecimal((uint64_t)ConsoleColumnCount);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)ConsoleRowCount);
    KernelWriteString(" characters of ");
    KernelWriteDecimal((uint64_t)FONT_WIDTH);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)FONT_HEIGHT);
    KernelWriteString(" pixels.\n");

    KernelWriteString("Console: written ");
    KernelWriteDecimal(ConsoleCharactersWritten);
    KernelWriteString(", scrolled ");
    KernelWriteDecimal(ConsoleScrollCount);
    KernelWriteString(", cursor at row ");
    KernelWriteDecimal((uint64_t)ConsoleCursorRow);
    KernelWriteString(", column ");
    KernelWriteDecimal((uint64_t)ConsoleCursorColumn);
    KernelWriteString(".\n");

    KernelWriteString("Console: ");
    KernelWriteDecimal((uint64_t)ConsoleEarlyLength);
    KernelWriteString(" bytes replayed from before the console existed");

    if (ConsoleEarlyDropped != 0U)
    {
        KernelWriteString(", and ");
        KernelWriteDecimal((uint64_t)ConsoleEarlyDropped);
        KernelWriteString(" dropped for want of room");
    }

    KernelWriteString(".\n");
}
