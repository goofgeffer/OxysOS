/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/jobs.c
 * Purpose: Job control, of sub-task 8.7: the table of jobs, each a process
 *          group made for one pipeline; the foreground job, which holds the
 *          terminal and is waited for until it ends or stops; the background
 *          job, which is left to run and reported at the next prompt; and
 *          `jobs`, `fg`, `bg` and `kill`, which are how a person governs them.
 * Key functions: ShellJobsInitialise, ShellJobBegin, ShellJobAddMember,
 *          ShellJobPrepareChild, ShellJobWaitForeground, ShellJobsNotify,
 *          ShellJobsList, ShellJobForeground, ShellJobBackground,
 *          ShellJobKill, ShellJobStatus.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.9.3.1 (Asynchronous Lists): a list
 *     ended by `&` is executed asynchronously and the shell does not wait for
 *     it, and Section 2.11 (Signals and Error Handling); `jobs`, `fg`, `bg`,
 *     `kill` and `wait` among the utilities; Section 2.9.2, that the status of
 *     a pipeline is the last command's.
 *   - IEEE Std 1003.1-2017, System Interfaces, Section 11.1.4 (Terminal
 *     Access Control): a process in a background group that reads the
 *     terminal is sent SIGTTIN, and `tcsetpgrp` is how the foreground group
 *     is chosen; `setpgid`, and why both the parent and the child call it.
 *   - IEEE Std 1003.1-2017, `sh`, EXTENDED DESCRIPTION, Job Control: the `%n`
 *     job identifier, and the report of a job's state.
 *   - docs/design/SHELL.md, Section 28.
 *
 * Why both the parent and the child set the group.
 *
 *   The child must be in its group before it reads the terminal, or SIGTTIN
 *   stops it; the parent must know the child is in its group before it sends
 *   the group a signal, or the signal misses. Neither can know which of the
 *   two runs first after the fork, so each sets the group and each sets the
 *   terminal's foreground, and the second of them to do so does nothing. That
 *   is what every shell of this lineage does, and it is not redundancy.
 *
 * Why the shell ignores SIGINT and SIGTSTP.
 *
 *   The terminal sends both to the foreground group, and between jobs the
 *   foreground group is the shell's own. A shell that took the default action
 *   would end at the first control-C typed at its prompt and stop at the
 *   first control-Z. Ignored dispositions survive `execve`, so each child
 *   puts them back to the default before it becomes a program: a program
 *   that inherited the shell's indifference to control-C could not be
 *   interrupted, which is the failure this file exists to prevent.
 */

#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <line.h>

#define SHELL_JOB_MAXIMUM 16U

typedef enum ShellJobState
{
    SHELL_JOB_RUNNING = 0,
    SHELL_JOB_STOPPED,
    SHELL_JOB_DONE
} ShellJobState;

typedef struct ShellJob
{
    int64_t group;
    int64_t member[SHELL_COMMAND_MAXIMUM];
    bool member_live[SHELL_COMMAND_MAXIMUM];
    bool member_stopped[SHELL_COMMAND_MAXIMUM];
    size_t member_count;
    int64_t last_status;    /* What the last member ended with, encoded. */
    ShellJobState state;
    bool background;
    bool reported;          /* Its ending or stop has been printed. */
    bool used;
    char text[LINE_CAPACITY];
} ShellJob;

static ShellJob ShellJobs[SHELL_JOB_MAXIMUM];
static int64_t ShellOwnGroup;

/* ------------------------------------------------------------------ helpers */

static ShellJob *ShellJobFind(int64_t group)
{
    for (size_t index = 0U; index < SHELL_JOB_MAXIMUM; ++index)
    {
        if (ShellJobs[index].used && (ShellJobs[index].group == group))
        {
            return &ShellJobs[index];
        }
    }

    return NULL;
}

static ShellJob *ShellJobByPid(int64_t pid, size_t *member)
{
    for (size_t index = 0U; index < SHELL_JOB_MAXIMUM; ++index)
    {
        ShellJob *const job = &ShellJobs[index];

        if (!job->used)
        {
            continue;
        }

        for (size_t which = 0U; which < job->member_count; ++which)
        {
            if (job->member[which] == pid)
            {
                *member = which;

                return job;
            }
        }
    }

    return NULL;
}

static unsigned ShellJobNumber(const ShellJob *job)
{
    return (unsigned)(job - ShellJobs) + 1U;
}

static const char *ShellJobStateText(const ShellJob *job)
{
    switch (job->state)
    {
    case SHELL_JOB_STOPPED:
        return "Stopped";
    case SHELL_JOB_DONE:
        /* Ended by a signal is said by the signal, as every shell of this
         * lineage says it, so that a job killed is not reported as done. */
        if (SYSCALL_STATUS_KIND(job->last_status) == SYSCALL_STATUS_KIND_SIGNALLED)
        {
            switch (SYSCALL_STATUS_NUMBER(job->last_status))
            {
            case SIGINT:
                return "Interrupt";
            case SIGKILL:
                return "Killed";
            case SIGTERM:
                return "Terminated";
            case SIGPIPE:
                return "Broken pipe";
            case SIGSEGV:
                return "Fault";
            default:
                return "Signalled";
            }
        }

        return "Done";
    case SHELL_JOB_RUNNING:
    default:
        return "Running";
    }
}

/* Turns the status of a job's last member into the number the shell reports:
 * the code, or 128 plus the signal that ended or stopped it — Section 2.8.2. */
int ShellJobStatus(int64_t status)
{
    if (SYSCALL_STATUS_KIND(status) == SYSCALL_STATUS_KIND_EXITED)
    {
        return (int)SYSCALL_STATUS_NUMBER(status);
    }

    return 128 + (int)SYSCALL_STATUS_NUMBER(status);
}

static void ShellJobPrint(const ShellJob *job)
{
    (void)printf("[%u]%s %-8s %s\n", ShellJobNumber(job), job->background ? " " : "+",
                 ShellJobStateText(job), job->text);
}

/* Records what waitpid reported of one member, and what that makes of the job. */
static void ShellJobRecord(ShellJob *job, size_t member, int64_t status)
{
    const uint64_t kind = SYSCALL_STATUS_KIND(status);
    bool any_live = false;
    bool all_stopped = true;

    if (kind == SYSCALL_STATUS_KIND_STOPPED)
    {
        job->member_stopped[member] = true;
    }
    else
    {
        job->member_live[member] = false;
        job->member_stopped[member] = false;

        if (member + 1U == job->member_count)
        {
            job->last_status = status;
        }
    }

    for (size_t which = 0U; which < job->member_count; ++which)
    {
        if (job->member_live[which])
        {
            any_live = true;

            if (!job->member_stopped[which])
            {
                all_stopped = false;
            }
        }
    }

    if (!any_live)
    {
        job->state = SHELL_JOB_DONE;
    }
    else if (all_stopped)
    {
        job->state = SHELL_JOB_STOPPED;
    }
    else
    {
        job->state = SHELL_JOB_RUNNING;
    }

    if (job->state != SHELL_JOB_RUNNING)
    {
        job->reported = false;
    }
}

/* Marks every member running again, which is what a continue does. */
static void ShellJobContinued(ShellJob *job)
{
    for (size_t which = 0U; which < job->member_count; ++which)
    {
        job->member_stopped[which] = false;
    }

    job->state = SHELL_JOB_RUNNING;
    job->reported = true;
}

/*
 * Whether this shell has a terminal, and therefore job control.
 *
 * It is false where `tcgroup` refused the shell at start, which since sub-task
 * 9.6 is what a shell whose standard input is not the terminal is told: the one
 * in the terminal emulator's window, whose input is a pipe, and one a person
 * ran with its input redirected from a file.
 *
 * **Without it the shell puts nothing in a group of its own.** That is not a
 * degraded version of job control but the absence of it: with no terminal there
 * is no foreground group to give a job, so `fg`, `bg` and control-Z have
 * nothing to act upon, and every child stays in the shell's own group — which
 * is what lets the emulator interrupt a running command by interrupting that
 * group. A shell that went on setting groups without a terminal would put its
 * children where nothing could reach them.
 *
 * docs/design/TERMINAL.md, Section 5, and docs/design/SHELL.md, Section 28.
 */
static bool ShellHasTerminal;

/* Gives the terminal back to the shell: what every foreground job's end does,
 * where there is a terminal to give. */
static void ShellJobReclaimTerminal(void)
{
    if (ShellHasTerminal)
    {
        (void)OxysTerminalGroup(ShellOwnGroup);
    }
}

/* --------------------------------------------------------------- the table */

void ShellJobsInitialise(void)
{
    ShellOwnGroup = OxysGetProcessGroup(0);
    ShellHasTerminal = (ShellOwnGroup > 0) && (OxysTerminalGroup(ShellOwnGroup) >= 0);

    /*
     * The two the terminal sends to the foreground group are ignored whether or
     * not this shell has a terminal. In a window they arrive because the
     * emulator sends them to the group the shell leads, and a shell that took
     * the default action would end at the first control-C typed at its prompt.
     */
    (void)signal(SIGINT, SIG_IGN);
    (void)signal(SIGTSTP, SIG_IGN);
}

ShellJob *ShellJobBegin(const char *text, bool background)
{
    for (size_t index = 0U; index < SHELL_JOB_MAXIMUM; ++index)
    {
        ShellJob *const job = &ShellJobs[index];

        if (job->used)
        {
            continue;
        }

        job->group = 0;
        job->member_count = 0U;
        job->last_status = 0;
        job->state = SHELL_JOB_RUNNING;
        job->background = background;
        job->reported = true;
        job->used = true;
        (void)snprintf(job->text, sizeof job->text, "%s", text);

        return job;
    }

    (void)fprintf(stderr, "sh: no room for another job; %u are held.\n",
                  (unsigned)SHELL_JOB_MAXIMUM);

    return NULL;
}

void ShellJobPrepareChild(const ShellJob *job, bool background)
{
    /* The child's half of the group, and of the terminal for a foreground
     * job; the parent's is ShellJobAddMember. The dispositions the shell
     * ignores go back to the default here, before the program that would
     * otherwise inherit the indifference. */
    const int64_t group = (job->group != 0) ? job->group : 0;

    if (ShellHasTerminal)
    {
        (void)OxysSetProcessGroup(0, group);

        if (!background)
        {
            (void)OxysTerminalGroup((group != 0) ? group : OxysGetProcessId());
        }
    }

    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTSTP, SIG_DFL);
}

void ShellJobAddMember(ShellJob *job, int64_t pid)
{
    if (job->group == 0)
    {
        job->group = pid;
    }

    if (ShellHasTerminal)
    {
        (void)OxysSetProcessGroup(pid, job->group);

        if (!job->background)
        {
            (void)OxysTerminalGroup(job->group);
        }
    }

    if (job->member_count < SHELL_COMMAND_MAXIMUM)
    {
        job->member[job->member_count] = pid;
        job->member_live[job->member_count] = true;
        job->member_stopped[job->member_count] = false;
        ++job->member_count;
    }
}

void ShellJobAnnounce(const ShellJob *job)
{
    (void)printf("[%u] %lld\n", ShellJobNumber(job), (long long)job->group);
}

/*
 * Waits for the foreground job until every member has ended or every live
 * member has stopped, then takes the terminal back. A stopped job stays in
 * the table for `fg` and `bg`; an ended one is released. Returns the status
 * the shell reports for the pipeline.
 */
/*
 * Whom a wait for this job's members asks for.
 *
 * With a terminal each job leads a group of its own and the wait names that
 * group. Without one — the shell in a window, whose standard input is a pipe —
 * nothing was put in a group at all, so the members are in the shell's own and
 * a wait naming the job's leader would find no child of that group and report
 * that there is none to collect. It did, in the first terminal window this
 * system ever opened: every command ran, printed, and was followed by `sh: a
 * member of the job could not be collected: No child to collect.`
 */
static int64_t ShellJobWaitTarget(const ShellJob *job)
{
    return ShellHasTerminal ? -job->group : -ShellOwnGroup;
}

int ShellJobWaitForeground(ShellJob *job)
{
    int result;

    while (job->state == SHELL_JOB_RUNNING)
    {
        int64_t status = 0;
        const int64_t ended = OxysWaitFor(ShellJobWaitTarget(job), &status,
                                          SYSCALL_WAIT_UNTRACED);
        size_t member;

        if (ended < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            (void)fprintf(stderr, "sh: a member of the job could not be collected: %s.\n",
                          strerror(errno));
            job->state = SHELL_JOB_DONE;
            break;
        }

        /*
         * Recorded against whichever job the child belongs to and not against
         * this one alone. Where the wait names the shell's whole group it may
         * collect a background job's member, and a status dropped on the floor
         * is a job that never ends.
         */
        if (ended > 0)
        {
            ShellJob *const owner = ShellJobByPid(ended, &member);

            if (owner != NULL)
            {
                ShellJobRecord(owner, member, status);
            }
        }
    }

    ShellJobReclaimTerminal();

    if (job->state == SHELL_JOB_STOPPED)
    {
        /* The line the person's control-Z interrupted has no newline yet. */
        (void)printf("\n");
        ShellJobPrint(job);
        job->reported = true;
        job->background = true;

        return 128 + SIGTSTP;
    }

    result = ShellJobStatus(job->last_status);
    job->used = false;

    return result;
}

/* Collects whatever has ended or stopped in the background, and says so. */
void ShellJobsNotify(void)
{
    for (;;)
    {
        int64_t status = 0;
        const int64_t ended =
            OxysWaitFor(-1, &status, SYSCALL_WAIT_NO_HANG | SYSCALL_WAIT_UNTRACED);
        size_t member;
        ShellJob *job;

        if (ended <= 0)
        {
            break;
        }

        job = ShellJobByPid(ended, &member);

        if (job != NULL)
        {
            ShellJobRecord(job, member, status);
        }
    }

    for (size_t index = 0U; index < SHELL_JOB_MAXIMUM; ++index)
    {
        ShellJob *const job = &ShellJobs[index];

        if (!job->used || job->reported)
        {
            continue;
        }

        ShellJobPrint(job);
        job->reported = true;

        if (job->state == SHELL_JOB_DONE)
        {
            job->used = false;
        }
    }
}

/* ------------------------------------------------------------ the commands */

void ShellJobsList(void)
{
    ShellJobsNotify();

    for (size_t index = 0U; index < SHELL_JOB_MAXIMUM; ++index)
    {
        if (ShellJobs[index].used)
        {
            ShellJobPrint(&ShellJobs[index]);
        }
    }
}

/* The job an operand names: `%n`, `n`, or with no operand the newest. */
static ShellJob *ShellJobNamed(const char *operand)
{
    unsigned number = 0U;

    if (operand == NULL)
    {
        for (size_t index = SHELL_JOB_MAXIMUM; index > 0U; --index)
        {
            if (ShellJobs[index - 1U].used)
            {
                return &ShellJobs[index - 1U];
            }
        }

        (void)fprintf(stderr, "sh: no current job.\n");

        return NULL;
    }

    if (*operand == '%')
    {
        ++operand;
    }

    for (; *operand != '\0'; ++operand)
    {
        if ((*operand < '0') || (*operand > '9'))
        {
            number = 0U;
            break;
        }

        number = (number * 10U) + (unsigned)(*operand - '0');
    }

    if ((number == 0U) || (number > SHELL_JOB_MAXIMUM) || !ShellJobs[number - 1U].used)
    {
        (void)fprintf(stderr, "sh: no such job.\n");

        return NULL;
    }

    return &ShellJobs[number - 1U];
}

int ShellJobForeground(const char *operand)
{
    ShellJob *job;

    /* `fg` asks for a job to be given the terminal, and a shell with no
     * terminal has none to give. It is said plainly rather than attempted and
     * silently doing nothing: a person typing `fg` in a window and seeing
     * nothing happen would have no way to learn why. */
    if (!ShellHasTerminal)
    {
        (void)fprintf(stderr, "sh: this shell has no terminal, so there is no job control.\n");

        return 1;
    }

    job = ShellJobNamed(operand);

    if (job == NULL)
    {
        return 1;
    }

    (void)printf("%s\n", job->text);
    job->background = false;
    (void)OxysTerminalGroup(job->group);

    /*
     * **The continue is sent whether or not the shell has yet noticed the
     * stop**, and that is a correction of 2026-09-22.
     *
     * It was sent only where `job->state` said stopped, which is what the
     * shell last *observed* and not what the job is: a job stopped by SIGTTIN
     * a moment ago is stopped in the kernel and running as far as this table
     * knows, until a wait reports it. `fg` upon it then continued nothing, and
     * the wait beneath collected the stop at once and printed `[1]+ Stopped`
     * for a job the person had just asked to bring forward. A person can reach
     * it by typing `fg` quickly after a control-Z.
     *
     * It was found under Bochs, where the shell's job-control session failed
     * with the status of `xyz` — 127, a command not found — in place of the
     * 130 a control-C leaves: the `cat` that `fg` should have brought forward
     * stayed stopped, so the line meant for it was read by the shell. The same
     * image passed under QEMU and under VirtualBox, and the same source passed
     * under Bochs one commit earlier; what moved was the size of an unrelated
     * program, which is how a race announces itself.
     *
     * SIGCONT to a process that is not stopped does nothing, so there is no
     * case to distinguish and no reason to ask first.
     */
    (void)OxysKill(-job->group, SIGCONT);
    ShellJobContinued(job);

    return ShellJobWaitForeground(job);
}

int ShellJobBackground(const char *operand)
{
    ShellJob *job;

    /* As `fg`: a stopped job is one control-Z made, and without a terminal
     * nothing can stop a job in the first place. */
    if (!ShellHasTerminal)
    {
        (void)fprintf(stderr, "sh: this shell has no terminal, so there is no job control.\n");

        return 1;
    }

    job = ShellJobNamed(operand);

    if (job == NULL)
    {
        return 1;
    }

    /* Unconditionally, for the reason `fg` above records: a job stopped a
     * moment ago is stopped whether or not this table has heard of it yet. */
    (void)OxysKill(-job->group, SIGCONT);
    ShellJobContinued(job);

    job->background = true;
    (void)printf("[%u] %s &\n", ShellJobNumber(job), job->text);

    return 0;
}

/* The signals `kill` knows by name. */
typedef struct ShellSignalName
{
    const char *name;
    int number;
} ShellSignalName;

static const ShellSignalName ShellSignalNames[] = {
    { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT }, { "ILL", SIGILL },
    { "TRAP", SIGTRAP }, { "ABRT", SIGABRT }, { "BUS", SIGBUS },   { "FPE", SIGFPE },
    { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 },
    { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD },
    { "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "TTIN", SIGTTIN },
    { "TTOU", SIGTTOU }
};

static int ShellSignalByName(const char *text)
{
    int number = 0;

    if ((text[0] >= '0') && (text[0] <= '9'))
    {
        for (; *text != '\0'; ++text)
        {
            if ((*text < '0') || (*text > '9'))
            {
                return -1;
            }

            number = (number * 10) + (*text - '0');
        }

        return number;
    }

    if ((text[0] == 'S') && (text[1] == 'I') && (text[2] == 'G'))
    {
        text += 3;
    }

    for (size_t index = 0U; index < sizeof ShellSignalNames / sizeof ShellSignalNames[0]; ++index)
    {
        if (strcmp(ShellSignalNames[index].name, text) == 0)
        {
            return ShellSignalNames[index].number;
        }
    }

    return -1;
}

/*
 * `kill [-SIGNAL | -n] target...`: each target a `%n` job, which receives the
 * signal as a group, or a process identifier. SIGTERM where none is named.
 */
int ShellJobKill(int argc, char **argv)
{
    int number = SIGTERM;
    int first = 1;
    int status = 0;

    if ((argc > 1) && (argv[1][0] == '-') && (argv[1][1] != '\0'))
    {
        number = ShellSignalByName(&argv[1][1]);

        if (number < 0)
        {
            (void)fprintf(stderr, "sh: kill: %s: no such signal.\n", &argv[1][1]);

            return 1;
        }

        first = 2;
    }

    if (first >= argc)
    {
        (void)fprintf(stderr, "sh: kill: a process or a %%job is required.\n");

        return 1;
    }

    for (int index = first; index < argc; ++index)
    {
        int64_t target;

        if (argv[index][0] == '%')
        {
            ShellJob *const job = ShellJobNamed(argv[index]);

            if (job == NULL)
            {
                status = 1;
                continue;
            }

            /*
             * A job is named by its group, and without job control there are
             * no groups: every child is in the shell's own, so `-job->group`
             * would name a group nothing is in. `kill %1` is therefore refused
             * here rather than sending a signal to nobody; `kill <pid>` still
             * works, the pid being a process and not a group.
             */
            if (!ShellHasTerminal)
            {
                (void)fprintf(stderr,
                              "sh: this shell has no terminal, so there is no job control; "
                              "name the process rather than the job.\n");
                status = 1;
                continue;
            }

            target = -job->group;
        }
        else
        {
            target = 0;

            for (const char *at = argv[index]; *at != '\0'; ++at)
            {
                if ((*at < '0') || (*at > '9'))
                {
                    target = -1;
                    break;
                }

                target = (target * 10) + (*at - '0');
            }

            if (target <= 0)
            {
                (void)fprintf(stderr, "sh: kill: %s: not a process or a %%job.\n", argv[index]);
                status = 1;
                continue;
            }
        }

        if (OxysKill(target, number) < 0)
        {
            (void)fprintf(stderr, "sh: kill: %s: %s.\n", argv[index], strerror(errno));
            status = 1;
        }
        else
        {
            ShellJob *const job = ShellJobFind((target < 0) ? -target : target);

            if (number == SIGCONT)
            {
                if (job != NULL)
                {
                    ShellJobContinued(job);
                }
            }
            else if ((job != NULL) && (job->state == SHELL_JOB_STOPPED) &&
                     (number != SIGSTOP) && (number != SIGTSTP) && (number != SIGTTIN) &&
                     (number != SIGTTOU))
            {
                /* A stopped job acts upon nothing until it is continued, so a
                 * `kill %1` of one would otherwise appear to do nothing until
                 * an `fg`; it is continued after the signal, as every shell of
                 * this lineage does, so that the signal is acted upon now. */
                (void)OxysKill(target, SIGCONT);
                ShellJobContinued(job);
            }
        }

    }

    return status;
}
