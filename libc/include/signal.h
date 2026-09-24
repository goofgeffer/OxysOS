/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/signal.h
 * Purpose: The signals a program may catch, ignore, send and receive, of
 *          sub-task 8.7: the numbers, the two dispositions that are not a
 *          handler, and the three functions ISO C and IEEE Std 1003.1-2017
 *          name for them.
 * Key definitions: sig_atomic_t, sighandler_t, SIG_DFL, SIG_IGN, SIG_ERR,
 *          SIGINT, SIGTERM and the rest, signal, raise, kill.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.14: `signal` and `raise`, SIG_DFL, SIG_IGN
 *     and SIG_ERR, and the six signals the language requires — SIGABRT,
 *     SIGFPE, SIGILL, SIGINT, SIGSEGV and SIGTERM — which are here with the
 *     rest of the standard's.
 *   - IEEE Std 1003.1-2017, `signal()`: the handler is not reset when it is
 *     entered, which is the reliable semantic this library provides and the
 *     one a program written for the standard expects; `kill()`.
 *   - kernel/abi/oxys/syscall_abi.h: the numbers, which are the kernel's and
 *     are asserted below to be what this header repeats.
 *   - docs/design/LIBC.md.
 *
 * What a handler may do.
 *
 *   It is entered upon the program's own stack, below the interrupted
 *   function's red zone, with the signal number as its argument, and returns
 *   into the kernel's restorer, which puts the interrupted context back. Every
 *   register the interrupted code held is restored, so a handler may call
 *   anything; but the interrupted code may have been in the middle of the
 *   heap or of a stream, and a handler that calls `malloc` or `printf` upon a
 *   structure the interruption left half-updated corrupts it. The standard's
 *   rule is the safe one: a handler sets a `volatile sig_atomic_t` and
 *   returns, and the program looks at it.
 */

#ifndef OXYS_LIBC_SIGNAL_H
#define OXYS_LIBC_SIGNAL_H

#include <oxys/syscall_abi.h>

/* An object a handler may write and the program may read without tearing. */
typedef int sig_atomic_t;

typedef void (*sighandler_t)(int signal);

/* The dispositions that are not a handler, as the kernel numbers them; and
 * the value `signal` returns where it failed, which is neither. */
#define SIG_DFL ((sighandler_t)SYSCALL_SIGNAL_DEFAULT)
#define SIG_IGN ((sighandler_t)SYSCALL_SIGNAL_IGNORE)
#define SIG_ERR ((sighandler_t)-1)

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define NSIG    32

_Static_assert(SIGINT == (int)SYSCALL_SIGINT, "SIGINT does not name SYSCALL_SIGINT.");
_Static_assert(SIGKILL == (int)SYSCALL_SIGKILL, "SIGKILL does not name SYSCALL_SIGKILL.");
_Static_assert(SIGPIPE == (int)SYSCALL_SIGPIPE, "SIGPIPE does not name SYSCALL_SIGPIPE.");
_Static_assert(SIGTERM == (int)SYSCALL_SIGTERM, "SIGTERM does not name SYSCALL_SIGTERM.");
_Static_assert(SIGCHLD == (int)SYSCALL_SIGCHLD, "SIGCHLD does not name SYSCALL_SIGCHLD.");
_Static_assert(SIGCONT == (int)SYSCALL_SIGCONT, "SIGCONT does not name SYSCALL_SIGCONT.");
_Static_assert(SIGTSTP == (int)SYSCALL_SIGTSTP, "SIGTSTP does not name SYSCALL_SIGTSTP.");
_Static_assert(SIGTTIN == (int)SYSCALL_SIGTTIN, "SIGTTIN does not name SYSCALL_SIGTTIN.");
_Static_assert(NSIG == (int)SYSCALL_SIGNAL_MAXIMUM + 1, "NSIG does not bound the kernel's signals.");

/*
 * Sets the disposition of `signal` and returns the previous one, or SIG_ERR
 * with errno set: EINVAL for a number the kernel has not, and for SIGKILL and
 * SIGSTOP, which may be neither caught nor ignored. The handler installed is
 * kept across its own invocation, as IEEE Std 1003.1-2017 has it.
 */
sighandler_t signal(int signal, sighandler_t handler);

/* Sends `signal` to the calling process. Returns 0, or non-zero with errno. */
int raise(int signal);

/* Sends `signal` to the process `pid`, or to the group -`pid`; 0 tests for
 * existence. Returns 0, or -1 with errno set to ESRCH or EINVAL. */
int kill(int64_t pid, int signal);

#endif /* OXYS_LIBC_SIGNAL_H */
