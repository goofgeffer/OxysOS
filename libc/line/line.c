/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/line/line.c
 * Purpose: Implements the line editor of sub-task 8.1 — the editing of a line,
 *          the parsing of the terminal's control sequences, and the history —
 *          above an output function, and without naming a descriptor.
 * Key functions: LineInitialise, LineBegin, LineFeed, LineText, LineLength,
 *          LineCursor, LineRemember, LineHistoryCount, LineHistoryAt, LinePreset.
 * References:
 *   - ECMA-48, 5th edition (1991), Section 5.4: the grammar of a control
 *     sequence, which LineFeed parses; Sections 8.3.18 to 8.3.22, the cursor
 *     movements the final bytes A to D denote.
 *   - XTerm Control Sequences (Dickey): the forms in which Home, End, Delete
 *     and the cursor keys arrive, all of which are accepted.
 *   - libc/include/line.h: the interface, and the table of what each byte does.
 *   - docs/design/SHELL.md, Section 3: the redraw discipline below and why it
 *     asks nothing of a display but a backspace.
 *
 * The one assumption made of the display, and why it is the only one.
 *
 *   Everything the editor draws is drawn with printable characters, spaces and
 *   the backspace character, and the backspace is assumed to move the cursor
 *   one position left without erasing. That is what a serial terminal does,
 *   what this kernel's text-mode display does, and what its framebuffer console
 *   does; none of the three is asked to interpret a control sequence, and so the
 *   editor draws correctly upon a display that interprets none. The price is
 *   that the editor cannot move the cursor up, so a line longer than the
 *   display is wide is drawn wrongly once it wraps — a limitation recorded in
 *   the design document, and a smaller one than depending upon the display's
 *   dialect would be.
 *
 *   The redraw of an insertion is therefore: write the character and every
 *   character after it, then backspace over the ones after it. Of a deletion:
 *   write every character after the gap, then a space to blank the position the
 *   line no longer reaches, then backspace over all of that. Of a recall from
 *   the history: backspace to the start, write the recalled line, blank whatever
 *   the old line extended beyond it, and backspace over the blanks. Each is a
 *   few more bytes than a terminal that could be told "delete character" would
 *   need, and the bytes are not the scarce thing.
 */

#include <line.h>
#include <string.h>

/* The two bytes a control sequence and a single shift three begin with, after
 * the escape. ECMA-48, Section 5.4, for the first; the second is the DEC form
 * every emulator sends for the cursor keys in application mode. */
#define LINE_ESCAPE           '\x1B'
#define LINE_CSI_INTRODUCER   '['
#define LINE_SS3_INTRODUCER   'O'

/* The control characters acted upon, by the letters they are the controls of. */
#define LINE_CONTROL(letter) ((char)((letter) & 0x1F))
#define LINE_BACKSPACE        '\b'
#define LINE_DELETE           '\x7F'

/* How many backspaces or spaces are emitted in one call of the output function.
 * A run longer than this is emitted in several; the count is small so that the
 * array it fills is small. */
#define LINE_RUN_CAPACITY 32U

static void LineEmit(LineEditor *editor, const char *bytes, size_t count)
{
    if ((editor->output != NULL) && (count > 0U))
    {
        editor->output(editor->context, bytes, count);
    }
}

/* Emits `count` copies of one byte. */
static void LineEmitRun(LineEditor *editor, char byte, size_t count)
{
    char run[LINE_RUN_CAPACITY];

    memset(run, byte, sizeof(run));

    while (count > 0U)
    {
        const size_t now = (count < sizeof(run)) ? count : sizeof(run);

        LineEmit(editor, run, now);
        count -= now;
    }
}

void LineInitialise(LineEditor *editor, LineOutput output, void *context)
{
    if (editor == NULL)
    {
        return;
    }

    memset(editor, 0, sizeof(*editor));
    editor->output = output;
    editor->context = context;
}

void LineBegin(LineEditor *editor)
{
    if (editor == NULL)
    {
        return;
    }

    editor->text[0] = '\0';
    editor->length = 0U;
    editor->cursor = 0U;
    editor->draft[0] = '\0';
    editor->draft_length = 0U;
    editor->browsing = editor->history_count;
    editor->state = LINE_PARSE_PLAIN;
    editor->parameter_length = 0U;
}

/* ---------------------------------------------------------------------------
 * The edits. Each changes the fields and the display together, which is the
 * invariant the whole file exists to keep.
 * ------------------------------------------------------------------------- */

static void LineInsert(LineEditor *editor, char byte)
{
    const size_t after = editor->length - editor->cursor;

    if (editor->length + 1U >= LINE_CAPACITY)
    {
        ++editor->discarded;

        return;
    }

    memmove(&editor->text[editor->cursor + 1U], &editor->text[editor->cursor], after + 1U);
    editor->text[editor->cursor] = byte;
    ++editor->length;

    LineEmit(editor, &editor->text[editor->cursor], after + 1U);
    LineEmitRun(editor, LINE_BACKSPACE, after);
    ++editor->cursor;
}

/* Removes the character at `position`, which must be within the line. */
static void LineRemoveAt(LineEditor *editor, size_t position)
{
    const size_t after = editor->length - position - 1U;

    memmove(&editor->text[position], &editor->text[position + 1U], after + 1U);
    --editor->length;

    /* The cursor is at `position` when this is called, whichever key led here:
     * a deletion before the cursor moves it first. */
    LineEmit(editor, &editor->text[position], after);
    LineEmitRun(editor, ' ', 1U);
    LineEmitRun(editor, LINE_BACKSPACE, after + 1U);
}

static void LineDeleteBefore(LineEditor *editor)
{
    if (editor->cursor == 0U)
    {
        ++editor->ignored;

        return;
    }

    --editor->cursor;
    LineEmitRun(editor, LINE_BACKSPACE, 1U);
    LineRemoveAt(editor, editor->cursor);
}

static void LineDeleteUnder(LineEditor *editor)
{
    if (editor->cursor >= editor->length)
    {
        ++editor->ignored;

        return;
    }

    LineRemoveAt(editor, editor->cursor);
}

static void LineMoveLeft(LineEditor *editor)
{
    if (editor->cursor == 0U)
    {
        ++editor->ignored;

        return;
    }

    --editor->cursor;
    LineEmitRun(editor, LINE_BACKSPACE, 1U);
}

/* Moving right is writing the character the cursor stands upon, which every
 * display advances past; there is no other way to move right with nothing but
 * a backspace, and it draws what is already there. */
static void LineMoveRight(LineEditor *editor)
{
    if (editor->cursor >= editor->length)
    {
        ++editor->ignored;

        return;
    }

    LineEmit(editor, &editor->text[editor->cursor], 1U);
    ++editor->cursor;
}

static void LineMoveToStart(LineEditor *editor)
{
    LineEmitRun(editor, LINE_BACKSPACE, editor->cursor);
    editor->cursor = 0U;
}

static void LineMoveToEnd(LineEditor *editor)
{
    LineEmit(editor, &editor->text[editor->cursor], editor->length - editor->cursor);
    editor->cursor = editor->length;
}

static void LineKillToEnd(LineEditor *editor)
{
    const size_t removed = editor->length - editor->cursor;

    editor->text[editor->cursor] = '\0';
    editor->length = editor->cursor;

    LineEmitRun(editor, ' ', removed);
    LineEmitRun(editor, LINE_BACKSPACE, removed);
}

static void LineKillToStart(LineEditor *editor)
{
    const size_t removed = editor->cursor;
    const size_t kept = editor->length - removed;

    memmove(editor->text, &editor->text[removed], kept + 1U);
    editor->length = kept;
    editor->cursor = 0U;

    LineEmitRun(editor, LINE_BACKSPACE, removed);
    LineEmit(editor, editor->text, kept);
    LineEmitRun(editor, ' ', removed);
    LineEmitRun(editor, LINE_BACKSPACE, kept + removed);
}

/*
 * Replaces the whole line with `replacement`, the cursor ending at its end.
 *
 * The old line is not erased and then the new one written; the new one is
 * written over it and only the excess blanked. The difference is visible on a
 * slow serial line as the absence of a flicker, and invisible otherwise.
 */
static void LineReplace(LineEditor *editor, const char *replacement)
{
    const size_t previous = editor->length;
    size_t incoming = strlen(replacement);

    if (incoming >= LINE_CAPACITY)
    {
        incoming = LINE_CAPACITY - 1U;
    }

    LineMoveToStart(editor);

    memcpy(editor->text, replacement, incoming);
    editor->text[incoming] = '\0';
    editor->length = incoming;
    editor->cursor = incoming;

    LineEmit(editor, editor->text, incoming);

    if (previous > incoming)
    {
        LineEmitRun(editor, ' ', previous - incoming);
        LineEmitRun(editor, LINE_BACKSPACE, previous - incoming);
    }
}

void LinePreset(LineEditor *editor, const char *text)
{
    if ((editor == NULL) || (text == NULL))
    {
        return;
    }

    /*
     * The replacement the history recall performs, offered to a caller: the
     * line becomes `text`, drawn, with the cursor at its end, and what the
     * person then does to it is editing and not typing. It exists for the
     * editor `micro`, whose `e` command hands a line back to be changed rather
     * than retyped — the difference between an editor and a program that asks
     * for a line again.
     */
    LineReplace(editor, text);
}

/* The subscript within the ring of the history's entry at `position`, 0 being
 * the oldest. */
static size_t LineHistorySlot(const LineEditor *editor, size_t position)
{
    return (editor->history_first + position) % LINE_HISTORY_DEPTH;
}

static void LineRecallPrevious(LineEditor *editor)
{
    if (editor->browsing == 0U)
    {
        ++editor->ignored;

        return;
    }

    /* Leaving the draft: keep it, so that stepping back down restores what the
     * person had typed before they went looking. */
    if (editor->browsing == editor->history_count)
    {
        memcpy(editor->draft, editor->text, editor->length + 1U);
        editor->draft_length = editor->length;
    }

    --editor->browsing;
    LineReplace(editor, editor->history[LineHistorySlot(editor, editor->browsing)]);
}

static void LineRecallNext(LineEditor *editor)
{
    if (editor->browsing >= editor->history_count)
    {
        ++editor->ignored;

        return;
    }

    ++editor->browsing;

    if (editor->browsing == editor->history_count)
    {
        LineReplace(editor, editor->draft);
    }
    else
    {
        LineReplace(editor, editor->history[LineHistorySlot(editor, editor->browsing)]);
    }
}

/* ---------------------------------------------------------------------------
 * The parser.
 * ------------------------------------------------------------------------- */

/* Acts upon the final byte of a control sequence or a single shift three, the
 * parameter bytes having been kept. Returns false where the sequence is one the
 * editor assigns no meaning to. */
static bool LineDispatchSequence(LineEditor *editor, char final)
{
    const char *const parameter = editor->parameter;
    const bool one = (editor->parameter_length == 1U);

    switch (final)
    {
    case 'A':
        LineRecallPrevious(editor);
        return true;
    case 'B':
        LineRecallNext(editor);
        return true;
    case 'C':
        LineMoveRight(editor);
        return true;
    case 'D':
        LineMoveLeft(editor);
        return true;
    case 'H':
        LineMoveToStart(editor);
        return true;
    case 'F':
        LineMoveToEnd(editor);
        return true;
    case '~':
        if (one && ((parameter[0] == '1') || (parameter[0] == '7')))
        {
            LineMoveToStart(editor);
            return true;
        }

        if (one && ((parameter[0] == '4') || (parameter[0] == '8')))
        {
            LineMoveToEnd(editor);
            return true;
        }

        if (one && (parameter[0] == '3'))
        {
            LineDeleteUnder(editor);
            return true;
        }

        return false;
    default:
        return false;
    }
}

static LineResult LineFeedPlain(LineEditor *editor, char byte)
{
    if ((byte >= 0x20) && (byte <= 0x7E))
    {
        LineInsert(editor, byte);

        return LINE_PENDING;
    }

    switch (byte)
    {
    case '\r':
    case '\n':
        LineEmit(editor, "\n", 1U);
        return LINE_COMPLETE;

    case LINE_ESCAPE:
        editor->state = LINE_PARSE_ESCAPE;
        return LINE_PENDING;

    case LINE_BACKSPACE:
    case LINE_DELETE:
        LineDeleteBefore(editor);
        return LINE_PENDING;

    case LINE_CONTROL('D'):
        if (editor->length == 0U)
        {
            LineEmit(editor, "\n", 1U);
            return LINE_END;
        }

        LineDeleteUnder(editor);
        return LINE_PENDING;

    case LINE_CONTROL('A'):
        LineMoveToStart(editor);
        return LINE_PENDING;

    case LINE_CONTROL('E'):
        LineMoveToEnd(editor);
        return LINE_PENDING;

    case LINE_CONTROL('B'):
        LineMoveLeft(editor);
        return LINE_PENDING;

    case LINE_CONTROL('F'):
        LineMoveRight(editor);
        return LINE_PENDING;

    case LINE_CONTROL('P'):
        LineRecallPrevious(editor);
        return LINE_PENDING;

    case LINE_CONTROL('N'):
        LineRecallNext(editor);
        return LINE_PENDING;

    case LINE_CONTROL('U'):
        LineKillToStart(editor);
        return LINE_PENDING;

    case LINE_CONTROL('K'):
        LineKillToEnd(editor);
        return LINE_PENDING;

    default:
        ++editor->ignored;
        return LINE_PENDING;
    }
}

LineResult LineFeed(LineEditor *editor, char byte)
{
    if (editor == NULL)
    {
        return LINE_PENDING;
    }

    switch (editor->state)
    {
    case LINE_PARSE_ESCAPE:
        editor->parameter_length = 0U;

        if (byte == LINE_CSI_INTRODUCER)
        {
            editor->state = LINE_PARSE_CSI;
        }
        else if (byte == LINE_SS3_INTRODUCER)
        {
            editor->state = LINE_PARSE_SS3;
        }
        else
        {
            /* An escape this editor does not understand: the escape and the
             * byte after it are dropped together, and counted as one. */
            editor->state = LINE_PARSE_PLAIN;
            ++editor->ignored;
        }

        return LINE_PENDING;

    case LINE_PARSE_CSI:
        /*
         * ECMA-48, Section 5.4: parameter bytes are 0x30 to 0x3F and
         * intermediate bytes 0x20 to 0x2F, in that order, and the final byte
         * is 0x40 to 0x7E. Both kinds are kept together — no sequence acted
         * upon here has an intermediate — and anything else ends the sequence
         * as malformed.
         */
        if ((byte >= 0x20) && (byte <= 0x3F))
        {
            if (editor->parameter_length < LINE_PARAMETER_CAPACITY)
            {
                editor->parameter[editor->parameter_length] = byte;
            }

            ++editor->parameter_length;

            return LINE_PENDING;
        }

        editor->state = LINE_PARSE_PLAIN;

        if ((byte >= 0x40) && (byte <= 0x7E))
        {
            if (editor->parameter_length > LINE_PARAMETER_CAPACITY)
            {
                editor->parameter_length = LINE_PARAMETER_CAPACITY;
            }

            if (!LineDispatchSequence(editor, byte))
            {
                ++editor->ignored;
            }
        }
        else
        {
            ++editor->ignored;
        }

        return LINE_PENDING;

    case LINE_PARSE_SS3:
        editor->state = LINE_PARSE_PLAIN;
        editor->parameter_length = 0U;

        if (!LineDispatchSequence(editor, byte))
        {
            ++editor->ignored;
        }

        return LINE_PENDING;

    case LINE_PARSE_PLAIN:
    default:
        return LineFeedPlain(editor, byte);
    }
}

/* ---------------------------------------------------------------------------
 * The accessors and the history.
 * ------------------------------------------------------------------------- */

const char *LineText(const LineEditor *editor)
{
    return (editor != NULL) ? editor->text : NULL;
}

size_t LineLength(const LineEditor *editor)
{
    return (editor != NULL) ? editor->length : 0U;
}

size_t LineCursor(const LineEditor *editor)
{
    return (editor != NULL) ? editor->cursor : 0U;
}

void LineRemember(LineEditor *editor)
{
    size_t slot;

    if ((editor == NULL) || (editor->length == 0U))
    {
        return;
    }

    if (editor->history_count > 0U)
    {
        const size_t newest = LineHistorySlot(editor, editor->history_count - 1U);

        if (strcmp(editor->history[newest], editor->text) == 0)
        {
            return;
        }
    }

    if (editor->history_count < LINE_HISTORY_DEPTH)
    {
        slot = LineHistorySlot(editor, editor->history_count);
        ++editor->history_count;
    }
    else
    {
        /* Full: the oldest slot is reused and the ring turns. */
        slot = editor->history_first;
        editor->history_first = (editor->history_first + 1U) % LINE_HISTORY_DEPTH;
    }

    memcpy(editor->history[slot], editor->text, editor->length + 1U);
}

size_t LineHistoryCount(const LineEditor *editor)
{
    return (editor != NULL) ? editor->history_count : 0U;
}

const char *LineHistoryAt(const LineEditor *editor, size_t position)
{
    if ((editor == NULL) || (position >= editor->history_count))
    {
        return NULL;
    }

    return editor->history[LineHistorySlot(editor, position)];
}
