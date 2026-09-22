/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/poll-check/main.c
 * Purpose: Asserts `poll` of sub-task 9.6 — the call the terminal emulator
 *          waits in, and the only way a program in this system waits upon two
 *          things at once — together with the refusal that keeps a shell
 *          without a terminal from taking the terminal away from one that has
 *          it. Ends with the number of assertions that failed.
 * Key functions: main, PollRequire.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_POLL, SyscallPollEntry,
 *     SYSCALL_POLL_WINDOWS, SYSCALL_POLL_NO_WAIT, SYSCALL_ENOTTY.
 *   - docs/design/TERMINAL.md, Section 6: every assertion here paired with the
 *     silent failure it would catch.
 *
 * Why the blocking wait is asserted with a child and not with a timer.
 *
 *   This system has no clock a program can sleep against, so "it waited" cannot
 *   be asserted by measuring. What can be asserted is that a blocking poll
 *   **returns for the right reason**: a child writes into the pipe and ends,
 *   and the parent's poll — which had nothing to read when it was called —
 *   comes back saying that end is ready. A poll that never slept would return
 *   the same answer, and a poll that slept and was never woken would return
 *   nothing at all and hang the boot, which is a failure this suite reports by
 *   not finishing.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static int PollFailures;

static void PollRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        ++PollFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

int main(void)
{
    SyscallPollEntry entries[2];
    int ends[2];
    char byte = 0;

    (void)printf("poll-check: poll and the terminal's refusal, from privilege level 3.\n");

    /* --- The refusals, asserted before anything is made. --- */

    entries[0].descriptor = 0;
    entries[0].ready = 0U;
    PollRequire(OxysPoll(entries, 0U, 0U) == -1, "a poll of nothing was not refused");
    PollRequire(errno == EINVAL, "a poll of nothing was refused with the wrong reason");

    PollRequire(OxysPoll(entries, (uint64_t)SYSCALL_POLL_MAXIMUM + 1U, 0U) == -1,
                "a poll of more entries than the call carries was not refused");

    entries[0].descriptor = 0;
    PollRequire(OxysPoll(entries, 1U, UINT64_C(0x8000)) == -1,
                "a poll with an option this kernel does not have was not refused");

    entries[0].descriptor = 40;
    PollRequire(OxysPoll(entries, 1U, SYSCALL_POLL_NO_WAIT) == -1,
                "a poll of a descriptor the caller does not hold was not refused");
    PollRequire(errno == EBADF, "a poll of a descriptor not held was refused wrongly");

    /*
     * The window queue, named by a program that owns no window. The terminal
     * emulator names it beside its pipe; a program with no window that asked
     * for it would otherwise wait for an event nothing could ever send.
     */
    entries[0].descriptor = SYSCALL_POLL_WINDOWS;
    PollRequire(OxysPoll(entries, 1U, SYSCALL_POLL_NO_WAIT) == -1,
                "a poll of the window queue by a program with no window was not refused");
    PollRequire(errno == EBADF,
                "a poll of the window queue with no window was refused wrongly");

    /* --- A pipe: empty, written, read, and at its end. --- */

    if (OxysPipe(ends) < 0)
    {
        PollRequire(false, "a pipe could not be made");
        (void)printf("poll-check: %d assertion(s) failed.\n", PollFailures);

        return PollFailures;
    }

    entries[0].descriptor = ends[0];
    entries[0].ready = 1U;
    PollRequire(OxysPoll(entries, 1U, SYSCALL_POLL_NO_WAIT) == 0,
                "an empty pipe was reported ready to read");
    PollRequire(entries[0].ready == 0U,
                "an empty pipe's entry was not written with a zero");

    byte = 'x';
    PollRequire(OxysWrite(ends[1], &byte, 1U) == 1, "a byte could not be written to the pipe");

    entries[0].descriptor = ends[0];
    entries[0].ready = 0U;
    PollRequire(OxysPoll(entries, 1U, SYSCALL_POLL_NO_WAIT) == 1,
                "a pipe holding a byte was not reported ready");
    PollRequire(entries[0].ready == 1U, "a ready pipe's entry was not written with a one");

    /*
     * The end that writes is never ready to read. A poller given it has made a
     * mistake that would otherwise become a read refused much later.
     */
    entries[0].descriptor = ends[1];
    entries[0].ready = 1U;
    PollRequire(OxysPoll(entries, 1U, SYSCALL_POLL_NO_WAIT) == 0,
                "the end of a pipe that writes was reported ready to read");

    byte = 0;
    PollRequire(OxysRead(ends[0], &byte, 1U) == 1, "the byte could not be read back");
    PollRequire(byte == 'x', "the byte read back was not the byte written");

    /* --- The blocking wait, ended by a child. --- */

    {
        const int64_t child = OxysFork();

        if (child == 0)
        {
            const char message = 'y';

            (void)OxysClose(ends[0]);
            (void)OxysWrite(ends[1], &message, 1U);
            (void)OxysClose(ends[1]);
            OxysExit(0);
        }

        PollRequire(child > 0, "the child that writes could not be forked");

        entries[0].descriptor = ends[0];
        entries[0].ready = 0U;
        PollRequire(OxysPoll(entries, 1U, 0U) == 1,
                    "a blocking poll did not report the pipe the child wrote to");
        PollRequire(entries[0].ready == 1U, "the blocking poll wrote no readiness");

        byte = 0;
        PollRequire(OxysRead(ends[0], &byte, 1U) == 1, "the child's byte was not there");
        PollRequire(byte == 'y', "the child's byte was not what the child wrote");

        (void)OxysWait(NULL);
    }

    /*
     * **A pipe at its end is ready, not unready.** With the last writer gone a
     * read returns zero at once, and a poll that called that "not ready" would
     * leave the terminal emulator asleep for ever upon the shell it started,
     * exactly when the shell has ended and the window should close.
     */
    PollRequire(OxysClose(ends[1]) == 0, "the end that writes could not be closed");

    entries[0].descriptor = ends[0];
    entries[0].ready = 0U;
    PollRequire(OxysPoll(entries, 1U, SYSCALL_POLL_NO_WAIT) == 1,
                "a pipe whose last writer has gone was not reported ready");

    byte = 'z';
    PollRequire(OxysRead(ends[0], &byte, 1U) == 0,
                "the pipe at its end did not read as an end of file");

    /* Two entries at once, which is the shape the emulator actually uses. */
    {
        int second[2];

        if (OxysPipe(second) == 0)
        {
            entries[0].descriptor = ends[0];
            entries[1].descriptor = second[0];
            entries[0].ready = 0U;
            entries[1].ready = 1U;

            PollRequire(OxysPoll(entries, 2U, SYSCALL_POLL_NO_WAIT) == 1,
                        "a poll of two found other than the one that was ready");
            PollRequire((entries[0].ready == 1U) && (entries[1].ready == 0U),
                        "a poll of two wrote the readiness against the wrong entry");

            (void)OxysClose(second[0]);
            (void)OxysClose(second[1]);
        }
        else
        {
            PollRequire(false, "a second pipe could not be made");
        }
    }

    (void)OxysClose(ends[0]);

    /*
     * --- The terminal's refusal, of sub-task 9.6. ---
     *
     * `tcgroup` upon a process whose standard input is not the terminal is
     * ENOTTY. Without it a shell started anywhere takes the terminal from the
     * shell a person is typing at, which is how the machine came to livelock;
     * docs/design/SHELL.md, Section 2.6.
     *
     * The child is the one that tries it, because the parent's own standard
     * input **is** the terminal and a parent that redirected its own would have
     * nothing to put back. The parent then asserts the other half: that a
     * process at the terminal is not refused. It asks for the group rather than
     * setting one, a self-test that took the terminal's foreground group from
     * the shell being a self-test with a consequence.
     */
    {
        int nowhere[2];

        if (OxysPipe(nowhere) == 0)
        {
            const int64_t child = OxysFork();

            if (child == 0)
            {
                int failures = 0;

                (void)OxysDuplicate(nowhere[0], 0);

                if (OxysTerminalGroup(0) != -1)
                {
                    (void)printf("  tcgroup with a pipe upon 0 was not refused FAILED.\n");
                    ++failures;
                }
                else if (errno != ENOTTY)
                {
                    (void)printf("  tcgroup with a pipe upon 0 was refused wrongly FAILED.\n");
                    ++failures;
                }

                OxysExit(failures);
            }

            PollRequire(child > 0, "the child that tries tcgroup could not be forked");

            {
                int64_t status = 0;

                    (void)OxysWait(&status);
                PollRequire(status == 0, "the child found tcgroup's refusal wrong");
            }

            (void)OxysClose(nowhere[0]);
            (void)OxysClose(nowhere[1]);
        }
        else
        {
            PollRequire(false, "a pipe for the tcgroup child could not be made");
        }
    }

    PollRequire(OxysTerminalGroup(0) >= 0,
                "tcgroup was refused to a process whose standard input is the terminal");

    (void)printf("poll-check: %d assertion(s) failed.\n", PollFailures);

    return PollFailures;
}
