/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/proc/signal.c
 * Purpose: Implements the signals of sub-task 8.7: sending one to a process or
 *          a group, the pending set and the dispositions, the default action
 *          of each signal, the stop and the continue, and the signal a fault
 *          is reported as. The frame a handler runs upon is the architecture's,
 *          in kernel/arch/x86_64/syscall/sigframe.c, which calls SignalTake.
 * Key functions: SignalSend, SignalSendGroup, SignalIsPending, SignalTake,
 *          SignalDefaultAction, SignalSetDisposition, SignalDisposition,
 *          SignalResetForExecute, SignalStopCurrent, SignalContinue,
 *          SignalFromVector, SignalReport.
 * References:
 *   - IEEE Std 1003.1-2017, System Interfaces, Section 2.4.3 (Signal Actions):
 *     the default action of each signal — terminate, ignore, stop or continue —
 *     and that SIGKILL and SIGSTOP cannot be caught or ignored.
 *   - IEEE Std 1003.1-2017, `kill()`: "if sig is 0 ... error checking is
 *     performed but no signal is actually sent"; a `pid` of -1 or less names
 *     a process group.
 *   - IEEE Std 1003.1-2017, `exec` family: "signals set to be caught by the
 *     calling process image shall be set to the default action in the new
 *     process image", and signals set to be ignored stay so.
 *   - Intel SDM, Volume 3A, Section 6.15: the exception vectors, from which
 *     SignalFromVector chooses the signal a fault is reported as.
 *   - docs/design/PROCESS.md, Section 18.
 *
 * Why a sender never edits the target.
 *
 *   A signal changes where the target will next execute, and the only moment
 *   every register of a program stands in a frame the kernel can edit is the
 *   program's own way out of the kernel: a system call returning, or an
 *   interrupt taken at privilege level 3. So sending is a bit set, and where
 *   the target is asleep in a call a wake of it, and delivery is the target's
 *   own act on its way out. A sender that reached into a target asleep upon a
 *   pipe and rewrote its frame would be rewriting a frame the pipe's read is
 *   still going to return through.
 *
 * Concurrency. Every sender and every target is a thread upon the bootstrap
 *   processor — a program, or the terminal's service run by that processor's
 *   own tick handler — and a user thread is never pre-empted inside the
 *   kernel, so the pending word and the dispositions are never written by two
 *   threads at once. The masked sections below are what the wait channel's
 *   sleep requires, and what keeps the tick handler's send from interleaving
 *   with a system call's take upon the same word. docs/design/CONCURRENCY.md,
 *   Section 10, limitation 1, counts this file with the rest.
 */

#include <oxys/proc/signal.h>
#include <oxys/proc/sched.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/kernel.h>

/* The vectors of Intel SDM, Volume 3A, Table 6-1, that map to a signal of
 * their own; every other fault is SIGSEGV. */
#define SIGNAL_VECTOR_DIVIDE_ERROR       0U
#define SIGNAL_VECTOR_DEBUG              1U
#define SIGNAL_VECTOR_BREAKPOINT         3U
#define SIGNAL_VECTOR_OVERFLOW           4U
#define SIGNAL_VECTOR_INVALID_OPCODE     6U
#define SIGNAL_VECTOR_ALIGNMENT_CHECK    17U
#define SIGNAL_VECTOR_SIMD_FLOATING      19U

/* Accounting. */
static uint64_t SignalSent;
static uint64_t SignalHandled;
static uint64_t SignalStops;
static uint64_t SignalContinues;

/* ------------------------------------------------------------------ helpers */

static bool SignalNumberIsValid(uint32_t signal)
{
    return (signal >= 1U) && (signal <= SYSCALL_SIGNAL_MAXIMUM);
}

static uint32_t SignalBit(uint32_t signal)
{
    return UINT32_C(1) << signal;
}

static bool SignalIsStop(uint32_t signal)
{
    return (signal == SYSCALL_SIGSTOP) || (signal == SYSCALL_SIGTSTP) ||
           (signal == SYSCALL_SIGTTIN) || (signal == SYSCALL_SIGTTOU);
}

/* The stop signals, as one mask, for the discarding SIGCONT performs. */
static uint32_t SignalStopMask(void)
{
    return SignalBit(SYSCALL_SIGSTOP) | SignalBit(SYSCALL_SIGTSTP) |
           SignalBit(SYSCALL_SIGTTIN) | SignalBit(SYSCALL_SIGTTOU);
}

/* ---------------------------------------------------------- the actions */

SignalAction SignalDefaultAction(uint32_t signal)
{
    switch (signal)
    {
    case SYSCALL_SIGCHLD:
        return SIGNAL_ACTION_IGNORE;
    case SYSCALL_SIGCONT:
        return SIGNAL_ACTION_CONTINUE;
    case SYSCALL_SIGSTOP:
    case SYSCALL_SIGTSTP:
    case SYSCALL_SIGTTIN:
    case SYSCALL_SIGTTOU:
        return SIGNAL_ACTION_STOP;
    default:
        return SIGNAL_ACTION_TERMINATE;
    }
}

uint32_t SignalFromVector(uint64_t vector)
{
    switch (vector)
    {
    case SIGNAL_VECTOR_DIVIDE_ERROR:
    case SIGNAL_VECTOR_SIMD_FLOATING:
        return SYSCALL_SIGFPE;
    case SIGNAL_VECTOR_DEBUG:
    case SIGNAL_VECTOR_BREAKPOINT:
        return SYSCALL_SIGTRAP;
    case SIGNAL_VECTOR_OVERFLOW:
    case SIGNAL_VECTOR_INVALID_OPCODE:
        return SYSCALL_SIGILL;
    case SIGNAL_VECTOR_ALIGNMENT_CHECK:
        return SYSCALL_SIGBUS;
    default:
        return SYSCALL_SIGSEGV;
    }
}

/* --------------------------------------------------------- dispositions */

uint64_t SignalDisposition(const Process *process, uint32_t signal)
{
    if ((process == NULL) || !SignalNumberIsValid(signal))
    {
        return SYSCALL_SIGNAL_DEFAULT;
    }

    return process->handlers[signal];
}

bool SignalSetDisposition(Process *process, uint32_t signal, uint64_t disposition,
                          uint64_t restorer, uint64_t *previous)
{
    if ((process == NULL) || !process->used || !SignalNumberIsValid(signal))
    {
        return false;
    }

    /* The two the standard withholds from a program, so that a process can
     * always be ended and always be stopped by whoever governs it. */
    if ((signal == SYSCALL_SIGKILL) || (signal == SYSCALL_SIGSTOP))
    {
        return false;
    }

    if (previous != NULL)
    {
        *previous = process->handlers[signal];
    }

    PerCpuPushInterruptState();

    process->handlers[signal] = disposition;

    if (disposition != SYSCALL_SIGNAL_DEFAULT)
    {
        process->restorer = restorer;
    }

    /* A signal set to be ignored is discarded if it was pending: the standard
     * leaves that unspecified, and discarding is what makes "ignore SIGINT"
     * mean the control-C already typed does nothing. */
    if (disposition == SYSCALL_SIGNAL_IGNORE)
    {
        process->pending &= ~SignalBit(signal);
    }

    PerCpuPopInterruptState();

    return true;
}

void SignalResetForExecute(Process *process)
{
    if (process == NULL)
    {
        return;
    }

    for (uint32_t signal = 1U; signal <= SYSCALL_SIGNAL_MAXIMUM; ++signal)
    {
        if (process->handlers[signal] != SYSCALL_SIGNAL_IGNORE)
        {
            process->handlers[signal] = SYSCALL_SIGNAL_DEFAULT;
        }
    }

    process->restorer = 0U;
}

/* ------------------------------------------------------------- sending */

bool SignalSend(Process *process, uint32_t signal)
{
    Thread *thread;

    if ((process == NULL) || !process->used || (process->state == PROCESS_EXITED) ||
        (process->state == PROCESS_UNUSED))
    {
        return false;
    }

    if (signal == 0U)
    {
        return true;
    }

    if (!SignalNumberIsValid(signal))
    {
        return false;
    }

    ++SignalSent;

    PerCpuPushInterruptState();

    /*
     * Discarded at generation where the process ignores it, whether by its
     * own disposition or by the default — SIGCHLD to a process that has not
     * asked for it, SIGCONT to one that is not stopped — so that a sleeping
     * call is not woken for a signal that would do nothing on the way out.
     * SIGCONT to a stopped process is the exception, and is the whole of what
     * continuing is.
     */
    if (process->handlers[signal] == SYSCALL_SIGNAL_IGNORE)
    {
        PerCpuPopInterruptState();

        return true;
    }

    if ((process->handlers[signal] == SYSCALL_SIGNAL_DEFAULT) &&
        (SignalDefaultAction(signal) == SIGNAL_ACTION_IGNORE))
    {
        PerCpuPopInterruptState();

        return true;
    }

    if (signal == SYSCALL_SIGCONT)
    {
        /* A pending stop is discarded by a continue, and the reverse below,
         * as Section 2.4.1 of the standard has it. */
        process->pending &= ~SignalStopMask();

        if (process->state == PROCESS_STOPPED)
        {
            PerCpuPopInterruptState();
            SignalContinue(process);

            /* A handler for SIGCONT, where one is set, runs on the way out
             * of the sleep the stop was; the bit is left for it. */
            if (process->handlers[signal] != SYSCALL_SIGNAL_DEFAULT)
            {
                process->pending |= SignalBit(signal);
            }

            return true;
        }

        if (process->handlers[signal] == SYSCALL_SIGNAL_DEFAULT)
        {
            PerCpuPopInterruptState();

            return true;
        }
    }

    if (SignalIsStop(signal))
    {
        process->pending &= ~SignalBit(SYSCALL_SIGCONT);
    }

    process->pending |= SignalBit(signal);

    /*
     * A stopped process is left asleep by everything but SIGKILL and SIGCONT
     * — the signal waits for the continue — and any other process asleep in
     * a call is woken so that the call reports EINTR and the way out is
     * reached. A process that is running or queued needs nothing: its next
     * way out of the kernel finds the bit.
     */
    thread = (process->thread_count > 0U) ? process->threads[0] : NULL;

    if (process->state == PROCESS_STOPPED)
    {
        if (signal == SYSCALL_SIGKILL)
        {
            PerCpuPopInterruptState();
            SignalContinue(process);

            return true;
        }
    }
    else if (thread != NULL)
    {
        (void)SchedulerWakeThread(thread);
    }

    PerCpuPopInterruptState();

    return true;
}

size_t SignalSendGroup(uint64_t group, uint32_t signal)
{
    size_t reached = 0U;

    if (group == 0U)
    {
        return 0U;
    }

    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        Process *const process = ProcessAt(index);

        if ((process == NULL) || !process->used || (process->group != group) ||
            (process->state == PROCESS_EXITED))
        {
            continue;
        }

        if (SignalSend(process, signal))
        {
            ++reached;
        }
    }

    return reached;
}

bool SignalIsPending(const Process *process)
{
    return (process != NULL) && (process->pending != 0U);
}

uint32_t SignalTake(Process *process)
{
    uint32_t taken = 0U;

    if ((process == NULL) || (process->pending == 0U))
    {
        return 0U;
    }

    PerCpuPushInterruptState();

    for (uint32_t signal = 1U; signal <= SYSCALL_SIGNAL_MAXIMUM; ++signal)
    {
        if ((process->pending & SignalBit(signal)) != 0U)
        {
            process->pending &= ~SignalBit(signal);
            taken = signal;
            break;
        }
    }

    PerCpuPopInterruptState();

    if ((taken != 0U) && (process->handlers[taken] != SYSCALL_SIGNAL_DEFAULT) &&
        (process->handlers[taken] != SYSCALL_SIGNAL_IGNORE))
    {
        ++SignalHandled;
    }

    return taken;
}

/* ---------------------------------------------------- stop and continue */

void SignalStopCurrent(Process *process, uint32_t signal)
{
    Process *parent;

    if ((process == NULL) || !process->used)
    {
        return;
    }

    ++SignalStops;

    PerCpuPushInterruptState();

    process->state = PROCESS_STOPPED;
    process->stop_signal = signal;
    process->stop_reported = false;
    process->wait_status = SYSCALL_STATUS_MAKE(SYSCALL_STATUS_KIND_STOPPED, signal);

    /*
     * The parent is told twice over: its wait channel is woken, so that a
     * `waitpid` asleep with SYSCALL_WAIT_UNTRACED reports the stop, and it is
     * sent SIGCHLD, which it ignores unless it asked. The shell is the parent
     * that asks, and what it does with the answer is `[1]+ Stopped`.
     */
    parent = ProcessById(process->parent_id);

    if (parent != NULL)
    {
        (void)SchedulerWake(parent);
        (void)SignalSend(parent, SYSCALL_SIGCHLD);
    }

    /*
     * And the process sleeps upon its own stop channel until SignalContinue
     * wakes it. The state is tested in a loop because a wake of the channel
     * is broadcast and the thread may be woken for a signal sent to it while
     * stopped — which waits, as SignalSend arranges, for the continue.
     */
    while (process->state == PROCESS_STOPPED)
    {
        SchedulerSleep(&process->stop_channel);
    }

    PerCpuPopInterruptState();
}

void SignalContinue(Process *process)
{
    if ((process == NULL) || !process->used || (process->state != PROCESS_STOPPED))
    {
        return;
    }

    ++SignalContinues;

    PerCpuPushInterruptState();

    process->state = PROCESS_READY;
    process->stop_signal = 0U;
    process->stop_reported = false;
    (void)SchedulerWake(&process->stop_channel);

    PerCpuPopInterruptState();
}

/* -------------------------------------------------------------- accounting */

uint64_t SignalSentCount(void)
{
    return SignalSent;
}

uint64_t SignalHandledCount(void)
{
    return SignalHandled;
}

void SignalReport(void)
{
    KernelWriteString("Signals: ");
    KernelWriteDecimal(SignalSent);
    KernelWriteString(" sent, ");
    KernelWriteDecimal(SignalHandled);
    KernelWriteString(" taken by a handler, ");
    KernelWriteDecimal(SignalStops);
    KernelWriteString(" stop(s), ");
    KernelWriteDecimal(SignalContinues);
    KernelWriteString(" continue(s).\n");
}
