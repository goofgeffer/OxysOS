/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/exec-check/main.c
 * Purpose: Asserts that an argument vector survives `execve` — that the strings
 *          a program passes are copied out of its address space before that
 *          space is destroyed — by replacing itself with `arg-check` and letting
 *          that program's status stand as its own.
 * Key functions: main.
 * References:
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1:
 *     the initial process stack `execve` must build for the program it loads.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_EXECVE, and the two bounds a vector
 *     is judged against.
 *   - userland/arg-check/main.c: the program this becomes, and the vector it
 *     expects — which is the vector below, and must remain so.
 *   - docs/design/LIBC.md: what this asserts.
 *
 * Why this program has no assertions of its own.
 *
 *   Because upon success it does not exist to make any. `execve` replaces the
 *   program, so everything after the call belongs to `arg-check`, and the status
 *   the kernel collects is `arg-check`'s count of its own failed assertions.
 *   That is the assertion: a vector that did not survive the crossing produces a
 *   non-zero status, and the kernel's self-test reads it.
 *
 *   The code after the call is therefore reached only where `execve` failed, and
 *   a failure there is reported as a status this program chooses — one that
 *   cannot be confused with a count of `arg-check`'s assertions, there being far
 *   fewer of those than this.
 *
 * Why the vector is a writable array of pointers and not a compound literal of
 * string literals.
 *
 *   `execve` takes `char *const []`, and this project compiles with
 *   -Wwrite-strings, under which a string literal is `const char[]`. The strings
 *   are therefore arrays of this program's own, which is what a caller of
 *   `execve` genuinely has in every real case: a shell's argument vector is
 *   words it has parsed out of a line, not literals.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* The status this program ends with where `execve` failed. It is far above any
 * count of assertions `arg-check` could report, so the kernel's self-test can
 * tell "the vector did not survive" from "the program was never reached". */
#define EXEC_CHECK_NOT_REACHED 70

/* The path `arg-check` is written to by the self-test that runs this. The two
 * must agree, and there is nothing that checks they do beyond this comment and
 * the status a mismatch produces — which is EXEC_CHECK_NOT_REACHED with an errno
 * of ENOENT printed beside it. */
static char ExecCheckProgram[] = "/bin/arg-check";

/* The vector, which must be the one userland/arg-check/main.c expects. */
static char ExecCheckName[] = "arg-check";
static char ExecCheckFirst[] = "alpha";
static char ExecCheckSecond[] = "beta gamma";
static char ExecCheckThird[] = "";

int main(void)
{
    char *const arguments[] = { ExecCheckName, ExecCheckFirst, ExecCheckSecond,
                                ExecCheckThird, NULL };

    (void)printf("exec-check: becoming %s with a vector of four.\n", ExecCheckProgram);

    /*
     * The stream is flushed before the call. `execve` replaces this program
     * entire, its buffered output included, so anything not transmitted by now
     * is lost — and the line above would vanish exactly when the call succeeded,
     * which is the case a reader most wants the log for.
     */
    (void)fflush(stdout);

    (void)OxysExecve(ExecCheckProgram, arguments, NULL);

    (void)fprintf(stderr, "exec-check: the program was not reached: %s\n",
                  strerror(errno));

    return EXEC_CHECK_NOT_REACHED;
}
