/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/variables.c
 * Purpose: The shell's variables, of sub-task 8.3: a fixed table of names and
 *          values, each marked exported or not, which `NAME=value` sets,
 *          `export` marks, `$NAME` reads, and — from sub-task 8.4 — the
 *          environment of a program is built from.
 * Key functions: ShellVariablesInitialise, ShellVariableSet, ShellVariableGet,
 *          ShellVariableExport, ShellVariableIsExported, ShellVariableUnset, ShellVariableCount,
 *          ShellVariableAt, ShellIsName, ShellIsAssignmentWord.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.5.3 (Shell Variables) and Section 3.235
 *     (Name): a name is letters, digits and underscores, not beginning with a
 *     digit.
 *   - IEEE Std 1003.1-2017, `export`: a variable marked for export is in the
 *     environment of every command the shell subsequently runs; `export` with
 *     no operand writes each exported variable as `export NAME=value`.
 *   - IEEE Std 1003.1-2017, Section 2.10.2, rule 7: a word of the form
 *     `NAME=value` before a command's name is a variable assignment.
 *   - docs/design/SHELL.md, Section 12.
 *
 * Why the table is fixed and static.
 *
 *   For the reason the line editor's and the parser's storage is: a shell that
 *   could not set a variable for want of memory could not say so. Sixty-four
 *   variables of a name and two hundred and fifty-five characters of value is
 *   some twenty kibibytes of `.bss`; a person who wants more is asking for a
 *   different shell, and the bound is a number they can be told.
 *
 * Concurrency. None. The shell is one thread of control, and this table is
 *   its own.
 */

#include "shell.h"

#include <string.h>

typedef struct ShellVariable
{
    bool used;
    bool exported;
    char name[SHELL_NAME_MAXIMUM + 1U];
    char value[SHELL_VALUE_MAXIMUM + 1U];
} ShellVariable;

static ShellVariable ShellVariables[SHELL_VARIABLE_MAXIMUM];

bool ShellIsName(const char *text, size_t length)
{
    if ((text == NULL) || (length == 0U) || (length > SHELL_NAME_MAXIMUM))
    {
        return false;
    }

    if (((text[0] >= '0') && (text[0] <= '9')))
    {
        return false;
    }

    for (size_t index = 0U; index < length; ++index)
    {
        const char character = text[index];
        const bool letter = ((character >= 'a') && (character <= 'z')) ||
                            ((character >= 'A') && (character <= 'Z'));
        const bool digit = (character >= '0') && (character <= '9');

        if (!letter && !digit && (character != '_'))
        {
            return false;
        }
    }

    return true;
}

size_t ShellIsAssignmentWord(const char *word)
{
    const char *const equals = (word != NULL) ? strchr(word, '=') : NULL;

    if (equals == NULL)
    {
        return 0U;
    }

    return ShellIsName(word, (size_t)(equals - word)) ? (size_t)(equals - word) : 0U;
}

void ShellVariablesInitialise(void)
{
    memset(ShellVariables, 0, sizeof ShellVariables);
}

static ShellVariable *ShellVariableFind(const char *name)
{
    for (size_t index = 0U; index < SHELL_VARIABLE_MAXIMUM; ++index)
    {
        if (ShellVariables[index].used && (strcmp(ShellVariables[index].name, name) == 0))
        {
            return &ShellVariables[index];
        }
    }

    return NULL;
}

bool ShellVariableSet(const char *name, const char *value)
{
    ShellVariable *variable;
    const size_t length = (name != NULL) ? strlen(name) : 0U;

    if (!ShellIsName(name, length) || (value == NULL) || (strlen(value) > SHELL_VALUE_MAXIMUM))
    {
        return false;
    }

    variable = ShellVariableFind(name);

    if (variable == NULL)
    {
        for (size_t index = 0U; index < SHELL_VARIABLE_MAXIMUM; ++index)
        {
            if (!ShellVariables[index].used)
            {
                variable = &ShellVariables[index];
                break;
            }
        }

        if (variable == NULL)
        {
            return false;
        }

        variable->used = true;
        variable->exported = false;
        memcpy(variable->name, name, length + 1U);
    }

    memcpy(variable->value, value, strlen(value) + 1U);

    return true;
}

const char *ShellVariableGet(const char *name)
{
    const ShellVariable *const variable = (name != NULL) ? ShellVariableFind(name) : NULL;

    return (variable != NULL) ? variable->value : NULL;
}

bool ShellVariableExport(const char *name)
{
    ShellVariable *variable = (name != NULL) ? ShellVariableFind(name) : NULL;

    /* `export NAME` of a name not yet set marks it: the standard has the
     * variable exported once it is assigned, and the simplest record of that
     * is an empty exported variable. */
    if (variable == NULL)
    {
        if (!ShellVariableSet(name, ""))
        {
            return false;
        }

        variable = ShellVariableFind(name);
    }

    variable->exported = true;

    return true;
}

bool ShellVariableIsExported(const char *name)
{
    const ShellVariable *const variable = (name != NULL) ? ShellVariableFind(name) : NULL;

    return (variable != NULL) && variable->exported;
}

size_t ShellVariableCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < SHELL_VARIABLE_MAXIMUM; ++index)
    {
        if (ShellVariables[index].used)
        {
            ++count;
        }
    }

    return count;
}

bool ShellVariableAt(size_t position, const char **name, const char **value, bool *exported)
{
    size_t seen = 0U;

    for (size_t index = 0U; index < SHELL_VARIABLE_MAXIMUM; ++index)
    {
        if (!ShellVariables[index].used)
        {
            continue;
        }

        if (seen == position)
        {
            *name = ShellVariables[index].name;
            *value = ShellVariables[index].value;
            *exported = ShellVariables[index].exported;

            return true;
        }

        ++seen;
    }

    return false;
}

bool ShellVariableUnset(const char *name)
{
    ShellVariable *const variable = (name != NULL) ? ShellVariableFind(name) : NULL;

    if (variable == NULL)
    {
        return false;
    }

    memset(variable, 0, sizeof *variable);

    return true;
}
