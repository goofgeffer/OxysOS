/*
 * File: kernel/mm/heap.c
 * Purpose: Implements the kernel heap as a slab allocator. Pages obtained from
 *          the kernel virtual address allocator are carved into objects of a
 *          fixed size class, and a header at the base of each page identifies
 *          the class to which its objects belong.
 * Key functions: KernelHeapInitialise, KernelAllocate, KernelAllocateZeroed,
 *          KernelFree, KernelHeapReport, HeapSizeClassFor, HeapRefillClass.
 * References:
 *   - docs/design/MEMORY-LAYOUT.md, Section 11: the design of the heap, and
 *     Section 11.4 for the sizes that are refused as unrepresentable.
 *   - Bonwick, J., "The Slab Allocator: An Object-Caching Kernel Memory
 *     Allocator", USENIX Summer 1994. Consulted for the object-caching concept
 *     alone; this implementation is original and simpler, having neither
 *     constructors nor per-processor caches.
 *
 * Design note. The size class of an allocation is recovered from its address
 * rather than from a header preceding each object. Every slab is one page and is
 * page aligned, and no object crosses a page boundary, so rounding a pointer
 * down to a page boundary yields the header of the slab that contains it. This
 * costs one header of 32 bytes per page instead of a header per object, which
 * for the 16-byte class is the difference between 6 per cent overhead and 100
 * per cent.
 *
 * Concurrency. Not yet safe against concurrent access. From sub-task 6.8 each
 * class free list requires a lock, and per-processor caches become worthwhile.
 */

#include <oxys/heap.h>
#include <oxys/vmm.h>
#include <oxys/memory.h>
#include <oxys/kernel.h>

/* Identifies a page as belonging to the heap, and distinguishes a corrupt or
 * foreign pointer from a valid one. */
#define HEAP_SLAB_MAGIC UINT64_C(0x4F5859534845415A)

/* The size classes served. Each is a multiple of HEAP_ALIGNMENT. */
#define HEAP_CLASS_COUNT 8U

static const uint32_t HeapSizeClasses[HEAP_CLASS_COUNT] = {
    16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U
};

/*
 * The header at the base of every page the heap owns. Its size is a multiple of
 * HEAP_ALIGNMENT so that the first object of a slab is correctly aligned.
 */
typedef struct HeapPageHeader
{
    uint64_t magic;
    /* The size of the objects in this slab, or zero if the page is part of a
     * large allocation. */
    uint32_t object_size;
    /* The number of pages in a large allocation; unused for a slab. */
    uint32_t page_count;
    /* The number of objects of this slab presently allocated. */
    uint32_t objects_in_use;
    uint32_t reserved;
    uint64_t padding;
} HeapPageHeader;

_Static_assert(sizeof(HeapPageHeader) == 32,
               "The heap page header must be 32 bytes so that objects remain aligned.");

_Static_assert((sizeof(HeapPageHeader) % HEAP_ALIGNMENT) == 0U,
               "The heap page header must be a multiple of the heap alignment.");

/*
 * The free objects of each class, threaded through the free objects themselves.
 * A free object is not in use by any caller, so its first eight bytes may hold
 * the link without additional storage. This requires the smallest class to be at
 * least the size of a pointer, which the assertion below enforces.
 */
static void *HeapClassFreeList[HEAP_CLASS_COUNT];

_Static_assert(sizeof(void *) <= 16U,
               "The smallest size class must accommodate a free-list link.");

/* Accounting, for reporting alone. */
static size_t HeapSlabPageCount;
static size_t HeapLargePageCount;
static size_t HeapLiveAllocationCount;

void KernelHeapInitialise(void)
{
    for (size_t index = 0U; index < HEAP_CLASS_COUNT; ++index)
    {
        HeapClassFreeList[index] = NULL;
    }

    HeapSlabPageCount = 0U;
    HeapLargePageCount = 0U;
    HeapLiveAllocationCount = 0U;
}

/* Clears a region to zero. Provided because the C library does not exist until
 * Phase 7. */
static void HeapZero(void *address, size_t size)
{
    uint8_t *bytes = (uint8_t *)address;

    for (size_t index = 0U; index < size; ++index)
    {
        bytes[index] = 0U;
    }
}

/*
 * Returns the index of the smallest size class that accommodates the request, or
 * HEAP_CLASS_COUNT if the request exceeds the largest class.
 */
static size_t HeapSizeClassFor(size_t size)
{
    for (size_t index = 0U; index < HEAP_CLASS_COUNT; ++index)
    {
        if (size <= HeapSizeClasses[index])
        {
            return index;
        }
    }

    return HEAP_CLASS_COUNT;
}

/*
 * Obtains one page from the kernel arena, carves it into objects of the given
 * class, and threads them onto that class's free list. Returns false if no page
 * is available.
 */
static bool HeapRefillClass(size_t class_index)
{
    void *page = KernelPagesAllocate(1U);
    HeapPageHeader *header;
    uint32_t object_size;
    size_t object_count;
    uint8_t *first_object;

    if (page == NULL)
    {
        return false;
    }

    object_size = HeapSizeClasses[class_index];

    header = (HeapPageHeader *)page;
    header->magic = HEAP_SLAB_MAGIC;
    header->object_size = object_size;
    header->page_count = 1U;
    header->objects_in_use = 0U;
    header->reserved = 0U;
    header->padding = 0U;

    first_object = (uint8_t *)page + sizeof(HeapPageHeader);
    object_count = (size_t)(PAGE_SIZE - sizeof(HeapPageHeader)) / object_size;

    /*
     * Thread the objects onto the free list in reverse, so that the lowest
     * address is at the head. Allocation then proceeds upward through the page,
     * which is marginally friendlier to the cache than the reverse.
     */
    for (size_t index = object_count; index > 0U; --index)
    {
        void *object = first_object + ((index - 1U) * object_size);

        *(void **)object = HeapClassFreeList[class_index];
        HeapClassFreeList[class_index] = object;
    }

    ++HeapSlabPageCount;

    return true;
}

void *KernelAllocate(size_t size)
{
    size_t class_index;
    void *object;

    if (size == 0U)
    {
        return NULL;
    }

    class_index = HeapSizeClassFor(size);

    /*
     * A request too large for any class is served by whole pages. The header
     * occupies the beginning of the first page, so the pages required accommodate
     * the request and the header together.
     */
    if (class_index == HEAP_CLASS_COUNT)
    {
        size_t page_count;
        void *pages;
        HeapPageHeader *header;

        /*
         * A size so large that adding the header, or rounding the sum up to a
         * page, would carry past the top of a 64-bit quantity.
         *
         * This is refused here rather than left to the page allocator, because
         * the page allocator would never see it: the sum wraps to a small number,
         * the rounding yields a page count of one or two, and the allocation
         * succeeds. The caller would receive a valid pointer to a few pages while
         * believing it holds very nearly the whole address space, and would
         * discover otherwise by writing past the end of it. A request that cannot
         * be represented must fail as a request that cannot be satisfied does,
         * and for the same reason: the answer NULL is the only honest one.
         */
        if (size > (SIZE_MAX - sizeof(HeapPageHeader) - (PAGE_SIZE - 1U)))
        {
            return NULL;
        }

        page_count =
            (size_t)((AlignUp((uint64_t)size + sizeof(HeapPageHeader), PAGE_SIZE)) / PAGE_SIZE);
        pages = KernelPagesAllocate(page_count);

        if (pages == NULL)
        {
            return NULL;
        }

        header = (HeapPageHeader *)pages;
        header->magic = HEAP_SLAB_MAGIC;
        header->object_size = 0U;
        header->page_count = (uint32_t)page_count;
        header->objects_in_use = 1U;
        header->reserved = 0U;
        header->padding = 0U;

        HeapLargePageCount += page_count;
        ++HeapLiveAllocationCount;

        return (uint8_t *)pages + sizeof(HeapPageHeader);
    }

    if (HeapClassFreeList[class_index] == NULL && !HeapRefillClass(class_index))
    {
        return NULL;
    }

    object = HeapClassFreeList[class_index];
    HeapClassFreeList[class_index] = *(void **)object;

    ((HeapPageHeader *)(uintptr_t)AlignDown((uint64_t)(uintptr_t)object,
                                            PAGE_SIZE))->objects_in_use++;
    ++HeapLiveAllocationCount;

    return object;
}

void *KernelAllocateZeroed(size_t size)
{
    void *address = KernelAllocate(size);

    if (address != NULL)
    {
        HeapZero(address, size);
    }

    return address;
}

void KernelFree(void *address)
{
    HeapPageHeader *header;
    size_t class_index;

    if (address == NULL)
    {
        return;
    }

    /*
     * The slab header lies at the base of the page containing the object. This
     * is the whole reason objects carry no header of their own; refer to the
     * design note at the head of this file.
     */
    header = (HeapPageHeader *)(uintptr_t)AlignDown((uint64_t)(uintptr_t)address, PAGE_SIZE);

    if (header->magic != HEAP_SLAB_MAGIC)
    {
        KernelPanic("A pointer not obtained from the kernel heap was passed to KernelFree.");
    }

    if (header->object_size == 0U)
    {
        size_t page_count = header->page_count;

        header->magic = 0U;
        HeapLargePageCount -= page_count;
        --HeapLiveAllocationCount;
        KernelPagesFree(header, page_count);
        return;
    }

    if (header->objects_in_use == 0U)
    {
        KernelPanic("An object was released from a slab that holds none.");
    }

    class_index = HeapSizeClassFor(header->object_size);

    *(void **)address = HeapClassFreeList[class_index];
    HeapClassFreeList[class_index] = address;

    --header->objects_in_use;
    --HeapLiveAllocationCount;

    /*
     * A slab whose objects are all free is not returned to the arena. Doing so
     * would require removing its remaining objects from the class free list,
     * which is singly linked and offers no means of finding them. The page is
     * therefore retained and reused by the next allocation of its class. This is
     * a deliberate limitation, recorded in docs/design/MEMORY-LAYOUT.md, Section 11.4.
     */
}

void KernelHeapReport(void)
{
    KernelWriteString("Kernel heap: ");
    KernelWriteDecimal((uint64_t)HeapLiveAllocationCount);
    KernelWriteString(" live allocations, ");
    KernelWriteDecimal((uint64_t)HeapSlabPageCount);
    KernelWriteString(" slab pages, ");
    KernelWriteDecimal((uint64_t)HeapLargePageCount);
    KernelWriteString(" pages in large allocations.\n");
}
