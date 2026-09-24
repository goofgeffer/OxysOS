/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/builtins.c
 * Purpose: The built-in commands — `cd`, `pwd`, `export` and `exit` of sub-task
 *          8.3, `unset`, `help`, `true` and `false` added at 8.5, `clear`
 *          added on 2026-09-16, and `jobs`, `fg`, `bg` and `kill` of 8.7 —
 *          which are the commands a shell must run itself because
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
 *   - docs/design/SHELL.md.
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
static const char *const ShellSpecialBuiltins[] = { "exit", "export", "unset" };
static const char *const ShellRegularBuiltins[] = { "cd",   "pwd", "help", "true", "false",
                                                    "clear", "jobs", "fg",  "bg",   "kill" };

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

/* `unset name...`: each variable removed, IEEE Std 1003.1-2017's `unset`
 * without its `-f`, this shell having no functions. */
static int ShellBuiltinUnset(int argc, char **argv)
{
    int status = 0;

    for (int index = 1; index < argc; ++index)
    {
        if (!ShellIsName(argv[index], strlen(argv[index])))
        {
            (void)fprintf(stderr, "sh: unset: %s: not a valid name.\n", argv[index]);
            status = 1;
            continue;
        }

        (void)ShellVariableUnset(argv[index]);
    }

    return status;
}

/*
 * `help`: every command a person can type, one to a line, as name, arguments,
 * a comma, and one sentence. It is here because a person at a prompt with no
 * manual has nothing else to ask, and it is a list and not a description
 * because a person who typed `help` wanted to find a command and not to read
 * about the shell — the project owner said so on 2026-09-16, and the paragraph
 * that stood here until then was removed at that request. The built-ins come
 * first, then the programs of /bin.
 */
static int ShellBuiltinHelp(void)
{
    (void)printf("cd [dir | -], changes the working directory; to HOME with no operand, "
                 "back with -.\n"
                 "pwd, prints the working directory.\n"
                 "export [NAME[=value]]..., marks a variable for the environment of every "
                 "program run; alone, lists the exported ones.\n"
                 "unset NAME..., removes a variable.\n"
                 "exit [n], ends the shell with status n, or the last status.\n"
                 "true, succeeds.\n"
                 "false, fails.\n"
                 "help, prints this list.\n"
                 "clear, clears the screen.\n"
                 "jobs, lists the jobs: each pipeline run with & or stopped by control-Z.\n"
                 "fg [%%n], brings a job to the foreground, the newest with no operand.\n"
                 "bg [%%n], continues a stopped job in the background.\n"
                 "kill [-SIGNAL] pid | %%n..., sends a signal, TERM with none named, to a "
                 "process or a job.\n"
                 "ls [-a] [dir]..., lists a directory, the working directory with no "
                 "operand; -a includes the entries that begin with a dot.\n"
                 "cat [file | -]..., copies each file to the standard output; the standard "
                 "input for no operand or -, which the terminal ends at control-D.\n"
                 "echo [word]..., prints its operands separated by spaces.\n"
                 "mkdir [-p] dir..., creates each directory; -p creates the parents too.\n"
                 "rmdir dir..., removes each empty directory.\n"
                 "rm [-f] file..., removes each file; -f is silent about one that is "
                 "not there.\n"
                 "touch file..., creates each file that does not exist.\n"
                 "cp source target, copies one file to another, created or truncated.\n"
                 "wc [-c] [-l] [-w] [file]..., counts the lines, words and bytes of each "
                 "file, or of the standard input.\n"
                 "micro file, edits a file one line at a time; h at its prompt lists its "
                 "commands.\n"
                 "head [-n N] [file]..., prints the first N lines of each file, or of the "
                 "standard input; ten by default.\n"
                 "tail [-n N] [file]..., prints the last N lines, likewise.\n"
                 "grep [-i] [-v] [-n] [-c] string [file]..., prints the lines holding the "
                 "string; -i ignores case, -v inverts, -n numbers, -c counts.\n"
                 "sort [-r] [file]..., prints the lines in order; -r reverses.\n"
                 "mv source... target, renames a file, or moves files into a directory.\n"
                 "ps, lists the processes.\n");

    return 0;
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

    if (strcmp(argv[0], "unset") == 0)
    {
        return ShellBuiltinUnset(argc, argv);
    }

    if (strcmp(argv[0], "help") == 0)
    {
        return ShellBuiltinHelp();
    }

    if (strcmp(argv[0], "clear") == 0)
    {
        /* One form feed: the display drivers clear the screen upon it and the
         * diagnostic path turns it into ECMA-48's erase-display and
         * cursor-home for a terminal upon the serial line. Flushed at once,
         * because a clear that arrived with the next prompt would clear the
         * prompt too. */
        (void)fputs("\f", stdout);
        (void)fflush(stdout);

        return 0;
    }

    if (strcmp(argv[0], "jobs") == 0)
    {
        ShellJobsList();

        return 0;
    }

    if (strcmp(argv[0], "fg") == 0)
    {
        return ShellJobForeground((argc > 1) ? argv[1] : NULL);
    }

    if (strcmp(argv[0], "bg") == 0)
    {
        return ShellJobBackground((argc > 1) ? argv[1] : NULL);
    }

    if (strcmp(argv[0], "kill") == 0)
    {
        return ShellJobKill(argc, argv);
    }

    if (strcmp(argv[0], "true") == 0)
    {
        return 0;
    }

    if (strcmp(argv[0], "false") == 0)
    {
        return 1;
    }

    return -1;
}
