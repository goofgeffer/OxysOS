/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/main.c
 * Purpose: The shell of Phase 8, as far as sub-task 8.4 takes it: a prompt, a
 *          line read through the editor with its history, the line tokenised
 *          and parsed — continued upon a second prompt where it is incomplete
 *          — its words expanded, and the built-ins `cd`, `pwd`, `export` and
 *          `exit` run, and every other command sought upon PATH, forked,
 *          executed with the exported environment, and waited for.
 * Key functions: main, ShellReadCommand, ShellRunList, ShellRunCommand,
 *          ShellLookupParameter, ShellComplain.
 * References:
 *   - IEEE Std 1003.1-2017, `sh`: the utility this will become, and its PS1
 *     and PS2 — the prompt, and the prompt for a line that continues one.
 *   - IEEE Std 1003.1-2017, Section 2.3, rule 1 and Section 2.10: a line that
 *     ends inside a quote or after an operator is not a complete command, and
 *     an interactive shell asks for the rest.
 *   - IEEE Std 1003.1-2017, Section 2.9.1 (Simple Commands): assignments
 *     before a name apply to the shell where there is no name; Section 2.9.2,
 *     `!`; Section 2.9.3, `&&` and `||`; Section 2.8.2, the status 127 of a
 *     command that could not be found; Section 2.14, which built-ins are
 *     special.
 *   - libc/include/line.h: the editor, and the table of what each key does.
 *   - userland/sh/shell.h: the tokeniser, the parser, the expansion, the
 *     variables and the built-ins.
 *   - docs/design/SHELL.md, Sections 4, 8 and 11 to 15.
 *
 * What this program is at sub-task 8.4, stated plainly so that nobody mistakes
 * it for more.
 *
 *   It prompts, reads, parses, expands `$NAME` and `$?`, sets variables, runs
 *   four built-ins, and runs every other command as a program: sought upon
 *   PATH, forked, executed with the exported variables as its environment,
 *   and waited for. `&&` and `||` are honoured, `!` inverts, and `$?` reports.
 *   Redirections are named and not performed, a pipeline of more than one
 *   command is refused, `&` is recorded and not honoured, and nothing can be
 *   interrupted: 8.5, 8.6 and 8.7 in turn.
 *
 * How a line continues.
 *
 *   A quote left open, or a `|`, `&&` or `||` with nothing after it, is a
 *   command that is not finished. The shell keeps what was typed, prompts
 *   with `> ` — the PS2 of every shell of this lineage — and appends the next
 *   line with the newline between them, which is a character inside the quote
 *   and a blank outside it. The whole is parsed again from the start; a
 *   parser that resumed from the middle would be a second parser.
 *
 * How it ends.
 *
 *   `exit [n]`, since 8.3, or control-D upon an empty line as before. The
 *   status the shell ends with is `exit`'s operand, or the last command's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <line.h>

#include "shell.h"

/* The prompts: PS1, and PS2 for the rest of a line that continues. */
#define SHELL_PROMPT       "oxys$ "
#define SHELL_CONTINUATION "> "

/*
 * A command may span several lines, so it is accumulated here; the bound is
 * the tokeniser's text bound, which is what a line of that length would need.
 */
#define SHELL_COMMAND_BYTES SHELL_TEXT_MAXIMUM

/* All of static storage duration, for the reason the editor's header gives:
 * a shell whose prompt could fail for want of memory could not say so. */
static LineEditor ShellEditor;
static char ShellCommandText[SHELL_COMMAND_BYTES];
static ShellTokens ShellCommandTokens;
static ShellList ShellCommandList;

/*
 * Reads one complete command into ShellCommandText, continuing across lines,
 * and leaves its tokens and structure in the two objects above. Returns the
 * parse status — OK, EMPTY, or the refusal — or INCOMPLETE only where the
 * input ended in the middle of a command. `ended` is set where the input
 * ended, whichever status accompanies it.
 */
static ShellParseStatus ShellReadCommand(bool *ended)
{
    size_t used = 0U;
    const char *prompt = SHELL_PROMPT;

    *ended = false;
    ShellCommandText[0] = '\0';

    for (;;)
    {
        const char *line;
        size_t length;
        ShellParseStatus status;

        (void)fflush(stdout);
        line = LineRead(&ShellEditor, prompt);

        if (line == NULL)
        {
            *ended = true;

            return (used == 0U) ? SHELL_PARSE_EMPTY : SHELL_PARSE_INCOMPLETE;
        }

        /* Each line is remembered on its own, as it was typed: a recalled
         * continuation is a thing a person may want, and a recalled command
         * of several lines would not fit the editor's line. */
        LineRemember(&ShellEditor);

        length = strlen(line);

        if (used + length + 2U > SHELL_COMMAND_BYTES)
        {
            return SHELL_PARSE_TOO_MANY_TOKENS;
        }

        if (used > 0U)
        {
            ShellCommandText[used++] = '\n';
        }

        memcpy(&ShellCommandText[used], line, length + 1U);
        used += length;

        status = ShellTokenise(ShellCommandText, &ShellCommandTokens);

        if (status == SHELL_PARSE_OK)
        {
            status = ShellParse(&ShellCommandTokens, &ShellCommandList, NULL);
        }

        if (status != SHELL_PARSE_INCOMPLETE)
        {
            return status;
        }

        prompt = SHELL_CONTINUATION;
    }
}

/* ---------------------------------------------------------------------------
 * Execution, as far as sub-task 8.3 takes it: the built-ins run, and every
 * other command is described and reported as not found.
 * ------------------------------------------------------------------------- */

/* The status of the last command run, which `$?` expands to and `exit` with
 * no operand ends with. Zero at the start, as Section 2.5.2 has it. */
static int ShellLastStatus;

/* What `$?` expands to: the last status, written into a buffer of this
 * function's own, the lookup returning a pointer and not a copy. */
static char ShellStatusText[8];

static const char *ShellLookupParameter(void *context, const char *name)
{
    (void)context;

    if (strcmp(name, "?") == 0)
    {
        (void)snprintf(ShellStatusText, sizeof ShellStatusText, "%d", ShellLastStatus);

        return ShellStatusText;
    }

    return ShellVariableGet(name);
}

/*
 * The expanded words of one command, and the vector built over them. The
 * words are bounded by the parser and each expansion by LINE_CAPACITY, so the
 * storage is fixed, for the reason everything else in this program is.
 */
static char ShellArguments[SHELL_WORD_MAXIMUM][LINE_CAPACITY];
static char *ShellArgumentVector[SHELL_WORD_MAXIMUM + 1U];
static char ShellAssignmentText[LINE_CAPACITY];

/* Applies one assignment word to the shell's variables, after expanding its
 * value. Returns false, having said why, where it cannot. */
static bool ShellApplyAssignment(const char *word)
{
    const size_t name_length = ShellIsAssignmentWord(word);
    char name[SHELL_NAME_MAXIMUM + 1U];

    if (!ShellExpandWord(&word[name_length + 1U], ShellAssignmentText, sizeof ShellAssignmentText,
                         ShellLookupParameter, NULL))
    {
        (void)fprintf(stderr, "sh: an assignment's value could not be expanded.\n");

        return false;
    }

    memcpy(name, word, name_length);
    name[name_length] = '\0';

    if (!ShellVariableSet(name, ShellAssignmentText))
    {
        (void)fprintf(stderr, "sh: %s: cannot be set.\n", name);

        return false;
    }

    return true;
}

/*
 * Runs one simple command, and returns its status.
 *
 * A command with no name is its assignments, applied to the shell (Section
 * special built-in (Section 2.14) and not otherwise. Anything else is a
 * program, since 8.4: sought upon PATH, forked, executed with the exported
 * environment and waited for by run.c, with the 127 of Section 2.8.2 where
 * it could not be found. Redirections are named and not performed until 8.5.
 * Redirections are named and not performed until 8.5.
 */
static int ShellRunCommand(const ShellCommand *command, bool *exit_requested)
{
    int argc = 0;
    bool special;

    if (command->word_count == 0U)
    {
        for (size_t index = 0U; index < command->assignment_count; ++index)
        {
            if (!ShellApplyAssignment(command->assignment[index]))
            {
                return 1;
            }
        }

        return 0;
    }

    for (size_t index = 0U; index < command->word_count; ++index)
    {
        if (!ShellExpandWord(command->word[index], ShellArguments[index],
                             sizeof ShellArguments[index], ShellLookupParameter, NULL))
        {
            (void)fprintf(stderr, "sh: a word could not be expanded.\n");

            return 1;
        }

        ShellArgumentVector[argc++] = ShellArguments[index];
    }

    ShellArgumentVector[argc] = NULL;

    if (!ShellIsBuiltin(ShellArgumentVector[0]))
    {
        /*
         * A program, since sub-task 8.4. Its redirections are still named and
         * not performed, 8.5 being where a descriptor is redirected; the
         * diagnostic says so rather than letting `ls > out` print to the
         * screen with no word about the file that was not made.
         */
        if (command->redirection_count > 0U)
        {
            (void)fprintf(stderr, "sh: %s: redirections are not performed until sub-task 8.5.\n",
                          ShellArgumentVector[0]);
        }

        return ShellRunProgram(ShellArgumentVector);
    }

    special = ShellIsSpecialBuiltin(ShellArgumentVector[0]);

    for (size_t index = 0U; special && (index < command->assignment_count); ++index)
    {
        if (!ShellApplyAssignment(command->assignment[index]))
        {
            return 1;
        }
    }

    if (command->redirection_count > 0U)
    {
        (void)fprintf(stderr, "sh: %s: redirections are not performed until sub-task 8.5.\n",
                      ShellArgumentVector[0]);
    }

    return ShellRunBuiltin(argc, ShellArgumentVector, ShellLastStatus, exit_requested);
}

/*
 * Runs a list: each pipeline in order, subject to the condition that joins it
 * to the one before (Section 2.9.3), `!` inverting its status (2.9.2), and a
 * pipeline of more than one command reported as not runnable until 8.6. The
 * `&` separator is recorded and not honoured: nothing runs in the background
 * until there is something to run.
 */
static void ShellRunList(const ShellList *list, bool *exit_requested)
{
    for (size_t index = 0U; (index < list->pipeline_count) && !*exit_requested; ++index)
    {
        const ShellPipeline *const pipeline = &list->pipeline[index];
        int status;

        if ((pipeline->condition == SHELL_CONDITION_AND_IF) && (ShellLastStatus != 0))
        {
            continue;
        }

        if ((pipeline->condition == SHELL_CONDITION_OR_IF) && (ShellLastStatus == 0))
        {
            continue;
        }

        if (pipeline->command_count > 1U)
        {
            (void)printf("sh: a pipeline of %u commands cannot run until sub-task 8.6.\n",
                         (unsigned)pipeline->command_count);
            status = 127;
        }
        else
        {
            status = ShellRunCommand(&pipeline->command[0], exit_requested);
        }

        if (pipeline->negated && !*exit_requested)
        {
            status = (status == 0) ? 1 : 0;
        }

        ShellLastStatus = status;
    }
}

/* The diagnostic for a line the parser refused, naming the token where it
 * stopped: the standard error, as every diagnostic goes. */
static void ShellComplain(ShellParseStatus status)
{
    (void)fflush(stdout);

    if (status == SHELL_PARSE_INCOMPLETE)
    {
        (void)fprintf(stderr, "sh: syntax error: the input ended inside a command.\n");

        return;
    }

    if ((status == SHELL_PARSE_UNEXPECTED_TOKEN) || (status == SHELL_PARSE_UNSUPPORTED) ||
        (status == SHELL_PARSE_BAD_DESCRIPTOR))
    {
        size_t offending = 0U;
        const ShellToken *token;

        /* Parsed a second time, for the position: the first parse was made
         * by ShellReadCommand, which does not keep it, and a parse is cheap. */
        (void)ShellParse(&ShellCommandTokens, &ShellCommandList, &offending);
        token = &ShellCommandTokens.token[offending];

        if (token->kind == SHELL_TOKEN_WORD)
        {
            (void)fprintf(stderr, "sh: syntax error: %s near `%s'.\n",
                          ShellParseStatusName(status), token->text);
        }
        else
        {
            (void)fprintf(stderr, "sh: syntax error: %s near `%s'.\n",
                          ShellParseStatusName(status), ShellTokenKindText(token->kind));
        }

        return;
    }

    (void)fprintf(stderr, "sh: %s.\n", ShellParseStatusName(status));
}

int main(void)
{
    bool exit_requested = false;

    LineInitialise(&ShellEditor, NULL, NULL);
    ShellVariablesInitialise();

    /* PWD is set from the kernel, which begins every program at the root, so
     * that `$PWD` means something before the first `cd`. */
    {
        char directory[SHELL_VALUE_MAXIMUM + 1U];

        if (OxysGetWorkingDirectory(directory, sizeof directory) >= 0)
        {
            (void)ShellVariableSet("PWD", directory);
            (void)ShellVariableExport("PWD");
        }
    }

    (void)printf("The Oxys-OS shell, sub-task 8.4: programs run from /bin with the exported\n"
                 "environment; cd, pwd, export and exit are built in; `exit' or control-D ends\n"
                 "the shell. No redirection, no pipeline, no job control yet.\n");

    while (!exit_requested)
    {
        bool ended;
        const ShellParseStatus status = ShellReadCommand(&ended);

        if (status == SHELL_PARSE_OK)
        {
            ShellRunList(&ShellCommandList, &exit_requested);
        }
        else if (status != SHELL_PARSE_EMPTY)
        {
            ShellComplain(status);
            ShellLastStatus = 2;
        }

        if (ended)
        {
            break;
        }
    }

    if (exit_requested)
    {
        (void)fflush(stdout);

        return ShellLastStatus;
    }

    (void)printf("sh: end of input.\n");

    return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
