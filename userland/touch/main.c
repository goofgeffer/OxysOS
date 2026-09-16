/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/touch/main.c
 * Purpose: Creates each operand that does not exist, and leaves each that
 *          does as it is — the first utility here to make a file, which
 *          sub-task 8.5's writable `open` made possible.
 * Key functions: main.
 * References:
 *   - IEEE Std 1003.1-2017, `touch`: "the utility shall change the last data
 *     modification timestamps ... and shall create the file if it does not
 *     exist". This system keeps no clock, so the first half is not done and
 *     the second is the whole of what `touch` is here.
 *   - libc/include/syscall.h: OxysOpen with SYSCALL_OPEN_CREATE.
 *   - docs/design/SHELL.md, Section 19.3.
 *
 * What is not here: the timestamps, there being no clock to take one from;
 * `-a`, `-m`, `-c`, `-r` and `-t`, each of which is about them. An operand
 * that is a directory is left as it is, `open` for writing refusing it, and
 * that is reported, because a person who touched a directory by mistake is
 * better told than not.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

int main(int argc, char *argv[])
{
    int status = EXIT_SUCCESS;

    if (argc < 2)
    {
        (void)fprintf(stderr, "touch: an operand is required.\n");

        return EXIT_FAILURE;
    }

    for (int index = 1; index < argc; ++index)
    {
        const int64_t descriptor = OxysOpen(argv[index], SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE,
                                            0644U);

        if (descriptor < 0)
        {
            (void)fprintf(stderr, "touch: %s: %s\n", argv[index], strerror(errno));
            status = EXIT_FAILURE;
            continue;
        }

        (void)OxysClose((int)descriptor);
    }

    return status;
}
