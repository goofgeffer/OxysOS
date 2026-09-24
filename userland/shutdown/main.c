/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/shutdown/main.c
 * Purpose: `shutdown`, of sub-task 9.3: asks `init` to stop the machine, by a
 *          signal, since a process that is not `init` may not stop it itself.
 * Key functions: main, ShutdownFindInit.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `procinfo`, by which the process named
 *     `init` is found, and `kill`, by which it is asked.
 *   - docs/design/WINDOWS.md: why this asks rather than acts.
 *
 * It does not call `power` itself. The kernel reserves that to `init`, and for
 * a reason a person can see: the machine is stopped in order — the desktop
 * stopped, its windows collected — and only `init` knows what it started.
 * `shutdown` finds `init` by walking the process table for the process of that
 * name, exactly as `ps` walks it, and sends it SIGTERM to halt or SIGINT to
 * restart.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <syscall.h>

/* The process named `init`, or -1 where none was found. */
static int64_t ShutdownFindInit(void)
{
    for (uint64_t index = 0U; index < SYSCALL_PROCESS_CAPACITY; ++index)
    {
        SyscallProcessInformation information;
        const int64_t result = OxysProcessInformation(index, &information);

        if (result < 0)
        {
            break;
        }

        if ((result == 1) && (strcmp(information.name, "init") == 0))
        {
            return (int64_t)information.id;
        }
    }

    return -1;
}

int main(int argc, char *argv[])
{
    bool reboot = false;
    int64_t init;

    for (int index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "-r") == 0)
        {
            reboot = true;
        }
        else if (strcmp(argv[index], "-h") == 0)
        {
            reboot = false;
        }
        else
        {
            (void)fprintf(stderr, "shutdown: usage: shutdown [-h | -r]\n");

            return EXIT_FAILURE;
        }
    }

    init = ShutdownFindInit();

    if (init < 0)
    {
        (void)fprintf(stderr, "shutdown: init is not running; the machine cannot be stopped "
                              "in order.\n");

        return EXIT_FAILURE;
    }

    (void)printf("%s\n", reboot ? "Restarting." : "Shutting down.");

    if (OxysKill(init, reboot ? SIGINT : SIGTERM) < 0)
    {
        (void)fprintf(stderr, "shutdown: init could not be signalled: %s\n", strerror(errno));

        return EXIT_FAILURE;
    }

    /* The machine stops beneath this process; the return is reached only where
     * init declined, and says nothing further. */
    return EXIT_SUCCESS;
}
