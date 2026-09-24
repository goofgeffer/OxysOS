/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/ls/main.c
 * Purpose: Lists the entries of each directory operand, one to a line — the
 *          first program upon this system that reads a directory.
 * Key functions: main, ListDirectory, ListEntryIsHidden, ListComplain.
 * References:
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), `ls`: with no operand the utility
 *     behaves as if "." were given; it writes one entry per line when the output
 *     is not a terminal; names beginning with a period are excluded unless -a is
 *     given; the exit status is 0 upon success and greater than zero otherwise.
 *   - IEEE Std 1003.1-2017, `ls`, the -a option: "Write out all directory
 *     entries, including those whose names begin with a <period>."
 *   - kernel/abi/oxys/syscall_abi.h: SyscallDirectoryEntry, which is what
 *     `OxysReadDirectory` fills, and SYSCALL_OPEN_DIRECTORY.
 *   - docs/design/LIBC.md: the five utilities and what each is
 *     for.
 *
 * What this implements of `ls`, and what it does not.
 *
 *   It implements the default listing and the -a option. It does not sort, does
 *   not list in columns, and implements none of the other twenty-odd options.
 *
 *   **The entries are not sorted**, and that is the one departure from the
 *   default behaviour worth stating plainly rather than burying in a list. POSIX
 *   sorts by the collating sequence of the locale; this system has no locale, and
 *   sorting would need every name held at once — which means a heap sized by a
 *   directory this program has not finished reading. The entries appear in the
 *   order the filesystem returns them, which for EXT2 is the order they stand in
 *   the directory's blocks. docs/design/LIBC.md.
 *
 *   **One entry per line, always.** POSIX writes one per line when the output is
 *   not a terminal and columns when it is. This system cannot tell: there is no
 *   call that says what a descriptor is connected to.
 *
 *   **An operand that is not a directory is written out as itself**, which is
 *   what POSIX requires, and is discovered by the open being refused rather than
 *   by asking what the path names — there being no call here that asks.
 *
 * Why a name is compared against "." and ".." rather than being hidden by its
 * first character alone.
 *
 *   It is not. `ListEntryIsHidden` tests the first character, exactly as POSIX
 *   describes, so a file a person named `.config` is hidden for the same reason
 *   `.` is. The two entries every directory holds are not special-cased, because
 *   special-casing them would make -a show two things and hide the rest.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* Says what went wrong with a path, to the standard error. */
static void ListComplain(const char *path)
{
    (void)fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
}

/* Whether POSIX's default listing excludes this name. */
static bool ListEntryIsHidden(const char *name)
{
    return name[0] == '.';
}

/*
 * Lists one directory. Returns false where anything went wrong, having said so.
 *
 * `heading` is written before the entries where more than one operand was given,
 * which is what POSIX requires so that the entries of two directories are not
 * run together into one list.
 */
static bool ListDirectory(const char *path, bool all, bool heading)
{
    SyscallDirectoryEntry entry;
    int64_t descriptor;
    bool succeeded = true;

    descriptor = OxysOpen(path, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY, 0U);

    if (descriptor < 0)
    {
        /*
         * ENOTDIR is not a failure: it is an operand naming something that is
         * not a directory, and POSIX has such an operand written out as itself.
         * Every other refusal is a failure.
         */
        if (errno == ENOTDIR)
        {
            (void)printf("%s\n", path);

            return true;
        }

        ListComplain(path);

        return false;
    }

    if (heading)
    {
        (void)printf("%s:\n", path);
    }

    for (;;)
    {
        const int64_t result = OxysReadDirectory((int)descriptor, &entry);

        if (result < 0)
        {
            ListComplain(path);
            succeeded = false;
            break;
        }

        /*
         * Zero is the end of the directory and -1 is a failure, and they are
         * told apart here rather than by a single test for "not one". A program
         * that treated a medium failure as the end would stop listing and report
         * that it had finished.
         */
        if (result == 0)
        {
            break;
        }

        if (all || !ListEntryIsHidden(entry.name))
        {
            (void)printf("%s\n", entry.name);
        }
    }

    if (OxysClose((int)descriptor) < 0)
    {
        ListComplain(path);
        succeeded = false;
    }

    return succeeded;
}

int main(int argc, char *argv[])
{
    bool all = false;
    bool succeeded = true;
    int first = 1;
    int operands;

    /*
     * The options are read before anything is listed, and the scan stops at the
     * first operand — POSIX's Utility Syntax Guideline 9. A scan that continued
     * would treat a file named `-a` as an option, and there would be no way to
     * list it.
     */
    while ((first < argc) && (argv[first][0] == '-') && (argv[first][1] != '\0'))
    {
        /* Guideline 10: `--` ends the options and is not an operand. */
        if (strcmp(argv[first], "--") == 0)
        {
            ++first;
            break;
        }

        if (strcmp(argv[first], "-a") == 0)
        {
            all = true;
            ++first;
            continue;
        }

        (void)fprintf(stderr, "ls: %s: this option is not implemented\n", argv[first]);

        return EXIT_FAILURE;
    }

    operands = argc - first;

    if (operands <= 0)
    {
        /*
         * POSIX has `ls` with no operand behave as if "." were given, and since
         * sub-task 8.3 it does: every process holds a working directory and
         * the kernel resolves a relative path against it. Until then this
         * named the root, there being no call that set or reported one —
         * docs/design/LIBC.md which that sub-task
         * closed.
         */
        return ListDirectory(".", all, false) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    for (int index = first; index < argc; ++index)
    {
        if (!ListDirectory(argv[index], all, operands > 1))
        {
            succeeded = false;
        }
    }

    if (fflush(stdout) != 0)
    {
        (void)fprintf(stderr, "ls: the standard output could not be flushed: %s\n",
                      strerror(errno));
        succeeded = false;
    }

    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
