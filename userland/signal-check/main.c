/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/signal-check/main.c
 * Purpose: Asserts the signals, the process groups and the `waitpid` of
 *          sub-task 8.7 from the only side that can reach them — a program at
 *          privilege level 3, and the children it makes — and ends with the
 *          number of assertions that failed.
 * Key functions: main, SignalRequire, SignalHandlers, SignalChildren,
 *          SignalGroups, SignalStops.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.14: `signal` and `raise`.
 *   - IEEE Std 1003.1-2017: `kill()`, `waitpid()` with WNOHANG and WUNTRACED,
 *     `setpgid()`, `getpgid()`; Section 2.4, the default actions, and that
 *     SIGKILL and SIGSTOP cannot be caught.
 *   - kernel/abi/oxys/syscall_abi.h: the status encoding the assertions read.
 *   - kernel/test/proc/signal.c: the self-test that runs this.
 *   - docs/design/PROCESS.md, Section 18.5.
 *
 * Why the children compute rather than sleep where they can.
 *
 *   A signal reaches a program in one of two ways: on the way out of a system
 *   call it was asleep in, or on the way out of the timer's interrupt where it
 *   never entered the kernel at all. A child that spins upon a flag is reached
 *   the second way and asserts the interrupt path; a child asleep upon a pipe
 *   is reached the first and asserts the wake and the EINTR. Both are here,
 *   because a defect in either would look, from a shell, like a program that
 *   sometimes ignores control-C.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static int SignalFailures;

static void SignalRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        ++SignalFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

/* What the handlers below record. */
static volatile sig_atomic_t SignalSeen;
static volatile sig_atomic_t SignalSeenCount;

static void SignalRecord(int number)
{
    SignalSeen = number;
    ++SignalSeenCount;
}

/* Reads a status, as a child of this program is asked about. */
static bool SignalStatusIs(int64_t status, uint64_t kind, unsigned number)
{
    return (SYSCALL_STATUS_KIND(status) == kind) && (SYSCALL_STATUS_NUMBER(status) == number);
}

/* Collects one child and reports its status; -1 where it could not be. */
static int64_t SignalCollect(int64_t child, uint64_t options, int64_t *status)
{
    *status = -1;

    return OxysWaitFor(child, status, options);
}

/* ---------------------------------------------------------------------------
 * Handlers, in this process.
 * ------------------------------------------------------------------------- */

static void SignalHandlers(void)
{
    volatile uint64_t kept = 0x1234567890ABCDEFULL;
    int64_t status;

    SignalRequire(signal(SIGUSR1, SignalRecord) == SIG_DFL, "SIGUSR1 was not at its default");
    SignalSeen = 0;
    SignalSeenCount = 0;
    SignalRequire(raise(SIGUSR1) == 0, "raise failed");
    SignalRequire(SignalSeen == SIGUSR1, "the handler was not entered with the signal's number");
    SignalRequire(SignalSeenCount == 1, "the handler ran other than once");
    SignalRequire(kept == 0x1234567890ABCDEFULL, "a value did not survive the handler");

    /* Kept across its own invocation, as the standard has it. */
    SignalRequire(raise(SIGUSR1) == 0, "a second raise failed");
    SignalRequire(SignalSeenCount == 2, "the handler was reset after it ran");
    SignalRequire(signal(SIGUSR1, SIG_DFL) == SignalRecord,
                  "signal did not report the handler that stood before");

    /* Ignored is nothing, and a program is not ended by what it ignores. */
    SignalRequire(signal(SIGINT, SIG_IGN) == SIG_DFL, "SIGINT was not at its default");
    SignalRequire(raise(SIGINT) == 0, "raising an ignored signal failed");
    SignalRequire(signal(SIGINT, SIG_DFL) == SIG_IGN, "an ignored signal was not reported so");

    /* The two the standard withholds. */
    SignalRequire((signal(SIGKILL, SignalRecord) == SIG_ERR) && (errno == EINVAL),
                  "SIGKILL could be caught");
    SignalRequire((signal(SIGSTOP, SIG_IGN) == SIG_ERR) && (errno == EINVAL),
                  "SIGSTOP could be ignored");
    SignalRequire((signal(0, SIG_IGN) == SIG_ERR) && (errno == EINVAL),
                  "signal 0 could be set");

    /* Existence, by a signal of 0. */
    SignalRequire(kill(OxysGetProcessId(), 0) == 0, "kill 0 to this process failed");
    SignalRequire((kill(60000, 0) < 0) && (errno == ESRCH),
                  "kill 0 to a process that is not there was not ESRCH");

    /* No child yet, and waitpid says so by name. */
    SignalRequire((SignalCollect(-1, 0U, &status) < 0) && (errno == ECHILD),
                  "waitpid with no child was not ECHILD");
}

/* ---------------------------------------------------------------------------
 * Children: ended by a signal, on either path.
 * ------------------------------------------------------------------------- */

static void SignalChildren(void)
{
    int64_t child;
    int64_t status;
    int ends[2];

    /* Computing at privilege level 3, never entering the kernel: the timer's
     * interrupt is where the signal reaches it. */
    child = OxysFork();

    if (child == 0)
    {
        for (;;)
        {
        }
    }

    SignalRequire(child > 0, "fork failed for the computing child");
    SignalRequire(SignalCollect(child, SYSCALL_WAIT_NO_HANG, &status) == 0,
                  "waitpid with NO_HANG did not report nothing for a running child");
    SignalRequire(kill(child, SIGTERM) == 0, "SIGTERM could not be sent");
    SignalRequire(SignalCollect(child, 0U, &status) == child, "the computing child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_SIGNALLED, SIGTERM),
                  "the computing child was not reported ended by SIGTERM");

    /* Asleep upon a pipe: the sleep is woken, the read reports EINTR, and the
     * signal is delivered on the way out. */
    SignalRequire(OxysPipe(ends) == 0, "a pipe could not be made");
    child = OxysFork();

    if (child == 0)
    {
        char byte;

        (void)OxysRead(ends[0], &byte, 1U);
        OxysExit(99);
    }

    SignalRequire(child > 0, "fork failed for the sleeping child");
    SignalRequire(kill(child, SIGINT) == 0, "SIGINT could not be sent");
    SignalRequire(SignalCollect(child, 0U, &status) == child, "the sleeping child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_SIGNALLED, SIGINT),
                  "the sleeping child was not reported ended by SIGINT");

    /* A handler in a computing child: the flag it sets is what ends the loop,
     * and the status is the code it chose. The child says it is ready through
     * the pipe before the parent sends, because a SIGTERM that arrived before
     * the handler was installed would end the child by the default. */
    child = OxysFork();

    if (child == 0)
    {
        SignalSeen = 0;
        (void)signal(SIGTERM, SignalRecord);
        (void)OxysWrite(ends[1], "r", 1U);

        while (SignalSeen != SIGTERM)
        {
        }

        OxysExit(3);
    }

    SignalRequire(child > 0, "fork failed for the catching child");

    {
        char ready = 0;

        SignalRequire((OxysRead(ends[0], &ready, 1U) == 1) && (ready == 'r'),
                      "the catching child did not say it was ready");
    }

    SignalRequire(kill(child, SIGTERM) == 0, "SIGTERM could not be sent to the catching child");
    SignalRequire(SignalCollect(child, 0U, &status) == child, "the catching child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_EXITED, 3U),
                  "the catching child did not end with the code its handler let it choose");

    /* SIGPIPE: a write with no reader ends the writer, by default. The read
     * end is closed before the fork, so that the child cannot write before
     * the parent has closed its own. */
    (void)OxysClose(ends[0]);
    child = OxysFork();

    if (child == 0)
    {
        (void)OxysWrite(ends[1], "x", 1U);
        OxysExit(0);
    }

    SignalRequire(child > 0, "fork failed for the SIGPIPE child");
    SignalRequire(SignalCollect(child, 0U, &status) == child, "the SIGPIPE child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_SIGNALLED, SIGPIPE),
                  "a write with no reader did not end the writer by SIGPIPE");
    (void)OxysClose(ends[1]);

    /* A fault is a signal. The invalid opcode is raised by the instruction
     * itself, so no address the compiler could judge is involved. */
    child = OxysFork();

    if (child == 0)
    {
        __asm__ __volatile__("ud2");
        OxysExit(0);
    }

    SignalRequire(child > 0, "fork failed for the faulting child");
    SignalRequire(SignalCollect(child, 0U, &status) == child, "the faulting child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_SIGNALLED, SIGILL),
                  "an invalid opcode was not reported as SIGILL");
}

/* ---------------------------------------------------------------------------
 * Groups.
 * ------------------------------------------------------------------------- */

static void SignalGroups(void)
{
    const int64_t self = OxysGetProcessId();
    int64_t first;
    int64_t second;
    int64_t status;

    SignalRequire(OxysGetProcessGroup(0) == OxysGetProcessGroup(self),
                  "getpgid(0) is not getpgid(self)");

    /* Two children in a group of the first's own, ended together. */
    first = OxysFork();

    if (first == 0)
    {
        (void)OxysSetProcessGroup(0, 0);

        for (;;)
        {
        }
    }

    SignalRequire(first > 0, "fork failed for the group's first member");
    (void)OxysSetProcessGroup(first, first);
    SignalRequire(OxysGetProcessGroup(first) == first, "the first member did not lead its group");

    second = OxysFork();

    if (second == 0)
    {
        (void)OxysSetProcessGroup(0, first);

        for (;;)
        {
        }
    }

    SignalRequire(second > 0, "fork failed for the group's second member");
    (void)OxysSetProcessGroup(second, first);
    SignalRequire(OxysGetProcessGroup(second) == first, "the second member did not join the group");
    SignalRequire(OxysGetProcessGroup(0) != first, "this process joined the group by mistake");

    SignalRequire(kill(-first, SIGTERM) == 0, "SIGTERM could not be sent to the group");

    for (int count = 0; count < 2; ++count)
    {
        const int64_t ended = SignalCollect(-first, 0U, &status);

        SignalRequire((ended == first) || (ended == second),
                      "waitpid upon the group did not collect a member");
        SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_SIGNALLED, SIGTERM),
                      "a member of the group was not ended by SIGTERM");
    }

    SignalRequire((kill(-first, 0) < 0) && (errno == ESRCH),
                  "an emptied group still answered to kill");
}

/* ---------------------------------------------------------------------------
 * Stops.
 * ------------------------------------------------------------------------- */

static void SignalStops(void)
{
    int64_t child;
    int64_t status;
    int ends[2];

    SignalRequire(OxysPipe(ends) == 0, "a pipe could not be made for the stopping child");
    child = OxysFork();

    if (child == 0)
    {
        char byte = 0;

        /* A read interrupted by the stop reports EINTR when the child is
         * continued; it is tried again until the byte arrives. */
        while (OxysRead(ends[0], &byte, 1U) < 0)
        {
            if (errno != EINTR)
            {
                OxysExit(98);
            }
        }

        OxysExit((byte == 'g') ? 7 : 8);
    }

    SignalRequire(child > 0, "fork failed for the stopping child");
    SignalRequire(kill(child, SIGSTOP) == 0, "SIGSTOP could not be sent");
    SignalRequire(SignalCollect(child, SYSCALL_WAIT_UNTRACED, &status) == child,
                  "the stopped child was not reported");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_STOPPED, SIGSTOP),
                  "the stopped child was not reported stopped by SIGSTOP");
    SignalRequire(SignalCollect(child, SYSCALL_WAIT_UNTRACED | SYSCALL_WAIT_NO_HANG, &status) == 0,
                  "a stop was reported twice");

    /* A byte written while it is stopped waits for it. */
    SignalRequire(OxysWrite(ends[1], "g", 1U) == 1, "the byte could not be written");
    SignalRequire(SignalCollect(child, SYSCALL_WAIT_NO_HANG, &status) == 0,
                  "a stopped child was reported ended");

    SignalRequire(kill(child, SIGCONT) == 0, "SIGCONT could not be sent");
    SignalRequire(SignalCollect(child, SYSCALL_WAIT_UNTRACED, &status) == child,
                  "the continued child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_EXITED, 7U),
                  "the continued child did not read the byte and end with 7");

    (void)OxysClose(ends[0]);
    (void)OxysClose(ends[1]);

    /* SIGKILL reaches a stopped process. */
    child = OxysFork();

    if (child == 0)
    {
        for (;;)
        {
        }
    }

    SignalRequire(child > 0, "fork failed for the killed child");
    SignalRequire(kill(child, SIGSTOP) == 0, "SIGSTOP could not be sent to the killed child");
    SignalRequire(SignalCollect(child, SYSCALL_WAIT_UNTRACED, &status) == child,
                  "the second stopped child was not reported");
    SignalRequire(kill(child, SIGKILL) == 0, "SIGKILL could not be sent");
    SignalRequire(SignalCollect(child, 0U, &status) == child, "the killed child was not collected");
    SignalRequire(SignalStatusIs(status, SYSCALL_STATUS_KIND_SIGNALLED, SIGKILL),
                  "the stopped child was not ended by SIGKILL");
}

int main(void)
{
    (void)printf("signal-check: signals, groups and waitpid, from a program.\n");

    SignalHandlers();
    SignalChildren();
    SignalGroups();
    SignalStops();

    (void)printf("signal-check: %d assertion(s) failed.\n", SignalFailures);

    return SignalFailures;
}
