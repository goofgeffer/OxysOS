/*
 * File: kernel/include/oxys/bootinfo.h
 * Purpose: Declares the boot-protocol-neutral description of the machine that
 *          the kernel consumes in place of the raw Multiboot2 structures. The
 *          kernel proper depends upon this description alone, so that the UEFI
 *          boot path of Phase 12 may populate it identically without any change
 *          above the handoff layer.
 * Key definitions: BootMemoryType, BootMemoryRegion, BootInformation,
 *          BootInformationParseMultiboot2, BootInformationReport.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.6.8: the memory region types from
 *     which BootMemoryType is derived.
 *   - docs/ARCHITECTURE.md, Section 1, premise 3: boot-protocol neutrality is a
 *     design constraint of the project, not a later accommodation.
 */

#ifndef OXYS_BOOTINFO_H
#define OXYS_BOOTINFO_H

#include <oxys/types.h>

/*
 * The greatest number of memory regions recorded. Firmware of the present era
 * reports considerably fewer than this; a machine reporting more would have its
 * map truncated, which BootInformationParseMultiboot2 reports as a diagnostic
 * rather than silently accepting.
 */
#define BOOT_MEMORY_REGION_MAXIMUM 64

/* The greatest length of the boot loader name and the command line retained. */
#define BOOT_STRING_MAXIMUM 128

/*
 * The classification of a physical memory region, independent of the boot
 * protocol that reported it.
 */
typedef enum BootMemoryType
{
    /* Available for the kernel to allocate. */
    BOOT_MEMORY_USABLE = 0,
    /* Reserved by the firmware or by a device. Must never be allocated. */
    BOOT_MEMORY_RESERVED = 1,
    /* Holds ACPI tables. Reclaimable once those tables have been consumed. */
    BOOT_MEMORY_ACPI_RECLAIMABLE = 2,
    /* Must be preserved across hibernation. */
    BOOT_MEMORY_NON_VOLATILE = 3,
    /* Occupied by defective memory modules. */
    BOOT_MEMORY_DEFECTIVE = 4
} BootMemoryType;

/* One contiguous region of physical memory of a single classification. */
typedef struct BootMemoryRegion
{
    PhysicalAddress base_address;
    uint64_t length;
    BootMemoryType type;
} BootMemoryRegion;

/*
 * The complete description of the machine as reported by the boot loader,
 * together with the extents that the kernel must not allocate.
 *
 * Strings are copied into this structure rather than pointed at, because the
 * memory the boot loader used is itself reclaimable once the kernel has
 * finished with it, and a retained pointer would become a defect at that moment.
 */
typedef struct BootInformation
{
    BootMemoryRegion memory_regions[BOOT_MEMORY_REGION_MAXIMUM];
    size_t memory_region_count;

    /* True if the machine reported more regions than could be recorded. */
    bool memory_map_truncated;

    /* The sum of the lengths of every region classified as usable. */
    uint64_t usable_byte_count;

    /* One beyond the highest address of any region classified as usable. */
    PhysicalAddress highest_usable_address;

    /*
     * The physical extent of the loaded kernel image, inclusive of the boot
     * section and of the BSS. The frame allocator must not issue any frame
     * within this range.
     */
    PhysicalAddress kernel_physical_start;
    PhysicalAddress kernel_physical_end;

    /*
     * The physical extent of the boot information structure itself. The
     * specification places it in memory the map reports as available, so it must
     * be reserved explicitly.
     */
    PhysicalAddress boot_information_start;
    PhysicalAddress boot_information_end;

    /* The number of ELF section headers reported, and their aggregate extent. */
    uint32_t elf_section_count;

    char boot_loader_name[BOOT_STRING_MAXIMUM];
    char command_line[BOOT_STRING_MAXIMUM];
} BootInformation;

/*
 * Parses the Multiboot2 boot information structure at the given physical address
 * and populates the supplied BootInformation.
 *
 * Returns true if the structure was parsed successfully. Returns false, having
 * emitted a diagnostic, if the structure is misaligned, if it declares an
 * implausible total size, or if it contains no memory map, since the kernel
 * cannot proceed without one.
 */
bool BootInformationParseMultiboot2(uint32_t information_address,
                                    BootInformation *information);

/* Emits the parsed description upon the console and the serial port. */
void BootInformationReport(const BootInformation *information);

/* Returns a constant, human-readable name for a memory region classification. */
const char *BootMemoryTypeName(BootMemoryType type);

#endif /* OXYS_BOOTINFO_H */
