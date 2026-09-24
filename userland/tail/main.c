/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/tail/main.c
 * Purpose: Copies the last lines of each operand, or of the standard input,
 *          to the standard output — ten unless `-n` says otherwise. Added on
 *          2026-09-16 beside sub-task 8.7.
 * Key functions: main, TailCopy.
 * References:
 *   - IEEE Std 1003.1-2017, `tail`: "copy the last part of a file"; `-n
 *     number`, the last `number` lines; ten by default.
 *   - docs/design/SHELL.md.
 *
 * The whole input is read before anything is written, because the last lines
 * of a stream are unknowable until it ends; it is kept in a buffer that grows
 * by `realloc`, which is the first utility here whose memory is the size of
 * its input. There is no `lseek` a program can reach, so a file is read the
 * same way a pipe is.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define TAIL_CHUNK_BYTES 4096U

static uint64_t TailLines = 10U;

static bool TailCopy(int descriptor, const char *name)
{
    char *held = NULL;
    size_t used = 0U;
    size_t capacity = 0U;
    size_t start;
    uint64_t seen = 0U;
    bool ok = true;

    for (;;)
    {
        int64_t read;

        if (used + TAIL_CHUNK_BYTES > capacity)
        {
            char *const grown = realloc(held, capacity + TAIL_CHUNK_BYTES);

            if (grown == NULL)
            {
                (void)fprintf(stderr, "tail: %s: out of memory.\n", name);
                free(held);

                return false;
            }

            held = grown;
            capacity += TAIL_CHUNK_BYTES;
        }

        read = OxysRead(descriptor, &held[used], capacity - used);

        if (read < 0)
        {
            (void)fprintf(stderr, "tail: %s: %s\n", name, strerror(errno));
            free(held);

            return false;
        }

        if (read == 0)
        {
            break;
        }

        used += (size_t)read;
    }

    /* Backwards from the end, counting newlines; a last line without one
     * counts as a line all the same. */
    start = used;

    if ((used > 0U) && (held[used - 1U] != '\n'))
    {
        seen = 1U;
    }

    while (start > 0U)
    {
        if ((held[start - 1U] == '\n') && (start != used))
        {
            ++seen;

            if (seen >= TailLines)
            {
                break;
            }
        }

        --start;
    }

    if ((used > start) && (fwrite(&held[start], 1U, used - start, stdout) != used - start))
    {
        (void)fprintf(stderr, "tail: the standard output could not be written.\n");
        ok = false;
    }

    free(held);

    return ok;
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
                (void)fprintf(stderr, "tail: %s is not a number of lines.\n", argv[1]);

                return EXIT_FAILURE;
            }

            number = (number * 10U) + (uint64_t)(*at - '0');
        }

        TailLines = number;
        first = 2;
    }
    else if ((argc > 2) && (strcmp(argv[1], "-n") == 0))
    {
        uint64_t number = 0U;

        for (const char *at = argv[2]; *at != '\0'; ++at)
        {
            if ((*at < '0') || (*at > '9'))
            {
                (void)fprintf(stderr, "tail: -n: %s is not a number of lines.\n", argv[2]);

                return EXIT_FAILURE;
            }

            number = (number * 10U) + (uint64_t)(*at - '0');
        }

        TailLines = number;
        first = 3;
    }

    if (first >= argc)
    {
        return TailCopy(SYSCALL_DESCRIPTOR_INPUT, "-") && (fflush(stdout) == 0) ? EXIT_SUCCESS
                                                                                : EXIT_FAILURE;
    }

    for (int index = first; index < argc; ++index)
    {
        const int64_t descriptor = OxysOpen(argv[index], SYSCALL_OPEN_READ, 0U);

        if (descriptor < 0)
        {
            (void)fprintf(stderr, "tail: %s: %s\n", argv[index], strerror(errno));
            status = EXIT_FAILURE;
            continue;
        }

        if (argc - first > 1)
        {
            (void)printf("%s==> %s <==\n", (index > first) ? "\n" : "", argv[index]);
        }

        if (!TailCopy((int)descriptor, argv[index]))
        {
            status = EXIT_FAILURE;
        }

        (void)OxysClose((int)descriptor);
    }

    return (fflush(stdout) == 0) ? status : EXIT_FAILURE;
}
