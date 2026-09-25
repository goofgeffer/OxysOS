/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/processes.c
 * Purpose: The first half of Phase 6: the privilege transition and the
 *          system calls, the ELF loader, processes and threads, the context
 *          switch and the descent to privilege level 3; and the serial line
 *          moved onto interrupts.
 * Key functions: KernelInitialiseProcesses.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order that fixes
 *     where this phase stands in KernelMain, and the order within it.
 *
 * Moved out of kernel/kernel.c on 2026-09-25, unchanged in order, when
 * KernelMain was reduced to the driver that calls one function per phase;
 * kernel/init/internal.h says why.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/dev/vga.h>
#include <oxys/dev/serial.h>
#include <oxys/proc/process.h>
void KernelInitialiseProcesses(void)
{
    /*
     * The apparatus of a privilege transition, and the system calls that will
     * arrive through it.
     *
     * The first asserts the configuration — the descriptors, the task state
     * segment's stacks, and the three registers that decide where SYSCALL goes
     * and what it clears. It executed SYSCALL until sub-task 6.7 and no longer
     * does; the reason is recorded where the routine that did it stood.
     *
     * The second asserts the dispatch and the validation of a caller's
     * arguments, which need no transition: the dispatcher is an ordinary
     * function of an ordinary structure. It composes a page accessible to
     * privilege level 3 in order to have something real to validate against, and
     * takes it away again, so it runs after the frame allocator and the paging
     * hierarchy are both established and asserted.
     */
    KernelVerifyPrivilege();
    KernelVerifySyscall();

    /*
     * The loader that will turn a file into a program. It composes an image in
     * memory and an address space to put it in, so it needs the frame allocator,
     * the paging hierarchy and the address spaces of Phase 2 — all of which are
     * established and asserted well above this line — and nothing of Phase 4.
     */
    KernelVerifyElf();

    /*
     * And the structures a loaded program will be held in. They are established
     * here rather than beside the memory manager because a process owns an
     * address space and a thread owns a kernel stack taken from the arena, so
     * both of those must exist and have been asserted first — and neither the
     * loader nor these structures needs anything of Phase 4.
     */
    ProcessInitialise();
    KernelVerifyProcess();

    /*
     * And the two transfers of sub-task 6.10: one thread exchanged for another,
     * and the first descent to privilege level 3.
     *
     * The switch is asserted first and alone, between two threads of the kernel,
     * because a failure there is a failure of the switch — where a failure in
     * the descent could be a failure of the switch, the loader, the address
     * space, the system call path or the exception dispositions, all of which
     * the descent puts together at once.
     *
     * Both run with the interrupt flag set, which is where it stands by this
     * point: a program that could not be interrupted could not be pre-empted,
     * and the descent sets the flag in the program's own RFLAGS regardless.
     */
    KernelVerifyContextSwitch();
    KernelVerifyUserMode();
    ProcessReport();

    /*
     * Phase 4 begins here. The serial adapter was configured in the first
     * instruction of this function, so that a failure anywhere above would be
     * recorded; only now, the interrupt controller existing, can it be promoted
     * from polling to interrupts and become a driver rather than a routine.
     */
    SerialActivateInterrupts();
    KernelVerifySerial();
    SerialReport();
    VgaReport();
}
