/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/term.c
 * Purpose: Asserts the two pure halves of the terminal emulator of sub-task 9.6
 *          — the character grid, driven through a session of bytes, and the
 *          translation of a key into what a terminal sends for it — and then
 *          runs poll-check at privilege level 3 for the call the emulator waits
 *          in, which no amount of memory can assert.
 * Key functions: KernelVerifyTerm.
 * References:
 *   - docs/design/TERMINAL.md, Section 6: every assertion here paired with the
 *     silent failure it would catch.
 *   - libc/include/term.h: the seam, and why the grid is in the C library.
 *   - kernel/terminal/terminal.c: the kernel's own translation of the same
 *     keys, which the second half of this compares against byte for byte.
 *
 * Why the key translation is asserted against the kernel's and not against a
 * table written here.
 *
 *   The C library's copy exists because a program under MIT may not link the
 *   LGPL kernel's, and two copies of a table are two things that can drift. A
 *   test that held a third copy would drift with neither. So this asserts that
 *   the library's answer is the kernel's answer, for every key that has one:
 *   the day somebody edits one table, this says the other was not edited.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>
#include <oxys/terminal/terminal.h>

#include <string.h>
#include <term.h>

extern const uint8_t KernelProgramPollCheckBegin[];
extern const uint8_t KernelProgramPollCheckEnd[];

static bool VerifyTermSucceeded;
static Thread *VerifyTermBoot;

/* One grid, reused: it is some kilobytes of cells, and a second would be a
 * second thing to keep in step for no assertion's sake. */
static TermScreen VerifyTermScreen;

static void VerifyTermRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyTermSucceeded = false;
    }
}

/* Writes a terminated string into the grid, which is how every session below
 * is expressed: what a program writes to a terminal is a run of bytes. */
static void VerifyTermPut(const char *text)
{
    TermWrite(&VerifyTermScreen, text, strlen(text));
}

/* Whether a row reads exactly as given, the remainder being spaces. A compare
 * of the whole row and not of its beginning: a row that still carried the tail
 * of what was there before would otherwise pass. */
static bool VerifyTermRowIs(uint32_t row, const char *text)
{
    const char *const actual = TermRow(&VerifyTermScreen, row);
    size_t index = 0U;

    if (actual == NULL)
    {
        return false;
    }

    while (text[index] != '\0')
    {
        if (actual[index] != text[index])
        {
            return false;
        }

        ++index;
    }

    while (index < TermColumns(&VerifyTermScreen))
    {
        if (actual[index] != ' ')
        {
            return false;
        }

        ++index;
    }

    return actual[index] == '\0';
}

/* ---------------------------------------------------------------- the grid */

static void VerifyTermGrid(void)
{
    VerifyTermRequire(!TermInitialise(&VerifyTermScreen, 0U, 4U),
                      "a grid of no columns was accepted");
    VerifyTermRequire(!TermInitialise(&VerifyTermScreen, TERM_COLUMNS_MAXIMUM + 1U, 4U),
                      "a grid wider than the array was accepted");
    VerifyTermRequire(!TermInitialise(&VerifyTermScreen, 8U, TERM_ROWS_MAXIMUM + 1U),
                      "a grid taller than the array was accepted");

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 8U, 3U),
                      "a grid of eight by three was refused");

    VerifyTermRequire(VerifyTermRowIs(0U, ""), "a new grid's first row was not spaces");
    VerifyTermRequire(TermRow(&VerifyTermScreen, 3U) == NULL,
                      "a row beyond the grid was returned rather than refused");
    VerifyTermRequire(TermRowChanged(&VerifyTermScreen, 0U),
                      "a grid nothing has drawn yet did not owe its first row");

    /* --- Printable text, and the wrap. --- */

    VerifyTermPut("abc");
    VerifyTermRequire(VerifyTermRowIs(0U, "abc"), "three characters did not stand in the row");
    VerifyTermRequire(TermCursorColumn(&VerifyTermScreen) == 3U,
                      "the cursor did not advance with the characters");

    /*
     * The wrap is at the moment of writing past the edge and not at the moment
     * of reaching it: a line exactly as wide as the grid leaves the cursor upon
     * its own last column, so that a backspace lands in that line and not in
     * the one beneath.
     */
    VerifyTermPut("defgh");
    VerifyTermRequire(VerifyTermRowIs(0U, "abcdefgh"), "the row did not fill to its width");
    VerifyTermRequire(TermCursorRow(&VerifyTermScreen) == 1U,
                      "writing the last column did not carry the cursor to the next row");
    VerifyTermRequire(TermCursorColumn(&VerifyTermScreen) == 0U,
                      "the wrap did not return the cursor to the left margin");

    /* --- The carriage return, which erases nothing. --- */

    VerifyTermPut("xy\ryz");
    VerifyTermRequire(VerifyTermRowIs(1U, "yz"),
                      "a carriage return did not overwrite from the left margin");

    /* --- The line feed, which is down and to the left margin both. --- */

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 8U, 3U), "the grid could not be reset");
    VerifyTermPut("ab\ncd");
    VerifyTermRequire(VerifyTermRowIs(0U, "ab"), "the first line was disturbed by the feed");
    VerifyTermRequire(VerifyTermRowIs(1U, "cd"),
                      "a line feed did not return to the left margin, so the second line "
                      "began under the end of the first");

    /* --- The backspace, which moves and does not erase, and which crosses. --- */

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 8U, 3U), "the grid could not be reset");
    VerifyTermPut("abc\b");
    VerifyTermRequire(VerifyTermRowIs(0U, "abc"), "a backspace erased where it should move");
    VerifyTermRequire(TermCursorColumn(&VerifyTermScreen) == 2U,
                      "a backspace did not move the cursor");

    /* The line editor erases by writing a space and backspacing again, which is
     * the whole of how a character typed in error disappears. */
    VerifyTermPut(" \b");
    VerifyTermRequire(VerifyTermRowIs(0U, "ab"),
                      "a space written over a character did not erase it");

    /*
     * **The crossing.** The editor wraps a long line across rows and erases it
     * the same way; a backspace that stopped at the left margin would leave the
     * first row of a wrapped line standing for ever, which is what the kernel's
     * console was corrected for in 2026-09-06 and what this inherits.
     */
    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 4U, 3U), "the grid could not be reset");
    VerifyTermPut("abcde");
    VerifyTermRequire(TermCursorRow(&VerifyTermScreen) == 1U, "the long line did not wrap");
    VerifyTermPut("\b\b");
    VerifyTermRequire(TermCursorRow(&VerifyTermScreen) == 0U,
                      "a backspace at the left margin did not cross to the row above");
    VerifyTermRequire(TermCursorColumn(&VerifyTermScreen) == 3U,
                      "the crossing did not land upon the last column of the row above");

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 4U, 3U), "the grid could not be reset");
    VerifyTermPut("\b");
    VerifyTermRequire((TermCursorRow(&VerifyTermScreen) == 0U) &&
                          (TermCursorColumn(&VerifyTermScreen) == 0U),
                      "a backspace at the top left moved off the grid");

    /* --- The scroll. --- */

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 8U, 3U), "the grid could not be reset");
    VerifyTermPut("one\ntwo\nthree");
    VerifyTermRequire(TermScrolls(&VerifyTermScreen) == 0U,
                      "the grid scrolled before it was full");

    VerifyTermPut("\nfour");
    VerifyTermRequire(TermScrolls(&VerifyTermScreen) == 1U,
                      "a line feed upon the last row did not scroll");
    VerifyTermRequire(VerifyTermRowIs(0U, "two"), "the scroll did not carry the rows up");
    VerifyTermRequire(VerifyTermRowIs(1U, "three"), "the scroll lost a row");
    VerifyTermRequire(VerifyTermRowIs(2U, "four"), "the scrolled-in row is not the new line");
    VerifyTermRequire(TermCursorRow(&VerifyTermScreen) == 2U,
                      "the cursor left the last row after a scroll");

    /*
     * Every row is owed after a scroll. A terminal that marked only the row it
     * wrote would draw a screen one line out of date and stay that way until
     * each row happened to be written to.
     */
    for (uint32_t row = 0U; row < TermRows(&VerifyTermScreen); ++row)
    {
        VerifyTermRequire(TermRowChanged(&VerifyTermScreen, row),
                          "a scroll left a row unmarked, so it would be drawn stale");
    }

    /* --- What is drawn, and what is owed. --- */

    for (uint32_t row = 0U; row < TermRows(&VerifyTermScreen); ++row)
    {
        TermRowDrawn(&VerifyTermScreen, row);
    }

    VerifyTermRequire(!TermRowChanged(&VerifyTermScreen, 0U),
                      "a row said to be drawn was still owed");

    VerifyTermPut("!");
    VerifyTermRequire(TermRowChanged(&VerifyTermScreen, 2U),
                      "writing a character did not mark its row");
    VerifyTermRequire(!TermRowChanged(&VerifyTermScreen, 0U),
                      "writing one row marked another, so every keystroke draws the screen");

    /* --- What is dropped. --- */

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 8U, 2U), "the grid could not be reset");
    VerifyTermPut("a\x1B[31m" "b");
    VerifyTermRequire(VerifyTermRowIs(0U, "a[31mb"),
                      "the escape byte itself was drawn, or something other than it was "
                      "dropped");

    VerifyTermPut("\x07");
    VerifyTermRequire(VerifyTermRowIs(0U, "a[31mb"), "a control byte was drawn as a glyph");

    /* --- The tab. --- */

    VerifyTermRequire(TermInitialise(&VerifyTermScreen, 16U, 2U), "the grid could not be reset");
    VerifyTermPut("ab\tc");
    VerifyTermRequire(TermCursorColumn(&VerifyTermScreen) == 9U,
                      "a tab did not advance to the next multiple of eight");
    VerifyTermRequire(VerifyTermRowIs(0U, "ab      c"), "a tab did not leave spaces behind it");
}

/* ------------------------------------------------------------------ the keys */

/* The library's answer for one key, as a terminated string. */
static const char *VerifyTermKey(char character, uint8_t scancode, uint8_t modifiers,
                                 bool extended)
{
    static char bytes[TERM_KEY_BYTES_MAXIMUM + 1U];
    const size_t count =
        TermKeyBytes(character, scancode, modifiers, extended, bytes, TERM_KEY_BYTES_MAXIMUM);

    bytes[count] = '\0';

    return bytes;
}

static void VerifyTermKeys(void)
{
    /* The scancodes of the seven keys that send a sequence, scan code set 1. */
    static const uint8_t extended[] = { 0x47U, 0x48U, 0x4BU, 0x4DU, 0x4FU, 0x50U, 0x53U };

    VerifyTermRequire(strcmp(VerifyTermKey('a', 0x1EU, 0U, false), "a") == 0,
                      "a letter did not become itself");
    VerifyTermRequire(strcmp(VerifyTermKey('A', 0x1EU, TERM_MODIFIER_SHIFT, false), "A") == 0,
                      "a shifted letter did not become itself");

    /*
     * Control-C is byte 3 whether the letter arrived as `c` or as `C`, which is
     * what a terminal sends and what the shell's interrupt is read from. A
     * translation that acted upon the lower case alone would leave control-C
     * with capitals lock on sending the letter C.
     */
    VerifyTermRequire(strcmp(VerifyTermKey('c', 0x2EU, TERM_MODIFIER_CONTROL, false), "\x03") == 0,
                      "control-c did not become byte 3");
    VerifyTermRequire(strcmp(VerifyTermKey('C', 0x2EU, TERM_MODIFIER_CONTROL, false), "\x03") == 0,
                      "control-C did not become byte 3");
    VerifyTermRequire(strcmp(VerifyTermKey('1', 0x02U, TERM_MODIFIER_CONTROL, false), "1") == 0,
                      "control with a digit altered the digit, which a terminal does not");

    VerifyTermRequire(strcmp(VerifyTermKey('\0', 0x2AU, 0U, false), "") == 0,
                      "a key that produces no character contributed bytes");
    VerifyTermRequire(strcmp(VerifyTermKey('\0', 0x3BU, 0U, true), "") == 0,
                      "an extended key with no sequence contributed bytes");

    /*
     * **The library's table and the kernel's are the same table.** The cursor
     * keys work at a window and at a serial line only while they agree, and
     * they are written twice because a program under MIT may not link the
     * kernel's.
     */
    for (size_t index = 0U; index < (sizeof extended / sizeof extended[0]); ++index)
    {
        const char *const mine = TermSequenceFor(extended[index]);
        const char *const kernels = TerminalSequenceForScancode(extended[index]);

        VerifyTermRequire((mine != NULL) && (kernels != NULL) && (strcmp(mine, kernels) == 0),
                          "the library's sequence for a cursor key is not the kernel's, so "
                          "that key behaves differently at a window and at a serial line");

        VerifyTermRequire(strcmp(VerifyTermKey('\0', extended[index], 0U, true), kernels) == 0,
                          "an extended key did not produce the sequence its table holds");
    }

    VerifyTermRequire(TermSequenceFor(0x01U) == NULL,
                      "a scancode with no sequence was given one");
}

/* ------------------------------------------------- poll-check, at level 3 */

static bool VerifyTermRun(int64_t *status)
{
    const uint64_t length = (uint64_t)(KernelProgramPollCheckEnd - KernelProgramPollCheckBegin);
    static const char name[] = "poll-check";
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)sizeof name;

    for (size_t index = 0U; index < sizeof name; ++index)
    {
        arguments.storage[index] = name[index];
    }

    process = ProcessCreate(name, NULL);

    if (process == NULL)
    {
        VerifyTermRequire(false, "a process could not be created for poll-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramPollCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyTermRequire(false, "poll-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyTermRequire(false, "poll-check was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyTermRequire(false, "poll-check could not be started");

        return false;
    }

    VerifyTermRequire(process->state == PROCESS_EXITED,
                      "poll-check's process was not marked as ended");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifyTermRequire(VfsOpenFileCount() == open_before,
                      "poll-check left an open file behind it");

    return true;
}

void KernelVerifyTerm(void)
{
    int64_t status = 0;

    VerifyTermSucceeded = true;

    KernelWriteString("Terminal emulator: asserting the grid and the keys in memory, then "
                      "running poll-check.\n");

    VerifyTermGrid();
    VerifyTermKeys();

    VerifyTermBoot = ThreadAdoptCurrent("boot");

    if (VerifyTermBoot == NULL)
    {
        VerifyTermRequire(false, "the kernel's own flow of control could not be adopted");
    }
    else
    {
        if (VerifyTermRun(&status) && (status != 0))
        {
            KernelWriteString("  poll-check FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifyTermSucceeded = false;
        }

        ThreadDestroy(VerifyTermBoot);
        VerifyTermBoot = NULL;
    }

    KernelWriteString(VerifyTermSucceeded
                          ? "Terminal emulator self-test passed: the grid's writing, wrapping, "
                            "backspace and scroll; the keys, agreeing with the kernel's own "
                            "table; and poll, waited in from privilege level 3.\n"
                          : "Terminal emulator self-test FAILED.\n");
}
