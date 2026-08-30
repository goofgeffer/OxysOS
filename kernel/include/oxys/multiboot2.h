/*
 * File: kernel/include/oxys/multiboot2.h
 * Purpose: Declares the raw on-memory layout of the Multiboot2 boot information
 *          structure and of the individual tags that the kernel consumes. These
 *          declarations describe memory written by the boot loader and are
 *          therefore fixed by the specification, not by the conventions of this
 *          project.
 * Key definitions: Multiboot2InformationHeader, Multiboot2Tag,
 *          Multiboot2MemoryMapTag, Multiboot2MemoryMapEntry,
 *          Multiboot2ElfSectionsTag, Elf64SectionHeader.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.6.2 (Basic tags structure): the
 *     fixed part comprises a 32-bit total_size followed by a 32-bit reserved
 *     field; the structure's start is 8-byte aligned; every tag begins with a
 *     32-bit type and a 32-bit size; the size excludes trailing padding; tags
 *     follow one another padded so that each begins at an 8-byte aligned
 *     address; the series is terminated by a tag of type 0 and size 8.
 *   - Multiboot2 Specification 2.0, Section 3.6.7 (ELF-Symbols): tag type 9.
 *   - Multiboot2 Specification 2.0, Section 3.6.8 (Memory map): tag type 6, its
 *     entry_size and entry_version fields, and the entry layout of base_addr,
 *     length, type and reserved.
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement, and the ELF specification: the 64-byte ELF64 section header.
 */

#ifndef OXYS_MULTIBOOT2_H
#define OXYS_MULTIBOOT2_H

#include <oxys/types.h>

/* The tag types that the kernel presently consumes. */
#define MULTIBOOT2_TAG_TYPE_END           UINT32_C(0)
#define MULTIBOOT2_TAG_TYPE_COMMAND_LINE  UINT32_C(1)
#define MULTIBOOT2_TAG_TYPE_BOOT_LOADER   UINT32_C(2)
#define MULTIBOOT2_TAG_TYPE_BASIC_MEMORY  UINT32_C(4)
#define MULTIBOOT2_TAG_TYPE_MEMORY_MAP    UINT32_C(6)
#define MULTIBOOT2_TAG_TYPE_ELF_SECTIONS  UINT32_C(9)

/*
 * The memory region types of Section 3.6.8. A value of 1 denotes available
 * random access memory; 3 denotes usable memory holding ACPI information; 4
 * denotes memory to be preserved across hibernation; 5 denotes memory occupied
 * by defective modules. Every other value denotes a reserved area.
 */
#define MULTIBOOT2_MEMORY_AVAILABLE         UINT32_C(1)
#define MULTIBOOT2_MEMORY_RESERVED          UINT32_C(2)
#define MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE  UINT32_C(3)
#define MULTIBOOT2_MEMORY_NON_VOLATILE      UINT32_C(4)
#define MULTIBOOT2_MEMORY_DEFECTIVE         UINT32_C(5)

/* The alignment upon which every tag begins, per Section 3.6.2. */
#define MULTIBOOT2_TAG_ALIGNMENT            UINT32_C(8)

/*
 * The fixed part of the boot information structure, per Section 3.6.2. The
 * reserved field is set to zero by the boot loader and must be ignored.
 */
typedef struct Multiboot2InformationHeader
{
    uint32_t total_size;
    uint32_t reserved;
} Multiboot2InformationHeader;

/*
 * The header common to every tag, per Section 3.6.2. The size includes these
 * header fields but excludes the padding that aligns the following tag.
 */
typedef struct Multiboot2Tag
{
    uint32_t type;
    uint32_t size;
} Multiboot2Tag;

/*
 * One entry of the memory map, per Section 3.6.8. The reserved field is set to
 * zero by the boot loader and must be ignored.
 *
 * Entries must be traversed using the entry_size field of the containing tag
 * rather than sizeof(Multiboot2MemoryMapEntry), because the specification
 * permits future versions to enlarge the entry.
 */
typedef struct Multiboot2MemoryMapEntry
{
    uint64_t base_address;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} Multiboot2MemoryMapEntry;

/*
 * The memory map tag, per Section 3.6.8. The entry_size field is guaranteed to
 * be a multiple of eight; entry_version is presently zero, and future versions
 * are guaranteed to remain backward compatible.
 */
typedef struct Multiboot2MemoryMapTag
{
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* The entries follow, contiguously, each of entry_size bytes. */
} Multiboot2MemoryMapTag;

/*
 * The ELF sections tag, per Section 3.6.7.
 *
 * DISCREPANCY, deliberately resolved. The prose of Section 3.6.7 describes the
 * fields num, entsize, shndx and reserved as four 16-bit quantities, whereas the
 * reference C header reproduced in the same document, in the definition of
 * struct multiboot_tag_elf_sections, declares num, entsize and shndx as three
 * 32-bit quantities with no reserved field. The two cannot both be correct.
 *
 * The 32-bit interpretation is adopted here, because it is the layout that GRUB
 * actually emits, and the header is the form against which existing consumers
 * are written. The choice is verified at runtime: Multiboot2Parse rejects the
 * tag unless entry_size equals 64, the size of an ELF64 section header. Were the
 * 16-bit interpretation correct, the field read as entry_size would hold a
 * conflation of two unrelated 16-bit values and would not equal 64.
 */
typedef struct Multiboot2ElfSectionsTag
{
    uint32_t type;
    uint32_t size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t string_table_index;
    /* The section headers follow, contiguously, each of entry_size bytes. */
} Multiboot2ElfSectionsTag;

/* A tag whose payload is a null-terminated string: types 1 and 2. */
typedef struct Multiboot2StringTag
{
    uint32_t type;
    uint32_t size;
    char string[1];
} Multiboot2StringTag;

/*
 * The ELF64 section header, of which the ELF sections tag carries an array.
 * Refer to the System V ABI, AMD64 supplement, and to the ELF specification.
 * The structure is 64 bytes, which is asserted at compilation below.
 */
typedef struct Elf64SectionHeader
{
    uint32_t name_offset;
    uint32_t type;
    uint64_t flags;
    uint64_t address;
    uint64_t file_offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t address_alignment;
    uint64_t entry_size;
} Elf64SectionHeader;

/* ELF section header type: a section that occupies no space and has no meaning. */
#define ELF_SECTION_TYPE_NULL   UINT32_C(0)

/* ELF section header flag: the section occupies memory during execution. */
#define ELF_SECTION_FLAG_ALLOCATE UINT64_C(0x2)

/*
 * The ELF64 section header is defined by its specification to be 64 bytes. A
 * discrepancy would mean the compiler had inserted padding, which would cause
 * every section header after the first to be misread.
 */
_Static_assert(sizeof(Elf64SectionHeader) == 64,
               "The ELF64 section header must be exactly 64 bytes.");

_Static_assert(sizeof(Multiboot2MemoryMapEntry) == 24,
               "The Multiboot2 memory map entry must be exactly 24 bytes.");

#endif /* OXYS_MULTIBOOT2_H */
