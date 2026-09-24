/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/exec/elf.h
 * Purpose: Declares the ELF64 loader for statically linked executables: the
 *          decoded header and program header, the validation an image must
 *          survive, and the loading of its segments into an address space.
 * Key definitions: ElfHeader, ElfProgramHeader, ElfImage, ElfResult, ElfDecode,
 *          ElfValidate, ElfLoad, ElfLoadFile, ElfResultName, ElfSegmentAt,
 *          ElfReport.
 * References:
 *   - Tool Interface Standard, Executable and Linking Format Specification,
 *     version 1.2, and the ELF-64 Object File Format, version 1.5 draft 2: the
 *     file header, the program header table, and the segment types.
 *   - ELF-64, the identification: bytes 0 to 3 are 7Fh 'E' 'L' 'F'; byte 4 the
 *     class, 2 being 64-bit; byte 5 the data encoding, 1 being two's complement
 *     little endian; byte 6 the version, 1 being current.
 *   - ELF-64, the file header: the type at offset 16, the machine at 18, the
 *     version at 20, the entry at 24, the program header offset at 32, the
 *     section header offset at 40, the flags at 48, the header size at 52, the
 *     program header entry size at 54 and their number at 56. The header is 64
 *     bytes and a program header entry is 56.
 *   - ELF-64, the program header: the type at offset 0, the flags at 4, the
 *     offset within the file at 8, the virtual address at 16, the physical
 *     address at 24, the size within the file at 32, the size in memory at 40
 *     and the alignment at 48.
 *   - System V Application Binary Interface, AMD64 supplement: the machine is
 *     62, and an executable is loaded at the addresses its program headers name.
 *   - docs/design/EXECUTABLE.md: the design of this loader and the reasoning.
 */

#ifndef OXYS_EXEC_ELF_H
#define OXYS_EXEC_ELF_H

#include <oxys/types.h>
#include <oxys/arch/mm/addrspace.h>

/* The sizes the format fixes. They are asserted against what is decoded rather
 * than used to overlay a structure; see CODING-STANDARDS.md. */
#define ELF_HEADER_BYTES         64U
#define ELF_PROGRAM_HEADER_BYTES 56U
#define ELF_IDENTIFICATION_BYTES 16U

/* Byte 4 of the identification: the class. */
#define ELF_CLASS_64 2U

/* Byte 5: the data encoding. */
#define ELF_DATA_LITTLE_ENDIAN 1U

/* Byte 6, and the file header's version field. */
#define ELF_VERSION_CURRENT 1U

/* The file types this loader distinguishes. */
#define ELF_TYPE_RELOCATABLE 1U
#define ELF_TYPE_EXECUTABLE  2U
#define ELF_TYPE_SHARED      3U

/* The machine, from the AMD64 supplement. */
#define ELF_MACHINE_X86_64 62U

/* The segment types this loader distinguishes. */
#define ELF_SEGMENT_NULL     0U
#define ELF_SEGMENT_LOAD     1U
#define ELF_SEGMENT_DYNAMIC  2U
#define ELF_SEGMENT_INTERPRETER 3U

/* The permissions a segment asks for. */
#define ELF_SEGMENT_EXECUTE UINT32_C(0x1)
#define ELF_SEGMENT_WRITE   UINT32_C(0x2)
#define ELF_SEGMENT_READ    UINT32_C(0x4)

/*
 * The greatest number of program headers this loader will read.
 *
 * A bound is needed and this one is generous: an executable of this kind has
 * four or five. What must not happen is an image claiming sixty thousand headers
 * and the loader walking every one of them before deciding the image is
 * nonsense — the validation is proportional to the count, so the count is what
 * must be bounded first.
 */
#define ELF_SEGMENT_MAXIMUM 16U

/* The decoded file header. The fields are named for what they are rather than
 * by the specification's abbreviations; the offsets each was read from are in
 * the reference block above. */
typedef struct ElfHeader
{
    uint8_t class_code;
    uint8_t data_encoding;
    uint8_t identification_version;
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
} ElfHeader;

/* One decoded program header. */
typedef struct ElfProgramHeader
{
    uint32_t type;
    uint32_t flags;
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} ElfProgramHeader;

/* What a successful load produced. */
typedef struct ElfImage
{
    uint64_t entry;    /* Where execution begins. */
    uint64_t lowest;   /* The lowest address mapped. */
    uint64_t highest;  /* One past the highest. */
    uint32_t segments; /* How many were loaded. */
    uint64_t pages;    /* How many pages were given to them. */
} ElfImage;

/*
 * Why a load failed, or that it did not.
 *
 * Each is a distinct refusal rather than one failure with a message, because
 * every one of them is something a caller may want to tell apart: an image that
 * is not an ELF file at all is a different fault from one that is an ELF file
 * for another machine, and both are different from one whose headers do not
 * describe themselves.
 */
typedef enum ElfResult
{
    ELF_OK = 0,
    ELF_NOT_ELF,          /* The identification does not begin 7Fh 'E' 'L' 'F'. */
    ELF_WRONG_CLASS,      /* Not a 64-bit object. */
    ELF_WRONG_ENCODING,   /* Not little endian. */
    ELF_WRONG_VERSION,    /* A version this loader does not know. */
    ELF_WRONG_MACHINE,    /* Compiled for another architecture. */
    ELF_WRONG_TYPE,       /* Not an executable: relocatable, or position independent. */
    ELF_MALFORMED,        /* A header that does not describe itself. */
    ELF_BAD_ADDRESS,      /* A segment outside what a user program may occupy. */
    ELF_NO_SEGMENTS,      /* Nothing to load. */
    ELF_NO_MEMORY,        /* A frame or a table could not be obtained. */
    ELF_UNREADABLE        /* The file could not be read. */
} ElfResult;

/* The name of a result, for a report and for a self-test. */
const char *ElfResultName(ElfResult result);

/*
 * Decodes the file header of an image held in memory.
 *
 * The fields are read byte by byte and assembled, never by laying a structure
 * over the bytes: CODING-STANDARDS.md. The format's byte order is
 * then visible in the decoder rather than invisible in a cast, and the image
 * need not be aligned for anything.
 *
 * Returns ELF_NOT_ELF and the like without touching the header where the image
 * is too short to hold one.
 */
ElfResult ElfDecode(const void *image, uint64_t length, ElfHeader *header);

/*
 * Establishes that an image is one this loader may load, without loading it.
 *
 * Every refusal is a fault an image could otherwise cause: a program header
 * table beyond the end of the file, a segment whose contents lie beyond it, a
 * segment claiming an address in the kernel's half, a size in the file greater
 * than the size in memory. See docs/design/EXECUTABLE.md.
 */
ElfResult ElfValidate(const void *image, uint64_t length);

/*
 * Loads the segments of an image into an address space and reports what it did.
 *
 * The pages are given to the space with the permissions the segment asked for,
 * and their contents are placed **through the direct physical map** rather than
 * by switching to the space: the loader never executes in the address space it
 * is filling, so the space need not be active and the kernel need not be
 * entered from it.
 *
 * The space is not cleaned up upon failure. A partly loaded space is the
 * caller's to destroy, and destroying it here would mean a loader that frees an
 * address space it did not create.
 */
ElfResult ElfLoad(AddressSpace *space, const void *image, uint64_t length,
                  ElfImage *loaded);

/*
 * The same, from a file reached through the virtual filesystem.
 *
 * The whole file is read into the kernel heap first, because the validation
 * examines offsets against a length and a loader that read the file piecewise
 * would be validating what it had already used. It is bounded: an image larger
 * than ELF_FILE_MAXIMUM is refused rather than read.
 */
#define ELF_FILE_MAXIMUM (1024U * 1024U)

ElfResult ElfLoadFile(AddressSpace *space, const char *path, ElfImage *loaded);

/* The program headers of the image most recently decoded, for a report and a
 * self-test; null beyond what was read. */
const ElfProgramHeader *ElfSegmentAt(size_t index);
size_t ElfSegmentCount(void);

/* Accounting. */
uint64_t ElfLoadCount(void);
uint64_t ElfRefusalCount(void);

/* Emits what the last image held. */
void ElfReport(void);

#endif /* OXYS_EXEC_ELF_H */
