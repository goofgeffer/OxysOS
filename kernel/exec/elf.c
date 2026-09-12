/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/exec/elf.c
 * Purpose: Implements the ELF64 loader for statically linked executables: the
 *          decoding of the file header and the program headers, the validation
 *          an image must survive before a page of it is mapped, and the placing
 *          of its segments into an address space.
 * Key functions: ElfDecode, ElfValidate, ElfLoad, ElfLoadFile, ElfResultName,
 *          ElfReport.
 * References: kernel/include/oxys/elf.h states every offset and constant this
 *          file relies upon, and docs/design/EXECUTABLE.md the reasoning.
 *
 * Nothing here is overlaid.
 *
 *   An ELF image is a format defined outside this project and read from a
 *   medium, so every field is read byte by byte and assembled rather than by
 *   laying a C structure over the bytes — CODING-STANDARDS.md, Section 7.1. The
 *   image is then not required to be aligned for anything, the format's byte
 *   order is visible in the decoder instead of invisible in a cast, and a header
 *   whose fields the compiler would have padded differently cannot be misread.
 *
 * The contents are placed through the direct physical map.
 *
 *   The loader never executes in the address space it is filling. Each page is a
 *   frame the loader allocates, writes through the direct map, and then gives to
 *   the space with the permissions the segment asked for. Switching to the space
 *   to write into it would be the obvious alternative and is worse in two ways:
 *   it would require the pages to be writable while being written, so a read-only
 *   segment could not be given its final permissions until afterwards, and it
 *   would put the kernel into an address space that is only half built.
 *
 * Validation happens before anything is mapped.
 *
 *   Not interleaved with the loading. An image is judged whole, and only then
 *   loaded, so that a malformed segment discovered half way through is not a
 *   half-loaded address space the caller must know to unpick. The one thing this
 *   costs is a second walk of the program headers, which are at most sixteen.
 */

#include <oxys/exec/elf.h>
#include <oxys/arch/mm/addrspace.h>
#include <oxys/mm/heap.h>
#include <oxys/kernel.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/mm/paging.h>
#include <oxys/mm/pmm.h>
#include <oxys/arch/syscall/syscall.h>
#include <oxys/fs/vfs.h>

/* The program headers of the image most recently decoded. */
static ElfProgramHeader ElfSegments[ELF_SEGMENT_MAXIMUM];
static size_t ElfSegmentsRead;
static ElfHeader ElfLastHeader;
static bool ElfHaveHeader;

static uint64_t ElfLoads;
static uint64_t ElfRefusals;

/* ------------------------------------------------------------------ decoding */

/*
 * The little-endian readers.
 *
 * A byte at a time and shifted into place, which is what makes the format's byte
 * order a statement in the code. The image is `const uint8_t *`, so no alignment
 * is required of it and none is assumed: an ELF image read into a heap buffer
 * lands wherever the allocator put it.
 */
static uint16_t ElfRead16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ElfRead32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t ElfRead64(const uint8_t *bytes)
{
    return (uint64_t)ElfRead32(bytes) | ((uint64_t)ElfRead32(bytes + 4U) << 32);
}

const char *ElfResultName(ElfResult result)
{
    switch (result)
    {
    case ELF_OK:
        return "loaded";
    case ELF_NOT_ELF:
        return "not an ELF image";
    case ELF_WRONG_CLASS:
        return "not a 64-bit object";
    case ELF_WRONG_ENCODING:
        return "not little endian";
    case ELF_WRONG_VERSION:
        return "an object file version this loader does not know";
    case ELF_WRONG_MACHINE:
        return "compiled for another architecture";
    case ELF_WRONG_TYPE:
        return "not a statically linked executable";
    case ELF_MALFORMED:
        return "headers that do not describe the file they are in";
    case ELF_BAD_ADDRESS:
        return "a segment outside what a user program may occupy";
    case ELF_NO_SEGMENTS:
        return "nothing to load";
    case ELF_NO_MEMORY:
        return "no memory for the segments";
    case ELF_UNREADABLE:
        return "the file could not be read";
    default:
        return "an unnamed failure";
    }
}

ElfResult ElfDecode(const void *image, uint64_t length, ElfHeader *header)
{
    const uint8_t *const bytes = (const uint8_t *)image;

    if ((image == NULL) || (header == NULL) || (length < ELF_HEADER_BYTES))
    {
        return ELF_NOT_ELF;
    }

    /*
     * The magic first, before any other field is even read. Everything below
     * interprets bytes at fixed offsets, and interpreting the offsets of a file
     * that is not an ELF file is reading arbitrary numbers out of arbitrary
     * data — which is exactly how a loader ends up mapping a segment described
     * by somebody's photograph.
     */
    if ((bytes[0] != 0x7FU) || (bytes[1] != 'E') || (bytes[2] != 'L') || (bytes[3] != 'F'))
    {
        return ELF_NOT_ELF;
    }

    header->class_code = bytes[4];
    header->data_encoding = bytes[5];
    header->identification_version = bytes[6];

    if (header->class_code != ELF_CLASS_64)
    {
        return ELF_WRONG_CLASS;
    }

    /*
     * The encoding is checked before any multi-byte field is assembled. The
     * readers above are little-endian by construction, so a big-endian image
     * decoded by them yields numbers that are wrong in a way nothing else here
     * would notice — a byte count of sixteen million rather than sixteen.
     */
    if (header->data_encoding != ELF_DATA_LITTLE_ENDIAN)
    {
        return ELF_WRONG_ENCODING;
    }

    if (header->identification_version != ELF_VERSION_CURRENT)
    {
        return ELF_WRONG_VERSION;
    }

    header->type = ElfRead16(&bytes[16]);
    header->machine = ElfRead16(&bytes[18]);
    header->version = ElfRead32(&bytes[20]);
    header->entry = ElfRead64(&bytes[24]);
    header->program_header_offset = ElfRead64(&bytes[32]);
    header->header_size = ElfRead16(&bytes[52]);
    header->program_header_size = ElfRead16(&bytes[54]);
    header->program_header_count = ElfRead16(&bytes[56]);

    if (header->version != ELF_VERSION_CURRENT)
    {
        return ELF_WRONG_VERSION;
    }

    if (header->machine != ELF_MACHINE_X86_64)
    {
        return ELF_WRONG_MACHINE;
    }

    /*
     * An executable and nothing else.
     *
     * A relocatable object has no segments to load at all. A shared object —
     * which is what a position-independent executable is — has segments whose
     * addresses are offsets from wherever it is placed, and placing it means
     * applying its relocations. This sub-task is the loader for *statically
     * linked* executables, and refusing the others by name is better than
     * loading one at the addresses it did not mean.
     */
    if (header->type != ELF_TYPE_EXECUTABLE)
    {
        return ELF_WRONG_TYPE;
    }

    return ELF_OK;
}

/* ---------------------------------------------------------------- validation */

/* Whether a range of the file lies wholly within it, without the sum wrapping. */
static bool ElfRangeIsWithinFile(uint64_t offset, uint64_t size, uint64_t length)
{
    /*
     * Written as a subtraction, for the reason the system call's validation is:
     * `offset + size` overflows for a size near the greatest value and the sum
     * is then smaller than the offset, so a range covering everything appears to
     * lie within the file.
     */
    return (offset <= length) && (size <= (length - offset));
}

/* Whether a range of the address space is one a user program may occupy. */
static bool ElfRangeIsWithinUserSpace(uint64_t address, uint64_t size)
{
    if (address >= SYSCALL_USER_LIMIT)
    {
        return false;
    }

    /*
     * The address is tested before the size, and an empty range at an address
     * outside the half is refused rather than admitted.
     *
     * The reverse order admitted any address whatever so long as the size was
     * zero — a segment nominally at a kernel address passed the check that
     * exists to keep a program out of the kernel's half. Nothing came of it,
     * because the loader's page arithmetic happened to produce an empty page
     * range for such a segment and map nothing; but a validation that is correct
     * only because of what its caller does with the answer is not a validation.
     */
    if (size == 0U)
    {
        return true;
    }

    /*
     * The page containing the last byte must also lie below the limit, so the
     * comparison is against the limit and not against the last address: a
     * segment ending in the last page of the user half would otherwise have its
     * final page rounded up past the boundary.
     */
    return size <= (SYSCALL_USER_LIMIT - address);
}

/*
 * Reads and judges the program headers.
 *
 * They are recorded as they are judged, so that a caller which has validated an
 * image may load it without decoding it a second time — and so that a report may
 * say what an image contained even when it was refused.
 */
static ElfResult ElfReadSegments(const uint8_t *bytes, uint64_t length,
                                 const ElfHeader *header)
{
    uint64_t previous_end = 0U;
    bool have_previous = false;

    ElfSegmentsRead = 0U;

    if (header->header_size != ELF_HEADER_BYTES)
    {
        return ELF_MALFORMED;
    }

    /*
     * The entry size is checked rather than assumed, because it is what the walk
     * below strides by. An image declaring a size of zero would have every
     * header read from the same offset; one declaring a larger size would have
     * them read from the wrong offsets entirely, and both produce numbers that
     * look like a program.
     */
    if (header->program_header_size != ELF_PROGRAM_HEADER_BYTES)
    {
        return ELF_MALFORMED;
    }

    if (header->program_header_count == 0U)
    {
        return ELF_NO_SEGMENTS;
    }

    if (header->program_header_count > ELF_SEGMENT_MAXIMUM)
    {
        return ELF_MALFORMED;
    }

    if (!ElfRangeIsWithinFile(header->program_header_offset,
                              (uint64_t)header->program_header_count *
                                  header->program_header_size,
                              length))
    {
        return ELF_MALFORMED;
    }

    for (uint16_t index = 0U; index < header->program_header_count; ++index)
    {
        const uint8_t *const entry =
            &bytes[header->program_header_offset +
                   ((uint64_t)index * header->program_header_size)];
        ElfProgramHeader segment;

        segment.type = ElfRead32(&entry[0]);
        segment.flags = ElfRead32(&entry[4]);
        segment.file_offset = ElfRead64(&entry[8]);
        segment.virtual_address = ElfRead64(&entry[16]);
        segment.file_size = ElfRead64(&entry[32]);
        segment.memory_size = ElfRead64(&entry[40]);
        segment.alignment = ElfRead64(&entry[48]);

        ElfSegments[ElfSegmentsRead] = segment;
        ++ElfSegmentsRead;

        if (segment.type != ELF_SEGMENT_LOAD)
        {
            /*
             * An interpreter is named rather than ignored. A program headed by
             * one is dynamically linked and expects something to load a library
             * for it before it runs; loading it regardless would produce a
             * program that reached its first call into that library and faulted.
             */
            if (segment.type == ELF_SEGMENT_INTERPRETER)
            {
                return ELF_WRONG_TYPE;
            }

            continue;
        }

        /* The contents must be in the file. */
        if (!ElfRangeIsWithinFile(segment.file_offset, segment.file_size, length))
        {
            return ELF_MALFORMED;
        }

        /*
         * A segment may be larger in memory than in the file — that is what a
         * zero-filled section is — but never smaller. The reverse would have the
         * loader copy more bytes than it had reserved pages for.
         */
        if (segment.file_size > segment.memory_size)
        {
            return ELF_MALFORMED;
        }

        if (!ElfRangeIsWithinUserSpace(segment.virtual_address, segment.memory_size))
        {
            return ELF_BAD_ADDRESS;
        }

        /*
         * No segment may occupy the first page of the address space.
         *
         * A program with a page mapped at address zero is one in which a null
         * pointer is a valid address: a dereference of one succeeds quietly and
         * reads whatever the segment put there, instead of raising the page
         * fault that is the only thing which makes a null pointer a detectable
         * mistake rather than a silent wrong answer. No toolchain produces such
         * an image; a file that asks for one is either damaged or is asking for
         * exactly that property, and neither is a request to grant.
         *
         * It also removes an ambiguity this loader would otherwise carry. The
         * lowest address of an image is recorded with zero standing for "none
         * recorded yet", so a segment genuinely at zero would leave the record
         * indistinguishable from an empty one — and the entry point is checked
         * against that record.
         */
        if (segment.virtual_address < PAGE_SIZE)
        {
            return ELF_BAD_ADDRESS;
        }

        /*
         * A loadable segment occupying no memory is skipped rather than loaded,
         * and skipped *here* — after every judgement upon its address and its
         * contents, and before it is allowed to influence anything.
         *
         * The generic ABI permits `p_memsz` to be zero, so such a segment is not
         * malformed and must not be refused; it simply has no memory image, and
         * a loader steps over it. This one must step over it for a reason of its
         * own besides: ElfLoadSegment computes the last page of a segment as
         * `virtual_address + (memory_size - 1)`, which underflows when the size
         * is zero. The wrapped sum yields a last page below the first for an
         * aligned address — harmless by luck — but for an *unaligned* one it
         * yields a last page equal to the first, and the loader maps a whole
         * frame for a segment that asked for no bytes at all, then records an
         * end address equal to its start, so the next segment may legitimately
         * claim a page this one has already mapped.
         *
         * The order matters and was got wrong first: skipping before the address
         * was judged let a segment nominally in the kernel's half through the
         * one check that exists to keep a program out of it. Nothing would have
         * come of it, the segment being skipped — but a validation that holds
         * only because of what is done with its answer is not one, and the
         * self-test now asserts this order rather than the outcome.
         *
         * It takes no part in the ascending-order check below, having no extent
         * to overlap with: `previous_end` is left where the last segment with a
         * memory image put it.
         */
        if (segment.memory_size == 0U)
        {
            continue;
        }

        /*
         * The segments must ascend and must not overlap.
         *
         * The specification requires loadable segments to appear in ascending
         * order of address, and this loader depends upon it: it reuses the page
         * a segment shares with the one before it, and a table out of order
         * would have it reuse a page belonging to a segment it had not reached.
         * Two segments claiming the same page with different permissions is a
         * question this loader would have to answer arbitrarily, so it is
         * refused instead.
         */
        if (have_previous && (segment.virtual_address < previous_end))
        {
            return ELF_MALFORMED;
        }

        previous_end = segment.virtual_address + segment.memory_size;
        have_previous = true;
    }

    return have_previous ? ELF_OK : ELF_NO_SEGMENTS;
}

ElfResult ElfValidate(const void *image, uint64_t length)
{
    ElfHeader header;
    const ElfResult decoded = ElfDecode(image, length, &header);

    if (decoded != ELF_OK)
    {
        ElfHaveHeader = false;
        return decoded;
    }

    ElfLastHeader = header;
    ElfHaveHeader = true;

    return ElfReadSegments((const uint8_t *)image, length, &header);
}

/* ------------------------------------------------------------------ loading */

/*
 * Places one segment into the address space.
 *
 * `shared` names the page the previous segment ended upon, if any, and the frame
 * behind it. A segment beginning within that page writes into that frame rather
 * than mapping a second one over it — which is not an optimisation but a
 * correctness requirement: mapping a fresh frame would discard the bytes of the
 * segment before it that lie in the same page, and the boundary between the text
 * and the data of an ordinary program falls in the middle of a page far more
 * often than not.
 */
static ElfResult ElfLoadSegment(AddressSpace *space, const uint8_t *bytes,
                                const ElfProgramHeader *segment, uint64_t *shared_page,
                                PhysicalAddress *shared_frame, ElfImage *loaded)
{
    const uint64_t first = AlignDown(segment->virtual_address, PAGE_SIZE);
    const uint64_t last = AlignDown(
        segment->virtual_address + (segment->memory_size - 1U), PAGE_SIZE);
    uint64_t flags = PAGE_ENTRY_USER;

    if ((segment->flags & ELF_SEGMENT_WRITE) != 0U)
    {
        flags |= PAGE_ENTRY_WRITABLE;
    }

    for (uint64_t page = first; page <= last; page += PAGE_SIZE)
    {
        PhysicalAddress frame;
        uint8_t *destination;
        uint64_t within;
        uint64_t copy;
        bool reused = false;

        if ((*shared_frame != 0U) && (page == *shared_page))
        {
            frame = *shared_frame;
            reused = true;
        }
        else
        {
            frame = FrameAllocate();

            if (frame == 0U)
            {
                return ELF_NO_MEMORY;
            }

            destination = (uint8_t *)(uintptr_t)PhysicalToDirect(frame);

            /*
             * Zeroed before anything is copied into it, and every page of the
             * segment and not merely the ones beyond the file's contents. A
             * frame arrives holding whatever the last owner left in it, and a
             * page given to a user program with the kernel's leavings still in
             * it discloses them — the bytes beyond a segment's file size being
             * exactly what a program is entitled to read as zero.
             */
            for (uint64_t offset = 0U; offset < PAGE_SIZE; ++offset)
            {
                destination[offset] = 0U;
            }
        }

        destination = (uint8_t *)(uintptr_t)PhysicalToDirect(frame);

        /* Where this page begins within the segment, and how much of the file's
         * contents falls upon it. */
        within = (page > segment->virtual_address) ? (page - segment->virtual_address) : 0U;

        if (within < segment->file_size)
        {
            const uint64_t start =
                (page > segment->virtual_address) ? 0U
                                                  : (segment->virtual_address - page);

            copy = segment->file_size - within;

            if (copy > (PAGE_SIZE - start))
            {
                copy = PAGE_SIZE - start;
            }

            for (uint64_t offset = 0U; offset < copy; ++offset)
            {
                destination[start + offset] = bytes[segment->file_offset + within + offset];
            }
        }

        if (!reused)
        {
            AddressSpaceMapPage(space, page, frame, flags);
            ++loaded->pages;
        }
        else if ((flags & PAGE_ENTRY_WRITABLE) != 0U)
        {
            /*
             * A shared page takes the more permissive of the two segments'
             * permissions. It holds bytes belonging to both, and a page the
             * writable segment cannot write to is a program whose first store to
             * its own data faults.
             */
            AddressSpaceMapPage(space, page, frame, flags);
        }

        *shared_page = page;
        *shared_frame = frame;
    }

    if ((loaded->lowest == 0U) || (segment->virtual_address < loaded->lowest))
    {
        loaded->lowest = segment->virtual_address;
    }

    if ((segment->virtual_address + segment->memory_size) > loaded->highest)
    {
        loaded->highest = segment->virtual_address + segment->memory_size;
    }

    ++loaded->segments;

    return ELF_OK;
}

ElfResult ElfLoad(AddressSpace *space, const void *image, uint64_t length,
                  ElfImage *loaded)
{
    const uint8_t *const bytes = (const uint8_t *)image;
    uint64_t shared_page = 0U;
    PhysicalAddress shared_frame = 0U;
    ElfResult result;

    if ((space == NULL) || (loaded == NULL))
    {
        ++ElfRefusals;
        return ELF_MALFORMED;
    }

    loaded->entry = 0U;
    loaded->lowest = 0U;
    loaded->highest = 0U;
    loaded->segments = 0U;
    loaded->pages = 0U;

    /* Judged whole, and only then loaded. */
    result = ElfValidate(image, length);

    if (result != ELF_OK)
    {
        ++ElfRefusals;
        return result;
    }

    for (size_t index = 0U; index < ElfSegmentsRead; ++index)
    {
        if (ElfSegments[index].type != ELF_SEGMENT_LOAD)
        {
            continue;
        }

        /*
         * Skipped for the reason ElfReadSegments skips it, and skipped here as
         * well because the two walks are separate: the validation steps over a
         * segment of no memory size, and this loop would otherwise load the very
         * segment the validation declined to judge.
         */
        if (ElfSegments[index].memory_size == 0U)
        {
            continue;
        }

        result = ElfLoadSegment(space, bytes, &ElfSegments[index], &shared_page,
                                &shared_frame, loaded);

        if (result != ELF_OK)
        {
            ++ElfRefusals;
            return result;
        }
    }

    /*
     * The entry must lie within what was loaded.
     *
     * An entry point outside every segment is a program whose first instruction
     * fetch faults, and the fault would be reported against an address nothing
     * in the image accounts for. It is checked here rather than in the
     * validation because it is checked against what the segments actually
     * covered, which the validation has not yet computed.
     */
    if ((ElfLastHeader.entry < loaded->lowest) || (ElfLastHeader.entry >= loaded->highest))
    {
        ++ElfRefusals;
        return ELF_BAD_ADDRESS;
    }

    loaded->entry = ElfLastHeader.entry;
    ++ElfLoads;

    return ELF_OK;
}

ElfResult ElfLoadFile(AddressSpace *space, const char *path, ElfImage *loaded)
{
    VfsAttributes status;
    void *buffer;
    uint64_t read = 0U;
    int descriptor;
    ElfResult result;

    if ((space == NULL) || (path == NULL) || (loaded == NULL))
    {
        return ELF_MALFORMED;
    }

    if (!VfsStat(path, &status))
    {
        return ELF_UNREADABLE;
    }

    if ((status.size == 0U) || (status.size > ELF_FILE_MAXIMUM))
    {
        return ELF_MALFORMED;
    }

    buffer = KernelAllocate((size_t)status.size);

    if (buffer == NULL)
    {
        return ELF_NO_MEMORY;
    }

    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        KernelFree(buffer);
        return ELF_UNREADABLE;
    }

    if (!VfsRead(descriptor, buffer, status.size, &read) || (read != status.size))
    {
        (void)VfsClose(descriptor);
        KernelFree(buffer);
        return ELF_UNREADABLE;
    }

    (void)VfsClose(descriptor);

    result = ElfLoad(space, buffer, status.size, loaded);

    /*
     * The buffer is released whatever happened. The segments were copied into
     * frames of their own, so nothing of the image is still referred to once the
     * load has returned.
     */
    KernelFree(buffer);

    return result;
}

/* --------------------------------------------------------------- accessors */

const ElfProgramHeader *ElfSegmentAt(size_t index)
{
    return (index < ElfSegmentsRead) ? &ElfSegments[index] : NULL;
}

size_t ElfSegmentCount(void)
{
    return ElfSegmentsRead;
}

uint64_t ElfLoadCount(void)
{
    return ElfLoads;
}

uint64_t ElfRefusalCount(void)
{
    return ElfRefusals;
}

void ElfReport(void)
{
    if (!ElfHaveHeader)
    {
        KernelWriteString("ELF: no image has been examined.\n");
        return;
    }

    KernelWriteString("ELF: last image type ");
    KernelWriteDecimal((uint64_t)ElfLastHeader.type);
    KernelWriteString(", machine ");
    KernelWriteDecimal((uint64_t)ElfLastHeader.machine);
    KernelWriteString(", entry ");
    KernelWriteHexadecimal(ElfLastHeader.entry);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)ElfSegmentsRead);
    KernelWriteString(" program header(s).\n");

    for (size_t index = 0U; index < ElfSegmentsRead; ++index)
    {
        const ElfProgramHeader *const segment = &ElfSegments[index];

        if (segment->type != ELF_SEGMENT_LOAD)
        {
            continue;
        }

        KernelWriteString("  load at ");
        KernelWriteHexadecimal(segment->virtual_address);
        KernelWriteString(", ");
        KernelWriteDecimal(segment->file_size);
        KernelWriteString(" bytes of ");
        KernelWriteDecimal(segment->memory_size);
        KernelWriteString(", ");
        KernelWriteString(((segment->flags & ELF_SEGMENT_READ) != 0U) ? "r" : "-");
        KernelWriteString(((segment->flags & ELF_SEGMENT_WRITE) != 0U) ? "w" : "-");
        KernelWriteString(((segment->flags & ELF_SEGMENT_EXECUTE) != 0U) ? "x" : "-");
        KernelWriteString("\n");
    }

    KernelWriteString("ELF: images loaded ");
    KernelWriteDecimal(ElfLoads);
    KernelWriteString(", refused ");
    KernelWriteDecimal(ElfRefusals);
    KernelWriteString(".\n");
}
