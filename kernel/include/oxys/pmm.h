/*
 * File: kernel/include/oxys/pmm.h
 * Purpose: Declares the physical frame allocator, which owns every page frame of
 *          physical memory and is the sole authority upon which frames are free.
 * Key definitions: PhysicalMemoryInitialise, FrameAllocate, FrameAllocateBelow,
 *          FrameFree, FrameTotalCount, FrameFreeCount, PhysicalMemoryReport,
 *          FRAME_ALLOCATION_FAILED.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the 4 KiB frame is the unit of physical allocation.
 *   - Multiboot2 Specification 2.0, Section 3.6.8: the memory map from which the
 *     set of existing frames is derived, and its warning that the map includes
 *     regions occupied by the kernel and the boot information structure.
 *   - docs/MEMORY-LAYOUT.md, Section 6: the extents reserved and the reasons.
 */

#ifndef OXYS_PMM_H
#define OXYS_PMM_H

#include <oxys/types.h>
#include <oxys/memory.h>
#include <oxys/bootinfo.h>

/*
 * The value returned by an allocation that could not be satisfied. Physical
 * address zero cannot be a valid allocation, because the whole of the low
 * mebibyte is reserved, so it is available as a sentinel without ambiguity.
 */
#define FRAME_ALLOCATION_FAILED ((PhysicalAddress)0)

/*
 * Constructs the frame allocator from the parsed boot information. Every frame
 * is initially marked unavailable; the frames of each region classified as
 * usable are then released; and the extents that must never be issued are marked
 * again. This order is deliberate, so that a region omitted from the map is
 * treated as unavailable rather than as free.
 *
 * This function must be called before any other declared here.
 */
void PhysicalMemoryInitialise(const BootInformation *information);

/*
 * Allocates one 4 KiB frame and returns its physical address, or
 * FRAME_ALLOCATION_FAILED if no frame is available. The contents of the frame
 * are not defined; a caller requiring a zeroed frame must zero it.
 */
PhysicalAddress FrameAllocate(void);

/*
 * Allocates one frame whose physical address is below the given limit.
 *
 * This exists because a frame is useful to the kernel only if the kernel can
 * address it. Until the direct physical map of sub-task 2.4 exists, only the
 * first gibibyte of physical memory is mapped into the higher half, so the page
 * tables built by sub-task 2.3 must themselves reside below that boundary.
 */
PhysicalAddress FrameAllocateBelow(PhysicalAddress limit);

/*
 * Returns a frame to the allocator. Freeing a frame that is already free, or one
 * that lies outside the range the allocator governs, is a defect in the caller
 * and is reported rather than ignored.
 */
void FrameFree(PhysicalAddress frame);

/* The number of frames the allocator governs, being every frame below the
 * highest usable address, whether or not it is usable. */
size_t FrameTotalCount(void);

/* The number of frames presently available for allocation. */
size_t FrameFreeCount(void);

/* The number of frames presently allocated or reserved. */
size_t FrameUsedCount(void);

/* Emits a summary of the allocator's state upon the console and serial port. */
void PhysicalMemoryReport(void);

#endif /* OXYS_PMM_H */
