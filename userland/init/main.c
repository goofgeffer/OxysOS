/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/init/main.c
 * Purpose: `init`, the first user process of sub-task 9.3: it starts and
 *          supervises the desktop where there is one, collects the orphans
 *          every process reparents to it, and stops the machine in order when
 *          it is asked to.
 * Key functions: main, InitSpawn, InitShutdownRequested, InitHalt, InitReboot.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `power`, `pause`, and the wait and signal
 *     calls the loop is built from.
 *   - docs/design/WINDOWS.md, Section 13: the supervision and the shutdown,
 *     paired with what each would leave undone.
 *   - IEEE Std 1003.1-2017: `waitpid`, `pause`, `fork`, `execve`, `kill`.
 *
 * Why the handlers only set a flag.
 *
 *   A shutdown may arrive while `init` is asleep in `waitpid` or `pause`; the
 *   signal wakes it, the call reports `EINTR`, and the handler has run. The
 *   handler records which shutdown was asked and returns, and the loop — which
 *   is where the children are and where a system call may block — performs it:
 *   it stops the desktop it started, collects it, and only then calls `power`.
 *   A handler that called `power` itself would stop the machine with the
 *   desktop still drawing, which is the disorder this process exists to avoid.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <syscall.h>

/* What shutdown was asked for: nothing, a halt, or a restart. */
static volatile sig_atomic_t InitShutdown;

/* Where the desktop stands, so that a person may change it in one place. */
#define INIT_DESKTOP "/bin/windows"

static void InitHalt(int signal)
{
    (void)signal;
    InitShutdown = SYSCALL_POWER_HALT;
}

static void InitReboot(int signal)
{
    (void)signal;
    InitShutdown = SYSCALL_POWER_REBOOT;
}

/* Starts a program and returns its process, or -1 where it could not fork. */
static int64_t InitSpawn(const char *path)
{
    const int64_t child = OxysFork();

    if (child == 0)
    {
        char *const argument_vector[] = { (char *)path, NULL };

        (void)OxysExecve(path, argument_vector, NULL);

        /* Reached only where the program could not be executed; a child that
         * returned from execve must not go on to be a second init. */
        OxysExit(127);
    }

    return child;
}

int main(void)
{
    SyscallWindowRectangle screen;
    bool desktop;
    int64_t desktop_pid = -1;

    /* The two shutdowns: SIGTERM halts, SIGINT restarts. `shutdown` sends
     * them; the terminal's control-C cannot, init being in no foreground
     * group. Every other signal keeps its default, SIGCHLD being ignored. */
    (void)signal(SIGTERM, InitHalt);
    (void)signal(SIGINT, InitReboot);

    /* The desktop exists where the window manager has the screen, which is the
     * default boot entry; window_screen is ENOTSUP where it does not, and init
     * then supervises nothing and only collects orphans and waits to shut
     * down. */
    desktop = (OxysWindowScreen(&screen) == 0);

    if (desktop)
    {
        desktop_pid = InitSpawn(INIT_DESKTOP);
    }

    for (;;)
    {
        int64_t status = 0;
        int64_t ended;

        /*
         * The order matters: the shutdown is acted upon before the wait, so
         * that a request that arrived while the last wait slept is not left
         * until the next child ends — which upon a quiet desktop could be
         * never.
         */
        if (InitShutdown != 0)
        {
            const int action = InitShutdown;

            /* Stop the desktop and collect it, so that the machine stops with
             * nothing still drawing. A desktop that will not stop is not
             * waited upon for ever: power stops every processor regardless. */
            if (desktop_pid > 0)
            {
                (void)OxysKill(desktop_pid, SIGTERM);
                (void)OxysWaitFor(desktop_pid, &status, 0);
                desktop_pid = -1;
            }

            (void)OxysPower((uint64_t)action);

            /* power does not return upon success; a return is a failure, and
             * there is nothing left to do but wait to be asked again. */
            InitShutdown = 0;
        }

        ended = OxysWaitFor((int64_t)-1, &status, 0);

        if (ended == desktop_pid)
        {
            /*
             * The desktop ended — every window closed, or it faulted — and is
             * started again: a desktop a person closed and could not get back
             * is a desktop that is gone, and supervision is the one thing init
             * adds over a program the kernel could have launched itself.
             */
            desktop_pid = InitSpawn(INIT_DESKTOP);
        }
        else if (ended < 0)
        {
            /*
             * ECHILD: nothing to collect and nothing to supervise — wait for a
             * signal rather than spin, and a signal is what a shutdown is or
             * what the adoption of an orphan sends. EINTR: a signal arrived;
             * the loop goes round and acts upon it at the top.
             */
            if (errno == ECHILD)
            {
                (void)OxysPause();
            }
        }

        /* Any other ended child was an orphan collected; the loop goes round. */
    }

    return EXIT_SUCCESS;
}
