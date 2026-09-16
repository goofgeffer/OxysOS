/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/cat/main.c
 * Purpose: Writes the contents of each operand to the standard output in the
 *          order the operands are given — the first program upon this system
 *          that reads a file.
 * Key functions: main, CatFile, CatStandardInput, CatComplain.
 * References:
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), `cat`: "The standard output shall
 *     contain the sequence of bytes read from the input files. Nothing else
 *     shall be written to the standard output." The exit status is 0 where all
 *     input files were output successfully and greater than zero otherwise.
 *   - IEEE Std 1003.1-2017, `cat`, OPERANDS: an operand of `-` reads the
 *     standard input, and no operand does likewise. Both since sub-task 8.5,
 *     for the reason below.
 *   - kernel/abi/oxys/syscall_abi.h: the calls beneath `OxysOpen`, `OxysRead`
 *     and `OxysClose`.
 *   - docs/design/LIBC.md, Section 12.3: the five utilities and what each is
 *     for.
 *
 * What this does not do, and why each is a property of the system.
 *
 *   **No operand, or the operand `-`, copies the standard input — since
 *   sub-task 8.5.** POSIX has it so, and until then this program refused: this
 *   kernel had no call that read a stream when it was written, and from 8.1
 *   to 8.4 the standard input was a raw terminal that no line discipline
 *   stood in front of, so a copy of it would have delivered keystrokes with
 *   no end. Since 8.5 the shell redirects it — `cat <file` — and a copy of
 *   the terminal ends at a control-D, which CatStandardInput records the
 *   reason for.
 *
 *   **`-u` is not recognised.** POSIX has it mean that the output is not to be
 *   buffered; this writes through the C library's buffered stream either way,
 *   and a flag that was accepted and ignored would be a claim the program does
 *   not meet.
 *
 * Why the diagnostics go to the standard error and the contents to the standard
 * output, when both presently reach the same serial channel.
 *
 *   Because the system call distinguishes them and the shell of Phase 8 will
 *   redirect them separately. A program that wrote its complaints to the
 *   standard output would put them in the middle of the bytes it was copying,
 *   and the defect would not appear until the first time somebody redirected
 *   the output to a file.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* How many bytes are asked for at a time. It is the greatest a single call of
 * this kernel transfers, so a larger buffer would be a buffer whose upper part
 * was never filled; and the loop below must handle a short read in any case,
 * since the kernel is entitled to return fewer than were asked for. */
#define CAT_BUFFER_BYTES 4096

/* Says what went wrong with a path, to the standard error, in the form every
 * utility here uses: the program, the path, and what the C library calls the
 * errno. */
static void CatComplain(const char *path)
{
    (void)fprintf(stderr, "cat: %s: %s\n", path, strerror(errno));
}

/* Copies one file to the standard output. Returns false where anything went
 * wrong, having already said what. */
static bool CatFile(const char *path)
{
    static char buffer[CAT_BUFFER_BYTES];
    int64_t descriptor;

    descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        CatComplain(path);

        return false;
    }

    for (;;)
    {
        const int64_t read = OxysRead((int)descriptor, buffer, sizeof buffer);

        if (read < 0)
        {
            CatComplain(path);
            (void)OxysClose((int)descriptor);

            return false;
        }

        if (read == 0)
        {
            break;
        }

        if (fwrite(buffer, 1U, (size_t)read, stdout) != (size_t)read)
        {
            /*
             * The output failed rather than the input, so the path named in the
             * diagnostic would be the wrong subject. The errno is whatever the
             * stream's last transfer set.
             */
            (void)fprintf(stderr, "cat: the standard output could not be written: %s\n",
                          strerror(errno));
            (void)OxysClose((int)descriptor);

            return false;
        }
    }

    /*
     * The close is checked. A close that failed upon a descriptor this program
     * opened means this program's own table and the kernel's disagree about what
     * it holds, which is worth a status even though every byte arrived.
     */
    if (OxysClose((int)descriptor) < 0)
    {
        CatComplain(path);

        return false;
    }

    return true;
}

/*
 * Copies the standard input, since sub-task 8.5, for no operand and for the
 * operand `-`.
 *
 * The standard input is a file the shell redirected, which ends, or the
 * terminal, which does not: the terminal is raw and delivers keystrokes with
 * no end until 8.7's job control gives a person a way to interrupt. So a
 * control-D — the byte a terminal sends for the key every shell treats as
 * the end of input — ends the copy where it stands in what a read delivered,
 * the bytes before it copied. That is what a canonical line discipline would
 * do for every program at once; this kernel has none, and `cat` is the one
 * program a person will run against the terminal by mistake.
 */
static bool CatStandardInput(void)
{
    static char buffer[CAT_BUFFER_BYTES];

    for (;;)
    {
        int64_t read = OxysRead(SYSCALL_DESCRIPTOR_INPUT, buffer, sizeof buffer);
        bool ended = false;

        if (read < 0)
        {
            CatComplain("-");

            return false;
        }

        if (read == 0)
        {
            break;
        }

        /* The control-D may arrive with bytes before it in one read; those
         * are copied and the copy ends there. */
        for (int64_t index = 0; index < read; ++index)
        {
            if (buffer[index] == '\x04')
            {
                read = index;
                ended = true;
                break;
            }
        }

        if ((read > 0) && (fwrite(buffer, 1U, (size_t)read, stdout) != (size_t)read))
        {
            (void)fprintf(stderr, "cat: the standard output could not be written: %s\n",
                          strerror(errno));

            return false;
        }

        if (ended)
        {
            break;
        }
    }

    return true;
}

int main(int argc, char *argv[])
{
    bool succeeded = true;

    if (argc < 2)
    {
        return (CatStandardInput() && (fflush(stdout) == 0)) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /*
     * Every operand is attempted even after one has failed, which POSIX requires
     * of `rm` explicitly and is the behaviour every utility of this set adopts:
     * a run over ten files that stopped at the second would leave eight
     * unreported, and the person would have to run it again to find out about
     * them one at a time.
     */
    for (int index = 1; index < argc; ++index)
    {
        const bool copied = (strcmp(argv[index], "-") == 0) ? CatStandardInput()
                                                             : CatFile(argv[index]);

        if (!copied)
        {
            succeeded = false;
        }
    }

    if (fflush(stdout) != 0)
    {
        (void)fprintf(stderr, "cat: the standard output could not be flushed: %s\n",
                      strerror(errno));
        succeeded = false;
    }

    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
