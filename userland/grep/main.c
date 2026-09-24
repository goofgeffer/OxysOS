/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/grep/main.c
 * Purpose: Prints the lines of each operand, or of the standard input, that
 *          hold a string — the fixed-string search of `grep -F`, with `-i`,
 *          `-v`, `-n` and `-c`. Added on 2026-09-16 beside sub-task 8.7, for
 *          pipelines: `ls | grep`, `cat f | grep -n word`.
 * Key functions: main, GrepDescriptor, GrepLineMatches.
 * References:
 *   - IEEE Std 1003.1-2017, `grep`: `-F` fixed strings, `-i` ignore case,
 *     `-v` select non-matching lines, `-n` precede each line by its number,
 *     `-c` write only a count; with more than one operand each line is
 *     preceded by its file's name; the status is 0 where a line was selected,
 *     1 where none was, and greater than 1 where an error occurred.
 *   - docs/design/SHELL.md.
 *
 * What is not here: a regular expression. The pattern is a string and a line
 * matches where it holds the string; `-E` and the basic expressions of the
 * standard are a matcher this system does not have yet, and `grep` is worth
 * having before it does, most searches being for a word.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define GREP_BUFFER_BYTES 4096U
#define GREP_LINE_BYTES   1024U

static bool GrepIgnoreCase;
static bool GrepInvert;
static bool GrepNumber;
static bool GrepCountOnly;
static bool GrepNameFiles;
static const char *GrepPattern;
static uint64_t GrepSelected;

static char GrepLower(char byte)
{
    return ((byte >= 'A') && (byte <= 'Z')) ? (char)(byte + ('a' - 'A')) : byte;
}

/* Whether a line holds the pattern, case folded where asked. */
static bool GrepLineMatches(const char *line, size_t length)
{
    const size_t pattern_length = strlen(GrepPattern);

    if (pattern_length == 0U)
    {
        return true;
    }

    if (pattern_length > length)
    {
        return false;
    }

    for (size_t start = 0U; start + pattern_length <= length; ++start)
    {
        size_t at;

        for (at = 0U; at < pattern_length; ++at)
        {
            const char a = GrepIgnoreCase ? GrepLower(line[start + at]) : line[start + at];
            const char b = GrepIgnoreCase ? GrepLower(GrepPattern[at]) : GrepPattern[at];

            if (a != b)
            {
                break;
            }
        }

        if (at == pattern_length)
        {
            return true;
        }
    }

    return false;
}

static void GrepEmit(const char *name, uint64_t number, const char *line, size_t length)
{
    if (GrepNameFiles)
    {
        (void)printf("%s:", name);
    }

    if (GrepNumber)
    {
        (void)printf("%llu:", (unsigned long long)number);
    }

    (void)fwrite(line, 1U, length, stdout);
    (void)putchar('\n');
}

/* Reads a descriptor line by line — a line longer than the buffer is cut,
 * and said to be, once — selecting what matches. Returns false having said
 * why. */
static bool GrepDescriptor(int descriptor, const char *name)
{
    static char buffer[GREP_BUFFER_BYTES];
    static char line[GREP_LINE_BYTES];
    size_t length = 0U;
    uint64_t number = 0U;
    uint64_t count = 0U;
    bool cut = false;

    for (;;)
    {
        const int64_t read = OxysRead(descriptor, buffer, sizeof buffer);

        if (read < 0)
        {
            (void)fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));

            return false;
        }

        for (int64_t index = 0; index < read; ++index)
        {
            const char byte = buffer[index];

            if (byte != '\n')
            {
                if (length + 1U < sizeof line)
                {
                    line[length] = byte;
                    ++length;
                }
                else
                {
                    cut = true;
                }

                continue;
            }

            ++number;

            if (GrepLineMatches(line, length) != GrepInvert)
            {
                ++count;

                if (!GrepCountOnly)
                {
                    GrepEmit(name, number, line, length);
                }
            }

            length = 0U;
        }

        if (read == 0)
        {
            break;
        }
    }

    if (length > 0U)
    {
        ++number;

        if (GrepLineMatches(line, length) != GrepInvert)
        {
            ++count;

            if (!GrepCountOnly)
            {
                GrepEmit(name, number, line, length);
            }
        }
    }

    if (GrepCountOnly)
    {
        if (GrepNameFiles)
        {
            (void)printf("%s:", name);
        }

        (void)printf("%llu\n", (unsigned long long)count);
    }

    if (cut)
    {
        (void)fprintf(stderr, "grep: %s: a line longer than %u bytes was cut.\n", name,
                      (unsigned)(GREP_LINE_BYTES - 1U));
    }

    GrepSelected += count;

    return true;
}

int main(int argc, char *argv[])
{
    int first = 1;
    bool failed = false;

    while ((first < argc) && (argv[first][0] == '-') && (argv[first][1] != '\0'))
    {
        for (const char *option = &argv[first][1]; *option != '\0'; ++option)
        {
            switch (*option)
            {
            case 'i':
                GrepIgnoreCase = true;
                break;
            case 'v':
                GrepInvert = true;
                break;
            case 'n':
                GrepNumber = true;
                break;
            case 'c':
                GrepCountOnly = true;
                break;
            case 'F':
                /* Every pattern here is a fixed string already. */
                break;
            default:
                (void)fprintf(stderr, "grep: -%c: no such option; -i, -v, -n, -c and -F are the options.\n",
                              *option);

                return 2;
            }
        }

        ++first;
    }

    if (first >= argc)
    {
        (void)fprintf(stderr, "grep: a pattern is required: grep [-ivnc] pattern [file...]\n");

        return 2;
    }

    GrepPattern = argv[first];
    ++first;
    GrepNameFiles = (argc - first) > 1;

    if (first >= argc)
    {
        if (!GrepDescriptor(SYSCALL_DESCRIPTOR_INPUT, "-"))
        {
            failed = true;
        }
    }

    for (int index = first; index < argc; ++index)
    {
        const int64_t descriptor = OxysOpen(argv[index], SYSCALL_OPEN_READ, 0U);

        if (descriptor < 0)
        {
            (void)fprintf(stderr, "grep: %s: %s\n", argv[index], strerror(errno));
            failed = true;
            continue;
        }

        if (!GrepDescriptor((int)descriptor, argv[index]))
        {
            failed = true;
        }

        (void)OxysClose((int)descriptor);
    }

    if (fflush(stdout) != 0)
    {
        failed = true;
    }

    if (failed)
    {
        return 2;
    }

    return (GrepSelected > 0U) ? 0 : 1;
}
