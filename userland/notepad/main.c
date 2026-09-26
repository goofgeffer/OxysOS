/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/notepad/main.c
 * Purpose: Notepad: a window in which a text file is written and edited with
 *          the keyboard, and saved — the editor for a person at the desktop, as
 *          `micro` is the editor at the shell. Added on 2026-09-25 at the
 *          project owner's request.
 * Key functions: main, NotepadInsert, NotepadDelete, NotepadMove, NotepadDraw,
 *          NotepadLoad, NotepadSave, NotepadHandleKey.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the window calls, the key event, `notify`.
 *   - libc/include/term.h: TERM_MODIFIER_CONTROL.
 *   - docs/design/UTILITIES.md: the design, and what only looking establishes.
 *
 * The keys. Letters, digits and punctuation are typed; Enter breaks the line;
 * Backspace and Delete remove; Tab types four spaces; the arrows, Home, End,
 * Page Up and Page Down move. Control-S saves, Control-O opens, Control-N
 * begins a new file. Saving a file with no name, and opening one, ask for a
 * path upon the bottom row, which Enter accepts and Escape abandons.
 *
 * The text is one buffer and the cursor an offset into it. A line is found by
 * walking from the start each time the window is drawn, which for the
 * NOTEPAD_BYTES this holds is less time than the drawing itself, and is one
 * representation rather than two kept in step.
 */

#include <palette.h>
#include <term.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* The most text held: sixty-four kibibytes, which is every text file this
 * system carries many times over. A larger file is refused at opening rather
 * than cut, because saving a file cut would lose its end. */
#define NOTEPAD_BYTES (64U * 1024U)

#define NOTEPAD_GLYPH 8
#define NOTEPAD_PITCH 10
#define NOTEPAD_TAB   4
#define NOTEPAD_COLUMNS_MAXIMUM 256

/* The paper left of the text, in units, so that the first column does not
 * touch the window's border. */
#define NOTEPAD_MARGIN 4

/* The set 1 scancodes the editor reads that produce no character: Escape
 * unprefixed, and the rest behind 0xE0. */
#define NOTEPAD_SCANCODE_ESCAPE    0x01U
#define NOTEPAD_SCANCODE_HOME      0x47U
#define NOTEPAD_SCANCODE_UP        0x48U
#define NOTEPAD_SCANCODE_PAGE_UP   0x49U
#define NOTEPAD_SCANCODE_LEFT      0x4BU
#define NOTEPAD_SCANCODE_RIGHT     0x4DU
#define NOTEPAD_SCANCODE_END       0x4FU
#define NOTEPAD_SCANCODE_DOWN      0x50U
#define NOTEPAD_SCANCODE_PAGE_DOWN 0x51U
#define NOTEPAD_SCANCODE_DELETE    0x53U

#define NOTEPAD_PAPER  OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define NOTEPAD_INK    OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define NOTEPAD_STATUS OXYS_RGB(OXYS_BAR_RED, OXYS_BAR_GREEN, OXYS_BAR_BLUE)

static char NotepadText[NOTEPAD_BYTES];
static size_t NotepadLength;
static size_t NotepadCursor;
static bool NotepadModified;

static char NotepadPath[SYSCALL_PATH_MAXIMUM + 1U];

/* The first line and column shown. */
static size_t NotepadTop;
static size_t NotepadLeft;

/* The prompt upon the bottom row, while one is open: what it asks, what has
 * been typed, and what Enter does with it. */
typedef enum NotepadPrompt
{
    NOTEPAD_PROMPT_NONE = 0,
    NOTEPAD_PROMPT_SAVE,
    NOTEPAD_PROMPT_OPEN
} NotepadPrompt;

static NotepadPrompt NotepadPrompting;
static char NotepadAnswer[SYSCALL_PATH_MAXIMUM + 1U];

/* A message upon the bottom row until the next key. */
static char NotepadMessage[NOTEPAD_COLUMNS_MAXIMUM + 1];

/* A close asked for with changes unsaved: a second one discards them. */
static bool NotepadCloseWarned;

static int64_t NotepadWindow = -1;
static int32_t NotepadScale = 2;
static int32_t NotepadColumns;
static int32_t NotepadRows;
static int32_t NotepadWidth;
static int32_t NotepadHeight;

/* ------------------------------------------------------------- the text */

static void NotepadSay(const char *message)
{
    (void)snprintf(NotepadMessage, sizeof NotepadMessage, "%s", message);
}

static bool NotepadInsert(const char *bytes, size_t count)
{
    if ((NotepadLength + count) > sizeof NotepadText)
    {
        NotepadSay("The file is as long as Notepad holds.");

        return false;
    }

    (void)memmove(&NotepadText[NotepadCursor + count], &NotepadText[NotepadCursor],
                  NotepadLength - NotepadCursor);
    (void)memcpy(&NotepadText[NotepadCursor], bytes, count);
    NotepadLength += count;
    NotepadCursor += count;
    NotepadModified = true;

    return true;
}

/* Removes the byte before the cursor, or the one at it. */
static void NotepadDelete(bool before)
{
    size_t at;

    if (before ? (NotepadCursor == 0U) : (NotepadCursor >= NotepadLength))
    {
        return;
    }

    at = before ? (NotepadCursor - 1U) : NotepadCursor;
    (void)memmove(&NotepadText[at], &NotepadText[at + 1U], NotepadLength - at - 1U);
    --NotepadLength;
    NotepadCursor = at;
    NotepadModified = true;
}

/* The offset at which the line holding `offset` begins, and where it ends. */
static size_t NotepadLineStart(size_t offset)
{
    while ((offset > 0U) && (NotepadText[offset - 1U] != '\n'))
    {
        --offset;
    }

    return offset;
}

static size_t NotepadLineEnd(size_t offset)
{
    while ((offset < NotepadLength) && (NotepadText[offset] != '\n'))
    {
        ++offset;
    }

    return offset;
}

/* The cursor's line and column, counted from zero. */
static void NotepadWhere(size_t *line, size_t *column)
{
    *line = 0U;

    for (size_t index = 0U; index < NotepadCursor; ++index)
    {
        *line += (NotepadText[index] == '\n') ? 1U : 0U;
    }

    *column = NotepadCursor - NotepadLineStart(NotepadCursor);
}

/* Moves up or down `lines`, keeping the column where the line is long
 * enough for it. */
static void NotepadMoveLines(long lines)
{
    const size_t column = NotepadCursor - NotepadLineStart(NotepadCursor);
    size_t start = NotepadLineStart(NotepadCursor);

    for (; lines < 0; ++lines)
    {
        if (start == 0U)
        {
            break;
        }

        start = NotepadLineStart(start - 1U);
    }

    for (; lines > 0; --lines)
    {
        const size_t end = NotepadLineEnd(start);

        if (end >= NotepadLength)
        {
            break;
        }

        start = end + 1U;
    }

    NotepadCursor = start + column;

    if (NotepadCursor > NotepadLineEnd(start))
    {
        NotepadCursor = NotepadLineEnd(start);
    }
}

/* --------------------------------------------------------------- files */

static bool NotepadLoad(const char *path)
{
    const int64_t descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);
    size_t total = 0U;

    if (descriptor < 0)
    {
        char message[NOTEPAD_COLUMNS_MAXIMUM + 1];

        (void)snprintf(message, sizeof message, "%s: %s", path, strerror(errno));
        NotepadSay(message);

        return false;
    }

    for (;;)
    {
        char probe;
        const int64_t got = (total < sizeof NotepadText)
                                ? OxysRead((int)descriptor, &NotepadText[total],
                                           sizeof NotepadText - total)
                                : OxysRead((int)descriptor, &probe, 1U);

        if (got <= 0)
        {
            break;
        }

        if (total >= sizeof NotepadText)
        {
            (void)OxysClose((int)descriptor);
            NotepadLength = 0U;
            NotepadCursor = 0U;
            NotepadSay("The file is longer than Notepad holds; it was not opened.");

            return false;
        }

        total += (size_t)got;
    }

    (void)OxysClose((int)descriptor);
    NotepadLength = total;
    NotepadCursor = 0U;
    NotepadTop = 0U;
    NotepadLeft = 0U;
    NotepadModified = false;
    (void)snprintf(NotepadPath, sizeof NotepadPath, "%s", path);
    NotepadSay("Opened.");

    return true;
}

/*
 * Writes the text as `micro` saves: into a file beside the one named, which
 * takes the name only once it is whole, so that a failure part way leaves the
 * file as it was. docs/design/UTILITIES.md.
 */
static bool NotepadWrite(const char *path)
{
    char beside[SYSCALL_PATH_MAXIMUM + 1U];
    const int composed = snprintf(beside, sizeof beside, "%s.notepad", path);
    int64_t descriptor;

    if ((composed < 0) || ((size_t)composed >= sizeof beside))
    {
        return false;
    }

    descriptor = OxysOpen(beside, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE,
                          0644U);

    if (descriptor < 0)
    {
        return false;
    }

    if (((NotepadLength > 0U) &&
         (OxysWrite((int)descriptor, NotepadText, NotepadLength) != (int64_t)NotepadLength)) ||
        (OxysClose((int)descriptor) < 0))
    {
        (void)OxysUnlink(beside);

        return false;
    }

    if (((OxysUnlink(path) < 0) && (errno != ENOENT)) || (OxysLink(beside, path) < 0))
    {
        (void)OxysUnlink(beside);

        return false;
    }

    (void)OxysUnlink(beside);

    return true;
}

static void NotepadSave(const char *path)
{
    char message[NOTEPAD_COLUMNS_MAXIMUM + 1];

    if (!NotepadWrite(path))
    {
        (void)snprintf(message, sizeof message, "%s could not be saved: %s", path,
                       strerror(errno));
        NotepadSay(message);
        (void)OxysNotify(SYSCALL_NOTIFY_ERROR, 0U, "Notepad could not save the file.");

        return;
    }

    (void)snprintf(NotepadPath, sizeof NotepadPath, "%s", path);
    NotepadModified = false;
    NotepadSay("Saved.");

    {
        const char *base = strrchr(path, '/');
        char text[SYSCALL_NOTIFICATION_TEXT_MAXIMUM + 1U];

        (void)snprintf(text, sizeof text, "Saved %s.", (base != NULL) ? base + 1 : path);
        (void)OxysNotify(SYSCALL_NOTIFY_SUCCESS, 0U, text);
    }
}

/* ------------------------------------------------------------- drawing */

static void NotepadRow(int32_t row, const char *text, uint32_t ink, uint32_t paper)
{
    SyscallWindowText placement;

    placement.x = NOTEPAD_MARGIN * NotepadScale;
    placement.y = row * NOTEPAD_PITCH * NotepadScale;
    placement.ink = ink;
    placement.paper = paper;
    placement.scale = NotepadScale;
    (void)OxysWindowText(NotepadWindow, &placement, text);
}

/* Keeps the cursor within what is shown, scrolling by as little as does. */
static void NotepadFollow(void)
{
    size_t line;
    size_t column;
    const size_t rows = (NotepadRows > 1) ? (size_t)(NotepadRows - 1) : 1U;
    const size_t columns = (NotepadColumns > 1) ? (size_t)NotepadColumns : 1U;

    NotepadWhere(&line, &column);

    if (line < NotepadTop)
    {
        NotepadTop = line;
    }
    else if (line >= (NotepadTop + rows))
    {
        NotepadTop = line - rows + 1U;
    }

    if (column < NotepadLeft)
    {
        NotepadLeft = column;
    }
    else if (column >= (NotepadLeft + columns))
    {
        NotepadLeft = column - columns + 1U;
    }
}

static void NotepadDraw(void)
{
    char row_text[NOTEPAD_COLUMNS_MAXIMUM + 1];
    size_t cursor_line;
    size_t cursor_column;
    size_t offset = 0U;
    const int32_t text_rows = NotepadRows - 1;

    NotepadFollow();
    NotepadWhere(&cursor_line, &cursor_column);

    /* To the first line shown. */
    for (size_t line = 0U; (line < NotepadTop) && (offset < NotepadLength); ++line)
    {
        offset = NotepadLineEnd(offset);
        offset += (offset < NotepadLength) ? 1U : 0U;
    }

    for (int32_t row = 0; row < text_rows; ++row)
    {
        const size_t line = NotepadTop + (size_t)row;
        const bool exists = (offset < NotepadLength) || (line == 0U) ||
                            ((offset == NotepadLength) && (NotepadLength > 0U) &&
                             (NotepadText[NotepadLength - 1U] == '\n'));
        size_t end = exists ? NotepadLineEnd(offset) : offset;

        for (int32_t column = 0; column < NotepadColumns; ++column)
        {
            const size_t at = offset + NotepadLeft + (size_t)column;

            row_text[column] = (exists && (at < end)) ? NotepadText[at] : ' ';
        }

        row_text[NotepadColumns] = '\0';
        NotepadRow(row, row_text, NOTEPAD_INK, NOTEPAD_PAPER);

        /* The cursor, a cell drawn in the other colours. */
        if (exists && (line == cursor_line) && (NotepadPrompting == NOTEPAD_PROMPT_NONE) &&
            (cursor_column >= NotepadLeft) &&
            (cursor_column < (NotepadLeft + (size_t)NotepadColumns)))
        {
            const size_t at = NotepadCursor;
            char cell[2] = { ((at < NotepadLength) && (NotepadText[at] != '\n')) ? NotepadText[at]
                                                                                  : ' ',
                             '\0' };
            SyscallWindowText placement;

            placement.x = (NOTEPAD_MARGIN * NotepadScale) +
                          ((int32_t)(cursor_column - NotepadLeft) * NOTEPAD_GLYPH * NotepadScale);
            placement.y = row * NOTEPAD_PITCH * NotepadScale;
            placement.ink = NOTEPAD_PAPER;
            placement.paper = NOTEPAD_INK;
            placement.scale = NotepadScale;
            (void)OxysWindowText(NotepadWindow, &placement, cell);
        }

        if (exists)
        {
            offset = (end < NotepadLength) ? end + 1U : NotepadLength + 1U;
        }
    }

    /* The bottom row: the prompt while one is open, or the message, or the
     * file's name, whether it is changed, and where the cursor is. */
    if (NotepadPrompting != NOTEPAD_PROMPT_NONE)
    {
        (void)snprintf(row_text, sizeof row_text, "%s %s_",
                       (NotepadPrompting == NOTEPAD_PROMPT_SAVE) ? "Save as:" : "Open:",
                       NotepadAnswer);
    }
    else if (NotepadMessage[0] != '\0')
    {
        (void)snprintf(row_text, sizeof row_text, "%s", NotepadMessage);
    }
    else
    {
        (void)snprintf(row_text, sizeof row_text, "%s%s  line %u, column %u   ^S save ^O open ^N new",
                       (NotepadPath[0] != '\0') ? NotepadPath : "Untitled",
                       NotepadModified ? " (changed)" : "", (unsigned)(cursor_line + 1U),
                       (unsigned)(cursor_column + 1U));
    }

    {
        const size_t used = strlen(row_text);

        for (size_t index = used; index < (size_t)NotepadColumns; ++index)
        {
            row_text[index] = ' ';
        }

        row_text[NotepadColumns] = '\0';
    }

    NotepadRow(NotepadRows - 1, row_text, NOTEPAD_INK, NOTEPAD_STATUS);
}

/* The grid a window of this extent holds. */
static void NotepadLayout(int32_t width, int32_t height)
{
    NotepadWidth = width;
    NotepadHeight = height;
    NotepadColumns = (width - (2 * NOTEPAD_MARGIN * NotepadScale)) / (NOTEPAD_GLYPH * NotepadScale);
    NotepadRows = height / (NOTEPAD_PITCH * NotepadScale);

    if (NotepadColumns > NOTEPAD_COLUMNS_MAXIMUM)
    {
        NotepadColumns = NOTEPAD_COLUMNS_MAXIMUM;
    }

    if (NotepadRows < 2)
    {
        NotepadRows = 2;
    }
}

/* The window's ground, once, and again after a new extent: the rows are
 * drawn over it, and what lies below the last row is kept paper. */
static void NotepadPaint(void)
{
    static uint32_t tile[2048];
    SyscallWindowRectangle area;
    const int32_t rows = (int32_t)(sizeof tile / sizeof tile[0]) /
                         ((NotepadWidth > 0) ? NotepadWidth : 1);

    if (rows <= 0)
    {
        return;
    }

    for (size_t index = 0U; index < (sizeof tile / sizeof tile[0]); ++index)
    {
        tile[index] = NOTEPAD_PAPER;
    }

    area.x = 0;
    area.width = NotepadWidth;

    for (int32_t top = 0; top < NotepadHeight; top += rows)
    {
        area.y = top;
        area.height = ((top + rows) <= NotepadHeight) ? rows : (NotepadHeight - top);
        (void)OxysWindowBlit(NotepadWindow, &area, tile);
    }
}

/* ---------------------------------------------------------------- keys */

/* A key while the bottom row asks for a path. */
static void NotepadPromptKey(const SyscallWindowEvent *event)
{
    const size_t length = strlen(NotepadAnswer);

    if ((event->key_extended == 0U) && (event->key_scancode == NOTEPAD_SCANCODE_ESCAPE))
    {
        NotepadPrompting = NOTEPAD_PROMPT_NONE;
        NotepadSay("Abandoned.");
    }
    else if (event->key_character == '\n')
    {
        const NotepadPrompt asked = NotepadPrompting;

        NotepadPrompting = NOTEPAD_PROMPT_NONE;

        if (NotepadAnswer[0] == '\0')
        {
            NotepadSay("No path was given.");
        }
        else if (asked == NOTEPAD_PROMPT_SAVE)
        {
            NotepadSave(NotepadAnswer);
        }
        else
        {
            (void)NotepadLoad(NotepadAnswer);
        }
    }
    else if (event->key_character == '\b')
    {
        if (length > 0U)
        {
            NotepadAnswer[length - 1U] = '\0';
        }
    }
    else if ((event->key_character >= ' ') && (event->key_character <= '~') &&
             (length < (sizeof NotepadAnswer - 1U)))
    {
        NotepadAnswer[length] = event->key_character;
        NotepadAnswer[length + 1U] = '\0';
    }
}

static void NotepadAsk(NotepadPrompt prompt, const char *start)
{
    NotepadPrompting = prompt;
    (void)snprintf(NotepadAnswer, sizeof NotepadAnswer, "%s", start);
}

static void NotepadHandleKey(const SyscallWindowEvent *event)
{
    const size_t page = (NotepadRows > 2) ? (size_t)(NotepadRows - 2) : 1U;

    if (event->key_pressed == 0U)
    {
        return;
    }

    NotepadMessage[0] = '\0';
    NotepadCloseWarned = false;

    if (NotepadPrompting != NOTEPAD_PROMPT_NONE)
    {
        NotepadPromptKey(event);

        return;
    }

    if ((event->key_modifiers & TERM_MODIFIER_CONTROL) != 0U)
    {
        switch (event->key_character)
        {
        case 's':
        case 'S':
            if (NotepadPath[0] != '\0')
            {
                NotepadSave(NotepadPath);
            }
            else
            {
                NotepadAsk(NOTEPAD_PROMPT_SAVE, "/");
            }
            break;
        case 'o':
        case 'O':
            NotepadAsk(NOTEPAD_PROMPT_OPEN, "/");
            break;
        case 'n':
        case 'N':
            NotepadLength = 0U;
            NotepadCursor = 0U;
            NotepadTop = 0U;
            NotepadLeft = 0U;
            NotepadModified = false;
            NotepadPath[0] = '\0';
            NotepadSay("A new file.");
            break;
        default:
            break;
        }

        return;
    }

    if (event->key_extended != 0U)
    {
        switch (event->key_scancode)
        {
        case NOTEPAD_SCANCODE_LEFT:
            NotepadCursor -= (NotepadCursor > 0U) ? 1U : 0U;
            break;
        case NOTEPAD_SCANCODE_RIGHT:
            NotepadCursor += (NotepadCursor < NotepadLength) ? 1U : 0U;
            break;
        case NOTEPAD_SCANCODE_UP:
            NotepadMoveLines(-1);
            break;
        case NOTEPAD_SCANCODE_DOWN:
            NotepadMoveLines(1);
            break;
        case NOTEPAD_SCANCODE_PAGE_UP:
            NotepadMoveLines(-(long)page);
            break;
        case NOTEPAD_SCANCODE_PAGE_DOWN:
            NotepadMoveLines((long)page);
            break;
        case NOTEPAD_SCANCODE_HOME:
            NotepadCursor = NotepadLineStart(NotepadCursor);
            break;
        case NOTEPAD_SCANCODE_END:
            NotepadCursor = NotepadLineEnd(NotepadCursor);
            break;
        case NOTEPAD_SCANCODE_DELETE:
            NotepadDelete(false);
            break;
        default:
            break;
        }

        return;
    }

    if (event->key_character == '\b')
    {
        NotepadDelete(true);
    }
    else if (event->key_character == '\t')
    {
        (void)NotepadInsert("    ", NOTEPAD_TAB);
    }
    else if ((event->key_character == '\n') ||
             ((event->key_character >= ' ') && (event->key_character <= '~')))
    {
        (void)NotepadInsert(&event->key_character, 1U);
    }
}

int main(int argument_count, char *argument_vector[])
{
    SyscallWindowRectangle screen;
    SyscallWindowRectangle geometry;
    bool running = true;

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "notepad: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    NotepadScale = (screen.width >= 1024) ? 2 : 1;
    geometry.width = ((screen.width * 3 / 5) / (NOTEPAD_GLYPH * NotepadScale)) * NOTEPAD_GLYPH *
                     NotepadScale;
    geometry.height = ((screen.height * 3 / 5) / (NOTEPAD_PITCH * NotepadScale)) * NOTEPAD_PITCH *
                      NotepadScale;
    geometry.x = (screen.width - geometry.width) / 4;
    geometry.y = (screen.height - geometry.height) / 4;

    NotepadWindow = OxysWindowCreate(&geometry, "Notepad", SYSCALL_WINDOW_LAYER_NORMAL);

    if (NotepadWindow < 0)
    {
        (void)fprintf(stderr, "notepad: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    NotepadLayout(geometry.width, geometry.height);

    if (argument_count > 1)
    {
        if (!NotepadLoad(argument_vector[1]) && (errno == ENOENT))
        {
            /* A name that does not exist yet is a file to be made. */
            (void)snprintf(NotepadPath, sizeof NotepadPath, "%s", argument_vector[1]);
            NotepadSay("A new file.");
        }
    }

    NotepadPaint();
    NotepadDraw();

    while (running)
    {
        SyscallWindowEvent event;
        bool changed = false;
        bool resized = false;
        int64_t result = OxysWindowEvent(NotepadWindow, &event, SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        while (result == 1)
        {
            if (event.kind == SYSCALL_WINDOW_EVENT_KEY)
            {
                NotepadHandleKey(&event);
                changed = true;
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_RESIZE)
            {
                NotepadLayout(event.x, event.y);
                resized = true;
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_CLOSE)
            {
                /* Changes unsaved are not lost to one press: the first says
                 * so, and a second, with nothing typed between, discards. */
                if (NotepadModified && !NotepadCloseWarned)
                {
                    NotepadCloseWarned = true;
                    NotepadSay("Unsaved changes. Close again to discard them.");
                    changed = true;
                }
                else
                {
                    running = false;
                }
            }

            result = OxysWindowEvent(NotepadWindow, &event, 0U);
        }

        if (resized)
        {
            NotepadPaint();
        }

        if (running && (changed || resized))
        {
            NotepadDraw();
        }
    }

    (void)OxysWindowDestroy(NotepadWindow);

    return EXIT_SUCCESS;
}
