/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/parser.c
 * Purpose: The command parser of sub-task 8.2: builds, from a line's tokens,
 *          the list of pipelines of simple commands with their redirections
 *          that IEEE Std 1003.1-2017, Section 2.10, defines — the subset of
 *          that grammar this shell implements, with everything outside the
 *          subset refused by name.
 * Key functions: ShellParse, ShellParseStatusName.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.10, the grammar: `list`, `and_or`,
 *     `pipeline`, `pipe_sequence`, `simple_command`, `cmd_prefix`,
 *     `cmd_suffix` and `io_redirect`, of which this is a recursive-descent
 *     reading in the same order.
 *   - IEEE Std 1003.1-2017, Section 2.10.2, rule 1: a word that is a reserved
 *     word is recognised only where the grammar permits one; `!` at the start
 *     of a pipeline is the one reserved word this shell acts upon.
 *   - IEEE Std 1003.1-2017, Section 2.9.1: the words of a simple command, in
 *     order, the first being the command name; and Section 2.7, the default
 *     descriptor of each redirection operator.
 *   - userland/sh/shell.h: the structure built here and the bounds upon it.
 *   - docs/design/SHELL.md, Section 8.
 *
 * What of the grammar is refused, and why by name.
 *
 *   Compound commands — `if`, `while`, `for`, `case`, `{ }` and `( )` —
 *   function definitions, and the here-document. Each is recognised as what
 *   it is and answered with UNSUPPORTED naming the token, rather than parsed
 *   as a simple command whose first word happens to be `if`: a shell that ran
 *   a program called `if` because it did not know the word would have failed
 *   in a way that looked like a missing program. The reserved words are
 *   checked only in command position, as rule 1 requires, so `echo if` is the
 *   word `if` and `if true` is a refusal.
 *
 *   Nothing here executes, and that is the point of the sub-task: 8.3 and 8.4
 *   act upon this structure and are asserted against it. A parser that also
 *   ran what it parsed would be a parser asserted only by running things.
 */

#include "shell.h"

#include <string.h>

/* The reserved words of Section 2.4 that begin a compound command or a
 * function, which this shell refuses in command position. */
static const char *const ShellCompoundWords[] = {
    "if", "then", "else", "elif", "fi", "do", "done", "case", "esac",
    "while", "until", "for", "in", "{", "}", "function"
};

#define SHELL_COMPOUND_WORD_COUNT \
    (sizeof ShellCompoundWords / sizeof ShellCompoundWords[0])

static bool ShellIsCompoundWord(const char *word)
{
    for (size_t index = 0U; index < SHELL_COMPOUND_WORD_COUNT; ++index)
    {
        if (strcmp(word, ShellCompoundWords[index]) == 0)
        {
            return true;
        }
    }

    return false;
}

/* The parser's position, and where it stopped if it stopped. */
typedef struct ShellParser
{
    const ShellTokens *tokens;
    size_t at;
} ShellParser;

static const ShellToken *ShellPeek(const ShellParser *parser)
{
    return &parser->tokens->token[parser->at];
}

static void ShellAdvance(ShellParser *parser)
{
    if (parser->at + 1U < parser->tokens->count)
    {
        ++parser->at;
    }
}

/* Whether a token is a redirection operator, and if so what it means and
 * which descriptor it acts upon by default. Section 2.7. */
static bool ShellRedirectionOf(ShellTokenKind kind, ShellRedirectionKind *meaning,
                               int *descriptor)
{
    switch (kind)
    {
    case SHELL_TOKEN_LESS:      *meaning = SHELL_REDIRECT_INPUT;         *descriptor = 0; return true;
    case SHELL_TOKEN_GREAT:     *meaning = SHELL_REDIRECT_OUTPUT;        *descriptor = 1; return true;
    case SHELL_TOKEN_CLOBBER:   *meaning = SHELL_REDIRECT_CLOBBER;       *descriptor = 1; return true;
    case SHELL_TOKEN_DGREAT:    *meaning = SHELL_REDIRECT_APPEND;        *descriptor = 1; return true;
    case SHELL_TOKEN_LESSAND:   *meaning = SHELL_REDIRECT_DUPLICATE_IN;  *descriptor = 0; return true;
    case SHELL_TOKEN_GREATAND:  *meaning = SHELL_REDIRECT_DUPLICATE_OUT; *descriptor = 1; return true;
    case SHELL_TOKEN_LESSGREAT: *meaning = SHELL_REDIRECT_READ_WRITE;    *descriptor = 0; return true;
    default:
        return false;
    }
}

/* Reads an io_number's digits into a descriptor, refusing one beyond what a
 * process may hold: the largest this kernel gives out is small, and a number
 * of ten digits is a mistake and not a descriptor. */
static bool ShellDescriptorOf(const ShellToken *token, int *descriptor)
{
    int value = 0;

    if (token->length > 3U)
    {
        return false;
    }

    for (size_t index = 0U; index < token->length; ++index)
    {
        value = (value * 10) + (token->text[index] - '0');
    }

    *descriptor = value;

    return true;
}

/*
 * simple_command: words and redirections in any order, Section 2.9.1 keeping
 * the words in the order given and the redirections in theirs. Returns OK
 * having consumed at least one word or redirection, or the status that
 * stopped it.
 */
static ShellParseStatus ShellParseSimpleCommand(ShellParser *parser, ShellCommand *command)
{
    command->word_count = 0U;
    command->redirection_count = 0U;

    for (;;)
    {
        const ShellToken *const token = ShellPeek(parser);
        ShellRedirectionKind meaning;
        int descriptor;

        if (token->kind == SHELL_TOKEN_WORD)
        {
            /* Rule 1 of 2.10.2: a reserved word is one only in command
             * position — the first word — and there it is refused. */
            if ((command->word_count == 0U) && ShellIsCompoundWord(token->text))
            {
                return SHELL_PARSE_UNSUPPORTED;
            }

            if (command->word_count >= SHELL_WORD_MAXIMUM)
            {
                return SHELL_PARSE_TOO_MANY_WORDS;
            }

            command->word[command->word_count++] = token->text;
            ShellAdvance(parser);
            continue;
        }

        if (token->kind == SHELL_TOKEN_IO_NUMBER)
        {
            const ShellToken *operator;
            const ShellToken *target;
            ShellRedirection *redirection;
            int number;

            if (!ShellDescriptorOf(token, &number))
            {
                return SHELL_PARSE_BAD_DESCRIPTOR;
            }

            ShellAdvance(parser);
            operator = ShellPeek(parser);

            if (!ShellRedirectionOf(operator->kind, &meaning, &descriptor))
            {
                /* The tokeniser only makes an io_number before < or >, so
                 * this is << or <<- : a here-document, refused by name. */
                return SHELL_PARSE_UNSUPPORTED;
            }

            /* The io_number names the descriptor; the operator's default,
             * which ShellRedirectionOf wrote, is what it replaces. */
            descriptor = number;
            ShellAdvance(parser);
            target = ShellPeek(parser);

            if (target->kind == SHELL_TOKEN_END)
            {
                return SHELL_PARSE_INCOMPLETE;
            }

            if (target->kind != SHELL_TOKEN_WORD)
            {
                return SHELL_PARSE_UNEXPECTED_TOKEN;
            }

            if (command->redirection_count >= SHELL_REDIRECTION_MAXIMUM)
            {
                return SHELL_PARSE_TOO_MANY_REDIRECTIONS;
            }

            redirection = &command->redirection[command->redirection_count++];
            redirection->kind = meaning;
            redirection->descriptor = descriptor;
            redirection->target = target->text;
            ShellAdvance(parser);
            continue;
        }

        if (ShellRedirectionOf(token->kind, &meaning, &descriptor))
        {
            const ShellToken *target;
            ShellRedirection *redirection;

            ShellAdvance(parser);
            target = ShellPeek(parser);

            if (target->kind == SHELL_TOKEN_END)
            {
                return SHELL_PARSE_INCOMPLETE;
            }

            if (target->kind != SHELL_TOKEN_WORD)
            {
                return SHELL_PARSE_UNEXPECTED_TOKEN;
            }

            if (command->redirection_count >= SHELL_REDIRECTION_MAXIMUM)
            {
                return SHELL_PARSE_TOO_MANY_REDIRECTIONS;
            }

            redirection = &command->redirection[command->redirection_count++];
            redirection->kind = meaning;
            redirection->descriptor = descriptor;
            redirection->target = target->text;
            ShellAdvance(parser);
            continue;
        }

        if ((token->kind == SHELL_TOKEN_DLESS) || (token->kind == SHELL_TOKEN_DLESSDASH) ||
            (token->kind == SHELL_TOKEN_LEFT_PAREN) || (token->kind == SHELL_TOKEN_RIGHT_PAREN) ||
            (token->kind == SHELL_TOKEN_DSEMI))
        {
            /* The here-document, the subshell, and the case terminator: named
             * by the grammar, not implemented here, and refused as such. */
            return SHELL_PARSE_UNSUPPORTED;
        }

        /* Anything else ends the command: an operator the pipeline or the list
         * acts upon, or the end of the line. */
        return ((command->word_count == 0U) && (command->redirection_count == 0U))
                   ? SHELL_PARSE_UNEXPECTED_TOKEN
                   : SHELL_PARSE_OK;
    }
}

/* pipeline: [!] command ( | command )* */
static ShellParseStatus ShellParsePipeline(ShellParser *parser, ShellPipeline *pipeline)
{
    ShellParseStatus status;

    pipeline->command_count = 0U;
    pipeline->negated = false;

    if ((ShellPeek(parser)->kind == SHELL_TOKEN_WORD) && (strcmp(ShellPeek(parser)->text, "!") == 0))
    {
        pipeline->negated = true;
        ShellAdvance(parser);
    }

    for (;;)
    {
        if (pipeline->command_count >= SHELL_COMMAND_MAXIMUM)
        {
            return SHELL_PARSE_TOO_MANY_COMMANDS;
        }

        if (ShellPeek(parser)->kind == SHELL_TOKEN_END)
        {
            /* `ls |` and nothing after: the pipeline continues upon the next
             * line, Section 2.10's `linebreak` after the `|`. A `!` alone is
             * the same. */
            return SHELL_PARSE_INCOMPLETE;
        }

        status = ShellParseSimpleCommand(parser, &pipeline->command[pipeline->command_count]);

        if (status != SHELL_PARSE_OK)
        {
            return status;
        }

        ++pipeline->command_count;

        if (ShellPeek(parser)->kind != SHELL_TOKEN_PIPE)
        {
            return SHELL_PARSE_OK;
        }

        ShellAdvance(parser);
    }
}

/* list: and_or ( ( ; | & ) and_or )* [ ; | & ], with and_or flattened into
 * the pipelines' conditions. */
static ShellParseStatus ShellParseList(ShellParser *parser, ShellList *list)
{
    ShellCondition condition = SHELL_CONDITION_NONE;

    list->pipeline_count = 0U;

    for (;;)
    {
        ShellPipeline *pipeline;
        ShellParseStatus status;
        const ShellToken *next;

        if (list->pipeline_count >= SHELL_PIPELINE_MAXIMUM)
        {
            return SHELL_PARSE_TOO_MANY_PIPELINES;
        }

        pipeline = &list->pipeline[list->pipeline_count];
        status = ShellParsePipeline(parser, pipeline);

        if (status != SHELL_PARSE_OK)
        {
            return status;
        }

        pipeline->condition = condition;
        pipeline->separator = SHELL_SEPARATOR_NONE;
        ++list->pipeline_count;

        next = ShellPeek(parser);

        switch (next->kind)
        {
        case SHELL_TOKEN_END:
            return SHELL_PARSE_OK;

        case SHELL_TOKEN_AND_IF:
        case SHELL_TOKEN_OR_IF:
            /* The next pipeline is conditional upon this one; `&&` at the end
             * of a line wants the next line, as `|` does. */
            condition = (next->kind == SHELL_TOKEN_AND_IF) ? SHELL_CONDITION_AND_IF
                                                           : SHELL_CONDITION_OR_IF;
            ShellAdvance(parser);

            if (ShellPeek(parser)->kind == SHELL_TOKEN_END)
            {
                return SHELL_PARSE_INCOMPLETE;
            }

            continue;

        case SHELL_TOKEN_SEMICOLON:
        case SHELL_TOKEN_AND:
            pipeline->separator = (next->kind == SHELL_TOKEN_AND) ? SHELL_SEPARATOR_BACKGROUND
                                                                  : SHELL_SEPARATOR_SEQUENCE;
            condition = SHELL_CONDITION_NONE;
            ShellAdvance(parser);

            /* A trailing separator ends the list: `ls;` is one command. */
            if (ShellPeek(parser)->kind == SHELL_TOKEN_END)
            {
                return SHELL_PARSE_OK;
            }

            continue;

        default:
            /* A `|` cannot reach here — the pipeline consumed it — so this is
             * an operator that ends nothing and begins nothing: `ls ) x`. */
            return SHELL_PARSE_UNEXPECTED_TOKEN;
        }
    }
}

ShellParseStatus ShellParse(const ShellTokens *tokens, ShellList *list, size_t *offending)
{
    ShellParser parser;
    ShellParseStatus status;

    if (offending != NULL)
    {
        *offending = 0U;
    }

    if ((tokens == NULL) || (list == NULL) || (tokens->count == 0U))
    {
        return SHELL_PARSE_UNEXPECTED_TOKEN;
    }

    parser.tokens = tokens;
    parser.at = 0U;
    list->pipeline_count = 0U;

    if (tokens->token[0].kind == SHELL_TOKEN_END)
    {
        return SHELL_PARSE_EMPTY;
    }

    status = ShellParseList(&parser, list);

    if ((status != SHELL_PARSE_OK) && (offending != NULL))
    {
        *offending = parser.at;
    }

    return status;
}

const char *ShellParseStatusName(ShellParseStatus status)
{
    switch (status)
    {
    case SHELL_PARSE_OK:                    return "parsed";
    case SHELL_PARSE_EMPTY:                 return "empty";
    case SHELL_PARSE_INCOMPLETE:            return "incomplete";
    case SHELL_PARSE_UNEXPECTED_TOKEN:      return "unexpected token";
    case SHELL_PARSE_UNSUPPORTED:           return "not implemented by this shell";
    case SHELL_PARSE_TOO_MANY_TOKENS:       return "too many tokens";
    case SHELL_PARSE_TOO_MANY_WORDS:        return "too many words in one command";
    case SHELL_PARSE_TOO_MANY_REDIRECTIONS: return "too many redirections in one command";
    case SHELL_PARSE_TOO_MANY_COMMANDS:     return "too many commands in one pipeline";
    case SHELL_PARSE_TOO_MANY_PIPELINES:    return "too many pipelines in one line";
    case SHELL_PARSE_BAD_DESCRIPTOR:        return "descriptor number too large";
    default:                                return "?";
    }
}
