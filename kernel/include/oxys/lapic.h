/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/lapic.h
 * Purpose: Declares the interface of the Local Advanced Programmable Interrupt
 *          Controller driver: the detection of the controller, the mapping of
 *          its register page, its enabling, the local vector table entries this
 *          kernel programmes, the end-of-interrupt it requires, and the
 *          identification of the processor it belongs to.
 * Key definitions: LAPIC_DEFAULT_BASE, LAPIC_REGISTER_*, LAPIC_SPURIOUS_VECTOR,
 *          LAPIC_ERROR_VECTOR, LAPIC_ICR_*, LocalApicIsSupported,
 *          LocalApicInitialise, LocalApicSignalEndOfInterrupt,
 *          LocalApicIdentifier, LocalApicIsEnabled,
 *          LocalApicIsBootstrapProcessor, LocalApicRead, LocalApicWrite,
 *          LocalApicSendCommand, LocalApicCommandIsIdle,
 *          LocalApicCalibrateTimer, LocalApicStartTimer, LocalApicStopTimer,
 *          LocalApicTimerCountsPerMillisecond, LocalApicTimerIsRunning,
 *          LocalApicReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Chapter 10 (Advanced Programmable Interrupt Controller). The chapter is
 *     numbered 11 in revisions of the manual issued after the renumbering that
 *     moved Memory Cache Control from Chapter 11 to Chapter 12; the numbering
 *     used throughout this project is the earlier one, in which the page
 *     attribute table is Section 11.12.2. See docs/project/REFERENCES.md.
 *   - Intel SDM, Volume 3A, Section 10.4.1: the registers are memory mapped to a
 *     4 KiB region whose initial address is 0xFEE00000, and that region must be
 *     mapped strong uncacheable for the controller to operate correctly.
 *   - Intel SDM, Volume 3A, Table 10-1 (Local APIC Register Address Map): the
 *     offsets of every register named below.
 *   - Intel SDM, Volume 3A, Section 10.4.2: CPUID leaf 1 reports the presence of
 *     an on-chip local APIC in bit 9 of EDX.
 *   - Intel SDM, Volume 3A, Sections 10.4.3 and 10.4.4, and Figure 10-5: the
 *     IA32_APIC_BASE model-specific register at 0x1B, of which bit 8 is the
 *     bootstrap processor flag, bit 11 the global enable, and bits 35:12 the
 *     base address.
 *   - Intel SDM, Volume 3A, Section 10.4.7.1: after reset every local vector
 *     table entry is masked and the spurious-interrupt vector register holds
 *     0x000000FF, bit 8 of which is clear — so the controller arrives disabled.
 *   - Intel SDM, Volume 3A, Section 10.5.1 and Figure 10-8: the local vector
 *     table entry, being the vector in bits 7:0, the delivery mode in bits 10:8,
 *     the pin polarity in bit 13, the trigger mode in bit 15 and the mask in
 *     bit 16.
 *   - Intel SDM, Volume 3A, Section 10.8.5: every handler save those entered by
 *     the non-maskable, system-management, initialisation and external delivery
 *     modes must write the end-of-interrupt register before returning.
 *   - Intel SDM, Volume 3A, Section 10.9 and Figure 10-23: the
 *     spurious-interrupt vector register, whose bit 8 enables the controller,
 *     and whose handler must return without an end-of-interrupt.
 *   - Intel SDM, Volume 3A, Section 10.6.1 and Figure 10-12: the interrupt
 *     command register, its fields, its four destination shorthands, and that
 *     "the act of writing to the low doubleword of the ICR causes the IPI to be
 *     sent". Every field is writable by software save the delivery status of bit
 *     12, which is read-only.
 *   - Intel SDM, Volume 3A, Section 10.6.2.1: in xAPIC mode the destination
 *     field is bits 63:56 of the register, being bits 31:24 of the high half.
 *   - docs/devices/APIC.md: why the spurious vector is 0xFF and what each of
 *     these registers is set to at initialisation.
 */

#ifndef OXYS_LAPIC_H
#define OXYS_LAPIC_H

#include <oxys/types.h>
#include <oxys/interrupts.h>

/* The address at which the register page is found after a reset, per Intel SDM,
 * Volume 3A, Section 10.4.4. The MSR is nevertheless read rather than this value
 * assumed: Section 10.4.5 permits the firmware to have relocated it. */
#define LAPIC_DEFAULT_BASE UINT64_C(0xFEE00000)

/* The extent of the register page. */
#define LAPIC_REGISTER_EXTENT UINT64_C(0x1000)

/* The registers this kernel reads or writes, per Intel SDM, Table 10-1. */
#define LAPIC_REGISTER_IDENTIFIER        UINT32_C(0x0020)
#define LAPIC_REGISTER_VERSION           UINT32_C(0x0030)
#define LAPIC_REGISTER_TASK_PRIORITY     UINT32_C(0x0080)
#define LAPIC_REGISTER_END_OF_INTERRUPT  UINT32_C(0x00B0)
#define LAPIC_REGISTER_LOGICAL_DESTINATION UINT32_C(0x00D0)
#define LAPIC_REGISTER_DESTINATION_FORMAT  UINT32_C(0x00E0)
#define LAPIC_REGISTER_SPURIOUS_VECTOR   UINT32_C(0x00F0)
#define LAPIC_REGISTER_IN_SERVICE        UINT32_C(0x0100)
#define LAPIC_REGISTER_INTERRUPT_REQUEST UINT32_C(0x0200)
#define LAPIC_REGISTER_ERROR_STATUS      UINT32_C(0x0280)
#define LAPIC_REGISTER_COMMAND_LOW       UINT32_C(0x0300)
#define LAPIC_REGISTER_COMMAND_HIGH      UINT32_C(0x0310)
#define LAPIC_REGISTER_LVT_TIMER         UINT32_C(0x0320)
#define LAPIC_REGISTER_LVT_THERMAL       UINT32_C(0x0330)
#define LAPIC_REGISTER_LVT_PERFORMANCE   UINT32_C(0x0340)
#define LAPIC_REGISTER_LVT_LINT0         UINT32_C(0x0350)
#define LAPIC_REGISTER_LVT_LINT1         UINT32_C(0x0360)
#define LAPIC_REGISTER_LVT_ERROR         UINT32_C(0x0370)
#define LAPIC_REGISTER_TIMER_INITIAL     UINT32_C(0x0380)
#define LAPIC_REGISTER_TIMER_CURRENT     UINT32_C(0x0390)
#define LAPIC_REGISTER_TIMER_DIVIDE      UINT32_C(0x03E0)

/* IA32_APIC_BASE, per Intel SDM, Volume 3A, Figure 10-5. */
#define IA32_APIC_BASE UINT32_C(0x0000001B)
#define LAPIC_BASE_BOOTSTRAP_PROCESSOR UINT64_C(0x0000000000000100)
#define LAPIC_BASE_EXTENDED_MODE       UINT64_C(0x0000000000000400)
#define LAPIC_BASE_GLOBAL_ENABLE       UINT64_C(0x0000000000000800)
#define LAPIC_BASE_ADDRESS_MASK        UINT64_C(0x0000000FFFFFF000)

/* Fields of a local vector table entry, per Intel SDM, Figure 10-8. */
#define LAPIC_LVT_VECTOR_MASK      UINT32_C(0x000000FF)
#define LAPIC_LVT_DELIVERY_FIXED   UINT32_C(0x00000000)
#define LAPIC_LVT_DELIVERY_NMI     UINT32_C(0x00000400)
#define LAPIC_LVT_DELIVERY_EXTINT  UINT32_C(0x00000700)
#define LAPIC_LVT_ACTIVE_LOW       UINT32_C(0x00002000)
#define LAPIC_LVT_LEVEL_TRIGGERED  UINT32_C(0x00008000)
#define LAPIC_LVT_MASKED           UINT32_C(0x00010000)

/* Fields of the spurious-interrupt vector register, per Intel SDM, Figure 10-23. */
#define LAPIC_SPURIOUS_SOFTWARE_ENABLE UINT32_C(0x00000100)

/*
 * Fields of the timer's local vector table entry, per Intel SDM, Volume 3A,
 * Section 10.5.4 and Figure 10-10.
 *
 * The mode is bits 18:17 and not a single flag, because there are three modes
 * and not two: 00 is one-shot, 01 is periodic, and 10 is the TSC deadline this
 * kernel does not use. The vector and the mask are the same bits every other
 * entry carries.
 */
#define LAPIC_LVT_TIMER_ONE_SHOT UINT32_C(0x00000000)
#define LAPIC_LVT_TIMER_PERIODIC UINT32_C(0x00020000)
#define LAPIC_LVT_TIMER_DEADLINE UINT32_C(0x00040000)

/*
 * The divide configuration register, per Intel SDM, Volume 3A, Section 10.5.4
 * and Figure 10-10.
 *
 * The encoding is not a plain binary divisor and is the sort of thing that is
 * wrong once and then wrong for ever: **bit 2 is reserved**, and the divisor is
 * carried in bits 3, 1 and 0. Divide-by-one is 1011B and not 0000B, which is
 * divide-by-two — so a register written with a value that "looks like one"
 * halves every interval the kernel believes it programmed.
 *
 * Sixteen is what this kernel uses. It is far enough from one that a calibration
 * counting down from 0xFFFFFFFF spans a useful interval without the counter
 * reaching zero, and far enough from 128 that a millisecond is still thousands
 * of counts rather than tens.
 */
#define LAPIC_TIMER_DIVIDE_1   UINT32_C(0x0000000B)
#define LAPIC_TIMER_DIVIDE_2   UINT32_C(0x00000000)
#define LAPIC_TIMER_DIVIDE_4   UINT32_C(0x00000001)
#define LAPIC_TIMER_DIVIDE_8   UINT32_C(0x00000002)
#define LAPIC_TIMER_DIVIDE_16  UINT32_C(0x00000003)
#define LAPIC_TIMER_DIVIDE_32  UINT32_C(0x00000008)
#define LAPIC_TIMER_DIVIDE_64  UINT32_C(0x00000009)
#define LAPIC_TIMER_DIVIDE_128 UINT32_C(0x0000000A)

/*
 * Fields of the interrupt command register, per Intel SDM, Volume 3A, Section
 * 10.6.1 and Figure 10-12.
 *
 * The register is two 32-bit halves: the low half at 0x300 holds the vector and
 * every option, the high half at 0x310 holds the destination in its bits 31:24.
 * Writing the low half is what sends the interrupt, so the destination must be
 * written first — a machine whose high half still held the previous
 * destination would deliver this interrupt to the previous target.
 */
#define LAPIC_ICR_VECTOR_MASK          UINT32_C(0x000000FF)
#define LAPIC_ICR_DELIVERY_FIXED       UINT32_C(0x00000000)
#define LAPIC_ICR_DELIVERY_LOWEST      UINT32_C(0x00000100)
#define LAPIC_ICR_DELIVERY_SMI         UINT32_C(0x00000200)
#define LAPIC_ICR_DELIVERY_NMI         UINT32_C(0x00000400)
#define LAPIC_ICR_DELIVERY_INIT        UINT32_C(0x00000500)
#define LAPIC_ICR_DELIVERY_STARTUP     UINT32_C(0x00000600)
#define LAPIC_ICR_DESTINATION_PHYSICAL UINT32_C(0x00000000)
#define LAPIC_ICR_DESTINATION_LOGICAL  UINT32_C(0x00000800)
#define LAPIC_ICR_DELIVERY_PENDING     UINT32_C(0x00001000)
#define LAPIC_ICR_LEVEL_ASSERT         UINT32_C(0x00004000)
#define LAPIC_ICR_TRIGGER_LEVEL        UINT32_C(0x00008000)

/*
 * The destination shorthands of Figure 10-12, bits 19:18.
 *
 * A shorthand replaces the destination field entirely, and the two this kernel
 * uses are the ones no list of identifiers could express as cheaply: "every
 * processor but me", which is the audience of a shootdown, and "me", which is
 * how a mechanism meant for other processors is exercised upon a machine that
 * has only started one.
 */
#define LAPIC_ICR_SHORTHAND_NONE           UINT32_C(0x00000000)
#define LAPIC_ICR_SHORTHAND_SELF           UINT32_C(0x00040000)
#define LAPIC_ICR_SHORTHAND_ALL            UINT32_C(0x00080000)
#define LAPIC_ICR_SHORTHAND_ALL_BUT_SELF   UINT32_C(0x000C0000)

/* The destination field occupies bits 31:24 of the high half in xAPIC mode, per
 * Intel SDM, Volume 3A, Section 10.6.2.1. */
#define LAPIC_ICR_DESTINATION_SHIFT 24U

/* Fields of the version register, per Intel SDM, Section 10.4.8. */
#define LAPIC_VERSION_MASK           UINT32_C(0x000000FF)
#define LAPIC_VERSION_MAX_LVT_SHIFT  16U
#define LAPIC_VERSION_MAX_LVT_MASK   UINT32_C(0x000000FF)

/*
 * The two vectors this driver claims for itself.
 *
 * The spurious vector is the highest available, and its low four bits are all
 * ones. Intel SDM, Volume 3A, Section 10.9, records that upon the P6 family and
 * the Pentium those four bits are hardwired to one and writes to them have no
 * effect; a vector chosen without that property would be programmed and then
 * silently altered by the hardware, and the handler entered would be one for a
 * vector nothing had registered.
 *
 * The error vector is the next below it. It is separate because an error the
 * controller detects in itself is a different event from a request it withdrew,
 * and a single counter covering both would hide either.
 */
#define LAPIC_SPURIOUS_VECTOR UINT8_C(0xFF)
#define LAPIC_ERROR_VECTOR    UINT8_C(0xFE)

/* Whether the processor reports an on-chip local APIC, per CPUID leaf 1. */
bool LocalApicIsSupported(void);

/*
 * Maps the register page, enables the controller and programmes the local vector
 * table: the spurious and error vectors, the task priority, and whichever local
 * interrupt pin the ACPI tables declare the non-maskable interrupt to be upon.
 * Every other entry is left masked.
 *
 * AcpiInitialise should have run: the base address the MADT declares is
 * preferred to the one IA32_APIC_BASE holds where the two differ, a Local APIC
 * Address Override being the firmware's own statement about a machine whose
 * controllers were relocated. Where no MADT was parsed the register is used
 * alone, which is correct for every machine this kernel has been run upon.
 *
 * Returns false where the processor reports no local APIC, or where its register
 * page could not be mapped.
 */
bool LocalApicInitialise(void);

/*
 * Programmes the executing processor's own controller, the register page having
 * already been mapped by LocalApicInitialise upon the bootstrap processor.
 *
 * It is what an application processor calls at sub-task 6.14. Every processor
 * has a controller of its own, reached at the same physical address, and a
 * reset leaves each of them software-disabled with its local vector table
 * masked — so a processor that never ran this would accept no interrupt at all,
 * including the shootdown its fellows will send it, and would present as a
 * processor that started and then stopped answering.
 *
 * Returns false where the register page has not been mapped, which is a
 * processor started against a kernel whose own controller never came up.
 */
bool LocalApicInitialiseThisProcessor(void);

/*
 * Signals the completion of an interrupt, per Intel SDM, Volume 3A, Section
 * 10.8.5. It is called by the routing layer of kernel/arch/x86_64/interrupt/irq.c upon the return
 * of a device handler, and by nothing else; see docs/design/INTERRUPTS.md,
 * Section 10.3, for why a device driver does not signal for itself.
 */
void LocalApicSignalEndOfInterrupt(void);

/* Whether the controller has been mapped and enabled. */
bool LocalApicIsEnabled(void);

/* The identifier of the controller belonging to the executing processor, being
 * bits 31:24 of the identifier register in xAPIC mode. */
uint8_t LocalApicIdentifier(void);

/* The version register, and the count of local vector table entries it reports,
 * which is the field's value plus one. */
uint32_t LocalApicVersion(void);
uint8_t LocalApicLocalVectorCount(void);

/* Whether the executing processor is the one the firmware selected to boot the
 * machine, per bit 8 of IA32_APIC_BASE. */
bool LocalApicIsBootstrapProcessor(void);

/* The physical address the register page was mapped from. */
PhysicalAddress LocalApicBaseAddress(void);

/*
 * Sends an interrupt through the interrupt command register.
 *
 * `destination` is the APIC identifier of the target and is ignored where the
 * command carries a shorthand. `command` is the low half of the register: a
 * vector, a delivery mode, a destination mode, a level, a trigger mode and a
 * shorthand, composed from the LAPIC_ICR_ definitions above.
 *
 * The controller is waited for both before and after: before, because the
 * register may still be carrying the previous interrupt and writing it then
 * would discard that one; after, because a caller that must know the interrupt
 * was accepted has no other way to find out. Intel SDM, Volume 3A, Section
 * 10.6.1, gives bit 12 as the delivery status, read-only, set while a send is
 * pending.
 *
 * Returns false where the controller is not enabled, or where the delivery
 * status did not clear within a bound — which is a controller that has stopped
 * answering, and a caller that waited for an acknowledgement from it would wait
 * for ever.
 */
bool LocalApicSendCommand(uint8_t destination, uint32_t command);

/* Whether the command register is idle, being bit 12 clear. */
bool LocalApicCommandIsIdle(void);

/* The number of interrupts sent through the command register, and the number of
 * sends abandoned because the delivery status did not clear. */
uint64_t LocalApicCommandCount(void);
uint64_t LocalApicCommandTimeoutCount(void);

/*
 * Establishes how fast this machine's local timers count, by measuring one
 * against the interval timer of drivers/pit/pit.c.
 *
 * The rate is the processor's bus clock or core crystal divided by the divide
 * configuration register, and Intel SDM, Volume 3A, Section 10.5.4, states no
 * figure for it: it is a property of the machine, so it is measured and not
 * assumed. The measurement is made once, upon the bootstrap processor, and the
 * result is used to programme every processor's timer — the clock being the
 * machine's rather than the processor's.
 *
 * It must be called with interrupts masked and after the interval timer is
 * running, the measurement being a busy wait upon that timer's counter.
 *
 * Returns false where the local controller is not enabled or where the count
 * measured is implausible, in which case no timer is programmed anywhere and the
 * scheduler is told rather than left to run upon a rate it invented.
 */
bool LocalApicCalibrateTimer(void);

/* Counts of the local timer in one millisecond, or zero if no calibration has
 * succeeded. Divided by LAPIC_TIMER_DIVIDE_16. */
uint32_t LocalApicTimerCountsPerMillisecond(void);

/*
 * Starts this processor's own timer, periodic, at the given interval.
 *
 * Each processor calls it for itself, the timer being one of the local vector
 * table entries and therefore per processor. A processor that never calls it
 * takes no timer interrupt and is never pre-empted, which is what an
 * uninitialised processor would silently be.
 *
 * Returns false where no calibration has been made.
 */
bool LocalApicStartTimer(uint8_t vector, uint32_t milliseconds);

/* Masks this processor's timer entry again. */
void LocalApicStopTimer(void);

/* Whether this processor's timer entry is unmasked, read back from the entry
 * itself rather than from a variable this kernel keeps. */
bool LocalApicTimerIsRunning(void);

/*
 * Reads and writes a register of the mapped page. Exposed so that the self-test
 * may assert upon the state this driver established without a second copy of the
 * mapping arithmetic; a caller outside this driver and its self-test has no
 * business here.
 */
uint32_t LocalApicRead(uint32_t offset);
void LocalApicWrite(uint32_t offset, uint32_t value);

/* The number of spurious interrupts delivered, the number of errors the
 * controller reported, and the error status last read. */
uint64_t LocalApicSpuriousCount(void);
uint64_t LocalApicErrorCount(void);
uint32_t LocalApicLastErrorStatus(void);

/* Emits a summary of the controller's state upon the console and the serial port. */
void LocalApicReport(void);

#endif /* OXYS_LAPIC_H */
