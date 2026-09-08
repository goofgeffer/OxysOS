/*
 * File: kernel/include/oxys/lapic.h
 * Purpose: Declares the interface of the Local Advanced Programmable Interrupt
 *          Controller driver: the detection of the controller, the mapping of
 *          its register page, its enabling, the local vector table entries this
 *          kernel programmes, the end-of-interrupt it requires, and the
 *          identification of the processor it belongs to.
 * Key definitions: LAPIC_DEFAULT_BASE, LAPIC_REGISTER_*, LAPIC_SPURIOUS_VECTOR,
 *          LAPIC_ERROR_VECTOR, LocalApicIsSupported, LocalApicInitialise,
 *          LocalApicSignalEndOfInterrupt, LocalApicIdentifier,
 *          LocalApicIsEnabled, LocalApicIsBootstrapProcessor, LocalApicRead,
 *          LocalApicWrite, LocalApicReport.
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
 * Signals the completion of an interrupt, per Intel SDM, Volume 3A, Section
 * 10.8.5. It is called by the routing layer of kernel/cpu/irq.c upon the return
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
