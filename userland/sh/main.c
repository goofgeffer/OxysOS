/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/main.c
 * Purpose: The shell of Phase 8, as far as sub-task 8.2 takes it: a prompt, a
 *          line read through the editor with its history, the line tokenised
 *          and parsed into the command structure — continued upon a second
 *          prompt where it is incomplete — and, there being nothing yet that
 *          runs a command, the structure described back to the person who
 *          typed it.
 * Key functions: main, ShellReadCommand, ShellDescribe, ShellComplain.
 * References:
 *   - IEEE Std 1003.1-2017, `sh`: the utility this will become, and its PS1
 *     and PS2 — the prompt, and the prompt for a line that continues one.
 *   - IEEE Std 1003.1-2017, Section 2.3, rule 1 and Section 2.10: a line that
 *     ends inside a quote or after an operator is not a complete command, and
 *     an interactive shell asks for the rest.
 *   - libc/include/line.h: the editor, and the table of what each key does.
 *   - userland/sh/shell.h: the tokeniser and the parser.
 *   - docs/design/SHELL.md, Sections 4 and 8.
 *
 * What this program is at sub-task 8.2, stated plainly so that nobody mistakes
 * it for more.
 *
 *   It prompts, reads, parses and describes. Every word is unquoted and every
 *   redirection named, so that a person can see what the shell understood of
 *   the line; nothing is expanded and nothing is run. That is what a tokeniser
 *   and a parser can be seen to do before there is anything to hand the
 *   structure to, and it is shipped in this state for the reason 8.1's was.
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
 *   Control-D upon an empty line. There is no `exit` word yet; that is a
 *   built-in of sub-task 8.3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* The text of a redirection operator, Section 2.7, for the description. */
static const char *ShellRedirectionText(ShellRedirectionKind kind)
{
    switch (kind)
    {
    case SHELL_REDIRECT_INPUT:         return "<";
    case SHELL_REDIRECT_OUTPUT:        return ">";
    case SHELL_REDIRECT_CLOBBER:       return ">|";
    case SHELL_REDIRECT_APPEND:        return ">>";
    case SHELL_REDIRECT_DUPLICATE_IN:  return "<&";
    case SHELL_REDIRECT_DUPLICATE_OUT: return ">&";
    case SHELL_REDIRECT_READ_WRITE:    return "<>";
    default:                           return "?";
    }
}

/* Prints a word with its quotes removed, between brackets so that a word
 * holding a space, or an empty one, is seen for what it is. */
static void ShellPrintWord(const char *word)
{
    char unquoted[LINE_CAPACITY];

    if (!ShellUnquote(word, unquoted, sizeof unquoted))
    {
        (void)printf("[?]");

        return;
    }

    (void)printf("[%s]", unquoted);
}

/*
 * Describes what was parsed: one line per pipeline, its commands joined by
 * `|`, each command's words in brackets and its redirections after them,
 * and the condition before and the separator after, where there is one.
 */
static void ShellDescribe(const ShellList *list)
{
    (void)printf("sh: parsed %u pipeline(s); nothing runs until sub-task 8.4:\n",
                 (unsigned)list->pipeline_count);

    for (size_t index = 0U; index < list->pipeline_count; ++index)
    {
        const ShellPipeline *const pipeline = &list->pipeline[index];

        (void)printf("  %u:", (unsigned)(index + 1U));

        if (pipeline->condition == SHELL_CONDITION_AND_IF)
        {
            (void)printf(" &&");
        }
        else if (pipeline->condition == SHELL_CONDITION_OR_IF)
        {
            (void)printf(" ||");
        }

        if (pipeline->negated)
        {
            (void)printf(" !");
        }

        for (size_t which = 0U; which < pipeline->command_count; ++which)
        {
            const ShellCommand *const command = &pipeline->command[which];

            if (which > 0U)
            {
                (void)printf(" |");
            }

            for (size_t word = 0U; word < command->word_count; ++word)
            {
                (void)printf(" ");
                ShellPrintWord(command->word[word]);
            }

            for (size_t redirection = 0U; redirection < command->redirection_count; ++redirection)
            {
                const ShellRedirection *const io = &command->redirection[redirection];

                (void)printf(" %d%s", io->descriptor, ShellRedirectionText(io->kind));
                ShellPrintWord(io->target);
            }
        }

        if (pipeline->separator == SHELL_SEPARATOR_BACKGROUND)
        {
            (void)printf(" &");
        }
        else if (pipeline->separator == SHELL_SEPARATOR_SEQUENCE)
        {
            (void)printf(" ;");
        }

        (void)printf("\n");
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
    LineInitialise(&ShellEditor, NULL, NULL);

    (void)printf("The Oxys-OS shell, sub-task 8.2: a line editor, a tokeniser and a parser. "
                 "Arrow keys\nedit and recall; a line that continues is prompted for; "
                 "control-D upon an empty\nline ends the shell.\n");

    for (;;)
    {
        bool ended;
        const ShellParseStatus status = ShellReadCommand(&ended);

        if (status == SHELL_PARSE_OK)
        {
            ShellDescribe(&ShellCommandList);
        }
        else if (status != SHELL_PARSE_EMPTY)
        {
            ShellComplain(status);
        }

        if (ended)
        {
            break;
        }
    }

    (void)printf("sh: end of input.\n");

    return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
