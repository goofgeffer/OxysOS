/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/view/main.c
 * Purpose: The text viewer of sub-task 9.7: a window showing one file's text,
 *          wrapped to the window's width, scrolled by the keys, and wrapped
 *          again whenever the window is given a new extent.
 * Key functions: main, ViewLoad, ViewWrap, ViewDrawRow, ViewDraw, ViewPaint,
 *          ViewHandleKey.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the window calls, `window_text`, and the
 *     resize event a window made full is sent.
 *   - docs/design/UTILITIES.md: the design, and what only looking
 *     establishes.
 *
 * What it shows, and what it does not.
 *
 *   The bytes of the file, as text: a tab to the next multiple of eight
 *   columns, a line feed ending a row, a carriage return nothing — so that a
 *   file written upon another system shows its lines and not a column of dots
 *   down the right — and every other byte outside printable ASCII as a full
 *   stop. It is a viewer and not an editor: `micro` edits, at the shell.
 *
 * Why it wraps rather than cutting long lines.
 *
 *   A line cut at the window's edge is a line whose end a person cannot see
 *   without a horizontal scroll, which this does not have; a wrapped line is all
 *   there. The wrap is recomputed upon a resize, which is the whole of what
 *   "adjusting to a window made full" means for text.
 */

#include <palette.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/*
 * The most of a file shown. A quarter of a megabyte is every text file this
 * system carries many times over; a larger file is shown to that much and says
 * so upon its last row, rather than being refused — the start of a large file
 * being more use than nothing.
 */
#define VIEW_BYTES_MAXIMUM (256U * 1024U)

/* The face is eight pixels square, with two of leading between rows, and every
 * extent here is multiplied by the scale. */
#define VIEW_GLYPH   8
#define VIEW_PITCH   10

/* The most characters one `window_text` carries, less a margin. A row wider is
 * drawn in pieces. */
#define VIEW_CHUNK 120

/* The widest row drawn, in characters: a screen of 4096 pixels at scale one. */
#define VIEW_COLUMNS_MAXIMUM 512

#define VIEW_PAPER OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define VIEW_INK   OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)

static char ViewText[VIEW_BYTES_MAXIMUM];
static size_t ViewLength;
static bool ViewTruncated;

/* Where each display row begins in the text, and one past the last. A file of
 * nothing but line feeds is a row per byte and one after the last, and the
 * table ends with the text's length: two more than the text. */
static uint32_t ViewRowStart[VIEW_BYTES_MAXIMUM + 2U];
static size_t ViewRowCount;

static const char *ViewPath;
static int64_t ViewWindow = -1;
static int32_t ViewScale = 2;
static int32_t ViewWidth;
static int32_t ViewHeight;
static int32_t ViewColumns;

/* The rows of text the window shows, the last row of the window being the
 * status. And the first of them, as an index into ViewRowStart. */
static int32_t ViewTextRows;
static size_t ViewTop;

/* ------------------------------------------------------------- the text */

/* Reads the file whole. False, with a message, where it cannot be opened or
 * read. */
static bool ViewLoad(const char *path)
{
    const int64_t descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        (void)fprintf(stderr, "view: %s: %s\n", path, strerror(errno));

        return false;
    }

    ViewLength = 0U;

    for (;;)
    {
        const int64_t taken = OxysRead((int)descriptor, &ViewText[ViewLength],
                                       VIEW_BYTES_MAXIMUM - ViewLength);

        if (taken < 0)
        {
            (void)fprintf(stderr, "view: %s: %s\n", path, strerror(errno));
            (void)OxysClose((int)descriptor);

            return false;
        }

        if (taken == 0)
        {
            break;
        }

        ViewLength += (size_t)taken;

        /* Full: a byte more would say whether it was cut. */
        if (ViewLength == VIEW_BYTES_MAXIMUM)
        {
            char probe;

            ViewTruncated = OxysRead((int)descriptor, &probe, 1U) > 0;
            break;
        }
    }

    (void)OxysClose((int)descriptor);

    return true;
}

/* How many columns a byte advances the column `column` by. */
static int32_t ViewAdvance(char byte, int32_t column)
{
    if (byte == '\t')
    {
        return 8 - (column % 8);
    }

    return (byte == '\r') ? 0 : 1;
}

/*
 * Finds where each display row begins, for the present width. A row ends at a
 * line feed, which it does not show, or where the next byte would pass the last
 * column. A tab that would cross the edge begins the next row rather than being
 * split across two, a tab being one byte.
 */
static void ViewWrap(void)
{
    int32_t column = 0;

    ViewRowCount = 0U;
    ViewRowStart[ViewRowCount++] = 0U;

    for (size_t at = 0U; at < ViewLength; ++at)
    {
        const char byte = ViewText[at];
        int32_t advance;

        if (byte == '\n')
        {
            ViewRowStart[ViewRowCount++] = (uint32_t)(at + 1U);
            column = 0;
            continue;
        }

        advance = ViewAdvance(byte, column);

        if ((column > 0) && ((column + advance) > ViewColumns))
        {
            ViewRowStart[ViewRowCount++] = (uint32_t)at;
            column = 0;
            advance = ViewAdvance(byte, column);
        }

        column += advance;
    }

    /* A file ending in a line feed has no row after it, as a person reads it. */
    if ((ViewRowCount > 1U) && (ViewRowStart[ViewRowCount - 1U] == ViewLength))
    {
        --ViewRowCount;
    }

    ViewRowStart[ViewRowCount] = (uint32_t)ViewLength;
}

/* ------------------------------------------------------------ drawing */

/* Draws `count` characters of `text` at a row, in pieces `window_text` will
 * carry. */
static void ViewDrawText(int32_t row, const char *text, int32_t count, uint32_t ink,
                         uint32_t paper)
{
    char piece[VIEW_CHUNK + 1];

    for (int32_t first = 0; first < count; first += VIEW_CHUNK)
    {
        const int32_t length = ((count - first) < VIEW_CHUNK) ? (count - first) : VIEW_CHUNK;
        SyscallWindowText placement;

        memcpy(piece, &text[first], (size_t)length);
        piece[length] = '\0';

        placement.x = first * VIEW_GLYPH * ViewScale;
        placement.y = row * VIEW_PITCH * ViewScale;
        placement.ink = ink;
        placement.paper = paper;
        placement.scale = ViewScale;

        (void)OxysWindowText(ViewWindow, &placement, piece);
    }
}

/* One display row, padded with spaces to the width so that it covers whatever
 * the row showed before. */
static void ViewDrawRow(int32_t screen_row, size_t row)
{
    char line[VIEW_COLUMNS_MAXIMUM + 1];
    int32_t column = 0;

    if (row < ViewRowCount)
    {
        for (uint32_t at = ViewRowStart[row]; (at < ViewRowStart[row + 1U]) &&
                                              (column < ViewColumns);
             ++at)
        {
            const char byte = ViewText[at];
            const int32_t advance = ViewAdvance(byte, column);

            if (byte == '\n')
            {
                break;
            }

            for (int32_t step = 0; (step < advance) && (column < ViewColumns); ++step)
            {
                line[column++] = ((byte >= 0x20) && (byte < 0x7F)) ? byte : ((byte == '\t') ? ' ' : '.');
            }
        }
    }

    while (column < ViewColumns)
    {
        line[column++] = ' ';
    }

    ViewDrawText(screen_row, line, ViewColumns, VIEW_INK, VIEW_PAPER);
}

/* The status: the file's name, and which rows of how many are shown. */
static void ViewDrawStatus(void)
{
    char line[VIEW_COLUMNS_MAXIMUM + 1];
    const size_t last = ((ViewTop + (size_t)ViewTextRows) < ViewRowCount)
                            ? (ViewTop + (size_t)ViewTextRows)
                            : ViewRowCount;
    int written = snprintf(line, sizeof line, " %s   %lu-%lu of %lu%s", ViewPath,
                           (unsigned long)(ViewTop + 1U), (unsigned long)last,
                           (unsigned long)ViewRowCount,
                           ViewTruncated ? "   (only the first 256 KiB)" : "");

    if (written < 0)
    {
        written = 0;
    }

    for (int32_t column = written; column < ViewColumns; ++column)
    {
        line[column] = ' ';
    }

    ViewDrawText(ViewTextRows, line, ViewColumns, VIEW_PAPER, VIEW_INK);
}

static void ViewDraw(void)
{
    for (int32_t row = 0; row < ViewTextRows; ++row)
    {
        ViewDrawRow(row, ViewTop + (size_t)row);
    }

    ViewDrawStatus();
}

/*
 * Paints the whole content in the paper, the margin past the last whole cell
 * included — there is no fill across the protocol, so it is a band blitted
 * down the window, as the terminal paints its ground.
 */
static void ViewPaint(void)
{
    const int32_t band = VIEW_PITCH * ViewScale;
    const size_t count = (size_t)ViewWidth * (size_t)band;
    uint32_t *const pixels = malloc(count * sizeof *pixels);
    SyscallWindowRectangle area;

    if (pixels == NULL)
    {
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        pixels[index] = VIEW_PAPER;
    }

    area.x = 0;
    area.width = ViewWidth;

    for (int32_t top = 0; top < ViewHeight; top += band)
    {
        area.y = top;
        area.height = ((top + band) <= ViewHeight) ? band : (ViewHeight - top);
        (void)OxysWindowBlit(ViewWindow, &area, pixels);
    }

    free(pixels);
}

/*
 * Takes a new extent: the columns and rows it holds, the text wrapped again,
 * and the first row kept at the same place in the text — the byte at the top
 * stays at the top, so that a person who made the window full is still
 * looking at what they were reading.
 */
static void ViewLayout(int32_t width, int32_t height)
{
    const uint32_t anchor = (ViewTop < ViewRowCount) ? ViewRowStart[ViewTop] : 0U;

    ViewWidth = width;
    ViewHeight = height;
    ViewColumns = width / (VIEW_GLYPH * ViewScale);
    ViewTextRows = (height / (VIEW_PITCH * ViewScale)) - 1;

    ViewColumns = (ViewColumns > VIEW_COLUMNS_MAXIMUM) ? VIEW_COLUMNS_MAXIMUM : ViewColumns;
    ViewColumns = (ViewColumns < 1) ? 1 : ViewColumns;
    ViewTextRows = (ViewTextRows < 1) ? 1 : ViewTextRows;

    ViewWrap();

    ViewTop = 0U;

    while (((ViewTop + 1U) < ViewRowCount) && (ViewRowStart[ViewTop + 1U] <= anchor))
    {
        ++ViewTop;
    }
}

/* ------------------------------------------------------------ the keys */

/* The largest first row: the last page, and not beyond it. */
static size_t ViewLastTop(void)
{
    return (ViewRowCount > (size_t)ViewTextRows) ? (ViewRowCount - (size_t)ViewTextRows) : 0U;
}

/* Acts upon a key; returns whether the view moved. */
static bool ViewHandleKey(const SyscallWindowEvent *event)
{
    const size_t before = ViewTop;
    const size_t page = (ViewTextRows > 1) ? (size_t)(ViewTextRows - 1) : 1U;

    if (event->key_pressed == 0U)
    {
        return false;
    }

    /* The keys of the set 1 scancodes the terminal also reads: the arrows, the
     * page keys, Home and End, all behind the 0xE0 prefix; and the space bar,
     * which pages down as it does in every pager of this lineage. */
    if (event->key_extended != 0U)
    {
        switch (event->key_scancode)
        {
        case 0x48U: ViewTop = (ViewTop > 0U) ? (ViewTop - 1U) : 0U; break;
        case 0x50U: ViewTop += 1U; break;
        case 0x49U: ViewTop = (ViewTop > page) ? (ViewTop - page) : 0U; break;
        case 0x51U: ViewTop += page; break;
        case 0x47U: ViewTop = 0U; break;
        case 0x4FU: ViewTop = ViewLastTop(); break;
        default: break;
        }
    }
    else if (event->key_character == ' ')
    {
        ViewTop += page;
    }

    if (ViewTop > ViewLastTop())
    {
        ViewTop = ViewLastTop();
    }

    return ViewTop != before;
}

/* The last part of a path, which is what the window's title can hold. */
static const char *ViewBaseName(const char *path)
{
    const char *base = path;

    for (const char *at = path; *at != '\0'; ++at)
    {
        if ((*at == '/') && (at[1] != '\0'))
        {
            base = at + 1;
        }
    }

    return base;
}

int main(int argument_count, char *argument_vector[])
{
    SyscallWindowRectangle screen;
    SyscallWindowRectangle geometry;
    bool running = true;

    if (argument_count != 2)
    {
        (void)fprintf(stderr, "usage: view file\n");

        return 2;
    }

    ViewPath = argument_vector[1];

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "view: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    if (!ViewLoad(ViewPath))
    {
        return EXIT_FAILURE;
    }

    /* Sized as the terminal is: the face doubled upon a screen at least 1024
     * wide, and three quarters of the screen's width in whole cells. */
    ViewScale = (screen.width >= 1024) ? 2 : 1;
    geometry.width = ((screen.width * 3 / 4) / (VIEW_GLYPH * ViewScale)) * VIEW_GLYPH * ViewScale;
    geometry.height = ((screen.height * 3 / 5) / (VIEW_PITCH * ViewScale)) * VIEW_PITCH * ViewScale;
    geometry.x = (screen.width - geometry.width) / 3;
    geometry.y = (screen.height - geometry.height) / 3;

    ViewWindow = OxysWindowCreate(&geometry, ViewBaseName(ViewPath), SYSCALL_WINDOW_LAYER_NORMAL);

    if (ViewWindow < 0)
    {
        (void)fprintf(stderr, "view: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    ViewLayout(geometry.width, geometry.height);
    ViewPaint();
    ViewDraw();

    while (running)
    {
        SyscallWindowEvent event;
        bool moved = false;
        bool resized = false;
        int64_t result = OxysWindowEvent(ViewWindow, &event, SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        /* Every event queued is taken before drawing once, so that a held key
         * repeating faster than the window can be drawn costs one drawing. */
        while (result == 1)
        {
            switch (event.kind)
            {
            case SYSCALL_WINDOW_EVENT_KEY:
                moved = ViewHandleKey(&event) || moved;
                break;

            case SYSCALL_WINDOW_EVENT_RESIZE:
                ViewLayout(event.x, event.y);
                resized = true;
                break;

            case SYSCALL_WINDOW_EVENT_CLOSE:
                running = false;
                break;

            default:
                break;
            }

            result = OxysWindowEvent(ViewWindow, &event, 0U);
        }

        if (resized)
        {
            ViewPaint();
        }

        if (running && (moved || resized))
        {
            ViewDraw();
        }
    }

    (void)OxysWindowDestroy(ViewWindow);

    return EXIT_SUCCESS;
}
