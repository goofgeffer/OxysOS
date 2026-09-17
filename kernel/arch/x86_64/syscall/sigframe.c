/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/syscall/sigframe.c
 * Purpose: Implements the architecture's half of signal delivery, of sub-task
 *          8.7: the pending signals of the current process acted upon on its
 *          way out of the kernel — through a system call frame or an interrupt
 *          frame — and, for a handler, the context saved upon the program's
 *          stack, the entry into the handler, and the return through
 *          `sigreturn` that puts the context back.
 * Key functions: SignalDeliverToSyscallFrame, SignalDeliverToTrapFrame,
 *          SignalReturn, SignalDeliver, SignalEnterHandler.
 * References:
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.2:
 *     the 128-byte red zone below the stack pointer, and the alignment of the
 *     stack pointer at a function's entry — sixteen bytes before the return
 *     address is pushed, so eight modulo sixteen after; Section 3.2.3: the
 *     first integer argument in RDI.
 *   - Intel SDM, Volume 2B, "SYSRET": RIP from RCX, RFLAGS from R11 — so a
 *     context restored into a system call frame is restored into those two
 *     registers, and a context saved from one is read from them.
 *   - Intel SDM, Volume 3A, Section 6.12.1 and Figure 6-8: the interrupt frame
 *     IRETQ returns through, RIP, RFLAGS and RSP explicit within it.
 *   - Intel SDM, Volume 3A, Section 2.3, EFLAGS: which flags a program at
 *     privilege level 3 may alter — the status flags, DF and TF — and which it
 *     may not, IOPL and IF among them, which is why a restored RFLAGS is
 *     masked rather than trusted.
 *   - IEEE Std 1003.1-2017, `sigaction()`: a handler receives the signal
 *     number as its argument and, upon returning, the interrupted program
 *     resumes; docs/design/PROCESS.md, Section 18.3.
 *
 * Why there are two frames and one delivery.
 *
 *   A program's registers stand in a SyscallFrame while it is inside a
 *   system call and in a TrapFrame while it is inside an interrupt taken at
 *   privilege level 3, and the two are laid out by two different entry paths
 *   for two different returns. Delivery does not care which: it wants the
 *   instruction pointer, the stack pointer, the flags and the fifteen general
 *   registers, and it edits three of them. So each frame is read into one
 *   register set, delivery is done upon that, and the set is written back —
 *   two adapters of a dozen lines apiece, and one delivery, rather than one
 *   delivery written twice and differing the first time it is corrected.
 *
 * Why the flags a program hands back are masked.
 *
 *   `sigreturn` restores RFLAGS from a frame the program could have edited,
 *   and SYSRET loads RFLAGS from R11 as it stands. A program that wrote IOPL
 *   3 there would return to privilege level 3 permitted to execute CLI, and
 *   one that cleared IF would return with interrupts masked and the machine
 *   would stop pre-empting it. Only the flags Intel SDM permits privilege
 *   level 3 to alter are taken from the frame; IF is forced set.
 */

#include <oxys/arch/syscall/sigframe.h>
#include <oxys/arch/syscall/syscall.h>
#include <oxys/proc/process.h>
#include <oxys/proc/signal.h>
#include <oxys/kernel.h>

/* The red zone a frame is pushed below, and the alignment. */
#define SIGNAL_RED_ZONE_BYTES  128U
#define SIGNAL_STACK_ALIGNMENT 16U

/* The flags a program at privilege level 3 may alter: CF, PF, AF, ZF, SF, TF,
 * DF and OF. Everything else is the kernel's. */
#define SIGNAL_USER_FLAGS                                                        \
    (RFLAGS_CARRY | RFLAGS_PARITY | RFLAGS_AUXILIARY | RFLAGS_ZERO | RFLAGS_SIGN | \
     RFLAGS_TRAP | RFLAGS_DIRECTION | RFLAGS_OVERFLOW)

/* The one register set delivery works upon. */
typedef struct SignalRegisters
{
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} SignalRegisters;

/* Accounting. */
static uint64_t SignalFramesBuilt;
static uint64_t SignalFramesRestored;
static uint64_t SignalFramesRefused;
static uint64_t SignalCallsRestarted;

/* ---------------------------------------------------------- the adapters */

static void SignalRegistersFromSyscallFrame(const SyscallFrame *frame, SignalRegisters *r)
{
    r->rip = frame->rcx;
    r->rsp = frame->user_stack;
    r->rflags = frame->r11;
    r->rax = frame->rax;
    r->rbx = frame->rbx;
    r->rcx = frame->rcx;
    r->rdx = frame->rdx;
    r->rsi = frame->rsi;
    r->rdi = frame->rdi;
    r->rbp = frame->rbp;
    r->r8 = frame->r8;
    r->r9 = frame->r9;
    r->r10 = frame->r10;
    r->r11 = frame->r11;
    r->r12 = frame->r12;
    r->r13 = frame->r13;
    r->r14 = frame->r14;
    r->r15 = frame->r15;
}

static void SignalRegistersToSyscallFrame(const SignalRegisters *r, SyscallFrame *frame)
{
    frame->rcx = r->rip;
    frame->user_stack = r->rsp;
    frame->r11 = r->rflags;
    frame->rax = r->rax;
    frame->rbx = r->rbx;
    frame->rdx = r->rdx;
    frame->rsi = r->rsi;
    frame->rdi = r->rdi;
    frame->rbp = r->rbp;
    frame->r8 = r->r8;
    frame->r9 = r->r9;
    frame->r10 = r->r10;
    frame->r12 = r->r12;
    frame->r13 = r->r13;
    frame->r14 = r->r14;
    frame->r15 = r->r15;
}

static void SignalRegistersFromTrapFrame(const TrapFrame *frame, SignalRegisters *r)
{
    r->rip = frame->rip;
    r->rsp = frame->rsp;
    r->rflags = frame->rflags;
    r->rax = frame->rax;
    r->rbx = frame->rbx;
    r->rcx = frame->rcx;
    r->rdx = frame->rdx;
    r->rsi = frame->rsi;
    r->rdi = frame->rdi;
    r->rbp = frame->rbp;
    r->r8 = frame->r8;
    r->r9 = frame->r9;
    r->r10 = frame->r10;
    r->r11 = frame->r11;
    r->r12 = frame->r12;
    r->r13 = frame->r13;
    r->r14 = frame->r14;
    r->r15 = frame->r15;
}

static void SignalRegistersToTrapFrame(const SignalRegisters *r, TrapFrame *frame)
{
    frame->rip = r->rip;
    frame->rsp = r->rsp;
    frame->rflags = r->rflags;
    frame->rax = r->rax;
    frame->rbx = r->rbx;
    frame->rcx = r->rcx;
    frame->rdx = r->rdx;
    frame->rsi = r->rsi;
    frame->rdi = r->rdi;
    frame->rbp = r->rbp;
    frame->r8 = r->r8;
    frame->r9 = r->r9;
    frame->r10 = r->r10;
    frame->r11 = r->r11;
    frame->r12 = r->r12;
    frame->r13 = r->r13;
    frame->r14 = r->r14;
    frame->r15 = r->r15;
}

/* -------------------------------------------------------------- delivery */

/* Ends the current process as `signal` ends one. Does not return. */
static void SignalTerminate(Process *process, uint32_t signal)
{
    process->termination_signal = signal;
    ProcessExit(-(int64_t)(128U + signal));
}

/*
 * Enters a handler: saves the register set upon the program's stack, below
 * the red zone and aligned as a function is entered, and points the set at
 * the handler with the signal as its argument and the restorer where the
 * return address belongs. Returns false where the stack cannot take the
 * frame, in which case the caller ends the program: a handler that could not
 * be entered would otherwise be a signal silently dropped, and a program
 * whose stack is gone is not one that can be told.
 */
static bool SignalEnterHandler(Process *process, uint32_t signal, uint64_t handler,
                               SignalRegisters *r)
{
    uint64_t base;
    SignalContext *context;

    base = r->rsp - SIGNAL_RED_ZONE_BYTES - (uint64_t)sizeof(SignalContext);
    base &= ~((uint64_t)SIGNAL_STACK_ALIGNMENT - 1U);
    base -= 8U;

    if (!SyscallUserRangeIsWritable(base, (uint64_t)sizeof(SignalContext)))
    {
        ++SignalFramesRefused;

        return false;
    }

    context = (SignalContext *)(uintptr_t)base;
    context->restorer = process->restorer;
    context->signal = signal;
    context->rip = r->rip;
    context->rsp = r->rsp;
    context->rflags = r->rflags;
    context->rax = r->rax;
    context->rbx = r->rbx;
    context->rcx = r->rcx;
    context->rdx = r->rdx;
    context->rsi = r->rsi;
    context->rdi = r->rdi;
    context->rbp = r->rbp;
    context->r8 = r->r8;
    context->r9 = r->r9;
    context->r10 = r->r10;
    context->r11 = r->r11;
    context->r12 = r->r12;
    context->r13 = r->r13;
    context->r14 = r->r14;
    context->r15 = r->r15;

    r->rip = handler;
    r->rsp = base;
    r->rdi = signal;
    /* The direction flag is clear at a function's entry, by the convention,
     * whatever the interrupted code had set it to. */
    r->rflags &= ~RFLAGS_DIRECTION;

    ++SignalFramesBuilt;

    return true;
}

/*
 * The delivery itself: every pending signal in turn, lowest first, until one
 * enters a handler — after which the program must run the handler before
 * anything else is delivered — or none is left.
 */
static void SignalDeliver(SignalRegisters *r)
{
    Process *const process = ProcessCurrent();

    if (process == NULL)
    {
        return;
    }

    for (;;)
    {
        const uint32_t signal = SignalTake(process);
        uint64_t disposition;

        if (signal == 0U)
        {
            return;
        }

        disposition = SignalDisposition(process, signal);

        if (disposition == SYSCALL_SIGNAL_IGNORE)
        {
            continue;
        }

        if (disposition == SYSCALL_SIGNAL_DEFAULT)
        {
            switch (SignalDefaultAction(signal))
            {
            case SIGNAL_ACTION_IGNORE:
            case SIGNAL_ACTION_CONTINUE:
                /* Continuing was done when the signal was sent; here it is
                 * nothing. */
                continue;

            case SIGNAL_ACTION_STOP:
                /* Returns when the process has been continued, upon this same
                 * way out; whatever was sent meanwhile is pending and is taken
                 * next. */
                SignalStopCurrent(process, signal);
                continue;

            case SIGNAL_ACTION_TERMINATE:
            default:
                SignalTerminate(process, signal);
                return;
            }
        }

        if (!SignalEnterHandler(process, signal, disposition, r))
        {
            SignalTerminate(process, SYSCALL_SIGSEGV);
        }

        return;
    }
}

void SignalDeliverToSyscallFrame(SyscallFrame *frame, uint64_t number)
{
    SignalRegisters r;
    uint64_t resumed_at;
    bool interrupted;

    if ((frame == NULL) || !SignalIsPending(ProcessCurrent()))
    {
        return;
    }

    /*
     * A call that slept and was woken by the signal reported EINTR. Where the
     * signal turns out to have entered no handler — it was ignored, or it
     * stopped the process and the process has since been continued — the call
     * is made again rather than the program being told EINTR for a signal it
     * never saw: the instruction pointer goes back two bytes to the SYSCALL
     * instruction and RAX back to the number, and the program re-executes
     * the call as though nothing had happened. That is what lets `cat`,
     * stopped by control-Z at the terminal and continued by `fg`, go on
     * reading. A call interrupted by a signal that did enter a handler
     * returns EINTR, as IEEE Std 1003.1-2017 has it without SA_RESTART, and
     * `sigreturn` is never restarted, its RAX being the interrupted program's.
     */
    interrupted = (frame->rax == (uint64_t)SYSCALL_EINTR) && (number != SYSCALL_SIGRETURN);

    SignalRegistersFromSyscallFrame(frame, &r);
    resumed_at = r.rip;
    SignalDeliver(&r);
    SignalRegistersToSyscallFrame(&r, frame);

    if (interrupted && (r.rip == resumed_at))
    {
        frame->rax = number;
        frame->rcx -= 2U;
        ++SignalCallsRestarted;
    }
}

void SignalDeliverToTrapFrame(TrapFrame *frame)
{
    SignalRegisters r;

    if ((frame == NULL) || ((frame->cs & 3U) == 0U) || !SignalIsPending(ProcessCurrent()))
    {
        return;
    }

    SignalRegistersFromTrapFrame(frame, &r);
    SignalDeliver(&r);
    SignalRegistersToTrapFrame(&r, frame);
}

/* ---------------------------------------------------------------- return */

int64_t SignalReturn(SyscallFrame *frame)
{
    const SignalContext *context;
    uint64_t address;

    if (frame == NULL)
    {
        return SYSCALL_EINVAL;
    }

    /*
     * The handler's `ret` popped the restorer's address, so the restorer's
     * own `syscall` arrives with the stack pointer at the signal field, eight
     * bytes above the frame's start.
     */
    address = frame->user_stack - 8U;

    if (!SyscallUserRangeIsReadable(address, (uint64_t)sizeof(SignalContext)))
    {
        ++SignalFramesRefused;

        return SYSCALL_EFAULT;
    }

    context = (const SignalContext *)(uintptr_t)address;

    /* A program may not return to an address the kernel would then execute
     * from, and the range check above says nothing about the target. */
    if (context->rip >= SYSCALL_USER_LIMIT)
    {
        ++SignalFramesRefused;

        return SYSCALL_EFAULT;
    }

    frame->rcx = context->rip;
    frame->user_stack = context->rsp;
    frame->r11 = (context->rflags & SIGNAL_USER_FLAGS) | RFLAGS_INTERRUPT_ENABLE |
                 UINT64_C(0x2);
    frame->rax = context->rax;
    frame->rbx = context->rbx;
    frame->rdx = context->rdx;
    frame->rsi = context->rsi;
    frame->rdi = context->rdi;
    frame->rbp = context->rbp;
    frame->r8 = context->r8;
    frame->r9 = context->r9;
    frame->r10 = context->r10;
    frame->r12 = context->r12;
    frame->r13 = context->r13;
    frame->r14 = context->r14;
    frame->r15 = context->r15;

    ++SignalFramesRestored;

    /* The result is the interrupted RAX, already in the frame; what this
     * returns is discarded by the dispatcher for this one call. */
    return (int64_t)context->rax;
}
