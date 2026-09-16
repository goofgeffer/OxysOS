/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdlib/environment.c
 * Purpose: `getenv` of ISO/IEC 9899:2011, Section 7.22.4.6, and the
 *          environment vector it searches — recorded by the startup object
 *          from the stack the kernel prepared, which is the only place a
 *          program's environment exists.
 * Key functions: getenv. Key definitions: OxysEnvironment.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.22.4.6: `getenv` searches an environment
 *     list for a string matching `name`, and returns a pointer to the
 *     associated string, or a null pointer where none is found. The set of
 *     names and the method of altering the list are implementation-defined;
 *     here the set is what the shell exported, and there is no method.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1:
 *     the environment pointers follow the argument vector's terminator upon
 *     the initial stack, each naming a string of the form NAME=value.
 *   - libc/crt/crt0.asm: which stores the vector's address here before it
 *     calls `main`.
 *   - docs/design/SHELL.md, Section 17: the environment a program is given,
 *     and where it comes from.
 *
 * Why the vector is stored and not copied.
 *
 *   The strings stand upon the program's own initial stack, placed there by
 *   the kernel and never moved; a copy would be a copy of something that
 *   cannot change. `getenv` returns a pointer into them, as Section 7.22.4.6
 *   permits, and a program that alters what the pointer names is altering its
 *   own environment, which the standard leaves implementation-defined too.
 */

#include <stdlib.h>
#include <string.h>

/* The vector, as `_start` found it. A null pointer only before `_start` has
 * run, which is before anything can call this. */
char **OxysEnvironment;

char *getenv(const char *name)
{
    size_t length;

    if ((name == NULL) || (name[0] == '\0') || (OxysEnvironment == NULL))
    {
        return NULL;
    }

    length = strlen(name);

    for (size_t index = 0U; OxysEnvironment[index] != NULL; ++index)
    {
        const char *const entry = OxysEnvironment[index];

        if ((strncmp(entry, name, length) == 0) && (entry[length] == '='))
        {
            /* The cast discards a const the standard's signature does not
             * carry; the string is the program's own. */
            return (char *)&entry[length + 1U];
        }
    }

    return NULL;
}
