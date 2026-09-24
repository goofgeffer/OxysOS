/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/line.h
 * Purpose: Declares the line editor of sub-task 8.1 — the thing that turns the
 *          bytes a terminal sends into a line a program may act upon, with the
 *          cursor movements, the deletions and the history a person expects of
 *          a prompt — and the seam between its editing, which runs anywhere,
 *          and the descriptors it reads and writes, which run only at privilege
 *          level 3.
 * Key definitions: LineEditor, LineResult, LineOutput, LINE_CAPACITY,
 *          LINE_HISTORY_DEPTH, LineInitialise, LineBegin, LineFeed, LineText,
 *          LineLength, LineCursor, LineRemember, LineHistoryCount, LineHistoryAt,
 *          LinePreset, LineRead, LineEdit.
 * References:
 *   - ECMA-48, 5th edition (1991), Section 5.4: a control sequence is CSI,
 *     parameter bytes in 0x30 to 0x3F, intermediate bytes in 0x20 to 0x2F, and
 *     a final byte in 0x40 to 0x7E — which is the grammar the parser below
 *     accepts, whatever the final byte turns out to be. Sections 8.3.18 (CUB),
 *     8.3.19 (CUD), 8.3.20 (CUF) and 8.3.22 (CUU): the four the cursor keys
 *     send.
 *   - XTerm Control Sequences (Dickey): Home and End as CSI H and CSI F or SS3 H
 *     and SS3 F, and as CSI 1 ~ and CSI 4 ~ upon a VT220; Delete as CSI 3 ~;
 *     the cursor keys as SS3 A to D when the terminal is in application mode.
 *     Every one of those forms is accepted, because which form arrives is the
 *     terminal's choice and not the program's.
 *   - IEEE Std 1003.1-2017, Section 11.1.9 and the `stty` utility: the erase
 *     character is DEL or BS according to the terminal, so both delete.
 *   - docs/design/SHELL.md: the design, the redraw discipline that
 *     needs nothing of a terminal but backspace, and the limitations.
 *
 * Why this is in the C library, when ISO/IEC 9899:2011 has no such thing.
 *
 *   For the reason <syscall.h> and <heap.h> are: it is this system's library,
 *   and a header that is not the standard's says so in its name. The editor
 *   could have been a translation unit of the shell alone; it is here because
 *   the shell is not the only program that will prompt — a debugger, a
 *   calculator, the terminal window of Phase 9 — and because the boot-time
 *   self-test asserts the library it ships and not a copy a program made of it.
 *
 * Why the editor writes through a function it is given rather than to a
 * descriptor.
 *
 *   The division of <heap.h> and <stream.h>, made a third time. The editing —
 *   where the cursor is, what an escape sequence means, which line the history
 *   recalls — is ordinary C and is wrong in ways a test can see; the transfer
 *   beneath it is a system call, and this kernel cannot execute one. So the
 *   editing takes an output function and never names a descriptor, and the
 *   kernel's self-test gives it a function that captures what it wrote and
 *   asserts the bytes — the one place in this project where what a program
 *   *printed* can be asserted rather than read by a person. LineRead, in
 *   libc/line/system.c, is where the descriptors are named, and a program at
 *   privilege level 3 asserts it.
 */

#ifndef OXYS_LIBC_LINE_H
#define OXYS_LIBC_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The greatest length of a line, terminator included, and how many lines the
 * history keeps.
 *
 * A line is bounded because it stands in the editor and not upon a heap: a
 * prompt must work before a heap exists, and a shell whose line editor could
 * fail for want of memory would be a shell that could not report the failure.
 * Five hundred and eleven characters is twice the longest path this kernel
 * accepts and a quarter of the bytes an argument vector may carry. A byte typed
 * beyond it is discarded and counted, not wrapped.
 *
 * The history is thirty-two lines deep, which is sixteen kibibytes of a
 * program's `.bss`, and is a ring: the thirty-third line displaces the oldest.
 */
#define LINE_CAPACITY      512U
#define LINE_HISTORY_DEPTH 32U

/*
 * What one byte did to the line.
 *
 * PENDING is every byte but two kinds. COMPLETE is a carriage return or a line
 * feed — either, because a serial terminal sends the first and the keyboard
 * driver the second, and a program should not have to know which it is
 * connected to. END is control-D upon an empty line, which is what a terminal
 * means by the end of its input; upon a line that is not empty control-D
 * deletes the character under the cursor, as it does everywhere else, and is
 * PENDING.
 */
typedef enum LineResult
{
    LINE_PENDING = 0,
    LINE_COMPLETE = 1,
    LINE_END = 2
} LineResult;

/* Where the editor's output goes. `count` bytes, never a terminated string:
 * the editor emits backspaces and spaces, and a function that stopped at a zero
 * would be a function that could be given none. */
typedef void (*LineOutput)(void *context, const char *bytes, size_t count);

/* The parser's position within a control sequence. Named in the header rather
 * than hidden because the structure holding it is not opaque — a program
 * declares one — and an enumeration a program can see is better than an
 * integer it cannot interpret. */
typedef enum LineParseState
{
    LINE_PARSE_PLAIN = 0,  /* Not within a sequence. */
    LINE_PARSE_ESCAPE = 1, /* ESC received; the next byte says which kind. */
    LINE_PARSE_CSI = 2,    /* ESC [ received; parameters follow. */
    LINE_PARSE_SS3 = 3     /* ESC O received; one final byte follows. */
} LineParseState;

/* The greatest number of parameter and intermediate bytes kept of a control
 * sequence. Every sequence this editor acts upon has at most one; a longer one
 * is parsed to its end and ignored. */
#define LINE_PARAMETER_CAPACITY 8U

/*
 * The editor. Every field is here so that a program may declare one as an
 * object of static storage duration, which is what lets a prompt work before a
 * heap exists; nothing in it is a pointer into anything else.
 *
 * A program may read `text`, `length` and `cursor`, and the accessors below are
 * the promised way to. It must not write them: the display and the fields agree
 * only because every change to the fields went through LineFeed, which wrote
 * the matching change to the display.
 */
typedef struct LineEditor
{
    /* The line being edited, terminated, and its length and cursor in bytes. */
    char text[LINE_CAPACITY];
    size_t length;
    size_t cursor;

    /*
     * The history, oldest first once `history_count` reaches the depth and the
     * ring has turned; `history_first` is the subscript of the oldest line.
     * `browsing` is which of them is shown, and equals `history_count` when
     * none is — when the line shown is the one being typed, which `draft` keeps
     * a copy of while an older line is displayed instead.
     */
    char history[LINE_HISTORY_DEPTH][LINE_CAPACITY];
    size_t history_first;
    size_t history_count;
    size_t browsing;
    char draft[LINE_CAPACITY];
    size_t draft_length;

    /* The parser. */
    LineParseState state;
    char parameter[LINE_PARAMETER_CAPACITY];
    size_t parameter_length;

    /* Where output goes. */
    LineOutput output;
    void *context;

    /* Bytes that did nothing because there was no room for them, and bytes
     * that did nothing because the editor assigns them no meaning. Counted, so
     * that a session which lost keystrokes can say so. */
    uint64_t discarded;
    uint64_t ignored;
} LineEditor;

/*
 * Prepares an editor with no line and no history, writing through `output`.
 * A null `output` is accepted and the editor then displays nothing, which is a
 * legitimate editor for a program whose input is not a terminal.
 */
void LineInitialise(LineEditor *editor, LineOutput output, void *context);

/*
 * Begins a new line: the text is emptied, the cursor placed at its start, the
 * parser reset, and the history's browsing position set past its newest entry.
 * It writes nothing. The prompt is the caller's to write, before this or after,
 * because the editor does not know what the caller wants a prompt to look like
 * and must not be the thing that decides.
 */
void LineBegin(LineEditor *editor);

/*
 * Applies one byte from the terminal, updating the line and the display, and
 * reports whether the line is complete.
 *
 * The bytes acted upon, and what each does:
 *
 *   0x20 to 0x7E        inserted at the cursor
 *   CR, LF              the line is complete; a line feed is written
 *   BS (0x08), DEL      delete the character before the cursor
 *   CSI 3 ~, control-D  delete the character under the cursor
 *   CSI D, SS3 D, control-B      cursor left
 *   CSI C, SS3 C, control-F      cursor right
 *   CSI H, SS3 H, CSI 1 ~, CSI 7 ~, control-A   cursor to the start
 *   CSI F, SS3 F, CSI 4 ~, CSI 8 ~, control-E   cursor to the end
 *   CSI A, SS3 A, control-P      the previous line of the history
 *   CSI B, SS3 B, control-N      the next line of the history, or the draft
 *   control-U           delete everything before the cursor
 *   control-K           delete everything from the cursor on
 *   control-D, empty line        the end of input
 *
 * Every other byte is ignored and counted, a complete sequence with a final
 * byte the editor does not act upon included. An ESC followed by a byte that is
 * neither `[` nor `O` is an escape the editor does not understand; both bytes
 * are ignored.
 *
 * After COMPLETE or END the line stands in the editor until LineBegin, so that
 * the caller may read it and, if it chooses, remember it.
 */
LineResult LineFeed(LineEditor *editor, char byte);

/* The line, terminated; its length; and the cursor's position within it. */
const char *LineText(const LineEditor *editor);
size_t LineLength(const LineEditor *editor);
size_t LineCursor(const LineEditor *editor);

/*
 * Appends the line to the history, unless it is empty or is the same as the
 * newest entry — a person pressing return upon a blank prompt, or running the
 * same command twice, should not find the history filled with those. When the
 * history is full the oldest line is displaced.
 *
 * It is a separate call, and not a thing LineFeed does upon COMPLETE, because
 * whether a line is worth remembering is the caller's decision: a shell that
 * asked for a password would not want it recalled by an arrow key.
 */
void LineRemember(LineEditor *editor);

/* How many lines the history holds, and the line at a position, 0 being the
 * oldest. A position at or beyond the count is a null pointer. */
size_t LineHistoryCount(const LineEditor *editor);
const char *LineHistoryAt(const LineEditor *editor, size_t position);

/*
 * Makes the current line `text`, drawn upon the display with the cursor at its
 * end, so that the person edits it rather than types it afresh. It is what the
 * history's recall does, offered to a caller: `micro`'s `e` command hands a
 * line of a file back through it. Call it after LineBegin — or let LineEdit
 * below do both — and before the first byte is fed; a text longer than the
 * capacity is cut to fit.
 */
void LinePreset(LineEditor *editor, const char *text);

/*
 * Writes `prompt` to the standard output, reads bytes from the standard input
 * until a line is complete, and returns it — or a null pointer at the end of
 * input, which is control-D upon an empty line, or a `read` that failed.
 *
 * This is the one function here that names a descriptor, and it is implemented
 * in libc/line/system.c, apart from the editing, for the reason the header of
 * this file records. The editor's output function is replaced for the duration
 * by one that writes to the standard output, and restored afterwards.
 *
 * The line is not remembered. The caller calls LineRemember if it wants it to
 * be, and before the next LineRead, which begins a new line.
 */
char *LineRead(LineEditor *editor, const char *prompt);

/*
 * LineRead with the line begun as `initial` rather than empty — the text drawn
 * after the prompt, the cursor at its end, and every edit the editor offers
 * applied to it before Return completes it. A null `initial` is LineRead. The
 * text returned is the editor's own line and is the edited text, which may be
 * the initial one unchanged.
 */
char *LineEdit(LineEditor *editor, const char *prompt, const char *initial);

#endif /* OXYS_LIBC_LINE_H */
