/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdlib/heap.c
 * Purpose: The user-space heap of sub-task 7.3 — the four memory management
 *          functions of ISO/IEC 9899:2011, Section 7.22.3, above a first-fit
 *          allocator over an address-ordered free list of boundary-marked
 *          blocks.
 * Key functions: malloc, calloc, realloc, free, OxysHeapAdopt, OxysHeapInspect.
 * Key structures: HeapBlock.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.22.3, paragraph 1: the order and contiguity
 *     of successive allocations is unspecified; every pointer returned is
 *     aligned for any type with a fundamental alignment requirement; a request
 *     of zero bytes is implementation-defined.
 *   - ISO/IEC 9899:2011, Sections 7.22.3.2 to 7.22.3.5: calloc, free, malloc and
 *     realloc, each of which is implemented below in the order that subsection
 *     numbers them.
 *   - ISO/IEC 9899:2011, Section 6.2.8, paragraph 2: a fundamental alignment is
 *     one no stricter than _Alignof(max_align_t).
 *   - ISO/IEC 9899:2011, Section 6.3.2.3, paragraph 5, and Section 6.5.8,
 *     paragraph 5: the two places this file does arithmetic upon addresses as
 *     integers rather than as pointers, and why.
 *   - ISO/IEC 9899:2011, Section 6.5, paragraph 6: memory the system supplies has
 *     no declared type, and its effective type is that of the lvalue it is first
 *     stored through — which is what makes a block header written into it a
 *     defined thing to read back.
 *   - docs/design/LIBC.md, Section 9: the design, the policy chosen and the
 *     three that were not, and what is asserted of each half.
 *
 * The policy, in one paragraph.
 *
 *   The heap is a set of blocks. Every block carries a header giving its whole
 *   size and whether it is free, and the free ones are linked into a single list
 *   ordered by address. A request is met by the first free block large enough,
 *   which is split where the remainder would itself be a block. A release marks
 *   the block free, inserts it at its place in the order, and joins it to
 *   whichever of its two neighbours happens to lie against it. When no block
 *   fits, the heap asks the system for a region and makes a block of it.
 *
 * Why first fit over an address-ordered list, and not one of the three obvious
 * alternatives.
 *
 *   **Not best fit.** It costs a walk of the whole list instead of a walk to the
 *   first fit, and the literature has held since Knuth that it fragments no less
 *   for the trouble: the remainder it leaves is by construction the smallest
 *   possible, which is to say the least likely ever to be usable again.
 *
 *   **Not a size-ordered list.** It makes a fit cheap and makes coalescing dear:
 *   joining a released block to its neighbour requires knowing which block lies
 *   against it in memory, which a list ordered by size cannot answer without
 *   boundary tags beneath every block or a walk of the whole list.
 *
 *   **Not segregated free lists by size class.** That is the design a heap under
 *   real load wants, and this heap is under no load at all: nothing in this
 *   system allocates yet. Choosing it now would mean writing several hundred
 *   lines against an allocation profile that has never been measured, which is
 *   the same judgement docs/design/LIBC.md, Section 6, limitation 1, records
 *   about the string functions. The workload that will justify measuring is a
 *   ported compiler, and the day it exists this file is one translation unit to
 *   replace behind an interface four functions wide.
 *
 *   The address ordering is what makes the release cheap where it matters: the
 *   walk that finds a released block's place in the list is the same walk that
 *   finds both of its neighbours, so coalescing costs nothing beyond the
 *   insertion it was going to perform anyway.
 *
 * What is not here, and is not an oversight.
 *
 *   There is no locking. There are no userland threads to contend with — see
 *   docs/design/LIBC.md, Section 8.6, limitation 1, which records the same thing
 *   of errno — and a lock taken against nothing would be a lock nothing asserts.
 *   The day threads exist, every entry point below needs one, and the census is
 *   where the absence is stated so that it is not discovered.
 */

#include <errno.h>
#include <heap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * The block.
 * ------------------------------------------------------------------------- */

/*
 * The two marks a header carries, and why they are words rather than a flag.
 *
 * A single bit would say whether a block is free in one byte of memory that any
 * overrun of the block before it would reach, and every value of that byte is a
 * valid answer. These are eight bytes apiece and are the ASCII of "OXYSFREE" and
 * "OXYSLIVE", so a header that has been written over says so: neither mark
 * arises by accident, and a pointer that never came from this allocator carries
 * one only by a coincidence of one in about two to the sixty-fourth.
 *
 * That is what lets `free` refuse a pointer instead of corrupting the heap with
 * it. ISO/IEC 9899:2011, Section 7.22.3.3, paragraph 2, makes such a call
 * undefined behaviour and this library defines it; Section 9.4 of the design
 * document records the limits of the courtesy.
 */
#define HEAP_BLOCK_FREE      UINT64_C(0x4F58595346524545)
#define HEAP_BLOCK_ALLOCATED UINT64_C(0x4F5859534C495645)

/*
 * A block: its whole size, its place in the free list, and its mark.
 *
 * The header is the same thirty-two bytes whether the block is free or
 * allocated, and `next` is therefore carried by a block that is not upon any
 * list. The usual practice is to overlay the link upon the first bytes of the
 * payload, which halves the overhead of every allocation; it is not done here
 * because it makes the payload of an allocated block and the link of a free one
 * the same storage read through two types, and this project is bound by
 * PROJECT_GUIDELINES.md, Section 8, not to rely upon behaviour it cannot point
 * at a paragraph for. The cost is recorded as a limitation rather than paid
 * silently: docs/design/LIBC.md, Section 9.6, limitation 1.
 *
 * The size is of the whole block, the header included, and is always a multiple
 * of OXYS_HEAP_ALIGNMENT. Storing the whole size rather than the payload size is
 * what makes the address of the next block in memory a single addition.
 */
typedef struct HeapBlock HeapBlock;

struct HeapBlock
{
    size_t size;
    HeapBlock *next;
    uint64_t state;
    uint64_t reserved;
};

_Static_assert(sizeof(HeapBlock) == OXYS_HEAP_OVERHEAD,
               "The header is what <heap.h> tells a caller it is.");
_Static_assert((sizeof(HeapBlock) % OXYS_HEAP_ALIGNMENT) == 0U,
               "A header that is not a whole number of alignments would leave every "
               "payload after the first misaligned.");
_Static_assert(_Alignof(max_align_t) <= OXYS_HEAP_ALIGNMENT,
               "A pointer this allocator returns must satisfy every fundamental "
               "alignment: ISO/IEC 9899:2011, Section 6.2.8, paragraph 2.");
_Static_assert(_Alignof(HeapBlock) <= OXYS_HEAP_ALIGNMENT,
               "A block header must be no stricter than the addresses blocks are "
               "placed at.");

/*
 * The smallest a block may be: a header and one alignment of payload.
 *
 * Nothing smaller is worth leaving behind. A remainder of sixteen bytes is a
 * block a sixteen-byte request can be met from; a remainder of eight could never
 * be allocated to anybody and would be a permanent hole in the free list that
 * every later walk paid for.
 */
#define HEAP_BLOCK_MINIMUM (sizeof(HeapBlock) + (size_t)OXYS_HEAP_ALIGNMENT)

/*
 * How much the heap asks the system for, and the ceiling upon it.
 *
 * A request is never met by asking for exactly what it needs. Every extension
 * costs a system call and leaves a region that cannot be joined to the one
 * before it unless the two happen to abut, so asking for sixty-four kibibytes to
 * satisfy a request for forty bytes is the cheaper mistake by a wide margin. The
 * amount doubles with each extension up to the ceiling, which is the usual
 * answer to a program whose appetite is not known in advance: a program that
 * allocates a little pays for one region and a program that allocates a great
 * deal stops paying for a system call per mebibyte.
 */
#define HEAP_EXTEND_MINIMUM ((size_t)65536)
#define HEAP_EXTEND_MAXIMUM ((size_t)1048576)

/* -------------------------------------------------------------------------
 * The heap's own state.
 * ------------------------------------------------------------------------- */

/* The free list, ordered by address, lowest first. */
static HeapBlock *HeapFreeList;

/* What the heap has been given, and what it has made of it. */
static size_t HeapRegionCount;
static size_t HeapRegionBytes;
static size_t HeapBlockCount;
static size_t HeapExtendNext = HEAP_EXTEND_MINIMUM;

/* What it has done. */
static uint64_t HeapAllocations;
static uint64_t HeapReleases;
static uint64_t HeapReallocations;
static uint64_t HeapExtensions;
static uint64_t HeapFailures;
static uint64_t HeapRefusals;
static uint64_t HeapSplits;
static uint64_t HeapCoalescences;

/* -------------------------------------------------------------------------
 * Addresses.
 * ------------------------------------------------------------------------- */

/*
 * Every comparison and every adjacency test below is performed upon uintptr_t
 * and not upon pointers, and that is not a stylistic preference.
 *
 * ISO/IEC 9899:2011, Section 6.5.8, paragraph 5, defines the relational
 * operators only for pointers into the same array object or the same structure.
 * The blocks of a heap are, by construction, not in one array: they lie in
 * regions the system supplied at unrelated times. So `first < second` upon two
 * block pointers is exactly the undefined behaviour PROJECT_GUIDELINES.md,
 * Section 8, forbids, however obviously it works. The conversion to uintptr_t is
 * implementation-defined by Section 6.3.2.3, paragraph 5 — which is a different
 * thing from undefined, and is defined by this implementation as the address.
 *
 * The same reasoning governs `HeapEndOf`: a pointer one past the end of a block
 * is formed as an integer rather than by pointer arithmetic, because the block's
 * size may be the whole of a region and the arithmetic would otherwise leave the
 * object it started in.
 */
static uintptr_t HeapAddressOf(const HeapBlock *block)
{
    return (uintptr_t)(const void *)block;
}

static uintptr_t HeapEndOf(const HeapBlock *block)
{
    return HeapAddressOf(block) + (uintptr_t)block->size;
}

/*
 * The payload of a block, and the block of a payload.
 *
 * Both are formed by arithmetic upon unsigned char, which Section 6.3.2.3,
 * paragraph 7, and Section 6.2.6.1 make the type any object may be examined as,
 * and both stay within the one block. The cast to HeapBlock * passes through
 * void * deliberately: a cast that increases the required alignment of the
 * pointed-to type is what -Wcast-align reports, and the conversion through void
 * * states that the alignment is the caller's to have established — which it is,
 * every block address in this file being a multiple of OXYS_HEAP_ALIGNMENT.
 */
static void *HeapPayloadOf(HeapBlock *block)
{
    return (void *)((unsigned char *)(void *)block + sizeof(HeapBlock));
}

static HeapBlock *HeapBlockOf(void *payload)
{
    return (HeapBlock *)(void *)((unsigned char *)payload - sizeof(HeapBlock));
}

/* Rounds a size up to the allocator's alignment, or reports that it cannot be
 * rounded up without wrapping. The check is a comparison against what remains,
 * because a sum that has already wrapped cannot be tested for having wrapped. */
static bool HeapRoundUp(size_t value, size_t alignment, size_t *rounded)
{
    const size_t remainder = value % alignment;

    if (remainder == 0U)
    {
        *rounded = value;

        return true;
    }

    if ((alignment - remainder) > (SIZE_MAX - value))
    {
        return false;
    }

    *rounded = value + (alignment - remainder);

    return true;
}

/* Whether a header carries this allocator's mark for a block presently handed
 * out, and describes a block of a size this allocator could have made. The size
 * is checked as well as the mark because a mark alone would accept a pointer
 * into the middle of a live block whose bytes happened to hold one. */
static bool HeapBlockIsLive(const HeapBlock *block)
{
    return (block->state == HEAP_BLOCK_ALLOCATED) && (block->size >= HEAP_BLOCK_MINIMUM) &&
           ((block->size % OXYS_HEAP_ALIGNMENT) == 0U);
}

/* -------------------------------------------------------------------------
 * The free list.
 * ------------------------------------------------------------------------- */

/*
 * Places a block upon the free list at its address's place in the order, and
 * joins it to whichever of its neighbours lies against it.
 *
 * The order of the two joins matters. The successor is absorbed first, so that
 * the predecessor — if it too abuts — absorbs a block that has already grown,
 * and three adjacent free blocks become one rather than two. Doing it the other
 * way round leaves the middle block merged backward and the successor stranded,
 * which is a heap that fragments under exactly the pattern a heap meets most:
 * a run of allocations released in the order they were made.
 */
static void HeapInsert(HeapBlock *block)
{
    HeapBlock *previous = NULL;
    HeapBlock *current = HeapFreeList;

    while ((current != NULL) && (HeapAddressOf(current) < HeapAddressOf(block)))
    {
        previous = current;
        current = current->next;
    }

    block->state = HEAP_BLOCK_FREE;
    block->next = current;

    if (previous == NULL)
    {
        HeapFreeList = block;
    }
    else
    {
        previous->next = block;
    }

    if ((current != NULL) && (HeapEndOf(block) == HeapAddressOf(current)))
    {
        block->size += current->size;
        block->next = current->next;
        --HeapBlockCount;
        ++HeapCoalescences;
    }

    if ((previous != NULL) && (HeapEndOf(previous) == HeapAddressOf(block)))
    {
        previous->size += block->size;
        previous->next = block->next;
        --HeapBlockCount;
        ++HeapCoalescences;
    }
}

/*
 * Divides a block that is larger than the request needs, leaving the remainder
 * upon the free list.
 *
 * The block must not itself be upon the free list when this is called: the
 * remainder is inserted, and inserting a block that lies against one already
 * linked would join the two and produce a free list holding a block the caller
 * believes it has just been given.
 *
 * Nothing is divided where the remainder would be smaller than a block can be.
 * The bytes stay with the allocation instead, which is the ordinary source of
 * the difference between what a caller asked for and what it received.
 */
static void HeapSplit(HeapBlock *block, size_t needed)
{
    HeapBlock *remainder;

    if (block->size < (needed + HEAP_BLOCK_MINIMUM))
    {
        return;
    }

    remainder = (HeapBlock *)(void *)((unsigned char *)(void *)block + needed);
    remainder->size = block->size - needed;
    remainder->next = NULL;
    remainder->reserved = 0U;

    block->size = needed;

    ++HeapBlockCount;
    ++HeapSplits;

    HeapInsert(remainder);
}

/* The first free block large enough, unlinked from the list, divided if the
 * remainder would be worth having, and marked as handed out. Null where nothing
 * upon the list is large enough. */
static HeapBlock *HeapTake(size_t needed)
{
    HeapBlock *previous = NULL;
    HeapBlock *block = HeapFreeList;

    while ((block != NULL) && (block->size < needed))
    {
        previous = block;
        block = block->next;
    }

    if (block == NULL)
    {
        return NULL;
    }

    if (previous == NULL)
    {
        HeapFreeList = block->next;
    }
    else
    {
        previous->next = block->next;
    }

    block->next = NULL;
    block->state = HEAP_BLOCK_ALLOCATED;
    block->reserved = 0U;

    HeapSplit(block, needed);

    return block;
}

/*
 * Joins a live block to the free block immediately after it, where there is one
 * and the two together are large enough.
 *
 * This is what makes a program that grows one allocation repeatedly — which is
 * every program that reads something of unknown length — cost one region rather
 * than a copy of everything it has read at every step. Nothing is moved and no
 * contents are touched.
 */
static bool HeapAbsorbNext(HeapBlock *block, size_t needed)
{
    HeapBlock *previous = NULL;
    HeapBlock *current = HeapFreeList;
    const uintptr_t after = HeapEndOf(block);

    while ((current != NULL) && (HeapAddressOf(current) < after))
    {
        previous = current;
        current = current->next;
    }

    if ((current == NULL) || (HeapAddressOf(current) != after))
    {
        return false;
    }

    if ((block->size + current->size) < needed)
    {
        return false;
    }

    if (previous == NULL)
    {
        HeapFreeList = current->next;
    }
    else
    {
        previous->next = current->next;
    }

    block->size += current->size;
    --HeapBlockCount;
    ++HeapCoalescences;

    HeapSplit(block, needed);

    return true;
}

/* -------------------------------------------------------------------------
 * Growth.
 * ------------------------------------------------------------------------- */

bool OxysHeapAdopt(void *region, size_t bytes)
{
    unsigned char *const base = (unsigned char *)region;
    size_t adjustment;
    size_t usable;
    uintptr_t aligned;
    HeapBlock *block;

    if (region == NULL)
    {
        return false;
    }

    /*
     * The first block has to stand at the region's own beginning, so a region
     * that does not begin at an alignment loses the bytes before the first one
     * that does. The arithmetic is done upon the address as an integer and the
     * pointer is then formed by advancing within the region, which keeps every
     * pointer this function makes inside the object it was given.
     */
    aligned = (uintptr_t)region + (uintptr_t)(OXYS_HEAP_ALIGNMENT - 1U);
    aligned -= aligned % (uintptr_t)OXYS_HEAP_ALIGNMENT;

    if (aligned < (uintptr_t)region)
    {
        return false;
    }

    adjustment = (size_t)(aligned - (uintptr_t)region);

    if (adjustment >= bytes)
    {
        return false;
    }

    /* The tail below a whole alignment is unusable for the same reason the head
     * is: a block's size is a multiple of the alignment, so that every block
     * after it begins at one. */
    usable = bytes - adjustment;
    usable -= usable % (size_t)OXYS_HEAP_ALIGNMENT;

    /*
     * Whether the region is large enough is asked here and not of `bytes` at the
     * head of this function, which is where it would naturally be written.
     *
     * It is asked here because here is where it is knowable. A region of a
     * hundred bytes that begins sixty bytes before an alignment has forty usable
     * ones, and a check made before the adjustment would have passed it — after
     * which the block built from it is smaller than its own header, and every
     * later walk of the free list reads past the end of the region. The negative
     * test of docs/design/LIBC.md, Section 9.7, found the earlier check
     * redundant against this one and it was removed rather than kept for
     * appearances.
     */
    if (usable < HEAP_BLOCK_MINIMUM)
    {
        return false;
    }

    block = (HeapBlock *)(void *)(base + adjustment);
    block->size = usable;
    block->next = NULL;
    block->reserved = 0U;

    ++HeapBlockCount;
    ++HeapRegionCount;
    HeapRegionBytes += usable;

    /* HeapInsert joins this to a neighbour where one abuts, which is the usual
     * case for a second region obtained from a break that has simply moved up.
     * The region counts above are of what was given, not of what remains
     * distinguishable afterwards; the two differ exactly when the joining
     * happened, and a count of regions that fell when two were merged would be a
     * count of something nobody asked about. */
    HeapInsert(block);

    return true;
}

/*
 * Asks the system for enough memory to meet a request that nothing upon the free
 * list can meet.
 *
 * The amount asked for is the larger of what is needed and what the doubling
 * schedule has reached, rounded up to the schedule's own step so that successive
 * regions taken from a break are whole multiples and abut cleanly. A failure
 * leaves the schedule where it was: the next request is very likely to be
 * smaller, and doubling past a refusal would mean every later request asking for
 * more than the one that had just been refused.
 */
static bool HeapGrow(size_t needed)
{
    size_t bytes;
    void *region;

    if (!HeapRoundUp(needed, HEAP_EXTEND_MINIMUM, &bytes))
    {
        return false;
    }

    if (bytes < HeapExtendNext)
    {
        bytes = HeapExtendNext;
    }

    region = OxysHeapExtend(bytes);

    if (region == NULL)
    {
        return false;
    }

    ++HeapExtensions;

    if (HeapExtendNext < HEAP_EXTEND_MAXIMUM)
    {
        HeapExtendNext *= 2U;
    }

    /*
     * A region that cannot be adopted is lost: there is no way to give one back,
     * the break being a single boundary and this region not necessarily the
     * topmost. It is reported as a failure to grow rather than concealed, and it
     * can only happen for a region the system returned misaligned or shorter
     * than a block — neither of which this system produces, `brk` returning whole
     * pages.
     */
    return OxysHeapAdopt(region, bytes);
}

/* The whole block a request of `size` payload bytes needs, or false where the
 * arithmetic would wrap. A request is never met by a block smaller than the
 * minimum, so that a remainder left by splitting one is always usable. */
static bool HeapBlockSizeFor(size_t size, size_t *needed)
{
    size_t payload;

    if (!HeapRoundUp(size, (size_t)OXYS_HEAP_ALIGNMENT, &payload))
    {
        return false;
    }

    if (payload > (SIZE_MAX - sizeof(HeapBlock)))
    {
        return false;
    }

    *needed = payload + sizeof(HeapBlock);

    if (*needed < HEAP_BLOCK_MINIMUM)
    {
        *needed = HEAP_BLOCK_MINIMUM;
    }

    return true;
}

/* -------------------------------------------------------------------------
 * ISO/IEC 9899:2011, Section 7.22.3.
 * ------------------------------------------------------------------------- */

void *malloc(size_t size)
{
    size_t needed;
    HeapBlock *block;

    if (!HeapBlockSizeFor(size, &needed))
    {
        errno = ENOMEM;
        ++HeapFailures;

        return NULL;
    }

    block = HeapTake(needed);

    if ((block == NULL) && HeapGrow(needed))
    {
        block = HeapTake(needed);
    }

    if (block == NULL)
    {
        errno = ENOMEM;
        ++HeapFailures;

        return NULL;
    }

    ++HeapAllocations;

    return HeapPayloadOf(block);
}

void *calloc(size_t count, size_t size)
{
    size_t total;
    void *pointer;

    /*
     * The product is checked before it is formed, by division rather than by
     * forming it and looking at the result. A product that has wrapped is a
     * small number that says nothing about the two that produced it, and the
     * failure it causes is not in this function: the caller writes `count`
     * elements into a block sized for however few the wrapped product came to.
     */
    if ((count != 0U) && (size > (SIZE_MAX / count)))
    {
        errno = ENOMEM;
        ++HeapFailures;

        return NULL;
    }

    total = count * size;
    pointer = malloc(total);

    if (pointer == NULL)
    {
        return NULL;
    }

    /*
     * The bytes are cleared here and are not assumed to arrive clear. This
     * kernel does zero every page it maps, so a block that has never been used
     * is already all bits zero — and a block that has been used and released is
     * not, which is the case this line exists for and the one a test upon a
     * fresh heap would never reach.
     */
    (void)memset(pointer, 0, total);

    return pointer;
}

void *realloc(void *pointer, size_t size)
{
    HeapBlock *block;
    size_t needed;
    size_t payload;
    void *fresh;

    if (pointer == NULL)
    {
        return malloc(size);
    }

    block = HeapBlockOf(pointer);

    if (!HeapBlockIsLive(block))
    {
        /*
         * Section 7.22.3.5, paragraph 3, makes this undefined. It is defined here
         * as a refusal for the reason `free`'s is, and it reports EINVAL rather
         * than ENOMEM: the machine has not run out of anything, the caller has
         * named something that is not an allocation.
         */
        ++HeapRefusals;
        errno = EINVAL;

        return NULL;
    }

    ++HeapReallocations;

    if (!HeapBlockSizeFor(size, &needed))
    {
        errno = ENOMEM;
        ++HeapFailures;

        return NULL;
    }

    /* Smaller, or no larger: the block stays where it is and gives back whatever
     * remainder is worth giving back. */
    if (needed <= block->size)
    {
        HeapSplit(block, needed);

        return pointer;
    }

    if (HeapAbsorbNext(block, needed))
    {
        return pointer;
    }

    /*
     * The move. The old block is released only after the new one is in hand and
     * its contents are copied, because Section 7.22.3.5, paragraph 3, requires
     * that a failure leave the old object untouched — and an implementation that
     * freed first would have nothing to return it to the caller as.
     */
    fresh = malloc(size);

    if (fresh == NULL)
    {
        return NULL;
    }

    payload = block->size - sizeof(HeapBlock);

    if (payload > size)
    {
        payload = size;
    }

    (void)memcpy(fresh, pointer, payload);
    free(pointer);

    return fresh;
}

void free(void *pointer)
{
    HeapBlock *block;

    if (pointer == NULL)
    {
        return;
    }

    block = HeapBlockOf(pointer);

    if (!HeapBlockIsLive(block))
    {
        /*
         * A pointer this allocator did not hand out, or one it handed out and has
         * already taken back. Both are undefined by Section 7.22.3.3, paragraph
         * 2, and both are refused rather than acted upon: a second release would
         * put a block upon the free list twice, after which two later requests
         * are met with the same memory and the failure appears in whichever of
         * the two callers writes second.
         */
        ++HeapRefusals;

        return;
    }

    HeapInsert(block);
    ++HeapReleases;
}

/* -------------------------------------------------------------------------
 * The census.
 * ------------------------------------------------------------------------- */

void OxysHeapInspect(OxysHeapCensus *census)
{
    const HeapBlock *block;
    size_t overhead;

    if (census == NULL)
    {
        return;
    }

    (void)memset(census, 0, sizeof *census);

    census->regions = HeapRegionCount;
    census->bytes = HeapRegionBytes;
    census->blocks = HeapBlockCount;

    for (block = HeapFreeList; block != NULL; block = block->next)
    {
        const size_t available = block->size - sizeof(HeapBlock);

        ++census->free_blocks;
        census->available += available;

        if (available > census->largest)
        {
            census->largest = available;
        }
    }

    overhead = HeapBlockCount * sizeof(HeapBlock);
    census->overhead = overhead;

    /*
     * What is handed out is what is left when the headers and the free payloads
     * are taken from the regions. It is derived rather than counted because
     * counting it would mean a walk of the allocated blocks, and the heap keeps
     * no list of those: a block that is not free is reachable only from the
     * pointer its owner holds, which is the whole economy of an allocator of
     * this shape.
     *
     * The subtraction is guarded because the three quantities are maintained by
     * different paths, and a defect in any of them should produce a census that
     * is visibly wrong rather than one that has wrapped to an enormous number
     * and looks like a heap four exbibytes in use.
     */
    if ((overhead + census->available) <= census->bytes)
    {
        census->allocated = census->bytes - overhead - census->available;
    }

    census->allocations = HeapAllocations;
    census->releases = HeapReleases;
    census->reallocations = HeapReallocations;
    census->extensions = HeapExtensions;
    census->failures = HeapFailures;
    census->refusals = HeapRefusals;
    census->splits = HeapSplits;
    census->coalescences = HeapCoalescences;
}
