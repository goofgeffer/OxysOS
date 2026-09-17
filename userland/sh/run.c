/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/run.c
 * Purpose: External program execution, of sub-task 8.4: the search for a
 *          command's program, the environment built from the exported
 *          variables, the `fork`, the `execve` in the child, and the `wait`
 *          that turns what the child ended with into a status — and, of 8.5,
 *          the redirections performed in the child between the two — and, of
 *          8.6, the pipeline: one child per command and a pipe between each
 *          pair, every child collected and the status the last one's.
 * Key functions: ShellRunProgram, ShellExecuteProgram, ShellRunPipeline,
 *          ShellDescribePipeline,
 *          ShellBuildEnvironment, ShellApplyRedirections.
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
 *   - IEEE Std 1003.1-2017, Section 2.9.2 (Pipelines): the standard output of
 *     each command is connected to the standard input of the next, each
 *     command runs in a subshell environment, the shell waits for the last
 *     command, and the status is the last command's unless `!` inverts it.
 *   - IEEE Std 1003.1-2017, `pipe()`: the read end first, the write end second.
 *   - docs/design/SHELL.md, Sections 16 and 22.
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
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <line.h>
#include <signal.h>

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

int ShellExecuteProgram(char **argv, const ShellCommand *command, ShellLookup lookup,
                        void *context)
{
    char **const envp = ShellBuildEnvironment();

    if (!ShellApplyRedirections(command, lookup, context))
    {
        return 1;
    }

    return ShellExecute(argv, envp);
}

/*
 * The text a job is reported by: the words of each command as typed, quotes
 * kept, joined by ` | `. What a person sees in `jobs` is what they typed,
 * which is the one description of a job they will recognise.
 */
static void ShellDescribePipeline(const ShellPipeline *pipeline, char *text, size_t capacity)
{
    size_t used = 0U;

    text[0] = '\0';

    for (size_t index = 0U; index < pipeline->command_count; ++index)
    {
        const ShellCommand *const command = &pipeline->command[index];

        if (index > 0U)
        {
            used += (size_t)snprintf(&text[used], (used < capacity) ? capacity - used : 0U, " | ");
        }

        for (size_t word = 0U; word < command->word_count; ++word)
        {
            used += (size_t)snprintf(&text[used], (used < capacity) ? capacity - used : 0U, "%s%s",
                                     (word > 0U) ? " " : "", command->word[word]);
        }
    }
}

/*
 * A program run alone, since sub-task 8.7 a job of one: forked into a group of
 * its own, given the terminal while it runs, and waited for until it ends or
 * stops — so that control-C reaches it and not the shell, and control-Z stops
 * it and returns the prompt. Its text for `jobs` is its argument vector.
 */
int ShellRunProgram(char **argv, const ShellCommand *command, ShellLookup lookup, void *context)
{
    char text[LINE_CAPACITY];
    size_t used = 0U;
    ShellJob *job;
    int64_t child;

    text[0] = '\0';

    for (size_t index = 0U; argv[index] != NULL; ++index)
    {
        used += (size_t)snprintf(&text[used], (used < sizeof text) ? sizeof text - used : 0U,
                                 "%s%s", (index > 0U) ? " " : "", argv[index]);
    }

    (void)fflush(stdout);

    job = ShellJobBegin(text, false);

    if (job == NULL)
    {
        return 126;
    }

    child = OxysFork();

    if (child < 0)
    {
        (void)fprintf(stderr, "sh: %s: cannot fork: %s.\n", argv[0], strerror(errno));

        return 126;
    }

    if (child == 0)
    {
        ShellJobPrepareChild(job, false);
        OxysExit(ShellExecuteProgram(argv, command, lookup, context));
    }

    ShellJobAddMember(job, child);

    return ShellJobWaitForeground(job);
}

/*
 * The pipeline, of sub-task 8.6, and since 8.7 a job.
 *
 * One child per command, each made before the next, and one pipe between each
 * pair: a child's 1 is the write end of the pipe that follows it and its 0 the
 * read end of the pipe before it, placed by `dup2` before the command's own
 * redirections so that `a 2>&1 | b` sends `a`'s diagnostics down the pipe as
 * Section 2.9.2 has it. Every end the child does not need is closed in the
 * child, and every end the shell holds is closed once the children that need
 * it have been made — because a pipe's reader sees the end of the file only
 * when the *last* write end is closed, and a write end left open in the shell
 * would keep every reader waiting for a writer that had already ended.
 *
 * Each command is run in its child by the function the caller supplies, which
 * runs a built-in there or becomes the program; the child ends with what it
 * returns. So a built-in in a pipeline runs in a subshell, which is what
 * Section 2.9.2 specifies and what makes `help | cat` mean something, and a
 * `cd` in a pipeline moves nothing but the child — the same surprise every
 * shell of this lineage offers.
 *
 * Every child joins the job's process group, the first child's identifier,
 * and a foreground job holds the terminal while it runs. The shell waits for
 * a foreground job until it ends or stops, and the status is the last
 * command's, as Section 2.9.2 has it, or 128 plus SIGTSTP for a job that
 * stopped; a background job is announced and left to run, and reported at a
 * later prompt. A child that could not be made ends the making, and the
 * children already made are still waited for, so that none is left in the
 * table.
 */
int ShellRunPipeline(const ShellPipeline *pipeline, ShellStageRunner run_stage, void *context,
                     bool background)
{
    char text[LINE_CAPACITY];
    ShellJob *job;
    size_t made = 0U;
    int previous_read = -1;

    ShellDescribePipeline(pipeline, text, sizeof text);
    (void)fflush(stdout);

    job = ShellJobBegin(text, background);

    if (job == NULL)
    {
        return 126;
    }

    for (size_t index = 0U; index < pipeline->command_count; ++index)
    {
        const bool last = (index + 1U == pipeline->command_count);
        int ends[2] = { -1, -1 };
        int64_t child;

        if (!last && (OxysPipe(ends) < 0))
        {
            (void)fprintf(stderr, "sh: cannot make a pipe: %s.\n", strerror(errno));
            break;
        }

        child = OxysFork();

        if (child < 0)
        {
            (void)fprintf(stderr, "sh: cannot fork: %s.\n", strerror(errno));

            if (!last)
            {
                (void)OxysClose(ends[0]);
                (void)OxysClose(ends[1]);
            }

            break;
        }

        if (child == 0)
        {
            ShellJobPrepareChild(job, background);

            if (previous_read >= 0)
            {
                if (OxysDuplicate(previous_read, SYSCALL_DESCRIPTOR_INPUT) < 0)
                {
                    (void)fprintf(stderr, "sh: the pipe could not be placed at 0: %s.\n",
                                  strerror(errno));
                    OxysExit(1);
                }

                (void)OxysClose(previous_read);
            }

            if (!last)
            {
                if (OxysDuplicate(ends[1], SYSCALL_DESCRIPTOR_OUTPUT) < 0)
                {
                    (void)fprintf(stderr, "sh: the pipe could not be placed at 1: %s.\n",
                                  strerror(errno));
                    OxysExit(1);
                }

                (void)OxysClose(ends[0]);
                (void)OxysClose(ends[1]);
            }

            /* The stage runs here, in the child, and its result is the
             * child's status; `exit` flushes what a built-in printed into the
             * pipe before the write end goes with the process. */
            exit(run_stage(&pipeline->command[index], context));
        }

        ShellJobAddMember(job, child);
        ++made;

        if (previous_read >= 0)
        {
            (void)OxysClose(previous_read);
        }

        if (!last)
        {
            (void)OxysClose(ends[1]);
            previous_read = ends[0];
        }
    }

    if (previous_read >= 0)
    {
        (void)OxysClose(previous_read);
    }

    if (made == 0U)
    {
        return 126;
    }

    if (background)
    {
        ShellJobAnnounce(job);

        return 0;
    }

    return ShellJobWaitForeground(job);
}
