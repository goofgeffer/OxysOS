/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdlib/exit.c
 * Purpose: The termination functions of ISO/IEC 9899:2011, Section 7.22.4 — the
 *          register of functions to call at normal termination, the order they
 *          are called in, the flush that follows them, and the two ways of
 *          ending without either.
 * Key functions: atexit, exit, _Exit, abort.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.22.4.2: atexit, and the requirement that at
 *     least 32 registrations succeed.
 *   - ISO/IEC 9899:2011, Section 7.22.4.4: exit — the registered functions are
 *     called in the reverse order of their registration, then every stream with
 *     unwritten buffered data is flushed, then control is returned to the host.
 *   - ISO/IEC 9899:2011, Section 7.22.4.5: _Exit, which calls none of them and
 *     for which the flushing of streams is implementation-defined.
 *   - ISO/IEC 9899:2011, Section 7.22.4.1: abort.
 *   - libc/include/syscall.h: OxysExit, which is what ends a program here.
 *   - docs/design/LIBC.md: the design of this sub-task's runtime,
 *     and why the order of the two things exit does is not free.
 *
 * Why this is a translation unit of its own and not part of heap.c.
 *
 * It is where the C library meets the end of a program, and the heap is where it
 * meets the middle of one. Keeping them apart means a program that never
 * terminates by `exit` — which is every program composed by hand in this
 * project's self-tests — links none of this, and a program that never allocates
 * links none of the heap. A library whose termination and whose allocator are
 * one object is a library in which either drags the other in.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

/*
 * The register, and its size.
 *
 * ISO/IEC 9899:2011, Section 7.22.4.2, paragraph 3, requires an implementation
 * to support at least 32 registrations. This supports exactly 32, in an array in
 * `.bss`: a program is not obliged to have a heap, and a library that allocated
 * here would make `atexit` fail for want of memory in exactly the circumstance a
 * program most wants it to work — just after an allocation failed.
 */
#define EXIT_HANDLERS_MAXIMUM 32U

static void (*ExitHandlers[EXIT_HANDLERS_MAXIMUM])(void);
static size_t ExitHandlerCount;

/*
 * Whether `exit` has already been entered.
 *
 * Section 7.22.4.4, paragraph 2, makes a second call to `exit` undefined
 * behaviour — which arises when a function registered here calls it. This
 * implementation defines it: the second call proceeds directly to `_Exit`,
 * calling nothing further. The alternative is a registered function that calls
 * `exit`, is called again, and calls `exit` again, which is a loop with no way
 * out and nothing to report it; and the alternative to *that* is to ignore the
 * second call and return, which cannot be done, `exit` not being a function that
 * may return.
 */
static bool ExitInProgress;

int atexit(void (*function)(void))
{
    if (function == NULL)
    {
        return 1;
    }

    if (ExitHandlerCount >= EXIT_HANDLERS_MAXIMUM)
    {
        return 1;
    }

    ExitHandlers[ExitHandlerCount] = function;
    ++ExitHandlerCount;

    return 0;
}

void exit(int status)
{
    if (ExitInProgress)
    {
        _Exit(status);
    }

    ExitInProgress = true;

    /*
     * The registered functions, in the reverse of the order they were
     * registered, which Section 7.22.4.4, paragraph 2, requires.
     *
     * It is the only order that lets a later registration depend upon an earlier
     * one: the thing registered second is torn down first, exactly as a stack
     * unwinds. The count is re-read each time round rather than copied, because
     * a registered function may call `atexit` — which the standard permits, and
     * which paragraph 2 says registers a function that will not be called, the
     * registration having come after this one was reached. Reading the count
     * afresh and stepping down from it produces exactly that.
     */
    while (ExitHandlerCount > 0U)
    {
        void (*const handler)(void) = ExitHandlers[ExitHandlerCount - 1U];

        --ExitHandlerCount;

        handler();
    }

    /*
     * Then the streams, and **the order of these two is not free**.
     *
     * A registered function that writes a diagnostic writes it into a buffer,
     * and a library that flushed before calling them would lose every one of
     * those diagnostics — silently, at the moment a program is ending, which is
     * when a diagnostic is most likely to be the only record of what happened.
     * Section 7.22.4.4, paragraph 2, states the order for this reason and it is
     * obeyed here for this reason.
     */
    (void)fflush(NULL);

    _Exit(status);
}

void _Exit(int status)
{
    /*
     * Nothing is flushed, which Section 7.22.4.5, paragraph 2, leaves to the
     * implementation. That is the whole point of the function: it is what `exit`
     * ends with, and it is what a program calls when it has reason to believe
     * the library's own state is no longer to be trusted — in which case
     * walking a list of stream buffers is the last thing it should do.
     *
     * The status is widened to the kernel's signed sixty-four-bit result. It is
     * not masked to eight bits: this kernel hands the parent whatever the child
     * passed, so a status of 256 arrives as 256 rather than as zero, which is
     * what the traditional masking would make of it.
     */
    OxysExit((int64_t)status);
}

void abort(void)
{
    /*
     * ISO C makes this raise SIGABRT. This system has no signals, so the program
     * ends with EXIT_FAILURE — and ends through `_Exit`, so nothing registered
     * by `atexit` is called and nothing is flushed, which Section 7.22.4.1,
     * paragraph 2, requires of the first and permits of the second.
     *
     * **A program that aborts loses its buffered output**, and that is the right
     * trade rather than an oversight: `abort` is what a program calls when it has
     * concluded its own state is wrong, and a flush walks data structures that
     * conclusion covers.
     */
    _Exit(EXIT_FAILURE);
}
