/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/head/main.c
 * Purpose: Copies the first lines of each operand, or of the standard input,
 *          to the standard output — ten unless `-n` says otherwise. Added on
 *          2026-09-16 beside sub-task 8.7, for pipelines: it is the reader
 *          that stops first, and SIGPIPE is what stops the writer behind it.
 * Key functions: main, HeadCopy.
 * References:
 *   - IEEE Std 1003.1-2017, `head`: "copy the first number of lines of each
 *     input file to standard output"; `-n number`; ten lines by default; with
 *     more than one operand, each is preceded by `==> name <==`.
 *   - docs/design/SHELL.md.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define HEAD_BUFFER_BYTES 4096U

static uint64_t HeadLines = 10U;

/* Copies up to HeadLines lines from a descriptor. Returns false having
 * said why. */
static bool HeadCopy(int descriptor, const char *name)
{
    static char buffer[HEAD_BUFFER_BYTES];
    uint64_t seen = 0U;

    while (seen < HeadLines)
    {
        const int64_t read = OxysRead(descriptor, buffer, sizeof buffer);
        int64_t keep = 0;

        if (read < 0)
        {
            (void)fprintf(stderr, "head: %s: %s\n", name, strerror(errno));

            return false;
        }

        if (read == 0)
        {
            return true;
        }

        for (keep = 0; keep < read; ++keep)
        {
            if (buffer[keep] == '\n')
            {
                ++seen;

                if (seen >= HeadLines)
                {
                    ++keep;
                    break;
                }
            }
        }

        if ((keep > 0) && (fwrite(buffer, 1U, (size_t)keep, stdout) != (size_t)keep))
        {
            (void)fprintf(stderr, "head: the standard output could not be written.\n");

            return false;
        }
    }

    return true;
}

int main(int argc, char *argv[])
{
    int first = 1;
    int status = EXIT_SUCCESS;

    /* `-n N`, and the older `-N`, which every hand still types. */
    if ((argc > 1) && (argv[1][0] == '-') && (argv[1][1] >= '0') && (argv[1][1] <= '9'))
    {
        uint64_t number = 0U;

        for (const char *at = &argv[1][1]; *at != '\0'; ++at)
        {
            if ((*at < '0') || (*at > '9'))
            {
                (void)fprintf(stderr, "head: %s is not a number of lines.\n", argv[1]);

                return EXIT_FAILURE;
            }

            number = (number * 10U) + (uint64_t)(*at - '0');
        }

        HeadLines = number;
        first = 2;
    }
    else if ((argc > 2) && (strcmp(argv[1], "-n") == 0))
    {
        uint64_t number = 0U;

        for (const char *at = argv[2]; *at != '\0'; ++at)
        {
            if ((*at < '0') || (*at > '9'))
            {
                (void)fprintf(stderr, "head: -n: %s is not a number of lines.\n", argv[2]);

                return EXIT_FAILURE;
            }

            number = (number * 10U) + (uint64_t)(*at - '0');
        }

        HeadLines = number;
        first = 3;
    }

    if (first >= argc)
    {
        return HeadCopy(SYSCALL_DESCRIPTOR_INPUT, "-") && (fflush(stdout) == 0) ? EXIT_SUCCESS
                                                                                : EXIT_FAILURE;
    }

    for (int index = first; index < argc; ++index)
    {
        const int64_t descriptor = OxysOpen(argv[index], SYSCALL_OPEN_READ, 0U);

        if (descriptor < 0)
        {
            (void)fprintf(stderr, "head: %s: %s\n", argv[index], strerror(errno));
            status = EXIT_FAILURE;
            continue;
        }

        if (argc - first > 1)
        {
            (void)printf("%s==> %s <==\n", (index > first) ? "\n" : "", argv[index]);
        }

        if (!HeadCopy((int)descriptor, argv[index]))
        {
            status = EXIT_FAILURE;
        }

        (void)OxysClose((int)descriptor);
    }

    return (fflush(stdout) == 0) ? status : EXIT_FAILURE;
}
