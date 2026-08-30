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
#include <oxys/bootinfo.h>

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
 * A flag reserved to software, marking a page as copy-on-write.
 *
 * Intel SDM, Volume 3A, Table 4-19 ("Format of a Page-Table Entry that Maps a
 * 4-KByte Page"), records bits 11:9 as Ignored, meaning the processor neither
 * interprets nor modifies them. Bit 9 is therefore available, and a page so
 * marked is one whose frame may be shared and which must be duplicated before
 * any write to it is permitted.
 *
 * A copy-on-write page is always mapped without PAGE_ENTRY_WRITABLE. The two
 * conditions together are what cause the processor to raise the fault that the
 * resolution routine then handles; the flag alone would be inert, since the
 * processor ignores it.
 */
#define PAGE_ENTRY_COPY_ON_WRITE UINT64_C(0x200)

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
void PagingInitialise(const BootInformation *information);

/*
 * Reports whether the direct physical map is established and active. Until it
 * is, only the first gibibyte of physical memory is addressable by the kernel,
 * and frames intended for kernel use must be obtained with FrameAllocateBelow.
 */
bool PagingDirectMapIsActive(void);

/* The extent of physical memory covered by the direct physical map. */
uint64_t PagingDirectMapExtent(void);

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

/*
 * Establishes a 4 KiB mapping in the kernel hierarchy and invalidates any stale
 * translation for the address. The flags are those of Table 4-15;
 * PAGE_ENTRY_PRESENT is supplied by the implementation.
 */
void PagingMapKernelPage(VirtualAddress virtual_address,
                         PhysicalAddress physical_address,
                         uint64_t flags);

/*
 * Removes a 4 KiB mapping from the kernel hierarchy and invalidates the
 * translation. The frame that was mapped is not freed; the caller owns it.
 */
void PagingUnmapKernelPage(VirtualAddress virtual_address);

/*
 * Marks a mapped page as copy-on-write: the writable flag is cleared and the
 * software flag set, so that the next write to it raises a page fault which
 * PagingResolveCopyOnWriteFault can resolve.
 *
 * Returns false if the address is not mapped by a 4 KiB page. Large pages are
 * not supported, a copy-on-write fault upon one requiring the mapping to be
 * split before it could be resolved.
 */
bool PagingMarkCopyOnWrite(VirtualAddress address);

/* Reports whether the page containing the address carries the software flag. */
bool PagingIsCopyOnWrite(VirtualAddress address);

/*
 * Attempts to resolve a page fault as a copy-on-write fault.
 *
 * Returns true if the fault was resolved, in which case the caller must return
 * from the exception so that the offending instruction is restarted. Returns
 * false if the fault was not a copy-on-write fault, or could not be resolved,
 * in which case the caller must report it.
 */
bool PagingResolveCopyOnWriteFault(VirtualAddress address);

/* The number of copy-on-write faults resolved, frames duplicated, and faults
 * resolved without a duplication because the frame had a single referrer. */
uint64_t PagingCopyOnWriteFaultCount(void);
uint64_t PagingCopyOnWriteCopyCount(void);
uint64_t PagingCopyOnWriteSoleOwnerCount(void);

/* The physical address of the active page-map level 4 table. */
PhysicalAddress PagingKernelRoot(void);

/* Emits a summary of the hierarchy upon the console and the serial port. */
void PagingReport(void);

#endif /* OXYS_PAGING_H */
