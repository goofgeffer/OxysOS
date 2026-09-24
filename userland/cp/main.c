/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/cp/main.c
 * Purpose: Copies one file to another, created or truncated — the first
 *          utility here to write what it read, which sub-task 8.5's writable
 *          `open` and file-reaching `write` made possible.
 * Key functions: main, CopyFile.
 * References:
 *   - IEEE Std 1003.1-2017, `cp`: the first synopsis form, `cp source_file
 *     target_file`, "copy the contents of source_file ... to the destination
 *     path named by target_file"; the exit status is 0 where the file was
 *     copied.
 *   - libc/include/syscall.h: OxysOpen, OxysRead, OxysWrite, OxysClose.
 *   - docs/design/SHELL.md.
 *
 * What is not here: the second and third forms, which copy into a directory
 * and need a name joined to a path; `-R`, `-p`, `-f` and `-i`. One source and
 * one target, because that is what a person at this prompt does with a file
 * and the rest can be added when they do something else.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* One transfer's worth, which is the most a single call moves. */
#define COPY_BUFFER_BYTES 4096

static char CopyBuffer[COPY_BUFFER_BYTES];

static int CopyFile(const char *source, const char *target)
{
    const int64_t from = OxysOpen(source, SYSCALL_OPEN_READ, 0U);
    int64_t to;

    if (from < 0)
    {
        (void)fprintf(stderr, "cp: %s: %s\n", source, strerror(errno));

        return EXIT_FAILURE;
    }

    to = OxysOpen(target, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE, 0644U);

    if (to < 0)
    {
        (void)fprintf(stderr, "cp: %s: %s\n", target, strerror(errno));
        (void)OxysClose((int)from);

        return EXIT_FAILURE;
    }

    for (;;)
    {
        const int64_t got = OxysRead((int)from, CopyBuffer, sizeof CopyBuffer);
        int64_t done = 0;

        if (got < 0)
        {
            (void)fprintf(stderr, "cp: %s: %s\n", source, strerror(errno));
            (void)OxysClose((int)from);
            (void)OxysClose((int)to);

            return EXIT_FAILURE;
        }

        if (got == 0)
        {
            break;
        }

        /* A short write is repeated from where it stopped, which the kernel
         * is entitled to require. */
        while (done < got)
        {
            const int64_t put = OxysWrite((int)to, &CopyBuffer[done], (size_t)(got - done));

            if (put <= 0)
            {
                (void)fprintf(stderr, "cp: %s: %s\n", target, strerror(errno));
                (void)OxysClose((int)from);
                (void)OxysClose((int)to);

                return EXIT_FAILURE;
            }

            done += put;
        }
    }

    (void)OxysClose((int)from);
    (void)OxysClose((int)to);

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        (void)fprintf(stderr, "cp: a source and a target are required.\n");

        return EXIT_FAILURE;
    }

    return CopyFile(argv[1], argv[2]);
}
