/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/signal/signal.c
 * Purpose: `signal`, `raise` and `kill`, of sub-task 8.7: the three functions
 *          of <signal.h>, each a wrapper's call with the standard's convention
 *          for the result.
 * Key functions: signal, raise, kill.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.14.1.1: `signal` returns the previous
 *     handler, or SIG_ERR; Section 7.14.2.1: `raise` returns zero on success.
 *   - IEEE Std 1003.1-2017, `signal()`: the handler is installed with the
 *     restorer the kernel returns through, which is the C library's own
 *     routine, so that a program never names it.
 *   - libc/include/syscall.h: OxysSignalAction, OxysKill, OxysGetProcessId.
 *
 * Why the restorer is named here and nowhere else.
 *
 *   The kernel enters a handler with a return address it was given when the
 *   handler was installed, and that address must be a routine ending in the
 *   `sigreturn` call. Every `signal` therefore passes the same routine, and a
 *   program that installed a handler through the raw wrapper with the wrong
 *   restorer — or none — would return from its handler into whatever stood
 *   there. The standard's `signal` is the form a program should use, and it
 *   is the one place the restorer is written.
 */

#include <signal.h>

#include <errno.h>
#include <syscall.h>

sighandler_t signal(int number, sighandler_t handler)
{
    const int64_t previous =
        OxysSignalAction(number, (uint64_t)(uintptr_t)handler,
                         (uint64_t)(uintptr_t)OxysSignalRestorer);

    if (previous < 0)
    {
        return SIG_ERR;
    }

    return (sighandler_t)(uintptr_t)previous;
}

int raise(int number)
{
    const int64_t self = OxysGetProcessId();

    if (self < 0)
    {
        return -1;
    }

    return (OxysKill(self, number) < 0) ? -1 : 0;
}

int kill(int64_t pid, int number)
{
    return (OxysKill(pid, number) < 0) ? -1 : 0;
}
