/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/run.c
 * Purpose: External program execution, of sub-task 8.4: the search for a
 *          command's program, the environment built from the exported
 *          variables, the `fork`, the `execve` in the child, and the `wait`
 *          that turns what the child ended with into a status — and, of 8.5,
 *          the redirections performed in the child between the two.
 * Key functions: ShellRunProgram, ShellBuildEnvironment, ShellApplyRedirections.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.9.1.1 (Command Search and Execution):
 *     a name holding a slash is the pathname of the program; one without is
 *     sought in each directory of PATH, in order; a command that cannot be
 *     found ends with 127 and one found but not executable with 126 (Section
 *     2.8.2).
 *   - IEEE Std 1003.1-2017, `fork()`, `execve()` and `wait()`: the child is a
 *     copy of the shell, replaced by the program, whose status the parent
 *     collects.
 *   - IEEE Std 1003.1-2017, Section 8.1 (Environment Variable Definition): the
 *     environment is NAME=value strings, and Section 2.12: those the shell has
 *     exported.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_ARGUMENT_COUNT_MAXIMUM and
 *     SYSCALL_ARGUMENT_BYTES_MAXIMUM, the bounds `execve` judges both vectors
 *     against, which are what bound the environment a program is given.
 *   - docs/design/SHELL.md, Section 16.
 *
 * Why the search tries each candidate by executing it.
 *
 *   The kernel has no call that asks whether a file exists short of opening
 *   it, and a program found by `open` and then not executable would be a
 *   second call's failure to report. `execve` is tried upon each candidate in
 *   turn, in the child, and ENOENT means "the next directory": whatever else
 *   it says is a program that is there and cannot run, which is the 126 of
 *   Section 2.8.2 and the end of the search. The search therefore happens in
 *   the child, after the fork, which is where the standard puts it too.
 */

#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <line.h>

/* Where a program is sought when PATH is unset: the one directory this
 * system's programs stand in. */
#define SHELL_DEFAULT_PATH "/bin"

/* The environment a program is given: one NAME=value string per exported
 * variable, and the vector over them. Static, for the reason every other
 * store in this shell is. */
static char ShellEnvironmentText[SHELL_VARIABLE_MAXIMUM][SHELL_NAME_MAXIMUM + SHELL_VALUE_MAXIMUM + 2U];
static char *ShellEnvironmentVector[SHELL_VARIABLE_MAXIMUM + 1U];

/* A candidate pathname during the search. */
static char ShellCandidate[SYSCALL_PATH_MAXIMUM + 1U];

char **ShellBuildEnvironment(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < ShellVariableCount(); ++index)
    {
        const char *name;
        const char *value;
        bool exported;

        if (!ShellVariableAt(index, &name, &value, &exported) || !exported)
        {
            continue;
        }

        (void)snprintf(ShellEnvironmentText[count], sizeof ShellEnvironmentText[count], "%s=%s",
                       name, value);
        ShellEnvironmentVector[count] = ShellEnvironmentText[count];
        ++count;
    }

    ShellEnvironmentVector[count] = NULL;

    return ShellEnvironmentVector;
}

/*
 * In the child: tries `execve` upon the name as given where it holds a slash,
 * and upon each directory of PATH otherwise. Returns only upon failure, with
 * the status the shell's child should end with.
 */
static int ShellExecute(char **argv, char **envp)
{
    const char *path;

    if (strchr(argv[0], '/') != NULL)
    {
        (void)OxysExecve(argv[0], argv, envp);

        (void)fprintf(stderr, "sh: %s: %s.\n", argv[0], strerror(errno));

        return (errno == ENOENT) ? 127 : 126;
    }

    path = ShellVariableGet("PATH");

    if ((path == NULL) || (path[0] == '\0'))
    {
        path = SHELL_DEFAULT_PATH;
    }

    while (*path != '\0')
    {
        const char *const colon = strchr(path, ':');
        const size_t directory_length = (colon != NULL) ? (size_t)(colon - path) : strlen(path);
        int written;

        written = snprintf(ShellCandidate, sizeof ShellCandidate, "%.*s/%s",
                           (int)directory_length, path, argv[0]);

        if ((written > 0) && ((size_t)written < sizeof ShellCandidate))
        {
            (void)OxysExecve(ShellCandidate, argv, envp);

            if (errno != ENOENT)
            {
                (void)fprintf(stderr, "sh: %s: %s.\n", ShellCandidate, strerror(errno));

                return 126;
            }
        }

        if (colon == NULL)
        {
            break;
        }

        path = colon + 1;
    }

    (void)fprintf(stderr, "sh: %s: not found.\n", argv[0]);

    return 127;
}


/*
 * Performs a command's redirections, in the child, in the order written —
 * IEEE Std 1003.1-2017, Section 2.7: each opens or duplicates, and then the
 * number the operator names comes to name what was opened. A later one may
 * undo an earlier, which is what `>out 2>&1` means and why the order is
 * kept. Returns false, having said what could not be done, and the child
 * then ends with 1 rather than running a program upon the wrong descriptors.
 */
static bool ShellApplyRedirections(const ShellCommand *command, ShellLookup lookup, void *context)
{
    for (size_t index = 0U; index < command->redirection_count; ++index)
    {
        const ShellRedirection *const io = &command->redirection[index];
        char target[LINE_CAPACITY];
        int64_t opened;
        uint64_t flags;

        if (!ShellExpandWord(io->target, target, sizeof target, lookup, context))
        {
            (void)fprintf(stderr, "sh: a redirection's target could not be expanded.\n");

            return false;
        }

        /* 2.7.5 and 2.7.6: `[n]<&word` and `[n]>&word`, the word a digit or
         * `-`, which closes. */
        if ((io->kind == SHELL_REDIRECT_DUPLICATE_IN) || (io->kind == SHELL_REDIRECT_DUPLICATE_OUT))
        {
            if (strcmp(target, "-") == 0)
            {
                (void)OxysClose(io->descriptor);
                continue;
            }

            if ((target[0] < '0') || (target[0] > '9') || (target[1] != '\0'))
            {
                (void)fprintf(stderr, "sh: %s: not a descriptor.\n", target);

                return false;
            }

            if (OxysDuplicate(target[0] - '0', io->descriptor) < 0)
            {
                (void)fprintf(stderr, "sh: %d: %s.\n", target[0] - '0', strerror(errno));

                return false;
            }

            continue;
        }

        switch (io->kind)
        {
        case SHELL_REDIRECT_INPUT:
            flags = SYSCALL_OPEN_READ;
            break;
        case SHELL_REDIRECT_APPEND:
            flags = SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_APPEND;
            break;
        case SHELL_REDIRECT_READ_WRITE:
            flags = SYSCALL_OPEN_READ | SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE;
            break;
        case SHELL_REDIRECT_OUTPUT:
        case SHELL_REDIRECT_CLOBBER:
        default:
            /* 2.7.2: created or truncated. `>|` is the same, this shell having
             * no `noclobber` to override. */
            flags = SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE;
            break;
        }

        opened = OxysOpen(target, flags, 0644U);

        if (opened < 0)
        {
            (void)fprintf(stderr, "sh: %s: %s.\n", target, strerror(errno));

            return false;
        }

        if ((int)opened != io->descriptor)
        {
            if (OxysDuplicate((int)opened, io->descriptor) < 0)
            {
                (void)fprintf(stderr, "sh: %s: %s.\n", target, strerror(errno));

                return false;
            }

            (void)OxysClose((int)opened);
        }
    }

    return true;
}
int ShellRunProgram(char **argv, const ShellCommand *command, ShellLookup lookup, void *context)
{
    char **const envp = ShellBuildEnvironment();
    int64_t child;
    int64_t status = 0;

    (void)fflush(stdout);

    child = OxysFork();

    if (child < 0)
    {
        (void)fprintf(stderr, "sh: %s: cannot fork: %s.\n", argv[0], strerror(errno));

        return 126;
    }

    if (child == 0)
    {
        /* The child. It ends here whatever happens: a return would carry on
         * reading the shell's input as a second shell. */
        if (!ShellApplyRedirections(command, lookup, context))
        {
            OxysExit(1);
        }

        OxysExit(ShellExecute(argv, envp));
    }

    if (OxysWait(&status) != child)
    {
        (void)fprintf(stderr, "sh: %s: the child could not be collected.\n", argv[0]);

        return 126;
    }

    /*
     * What the child ended with. A program that ended by `exit` has a status
     * in 0 to 255; one ended by a fault has the negated vector, which Section
     * 2.8.2 has reported as 128 plus the signal's number — this system has no
     * signals until 8.7, and the vector stands in for one.
     */
    if (status < 0)
    {
        return 128 + (int)((-status) & 0x7F);
    }

    return (int)(status & 0xFF);
}
