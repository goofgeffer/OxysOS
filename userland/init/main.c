/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/init/main.c
 * Purpose: `init`, the first user process of sub-task 9.3: it starts and
 *          supervises the services — read, since sub-task 9.4, from
 *          /etc/system.conf rather than written into this program — collects
 *          the orphans every process reparents to it, and stops the machine in
 *          order when it is asked to.
 * Key functions: main, InitReadServices, InitSpawn, InitStartService,
 *          InitSupervise, InitHalt, InitReboot.
 * References:
 *   - libc/include/config.h: the format, and the parser that reads it.
 *   - kernel/abi/oxys/syscall_abi.h: `power`, `pause`, and the wait and signal
 *     calls the loop is built from.
 *   - docs/design/INIT.md, Section 2: the supervision and the shutdown;
 *     docs/design/CONFIG.md, Section 3: the keys this reads and what each
 *     means.
 *   - IEEE Std 1003.1-2017: `waitpid`, `pause`, `fork`, `execve`, `kill`.
 *
 * Why the handlers only set a flag.
 *
 *   A shutdown may arrive while `init` is asleep in `waitpid` or `pause`; the
 *   signal wakes it, the call reports `EINTR`, and the handler has run. The
 *   handler records which shutdown was asked and returns, and the loop — which
 *   is where the children are and where a system call may block — performs it:
 *   it stops the services it started, collects them, and only then calls
 *   `power`. A handler that called `power` itself would stop the machine with
 *   the desktop still drawing, which is the disorder this process exists to
 *   avoid.
 *
 * What a configuration that cannot be read costs.
 *
 *   A machine whose /etc/system.conf is missing or unreadable is not a machine
 *   with no supervisor: `init` says so upon the serial line and falls back to
 *   the one service it would otherwise have had written into it. That is the
 *   whole of the defence, and it is deliberate — the alternative is a machine
 *   that boots to a bare screen because somebody mistyped a path in a file, and
 *   the person best placed to notice is the one who cannot then start a shell.
 */

#include <config.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* Where the configuration stands, and what is used in its absence. */
#define INIT_CONFIGURATION "/etc/system.conf"
#define INIT_FALLBACK_NAME "desktop"
#define INIT_FALLBACK_RUN  "/bin/windows"

/*
 * How many times a service that keeps ending is started again before `init`
 * gives up on it.
 *
 * A bound upon consecutive failures and not a rate, there being no clock a
 * program may read — docs/design/LIBC.md, Section 13. It is what stops a
 * service whose program is missing, or which faults upon its first
 * instruction, from being restarted for the machine's whole life; the count is
 * cleared whenever the service is stopped by `init` itself, which is the one
 * ending that says nothing about whether the program works.
 */
#define INIT_RESTART_LIMIT 5

#define INIT_SERVICES_MAXIMUM 8

typedef struct InitService
{
    char name[CONFIG_SECTION_MAXIMUM + 1U];
    char run[CONFIG_VALUE_MAXIMUM + 1U];
    bool restart;
    bool needs_display;

    int64_t pid;       /* -1 where it is not running. */
    int failures;      /* Consecutive endings; INIT_RESTART_LIMIT gives up. */
    bool abandoned;
} InitService;

static InitService InitServices[INIT_SERVICES_MAXIMUM];
static size_t InitServiceCount;

static OxysConfig InitConfig;

/* What shutdown was asked for: nothing, a halt, or a restart. */
static volatile sig_atomic_t InitShutdown;

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

/* Copies at most `capacity` characters, terminating always. */
static void InitCopy(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    while ((source[index] != '\0') && (index < capacity))
    {
        destination[index] = source[index];
        ++index;
    }

    destination[index] = '\0';
}

static void InitAddService(const char *name, const char *run, bool restart, bool needs_display)
{
    InitService *service;

    if (InitServiceCount >= INIT_SERVICES_MAXIMUM)
    {
        (void)fprintf(stderr, "init: more services than %d; %s is not started.\n",
                      INIT_SERVICES_MAXIMUM, name);

        return;
    }

    service = &InitServices[InitServiceCount];
    InitCopy(service->name, CONFIG_SECTION_MAXIMUM, name);
    InitCopy(service->run, CONFIG_VALUE_MAXIMUM, run);
    service->restart = restart;
    service->needs_display = needs_display;
    service->pid = -1;
    service->failures = 0;
    service->abandoned = false;
    ++InitServiceCount;
}

/*
 * Reads the services from the configuration. Returns whether the file was
 * read at all; the services it held are in the table either way, and a file
 * that named none leaves the table empty, which is a machine a person asked
 * for and not a machine that failed.
 */
static bool InitReadServices(void)
{
    const bool read = OxysConfigRead(&InitConfig, INIT_CONFIGURATION);
    const size_t blocks = OxysConfigCount(&InitConfig, "service");
    const char *banner;

    for (size_t index = 0U; index < OxysConfigFaultCount(&InitConfig); ++index)
    {
        (void)fprintf(stderr, "init: %s, line %lu: %s.\n", INIT_CONFIGURATION,
                      (unsigned long)OxysConfigFaultLine(&InitConfig, index),
                      OxysConfigFaultReason(&InitConfig, index));
    }

    if (!read && (blocks == 0U))
    {
        (void)fprintf(stderr, "init: %s could not be read; starting %s alone.\n",
                      INIT_CONFIGURATION, INIT_FALLBACK_RUN);
        InitAddService(INIT_FALLBACK_NAME, INIT_FALLBACK_RUN, true, true);

        return false;
    }

    banner = OxysConfigValue(&InitConfig, "system", 0U, "banner");

    if (banner != NULL)
    {
        (void)printf("init: %s\n", banner);
    }

    for (size_t index = 0U; index < blocks; ++index)
    {
        const char *const run = OxysConfigValue(&InitConfig, "service", index, "run");
        const char *const name = OxysConfigValue(&InitConfig, "service", index, "name");
        const char *const needs = OxysConfigValue(&InitConfig, "service", index, "needs");
        const char *const restart = OxysConfigValue(&InitConfig, "service", index, "restart");

        if (run == NULL)
        {
            (void)fprintf(stderr, "init: a service with no `run` is not started.\n");
            continue;
        }

        InitAddService((name != NULL) ? name : run, run,
                       (restart == NULL) || (strcmp(restart, "never") != 0),
                       (needs != NULL) && (strcmp(needs, "display") == 0));
    }

    return true;
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

/* Starts one service, unless it needs a display there is not or it has been
 * given up on. */
static void InitStartService(InitService *service, bool display)
{
    if (service->abandoned || (service->needs_display && !display))
    {
        return;
    }

    service->pid = InitSpawn(service->run);

    if (service->pid < 0)
    {
        (void)fprintf(stderr, "init: %s could not be started.\n", service->name);
    }
}

/* The service that child belongs to, or null. */
static InitService *InitServiceOf(int64_t pid)
{
    for (size_t index = 0U; index < InitServiceCount; ++index)
    {
        if (InitServices[index].pid == pid)
        {
            return &InitServices[index];
        }
    }

    return NULL;
}

/* A service has ended: start it again, or give up upon it and say so. */
static void InitSupervise(InitService *service, int64_t status, bool display)
{
    service->pid = -1;

    if (!service->restart)
    {
        return;
    }

    ++service->failures;

    if (service->failures >= INIT_RESTART_LIMIT)
    {
        service->abandoned = true;
        (void)fprintf(stderr,
                      "init: %s ended %d times in a row, last with status 0x%lx; "
                      "it is not started again.\n",
                      service->name, service->failures, (unsigned long)status);

        return;
    }

    InitStartService(service, display);
}

int main(void)
{
    SyscallWindowRectangle screen;
    bool display;

    /* The two shutdowns: SIGTERM halts, SIGINT restarts. `shutdown` sends
     * them; the terminal's control-C cannot, init being in no foreground
     * group. Every other signal keeps its default, SIGCHLD being ignored. */
    (void)signal(SIGTERM, InitHalt);
    (void)signal(SIGINT, InitReboot);

    /* A display exists where the window manager has the screen, which is the
     * default boot entry; window_screen is ENOTSUP where it does not, and a
     * service that needs one is then not started at all. */
    display = (OxysWindowScreen(&screen) == 0);

    (void)InitReadServices();

    for (size_t index = 0U; index < InitServiceCount; ++index)
    {
        InitStartService(&InitServices[index], display);
    }

    for (;;)
    {
        int64_t status = 0;
        int64_t ended;
        InitService *service;

        /*
         * The order matters: the shutdown is acted upon before the wait, so
         * that a request that arrived while the last wait slept is not left
         * until the next child ends — which upon a quiet desktop could be
         * never.
         */
        if (InitShutdown != 0)
        {
            const int action = InitShutdown;

            /* Stop every service and collect it, so that the machine stops
             * with nothing still drawing. A service that will not stop is not
             * waited upon for ever: power stops every processor regardless. */
            for (size_t index = 0U; index < InitServiceCount; ++index)
            {
                if (InitServices[index].pid > 0)
                {
                    (void)OxysKill(InitServices[index].pid, SIGTERM);
                    (void)OxysWaitFor(InitServices[index].pid, &status, 0);
                    InitServices[index].pid = -1;
                }
            }

            (void)OxysPower((uint64_t)action);

            /* power does not return upon success; a return is a failure, and
             * there is nothing left to do but wait to be asked again. */
            InitShutdown = 0;
        }

        ended = OxysWaitFor((int64_t)-1, &status, 0);

        if (ended > 0)
        {
            service = InitServiceOf(ended);

            if (service != NULL)
            {
                InitSupervise(service, status, display);
            }

            /* Any other ended child was an orphan collected; the loop goes
             * round. */
            continue;
        }

        /*
         * ECHILD: nothing to collect and nothing running — wait for a signal
         * rather than spin, and a signal is what a shutdown is or what the
         * adoption of an orphan sends. EINTR: a signal arrived; the loop goes
         * round and acts upon it at the top.
         */
        if (errno == ECHILD)
        {
            (void)OxysPause();
        }
    }

    return EXIT_SUCCESS;
}
