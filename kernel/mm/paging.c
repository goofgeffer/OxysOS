/*
 * File: kernel/mm/paging.c
 * Purpose: Constructs and activates the permanent kernel paging hierarchy,
 *          superseding the boot-time structures of boot/boot.asm and removing
 *          the identity mapping of low memory that they established.
 * Key functions: PagingInitialise, PagingTranslate, PagingKernelRoot,
 *          PagingReport, PagingAllocateTable, PagingMapPage, PagingMapLargePage.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5 and Figure 4-8: four-level paging; a linear address is
 *     decomposed into a page-map-level-4 index at bits 47:39, a
 *     page-directory-pointer index at bits 38:30, a page-directory index at bits
 *     29:21, a page-table index at bits 20:12, and an offset at bits 11:0.
 *   - Intel SDM, Volume 3A, Table 4-15: the paging-structure entry flags,
 *     including PS, which in a page-directory entry maps a 2 MiB page.
 *   - Intel SDM, Volume 3A, Section 4.10.4.1: writing CR3 invalidates every
 *     translation-lookaside-buffer entry for the current process context save
 *     those marked global. No mapping here is marked global, so the write
 *     performed by PagingActivate flushes the whole of the buffer.
 *   - docs/MEMORY-LAYOUT.md, Section 8: the design of this hierarchy.
 *
 * Concurrency. This code runs once, before any application processor is started,
 * and requires no synchronisation. From sub-task 6.9, any later modification of
 * a mapping shared between processors must be followed by a translation
 * lookaside buffer shootdown by inter-processor interrupt.
 */

#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/kernel.h>

/*
 * The extent of physical memory mapped into the higher half. It matches the
 * extent that the boot-time hierarchy established, so that every address the
 * kernel has formed up to this point remains valid across the transition.
 */
#define KERNEL_PHYSICAL_MAP_LIMIT UINT64_C(0x40000000)

/*
 * The extent of physical memory that is mapped with 4 KiB pages rather than with
 * 2 MiB pages. The kernel image lies within the first 2 MiB of physical memory,
 * and per-section permissions cannot be applied at a granularity coarser than
 * the sections themselves.
 */
#define KERNEL_FINE_MAP_LIMIT PAGE_SIZE_LARGE

/*
 * The section boundaries defined by linker.ld, as virtual addresses. Only the
 * addresses of these symbols are meaningful; they have no storage.
 */
extern char KernelTextStart[];
extern char KernelTextEnd[];
extern char KernelRodataStart[];
extern char KernelRodataEnd[];

/* The physical address of the permanent page-map level 4 table. */
static PhysicalAddress PagingRootTable;

/* The number of frames consumed by the hierarchy, for reporting. */
static size_t PagingTableFrameCount;

/*
 * True once the direct physical map is established and active. Before that
 * point a paging structure must be reached through the kernel image window,
 * which confines every structure to the first gibibyte of physical memory;
 * afterwards the whole of physical memory is addressable and that restriction
 * is lifted.
 */
static bool PagingDirectMapActive;

/* The extent of physical memory covered by the direct map. */
static uint64_t PagingDirectMapBytes;

/* Extracts the index of each paging structure from a linear address. */
static size_t PagingLevel4Index(VirtualAddress address)
{
    return (size_t)((address >> 39) & 0x1FFU);
}

static size_t PagingLevel3Index(VirtualAddress address)
{
    return (size_t)((address >> 30) & 0x1FFU);
}

static size_t PagingLevel2Index(VirtualAddress address)
{
    return (size_t)((address >> 21) & 0x1FFU);
}

static size_t PagingLevel1Index(VirtualAddress address)
{
    return (size_t)((address >> 12) & 0x1FFU);
}

/*
 * Returns a pointer through which a paging structure may be read and written.
 * The structure is reached through the higher-half mapping established by the
 * boot-time hierarchy, which remains active while this one is constructed. Every
 * table is therefore allocated below KERNEL_PHYSICAL_MAP_LIMIT.
 */
static uint64_t *PagingTableAt(PhysicalAddress table)
{
    if (PagingDirectMapActive)
    {
        return (uint64_t *)(uintptr_t)PhysicalToDirect(table);
    }

    return (uint64_t *)(uintptr_t)PhysicalToVirtual(table);
}

/*
 * Allocates one frame for a paging structure and clears it. The frame is
 * constrained to lie below the extent of the higher-half mapping, because a
 * structure the kernel cannot address is a structure it cannot populate.
 *
 * Clearing is essential rather than tidy: a paging structure whose entries hold
 * residual data would present that data to the processor as page-table entries,
 * and the resulting translations would be arbitrary.
 */
static PhysicalAddress PagingAllocateTable(void)
{
    /*
     * Before the direct map exists a structure must be reachable through the
     * kernel image window, which extends only to the first gibibyte. Once the
     * direct map is active any frame may be used, and the restriction is lifted
     * so that machines with more than a gibibyte of memory are not confined to
     * their lowest gibibyte for page tables.
     */
    PhysicalAddress frame = PagingDirectMapActive
                                ? FrameAllocate()
                                : FrameAllocateBelow(KERNEL_PHYSICAL_MAP_LIMIT);
    uint64_t *entries;

    if (frame == FRAME_ALLOCATION_FAILED)
    {
        KernelPanic("No frame is available for a kernel paging structure.");
    }

    entries = PagingTableAt(frame);

    for (size_t index = 0U; index < PAGE_TABLE_ENTRY_COUNT; ++index)
    {
        entries[index] = 0U;
    }

    ++PagingTableFrameCount;

    return frame;
}

/*
 * Returns the physical address of the structure referenced by the given entry of
 * the given table, allocating and installing one if the entry is not present.
 */
static PhysicalAddress PagingObtainTable(PhysicalAddress table, size_t index)
{
    uint64_t *entries = PagingTableAt(table);

    if ((entries[index] & PAGE_ENTRY_PRESENT) == 0U)
    {
        PhysicalAddress allocated = PagingAllocateTable();

        entries[index] = allocated | PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE;

        return allocated;
    }

    return entries[index] & PAGE_ENTRY_ADDRESS_MASK;
}

/*
 * Establishes a 4 KiB mapping from the given virtual address to the given
 * physical frame, with the given flags, creating intermediate structures as
 * required.
 *
 * The intermediate entries are created permissive - present and writable - and
 * the restriction is applied at the leaf. This is the architectural rule: Intel
 * SDM, Volume 3A, Section 4.6, provides that the permissions of a translation
 * are the conjunction of those at every level, so a restrictive intermediate
 * entry would restrict every mapping beneath it, not merely this one.
 */
static void PagingMapPage(PhysicalAddress root, VirtualAddress virtual_address,
                          PhysicalAddress physical_address, uint64_t flags)
{
    PhysicalAddress level3 = PagingObtainTable(root, PagingLevel4Index(virtual_address));
    PhysicalAddress level2 = PagingObtainTable(level3, PagingLevel3Index(virtual_address));
    PhysicalAddress level1 = PagingObtainTable(level2, PagingLevel2Index(virtual_address));
    uint64_t *entries = PagingTableAt(level1);

    entries[PagingLevel1Index(virtual_address)] =
        (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | PAGE_ENTRY_PRESENT;
}

/*
 * Establishes a 2 MiB mapping by setting the PS flag in a page-directory entry,
 * per Intel SDM, Volume 3A, Table 4-15. A large page requires the physical
 * address to be aligned upon its own size.
 */
static void PagingMapLargePage(PhysicalAddress root, VirtualAddress virtual_address,
                               PhysicalAddress physical_address, uint64_t flags)
{
    PhysicalAddress level3 = PagingObtainTable(root, PagingLevel4Index(virtual_address));
    PhysicalAddress level2 = PagingObtainTable(level3, PagingLevel3Index(virtual_address));
    uint64_t *entries = PagingTableAt(level2);

    entries[PagingLevel2Index(virtual_address)] =
        (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags |
        PAGE_ENTRY_PRESENT | PAGE_ENTRY_LARGE;
}

/*
 * Reports whether a kernel virtual address falls within a section that must not
 * be writable. The text and the read-only data are so treated.
 *
 * The execute-disable bit is not applied here. It requires IA32_EFER.NXE to be
 * set, and its introduction, together with SMEP and SMAP, belongs to Phase 13,
 * sub-task 13.3. Withholding write permission is the part of the protection that
 * may be had now without that machinery.
 */
static bool PagingAddressIsReadOnly(VirtualAddress address)
{
    const VirtualAddress text_start = (VirtualAddress)(uintptr_t)KernelTextStart;
    const VirtualAddress text_end = (VirtualAddress)(uintptr_t)KernelTextEnd;
    const VirtualAddress rodata_start = (VirtualAddress)(uintptr_t)KernelRodataStart;
    const VirtualAddress rodata_end = (VirtualAddress)(uintptr_t)KernelRodataEnd;

    if (address >= text_start && address < text_end)
    {
        return true;
    }

    if (address >= rodata_start && address < rodata_end)
    {
        return true;
    }

    return false;
}

/*
 * Loads the given hierarchy into CR3, which activates it and, per Intel SDM,
 * Volume 3A, Section 4.10.4.1, invalidates every translation-lookaside-buffer
 * entry for the current process context save those marked global. No mapping
 * established here is global, so the whole of the buffer is flushed and no stale
 * translation of the identity map can survive.
 *
 * The instruction that follows this write is fetched through the new hierarchy.
 * It succeeds because the kernel executes from the higher half, which the new
 * hierarchy maps, and because the stack likewise resides in the kernel's BSS.
 */
static void PagingActivate(PhysicalAddress root)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"((uint64_t)root) : "memory");
}

void PagingInitialise(const BootInformation *information)
{
    PhysicalAddress root;
    uint64_t direct_map_limit;

    PagingTableFrameCount = 0U;
    PagingDirectMapActive = false;
    root = PagingAllocateTable();

    /*
     * Map the first 2 MiB of physical memory with 4 KiB pages, so that the
     * kernel's own sections may be given distinct permissions. The virtual
     * address of a physical address in this range is PhysicalToVirtual of it,
     * which is exactly where linker.ld placed the kernel's sections; the kernel
     * image is therefore mapped by this loop without needing separate treatment.
     */
    for (PhysicalAddress frame = 0U; frame < KERNEL_FINE_MAP_LIMIT; frame += PAGE_SIZE)
    {
        VirtualAddress virtual_address = PhysicalToVirtual(frame);
        uint64_t flags = PagingAddressIsReadOnly(virtual_address)
                             ? UINT64_C(0)
                             : PAGE_ENTRY_WRITABLE;

        PagingMapPage(root, virtual_address, frame, flags);
    }

    /*
     * Map the remainder of the first gibibyte with 2 MiB pages. This is the
     * kernel image window, which is retained after the direct map exists because
     * the kernel is linked within it: every code address, every string literal
     * and the kernel stack are addresses in this window, and abandoning it would
     * invalidate all of them.
     */
    for (PhysicalAddress frame = KERNEL_FINE_MAP_LIMIT;
         frame < KERNEL_PHYSICAL_MAP_LIMIT;
         frame += PAGE_SIZE_LARGE)
    {
        PagingMapLargePage(root, PhysicalToVirtual(frame), frame, PAGE_ENTRY_WRITABLE);
    }

    /*
     * Establish the direct physical map, at which the whole of physical memory
     * is addressable. The extent is taken to the highest usable address, rounded
     * up to a large-page boundary; nothing usable lies beyond it, and the
     * reserved regions at the top of the address space would demand an enormous
     * number of entries to describe memory that does not exist.
     *
     * Large pages are used throughout. A gibibyte of memory costs 512 entries in
     * one page directory, whereas 4 KiB pages would cost 262144 entries across
     * 512 page tables, which is 2 MiB of paging structures per gibibyte mapped.
     */
    direct_map_limit = AlignUp(information->highest_usable_address, PAGE_SIZE_LARGE);

    for (PhysicalAddress frame = 0U; frame < direct_map_limit; frame += PAGE_SIZE_LARGE)
    {
        PagingMapLargePage(root, PhysicalToDirect(frame), frame, PAGE_ENTRY_WRITABLE);
    }

    PagingDirectMapBytes = direct_map_limit;

    /*
     * No entry is created at index 0 of the page-map level 4 table, so the
     * identity mapping of low memory that boot/boot.asm established does not
     * exist in this hierarchy. It ceases to be reachable the instant CR3 is
     * written. Nothing depends upon it: the values the boot code preserved at
     * low physical addresses were consumed before this point, and every pointer
     * the kernel holds is a higher-half address.
     */

    PagingRootTable = root;
    PagingActivate(root);

    /*
     * The direct map is now live. This flag is set only after activation,
     * because until CR3 is written the map exists in the structures but not in
     * the translations the processor performs.
     */
    PagingDirectMapActive = true;
}

bool PagingDirectMapIsActive(void)
{
    return PagingDirectMapActive;
}

uint64_t PagingDirectMapExtent(void)
{
    return PagingDirectMapBytes;
}

PhysicalAddress PagingTranslate(VirtualAddress address)
{
    const uint64_t *entries;
    uint64_t entry;
    PhysicalAddress table;

    if (PagingRootTable == 0U)
    {
        return 0U;
    }

    entries = PagingTableAt(PagingRootTable);
    entry = entries[PagingLevel4Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return 0U;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel3Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return 0U;
    }

    /* A page-directory-pointer entry with PS set maps a 1 GiB page. */
    if ((entry & PAGE_ENTRY_LARGE) != 0U)
    {
        return (entry & PAGE_ENTRY_ADDRESS_MASK) + (address & (UINT64_C(0x3FFFFFFF)));
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel2Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return 0U;
    }

    /* A page-directory entry with PS set maps a 2 MiB page. */
    if ((entry & PAGE_ENTRY_LARGE) != 0U)
    {
        return (entry & PAGE_ENTRY_ADDRESS_MASK) + (address & (PAGE_SIZE_LARGE - 1U));
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel1Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return 0U;
    }

    return (entry & PAGE_ENTRY_ADDRESS_MASK) + (address & (PAGE_SIZE - 1U));
}

bool PagingAddressIsWritable(VirtualAddress address)
{
    const uint64_t *entries;
    uint64_t entry;
    PhysicalAddress table;
    uint64_t accumulated = PAGE_ENTRY_WRITABLE;

    entries = PagingTableAt(PagingRootTable);
    entry = entries[PagingLevel4Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }
    accumulated &= entry;

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel3Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }
    accumulated &= entry;

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel2Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }
    accumulated &= entry;

    if ((entry & PAGE_ENTRY_LARGE) != 0U)
    {
        return (accumulated & PAGE_ENTRY_WRITABLE) != 0U;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel1Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }
    accumulated &= entry;

    return (accumulated & PAGE_ENTRY_WRITABLE) != 0U;
}

PhysicalAddress PagingKernelRoot(void)
{
    return PagingRootTable;
}

void PagingReport(void)
{
    const uint64_t *entries = PagingTableAt(PagingRootTable);

    KernelWriteString("Kernel paging hierarchy: root at ");
    KernelWriteHexadecimal(PagingRootTable);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)PagingTableFrameCount);
    KernelWriteString(" frames (");
    KernelWriteDecimal(((uint64_t)PagingTableFrameCount * PAGE_SIZE) / 1024U);
    KernelWriteString(" KiB).\n");

    KernelWriteString("Direct physical map: ");
    KernelWriteHexadecimal(DIRECT_MAP_BASE);
    KernelWriteString(" covering ");
    KernelWriteDecimal(PagingDirectMapBytes / 1024U);
    KernelWriteString(" KiB of physical memory.\n");

    KernelWriteString("Low identity mapping: ");
    KernelWriteString((entries[0] & PAGE_ENTRY_PRESENT) != 0U
                          ? "PRESENT (unexpected)\n"
                          : "removed.\n");
}
