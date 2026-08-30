/*
 * File: kernel/multiboot2.c
 * Purpose: Parses the Multiboot2 boot information structure supplied by GRUB and
 *          reduces it to the boot-protocol-neutral BootInformation description
 *          upon which the remainder of the kernel depends.
 * Key functions: BootInformationParseMultiboot2, BootInformationReport,
 *          BootMemoryTypeName, Multiboot2TranslateMemoryType,
 *          Multiboot2ParseMemoryMap, Multiboot2ParseElfSections.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.6.2: the fixed part of the
 *     structure; the common tag header; the rule that a tag's size excludes its
 *     trailing padding and that each tag begins at an 8-byte aligned address;
 *     the terminating tag of type 0 and size 8.
 *   - Multiboot2 Specification 2.0, Section 3.6.7: the ELF sections tag.
 *   - Multiboot2 Specification 2.0, Section 3.6.8: the memory map tag, the
 *     guarantee that entry_size is a multiple of eight, and the region types.
 *     The same section warns that the map "includes the regions occupied by
 *     kernel, mbi, segments and modules", and that the kernel must take care not
 *     to overwrite them. That warning is the reason the kernel and boot
 *     information extents are recorded here as separate reservations.
 *   - Multiboot2 Specification 2.0, Section 3.6.1: the boot loader may place the
 *     structure anywhere in memory, and the operating system must avoid
 *     overwriting it until it has finished using it.
 */

#include <oxys/bootinfo.h>
#include <oxys/multiboot2.h>
#include <oxys/kernel.h>

/*
 * The physical extent of the loaded kernel image, defined by linker.ld. Only the
 * addresses of these symbols are meaningful; they have no storage, which is why
 * they are declared as arrays and used without dereference.
 */
extern char KernelPhysicalStart[];
extern char KernelPhysicalEnd[];

/* The greatest total size accepted for the boot information structure. A larger
 * value indicates a corrupt or misidentified pointer rather than a genuine
 * structure, and is rejected in preference to walking arbitrary memory. */
#define MULTIBOOT2_TOTAL_SIZE_MAXIMUM UINT32_C(0x00100000)

/* The size of an ELF64 section header, used to validate the ELF sections tag. */
#define ELF64_SECTION_HEADER_SIZE UINT32_C(64)

/*
 * Rounds a value upward to the next multiple of MULTIBOOT2_TAG_ALIGNMENT, which
 * is the rule by which one tag's address is derived from the previous tag's
 * size, per Section 3.6.2.
 */
static uint32_t Multiboot2AlignTagSize(uint32_t size)
{
    return (size + (MULTIBOOT2_TAG_ALIGNMENT - 1U)) & ~(MULTIBOOT2_TAG_ALIGNMENT - 1U);
}

/*
 * Translates a Multiboot2 region type into the boot-protocol-neutral
 * classification. Section 3.6.8 provides that every value other than those
 * enumerated denotes a reserved area, so the default is deliberately
 * conservative: an unrecognised region is never treated as usable.
 */
static BootMemoryType Multiboot2TranslateMemoryType(uint32_t multiboot_type)
{
    switch (multiboot_type)
    {
    case MULTIBOOT2_MEMORY_AVAILABLE:
        return BOOT_MEMORY_USABLE;
    case MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE:
        return BOOT_MEMORY_ACPI_RECLAIMABLE;
    case MULTIBOOT2_MEMORY_NON_VOLATILE:
        return BOOT_MEMORY_NON_VOLATILE;
    case MULTIBOOT2_MEMORY_DEFECTIVE:
        return BOOT_MEMORY_DEFECTIVE;
    case MULTIBOOT2_MEMORY_RESERVED:
    default:
        return BOOT_MEMORY_RESERVED;
    }
}

const char *BootMemoryTypeName(BootMemoryType type)
{
    switch (type)
    {
    case BOOT_MEMORY_USABLE:
        return "usable";
    case BOOT_MEMORY_RESERVED:
        return "reserved";
    case BOOT_MEMORY_ACPI_RECLAIMABLE:
        return "ACPI reclaimable";
    case BOOT_MEMORY_NON_VOLATILE:
        return "non-volatile";
    case BOOT_MEMORY_DEFECTIVE:
        return "defective";
    default:
        return "unknown";
    }
}

/*
 * Copies a null-terminated string into a fixed buffer, truncating it if
 * necessary and terminating it in every case. A dedicated routine is provided
 * because the C library does not exist until Phase 7.
 */
static void Multiboot2CopyString(char *destination, const char *source, size_t capacity)
{
    size_t index;

    if (capacity == 0U)
    {
        return;
    }

    for (index = 0U; (index + 1U) < capacity && source[index] != '\0'; ++index)
    {
        destination[index] = source[index];
    }

    destination[index] = '\0';
}

/*
 * Parses the memory map tag, recording each region and accumulating the usable
 * total and the highest usable address.
 *
 * Entries are traversed by the tag's entry_size field rather than by the size of
 * the structure declared in this kernel, because Section 3.6.8 permits a future
 * version of the specification to enlarge the entry, and guarantees only that
 * the existing fields retain their meaning.
 */
static void Multiboot2ParseMemoryMap(const Multiboot2MemoryMapTag *tag,
                                     BootInformation *information)
{
    const uint8_t *entry_cursor;
    const uint8_t *entry_limit;
    uint32_t entry_size = tag->entry_size;

    /*
     * A zero or misaligned entry size would cause the traversal below either to
     * loop without end or to misread every entry. The specification guarantees a
     * non-zero multiple of eight, so a violation indicates a defective loader.
     */
    if (entry_size < sizeof(Multiboot2MemoryMapEntry) ||
        (entry_size % MULTIBOOT2_TAG_ALIGNMENT) != 0U)
    {
        KernelWriteString("  The memory map entry size is invalid; the map is ignored.\n");
        return;
    }

    entry_cursor = (const uint8_t *)tag + sizeof(Multiboot2MemoryMapTag);
    entry_limit = (const uint8_t *)tag + tag->size;

    while ((entry_cursor + entry_size) <= entry_limit)
    {
        const Multiboot2MemoryMapEntry *entry =
            (const Multiboot2MemoryMapEntry *)entry_cursor;
        BootMemoryType type = Multiboot2TranslateMemoryType(entry->type);

        /* A region of zero length conveys nothing and is discarded. */
        if (entry->length != 0U)
        {
            if (information->memory_region_count < BOOT_MEMORY_REGION_MAXIMUM)
            {
                BootMemoryRegion *region =
                    &information->memory_regions[information->memory_region_count];

                region->base_address = (PhysicalAddress)entry->base_address;
                region->length = entry->length;
                region->type = type;
                ++information->memory_region_count;
            }
            else
            {
                information->memory_map_truncated = true;
            }

            if (type == BOOT_MEMORY_USABLE)
            {
                PhysicalAddress region_end =
                    (PhysicalAddress)(entry->base_address + entry->length);

                information->usable_byte_count += entry->length;

                if (region_end > information->highest_usable_address)
                {
                    information->highest_usable_address = region_end;
                }
            }
        }

        entry_cursor += entry_size;
    }
}

/*
 * Parses the ELF sections tag and records the number of section headers.
 *
 * The physical extent of the kernel is deliberately NOT derived from the address
 * fields of these section headers. Section 3.6.7 states that "the physical
 * address fields of the ELF section header then refer to where the sections are
 * in memory", which holds for a kernel linked at the address at which it is
 * loaded. Oxys-OS is a higher-half kernel: the address field of every section
 * other than .boot holds a virtual address in the topmost two gibibytes, not a
 * physical one. Deriving a physical extent from those values would yield an
 * absurd range. The extent is therefore taken from the linker symbols
 * KernelPhysicalStart and KernelPhysicalEnd, which are correct by construction,
 * and this tag is parsed for validation and reporting alone.
 */
static void Multiboot2ParseElfSections(const Multiboot2ElfSectionsTag *tag,
                                       BootInformation *information)
{
    if (tag->entry_size != ELF64_SECTION_HEADER_SIZE)
    {
        /*
         * The tag was not laid out as expected. Refer to the discrepancy recorded
         * in the declaration of Multiboot2ElfSectionsTag: this check is what
         * distinguishes the two candidate interpretations of the tag's header.
         */
        KernelWriteString("  The ELF sections tag has an unexpected entry size: ");
        KernelWriteHexadecimal((uint64_t)tag->entry_size);
        KernelWriteString("; the tag is ignored.\n");
        return;
    }

    information->elf_section_count = tag->entry_count;
}

bool BootInformationParseMultiboot2(uint32_t information_address,
                                    BootInformation *information)
{
    const Multiboot2InformationHeader *header;
    const uint8_t *tag_cursor;
    const uint8_t *tag_limit;
    bool memory_map_found = false;

    /* Establish a defined initial state, the C library not being available. */
    for (size_t index = 0U; index < sizeof(BootInformation); ++index)
    {
        ((uint8_t *)information)[index] = 0U;
    }

    /*
     * Section 3.6.2 requires the structure to begin at an 8-byte aligned address.
     * A misaligned pointer indicates a defective loader or a corrupted register,
     * and the structure must not be walked.
     */
    if ((information_address % MULTIBOOT2_TAG_ALIGNMENT) != 0U)
    {
        KernelWriteString("The Multiboot2 information structure is misaligned.\n");
        return false;
    }

    header = (const Multiboot2InformationHeader *)(uintptr_t)
        PhysicalToVirtual((PhysicalAddress)information_address);

    if (header->total_size < sizeof(Multiboot2InformationHeader) ||
        header->total_size > MULTIBOOT2_TOTAL_SIZE_MAXIMUM)
    {
        KernelWriteString("The Multiboot2 information structure declares an implausible size.\n");
        return false;
    }

    information->boot_information_start = (PhysicalAddress)information_address;
    information->boot_information_end =
        (PhysicalAddress)information_address + header->total_size;

    information->kernel_physical_start = (PhysicalAddress)(uintptr_t)KernelPhysicalStart;
    information->kernel_physical_end = (PhysicalAddress)(uintptr_t)KernelPhysicalEnd;

    tag_cursor = (const uint8_t *)header + sizeof(Multiboot2InformationHeader);
    tag_limit = (const uint8_t *)header + header->total_size;

    while ((tag_cursor + sizeof(Multiboot2Tag)) <= tag_limit)
    {
        const Multiboot2Tag *tag = (const Multiboot2Tag *)tag_cursor;

        /*
         * A tag smaller than its own header, or one extending beyond the
         * declared total size, would cause the traversal to loop without end or
         * to read beyond the structure. Either indicates corruption.
         */
        if (tag->size < sizeof(Multiboot2Tag) ||
            (tag_cursor + tag->size) > tag_limit)
        {
            KernelWriteString("The Multiboot2 tag series is malformed; parsing stops.\n");
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_END)
        {
            break;
        }

        switch (tag->type)
        {
        case MULTIBOOT2_TAG_TYPE_MEMORY_MAP:
            Multiboot2ParseMemoryMap((const Multiboot2MemoryMapTag *)tag, information);
            memory_map_found = true;
            break;

        case MULTIBOOT2_TAG_TYPE_ELF_SECTIONS:
            Multiboot2ParseElfSections((const Multiboot2ElfSectionsTag *)tag, information);
            break;

        case MULTIBOOT2_TAG_TYPE_BOOT_LOADER:
            Multiboot2CopyString(information->boot_loader_name,
                                 ((const Multiboot2StringTag *)tag)->string,
                                 sizeof(information->boot_loader_name));
            break;

        case MULTIBOOT2_TAG_TYPE_COMMAND_LINE:
            Multiboot2CopyString(information->command_line,
                                 ((const Multiboot2StringTag *)tag)->string,
                                 sizeof(information->command_line));
            break;

        default:
            /* Tags the kernel does not yet consume are skipped deliberately. */
            break;
        }

        tag_cursor += Multiboot2AlignTagSize(tag->size);
    }

    if (!memory_map_found)
    {
        KernelWriteString("The Multiboot2 information structure contains no memory map.\n");
        return false;
    }

    return true;
}

void BootInformationReport(const BootInformation *information)
{
    KernelWriteString("Boot loader: ");
    KernelWriteString(information->boot_loader_name[0] != '\0'
                          ? information->boot_loader_name
                          : "not reported");
    KernelWriteString("\n");

    KernelWriteString("Physical memory map, as reported by the boot loader:\n");

    for (size_t index = 0U; index < information->memory_region_count; ++index)
    {
        const BootMemoryRegion *region = &information->memory_regions[index];

        KernelWriteString("  ");
        KernelWriteHexadecimal(region->base_address);
        KernelWriteString(" - ");
        KernelWriteHexadecimal(region->base_address + region->length);
        KernelWriteString("  ");
        KernelWriteDecimal(region->length / 1024U);
        KernelWriteString(" KiB  ");
        KernelWriteString(BootMemoryTypeName(region->type));
        KernelWriteString("\n");
    }

    if (information->memory_map_truncated)
    {
        KernelWriteString("  The memory map was truncated; not every region is listed.\n");
    }

    KernelWriteString("Usable memory: ");
    KernelWriteDecimal(information->usable_byte_count / 1024U);
    KernelWriteString(" KiB in ");
    KernelWriteDecimal((uint64_t)information->memory_region_count);
    KernelWriteString(" regions; highest usable address ");
    KernelWriteHexadecimal(information->highest_usable_address);
    KernelWriteString(".\n");

    KernelWriteString("Kernel image: ");
    KernelWriteHexadecimal(information->kernel_physical_start);
    KernelWriteString(" - ");
    KernelWriteHexadecimal(information->kernel_physical_end);
    KernelWriteString(" (");
    KernelWriteDecimal((information->kernel_physical_end -
                        information->kernel_physical_start) / 1024U);
    KernelWriteString(" KiB), in ");
    KernelWriteDecimal((uint64_t)information->elf_section_count);
    KernelWriteString(" ELF sections.\n");

    KernelWriteString("Boot information structure: ");
    KernelWriteHexadecimal(information->boot_information_start);
    KernelWriteString(" - ");
    KernelWriteHexadecimal(information->boot_information_end);
    KernelWriteString(".\n");
}
