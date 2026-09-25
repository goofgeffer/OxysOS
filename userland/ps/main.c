/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/ps/main.c
 * Purpose: Lists the processes: identifier, parent, group, state, pages
 *          mapped and name, one to a line, by walking the kernel's table
 *          through `procinfo`. Added on 2026-09-16 beside sub-task 8.7, when
 *          there first were processes a person had left running.
 * Key functions: main, PsStateText.
 * References:
 *   - IEEE Std 1003.1-2017, `ps`: the columns PID, PPID, PGID and the command
 *     name, which are the ones this has; the state column is this system's.
 *   - kernel/abi/oxys/syscall_abi.h: SyscallProcessInformation and the
 *     `procinfo` call, which is one slot of the table per call.
 *   - docs/design/SHELL.md.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static const char *PsStateText(uint64_t state)
{
    switch (state)
    {
    case SYSCALL_PROCESS_STATE_RUNNING:
        return "running";
    case SYSCALL_PROCESS_STATE_BLOCKED:
        return "sleeping";
    case SYSCALL_PROCESS_STATE_STOPPED:
        return "stopped";
    case SYSCALL_PROCESS_STATE_EXITED:
        return "exited";
    case SYSCALL_PROCESS_STATE_READY:
    default:
        return "ready";
    }
}

int main(void)
{
    (void)printf("%5s %5s %5s %-9s %6s %s\n", "PID", "PPID", "PGID", "STATE", "PAGES", "NAME");

    for (uint64_t index = 0U;; ++index)
    {
        SyscallProcessInformation information;
        const int64_t result = OxysProcessInformation(index, &information);

        /* EINVAL is the end of the table, which grows and so has no fixed
         * bound to walk to. */
        if ((result < 0) && (errno == EINVAL))
        {
            break;
        }

        if (result < 0)
        {
            (void)fprintf(stderr, "ps: %s\n", strerror(errno));

            return EXIT_FAILURE;
        }

        if (result == 0)
        {
            continue;
        }

        (void)printf("%5llu %5llu %5llu %-9s %6llu %s\n", (unsigned long long)information.id,
                     (unsigned long long)information.parent,
                     (unsigned long long)information.group, PsStateText(information.state),
                     (unsigned long long)information.pages, information.name);
    }

    return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
