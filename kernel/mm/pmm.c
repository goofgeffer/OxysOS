/*
 * File: kernel/mm/pmm.c
 * Purpose: Implements the physical frame allocator as a bitmap, one bit per
 *          4 KiB frame, in which a set bit denotes a frame that is allocated or
 *          reserved and a clear bit denotes one that is available.
 * Key functions: PhysicalMemoryInitialise, FrameAllocate, FrameAllocateBelow,
 *          FrameFree, PhysicalMemoryReport, FrameMarkRange, FrameReleaseRange,
 *          PhysicalMemoryPlaceBitmap.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the 4 KiB frame is the unit of physical allocation.
 *   - Multiboot2 Specification 2.0, Section 3.6.8: the memory map, and its
 *     warning that the map "includes the regions occupied by kernel, mbi,
 *     segments and modules" and that the kernel must not overwrite them.
 *   - docs/design/MEMORY-LAYOUT.md, Section 6: the reserved extents and their reasons.
 *
 * Design note. A bitmap is chosen in preference to a free-frame stack because it
 * permits a frame to be reserved by address, which the initialisation sequence
 * requires: the kernel image, the boot information structure and the bitmap
 * itself all lie within regions the memory map classifies as usable, and must be
 * excluded after those regions have been released. A stack would offer constant
 * time allocation but no means of removing a particular frame from the middle.
 *
 * Concurrency. This implementation is not yet safe against concurrent access.
 * From sub-task 6.8 the bitmap must be protected by a spinlock, since several
 * processors will allocate simultaneously. The single mutable index
 * FrameSearchHint is the other structure that will require protection.
 */

#include <oxys/pmm.h>
#include <oxys/kernel.h>
#include <oxys/heap.h>

/* The number of frames represented by one element of the bitmap. */
#define FRAME_BITS_PER_WORD 64U

/*
 * The bitmap, addressed through the higher-half mapping. A set bit denotes a
 * frame that is allocated or reserved.
 */
static uint64_t *FrameBitmap;

/* The number of frames the allocator governs. */
static size_t FrameCount;

/* The number of 64-bit words the bitmap occupies. */
static size_t FrameBitmapWordCount;

/* The number of frames presently available. */
static size_t FrameFreeFrames;

/*
 * The index of the frame at which the next search begins. Allocation is
 * overwhelmingly sequential, so beginning where the previous search succeeded
 * avoids rescanning the low frames on every call.
 */
static size_t FrameSearchHint;

/*
 * The reference count of every frame, or NULL until reference counting is
 * established. A count of zero denotes a free frame.
 *
 * The width is 16 bits, which bounds the number of address spaces that may share
 * one frame at 65535. That is far beyond any plausible degree of sharing, and an
 * attempt to exceed it is reported rather than allowed to wrap, since a wrapped
 * count would free a frame that is still in use.
 */
static uint16_t *FrameReferenceTable;

/* The greatest reference count reached, for reporting. */
static uint16_t FrameReferenceHighWaterMark;

/* The physical extent of the bitmap itself, retained so that it may be reserved. */
static PhysicalAddress FrameBitmapPhysicalStart;
static PhysicalAddress FrameBitmapPhysicalEnd;

/* Converts a physical address to the index of the frame containing it. */
static size_t FrameIndexOf(PhysicalAddress address)
{
    return (size_t)(address >> PAGE_SHIFT);
}

/* Converts a frame index to the physical address of that frame's base. */
static PhysicalAddress FrameAddressOf(size_t index)
{
    return (PhysicalAddress)index << PAGE_SHIFT;
}

/* Reports whether the frame of the given index is allocated or reserved. */
static bool FrameIsUsed(size_t index)
{
    return (FrameBitmap[index / FRAME_BITS_PER_WORD] &
            (UINT64_C(1) << (index % FRAME_BITS_PER_WORD))) != 0U;
}

/* Marks one frame as allocated or reserved, adjusting the free count. */
static void FrameMarkUsed(size_t index)
{
    if (!FrameIsUsed(index))
    {
        FrameBitmap[index / FRAME_BITS_PER_WORD] |=
            (UINT64_C(1) << (index % FRAME_BITS_PER_WORD));
        --FrameFreeFrames;
    }
}

/* Marks one frame as available, adjusting the free count. */
static void FrameMarkFree(size_t index)
{
    if (FrameIsUsed(index))
    {
        FrameBitmap[index / FRAME_BITS_PER_WORD] &=
            ~(UINT64_C(1) << (index % FRAME_BITS_PER_WORD));
        ++FrameFreeFrames;
    }
}

/*
 * Marks every frame intersecting the physical range [start, end) as reserved.
 * The start is rounded down and the end upward, so that a partially occupied
 * frame is reserved in its entirety; issuing the remainder of such a frame would
 * hand a caller memory that is already in use.
 */
static void FrameMarkRange(PhysicalAddress start, PhysicalAddress end)
{
    size_t first_index;
    size_t last_index;

    if (end <= start)
    {
        return;
    }

    first_index = FrameIndexOf(AlignDown(start, PAGE_SIZE));
    last_index = FrameIndexOf(AlignUp(end, PAGE_SIZE) - 1U);

    if (first_index >= FrameCount)
    {
        return;
    }

    if (last_index >= FrameCount)
    {
        last_index = FrameCount - 1U;
    }

    for (size_t index = first_index; index <= last_index; ++index)
    {
        FrameMarkUsed(index);
    }
}

/*
 * Releases every frame wholly contained within the physical range [start, end).
 * In contrast with FrameMarkRange, the start is rounded upward and the end
 * downward, so that a frame only partially covered by a usable region is not
 * released; the remainder of such a frame belongs to an adjacent region whose
 * classification may be reserved.
 */
static void FrameReleaseRange(PhysicalAddress start, PhysicalAddress end)
{
    PhysicalAddress aligned_start = AlignUp(start, PAGE_SIZE);
    PhysicalAddress aligned_end = AlignDown(end, PAGE_SIZE);
    size_t first_index;
    size_t last_index;

    if (aligned_end <= aligned_start)
    {
        return;
    }

    first_index = FrameIndexOf(aligned_start);
    last_index = FrameIndexOf(aligned_end) - 1U;

    if (first_index >= FrameCount)
    {
        return;
    }

    if (last_index >= FrameCount)
    {
        last_index = FrameCount - 1U;
    }

    for (size_t index = first_index; index <= last_index; ++index)
    {
        FrameMarkFree(index);
    }
}

/*
 * Determines where the bitmap itself may be placed.
 *
 * The bitmap must lie in memory that is usable, that is not occupied by the
 * kernel image or by the boot information structure, and that is addressable
 * through the higher-half mapping, which presently covers only the first
 * gibibyte of physical memory. A region is examined by advancing a candidate
 * address past each obstruction in turn and asking whether what remains of the
 * region is large enough.
 *
 * Returns the chosen physical address, or FRAME_ALLOCATION_FAILED if no region
 * can accommodate the bitmap.
 */
static PhysicalAddress PhysicalMemoryPlaceBitmap(const BootInformation *information,
                                                 uint64_t required_bytes)
{
    /* The extent addressable through the boot-time higher-half mapping. */
    const PhysicalAddress addressable_limit = UINT64_C(0x40000000);

    for (size_t index = 0U; index < information->memory_region_count; ++index)
    {
        const BootMemoryRegion *region = &information->memory_regions[index];
        PhysicalAddress region_end;
        PhysicalAddress candidate;

        if (region->type != BOOT_MEMORY_USABLE)
        {
            continue;
        }

        region_end = region->base_address + region->length;

        if (region_end > addressable_limit)
        {
            region_end = addressable_limit;
        }

        candidate = AlignUp(region->base_address, PAGE_SIZE);

        if (candidate < LOW_MEMORY_LIMIT)
        {
            candidate = LOW_MEMORY_LIMIT;
        }

        /*
         * Advance past the kernel image and the boot information structure. Two
         * passes are performed because advancing past one obstruction may bring
         * the candidate into the other, and the two are not ordered.
         */
        for (unsigned int pass = 0U; pass < 2U; ++pass)
        {
            if (candidate < information->kernel_physical_end &&
                (candidate + required_bytes) > information->kernel_physical_start)
            {
                candidate = AlignUp(information->kernel_physical_end, PAGE_SIZE);
            }

            if (candidate < information->boot_information_end &&
                (candidate + required_bytes) > information->boot_information_start)
            {
                candidate = AlignUp(information->boot_information_end, PAGE_SIZE);
            }
        }

        if (candidate >= region->base_address &&
            (candidate + required_bytes) <= region_end)
        {
            return candidate;
        }
    }

    return FRAME_ALLOCATION_FAILED;
}

void PhysicalMemoryInitialise(const BootInformation *information)
{
    uint64_t bitmap_bytes;

    if (information->highest_usable_address == 0U)
    {
        KernelPanic("The boot loader reported no usable physical memory.");
    }

    /*
     * The allocator governs every frame below the highest usable address. Frames
     * above it are not represented at all, which is correct: nothing usable lies
     * beyond, and representing the reserved regions at the top of the address
     * space would require a bitmap of absurd size. The largest region reported by
     * QEMU begins at 0xFD00000000, and covering it would demand a bitmap of some
     * two mebibytes to describe memory that does not exist.
     */
    FrameCount = FrameIndexOf(AlignUp(information->highest_usable_address, PAGE_SIZE));
    FrameBitmapWordCount =
        (FrameCount + (FRAME_BITS_PER_WORD - 1U)) / FRAME_BITS_PER_WORD;
    bitmap_bytes = (uint64_t)FrameBitmapWordCount * sizeof(uint64_t);

    FrameBitmapPhysicalStart = PhysicalMemoryPlaceBitmap(information, bitmap_bytes);

    if (FrameBitmapPhysicalStart == FRAME_ALLOCATION_FAILED)
    {
        KernelPanic("No usable region can accommodate the physical memory bitmap.");
    }

    FrameBitmapPhysicalEnd = FrameBitmapPhysicalStart + bitmap_bytes;
    FrameBitmap = (uint64_t *)(uintptr_t)PhysicalToVirtual(FrameBitmapPhysicalStart);

    /*
     * Mark every frame as unavailable before consulting the map. A region that
     * the boot loader did not describe is thereby treated as reserved, which is
     * the conservative reading: memory whose existence is unattested must not be
     * issued.
     */
    for (size_t word = 0U; word < FrameBitmapWordCount; ++word)
    {
        FrameBitmap[word] = ~UINT64_C(0);
    }
    FrameFreeFrames = 0U;

    /* Release the frames of every region the map classifies as usable. */
    for (size_t index = 0U; index < information->memory_region_count; ++index)
    {
        const BootMemoryRegion *region = &information->memory_regions[index];

        if (region->type == BOOT_MEMORY_USABLE)
        {
            FrameReleaseRange(region->base_address,
                              region->base_address + region->length);
        }
    }

    /*
     * Reserve the extents that lie within usable regions but must never be
     * issued. The order matters: these must follow the release above, or the
     * release would undo them.
     */
    FrameMarkRange(0U, LOW_MEMORY_LIMIT);
    FrameMarkRange(information->kernel_physical_start,
                   information->kernel_physical_end);
    FrameMarkRange(information->boot_information_start,
                   information->boot_information_end);
    FrameMarkRange(FrameBitmapPhysicalStart, FrameBitmapPhysicalEnd);

    FrameSearchHint = FrameIndexOf(LOW_MEMORY_LIMIT);
}

PhysicalAddress FrameAllocateBelow(PhysicalAddress limit)
{
    size_t limit_index = FrameIndexOf(AlignDown(limit, PAGE_SIZE));
    size_t start_index = FrameSearchHint;

    if (limit_index > FrameCount)
    {
        limit_index = FrameCount;
    }

    if (start_index >= limit_index)
    {
        start_index = FrameIndexOf(LOW_MEMORY_LIMIT);
    }

    /*
     * Search from the hint to the limit, then from the low boundary to the hint,
     * so that the whole of the permitted range is examined irrespective of where
     * the hint happens to stand.
     */
    for (unsigned int pass = 0U; pass < 2U; ++pass)
    {
        size_t first = (pass == 0U) ? start_index : FrameIndexOf(LOW_MEMORY_LIMIT);
        size_t last = (pass == 0U) ? limit_index : start_index;

        for (size_t index = first; index < last; ++index)
        {
            /*
             * Skip a whole word when every frame it represents is in use. The
             * low frames are permanently reserved, so this materially shortens
             * the common case.
             */
            if ((index % FRAME_BITS_PER_WORD) == 0U &&
                FrameBitmap[index / FRAME_BITS_PER_WORD] == ~UINT64_C(0))
            {
                index += FRAME_BITS_PER_WORD - 1U;
                continue;
            }

            if (!FrameIsUsed(index))
            {
                FrameMarkUsed(index);

                if (FrameReferenceTable != NULL)
                {
                    FrameReferenceTable[index] = 1U;
                }

                FrameSearchHint = index + 1U;
                return FrameAddressOf(index);
            }
        }
    }

    return FRAME_ALLOCATION_FAILED;
}

PhysicalAddress FrameAllocate(void)
{
    return FrameAllocateBelow(FrameAddressOf(FrameCount));
}

void FrameFree(PhysicalAddress frame)
{
    size_t index;

    if (!IsPageAligned(frame))
    {
        KernelPanic("A misaligned address was passed to FrameFree.");
    }

    index = FrameIndexOf(frame);

    if (index >= FrameCount)
    {
        KernelPanic("An out-of-range address was passed to FrameFree.");
    }

    if (!FrameIsUsed(index))
    {
        KernelPanic("A frame that was already free was passed to FrameFree.");
    }

    /*
     * With reference counting established, a frame is returned to the allocator
     * only when its last reference is released. This is what permits a frame to
     * be shared between address spaces, and is the mechanism upon which
     * copy-on-write rests.
     */
    if (FrameReferenceTable != NULL)
    {
        if (FrameReferenceTable[index] == 0U)
        {
            KernelPanic("A frame with no references was passed to FrameFree.");
        }

        --FrameReferenceTable[index];

        if (FrameReferenceTable[index] != 0U)
        {
            return;
        }
    }

    FrameMarkFree(index);

    if (index < FrameSearchHint)
    {
        FrameSearchHint = index;
    }
}

void FrameReferenceInitialise(void)
{
    uint16_t *table;

    if (FrameReferenceTable != NULL)
    {
        return;
    }

    /*
     * The table is allocated from the kernel heap, which is why this is a
     * separate step performed after sub-task 2.5 rather than part of
     * PhysicalMemoryInitialise. Allocating it consumes frames itself, and those
     * frames are seeded below along with every other allocated frame.
     */
    table = (uint16_t *)KernelAllocateZeroed(FrameCount * sizeof(uint16_t));

    if (table == NULL)
    {
        KernelPanic("The frame reference table could not be allocated.");
    }

    /*
     * Seed the table before publishing it. Every frame presently allocated or
     * reserved has been issued once and released no times, so its count is one.
     * Publishing the table first and seeding afterwards would leave a window in
     * which FrameFree observed a zero count for a live frame.
     */
    for (size_t index = 0U; index < FrameCount; ++index)
    {
        table[index] = FrameIsUsed(index) ? (uint16_t)1U : (uint16_t)0U;
    }

    FrameReferenceTable = table;
    FrameReferenceHighWaterMark = 1U;
}

void FrameReferenceIncrement(PhysicalAddress frame)
{
    size_t index;

    if (!IsPageAligned(frame))
    {
        KernelPanic("A misaligned address was passed to FrameReferenceIncrement.");
    }

    index = FrameIndexOf(frame);

    if (index >= FrameCount)
    {
        KernelPanic("An out-of-range address was passed to FrameReferenceIncrement.");
    }

    if (FrameReferenceTable == NULL)
    {
        KernelPanic("A reference was taken before reference counting was established.");
    }

    if (FrameReferenceTable[index] == 0U)
    {
        KernelPanic("A reference was taken to a frame that is not allocated.");
    }

    if (FrameReferenceTable[index] == UINT16_MAX)
    {
        KernelPanic("The reference count of a frame would overflow.");
    }

    ++FrameReferenceTable[index];

    if (FrameReferenceTable[index] > FrameReferenceHighWaterMark)
    {
        FrameReferenceHighWaterMark = FrameReferenceTable[index];
    }
}

uint32_t FrameReferenceCount(PhysicalAddress frame)
{
    size_t index = FrameIndexOf(frame);

    if (index >= FrameCount)
    {
        return 0U;
    }

    if (FrameReferenceTable == NULL)
    {
        return FrameIsUsed(index) ? 1U : 0U;
    }

    return (uint32_t)FrameReferenceTable[index];
}

bool FrameReferenceIsActive(void)
{
    return FrameReferenceTable != NULL;
}

size_t FrameTotalCount(void)
{
    return FrameCount;
}

size_t FrameFreeCount(void)
{
    return FrameFreeFrames;
}

size_t FrameUsedCount(void)
{
    return FrameCount - FrameFreeFrames;
}

void PhysicalMemoryReport(void)
{
    KernelWriteString("Physical frame allocator: ");
    KernelWriteDecimal((uint64_t)FrameCount);
    KernelWriteString(" frames governed, ");
    KernelWriteDecimal((uint64_t)FrameFreeFrames);
    KernelWriteString(" free (");
    KernelWriteDecimal(((uint64_t)FrameFreeFrames * PAGE_SIZE) / 1024U);
    KernelWriteString(" KiB), ");
    KernelWriteDecimal((uint64_t)(FrameCount - FrameFreeFrames));
    KernelWriteString(" used.\n");

    KernelWriteString("Frame reference counting: ");
    if (FrameReferenceTable != NULL)
    {
        KernelWriteString("active, ");
        KernelWriteDecimal((uint64_t)FrameCount * sizeof(uint16_t) / 1024U);
        KernelWriteString(" KiB table, greatest count ");
        KernelWriteDecimal((uint64_t)FrameReferenceHighWaterMark);
        KernelWriteString(".\n");
    }
    else
    {
        KernelWriteString("not yet established.\n");
    }

    KernelWriteString("Frame bitmap: ");
    KernelWriteHexadecimal(FrameBitmapPhysicalStart);
    KernelWriteString(" - ");
    KernelWriteHexadecimal(FrameBitmapPhysicalEnd);
    KernelWriteString(" (");
    KernelWriteDecimal((FrameBitmapPhysicalEnd - FrameBitmapPhysicalStart + 1023U) / 1024U);
    KernelWriteString(" KiB).\n");
}
