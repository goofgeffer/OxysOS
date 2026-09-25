/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/interrupts.c
 * Purpose: Phase 3: the descriptor tables, the task state segment and its
 *          interrupt stacks, the system call entry, the exceptions, and the
 *          8259A request layer; and the copy-on-write and address-space tests
 *          of Phase 2 that needed the fault handler.
 * Key functions: KernelInitialiseInterrupts.
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
#include <oxys/arch/cpu/gdt.h>
#include <oxys/arch/cpu/tss.h>
#include <oxys/arch/syscall/syscall.h>
#include <oxys/arch/cpu/idt.h>
#include <oxys/arch/interrupt/interrupts.h>
#include <oxys/arch/interrupt/exceptions.h>
#include <oxys/dev/pic.h>
#include <oxys/arch/interrupt/irq.h>
void KernelInitialiseInterrupts(void)
{
    /*
     * The table established by boot/boot.asm resides at a low address that
     * sub-task 2.3 unmapped. It must be replaced before any interrupt gate is
     * installed, because delivering an interrupt obliges the processor to read
     * the descriptor named by the gate's selector.
     */
    GdtInitialise();
    GdtReport();

    IdtInitialise();
    InterruptInitialise();
    ExceptionInitialise();

    /*
     * Phase 6, sub-task 6.1. The apparatus of a privilege transition: the
     * user-mode descriptors, the task state segment that names the stacks the
     * processor loads, and the three registers that configure SYSCALL.
     *
     * It is established here, after the gates exist, and not beside the global
     * descriptor table it extends. LTR reads the descriptor this builds and
     * raises a general-protection exception where it is malformed; done before
     * the interrupt descriptor table existed, that exception would have found no
     * gate and escalated to a reset, and the diagnosis would have been a machine
     * that reboots. Done here it is reported.
     */
    TssInitialise();

    if (!ExceptionInstallInterruptStacks())
    {
        KernelPanic("The double fault could not be given a stack of its own.");
    }

    /*
     * A processor that cannot report SYSCALL cannot run a user program at all,
     * every one capable of long mode supporting it. The kernel proceeds so that
     * the machine may still be examined, and the report and the self-test both
     * state the absence.
     */
    (void)SyscallInitialise();

    /*
     * The global descriptor table reports itself above, at its initialisation;
     * every field of that report is a constant of the table's layout and none of
     * it changes when the task state segment descriptor is filled in, so it is
     * not repeated here.
     */
    TssReport();
    SyscallReport();
    IdtReport();
    InterruptReport();
    KernelVerifyIdt();
    KernelVerifyInterruptStubs();
    KernelVerifyDispatcher();
    KernelVerifyExceptions();
    KernelVerifyCopyOnWrite();
    KernelVerifyAddressSpaces();

    /*
     * The controllers are remapped only now, after the exception handlers exist.
     * Remapping them earlier would have placed device vectors clear of the
     * exceptions without providing anywhere for them to go.
     *
     * IrqInitialise performs the remapping and installs the routing above it. The
     * 8259A pair is what answers here and for the whole of the initialisation
     * that follows; sub-task 6.12 retires it in favour of the APIC only once
     * every driver has claimed the line it wants, which is after the serial
     * adapter is promoted to interrupts below.
     */
    IrqInitialise();
    KernelVerifyPic();
    KernelVerifyIrq();
    PicReport();
    IrqReport();
}
