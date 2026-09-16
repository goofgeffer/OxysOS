/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/expand.c
 * Purpose: The word expansion of sub-task 8.3, as far as the built-ins need
 *          it: parameter expansion — `$NAME`, `${NAME}` and `$?` — performed
 *          upon a word with its quotes still upon it, and the quote removal of
 *          Section 2.6.7 applied last, in one pass, so that a `$` within single
 *          quotes is a character and one within double quotes is not.
 * Key functions: ShellExpandWord.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.6 (Word Expansions): the order — tilde,
 *     parameter, command, arithmetic, field splitting, pathname expansion,
 *     quote removal — of which parameter expansion and quote removal are here
 *     and the rest are recorded as absent in docs/design/SHELL.md, Section 15.
 *   - IEEE Std 1003.1-2017, Section 2.6.2 (Parameter Expansion): `${name}` and
 *     `$name`, the name being the longest sequence of the characters of a
 *     name; and Section 2.5.2, the special parameter `?`.
 *   - IEEE Std 1003.1-2017, Section 2.2: the quoting this respects, restated
 *     from lexer.c because the two must agree about what a backslash escapes.
 *   - docs/design/SHELL.md, Section 12.
 *
 * Why expansion and quote removal are one pass.
 *
 *   Section 2.6 has them as separate steps, expansion producing a word that
 *   quote removal then strips. Done as two passes, the result of an expansion
 *   would be scanned by quote removal as if it had been typed — so a value
 *   holding a quote would be treated as quoting, which the standard forbids:
 *   the characters an expansion produces are never quoting characters. One
 *   pass that copies a value straight to the output, past the quote scan, is
 *   the shortest way to be right about that.
 */

#include "shell.h"

#include <string.h>

static bool ShellIsNameStart(char character)
{
    return ((character >= 'a') && (character <= 'z')) ||
           ((character >= 'A') && (character <= 'Z')) || (character == '_');
}

static bool ShellIsNameCharacter(char character)
{
    return ShellIsNameStart(character) || ((character >= '0') && (character <= '9'));
}

/* Appends one character to the output, refusing where it does not fit. */
static bool ShellExpandPut(char *destination, size_t capacity, size_t *out, char character)
{
    if (*out + 1U >= capacity)
    {
        return false;
    }

    destination[(*out)++] = character;

    return true;
}

/* Appends a string to the output, refusing where it does not fit. A null
 * string — an unset variable — appends nothing, as Section 2.6.2 has it. */
static bool ShellExpandPutString(char *destination, size_t capacity, size_t *out,
                                 const char *string)
{
    if (string == NULL)
    {
        return true;
    }

    while (*string != '\0')
    {
        if (!ShellExpandPut(destination, capacity, out, *string++))
        {
            return false;
        }
    }

    return true;
}

/*
 * Expands a `$` at word[*index], appending the value and advancing the index
 * past the expansion. A `$` followed by nothing that can begin an expansion
 * is a literal `$`, as every shell of this lineage has it.
 */
static bool ShellExpandParameter(const char *word, size_t *index, char *destination,
                                 size_t capacity, size_t *out, ShellLookup lookup,
                                 void *context)
{
    char name[SHELL_NAME_MAXIMUM + 1U];
    size_t length = 0U;
    size_t at = *index + 1U;

    if (word[at] == '{')
    {
        ++at;

        while ((word[at] != '\0') && (word[at] != '}'))
        {
            if (length >= SHELL_NAME_MAXIMUM)
            {
                return false;
            }

            name[length++] = word[at++];
        }

        if (word[at] != '}')
        {
            /* `${` with no `}`: taken literally rather than refused, the
             * tokeniser having accepted the word. */
            ++(*index);
            return ShellExpandPut(destination, capacity, out, '$');
        }

        ++at;
    }
    else if (word[at] == '?')
    {
        name[length++] = '?';
        ++at;
    }
    else if (ShellIsNameStart(word[at]))
    {
        while (ShellIsNameCharacter(word[at]))
        {
            if (length >= SHELL_NAME_MAXIMUM)
            {
                return false;
            }

            name[length++] = word[at++];
        }
    }
    else
    {
        ++(*index);

        return ShellExpandPut(destination, capacity, out, '$');
    }

    name[length] = '\0';
    *index = at;

    return ShellExpandPutString(destination, capacity, out,
                                (lookup != NULL) ? lookup(context, name) : NULL);
}

bool ShellExpandWord(const char *word, char *destination, size_t capacity,
                     ShellLookup lookup, void *context)
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
            /* 2.2.1: the character after a backslash is literal; an escaped
             * newline is removed entirely. */
            if (word[index + 1U] == '\0')
            {
                break;
            }

            if (word[index + 1U] != '\n')
            {
                if (!ShellExpandPut(destination, capacity, &out, word[index + 1U]))
                {
                    return false;
                }
            }

            index += 2U;
            continue;
        }

        if (character == '\'')
        {
            /* 2.2.2: everything to the closing quote is literal, `$` included. */
            ++index;

            while ((word[index] != '\0') && (word[index] != '\''))
            {
                if (!ShellExpandPut(destination, capacity, &out, word[index++]))
                {
                    return false;
                }
            }

            if (word[index] == '\'')
            {
                ++index;
            }

            continue;
        }

        if (character == '"')
        {
            /* 2.2.3: `$` expands within double quotes; a backslash escapes the
             * five characters the section names and is otherwise literal. */
            ++index;

            while ((word[index] != '\0') && (word[index] != '"'))
            {
                const char inner = word[index];

                if ((inner == '\\') && (word[index + 1U] != '\0') &&
                    ((word[index + 1U] == '$') || (word[index + 1U] == '`') ||
                     (word[index + 1U] == '"') || (word[index + 1U] == '\\') ||
                     (word[index + 1U] == '\n')))
                {
                    if (word[index + 1U] != '\n')
                    {
                        if (!ShellExpandPut(destination, capacity, &out, word[index + 1U]))
                        {
                            return false;
                        }
                    }

                    index += 2U;
                    continue;
                }

                if (inner == '$')
                {
                    if (!ShellExpandParameter(word, &index, destination, capacity, &out,
                                              lookup, context))
                    {
                        return false;
                    }

                    continue;
                }

                if (!ShellExpandPut(destination, capacity, &out, inner))
                {
                    return false;
                }

                ++index;
            }

            if (word[index] == '"')
            {
                ++index;
            }

            continue;
        }

        if (character == '$')
        {
            if (!ShellExpandParameter(word, &index, destination, capacity, &out, lookup,
                                      context))
            {
                return false;
            }

            continue;
        }

        if (!ShellExpandPut(destination, capacity, &out, character))
        {
            return false;
        }

        ++index;
    }

    destination[out] = '\0';

    return true;
}
