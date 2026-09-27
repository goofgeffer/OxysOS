/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/init-check/main.c
 * Purpose: Asserts the two calls of sub-task 9.3 from privilege level 3, the
 *          only side that can reach them: that `power` is refused a process
 *          that is not `init`, before the action is even looked at, and that
 *          `pause` returns EINTR for a signal, delivering it. Ends with the
 *          number of assertions that failed.
 * Key functions: main, InitRequire.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `power`, `pause`, and the results.
 *   - kernel/test/proc/init.c: the self-test that runs this, and the kernel
 *     half that asserts the adoption of orphans.
 *   - docs/design/WINDOWS.md.
 *
 * The self-test that runs this is not `init` and none is set, so `power` is
 * EPERM to it whatever it asks — which is the property asserted, and is what
 * lets this program call `power` at all without stopping the machine.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

static int InitFailures;

static void InitRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        ++InitFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

static volatile sig_atomic_t InitSignalSeen;

static void InitRecord(int signal)
{
    InitSignalSeen = signal;
}

/*
 * Returns once the process `id` is blocked, by walking the process table with
 * `procinfo` until its slot says so. Bounded, so that a parent which never
 * sleeps — the defect this program exists to find — ends in the assertion
 * after `pause` failing rather than in a child that spins forever.
 */
static void InitAwaitSleep(int64_t id)
{
    for (long attempt = 0; attempt < 1000000L; ++attempt)
    {
        SyscallProcessInformation information;
        int64_t result = 0;

        for (uint64_t index = 0U; result >= 0; ++index)
        {
            result = OxysProcessInformation(index, &information);

            if ((result == 1) && ((int64_t)information.id == id))
            {
                if (information.state == SYSCALL_PROCESS_STATE_BLOCKED)
                {
                    return;
                }

                break;
            }
        }
    }
}

int main(void)
{
    (void)printf("init-check: power and pause, from privilege level 3.\n");

    /* --- power is EPERM to a process that is not init, before the action. --- */

    errno = 0;
    InitRequire((OxysPower(SYSCALL_POWER_HALT) == -1) && (errno == EPERM),
                "power(HALT) from a process that is not init was not EPERM");
    errno = 0;
    InitRequire((OxysPower(SYSCALL_POWER_REBOOT) == -1) && (errno == EPERM),
                "power(REBOOT) from a process that is not init was not EPERM");
    errno = 0;
    InitRequire((OxysPower(9999U) == -1) && (errno == EPERM),
                "power with a bad action was not EPERM: the authority is not checked first");

    /* --- pause suspends until a signal, reports EINTR, and delivers it. --- */

    InitRequire(signal(SIGTERM, InitRecord) == SIG_DFL, "SIGTERM was not at its default");

    /*
     * A child sends the signal, rather than the parent to itself, because a
     * signal a process sends itself is delivered on the way out of `kill` —
     * before `pause` is reached — and `pause` would then sleep for a signal
     * already gone.
     *
     * **The child waits until it sees its parent asleep**, by `procinfo`,
     * before it sends. Until 2026-09-27 it spun a fixed while instead, on the
     * reasoning that a child cannot run until its parent sleeps or is
     * pre-empted. A pre-emption between `fork` and `pause` let the child finish
     * its spin and signal first: the handler ran as the parent resumed, and
     * `pause` then slept forever for a signal already delivered. Under
     * VirtualBox that hung the boot at this program's first line in about a
     * third of boots; a delay placed before `pause` reproduces it under QEMU.
     * The parent blocks nowhere between the two calls, so BLOCKED is `pause`.
     */
    {
        const int64_t self = OxysGetProcessId();
        const int64_t child = OxysFork();

        if (child == 0)
        {
            InitAwaitSleep(self);
            (void)OxysKill(self, SIGTERM);
            OxysExit(0);
        }

        errno = 0;
        InitRequire((OxysPause() == -1) && (errno == EINTR),
                    "pause did not report EINTR when a signal arrived");
        InitRequire(InitSignalSeen == SIGTERM, "the signal that ended the pause was not delivered");

        if (child > 0)
        {
            (void)OxysWaitFor(child, NULL, 0);
        }
    }

    (void)printf("init-check: %d assertion(s) failed.\n", InitFailures);

    return InitFailures;
}
