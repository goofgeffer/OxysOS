/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/lexer.c
 * Purpose: The tokeniser of sub-task 8.2: turns a line into the operators and
 *          words of IEEE Std 1003.1-2017, Section 2.3, keeping the quotes upon
 *          the words for the expansions of a later sub-task to read — and the
 *          quote removal those words are given before they are used.
 * Key functions: ShellTokenise, ShellUnquote, ShellTokenKindText.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.3 (Token Recognition), rules 1 to 10,
 *     cited at the code that implements each.
 *   - IEEE Std 1003.1-2017, Section 2.2 (Quoting): 2.2.1 the escape character,
 *     2.2.2 single quotes, 2.2.3 double quotes.
 *   - IEEE Std 1003.1-2017, Section 2.10.2, rule 2, and the grammar's
 *     `io_number`: digits delimited by `<` or `>` are a token of their own.
 *   - userland/sh/shell.h: why the quotes are kept, and every bound.
 *   - docs/design/SHELL.md.
 *
 * What of Section 2.3 is not here.
 *
 *   Rule 5, the expansions: an unquoted `$` or backquote begins an expansion
 *   and the tokeniser is to recognise its extent. No expansion is performed at
 *   this sub-task, so a `$` is an ordinary character of a word and nothing
 *   here looks for the `}` or the `)` that would close one. When the
 *   expansions arrive the tokeniser will have to find those extents, and that
 *   is recorded as a limitation rather than pretended to.
 *
 *   The here-document of `<<`, whose body is the lines that follow: the
 *   operator is recognised, so that it is refused by name, and no body is read.
 */

#include "shell.h"

#include <string.h>

/* Rule 7: a blank is a space or a tab. Section 2.3 leaves the set to the
 * locale, and this system has one locale and it is this. */
static bool ShellIsBlank(char character)
{
    return (character == ' ') || (character == '\t');
}

static bool ShellIsDigit(char character)
{
    return (character >= '0') && (character <= '9');
}

/* Whether a character can begin an operator, Section 2.3, rule 6. */
static bool ShellStartsOperator(char character)
{
    switch (character)
    {
    case '|':
    case '&':
    case ';':
    case '<':
    case '>':
    case '(':
    case ')':
        return true;
    default:
        return false;
    }
}

/*
 * Reads the longest operator at `at`, Section 2.3, rules 2 and 3: an operator
 * is extended while the next character could continue one, which is what
 * makes `>>` one token and `> >` two. Returns its kind and its length.
 */
static ShellTokenKind ShellReadOperator(const char *at, size_t *length)
{
    const char first = at[0];
    const char second = at[1];
    const char third = (second != '\0') ? at[2] : '\0';

    *length = 2U;

    switch (first)
    {
    case '&':
        if (second == '&') { return SHELL_TOKEN_AND_IF; }
        break;
    case '|':
        if (second == '|') { return SHELL_TOKEN_OR_IF; }
        break;
    case ';':
        if (second == ';') { return SHELL_TOKEN_DSEMI; }
        break;
    case '<':
        if ((second == '<') && (third == '-')) { *length = 3U; return SHELL_TOKEN_DLESSDASH; }
        if (second == '<') { return SHELL_TOKEN_DLESS; }
        if (second == '&') { return SHELL_TOKEN_LESSAND; }
        if (second == '>') { return SHELL_TOKEN_LESSGREAT; }
        break;
    case '>':
        if (second == '>') { return SHELL_TOKEN_DGREAT; }
        if (second == '&') { return SHELL_TOKEN_GREATAND; }
        if (second == '|') { return SHELL_TOKEN_CLOBBER; }
        break;
    default:
        break;
    }

    *length = 1U;

    switch (first)
    {
    case '&': return SHELL_TOKEN_AND;
    case '|': return SHELL_TOKEN_PIPE;
    case ';': return SHELL_TOKEN_SEMICOLON;
    case '<': return SHELL_TOKEN_LESS;
    case '>': return SHELL_TOKEN_GREAT;
    case '(': return SHELL_TOKEN_LEFT_PAREN;
    default:  return SHELL_TOKEN_RIGHT_PAREN;
    }
}

/* Appends a token whose text is `length` characters at `at`. */
static ShellParseStatus ShellAppendToken(ShellTokens *tokens, ShellTokenKind kind,
                                         const char *at, size_t length, size_t column)
{
    ShellToken *token;

    if (tokens->count >= SHELL_TOKEN_MAXIMUM)
    {
        return SHELL_PARSE_TOO_MANY_TOKENS;
    }

    if (tokens->text_used + length + 1U > SHELL_TEXT_MAXIMUM)
    {
        return SHELL_PARSE_TOO_MANY_TOKENS;
    }

    token = &tokens->token[tokens->count];
    token->kind = kind;
    token->text = &tokens->text[tokens->text_used];
    token->length = length;
    token->column = column;

    memcpy(&tokens->text[tokens->text_used], at, length);
    tokens->text[tokens->text_used + length] = '\0';
    tokens->text_used += length + 1U;
    ++tokens->count;

    return SHELL_PARSE_OK;
}

/*
 * Finds the end of the word beginning at `at`, honouring the quoting of
 * Section 2.2, and reports whether a quote was left open.
 *
 * Rule 4: a quoting character affects everything up to the end of the quoted
 * text, and the quoted text is part of the word — so a blank or an operator
 * character within quotes delimits nothing. Rule 8: an unquoted character that
 * is neither is appended.
 */
static size_t ShellWordExtent(const char *at, bool *unterminated)
{
    size_t index = 0U;

    *unterminated = false;

    while (at[index] != '\0')
    {
        const char character = at[index];

        if (character == '\\')
        {
            /* 2.2.1: the backslash preserves the character that follows it,
             * whatever it is; a backslash at the end of the line is an escaped
             * newline, which is a continuation and therefore incomplete. */
            if (at[index + 1U] == '\0')
            {
                *unterminated = true;
                return index;
            }

            index += 2U;
            continue;
        }

        if (character == '\'')
        {
            /* 2.2.2: everything to the next single quote is literal, the
             * backslash included. */
            const char *close = strchr(&at[index + 1U], '\'');

            if (close == NULL)
            {
                *unterminated = true;
                return index;
            }

            index = (size_t)(close - at) + 1U;
            continue;
        }

        if (character == '"')
        {
            /* 2.2.3: to the next double quote, a backslash escaping only the
             * five characters the section names — of which `$` and the
             * backquote matter only to an expansion nothing here performs. */
            ++index;

            for (;;)
            {
                const char inner = at[index];

                if (inner == '\0')
                {
                    *unterminated = true;
                    return index;
                }

                if (inner == '"')
                {
                    ++index;
                    break;
                }

                if ((inner == '\\') && (at[index + 1U] != '\0') &&
                    ((at[index + 1U] == '$') || (at[index + 1U] == '`') ||
                     (at[index + 1U] == '"') || (at[index + 1U] == '\\') ||
                     (at[index + 1U] == '\n')))
                {
                    index += 2U;
                    continue;
                }

                ++index;
            }

            continue;
        }

        if (ShellIsBlank(character) || (character == '\n') || ShellStartsOperator(character))
        {
            /* Rules 6 and 7: an unquoted operator character or blank delimits
             * the word and is not part of it. */
            return index;
        }

        ++index;
    }

    return index;
}

ShellParseStatus ShellTokenise(const char *line, ShellTokens *tokens)
{
    size_t at = 0U;

    if ((line == NULL) || (tokens == NULL))
    {
        return SHELL_PARSE_UNEXPECTED_TOKEN;
    }

    tokens->count = 0U;
    tokens->text_used = 0U;

    for (;;)
    {
        ShellParseStatus status;

        /* Rule 7: blanks between tokens are discarded. */
        while (ShellIsBlank(line[at]) || (line[at] == '\n'))
        {
            ++at;
        }

        /* Rule 1: the end of the input delimits the last token, and the END
         * token is what the parser looks for rather than a count. */
        if (line[at] == '\0')
        {
            return ShellAppendToken(tokens, SHELL_TOKEN_END, "", 0U, at);
        }

        /* Rule 9: an unquoted `#` at the start of a token begins a comment
         * that runs to the end of the line, and the comment is no token. */
        if (line[at] == '#')
        {
            return ShellAppendToken(tokens, SHELL_TOKEN_END, "", 0U, at);
        }

        if (ShellStartsOperator(line[at]))
        {
            size_t length;
            const ShellTokenKind kind = ShellReadOperator(&line[at], &length);

            status = ShellAppendToken(tokens, kind, &line[at], length, at);

            if (status != SHELL_PARSE_OK)
            {
                return status;
            }

            at += length;
            continue;
        }

        /* io_number: digits immediately followed by < or >, Section 2.10.2,
         * rule 2. `2>` is a redirection of descriptor 2 and `2 >` is the word
         * 2 followed by one of descriptor 1, which is why the digits must be
         * immediately before the operator. */
        {
            size_t digits = 0U;

            while (ShellIsDigit(line[at + digits]))
            {
                ++digits;
            }

            if ((digits > 0U) && ((line[at + digits] == '<') || (line[at + digits] == '>')))
            {
                status = ShellAppendToken(tokens, SHELL_TOKEN_IO_NUMBER, &line[at], digits, at);

                if (status != SHELL_PARSE_OK)
                {
                    return status;
                }

                at += digits;
                continue;
            }
        }

        /* Rule 10: anything else begins a word. */
        {
            bool unterminated;
            const size_t length = ShellWordExtent(&line[at], &unterminated);

            if (unterminated)
            {
                return SHELL_PARSE_INCOMPLETE;
            }

            status = ShellAppendToken(tokens, SHELL_TOKEN_WORD, &line[at], length, at);

            if (status != SHELL_PARSE_OK)
            {
                return status;
            }

            at += length;
        }
    }
}

bool ShellUnquote(const char *word, char *destination, size_t capacity)
{
    size_t out = 0U;
    size_t index = 0U;

    if ((word == NULL) || (destination == NULL) || (capacity == 0U))
    {
        return false;
    }

    while (word[index] != '\0')
    {
        const char character = word[index];

        if (character == '\\')
        {
            /* 2.2.1. An escaped newline is removed entirely — it is a line
             * continuation and produces no character. */
            if (word[index + 1U] == '\0')
            {
                break;
            }

            if (word[index + 1U] != '\n')
            {
                if (out + 1U >= capacity) { return false; }
                destination[out++] = word[index + 1U];
            }

            index += 2U;
            continue;
        }

        if (character == '\'')
        {
            ++index;

            while ((word[index] != '\0') && (word[index] != '\''))
            {
                if (out + 1U >= capacity) { return false; }
                destination[out++] = word[index++];
            }

            if (word[index] == '\'')
            {
                ++index;
            }

            continue;
        }

        if (character == '"')
        {
            ++index;

            while ((word[index] != '\0') && (word[index] != '"'))
            {
                if ((word[index] == '\\') && (word[index + 1U] != '\0') &&
                    ((word[index + 1U] == '$') || (word[index + 1U] == '`') ||
                     (word[index + 1U] == '"') || (word[index + 1U] == '\\') ||
                     (word[index + 1U] == '\n')))
                {
                    if (word[index + 1U] != '\n')
                    {
                        if (out + 1U >= capacity) { return false; }
                        destination[out++] = word[index + 1U];
                    }

                    index += 2U;
                    continue;
                }

                if (out + 1U >= capacity) { return false; }
                destination[out++] = word[index++];
            }

            if (word[index] == '"')
            {
                ++index;
            }

            continue;
        }

        if (out + 1U >= capacity) { return false; }
        destination[out++] = character;
        ++index;
    }

    destination[out] = '\0';

    return true;
}

const char *ShellTokenKindText(ShellTokenKind kind)
{
    switch (kind)
    {
    case SHELL_TOKEN_END:         return "end of line";
    case SHELL_TOKEN_WORD:        return "word";
    case SHELL_TOKEN_IO_NUMBER:   return "io_number";
    case SHELL_TOKEN_PIPE:        return "|";
    case SHELL_TOKEN_AND:         return "&";
    case SHELL_TOKEN_SEMICOLON:   return ";";
    case SHELL_TOKEN_AND_IF:      return "&&";
    case SHELL_TOKEN_OR_IF:       return "||";
    case SHELL_TOKEN_DSEMI:       return ";;";
    case SHELL_TOKEN_LESS:        return "<";
    case SHELL_TOKEN_GREAT:       return ">";
    case SHELL_TOKEN_DLESS:       return "<<";
    case SHELL_TOKEN_DGREAT:      return ">>";
    case SHELL_TOKEN_LESSAND:     return "<&";
    case SHELL_TOKEN_GREATAND:    return ">&";
    case SHELL_TOKEN_LESSGREAT:   return "<>";
    case SHELL_TOKEN_DLESSDASH:   return "<<-";
    case SHELL_TOKEN_CLOBBER:     return ">|";
    case SHELL_TOKEN_LEFT_PAREN:  return "(";
    case SHELL_TOKEN_RIGHT_PAREN: return ")";
    default:                      return "?";
    }
}
