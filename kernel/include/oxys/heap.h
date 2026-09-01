/*
 * File: kernel/include/oxys/heap.h
 * Purpose: Declares the kernel heap, a slab allocator over the kernel virtual
 *          address allocator, providing allocation of arbitrary size.
 * Key definitions: KernelAllocate, KernelAllocateZeroed, KernelFree,
 *          KernelHeapInitialise, KernelHeapReport.
 * References:
 *   - docs/design/MEMORY-LAYOUT.md, Section 11: the design of the heap.
 *   - Bonwick, J., "The Slab Allocator: An Object-Caching Kernel Memory
 *     Allocator", USENIX Summer 1994. Consulted for the object-caching concept
 *     alone; the implementation here is original and considerably simpler,
 *     having neither constructors nor per-processor caches.
 */

#ifndef OXYS_HEAP_H
#define OXYS_HEAP_H

#include <oxys/types.h>

/*
 * The greatest allocation served from a size class. A request larger than this
 * is served by whole pages from the kernel arena.
 */
#define HEAP_LARGEST_SIZE_CLASS 2048U

/* The alignment guaranteed of every pointer returned. It is sufficient for every
 * scalar type of the AMD64 ABI. */
#define HEAP_ALIGNMENT 16U

/*
 * Prepares the heap. The kernel virtual address allocator must be initialised
 * before this is called.
 */
void KernelHeapInitialise(void);

/*
 * Allocates at least the requested number of bytes and returns a pointer aligned
 * upon HEAP_ALIGNMENT, or NULL if the request cannot be satisfied. The contents
 * are not defined. A request of zero bytes returns NULL.
 */
void *KernelAllocate(size_t size);

/* As KernelAllocate, but the returned memory is cleared to zero. */
void *KernelAllocateZeroed(size_t size);

/*
 * Releases an allocation previously returned by KernelAllocate. Passing NULL is
 * permitted and does nothing. Passing any other pointer not returned by
 * KernelAllocate, or releasing the same pointer twice, is a defect in the caller
 * and is reported rather than ignored.
 */
void KernelFree(void *address);

/* Emits a summary of the heap's state upon the console and the serial port. */
void KernelHeapReport(void);

#endif /* OXYS_HEAP_H */
