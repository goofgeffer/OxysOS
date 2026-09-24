/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/proc/signal.h
 * Purpose: Declares the signals of sub-task 8.7 — how one is sent to a process
 *          or a process group, how a pending one is chosen for delivery, what
 *          its default action is, and how a process stops and is continued —
 *          apart from the frame a handler is entered upon, which is the
 *          architecture's and stands in kernel/arch/x86_64/syscall/sigframe.c.
 * Key definitions: SignalAction, SignalSend, SignalSendGroup, SignalIsPending,
 *          SignalTake, SignalDefaultAction, SignalSetDisposition,
 *          SignalDisposition, SignalResetForExecute, SignalStopCurrent,
 *          SignalContinue, SignalFromVector, SignalCount, SignalReport.
 * References:
 *   - IEEE Std 1003.1-2017, System Interfaces, Section 2.4 (Signal Concepts):
 *     generation and delivery, that a signal ignored at generation is
 *     discarded, that SIGKILL and SIGSTOP cannot be caught, blocked or
 *     ignored, that a stop signal discards a pending SIGCONT and SIGCONT a
 *     pending stop, and the default action of each signal.
 *   - IEEE Std 1003.1-2017, `kill()`: a `pid` of -1 or below names a group;
 *     a signal of 0 checks for existence and sends nothing.
 *   - IEEE Std 1003.1-2017, `sigaction()` and `signal()`: SIG_DFL and SIG_IGN,
 *     and that a handler is reset to the default by `exec` while an ignored
 *     signal stays ignored.
 *   - docs/design/PROCESS.md: the design, and what a delivered
 *     signal does to the process it reaches.
 *
 * What a signal is here.
 *
 *   A bit in the target process, set by the sender and cleared by the target
 *   itself on its next way out of the kernel — a system call returning, or an
 *   interrupt taken at privilege level 3 — which is the one moment the target's
 *   registers are all in a frame that can be edited. A sender never touches
 *   the target's context: it sets the bit and, where the target is asleep in a
 *   call, wakes it, and the call reports EINTR so that the target reaches the
 *   way out. Sixteen of the thirty-one numbers are given a meaning; the rest
 *   terminate, which is what IEEE Std 1003.1-2017 makes the default for a
 *   signal it does not name otherwise.
 */

#ifndef OXYS_PROC_SIGNAL_H
#define OXYS_PROC_SIGNAL_H

#include <oxys/types.h>
#include <oxys/proc/process.h>

/* What delivering a signal does where no handler is set. */
typedef enum SignalAction
{
    SIGNAL_ACTION_TERMINATE = 0,
    SIGNAL_ACTION_IGNORE,
    SIGNAL_ACTION_STOP,
    SIGNAL_ACTION_CONTINUE
} SignalAction;

/* The default action of a signal number; TERMINATE for one outside the set. */
SignalAction SignalDefaultAction(uint32_t signal);

/*
 * Sends a signal to a process: sets its bit, unless the process ignores it —
 * an ignored signal is discarded at generation, as the standard has it — and
 * wakes the process where it is asleep in a call, so that the call reports
 * EINTR and the signal is delivered on the way out. SIGCONT continues a stopped
 * process at once and discards any pending stop; a stop signal discards a
 * pending SIGCONT. SIGKILL reaches a stopped process; nothing else does until
 * it is continued. A signal of 0 sends nothing and reports whether the process
 * is one a signal could reach. Returns false where the process is unusable.
 */
bool SignalSend(Process *process, uint32_t signal);

/* Sends a signal to every process of a group. Returns how many it reached. */
size_t SignalSendGroup(uint64_t group, uint32_t signal);

/* Whether any signal is pending upon a process. It is what a sleeping call
 * asks after it is woken, to decide between its result and EINTR. */
bool SignalIsPending(const Process *process);

/*
 * Takes the lowest-numbered pending signal off the process and reports it, or
 * 0 where none is pending. The delivery path calls it repeatedly until 0.
 */
uint32_t SignalTake(Process *process);

/*
 * Sets a signal's disposition — SYSCALL_SIGNAL_DEFAULT, SYSCALL_SIGNAL_IGNORE
 * or a handler's address — and the restorer every handler returns through,
 * reporting the disposition that stood before. Refuses SIGKILL and SIGSTOP,
 * and a number outside 1 to SYSCALL_SIGNAL_MAXIMUM, with false. Setting a
 * signal to ignore discards a pending instance of it.
 */
bool SignalSetDisposition(Process *process, uint32_t signal, uint64_t disposition,
                          uint64_t restorer, uint64_t *previous);
uint64_t SignalDisposition(const Process *process, uint32_t signal);

/* What `execve` does to the dispositions: a handler goes back to the default,
 * an ignored signal stays ignored, and nothing pending is discarded. */
void SignalResetForExecute(Process *process);

/*
 * Stops the calling process upon `signal`: marks it stopped, tells its parent
 * — a wake of the parent's wait channel, and SIGCHLD — and sleeps until
 * SignalContinue. Called from the delivery path, upon the process's own
 * thread, and returns when the process has been continued.
 */
void SignalStopCurrent(Process *process, uint32_t signal);

/* Continues a stopped process: SIGCONT's action, and what `fg` and `bg` cause. */
void SignalContinue(Process *process);

/* The signal a fault at privilege level 3 is reported as: SIGSEGV for a page
 * or general protection fault, SIGILL, SIGFPE, SIGBUS and SIGTRAP for the
 * exceptions that correspond, SIGSEGV for the rest. */
uint32_t SignalFromVector(uint64_t vector);

/* How many signals have been sent, and how many delivered to a handler. */
uint64_t SignalSentCount(void);
uint64_t SignalHandledCount(void);

/* Emits a summary upon the console and the serial port. */
void SignalReport(void);

#endif /* OXYS_PROC_SIGNAL_H */
