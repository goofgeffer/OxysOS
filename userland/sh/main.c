/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/main.c
 * Purpose: The shell of Phase 8, as far as sub-task 8.1 takes it: a prompt, a
 *          line read through the editor with its history, and — there being no
 *          tokeniser yet — the line handed back to the person who typed it.
 * Key functions: main, ShellAnswer.
 * References:
 *   - IEEE Std 1003.1-2017, `sh`: the utility this will become. Nothing of its
 *     grammar is here yet; sub-task 8.2 is the tokeniser and the parser.
 *   - libc/include/line.h: the editor, and the table of what each key does.
 *   - docs/design/SHELL.md, Section 4: what this program does at this sub-task,
 *     why it echoes rather than executes, and how it ends.
 *
 * What this program is at sub-task 8.1, stated plainly so that nobody mistakes
 * it for more.
 *
 *   It prompts, it reads a line with every editing key the library offers, it
 *   keeps the lines in a history the arrow keys walk, and it prints each line
 *   back preceded by a statement that nothing runs it. It executes nothing,
 *   parses nothing and expands nothing. That is the whole of what "line editing
 *   with history" can be seen to do before there is a command to edit a line
 *   *for*, and it is shipped in this state rather than held back until 8.4
 *   because it is the first thing upon this system a person can type at and be
 *   answered by — which is a property worth having on the day it exists.
 *
 * How it ends.
 *
 *   Control-D upon an empty line, which is what a terminal means by the end of
 *   its input, and which every shell of this lineage treats as `exit`. There is
 *   no `exit` word yet; that is a built-in of sub-task 8.3, and a word this
 *   program recognised specially would be the beginning of a parser written in
 *   the wrong file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <line.h>

/* The prompt. A name and a sigil, so that the transcript of a session says
 * which program was asking. */
#define SHELL_PROMPT "oxys$ "

/*
 * The editor is an object of static storage duration and not a local, for the
 * reason its header gives: it is some seventeen kibibytes, and a program's
 * initial stack is not the place for it.
 */
static LineEditor ShellEditor;

/* What the shell does with a line at this sub-task: says what it cannot do
 * with it, and shows the line so that the editing can be seen to have worked. */
static void ShellAnswer(const char *line)
{
    (void)printf("sh: no tokeniser yet, so nothing runs: %s\n", line);
}

int main(void)
{
    LineInitialise(&ShellEditor, NULL, NULL);

    (void)printf("The Oxys-OS shell, sub-task 8.1: a prompt and a line editor. "
                 "Arrow keys edit and recall;\ncontrol-D upon an empty line ends "
                 "the shell.\n");

    for (;;)
    {
        const char *line;

        /*
         * Everything printed so far must reach the terminal before the prompt
         * does, and the prompt is written beneath the stream by the editor —
         * so the stream is emptied first, or the answer to the last line would
         * appear after the prompt for the next.
         */
        (void)fflush(stdout);

        line = LineRead(&ShellEditor, SHELL_PROMPT);

        if (line == NULL)
        {
            break;
        }

        if (line[0] == '\0')
        {
            continue;
        }

        LineRemember(&ShellEditor);
        ShellAnswer(line);
    }

    (void)printf("sh: end of input.\n");

    return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
