/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/acpi.h
 * Purpose: Declares the reading of the firmware's ACPI description tables: the
 *          discovery and validation of the Root System Description Pointer, the
 *          walk of the table directory it names, and the parse of the Multiple
 *          APIC Description Table into the description of the machine's
 *          interrupt controllers that sub-task 6.12 is built upon.
 * Key definitions: AcpiProcessor, AcpiIoApic, AcpiInterruptOverride,
 *          AcpiLocalNmi, ACPI_MADT_TYPE_*, ACPI_MPS_INTI_*, AcpiInitialise,
 *          AcpiIsAvailable, AcpiLocalApicAddress, AcpiDualPicPresent,
 *          AcpiProcessorCount, AcpiIoApicCount, AcpiOverrideCount,
 *          AcpiLocalNmiCount, AcpiGlobalInterruptForIsaIrq,
 *          AcpiIsaIrqIsActiveLow, AcpiIsaIrqIsLevelTriggered, AcpiReport.
 * References:
 *   - ACPI Specification 6.5, Section 5.2.5.1 (Finding the RSDP on IA-PC
 *     Systems): the pointer is found by searching, upon 16-byte boundaries, the
 *     first kibibyte of the Extended BIOS Data Area, whose segment address lies
 *     in the two bytes at 0x40E, and the read-only memory between 0x0E0000 and
 *     0x0FFFFF.
 *   - ACPI Specification 6.5, Section 5.2.5.3 and Table 5.3 (RSDP Structure):
 *     the signature "RSD PTR " with its trailing space, the checksum over the
 *     first twenty bytes, the revision, the 32-bit RSDT address, and — from
 *     revision 2 — the length, the 64-bit XSDT address and the extended checksum
 *     over the whole of the declared length.
 *   - ACPI Specification 6.5, Section 5.2.6 and Table 5.4 (DESCRIPTION_HEADER
 *     Fields): the thirty-six byte header every description table begins with,
 *     and the requirement that the whole table sum to zero.
 *   - ACPI Specification 6.5, Sections 5.2.7 and 5.2.8: the RSDT, whose entries
 *     are 32-bit addresses, and the XSDT, whose entries are 64-bit; an
 *     ACPI-compatible system must use the XSDT where one is present.
 *   - ACPI Specification 6.5, Section 5.2.12 and Tables 5.19 to 5.21 (Multiple
 *     APIC Description Table): the 32-bit local interrupt controller address at
 *     offset 36, the flags at offset 40 of which bit 0 is PCAT_COMPAT, the list
 *     of interrupt controller structures beginning at offset 44, and the type
 *     values those structures are distinguished by.
 *   - ACPI Specification 6.5, Sections 5.2.12.2, 5.2.12.3, 5.2.12.5, 5.2.12.7,
 *     5.2.12.8 and 5.2.12.12: the field layouts of the Processor Local APIC, I/O
 *     APIC, Interrupt Source Override, Local APIC NMI, Local APIC Address
 *     Override and Processor Local x2APIC structures.
 *   - ACPI Specification 6.5, Table 5.26 (MPS INTI Flags): the polarity in bits
 *     1:0 and the trigger mode in bits 3:2, the value 00 in either meaning that
 *     the source conforms to the specification of the bus it is upon.
 *   - Multiboot2 Specification 2.0, Sections 3.6.16 and 3.6.17: the boot loader
 *     may supply a copy of the RSDP as tag type 14 or 15, which spares the
 *     kernel the search of Section 5.2.5.1.
 *   - docs/devices/ACPI.md: why the tables are copied out and unmapped rather
 *     than retained, and what this parse deliberately does not do.
 */

#ifndef OXYS_ACPI_H
#define OXYS_ACPI_H

#include <oxys/types.h>
#include <oxys/bootinfo.h>

/*
 * The greatest number of each kind of description this parse records.
 *
 * Every one of them is a fixed array rather than a heap allocation, because the
 * parse runs before the machine is known to be sound and a failure to allocate
 * would have to be reported through a path that itself needs the interrupt
 * controllers this table describes. A machine declaring more than these is
 * recorded as truncated rather than silently accepted, in the same manner as the
 * memory map of BootInformation.
 */
#define ACPI_PROCESSOR_MAXIMUM  64U
#define ACPI_IO_APIC_MAXIMUM    8U
#define ACPI_OVERRIDE_MAXIMUM   32U
#define ACPI_LOCAL_NMI_MAXIMUM  8U
#define ACPI_TABLE_MAXIMUM      32U

/* The interrupt controller structure types of ACPI 6.5, Table 5.21. Only those
 * this kernel acts upon are named; the remainder are counted as unrecognised. */
#define ACPI_MADT_TYPE_LOCAL_APIC          UINT8_C(0)
#define ACPI_MADT_TYPE_IO_APIC             UINT8_C(1)
#define ACPI_MADT_TYPE_INTERRUPT_OVERRIDE  UINT8_C(2)
#define ACPI_MADT_TYPE_NMI_SOURCE          UINT8_C(3)
#define ACPI_MADT_TYPE_LOCAL_APIC_NMI      UINT8_C(4)
#define ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE UINT8_C(5)
#define ACPI_MADT_TYPE_LOCAL_X2APIC        UINT8_C(9)

/*
 * The MPS INTI flags of ACPI 6.5, Table 5.26. A polarity or trigger mode of zero
 * means the source conforms to the convention of its bus, which for the ISA bus
 * is active high and edge triggered.
 */
#define ACPI_MPS_INTI_POLARITY_MASK      UINT16_C(0x0003)
#define ACPI_MPS_INTI_POLARITY_CONFORMS  UINT16_C(0x0000)
#define ACPI_MPS_INTI_POLARITY_HIGH      UINT16_C(0x0001)
#define ACPI_MPS_INTI_POLARITY_LOW       UINT16_C(0x0003)
#define ACPI_MPS_INTI_TRIGGER_MASK       UINT16_C(0x000C)
#define ACPI_MPS_INTI_TRIGGER_CONFORMS   UINT16_C(0x0000)
#define ACPI_MPS_INTI_TRIGGER_EDGE       UINT16_C(0x0004)
#define ACPI_MPS_INTI_TRIGGER_LEVEL      UINT16_C(0x000C)

/* Bit 0 of the MADT flags, ACPI 6.5, Table 5.20: the machine also carries the
 * PC-AT pair of 8259A controllers, whose lines must be masked before the APIC is
 * enabled. */
#define ACPI_MADT_FLAG_DUAL_PIC_PRESENT UINT32_C(0x00000001)

/* Bits 0 and 1 of the Local APIC flags, ACPI 6.5, Table 5.23. */
#define ACPI_LOCAL_APIC_FLAG_ENABLED        UINT32_C(0x00000001)
#define ACPI_LOCAL_APIC_FLAG_ONLINE_CAPABLE UINT32_C(0x00000002)

/*
 * One logical processor, as declared by a Processor Local APIC structure or by a
 * Processor Local x2APIC structure. The two are recorded in one form because
 * nothing above this layer has any interest in which of them declared a
 * processor; sub-task 6.14 brings up whichever are usable.
 *
 * A processor is usable if it is enabled, or if it is online capable, per the
 * rule of Table 5.23: a structure with both bits clear describes a processor
 * that is not present, and its contents must be ignored entirely.
 */
typedef struct AcpiProcessor
{
    uint32_t acpi_uid;
    uint32_t apic_id;
    bool enabled;
    bool online_capable;
    /* True where the declaration was a Processor Local x2APIC structure. */
    bool extended;
} AcpiProcessor;

/* One I/O APIC, and the range of global system interrupts its inputs carry. */
typedef struct AcpiIoApic
{
    uint8_t identifier;
    PhysicalAddress address;
    uint32_t interrupt_base;
} AcpiIoApic;

/*
 * One departure from the assumption that ISA interrupt request n is carried by
 * global system interrupt n. Only the exceptions are declared, per Section
 * 5.2.12.5, so the absence of an override is itself information.
 */
typedef struct AcpiInterruptOverride
{
    uint8_t bus;
    uint8_t source;
    uint32_t global_interrupt;
    uint16_t flags;
} AcpiInterruptOverride;

/* One connection of the non-maskable interrupt to a processor's LINT pin. A
 * processor identifier of 0xFF applies the entry to every processor. */
typedef struct AcpiLocalNmi
{
    uint32_t acpi_uid;
    uint16_t flags;
    uint8_t local_interrupt;
} AcpiLocalNmi;

/*
 * Finds the Root System Description Pointer, validates it, walks the table
 * directory and parses the Multiple APIC Description Table. Returns true where a
 * MADT was found and parsed.
 *
 * The kernel arena must be available: every table is mapped through
 * KernelDeviceMap for as long as it is being read and unmapped immediately
 * afterwards, so that nothing here retains a pointer into memory the firmware
 * declared reclaimable.
 *
 * A failure is not fatal. Where no MADT is found the machine is one this kernel
 * must continue to drive through the 8259A pair, and the report says so.
 */
bool AcpiInitialise(const BootInformation *information);

/* Whether a Multiple APIC Description Table was found and parsed. */
bool AcpiIsAvailable(void);

/* Where the Root System Description Pointer was found, and how. The description
 * is a constant string: the boot loader's tag, or the region searched. */
PhysicalAddress AcpiRsdpAddress(void);
uint8_t AcpiRsdpRevision(void);
const char *AcpiRsdpSource(void);

/* The address of the table directory actually used, and whether it was the XSDT. */
PhysicalAddress AcpiDirectoryAddress(void);
bool AcpiDirectoryIsExtended(void);

/*
 * The physical address at which each processor reaches its own local interrupt
 * controller, being the 32-bit field of the MADT header or the 64-bit value of a
 * Local APIC Address Override structure where one was declared. Zero where no
 * MADT was parsed.
 */
PhysicalAddress AcpiLocalApicAddress(void);

/* Whether the MADT declares that a PC-AT pair of 8259A controllers is present. */
bool AcpiDualPicPresent(void);

/* The descriptions recorded, and access to each in the order declared. */
size_t AcpiProcessorCount(void);
const AcpiProcessor *AcpiProcessorAt(size_t index);
size_t AcpiUsableProcessorCount(void);
size_t AcpiIoApicCount(void);
const AcpiIoApic *AcpiIoApicAt(size_t index);
size_t AcpiOverrideCount(void);
const AcpiInterruptOverride *AcpiOverrideAt(size_t index);
size_t AcpiLocalNmiCount(void);
const AcpiLocalNmi *AcpiLocalNmiAt(size_t index);

/* The signatures of the tables the directory named, for the report. */
size_t AcpiTableCount(void);
const char *AcpiTableSignatureAt(size_t index);

/*
 * The global system interrupt that carries an ISA interrupt request, applying
 * any override the MADT declared. Where none is declared the identity mapping of
 * Section 5.2.12.4 applies and the request number is returned unchanged.
 */
uint32_t AcpiGlobalInterruptForIsaIrq(uint8_t irq);

/*
 * How the input carrying an ISA interrupt request is to be programmed. Where the
 * MADT declares nothing, or declares that the source conforms to its bus, the
 * ISA convention applies: active high and edge triggered.
 */
bool AcpiIsaIrqIsActiveLow(uint8_t irq);
bool AcpiIsaIrqIsLevelTriggered(uint8_t irq);

/* Diagnostics: tables whose checksum did not sum to zero, entries of a type this
 * parse does not recognise, and descriptions dropped for want of room. */
uint64_t AcpiChecksumFailureCount(void);
uint64_t AcpiUnrecognisedEntryCount(void);
bool AcpiDescriptionsTruncated(void);

/* Emits a summary of what the firmware declared upon the console and the serial
 * port. */
void AcpiReport(void);

#endif /* OXYS_ACPI_H */
