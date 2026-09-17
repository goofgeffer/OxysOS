/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/proc/signal.c
 * Purpose: Asserts the signals, the process groups and the `waitpid` of
 *          sub-task 8.7 — from the kernel, the parts a program cannot see, and
 *          then by running signal-check at privilege level 3, which asserts
 *          the rest from the only side that can reach it.
 * Key functions: KernelVerifySignals, VerifySignalsKernel, VerifySignalsRun.
 * References:
 *   - userland/signal-check/main.c: the program, and what it asserts.
 *   - kernel/include/oxys/proc/signal.h: the interface the kernel-side
 *     assertions are made against.
 *   - docs/design/PROCESS.md, Section 18.5: the table pairing every property
 *     asserted here with the silent failure the assertion exists to catch.
 *
 * What is asserted from the kernel and why.
 *
 *   A program cannot see a pending bit, only what happens when it is
 *   delivered; and it cannot send a signal to a process that has no thread
 *   to deliver it on. The kernel-side assertions below make a process that
 *   never runs, send it signals, and read the bits: that an ignored signal is
 *   discarded at generation, that SIGCONT discards a pending stop and a stop a
 *   pending SIGCONT, that SIGKILL and SIGSTOP cannot be given a disposition,
 *   and that the vectors map to the signals they should. Each of those is a
 *   rule a program would only see the consequence of, and the consequence of
 *   the wrong rule is a program that sometimes does not die.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>
#include <oxys/proc/signal.h>

extern const uint8_t KernelProgramSignalCheckBegin[];
extern const uint8_t KernelProgramSignalCheckEnd[];

static bool VerifySignalsSucceeded;

static void VerifySignalsRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        VerifySignalsSucceeded = false;
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
    }
}

/* The rules a program cannot see, upon a process that never runs. */
static void VerifySignalsKernel(void)
{
    Process *const process = ProcessCreate("signal-fixture", NULL);
    uint64_t previous = 99U;

    if (process == NULL)
    {
        VerifySignalsRequire(false, "a fixture process could not be created");

        return;
    }

    VerifySignalsRequire(process->group == process->id, "a new process does not lead its own group");
    VerifySignalsRequire(!SignalIsPending(process), "a new process has a signal pending");

    /* Sent is pending; taken is the lowest first, and then nothing. */
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGTERM), "SIGTERM could not be sent");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGUSR1), "SIGUSR1 could not be sent");
    VerifySignalsRequire(SignalIsPending(process), "a sent signal is not pending");
    VerifySignalsRequire(SignalTake(process) == SYSCALL_SIGUSR1,
                         "the lowest-numbered pending signal was not taken first");
    VerifySignalsRequire(SignalTake(process) == SYSCALL_SIGTERM,
                         "the second pending signal was not taken next");
    VerifySignalsRequire(SignalTake(process) == 0U, "a signal was taken from an empty set");

    /* Ignored at generation. */
    VerifySignalsRequire(SignalSetDisposition(process, SYSCALL_SIGINT, SYSCALL_SIGNAL_IGNORE,
                                              0U, &previous) &&
                             (previous == SYSCALL_SIGNAL_DEFAULT),
                         "SIGINT could not be set to ignore");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGINT) && !SignalIsPending(process),
                         "an ignored signal was made pending");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGCHLD) && !SignalIsPending(process),
                         "SIGCHLD, ignored by default, was made pending");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGCONT) && !SignalIsPending(process),
                         "SIGCONT to a process that is not stopped was made pending");

    /* A signal of 0 is a test and nothing more; an invalid one is refused. */
    VerifySignalsRequire(SignalSend(process, 0U) && !SignalIsPending(process),
                         "a signal of 0 did something");
    VerifySignalsRequire(!SignalSend(process, SYSCALL_SIGNAL_MAXIMUM + 1U),
                         "a signal beyond the maximum was accepted");

    /* The two the standard withholds. */
    VerifySignalsRequire(!SignalSetDisposition(process, SYSCALL_SIGKILL, SYSCALL_SIGNAL_IGNORE,
                                               0U, NULL),
                         "SIGKILL could be ignored");
    VerifySignalsRequire(!SignalSetDisposition(process, SYSCALL_SIGSTOP, 0x1000U, 0x2000U, NULL),
                         "SIGSTOP could be caught");

    /* A stop discards a pending continue, and a continue a pending stop. */
    VerifySignalsRequire(SignalSetDisposition(process, SYSCALL_SIGCONT, 0x1000U, 0x2000U, NULL),
                         "SIGCONT could not be given a handler");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGCONT) && SignalIsPending(process),
                         "SIGCONT with a handler was not made pending");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGTSTP), "SIGTSTP could not be sent");
    VerifySignalsRequire(SignalTake(process) == SYSCALL_SIGTSTP,
                         "a pending SIGCONT survived a stop signal");
    VerifySignalsRequire(SignalTake(process) == 0U, "something remained after the stop");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGSTOP), "SIGSTOP could not be sent");
    VerifySignalsRequire(SignalSend(process, SYSCALL_SIGCONT), "SIGCONT could not be sent");
    VerifySignalsRequire(SignalTake(process) == SYSCALL_SIGCONT,
                         "a pending stop survived SIGCONT, or SIGCONT was not pending");
    VerifySignalsRequire(SignalTake(process) == 0U, "something remained after the continue");

    /* An exec resets a handler and keeps an ignore. */
    SignalResetForExecute(process);
    VerifySignalsRequire(SignalDisposition(process, SYSCALL_SIGCONT) == SYSCALL_SIGNAL_DEFAULT,
                         "a handler survived an exec");
    VerifySignalsRequire(SignalDisposition(process, SYSCALL_SIGINT) == SYSCALL_SIGNAL_IGNORE,
                         "an ignored signal did not survive an exec");

    /* The vectors. */
    VerifySignalsRequire(SignalFromVector(14U) == SYSCALL_SIGSEGV, "a page fault is not SIGSEGV");
    VerifySignalsRequire(SignalFromVector(13U) == SYSCALL_SIGSEGV,
                         "a general protection fault is not SIGSEGV");
    VerifySignalsRequire(SignalFromVector(6U) == SYSCALL_SIGILL, "an invalid opcode is not SIGILL");
    VerifySignalsRequire(SignalFromVector(0U) == SYSCALL_SIGFPE, "a divide error is not SIGFPE");
    VerifySignalsRequire(SignalFromVector(17U) == SYSCALL_SIGBUS, "an alignment check is not SIGBUS");
    VerifySignalsRequire(SignalFromVector(3U) == SYSCALL_SIGTRAP, "a breakpoint is not SIGTRAP");

    /* The default actions. */
    VerifySignalsRequire(SignalDefaultAction(SYSCALL_SIGTERM) == SIGNAL_ACTION_TERMINATE,
                         "SIGTERM does not terminate by default");
    VerifySignalsRequire(SignalDefaultAction(SYSCALL_SIGTSTP) == SIGNAL_ACTION_STOP,
                         "SIGTSTP does not stop by default");
    VerifySignalsRequire(SignalDefaultAction(SYSCALL_SIGCHLD) == SIGNAL_ACTION_IGNORE,
                         "SIGCHLD is not ignored by default");
    VerifySignalsRequire(SignalDefaultAction(SYSCALL_SIGCONT) == SIGNAL_ACTION_CONTINUE,
                         "SIGCONT does not continue by default");

    ProcessDestroy(process);
}

static Thread *VerifySignalsBoot;

static bool VerifySignalsRun(int64_t *status)
{
    const uint64_t length =
        (uint64_t)(KernelProgramSignalCheckEnd - KernelProgramSignalCheckBegin);
    static const char name[] = "signal-check";
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();
    const size_t processes_before = ProcessCount();

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)sizeof name;

    for (size_t index = 0U; index < sizeof name; ++index)
    {
        arguments.storage[index] = name[index];
    }

    process = ProcessCreate(name, NULL);

    if (process == NULL)
    {
        VerifySignalsRequire(false, "a process could not be created for signal-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramSignalCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifySignalsRequire(false, "signal-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifySignalsRequire(false, "signal-check was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifySignalsRequire(false, "signal-check could not be started");

        return false;
    }

    VerifySignalsRequire(ThreadCurrent() == VerifySignalsBoot,
                         "the kernel did not resume the thread that started signal-check");
    VerifySignalsRequire(process->state == PROCESS_EXITED,
                         "signal-check's process was not marked as ended");
    VerifySignalsRequire(SYSCALL_STATUS_KIND(process->wait_status) == SYSCALL_STATUS_KIND_EXITED,
                         "signal-check itself was ended by a signal");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifySignalsRequire(VfsOpenFileCount() == open_before,
                         "signal-check left an open file behind it");
    VerifySignalsRequire(ProcessCount() == processes_before,
                         "signal-check left a child in the table");

    return true;
}

void KernelVerifySignals(void)
{
    int64_t status = 0;

    VerifySignalsSucceeded = true;

    KernelWriteString("Signals: asserting the pending set, the dispositions and the default "
                      "actions, then running signal-check.\n");

    VerifySignalsKernel();

    VerifySignalsBoot = ThreadAdoptCurrent("boot");

    if (VerifySignalsBoot == NULL)
    {
        VerifySignalsRequire(false, "the kernel's own flow of control could not be adopted");
    }
    else
    {
        if (VerifySignalsRun(&status) && (status != 0))
        {
            KernelWriteString("  signal-check FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifySignalsSucceeded = false;
        }

        ThreadDestroy(VerifySignalsBoot);
        VerifySignalsBoot = NULL;
    }

    if (VerifySignalsSucceeded)
    {
        KernelWriteString("Signal self-test passed: a sent signal is pending and an ignored one "
                          "is not, SIGKILL and SIGSTOP cannot be given a disposition, a stop and "
                          "a continue discard each other, and signal-check caught, ignored, "
                          "sent, stopped, continued and killed from privilege level 3.\n");
    }
    else
    {
        KernelWriteString("Signal self-test FAILED.\n");
    }
}
