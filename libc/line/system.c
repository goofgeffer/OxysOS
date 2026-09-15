/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/line/system.c
 * Purpose: The one place the line editor of libc/line/line.c touches the
 *          system: the prompt written to the standard output, the bytes read
 *          from the standard input, and the editor's own output carried to the
 *          standard output between them.
 * Key functions: LineRead.
 * References:
 *   - libc/include/line.h: the seam this implements, and why the editing is
 *     apart from the descriptors it is used with.
 *   - libc/include/syscall.h: OxysRead and OxysWrite, and what each returns.
 *   - kernel/abi/oxys/syscall_abi.h: descriptor 0, which since sub-task 8.1 is
 *     the terminal, and what a `read` of it promises — at least one byte, and
 *     no more than were typed.
 *   - docs/design/SHELL.md, Section 3.4: the division, and the program at
 *     privilege level 3 that asserts this half of it.
 *
 * This is the arrangement of libc/stdlib/system.c and libc/stdio/system.c a
 * third time, and for the same reason: the editing can be asserted by the
 * kernel's boot-time self-test, which cannot execute a system call, and the
 * transfer can be asserted only by a program, which can. What joins them is
 * this one function.
 *
 * **Nothing here may be called by the kernel.** Every path through LineRead
 * reaches SYSCALL, and the reason the kernel must not is the one every
 * `system.c` in this library records: SYSRET returns to privilege level 3
 * unconditionally.
 *
 * Why the bytes are read one at a time.
 *
 *   Not because the kernel delivers them so — it delivers what has been typed,
 *   up to the length asked for — but because a read that fetched several would
 *   have to hold the ones after the line's end for the *next* line, and the
 *   editor is not the only thing that may read the standard input between two
 *   prompts. A program that reads a line and then reads a character itself
 *   would find the character already taken. One byte per call is the only
 *   size at which nothing is taken ahead of need, and a person types slowly
 *   enough that the calls are not the cost.
 */

#include <line.h>
#include <syscall.h>
#include <string.h>

/* The editor's output, carried to the standard output. A short write is
 * repeated from where it stopped, which the kernel is entitled to require. */
static void LineWriteOutput(void *context, const char *bytes, size_t count)
{
    (void)context;

    while (count > 0U)
    {
        const int64_t written = OxysWrite(SYSCALL_DESCRIPTOR_OUTPUT, bytes, count);

        if (written <= 0)
        {
            return;
        }

        bytes += written;
        count -= (size_t)written;
    }
}

char *LineRead(LineEditor *editor, const char *prompt)
{
    LineOutput previous_output;
    void *previous_context;
    LineResult result = LINE_PENDING;

    if (editor == NULL)
    {
        return NULL;
    }

    previous_output = editor->output;
    previous_context = editor->context;
    editor->output = LineWriteOutput;
    editor->context = NULL;

    LineBegin(editor);

    if (prompt != NULL)
    {
        LineWriteOutput(NULL, prompt, strlen(prompt));
    }

    while (result == LINE_PENDING)
    {
        char byte;
        const int64_t read = OxysRead(SYSCALL_DESCRIPTOR_INPUT, &byte, 1U);

        if (read <= 0)
        {
            /* A read that failed, or one that delivered nothing — which the
             * terminal never does, so it is a standard input that is not the
             * terminal and has ended. Either is the end of input. */
            result = LINE_END;
            break;
        }

        result = LineFeed(editor, byte);
    }

    editor->output = previous_output;
    editor->context = previous_context;

    return (result == LINE_COMPLETE) ? editor->text : NULL;
}
