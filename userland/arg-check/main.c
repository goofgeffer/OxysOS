/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/arg-check/main.c
 * Purpose: Asserts the half of sub-task 7.6 that no utility can assert of
 *          itself — that the argument vector a program finds upon its stack is
 *          the one it was given — and reports the number of assertions that
 *          failed as its exit status.
 * Key functions: main, ArgumentRequire.
 * References:
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1,
 *     "Initial Stack and Register State": the argument count stands at the stack
 *     pointer, the argument pointers above it, a null pointer ends them, the
 *     environment pointers follow, a null pointer ends those, and the auxiliary
 *     vector ends with a null entry.
 *   - ISO/IEC 9899:2011, Section 5.1.2.2.1, paragraph 2: `argv[argc]` shall be a
 *     null pointer, and the strings shall be modifiable by the program.
 *   - docs/design/LIBC.md: what this asserts and what the kernel
 *     asserts of it afterwards.
 *
 * Why a program of its own, when five utilities already read their arguments.
 *
 *   Because what a utility does with its arguments is *printed*, and nothing in
 *   this system can read what a program printed. `ls` given a directory prints
 *   names, and a kernel watching it cannot tell the names it printed from the
 *   names it should have printed. This program compares instead, and ends with
 *   the number of comparisons that failed — which the kernel reads directly, as
 *   it reads the status of the program of sub-task 7.5.
 *
 * Why the expected vector is written into this program rather than passed to it.
 *
 *   Because it is the thing under test. A program told what to expect by the
 *   same mechanism that is being asserted would agree with itself however the
 *   mechanism failed.
 *
 * What it is run twice for.
 *
 *   Once by the kernel, which builds the stack directly, and once by
 *   `userland/exec-check`, which reaches it through `execve` with a vector of
 *   its own. The two paths through the kernel are different — one lays out a
 *   stack for a process it is creating, the other copies strings out of a
 *   caller's address space before destroying it — and only the second can fail
 *   by reading memory that has already been released.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vector this program is to have been given. Every caller of it — the
 * kernel's self-test and userland/exec-check — passes exactly this, and the two
 * are the only callers there are. */
static const char *const ArgumentExpected[] = { "arg-check", "alpha", "beta gamma", "" };

#define ARGUMENT_EXPECTED_COUNT \
    ((int)(sizeof ArgumentExpected / sizeof ArgumentExpected[0]))

/* How many assertions have failed. It is this program's exit status. */
static int ArgumentFailures;

static void ArgumentRequire(int condition, const char *statement)
{
    if (!condition)
    {
        ++ArgumentFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

int main(int argc, char *argv[], char *envp[])
{
    (void)printf("arg-check: the vector a program finds upon its stack.\n");

    ArgumentRequire(argc == ARGUMENT_EXPECTED_COUNT,
                    "the argument count is not the number of strings that were passed");

    /*
     * The count is checked before the strings are, and the loop is bounded by
     * whichever is smaller. A program that trusted a count it had just found
     * wrong would read past the end of the vector looking for the strings the
     * count promised.
     */
    for (int index = 0; (index < argc) && (index < ARGUMENT_EXPECTED_COUNT); ++index)
    {
        ArgumentRequire(argv[index] != NULL,
                        "a pointer within the argument vector is null");

        if (argv[index] == NULL)
        {
            continue;
        }

        ArgumentRequire(strcmp(argv[index], ArgumentExpected[index]) == 0,
                        "an argument is not the string that was passed");
    }

    /*
     * The empty string is the one argument worth naming separately. It is a
     * string of length zero and a perfectly ordinary operand, and a kernel that
     * laid the vector out by writing each string and skipping the ones that were
     * empty would produce a vector one short — with every pointer after it
     * naming the wrong string, and nothing faulting.
     */
    if (argc == ARGUMENT_EXPECTED_COUNT)
    {
        ArgumentRequire(argv[argc - 1][0] == '\0',
                        "the empty argument did not survive the vector");
    }

    /* ISO/IEC 9899:2011, Section 5.1.2.2.1: argv[argc] is a null pointer. It is
     * what every loop over a vector without a count stops at, and a kernel that
     * omitted the terminator would produce a program that walked into the
     * environment vector and then into the auxiliary one. */
    ArgumentRequire(argv[argc] == NULL, "the argument vector is not terminated");

    /*
     * The environment is empty and terminated. Nothing in this system sets one,
     * and `envp[0]` being null is what says so — as against `envp` itself being
     * null, which would be a vector that does not exist and which Section 3.4.1
     * does not permit.
     */
    ArgumentRequire(envp != NULL, "there is no environment vector at all");
    ArgumentRequire((envp == NULL) || (envp[0] == NULL),
                    "the environment vector is not empty");

    /*
     * The strings are modifiable, which Section 5.1.2.2.1 promises a program and
     * which is true here because they stand upon this program's own stack. The
     * byte is put back, so that this assertion does not change what a later one
     * compares.
     */
    if (argc > 1)
    {
        const char first = argv[1][0];

        argv[1][0] = 'X';
        ArgumentRequire(argv[1][0] == 'X', "the argument strings are not modifiable");
        argv[1][0] = first;
    }

    (void)printf("arg-check: %d assertion(s) failed.\n", ArgumentFailures);

    return ArgumentFailures;
}
