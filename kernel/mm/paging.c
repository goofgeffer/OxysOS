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
 *   - docs/design/MEMORY-LAYOUT.md, Section 8: the design of this hierarchy.
 *
 * Concurrency. This code runs once, before any application processor is started,
 * and requires no synchronisation. From sub-task 6.8, any later modification of
 * a mapping shared between processors must be followed by a translation
 * lookaside buffer shootdown by inter-processor interrupt.
 */

#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/cpu.h>
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
 * The read-only data boundaries defined by linker.ld, as virtual addresses. Only
 * the addresses of these symbols are meaningful; they have no storage. The text
 * boundaries are declared in <oxys/kernel.h>, three files now needing them.
 */
extern char KernelRodataStart[];
extern char KernelRodataEnd[];

/* The physical address of the permanent page-map level 4 table. */
static PhysicalAddress PagingRootTable;

/*
 * The physical address of the hierarchy presently loaded in CR3.
 *
 * Until sub-task 2.8 this was necessarily the kernel hierarchy, and the two were
 * one value. An address space cloned from another may now be activated, and the
 * distinction becomes material: a walk performed in software must follow the
 * hierarchy the processor is following, whereas a mapping established for the
 * kernel belongs in the kernel hierarchy, whose higher-half entries every
 * address space shares.
 */
static PhysicalAddress PagingActiveTable;

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

/* Copy-on-write accounting. */
static uint64_t PagingCopyOnWriteFaults;
static uint64_t PagingCopyOnWriteCopies;
static uint64_t PagingCopyOnWriteSoleOwners;

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
    PagingActiveTable = root;
}

void PagingActivateRoot(PhysicalAddress root)
{
    PagingActivate(root);
}

PhysicalAddress PagingActiveRoot(void)
{
    return PagingActiveTable;
}

/*
 * Sets the write-protect flag in CR0.
 *
 * Intel SDM, Volume 3A, Section 6.15, provides that code running in user mode
 * always faults upon writing to a read-only page, but that supervisor-mode code
 * does so only when CR0.WP is set. The flag is clear upon reset and the boot
 * loader does not set it.
 *
 * Without it the read-only mappings established for the kernel text and
 * read-only data would be advisory: the kernel could write through them and no
 * fault would be raised, so the protection recorded in the paging structures
 * would not exist in fact. It is equally a prerequisite of copy-on-write in
 * sub-task 2.8, whose whole mechanism is a write to a page deliberately marked
 * read-only.
 */
static void PagingEnableWriteProtection(void)
{
    WriteCr0(ReadCr0() | CR0_WRITE_PROTECT);
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

    PagingEnableWriteProtection();
}

bool PagingDirectMapIsActive(void)
{
    return PagingDirectMapActive;
}

uint64_t PagingDirectMapExtent(void)
{
    return PagingDirectMapBytes;
}

/*
 * Invalidates the translation-lookaside-buffer entry for one page.
 *
 * Intel SDM, Volume 3A, Section 4.10.4.1, requires software to invalidate a
 * translation whenever it changes a paging-structure entry that the processor
 * may have cached. INVLPG is used in preference to reloading CR3 because it
 * discards one entry rather than the whole buffer.
 *
 * From sub-task 6.8 this must be accompanied by a shootdown: other processors
 * hold their own translation-lookaside buffers, and an entry cached there is not
 * affected by an invalidation performed here.
 */
static void PagingInvalidate(VirtualAddress address)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(address) : "memory");
}

void PagingMapKernelPage(VirtualAddress virtual_address,
                         PhysicalAddress physical_address,
                         uint64_t flags)
{
    PagingMapPage(PagingRootTable, virtual_address, physical_address, flags);
    PagingInvalidate(virtual_address);
}

void PagingUnmapKernelPage(VirtualAddress virtual_address)
{
    PhysicalAddress level3;
    PhysicalAddress level2;
    PhysicalAddress level1;
    uint64_t *entries;

    entries = PagingTableAt(PagingRootTable);
    if ((entries[PagingLevel4Index(virtual_address)] & PAGE_ENTRY_PRESENT) == 0U)
    {
        return;
    }
    level3 = entries[PagingLevel4Index(virtual_address)] & PAGE_ENTRY_ADDRESS_MASK;

    entries = PagingTableAt(level3);
    if ((entries[PagingLevel3Index(virtual_address)] & PAGE_ENTRY_PRESENT) == 0U)
    {
        return;
    }
    level2 = entries[PagingLevel3Index(virtual_address)] & PAGE_ENTRY_ADDRESS_MASK;

    entries = PagingTableAt(level2);
    if ((entries[PagingLevel2Index(virtual_address)] & PAGE_ENTRY_PRESENT) == 0U)
    {
        return;
    }

    /*
     * A large page cannot be unmapped one 4 KiB page at a time. The kernel arena
     * is mapped exclusively with 4 KiB pages, so encountering one here means the
     * caller has passed an address outside the arena.
     */
    if ((entries[PagingLevel2Index(virtual_address)] & PAGE_ENTRY_LARGE) != 0U)
    {
        KernelPanic("An attempt was made to unmap a page within a large mapping.");
    }

    level1 = entries[PagingLevel2Index(virtual_address)] & PAGE_ENTRY_ADDRESS_MASK;

    entries = PagingTableAt(level1);
    entries[PagingLevel1Index(virtual_address)] = 0U;

    PagingInvalidate(virtual_address);
}

PhysicalAddress PagingTranslate(VirtualAddress address)
{
    const uint64_t *entries;
    uint64_t entry;
    PhysicalAddress table;

    if (PagingActiveTable == 0U)
    {
        return 0U;
    }

    entries = PagingTableAt(PagingActiveTable);
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

    entries = PagingTableAt(PagingActiveTable);
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

/*
 * Locates the page-table entry that maps the given address, if the address is
 * mapped by a 4 KiB page.
 *
 * Returns NULL where any level is absent, or where the translation is provided
 * by a large page. A large page has no page-table entry to return, and a
 * copy-on-write fault upon one could not be resolved without first splitting the
 * mapping into 4 KiB pages, which nothing presently requires.
 */
static uint64_t *PagingLeafEntry(VirtualAddress address)
{
    uint64_t *entries;
    uint64_t entry;
    PhysicalAddress table;

    entries = PagingTableAt(PagingActiveTable);
    entry = entries[PagingLevel4Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return NULL;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel3Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U || (entry & PAGE_ENTRY_LARGE) != 0U)
    {
        return NULL;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel2Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U || (entry & PAGE_ENTRY_LARGE) != 0U)
    {
        return NULL;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);

    return &entries[PagingLevel1Index(address)];
}

/*
 * Copies the contents of one frame to another, both being reached through the
 * direct physical map.
 *
 * This is the operation for which the direct map of sub-task 2.4 exists. Neither
 * frame need have any other virtual address, and the two need not be related in
 * the address space of the faulting code.
 */
static void PagingCopyFrame(PhysicalAddress destination, PhysicalAddress source)
{
    const uint64_t *from = (const uint64_t *)(uintptr_t)PhysicalToDirect(source);
    uint64_t *to = (uint64_t *)(uintptr_t)PhysicalToDirect(destination);

    for (size_t index = 0U; index < (PAGE_SIZE / sizeof(uint64_t)); ++index)
    {
        to[index] = from[index];
    }
}

bool PagingMarkCopyOnWrite(VirtualAddress address)
{
    uint64_t *entry = PagingLeafEntry(AlignDown(address, PAGE_SIZE));

    if (entry == NULL || (*entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }

    /*
     * Write permission must be withdrawn as well as the flag set. The processor
     * ignores the software flag entirely; it is the absence of write permission
     * that raises the fault, and the flag merely records why.
     */
    *entry = (*entry & ~PAGE_ENTRY_WRITABLE) | PAGE_ENTRY_COPY_ON_WRITE;

    PagingInvalidate(AlignDown(address, PAGE_SIZE));

    return true;
}

bool PagingIsCopyOnWrite(VirtualAddress address)
{
    const uint64_t *entry = PagingLeafEntry(AlignDown(address, PAGE_SIZE));

    if (entry == NULL || (*entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }

    return (*entry & PAGE_ENTRY_COPY_ON_WRITE) != 0U;
}

bool PagingResolveCopyOnWriteFault(VirtualAddress address)
{
    const VirtualAddress page = AlignDown(address, PAGE_SIZE);
    uint64_t *entry = PagingLeafEntry(page);
    PhysicalAddress old_frame;
    PhysicalAddress new_frame;
    uint64_t flags;

    if (entry == NULL)
    {
        return false;
    }

    /*
     * Three conditions must hold for this to be a copy-on-write fault. The page
     * must be present, for a fault upon an absent page is a different matter
     * entirely. It must carry the software flag, for otherwise it was never
     * shared. And it must lack write permission, for if it has write permission
     * the fault was raised for some other reason and granting it again would
     * resolve nothing, leaving the instruction to fault without end.
     */
    if ((*entry & PAGE_ENTRY_PRESENT) == 0U ||
        (*entry & PAGE_ENTRY_COPY_ON_WRITE) == 0U ||
        (*entry & PAGE_ENTRY_WRITABLE) != 0U)
    {
        return false;
    }

    ++PagingCopyOnWriteFaults;

    old_frame = *entry & PAGE_ENTRY_ADDRESS_MASK;

    /*
     * Where the frame has but one referrer there is nothing to copy from and
     * nothing to protect: the page may simply be made writable again. This is
     * the common case once the other holders of a shared page have released it,
     * and avoiding the copy is the whole economy of the scheme.
     */
    if (FrameReferenceCount(old_frame) <= 1U)
    {
        *entry = (*entry & ~PAGE_ENTRY_COPY_ON_WRITE) | PAGE_ENTRY_WRITABLE;
        PagingInvalidate(page);
        ++PagingCopyOnWriteSoleOwners;

        return true;
    }

    new_frame = FrameAllocate();

    if (new_frame == FRAME_ALLOCATION_FAILED)
    {
        /*
         * The fault cannot be resolved. It is reported rather than retried:
         * returning would restart the instruction and fault again immediately.
         */
        return false;
    }

    PagingCopyFrame(new_frame, old_frame);

    /*
     * Install the private copy, preserving every other attribute of the mapping,
     * granting write permission and clearing the software flag. The page is no
     * longer shared and must not fault again for this reason.
     */
    flags = *entry & ~PAGE_ENTRY_ADDRESS_MASK & ~PAGE_ENTRY_COPY_ON_WRITE;
    *entry = new_frame | flags | PAGE_ENTRY_WRITABLE;

    PagingInvalidate(page);

    /*
     * Release this holder's reference to the shared frame. The frame returns to
     * the allocator only when the last holder does likewise, which is exactly
     * the property sub-task 2.6 was built to provide.
     */
    FrameFree(old_frame);

    ++PagingCopyOnWriteCopies;

    return true;
}

uint64_t PagingCopyOnWriteFaultCount(void)
{
    return PagingCopyOnWriteFaults;
}

uint64_t PagingCopyOnWriteCopyCount(void)
{
    return PagingCopyOnWriteCopies;
}

uint64_t PagingCopyOnWriteSoleOwnerCount(void)
{
    return PagingCopyOnWriteSoleOwners;
}

PhysicalAddress PagingKernelRoot(void)
{
    return PagingRootTable;
}

/*
 * The three routines that follow expose to the address-space code of sub-task
 * 2.8 the primitives with which this file builds a hierarchy. They are exposed
 * rather than duplicated: an address space is a paging hierarchy and nothing
 * else, and a second implementation of table allocation or of the walk would be
 * a second thing to keep correct.
 */
uint64_t *PagingTableEntries(PhysicalAddress table)
{
    return PagingTableAt(table);
}

PhysicalAddress PagingAllocateStructure(void)
{
    return PagingAllocateTable();
}

void PagingMapPageIn(PhysicalAddress root, VirtualAddress virtual_address,
                     PhysicalAddress physical_address, uint64_t flags)
{
    PagingMapPage(root, virtual_address, physical_address, flags);

    /*
     * The translation is invalidated only where the hierarchy is the active one.
     * A hierarchy that CR3 does not name has no cached translations to discard,
     * per Intel SDM, Volume 3A, Section 4.10.4, which describes the caches as
     * holding translations derived from the paging structures in use.
     */
    if (root == PagingActiveTable)
    {
        PagingInvalidate(virtual_address);
    }
}

void PagingInvalidatePage(VirtualAddress address)
{
    PagingInvalidate(address);
}

void PagingReleaseStructure(PhysicalAddress table)
{
    if (PagingTableFrameCount > 0U)
    {
        --PagingTableFrameCount;
    }

    FrameFree(table);
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

    KernelWriteString("Copy-on-write: faults resolved ");
    KernelWriteDecimal(PagingCopyOnWriteFaults);
    KernelWriteString(", frames duplicated ");
    KernelWriteDecimal(PagingCopyOnWriteCopies);
    KernelWriteString(", resolved without duplication ");
    KernelWriteDecimal(PagingCopyOnWriteSoleOwners);
    KernelWriteString(".\n");

    KernelWriteString("Low identity mapping: ");
    KernelWriteString((entries[0] & PAGE_ENTRY_PRESENT) != 0U
                          ? "PRESENT (unexpected)\n"
                          : "removed.\n");
}
