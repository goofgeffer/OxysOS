/*
 * File: kernel/mm/vmm.c
 * Purpose: Implements the kernel virtual address allocator. It issues virtually
 *          contiguous ranges of the kernel arena and backs each page with a
 *          frame obtained from the physical allocator.
 * Key functions: KernelVirtualInitialise, KernelPagesAllocate, KernelPagesFree,
 *          KernelVirtualReport, ArenaFreeListInsert, ArenaFreeListTake.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the 4 KiB page is the unit of mapping.
 *   - docs/design/MEMORY-LAYOUT.md, Section 10: the design of the arena, and
 *     Section 10.4 for the bound every page count is held to.
 *
 * Design note upon page counts. Every bound in this file is computed as
 * page_count * PAGE_SIZE, a 64-bit unsigned product that a large enough count
 * wraps: 2^52 pages multiply to zero and 2^38 pages carry the bump pointer past
 * the top of the address space. Each entry point therefore refuses a count above
 * ARENA_PAGE_CAPACITY before performing any arithmetic upon it, which makes every
 * multiplication and addition below safe by construction rather than by a test
 * repeated at each site.
 *
 * Design note. Address space is allocated by a bump pointer with a free list of
 * released ranges searched first. The free list is a fixed-capacity array rather
 * than a linked structure because this allocator is beneath the heap: the heap
 * obtains its pages here, so allocating a list node from the heap would be
 * circular. A fixed array has no such dependency, and its capacity is a bound on
 * fragmentation rather than on the number of live allocations.
 *
 * Concurrency. Not yet safe against concurrent access. From sub-task 6.9 the
 * bump pointer and the free list require a spinlock.
 */

#include <oxys/vmm.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/kernel.h>

/*
 * The number of released ranges that may be retained for reuse. A range released
 * when the list is full is not reused and its address space is forfeit; the
 * frames backing it are always returned to the physical allocator, so the loss
 * is address space alone, of which the arena has 32 TiB.
 */
#define ARENA_FREE_LIST_CAPACITY 128U

typedef struct ArenaFreeRange
{
    VirtualAddress base;
    size_t page_count;
} ArenaFreeRange;

/* The free list, held in ascending order of base address so that adjacent
 * ranges may be coalesced upon insertion. */
static ArenaFreeRange ArenaFreeList[ARENA_FREE_LIST_CAPACITY];
static size_t ArenaFreeListCount;

/* The first address never yet issued. Address space beyond this point is
 * untouched. */
static VirtualAddress ArenaBumpPointer;

/*
 * The greatest number of pages the arena could hold were it wholly empty, and
 * therefore the greatest count any request may name. Every page count entering
 * this file is bounded by it before it is multiplied by anything; see
 * KernelPagesAllocate.
 */
#define ARENA_PAGE_CAPACITY (KERNEL_ARENA_SIZE / PAGE_SIZE)

/* The number of pages presently allocated, and the greatest number ever
 * allocated, for reporting. */
static size_t ArenaPagesInUse;
static size_t ArenaPagesHighWaterMark;

/* The number of ranges lost because the free list was full when they were
 * released. A non-zero value indicates the capacity should be raised. */
static size_t ArenaRangesForfeited;

void KernelVirtualInitialise(void)
{
    ArenaBumpPointer = KERNEL_ARENA_BASE;
    ArenaFreeListCount = 0U;
    ArenaPagesInUse = 0U;
    ArenaPagesHighWaterMark = 0U;
    ArenaRangesForfeited = 0U;
}

/*
 * Inserts a released range into the free list, preserving ascending order and
 * coalescing with an immediately adjacent neighbour on either side.
 *
 * Coalescing matters because without it a sequence of allocations and releases
 * of differing sizes would fragment the list into entries too small to satisfy
 * any request, while the address space they describe remained contiguous.
 */
static void ArenaFreeListInsert(VirtualAddress base, size_t page_count)
{
    size_t position = 0U;

    while (position < ArenaFreeListCount && ArenaFreeList[position].base < base)
    {
        ++position;
    }

    /* Coalesce with the preceding range if it ends exactly where this begins. */
    if (position > 0U)
    {
        ArenaFreeRange *previous = &ArenaFreeList[position - 1U];

        if ((previous->base + (previous->page_count * PAGE_SIZE)) == base)
        {
            previous->page_count += page_count;

            /* The enlarged range may now meet the following one. */
            if (position < ArenaFreeListCount &&
                (previous->base + (previous->page_count * PAGE_SIZE)) ==
                    ArenaFreeList[position].base)
            {
                previous->page_count += ArenaFreeList[position].page_count;

                for (size_t index = position; (index + 1U) < ArenaFreeListCount; ++index)
                {
                    ArenaFreeList[index] = ArenaFreeList[index + 1U];
                }
                --ArenaFreeListCount;
            }

            return;
        }
    }

    /* Coalesce with the following range if this ends exactly where it begins. */
    if (position < ArenaFreeListCount &&
        (base + (page_count * PAGE_SIZE)) == ArenaFreeList[position].base)
    {
        ArenaFreeList[position].base = base;
        ArenaFreeList[position].page_count += page_count;
        return;
    }

    if (ArenaFreeListCount >= ARENA_FREE_LIST_CAPACITY)
    {
        ++ArenaRangesForfeited;
        return;
    }

    for (size_t index = ArenaFreeListCount; index > position; --index)
    {
        ArenaFreeList[index] = ArenaFreeList[index - 1U];
    }

    ArenaFreeList[position].base = base;
    ArenaFreeList[position].page_count = page_count;
    ++ArenaFreeListCount;
}

/*
 * Removes and returns a range of at least the requested size from the free list,
 * splitting a larger range and retaining the remainder. Returns 0 if no range is
 * large enough.
 *
 * First fit is used rather than best fit. Best fit would reduce waste but
 * requires scanning the whole list on every request, and the coalescing
 * performed upon insertion already prevents the fragmentation that would make
 * the difference material.
 */
static VirtualAddress ArenaFreeListTake(size_t page_count)
{
    for (size_t index = 0U; index < ArenaFreeListCount; ++index)
    {
        if (ArenaFreeList[index].page_count >= page_count)
        {
            VirtualAddress base = ArenaFreeList[index].base;

            if (ArenaFreeList[index].page_count == page_count)
            {
                for (size_t shift = index; (shift + 1U) < ArenaFreeListCount; ++shift)
                {
                    ArenaFreeList[shift] = ArenaFreeList[shift + 1U];
                }
                --ArenaFreeListCount;
            }
            else
            {
                ArenaFreeList[index].base += page_count * PAGE_SIZE;
                ArenaFreeList[index].page_count -= page_count;
            }

            return base;
        }
    }

    return 0U;
}

void *KernelPagesAllocate(size_t page_count)
{
    VirtualAddress base;
    size_t mapped_count = 0U;

    if (page_count == 0U)
    {
        return NULL;
    }

    /*
     * A count the arena could not hold however empty it is, refused before any
     * arithmetic is performed upon it.
     *
     * This is not merely an early rejection of a request that would fail anyway.
     * Every bound below is computed as page_count * PAGE_SIZE, and that product
     * is a 64-bit unsigned quantity: a count of 2^52 multiplies to exactly zero,
     * and a count of 2^38 gives a product that carries the bump pointer past the
     * top of the address space and back to a small value. In either case the
     * comparison that follows compares a wrapped number against the end of the
     * arena, finds it smaller, and admits the request — the guard computing a
     * value the guard itself cannot trust.
     *
     * Bounding the count by what the arena holds makes every later multiplication
     * and addition safe by construction rather than by a check at each site: the
     * product cannot exceed KERNEL_ARENA_SIZE, and the arena ends well below the
     * top of the address space, so no sum of the two can wrap.
     */
    if (page_count > ARENA_PAGE_CAPACITY)
    {
        return NULL;
    }

    base = ArenaFreeListTake(page_count);

    if (base == 0U)
    {
        if ((ArenaBumpPointer + (page_count * PAGE_SIZE)) >
            (KERNEL_ARENA_BASE + KERNEL_ARENA_SIZE))
        {
            return NULL;
        }

        base = ArenaBumpPointer;
        ArenaBumpPointer += page_count * PAGE_SIZE;
    }

    /*
     * Back every page with a frame. Should a frame become unavailable partway
     * through, the pages already mapped must be released; leaving them mapped
     * would leak both frames and address space, and returning NULL while some of
     * the range was mapped would leave the arena in an inconsistent state.
     */
    for (size_t index = 0U; index < page_count; ++index)
    {
        PhysicalAddress frame = FrameAllocate();

        if (frame == FRAME_ALLOCATION_FAILED)
        {
            for (size_t undo = 0U; undo < mapped_count; ++undo)
            {
                VirtualAddress page = base + (undo * PAGE_SIZE);
                PhysicalAddress mapped_frame = PagingTranslate(page);

                PagingUnmapKernelPage(page);
                FrameFree(mapped_frame);
            }

            ArenaFreeListInsert(base, page_count);
            return NULL;
        }

        PagingMapKernelPage(base + (index * PAGE_SIZE), frame, PAGE_ENTRY_WRITABLE);
        ++mapped_count;
    }

    ArenaPagesInUse += page_count;
    if (ArenaPagesInUse > ArenaPagesHighWaterMark)
    {
        ArenaPagesHighWaterMark = ArenaPagesInUse;
    }

    return (void *)(uintptr_t)base;
}

void KernelPagesFree(void *address, size_t page_count)
{
    VirtualAddress base = (VirtualAddress)(uintptr_t)address;

    if (address == NULL || page_count == 0U)
    {
        return;
    }

    if (base < KERNEL_ARENA_BASE || base >= (KERNEL_ARENA_BASE + KERNEL_ARENA_SIZE))
    {
        KernelPanic("An address outside the kernel arena was passed to KernelPagesFree.");
    }

    /*
     * A count the arena could not hold. No such range was ever issued, so this
     * is a programming error in the caller and is treated as the other two are.
     * It is checked for the reason KernelPagesAllocate gives: the accounting
     * below subtracts the count, and ArenaFreeListInsert multiplies it, and a
     * count that wrapped either would corrupt the free list with a range that
     * outlives this call and is handed to somebody else.
     */
    if (page_count > ARENA_PAGE_CAPACITY)
    {
        KernelPanic("A page count larger than the kernel arena was passed to KernelPagesFree.");
    }

    if (!IsPageAligned(base))
    {
        KernelPanic("A misaligned address was passed to KernelPagesFree.");
    }

    for (size_t index = 0U; index < page_count; ++index)
    {
        VirtualAddress page = base + (index * PAGE_SIZE);
        PhysicalAddress frame = PagingTranslate(page);

        if (frame == 0U)
        {
            KernelPanic("An unmapped page was passed to KernelPagesFree.");
        }

        PagingUnmapKernelPage(page);
        FrameFree(frame);
    }

    ArenaPagesInUse -= page_count;
    ArenaFreeListInsert(base, page_count);
}

size_t KernelVirtualPagesInUse(void)
{
    return ArenaPagesInUse;
}

void KernelVirtualReport(void)
{
    KernelWriteString("Kernel virtual arena: base ");
    KernelWriteHexadecimal(KERNEL_ARENA_BASE);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)ArenaPagesInUse);
    KernelWriteString(" pages in use, high water mark ");
    KernelWriteDecimal((uint64_t)ArenaPagesHighWaterMark);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)ArenaFreeListCount);
    KernelWriteString(" free ranges");

    if (ArenaRangesForfeited != 0U)
    {
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)ArenaRangesForfeited);
        KernelWriteString(" ranges forfeited");
    }

    KernelWriteString(".\n");
}
