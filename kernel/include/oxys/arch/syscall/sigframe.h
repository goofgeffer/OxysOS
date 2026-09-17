/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/arch/syscall/sigframe.h
 * Purpose: Declares the architecture's half of signal delivery, of sub-task
 *          8.7: the frame a handler is entered upon, built from either of the
 *          two frames a program's registers stand in when it is inside the
 *          kernel, and the return through it.
 * Key definitions: SignalContext, SignalDeliverToSyscallFrame,
 *          SignalDeliverToTrapFrame, SignalReturn.
 * References:
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.2:
 *     the red zone of 128 bytes below the stack pointer that a leaf function
 *     may use without moving the pointer, which a frame pushed by the kernel
 *     must not overwrite; and the alignment a function is entered with, the
 *     stack pointer eight modulo sixteen after the return address is pushed.
 *   - Intel SDM, Volume 2B, "SYSRET": RIP is loaded from RCX and RFLAGS from
 *     R11, which is why a return through a system call frame edits those two.
 *   - Intel SDM, Volume 3A, Section 6.12.1: the frame IRETQ returns through,
 *     which is why a return through an interrupt frame edits RIP, RSP and
 *     RFLAGS directly.
 *   - docs/design/PROCESS.md, Section 18.3.
 */

#ifndef OXYS_ARCH_SYSCALL_SIGFRAME_H
#define OXYS_ARCH_SYSCALL_SIGFRAME_H

#include <oxys/types.h>
#include <oxys/arch/syscall/syscall.h>
#include <oxys/arch/interrupt/interrupts.h>

/*
 * What stands upon the program's stack while a handler runs, at the stack
 * pointer the handler is entered with: the restorer's address first, where a
 * return address belongs, so that the handler's `ret` enters the restorer;
 * then the signal; then the interrupted context, which `sigreturn` puts back.
 * The layout is the C library's to know as well — it does not read it, the
 * restorer being two instructions — and the kernel's to write, and it is
 * stated here so that there is one statement.
 */
typedef struct SignalContext
{
    uint64_t restorer;
    uint64_t signal;
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
} SignalContext;

/*
 * Delivers every signal pending upon the current process, on its way out of
 * the kernel through the frame given: an ignored one is discarded, a stop
 * stops the process here and returns when it is continued, a termination does
 * not return, and a handler is entered by editing the frame so that the
 * program resumes in the handler rather than where it was — one handler per
 * way out, the rest waiting for the next. `number` is the call the system call
 * frame arrived with: a call that reported EINTR and whose signal entered no
 * handler is restarted, by the instruction pointer put back to the SYSCALL.
 */
void SignalDeliverToSyscallFrame(SyscallFrame *frame, uint64_t number);
void SignalDeliverToTrapFrame(TrapFrame *frame);

/*
 * The `sigreturn` call: puts the interrupted context back from the frame the
 * restorer stands upon, into the system call frame the restorer's own call
 * arrived through. Returns SYSCALL_EFAULT where the frame cannot be read,
 * which ends the program by the same signal a fault would.
 */
int64_t SignalReturn(SyscallFrame *frame);

#endif /* OXYS_ARCH_SYSCALL_SIGFRAME_H */
