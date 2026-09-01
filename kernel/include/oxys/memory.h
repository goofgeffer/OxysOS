/*
 * File: kernel/include/oxys/memory.h
 * Purpose: Defines the page granularity of the x86_64 architecture and the
 *          alignment helpers used throughout the memory management subsystem.
 * Key definitions: PAGE_SIZE, PAGE_SHIFT, PAGE_SIZE_LARGE, AlignUp, AlignDown,
 *          IsPageAligned.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: four-level paging translates a linear address to a 4 KiB
 *     page, or to a 2 MiB page where the PS flag is set in the page-directory
 *     entry.
 */

#ifndef OXYS_MEMORY_H
#define OXYS_MEMORY_H

#include <oxys/types.h>

/* The size of the ordinary page, and the count of bits in its offset field. */
#define PAGE_SIZE  UINT64_C(4096)
#define PAGE_SHIFT 12U

/* The size of the large page produced by the PS flag in a page-directory entry. */
#define PAGE_SIZE_LARGE UINT64_C(0x200000)

/* The extent of the low memory reserved in its entirety; refer to
 * docs/design/MEMORY-LAYOUT.md, Section 6.1, for the reasons. */
#define LOW_MEMORY_LIMIT UINT64_C(0x100000)

/*
 * Rounds a value upward to the next multiple of the given alignment, which must
 * be a power of two. The expression is exact for every input that does not
 * overflow, since the alignment is a power of two and the addition therefore
 * cannot carry past the mask.
 */
static inline uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + (alignment - 1U)) & ~(alignment - 1U);
}

/* Rounds a value downward to the preceding multiple of the given alignment. */
static inline uint64_t AlignDown(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1U);
}

/* Reports whether an address lies upon a page boundary. */
static inline bool IsPageAligned(uint64_t value)
{
    return (value & (PAGE_SIZE - 1U)) == 0U;
}

#endif /* OXYS_MEMORY_H */
