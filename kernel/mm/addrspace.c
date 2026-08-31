/*
 * File: kernel/mm/addrspace.c
 * Purpose: Implements the address space: the creation, cloning by copy-on-write,
 *          activation and destruction of a paging hierarchy. Sub-task 2.8, and
 *          the last of the memory-management substrate upon which fork() will be
 *          built in sub-task 6.6.
 * Key functions: AddressSpaceCreate, AddressSpaceClone, AddressSpaceDestroy,
 *          AddressSpaceSwitch, AddressSpaceCloneStructure, AddressSpaceReleaseStructure.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5 and Figure 4-8: four-level paging, and the decomposition of a
 *     linear address that makes page-map level 4 indices 0 to 255 the lower half
 *     of the address space and 256 to 511 the higher half.
 *   - Intel SDM, Volume 3A, Table 4-15: the paging-structure entry flags.
 *   - Intel SDM, Volume 3A, Section 4.10.4.1: writing CR3 invalidates every
 *     translation-lookaside-buffer entry for the current process context save
 *     those marked global; INVLPG invalidates the entries for one page.
 *   - Intel SDM, Volume 3A, Section 6.15: a write to a read-only page raises a
 *     page fault, in supervisor mode only where CR0.WP is set. That fault is what
 *     the protection established here exists to provoke.
 *   - docs/MEMORY-LAYOUT.md, Section 14.
 *
 * Concurrency. Cloning walks and modifies the source hierarchy, and is therefore
 * not safe against a concurrent fault upon the same address space. From sub-task
 * 6.9 it must be performed under the lock governing the address space, and the
 * invalidation performed here must be accompanied by a shootdown to the other
 * processors upon which the source may be active.
 */

#include <oxys/addrspace.h>
#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/kernel.h>

/* The address space describing the kernel hierarchy, formed upon first request. */
static AddressSpace AddressSpaceKernelSpace;

/* Accounting. */
static uint64_t AddressSpaceClones;
static uint64_t AddressSpaceSharedPages;
static uint64_t AddressSpaceProtectedPages;

/*
 * The depth of the hierarchy, naming each level by the count of translation steps
 * that remain beneath it. Level 4 is the page-map level 4 table and level 1 the
 * page table, whose entries map frames rather than further structures.
 */
#define ADDRESS_SPACE_LEAF_LEVEL 1U
#define ADDRESS_SPACE_ROOT_LEVEL 4U

/*
 * Duplicates one paging structure of the lower half from a source hierarchy into
 * a destination hierarchy.
 *
 * The recursion is bounded at four frames by the architecture itself, the
 * hierarchy having exactly four levels, so it carries none of the risk that
 * unbounded recursion would carry upon a kernel stack.
 *
 * Returns false if a frame could not be obtained, or if a large page is
 * encountered. A large page in the lower half cannot be shared by this
 * mechanism: the copy-on-write resolution of sub-task 2.7 operates upon a
 * page-table entry, and resolving a fault upon a 2 MiB page would require the
 * mapping to be split into 4 KiB pages first. Nothing presently establishes such
 * a mapping in the lower half, so the case is rejected rather than provided for.
 */
static bool AddressSpaceCloneStructure(PhysicalAddress destination,
                                       PhysicalAddress source,
                                       unsigned int level,
                                       size_t entry_count)
{
    uint64_t *source_entries = PagingTableEntries(source);
    uint64_t *destination_entries = PagingTableEntries(destination);

    for (size_t index = 0U; index < entry_count; ++index)
    {
        uint64_t entry = source_entries[index];
        uint64_t flags;
        PhysicalAddress target;

        if ((entry & PAGE_ENTRY_PRESENT) == 0U)
        {
            continue;
        }

        if (level != ADDRESS_SPACE_LEAF_LEVEL && (entry & PAGE_ENTRY_LARGE) != 0U)
        {
            return false;
        }

        flags = entry & ~PAGE_ENTRY_ADDRESS_MASK;
        target = entry & PAGE_ENTRY_ADDRESS_MASK;

        if (level == ADDRESS_SPACE_LEAF_LEVEL)
        {
            /*
             * A writable page must be protected in both hierarchies before the
             * frame is shared. Withdrawing write permission is what raises the
             * fault; the software flag records why it was raised, so that
             * PagingResolveCopyOnWriteFault can distinguish this fault from a
             * write to a page that is genuinely read-only.
             *
             * A page that is already read-only is shared as it stands. It cannot
             * be written by either holder, so neither can observe a change made
             * by the other, and there is nothing for the protection to prevent.
             */
            if ((flags & PAGE_ENTRY_WRITABLE) != 0U)
            {
                flags = (flags & ~PAGE_ENTRY_WRITABLE) | PAGE_ENTRY_COPY_ON_WRITE;
                source_entries[index] = target | flags;
                ++AddressSpaceProtectedPages;
            }

            destination_entries[index] = target | flags;

            /*
             * The new holder's reference is recorded before the mapping can be
             * used. The frame now has two referrers, which is precisely the
             * condition under which the resolution routine duplicates it rather
             * than merely restoring write permission.
             */
            FrameReferenceIncrement(target);
            ++AddressSpaceSharedPages;

            continue;
        }

        /*
         * An intermediate structure is duplicated rather than shared. The two
         * address spaces must be able to diverge, and they diverge by acquiring
         * different entries; a shared page table would propagate every such
         * change from one space to the other.
         */
        {
            PhysicalAddress copy = PagingAllocateStructure();

            if (copy == FRAME_ALLOCATION_FAILED)
            {
                return false;
            }

            destination_entries[index] = copy | flags;

            if (!AddressSpaceCloneStructure(copy, target, level - 1U,
                                            PAGE_TABLE_ENTRY_COUNT))
            {
                return false;
            }
        }
    }

    return true;
}

/*
 * Releases one paging structure of the lower half and everything beneath it.
 *
 * At the leaf, one reference to each mapped frame is released; the frame returns
 * to the allocator only upon the last, so a frame still shared with another
 * address space survives. Above the leaf, the structure is released outright,
 * being private to this hierarchy by construction.
 */
static void AddressSpaceReleaseStructure(PhysicalAddress table, unsigned int level,
                                         size_t entry_count)
{
    uint64_t *entries = PagingTableEntries(table);

    for (size_t index = 0U; index < entry_count; ++index)
    {
        uint64_t entry = entries[index];

        if ((entry & PAGE_ENTRY_PRESENT) == 0U)
        {
            continue;
        }

        entries[index] = 0U;

        if (level == ADDRESS_SPACE_LEAF_LEVEL)
        {
            FrameFree(entry & PAGE_ENTRY_ADDRESS_MASK);

            continue;
        }

        /*
         * A large page above the leaf maps frames directly and has no structure
         * beneath it. AddressSpaceCloneStructure rejects such a mapping, so one
         * can be present here only if it was established directly; the frames it
         * maps are the caller's and are not released.
         */
        if ((entry & PAGE_ENTRY_LARGE) != 0U)
        {
            continue;
        }

        AddressSpaceReleaseStructure(entry & PAGE_ENTRY_ADDRESS_MASK, level - 1U,
                                     PAGE_TABLE_ENTRY_COUNT);
    }

    PagingReleaseStructure(table);
}

bool AddressSpaceCreate(AddressSpace *space)
{
    PhysicalAddress root = PagingAllocateStructure();
    const uint64_t *kernel_entries;
    uint64_t *entries;

    if (root == FRAME_ALLOCATION_FAILED)
    {
        return false;
    }

    kernel_entries = PagingTableEntries(PagingKernelRoot());
    entries = PagingTableEntries(root);

    /*
     * The higher half is shared with the kernel by copying its page-map level 4
     * entries, which refer to the kernel's own page tables. The lower half is
     * left as PagingAllocateStructure cleared it: empty.
     *
     * A mapping established in the kernel's higher half after this point is
     * visible here, because the structures beneath these entries are the very
     * ones the kernel modifies. The exception is a mapping that requires a
     * page-map level 4 entry that did not exist when this copy was taken; the
     * kernel establishes all four of its higher-half entries during
     * PagingInitialise, so no such case arises.
     */
    for (size_t index = PAGE_TABLE_USER_ENTRY_COUNT;
         index < PAGE_TABLE_ENTRY_COUNT;
         ++index)
    {
        entries[index] = kernel_entries[index];
    }

    space->root = root;

    return true;
}

bool AddressSpaceClone(AddressSpace *destination, const AddressSpace *source)
{
    if (!AddressSpaceCreate(destination))
    {
        return false;
    }

    if (!AddressSpaceCloneStructure(destination->root, source->root,
                                    ADDRESS_SPACE_ROOT_LEVEL,
                                    PAGE_TABLE_USER_ENTRY_COUNT))
    {
        /*
         * The clone is abandoned and the partial destination released. The
         * protection already applied to the source is left in place: it is not
         * incorrect, merely unnecessary, and the first write to such a page will
         * find a frame with one referrer and restore write permission without a
         * copy.
         */
        AddressSpaceDestroy(destination);

        return false;
    }

    /*
     * The source hierarchy has been modified: pages that were writable are so no
     * longer. Where it is the active hierarchy, the processor may hold cached
     * translations that still grant write permission, and a write through such a
     * translation would proceed without the fault upon which the whole mechanism
     * depends.
     *
     * CR3 is rewritten rather than each page invalidated in turn. Intel SDM,
     * Volume 3A, Section 4.10.4.1, provides that this discards every entry for
     * the current process context save the global ones, and a clone may protect
     * an arbitrary number of pages; the single write is bounded where a sequence
     * of INVLPG instructions is not.
     */
    if (source->root == PagingActiveRoot())
    {
        PagingActivateRoot(source->root);
    }

    ++AddressSpaceClones;

    return true;
}

void AddressSpaceDestroy(AddressSpace *space)
{
    if (space->root == FRAME_ALLOCATION_FAILED)
    {
        return;
    }

    if (space->root == PagingActiveRoot())
    {
        KernelPanic("An attempt was made to destroy the active address space.");
    }

    AddressSpaceReleaseStructure(space->root, ADDRESS_SPACE_ROOT_LEVEL,
                                 PAGE_TABLE_USER_ENTRY_COUNT);

    space->root = FRAME_ALLOCATION_FAILED;
}

void AddressSpaceSwitch(const AddressSpace *space)
{
    PagingActivateRoot(space->root);
}

void AddressSpaceMapPage(AddressSpace *space, VirtualAddress virtual_address,
                         PhysicalAddress physical_address, uint64_t flags)
{
    PagingMapPageIn(space->root, virtual_address, physical_address, flags);
}

const AddressSpace *AddressSpaceKernel(void)
{
    AddressSpaceKernelSpace.root = PagingKernelRoot();

    return &AddressSpaceKernelSpace;
}

uint64_t AddressSpaceCloneCount(void)
{
    return AddressSpaceClones;
}

uint64_t AddressSpaceSharedPageCount(void)
{
    return AddressSpaceSharedPages;
}

uint64_t AddressSpaceProtectedPageCount(void)
{
    return AddressSpaceProtectedPages;
}

void AddressSpaceReport(void)
{
    KernelWriteString("Address spaces: clones ");
    KernelWriteDecimal(AddressSpaceClones);
    KernelWriteString(", pages shared ");
    KernelWriteDecimal(AddressSpaceSharedPages);
    KernelWriteString(", of which protected ");
    KernelWriteDecimal(AddressSpaceProtectedPages);
    KernelWriteString(".\n");
}
