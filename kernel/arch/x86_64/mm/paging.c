/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/mm/paging.c
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
 * and requires no synchronisation. Any later modification of a mapping shared
 * between processors is followed by a translation-lookaside-buffer shootdown, as
 * of sub-task 6.13; PagingInvalidate is where that happens.
 */

#include <oxys/arch/mm/paging.h>
#include <oxys/mm/pmm.h>
#include <oxys/arch/cpu/cpu.h>
#include <oxys/arch/mm/shootdown.h>
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

/*
 * Which hierarchy was last activated, for the report.
 *
 * **It is a per-processor fact held in a machine-wide variable**, and from
 * sub-task 6.15 every processor writes it on every context switch. That is
 * presently harmless for a reason rather than by luck: every kernel thread runs
 * upon the kernel root, and a user thread's affinity mask names the bootstrap
 * processor alone — so every writer writes the same value. It becomes wrong the
 * moment a user thread may run elsewhere, and must become per processor in the
 * same change that widens that mask. docs/design/SCHEDULER.md, Section 10,
 * limitation 7, records it so that the two cannot be separated.
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
static PhysicalAddress PagingObtainTable(PhysicalAddress table, size_t index,
                                         uint64_t flags)
{
    /*
     * An intermediate entry is created as permissive as anything beneath it may
     * need, and the restriction is applied at the leaf.
     *
     * That is the architectural rule of Section 4.6: the permissions of a
     * translation are the conjunction of those at every level, so a restrictive
     * intermediate restricts *every* mapping beneath it and not merely this one.
     * The write permission has always been given here for that reason. The user
     * bit was not, and had to be: a leaf marked accessible to privilege level 3
     * beneath a directory that is not is a page privilege level 3 cannot reach —
     * which is a mapping that looks correct at the only level anybody inspects
     * and faults at the first instruction of the first user program.
     *
     * Giving the bit to an intermediate grants nothing by itself. A kernel page
     * beneath the same directory still has no user bit at its leaf, so the
     * conjunction still refuses it; the leaf remains the authority and the
     * intermediate merely stops overriding it.
     */
    const uint64_t inherited =
        PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE | (flags & PAGE_ENTRY_USER);
    uint64_t *entries = PagingTableAt(table);

    if ((entries[index] & PAGE_ENTRY_PRESENT) == 0U)
    {
        PhysicalAddress allocated = PagingAllocateTable();

        entries[index] = allocated | inherited;

        return allocated;
    }

    /*
     * A table that already exists may have been made for a kernel mapping and
     * now be asked to admit a user one. The bit is added rather than assumed,
     * because which mapping came first is an accident of the order things are
     * established in and must not decide whether the second one works.
     */
    entries[index] |= (flags & PAGE_ENTRY_USER);

    return entries[index] & PAGE_ENTRY_ADDRESS_MASK;
}

/*
 * Establishes a 4 KiB mapping from the given virtual address to the given
 * physical frame, with the given flags, creating intermediate structures as
 * required.
 *
 * The intermediate entries are created as permissive as the leaf requires and
 * the restriction is applied there. This is the architectural rule: Intel SDM,
 * Volume 3A, Section 4.6, provides that the permissions of a translation are the
 * conjunction of those at every level, so a restrictive intermediate entry would
 * restrict every mapping beneath it, not merely this one. See
 * PagingObtainTable for what that means for the user bit in particular.
 */
static void PagingMapPage(PhysicalAddress root, VirtualAddress virtual_address,
                          PhysicalAddress physical_address, uint64_t flags)
{
    PhysicalAddress level3 = PagingObtainTable(root, PagingLevel4Index(virtual_address), flags);
    PhysicalAddress level2 = PagingObtainTable(level3, PagingLevel3Index(virtual_address), flags);
    PhysicalAddress level1 = PagingObtainTable(level2, PagingLevel2Index(virtual_address), flags);
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
    PhysicalAddress level3 = PagingObtainTable(root, PagingLevel4Index(virtual_address), flags);
    PhysicalAddress level2 = PagingObtainTable(level3, PagingLevel3Index(virtual_address), flags);
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
 * Invalidates the translation-lookaside-buffer entry for one page, upon the
 * executing processor alone.
 *
 * Intel SDM, Volume 3A, Section 4.10.4.1, requires software to invalidate a
 * translation whenever it changes a paging-structure entry that the processor
 * may have cached. INVLPG is used in preference to reloading CR3 because it
 * discards one entry rather than the whole buffer.
 */
static void PagingInvalidateHere(VirtualAddress address)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(address) : "memory");
}

/*
 * Invalidates the entry everywhere.
 *
 * Intel SDM, Volume 3A, Section 4.10.5, records that the instruction above
 * reaches the executing processor and no other, and that where several
 * processors may have cached a translation each must be made to invalidate it
 * for itself. That is the shootdown of sub-task 6.13, and it is announced from
 * here because here is the one place a paging-structure change becomes visible
 * as a change to a particular address.
 *
 * A shootdown that is not acknowledged is fatal. Section 4.10.4.4 permits an
 * invalidation to be deferred only while no processor can use the stale
 * translation, and a caller that returned from here would go on to give the
 * frame away — after which another processor writes through a translation to a
 * page that now belongs to somebody else. There is no report that could be made
 * later about that, because the state that would explain it is what gets
 * overwritten.
 *
 * Upon a machine with one processor started the broadcast returns at once,
 * having nobody to tell; that is the whole cost on the ordinary path.
 */
static void PagingInvalidate(VirtualAddress address)
{
    PagingInvalidateHere(address);

    if (!ShootdownBroadcast(address))
    {
        KernelPanic("A translation-lookaside-buffer shootdown was not acknowledged.");
    }
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

/*
 * Whether every level of the translation of an address carries a permission bit.
 *
 * Intel SDM, Volume 3A, Section 4.6 provides that the permissions of a
 * translation are the **conjunction** of those at every level, so a page marked
 * writable beneath a directory that is not is not writable. Accumulating by
 * conjunction is therefore the whole of the arithmetic, and consulting the last
 * level alone — which is the obvious implementation — reports a permission the
 * processor will not grant.
 *
 * The same walk answers two questions and is written once. The user bit governs
 * whether privilege level 3 may touch the page at all, and is what the system
 * call argument validation of sub-task 6.7 asks about: a kernel that copied from
 * a page merely because it was mapped would read its own memory on behalf of a
 * caller that named an address it could never have reached itself.
 */
static bool PagingAddressPermits(VirtualAddress address, uint64_t permission)
{
    const uint64_t *entries;
    uint64_t entry;
    PhysicalAddress table;
    uint64_t accumulated = permission;

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
        return (accumulated & permission) != 0U;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel1Index(address)];
    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return false;
    }
    accumulated &= entry;

    return (accumulated & permission) != 0U;
}
bool PagingAddressIsWritable(VirtualAddress address)
{
    return PagingAddressPermits(address, PAGE_ENTRY_WRITABLE);
}

bool PagingAddressIsUser(VirtualAddress address)
{
    return PagingAddressPermits(address, PAGE_ENTRY_USER);
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

/*
 * Withdraws a 4 KiB mapping from a hierarchy and reports the frame it named.
 *
 * The frame is returned rather than released, because this function cannot know
 * whether the caller is finished with it: a mapping withdrawn from one hierarchy
 * may be the last reference to its frame or may be one of several, and only the
 * caller knows which operation it is performing. Releasing here would make the
 * function unusable for anything but the one case, and — worse — would make the
 * commoner case look correct while freeing a frame another address space is
 * still translating through.
 *
 * FRAME_ALLOCATION_FAILED is returned where nothing was mapped. That is not a
 * failure: a caller withdrawing a range it believes it owns may meet a page it
 * never established, and a caller that treated the answer as a frame would free
 * frame zero.
 *
 * A large page is refused with a panic rather than unmapped, for the reason
 * PagingUnmapKernelPage gives: a 2 MiB mapping cannot be withdrawn one 4 KiB
 * page at a time, and nothing in this kernel establishes one in the half of the
 * address space this function is called upon.
 */
PhysicalAddress PagingUnmapPageIn(PhysicalAddress root, VirtualAddress virtual_address)
{
    uint64_t *entries;
    uint64_t entry;
    PhysicalAddress table;
    PhysicalAddress frame;

    entries = PagingTableAt(root);
    entry = entries[PagingLevel4Index(virtual_address)];

    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return FRAME_ALLOCATION_FAILED;
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel3Index(virtual_address)];

    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return FRAME_ALLOCATION_FAILED;
    }

    if ((entry & PAGE_ENTRY_LARGE) != 0U)
    {
        KernelPanic("An attempt was made to unmap a page within a large mapping.");
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel2Index(virtual_address)];

    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return FRAME_ALLOCATION_FAILED;
    }

    if ((entry & PAGE_ENTRY_LARGE) != 0U)
    {
        KernelPanic("An attempt was made to unmap a page within a large mapping.");
    }

    table = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries = PagingTableAt(table);
    entry = entries[PagingLevel1Index(virtual_address)];

    if ((entry & PAGE_ENTRY_PRESENT) == 0U)
    {
        return FRAME_ALLOCATION_FAILED;
    }

    frame = entry & PAGE_ENTRY_ADDRESS_MASK;
    entries[PagingLevel1Index(virtual_address)] = 0U;

    /*
     * The intermediate structures are left standing. They describe a region the
     * caller is very likely to use again — a heap that shrank is a heap that will
     * grow — and releasing a page table because its last entry went empty would
     * mean allocating one again upon the next byte asked for. They are released
     * with the address space, by AddressSpaceDestroy, which is the one moment
     * nothing can ask for them back.
     *
     * The invalidation is performed only where this hierarchy is the active one,
     * for the reason PagingMapPageIn gives: a hierarchy CR3 does not name has no
     * cached translation to discard.
     */
    if (root == PagingActiveTable)
    {
        PagingInvalidate(virtual_address);
    }

    return frame;
}

void PagingInvalidatePage(VirtualAddress address)
{
    PagingInvalidate(address);
}

void PagingInvalidateLocalPage(VirtualAddress address)
{
    PagingInvalidateHere(address);
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

    /*
     * Whether the boot loader's identity mapping is still standing.
     *
     * The question is asked of a translation and not of the root entry that
     * happens to lead to it, and that distinction is the whole of this report's
     * correctness. PML4[0] spans the first 512 GiB, so it is present whenever
     * *anything* is mapped low — and something transiently is: the self-test of
     * sub-task 6.7 maps a page at 1 GiB to assert the validation of a caller's
     * arguments against a real mapping, then unmaps it. Unmapping clears the
     * leaf and leaves the three tables above it standing, as it must, since they
     * are the tables every later low mapping would need; so PML4[0] remains
     * present for the rest of the machine's life.
     *
     * Reading that as the identity mapping had this report announce
     * "PRESENT (unexpected)" upon every boot, of a mapping that had in fact been
     * removed two hundred lines of log earlier — an alarm about the one thing in
     * this subsystem whose failure would be catastrophic, raised on every
     * healthy boot, which is how a reader learns to disregard it.
     *
     * What is asked instead is whether a representative address the boot mapping
     * covered still translates. boot/boot.asm identity-mapped [0, 1 GiB); the
     * kernel's own load address is the natural probe, being within that range,
     * being an address nothing else has any reason to map, and being the one
     * whose identity translation would be most dangerous to retain.
     */
    KernelWriteString("Low identity mapping: ");
    KernelWriteString(PagingTranslate(LOW_MEMORY_LIMIT) != 0U
                          ? "PRESENT (unexpected)\n"
                          : "removed.\n");
}
