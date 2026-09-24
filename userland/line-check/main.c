/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/line-check/main.c
 * Purpose: Reads an editing session the kernel's self-test has placed upon the
 *          terminal, through the same LineRead the shell uses, and ends with
 *          the number of lines that were not what that session should have
 *          produced. It asserts the half of sub-task 8.1 the kernel cannot: the
 *          `read` of descriptor 0, and the editor's output reaching descriptor
 *          1, both of which execute SYSCALL.
 * Key functions: main, LineCheckRequire.
 * References:
 *   - kernel/test/libc/line.c: the self-test that injects the session and runs
 *     this program, and the one place the session's bytes are written down.
 *     The lines expected below are what those bytes edit into, and the two
 *     files must agree.
 *   - libc/include/line.h: LineRead, and the table of what each byte does.
 *   - docs/design/SHELL.md: what is asserted at privilege level 3
 *     and what is asserted by the kernel, and why the division falls where it
 *     does.
 *
 * Why the session is not read from a file.
 *
 *   Because the thing under test is that a program's standard input reaches
 *   the terminal. A session read from a file would assert the editor against
 *   bytes that arrived by `open` and `read` of a path, which sub-task 7.6
 *   already asserts, and would say nothing about descriptor 0 at all.
 *
 * Why the editor's output is not discarded.
 *
 *   It goes to the standard output, as the shell's does, and appears in the
 *   boot log with its backspaces — which is untidy and is left so, because a
 *   program that gave the editor a silent output would not be exercising the
 *   path the shell exercises. What the editor writes is asserted byte for byte
 *   by the kernel's half of the test, against a captured output; this half
 *   asserts that the same code, writing through the descriptor, produces the
 *   same lines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <line.h>

/* The lines the session edits into, in order, and what the history should
 * hold afterwards. The fifth line is the fourth recalled and entered again,
 * which the history must not record twice. */
static const char *const LineCheckExpected[] = { "hello", "world", "again", "y", "y" };

#define LINE_CHECK_EXPECTED_COUNT \
    (sizeof LineCheckExpected / sizeof LineCheckExpected[0])

static const char *const LineCheckHistory[] = { "hello", "world", "again", "y" };

#define LINE_CHECK_HISTORY_COUNT \
    (sizeof LineCheckHistory / sizeof LineCheckHistory[0])

static LineEditor LineCheckEditor;

static int LineCheckFailures;

static void LineCheckRequire(int condition, const char *statement)
{
    if (!condition)
    {
        ++LineCheckFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

int main(void)
{
    size_t produced = 0U;

    (void)printf("line-check: the editor reading the terminal through descriptor 0.\n");
    (void)fflush(stdout);

    LineInitialise(&LineCheckEditor, NULL, NULL);

    for (;;)
    {
        const char *line;

        (void)fflush(stdout);
        line = LineRead(&LineCheckEditor, "> ");

        if (line == NULL)
        {
            break;
        }

        if (produced < LINE_CHECK_EXPECTED_COUNT)
        {
            if (strcmp(line, LineCheckExpected[produced]) != 0)
            {
                ++LineCheckFailures;
                (void)printf("  line %u is \"%s\" and not \"%s\" FAILED.\n",
                             (unsigned)(produced + 1U), line, LineCheckExpected[produced]);
            }
        }

        ++produced;
        LineRemember(&LineCheckEditor);
    }

    LineCheckRequire(produced == LINE_CHECK_EXPECTED_COUNT,
                     "the session did not produce the number of lines it should have");

    LineCheckRequire(LineHistoryCount(&LineCheckEditor) == LINE_CHECK_HISTORY_COUNT,
                     "the history does not hold the number of lines it should");

    for (size_t index = 0U; index < LINE_CHECK_HISTORY_COUNT; ++index)
    {
        const char *const entry = LineHistoryAt(&LineCheckEditor, index);

        LineCheckRequire((entry != NULL) && (strcmp(entry, LineCheckHistory[index]) == 0),
                         "a line in the history is not the line that was entered");
    }

    (void)printf("line-check: %d failure(s).\n", LineCheckFailures);
    (void)fflush(stdout);

    return LineCheckFailures;
}
