/*
 * File: kernel/test/verify_memory.c
 * Purpose: Asserts the memory management of Phase 2: the physical frame
 *          allocator, the permanent paging hierarchy, the virtual address
 *          allocator and the heap above it, per-frame reference counting, the
 *          resolution of a copy-on-write fault, and the cloning of an address
 *          space.
 * Key functions: KernelVerifyFrameAllocator, KernelVerifyPaging,
 *          KernelVerifyAllocators, KernelVerifyReferenceCounting,
 *          KernelVerifyCopyOnWrite, KernelVerifyAddressSpaces.
 * References:
   - docs/design/MEMORY-LAYOUT.md, Sections 10 and 11: the properties each
 *     assertion below establishes, and the silent failure each would catch.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Table 4-15: the paging-structure entry flags the assertions read.
 *
 * The last two are conducted here although they belong to Phase 2, because
 * neither can be conducted until Phase 3 has supplied the page-fault handler
 * they depend upon. KernelMain runs them in that later position for the same
 * reason.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/bootinfo.h>
#include <oxys/memory.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/addrspace.h>
#include <oxys/vmm.h>
#include <oxys/heap.h>
#include <oxys/cpu.h>
#include <oxys/interrupts.h>
#include <oxys/vga.h>

/*
 * Exercises the frame allocator and reports the outcome.
 *
 * There is no test harness in a kernel, and no means of running one before the
 * userland of Phase 7 exists. A boot-time self-test is therefore the only
 * mechanism by which the allocator's invariants may be checked upon the hardware
 * that will actually run it. The properties asserted are those whose violation
 * would corrupt memory silently: that frames are page aligned, that a frame is
 * not issued twice, that a freed frame is reissued, and that no frame is issued
 * from a reserved extent.
 */
void KernelVerifyFrameAllocator(void)
{
    PhysicalAddress first_frame;
    PhysicalAddress second_frame;
    PhysicalAddress reissued_frame;
    size_t free_before;
    bool succeeded = true;

    free_before = FrameFreeCount();

    first_frame = FrameAllocate();
    second_frame = FrameAllocate();

    if (first_frame == FRAME_ALLOCATION_FAILED ||
        second_frame == FRAME_ALLOCATION_FAILED)
    {
        KernelWriteString("  Allocation failed while frames remained.\n");
        succeeded = false;
    }
    else
    {
        if (!IsPageAligned(first_frame) || !IsPageAligned(second_frame))
        {
            KernelWriteString("  An allocated frame was not page aligned.\n");
            succeeded = false;
        }

        if (first_frame == second_frame)
        {
            KernelWriteString("  The same frame was issued twice.\n");
            succeeded = false;
        }

        if (first_frame < LOW_MEMORY_LIMIT || second_frame < LOW_MEMORY_LIMIT)
        {
            KernelWriteString("  A frame was issued from the reserved low memory.\n");
            succeeded = false;
        }

        if ((first_frame >= KernelBootInformation.kernel_physical_start &&
             first_frame < KernelBootInformation.kernel_physical_end) ||
            (second_frame >= KernelBootInformation.kernel_physical_start &&
             second_frame < KernelBootInformation.kernel_physical_end))
        {
            KernelWriteString("  A frame was issued from within the kernel image.\n");
            succeeded = false;
        }

        if (FrameFreeCount() != (free_before - 2U))
        {
            KernelWriteString("  The free count did not fall by two.\n");
            succeeded = false;
        }

        /*
         * A freed frame must be reissued in preference to an untouched one,
         * since FrameFree moves the search hint back to it. This confirms both
         * that the bit was cleared and that the hint was adjusted.
         */
        FrameFree(first_frame);
        reissued_frame = FrameAllocate();

        if (reissued_frame != first_frame)
        {
            KernelWriteString("  A freed frame was not reissued.\n");
            succeeded = false;
        }

        FrameFree(reissued_frame);
        FrameFree(second_frame);

        if (FrameFreeCount() != free_before)
        {
            KernelWriteString("  The free count did not return to its initial value.\n");
            succeeded = false;
        }
    }

    KernelWriteString(succeeded
                          ? "Frame allocator self-test passed.\n"
                          : "Frame allocator self-test FAILED.\n");
}

/*
 * A datum in the BSS, written by the paging self-test to confirm that writable
 * mappings genuinely permit writing after the hierarchy has been replaced. It is
 * volatile so that the compiler cannot discard the store as unobservable.
 */
static volatile uint64_t KernelPagingWriteProbe;

/*
 * Exercises the permanent paging hierarchy and reports the outcome.
 *
 * Every assertion here is made by walking the hierarchy in software rather than
 * by dereferencing an address. There is no interrupt descriptor table until
 * Phase 3, so a page fault would escalate to a triple fault and reset the
 * machine, destroying the evidence. A read-only mapping therefore cannot be
 * tested by attempting a write; it is tested by inspecting the entries that
 * govern it. The negative test becomes possible in Phase 3, sub-task 3.4.
 */
void KernelVerifyPaging(void)
{
    const VirtualAddress text_address = (VirtualAddress)(uintptr_t)&KernelMain;
    const VirtualAddress data_address = (VirtualAddress)(uintptr_t)&KernelPagingWriteProbe;
    const VirtualAddress vga_address = PhysicalToVirtual(VGA_TEXT_BUFFER_PHYSICAL);
    bool succeeded = true;

    /* A higher-half address must translate to the physical address it was
     * derived from; this is the invariant the whole layout rests upon. */
    if (PagingTranslate(vga_address) != VGA_TEXT_BUFFER_PHYSICAL)
    {
        KernelWriteString("  The VGA frame buffer does not translate correctly.\n");
        succeeded = false;
    }

    if (PagingTranslate(text_address) != VirtualToPhysical(text_address))
    {
        KernelWriteString("  The kernel text does not translate correctly.\n");
        succeeded = false;
    }

    /* The identity mapping must have gone. A low virtual address must now
     * resolve to nothing at all. */
    if (PagingTranslate((VirtualAddress)0x100000U) != 0U)
    {
        KernelWriteString("  The low identity mapping survives.\n");
        succeeded = false;
    }

    /* The kernel's text must not be writable; its data must be. */
    if (PagingAddressIsWritable(text_address))
    {
        KernelWriteString("  The kernel text is mapped writable.\n");
        succeeded = false;
    }

    if (!PagingAddressIsWritable(data_address))
    {
        KernelWriteString("  The kernel data is not mapped writable.\n");
        succeeded = false;
    }

    /* The direct map must translate every physical address to itself, including
     * addresses beyond the gibibyte the kernel image window covers. */
    if (PagingTranslate(PhysicalToDirect(VGA_TEXT_BUFFER_PHYSICAL)) !=
        VGA_TEXT_BUFFER_PHYSICAL)
    {
        KernelWriteString("  The direct map does not translate correctly.\n");
        succeeded = false;
    }

    /*
     * The kernel image window and the direct map must resolve the same physical
     * frame by two different virtual addresses. This is the property that makes
     * the direct map useful, and its failure would be silent.
     */
    if (PagingTranslate(PhysicalToDirect(VGA_TEXT_BUFFER_PHYSICAL)) !=
        PagingTranslate(PhysicalToVirtual(VGA_TEXT_BUFFER_PHYSICAL)))
    {
        KernelWriteString("  The window and the direct map disagree.\n");
        succeeded = false;
    }

    if (!PagingDirectMapIsActive())
    {
        KernelWriteString("  The direct map is not reported active.\n");
        succeeded = false;
    }

    /* A write through a writable mapping must succeed and be observable. The
     * kernel reaching the next line at all is itself the proof that the
     * hierarchy supports execution and a stack. */
    KernelPagingWriteProbe = UINT64_C(0x0BADC0DEDEADBEEF);
    if (KernelPagingWriteProbe != UINT64_C(0x0BADC0DEDEADBEEF))
    {
        KernelWriteString("  A write through a writable mapping was not observed.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Paging self-test passed.\n"
                          : "Paging self-test FAILED.\n");
}

/*
 * Exercises the kernel virtual address allocator and the heap, and reports the
 * outcome. The properties asserted are those whose violation would corrupt
 * memory silently rather than announce itself.
 */
void KernelVerifyAllocators(void)
{
    const size_t probe_page_count = 4U;
    bool succeeded = true;
    uint8_t *pages;
    uint8_t *pages_again;
    void *small;
    void *medium;
    void *large;
    void *zeroed;
    void *after;
    size_t arena_before;
    size_t live_before;

    /* --- The page allocator. --- */

    /*
     * The impossible arguments to KernelPagesFree — an address outside the
     * arena, a misaligned one, an unmapped page, and a range extending beyond
     * the arena — are not asserted here and cannot be. Each is a programming
     * error in the caller and each panics, which halts the machine; asserting
     * one would require a means of surviving a panic, and there is none before
     * the test harness of Phase 7. What is asserted below is the other
     * direction: that a legitimate multi-page range is released without being
     * refused, and that the arena is exactly as it was afterwards. An inverted
     * or off-by-one bound would panic here rather than pass silently.
     */
    arena_before = KernelVirtualPagesInUse();

    pages = (uint8_t *)KernelPagesAllocate(probe_page_count);

    if (pages == NULL)
    {
        KernelWriteString("  A four-page allocation failed.\n");
        succeeded = false;
    }
    else
    {
        /*
         * Write a distinct value into every page and read it back. A range that
         * was mapped short, or whose pages aliased one another, would fail here
         * and nowhere else.
         */
        for (size_t index = 0U; index < probe_page_count; ++index)
        {
            pages[index * PAGE_SIZE] = (uint8_t)(index + 1U);
        }

        for (size_t index = 0U; index < probe_page_count; ++index)
        {
            if (pages[index * PAGE_SIZE] != (uint8_t)(index + 1U))
            {
                KernelWriteString("  A page of the range did not retain its contents.\n");
                succeeded = false;
                break;
            }

            if (PagingTranslate((VirtualAddress)(uintptr_t)&pages[index * PAGE_SIZE]) == 0U)
            {
                KernelWriteString("  A page of the range is not mapped.\n");
                succeeded = false;
                break;
            }
        }

        KernelPagesFree(pages, probe_page_count);

        /* The released range must be reused rather than the bump pointer
         * advanced, which is the property the free list exists to provide. */
        pages_again = (uint8_t *)KernelPagesAllocate(probe_page_count);

        if (pages_again != pages)
        {
            KernelWriteString("  A released range was not reused.\n");
            succeeded = false;
        }

        if (pages_again != NULL)
        {
            KernelPagesFree(pages_again, probe_page_count);
        }

        /*
         * The arena returns to exactly what it was. This is what shows the
         * accounting of a release to be sound: KernelPagesFree subtracts the
         * count it was given, and a count admitted wrongly, or subtracted
         * wrongly, leaves the figure adrift with nothing else to report it.
         */
        if (KernelVirtualPagesInUse() != arena_before)
        {
            KernelWriteString("  A released range left the arena's accounting adrift.\n");
            succeeded = false;
        }
    }

    /* --- The heap. --- */

    small = KernelAllocate(16U);
    medium = KernelAllocate(1000U);
    large = KernelAllocate(5000U);

    if (small == NULL || medium == NULL || large == NULL)
    {
        KernelWriteString("  A heap allocation failed.\n");
        succeeded = false;
    }
    else
    {
        if ((((uintptr_t)small | (uintptr_t)medium | (uintptr_t)large) %
             HEAP_ALIGNMENT) != 0U)
        {
            KernelWriteString("  A heap allocation was not correctly aligned.\n");
            succeeded = false;
        }

        if (small == medium || medium == large || small == large)
        {
            KernelWriteString("  Two heap allocations shared an address.\n");
            succeeded = false;
        }

        /* Fill each allocation to its full requested extent. An object smaller
         * than its class, or a large allocation short of its pages, would
         * corrupt a neighbour here. */
        for (size_t index = 0U; index < 16U; ++index)
        {
            ((uint8_t *)small)[index] = 0xA5U;
        }
        for (size_t index = 0U; index < 1000U; ++index)
        {
            ((uint8_t *)medium)[index] = 0x5AU;
        }
        for (size_t index = 0U; index < 5000U; ++index)
        {
            ((uint8_t *)large)[index] = 0x3CU;
        }

        for (size_t index = 0U; index < 16U; ++index)
        {
            if (((uint8_t *)small)[index] != 0xA5U)
            {
                KernelWriteString("  A small allocation was corrupted.\n");
                succeeded = false;
                break;
            }
        }
        for (size_t index = 0U; index < 5000U; ++index)
        {
            if (((uint8_t *)large)[index] != 0x3CU)
            {
                KernelWriteString("  A large allocation was corrupted.\n");
                succeeded = false;
                break;
            }
        }

        KernelFree(medium);
        KernelFree(large);

        /* An object released to a class free list must be the next issued from
         * that class. */
        KernelFree(small);
        if (KernelAllocate(16U) != small)
        {
            KernelWriteString("  A released object was not reissued.\n");
            succeeded = false;
        }
        else
        {
            KernelFree(small);
        }
    }

    /*
     * The arena as it stands, recorded immediately before the refusals below so
     * that each may be shown to have left it exactly as it was. It is taken here
     * rather than at the start of this self-test because a slab, once taken from
     * the arena, is not returned to it: the pages the heap acquired above are
     * still held, legitimately, and a baseline older than they are would report
     * that as damage.
     */
    live_before = KernelVirtualPagesInUse();

    /*
     * A page count larger than the arena is refused, and refused without
     * wrapping the arithmetic that bounds it.
     *
     * The three counts below are chosen for what each does to that arithmetic
     * rather than for being large. One page beyond the arena's capacity is the
     * boundary the check states. 2^38 pages multiply to 2^50 bytes, which added
     * to the bump pointer carries past the top of the address space and returns
     * a small address that compares below the end of the arena. 2^52 pages
     * multiply to exactly zero, so the bound becomes the bump pointer itself and
     * every request is admitted. Before the check existed the second and third
     * were accepted, and the failure was not the allocation but what it left
     * behind: the unwinding inserts the range into the free list, where it
     * outlives the call and is handed to somebody else.
     */
    if ((KernelPagesAllocate((KERNEL_ARENA_SIZE / PAGE_SIZE) + 1U) != NULL) ||
        (KernelPagesAllocate((size_t)1U << 38) != NULL) ||
        (KernelPagesAllocate((size_t)1U << 52) != NULL))
    {
        KernelWriteString("  A page count larger than the arena was accepted.\n");
        succeeded = false;
    }

    /*
     * A size that cannot be represented once the heap's header and the rounding
     * to a page are added to it is refused rather than wrapped.
     *
     * The failure this guards is the worst kind an allocator has: the sum wraps
     * to a small number, a page or two is allocated, and a valid pointer is
     * returned for a request of very nearly the whole address space. Nothing
     * reports an error, and the caller discovers the truth by writing past the
     * end.
     */
    if ((KernelAllocate(SIZE_MAX) != NULL) ||
        (KernelAllocate(SIZE_MAX - sizeof(void *)) != NULL) ||
        (KernelAllocate(SIZE_MAX - PAGE_SIZE) != NULL))
    {
        KernelWriteString("  A size that cannot be represented was allocated.\n");
        succeeded = false;
    }

    if (KernelVirtualPagesInUse() != live_before)
    {
        KernelWriteString("  A refused allocation altered the pages in use.\n");
        succeeded = false;
    }

    /*
     * An ordinary allocation made after those refusals must still come from
     * within the arena.
     *
     * This is the assertion that does the work, and the reason the three
     * refusals above are not sufficient on their own: a request of 2^52 pages
     * was refused before this check existed too, because the mapping loop
     * exhausted physical memory and unwound, so asserting NULL alone would have
     * passed against the defect it is meant to catch. What the wrapped
     * arithmetic actually did was leave the arena broken behind it — a request
     * of 2^38 pages advanced the bump pointer by 2^50 bytes, carrying it out of
     * the upper half entirely and leaving it at 0x0003C00000000000. The
     * allocation that followed would have been served from the lower half, which
     * is user address space, and would have been reported as a success.
     */
    after = KernelPagesAllocate(1U);

    if (after == NULL)
    {
        KernelWriteString("  The arena served nothing after a refusal.\n");
        succeeded = false;
    }
    else
    {
        const VirtualAddress address = (VirtualAddress)(uintptr_t)after;

        if ((address < KERNEL_ARENA_BASE) ||
            (address >= (KERNEL_ARENA_BASE + KERNEL_ARENA_SIZE)))
        {
            KernelWriteString("  The arena issued an address outside itself.\n");
            succeeded = false;
        }

        KernelPagesFree(after, 1U);
    }

    zeroed = KernelAllocateZeroed(256U);

    if (zeroed == NULL)
    {
        KernelWriteString("  A zeroed allocation failed.\n");
        succeeded = false;
    }
    else
    {
        for (size_t index = 0U; index < 256U; ++index)
        {
            if (((const uint8_t *)zeroed)[index] != 0U)
            {
                KernelWriteString("  A zeroed allocation was not cleared.\n");
                succeeded = false;
                break;
            }
        }

        KernelFree(zeroed);
    }

    KernelWriteString(succeeded
                          ? "Allocator self-test passed.\n"
                          : "Allocator self-test FAILED.\n");
}

/*
 * Exercises per-frame reference counting and reports the outcome.
 *
 * The property under test is the one copy-on-write will depend upon: a frame
 * held by more than one referrer must survive the release of all but the last.
 * Its failure would either free memory still in use, which corrupts silently, or
 * retain memory nothing refers to, which leaks.
 */
void KernelVerifyReferenceCounting(void)
{
    PhysicalAddress frame;
    bool succeeded = true;

    if (!FrameReferenceIsActive())
    {
        KernelWriteString("  Reference counting is not active.\n");
        KernelWriteString("Reference counting self-test FAILED.\n");
        return;
    }

    frame = FrameAllocate();

    if (frame == FRAME_ALLOCATION_FAILED)
    {
        KernelWriteString("  A frame could not be allocated.\n");
        succeeded = false;
    }
    else
    {
        size_t free_after_allocation = FrameFreeCount();

        /* A newly allocated frame carries exactly one reference. */
        if (FrameReferenceCount(frame) != 1U)
        {
            KernelWriteString("  A newly allocated frame did not carry one reference.\n");
            succeeded = false;
        }

        /* Sharing the frame twice more brings it to three references. */
        FrameReferenceIncrement(frame);
        FrameReferenceIncrement(frame);

        if (FrameReferenceCount(frame) != 3U)
        {
            KernelWriteString("  The reference count did not rise to three.\n");
            succeeded = false;
        }

        /*
         * Releasing two of the three references must leave the frame allocated.
         * The free count must not move: no frame has returned to the allocator.
         */
        FrameFree(frame);
        FrameFree(frame);

        if (FrameReferenceCount(frame) != 1U)
        {
            KernelWriteString("  The reference count did not fall to one.\n");
            succeeded = false;
        }

        if (FrameFreeCount() != free_after_allocation)
        {
            KernelWriteString("  A shared frame was returned before its last release.\n");
            succeeded = false;
        }

        /* Releasing the last reference returns the frame. */
        FrameFree(frame);

        if (FrameReferenceCount(frame) != 0U)
        {
            KernelWriteString("  A fully released frame retains references.\n");
            succeeded = false;
        }

        if (FrameFreeCount() != (free_after_allocation + 1U))
        {
            KernelWriteString("  A fully released frame did not return to the allocator.\n");
            succeeded = false;
        }
    }

    KernelWriteString(succeeded
                          ? "Reference counting self-test passed.\n"
                          : "Reference counting self-test FAILED.\n");
}

/*
 * Exercises copy-on-write fault resolution and reports the outcome.
 *
 * Unlike the probe of the exception self-test, this exercises the real handler:
 * no handler is substituted, and the fault travels the same path that a
 * duplicated address space will take in sub-task 2.8. What is simulated is only
 * the sharing itself, a reference being taken to the frame directly rather than
 * by cloning an address space, since the cloning is the subject of sub-task 2.8
 * and does not yet exist.
 *
 * Two cases are distinguished, and they are the two the resolution routine
 * distinguishes. A frame with more than one referrer must be duplicated, or a
 * write by one holder would be visible to the other. A frame with a single
 * referrer must not be duplicated, since there is nobody to protect from the
 * write, and copying it would be pure waste.
 */
void KernelVerifyCopyOnWrite(void)
{
    const uint64_t copies_before = PagingCopyOnWriteCopyCount();
    const uint64_t sole_owners_before = PagingCopyOnWriteSoleOwnerCount();
    const size_t free_frames_before = FrameFreeCount();
    bool succeeded = true;
    volatile uint8_t *shared_page;
    volatile uint8_t *private_page;
    PhysicalAddress original_frame;
    PhysicalAddress replacement_frame;
    PhysicalAddress private_frame;

    /* --- A shared frame must be duplicated. --- */

    shared_page = (volatile uint8_t *)KernelPagesAllocate(1U);

    if (shared_page == NULL)
    {
        KernelWriteString("  A page could not be allocated.\n");
        KernelWriteString("Copy-on-write self-test FAILED.\n");
        return;
    }

    /* A pattern, so that a copy that omitted or corrupted the contents would be
     * detected rather than merely a copy that failed to occur. */
    for (size_t index = 0U; index < PAGE_SIZE; ++index)
    {
        shared_page[index] = (uint8_t)((index * 7U) + 3U);
    }

    original_frame = PagingTranslate((VirtualAddress)(uintptr_t)shared_page);

    /* Simulate a second holder of the frame, as address-space cloning will
     * create in sub-task 2.8. */
    FrameReferenceIncrement(original_frame);

    if (!PagingMarkCopyOnWrite((VirtualAddress)(uintptr_t)shared_page))
    {
        KernelWriteString("  The page could not be marked copy-on-write.\n");
        succeeded = false;
    }

    if (PagingAddressIsWritable((VirtualAddress)(uintptr_t)shared_page))
    {
        KernelWriteString("  A copy-on-write page retained write permission.\n");
        succeeded = false;
    }

    if (!PagingIsCopyOnWrite((VirtualAddress)(uintptr_t)shared_page))
    {
        KernelWriteString("  The copy-on-write flag was not recorded.\n");
        succeeded = false;
    }

    /* This write faults, and the fault is resolved by duplication. */
    shared_page[0] = 0xAAU;

    replacement_frame = PagingTranslate((VirtualAddress)(uintptr_t)shared_page);

    if (replacement_frame == original_frame)
    {
        KernelWriteString("  A shared frame was not duplicated.\n");
        succeeded = false;
    }

    if (PagingCopyOnWriteCopyCount() != (copies_before + 1U))
    {
        KernelWriteString("  The duplication was not counted.\n");
        succeeded = false;
    }

    if (shared_page[0] != 0xAAU)
    {
        KernelWriteString("  The faulting write did not take effect.\n");
        succeeded = false;
    }

    /*
     * Every byte but the one written must survive the duplication. A copy that
     * moved the wrong frame, or copied a partial page, would pass every test
     * above and fail here.
     */
    for (size_t index = 1U; index < PAGE_SIZE; ++index)
    {
        if (shared_page[index] != (uint8_t)((index * 7U) + 3U))
        {
            KernelWriteString("  The duplicated page does not retain its contents.\n");
            succeeded = false;
            break;
        }
    }

    /* The page is now private and must neither be marked nor be read-only. */
    if (PagingIsCopyOnWrite((VirtualAddress)(uintptr_t)shared_page) ||
        !PagingAddressIsWritable((VirtualAddress)(uintptr_t)shared_page))
    {
        KernelWriteString("  The duplicated page remains copy-on-write.\n");
        succeeded = false;
    }

    /*
     * The simulated other holder must still hold the original frame. Had the
     * resolution released it outright rather than dropping one reference, the
     * frame would have returned to the allocator while still in use.
     */
    if (FrameReferenceCount(original_frame) != 1U)
    {
        KernelWriteString("  The original frame's reference count is wrong.\n");
        succeeded = false;
    }

    /* Release the simulated holder. */
    FrameFree(original_frame);

    KernelPagesFree((void *)(uintptr_t)shared_page, 1U);

    /* --- A frame with a single referrer must not be duplicated. --- */

    private_page = (volatile uint8_t *)KernelPagesAllocate(1U);

    if (private_page == NULL)
    {
        KernelWriteString("  A second page could not be allocated.\n");
        KernelWriteString("Copy-on-write self-test FAILED.\n");
        return;
    }

    private_page[0] = 0x5CU;
    private_frame = PagingTranslate((VirtualAddress)(uintptr_t)private_page);

    (void)PagingMarkCopyOnWrite((VirtualAddress)(uintptr_t)private_page);

    /* This write faults, and is resolved by restoring write permission alone. */
    private_page[0] = 0x3DU;

    if (PagingTranslate((VirtualAddress)(uintptr_t)private_page) != private_frame)
    {
        KernelWriteString("  A frame with one referrer was needlessly duplicated.\n");
        succeeded = false;
    }

    if (PagingCopyOnWriteSoleOwnerCount() != (sole_owners_before + 1U))
    {
        KernelWriteString("  The unduplicated resolution was not counted.\n");
        succeeded = false;
    }

    if (PagingCopyOnWriteCopyCount() != (copies_before + 1U))
    {
        KernelWriteString("  A duplication occurred where none was required.\n");
        succeeded = false;
    }

    if (private_page[0] != 0x3DU)
    {
        KernelWriteString("  The write to the sole-owner page did not take effect.\n");
        succeeded = false;
    }

    KernelPagesFree((void *)(uintptr_t)private_page, 1U);

    /*
     * Every frame taken during the test must have been returned. Copy-on-write
     * allocates a frame on one path and releases a reference on another, and an
     * imbalance between the two would leak physical memory at a rate
     * proportional to the number of faults - the least visible and most damaging
     * way for this mechanism to be wrong.
     */
    if (FrameFreeCount() != free_frames_before)
    {
        KernelWriteString("  Frames were leaked: free count before ");
        KernelWriteDecimal((uint64_t)free_frames_before);
        KernelWriteString(", after ");
        KernelWriteDecimal((uint64_t)FrameFreeCount());
        KernelWriteString(".\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Copy-on-write self-test passed.\n"
                          : "Copy-on-write self-test FAILED.\n");
}

/*
 * The addresses within the lower half at which the test places its pages. Any
 * lower-half address would serve; these lie a gibibyte in, clear of the first
 * page, whose absence from every address space is a deliberate protection
 * against the dereference of a null pointer.
 */
#define KERNEL_TEST_WRITABLE_PAGE UINT64_C(0x0000000040000000)
#define KERNEL_TEST_READONLY_PAGE UINT64_C(0x0000000040001000)

/* The byte written into each test page, and the byte later written over it. */
#define KERNEL_TEST_PATTERN     0x71U
#define KERNEL_TEST_OVERWRITTEN 0xBBU

/*
 * Verifies address-space cloning, being sub-task 2.8.
 *
 * The properties asserted are those whose violation would be silent. A clone
 * that failed to protect the parent would leave the two spaces sharing memory
 * that each believes to be private, and neither would report anything; a clone
 * that copied the frames outright would be correct in every observable respect
 * and merely slow; and a destruction that released a shared frame outright would
 * hand a frame still in use back to the allocator.
 */
void KernelVerifyAddressSpaces(void)
{
    AddressSpace parent;
    AddressSpace child;
    const size_t free_frames_before = FrameFreeCount();
    PhysicalAddress writable_frame;
    PhysicalAddress readonly_frame;
    PhysicalAddress parent_frame;
    volatile uint8_t *writable_page = (volatile uint8_t *)KERNEL_TEST_WRITABLE_PAGE;
    volatile uint8_t *readonly_page = (volatile uint8_t *)KERNEL_TEST_READONLY_PAGE;
    bool succeeded = true;

    if (!AddressSpaceCreate(&parent))
    {
        KernelWriteString("  An address space could not be created.\n");
        KernelWriteString("Address-space self-test FAILED.\n");
        return;
    }

    writable_frame = FrameAllocate();
    readonly_frame = FrameAllocate();

    if (writable_frame == FRAME_ALLOCATION_FAILED ||
        readonly_frame == FRAME_ALLOCATION_FAILED)
    {
        KernelWriteString("  A frame for the test could not be allocated.\n");
        KernelWriteString("Address-space self-test FAILED.\n");
        return;
    }

    /*
     * The contents are placed through the direct map, the pages not yet being
     * reachable by their own addresses: the hierarchy that maps them is not the
     * active one.
     */
    *(volatile uint8_t *)(uintptr_t)PhysicalToDirect(writable_frame) = KERNEL_TEST_PATTERN;
    *(volatile uint8_t *)(uintptr_t)PhysicalToDirect(readonly_frame) = KERNEL_TEST_PATTERN;

    AddressSpaceMapPage(&parent, KERNEL_TEST_WRITABLE_PAGE, writable_frame,
                        PAGE_ENTRY_WRITABLE | PAGE_ENTRY_USER);
    AddressSpaceMapPage(&parent, KERNEL_TEST_READONLY_PAGE, readonly_frame,
                        PAGE_ENTRY_USER);

    AddressSpaceSwitch(&parent);

    /* --- The clone must protect the parent, not merely the child. --- */

    if (!AddressSpaceClone(&child, &parent))
    {
        AddressSpaceSwitch(AddressSpaceKernel());
        KernelWriteString("  The address space could not be cloned.\n");
        KernelWriteString("Address-space self-test FAILED.\n");
        return;
    }

    if (child.root == parent.root)
    {
        KernelWriteString("  The clone shares the parent's root table.\n");
        succeeded = false;
    }

    if (PagingAddressIsWritable(KERNEL_TEST_WRITABLE_PAGE) ||
        !PagingIsCopyOnWrite(KERNEL_TEST_WRITABLE_PAGE))
    {
        KernelWriteString("  The parent's writable page was not protected.\n");
        succeeded = false;
    }

    /*
     * A page that was already read-only is shared as it stands. Marking it would
     * be harmless but wasteful: it would provoke a fault that could resolve to
     * nothing, the page having no write permission to restore.
     */
    if (PagingIsCopyOnWrite(KERNEL_TEST_READONLY_PAGE))
    {
        KernelWriteString("  A read-only page was needlessly marked.\n");
        succeeded = false;
    }

    if (FrameReferenceCount(writable_frame) != 2U ||
        FrameReferenceCount(readonly_frame) != 2U)
    {
        KernelWriteString("  The clone did not record its references.\n");
        succeeded = false;
    }

    /* --- A write by the parent must not be observed by the child. --- */

    writable_page[0] = KERNEL_TEST_OVERWRITTEN;

    parent_frame = PagingTranslate(KERNEL_TEST_WRITABLE_PAGE);

    if (parent_frame == writable_frame)
    {
        KernelWriteString("  The parent's write did not duplicate the frame.\n");
        succeeded = false;
    }

    if (writable_page[0] != KERNEL_TEST_OVERWRITTEN)
    {
        KernelWriteString("  The parent's write did not take effect.\n");
        succeeded = false;
    }

    if (FrameReferenceCount(writable_frame) != 1U)
    {
        KernelWriteString("  The shared frame's reference was not released.\n");
        succeeded = false;
    }

    AddressSpaceSwitch(&child);

    if (PagingTranslate(KERNEL_TEST_WRITABLE_PAGE) != writable_frame)
    {
        KernelWriteString("  The child no longer maps the original frame.\n");
        succeeded = false;
    }

    if (writable_page[0] != KERNEL_TEST_PATTERN)
    {
        KernelWriteString("  The child observed the parent's write.\n");
        succeeded = false;
    }

    /*
     * The child now holds the frame alone, so its own write must be resolved by
     * restoring write permission rather than by a second duplication.
     */
    writable_page[0] = KERNEL_TEST_OVERWRITTEN;

    if (PagingTranslate(KERNEL_TEST_WRITABLE_PAGE) != writable_frame)
    {
        KernelWriteString("  The sole remaining holder duplicated needlessly.\n");
        succeeded = false;
    }

    /* The read-only page is shared, and identical in both. */
    if (PagingTranslate(KERNEL_TEST_READONLY_PAGE) != readonly_frame ||
        readonly_page[0] != KERNEL_TEST_PATTERN)
    {
        KernelWriteString("  The read-only page was not shared.\n");
        succeeded = false;
    }

    /* --- Destruction must release exactly what was taken. --- */

    AddressSpaceSwitch(AddressSpaceKernel());

    AddressSpaceDestroy(&child);

    if (FrameReferenceCount(readonly_frame) != 1U)
    {
        KernelWriteString("  Destroying the child released a shared frame.\n");
        succeeded = false;
    }

    AddressSpaceDestroy(&parent);

    if (FrameFreeCount() != free_frames_before)
    {
        KernelWriteString("  Frames were leaked: free count before ");
        KernelWriteDecimal((uint64_t)free_frames_before);
        KernelWriteString(", after ");
        KernelWriteDecimal((uint64_t)FrameFreeCount());
        KernelWriteString(".\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Address-space self-test passed.\n"
                          : "Address-space self-test FAILED.\n");
}
