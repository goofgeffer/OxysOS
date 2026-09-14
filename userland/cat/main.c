/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/cat/main.c
 * Purpose: Writes the contents of each operand to the standard output in the
 *          order the operands are given — the first program upon this system
 *          that reads a file.
 * Key functions: main, CatFile, CatComplain.
 * References:
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), `cat`: "The standard output shall
 *     contain the sequence of bytes read from the input files. Nothing else
 *     shall be written to the standard output." The exit status is 0 where all
 *     input files were output successfully and greater than zero otherwise.
 *   - IEEE Std 1003.1-2017, `cat`, OPERANDS: an operand of `-` reads the
 *     standard input. This implementation does not, for the reason below.
 *   - kernel/abi/oxys/syscall_abi.h: the calls beneath `OxysOpen`, `OxysRead`
 *     and `OxysClose`.
 *   - docs/design/LIBC.md, Section 12.3: the five utilities and what each is
 *     for.
 *
 * What this does not do, and why each is a property of the system.
 *
 *   **No operand means a failure and not the standard input.** POSIX has `cat`
 *   with no operand copy standard input to standard output. This kernel has no
 *   call that reads a stream — `stdin` is permanently at its end, as
 *   libc/include/stdio.h records — so such a run would copy nothing and exit
 *   successfully, which is a program that appears to have worked and did not.
 *   It reports the absence instead.
 *
 *   **An operand of `-` is a path and not the standard input**, for the same
 *   reason. It will name no file, and the diagnostic will say so.
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

    descriptor = OxysOpen(path, SYSCALL_OPEN_READ);

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

int main(int argc, char *argv[])
{
    bool succeeded = true;

    if (argc < 2)
    {
        (void)fprintf(stderr,
                      "cat: no file was named, and this system has no standard input "
                      "to read instead\n");

        return EXIT_FAILURE;
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
        if (!CatFile(argv[index]))
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
