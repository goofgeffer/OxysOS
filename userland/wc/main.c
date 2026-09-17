/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/wc/main.c
 * Purpose: Counts the newlines, words and bytes of each operand, or of the
 *          standard input where there is none — the first utility here whose
 *          reason to exist is the pipeline of sub-task 8.6, `wc` being what a
 *          person puts at the end of one to learn how much came through it.
 * Key functions: main, WcCount, WcPrint.
 * References:
 *   - IEEE Std 1003.1-2017, `wc`: with no option "the number of <newline>
 *     characters, words, and bytes contained in each input file" in that
 *     order, by the format "%d %d %d %s\n"; "a word is a non-zero-length
 *     string of characters delimited by white space"; with more than one
 *     operand "an additional line shall be written" totalling them, with
 *     `total` for the name; and with no operand "the standard input shall be
 *     used" and no name is written.
 *   - IEEE Std 1003.1-2017, `wc`, OPTIONS: -c bytes, -l newlines, -w words,
 *     any of which selects that count alone, in the order above whatever the
 *     order given.
 *   - docs/design/SHELL.md, Section 22.4.
 *
 * What is not here: `-m`, the count of characters, which in this system's one
 * encoding is the count of bytes; and a `-` operand, the standard input being
 * read only where there is no operand at all, because a pipeline names none.
 *
 * The standard input is read to its end and not to a control-D, unlike `cat`:
 * `wc` is what a pipe feeds, and a pipe ends when its writer does. A person
 * who runs `wc` against the terminal by mistake ends it with a control-C, since
 * 8.7; a control-D the terminal delivers as a byte, counted.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define WC_BUFFER_BYTES 4096U

typedef struct WcCounts
{
    uint64_t lines;
    uint64_t words;
    uint64_t bytes;
} WcCounts;

/* Which counts to print. All three where no option was given. */
static bool WcPrintLines;
static bool WcPrintWords;
static bool WcPrintBytes;

static bool WcIsSpace(char byte)
{
    return (byte == ' ') || (byte == '\n') || (byte == '\t') || (byte == '\r') ||
           (byte == '\f') || (byte == '\v');
}

/* Reads a descriptor to its end, counting. Returns false having said why. */
static bool WcCount(int descriptor, const char *name, WcCounts *counts)
{
    static char buffer[WC_BUFFER_BYTES];
    bool in_word = false;

    counts->lines = 0U;
    counts->words = 0U;
    counts->bytes = 0U;

    for (;;)
    {
        const int64_t read = OxysRead(descriptor, buffer, sizeof buffer);

        if (read < 0)
        {
            (void)fprintf(stderr, "wc: %s: %s\n", name, strerror(errno));

            return false;
        }

        if (read == 0)
        {
            return true;
        }

        counts->bytes += (uint64_t)read;

        for (int64_t index = 0; index < read; ++index)
        {
            const char byte = buffer[index];

            if (byte == '\n')
            {
                ++counts->lines;
            }

            if (WcIsSpace(byte))
            {
                in_word = false;
            }
            else if (!in_word)
            {
                in_word = true;
                ++counts->words;
            }
        }
    }
}

/* One line: the selected counts in the standard's order, then the name where
 * there is one. */
static void WcPrint(const WcCounts *counts, const char *name)
{
    const char *separator = "";

    if (WcPrintLines)
    {
        (void)printf("%s%llu", separator, (unsigned long long)counts->lines);
        separator = " ";
    }

    if (WcPrintWords)
    {
        (void)printf("%s%llu", separator, (unsigned long long)counts->words);
        separator = " ";
    }

    if (WcPrintBytes)
    {
        (void)printf("%s%llu", separator, (unsigned long long)counts->bytes);
        separator = " ";
    }

    if (name != NULL)
    {
        (void)printf("%s%s", separator, name);
    }

    (void)printf("\n");
}

int main(int argc, char *argv[])
{
    int status = EXIT_SUCCESS;
    int first = 1;
    int operands;
    WcCounts total = { 0U, 0U, 0U };

    /* The options: any of -c, -l and -w, in any order, together or apart. An
     * operand that begins with a dash and is none of them is refused rather
     * than counted, because a person who typed `wc -x` meant an option. */
    while ((first < argc) && (argv[first][0] == '-') && (argv[first][1] != '\0'))
    {
        for (const char *option = &argv[first][1]; *option != '\0'; ++option)
        {
            switch (*option)
            {
            case 'c':
                WcPrintBytes = true;
                break;
            case 'l':
                WcPrintLines = true;
                break;
            case 'w':
                WcPrintWords = true;
                break;
            default:
                (void)fprintf(stderr, "wc: -%c: no such option; -c, -l and -w are the options.\n",
                              *option);

                return EXIT_FAILURE;
            }
        }

        ++first;
    }

    if (!WcPrintLines && !WcPrintWords && !WcPrintBytes)
    {
        WcPrintLines = true;
        WcPrintWords = true;
        WcPrintBytes = true;
    }

    operands = argc - first;

    if (operands == 0)
    {
        WcCounts counts;

        if (!WcCount(SYSCALL_DESCRIPTOR_INPUT, "-", &counts))
        {
            return EXIT_FAILURE;
        }

        WcPrint(&counts, NULL);

        return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    for (int index = first; index < argc; ++index)
    {
        WcCounts counts;
        const int64_t descriptor = OxysOpen(argv[index], SYSCALL_OPEN_READ, 0U);

        if (descriptor < 0)
        {
            (void)fprintf(stderr, "wc: %s: %s\n", argv[index], strerror(errno));
            status = EXIT_FAILURE;
            continue;
        }

        if (WcCount((int)descriptor, argv[index], &counts))
        {
            WcPrint(&counts, argv[index]);
            total.lines += counts.lines;
            total.words += counts.words;
            total.bytes += counts.bytes;
        }
        else
        {
            status = EXIT_FAILURE;
        }

        (void)OxysClose((int)descriptor);
    }

    if (operands > 1)
    {
        WcPrint(&total, "total");
    }

    if (fflush(stdout) != 0)
    {
        status = EXIT_FAILURE;
    }

    return status;
}
