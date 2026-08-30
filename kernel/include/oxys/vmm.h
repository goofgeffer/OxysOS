/*
 * File: kernel/include/oxys/vmm.h
 * Purpose: Declares the kernel virtual address allocator, which issues ranges of
 *          the kernel arena backed by frames from the physical allocator.
 * Key definitions: KERNEL_ARENA_BASE, KERNEL_ARENA_SIZE, KernelVirtualInitialise,
 *          KernelPagesAllocate, KernelPagesFree, KernelVirtualReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the 4 KiB page is the unit of mapping.
 *   - docs/MEMORY-LAYOUT.md, Sections 2 and 10: the placement and design of the
 *     arena.
 */

#ifndef OXYS_VMM_H
#define OXYS_VMM_H

#include <oxys/types.h>
#include <oxys/memory.h>

/*
 * The base and extent of the kernel arena, the region from which virtually
 * contiguous kernel allocations are issued. It is disjoint from both the direct
 * physical map and the kernel image window; refer to docs/MEMORY-LAYOUT.md,
 * Section 2.
 */
#define KERNEL_ARENA_BASE UINT64_C(0xFFFFC00000000000)
#define KERNEL_ARENA_SIZE UINT64_C(0x0000200000000000) /* 32 TiB. */

/*
 * Prepares the arena. The physical frame allocator and the paging hierarchy must
 * both be initialised before this is called.
 */
void KernelVirtualInitialise(void);

/*
 * Allocates a virtually contiguous range of the given number of pages, backs
 * every page with a frame from the physical allocator, and returns the address
 * of the first page. Returns NULL if the arena is exhausted or if no frame is
 * available.
 *
 * The pages are mapped writable and are not zeroed. The physical frames backing
 * the range are not contiguous and must not be assumed to be; a caller needing
 * physically contiguous memory, such as a device driver programming a bus
 * master, requires a facility this allocator does not provide.
 */
void *KernelPagesAllocate(size_t page_count);

/*
 * Releases a range previously returned by KernelPagesAllocate, unmapping every
 * page and returning its frame to the physical allocator. The page count must
 * equal that given to the allocating call.
 */
void KernelPagesFree(void *address, size_t page_count);

/* The number of pages presently allocated from the arena. */
size_t KernelVirtualPagesInUse(void);

/* Emits a summary of the arena's state upon the console and the serial port. */
void KernelVirtualReport(void);

#endif /* OXYS_VMM_H */
