/*
 * File: kernel/include/oxys/addrspace.h
 * Purpose: Declares the address space, being a paging hierarchy that may be
 *          created, cloned by copy-on-write, activated and destroyed. This is
 *          the substrate upon which fork() is built in sub-task 6.6.
 * Key definitions: AddressSpace, AddressSpaceCreate, AddressSpaceClone,
 *          AddressSpaceDestroy, AddressSpaceSwitch, AddressSpaceMapPage.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5 and Figure 4-8: four-level paging. The page-map level 4 index
 *     occupies bits 47:39 of a linear address, so an index below 256 has bit 47
 *     clear and names the lower half of the address space.
 *   - Intel SDM, Volume 1, Section 3.3.7.1: an address is canonical only if bits
 *     63:48 replicate bit 47. The lower half is therefore
 *     0x0000000000000000 to 0x00007FFFFFFFFFFF and the higher half
 *     0xFFFF800000000000 to 0xFFFFFFFFFFFFFFFF; there are no canonical addresses
 *     between them.
 *   - Intel SDM, Volume 3A, Section 4.10.4.1: writing CR3 invalidates every
 *     translation-lookaside-buffer entry for the current process context save
 *     those marked global.
 *   - docs/design/MEMORY-LAYOUT.md, Section 14: the design of address-space cloning.
 */

#ifndef OXYS_ADDRSPACE_H
#define OXYS_ADDRSPACE_H

#include <oxys/types.h>
#include <oxys/memory.h>
#include <oxys/paging.h>

/*
 * The number of page-map level 4 entries that describe the lower half of the
 * address space. An index below this value has bit 47 of the linear address
 * clear; an index at or above it has that bit set. The division is therefore
 * exactly the division between the two canonical halves, and it is the boundary
 * at which cloning stops: the lower half belongs to the address space and is
 * cloned, the higher half is the kernel and is shared.
 */
#define PAGE_TABLE_USER_ENTRY_COUNT 256U

/*
 * An address space is a paging hierarchy and nothing besides. The structure
 * exists so that the hierarchy may be named, passed and later extended with the
 * accounting a process will require, rather than passed as a bare physical
 * address that nothing distinguishes from any other frame.
 */
typedef struct AddressSpace
{
    /* The physical address of the page-map level 4 table. */
    PhysicalAddress root;
} AddressSpace;

/*
 * Constructs an empty address space: a page-map level 4 table whose lower half
 * is empty and whose higher half is that of the kernel.
 *
 * The higher-half entries are copied from the kernel hierarchy, so every address
 * space refers to the same kernel page tables rather than to copies of them. The
 * kernel is thereby mapped identically in every address space, which is what
 * permits an interrupt to be serviced whichever space is active, and what permits
 * the kernel to continue executing across a change of CR3.
 *
 * Returns false if a frame could not be obtained.
 */
bool AddressSpaceCreate(AddressSpace *space);

/*
 * Clones an address space by the copy-on-write discipline.
 *
 * The lower half of the source is walked. The paging structures themselves are
 * duplicated, since the two spaces must be able to diverge; the frames they map
 * are not. Every writable page is instead made read-only and marked
 * copy-on-write in both hierarchies, and a reference to its frame is recorded
 * for the new holder. A read-only page is shared unchanged, there being nothing
 * to protect against.
 *
 * Marking both hierarchies is essential rather than symmetric: were only the
 * child protected, a write by the parent would alter memory the child observes.
 *
 * Returns false if a frame could not be obtained, or if the lower half contains
 * a large page, which cannot be shared at 4 KiB granularity without first being
 * split. Upon failure the destination is destroyed and left empty.
 */
bool AddressSpaceClone(AddressSpace *destination, const AddressSpace *source);

/*
 * Releases an address space: every frame mapped in its lower half, every paging
 * structure describing that half, and the page-map level 4 table itself.
 *
 * A mapped frame is released by FrameFree, which returns it to the allocator only
 * upon the last reference. A frame shared with another address space therefore
 * survives the destruction of this one. The higher half is not walked; it is the
 * kernel's and is merely referred to.
 *
 * The address space must not be the active one.
 */
void AddressSpaceDestroy(AddressSpace *space);

/*
 * Loads the address space into CR3, making it the one the processor translates
 * through.
 */
void AddressSpaceSwitch(const AddressSpace *space);

/*
 * Establishes a 4 KiB mapping within an address space. The flags are those of
 * Intel SDM, Volume 3A, Table 4-15; PAGE_ENTRY_PRESENT is supplied by the
 * implementation.
 */
void AddressSpaceMapPage(AddressSpace *space, VirtualAddress virtual_address,
                         PhysicalAddress physical_address, uint64_t flags);

/* The address space describing the kernel hierarchy alone. */
const AddressSpace *AddressSpaceKernel(void);

/* Accounting: the number of clones performed, the number of pages shared by
 * them, and the number of those pages that had to be protected because they were
 * writable. */
uint64_t AddressSpaceCloneCount(void);
uint64_t AddressSpaceSharedPageCount(void);
uint64_t AddressSpaceProtectedPageCount(void);

/* Emits a summary of the accounting upon the console and the serial port. */
void AddressSpaceReport(void);

#endif /* OXYS_ADDRSPACE_H */
