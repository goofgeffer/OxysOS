/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/acpi/acpi.c
 * Purpose: Implements the reading of the firmware's ACPI description tables: the
 *          discovery and validation of the Root System Description Pointer, the
 *          walk of the RSDT or XSDT, and the parse of the Multiple APIC
 *          Description Table into the description of the machine's interrupt
 *          controllers that the Local APIC and I/O APIC drivers are programmed
 *          from.
 * Key functions: AcpiInitialise, AcpiIsAvailable, AcpiLocalApicAddress,
 *          AcpiDualPicPresent, AcpiProcessorCount, AcpiIoApicCount,
 *          AcpiOverrideCount, AcpiLocalNmiCount, AcpiGlobalInterruptForIsaIrq,
 *          AcpiIsaIrqIsActiveLow, AcpiIsaIrqIsLevelTriggered, AcpiReport.
 * References:
 *   - ACPI Specification 6.5, Section 5.2.5.1: the two regions searched for the
 *     pointer, upon 16-byte boundaries — the first kibibyte of the Extended BIOS
 *     Data Area, whose segment address is the two bytes at 0x40E, and the
 *     read-only memory between 0x0E0000 and 0x0FFFFF.
 *   - ACPI Specification 6.5, Section 5.2.5.3 and Table 5.3: the RSDP fields, the
 *     checksum over bytes 0 to 19, and the extended checksum over the declared
 *     length that revision 2 adds.
 *   - ACPI Specification 6.5, Section 5.2.6 and Table 5.4: the 36-byte header,
 *     and the requirement that the whole of a table sum to zero.
 *   - ACPI Specification 6.5, Sections 5.2.7 and 5.2.8: the RSDT with 32-bit
 *     entries and the XSDT with 64-bit ones; the XSDT must be used where present.
 *   - ACPI Specification 6.5, Section 5.2.12 and Tables 5.19 to 5.21: the MADT
 *     header, the PCAT_COMPAT flag, and the interrupt controller structure types.
 *   - ACPI Specification 6.5, Sections 5.2.12.2, 5.2.12.3, 5.2.12.5, 5.2.12.7,
 *     5.2.12.8 and 5.2.12.12: the layouts of the structures parsed here.
 *   - ACPI Specification 6.5, Table 5.26: the MPS INTI polarity and trigger mode.
 *   - Multiboot2 Specification 2.0, Sections 3.6.16 and 3.6.17: the copies of the
 *     RSDP that the boot loader may supply.
 *
 * Why every field is assembled from bytes rather than read through a structure.
 *
 *   The interrupt controller structures of the MADT follow one another with no
 *   padding whatever, and their lengths are 6, 8, 10, 12 and 16 bytes. An entry
 *   therefore begins at whatever offset the entries before it happen to end at,
 *   and a 32-bit field within it is frequently not upon a four-byte boundary. The
 *   same is true of the XSDT, whose 64-bit entries begin at offset 36. Reading
 *   such a field through a pointer to a structure is undefined behaviour, which
 *   PROJECT_GUIDELINES.md, Section 8, forbids; upon x86_64 it would in fact
 *   work, which is precisely what makes the practice dangerous to adopt.
 *
 *   Every multi-byte field is therefore assembled from its bytes, in the little
 *   endian order the specification records, by the three readers below. The cost
 *   is a few instructions in a routine that runs once.
 *
 * Why nothing here retains a pointer into the tables.
 *
 *   The memory map classifies the region holding these tables as ACPI
 *   reclaimable, meaning the kernel may take it back once the tables have been
 *   consumed. A retained pointer would become a defect at that moment, and the
 *   defect would present as an interrupt controller programmed from whatever the
 *   allocator had since written there. Every table is therefore mapped for the
 *   duration of its parse, copied from into the arrays below, and unmapped
 *   before this file returns to its caller.
 *
 * Concurrency. This runs once, upon the bootstrap processor, before any other
 * processor exists and with the interrupt flag clear. Every accessor below reads
 * state that is never written again after AcpiInitialise returns.
 */

#include <oxys/acpi.h>
#include <oxys/kernel.h>
#include <oxys/vmm.h>
#include <oxys/paging.h>

/* The length of the header every description table begins with, ACPI 6.5,
 * Table 5.4, and the offsets of the fields this file reads from it. */
#define ACPI_HEADER_LENGTH            36U
#define ACPI_HEADER_OFFSET_SIGNATURE   0U
#define ACPI_HEADER_OFFSET_LENGTH      4U
#define ACPI_HEADER_OFFSET_REVISION    8U
#define ACPI_HEADER_OFFSET_CHECKSUM    9U
#define ACPI_HEADER_OFFSET_OEM_ID     10U

/* The offsets of the RSDP fields, ACPI 6.5, Table 5.3. */
#define ACPI_RSDP_OFFSET_SIGNATURE          0U
#define ACPI_RSDP_OFFSET_CHECKSUM           8U
#define ACPI_RSDP_OFFSET_OEM_ID             9U
#define ACPI_RSDP_OFFSET_REVISION          15U
#define ACPI_RSDP_OFFSET_RSDT_ADDRESS      16U
#define ACPI_RSDP_OFFSET_LENGTH            20U
#define ACPI_RSDP_OFFSET_XSDT_ADDRESS      24U
#define ACPI_RSDP_OFFSET_EXTENDED_CHECKSUM 32U

/* The extent of the two parts of the RSDP, per Table 5.3: the first is all that
 * ACPI 1.0 defined and all that the first checksum covers. */
#define ACPI_RSDP_LEGACY_LENGTH   20U
#define ACPI_RSDP_EXTENDED_LENGTH 36U

/* The offsets of the MADT fields beyond the common header, ACPI 6.5, Table 5.19. */
#define ACPI_MADT_OFFSET_LOCAL_ADDRESS 36U
#define ACPI_MADT_OFFSET_FLAGS         40U
#define ACPI_MADT_OFFSET_ENTRIES       44U

/* Every interrupt controller structure begins with a type and a length. */
#define ACPI_MADT_ENTRY_HEADER_LENGTH 2U

/*
 * The greatest length this file will map for a single table.
 *
 * A description table of more than a mebibyte is not a table this kernel can
 * have any use for; a length field naming one is corruption, and mapping it
 * would exhaust the arena rather than report the corruption.
 */
#define ACPI_TABLE_LENGTH_MAXIMUM UINT32_C(0x00100000)

/* The regions searched for the pointer, per ACPI 6.5, Section 5.2.5.1. */
#define ACPI_EBDA_SEGMENT_POINTER UINT64_C(0x0000040E)
#define ACPI_EBDA_SEARCH_LENGTH   UINT64_C(1024)
#define ACPI_ROM_SEARCH_START     UINT64_C(0x000E0000)
#define ACPI_ROM_SEARCH_END       UINT64_C(0x00100000)
#define ACPI_RSDP_ALIGNMENT       UINT64_C(16)

/* The processor identifier that a Local APIC NMI structure uses to mean every
 * processor, per ACPI 6.5, Table 5.28. */
#define ACPI_LOCAL_NMI_ALL_PROCESSORS UINT32_C(0xFF)

/* The bus number that Section 5.2.12.5 fixes as meaning the ISA bus, that being
 * the only bus whose sources an override may describe. */
#define ACPI_OVERRIDE_BUS_ISA UINT8_C(0)

/* What was found, and where. */
static bool AcpiMadtParsed;
static PhysicalAddress AcpiPointerAddress;
static uint8_t AcpiPointerRevision;
static const char *AcpiPointerSource = "not found";
static PhysicalAddress AcpiDirectory;
static bool AcpiDirectoryExtended;

/* What the MADT declared. */
static PhysicalAddress AcpiLocalControllerAddress;
static bool AcpiDualPic;

static AcpiProcessor AcpiProcessors[ACPI_PROCESSOR_MAXIMUM];
static size_t AcpiProcessorTotal;
static AcpiIoApic AcpiIoApics[ACPI_IO_APIC_MAXIMUM];
static size_t AcpiIoApicTotal;
static AcpiInterruptOverride AcpiOverrides[ACPI_OVERRIDE_MAXIMUM];
static size_t AcpiOverrideTotal;
static AcpiLocalNmi AcpiLocalNmis[ACPI_LOCAL_NMI_MAXIMUM];
static size_t AcpiLocalNmiTotal;

/* The signatures of the tables the directory named, retained for the report. */
static char AcpiSignatures[ACPI_TABLE_MAXIMUM][5];
static size_t AcpiSignatureTotal;

/* Diagnostics. */
static uint64_t AcpiChecksumFailures;
static uint64_t AcpiUnrecognisedEntries;
static bool AcpiTruncated;

/*
 * The three little-endian readers.
 *
 * ACPI 6.5 records every multi-byte field of these tables in the byte order of
 * the IA-PC platform, least significant byte first. Assembling the value here
 * rather than dereferencing a wider pointer is what makes an unaligned field
 * safe to read; see the commentary in the file header.
 */
static uint16_t AcpiRead16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t AcpiRead32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t AcpiRead64(const uint8_t *bytes)
{
    return (uint64_t)AcpiRead32(bytes) | ((uint64_t)AcpiRead32(bytes + 4) << 32);
}

/*
 * Whether a run of bytes sums to zero, modulo 256.
 *
 * This is the only integrity check ACPI provides, and it is the reason a table
 * whose checksum fails is refused rather than parsed. A single corrupted byte in
 * an I/O APIC address would otherwise be programmed into the redirection
 * hardware, and the machine would receive its interrupts at an address that is
 * not a controller.
 */
static bool AcpiChecksumIsValid(const uint8_t *bytes, size_t length)
{
    uint8_t sum = 0U;

    for (size_t index = 0U; index < length; ++index)
    {
        sum = (uint8_t)(sum + bytes[index]);
    }

    return sum == 0U;
}

/* Whether the four bytes at the address are the signature named. */
static bool AcpiSignatureMatches(const uint8_t *bytes, const char *signature,
                                 size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if ((char)bytes[index] != signature[index])
        {
            return false;
        }
    }

    return true;
}

/*
 * Maps a description table and reports its declared length.
 *
 * The length is not known until the header has been read, and the header cannot
 * be read until something is mapped, so the mapping is made twice: once for the
 * header alone, and once for the whole. Mapping a fixed generous amount instead
 * would be simpler and wrong — a table lying at the very end of the region the
 * firmware reserved would have the mapping run past it into whatever follows.
 *
 * Returns NULL where the table could not be mapped, declares an implausible
 * length, or fails its checksum.
 */
static const uint8_t *AcpiMapTable(PhysicalAddress address, uint32_t *length_out)
{
    const uint8_t *header;
    const uint8_t *table;
    uint32_t length;

    if (address == 0U)
    {
        return NULL;
    }

    header = (const uint8_t *)KernelDeviceMap(address, ACPI_HEADER_LENGTH, 0U);

    if (header == NULL)
    {
        return NULL;
    }

    length = AcpiRead32(header + ACPI_HEADER_OFFSET_LENGTH);
    KernelDeviceUnmap((void *)(uintptr_t)header, ACPI_HEADER_LENGTH);

    if (length < ACPI_HEADER_LENGTH || length > ACPI_TABLE_LENGTH_MAXIMUM)
    {
        return NULL;
    }

    table = (const uint8_t *)KernelDeviceMap(address, length, 0U);

    if (table == NULL)
    {
        return NULL;
    }

    if (!AcpiChecksumIsValid(table, length))
    {
        ++AcpiChecksumFailures;
        KernelDeviceUnmap((void *)(uintptr_t)table, length);
        return NULL;
    }

    *length_out = length;

    return table;
}

static void AcpiUnmapTable(const uint8_t *table, uint32_t length)
{
    KernelDeviceUnmap((void *)(uintptr_t)table, (uint64_t)length);
}

/*
 * Records one interrupt controller structure of the MADT.
 *
 * An entry of a type this kernel does not act upon is counted rather than
 * ignored silently: a machine that declares its processors only through
 * structures this parse does not read would otherwise appear to have none, and
 * the count is what distinguishes that from a machine that truly declares none.
 */
static void AcpiRecordMadtEntry(const uint8_t *entry, uint8_t type, uint8_t length)
{
    switch (type)
    {
    case ACPI_MADT_TYPE_LOCAL_APIC:
        if (length < 8U)
        {
            ++AcpiUnrecognisedEntries;
            return;
        }

        if (AcpiProcessorTotal >= ACPI_PROCESSOR_MAXIMUM)
        {
            AcpiTruncated = true;
            return;
        }

        {
            const uint32_t flags = AcpiRead32(entry + 4U);
            AcpiProcessor *const processor = &AcpiProcessors[AcpiProcessorTotal];

            processor->acpi_uid = (uint32_t)entry[2];
            processor->apic_id = (uint32_t)entry[3];
            processor->enabled = (flags & ACPI_LOCAL_APIC_FLAG_ENABLED) != 0U;
            processor->online_capable =
                (flags & ACPI_LOCAL_APIC_FLAG_ONLINE_CAPABLE) != 0U;
            processor->extended = false;
            ++AcpiProcessorTotal;
        }
        return;

    case ACPI_MADT_TYPE_LOCAL_X2APIC:
        if (length < 16U)
        {
            ++AcpiUnrecognisedEntries;
            return;
        }

        if (AcpiProcessorTotal >= ACPI_PROCESSOR_MAXIMUM)
        {
            AcpiTruncated = true;
            return;
        }

        {
            const uint32_t flags = AcpiRead32(entry + 8U);
            AcpiProcessor *const processor = &AcpiProcessors[AcpiProcessorTotal];

            processor->apic_id = AcpiRead32(entry + 4U);
            processor->acpi_uid = AcpiRead32(entry + 12U);
            processor->enabled = (flags & ACPI_LOCAL_APIC_FLAG_ENABLED) != 0U;
            processor->online_capable =
                (flags & ACPI_LOCAL_APIC_FLAG_ONLINE_CAPABLE) != 0U;
            processor->extended = true;
            ++AcpiProcessorTotal;
        }
        return;

    case ACPI_MADT_TYPE_IO_APIC:
        if (length < 12U)
        {
            ++AcpiUnrecognisedEntries;
            return;
        }

        if (AcpiIoApicTotal >= ACPI_IO_APIC_MAXIMUM)
        {
            AcpiTruncated = true;
            return;
        }

        AcpiIoApics[AcpiIoApicTotal].identifier = entry[2];
        AcpiIoApics[AcpiIoApicTotal].address =
            (PhysicalAddress)AcpiRead32(entry + 4U);
        AcpiIoApics[AcpiIoApicTotal].interrupt_base = AcpiRead32(entry + 8U);
        ++AcpiIoApicTotal;
        return;

    case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE:
        if (length < 10U)
        {
            ++AcpiUnrecognisedEntries;
            return;
        }

        if (AcpiOverrideTotal >= ACPI_OVERRIDE_MAXIMUM)
        {
            AcpiTruncated = true;
            return;
        }

        AcpiOverrides[AcpiOverrideTotal].bus = entry[2];
        AcpiOverrides[AcpiOverrideTotal].source = entry[3];
        AcpiOverrides[AcpiOverrideTotal].global_interrupt = AcpiRead32(entry + 4U);
        AcpiOverrides[AcpiOverrideTotal].flags = AcpiRead16(entry + 8U);
        ++AcpiOverrideTotal;
        return;

    case ACPI_MADT_TYPE_LOCAL_APIC_NMI:
        if (length < 6U)
        {
            ++AcpiUnrecognisedEntries;
            return;
        }

        if (AcpiLocalNmiTotal >= ACPI_LOCAL_NMI_MAXIMUM)
        {
            AcpiTruncated = true;
            return;
        }

        AcpiLocalNmis[AcpiLocalNmiTotal].acpi_uid = (uint32_t)entry[2];
        AcpiLocalNmis[AcpiLocalNmiTotal].flags = AcpiRead16(entry + 3U);
        AcpiLocalNmis[AcpiLocalNmiTotal].local_interrupt = entry[5];
        ++AcpiLocalNmiTotal;
        return;

    case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE:
        if (length < 12U)
        {
            ++AcpiUnrecognisedEntries;
            return;
        }

        /*
         * Section 5.2.12.8 requires this address to be used for every local
         * controller in place of the 32-bit field of the MADT header, and
         * permits only one such structure. It is the reason the header's field
         * is read first and may be replaced afterwards rather than being taken
         * as final.
         */
        AcpiLocalControllerAddress = (PhysicalAddress)AcpiRead64(entry + 4U);
        return;

    default:
        ++AcpiUnrecognisedEntries;
        return;
    }
}

/*
 * Parses the Multiple APIC Description Table.
 *
 * The walk is bounded by the table's own declared length and advances by each
 * entry's declared length. An entry declaring a length below its own two-byte
 * header would leave the cursor where it was, so that case ends the walk: a
 * malformed table must not become a loop that does not terminate.
 */
static void AcpiParseMadt(const uint8_t *table, uint32_t length)
{
    uint32_t offset = ACPI_MADT_OFFSET_ENTRIES;

    if (length < ACPI_MADT_OFFSET_ENTRIES)
    {
        return;
    }

    AcpiLocalControllerAddress =
        (PhysicalAddress)AcpiRead32(table + ACPI_MADT_OFFSET_LOCAL_ADDRESS);
    AcpiDualPic = (AcpiRead32(table + ACPI_MADT_OFFSET_FLAGS) &
                   ACPI_MADT_FLAG_DUAL_PIC_PRESENT) != 0U;

    while ((offset + ACPI_MADT_ENTRY_HEADER_LENGTH) <= length)
    {
        const uint8_t *const entry = table + offset;
        const uint8_t type = entry[0];
        const uint8_t entry_length = entry[1];

        if (entry_length < ACPI_MADT_ENTRY_HEADER_LENGTH ||
            (offset + (uint32_t)entry_length) > length)
        {
            KernelWriteString("ACPI: the MADT entry list is malformed; the walk stops.\n");
            break;
        }

        AcpiRecordMadtEntry(entry, type, entry_length);
        offset += (uint32_t)entry_length;
    }

    AcpiMadtParsed = true;
}

/* Records a table's signature for the report, and parses it where it is one this
 * kernel reads. */
static void AcpiConsumeTable(PhysicalAddress address)
{
    uint32_t length = 0U;
    const uint8_t *table = AcpiMapTable(address, &length);

    if (table == NULL)
    {
        return;
    }

    if (AcpiSignatureTotal < ACPI_TABLE_MAXIMUM)
    {
        for (size_t index = 0U; index < 4U; ++index)
        {
            AcpiSignatures[AcpiSignatureTotal][index] = (char)table[index];
        }

        AcpiSignatures[AcpiSignatureTotal][4] = '\0';
        ++AcpiSignatureTotal;
    }
    else
    {
        AcpiTruncated = true;
    }

    if (AcpiSignatureMatches(table, "APIC", 4U) && !AcpiMadtParsed)
    {
        AcpiParseMadt(table, length);
    }

    AcpiUnmapTable(table, length);
}

/*
 * Whether a candidate address holds a valid Root System Description Pointer.
 *
 * Both checksums are applied where the revision claims the extended structure.
 * The first covers only the twenty bytes ACPI 1.0 defined, and a firmware that
 * got the second wrong while getting the first right would otherwise have its
 * 64-bit XSDT address believed.
 */
static bool AcpiPointerIsValid(const uint8_t *candidate)
{
    if (!AcpiSignatureMatches(candidate, "RSD PTR ", 8U))
    {
        return false;
    }

    if (!AcpiChecksumIsValid(candidate, ACPI_RSDP_LEGACY_LENGTH))
    {
        ++AcpiChecksumFailures;
        return false;
    }

    if (candidate[ACPI_RSDP_OFFSET_REVISION] >= 2U)
    {
        const uint32_t length = AcpiRead32(candidate + ACPI_RSDP_OFFSET_LENGTH);

        if (length < ACPI_RSDP_EXTENDED_LENGTH ||
            length > ACPI_TABLE_LENGTH_MAXIMUM ||
            !AcpiChecksumIsValid(candidate, (size_t)length))
        {
            ++AcpiChecksumFailures;
            return false;
        }
    }

    return true;
}

/*
 * Searches a region of low physical memory for the pointer.
 *
 * The direct physical map is used rather than a device mapping: the regions
 * searched lie below one mebibyte, which is mapped in every hierarchy this
 * kernel builds, and a search that had to establish and withdraw a mapping for
 * each candidate would be a great deal of machinery for reading memory that is
 * already addressable.
 */
static PhysicalAddress AcpiSearchRegion(PhysicalAddress start, PhysicalAddress end)
{
    for (PhysicalAddress address = start; (address + ACPI_RSDP_LEGACY_LENGTH) <= end;
         address += ACPI_RSDP_ALIGNMENT)
    {
        const uint8_t *const candidate =
            (const uint8_t *)(uintptr_t)PhysicalToDirect(address);

        if (AcpiPointerIsValid(candidate))
        {
            return address;
        }
    }

    return 0U;
}

/*
 * Finds the Root System Description Pointer.
 *
 * The boot loader's copy is preferred where it supplied one, because it is the
 * only source that remains correct upon a UEFI machine: Section 5.2.5.2 places
 * the pointer in the EFI System Table, and the two regions of Section 5.2.5.1
 * need not contain it at all. The search is retained for the machine whose boot
 * loader supplies no such tag, and is the path a legacy BIOS boot takes today.
 */
static bool AcpiLocatePointer(const BootInformation *information)
{
    PhysicalAddress found;

    if (information != NULL && information->acpi_rsdp_address != 0U)
    {
        const uint8_t *const supplied =
            (const uint8_t *)(uintptr_t)PhysicalToDirect(information->acpi_rsdp_address);

        if (AcpiPointerIsValid(supplied))
        {
            AcpiPointerAddress = information->acpi_rsdp_address;
            AcpiPointerRevision = supplied[ACPI_RSDP_OFFSET_REVISION];
            AcpiPointerSource = "the boot loader's copy";
            return true;
        }

        KernelWriteString("ACPI: the boot loader's copy of the pointer is not valid; "
                          "searching instead.\n");
    }

    /*
     * The segment address of the Extended BIOS Data Area is a paragraph count,
     * so it is scaled by sixteen to yield the address itself. A machine that
     * reports zero there has no such area and only the read-only memory is
     * searched.
     */
    {
        const uint16_t segment =
            AcpiRead16((const uint8_t *)(uintptr_t)PhysicalToDirect(ACPI_EBDA_SEGMENT_POINTER));
        const PhysicalAddress ebda = (PhysicalAddress)segment * 16U;

        if (ebda != 0U && (ebda + ACPI_EBDA_SEARCH_LENGTH) <= ACPI_ROM_SEARCH_END)
        {
            found = AcpiSearchRegion(ebda, ebda + ACPI_EBDA_SEARCH_LENGTH);

            if (found != 0U)
            {
                AcpiPointerAddress = found;
                AcpiPointerRevision =
                    ((const uint8_t *)(uintptr_t)PhysicalToDirect(found))[ACPI_RSDP_OFFSET_REVISION];
                AcpiPointerSource = "the Extended BIOS Data Area";
                return true;
            }
        }
    }

    found = AcpiSearchRegion(ACPI_ROM_SEARCH_START, ACPI_ROM_SEARCH_END);

    if (found != 0U)
    {
        AcpiPointerAddress = found;
        AcpiPointerRevision =
            ((const uint8_t *)(uintptr_t)PhysicalToDirect(found))[ACPI_RSDP_OFFSET_REVISION];
        AcpiPointerSource = "the BIOS read-only memory";
        return true;
    }

    return false;
}

/*
 * Walks the table directory the pointer names.
 *
 * The XSDT is preferred where the revision provides one and it validates, as
 * Section 5.2.8 requires. Falling back to the RSDT when it does not is a
 * deliberate choice: a machine whose XSDT is malformed is more usefully driven
 * from its RSDT than not driven at all, and the fall back is reported.
 */
static bool AcpiWalkDirectory(void)
{
    const uint8_t *directory;
    uint32_t length = 0U;
    size_t entry_width;
    size_t entry_count;

    AcpiDirectory = 0U;
    AcpiDirectoryExtended = false;

    if (AcpiPointerRevision >= 2U)
    {
        const uint8_t *const pointer =
            (const uint8_t *)(uintptr_t)PhysicalToDirect(AcpiPointerAddress);
        const PhysicalAddress extended =
            (PhysicalAddress)AcpiRead64(pointer + ACPI_RSDP_OFFSET_XSDT_ADDRESS);

        if (extended != 0U)
        {
            directory = AcpiMapTable(extended, &length);

            if (directory != NULL && AcpiSignatureMatches(directory, "XSDT", 4U))
            {
                AcpiDirectory = extended;
                AcpiDirectoryExtended = true;
            }
            else if (directory != NULL)
            {
                AcpiUnmapTable(directory, length);
            }
        }
    }

    if (!AcpiDirectoryExtended)
    {
        const uint8_t *const pointer =
            (const uint8_t *)(uintptr_t)PhysicalToDirect(AcpiPointerAddress);
        const PhysicalAddress root =
            (PhysicalAddress)AcpiRead32(pointer + ACPI_RSDP_OFFSET_RSDT_ADDRESS);

        directory = AcpiMapTable(root, &length);

        if (directory == NULL || !AcpiSignatureMatches(directory, "RSDT", 4U))
        {
            if (directory != NULL)
            {
                AcpiUnmapTable(directory, length);
            }

            return false;
        }

        AcpiDirectory = root;
    }

    entry_width = AcpiDirectoryExtended ? 8U : 4U;
    entry_count = ((size_t)length - ACPI_HEADER_LENGTH) / entry_width;

    /*
     * The directory remains mapped for the whole of the walk, and each table it
     * names is mapped and unmapped within it. That order matters: consuming a
     * table establishes and withdraws a mapping in the same arena the directory
     * itself is mapped from, and a walk that had released the directory first
     * would be reading an address the arena was free to reissue.
     */
    for (size_t index = 0U; index < entry_count; ++index)
    {
        const uint8_t *const slot =
            directory + ACPI_HEADER_LENGTH + (index * entry_width);
        const PhysicalAddress address = AcpiDirectoryExtended
                                            ? (PhysicalAddress)AcpiRead64(slot)
                                            : (PhysicalAddress)AcpiRead32(slot);

        AcpiConsumeTable(address);
    }

    AcpiUnmapTable(directory, length);

    return true;
}

bool AcpiInitialise(const BootInformation *information)
{
    AcpiMadtParsed = false;
    AcpiProcessorTotal = 0U;
    AcpiIoApicTotal = 0U;
    AcpiOverrideTotal = 0U;
    AcpiLocalNmiTotal = 0U;
    AcpiSignatureTotal = 0U;
    AcpiChecksumFailures = 0U;
    AcpiUnrecognisedEntries = 0U;
    AcpiTruncated = false;
    AcpiLocalControllerAddress = 0U;
    AcpiDualPic = false;

    if (!AcpiLocatePointer(information))
    {
        KernelWriteString("ACPI: no Root System Description Pointer was found.\n");
        return false;
    }

    if (!AcpiWalkDirectory())
    {
        KernelWriteString("ACPI: the table directory could not be read.\n");
        return false;
    }

    if (!AcpiMadtParsed)
    {
        KernelWriteString("ACPI: the tables contain no Multiple APIC Description Table.\n");
        return false;
    }

    return true;
}

bool AcpiIsAvailable(void)
{
    return AcpiMadtParsed;
}

PhysicalAddress AcpiRsdpAddress(void)
{
    return AcpiPointerAddress;
}

uint8_t AcpiRsdpRevision(void)
{
    return AcpiPointerRevision;
}

const char *AcpiRsdpSource(void)
{
    return AcpiPointerSource;
}

PhysicalAddress AcpiDirectoryAddress(void)
{
    return AcpiDirectory;
}

bool AcpiDirectoryIsExtended(void)
{
    return AcpiDirectoryExtended;
}

PhysicalAddress AcpiLocalApicAddress(void)
{
    return AcpiLocalControllerAddress;
}

bool AcpiDualPicPresent(void)
{
    return AcpiDualPic;
}

size_t AcpiProcessorCount(void)
{
    return AcpiProcessorTotal;
}

const AcpiProcessor *AcpiProcessorAt(size_t index)
{
    return (index < AcpiProcessorTotal) ? &AcpiProcessors[index] : NULL;
}

size_t AcpiUsableProcessorCount(void)
{
    size_t usable = 0U;

    for (size_t index = 0U; index < AcpiProcessorTotal; ++index)
    {
        if (AcpiProcessors[index].enabled || AcpiProcessors[index].online_capable)
        {
            ++usable;
        }
    }

    return usable;
}

size_t AcpiIoApicCount(void)
{
    return AcpiIoApicTotal;
}

const AcpiIoApic *AcpiIoApicAt(size_t index)
{
    return (index < AcpiIoApicTotal) ? &AcpiIoApics[index] : NULL;
}

size_t AcpiOverrideCount(void)
{
    return AcpiOverrideTotal;
}

const AcpiInterruptOverride *AcpiOverrideAt(size_t index)
{
    return (index < AcpiOverrideTotal) ? &AcpiOverrides[index] : NULL;
}

size_t AcpiLocalNmiCount(void)
{
    return AcpiLocalNmiTotal;
}

const AcpiLocalNmi *AcpiLocalNmiAt(size_t index)
{
    return (index < AcpiLocalNmiTotal) ? &AcpiLocalNmis[index] : NULL;
}

size_t AcpiTableCount(void)
{
    return AcpiSignatureTotal;
}

const char *AcpiTableSignatureAt(size_t index)
{
    return (index < AcpiSignatureTotal) ? AcpiSignatures[index] : NULL;
}

/* The override declared for an ISA request line, or NULL where none is. */
static const AcpiInterruptOverride *AcpiOverrideForIsaIrq(uint8_t irq)
{
    for (size_t index = 0U; index < AcpiOverrideTotal; ++index)
    {
        if (AcpiOverrides[index].bus == ACPI_OVERRIDE_BUS_ISA &&
            AcpiOverrides[index].source == irq)
        {
            return &AcpiOverrides[index];
        }
    }

    return NULL;
}

uint32_t AcpiGlobalInterruptForIsaIrq(uint8_t irq)
{
    const AcpiInterruptOverride *const override = AcpiOverrideForIsaIrq(irq);

    return (override != NULL) ? override->global_interrupt : (uint32_t)irq;
}

bool AcpiIsaIrqIsActiveLow(uint8_t irq)
{
    const AcpiInterruptOverride *const override = AcpiOverrideForIsaIrq(irq);

    if (override == NULL)
    {
        return false;
    }

    return (override->flags & ACPI_MPS_INTI_POLARITY_MASK) == ACPI_MPS_INTI_POLARITY_LOW;
}

bool AcpiIsaIrqIsLevelTriggered(uint8_t irq)
{
    const AcpiInterruptOverride *const override = AcpiOverrideForIsaIrq(irq);

    if (override == NULL)
    {
        return false;
    }

    return (override->flags & ACPI_MPS_INTI_TRIGGER_MASK) == ACPI_MPS_INTI_TRIGGER_LEVEL;
}

uint64_t AcpiChecksumFailureCount(void)
{
    return AcpiChecksumFailures;
}

uint64_t AcpiUnrecognisedEntryCount(void)
{
    return AcpiUnrecognisedEntries;
}

bool AcpiDescriptionsTruncated(void)
{
    return AcpiTruncated;
}

void AcpiReport(void)
{
    KernelWriteString("ACPI: ");

    if (AcpiPointerAddress == 0U)
    {
        KernelWriteString("no description tables were found.\n");
        return;
    }

    KernelWriteString("pointer at ");
    KernelWriteHexadecimal(AcpiPointerAddress);
    KernelWriteString(", revision ");
    KernelWriteDecimal((uint64_t)AcpiPointerRevision);
    KernelWriteString(", from ");
    KernelWriteString(AcpiPointerSource);
    KernelWriteString(".\n");

    KernelWriteString("ACPI: directory ");
    KernelWriteString(AcpiDirectoryExtended ? "XSDT at " : "RSDT at ");
    KernelWriteHexadecimal(AcpiDirectory);
    KernelWriteString(", tables ");
    KernelWriteDecimal((uint64_t)AcpiSignatureTotal);
    KernelWriteString(":");

    for (size_t index = 0U; index < AcpiSignatureTotal; ++index)
    {
        KernelWriteString(" ");
        KernelWriteString(AcpiSignatures[index]);
    }

    KernelWriteString(".\n");

    if (!AcpiMadtParsed)
    {
        KernelWriteString("ACPI: no Multiple APIC Description Table.\n");
        return;
    }

    KernelWriteString("ACPI: local controllers at ");
    KernelWriteHexadecimal(AcpiLocalControllerAddress);
    KernelWriteString(", processors ");
    KernelWriteDecimal((uint64_t)AcpiProcessorTotal);
    KernelWriteString(" of which usable ");
    KernelWriteDecimal((uint64_t)AcpiUsableProcessorCount());
    KernelWriteString(", 8259A pair ");
    KernelWriteString(AcpiDualPic ? "declared present.\n" : "declared absent.\n");

    for (size_t index = 0U; index < AcpiIoApicTotal; ++index)
    {
        KernelWriteString("  I/O APIC ");
        KernelWriteDecimal((uint64_t)AcpiIoApics[index].identifier);
        KernelWriteString(" at ");
        KernelWriteHexadecimal(AcpiIoApics[index].address);
        KernelWriteString(", global interrupts from ");
        KernelWriteDecimal((uint64_t)AcpiIoApics[index].interrupt_base);
        KernelWriteString(".\n");
    }

    /*
     * The overrides are named individually because they are the one part of this
     * table whose absence is indistinguishable from its correctness. A machine
     * whose timer is upon global interrupt 2 rather than 0 will interrupt
     * nothing at all if the override is missed, and the failure looks exactly
     * like a timer that was never programmed.
     */
    for (size_t index = 0U; index < AcpiOverrideTotal; ++index)
    {
        KernelWriteString("  ISA request ");
        KernelWriteDecimal((uint64_t)AcpiOverrides[index].source);
        KernelWriteString(" is carried by global interrupt ");
        KernelWriteDecimal((uint64_t)AcpiOverrides[index].global_interrupt);
        KernelWriteString(", flags ");
        KernelWriteHexadecimal((uint64_t)AcpiOverrides[index].flags);
        KernelWriteString(".\n");
    }

    for (size_t index = 0U; index < AcpiLocalNmiTotal; ++index)
    {
        KernelWriteString("  Non-maskable interrupt upon LINT");
        KernelWriteDecimal((uint64_t)AcpiLocalNmis[index].local_interrupt);
        KernelWriteString(" of processor ");

        if (AcpiLocalNmis[index].acpi_uid == ACPI_LOCAL_NMI_ALL_PROCESSORS)
        {
            KernelWriteString("every");
        }
        else
        {
            KernelWriteDecimal((uint64_t)AcpiLocalNmis[index].acpi_uid);
        }

        KernelWriteString(", flags ");
        KernelWriteHexadecimal((uint64_t)AcpiLocalNmis[index].flags);
        KernelWriteString(".\n");
    }

    KernelWriteString("ACPI: checksum failures ");
    KernelWriteDecimal(AcpiChecksumFailures);
    KernelWriteString(", unrecognised entries ");
    KernelWriteDecimal(AcpiUnrecognisedEntries);
    KernelWriteString(AcpiTruncated ? ", descriptions truncated.\n" : ".\n");
}
