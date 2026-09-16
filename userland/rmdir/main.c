/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/rmdir/main.c
 * Purpose: Removes each directory operand, which must be empty — the utility
 *          `rm` could not stand in for since sub-task 7.6, there being no call
 *          that removed a directory until 8.5 exposed one.
 * Key functions: main.
 * References:
 *   - IEEE Std 1003.1-2017, `rmdir`: "the rmdir utility shall remove the
 *     directory entry specified by each dir operand", each being empty; the
 *     exit status is 0 where every operand was removed.
 *   - libc/include/syscall.h: OxysRemoveDirectory, and the refusals it reports.
 *   - docs/design/SHELL.md, Section 19.3.
 *
 * `-p`, which removes each component of the operand in turn, is not here.
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
        (void)fprintf(stderr, "rmdir: an operand is required.\n");

        return EXIT_FAILURE;
    }

    for (int index = 1; index < argc; ++index)
    {
        if (OxysRemoveDirectory(argv[index]) < 0)
        {
            (void)fprintf(stderr, "rmdir: %s: %s\n", argv[index], strerror(errno));
            status = EXIT_FAILURE;
        }
    }

    return status;
}
