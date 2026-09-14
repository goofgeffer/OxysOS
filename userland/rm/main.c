/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/rm/main.c
 * Purpose: Removes each operand — the only program upon this system that
 *          destroys anything a person put upon a volume.
 * Key functions: main, RemoveOne.
 * References:
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), `rm`: the synopsis is
 *     `rm [-iRr] file...` and `rm -f [-iRr] [file...]`; -f suppresses the
 *     prompts and the diagnostic for a file that is not there; where an operand
 *     names a directory and neither -r nor -R was given, "rm shall write a
 *     diagnostic message to standard error, do nothing more with file, and go on
 *     to any remaining files". The exit status is 0 where every entry was
 *     removed and greater than zero otherwise.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_UNLINK, beneath `OxysUnlink`.
 *   - docs/design/LIBC.md, Section 12.3: the five utilities and what each is
 *     for.
 *
 * What is implemented, and what each absence is a property of.
 *
 *   **-f is implemented**, in the half that means anything here: a file that is
 *   not there is neither a diagnostic nor a failure. Its other half — suppressing
 *   the prompt — has nothing to suppress.
 *
 *   **-i is not implemented**, and cannot be: it prompts upon the standard error
 *   and reads the answer from the standard input, and this kernel has no call
 *   that reads. A program that accepted -i and prompted nobody would delete
 *   everything it asked about.
 *
 *   **-r and -R are not implemented**, because there is no call that removes a
 *   directory. `OxysUnlink` refuses one with EISDIR, so a recursive removal
 *   could empty a directory and then be unable to remove it — which is worse
 *   than not offering the option, since the person would be left with the
 *   contents gone and the directory still there. docs/design/LIBC.md, Section
 *   12.7, limitation 3.
 *
 *   The consequence is that a directory is always the case POSIX describes for
 *   an operand without -r: a diagnostic, no removal, and on to the next operand.
 *   The diagnostic arrives from the kernel's own EISDIR and is not composed here,
 *   so it says what actually happened rather than what this program assumed.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* Removes one name. Returns false where it could not, having said why unless -f
 * was given and the cause was that there was nothing of that name. */
static bool RemoveOne(const char *path, bool force)
{
    if (OxysUnlink(path) >= 0)
    {
        return true;
    }

    if (force && (errno == ENOENT))
    {
        return true;
    }

    (void)fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));

    return false;
}

int main(int argc, char *argv[])
{
    bool force = false;
    bool succeeded = true;
    int first = 1;

    while ((first < argc) && (argv[first][0] == '-') && (argv[first][1] != '\0'))
    {
        if (strcmp(argv[first], "--") == 0)
        {
            ++first;
            break;
        }

        if (strcmp(argv[first], "-f") == 0)
        {
            force = true;
            ++first;
            continue;
        }

        (void)fprintf(stderr, "rm: %s: this option is not implemented\n", argv[first]);

        return EXIT_FAILURE;
    }

    /*
     * With no operand, -f is silent and success, and its absence is a
     * diagnostic and a failure. That is exactly what the two synopses of IEEE
     * Std 1003.1-2017 say: `rm -f [-iRr] [file...]` makes the operand optional
     * and `rm [-iRr] file...` does not.
     */
    if (first >= argc)
    {
        if (force)
        {
            return EXIT_SUCCESS;
        }

        (void)fprintf(stderr, "rm: no file was named\n");

        return EXIT_FAILURE;
    }

    for (int index = first; index < argc; ++index)
    {
        if (!RemoveOne(argv[index], force))
        {
            succeeded = false;
        }
    }

    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
