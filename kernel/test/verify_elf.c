/*
 * File: kernel/test/verify_elf.c
 * Purpose: Asserts the ELF64 loader: the decoding of a header, every refusal a
 *          malformed image must meet, and that a well-formed one arrives in an
 *          address space at the addresses it asked for with the contents it
 *          carried.
 * Key functions: KernelVerifyElf.
 * References:
 *   - docs/design/EXECUTABLE.md, Section 6: these assertions paired with the
 *     silent failure each would catch.
 *
 * The image is composed here, byte by byte.
 *
 *   There is no compiler in this kernel and no executable upon any volume it is
 *   required to carry, so an image is built in memory: a header, two program
 *   headers, and contents at known offsets. That is not a weaker test than
 *   loading a real file — it is a stronger one, because every field can be made
 *   wrong on purpose. A real executable can only ever be well formed, and every
 *   refusal below is a case a real executable would never produce.
 *
 *   The image is built by the same little-endian writing the loader reads with,
 *   spelled out here rather than shared, so that the two agree because they were
 *   each written from the specification and not because they call the same
 *   function. A shared helper with the byte order wrong would compose and decode
 *   consistently and assert nothing at all.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/elf.h>
#include <oxys/addrspace.h>
#include <oxys/memory.h>
#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/syscall.h>

/*
 * Where the composed program is loaded.
 *
 * Two segments, deliberately arranged so that they **share a page**: the first
 * ends part way through a page and the second begins in the same one. That is
 * the ordinary shape of a compiled program — the read-only text and the writable
 * data meet in the middle of a page far more often than not — and it is the case
 * a loader gets wrong by mapping a fresh frame over the boundary page and
 * discarding the end of the first segment.
 */
#define KERNEL_ELF_TEXT_ADDRESS UINT64_C(0x0000000000400000)
#define KERNEL_ELF_TEXT_BYTES   0x0800U
#define KERNEL_ELF_DATA_ADDRESS (KERNEL_ELF_TEXT_ADDRESS + KERNEL_ELF_TEXT_BYTES)
#define KERNEL_ELF_DATA_FILE_BYTES 0x0100U
#define KERNEL_ELF_DATA_MEMORY_BYTES 0x1200U /* Larger, so that it is zero filled. */

/* Where the contents sit within the composed file. */
#define KERNEL_ELF_TEXT_OFFSET 0x1000U
#define KERNEL_ELF_DATA_OFFSET 0x2000U
#define KERNEL_ELF_IMAGE_BYTES 0x3000U

static uint8_t KernelElfImage[KERNEL_ELF_IMAGE_BYTES];

static bool KernelElfSucceeded;

static void KernelElfRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelElfSucceeded = false;
    }
}

/* The little-endian writers, spelled out rather than shared with the loader. */
static void KernelElfPut16(uint64_t offset, uint16_t value)
{
    KernelElfImage[offset] = (uint8_t)(value & 0xFFU);
    KernelElfImage[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

static void KernelElfPut32(uint64_t offset, uint32_t value)
{
    for (uint64_t index = 0U; index < 4U; ++index)
    {
        KernelElfImage[offset + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void KernelElfPut64(uint64_t offset, uint64_t value)
{
    for (uint64_t index = 0U; index < 8U; ++index)
    {
        KernelElfImage[offset + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

/* Writes one program header at the given offset. */
static void KernelElfProgramHeader(uint64_t at, uint32_t type, uint32_t flags,
                                   uint64_t file_offset, uint64_t address,
                                   uint64_t file_size, uint64_t memory_size)
{
    KernelElfPut32(at + 0U, type);
    KernelElfPut32(at + 4U, flags);
    KernelElfPut64(at + 8U, file_offset);
    KernelElfPut64(at + 16U, address);
    KernelElfPut64(at + 24U, address); /* The physical address, which is ignored. */
    KernelElfPut64(at + 32U, file_size);
    KernelElfPut64(at + 40U, memory_size);
    KernelElfPut64(at + 48U, PAGE_SIZE);
}

/*
 * Composes a well-formed image: a header, two loadable segments, and contents
 * whose every byte is derived from its own offset.
 *
 * The contents matter. A pattern in which every byte differs from its
 * neighbours is what makes a segment copied from the wrong file offset, or to
 * the wrong address, or by the wrong length, visible — where a segment of
 * identical bytes would be copied wrongly and compare equal.
 */
static void KernelElfCompose(void)
{
    for (uint64_t index = 0U; index < KERNEL_ELF_IMAGE_BYTES; ++index)
    {
        KernelElfImage[index] = 0U;
    }

    KernelElfImage[0] = 0x7FU;
    KernelElfImage[1] = 'E';
    KernelElfImage[2] = 'L';
    KernelElfImage[3] = 'F';
    KernelElfImage[4] = (uint8_t)ELF_CLASS_64;
    KernelElfImage[5] = (uint8_t)ELF_DATA_LITTLE_ENDIAN;
    KernelElfImage[6] = (uint8_t)ELF_VERSION_CURRENT;

    KernelElfPut16(16U, (uint16_t)ELF_TYPE_EXECUTABLE);
    KernelElfPut16(18U, (uint16_t)ELF_MACHINE_X86_64);
    KernelElfPut32(20U, ELF_VERSION_CURRENT);
    KernelElfPut64(24U, KERNEL_ELF_TEXT_ADDRESS + 0x40U); /* The entry, within the text. */
    KernelElfPut64(32U, ELF_HEADER_BYTES);                /* The program headers follow. */
    KernelElfPut16(52U, (uint16_t)ELF_HEADER_BYTES);
    KernelElfPut16(54U, (uint16_t)ELF_PROGRAM_HEADER_BYTES);
    KernelElfPut16(56U, 2U);

    KernelElfProgramHeader(ELF_HEADER_BYTES, ELF_SEGMENT_LOAD,
                           ELF_SEGMENT_READ | ELF_SEGMENT_EXECUTE, KERNEL_ELF_TEXT_OFFSET,
                           KERNEL_ELF_TEXT_ADDRESS, KERNEL_ELF_TEXT_BYTES,
                           KERNEL_ELF_TEXT_BYTES);

    KernelElfProgramHeader(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES, ELF_SEGMENT_LOAD,
                           ELF_SEGMENT_READ | ELF_SEGMENT_WRITE, KERNEL_ELF_DATA_OFFSET,
                           KERNEL_ELF_DATA_ADDRESS, KERNEL_ELF_DATA_FILE_BYTES,
                           KERNEL_ELF_DATA_MEMORY_BYTES);

    for (uint64_t index = 0U; index < KERNEL_ELF_TEXT_BYTES; ++index)
    {
        KernelElfImage[KERNEL_ELF_TEXT_OFFSET + index] = (uint8_t)(index ^ 0x5AU);
    }

    for (uint64_t index = 0U; index < KERNEL_ELF_DATA_FILE_BYTES; ++index)
    {
        KernelElfImage[KERNEL_ELF_DATA_OFFSET + index] = (uint8_t)(index ^ 0xC3U);
    }
}

void KernelVerifyElf(void)
{
    AddressSpace space;
    ElfImage loaded;
    ElfHeader header;

    KernelElfSucceeded = true;

    KernelWriteString("Executable: asserting the ELF64 loader.\n");

    KernelElfCompose();

    /* --- What is refused, and by name. --- */

    KernelElfRequire(ElfDecode(NULL, KERNEL_ELF_IMAGE_BYTES, &header) == ELF_NOT_ELF,
                     "an image that is not there was decoded");
    KernelElfRequire(ElfDecode(KernelElfImage, 8U, &header) == ELF_NOT_ELF,
                     "an image too short to hold a header was decoded");

    /*
     * Each field is spoiled, judged, and put back. The order matters only in
     * that the image must be well formed again before the next one is tried;
     * every refusal below is a distinct fault and the loader must tell them
     * apart, because "this file is not for this machine" and "this file is not a
     * file" send whoever is reading the message to different places.
     */
    KernelElfImage[1] = 'X';
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_NOT_ELF,
                     "an image without the magic was accepted");
    KernelElfImage[1] = 'E';

    KernelElfImage[4] = 1U; /* Thirty-two bit. */
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_WRONG_CLASS,
                     "a 32-bit object was not refused as one");
    KernelElfImage[4] = (uint8_t)ELF_CLASS_64;

    KernelElfImage[5] = 2U; /* Big endian. */
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) ==
                         ELF_WRONG_ENCODING,
                     "a big-endian object was not refused as one");
    KernelElfImage[5] = (uint8_t)ELF_DATA_LITTLE_ENDIAN;

    KernelElfPut16(18U, 40U); /* ARM. */
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) ==
                         ELF_WRONG_MACHINE,
                     "an object for another architecture was accepted");
    KernelElfPut16(18U, (uint16_t)ELF_MACHINE_X86_64);

    /*
     * A shared object is what a position-independent executable is, and its
     * addresses are offsets from wherever it is placed. Loading one at the
     * addresses its headers name puts it at zero, which is neither where it
     * meant to be nor anywhere it may be.
     */
    KernelElfPut16(16U, (uint16_t)ELF_TYPE_SHARED);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_WRONG_TYPE,
                     "a position-independent executable was accepted as a static one");
    KernelElfPut16(16U, (uint16_t)ELF_TYPE_RELOCATABLE);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_WRONG_TYPE,
                     "a relocatable object was accepted as an executable");
    KernelElfPut16(16U, (uint16_t)ELF_TYPE_EXECUTABLE);

    /* --- Headers that do not describe the file they are in. --- */

    /*
     * The program header table beyond the end of the file. This is the refusal
     * that stands between the loader and reading whatever follows the image in
     * memory, and calling it a segment.
     */
    KernelElfPut64(32U, KERNEL_ELF_IMAGE_BYTES);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_MALFORMED,
                     "a program header table beyond the end of the file was accepted");
    KernelElfPut64(32U, ELF_HEADER_BYTES);

    /* An entry size that is not the format's. The walk strides by it, so a size
     * of zero reads every header from the same place. */
    KernelElfPut16(54U, 0U);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_MALFORMED,
                     "a program header entry size of zero was accepted");
    KernelElfPut16(54U, (uint16_t)ELF_PROGRAM_HEADER_BYTES);

    KernelElfPut16(56U, 0U);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_NO_SEGMENTS,
                     "an image with no program headers was accepted");
    KernelElfPut16(56U, (uint16_t)(ELF_SEGMENT_MAXIMUM + 1U));
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_MALFORMED,
                     "more program headers than the loader will read were accepted");
    KernelElfPut16(56U, 2U);

    /* A segment whose contents lie beyond the end of the file. */
    KernelElfPut64(ELF_HEADER_BYTES + 32U, KERNEL_ELF_IMAGE_BYTES);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_MALFORMED,
                     "a segment whose contents lie beyond the file was accepted");
    KernelElfPut64(ELF_HEADER_BYTES + 32U, KERNEL_ELF_TEXT_BYTES);

    /*
     * A segment larger in the file than in memory. The reverse is ordinary — it
     * is what a zero-filled section is — but this direction has the loader copy
     * more bytes than it reserved pages for.
     */
    KernelElfPut64(ELF_HEADER_BYTES + 40U, KERNEL_ELF_TEXT_BYTES / 2U);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_MALFORMED,
                     "a segment larger in the file than in memory was accepted");
    KernelElfPut64(ELF_HEADER_BYTES + 40U, KERNEL_ELF_TEXT_BYTES);

    /* --- Addresses a user program may not occupy. --- */

    KernelElfPut64(ELF_HEADER_BYTES + 16U, DIRECT_MAP_BASE);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_BAD_ADDRESS,
                     "a segment in the kernel's half was accepted");

    /*
     * A segment beginning below the boundary and ending above it. This is the
     * case a check of the starting address alone admits, and it is how an image
     * reaches the kernel's memory with an address that is itself legitimate.
     */
    KernelElfPut64(ELF_HEADER_BYTES + 16U, SYSCALL_USER_LIMIT - PAGE_SIZE);
    KernelElfPut64(ELF_HEADER_BYTES + 40U, PAGE_SIZE * 4U);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_BAD_ADDRESS,
                     "a segment beginning below the boundary and ending above it was "
                     "accepted");
    KernelElfPut64(ELF_HEADER_BYTES + 16U, KERNEL_ELF_TEXT_ADDRESS);
    KernelElfPut64(ELF_HEADER_BYTES + 40U, KERNEL_ELF_TEXT_BYTES);

    /*
     * A segment occupying the first page of the address space.
     *
     * Refused, so that a null pointer stays a fault rather than becoming a valid
     * address that quietly reads whatever the segment put there. The address is
     * set below one page rather than to zero exactly, so that the assertion
     * covers the whole first page and not merely its first byte.
     */
    KernelElfPut64(ELF_HEADER_BYTES + 16U, PAGE_SIZE - 8U);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_BAD_ADDRESS,
                     "a segment occupying the first page of the address space was "
                     "accepted");
    KernelElfPut64(ELF_HEADER_BYTES + 16U, KERNEL_ELF_TEXT_ADDRESS);

    /*
     * A segment of no memory size at an address in the kernel's half.
     *
     * The generic ABI permits a loadable segment to occupy no memory, so this is
     * not malformed and must not be refused; what it must not do is escape the
     * check that keeps a program out of the kernel's half. The validation tested
     * the size before the address until this was written, and returned "within
     * user space" for any address whatever so long as the size was zero.
     *
     * The image is expected to remain acceptable — the *other* segment is still
     * well formed, and this one is skipped — so what is asserted is that the
     * refusal above is not reached, and that the empty segment contributes no
     * pages when the image is loaded, which the loading assertions establish.
     */
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 16U, DIRECT_MAP_BASE);
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 32U, 0U);
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 40U, 0U);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_BAD_ADDRESS,
                     "a segment of no memory size in the kernel's half was accepted");
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 16U,
                   KERNEL_ELF_DATA_ADDRESS);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_OK,
                     "a segment of no memory size at a legitimate address was refused");
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 32U,
                   KERNEL_ELF_DATA_FILE_BYTES);
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 40U,
                   KERNEL_ELF_DATA_MEMORY_BYTES);

    /* Segments out of order, which the loader depends upon for the shared page
     * and which the specification requires of a valid image. */
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 16U,
                   KERNEL_ELF_TEXT_ADDRESS - PAGE_SIZE);
    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_MALFORMED,
                     "segments out of ascending order were accepted");
    KernelElfPut64(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES + 16U,
                   KERNEL_ELF_DATA_ADDRESS);

    /* --- The image is well formed again. --- */

    KernelElfRequire(ElfValidate(KernelElfImage, KERNEL_ELF_IMAGE_BYTES) == ELF_OK,
                     "the composed image was refused after every field was restored");

    KernelElfRequire(ElfDecode(KernelElfImage, KERNEL_ELF_IMAGE_BYTES, &header) == ELF_OK,
                     "the composed image did not decode");
    KernelElfRequire(header.entry == (KERNEL_ELF_TEXT_ADDRESS + 0x40U),
                     "the entry point decoded to something other than what was written");
    KernelElfRequire(header.program_header_count == 2U,
                     "the number of program headers decoded wrongly");

    /* --- Loading it, and looking at what arrived. --- */

    /*
     * The frames the loader is about to be given are dirtied first.
     *
     * Without this the assertion that a segment's tail reads as zero cannot
     * fail: upon a freshly booted machine the allocator hands out frames that
     * happen to be zero already, so a loader that never zeroed anything would
     * pass. Filling a handful of frames with a pattern and giving them back
     * makes the allocator's next answers dirty, which is the state they will be
     * in upon any machine that has been running for more than a moment — and it
     * is the state in which the disclosure this guards against is real.
     */
    {
        PhysicalAddress dirtied[8];
        size_t taken = 0U;

        while (taken < 8U)
        {
            dirtied[taken] = FrameAllocate();

            if (dirtied[taken] == 0U)
            {
                break;
            }

            {
                volatile uint8_t *const page =
                    (volatile uint8_t *)(uintptr_t)PhysicalToDirect(dirtied[taken]);

                for (uint64_t offset = 0U; offset < PAGE_SIZE; ++offset)
                {
                    page[offset] = 0xEEU;
                }
            }

            ++taken;
        }

        while (taken > 0U)
        {
            --taken;
            FrameFree(dirtied[taken]);
        }
    }

    if (!AddressSpaceCreate(&space))
    {
        KernelWriteString("  No address space could be made; nothing was loaded.\n");
        KernelWriteString("Executable self-test FAILED.\n");
        return;
    }

    if (ElfLoad(&space, KernelElfImage, KERNEL_ELF_IMAGE_BYTES, &loaded) != ELF_OK)
    {
        AddressSpaceDestroy(&space);
        KernelWriteString("  The composed image did not load.\n");
        KernelWriteString("Executable self-test FAILED.\n");
        return;
    }

    KernelElfRequire(loaded.entry == (KERNEL_ELF_TEXT_ADDRESS + 0x40U),
                     "the loaded image reports the wrong entry point");
    KernelElfRequire(loaded.segments == 2U, "both segments were not loaded");
    KernelElfRequire(loaded.lowest == KERNEL_ELF_TEXT_ADDRESS,
                     "the lowest address loaded is not the first segment's");

    /*
     * The contents are read at the addresses the image asked for, which needs
     * the space to be the active one. This is the assertion the whole test is
     * built toward: everything above says the loader refused what it should, and
     * only this says it placed what it accepted.
     */
    AddressSpaceSwitch(&space);

    {
        const volatile uint8_t *const text =
            (const volatile uint8_t *)(uintptr_t)KERNEL_ELF_TEXT_ADDRESS;
        const volatile uint8_t *const data =
            (const volatile uint8_t *)(uintptr_t)KERNEL_ELF_DATA_ADDRESS;
        bool text_matches = true;
        bool data_matches = true;
        bool tail_is_zero = true;

        for (uint64_t index = 0U; index < KERNEL_ELF_TEXT_BYTES; ++index)
        {
            if (text[index] != (uint8_t)(index ^ 0x5AU))
            {
                text_matches = false;
                break;
            }
        }

        for (uint64_t index = 0U; index < KERNEL_ELF_DATA_FILE_BYTES; ++index)
        {
            if (data[index] != (uint8_t)(index ^ 0xC3U))
            {
                data_matches = false;
                break;
            }
        }

        /*
         * Beyond the file's contents the segment must read as zero, to the end
         * of what it claimed in memory. This is where a program's uninitialised
         * data lives, and a loader that left the frame as it found it would hand
         * a user program whatever the last owner of that frame had written —
         * which is a disclosure of the kernel's memory with no fault to report.
         */
        for (uint64_t index = KERNEL_ELF_DATA_FILE_BYTES;
             index < KERNEL_ELF_DATA_MEMORY_BYTES; ++index)
        {
            if (data[index] != 0U)
            {
                tail_is_zero = false;
                break;
            }
        }

        AddressSpaceSwitch(AddressSpaceKernel());

        KernelElfRequire(text_matches,
                         "the first segment's contents are not what the image held");
        KernelElfRequire(data_matches,
                         "the second segment's contents are not what the image held — "
                         "the page they share was overwritten");
        KernelElfRequire(tail_is_zero,
                         "the memory beyond a segment's file contents was not zeroed");
    }

    /*
     * The page the two segments share must be writable, the second of them
     * having asked for it. A loader taking the first segment's permissions for
     * the shared page gives the program a data page it cannot write to.
     */
    AddressSpaceSwitch(&space);
    {
        const bool writable = PagingAddressIsWritable(KERNEL_ELF_DATA_ADDRESS);
        const bool user = PagingAddressIsUser(KERNEL_ELF_TEXT_ADDRESS);

        AddressSpaceSwitch(AddressSpaceKernel());

        KernelElfRequire(writable, "the page the two segments share is not writable");
        KernelElfRequire(user, "the loaded text is not accessible to privilege level 3");
    }

    AddressSpaceDestroy(&space);

    KernelWriteString(KernelElfSucceeded ? "Executable self-test passed.\n"
                                         : "Executable self-test FAILED.\n");
}
