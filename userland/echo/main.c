/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/echo/main.c
 * Purpose: Writes its operands to the standard output, separated by one space
 *          and followed by one newline — the whole of IEEE Std 1003.1-2017's
 *          `echo`, and the first program upon this system whose behaviour
 *          depends entirely upon what it was given rather than upon what it
 *          contains.
 * Key functions: main.
 * References:
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), `echo`: "The echo utility arguments
 *     shall be separated by single <space> characters and a <newline> character
 *     shall follow the last argument." The utility "shall not recognize any
 *     options", and `--` is an operand and not a delimiter.
 *   - IEEE Std 1003.1-2017, `echo`, OPERANDS: where the first operand is `-n`,
 *     or an operand contains a backslash, the results are implementation
 *     defined. What this implementation does with each is recorded below.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1:
 *     the argument vector, which sub-task 7.6 is the first thing here to read.
 *   - docs/design/LIBC.md: the five utilities and what each one
 *     is for.
 *
 * What this does with the two implementation-defined cases, and why.
 *
 *   **`-n` is an operand.** It is written out like any other. The alternative —
 *   suppressing the trailing newline — is what several systems do, and it makes
 *   `echo` a utility that cannot print the two characters `-n`. A shell needing
 *   output without a newline has `printf`, which this system does not have yet
 *   and which is the right place for it.
 *
 *   **A backslash is an ordinary character.** No sequence is interpreted. The
 *   XSI variant expands `\n`, `\t`, `\c` and octal escapes, and the consequence
 *   is that `echo` cannot print a path containing a backslash unaltered. Neither
 *   behaviour is more conforming than the other; this one is the one a person
 *   can predict without knowing which variant they have.
 *
 * Why the output goes through one `fputs` a word at a time and not one `printf`.
 *
 *   `printf` would have to be handed the operand as a conversion argument, and
 *   an operand containing a percent sign would then be read as a conversion
 *   specification — the classic format-string defect, in the one program of this
 *   set whose whole input is somebody else's text. `fputs` has no format.
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    /*
     * From one and not from zero: `argv[0]` is the name the program was invoked
     * by, which Section 3.4.1 puts in the vector and which IEEE Std 1003.1-2017
     * does not count among the operands.
     */
    for (int index = 1; index < argc; ++index)
    {
        if (index > 1)
        {
            if (fputc(' ', stdout) == EOF)
            {
                return EXIT_FAILURE;
            }
        }

        if (fputs(argv[index], stdout) == EOF)
        {
            return EXIT_FAILURE;
        }
    }

    if (fputc('\n', stdout) == EOF)
    {
        return EXIT_FAILURE;
    }

    /*
     * The stream is flushed here rather than left to `exit`, so that a failure to
     * transmit is reported by the status rather than discovered by nobody.
     * ISO/IEC 9899:2011, Section 7.22.4.4, has `exit` flush every open stream,
     * and it has no way to tell anybody that the flush failed.
     */
    return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
