/*
 * File: kernel/include/oxys/paging.h
 * Purpose: Declares the permanent kernel paging hierarchy, which supersedes the
 *          boot-time structures built in boot/boot.asm, and the paging-structure
 *          entry flags defined by the architecture.
 * Key definitions: PAGE_ENTRY_PRESENT and the remaining entry flags,
 *          PagingInitialise, PagingTranslate, PagingKernelRoot, PagingReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5 and Figure 4-8: four-level paging, and the decomposition of a
 *     linear address into the indices of the four structures.
 *   - Intel SDM, Volume 3A, Table 4-15: the paging-structure entry flags.
 *   - Intel SDM, Volume 3A, Section 4.10.4.1: writing CR3 invalidates every
 *     translation-lookaside-buffer entry associated with the current process
 *     context, save those for global pages.
 */

#ifndef OXYS_PAGING_H
#define OXYS_PAGING_H

#include <oxys/types.h>
#include <oxys/memory.h>

/*
 * Paging-structure entry flags, per Intel SDM, Volume 3A, Table 4-15.
 */
#define PAGE_ENTRY_PRESENT   UINT64_C(0x001) /* The entry is valid. */
#define PAGE_ENTRY_WRITABLE  UINT64_C(0x002) /* Writes are permitted. */
#define PAGE_ENTRY_USER      UINT64_C(0x004) /* Accessible at privilege level 3. */
#define PAGE_ENTRY_WRITE_THROUGH UINT64_C(0x008)
#define PAGE_ENTRY_CACHE_DISABLE UINT64_C(0x010)
#define PAGE_ENTRY_ACCESSED  UINT64_C(0x020) /* Set by the processor upon access. */
#define PAGE_ENTRY_DIRTY     UINT64_C(0x040) /* Set by the processor upon write. */
#define PAGE_ENTRY_LARGE     UINT64_C(0x080) /* PS: the entry maps a large page. */
#define PAGE_ENTRY_GLOBAL    UINT64_C(0x100) /* Not invalidated by a CR3 write. */

/*
 * The bits of an entry that hold the physical address of the next structure or
 * of the mapped page. Bits 51:12 of the entry, the remainder being flags or
 * reserved.
 */
#define PAGE_ENTRY_ADDRESS_MASK UINT64_C(0x000FFFFFFFFFF000)

/* The number of entries in every paging structure, each being 4096 bytes of
 * 8-byte entries. */
#define PAGE_TABLE_ENTRY_COUNT 512U

/*
 * Constructs the permanent kernel paging hierarchy from frames obtained from the
 * physical allocator, activates it by writing CR3, and thereby removes the
 * identity mapping of low memory that the boot-time hierarchy established.
 *
 * The physical frame allocator must have been initialised before this is called.
 * This function does not return if a required frame cannot be allocated.
 */
void PagingInitialise(void);

/*
 * Resolves a virtual address to the physical address it maps to, by walking the
 * active hierarchy in software. Returns 0 if the address is not mapped.
 *
 * This is the means by which the hierarchy is verified without provoking a page
 * fault, there being no interrupt descriptor table until Phase 3 and hence no
 * handler to recover from one.
 */
PhysicalAddress PagingTranslate(VirtualAddress address);

/*
 * Reports whether the mapping governing a virtual address permits writing,
 * accumulating the writable flag across all four levels. Intel SDM, Volume 3A,
 * Section 4.6, provides that the permissions of a translation are the
 * conjunction of those at every level, so every level must be consulted.
 *
 * Returns false if the address is not mapped.
 */
bool PagingAddressIsWritable(VirtualAddress address);

/* The physical address of the active page-map level 4 table. */
PhysicalAddress PagingKernelRoot(void);

/* Emits a summary of the hierarchy upon the console and the serial port. */
void PagingReport(void);

#endif /* OXYS_PAGING_H */
