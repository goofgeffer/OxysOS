/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/startup-check/main.c
 * Purpose: The first program in this project built from source rather than
 *          composed byte by byte, and the thing that asserts sub-task 7.5 — the
 *          startup object, the link procedure, and every part of the C library
 *          that had never run at privilege level 3.
 * Key functions: main, StartupRequire, StartupReport, StartupFarewell.
 * References:
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1:
 *     the initial process stack, whose contents the first group of assertions
 *     below is about.
 *   - ISO/IEC 9899:2011, Section 5.1.2.2.1: the two forms of main, of which this
 *     uses the three-parameter form the ABI names.
 *   - ISO/IEC 9899:2011, Section 7.22.4.4: exit, and the order in which it calls
 *     what atexit registered and flushes what the streams hold.
 *   - docs/design/LIBC.md: what this asserts and what the kernel
 *     checks of it afterwards.
 *
 * How this reports, and why it reports twice.
 *
 *   It **prints**, through the library's own printf, so that a person reading
 *   the serial log sees what happened and so that every failure carries the word
 *   `FAILED` — which is what the `verify` target of the Makefile greps for. That
 *   is the half a person reads.
 *
 *   It also **ends with a status** that is the number of assertions that failed,
 *   and the kernel checks that status against zero independently of anything
 *   printed. That is the half a machine reads, and it exists because the first
 *   half depends upon the very thing under test: a program whose printf did not
 *   work would print nothing, and a test whose only evidence is output would call
 *   that a pass.
 *
 * What it must not do.
 *
 *   It must not read the standard input, which since sub-task 8.1 is the terminal
 *   and would wait for a person; it must not open a file, there being no call
 *   that opened one when this was written; and it must not allocate more than
 *   PROCESS_BREAK_MAXIMUM, which is sixteen mebibytes. Each of those is a
 *   property of the system and not of this program.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stream.h>
#include <string.h>
#include <syscall.h>

/* How many assertions have failed. It is the program's exit status, so a run
 * that printed nothing at all is still distinguishable from a run that passed. */
static int StartupFailures;

static void StartupRequire(int condition, const char *statement)
{
    if (!condition)
    {
        ++StartupFailures;
        printf("  %s FAILED.\n", statement);
    }
}

/*
 * What the program leaves in its buffer before main returns, and how much of
 * its output had reached the kernel at that moment.
 *
 * The pair is what makes the *order* of the two things `exit` does assertable
 * from inside the program. ISO/IEC 9899:2011, Section 7.22.4.4, paragraph 2,
 * requires exit to call what atexit registered and *then* flush the streams; a
 * library that flushed first would deliver the partial line below before the
 * registrations ran, and nothing about the finished output would say so.
 */
static size_t StartupDelivered;
static int StartupOrder;

/*
 * Registered second and therefore called first, and it deliberately writes
 * nothing.
 *
 * A handler that printed would flush the standard output — it is line buffered,
 * so a completed line goes out as it is written — and the flush would deliver
 * the partial line the other handler is about to look for. The silence is the
 * assertion's, not modesty.
 */
static void StartupFarewellFirst(void)
{
    StartupOrder = 1;
}

/*
 * Registered first and therefore called second, and the only thing in this
 * program that runs after main returns.
 *
 * It makes two assertions that no code before main returns could make, and where
 * either fails it ends the program itself with a non-zero status — because the
 * status main returned has already been decided and handed to exit, and the
 * kernel reads that status. `_Exit` from within a registered function is what
 * Section 7.22.4.4 leaves undefined and what libc/stdlib/exit.c defines: it ends
 * the program at once, calling nothing further.
 */
static void StartupFarewell(void)
{
    int failed = 0;

    /* Reverse order, which paragraph 2 requires and which no single
     * registration can demonstrate. */
    if (StartupOrder != 1)
    {
        printf("  atexit called its registrations in the order they were made "
               "FAILED.\n");
        ++failed;
    }

    /*
     * And the partial line main left in the buffer is still in it: exit has not
     * flushed yet, because exit flushes after this returns. A library that
     * flushed first would have delivered it, and this is the only moment at
     * which the difference can be seen.
     */
    if (OxysStreamDelivered(stdout) != StartupDelivered)
    {
        printf("  exit flushed the streams before calling what atexit registered "
               "FAILED.\n");
        ++failed;
    }

    printf("\n  The two lines above were written after main returned, by a "
           "function\n  atexit registered — and the partial line before them was "
           "flushed by the\n  exit that called it.\n");

    if (failed != 0)
    {
        _Exit(StartupFailures + failed);
    }
}

int main(int argc, char *argv[], char *envp[]);

int main(int argc, char *argv[], char *envp[])
{
    printf("Startup: a program built from source is running at privilege "
           "level 3.\n");

    /* ------------------------------------------------- the initial stack */

    /*
     * What the System V ABI, Section 3.4.1, put upon the stack, as the startup
     * object handed it on.
     *
     * This kernel's execve refuses both vectors, so the count is zero and both
     * vectors hold nothing but their terminator. **The terminators are the
     * assertion**, not the count: a kernel that left the stack pointer at the
     * top of the stack would have this program read its argument count from
     * unmapped memory and fault before printing anything, and a kernel that
     * built the frame but forgot a terminator would give a program walking argv
     * a pointer out of whatever the stack happened to hold.
     */
    StartupRequire(argc == 0, "the argument count was not zero");
    StartupRequire(argv != NULL, "the argument vector was a null pointer");
    StartupRequire((argv != NULL) && (argv[0] == NULL),
                   "the argument vector was not terminated");
    StartupRequire(envp != NULL, "the environment vector was a null pointer");
    StartupRequire((envp != NULL) && (envp[0] == NULL),
                   "the environment vector was not terminated");

    /*
     * And that the two vectors are where the ABI says they are relative to one
     * another: envp is argc+1 eightbytes above argv. With argc of zero that is
     * one eightbyte, and it is asserted because the startup object computes it
     * with an address expression that would be plausible and wrong if the
     * displacement were left out.
     */
    StartupRequire(envp == (argv + argc + 1),
                   "the environment vector does not follow the argument vector");

    /* ------------------------------------------------- the string functions */

    /*
     * Sub-task 7.1's limitation 4: nothing in this library had ever run at
     * privilege level 3, and the compilation flags differ. These are the same
     * functions the kernel asserts at boot, run here for the first time as a
     * program — which is a second verification and not a formality.
     */
    {
        char buffer[32];

        StartupRequire(strlen("Oxys") == 4U, "strlen did not run at privilege level 3");
        StartupRequire(strcmp(strcpy(buffer, "Oxys-OS"), "Oxys-OS") == 0,
                       "strcpy or strcmp did not run at privilege level 3");
        StartupRequire(memcmp(memset(buffer, 'x', 4U), "xxxx", 4U) == 0,
                       "memset or memcmp did not run at privilege level 3");
        StartupRequire(strstr("Oxys-OS", "s-O") != NULL,
                       "strstr did not run at privilege level 3");
    }

    /* ------------------------------------------------- the wrappers */

    {
        char version[64];
        const int64_t written = OxysVersion(version, sizeof version);

        StartupRequire(written > 0, "the version could not be read");
        StartupRequire((written > 0) && (strlen(version) == (size_t)written),
                       "the version was not terminated where it said it ended");

        printf("  This system reports itself as: %s\n", version);

        /* A wrapper's failure path, from a program. The kernel's self-test
         * asserts the translation of a result into an errno by calling it
         * directly; this asserts that the same translation happens when the
         * result comes through SYSCALL. */
        StartupRequire(OxysWrite(99, "x", 1U) == -1,
                       "a write to a descriptor that does not exist succeeded");
        StartupRequire(errno == EBADF,
                       "a refused write did not leave EBADF in errno");
    }

    /* ------------------------------------------------- the heap */

    /*
     * Sub-task 7.3's two halves, joined for the first time. The kernel asserts
     * the allocator against a region it supplies and `brk` through a program
     * composed by hand; this is the first time the allocator has obtained its
     * memory from the break, which is the one path neither of those covers.
     */
    {
        char *const first = malloc(64U);
        char *const second = malloc(4096U);
        char *third;

        StartupRequire(first != NULL, "malloc could not obtain memory from the break");
        StartupRequire(second != NULL, "a second allocation could not be made");
        StartupRequire(first != second, "two allocations were the same block");

        if ((first != NULL) && (second != NULL))
        {
            (void)memset(first, 0x41, 64U);
            (void)memset(second, 0x42, 4096U);

            /* The two are disjoint, proved by writing rather than by comparing
             * addresses: an allocator that handed out overlapping blocks would
             * pass an address comparison and fail this. */
            StartupRequire(first[0] == 0x41, "one allocation was written over by another");
            StartupRequire(second[4095] == 0x42, "an allocation was shorter than it claimed");
        }

        third = realloc(first, 256U);
        StartupRequire(third != NULL, "realloc could not grow an allocation");
        StartupRequire((third != NULL) && (third[0] == 0x41),
                       "realloc did not preserve the contents it was given");

        free(third);
        free(second);

        {
            /* calloc's product check, reached from a program. There is no
             * allocation large enough to satisfy it, so a library that did not
             * check would attempt a small one and report success. */
            void *const wrapped = calloc((size_t)-1, 2U);

            StartupRequire(wrapped == NULL,
                           "a count and size whose product wraps was met");
        }
    }

    /* ------------------------------------------------- the streams */

    /*
     * Sub-task 7.4's other half, which the kernel could not assert at all: the
     * transfer that carries a buffer to a descriptor.
     *
     * Every line this program has printed is already evidence that it works. The
     * assertions below are about the things a line of output does not show.
     */
    {
        char composed[32];
        const int produced = snprintf(composed, sizeof composed, "%s-%d", "brk", 7);

        StartupRequire(produced == 5, "snprintf did not report what it produced");
        StartupRequire(strcmp(composed, "brk-7") == 0,
                       "snprintf did not compose what it was asked for");

        /* printf returns what it transmitted, and the count is checked because
         * it is the one thing about a successful write that a reader of the log
         * cannot verify. The expected number is the length of the literal, taken
         * at compile time, so the assertion is upon what printf reports and not
         * upon a number somebody counted by hand and will one day mis-edit. */
        static const char line[] = "  The standard output is line buffered, so "
                                   "this arrived with its newline.\n";

        StartupRequire(printf("%s", line) == (int)(sizeof line - 1U),
                       "printf did not report the characters it transmitted");

        StartupRequire(fflush(stdout) == 0, "the standard output could not be flushed");

        /* The standard error is unbuffered, so this reaches the log before the
         * next buffered line does — which is the property that makes it useful
         * in a program that is about to fault, and which a reader of the log can
         * see for themselves in the order of these two lines. */
        (void)fprintf(stderr, "  This line went to the standard error, which is "
                              "unbuffered.\n");

        /*
         * The standard input is not read. Until sub-task 8.1 it was, to assert
         * that it reported an end rather than an error, this kernel having no
         * call that reads; since 8.1 it is the terminal, and a program that
         * read it here would wait for a person to press a key in the middle of
         * `make verify`. That the terminal delivers what was typed is asserted
         * by `line-check`, which is given a session to read.
         */
    }

    /* ------------------------------------------------- termination */

    StartupRequire(atexit(StartupFarewell) == 0, "atexit refused a registration");
    StartupRequire(atexit(StartupFarewellFirst) == 0,
                   "atexit refused a second registration");

    if (StartupFailures == 0)
    {
        printf("  The program's own assertions all passed.\n");
    }
    else
    {
        printf("  The program's own assertions FAILED: %d of them.\n",
               StartupFailures);
    }

    /*
     * A partial line, deliberately without a newline, and the record of how much
     * has reached the kernel before it is written.
     *
     * The standard output is line buffered, so a completed line goes out as it
     * is written and nothing stays behind. This one has no newline, so it sits
     * in the buffer until something flushes it — and the only thing that will is
     * the `exit` the startup object calls after main returns. StartupFarewell
     * checks that it is still there, which is how the order of the two things
     * exit does becomes an assertion rather than a line somebody reads.
     */
    StartupDelivered = OxysStreamDelivered(stdout);
    printf("  This partial line was left in the buffer when main returned.");

    /*
     * The status is the number of failures, and the return is the assertion.
     *
     * Returning from main rather than calling exit is deliberate: the System V
     * ABI, Section 3.4.1, says that when main returns its value is passed to
     * exit, and the startup object is what has to do the passing. A crt0 that
     * dropped the value, or that widened it from the whole of %rax rather than
     * from %eax, would produce a status the parent reads as something else — and
     * the kernel checks this status.
     */
    return StartupFailures;
}
