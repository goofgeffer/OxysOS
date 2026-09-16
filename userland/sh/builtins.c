/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/builtins.c
 * Purpose: The four built-in commands of sub-task 8.3 — `cd`, `pwd`, `export`
 *          and `exit` — which are the commands a shell must run itself because
 *          a child process could not do them on the shell's behalf: a
 *          directory changed in a child is changed for the child, and so is a
 *          variable set there.
 * Key functions: ShellIsBuiltin, ShellIsSpecialBuiltin, ShellRunBuiltin.
 * References:
 *   - IEEE Std 1003.1-2017, `cd`: the operand, `-` for the previous directory,
 *     `HOME` where there is none, and the setting of `PWD` and `OLDPWD`.
 *   - IEEE Std 1003.1-2017, `pwd`: the absolute pathname of the working
 *     directory, with no `.` or `..` component, followed by a newline.
 *   - IEEE Std 1003.1-2017, `export`: `export name[=word]...`, and with no
 *     operand the exported variables written as `export name=value`.
 *   - IEEE Std 1003.1-2017, `exit`: `exit [n]`, the status being n or, with no
 *     operand, that of the last command; and Section 2.14, that `exit` and
 *     `export` are special built-ins and `cd` and `pwd` are not.
 *   - libc/include/syscall.h: OxysChangeDirectory and OxysGetWorkingDirectory,
 *     the two calls beneath `cd` and `pwd`.
 *   - docs/design/SHELL.md, Section 13.
 *
 * What is not here, and why.
 *
 *   `cd -L` and `cd -P`: the kernel keeps the working directory as a path
 *   reduced lexically, which is `-L`'s answer, and has no way to give `-P`'s;
 *   the options are refused rather than accepted and ignored. `pwd -L` and
 *   `pwd -P` likewise. `export -p` is accepted, being what `export` alone
 *   already does. A directory named by `CDPATH` is not searched.
 */

#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

/* The names, and which of them Section 2.14 makes special: an assignment
 * before a special built-in persists in the shell, and before a regular one
 * does not. */
static const char *const ShellSpecialBuiltins[] = { "exit", "export" };
static const char *const ShellRegularBuiltins[] = { "cd", "pwd" };

static bool ShellNameIsAmong(const char *name, const char *const *names, size_t count)
{
    for (size_t index = 0U; index < count; ++index)
    {
        if (strcmp(name, names[index]) == 0)
        {
            return true;
        }
    }

    return false;
}

bool ShellIsSpecialBuiltin(const char *name)
{
    return (name != NULL) &&
           ShellNameIsAmong(name, ShellSpecialBuiltins,
                            sizeof ShellSpecialBuiltins / sizeof ShellSpecialBuiltins[0]);
}

bool ShellIsBuiltin(const char *name)
{
    return ShellIsSpecialBuiltin(name) ||
           ((name != NULL) &&
            ShellNameIsAmong(name, ShellRegularBuiltins,
                             sizeof ShellRegularBuiltins / sizeof ShellRegularBuiltins[0]));
}

/* Where the working directory is read into, for `cd` and `pwd`. */
static char ShellDirectory[SHELL_VALUE_MAXIMUM + 1U];

/* `cd`. Returns the status: 0, or 1 with the reason upon the standard error. */
static int ShellBuiltinChangeDirectory(int argc, char **argv)
{
    const char *target;
    const char *previous;

    if (argc > 2)
    {
        (void)fprintf(stderr, "sh: cd: too many operands.\n");

        return 1;
    }

    if ((argc == 2) && (argv[1][0] == '-') && (argv[1][1] != '\0') && (strcmp(argv[1], "-") != 0))
    {
        (void)fprintf(stderr, "sh: cd: options are not supported; the working directory "
                              "is kept as `-L' reports it.\n");

        return 1;
    }

    if (argc == 1)
    {
        target = ShellVariableGet("HOME");

        if ((target == NULL) || (target[0] == '\0'))
        {
            (void)fprintf(stderr, "sh: cd: HOME is not set.\n");

            return 1;
        }
    }
    else if (strcmp(argv[1], "-") == 0)
    {
        target = ShellVariableGet("OLDPWD");

        if ((target == NULL) || (target[0] == '\0'))
        {
            (void)fprintf(stderr, "sh: cd: OLDPWD is not set.\n");

            return 1;
        }
    }
    else
    {
        target = argv[1];
    }

    /* The directory left is the next OLDPWD, and it is read before the change
     * so that a `cd -` returns where the person was and not where the shell
     * believed them to be. */
    previous = (OxysGetWorkingDirectory(ShellDirectory, sizeof ShellDirectory) >= 0)
                   ? ShellDirectory
                   : "/";

    if (OxysChangeDirectory(target) < 0)
    {
        (void)fprintf(stderr, "sh: cd: %s: %s.\n", target, strerror(errno));

        return 1;
    }

    (void)ShellVariableSet("OLDPWD", previous);
    (void)ShellVariableExport("OLDPWD");

    if (OxysGetWorkingDirectory(ShellDirectory, sizeof ShellDirectory) >= 0)
    {
        (void)ShellVariableSet("PWD", ShellDirectory);
        (void)ShellVariableExport("PWD");
    }

    /* `cd -` writes the new directory, as the standard has it. */
    if ((argc == 2) && (strcmp(argv[1], "-") == 0))
    {
        (void)printf("%s\n", ShellDirectory);
    }

    return 0;
}

/* `pwd`. */
static int ShellBuiltinPrintDirectory(int argc, char **argv)
{
    if (argc > 1)
    {
        (void)fprintf(stderr, "sh: pwd: %s: %s.\n", argv[1],
                      (argv[1][0] == '-') ? "options are not supported" : "no operand is taken");

        return 1;
    }

    if (OxysGetWorkingDirectory(ShellDirectory, sizeof ShellDirectory) < 0)
    {
        (void)fprintf(stderr, "sh: pwd: %s.\n", strerror(errno));

        return 1;
    }

    (void)printf("%s\n", ShellDirectory);

    return 0;
}

/* `export`. Each operand is `name` or `name=value`; none, or `-p`, lists. */
static int ShellBuiltinExport(int argc, char **argv)
{
    int status = 0;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "-p") == 0)))
    {
        for (size_t index = 0U; index < ShellVariableCount(); ++index)
        {
            const char *name;
            const char *value;
            bool exported;

            if (ShellVariableAt(index, &name, &value, &exported) && exported)
            {
                (void)printf("export %s=%s\n", name, value);
            }
        }

        return 0;
    }

    for (int index = 1; index < argc; ++index)
    {
        const char *const operand = argv[index];
        const size_t name_length = ShellIsAssignmentWord(operand);
        char name[SHELL_NAME_MAXIMUM + 1U];

        if (name_length > 0U)
        {
            memcpy(name, operand, name_length);
            name[name_length] = '\0';

            if (!ShellVariableSet(name, &operand[name_length + 1U]))
            {
                (void)fprintf(stderr, "sh: export: %s: cannot be set.\n", name);
                status = 1;
                continue;
            }
        }
        else if (ShellIsName(operand, strlen(operand)))
        {
            memcpy(name, operand, strlen(operand) + 1U);
        }
        else
        {
            (void)fprintf(stderr, "sh: export: %s: not a valid name.\n", operand);
            status = 1;
            continue;
        }

        if (!ShellVariableExport(name))
        {
            (void)fprintf(stderr, "sh: export: %s: cannot be exported.\n", name);
            status = 1;
        }
    }

    return status;
}

/* `exit [n]`. The status is n, or the last command's; `exit_requested` is
 * what the shell's loop looks at, this function having no way to end the
 * shell itself that would flush what it has printed. */
static int ShellBuiltinExit(int argc, char **argv, int last_status, bool *exit_requested)
{
    int status = last_status;

    if (argc > 2)
    {
        (void)fprintf(stderr, "sh: exit: too many operands.\n");

        return 1;
    }

    if (argc == 2)
    {
        const char *digits = argv[1];

        status = 0;

        if (*digits == '\0')
        {
            (void)fprintf(stderr, "sh: exit: %s: not a number.\n", argv[1]);

            return 1;
        }

        while (*digits != '\0')
        {
            if ((*digits < '0') || (*digits > '9'))
            {
                (void)fprintf(stderr, "sh: exit: %s: not a number.\n", argv[1]);

                return 1;
            }

            status = (status * 10 + (*digits - '0')) & 0xFF;
            ++digits;
        }
    }

    *exit_requested = true;

    return status;
}

int ShellRunBuiltin(int argc, char **argv, int last_status, bool *exit_requested)
{
    if ((argc < 1) || (argv == NULL) || (argv[0] == NULL))
    {
        return -1;
    }

    if (strcmp(argv[0], "cd") == 0)
    {
        return ShellBuiltinChangeDirectory(argc, argv);
    }

    if (strcmp(argv[0], "pwd") == 0)
    {
        return ShellBuiltinPrintDirectory(argc, argv);
    }

    if (strcmp(argv[0], "export") == 0)
    {
        return ShellBuiltinExport(argc, argv);
    }

    if (strcmp(argv[0], "exit") == 0)
    {
        return ShellBuiltinExit(argc, argv, last_status, exit_requested);
    }

    return -1;
}
