/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/line.c
 * Purpose: Asserts the line editor of sub-task 8.1 — first by driving the
 *          library's own editing code, in the kernel, with an output function
 *          that captures what it writes; then by placing an editing session
 *          upon the terminal and running the program that reads it through
 *          `read` of descriptor 0 at privilege level 3.
 * Key functions: KernelVerifyLine.
 * References:
 *   - docs/design/SHELL.md, Section 5: the table pairing every property
 *     asserted below with the silent failure the assertion exists to catch.
 *   - libc/include/line.h: the interface, and the table of what each byte does,
 *     which is what the first half asserts byte by byte.
 *   - userland/line-check/main.c: the program the second half runs, and the
 *     lines it expects the session below to edit into. **The two must agree**,
 *     and the session is written down here and nowhere else.
 *   - kernel/test/libc/line_image.asm: the program, embedded.
 *   - kernel/test/libc/utilities.c: the run procedure this repeats, and why a
 *     test repeats it rather than sharing it with the thing it asserts.
 *
 * Why this test can assert what a program printed, when no other can.
 *
 *   docs/design/LIBC.md, Section 12.7, limitation 1, records that nothing in
 *   this kernel can read what a program wrote to the diagnostic path. The editor
 *   is the first thing in this project built so that the limitation does not
 *   apply to it: it writes through a function it is given, and here that
 *   function appends to an array. So the *display* an editing key produces is
 *   asserted — the character written, the tail redrawn, the backspaces that
 *   return the cursor — and not merely the line the keys left behind. A
 *   redraw that left the line right and the screen wrong is exactly the defect
 *   a person would see and no assertion upon the text could.
 *
 * The session, and why it is bytes and not keys.
 *
 *   The program reads bytes, so the session is bytes: the control sequences a
 *   terminal sends, written out as a terminal would send them. That the
 *   keyboard produces the same bytes is asserted separately, by
 *   kernel/test/terminal/terminal.c, from scancodes. The two together say that
 *   a key becomes the right bytes and the right bytes become the right edit;
 *   neither alone would.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>
#include <oxys/terminal/terminal.h>

#include <line.h>
#include <string.h>

extern const uint8_t KernelProgramLineCheckBegin[];
extern const uint8_t KernelProgramLineCheckEnd[];

static bool VerifyLineSucceeded;

static void VerifyLineRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyLineSucceeded = false;
    }
}

/* ---------------------------------------------------------------------------
 * The first half: the editing, captured.
 * ------------------------------------------------------------------------- */

/* Where the editor's output is captured. Large enough for the longest redraw
 * below several times over, and a capture that would overflow is truncated and
 * the truncation reported by the length assertion that follows it. */
#define VERIFY_LINE_CAPTURE_CAPACITY 2048U

static char VerifyLineCapture[VERIFY_LINE_CAPTURE_CAPACITY];
static size_t VerifyLineCaptured;

static void VerifyLineOutput(void *context, const char *bytes, size_t count)
{
    (void)context;

    for (size_t index = 0U; index < count; ++index)
    {
        if (VerifyLineCaptured < VERIFY_LINE_CAPTURE_CAPACITY)
        {
            VerifyLineCapture[VerifyLineCaptured] = bytes[index];
        }

        ++VerifyLineCaptured;
    }
}

static void VerifyLineClearCapture(void)
{
    VerifyLineCaptured = 0U;
}

/* Feeds a string of bytes, returning the result of the last. */
static LineResult VerifyLineFeed(LineEditor *editor, const char *bytes)
{
    LineResult result = LINE_PENDING;

    for (size_t index = 0U; bytes[index] != '\0'; ++index)
    {
        result = LineFeed(editor, bytes[index]);
    }

    return result;
}

/* Whether the capture holds exactly `expected`, compared by length first so
 * that a redraw one byte short is reported as the wrong length. */
static bool VerifyLineCaptureIs(const char *expected)
{
    const size_t length = strlen(expected);

    if (VerifyLineCaptured != length)
    {
        return false;
    }

    return memcmp(VerifyLineCapture, expected, length) == 0;
}

/* The editor under test. Static for the reason the shell's is: it is some
 * seventeen kibibytes. */
static LineEditor VerifyLineEditor;

static void VerifyLineEditing(void)
{
    LineEditor *const editor = &VerifyLineEditor;

    LineInitialise(editor, VerifyLineOutput, NULL);
    LineBegin(editor);

    /* Insertion at the end echoes the character and nothing else. */
    VerifyLineClearCapture();
    VerifyLineRequire(VerifyLineFeed(editor, "abc") == LINE_PENDING,
                      "three printable bytes did not leave the line pending");
    VerifyLineRequire(strcmp(LineText(editor), "abc") == 0,
                      "three printable bytes did not become the line abc");
    VerifyLineRequire(LineCursor(editor) == 3U, "the cursor did not follow an insertion");
    VerifyLineRequire(VerifyLineCaptureIs("abc"),
                      "insertion at the end wrote something other than the characters");

    /* Cursor left is one backspace; insertion in the middle redraws the tail
     * and backspaces over it. */
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1B[D\x1B[D");
    VerifyLineRequire(LineCursor(editor) == 1U, "two cursor-left sequences did not move two");
    VerifyLineRequire(VerifyLineCaptureIs("\b\b"),
                      "cursor left wrote something other than a backspace");

    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "X");
    VerifyLineRequire(strcmp(LineText(editor), "aXbc") == 0,
                      "insertion in the middle did not place the character at the cursor");
    VerifyLineRequire(LineCursor(editor) == 2U,
                      "the cursor did not advance past an insertion in the middle");
    VerifyLineRequire(VerifyLineCaptureIs("Xbc\b\b"),
                      "insertion in the middle did not redraw the tail and return");

    /* Backspace: back over the character, redraw the tail, blank the last
     * position, return. */
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\b");
    VerifyLineRequire(strcmp(LineText(editor), "abc") == 0,
                      "backspace did not remove the character before the cursor");
    VerifyLineRequire(VerifyLineCaptureIs("\bbc \b\b\b"),
                      "backspace did not redraw the tail and blank the vacated position");

    /* DEL is an erase too, as a terminal whose erase character is DEL sends. */
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x7F");
    VerifyLineRequire(strcmp(LineText(editor), "bc") == 0,
                      "DEL did not erase the character before the cursor");
    VerifyLineRequire(LineCursor(editor) == 0U, "DEL at the start of the line moved the cursor");

    /* Backspace at the start does nothing, writes nothing, and is counted. */
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\b");
    VerifyLineRequire(strcmp(LineText(editor), "bc") == 0,
                      "backspace at the start of the line removed something");
    VerifyLineRequire(VerifyLineCaptured == 0U,
                      "backspace at the start of the line wrote something");
    VerifyLineRequire(editor->ignored == 1U,
                      "a backspace that did nothing was not counted as ignored");

    /* Delete under the cursor, by the VT220 sequence: redraw the tail, blank,
     * return. Then by control-D, which upon a non-empty line is the same. */
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1B[3~");
    VerifyLineRequire(strcmp(LineText(editor), "c") == 0,
                      "CSI 3 ~ did not delete the character under the cursor");
    VerifyLineRequire(VerifyLineCaptureIs("c \b\b"),
                      "delete under the cursor did not redraw the tail and blank");

    VerifyLineRequire(VerifyLineFeed(editor, "\x04") == LINE_PENDING,
                      "control-D upon a non-empty line ended the input");
    VerifyLineRequire(LineLength(editor) == 0U,
                      "control-D upon a non-empty line did not delete under the cursor");

    /* End, Home, and the application-mode forms; right at the end is ignored. */
    (void)VerifyLineFeed(editor, "hello");
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1B[H");
    VerifyLineRequire(LineCursor(editor) == 0U, "CSI H did not move the cursor to the start");
    VerifyLineRequire(VerifyLineCaptureIs("\b\b\b\b\b"),
                      "moving to the start wrote something other than backspaces");

    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1BOF");
    VerifyLineRequire(LineCursor(editor) == 5U, "SS3 F did not move the cursor to the end");
    VerifyLineRequire(VerifyLineCaptureIs("hello"),
                      "moving to the end did not rewrite the characters passed over");

    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1B[C");
    VerifyLineRequire((LineCursor(editor) == 5U) && (VerifyLineCaptured == 0U),
                      "cursor right at the end moved or wrote");

    (void)VerifyLineFeed(editor, "\x1B[1~");
    VerifyLineRequire(LineCursor(editor) == 0U, "CSI 1 ~ did not move the cursor to the start");
    (void)VerifyLineFeed(editor, "\x1B[4~");
    VerifyLineRequire(LineCursor(editor) == 5U, "CSI 4 ~ did not move the cursor to the end");
    (void)VerifyLineFeed(editor, "\x01");
    VerifyLineRequire(LineCursor(editor) == 0U, "control-A did not move the cursor to the start");
    (void)VerifyLineFeed(editor, "\x05");
    VerifyLineRequire(LineCursor(editor) == 5U, "control-E did not move the cursor to the end");
    (void)VerifyLineFeed(editor, "\x02\x02");
    VerifyLineRequire(LineCursor(editor) == 3U, "control-B did not move the cursor left");
    (void)VerifyLineFeed(editor, "\x06");
    VerifyLineRequire(LineCursor(editor) == 4U, "control-F did not move the cursor right");

    /* Kill to the end blanks what was removed and returns; kill to the start
     * redraws what is kept over the removed text and blanks the rest. */
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x0B");
    VerifyLineRequire(strcmp(LineText(editor), "hell") == 0,
                      "control-K did not remove everything from the cursor on");
    VerifyLineRequire(VerifyLineCaptureIs(" \b"),
                      "kill to the end did not blank the removed text");

    (void)VerifyLineFeed(editor, "\x1B[D\x1B[D");
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x15");
    VerifyLineRequire(strcmp(LineText(editor), "ll") == 0,
                      "control-U did not remove everything before the cursor");
    VerifyLineRequire(LineCursor(editor) == 0U, "kill to the start left the cursor elsewhere");
    VerifyLineRequire(VerifyLineCaptureIs("\b\bll  \b\b\b\b"),
                      "kill to the start did not redraw the kept text and blank the rest");

    /* An unknown escape, a malformed sequence and an unassigned final byte are
     * each ignored and counted, and none of them touches the line. */
    {
        const uint64_t before = editor->ignored;

        VerifyLineClearCapture();
        (void)VerifyLineFeed(editor, "\x1Bz");
        (void)VerifyLineFeed(editor, "\x1B[\x01");
        (void)VerifyLineFeed(editor, "\x1B[Z");
        (void)VerifyLineFeed(editor, "\t");
        VerifyLineRequire(editor->ignored == before + 4U,
                          "the four bytes and sequences the editor assigns no meaning "
                          "were not each counted once");
        VerifyLineRequire((strcmp(LineText(editor), "ll") == 0) && (VerifyLineCaptured == 0U),
                          "an ignored byte or sequence altered the line or the display");
    }

    /* A carriage return completes the line and writes a line feed; so does a
     * line feed. Both, because the serial line sends one and the keyboard the
     * other. */
    VerifyLineClearCapture();
    VerifyLineRequire(VerifyLineFeed(editor, "\r") == LINE_COMPLETE,
                      "a carriage return did not complete the line");
    VerifyLineRequire(VerifyLineCaptureIs("\n"),
                      "completing the line did not write exactly one line feed");
    VerifyLineRequire(strcmp(LineText(editor), "ll") == 0,
                      "the line did not survive its completion for the caller to read");

    LineBegin(editor);
    (void)VerifyLineFeed(editor, "x");
    VerifyLineRequire(VerifyLineFeed(editor, "\n") == LINE_COMPLETE,
                      "a line feed did not complete the line");

    /* Control-D upon an empty line is the end of input. */
    LineBegin(editor);
    VerifyLineClearCapture();
    VerifyLineRequire(VerifyLineFeed(editor, "\x04") == LINE_END,
                      "control-D upon an empty line did not end the input");
    VerifyLineRequire(VerifyLineCaptureIs("\n"),
                      "the end of input did not write a line feed");

    /* The bound: the five hundred and twelfth character is discarded and
     * counted, and nothing is written for it. */
    LineBegin(editor);

    for (size_t index = 0U; index < LINE_CAPACITY - 1U; ++index)
    {
        (void)LineFeed(editor, 'a');
    }

    VerifyLineClearCapture();
    (void)LineFeed(editor, 'b');
    VerifyLineRequire(LineLength(editor) == LINE_CAPACITY - 1U,
                      "a line grew beyond its capacity");
    VerifyLineRequire((editor->discarded == 1U) && (VerifyLineCaptured == 0U),
                      "a byte beyond the capacity was not discarded silently and counted");
}

static void VerifyLineHistory(void)
{
    LineEditor *const editor = &VerifyLineEditor;

    LineInitialise(editor, VerifyLineOutput, NULL);

    /* An empty line is not remembered, and neither is a repeat of the newest. */
    LineBegin(editor);
    LineRemember(editor);
    VerifyLineRequire(LineHistoryCount(editor) == 0U, "an empty line was remembered");

    LineBegin(editor);
    (void)VerifyLineFeed(editor, "first\r");
    LineRemember(editor);
    LineBegin(editor);
    (void)VerifyLineFeed(editor, "first\r");
    LineRemember(editor);
    VerifyLineRequire(LineHistoryCount(editor) == 1U,
                      "a line equal to the newest entry was remembered twice");

    LineBegin(editor);
    (void)VerifyLineFeed(editor, "second\r");
    LineRemember(editor);
    VerifyLineRequire((LineHistoryCount(editor) == 2U) &&
                          (strcmp(LineHistoryAt(editor, 0U), "first") == 0) &&
                          (strcmp(LineHistoryAt(editor, 1U), "second") == 0),
                      "the history does not hold the two lines in the order entered");
    VerifyLineRequire(LineHistoryAt(editor, 2U) == NULL,
                      "a position beyond the history's count is not a null pointer");

    /*
     * Recall. Up from a draft keeps the draft; up again reaches the older line;
     * up past the oldest is ignored; down returns; down past the newest restores
     * the draft. The redraw of a recall over a longer line blanks the excess.
     */
    LineBegin(editor);
    (void)VerifyLineFeed(editor, "dra");
    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1B[A");
    VerifyLineRequire(strcmp(LineText(editor), "second") == 0,
                      "cursor up did not recall the newest line");
    VerifyLineRequire(VerifyLineCaptureIs("\b\b\bsecond"),
                      "recalling a longer line did not backspace to the start and write it");

    VerifyLineClearCapture();
    (void)VerifyLineFeed(editor, "\x1B[A");
    VerifyLineRequire(strcmp(LineText(editor), "first") == 0,
                      "cursor up twice did not recall the older line");
    VerifyLineRequire(VerifyLineCaptureIs("\b\b\b\b\b\bfirst \b"),
                      "recalling a shorter line did not blank the excess and return");

    {
        const uint64_t before = editor->ignored;

        (void)VerifyLineFeed(editor, "\x1B[A");
        VerifyLineRequire((strcmp(LineText(editor), "first") == 0) &&
                              (editor->ignored == before + 1U),
                          "cursor up past the oldest line was not ignored and counted");
    }

    (void)VerifyLineFeed(editor, "\x1B[B");
    VerifyLineRequire(strcmp(LineText(editor), "second") == 0,
                      "cursor down did not return to the newer line");
    (void)VerifyLineFeed(editor, "\x1BOB");
    VerifyLineRequire(strcmp(LineText(editor), "dra") == 0,
                      "cursor down past the newest line did not restore the draft");
    VerifyLineRequire(LineCursor(editor) == 3U,
                      "the restored draft did not leave the cursor at its end");

    /* A recalled line, edited and entered, is a new entry and the original
     * stands. */
    (void)VerifyLineFeed(editor, "\x1B[A\x1B[Aly\r");
    VerifyLineRequire(strcmp(LineText(editor), "firstly") == 0,
                      "a recalled line could not be edited");
    LineRemember(editor);
    VerifyLineRequire((LineHistoryCount(editor) == 3U) &&
                          (strcmp(LineHistoryAt(editor, 0U), "first") == 0) &&
                          (strcmp(LineHistoryAt(editor, 2U), "firstly") == 0),
                      "entering an edited recall did not add an entry and keep the original");

    /* Control-P and control-N are the same as the arrows. */
    LineBegin(editor);
    (void)VerifyLineFeed(editor, "\x10");
    VerifyLineRequire(strcmp(LineText(editor), "firstly") == 0,
                      "control-P did not recall the newest line");
    (void)VerifyLineFeed(editor, "\x0E");
    VerifyLineRequire(LineLength(editor) == 0U,
                      "control-N did not return to the empty draft");

    /*
     * The ring. Thirty-two entries fill it; the thirty-third displaces the
     * oldest, and the order of the rest is kept. Each line is distinct so that
     * the duplicate rule does not apply.
     */
    LineInitialise(editor, VerifyLineOutput, NULL);

    for (unsigned number = 0U; number < LINE_HISTORY_DEPTH + 1U; ++number)
    {
        char digits[4];

        digits[0] = (char)('a' + (number / 10U));
        digits[1] = (char)('0' + (number % 10U));
        digits[2] = '\r';
        digits[3] = '\0';

        LineBegin(editor);
        (void)VerifyLineFeed(editor, digits);
        LineRemember(editor);
    }

    VerifyLineRequire(LineHistoryCount(editor) == LINE_HISTORY_DEPTH,
                      "the history grew beyond its depth");
    VerifyLineRequire(strcmp(LineHistoryAt(editor, 0U), "a1") == 0,
                      "the thirty-third line did not displace the oldest");
    VerifyLineRequire(strcmp(LineHistoryAt(editor, LINE_HISTORY_DEPTH - 1U), "d2") == 0,
                      "the newest line is not at the end of a full history");

    /* And a recall walks the full ring in order, oldest last. */
    LineBegin(editor);

    for (size_t step = 0U; step < LINE_HISTORY_DEPTH; ++step)
    {
        (void)VerifyLineFeed(editor, "\x1B[A");
    }

    VerifyLineRequire(strcmp(LineText(editor), "a1") == 0,
                      "walking up through a full history did not reach the oldest line");

    /* A remembered line is copied, not referenced: editing the next line does
     * not alter the entry. */
    LineBegin(editor);
    (void)VerifyLineFeed(editor, "kept\r");
    LineRemember(editor);
    LineBegin(editor);
    (void)VerifyLineFeed(editor, "other");
    VerifyLineRequire(strcmp(LineHistoryAt(editor, LINE_HISTORY_DEPTH - 1U), "kept") == 0,
                      "a remembered line changed when the next line was edited");

}

/* ---------------------------------------------------------------------------
 * The second half: the session, the terminal, and the program.
 * ------------------------------------------------------------------------- */

/*
 * The session `line-check` reads. What it edits into is written beside each
 * line; userland/line-check/main.c expects exactly those lines, in this order,
 * and then the end of input.
 *
 * Every family of key is used at least once, so that a key which the keyboard
 * translated rightly and the editor parsed rightly in isolation is also seen
 * to survive the `read` in company: sequences are delivered one byte to a
 * call, and a parser that lost its state between calls would pass every
 * assertion above.
 */
static const char VerifyLineSession[] =
    /* "hello" */
    "hello\r"
    /* "wrold" → Home, Right, Delete → "wold"; Right, "r", End → "world" */
    "wrold\x1B[H\x1B[C\x1B[3~\x1B[C" "r\x1B[F\r"
    /* Up recalls "world", Up "hello", Down "world"; control-A, control-K clears;
     * "again", two backspaces, "in" → "again" */
    "\x1B[A\x1B[A\x1B[B\x01\x0B" "again\b\bin\n"
    /* "draft", Up → "again", Down → "draft"; control-U clears; "x", DEL, "y" */
    "draft\x1B[A\x1B[B\x15" "x\x7Fy\n"
    /* SS3 A recalls "y"; entered again, and the history must not hold it twice */
    "\x1BOA\r"
    /* control-D upon an empty line: the end */
    "\x04";

/* The thread the kernel is executing upon, adopted so that the program has
 * something to return to. */
static Thread *VerifyLineBoot;

/* Runs the embedded program with an empty argument vector and reports the
 * status it ended with. The procedure is kernel/test/libc/utilities.c's, and is
 * not shared with it for the reason that file records. */
static bool VerifyLineRun(int64_t *status)
{
    const uint64_t length = (uint64_t)(KernelProgramLineCheckEnd - KernelProgramLineCheckBegin);
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)(sizeof "line-check");
    memcpy(arguments.storage, "line-check", sizeof "line-check");

    process = ProcessCreate("line-check", NULL);

    if (process == NULL)
    {
        VerifyLineRequire(false, "a process could not be created for line-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramLineCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyLineRequire(false, "line-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyLineRequire(false, "line-check was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyLineRequire(false, "line-check could not be started");

        return false;
    }

    VerifyLineRequire(ThreadCurrent() == VerifyLineBoot,
                      "the kernel did not resume the thread that started line-check");
    VerifyLineRequire(process->state == PROCESS_EXITED,
                      "line-check's process was not marked as ended");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifyLineRequire(VfsOpenFileCount() == open_before,
                      "line-check left an open file behind it");

    return true;
}

static void VerifyLineProgram(void)
{
    int64_t status = 0;
    const uint64_t delivered_before = TerminalBytesDelivered();

    VerifyLineBoot = ThreadAdoptCurrent("boot");

    if (VerifyLineBoot == NULL)
    {
        VerifyLineRequire(false, "the kernel's own flow of control could not be adopted");

        return;
    }

    /*
     * Whatever a person has typed during the boot is discarded before the
     * session is placed, and the session is placed whole before the program
     * starts: the program runs upon this processor, so nothing can add to the
     * queue while it runs except an interrupt — and a keystroke arriving in the
     * middle of the session would be a failure of this test that was really a
     * person at the keyboard. That case is accepted rather than defended
     * against; it is visible in the log as the line that differed.
     */
    TerminalFlush();
    TerminalInject(VerifyLineSession, sizeof VerifyLineSession - 1U);
    VerifyLineRequire(TerminalBytesQueued() == sizeof VerifyLineSession - 1U,
                      "the session did not fit upon the terminal's queue");

    if (VerifyLineRun(&status))
    {
        if (status != 0)
        {
            KernelWriteString("  line-check FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifyLineSucceeded = false;
        }

        /* The program consumed the whole session and nothing beyond it: a
         * reader that took bytes ahead of need, or left some behind, would
         * leave the count wrong in one direction or the other. */
        VerifyLineRequire(TerminalBytesDelivered() - delivered_before ==
                              sizeof VerifyLineSession - 1U,
                          "line-check did not consume exactly the session");
        VerifyLineRequire(TerminalBytesQueued() == 0U,
                          "line-check left bytes of the session unread");
    }

    ThreadDestroy(VerifyLineBoot);
    VerifyLineBoot = NULL;
    TerminalFlush();
}

void KernelVerifyLine(void)
{
    VerifyLineSucceeded = true;

    KernelWriteString("Line editor: asserting the C library's editing, its history, "
                      "and a program reading the terminal.\n");

    VerifyLineEditing();
    VerifyLineHistory();
    VerifyLineProgram();

    if (VerifyLineSucceeded)
    {
        KernelWriteString("Line editor self-test passed: every editing key redrew what it "
                          "should, the history recalled and displaced in order, and "
                          "line-check read a session of ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyLineSession - 1U));
        KernelWriteString(" bytes through descriptor 0 at privilege level 3.\n");
    }
    else
    {
        KernelWriteString("Line editor self-test FAILED.\n");
    }
}
