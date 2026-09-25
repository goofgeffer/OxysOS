/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/processors.c
 * Purpose: Sub-tasks 6.12 to 6.15: the firmware tables and the two APICs, the
 *          per-processor facilities, the scheduler, and the application
 *          processors started upon it.
 * Key functions: KernelInitialiseProcessors.
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
#include <oxys/arch/cpu/percpu.h>
#include <oxys/arch/smp/ipi.h>
#include <oxys/arch/mm/shootdown.h>
#include <oxys/arch/smp/smp.h>
#include <oxys/proc/sched.h>
#include <oxys/dev/pic.h>
#include <oxys/arch/interrupt/irq.h>
#include <oxys/acpi/acpi.h>
#include <oxys/dev/lapic.h>
#include <oxys/dev/ioapic.h>
void KernelInitialiseProcessors(void)
{
    /*
     * Sub-task 6.12: the firmware's description tables, the two APICs, and the
     * retirement of the 8259A pair.
     *
     * It stands here, and not among the interrupt work of Phase 3, because of
     * what it needs on either side. It needs the kernel arena, which is why it
     * cannot precede Phase 2; and it needs every driver that will ever claim a
     * request line to have claimed it, because the adoption carries the claimed
     * lines across and a line claimed afterwards would have to be programmed by
     * a second path. The serial adapter, immediately above, is the last of them.
     *
     * The interrupt flag is clear throughout, as IrqAdoptApic requires: between
     * the masking of the 8259A and the programming of the redirection tables
     * there is no controller that would deliver a device's request, and one
     * raised in that interval would be lost.
     */
    (void)AcpiInitialise(&KernelBootInformation);
    KernelVerifyAcpi();
    AcpiReport();

    (void)LocalApicInitialise();
    KernelVerifyLocalApic();
    LocalApicReport();

    (void)IoApicInitialise();
    KernelVerifyIoApic();
    IoApicReport();

    (void)IrqAdoptApic();
    KernelVerifyApicRouting();
    PicReport();
    IoApicReport();
    IrqReport();

    /*
     * Sub-task 6.13: the locks, the per-processor data and the interrupt one
     * processor sends to another.
     *
     * The area itself was established before the frame allocator, a lock needing
     * it; what stands here is the half that needs the local controller. An
     * inter-processor interrupt is written into the controller's command
     * register and delivered through the same gate a device's request uses, so
     * neither the layer nor the shootdown above it can exist before the
     * controller is enabled and the routing has been adopted.
     *
     * The shootdown registers its handler after the layer that carries it, and
     * before anything may broadcast one — which upon a machine with one processor
     * is never, the broadcast returning at once for want of an audience. It is
     * nevertheless exercised in full below, by a request this processor addresses
     * to itself.
     */
    IpiInitialise();
    ShootdownInitialise();

    KernelVerifyPerCpu();
    KernelVerifySpinlock();
    KernelVerifyIpi();
    KernelVerifyShootdown();

    /*
     * Sub-task 6.15: the scheduler, prepared before the processors it will
     * schedule upon.
     *
     * It stands before SmpInitialise because a processor that comes online goes
     * straight into SchedulerEnterIdle, and there must be a tick handler
     * registered and a calibrated rate for it to find. The calibration is a busy
     * wait upon the interval timer, so it must stand after that timer is
     * running — and it runs upon the bootstrap processor alone, which at this
     * point in the boot it does by construction.
     *
     * A failure here is reported and survived. The machine then runs
     * unpre-empted, which is what it did until this sub-task, rather than upon a
     * quantum computed from a rate nothing measured.
     */
    if (!SchedulerInitialise())
    {
        KernelWriteString("Scheduler: the local timer could not be calibrated; "
                          "nothing will be pre-empted.\n");
    }

    /*
     * Sub-task 6.14: the application processors.
     *
     * It stands after everything above it because a starting processor is given
     * everything and takes nothing: a stack and a double-fault stack from the
     * kernel arena, a task state segment descriptor in a table already built,
     * the interrupt descriptor table already filled, and a per-processor area
     * from a reservation that exists. It needs the local controller enabled,
     * because it is started by a command written into that controller; the
     * inter-processor interrupt layer and the shootdown, because the first thing
     * done after the last processor is up is a shootdown addressed to all of
     * them; and the interval timer running, because the delays the startup
     * protocol prescribes are measured by polling its counter with interrupts
     * masked.
     *
     * It stands before the bus enumeration and the storage drivers so that the
     * whole of the remainder of the boot runs upon a machine that has more than
     * one processor, rather than upon one that acquires them at the end.
     */
    SmpInitialise();

    /*
     * And this processor's own timer, last.
     *
     * It is started after the bring-up rather than before it because the
     * bring-up masks interrupts for its whole duration and measures the
     * protocol's delays by polling the interval timer's counter. A local timer
     * running through that would deliver its ticks the moment the flag was
     * restored — a burst of pre-emptions against a processor that had just
     * finished starting the machine, for no purpose.
     */
    (void)SchedulerStartOnThisProcessor();

    PerCpuReport();
    IpiReport();
    ShootdownReport();
    SmpReport();

    KernelVerifyApplicationProcessors();
    KernelVerifyScheduler();

    /*
     * The scheduler's report comes after its self-test and not before it, which
     * is the other way round from every report above.
     *
     * The reason is that there is nothing to report until something has been
     * scheduled. The bring-up of sub-task 6.14 had done its work by the time
     * SmpReport ran; the scheduler has admitted nobody until its own test admits
     * somebody, so a report before it would print four zeroes and a queue length
     * of none — which reads exactly like a scheduler that does not work.
     */
    SchedulerReport();
}
