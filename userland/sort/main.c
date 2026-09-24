/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sort/main.c
 * Purpose: Writes the lines of its operands, or of the standard input, in
 *          order — by byte, `-r` for the reverse — to the standard output.
 *          Added on 2026-09-16 beside sub-task 8.7; `ls` does not sort, and
 *          `ls | sort` is the first thing a person wants of a pipe.
 * Key functions: main, SortRead, SortLines, SortCompare.
 * References:
 *   - IEEE Std 1003.1-2017, `sort`: the lines of all input files together,
 *     in collating order; `-r` reverses; with no operand the standard input.
 *     The collation is by byte, this system having no locale.
 *   - docs/design/SHELL.md.
 *
 * The lines are held whole, in a buffer that grows by `realloc`, and sorted by
 * merging — there being no `qsort` in this C library yet, and a merge being
 * stable, which keeps two equal lines in the order they arrived.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define SORT_CHUNK_BYTES 4096U

static bool SortReverse;

static char *SortText;
static size_t SortUsed;
static size_t SortCapacity;

/* Reads a descriptor onto the end of the text. Returns false having said why. */
static bool SortRead(int descriptor, const char *name)
{
    for (;;)
    {
        int64_t read;

        if (SortUsed + SORT_CHUNK_BYTES > SortCapacity)
        {
            char *const grown = realloc(SortText, SortCapacity + SORT_CHUNK_BYTES);

            if (grown == NULL)
            {
                (void)fprintf(stderr, "sort: %s: out of memory.\n", name);

                return false;
            }

            SortText = grown;
            SortCapacity += SORT_CHUNK_BYTES;
        }

        read = OxysRead(descriptor, &SortText[SortUsed], SortCapacity - SortUsed);

        if (read < 0)
        {
            (void)fprintf(stderr, "sort: %s: %s\n", name, strerror(errno));

            return false;
        }

        if (read == 0)
        {
            return true;
        }

        SortUsed += (size_t)read;
    }
}

static int SortCompare(const char *left, const char *right)
{
    const int order = strcmp(left, right);

    return SortReverse ? -order : order;
}

/* A merge sort of an array of line pointers, using `scratch` of the same
 * length. */
static void SortLines(const char **lines, const char **scratch, size_t count)
{
    size_t half;

    if (count < 2U)
    {
        return;
    }

    half = count / 2U;
    SortLines(lines, scratch, half);
    SortLines(&lines[half], &scratch[half], count - half);

    {
        size_t left = 0U;
        size_t right = half;
        size_t out = 0U;

        while ((left < half) && (right < count))
        {
            if (SortCompare(lines[right], lines[left]) < 0)
            {
                scratch[out++] = lines[right++];
            }
            else
            {
                scratch[out++] = lines[left++];
            }
        }

        while (left < half)
        {
            scratch[out++] = lines[left++];
        }

        while (right < count)
        {
            scratch[out++] = lines[right++];
        }

        memcpy(lines, scratch, count * sizeof lines[0]);
    }
}

int main(int argc, char *argv[])
{
    int first = 1;
    bool failed = false;
    size_t count = 0U;
    const char **lines;
    const char **scratch;

    if ((argc > 1) && (strcmp(argv[1], "-r") == 0))
    {
        SortReverse = true;
        first = 2;
    }

    if (first >= argc)
    {
        failed = !SortRead(SYSCALL_DESCRIPTOR_INPUT, "-");
    }

    for (int index = first; index < argc; ++index)
    {
        const int64_t descriptor = OxysOpen(argv[index], SYSCALL_OPEN_READ, 0U);

        if (descriptor < 0)
        {
            (void)fprintf(stderr, "sort: %s: %s\n", argv[index], strerror(errno));
            failed = true;
            continue;
        }

        if (!SortRead((int)descriptor, argv[index]))
        {
            failed = true;
        }

        (void)OxysClose((int)descriptor);
    }

    /* Each newline becomes a terminator, and each line a pointer; a last line
     * without a newline is a line all the same. */
    if ((SortUsed > 0U) && (SortText[SortUsed - 1U] != '\n'))
    {
        if (SortUsed + 1U > SortCapacity)
        {
            char *const grown = realloc(SortText, SortCapacity + 1U);

            if (grown == NULL)
            {
                (void)fprintf(stderr, "sort: out of memory.\n");

                return 2;
            }

            SortText = grown;
            ++SortCapacity;
        }

        SortText[SortUsed] = '\n';
        ++SortUsed;
    }

    for (size_t at = 0U; at < SortUsed; ++at)
    {
        if (SortText[at] == '\n')
        {
            ++count;
        }
    }

    lines = malloc((count + 1U) * sizeof lines[0]);
    scratch = malloc((count + 1U) * sizeof scratch[0]);

    if ((lines == NULL) || (scratch == NULL))
    {
        (void)fprintf(stderr, "sort: out of memory.\n");

        return 2;
    }

    {
        size_t line = 0U;
        size_t start = 0U;

        for (size_t at = 0U; at < SortUsed; ++at)
        {
            if (SortText[at] == '\n')
            {
                SortText[at] = '\0';
                lines[line] = &SortText[start];
                ++line;
                start = at + 1U;
            }
        }
    }

    SortLines(lines, scratch, count);

    for (size_t line = 0U; line < count; ++line)
    {
        (void)fputs(lines[line], stdout);
        (void)putchar('\n');
    }

    if (fflush(stdout) != 0)
    {
        failed = true;
    }

    return failed ? 2 : EXIT_SUCCESS;
}
