/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel. It validates the
 *          state established by the boot loader, initialises in dependency order
 *          every subsystem of Phases 1 to 3, asserts the properties of each by a
 *          boot-time self-test, and then either enters the keyboard echo loop,
 *          where a keyboard is present, or halts the processor where none is.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelVerifyVga,
 *          KernelVerifySerial, KernelVerifyPci, KernelVerifyAta,
 *          KernelVerifyBlock, KernelVerifyBuffer, KernelVerifyExt2,
 *          KernelCommandLineHasOption,
 *          KernelEchoBackspace,
 *          KernelEchoLoop,
 *          KernelWriteString, KernelWriteHexadecimal, KernelWriteDecimal.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.3 ("I386 machine state"): EAX
 *     contains 0x36D76289 and EBX the physical address of the Multiboot2
 *     information structure. Those values are preserved by boot/boot.asm and
 *     supplied as the arguments of this function.
 *   - Multiboot2 Specification 2.0, Section 3.6 ("Boot information format"): the
 *     information structure commences with a 32-bit total size followed by a
 *     32-bit reserved field, and is aligned on an 8-byte boundary.
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement, Section 3.2.3: the first two integer arguments are passed in
 *     RDI and RSI respectively.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
 *     "HLT": the instruction halts the processor until an interrupt, a debug
 *     exception, a non-maskable interrupt, or a reset occurs.
 *   - Intel SDM, Volume 2B, "STI": the instruction's effect upon the interrupt
 *     flag is delayed by one instruction, so that an interrupt cannot be
 *     delivered until after the instruction following it. This is what makes the
 *     sequence STI followed immediately by HLT free of the window in which a
 *     keyboard echo loop would otherwise service an interrupt and then halt with
 *     nothing left to wake it.
 *   - Intel SDM, Volume 3A, Section 6.2 and Table 6-1: vectors 0 to 31 are
 *     reserved to the architecture-defined exceptions, of which vector 8 is the
 *     double fault. Relied upon by the interrupt controller self-test, which
 *     establishes the remapping by the fact that no double fault arises when the
 *     interrupt flag is set.
 *   - Intel 8259A Programmable Interrupt Controller datasheet, section
 *     "OPERATION COMMAND WORDS (OCWS)": the non-specific end-of-interrupt resets
 *     the highest priority bit set in the in-service register, and therefore does
 *     nothing when no bit is set. Relied upon by the interrupt controller
 *     self-test, which raises the controllers' vectors by software.
 *   - IBM Personal Computer AT technical reference: the firmware leaves counter
 *     0 of the interval timer running and its output attached to the interrupt
 *     controller's IR0 input, which is what makes the self-test of the remapping
 *     possible without programming the timer first.
 */

#include <oxys/kernel.h>
#include <oxys/bootinfo.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/addrspace.h>
#include <oxys/vmm.h>
#include <oxys/heap.h>
#include <oxys/gdt.h>
#include <oxys/idt.h>
#include <oxys/interrupts.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>
#include <oxys/pic.h>
#include <oxys/pit.h>
#include <oxys/keyboard.h>
#include <oxys/vga.h>
#include <oxys/serial.h>
#include <oxys/pci.h>
#include <oxys/ata.h>
#include <oxys/block.h>
#include <oxys/buffer.h>
#include <oxys/ext2.h>
#include <oxys/vfs.h>
#include <oxys/ext2_vfs.h>

/*
 * The extents of the kernel text section, established by the link script. They
 * are arrays of char because a linker symbol has an address and no value; taking
 * the address of the array yields the address the linker assigned.
 */
extern char KernelTextStart[];
extern char KernelTextEnd[];

/*
 * The name and version of the system, presented upon the console and emitted
 * upon the serial port at every start.
 */
#define OXYS_SYSTEM_NAME    "Oxys-OS"
#define OXYS_VERSION_STRING "0.1.0"

/*
 * Halts the processor permanently with interrupts masked. Execution does not
 * proceed beyond this function. The halt is placed within a loop because the
 * HLT instruction resumes execution upon a non-maskable interrupt or a system
 * management interrupt, neither of which is masked by the CLI instruction.
 */
static _Noreturn void KernelHalt(void)
{
    /*
     * The diagnostic channel is buffered once its interrupts are active, and the
     * halt below clears the interrupt flag permanently, so anything still queued
     * would never be carried. A machine that stops has usually just written the
     * one thing worth reading.
     */
    SerialFlush();

    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

/*
 * Converts an unsigned 64-bit value into its hexadecimal representation and
 * writes it, prefixed by "0x", to both output devices. A minimal conversion
 * routine is provided here because the formatted output facilities of the C
 * library are not implemented until Phase 7.
 */
void KernelWriteHexadecimal(uint64_t value)
{
    static const char HexadecimalDigits[] = "0123456789ABCDEF";

    /* Sixteen digits suffice for a 64-bit value, plus a null terminator. */
    char buffer[17];
    size_t index = sizeof(buffer) - 1U;

    buffer[index] = '\0';

    do
    {
        --index;
        buffer[index] = HexadecimalDigits[value & UINT64_C(0x0F)];
        value >>= 4;
    } while (value != 0U);

    VgaWriteString("0x");
    VgaWriteString(&buffer[index]);
    SerialWriteString("0x");
    SerialWriteString(&buffer[index]);
}

/*
 * Writes an unsigned value in decimal to both output devices. The digits are
 * generated least significant first and therefore emitted from the end of the
 * buffer backwards.
 */
void KernelWriteDecimal(uint64_t value)
{
    /* Twenty digits suffice for the greatest 64-bit value, plus a terminator. */
    char buffer[21];
    size_t index = sizeof(buffer) - 1U;

    buffer[index] = '\0';

    do
    {
        --index;
        buffer[index] = (char)('0' + (unsigned char)(value % 10U));
        value /= 10U;
    } while (value != 0U);

    VgaWriteString(&buffer[index]);
    SerialWriteString(&buffer[index]);
}

/*
 * Writes a string to both the text console and the serial port, so that the
 * diagnostic record is complete irrespective of which device the operator is
 * observing.
 */
void KernelWriteString(const char *string)
{
    VgaWriteString(string);
    SerialWriteString(string);
}

void KernelPanic(const char *message)
{
    VgaSetColour(VGA_COLOUR_WHITE, VGA_COLOUR_RED);
    KernelWriteString("\nKERNEL PANIC: ");
    KernelWriteString(message);
    KernelWriteString("\nThe system has been halted.\n");

    KernelHalt();
}

/*
 * The parsed, boot-protocol-neutral description of the machine. It is held at
 * file scope rather than upon the stack because it is substantial, and the boot
 * stack is only 64 KiB.
 */
static BootInformation KernelBootInformation;

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
static void KernelVerifyFrameAllocator(void)
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
static void KernelVerifyPaging(void)
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
static void KernelVerifyAllocators(void)
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
static void KernelVerifyReferenceCounting(void)
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
 * Confirms that the interrupt descriptor table was loaded as intended.
 *
 * The table register is read back with SIDT rather than trusting the value that
 * was written to it. A LIDT that silently failed, or an operand corrupted by
 * padding the compiler inserted, would produce a table register that does not
 * describe the table, and nothing else would reveal it until the first interrupt
 * escalated to a reset.
 */
static void KernelVerifyIdt(void)
{
    const uint16_t expected_limit = (uint16_t)((IDT_ENTRY_COUNT * 16U) - 1U);
    const uint64_t expected_base = (uint64_t)(uintptr_t)IdtTableAddress();
    bool succeeded = true;

    if (IdtLimit() != expected_limit)
    {
        KernelWriteString("  The table limit read back does not match.\n");
        succeeded = false;
    }

    if (IdtBase() != expected_base)
    {
        KernelWriteString("  The table base read back does not match.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Interrupt descriptor table self-test passed.\n"
                          : "Interrupt descriptor table self-test FAILED.\n");
}

/*
 * Exercises the interrupt stubs and the trap frame they construct.
 *
 * The frame is the interface between the assembly stubs and every handler the
 * kernel will ever have. An error in its layout would not announce itself: the
 * dispatcher would read plausible values from the wrong offsets, and every
 * diagnosis founded upon them would be wrong. The test therefore checks the
 * frame against values it planted, rather than merely checking that an interrupt
 * was taken.
 */
static void KernelVerifyInterruptStubs(void)
{
    const uint64_t sentinel = UINT64_C(0xFEEDFACECAFEB00D);
    const uint64_t count_before = InterruptCount();
    const TrapFrame *frame;
    bool succeeded = true;

    /*
     * Raise a breakpoint exception with a known value in RAX. INT3 is a trap
     * rather than a fault, so the handler returns to the instruction following
     * and execution continues; and the value in RAX proves that the common stub
     * saved the registers in the order the structure declares. Were the order
     * reversed, this field would hold the contents of R15.
     */
    __asm__ __volatile__("mov %0, %%rax\n\t"
                         "int3"
                         :
                         : "r"(sentinel)
                         : "rax", "memory");

    frame = InterruptLastFrame();

    if (InterruptCount() != (count_before + 1U))
    {
        KernelWriteString("  The breakpoint was not dispatched.\n");
        succeeded = false;
    }

    if (frame->vector != 3U)
    {
        KernelWriteString("  The breakpoint reported the wrong vector.\n");
        succeeded = false;
    }

    if (frame->rax != sentinel)
    {
        KernelWriteString("  The saved RAX does not hold the planted value.\n");
        succeeded = false;
    }

    /* A vector that pushes no error code must present a zero in its place. */
    if (frame->error_code != 0U)
    {
        KernelWriteString("  The substituted error code is not zero.\n");
        succeeded = false;
    }

    /* The processor pushes the segment and flags of the interrupted code. */
    if (frame->cs != 0x08U)
    {
        KernelWriteString("  The saved CS is not the kernel code selector.\n");
        succeeded = false;
    }

    /*
     * The saved RIP must lie within the kernel text, being the address of the
     * instruction after INT3. A displaced frame would place something else here,
     * and a value outside the text is the clearest evidence of that.
     *
     * The bounds are the linker's own symbols for the text section. They were
     * once the address of KernelMain and an arbitrary extent beyond the virtual
     * base, which asserted something narrower than it appeared to: that the
     * compiler had placed KernelMain before every function that might execute
     * INT3. Adding a function above it in this file was enough to fail the test
     * without anything being wrong.
     */
    if ((frame->rip < (uint64_t)(uintptr_t)KernelTextStart) ||
        (frame->rip >= (uint64_t)(uintptr_t)KernelTextEnd))
    {
        KernelWriteString("  The saved RIP does not lie within the kernel image.\n");
        succeeded = false;
    }

    /* The saved RSP must lie above the frame itself, the stack growing down. */
    if (frame->rsp <= (uint64_t)(uintptr_t)frame)
    {
        KernelWriteString("  The saved RSP does not lie above the frame.\n");
        succeeded = false;
    }

    /*
     * Raise a vector above the architecture-defined range, to confirm that a
     * stub other than the first thirty-two is reached and reports its own
     * number. Vector 42 is arbitrary and unassigned.
     */
    __asm__ __volatile__("int $42" : : : "memory");

    frame = InterruptLastFrame();

    if (InterruptCount() != (count_before + 2U))
    {
        KernelWriteString("  The software interrupt was not dispatched.\n");
        succeeded = false;
    }

    if (frame->vector != 42U)
    {
        KernelWriteString("  The software interrupt reported the wrong vector.\n");
        succeeded = false;
    }

    /*
     * The set of vectors that the C code believes push an error code must match
     * the set the assembly stubs were generated from. The two are written
     * separately and a divergence would displace the frame for exactly those
     * vectors that matter most, the faults.
     */
    {
        size_t counted = 0U;

        for (uint64_t vector = 0U; vector < IDT_ENTRY_COUNT; ++vector)
        {
            if (InterruptVectorPushesErrorCode(vector))
            {
                ++counted;
            }
        }

        if (counted != 10U)
        {
            KernelWriteString("  The error-code vector set is not of the expected size.\n");
            succeeded = false;
        }
    }

    KernelWriteString(succeeded
                          ? "Interrupt stub self-test passed.\n"
                          : "Interrupt stub self-test FAILED.\n");
}

/* State recorded by the probe handlers of the dispatcher self-test. */
static uint64_t KernelProbeHandlerCount;
static uint64_t KernelProbeHandlerVector;
static uint64_t KernelExceptionProbeCount;

/* The value the probe handler writes into the frame, to be observed in RAX after
 * the return from the interrupt. */
#define KERNEL_PROBE_RESULT UINT64_C(0x00C0FFEE0000BEEF)

/*
 * A probe handler that records its invocation and alters the frame.
 *
 * The alteration is the point of the exercise. A handler that could observe the
 * interrupted state but not change it would be sufficient for reporting and for
 * nothing else. Correcting a fault, delivering the result of a system call and
 * switching context all require that a change to the frame become the state to
 * which control returns.
 */
static void KernelProbeHandler(TrapFrame *frame)
{
    ++KernelProbeHandlerCount;
    KernelProbeHandlerVector = frame->vector;
    frame->rax = KERNEL_PROBE_RESULT;
}

/* A probe handler for an architecture-defined vector, to confirm that a
 * registered handler displaces the fatal default. */
static void KernelExceptionProbeHandler(TrapFrame *frame)
{
    (void)frame;
    ++KernelExceptionProbeCount;
}

/*
 * Exercises the interrupt dispatcher and reports the outcome.
 */
static void KernelVerifyDispatcher(void)
{
    const uint64_t unhandled_before = InterruptUnhandledCount();
    const uint64_t vector_count_before = InterruptVectorCount(42U);
    uint64_t returned_value = 0U;
    bool succeeded = true;

    /* --- A registered handler receives the interrupt. --- */

    InterruptRegisterHandler(42U, KernelProbeHandler, "self-test probe");

    if (InterruptRegisteredHandler(42U) != KernelProbeHandler)
    {
        KernelWriteString("  The registered handler was not recorded.\n");
        succeeded = false;
    }

    __asm__ __volatile__("int $42" : "=a"(returned_value) : : "memory");

    if (KernelProbeHandlerCount != 1U)
    {
        KernelWriteString("  The registered handler was not entered.\n");
        succeeded = false;
    }

    if (KernelProbeHandlerVector != 42U)
    {
        KernelWriteString("  The handler received the wrong vector.\n");
        succeeded = false;
    }

    /*
     * The handler wrote to the frame. That write must have been restored into
     * RAX by the common stub, and so be the value the interrupted code observes.
     */
    if (returned_value != KERNEL_PROBE_RESULT)
    {
        KernelWriteString("  A handler's change to the frame was not restored.\n");
        succeeded = false;
    }

    if (InterruptVectorCount(42U) != (vector_count_before + 1U))
    {
        KernelWriteString("  The per-vector count did not advance.\n");
        succeeded = false;
    }

    if (InterruptUnhandledCount() != unhandled_before)
    {
        KernelWriteString("  A handled interrupt was counted as unhandled.\n");
        succeeded = false;
    }

    /* --- An unregistered vector above the architectural range is ignored. --- */

    InterruptUnregisterHandler(42U);

    if (InterruptRegisteredHandler(42U) != NULL)
    {
        KernelWriteString("  The handler was not removed.\n");
        succeeded = false;
    }

    __asm__ __volatile__("int $42" : : : "memory", "rax");

    if (KernelProbeHandlerCount != 1U)
    {
        KernelWriteString("  A removed handler was entered.\n");
        succeeded = false;
    }

    if (InterruptUnhandledCount() != (unhandled_before + 1U))
    {
        KernelWriteString("  An unhandled interrupt was not counted.\n");
        succeeded = false;
    }

    /* --- A registered handler displaces the fatal default for an exception. --- */

    {
        InterruptHandler previous_overflow = InterruptRegisteredHandler(4U);

        InterruptRegisterHandler(4U, KernelExceptionProbeHandler,
                                 "self-test overflow probe");

        __asm__ __volatile__("int $4" : : : "memory");

        /*
         * Restore the handler that was displaced rather than removing it. The
         * exception handlers are registered before this test runs, and leaving
         * vector 4 unregistered would make a genuine overflow trap fatal.
         */
        InterruptRegisterHandler(4U, previous_overflow, "overflow");
    }

    if (KernelExceptionProbeCount != 1U)
    {
        KernelWriteString("  The exception handler was not entered.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Interrupt dispatcher self-test passed.\n"
                          : "Interrupt dispatcher self-test FAILED.\n");
}

/* State recorded by the page-fault probe of the exception self-test. */
static volatile uint64_t KernelFaultProbeCount;
static volatile uint64_t KernelFaultProbeAddress;
static volatile uint64_t KernelFaultProbeErrorCode;
static VirtualAddress KernelFaultProbePage;
static PhysicalAddress KernelFaultProbeFrame;

/*
 * A page-fault handler that records the fault and then resolves it.
 *
 * Resolution is what distinguishes this from a diagnostic. The page fault is a
 * fault, not a trap: the offending instruction is restarted upon return, so a
 * handler that merely recorded the fault would be re-entered without end. By
 * restoring write permission before returning, the handler makes the restarted
 * instruction succeed.
 *
 * This is precisely the shape of the copy-on-write handler of sub-task 2.8,
 * which will differ in substituting a private copy of the frame for the shared
 * one rather than simply granting write permission.
 */
static void KernelPageFaultProbe(TrapFrame *frame)
{
    ++KernelFaultProbeCount;
    KernelFaultProbeAddress = ReadCr2();
    KernelFaultProbeErrorCode = frame->error_code;

    PagingMapKernelPage(KernelFaultProbePage, KernelFaultProbeFrame,
                        PAGE_ENTRY_WRITABLE);
}

/*
 * Exercises the exception handlers, and performs the negative test of the
 * read-only kernel mappings that was deferred from sub-task 2.3.
 *
 * Until this point the read-only mappings had been confirmed only by inspecting
 * the paging structures in software. That establishes what the entries say, not
 * what the processor does. The test below establishes the latter: a page is
 * mapped read-only, written to, and the resulting fault examined.
 */
static void KernelVerifyExceptions(void)
{
    InterruptHandler previous_handler;
    volatile uint8_t *probe;
    bool succeeded = true;

    /* CR0.WP must be set, or a supervisor write to a read-only page raises no
     * fault at all and every read-only kernel mapping is merely advisory. */
    if ((ReadCr0() & CR0_WRITE_PROTECT) == 0U)
    {
        KernelWriteString("  CR0.WP is clear; read-only mappings are not enforced.\n");
        succeeded = false;
    }

    probe = (volatile uint8_t *)KernelPagesAllocate(1U);

    if (probe == NULL)
    {
        KernelWriteString("  The probe page could not be allocated.\n");
        KernelWriteString("Exception self-test FAILED.\n");
        return;
    }

    KernelFaultProbePage = (VirtualAddress)(uintptr_t)probe;
    KernelFaultProbeFrame = PagingTranslate(KernelFaultProbePage);

    /* Write once while the page is still writable, to establish that the
     * mapping works before it is restricted. */
    *probe = 0x11U;

    /* Remap the page read-only. */
    PagingMapKernelPage(KernelFaultProbePage, KernelFaultProbeFrame, 0U);

    if (PagingAddressIsWritable(KernelFaultProbePage))
    {
        KernelWriteString("  The probe page is still writable after restriction.\n");
        succeeded = false;
    }

    /* Substitute the probe handler for the fatal default. */
    previous_handler = InterruptRegisteredHandler(14U);
    InterruptRegisterHandler(14U, KernelPageFaultProbe, "page fault probe");

    /* This write must fault, be resolved by the handler, and then succeed. */
    *probe = 0xA5U;

    InterruptRegisterHandler(14U, previous_handler, "page fault");

    if (KernelFaultProbeCount != 1U)
    {
        KernelWriteString("  The write to a read-only page raised no fault.\n");
        succeeded = false;
    }

    if (KernelFaultProbeAddress != KernelFaultProbePage)
    {
        KernelWriteString("  CR2 does not hold the address written.\n");
        succeeded = false;
    }

    /*
     * The page was present but not writable, so the error code must record a
     * protection violation rather than an absent translation, a write rather
     * than a read, and a supervisor-mode access.
     */
    if ((KernelFaultProbeErrorCode & PAGE_FAULT_PRESENT) == 0U)
    {
        KernelWriteString("  The fault was reported as a missing translation.\n");
        succeeded = false;
    }

    if ((KernelFaultProbeErrorCode & PAGE_FAULT_WRITE) == 0U)
    {
        KernelWriteString("  The fault was not reported as a write.\n");
        succeeded = false;
    }

    if ((KernelFaultProbeErrorCode & PAGE_FAULT_USER) != 0U)
    {
        KernelWriteString("  The fault was reported as a user-mode access.\n");
        succeeded = false;
    }

    /* The instruction must have been restarted and completed. */
    if (*probe != 0xA5U)
    {
        KernelWriteString("  The faulting instruction did not complete.\n");
        succeeded = false;
    }

    KernelPagesFree((void *)(uintptr_t)KernelFaultProbePage, 1U);

    KernelWriteString(succeeded
                          ? "Exception self-test passed: a read-only page faulted "
                            "and was resolved.\n"
                          : "Exception self-test FAILED.\n");
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
static void KernelVerifyCopyOnWrite(void)
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
static void KernelVerifyAddressSpaces(void)
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

/* State recorded by the probe handler of the interrupt controller self-test. */
static uint64_t KernelPicProbeCount;

/* A probe handler standing in for a device driver upon a request line. */
static void KernelPicProbeHandler(TrapFrame *frame)
{
    (void)frame;
    ++KernelPicProbeCount;
}

/*
 * Exercises the 8259A driver, being sub-task 3.5.
 *
 * Three properties are asserted, and the failure of each would present quite
 * differently. A mask register that did not respond would leave a device unable
 * to interrupt, or unable to stop; a routing layer that mistook the vector for
 * the request line would deliver every interrupt to the wrong driver; and an
 * end-of-interrupt sent upon a spurious request would reset the bit of whatever
 * line was genuinely in service, losing a real interrupt at a rate governed by
 * electrical noise and therefore reproducible nowhere.
 */
static void KernelVerifyPic(void)
{
    const uint64_t spurious_before = PicSpuriousCount();
    const uint64_t requests_before = PicRequestCount();
    const uint64_t unclaimed_before = PicUnclaimedCount();
    bool succeeded = true;

    /* --- Every line is withheld until a driver claims it. --- */

    if (PicMaskValue() != UINT16_C(0xFFFF))
    {
        KernelWriteString("  Not every request line is masked after initialisation.\n");
        succeeded = false;
    }

    /* Nothing can be in service, no line having been permitted. */
    if (PicInServiceRegister() != 0U)
    {
        KernelWriteString("  A line is in service before any was unmasked.\n");
        succeeded = false;
    }

    /* --- A mask register responds, and the correct controller is addressed. --- */

    PicUnmaskLine(1U);

    if (PicLineIsMasked(1U) || PicMaskValue() != UINT16_C(0xFFFD))
    {
        KernelWriteString("  A master line was not unmasked correctly.\n");
        succeeded = false;
    }

    PicMaskLine(1U);

    if (!PicLineIsMasked(1U) || PicMaskValue() != UINT16_C(0xFFFF))
    {
        KernelWriteString("  A master line was not masked again.\n");
        succeeded = false;
    }

    /*
     * Unmasking a slave line must unmask the cascade with it. Without that, the
     * line would be permitted at the slave and the request would still never
     * reach the processor, the slave's output being attached to the master's IR2.
     */
    PicUnmaskLine(12U);

    if (PicLineIsMasked(12U))
    {
        KernelWriteString("  A slave line was not unmasked.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(PIC_CASCADE_IRQ))
    {
        KernelWriteString("  Unmasking a slave line did not unmask the cascade.\n");
        succeeded = false;
    }

    PicMaskLine(12U);
    PicMaskLine(PIC_CASCADE_IRQ);

    /* --- A request is routed to the driver that claims its line. --- */

    PicInstallHandler(3U, KernelPicProbeHandler, "self-test probe");

    if (PicRegisteredHandler(3U) != KernelPicProbeHandler)
    {
        KernelWriteString("  The claimed line did not record its handler.\n");
        succeeded = false;
    }

    /*
     * Vector 35 is the third request line of the master. Raising it by software
     * exercises the routing arithmetic without requiring a device to be present.
     * The end-of-interrupt this provokes is harmless: per the 8259A datasheet,
     * section "OPERATION COMMAND WORDS (OCWS)", a non-specific command resets the
     * highest priority bit set in the in-service register, and no bit is set.
     */
    __asm__ __volatile__("int $35" : : : "memory");

    if (KernelPicProbeCount != 1U)
    {
        KernelWriteString("  The claimed line's handler was not entered.\n");
        succeeded = false;
    }

    if (PicRequestCount() != (requests_before + 1U))
    {
        KernelWriteString("  The request was not counted.\n");
        succeeded = false;
    }

    PicRemoveHandler(3U);

    /* An unclaimed line must still be acknowledged, and counted as unclaimed. */
    __asm__ __volatile__("int $35" : : : "memory");

    if (KernelPicProbeCount != 1U)
    {
        KernelWriteString("  A removed handler was entered.\n");
        succeeded = false;
    }

    if (PicUnclaimedCount() != (unclaimed_before + 1U))
    {
        KernelWriteString("  An unclaimed request was not counted.\n");
        succeeded = false;
    }

    /* --- A spurious request is recognised and not acknowledged. --- */

    /*
     * Vector 39 is the master's lowest priority line, upon which a spurious
     * request is delivered. No line is in service, so the in-service register
     * bit is clear and the request must be recognised as spurious: counted, not
     * routed, and above all not acknowledged.
     */
    __asm__ __volatile__("int $39" : : : "memory");

    if (PicSpuriousCount() != (spurious_before + 1U))
    {
        KernelWriteString("  A spurious request was not recognised.\n");
        succeeded = false;
    }

    if (PicRequestCount() != (requests_before + 2U))
    {
        KernelWriteString("  A spurious request was counted as a genuine one.\n");
        succeeded = false;
    }

    /* --- The remapping holds with the interrupt flag set. --- */

    /*
     * This is the assertion that the remapping itself is correct, and it cannot
     * be made by inspection: the 8259A does not present ICW2 for reading, so the
     * vector base cannot be read back from the device.
     *
     * Were the controllers still presenting the vectors the firmware programmed,
     * the interval timer, which the IBM Personal Computer AT technical reference
     * records as left running by the firmware upon IR0, would deliver its request
     * as vector 8 the instant the interrupt flag was set. Vector 8 is the double
     * fault, per Intel SDM, Volume 3A, Table 6-1, and the machine would not reach
     * the following line.
     *
     * Every line is masked, so nothing should be delivered at all; the pause is
     * long enough for many timer periods at the firmware's default rate.
     */
    __asm__ __volatile__("sti" : : : "memory");

    for (volatile uint32_t spin = 0U; spin < 2000000U; ++spin)
    {
        /* Deliberately empty: time is allowed to pass with interrupts enabled. */
    }

    __asm__ __volatile__("cli" : : : "memory");

    if (PicRequestCount() != (requests_before + 2U))
    {
        KernelWriteString("  A masked line was nevertheless delivered.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Interrupt controller self-test passed.\n"
                          : "Interrupt controller self-test FAILED.\n");
}

/*
 * Exercises the programmable interval timer, being sub-task 3.6.
 *
 * This is the first self-test whose subject is a device that acts of its own
 * accord, and the difficulty that introduces is that there is no second clock
 * against which to check the first. Every assertion below is therefore either
 * internal to the timer, or concerns the relationship between the timer and the
 * interrupt controller beneath it, which is the part most likely to be wrong.
 */
static void KernelVerifyPit(void)
{
    const uint32_t expected_divisor = PIT_BASE_FREQUENCY / PIT_DEFAULT_FREQUENCY;
    uint16_t first_reading;
    uint16_t second_reading;
    uint64_t ticks_before;
    uint64_t ticks_after;
    uint64_t ticks_while_masked;
    bool observed_change = false;
    bool succeeded = true;

    if (!PitIsRunning())
    {
        KernelWriteString("  The timer reports that it was not initialised.\n");
        KernelWriteString("Interval timer self-test FAILED.\n");
        return;
    }

    /* --- The divisor took effect. --- */

    /*
     * The requested frequency is 1000 Hz and the clock 1193182 Hz, so the
     * divisor rounds to 1193. A divisor that differed would mean the arithmetic
     * of PitDivisorForFrequency was wrong, and every interval the kernel ever
     * measured would be wrong with it.
     */
    if (PitDivisor() != expected_divisor && PitDivisor() != (expected_divisor + 1U))
    {
        KernelWriteString("  The divisor is not that required by the frequency.\n");
        succeeded = false;
    }

    /* --- The counter is running, and running within the divisor. --- */

    /*
     * Two latched readings separated by a delay must differ. This is the only
     * assertion available before interrupts are enabled, and it distinguishes a
     * counter that was programmed from one that was not.
     */
    first_reading = PitReadCounter();

    for (volatile uint32_t spin = 0U; spin < 100000U; ++spin)
    {
        /* Deliberately empty: time is allowed to pass. */
    }

    second_reading = PitReadCounter();

    if (first_reading == second_reading)
    {
        KernelWriteString("  The counter is not counting.\n");
        succeeded = false;
    }

    /*
     * Every reading must lie within the divisor, the counter counting down from
     * it and reloading. Were the divisor not in force the counter would range
     * over the whole of its sixteen bits, and readings above the divisor would
     * appear almost at once. This is the only means of confirming the divisor
     * from within the machine, the 8254 offering no way to read a count back
     * other than the one in progress.
     */
    for (size_t sample = 0U; sample < 64U; ++sample)
    {
        const uint16_t reading = PitReadCounter();

        if ((uint32_t)reading > PitDivisor())
        {
            KernelWriteString("  A count exceeded the divisor.\n");
            succeeded = false;
            break;
        }

        if (reading != first_reading)
        {
            observed_change = true;
        }
    }

    if (!observed_change)
    {
        KernelWriteString("  Repeated readings of the counter never changed.\n");
        succeeded = false;
    }

    /* --- The line is claimed and permitted. --- */

    if (PicRegisteredHandler(PIT_IRQ) == NULL)
    {
        KernelWriteString("  The timer did not claim its request line.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(PIT_IRQ))
    {
        KernelWriteString("  The timer's request line is masked.\n");
        succeeded = false;
    }

    /* --- No tick is counted while interrupts are disabled. --- */

    ticks_before = PitTickCount();

    for (volatile uint32_t spin = 0U; spin < 500000U; ++spin)
    {
        /* Deliberately empty. */
    }

    if (PitTickCount() != ticks_before)
    {
        KernelWriteString("  A tick was counted with the interrupt flag clear.\n");
        succeeded = false;
    }

    /* --- Ticks are counted once interrupts are enabled. --- */

    __asm__ __volatile__("sti" : : : "memory");

    /*
     * The wait is bounded and reports its own failure rather than spinning for
     * ever. A timer that never fires is precisely the defect this test exists to
     * find, and a test that hung upon finding it would destroy the diagnosis it
     * was written to produce.
     */
    if (!PitWaitTicks(10U))
    {
        KernelWriteString("  No tick arrived within the permitted interval.\n");
        succeeded = false;
    }

    ticks_after = PitTickCount();

    if (ticks_after < (ticks_before + 10U))
    {
        KernelWriteString("  The tick count did not advance as far as awaited.\n");
        succeeded = false;
    }

    /*
     * The request must have reached the routing layer of the interrupt
     * controller and been counted there. A tick counted here but not there would
     * mean the handler was being entered by some path other than the one the
     * controller uses, and the end-of-interrupt would not be being sent.
     */
    if (PicRequestCount() == 0U)
    {
        KernelWriteString("  The controller recorded no request for the timer.\n");
        succeeded = false;
    }

    if (PicUnclaimedCount() > 1U)
    {
        KernelWriteString("  A timer request was recorded as unclaimed.\n");
        succeeded = false;
    }

    /*
     * The elapsed time must agree with the tick count and the realised
     * frequency. At 1000 Hz the two are numerically equal to within a
     * millisecond, and a gross disagreement would denote an error in the
     * conversion rather than in the timer.
     */
    if (PitMillisecondsElapsed() < (ticks_after - 1U) ||
        PitMillisecondsElapsed() > (ticks_after + 1U))
    {
        KernelWriteString("  The elapsed time does not agree with the tick count.\n");
        succeeded = false;
    }

    /* --- Masking the line stops the ticks, and unmasking resumes them. --- */

    PicMaskLine(PIT_IRQ);

    ticks_while_masked = PitTickCount();

    for (volatile uint32_t spin = 0U; spin < 2000000U; ++spin)
    {
        /* Deliberately empty: far longer than a tick period. */
    }

    if (PitTickCount() != ticks_while_masked)
    {
        KernelWriteString("  A tick was counted while the line was masked.\n");
        succeeded = false;
    }

    PicUnmaskLine(PIT_IRQ);

    if (!PitWaitTicks(2U))
    {
        KernelWriteString("  Ticks did not resume when the line was unmasked.\n");
        succeeded = false;
    }

    __asm__ __volatile__("cli" : : : "memory");

    KernelWriteString(succeeded
                          ? "Interval timer self-test passed.\n"
                          : "Interval timer self-test FAILED.\n");
}

/*
 * Drives the keyboard decoder with one scancode and yields the character of the
 * event it produced, or zero where it produced none.
 *
 * The decoder is driven directly rather than by way of the controller, which is
 * what permits the whole of scan code set 1 to be exercised upon a machine at
 * which nobody is typing. The path from the controller to the decoder is covered
 * separately, by the assertions upon the request line.
 */
static char KernelKeyboardDecode(uint8_t scancode)
{
    KeyEvent event;

    KeyboardProcessScancode(scancode);

    if (!KeyboardReadEvent(&event))
    {
        return '\0';
    }

    return event.pressed ? event.character : '\0';
}

/*
 * Exercises the PS/2 keyboard driver, being sub-task 3.7.
 *
 * A keyboard cannot be made to produce a keystroke by the kernel that drives it,
 * so the test is divided. The controller and the request line are asserted as
 * configured state; the decoding of scan code set 1, the modifier discipline and
 * the circular buffer are asserted by driving the decoder with codes of the
 * kernel's own choosing, which is exactly what the hardware would deliver.
 */
static void KernelVerifyKeyboard(void)
{
    KeyEvent event;
    char character;
    uint64_t discarded_before;
    bool succeeded = true;

    if (!KeyboardIsPresent())
    {
        /*
         * Not a failure of the test. A machine may genuinely have no PS/2
         * keyboard, and the driver is required to discover that without
         * blocking; reaching this line at all is evidence that it did.
         */
        KernelWriteString("  No keyboard was found; the decoder is not exercised.\n");
        KernelWriteString("Keyboard self-test skipped.\n");
        return;
    }

    /* --- The controller was configured and the line claimed. --- */

    if (PicRegisteredHandler(KEYBOARD_IRQ) == NULL)
    {
        KernelWriteString("  The keyboard did not claim its request line.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(KEYBOARD_IRQ))
    {
        KernelWriteString("  The keyboard's request line is masked.\n");
        succeeded = false;
    }

    /* Begin from a known state, the firmware having used the keyboard before us. */
    KeyboardFlush();

    if (KeyboardHasEvent() || KeyboardReadEvent(&event))
    {
        KernelWriteString("  The buffer is not empty after a flush.\n");
        succeeded = false;
    }

    if (KeyboardModifiers() != 0U)
    {
        KernelWriteString("  A modifier is in force after a flush.\n");
        succeeded = false;
    }

    /* --- An unshifted key yields its lower-case character. --- */

    if (KernelKeyboardDecode(0x1EU) != 'a')
    {
        KernelWriteString("  An unshifted key did not yield its character.\n");
        succeeded = false;
    }

    /* --- A release is recorded, and is distinguished from a depression. --- */

    KeyboardProcessScancode(0x9EU);

    if (!KeyboardReadEvent(&event))
    {
        KernelWriteString("  A release produced no event.\n");
        succeeded = false;
    }
    else if (event.pressed || event.scancode != 0x1EU)
    {
        KernelWriteString("  A release was decoded as a depression.\n");
        succeeded = false;
    }

    /* --- A modifier produces no event of its own, and alters the next key. --- */

    KeyboardProcessScancode(0x2AU);

    if (KeyboardHasEvent())
    {
        KernelWriteString("  A modifier key produced an event of its own.\n");
        succeeded = false;
    }

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_SHIFT) == 0U)
    {
        KernelWriteString("  A depressed shift key did not set its flag.\n");
        succeeded = false;
    }

    if (KernelKeyboardDecode(0x1EU) != 'A')
    {
        KernelWriteString("  A shifted letter did not yield its capital.\n");
        succeeded = false;
    }

    /* A shifted digit yields its punctuation, which capitals lock must not. */
    if (KernelKeyboardDecode(0x02U) != '!')
    {
        KernelWriteString("  A shifted digit did not yield its punctuation.\n");
        succeeded = false;
    }

    KeyboardProcessScancode(0xAAU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_SHIFT) != 0U)
    {
        KernelWriteString("  A released shift key did not clear its flag.\n");
        succeeded = false;
    }

    if (KernelKeyboardDecode(0x1EU) != 'a')
    {
        KernelWriteString("  The letter did not revert when shift was released.\n");
        succeeded = false;
    }

    /* --- Capitals lock is a latch, and applies to letters alone. --- */

    KeyboardProcessScancode(0x3AU);
    KeyboardProcessScancode(0xBAU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CAPS_LOCK) == 0U)
    {
        KernelWriteString("  Capitals lock did not latch upon a full keystroke.\n");
        succeeded = false;
    }

    if (KernelKeyboardDecode(0x1EU) != 'A')
    {
        KernelWriteString("  Capitals lock did not capitalise a letter.\n");
        succeeded = false;
    }

    /*
     * The lock must not act upon a digit. Were it implemented as a second shift
     * this would yield an exclamation mark, which is the commonest way for this
     * to be got wrong.
     */
    if (KernelKeyboardDecode(0x02U) != '1')
    {
        KernelWriteString("  Capitals lock altered a digit.\n");
        succeeded = false;
    }

    /*
     * Shift with the lock engaged yields the lower-case letter. The two combine
     * as an exclusive disjunction, not as a disjunction.
     */
    KeyboardProcessScancode(0x2AU);

    if (KernelKeyboardDecode(0x1EU) != 'a')
    {
        KernelWriteString("  Shift with capitals lock did not yield lower case.\n");
        succeeded = false;
    }

    KeyboardProcessScancode(0xAAU);

    /* Release the latch, so that the state left behind is the state found. */
    KeyboardProcessScancode(0x3AU);
    KeyboardProcessScancode(0xBAU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CAPS_LOCK) != 0U)
    {
        KernelWriteString("  Capitals lock did not unlatch.\n");
        succeeded = false;
    }

    /* --- The extended prefix is consumed and marks the event it precedes. --- */

    KeyboardProcessScancode(0xE0U);
    KeyboardProcessScancode(0x1DU);

    if (KeyboardHasEvent())
    {
        KernelWriteString("  The right control key produced an event.\n");
        succeeded = false;
    }

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CONTROL) == 0U)
    {
        KernelWriteString("  The right control key did not set the control flag.\n");
        succeeded = false;
    }

    KeyboardProcessScancode(0xE0U);
    KeyboardProcessScancode(0x9DU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CONTROL) != 0U)
    {
        KernelWriteString("  The right control key did not clear the control flag.\n");
        succeeded = false;
    }

    /*
     * An extended key that is not a modifier produces an event marked extended
     * and bearing no character, its number being shared with an ordinary key.
     */
    KeyboardProcessScancode(0xE0U);
    KeyboardProcessScancode(0x48U);

    if (!KeyboardReadEvent(&event))
    {
        KernelWriteString("  An extended key produced no event.\n");
        succeeded = false;
    }
    else if (!event.extended || event.character != '\0' || event.scancode != 0x48U)
    {
        KernelWriteString("  An extended key was decoded as its ordinary twin.\n");
        succeeded = false;
    }

    /* --- Reading a character skips releases. --- */

    KeyboardProcessScancode(0x30U); /* 'b' depressed. */
    KeyboardProcessScancode(0xB0U); /* 'b' released. */
    KeyboardProcessScancode(0x2EU); /* 'c' depressed. */

    if (!KeyboardReadCharacter(&character) || character != 'b')
    {
        KernelWriteString("  A character was not read from the buffer.\n");
        succeeded = false;
    }

    if (!KeyboardReadCharacter(&character) || character != 'c')
    {
        KernelWriteString("  A release was not skipped when reading a character.\n");
        succeeded = false;
    }

    if (KeyboardReadCharacter(&character))
    {
        KernelWriteString("  A character was read from an exhausted buffer.\n");
        succeeded = false;
    }

    /* --- The buffer is circular, and an overrun is counted rather than silent. --- */

    discarded_before = KeyboardOverflowCount();

    for (size_t index = 0U; index < (KEYBOARD_BUFFER_CAPACITY + 8U); ++index)
    {
        KeyboardProcessScancode(0x1EU);
    }

    if (KeyboardOverflowCount() != (discarded_before + 8U))
    {
        KernelWriteString("  An overrun was not counted exactly.\n");
        succeeded = false;
    }

    /*
     * The events that were accepted must still be readable and intact. An
     * overrun that corrupted the buffer rather than refusing the surplus would
     * be far worse than one that simply lost keystrokes.
     */
    {
        size_t recovered = 0U;

        while (KeyboardReadEvent(&event))
        {
            if (event.scancode != 0x1EU || !event.pressed)
            {
                KernelWriteString("  An event survived the overrun corrupted.\n");
                succeeded = false;
                break;
            }

            ++recovered;
        }

        if (recovered != KEYBOARD_BUFFER_CAPACITY)
        {
            KernelWriteString("  The buffer did not hold its stated capacity.\n");
            succeeded = false;
        }
    }

    KeyboardFlush();

    KernelWriteString(succeeded
                          ? "Keyboard self-test passed.\n"
                          : "Keyboard self-test FAILED.\n");
}

/*
 * Exercises the serial driver, being sub-task 4.1.
 *
 * Three things here can fail silently, and they are what the test is for.
 *
 * The first is the transmitter interrupt. The condition it reports is a level
 * and not an event: an adapter with nothing to send holds its transmitter
 * holding register empty permanently, so a driver that left the interrupt
 * enabled would be asked to service a condition it could not dismiss, and the
 * machine would make no further progress while reporting nothing at all.
 *
 * The second is the interrupt path itself. The driver retains a polled path for
 * the circumstances in which no interrupt can arrive, and that path works
 * whether or not the request line was ever claimed; a driver that had claimed
 * nothing would therefore appear to function perfectly. The count of the
 * adapter's interrupts is what distinguishes the two.
 *
 * The third is the line parameters. A divisor computed wrongly yields output at
 * a rate nothing is listening at, which is indistinguishable from an absent
 * adapter, and the rate realised is not in general the rate requested.
 */
static void KernelVerifySerial(void)
{
    static const SerialConfiguration alternative = {
        9600U, 7U, SERIAL_PARITY_EVEN, SERIAL_STOP_BITS_TWO
    };
    static const SerialConfiguration standard = {
        115200U, 8U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE
    };

    /*
     * A rate of zero, a rate above the greatest the oscillator can produce, and
     * word lengths on either side of the five to eight the register can express.
     * Each must be refused with the parameters in force left untouched.
     */
    static const SerialConfiguration impossible[] = {
        { 0U, 8U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE },
        { 230400U, 8U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE },
        { 9600U, 4U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE },
        { 9600U, 9U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE }
    };

    uint64_t interrupts_before;
    uint64_t transmitted_before;
    bool loopback_passed;
    bool rejected_impossible = true;
    bool accepted_alternative;
    bool alternative_divisor_correct;
    bool restored;
    bool succeeded = true;

    if (!SerialIsPresent())
    {
        /*
         * Not a failure of the test. A machine may genuinely have no serial
         * adapter, and the loopback test at initialisation is what discovers it.
         */
        KernelWriteString("Serial self-test skipped; no adapter is present.\n");
        return;
    }

    /* --- The line was claimed and the request line permitted. --- */

    if (!SerialInterruptsActive())
    {
        KernelWriteString("  The serial driver is still polling.\n");
        succeeded = false;
    }

    if (PicRegisteredHandler(SERIAL_COM1_IRQ) == NULL)
    {
        KernelWriteString("  The serial adapter did not claim its request line.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(SERIAL_COM1_IRQ))
    {
        KernelWriteString("  The serial adapter's request line is masked.\n");
        succeeded = false;
    }

    /* --- The adapter carries a character out and back unaltered. --- */

    loopback_passed = SerialLoopbackTest();

    if (!loopback_passed)
    {
        KernelWriteString("  A sequence did not return unaltered through the loopback.\n");
        succeeded = false;
    }

    /*
     * --- The line parameters are computed, and the impossible refused. ---
     *
     * Nothing is written to the console between the two configurations below.
     * The alternative rate is applied to the adapter, and anything transmitted
     * while it stood would reach a listening terminal as noise.
     */

    for (size_t index = 0U;
         index < (sizeof impossible / sizeof impossible[0]);
         ++index)
    {
        if (SerialConfigure(&impossible[index]))
        {
            rejected_impossible = false;
        }
    }

    if (SerialConfigure(NULL))
    {
        rejected_impossible = false;
    }

    accepted_alternative = SerialConfigure(&alternative);
    alternative_divisor_correct =
        (SerialDivisor() == 12U) && (SerialRealisedBaudRate() == 9600U);
    restored = SerialConfigure(&standard) && (SerialDivisor() == 1U) &&
               (SerialRealisedBaudRate() == SERIAL_MAXIMUM_BAUD_RATE);

    if (!rejected_impossible)
    {
        KernelWriteString("  An impossible line configuration was accepted.\n");
        succeeded = false;
    }

    if (!accepted_alternative || !alternative_divisor_correct)
    {
        KernelWriteString("  9600 baud did not yield a divisor of twelve.\n");
        succeeded = false;
    }

    if (!restored)
    {
        KernelWriteString("  The default line parameters were not restored.\n");
        succeeded = false;
    }

    /* --- Characters leave by way of an interrupt, and the request is withdrawn. --- */

    interrupts_before = SerialInterruptCount();
    transmitted_before = SerialCharactersTransmitted();

    /*
     * The interrupt flag is set for the duration, this being the only way an
     * interrupt can be taken; the flag is otherwise clear throughout
     * initialisation. The string is written to the adapter alone, the display
     * having no part in what is being asserted.
     */
    __asm__ __volatile__("sti" : : : "memory");

    SerialWriteString("Serial self-test: this line was carried by interrupt.\n");
    SerialFlush();

    __asm__ __volatile__("cli" : : : "memory");

    if (SerialInterruptCount() == interrupts_before)
    {
        KernelWriteString("  The adapter transmitted without raising an interrupt.\n");
        succeeded = false;
    }

    if (SerialCharactersTransmitted() == transmitted_before)
    {
        KernelWriteString("  No character was transmitted.\n");
        succeeded = false;
    }

    if (SerialTransmitInterruptEnabled())
    {
        KernelWriteString("  The transmitter interrupt was not withdrawn when idle.\n");
        succeeded = false;
    }

    if (SerialLineErrorCount() != 0U)
    {
        KernelWriteString("  The line reported an error during the test.\n");
        succeeded = false;
    }

    /* The loopback and the firmware may both have left characters behind. */
    SerialFlushBuffers();

    KernelWriteString(succeeded
                          ? "Serial self-test passed.\n"
                          : "Serial self-test FAILED.\n");
}

/*
 * Asserts that the display driver moves the cursor as the control characters
 * require, that it reaches the CRT controller, and that the backspace stops
 * where it is told to. The failure this guards against is a silent one: a
 * control character for which the driver has no case is written into the frame
 * buffer as whatever glyph the adapter's font holds at that code point, and the
 * cursor then advances to the right. The display is not corrupted in any way the
 * machine can notice, and the defect is visible only to somebody reading the
 * screen. That is exactly how the backspace came to be broken.
 *
 * The properties asserted here are chosen upon the same principle throughout: a
 * cursor written to the wrong CRT controller register, an attribute written
 * while the controller's flip-flop stood at the data register, a scroll that
 * moved the display by the wrong number of rows — each leaves a machine that
 * runs perfectly and a display that is wrong to look at.
 *
 * The test writes upon the display, so it begins at the start of a row and
 * leaves its own result to overwrite the characters used.
 */
static void KernelVerifyVga(void)
{
    size_t row;
    size_t column;
    size_t original_row;
    size_t limit_row;
    size_t limit_column;
    uint64_t scroll_marker;
    bool succeeded = true;

    /* The adapter's configuration governs every register access that follows. */
    if (!VgaIsColourAdapter() || (VgaCrtcIndexPort() != 0x03D4U))
    {
        KernelWriteString("  The display adapter is not in its colour configuration.\n");
        succeeded = false;
    }

    if (VgaBlinkEnabled())
    {
        KernelWriteString("  Blinking was not disabled; bright backgrounds will blink.\n");
        succeeded = false;
    }

    VgaPutCharacter('\n');
    VgaCursorPosition(&original_row, &column);

    if (column != 0U)
    {
        KernelWriteString("  A line feed did not return the cursor to the first column.\n");
        succeeded = false;
    }

    /*
     * The erase limit is placed here. Everything the test writes below stands
     * after it, and the boot log above it is therefore beyond the reach of the
     * backspaces the test performs, which is the property the limit exists for.
     */
    VgaSetEraseLimit();
    VgaEraseLimit(&limit_row, &limit_column);

    if ((limit_row != original_row) || (limit_column != 0U))
    {
        KernelWriteString("  The erase limit was not recorded at the cursor.\n");
        succeeded = false;
    }

    /* A backspace at the limit must not move at all. */
    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U))
    {
        KernelWriteString("  A backspace at the erase limit moved the cursor.\n");
        succeeded = false;
    }

    /* A backspace elsewhere retreats by exactly one column. */
    VgaPutCharacter('X');
    VgaPutCharacter('Y');
    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 1U))
    {
        KernelWriteString("  A backspace did not retreat by one column.\n");
        succeeded = false;
    }

    /*
     * The erasing sequence the callers use must leave the cursor where the
     * erased character stood, so that the next character written replaces it,
     * and must have blanked the cell it passed over.
     */
    VgaWriteString("\b \b");
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U) || (VgaCharacterAt(row, 0U) != ' '))
    {
        KernelWriteString("  The erasing sequence did not erase and restore the cursor.\n");
        succeeded = false;
    }

    /* A tabulation advances to a multiple of eight columns, not by eight. */
    VgaPutCharacter('A');
    VgaPutCharacter('\t');
    VgaCursorPosition(&row, &column);

    if (column != 8U)
    {
        KernelWriteString("  A tabulation did not advance to a multiple of eight.\n");
        succeeded = false;
    }

    /* A carriage return returns to the first column without changing the row. */
    VgaPutCharacter('\r');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U))
    {
        KernelWriteString("  A carriage return did not return to the first column.\n");
        succeeded = false;
    }

    /*
     * A backspace in the first column crosses into the row above and stops
     * immediately after the text standing there, having consumed the separator
     * between the two rows and nothing else. This is what allows a line of input
     * to be corrected after a line feed; the character at the end of the row
     * above is erased by the next backspace, not by this one.
     */
    scroll_marker = VgaScrollCount();
    VgaWriteString("ab\n");

    /*
     * That line feed stood upon the final row if the boot log had filled the
     * display, in which case everything above has moved up by one row and the
     * row the test is reasoning about has moved with it.
     */
    if (VgaScrollCount() != scroll_marker)
    {
        --original_row;
    }

    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 2U) || (VgaCharacterAt(row, 1U) != 'b'))
    {
        KernelWriteString("  A backspace across the row boundary consumed a character.\n");
        succeeded = false;
    }

    /* Erasing both characters returns the cursor to the limit, and no further. */
    VgaWriteString("\b \b");
    VgaWriteString("\b \b");
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U) || (VgaCharacterAt(row, 1U) != ' '))
    {
        KernelWriteString("  Erasing across the row boundary left the cursor astray.\n");
        succeeded = false;
    }

    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U))
    {
        KernelWriteString("  A backspace passed the erase limit into the boot log.\n");
        succeeded = false;
    }

    /* The controller must hold the position the driver believes it holds. */
    VgaCursorPosition(&row, &column);

    {
        size_t hardware_row;
        size_t hardware_column;
        const bool visible = VgaHardwareCursorPosition(&hardware_row, &hardware_column);

        if ((hardware_row != row) || (hardware_column != column))
        {
            KernelWriteString("  The hardware cursor is not where the driver believes.\n");
            succeeded = false;
        }

        if (!visible)
        {
            KernelWriteString("  The hardware cursor was not displayed.\n");
            succeeded = false;
        }
    }

    /* A position outside the display is refused rather than wrapped. */
    if (VgaSetCursorPosition(VGA_HEIGHT, 0U) || VgaSetCursorPosition(0U, VGA_WIDTH))
    {
        KernelWriteString("  A cursor position outside the display was accepted.\n");
        succeeded = false;
    }

    /* Hiding the cursor must be observable in the controller, and reversible. */
    VgaSetCursorVisible(false);

    if (VgaCursorVisible())
    {
        KernelWriteString("  The cursor was not hidden when it was hidden.\n");
        succeeded = false;
    }

    VgaSetCursorVisible(true);

    if (!VgaCursorVisible())
    {
        KernelWriteString("  The cursor was not restored after being hidden.\n");
        succeeded = false;
    }

    /* A shape whose first scan line is below its last would present no cursor. */
    if (VgaSetCursorShape(4U, 2U) || VgaSetCursorShape(0U, 32U))
    {
        KernelWriteString("  An impossible cursor shape was accepted.\n");
        succeeded = false;
    }

    /*
     * The scroll is asserted upon the contents of the display itself, a scroll
     * by the wrong number of rows being invisible to everything else. One row of
     * the boot log leaves the display for the purpose; the record upon the
     * serial line is unaffected.
     */
    if (original_row > 0U)
    {
        const uint64_t scrolls = VgaScrollCount();

        VgaSetCursorPosition(original_row, 0U);
        VgaPutCharacter('Z');
        VgaScroll();

        if ((VgaCharacterAt(original_row - 1U, 0U) != 'Z') ||
            (VgaCharacterAt(VGA_HEIGHT - 1U, 0U) != ' ') || (VgaScrollCount() != scrolls + 1U))
        {
            KernelWriteString("  The display did not scroll by exactly one row.\n");
            succeeded = false;
        }

        VgaSetCursorPosition(original_row, 0U);
    }

    /*
     * The characters written above stand upon this row still. The result is
     * written over them, and padded so that none survives to its right.
     */
    KernelWriteString(succeeded
                          ? "Display self-test passed.            \n"
                          : "Display self-test FAILED.            \n");
}

/*
 * Asserts that the bus enumeration reached the hardware and understood what it
 * read.
 *
 * The failure this guards against is that the enumeration is unfalsifiable by
 * inspection. A configuration read of a function that is not there returns all
 * ones rather than failing, and so does a read composed with the bus, device and
 * function fields shifted into the wrong positions: an enumerator with its
 * address arithmetic wrong finds nothing at all and reports an empty bus, which
 * is indistinguishable from a machine that has no devices. The test therefore
 * asserts that specific things were found and that the accessors agree with one
 * another, rather than that the enumeration completed.
 */
static void KernelVerifyPci(void)
{
    static const PciAddress host = { 0U, 0U, 0U };
    static const PciAddress absent = { 255U, 31U, 7U };
    const PciFunction *entry;
    uint32_t identifiers;
    size_t found_at = 0U;
    bool succeeded = true;

    if (!PciMechanismOnePresent())
    {
        KernelWriteString("  Configuration mechanism one did not answer.\n");
        KernelWriteString("Bus self-test FAILED.\n");
        return;
    }

    /* An address nothing decodes must read as all ones, not as a device. */
    if (PciReadConfiguration32(absent, PCI_OFFSET_VENDOR_ID) != UINT32_C(0xFFFFFFFF))
    {
        KernelWriteString("  An absent function did not read as all ones.\n");
        succeeded = false;
    }

    /*
     * The narrow accessors extract their field from the double word containing
     * it. A shift taken from the wrong bits of the offset would yield a
     * plausible number rather than an obviously wrong one, so the halves are
     * compared against the whole.
     */
    identifiers = PciReadConfiguration32(host, PCI_OFFSET_VENDOR_ID);

    if ((PciReadConfiguration16(host, PCI_OFFSET_VENDOR_ID) !=
         (uint16_t)(identifiers & 0xFFFFU)) ||
        (PciReadConfiguration16(host, PCI_OFFSET_DEVICE_ID) !=
         (uint16_t)(identifiers >> 16)) ||
        (PciReadConfiguration8(host, PCI_OFFSET_VENDOR_ID) != (uint8_t)(identifiers & 0xFFU)))
    {
        KernelWriteString("  The narrow accessors disagree with the wide one.\n");
        succeeded = false;
    }

    /* Something must have been found, and the first bus must have been scanned. */
    if ((PciFunctionCount() == 0U) || (PciBusesScanned() == 0U))
    {
        KernelWriteString("  The enumeration found nothing at all.\n");
        succeeded = false;
    }

    if (PciFunctionsDiscarded() != 0U)
    {
        KernelWriteString("  More functions answered than the table holds.\n");
        succeeded = false;
    }

    /*
     * Every machine this kernel runs upon presents a host bridge at the first
     * address of the first bus. Its absence means the enumeration is reading
     * somewhere other than where it believes.
     */
    entry = PciFunctionAt(0U);

    if ((entry == NULL) || (entry->address.bus != 0U) || (entry->address.device != 0U) ||
        (entry->address.function != 0U) || (entry->class_code != PCI_CLASS_BRIDGE) ||
        (entry->subclass != PCI_SUBCLASS_HOST_BRIDGE))
    {
        KernelWriteString("  No host bridge stands at the root of the bus.\n");
        succeeded = false;
    }

    /* The index is bounded, and the search finds what the table holds. */
    if (PciFunctionAt(PciFunctionCount()) != NULL)
    {
        KernelWriteString("  A function was reported beyond the end of the table.\n");
        succeeded = false;
    }

    if ((entry != NULL) &&
        (PciFindByIdentifier(entry->vendor_id, entry->device_id) == NULL))
    {
        KernelWriteString("  A recorded function was not found by its identifiers.\n");
        succeeded = false;
    }

    if (PciFindByClass(PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST_BRIDGE, 0U, &found_at) == NULL)
    {
        KernelWriteString("  The host bridge was not found by its class.\n");
        succeeded = false;
    }

    if (PciFindByClass(0xFFU, 0xFFU, PciFunctionCount(), NULL) != NULL)
    {
        KernelWriteString("  A search beginning past the table returned a function.\n");
        succeeded = false;
    }

    /*
     * Every function recorded must be a function that answered, and every base
     * address must have had its type and attribute bits removed. A base address
     * still carrying them would be a port number or an address off by up to
     * fifteen, which addresses hardware that is nearly right.
     */
    for (size_t index = 0U; index < PciFunctionCount(); ++index)
    {
        const PciFunction *const current = PciFunctionAt(index);

        if ((current == NULL) || (current->vendor_id == PCI_VENDOR_INVALID))
        {
            KernelWriteString("  A function was recorded that did not answer.\n");
            succeeded = false;
            break;
        }

        for (size_t bar = 0U; bar < PCI_BAR_COUNT; ++bar)
        {
            const uint64_t base = PciBarBase(current, bar);
            const uint64_t alignment = PciBarIsIoPort(current, bar) ? 3U : 15U;

            if ((base & alignment) != 0U)
            {
                KernelWriteString("  A base address retains its type bits.\n");
                succeeded = false;
                break;
            }
        }
    }

    KernelWriteString(succeeded ? "Bus self-test passed.\n" : "Bus self-test FAILED.\n");
}

/*
 * True if the boot loader's command line contains the stated option as a
 * complete word.
 *
 * The kernel has no other means of being told anything at the moment it starts,
 * and one thing it must be told is whether it is permitted to write to a disk.
 * Comparing complete words rather than substrings matters: an option is a
 * decision the operator made, and a decision must not be triggered by a longer
 * word that happens to contain it.
 */
static bool KernelCommandLineHasOption(const char *option)
{
    const char *const line = KernelBootInformation.command_line;
    size_t position = 0U;

    while (line[position] != '\0')
    {
        size_t length = 0U;

        while ((line[position] == ' ') || (line[position] == '\t'))
        {
            ++position;
        }

        while ((option[length] != '\0') && (line[position + length] == option[length]))
        {
            ++length;
        }

        if ((option[length] == '\0') &&
            ((line[position + length] == '\0') || (line[position + length] == ' ') ||
             (line[position + length] == '\t')))
        {
            return true;
        }

        while ((line[position] != '\0') && (line[position] != ' ') && (line[position] != '\t'))
        {
            ++position;
        }
    }

    return false;
}

/*
 * The buffers the disk self-test reads into. They are of static storage duration
 * because the boot stack is 64 KiB and three sectors of it would be a
 * disproportionate share of what remains after the self-tests above.
 */
static uint8_t KernelDiskBufferA[ATA_SECTOR_SIZE * 2U];
static uint8_t KernelDiskBufferB[ATA_SECTOR_SIZE * 2U];

/* True if two regions hold the same bytes. */
static bool KernelRegionsMatch(const uint8_t *left, const uint8_t *right, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the disk driver addresses the sector it was asked for and
 * transfers exactly its contents.
 *
 * The failure this guards against is the worst kind the kernel has yet had to
 * consider: a driver that reads the wrong sector returns data, and data that
 * arrived is indistinguishable from data that is correct until something tries
 * to interpret it. An address composed with a byte in the wrong register, a
 * transfer of 255 words instead of 256, a second sector written over the first —
 * each of these produces a disk that appears to work and a filesystem that
 * decays. Every assertion below is chosen to make one of those visible.
 *
 * The test reads. It writes only when the operator has asked for it upon the
 * command line, and then only to a sector whose previous contents it has read
 * and restores afterwards: a self-test that wrote to a disk unbidden would
 * destroy the data of anybody who booted this kernel upon their own machine.
 */
static void KernelVerifyAta(void)
{
    const AtaDevice *const disk = AtaFirstDisk();
    bool succeeded = true;

    if (AtaDeviceCount() == 0U)
    {
        KernelWriteString("Disk self-test: no device answered; nothing to assert.\n");
        return;
    }

    if (disk == NULL)
    {
        KernelWriteString("Disk self-test: devices answered but none is a disk.\n");
        return;
    }

    /* An identification that yielded no capacity was not understood. */
    if ((disk->sector_count == 0U) || (disk->model[0] == '\0'))
    {
        KernelWriteString("  The identification data yielded no capacity or model.\n");
        succeeded = false;
    }

    /* The first sector must be readable, and must read the same way twice. */
    if (!AtaRead(disk, 0U, 1U, KernelDiskBufferA))
    {
        KernelWriteString("  The first sector could not be read: ");
        KernelWriteString(AtaLastError());
        KernelWriteString("\n");
        succeeded = false;
    }
    else if (!AtaRead(disk, 0U, 1U, KernelDiskBufferB) ||
             !KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
    {
        KernelWriteString("  The same sector read differently upon a second attempt.\n");
        succeeded = false;
    }

    /*
     * A two-sector read must place the second sector after the first, and the
     * first must be what a one-sector read of the same address returned. A
     * driver that lost synchronisation between sectors, or that overwrote the
     * first with the second, passes every other assertion here.
     */
    if (disk->sector_count >= 2U)
    {
        if (!AtaRead(disk, 0U, 2U, KernelDiskBufferB))
        {
            KernelWriteString("  A two-sector read failed.\n");
            succeeded = false;
        }
        else
        {
            if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
            {
                KernelWriteString("  A two-sector read did not begin where a one-sector "
                                  "read did.\n");
                succeeded = false;
            }

            if (!AtaRead(disk, 1U, 1U, KernelDiskBufferA) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[ATA_SECTOR_SIZE],
                                    ATA_SECTOR_SIZE))
            {
                KernelWriteString("  The second sector of a two-sector read is not the "
                                  "sector that follows.\n");
                succeeded = false;
            }
        }
    }

    /* A range beyond the capacity is refused rather than attempted. */
    if (AtaRead(disk, disk->sector_count, 1U, KernelDiskBufferA) ||
        AtaRead(disk, disk->sector_count - 1U, 2U, KernelDiskBufferA))
    {
        KernelWriteString("  A read beyond the capacity of the device was accepted.\n");
        succeeded = false;
    }

    /* A request without a buffer, and one for no sectors, are both harmless. */
    if (AtaRead(disk, 0U, 1U, NULL) || !AtaRead(disk, 0U, 0U, KernelDiskBufferA))
    {
        KernelWriteString("  A degenerate request was mishandled.\n");
        succeeded = false;
    }

    /*
     * A device larger than 28 bits can name exercises the 48-bit commands, which
     * are otherwise never reached. The register writing they require is
     * different in kind and not merely in width — each register is written
     * twice, high-order byte first — so a driver that has never issued one has
     * not been tested at all in that mode.
     */
    if (disk->supports_lba48 && (disk->sector_count > ATA_LBA28_LIMIT))
    {
        if (!AtaRead(disk, ATA_LBA28_LIMIT + 1U, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  A sector beyond the 28-bit limit could not be read: ");
            KernelWriteString(AtaLastError());
            KernelWriteString("\n");
            succeeded = false;
        }
    }

    /*
     * The write path, only upon request. The sector is read, overwritten with a
     * pattern, read back, compared, and then restored from what was read; the
     * restoration is verified in its turn, since a test that damaged the disk
     * and reported success would be worse than no test.
     */
    if (KernelCommandLineHasOption("disk-write-test"))
    {
        const uint64_t target = disk->sector_count - 1U;

        KernelWriteString("  Writing to the final sector, as the command line permits.\n");

        if (!AtaRead(disk, target, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  The sector to be written could not first be read.\n");
            succeeded = false;
        }
        else
        {
            for (size_t index = 0U; index < ATA_SECTOR_SIZE; ++index)
            {
                KernelDiskBufferB[index] = (uint8_t)(index ^ 0xA5U);
            }

            if (!AtaWrite(disk, target, 1U, KernelDiskBufferB))
            {
                KernelWriteString("  The pattern could not be written: ");
                KernelWriteString(AtaLastError());
                KernelWriteString("\n");
                succeeded = false;
            }
            else if (!AtaRead(disk, target, 1U, &KernelDiskBufferB[ATA_SECTOR_SIZE]))
            {
                KernelWriteString("  The pattern could not be read back.\n");
                succeeded = false;
            }
            else
            {
                for (size_t index = 0U; index < ATA_SECTOR_SIZE; ++index)
                {
                    if (KernelDiskBufferB[ATA_SECTOR_SIZE + index] != (uint8_t)(index ^ 0xA5U))
                    {
                        KernelWriteString("  The pattern read back altered.\n");
                        succeeded = false;
                        break;
                    }
                }
            }

            /* Whatever happened above, the sector is put back as it was found. */
            if (!AtaWrite(disk, target, 1U, KernelDiskBufferA) ||
                !AtaRead(disk, target, 1U, KernelDiskBufferB) ||
                !KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
            {
                KernelWriteString("  The sector was not restored to its previous contents.\n");
                succeeded = false;
            }
        }
    }

    if (AtaTimeoutCount() != 0U)
    {
        KernelWriteString("  A device failed to respond within the driver's patience.\n");
        succeeded = false;
    }

    /*
     * Every refusal above was provoked deliberately; an error is a failure of the
     * hardware and none was expected.
     */
    if (AtaErrorCount() != 0U)
    {
        KernelWriteString("  A device reported an error: ");
        KernelWriteString(AtaLastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Disk self-test passed.\n" : "Disk self-test FAILED.\n");
}

/*
 * A block device backed by memory, existing only for the self-test below.
 *
 * The disk this kernel can reach is not a fit subject for a test of the layer
 * above it: the machine that `make verify` runs upon has no ATA device at all,
 * and a machine that does has data upon it that a self-test must not write to.
 * A device of known contents, of a known size, whose every transfer succeeds,
 * makes the layer testable everywhere and testable exactly — the assertions
 * below can state what a read must return rather than merely that it returned.
 */
#define KERNEL_MEMORY_DEVICE_BLOCKS 256U

static uint8_t KernelMemoryDeviceStore[KERNEL_MEMORY_DEVICE_BLOCKS * BLOCK_SIZE_DEFAULT];

/*
 * A second store, and with it a second device.
 *
 * The filesystem layer of sub-task 5.8 joins several volumes into one tree, and
 * a layer that composes a tree from one volume has not been shown to compose one
 * at all: crossing a mount point, and returning across it by "..", are exactly
 * the properties that do not arise until a second volume exists. The second
 * store is what supplies one. It is filled by copying the first and then
 * altering one field, so that the two volumes are identical in every respect but
 * the one an assertion can name — which is what makes an assertion able to say
 * *which* volume a path reached, and not merely that it reached one.
 */
static uint8_t KernelMemoryDeviceSecondStore[KERNEL_MEMORY_DEVICE_BLOCKS * BLOCK_SIZE_DEFAULT];

/*
 * The store a device addresses. The driver's context selects it: a null context
 * is the first store, which every device registered before sub-task 5.8 passes
 * and which therefore needs no alteration.
 */
static uint8_t *KernelMemoryDeviceStoreOf(void *context)
{
    return (context == NULL) ? KernelMemoryDeviceStore : (uint8_t *)context;
}

static bool KernelMemoryDeviceRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    uint8_t *const destination = (uint8_t *)buffer;
    const uint8_t *const store = KernelMemoryDeviceStoreOf(context);
    const size_t offset = (size_t)block * BLOCK_SIZE_DEFAULT;

    for (size_t index = 0U; index < ((size_t)count * BLOCK_SIZE_DEFAULT); ++index)
    {
        destination[index] = store[offset + index];
    }

    return true;
}

static bool KernelMemoryDeviceWrite(void *context, uint64_t block, uint32_t count,
                                    const void *buffer)
{
    const uint8_t *const source = (const uint8_t *)buffer;
    uint8_t *const store = KernelMemoryDeviceStoreOf(context);
    const size_t offset = (size_t)block * BLOCK_SIZE_DEFAULT;

    for (size_t index = 0U; index < ((size_t)count * BLOCK_SIZE_DEFAULT); ++index)
    {
        store[offset + index] = source[index];
    }

    return true;
}

static const BlockOperations KernelMemoryDeviceOperations = { KernelMemoryDeviceRead,
                                                              KernelMemoryDeviceWrite };

/* The same device without a writer, for the read-only assertions. */
static const BlockOperations KernelMemoryDeviceReadOnlyOperations = { KernelMemoryDeviceRead,
                                                                      NULL };

/* Two blocks of working space for the transfers the self-tests perform. */
static uint8_t KernelBlockBufferA[BLOCK_SIZE_DEFAULT * 2U];
static uint8_t KernelBlockBufferB[BLOCK_SIZE_DEFAULT * 2U];

/* Fills a region with a pattern that depends upon the seed, so that two regions
 * filled from different seeds cannot be confused for one another. */
static void KernelFillPattern(uint8_t *region, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; ++index)
    {
        region[index] = (uint8_t)((index * 31U) + seed);
    }
}

/* True if a region holds the pattern that seed would have produced. */
static bool KernelPatternMatches(const uint8_t *region, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (region[index] != (uint8_t)((index * 31U) + seed))
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the block layer validates what it is asked before it reaches a
 * driver, and transfers what it was given when it does.
 *
 * The layer exists precisely so that the four tests every driver would otherwise
 * repeat are written once, and the consequence of that is that a defect here is
 * a defect in every device at once. The assertions are made against a device of
 * known contents rather than against a disk, for the reason given where that
 * device is defined.
 */
static void KernelVerifyBlock(void)
{
    BlockDevice *device;
    BlockDevice *read_only;
    const size_t already_registered = BlockDeviceCount();
    bool succeeded = true;

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Block self-test FAILED.\n");
        return;
    }

    read_only = BlockRegister("mem1", &KernelMemoryDeviceReadOnlyOperations, NULL,
                              BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, true);

    if (read_only == NULL)
    {
        KernelWriteString("  A read-only device could not be registered.\n");
        succeeded = false;
    }

    /* A name identifies a device, so a second device may not take one in use. */
    if (BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 1U,
                      false) != NULL)
    {
        KernelWriteString("  A name already registered was accepted a second time.\n");
        succeeded = false;
    }

    /*
     * A writable device without a writer, and a read-only device with one, are
     * both refused: either would be a device whose declared nature and whose
     * behaviour disagree.
     */
    if ((BlockRegister("mem2", &KernelMemoryDeviceReadOnlyOperations, NULL, BLOCK_SIZE_DEFAULT,
                       1U, false) != NULL) ||
        (BlockRegister("mem3", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 1U,
                       true) != NULL))
    {
        KernelWriteString("  A device was registered whose nature and operations disagree.\n");
        succeeded = false;
    }

    /* A degenerate geometry, and a name that cannot be held, are refused. */
    if ((BlockRegister("mem4", &KernelMemoryDeviceOperations, NULL, 0U, 1U, false) != NULL) ||
        (BlockRegister("mem5", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 0U,
                       false) != NULL) ||
        (BlockRegister("a-name-far-too-long-to-hold", &KernelMemoryDeviceOperations, NULL,
                       BLOCK_SIZE_DEFAULT, 1U, false) != NULL))
    {
        KernelWriteString("  A degenerate registration was accepted.\n");
        succeeded = false;
    }

    if ((BlockFindByName("mem0") != device) || (BlockFindByName("mem") != NULL) ||
        (BlockFindByName("mem00") != NULL))
    {
        KernelWriteString("  A device was found by a name that is not its own.\n");
        succeeded = false;
    }

    if (BlockDeviceCount() != (already_registered + 2U))
    {
        KernelWriteString("  The registry holds a different number of devices than "
                          "were registered.\n");
        succeeded = false;
    }

    if (BlockDeviceAt(BlockDeviceCount()) != NULL)
    {
        KernelWriteString("  A device was reported beyond the end of the registry.\n");
        succeeded = false;
    }

    /* What is written to a block must be what is read back from it. */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x11U);

    if (!BlockWrite(device, 3U, 1U, KernelBlockBufferA) ||
        !BlockRead(device, 3U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x11U))
    {
        KernelWriteString("  A block did not read back as it was written.\n");
        succeeded = false;
    }

    /*
     * A two-block transfer must carry both blocks and must not carry a third.
     * The two halves are given different patterns so that a layer which passed
     * the same block twice, or which lost the count, cannot pass this.
     */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x22U);
    KernelFillPattern(&KernelBlockBufferA[BLOCK_SIZE_DEFAULT], BLOCK_SIZE_DEFAULT, 0x33U);

    if (!BlockWrite(device, 8U, 2U, KernelBlockBufferA) ||
        !BlockRead(device, 8U, 2U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x22U) ||
        !KernelPatternMatches(&KernelBlockBufferB[BLOCK_SIZE_DEFAULT], BLOCK_SIZE_DEFAULT,
                              0x33U))
    {
        KernelWriteString("  A two-block transfer did not carry both blocks in order.\n");
        succeeded = false;
    }

    if (!BlockRead(device, 9U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x33U))
    {
        KernelWriteString("  The second block of a two-block write is not the block "
                          "that follows.\n");
        succeeded = false;
    }

    /* A range outside the device is refused, and so is one that would wrap. */
    if (BlockRead(device, KERNEL_MEMORY_DEVICE_BLOCKS, 1U, KernelBlockBufferB) ||
        BlockRead(device, KERNEL_MEMORY_DEVICE_BLOCKS - 1U, 2U, KernelBlockBufferB) ||
        BlockRead(device, UINT64_MAX, 2U, KernelBlockBufferB))
    {
        KernelWriteString("  A range outside the device was accepted.\n");
        succeeded = false;
    }

    /* A request without a buffer is refused; one for no blocks is harmless. */
    if (BlockRead(device, 0U, 1U, NULL) || BlockWrite(device, 0U, 1U, NULL) ||
        !BlockRead(device, 0U, 0U, NULL))
    {
        KernelWriteString("  A degenerate request was mishandled.\n");
        succeeded = false;
    }

    /* A read-only device refuses a write before the driver is reached. */
    if (BlockWrite(read_only, 0U, 1U, KernelBlockBufferA))
    {
        KernelWriteString("  A read-only device accepted a write.\n");
        succeeded = false;
    }

    if (!BlockRead(read_only, 3U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x11U))
    {
        KernelWriteString("  A read-only device did not read.\n");
        succeeded = false;
    }

    /* The accounting must reflect the blocks that actually moved. */
    if ((device->blocks_written != 3U) || (device->blocks_read != 4U))
    {
        KernelWriteString("  The accounting does not match the transfers performed.\n");
        succeeded = false;
    }

    /* A device may be withdrawn, and is then neither found nor addressable. */
    if (!BlockUnregister(read_only) || !BlockUnregister(device))
    {
        KernelWriteString("  A registered device could not be withdrawn.\n");
        succeeded = false;
    }

    if (BlockUnregister(device) || (BlockFindByName("mem0") != NULL) ||
        BlockRead(device, 0U, 1U, KernelBlockBufferB) ||
        (BlockDeviceCount() != already_registered))
    {
        KernelWriteString("  A withdrawn device was still reachable.\n");
        succeeded = false;
    }

    if (BlockTotalErrors() != 0U)
    {
        KernelWriteString("  A device reported an error where none was expected.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Block self-test passed.\n" : "Block self-test FAILED.\n");
}

/* The buffers held at once by the assertion that every buffer may be held. */
static Buffer *KernelHeldBuffers[BUFFER_CAPACITY];

/*
 * Asserts that the cache returns the block that was asked for, that a block held
 * is not read again, that a modified block reaches its device, and that a buffer
 * somebody is using is never taken from them.
 *
 * A cache is a thing that lies about where data came from, and every one of its
 * failures is silent by construction. A lookup that matched the wrong device
 * returns a block; an eviction that discarded a dirty buffer reports success and
 * loses a write; a buffer handed to two callers at once corrupts whichever of
 * them writes second, at a place unrelated to the defect. The assertions below
 * are chosen so that each of those produces a failure here instead.
 *
 * They are made against the device of memory, for the reasons given where it is
 * defined: this must be assertable upon a machine with no disk, and must not
 * write to a machine that has one.
 */
static void KernelVerifyBuffer(void)
{
    BlockDevice *device;
    Buffer *first;
    Buffer *second;
    uint64_t reads;
    uint64_t writes;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    bool succeeded = true;

    if (BufferCount() == 0U)
    {
        KernelWriteString("  The cache has no buffers; its storage was not allocated.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        return;
    }

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        return;
    }

    /* Blocks 5, 6 and 7 are given contents the assertions below can name. */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x55U);
    (void)BlockWrite(device, 5U, 1U, KernelBlockBufferA);
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x66U);
    (void)BlockWrite(device, 6U, 1U, KernelBlockBufferA);
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x77U);
    (void)BlockWrite(device, 7U, 1U, KernelBlockBufferA);

    /* A block not held is read from the device, and read correctly. */
    reads = device->blocks_read;
    first = BufferGet(device, 5U);

    if ((first == NULL) || (device->blocks_read != (reads + 1U)) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x55U) ||
        (first->block != 5U) || (first->device != device))
    {
        KernelWriteString("  A block was not fetched from the device correctly.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
        return;
    }

    BufferRelease(first);

    /* The same block is then found in the cache, and the device is not touched. */
    hits = BufferHits();
    reads = device->blocks_read;
    second = BufferGet(device, 5U);

    if ((second != first) || (BufferHits() != (hits + 1U)) || (device->blocks_read != reads))
    {
        KernelWriteString("  A block already held was fetched from the device again.\n");
        succeeded = false;
    }

    BufferRelease(second);

    /* Two different blocks occupy two different buffers. */
    first = BufferGet(device, 6U);
    second = BufferGet(device, 7U);

    if ((first == NULL) || (second == NULL) || (first == second) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x66U) ||
        !KernelPatternMatches(second->data, BLOCK_SIZE_DEFAULT, 0x77U))
    {
        KernelWriteString("  Two blocks were confused for one another.\n");
        succeeded = false;
    }

    BufferRelease(second);

    /*
     * A modified block does not reach the device until it is written back. That
     * deferral is the whole difference between this cache and none at all, so it
     * is asserted directly: the device must still hold the old contents.
     */
    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0x88U);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    writes = device->blocks_written;

    if ((device->blocks_written != writes) ||
        !BlockRead(device, 6U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x66U))
    {
        KernelWriteString("  A modified block reached the device before it was written "
                          "back.\n");
        succeeded = false;
    }

    if (BufferDirtyCount() == 0U)
    {
        KernelWriteString("  A modified block was not recorded as dirty.\n");
        succeeded = false;
    }

    /* Synchronising writes it back, and the device then holds the new contents. */
    if (!BufferSync() || (device->blocks_written != (writes + 1U)) ||
        !BlockRead(device, 6U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x88U) ||
        (BufferDirtyCount() != 0U))
    {
        KernelWriteString("  A modified block did not reach the device upon "
                          "synchronisation.\n");
        succeeded = false;
    }

    /*
     * A dirty block evicted under pressure must be written back as it goes. The
     * failure this catches is the one that loses data: an eviction that dropped
     * the contents would report nothing and be discovered only by a later read.
     */
    first = BufferGet(device, 7U);

    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0x99U);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    evictions = BufferEvictions();

    for (uint64_t block = 16U; block < (16U + (uint64_t)BUFFER_CAPACITY); ++block)
    {
        Buffer *const transient = BufferGet(device, block);

        BufferRelease(transient);
    }

    if ((BufferEvictions() <= evictions) || !BlockRead(device, 7U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x99U))
    {
        KernelWriteString("  A dirty block was evicted without being written back.\n");
        succeeded = false;
    }

    /* Having been evicted, the block is fetched from the device once more. */
    misses = BufferMisses();
    reads = device->blocks_read;
    first = BufferGet(device, 5U);

    if ((first == NULL) || (BufferMisses() != (misses + 1U)) ||
        (device->blocks_read != (reads + 1U)))
    {
        KernelWriteString("  An evicted block was reported as still held.\n");
        succeeded = false;
    }

    /*
     * A buffer a caller is holding is passed over by the eviction, however long
     * it has been there. The reference above is deliberately not released.
     */
    for (uint64_t block = 128U; block < (128U + (uint64_t)BUFFER_CAPACITY); ++block)
    {
        Buffer *const transient = BufferGet(device, block);

        BufferRelease(transient);
    }

    hits = BufferHits();
    second = BufferGet(device, 5U);

    if ((second != first) || (BufferHits() != (hits + 1U)) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x55U))
    {
        KernelWriteString("  A buffer being held by a caller was evicted beneath them.\n");
        succeeded = false;
    }

    BufferRelease(second);
    BufferRelease(first);

    /*
     * With every buffer held, a further request is refused rather than served by
     * evicting one of them. Handing out storage twice is the failure this
     * prevents, and it would appear as corruption somewhere else entirely.
     */
    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        KernelHeldBuffers[index] = BufferGet(device, (uint64_t)index);

        if (KernelHeldBuffers[index] == NULL)
        {
            KernelWriteString("  A buffer could not be held while others were.\n");
            succeeded = false;
            break;
        }
    }

    if (BufferHeldCount() != BUFFER_CAPACITY)
    {
        KernelWriteString("  The cache does not agree upon how many buffers are held.\n");
        succeeded = false;
    }

    if (BufferGet(device, (uint64_t)BUFFER_CAPACITY + 1U) != NULL)
    {
        KernelWriteString("  A buffer was issued when every one of them was held.\n");
        succeeded = false;
    }

    /* Nor may a device be discarded while its buffers are held. */
    if (BufferInvalidateDevice(device))
    {
        KernelWriteString("  A device was invalidated while its buffers were held.\n");
        succeeded = false;
    }

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        BufferRelease(KernelHeldBuffers[index]);
        KernelHeldBuffers[index] = NULL;
    }

    if (BufferHeldCount() != 0U)
    {
        KernelWriteString("  A buffer remained held after being released.\n");
        succeeded = false;
    }

    /*
     * Invalidation writes back what is dirty and then discards everything of the
     * device, which is what makes it safe to withdraw the device afterwards.
     */
    first = BufferGet(device, 4U);

    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0xAAU);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    if (!BufferInvalidateDevice(device) || (BufferValidCount() != 0U) ||
        !BlockRead(device, 4U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0xAAU))
    {
        KernelWriteString("  Invalidation did not write back and discard the device.\n");
        succeeded = false;
    }

    misses = BufferMisses();
    first = BufferGet(device, 5U);

    if ((first == NULL) || (BufferMisses() != (misses + 1U)))
    {
        KernelWriteString("  A block survived the invalidation of its device.\n");
        succeeded = false;
    }

    BufferRelease(first);

    /*
     * Nothing beneath the cache failed. Every transfer the test performed was of
     * a block the device holds, so a failure here means the cache asked for one
     * it should not have.
     */
    if (BufferFailures() != 0U)
    {
        KernelWriteString("  A transfer beneath the cache failed.\n");
        succeeded = false;
    }

    /* The device is discarded and withdrawn in that order, as it must be. */
    (void)BufferInvalidateDevice(device);
    (void)BlockUnregister(device);

    KernelWriteString(succeeded ? "Buffer self-test passed.\n" : "Buffer self-test FAILED.\n");
}

/*
 * Reports the root directory of a volume and the blocks it occupies.
 *
 * The root is inode 2 upon every EXT2 volume, so it is the one inode that can be
 * named without reading a directory first, and it is therefore the corroboration
 * available at every boot: a volume this kernel did not compose, whose root
 * inode and block list may be compared against what made the volume.
 *
 * A directory of any size would fill the log, so the list is a prefix and not
 * the whole of it. The prefix is followed by the block standing at the first
 * index of each indirect range the directory is long enough to reach, named by
 * its index. Those are the resolutions worth reporting: a prefix alone shows
 * only that the direct pointers were copied out of the inode, whereas the block
 * at index 12, at 12 + pointers-per-block, and at 12 + pointers + pointers
 * squared can each be reached only by following one, two or three blocks of
 * pointers, and each can be compared against what made the volume.
 */
/* The regular file within the subdirectory: two blocks, the second partly
 * filled, so that a read crossing a block boundary and a read ending within a
 * block are both exercised. */
#define KERNEL_VOLUME_INNER_SIZE 1500U

/*
 * The byte a composed file holds at an offset within itself.
 *
 * The value depends upon the offset, so a read that returned the right number of
 * bytes from the wrong place fails: a constant fill, or a pattern repeating
 * every block, would be returned identically by a reader that resolved the wrong
 * block, and the test would pass upon a defect it exists to catch.
 */
static uint8_t KernelFileByteAt(uint64_t offset)
{
    return (uint8_t)(((offset * 31U) + 7U) & 0xFFU);
}

/*
 * The buffer a file is read into or written from.
 *
 * It is static rather than automatic because it is larger than a page and the
 * kernel stack, though 64 KiB, is shared with every interrupt taken while this
 * runs. Nothing here is concurrent, so one buffer suffices.
 */
static uint8_t KernelFileBuffer[KERNEL_VOLUME_INNER_SIZE + 64U];

#define KERNEL_REPORTED_BLOCKS 13U

/* The path resolved upon every volume the machine carries, as a demonstration
 * that resolution works upon a volume this kernel did not compose. */
#define KERNEL_PROBE_PATH "/lost+found"

/* How many bytes of a regular file the probe reports. */
#define KERNEL_REPORTED_BYTES 16U

static void KernelReportBlockAt(BlockDevice *device, const Ext2Superblock *superblock,
                                const Ext2Inode *inode, uint64_t index)
{
    uint32_t block;

    KernelWriteString(" [");
    KernelWriteDecimal(index);
    KernelWriteString("]=");

    if (!Ext2InodeBlock(device, superblock, inode, index, &block))
    {
        KernelWriteString("refused");
        return;
    }

    KernelWriteDecimal((uint64_t)block);
}

static void KernelReportRootInode(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t pointers = superblock->block_size / EXT2_BLOCK_POINTER_SIZE;
    const uint64_t indirect = EXT2_DIRECT_BLOCK_COUNT;
    const uint64_t doubly = indirect + pointers;
    const uint64_t triply = doubly + (pointers * pointers);
    Ext2Inode root;
    Ext2Inode probe;
    uint64_t count;

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("EXT2: the root inode could not be read: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    Ext2ReportInode(&root);

    count = Ext2InodeBlockCount(superblock, &root);
    KernelWriteString("EXT2 root blocks:");

    for (uint64_t index = 0U; (index < count) && (index < KERNEL_REPORTED_BLOCKS); ++index)
    {
        uint32_t block;

        if (!Ext2InodeBlock(device, superblock, &root, index, &block))
        {
            KernelWriteString(" (refused: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString(")");
            break;
        }

        KernelWriteString(" ");
        KernelWriteDecimal((uint64_t)block);
    }

    if (count > KERNEL_REPORTED_BLOCKS)
    {
        KernelWriteString(" ...");
    }

    /* The first block of each indirect range the directory reaches. */
    if (count > doubly)
    {
        KernelReportBlockAt(device, superblock, &root, doubly);
    }

    if (count > triply)
    {
        KernelReportBlockAt(device, superblock, &root, triply);
    }

    KernelWriteString("\n");

    /*
     * The root directory of the volume, listed. This is the first report the
     * kernel makes that names anything a person would recognise, and it is the
     * only assertion available upon a real volume: the self-test's composed
     * directory is by construction the directory the traversal expects, whereas
     * the names below were written by mke2fs and by whoever used the disk.
     */
    Ext2ReportDirectory(device, superblock, &root);

    /*
     * One path of the volume, resolved. Every volume `mke2fs` creates holds a
     * lost+found directory in its root, so the probe is a name this kernel may
     * look for upon a volume it knows nothing else about; a volume that does not
     * hold it reports the refusal, which is itself the correct answer.
     */
    if (Ext2ResolvePathNoFollow(device, superblock, KERNEL_PROBE_PATH, &probe))
    {
        KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " resolves to inode ");
        KernelWriteDecimal((uint64_t)probe.number);
        KernelWriteString(", ");
        KernelWriteString(Ext2FileTypeName(Ext2FileTypeOfMode(probe.mode)));
        KernelWriteString(" of ");
        KernelWriteDecimal(probe.size);
        KernelWriteString(" bytes.\n");

        /*
         * What the probe holds, which is the one exercise of the reading of
         * Section 5.5 upon a volume this kernel did not compose. The path is
         * resolved without following a last link, so that a link reports itself
         * and its target rather than silently reporting what it names.
         */
        if (Ext2InodeIsSymbolicLink(&probe))
        {
            char target[EXT2_SYMLINK_MAXIMUM + 1U];

            if (Ext2ReadSymbolicLink(device, superblock, &probe, target, sizeof target))
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " is a ");
                KernelWriteString(Ext2InodeIsFastSymbolicLink(superblock, &probe)
                                      ? "target held within its inode: "
                                      : "target held in a block: ");
                KernelWriteString(target);
                KernelWriteString("\n");
            }
            else
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " has no readable target: ");
                KernelWriteString(Ext2LastError());
                KernelWriteString("\n");
            }
        }
        else if (Ext2InodeIsRegular(&probe))
        {
            uint8_t head[KERNEL_REPORTED_BYTES];
            uint64_t read = 0U;

            if (Ext2ReadFile(device, superblock, &probe, 0U, head, sizeof head, &read))
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " begins:");

                for (uint64_t index = 0U; index < read; ++index)
                {
                    KernelWriteString(" ");
                    KernelWriteHexadecimal((uint64_t)head[index]);
                }

                KernelWriteString(" (");
                KernelWriteDecimal(read);
                KernelWriteString(" bytes read)\n");
            }
            else
            {
                KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " could not be read: ");
                KernelWriteString(Ext2LastError());
                KernelWriteString("\n");
            }
        }
    }
    else
    {
        KernelWriteString("EXT2 path " KERNEL_PROBE_PATH " does not resolve: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
    }
}

/*
 * Writes to a volume the machine actually carries, and only when the operator
 * has asked for it at the boot menu.
 *
 * Every other exercise of the writing of sub-task 5.6 is performed upon the
 * device of memory, whose contents this kernel composed and owns. A volume upon
 * a disk belongs to whoever booted this kernel, and a self-test that altered one
 * unbidden would destroy their data to prove a point about its own correctness.
 *
 * Two further precautions bound what this can damage even when it is asked for.
 * It writes only to a file named KERNEL_WRITE_PROBE_PATH, which nothing but a
 * deliberate preparation for this test would have created, and it refuses to
 * proceed unless that file is a regular file the volume already holds — it
 * creates nothing, and it touches nothing it was not pointed at. The file is
 * left holding what this writes, so the operator may compare it from outside.
 */
#define KERNEL_WRITE_PROBE_PATH "/oxys-write-test"
#define KERNEL_WRITE_PROBE_SIZE 8192U

static void KernelWriteProbeVolume(BlockDevice *device, Ext2Superblock *superblock)
{
    Ext2Inode probe;
    Ext2Inode parent;
    Ext2Inode made;
    Ext2Inode within;
    uint64_t moved = 0U;
    uint64_t index;

    if (!KernelCommandLineHasOption("ext2-write-test"))
    {
        return;
    }

    KernelWriteString("EXT2 write test: the command line permits writing to ");
    KernelWriteString(device->name);
    KernelWriteString(".\n");

    if (superblock->read_only)
    {
        KernelWriteString("EXT2 write test: the volume is read-only; nothing written.\n");
        return;
    }

    if (!Ext2ResolvePath(device, superblock, KERNEL_WRITE_PROBE_PATH, &probe))
    {
        KernelWriteString("EXT2 write test: " KERNEL_WRITE_PROBE_PATH " is not present; "
                          "nothing written.\n");
        return;
    }

    if (!Ext2InodeIsRegular(&probe))
    {
        KernelWriteString("EXT2 write test: " KERNEL_WRITE_PROBE_PATH " is not a regular "
                          "file; nothing written.\n");
        return;
    }

    /*
     * The file is emptied and then written afresh, so that the allocation, the
     * freeing and the extension are all exercised upon a real volume. The
     * contents are derived from the offset, so that a file written from the
     * wrong place is distinguishable from one written correctly when it is
     * examined from outside.
     */
    if (!Ext2TruncateFile(device, superblock, &probe, 0U))
    {
        KernelWriteString("EXT2 write test: the file could not be emptied: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    for (index = 0U; index < KERNEL_WRITE_PROBE_SIZE; index += sizeof KernelFileBuffer)
    {
        uint64_t run = KERNEL_WRITE_PROBE_SIZE - index;

        if (run > sizeof KernelFileBuffer)
        {
            run = sizeof KernelFileBuffer;
        }

        for (uint64_t offset = 0U; offset < run; ++offset)
        {
            KernelFileBuffer[offset] = KernelFileByteAt(index + offset);
        }

        if (!Ext2WriteFile(device, superblock, &probe, index, KernelFileBuffer, run, &moved) ||
            (moved != run))
        {
            KernelWriteString("EXT2 write test: the file could not be written: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return;
        }
    }

    /*
     * The cache is written back before anything is reported. Until it is, the
     * volume upon the disk holds none of this, and a report of success would
     * describe memory rather than the medium.
     */
    if (!BufferSync())
    {
        KernelWriteString("EXT2 write test: the cache could not be written back.\n");
        return;
    }

    /*
     * The names of sub-task 5.7, made and unmade within a directory of this
     * kernel's own creation. Everything here is removed again before the report,
     * so a volume that held /oxys-write-test before this ran holds exactly the
     * same set of names afterwards, with that one file rewritten. What is left
     * for e2fsck to judge is therefore the accounting rather than the tree.
     */
    if (!Ext2ResolvePath(device, superblock, "/", &parent))
    {
        KernelWriteString("EXT2 write test: the root could not be read.\n");
        return;
    }

    if (!Ext2CreateDirectory(device, superblock, &parent, "oxys-made", 9U, 0x01EDU, &made))
    {
        KernelWriteString("EXT2 write test: a directory could not be created: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    if (!Ext2CreateFile(device, superblock, &made, "within", 6U,
                        (uint16_t)(EXT2_S_IFREG | 0x01A4U), &within) ||
        !Ext2WriteFile(device, superblock, &within, 0U, KernelFileBuffer, 64U, &moved) ||
        (moved != 64U))
    {
        KernelWriteString("EXT2 write test: a file could not be created within it: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("EXT2 write test: created /oxys-made (inode ");
    KernelWriteDecimal((uint64_t)made.number);
    KernelWriteString(") holding within (inode ");
    KernelWriteDecimal((uint64_t)within.number);
    KernelWriteString(").\n");

    if (!Ext2Unlink(device, superblock, &made, "within", 6U) ||
        !Ext2RemoveDirectory(device, superblock, &parent, "oxys-made", 9U))
    {
        KernelWriteString("EXT2 write test: what was created could not be removed: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("EXT2 write test: removed both again.\n");

    KernelWriteString("EXT2 write test: wrote ");
    KernelWriteDecimal(probe.size);
    KernelWriteString(" bytes to " KERNEL_WRITE_PROBE_PATH " (inode ");
    KernelWriteDecimal((uint64_t)probe.number);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)probe.sector_count);
    KernelWriteString(" sectors); volume now reports ");
    KernelWriteDecimal((uint64_t)superblock->free_block_count);
    KernelWriteString(" free blocks and ");
    KernelWriteDecimal((uint64_t)superblock->free_inode_count);
    KernelWriteString(" free inodes.\n");
}

/*
 * Reads and reports the superblock of every block device the machine carries.
 *
 * Nothing is mounted and nothing is retained. The purpose is that a volume the
 * machine actually holds is put through the parser at every boot, since the
 * self-test's composed volume is by construction the volume the parser expects.
 */
static void KernelReportVolumes(void)
{
    const size_t count = BlockDeviceCount();

    if (count == 0U)
    {
        KernelWriteString("EXT2: no block device to examine.\n");
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        BlockDevice *const device = BlockDeviceAt(index);
        Ext2Superblock superblock;

        if (device == NULL)
        {
            break;
        }

        if (Ext2ReadSuperblock(device, &superblock))
        {
            Ext2GroupDescriptor descriptor;

            Ext2ReportVolume(&superblock, device->name);

            /*
             * The first group's descriptor, and the verification of the whole
             * table. Both are reported because the table is the structure every
             * later part of the filesystem is found through, and a volume whose
             * table this kernel refuses is one it could not mount.
             */
            if (Ext2ReadGroupDescriptor(device, &superblock, 0U, &descriptor))
            {
                Ext2ReportGroup(&descriptor);
            }

            if (!Ext2VerifyGroupDescriptors(device, &superblock))
            {
                KernelWriteString("EXT2: the descriptor table of ");
                KernelWriteString(device->name);
                KernelWriteString(" is not trustworthy: ");
                KernelWriteString(Ext2LastError());
                KernelWriteString("\n");
            }

            KernelReportRootInode(device, &superblock);
            KernelWriteProbeVolume(device, &superblock);
        }
        else
        {
            KernelWriteString("EXT2: ");
            KernelWriteString(device->name);
            KernelWriteString(" holds no volume this kernel can read: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
        }
    }
}

/*
 * The composition of an EXT2 superblock within the device of memory, so that
 * the parser may be asserted against a volume whose every field is known.
 *
 * No machine this kernel is verified upon carries an EXT2 volume, and one that
 * did would carry somebody's data. The superblock is therefore built here, field
 * by field, from the offsets the format defines — the same names the parser
 * reads, so that a mistaken offset cannot agree with itself.
 */
static void KernelStoreHalf(size_t offset, uint16_t value)
{
    uint8_t *const field = &KernelMemoryDeviceStore[offset];

    field[0] = (uint8_t)(value & 0xFFU);
    field[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void KernelStoreWord(size_t offset, uint32_t value)
{
    uint8_t *const field = &KernelMemoryDeviceStore[offset];

    field[0] = (uint8_t)(value & 0xFFU);
    field[1] = (uint8_t)((value >> 8) & 0xFFU);
    field[2] = (uint8_t)((value >> 16) & 0xFFU);
    field[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/* The same, addressed by a field's offset within the superblock. */
static void KernelSetVolumeHalf(size_t offset, uint16_t value)
{
    KernelStoreHalf(EXT2_SUPERBLOCK_OFFSET + offset, value);
}

static void KernelSetVolumeWord(size_t offset, uint32_t value)
{
    KernelStoreWord(EXT2_SUPERBLOCK_OFFSET + offset, value);
}

/*
 * The layout of the composed volume, in blocks of 1024 bytes.
 *
 * Block 0 is the boot block, block 1 holds the superblock, and the group
 * descriptor table follows it. The remainder is laid out as mke2fs would lay it
 * out: the two bitmaps, then the inode table, then the data blocks. The volume
 * is 128 blocks and the device of memory is 256 blocks of 512 bytes, so the two
 * are exactly the same length.
 */
#define KERNEL_VOLUME_BLOCK_SIZE       1024U
#define KERNEL_VOLUME_DESCRIPTOR_BLOCK 2U
#define KERNEL_VOLUME_BLOCK_BITMAP     3U
#define KERNEL_VOLUME_INODE_BITMAP     4U
#define KERNEL_VOLUME_INODE_TABLE      5U
/*
 * The free counts, which must agree with the bitmaps composed below and with one
 * another. Until sub-task 5.6 the bitmaps were zeroes and the counts were
 * whatever the composition said, nothing having read a bitmap; an allocator
 * reads them, so they now describe the same volume or they describe none.
 *
 * The group holds 127 blocks — 128 less the boot block, the volume's first data
 * block being 1 — of which blocks 1 to 33 are the metadata and the composed
 * files. Sixteen inodes, of which every one but 14 is in use; 14 is the inode
 * the self-test of sub-task 5.3 requires to be empty, and is now also the only
 * one an allocation can be given.
 */
/*
 * The inodes the volume holds. Thirty-two rather than sixteen since sub-task
 * 5.7, which creates files and directories and therefore needs inodes to create
 * them with; the table accordingly occupies four blocks rather than two, and the
 * data blocks begin two blocks later than they did.
 */
#define KERNEL_VOLUME_INODES           32U
#define KERNEL_VOLUME_USED_INODES      15U

#define KERNEL_VOLUME_GROUP_BLOCKS     127U
#define KERNEL_VOLUME_USED_BLOCKS      (KERNEL_VOLUME_LAST_BLOCK - 1U)
#define KERNEL_VOLUME_FREE_BLOCKS      (KERNEL_VOLUME_GROUP_BLOCKS - KERNEL_VOLUME_USED_BLOCKS)
#define KERNEL_VOLUME_FREE_INODES      (KERNEL_VOLUME_INODES - KERNEL_VOLUME_USED_INODES)
#define KERNEL_VOLUME_DIRECTORIES      2U

/* The byte at which a block of the composed volume begins. */
static size_t KernelVolumeBlock(uint32_t block)
{
    return (size_t)block * KERNEL_VOLUME_BLOCK_SIZE;
}

/* A field of the group descriptor of group 0, which lies first in the table. */
static size_t KernelDescriptorField(size_t offset)
{
    return KernelVolumeBlock(KERNEL_VOLUME_DESCRIPTOR_BLOCK) + offset;
}

/*
 * The blocks of the composed file, chosen so that every level of the
 * indirection is exercised and every one of them lies within the 128 blocks the
 * volume holds. Blocks 7 to 18 are the twelve direct blocks; the rest are the
 * pointer blocks and the data blocks they lead to.
 */
#define KERNEL_VOLUME_FILE_INODE      11U
#define KERNEL_VOLUME_DIRECT_FIRST    9U
#define KERNEL_VOLUME_INDIRECT        21U
#define KERNEL_VOLUME_INDIRECT_DATA   22U
#define KERNEL_VOLUME_INDIRECT_LAST   23U
#define KERNEL_VOLUME_DOUBLE          24U
#define KERNEL_VOLUME_DOUBLE_LEVEL    25U
#define KERNEL_VOLUME_DOUBLE_DATA     26U
#define KERNEL_VOLUME_TRIPLE          27U
#define KERNEL_VOLUME_TRIPLE_DOUBLE   28U
#define KERNEL_VOLUME_TRIPLE_INDIRECT 29U
#define KERNEL_VOLUME_TRIPLE_DATA     30U
#define KERNEL_VOLUME_ROOT_DATA       31U
#define KERNEL_VOLUME_SUB_DATA        32U
#define KERNEL_VOLUME_INNER_DATA      33U
#define KERNEL_VOLUME_INNER_DATA_LAST 34U
#define KERNEL_VOLUME_LINK_DATA       35U
#define KERNEL_VOLUME_LAST_BLOCK      36U

/*
 * The two inodes the directories lead to besides the file: a subdirectory of the
 * root, and a regular file within it. They exist so that a path of more than one
 * component may be resolved, which is the whole of what distinguishes resolution
 * from a single lookup.
 */
#define KERNEL_VOLUME_SUB_INODE   12U
#define KERNEL_VOLUME_INNER_INODE 13U

/*
 * An inode of the table that is deliberately left empty, so that a self-test may
 * assert that an entry never filled is refused. It is named rather than reached
 * by arithmetic upon the inode before it: the inodes in use grow as the composed
 * volume acquires structure, and a test that took the next number after some
 * other inode would begin asserting the wrong thing without saying so.
 */
#define KERNEL_VOLUME_UNUSED_INODE 14U

/*
 * The two symbolic links, one of each form. A target shorter than sixty bytes is
 * held within the inode, in the fields that would otherwise be block pointers;
 * a longer one is held in a block like any other file. Both forms are composed
 * because they are read by entirely different code and a volume carrying only
 * the common one would leave half of that code unexercised.
 */
#define KERNEL_VOLUME_FAST_LINK_INODE 15U
#define KERNEL_VOLUME_SLOW_LINK_INODE 16U

/* What each names. The fast target is relative and is resolved against the
 * directory holding the link; the slow one is absolute and long enough to
 * require a block, which is what makes it slow. */
#define KERNEL_VOLUME_FAST_LINK_TARGET "sub"
#define KERNEL_VOLUME_SLOW_LINK_TARGET \
    "/sub/../sub/../sub/../sub/../sub/../sub/../sub/../sub/../sub/inner"


/* How many pointers a block of the composed volume holds: 1024 / 4. */
#define KERNEL_VOLUME_POINTERS 256U

/* The size given to the composed file, which is not what the resolver uses. */
#define KERNEL_VOLUME_FILE_SIZE 274432U

/* A field of an inode, the table beginning at KERNEL_VOLUME_INODE_TABLE. */
static size_t KernelInodeField(uint32_t number, size_t offset)
{
    return KernelVolumeBlock(KERNEL_VOLUME_INODE_TABLE) +
           ((size_t)(number - 1U) * EXT2_GOOD_OLD_INODE_SIZE) + offset;
}

/* One entry of a block of pointers. */
static size_t KernelPointerField(uint32_t block, uint32_t entry)
{
    return KernelVolumeBlock(block) + ((size_t)entry * EXT2_BLOCK_POINTER_SIZE);
}

/* One of the fifteen block pointers of an inode. */
static size_t KernelInodeBlockField(uint32_t number, uint32_t entry)
{
    return KernelInodeField(number, EXT2_OFFSET_I_BLOCK + ((size_t)entry *
                                                           EXT2_BLOCK_POINTER_SIZE));
}

/*
 * Composes an inode table and the blocks of pointers one of its inodes leads to.
 *
 * Inode 2 is the root directory, as the format reserves it, with a single direct
 * block. Inode 11 is a regular file whose fifteen pointers reach every level of
 * the indirection: twelve direct blocks, an indirect block whose first and last
 * entries are used and whose second is a hole, a doubly indirect block, and a
 * triply indirect block. The holes are deliberate — a sparse file is the usual
 * case and not a curiosity, and a resolver that mistook a hole for the end of
 * the file would be wrong upon most of the files a system holds.
 */
static void KernelComposeInodes(void)
{
    for (size_t index = KernelVolumeBlock(KERNEL_VOLUME_INODE_TABLE);
         index < KernelVolumeBlock(KERNEL_VOLUME_LAST_BLOCK); ++index)
    {
        KernelMemoryDeviceStore[index] = 0U;
    }

    /* The root directory: one block, three links — its own entry, the entry it
     * holds for itself, and the parent entry of the one subdirectory. */
    KernelStoreHalf(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFDIR | 0x01EDU));
    KernelStoreWord(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_BLOCK_SIZE);
    KernelStoreHalf(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_LINKS_COUNT), 3U);
    KernelStoreWord(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_BLOCKS), 2U);
    KernelStoreWord(KernelInodeBlockField(EXT2_ROOT_INODE, 0U), KERNEL_VOLUME_ROOT_DATA);

    /* The subdirectory: one block, two links — its own entry and the root's. */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFDIR | 0x01EDU));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_BLOCK_SIZE);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_LINKS_COUNT), 2U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_BLOCKS), 2U);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_SUB_INODE, 0U), KERNEL_VOLUME_SUB_DATA);

    /* The regular file within the subdirectory. */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFREG | 0x01A4U));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_INNER_SIZE);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_LINKS_COUNT), 1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_BLOCKS), 4U);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_INNER_INODE, 0U),
                    KERNEL_VOLUME_INNER_DATA);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_INNER_INODE, 1U),
                    KERNEL_VOLUME_INNER_DATA_LAST);

    /*
     * The two symbolic links. The fast one declares no sectors, which is what
     * says its target is within the inode; the slow one declares the two sectors
     * of its single block, which is what says the target is not.
     */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFLNK | 0x01FFU));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_SIZE),
                    (uint32_t)(sizeof(KERNEL_VOLUME_FAST_LINK_TARGET) - 1U));
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_LINKS_COUNT),
                    1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCKS), 0U);

    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFLNK | 0x01FFU));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_SIZE),
                    (uint32_t)(sizeof(KERNEL_VOLUME_SLOW_LINK_TARGET) - 1U));
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_LINKS_COUNT),
                    1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_BLOCKS), 2U);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_SLOW_LINK_INODE, 0U),
                    KERNEL_VOLUME_LINK_DATA);

    /* The file. */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFREG | 0x01A4U));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_FILE_SIZE);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_LINKS_COUNT), 1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_BLOCKS), 32U);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_UID), 1000U);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_GID), 1001U);

    for (uint32_t entry = 0U; entry < EXT2_DIRECT_BLOCK_COUNT; ++entry)
    {
        KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, entry),
                        KERNEL_VOLUME_DIRECT_FIRST + entry);
    }

    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, EXT2_INDIRECT_INDEX),
                    KERNEL_VOLUME_INDIRECT);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, EXT2_DOUBLE_INDIRECT_INDEX),
                    KERNEL_VOLUME_DOUBLE);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, EXT2_TRIPLE_INDIRECT_INDEX),
                    KERNEL_VOLUME_TRIPLE);

    /* The indirect block: its first and last entries used, its second a hole. */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, 0U), KERNEL_VOLUME_INDIRECT_DATA);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, KERNEL_VOLUME_POINTERS - 1U),
                    KERNEL_VOLUME_INDIRECT_LAST);

    /* The doubly indirect block, and the indirect block beneath it. */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_DOUBLE, 0U), KERNEL_VOLUME_DOUBLE_LEVEL);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_DOUBLE_LEVEL, 5U),
                    KERNEL_VOLUME_DOUBLE_DATA);

    /* The triply indirect block, and the two levels beneath it. */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_TRIPLE, 0U), KERNEL_VOLUME_TRIPLE_DOUBLE);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_TRIPLE_DOUBLE, 0U),
                    KERNEL_VOLUME_TRIPLE_INDIRECT);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_TRIPLE_INDIRECT, 3U),
                    KERNEL_VOLUME_TRIPLE_DATA);
}

/* Writes a terminated string into the device's storage, without its terminator,
 * a target upon the volume being bounded by the file's size and not terminated. */
static void KernelStoreText(size_t offset, const char *text)
{
    for (size_t index = 0U; text[index] != '\0'; ++index)
    {
        KernelMemoryDeviceStore[offset + index] = (uint8_t)text[index];
    }
}

/*
 * Composes the contents of the files: the two blocks of the regular file within
 * the subdirectory, one block of the sparse file so that a block holding data
 * may be contrasted with the hole beside it, and the target of the symbolic link
 * too long to be held within its inode.
 *
 * The fast link's target is stored in the fifteen words of i_block, which is
 * where the pointers would otherwise be. It is written here as the bytes the
 * volume holds, in the volume's own order, so that the parser recovers it by the
 * decoding it applies to every other field rather than by agreement with this.
 */
static void KernelComposeFiles(void)
{
    const size_t indirect = KernelVolumeBlock(KERNEL_VOLUME_INDIRECT_DATA);

    /*
     * The file occupies two blocks, which are addressed through the pointers the
     * inode holds rather than by relying upon their being adjacent in the store.
     */
    for (uint64_t offset = 0U; offset < KERNEL_VOLUME_INNER_SIZE; ++offset)
    {
        const uint32_t block = (offset < KERNEL_VOLUME_BLOCK_SIZE)
                                   ? KERNEL_VOLUME_INNER_DATA
                                   : KERNEL_VOLUME_INNER_DATA_LAST;
        const size_t within = (size_t)(offset % KERNEL_VOLUME_BLOCK_SIZE);

        KernelMemoryDeviceStore[KernelVolumeBlock(block) + within] = KernelFileByteAt(offset);
    }

    /*
     * Block 12 of the sparse file, which is the first block its indirect block
     * names. Block 13 is a hole and is deliberately left as it is: the two lie
     * beside one another so that a reader returning zeroes for both, or data for
     * both, is caught.
     */
    for (uint32_t offset = 0U; offset < KERNEL_VOLUME_BLOCK_SIZE; ++offset)
    {
        KernelMemoryDeviceStore[indirect + offset] =
            KernelFileByteAt((uint64_t)EXT2_DIRECT_BLOCK_COUNT * KERNEL_VOLUME_BLOCK_SIZE +
                             offset);
    }

    KernelStoreText(KernelVolumeBlock(KERNEL_VOLUME_LINK_DATA), KERNEL_VOLUME_SLOW_LINK_TARGET);
    KernelStoreText(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCK),
                    KERNEL_VOLUME_FAST_LINK_TARGET);
}

/*
 * Composes the directory data of the volume: the root directory and the one
 * subdirectory beneath it.
 *
 * The entries are written from the same offset names the parser reads, for the
 * reason the superblock's fields are, and the block is laid out as Table 4.3 of
 * the specification lays out its sample: entries aligned upon four bytes, an
 * unused record left where a name was removed, and a final record whose length
 * runs to the end of the block rather than stopping after its name.
 *
 * Both of those last two are deliberate and neither is a curiosity. A traversal
 * that mistook the unused record for a name would report a file that does not
 * exist; one that stopped at the end of the last name rather than at the end of
 * the block would read the padding as a further entry.
 */
static size_t KernelComposeEntry(size_t position, uint32_t inode, uint16_t record_length,
                                 uint8_t file_type, const char *name)
{
    size_t length = 0U;

    while (name[length] != '\0')
    {
        ++length;
    }

    KernelStoreWord(position + EXT2_OFFSET_DE_INODE, inode);
    KernelStoreHalf(position + EXT2_OFFSET_DE_RECORD_LENGTH, record_length);
    KernelMemoryDeviceStore[position + EXT2_OFFSET_DE_NAME_LENGTH] = (uint8_t)length;
    KernelMemoryDeviceStore[position + EXT2_OFFSET_DE_FILE_TYPE] = file_type;

    for (size_t index = 0U; index < length; ++index)
    {
        KernelMemoryDeviceStore[position + EXT2_OFFSET_DE_NAME + index] = (uint8_t)name[index];
    }

    return position + record_length;
}

static void KernelComposeDirectories(void)
{
    const size_t root = KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA);
    const size_t sub = KernelVolumeBlock(KERNEL_VOLUME_SUB_DATA);
    size_t position;

    /*
     * The root directory. Its entries "." and ".." are ordinary entries upon the
     * volume and are composed as such: the resolver is meant to find them by
     * looking, not by knowing what they mean, and the ".." of the root names the
     * root itself because the root has no parent.
     */
    position = KernelComposeEntry(root, EXT2_ROOT_INODE, 12U, (uint8_t)EXT2_FT_DIR, ".");
    position = KernelComposeEntry(position, EXT2_ROOT_INODE, 12U, (uint8_t)EXT2_FT_DIR, "..");
    position = KernelComposeEntry(position, KERNEL_VOLUME_FILE_INODE, 16U,
                                  (uint8_t)EXT2_FT_REG_FILE, "file");

    /*
     * The record of a name that was removed. It retains the bytes of the name it
     * held, which is what a volume looks like when the first entry of a block is
     * removed: the record is marked unused by having its inode number set to
     * zero and nothing else about it is touched. A traversal that read the name
     * rather than the inode number would report a file that was deleted.
     */
    position = KernelComposeEntry(position, 0U, 16U, (uint8_t)EXT2_FT_UNKNOWN, "removed");

    position = KernelComposeEntry(position, KERNEL_VOLUME_SUB_INODE, 12U,
                                  (uint8_t)EXT2_FT_DIR, "sub");
    position = KernelComposeEntry(position, KERNEL_VOLUME_FAST_LINK_INODE, 20U,
                                  (uint8_t)EXT2_FT_SYMLINK, "link-fast");
    position = KernelComposeEntry(position, KERNEL_VOLUME_SLOW_LINK_INODE, 20U,
                                  (uint8_t)EXT2_FT_SYMLINK, "link-slow");

    /* The last record of the block runs to the end of it. */
    (void)KernelComposeEntry(position, 0U,
                             (uint16_t)((root + KERNEL_VOLUME_BLOCK_SIZE) - position),
                             (uint8_t)EXT2_FT_UNKNOWN, "");

    /* The subdirectory, whose ".." names the root. */
    position = KernelComposeEntry(sub, KERNEL_VOLUME_SUB_INODE, 12U, (uint8_t)EXT2_FT_DIR, ".");
    position = KernelComposeEntry(position, EXT2_ROOT_INODE, 12U, (uint8_t)EXT2_FT_DIR, "..");
    position = KernelComposeEntry(position, KERNEL_VOLUME_INNER_INODE, 16U,
                                  (uint8_t)EXT2_FT_REG_FILE, "inner");
    (void)KernelComposeEntry(position, 0U,
                             (uint16_t)((sub + KERNEL_VOLUME_BLOCK_SIZE) - position),
                             (uint8_t)EXT2_FT_UNKNOWN, "");
}

/*
 * Composes the two bitmaps.
 *
 * One bit stands for each block of the group and each inode of it, 1 meaning
 * used, the first of the group being bit 0 of byte 0 and the ninth bit 0 of
 * byte 1. The order is written out here as the parser writes it out, and for the
 * same reason: it is the format's and not the composer's opinion of it.
 *
 * Every block from the first data block to the last the composition uses is
 * marked, and every inode but 14. The counts stated in the superblock and the
 * descriptor are derived from the same constants, so a bitmap and a count that
 * disagreed would be a mistake in one place rather than a difference between two.
 */
static void KernelSetBitmapBit(uint32_t block, uint32_t index)
{
    KernelMemoryDeviceStore[KernelVolumeBlock(block) + (index / 8U)] |=
        (uint8_t)(1U << (index % 8U));
}

static void KernelComposeBitmaps(void)
{
    for (uint32_t offset = 0U; offset < KERNEL_VOLUME_BLOCK_SIZE; ++offset)
    {
        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_BLOCK_BITMAP) + offset] = 0U;
        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_INODE_BITMAP) + offset] = 0U;
    }

    /*
     * The blocks in use. Block 1 is the first data block, so it is bit 0; the
     * subtraction is the same one the allocator performs, and getting it wrong
     * in either place marks a block that is not the one meant.
     */
    for (uint32_t block = 1U; block < KERNEL_VOLUME_LAST_BLOCK; ++block)
    {
        KernelSetBitmapBit(KERNEL_VOLUME_BLOCK_BITMAP, block - 1U);
    }

    /*
     * The inodes in use: 1 to 16 but for 14, which is left free deliberately.
     * Everything above 16 is free and is what an allocation is given.
     */
    for (uint32_t number = 1U; number <= 16U; ++number)
    {
        if (number != KERNEL_VOLUME_UNUSED_INODE)
        {
            KernelSetBitmapBit(KERNEL_VOLUME_INODE_BITMAP, number - 1U);
        }
    }
}

/*
 * Composes the descriptor of the volume's single group.
 *
 * The free counts must agree with the superblock's, there being one group to
 * account for the whole volume; the verification of the table asserts exactly
 * that, so a self-test composing them inconsistently would fail upon its own
 * arithmetic rather than upon the parser's.
 */
static void KernelComposeGroupDescriptor(void)
{
    for (size_t index = 0U; index < KERNEL_VOLUME_BLOCK_SIZE; ++index)
    {
        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_DESCRIPTOR_BLOCK) + index] = 0U;
    }

    KernelStoreWord(KernelDescriptorField(EXT2_OFFSET_BG_BLOCK_BITMAP),
                    KERNEL_VOLUME_BLOCK_BITMAP);
    KernelStoreWord(KernelDescriptorField(EXT2_OFFSET_BG_INODE_BITMAP),
                    KERNEL_VOLUME_INODE_BITMAP);
    KernelStoreWord(KernelDescriptorField(EXT2_OFFSET_BG_INODE_TABLE),
                    KERNEL_VOLUME_INODE_TABLE);
    KernelStoreHalf(KernelDescriptorField(EXT2_OFFSET_BG_FREE_BLOCKS),
                    (uint16_t)KERNEL_VOLUME_FREE_BLOCKS);
    KernelStoreHalf(KernelDescriptorField(EXT2_OFFSET_BG_FREE_INODES),
                    (uint16_t)KERNEL_VOLUME_FREE_INODES);
    KernelStoreHalf(KernelDescriptorField(EXT2_OFFSET_BG_USED_DIRECTORIES),
                    (uint16_t)KERNEL_VOLUME_DIRECTORIES);
}

/*
 * Composes a volume that every rule of the parser accepts: revision 1, blocks of
 * 1024 bytes, one group, and the two features this kernel implements.
 */
static void KernelComposeVolume(void)
{
    static const char label[] = "oxys-test";

    for (size_t index = 0U; index < EXT2_SUPERBLOCK_SIZE; ++index)
    {
        KernelMemoryDeviceStore[EXT2_SUPERBLOCK_OFFSET + index] = 0U;
    }

    KernelSetVolumeWord(EXT2_OFFSET_INODE_COUNT, KERNEL_VOLUME_INODES);
    KernelSetVolumeWord(EXT2_OFFSET_BLOCK_COUNT, 128U);
    KernelSetVolumeWord(EXT2_OFFSET_RESERVED_BLOCKS, 6U);
    KernelSetVolumeWord(EXT2_OFFSET_FREE_BLOCKS, KERNEL_VOLUME_FREE_BLOCKS);
    KernelSetVolumeWord(EXT2_OFFSET_FREE_INODES, KERNEL_VOLUME_FREE_INODES);
    KernelSetVolumeWord(EXT2_OFFSET_FIRST_DATA_BLOCK, 1U);
    KernelSetVolumeWord(EXT2_OFFSET_LOG_BLOCK_SIZE, 0U);
    KernelSetVolumeWord(EXT2_OFFSET_LOG_FRAGMENT_SIZE, 0U);
    KernelSetVolumeWord(EXT2_OFFSET_BLOCKS_PER_GROUP, 8192U);
    KernelSetVolumeWord(EXT2_OFFSET_FRAGS_PER_GROUP, 8192U);
    KernelSetVolumeWord(EXT2_OFFSET_INODES_PER_GROUP, KERNEL_VOLUME_INODES);
    KernelSetVolumeHalf(EXT2_OFFSET_MAGIC, EXT2_SUPER_MAGIC);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_VALID_FS);
    KernelSetVolumeHalf(EXT2_OFFSET_ERRORS, 1U);
    KernelSetVolumeWord(EXT2_OFFSET_REVISION, EXT2_DYNAMIC_REV);
    KernelSetVolumeWord(EXT2_OFFSET_FIRST_INODE, EXT2_GOOD_OLD_FIRST_INODE);
    KernelSetVolumeHalf(EXT2_OFFSET_INODE_SIZE, (uint16_t)EXT2_GOOD_OLD_INODE_SIZE);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_FILETYPE);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_RO_COMPAT, EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER);

    for (size_t index = 0U; label[index] != '\0'; ++index)
    {
        KernelMemoryDeviceStore[EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_VOLUME_NAME + index] =
            (uint8_t)label[index];
    }

    KernelComposeGroupDescriptor();
    KernelComposeBitmaps();
    KernelComposeInodes();
    KernelComposeDirectories();
    KernelComposeFiles();
}

/*
 * Alters one field of the composed volume and reports whether the parser refused
 * the result, restoring the volume afterwards.
 *
 * The cache is invalidated around the alteration. The superblock is written into
 * the device's storage directly, beneath both the block layer and the cache, so
 * a cache holding the previous contents would answer the next read with them and
 * the assertion would be made against the volume that no longer exists.
 */
static bool KernelVolumeRefusedWith(BlockDevice *device, size_t offset, uint32_t value,
                                    bool half)
{
    Ext2Superblock superblock;
    bool refused;

    if (half)
    {
        KernelSetVolumeHalf(offset, (uint16_t)value);
    }
    else
    {
        KernelSetVolumeWord(offset, value);
    }

    (void)BufferInvalidateDevice(device);
    refused = !Ext2ReadSuperblock(device, &superblock);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Alters one field of the composed group descriptor and reports whether the
 * given judgement refused the result, restoring the descriptor afterwards.
 *
 * The cache is invalidated on both sides of the alteration for the reason
 * KernelVolumeRefusedWith gives: the descriptor is written beneath the cache,
 * and a cache still holding the previous descriptor would answer with it.
 */
static bool KernelDescriptorRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                        size_t offset, uint32_t value, bool half,
                                        bool whole_table)
{
    Ext2GroupDescriptor descriptor;
    bool refused;

    if (half)
    {
        KernelStoreHalf(KernelDescriptorField(offset), (uint16_t)value);
    }
    else
    {
        KernelStoreWord(KernelDescriptorField(offset), value);
    }

    (void)BufferInvalidateDevice(device);

    refused = whole_table ? !Ext2VerifyGroupDescriptors(device, superblock)
                          : !Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor);

    KernelComposeGroupDescriptor();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Asserts that the block group descriptor table is read as it stands, and that a
 * table this kernel must not trust is refused.
 *
 * A descriptor is three block numbers and three counts, and every one of them is
 * a plausible number wherever it is read from. A table read one block early, or
 * a descriptor taken to be 24 or 40 bytes rather than 32, yields block numbers
 * that address real blocks of the volume — the wrong ones — and a kernel that
 * then wrote an inode would write it over a file. Naming the values, and
 * asserting the one statement the table makes as a whole, is what catches that.
 */
static bool KernelVerifyGroups(BlockDevice *device, const Ext2Superblock *superblock)
{
    Ext2GroupDescriptor descriptor;
    bool succeeded = true;

    /* The derived geometry of the table itself, before any of it is read. */
    if ((Ext2GroupDescriptorBlock(superblock) != KERNEL_VOLUME_DESCRIPTOR_BLOCK) ||
        (Ext2GroupDescriptorBlocks(superblock) != 1U) ||
        (Ext2InodeTableBlocks(superblock) != 4U) ||
        (Ext2GroupFirstBlock(superblock, 0U) != 1U) ||
        (Ext2GroupBlockCount(superblock, 0U) != 127U))
    {
        KernelWriteString("  The geometry of the descriptor table is wrong.\n");
        succeeded = false;
    }

    if (!Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor))
    {
        KernelWriteString("  A well-formed group descriptor was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((descriptor.group != 0U) || (descriptor.block_bitmap != KERNEL_VOLUME_BLOCK_BITMAP) ||
        (descriptor.inode_bitmap != KERNEL_VOLUME_INODE_BITMAP) ||
        (descriptor.inode_table != KERNEL_VOLUME_INODE_TABLE) ||
        (descriptor.free_block_count != KERNEL_VOLUME_FREE_BLOCKS) ||
        (descriptor.free_inode_count != KERNEL_VOLUME_FREE_INODES) ||
        (descriptor.used_directory_count != KERNEL_VOLUME_DIRECTORIES))
    {
        KernelWriteString("  A field of the group descriptor was read from the wrong "
                          "place.\n");
        succeeded = false;
    }

    /* The whole table, and the one statement it makes about the volume. */
    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  A well-formed descriptor table was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* A group the volume does not hold. */
    if (Ext2ReadGroupDescriptor(device, superblock, superblock->group_count, &descriptor))
    {
        KernelWriteString("  A descriptor beyond the end of the table was read.\n");
        succeeded = false;
    }

    /* A structure of the group outside the volume, in both directions. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_TABLE, 200U,
                                     false, false) ||
        !KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_BLOCK_BITMAP, 0U,
                                     false, false))
    {
        KernelWriteString("  A group whose structures lie outside the volume was "
                          "accepted.\n");
        succeeded = false;
    }

    /*
     * An inode table that begins within the volume and ends beyond it. The
     * length is not stored anywhere and follows from the inode size, so a kernel
     * that checked only the first block would read the last inodes of the group
     * from nowhere.
     */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_TABLE, 127U,
                                     false, false))
    {
        KernelWriteString("  An inode table running past the end of the volume was "
                          "accepted.\n");
        succeeded = false;
    }

    /* Two structures beginning upon the same block. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_BITMAP,
                                     KERNEL_VOLUME_BLOCK_BITMAP, false, false))
    {
        KernelWriteString("  A group with two structures upon one block was accepted.\n");
        succeeded = false;
    }

    /* Counts beyond what the group holds. The count of directories is derived
     * from the volume rather than stated, the bound it must exceed being the
     * inodes in use, which changes whenever the composition does. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_BLOCKS, 200U,
                                     true, false) ||
        !KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_INODES,
                                     superblock->inodes_per_group + 1U, true, false) ||
        !KernelDescriptorRefusedWith(
            device, superblock, EXT2_OFFSET_BG_USED_DIRECTORIES,
            (superblock->inodes_per_group - superblock->free_inode_count) + 1U, true, false))
    {
        KernelWriteString("  A group reporting more than it holds was accepted.\n");
        succeeded = false;
    }

    /*
     * A descriptor every rule above accepts, whose free count nevertheless
     * disagrees with the superblock's. This is the assertion the table makes as
     * a whole and it is the one a misread table fails.
     */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_BLOCKS, 50U, true,
                                     true))
    {
        KernelWriteString("  A table not accounting for the volume's free space was "
                          "accepted.\n");
        succeeded = false;
    }

    /* Requests with nothing to work upon. */
    if (Ext2ReadGroupDescriptor(NULL, superblock, 0U, &descriptor) ||
        Ext2ReadGroupDescriptor(device, superblock, 0U, NULL) ||
        Ext2VerifyGroupDescriptors(NULL, superblock))
    {
        KernelWriteString("  A degenerate descriptor request was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

/*
 * Alters one word of the composed filesystem, beneath the superblock, and
 * reports whether the inode reader refused the result. The volume is recomposed
 * and the cache invalidated afterwards, for the reason KernelVolumeRefusedWith
 * gives.
 */
static bool KernelInodeRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                   size_t offset, uint32_t value)
{
    Ext2Inode inode;
    bool refused;

    KernelStoreWord(offset, value);
    (void)BufferInvalidateDevice(device);

    refused = !Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &inode);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Asserts that an inode is found where the format says it is, that its fields
 * are read from the right offsets, and that a file block index is resolved
 * through however many levels of indirection it requires.
 *
 * Locating an inode is three pieces of arithmetic upon numbers that begin at one
 * and indices that begin at zero, and every plausible mistake in it — omitting
 * the subtraction, using the block size where the inode size belongs, taking the
 * group's first block for its inode table — yields an offset that lands upon
 * some other inode of the same volume. That inode is a valid inode. It simply
 * belongs to a different file, and nothing in the machine can tell.
 *
 * The resolution of the block pointers fails the same way. An index that lands
 * one entry adrift within an indirect block, or a level of the walk that divides
 * by the wrong span, produces a block number that is a real block of the volume
 * holding somebody else's data.
 */
static bool KernelVerifyInodes(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t indirect_base = EXT2_DIRECT_BLOCK_COUNT;
    const uint64_t double_base = indirect_base + KERNEL_VOLUME_POINTERS;
    const uint64_t triple_base =
        double_base + ((uint64_t)KERNEL_VOLUME_POINTERS * KERNEL_VOLUME_POINTERS);
    const uint64_t beyond =
        triple_base + ((uint64_t)KERNEL_VOLUME_POINTERS * KERNEL_VOLUME_POINTERS *
                       KERNEL_VOLUME_POINTERS);
    Ext2Inode root;
    Ext2Inode file;
    uint32_t block;
    bool succeeded = true;

    /* The root directory, which the format reserves as inode 2. */
    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("  The root inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((root.number != EXT2_ROOT_INODE) || !Ext2InodeIsDirectory(&root) ||
        Ext2InodeIsRegular(&root) || (root.link_count != 3U) ||
        (root.size != KERNEL_VOLUME_BLOCK_SIZE) ||
        (root.block[0] != KERNEL_VOLUME_ROOT_DATA) ||
        ((root.mode & EXT2_PERMISSION_MASK) != 0x01EDU))
    {
        KernelWriteString("  The root inode was not read correctly.\n");
        succeeded = false;
    }

    /*
     * The file, which lies in the second block of the inode table: inode 11 is
     * index 10, and eight inodes of 128 bytes occupy a block of 1024. An inode
     * reader that never crossed out of the first block of the table would pass
     * every assertion above and fail here.
     */
    if (!Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &file))
    {
        KernelWriteString("  The composed file inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((file.number != KERNEL_VOLUME_FILE_INODE) || !Ext2InodeIsRegular(&file) ||
        Ext2InodeIsDirectory(&file) || (file.size != KERNEL_VOLUME_FILE_SIZE) ||
        (file.link_count != 1U) || (file.sector_count != 32U) || (file.uid != 1000U) ||
        (file.gid != 1001U) || ((file.mode & EXT2_PERMISSION_MASK) != 0x01A4U))
    {
        KernelWriteString("  A field of the file inode was read from the wrong place.\n");
        succeeded = false;
    }

    if (Ext2InodeBlockCount(superblock, &file) != (KERNEL_VOLUME_FILE_SIZE / 1024U))
    {
        KernelWriteString("  The blocks the file's size spans were counted wrongly.\n");
        succeeded = false;
    }

    /* The twelve direct blocks, at both ends of the range. */
    if (!Ext2InodeBlock(device, superblock, &file, 0U, &block) ||
        (block != KERNEL_VOLUME_DIRECT_FIRST))
    {
        KernelWriteString("  The first direct block was resolved wrongly.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, EXT2_DIRECT_BLOCK_COUNT - 1U, &block) ||
        (block != (KERNEL_VOLUME_DIRECT_FIRST + EXT2_DIRECT_BLOCK_COUNT - 1U)))
    {
        KernelWriteString("  The last direct block was resolved wrongly.\n");
        succeeded = false;
    }

    /*
     * The indirect block, at its first and last entries. The last is the
     * boundary the whole decomposition turns upon: an index one beyond it must
     * enter the doubly indirect block instead.
     */
    if (!Ext2InodeBlock(device, superblock, &file, indirect_base, &block) ||
        (block != KERNEL_VOLUME_INDIRECT_DATA))
    {
        KernelWriteString("  The first indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, double_base - 1U, &block) ||
        (block != KERNEL_VOLUME_INDIRECT_LAST))
    {
        KernelWriteString("  The last indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /* A hole within an indirect block, which is a block of zeroes and not an
     * error and not the end of the file. */
    if (!Ext2InodeBlock(device, superblock, &file, indirect_base + 1U, &block) ||
        (block != 0U))
    {
        KernelWriteString("  A hole within an indirect block was not reported as one.\n");
        succeeded = false;
    }

    /* The doubly indirect block: a hole at its first entry, data at its sixth. */
    if (!Ext2InodeBlock(device, superblock, &file, double_base, &block) || (block != 0U))
    {
        KernelWriteString("  A hole beneath the doubly indirect block was not reported "
                          "as one.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, double_base + 5U, &block) ||
        (block != KERNEL_VOLUME_DOUBLE_DATA))
    {
        KernelWriteString("  A doubly indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /* The triply indirect block, three levels down. */
    if (!Ext2InodeBlock(device, superblock, &file, triple_base + 3U, &block) ||
        (block != KERNEL_VOLUME_TRIPLE_DATA))
    {
        KernelWriteString("  A triply indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /*
     * A hole at the top of a subtree. The triply indirect entry of this inode is
     * present, but the doubly indirect block beneath it holds one entry only, so
     * everything past that entry's range is a hole reached without any block
     * being read at all.
     */
    if (!Ext2InodeBlock(device, superblock, &file,
                        triple_base + ((uint64_t)KERNEL_VOLUME_POINTERS *
                                       KERNEL_VOLUME_POINTERS),
                        &block) ||
        (block != 0U))
    {
        KernelWriteString("  A hole occupying a whole subtree was not reported as one.\n");
        succeeded = false;
    }

    /* An index beyond what fifteen pointers can address is refused, not held. */
    if (Ext2InodeBlock(device, superblock, &file, beyond, &block))
    {
        KernelWriteString("  An index beyond the triply indirect range was resolved.\n");
        succeeded = false;
    }

    /* Inode numbers the volume does not hold, at both ends. */
    if (Ext2ReadInode(device, superblock, 0U, &file) ||
        Ext2ReadInode(device, superblock, superblock->inode_count + 1U, &file))
    {
        KernelWriteString("  An inode outside the volume was read.\n");
        succeeded = false;
    }

    /*
     * An inode of the table that was never filled. The bytes are zeroes, which
     * are a valid encoding of nothing, so accepting them would let arithmetic
     * that had strayed beyond the table report a file rather than a mistake.
     */
    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_UNUSED_INODE, &file))
    {
        KernelWriteString("  An inode not in use was read as a file.\n");
        succeeded = false;
    }

    /* A direct pointer outside the volume, refused when the inode is read. */
    if (!KernelInodeRefusedWith(device, superblock,
                                KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, 0U), 9999U))
    {
        KernelWriteString("  An inode naming a block outside the volume was accepted.\n");
        succeeded = false;
    }

    /*
     * A pointer within an indirect block that lies outside the volume. It cannot
     * be caught when the inode is read, the block holding it not having been
     * read then, so it is checked where it is fetched.
     */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, 0U), 9999U);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &file) &&
        Ext2InodeBlock(device, superblock, &file, indirect_base, &block))
    {
        KernelWriteString("  An indirect pointer outside the volume was resolved.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /* Requests with nothing to work upon. */
    if (Ext2ReadInode(NULL, superblock, EXT2_ROOT_INODE, &file) ||
        Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, NULL) ||
        Ext2InodeBlock(device, superblock, NULL, 0U, &block) ||
        Ext2InodeBlock(device, superblock, &root, 0U, NULL))
    {
        KernelWriteString("  A degenerate inode request was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

/*
 * Whether two terminated strings hold the same characters. There is no C library
 * until Phase 7, and this is the only place in the self-test that needs one.
 */
static bool KernelSameString(const char *left, const char *right)
{
    size_t index = 0U;

    while ((left[index] != '\0') && (left[index] == right[index]))
    {
        ++index;
    }

    return left[index] == right[index];
}

/*
 * Alters one field of the composed volume and reports whether a traversal of the
 * root directory refused the result, restoring the volume afterwards.
 *
 * The offset is a byte of the device's storage rather than of a block, because
 * the rules a directory is held to are stated partly by its entries and partly
 * by the inode that owns them, and both must be reachable from one helper.
 *
 * The cache is invalidated on both sides of the alteration for the reason
 * KernelVolumeRefusedWith gives.
 */
static bool KernelDirectoryRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                       size_t offset, uint32_t value, uint32_t width)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    bool refused = true;

    if (width == 1U)
    {
        KernelMemoryDeviceStore[offset] = (uint8_t)value;
    }
    else if (width == 2U)
    {
        KernelStoreHalf(offset, (uint16_t)value);
    }
    else
    {
        KernelStoreWord(offset, value);
    }

    (void)BufferInvalidateDevice(device);

    if (Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        Ext2DirectoryOpen(&cursor, &root);
        refused = false;

        for (;;)
        {
            const Ext2DirectoryStep step =
                Ext2DirectoryNext(device, superblock, &cursor, &entry);

            if (step == EXT2_DIRECTORY_FAILED)
            {
                refused = true;
                break;
            }

            if (step == EXT2_DIRECTORY_END)
            {
                break;
            }
        }
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * The same for one byte and a path, for the rules that are not visible until an
 * entry and the inode it names are compared with one another.
 */
static bool KernelPathRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                  size_t offset, uint8_t value, const char *path)
{
    Ext2Inode inode;
    bool refused;

    KernelMemoryDeviceStore[offset] = value;
    (void)BufferInvalidateDevice(device);

    refused = !Ext2ResolvePath(device, superblock, path, &inode);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/* Whether a path resolves to the inode expected of it. */
static bool KernelPathIs(BlockDevice *device, const Ext2Superblock *superblock,
                         const char *path, uint32_t expected)
{
    Ext2Inode inode;

    return Ext2ResolvePath(device, superblock, path, &inode) && (inode.number == expected);
}

/* Whether a path is refused, which a path naming nothing must be. */
static bool KernelPathRefused(BlockDevice *device, const Ext2Superblock *superblock,
                              const char *path)
{
    Ext2Inode inode;

    return !Ext2ResolvePath(device, superblock, path, &inode);
}

/*
 * Asserts that the two readings of the two bytes at offset 6 of an entry are
 * distinguished by the incompatible feature flag and not by anything else.
 *
 * This is the one property of the format where the same bytes have two lawful
 * meanings, and where reading the wrong one produces no diagnostic of its own.
 * The entry "." bears a name length of 1 and a file type of EXT2_FT_DIR; read as
 * one sixteen-bit quantity those two bytes are 1 + 256 * 2 = 513, a name that
 * cannot fit within a record of twelve bytes. So the volume that states no file
 * type must refuse the entry the volume that states one accepts, and with the
 * file type byte cleared the same entry must read correctly with no type stated.
 */
static bool KernelVerifyEntryReadings(BlockDevice *device)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Superblock plain;
    Ext2Inode root;
    bool succeeded = true;

    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, 0U);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadSuperblock(device, &plain) &&
        Ext2ReadInode(device, &plain, EXT2_ROOT_INODE, &root))
    {
        Ext2DirectoryOpen(&cursor, &root);

        if (Ext2DirectoryNext(device, &plain, &cursor, &entry) != EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  A name length was read as eight bits upon a volume that "
                              "states no file type.\n");
            succeeded = false;
        }

        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA) +
                                EXT2_OFFSET_DE_FILE_TYPE] = 0U;
        (void)BufferInvalidateDevice(device);

        if (!Ext2DirectoryFind(device, &plain, &root, ".", 1U, &entry) ||
            (entry.inode != EXT2_ROOT_INODE) ||
            (entry.file_type != (uint8_t)EXT2_FT_UNKNOWN))
        {
            KernelWriteString("  An entry of a volume that states no file type was not "
                              "read.\n");
            succeeded = false;
        }
    }
    else
    {
        KernelWriteString("  A volume stating no file type was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return succeeded;
}

/* Whether a run of the buffer holds the bytes the composed file holds at an
 * offset within itself. */
static bool KernelFileBufferMatches(uint64_t offset, uint64_t length)
{
    for (uint64_t index = 0U; index < length; ++index)
    {
        if (KernelFileBuffer[index] != KernelFileByteAt(offset + index))
        {
            return false;
        }
    }

    return true;
}

/* Whether a run of the buffer is entirely zero, which is what a hole reads as. */
static bool KernelFileBufferIsZero(uint64_t length)
{
    for (uint64_t index = 0U; index < length; ++index)
    {
        if (KernelFileBuffer[index] != 0U)
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the contents of a file are read, that a hole reads as zeroes,
 * that the end of the file is reported by the count rather than as a failure,
 * and that both forms of symbolic link are read and followed.
 *
 * The composed file holds a byte derived from its own offset rather than a
 * constant or a pattern repeating every block. That is deliberate: a reader that
 * returned the right number of bytes from the wrong block would be
 * indistinguishable from a correct one under either of those, and resolving the
 * wrong block is the failure this whole chapter is arranged to catch.
 */
static bool KernelVerifyFiles(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t data_offset = (uint64_t)EXT2_DIRECT_BLOCK_COUNT * KERNEL_VOLUME_BLOCK_SIZE;
    const uint64_t hole_offset = data_offset + KERNEL_VOLUME_BLOCK_SIZE;
    char target[EXT2_SYMLINK_MAXIMUM + 1U];
    Ext2Inode inode;
    uint64_t read = 0U;
    bool succeeded = true;

    KernelWriteString("EXT2 files: asserting reading, holes and symbolic links.\n");

    if (!Ext2ResolvePath(device, superblock, "/sub/inner", &inode))
    {
        KernelWriteString("  The composed file was not found: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    /* The whole file, every byte of it, across the boundary between its two
     * blocks and ending part-way through the second. */
    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer,
                      sizeof KernelFileBuffer, &read) ||
        (read != KERNEL_VOLUME_INNER_SIZE) || !KernelFileBufferMatches(0U, read))
    {
        KernelWriteString("  A file was not read as it was composed.\n");
        succeeded = false;
    }

    /*
     * A run crossing the boundary between the two blocks. The first block ends
     * at 1024 and this run begins at 1000, so a reader that took the whole run
     * from one block would return 24 correct bytes and 76 wrong ones.
     */
    if (!Ext2ReadFile(device, superblock, &inode, 1000U, KernelFileBuffer, 100U, &read) ||
        (read != 100U) || !KernelFileBufferMatches(1000U, read))
    {
        KernelWriteString("  A read across a block boundary returned the wrong bytes.\n");
        succeeded = false;
    }

    /* A run wholly within the second block, which begins at an offset the first
     * block does not contain. */
    if (!Ext2ReadFile(device, superblock, &inode, 1100U, KernelFileBuffer, 64U, &read) ||
        (read != 64U) || !KernelFileBufferMatches(1100U, read))
    {
        KernelWriteString("  A read from the second block returned the wrong bytes.\n");
        succeeded = false;
    }

    /*
     * The end of the file. A read that would cross it is shortened to it, and a
     * read beginning at or beyond it yields nothing and succeeds — the end of a
     * file is where every reader arrives, and reporting it as a failure would
     * oblige each of them to treat the conclusion of its work as a fault.
     */
    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE - 100U,
                      KernelFileBuffer, 1000U, &read) ||
        (read != 100U) || !KernelFileBufferMatches(KERNEL_VOLUME_INNER_SIZE - 100U, read))
    {
        KernelWriteString("  A read crossing the end of the file was not shortened to it.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE, KernelFileBuffer,
                      64U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read at the end of the file did not report the end.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE + 4096U,
                      KernelFileBuffer, 64U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read beyond the end of the file did not report the end.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 0U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read of no length was not answered with no bytes.\n");
        succeeded = false;
    }

    /*
     * A hole reads as zeroes, and the block beside it reads as data. The sparse
     * file holds block 12 and not block 13, and the two are asserted together
     * because a reader that returned zeroes for both, or data for both, would
     * pass either assertion alone.
     */
    if (!Ext2ResolvePath(device, superblock, "/file", &inode))
    {
        KernelWriteString("  The composed sparse file was not found.\n");
        succeeded = false;
    }
    else
    {
        if (!Ext2ReadFile(device, superblock, &inode, data_offset, KernelFileBuffer, 64U,
                          &read) ||
            (read != 64U) || !KernelFileBufferMatches(data_offset, read))
        {
            KernelWriteString("  A block reached through the indirect block read wrongly.\n");
            succeeded = false;
        }

        if (!Ext2ReadFile(device, superblock, &inode, hole_offset, KernelFileBuffer, 64U,
                          &read) ||
            (read != 64U) || !KernelFileBufferIsZero(read))
        {
            KernelWriteString("  A hole did not read as zeroes.\n");
            succeeded = false;
        }
    }

    /* A directory is traversed and not read. */
    if (Ext2ResolvePath(device, superblock, "/sub", &inode) &&
        Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 64U, &read))
    {
        KernelWriteString("  A directory was read as a stream of bytes.\n");
        succeeded = false;
    }

    /* --- The symbolic links. --- */

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast", &inode) ||
        (inode.number != KERNEL_VOLUME_FAST_LINK_INODE) ||
        !Ext2InodeIsFastSymbolicLink(superblock, &inode) ||
        !Ext2ReadSymbolicLink(device, superblock, &inode, target, sizeof target) ||
        !KernelSameString(target, KERNEL_VOLUME_FAST_LINK_TARGET))
    {
        KernelWriteString("  A target held within its inode was not read.\n");
        succeeded = false;
    }

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-slow", &inode) ||
        (inode.number != KERNEL_VOLUME_SLOW_LINK_INODE) ||
        Ext2InodeIsFastSymbolicLink(superblock, &inode) ||
        !Ext2ReadSymbolicLink(device, superblock, &inode, target, sizeof target) ||
        !KernelSameString(target, KERNEL_VOLUME_SLOW_LINK_TARGET))
    {
        KernelWriteString("  A target held in a block was not read.\n");
        succeeded = false;
    }

    /* A target the caller has no room for is refused rather than truncated. */
    if (Ext2ResolvePathNoFollow(device, superblock, "/link-slow", &inode) &&
        Ext2ReadSymbolicLink(device, superblock, &inode, target,
                             sizeof(KERNEL_VOLUME_SLOW_LINK_TARGET) - 1U))
    {
        KernelWriteString("  A target longer than the buffer was accepted.\n");
        succeeded = false;
    }

    /*
     * Resolution through the links. The fast link names a directory by a
     * relative target, so it is resolved against the root, which holds the link;
     * the slow link names a file by an absolute target that walks through the
     * subdirectory eight times before descending into it.
     */
    if (!KernelPathIs(device, superblock, "/link-fast", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/link-fast/", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/link-fast/inner", KERNEL_VOLUME_INNER_INODE) ||
        !KernelPathIs(device, superblock, "/link-fast/../file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/link-slow", KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A path through a symbolic link did not resolve.\n");
        succeeded = false;
    }

    /*
     * The link itself, rather than what it names. A trailing separator overrides
     * the distinction: a path asserting a directory is asking for what the link
     * names, a link not being one.
     */
    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast", &inode) ||
        (inode.number != KERNEL_VOLUME_FAST_LINK_INODE))
    {
        KernelWriteString("  A last symbolic link was followed when it should not be.\n");
        succeeded = false;
    }

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast/", &inode) ||
        (inode.number != KERNEL_VOLUME_SUB_INODE))
    {
        KernelWriteString("  A trailing separator did not override the link.\n");
        succeeded = false;
    }

    /* A link within the path is followed whether or not the last one is. */
    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast/inner", &inode) ||
        (inode.number != KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A symbolic link within the path was not followed.\n");
        succeeded = false;
    }

    /*
     * A link that names itself. The format permits it — it is a valid file whose
     * contents happen to be its own name — and nothing but a depth bound stops
     * the resolver from following it until the stack is gone.
     */
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_SIZE), 9U);
    KernelStoreText(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCK),
                    "link-fast");
    (void)BufferInvalidateDevice(device);

    if (Ext2ResolvePath(device, superblock, "/link-fast", &inode))
    {
        KernelWriteString("  A symbolic link naming itself was followed to an end.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /* Nothing may be asked of a null argument. */
    if (Ext2ReadFile(NULL, superblock, &inode, 0U, KernelFileBuffer, 64U, &read) ||
        Ext2ReadFile(device, superblock, NULL, 0U, KernelFileBuffer, 64U, &read) ||
        Ext2ReadFile(device, superblock, &inode, 0U, NULL, 64U, &read) ||
        Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 64U, NULL) ||
        Ext2ReadSymbolicLink(device, superblock, &inode, NULL, sizeof target) ||
        Ext2ReadSymbolicLink(device, superblock, &inode, target, 0U) ||
        Ext2ResolvePathNoFollow(device, superblock, NULL, &inode))
    {
        KernelWriteString("  A read accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 files: reading, holes and symbolic links are sound.\n");
    }

    return succeeded;
}

/*
 * Restores the composed volume after something has written to it.
 *
 * The order matters and is not the order used everywhere else in this file.
 * BufferInvalidateDevice writes dirty buffers back before it discards them, so
 * composing first and invalidating afterwards would flush the writes of the test
 * just finished onto the volume just composed — restoring nothing and leaving a
 * volume that is neither what was written nor what was composed. The cache is
 * therefore emptied first, and the composition follows it.
 *
 * Every self-test before sub-task 5.6 could use the other order safely, none of
 * them having left a dirty buffer behind.
 */
static void KernelRestoreVolume(BlockDevice *device)
{
    (void)BufferInvalidateDevice(device);
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
}

/*
 * Asserts that a volume may be altered, and that it still describes itself
 * afterwards.
 *
 * This is the first self-test in the project that writes to a filesystem, and
 * the standard it is held to differs from every one before it. A read that goes
 * wrong returns the wrong bytes to one caller; a write that goes wrong destroys
 * data and cannot be undone, and the destruction is ordinarily silent — a block
 * allocated to two files reads correctly for both of them until one of them
 * writes.
 *
 * Two things follow. The assertions are made about the volume as a whole and not
 * only about the operation performed: after every sequence below, the free counts
 * of the group and of the superblock must agree with one another and with what
 * was actually taken, which is the statement a corrupted allocator cannot
 * satisfy. And every write here is made to the device of memory, never to a disk
 * the machine carries: the volumes upon those belong to whoever booted this
 * kernel.
 */
static bool KernelVerifyWrites(BlockDevice *device, Ext2Superblock *superblock)
{
    const uint64_t indirect_base = EXT2_DIRECT_BLOCK_COUNT + KERNEL_VOLUME_POINTERS;
    const uint64_t double_base = indirect_base + (KERNEL_VOLUME_POINTERS *
                                                  KERNEL_VOLUME_POINTERS);
    Ext2Superblock reread;
    Ext2GroupDescriptor descriptor;
    Ext2Inode inode;
    uint32_t free_blocks;
    uint32_t block = 0U;
    uint32_t number = 0U;
    uint64_t moved = 0U;
    bool used = false;
    bool succeeded = true;

    KernelWriteString("EXT2 writes: asserting allocation, writing and truncation.\n");

    /* --- The bitmaps, read against the composition. --- */

    if (!Ext2BlockInUse(device, superblock, KERNEL_VOLUME_INODE_TABLE, &used) || !used ||
        !Ext2BlockInUse(device, superblock, KERNEL_VOLUME_LAST_BLOCK, &used) || used ||
        !Ext2InodeInUse(device, superblock, EXT2_ROOT_INODE, &used) || !used ||
        !Ext2InodeInUse(device, superblock, KERNEL_VOLUME_UNUSED_INODE, &used) || used ||
        !Ext2InodeInUse(device, superblock, KERNEL_VOLUME_SLOW_LINK_INODE, &used) || !used)
    {
        KernelWriteString("  A bitmap did not report the volume as it was composed.\n");
        succeeded = false;
    }

    /* --- One block, allocated and returned. --- */

    free_blocks = superblock->free_block_count;

    if (!Ext2AllocateBlock(device, superblock, 0U, &block) ||
        !Ext2BlockInUse(device, superblock, block, &used) || !used ||
        (superblock->free_block_count != (free_blocks - 1U)))
    {
        KernelWriteString("  A block was not allocated, or was not then in use.\n");
        succeeded = false;
    }
    else
    {
        /*
         * The superblock upon the volume, and not the copy in memory. An
         * allocator that decremented its own structure and did not write it back
         * would satisfy every assertion made against memory and would leave the
         * volume claiming a block it had given away.
         */
        if (!Ext2ReadSuperblock(device, &reread) ||
            (reread.free_block_count != superblock->free_block_count) ||
            !Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor) ||
            (descriptor.free_block_count != superblock->free_block_count))
        {
            KernelWriteString("  An allocation was not written back to the volume.\n");
            succeeded = false;
        }

        if (!Ext2FreeBlock(device, superblock, block) ||
            !Ext2BlockInUse(device, superblock, block, &used) || used ||
            (superblock->free_block_count != free_blocks))
        {
            KernelWriteString("  A block was not returned to the volume.\n");
            succeeded = false;
        }
    }

    /* Freeing what is already free is refused: the second free is what allows a
     * block to be given to two files at once. */
    if (Ext2FreeBlock(device, superblock, block) ||
        Ext2FreeInode(device, superblock, KERNEL_VOLUME_UNUSED_INODE, false))
    {
        KernelWriteString("  Something already free was freed a second time.\n");
        succeeded = false;
    }

    /* --- The one free inode, and the exhaustion after it. --- */

    {
        /*
         * Every free inode of the volume, taken until there are none, and then
         * returned. Exhausting the volume rather than allocating one is what
         * asserts that the free count and the bitmap describe the same set: an
         * allocator that miscounted would either stop early, leaving inodes the
         * bitmap says are free, or run past the count and issue one twice.
         */
        uint32_t taken[KERNEL_VOLUME_INODES];
        uint32_t count = 0U;
        const uint32_t available = superblock->free_inode_count;

        while ((count < KERNEL_VOLUME_INODES) &&
               Ext2AllocateInode(device, superblock, false, &number))
        {
            taken[count] = number;
            ++count;
        }

        if ((count != available) || (superblock->free_inode_count != 0U))
        {
            KernelWriteString("  The free inodes of the volume were not all issued.\n");
            succeeded = false;
        }

        /* The lowest free inode is issued first, which is inode 14: the one the
         * self-test of sub-task 5.3 requires to be empty. */
        if ((count == 0U) || (taken[0] != KERNEL_VOLUME_UNUSED_INODE))
        {
            KernelWriteString("  The lowest free inode was not the first issued.\n");
            succeeded = false;
        }

        if (Ext2AllocateInode(device, superblock, false, &number))
        {
            KernelWriteString("  An inode was allocated from a volume holding none.\n");
            succeeded = false;
        }

        for (uint32_t index = 0U; index < count; ++index)
        {
            if (!Ext2InodeInUse(device, superblock, taken[index], &used) || !used ||
                !Ext2FreeInode(device, superblock, taken[index], false))
            {
                KernelWriteString("  An inode was not returned to the volume.\n");
                succeeded = false;
                break;
            }
        }

        if (superblock->free_inode_count != available)
        {
            KernelWriteString("  The inodes returned did not restore the free count.\n");
            succeeded = false;
        }
    }

    /* An inode belonging to the filesystem is never issued and never freed. */
    if (Ext2FreeInode(device, superblock, EXT2_ROOT_INODE, true))
    {
        KernelWriteString("  A reserved inode was freed.\n");
        succeeded = false;
    }

    /* --- Writing within a file that already has the blocks. --- */

    if (!Ext2ResolvePath(device, superblock, "/sub/inner", &inode))
    {
        KernelWriteString("  The composed file was not found.\n");
        return false;
    }

    for (uint32_t index = 0U; index < 128U; ++index)
    {
        KernelFileBuffer[index] = (uint8_t)(0xA0U + (index & 0x0FU));
    }

    if (!Ext2WriteFile(device, superblock, &inode, 100U, KernelFileBuffer, 128U, &moved) ||
        (moved != 128U) || (inode.size != KERNEL_VOLUME_INNER_SIZE))
    {
        KernelWriteString("  A write within a file did not write what it was given.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 100U, KernelFileBuffer, 128U, &moved) ||
        (moved != 128U))
    {
        KernelWriteString("  A file could not be read after being written.\n");
        succeeded = false;
    }
    else
    {
        for (uint32_t index = 0U; index < 128U; ++index)
        {
            if (KernelFileBuffer[index] != (uint8_t)(0xA0U + (index & 0x0FU)))
            {
                KernelWriteString("  A write did not reach the volume.\n");
                succeeded = false;
                break;
            }
        }
    }

    /* The bytes on either side of the write are untouched. */
    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 100U, &moved) ||
        !KernelFileBufferMatches(0U, moved) ||
        !Ext2ReadFile(device, superblock, &inode, 228U, KernelFileBuffer, 100U, &moved) ||
        !KernelFileBufferMatches(228U, moved))
    {
        KernelWriteString("  A write altered bytes beyond the range it was given.\n");
        succeeded = false;
    }

    /* --- Conservation: what a file gives up, it takes back. --- */

    free_blocks = superblock->free_block_count;

    if (!Ext2TruncateFile(device, superblock, &inode, 0U) || (inode.size != 0U) ||
        (inode.sector_count != 0U) || (superblock->free_block_count != (free_blocks + 2U)))
    {
        KernelWriteString("  Truncation to nothing did not return the file's blocks.\n");
        succeeded = false;
    }

    for (uint64_t index = 0U; index < KERNEL_VOLUME_INNER_SIZE; ++index)
    {
        KernelFileBuffer[index] = KernelFileByteAt(index);
    }

    if (!Ext2WriteFile(device, superblock, &inode, 0U, KernelFileBuffer,
                       KERNEL_VOLUME_INNER_SIZE, &moved) ||
        (moved != KERNEL_VOLUME_INNER_SIZE) || (inode.size != KERNEL_VOLUME_INNER_SIZE) ||
        (superblock->free_block_count != free_blocks))
    {
        KernelWriteString("  Rewriting a truncated file did not restore the volume.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer,
                      KERNEL_VOLUME_INNER_SIZE, &moved) ||
        (moved != KERNEL_VOLUME_INNER_SIZE) || !KernelFileBufferMatches(0U, moved))
    {
        KernelWriteString("  A file rewritten after truncation did not read back.\n");
        succeeded = false;
    }

    /* --- Extension, and the hole a write beyond the end leaves. --- */

    if (!Ext2WriteFile(device, superblock, &inode, 4096U, KernelFileBuffer, 16U, &moved) ||
        (moved != 16U) || (inode.size != (4096U + 16U)))
    {
        KernelWriteString("  A write beyond the end did not extend the file.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 2048U, KernelFileBuffer, 512U, &moved) ||
        (moved != 512U) || !KernelFileBufferIsZero(moved))
    {
        KernelWriteString("  The hole left by an extending write did not read as zeroes.\n");
        succeeded = false;
    }

    /* Truncation upward allocates nothing: the file grows by a hole. */
    free_blocks = superblock->free_block_count;

    if (!Ext2TruncateFile(device, superblock, &inode, 1U << 20) ||
        (inode.size != (1U << 20)) || (superblock->free_block_count != free_blocks))
    {
        KernelWriteString("  Truncation upward allocated blocks it need not have.\n");
        succeeded = false;
    }

    /* --- Allocation through the indirection. --- */

    if (!Ext2ResolvePath(device, superblock, "/file", &inode))
    {
        KernelWriteString("  The composed sparse file was not found.\n");
        succeeded = false;
    }
    else
    {
        /*
         * An entry of the doubly indirect block that holds nothing, so that both
         * an indirect block and a data block must be allocated to reach it. Two
         * blocks, and the difference between one and two is the whole of whether
         * a level of the walk was allocated or silently skipped.
         */
        const uint64_t offset = (double_base + KERNEL_VOLUME_POINTERS) *
                                (uint64_t)KERNEL_VOLUME_BLOCK_SIZE;

        free_blocks = superblock->free_block_count;
        KernelFileBuffer[0] = 0x5AU;

        if (!Ext2WriteFile(device, superblock, &inode, offset, KernelFileBuffer, 1U, &moved) ||
            (moved != 1U) || (superblock->free_block_count != (free_blocks - 2U)))
        {
            KernelWriteString("  A write through the indirection did not allocate a chain.\n");
            succeeded = false;
        }

        KernelFileBuffer[0] = 0U;

        if (!Ext2ReadFile(device, superblock, &inode, offset, KernelFileBuffer, 1U, &moved) ||
            (moved != 1U) || (KernelFileBuffer[0] != 0x5AU))
        {
            KernelWriteString("  A byte written through the indirection did not read back.\n");
            succeeded = false;
        }
    }

    /* --- The volume still describes itself. --- */

    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  The volume no longer accounts for itself: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* --- A volume that may not be written is not written. --- */

    KernelRestoreVolume(device);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadSuperblock(device, &reread) && reread.read_only)
    {
        Ext2Inode victim;

        if (Ext2AllocateBlock(device, &reread, 0U, &block) ||
            Ext2AllocateInode(device, &reread, false, &number) ||
            Ext2FreeBlock(device, &reread, KERNEL_VOLUME_LAST_BLOCK) ||
            Ext2WriteSuperblock(device, &reread) ||
            (Ext2ReadInode(device, &reread, KERNEL_VOLUME_INNER_INODE, &victim) &&
             (Ext2WriteInode(device, &reread, &victim) ||
              Ext2WriteFile(device, &reread, &victim, 0U, KernelFileBuffer, 16U, &moved) ||
              Ext2TruncateFile(device, &reread, &victim, 0U))))
        {
            KernelWriteString("  A read-only volume was altered.\n");
            succeeded = false;
        }
    }
    else
    {
        KernelWriteString("  A volume not cleanly unmounted was not made read-only.\n");
        succeeded = false;
    }

    KernelRestoreVolume(device);

    /* Nothing may be asked of a null argument. */
    if (Ext2AllocateBlock(NULL, superblock, 0U, &block) ||
        Ext2AllocateBlock(device, superblock, 0U, NULL) ||
        Ext2AllocateInode(device, NULL, false, &number) ||
        Ext2WriteInode(device, superblock, NULL) ||
        Ext2WriteFile(device, superblock, NULL, 0U, KernelFileBuffer, 16U, &moved) ||
        Ext2TruncateFile(device, superblock, NULL, 0U) ||
        Ext2BlockInUse(device, superblock, KERNEL_VOLUME_LAST_BLOCK, NULL))
    {
        KernelWriteString("  A write accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 writes: allocation, writing and truncation are sound.\n");
    }

    return succeeded;
}

/* How many entries a directory holds, for an assertion about a whole directory
 * rather than about one name within it. */
static bool KernelCountEntries(BlockDevice *device, const Ext2Superblock *superblock,
                               const Ext2Inode *directory, uint64_t *count)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;

    *count = 0U;
    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            return true;
        }

        ++*count;
    }
}

/*
 * Asserts that names may be inserted into a directory and removed from it, and
 * that files and directories may be created and destroyed.
 *
 * A directory is a linked list of records within each of its blocks, and every
 * operation here is an alteration of that list. The failures are accordingly the
 * failures of a list: a record whose length no longer reaches the next one, two
 * records overlapping, a record left in use that nothing points past. None of
 * them is visible in the operation that caused it — the directory reads
 * correctly until the traversal reaches the record that was damaged — so the
 * assertions are made by traversing the whole directory afterwards and counting
 * what comes out, and by requiring the volume to account for itself at the end.
 */
static bool KernelVerifyDirectoryWrites(BlockDevice *device, Ext2Superblock *superblock)
{
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    Ext2Inode made;
    Ext2Inode found;
    uint64_t count = 0U;
    uint64_t before = 0U;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint16_t root_links;
    bool empty = false;
    bool succeeded = true;

    KernelWriteString("EXT2 names: asserting insertion, removal and creation.\n");

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root) ||
        !KernelCountEntries(device, superblock, &root, &before))
    {
        KernelWriteString("  The root directory could not be read.\n");
        return false;
    }

    free_blocks = superblock->free_block_count;
    free_inodes = superblock->free_inode_count;
    root_links = root.link_count;

    /* --- A name inserted, found, and removed. --- */

    if (!Ext2DirectoryInsert(device, superblock, &root, "inserted", 8U,
                             KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  A name could not be inserted: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    if (!Ext2DirectoryFind(device, superblock, &root, "inserted", 8U, &entry) ||
        (entry.inode != KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/inserted", KERNEL_VOLUME_FILE_INODE))
    {
        KernelWriteString("  An inserted name was not found by looking for it.\n");
        succeeded = false;
    }

    /*
     * The whole directory, traversed. An insertion that split a record wrongly
     * leaves the records after it unreachable or overlapping, and neither shows
     * in the name just inserted — only in the count of everything.
     */
    if (!KernelCountEntries(device, superblock, &root, &count) || (count != (before + 1U)))
    {
        KernelWriteString("  The directory no longer yields the entries it holds.\n");
        succeeded = false;
    }

    /* A name already present is refused, a directory holding one name twice
     * making the path to it ambiguous. */
    if (Ext2DirectoryInsert(device, superblock, &root, "inserted", 8U, KERNEL_VOLUME_SUB_INODE,
                            (uint8_t)EXT2_FT_DIR) ||
        Ext2DirectoryInsert(device, superblock, &root, "file", 4U, KERNEL_VOLUME_FILE_INODE,
                            (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  A name already present was inserted a second time.\n");
        succeeded = false;
    }

    if (!Ext2DirectoryRemove(device, superblock, &root, "inserted", 8U) ||
        Ext2DirectoryFind(device, superblock, &root, "inserted", 8U, &entry) ||
        !KernelCountEntries(device, superblock, &root, &count) || (count != before))
    {
        KernelWriteString("  A name was not removed, or the directory did not recover.\n");
        succeeded = false;
    }

    /* Removing what is not there, and removing what may not be removed. */
    if (Ext2DirectoryRemove(device, superblock, &root, "inserted", 8U) ||
        Ext2DirectoryRemove(device, superblock, &root, ".", 1U) ||
        Ext2DirectoryRemove(device, superblock, &root, "..", 2U))
    {
        KernelWriteString("  A name that may not be removed was removed.\n");
        succeeded = false;
    }

    /*
     * The space a removal leaves is reused rather than the directory growing.
     * Inserting and removing the same name many times over must not consume a
     * block: the record before the removed one absorbs its space, and the next
     * insertion splits it again.
     */
    for (uint32_t attempt = 0U; attempt < 64U; ++attempt)
    {
        if (!Ext2DirectoryInsert(device, superblock, &root, "recycled", 8U,
                                 KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE) ||
            !Ext2DirectoryRemove(device, superblock, &root, "recycled", 8U))
        {
            KernelWriteString("  A name could not be inserted and removed repeatedly.\n");
            succeeded = false;
            break;
        }
    }

    if (superblock->free_block_count != free_blocks)
    {
        KernelWriteString("  Repeated insertion and removal consumed blocks.\n");
        succeeded = false;
    }

    /* --- A file created, written, linked and destroyed. --- */

    if (!Ext2CreateFile(device, superblock, &root, "created", 7U,
                        (uint16_t)(EXT2_S_IFREG | 0x01A4U), &made) ||
        !Ext2InodeIsRegular(&made) || (made.link_count != 1U) || (made.size != 0U) ||
        (superblock->free_inode_count != (free_inodes - 1U)))
    {
        KernelWriteString("  A file was not created.\n");
        succeeded = false;
    }
    else
    {
        uint64_t moved = 0U;

        KernelFileBuffer[0] = 0x11U;
        KernelFileBuffer[1] = 0x22U;

        if (!Ext2WriteFile(device, superblock, &made, 0U, KernelFileBuffer, 2U, &moved) ||
            (moved != 2U) ||
            !KernelPathIs(device, superblock, "/created", made.number))
        {
            KernelWriteString("  A created file could not be written or reached.\n");
            succeeded = false;
        }

        /* A second name for the same file, and the count that records it. */
        if (!Ext2Link(device, superblock, &root, "linked", 6U, &made) ||
            (made.link_count != 2U) ||
            !KernelPathIs(device, superblock, "/linked", made.number))
        {
            KernelWriteString("  A file was not given a second name.\n");
            succeeded = false;
        }

        /*
         * Removing one of two names removes the name and not the file. An unlink
         * that destroyed the file here would leave the other name leading to an
         * inode that had been freed, and quite possibly reissued.
         */
        if (!Ext2Unlink(device, superblock, &root, "created", 7U) ||
            !Ext2ReadInode(device, superblock, made.number, &found) ||
            (found.link_count != 1U) ||
            !KernelPathIs(device, superblock, "/linked", made.number))
        {
            KernelWriteString("  Removing one of two names destroyed the file.\n");
            succeeded = false;
        }

        if (!Ext2Unlink(device, superblock, &root, "linked", 6U) ||
            (superblock->free_inode_count != free_inodes) ||
            (superblock->free_block_count != free_blocks))
        {
            KernelWriteString("  Removing the last name did not destroy the file.\n");
            succeeded = false;
        }

        /*
         * The inode is free in the bitmap and is refused as a deleted file. Both
         * are asserted: the bitmap is what lets it be issued again, and the
         * refusal is what stops anything reaching it in the meantime.
         */
        if (!Ext2InodeInUse(device, superblock, made.number, &empty) || empty ||
            Ext2ReadInode(device, superblock, made.number, &found))
        {
            KernelWriteString("  An inode freed with its last name was still in use.\n");
            succeeded = false;
        }
    }

    /* --- A directory created and removed. --- */

    if (!Ext2CreateDirectory(device, superblock, &root, "made", 4U, 0x01EDU, &made) ||
        !Ext2InodeIsDirectory(&made) || (made.link_count != 2U) ||
        (made.size != KERNEL_VOLUME_BLOCK_SIZE) || (root.link_count != (root_links + 1U)))
    {
        KernelWriteString("  A directory was not created with its two links.\n");
        succeeded = false;
    }
    else
    {
        /*
         * The two entries every directory holds, resolved by the ordinary lookup
         * rather than assumed. A directory whose ".." named the wrong inode would
         * be reachable and would lead out of itself to somewhere else.
         */
        if (!KernelPathIs(device, superblock, "/made", made.number) ||
            !KernelPathIs(device, superblock, "/made/.", made.number) ||
            !KernelPathIs(device, superblock, "/made/..", EXT2_ROOT_INODE) ||
            !KernelPathIs(device, superblock, "/made/../made", made.number))
        {
            KernelWriteString("  A created directory did not hold \".\" and \"..\".\n");
            succeeded = false;
        }

        if (!Ext2DirectoryIsEmpty(device, superblock, &made, &empty) || !empty)
        {
            KernelWriteString("  A newly created directory was not empty.\n");
            succeeded = false;
        }

        /* A directory holding something is not removed. */
        if (!Ext2CreateFile(device, superblock, &made, "within", 6U,
                            (uint16_t)(EXT2_S_IFREG | 0x01A4U), &found) ||
            !Ext2DirectoryIsEmpty(device, superblock, &made, &empty) || empty)
        {
            KernelWriteString("  A directory holding a file reported itself empty.\n");
            succeeded = false;
        }

        if (Ext2RemoveDirectory(device, superblock, &root, "made", 4U))
        {
            KernelWriteString("  A directory holding a file was removed.\n");
            succeeded = false;
        }

        /* A directory may not be unlinked as a file, nor given a second name. */
        if (Ext2Unlink(device, superblock, &root, "made", 4U) ||
            Ext2Link(device, superblock, &root, "second", 6U, &made))
        {
            KernelWriteString("  A directory was treated as a file.\n");
            succeeded = false;
        }

        if (!Ext2Unlink(device, superblock, &made, "within", 6U) ||
            !Ext2RemoveDirectory(device, superblock, &root, "made", 4U) ||
            (root.link_count != root_links))
        {
            KernelWriteString("  An emptied directory was not removed.\n");
            succeeded = false;
        }
    }

    /* The root may never be removed, whatever it is named by. */
    if (Ext2RemoveDirectory(device, superblock, &root, ".", 1U))
    {
        KernelWriteString("  The root directory was removed.\n");
        succeeded = false;
    }

    /* --- Everything taken has been given back. --- */

    if ((superblock->free_block_count != free_blocks) ||
        (superblock->free_inode_count != free_inodes) ||
        !KernelCountEntries(device, superblock, &root, &count) || (count != before))
    {
        KernelWriteString("  The volume did not return to what it was.\n");
        succeeded = false;
    }

    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  The volume no longer accounts for itself: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* --- A read-only volume holds its names. --- */

    KernelRestoreVolume(device);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    {
        Ext2Superblock frozen;

        if (Ext2ReadSuperblock(device, &frozen) && frozen.read_only &&
            Ext2ReadInode(device, &frozen, EXT2_ROOT_INODE, &found))
        {
            if (Ext2DirectoryInsert(device, &frozen, &found, "no", 2U,
                                    KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE) ||
                Ext2DirectoryRemove(device, &frozen, &found, "file", 4U) ||
                Ext2CreateFile(device, &frozen, &found, "no", 2U,
                               (uint16_t)(EXT2_S_IFREG | 0x01A4U), &made) ||
                Ext2CreateDirectory(device, &frozen, &found, "no", 2U, 0x01EDU, &made) ||
                Ext2Unlink(device, &frozen, &found, "file", 4U) ||
                Ext2RemoveDirectory(device, &frozen, &found, "sub", 3U))
            {
                KernelWriteString("  A read-only volume had its names altered.\n");
                succeeded = false;
            }
        }
        else
        {
            KernelWriteString("  A volume not cleanly unmounted was not made read-only.\n");
            succeeded = false;
        }
    }

    KernelRestoreVolume(device);

    /* Nothing may be asked of a null argument, or of a name that is not one. */
    if (Ext2DirectoryInsert(device, superblock, NULL, "x", 1U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, NULL, 1U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 0U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "a/b", 3U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 1U, 0U, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 1U, superblock->inode_count + 1U,
                            0U) ||
        Ext2DirectoryRemove(device, superblock, NULL, "x", 1U) ||
        Ext2CreateFile(device, superblock, &root, "d", 1U, (uint16_t)EXT2_S_IFDIR, &made) ||
        Ext2CreateDirectory(device, superblock, NULL, "x", 1U, 0x01EDU, &made) ||
        Ext2DirectoryIsEmpty(device, superblock, &root, NULL))
    {
        KernelWriteString("  A directory operation accepted what it should refuse.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 names: insertion, removal and creation are sound.\n");
    }

    return succeeded;
}

/*
 * Asserts that a directory is traversed as the format lays it out, and that a
 * path is resolved to the inode it names.
 *
 * A directory is the first structure of the volume whose contents are variable
 * rather than fixed: a superblock lies at a known offset, a descriptor is 32
 * bytes and an inode is 128, but an entry is as long as its record length says
 * and the next one begins wherever that lands. Every mistake in reading it is
 * therefore self-propagating — one record length taken from the wrong offset, or
 * one entry advanced by the length of its name rather than by its record length,
 * and every entry after it in the block is read from the middle of something
 * else. The names that come out of that are not obviously wrong; they are
 * fragments of real names, and a lookup that fails to find a file that is there
 * is indistinguishable from a file that is not.
 *
 * The traversal is therefore asserted entry by entry against the layout the
 * volume was composed with, and not merely counted.
 */
static bool KernelVerifyDirectories(BlockDevice *device, const Ext2Superblock *superblock)
{
    static const char *const expected_names[] = {".",   "..",        "file",
                                                "sub", "link-fast", "link-slow"};
    static const uint32_t expected_inodes[] = {
        EXT2_ROOT_INODE,          EXT2_ROOT_INODE,               KERNEL_VOLUME_FILE_INODE,
        KERNEL_VOLUME_SUB_INODE,  KERNEL_VOLUME_FAST_LINK_INODE, KERNEL_VOLUME_SLOW_LINK_INODE};
    static const uint8_t expected_types[] = {
        (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_REG_FILE,
        (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_SYMLINK, (uint8_t)EXT2_FT_SYMLINK};
    const size_t expected_count = sizeof(expected_names) / sizeof(expected_names[0]);
    const size_t root_block = KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA);
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    Ext2Inode subdirectory;
    size_t counted = 0U;
    bool succeeded = true;

    KernelWriteString("EXT2 directories: asserting traversal and path resolution.\n");

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("  The root inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    /*
     * The root, entry by entry. The unused record standing between "file" and
     * "sub" must be passed over, and the final record, whose length runs to the
     * end of the block, must end the traversal rather than yield an entry.
     */
    Ext2DirectoryOpen(&cursor, &root);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  The root directory could not be traversed: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            break;
        }

        if (counted >= expected_count)
        {
            KernelWriteString("  The root directory yielded more entries than it holds.\n");
            succeeded = false;
            break;
        }

        if (!KernelSameString(entry.name, expected_names[counted]) ||
            (entry.inode != expected_inodes[counted]) ||
            (entry.file_type != expected_types[counted]))
        {
            KernelWriteString("  An entry of the root directory was read wrongly: ");
            KernelWriteString(entry.name);
            KernelWriteString("\n");
            succeeded = false;
        }

        ++counted;
    }

    if (counted != expected_count)
    {
        KernelWriteString("  The root directory did not yield the entries it holds.\n");
        succeeded = false;
    }

    /* A name the directory holds is found by looking for it. */
    if (!Ext2DirectoryFind(device, superblock, &root, "file", 4U, &entry) ||
        (entry.inode != KERNEL_VOLUME_FILE_INODE) || (entry.record_length != 16U) ||
        (entry.block != KERNEL_VOLUME_ROOT_DATA) || (entry.offset != 24U))
    {
        KernelWriteString("  A name the root directory holds was not found where it "
                          "stands.\n");
        succeeded = false;
    }

    /*
     * A name is matched by its whole length and not by a prefix of it. The
     * comparison is given a length rather than a terminator, and one that
     * stopped at the shorter of the two would match "fil" against "file".
     */
    if (Ext2DirectoryFind(device, superblock, &root, "fil", 3U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "files", 5U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "file", 3U, &entry))
    {
        KernelWriteString("  A name was matched against a prefix of another.\n");
        succeeded = false;
    }

    /* The name standing upon the unused record is not a name. */
    if (Ext2DirectoryFind(device, superblock, &root, "removed", 7U, &entry))
    {
        KernelWriteString("  The name upon an unused record was found.\n");
        succeeded = false;
    }

    /*
     * Path resolution. The root is named by the separator alone; repeated
     * separators are one; "." and ".." are resolved as the ordinary entries they
     * are, the ".." of the root naming the root itself; and a path of two
     * components reaches the file within the subdirectory.
     */
    if (!KernelPathIs(device, superblock, "/", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "///", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/.", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/..", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/sub", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/sub/", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/sub/.", KERNEL_VOLUME_SUB_INODE) ||
        !KernelPathIs(device, superblock, "/sub/..", EXT2_ROOT_INODE) ||
        !KernelPathIs(device, superblock, "/sub/../file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelPathIs(device, superblock, "/sub/inner", KERNEL_VOLUME_INNER_INODE) ||
        !KernelPathIs(device, superblock, "//sub///inner", KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A path did not resolve to the inode it names.\n");
        succeeded = false;
    }

    /*
     * The refusals. A relative path has nothing to be resolved against; a
     * component that does not exist cannot be traversed; and a component that is
     * not a directory holds no names, whether it stands within the path or is
     * asserted to be a directory by a separator at the end of it.
     */
    if (!KernelPathRefused(device, superblock, "file") ||
        !KernelPathRefused(device, superblock, "") ||
        !KernelPathRefused(device, superblock, "/missing") ||
        !KernelPathRefused(device, superblock, "/sub/missing") ||
        !KernelPathRefused(device, superblock, "/removed") ||
        !KernelPathRefused(device, superblock, "/file/") ||
        !KernelPathRefused(device, superblock, "/file/inner") ||
        !KernelPathRefused(device, superblock, "/sub/inner/"))
    {
        KernelWriteString("  A path that names nothing was resolved.\n");
        succeeded = false;
    }

    /* The subdirectory is a directory, and holds what was composed within it. */
    if (!Ext2ResolvePath(device, superblock, "/sub", &subdirectory) ||
        !Ext2InodeIsDirectory(&subdirectory) ||
        !Ext2DirectoryFind(device, superblock, &subdirectory, "inner", 5U, &entry) ||
        (entry.inode != KERNEL_VOLUME_INNER_INODE) ||
        (entry.file_type != (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  The subdirectory did not hold what was composed within it.\n");
        succeeded = false;
    }

    /* A file is not a directory, and holds no entries whatever its data is. */
    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &subdirectory))
    {
        Ext2DirectoryOpen(&cursor, &subdirectory);

        if (Ext2DirectoryNext(device, superblock, &cursor, &entry) != EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  A regular file was traversed as a directory.\n");
            succeeded = false;
        }
    }

    /*
     * A record contradicting the format is refused. Each of these is a rule the
     * traversal depends upon to terminate or to stay within its block: a record
     * length below the header cannot be advanced past; one that is not a multiple
     * of four leaves the next entry unaligned; one reaching beyond the block
     * contradicts the rule that no entry spans two; a name longer than its record
     * would be read out of the entry that follows; an inode number beyond the
     * volume names nothing; and a name holding the separator is reachable by no
     * path.
     */
    if (!KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH, 0U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH, 14U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH,
                                    KERNEL_VOLUME_BLOCK_SIZE + 4U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_NAME_LENGTH, 200U, 1U) ||
        !KernelDirectoryRefusedWith(device, superblock, root_block + EXT2_OFFSET_DE_INODE,
                                    superblock->inode_count + 1U, 4U) ||
        !KernelDirectoryRefusedWith(device, superblock, root_block + EXT2_OFFSET_DE_NAME,
                                    (uint32_t)EXT2_PATH_SEPARATOR, 1U))
    {
        KernelWriteString("  A directory entry contradicting the format was accepted.\n");
        succeeded = false;
    }

    /*
     * A directory occupies whole blocks, and holds at least its own entry. A
     * size that is not a multiple of the block size describes a final block
     * ending in the middle of a record.
     */
    if (!KernelDirectoryRefusedWith(device, superblock,
                                    KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE),
                                    KERNEL_VOLUME_BLOCK_SIZE - 24U, 4U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE), 0U,
                                    4U))
    {
        KernelWriteString("  A directory whose size cannot be traversed was accepted.\n");
        succeeded = false;
    }

    /*
     * The file type an entry declares must agree with the format of the inode it
     * names. The entry for "file" is made to declare a directory; the inode it
     * names is a regular file, and the path must be refused rather than resolved
     * to a file the caller will then treat as a directory.
     */
    if (!KernelPathRefusedWith(device, superblock, root_block + 24U + EXT2_OFFSET_DE_FILE_TYPE,
                               (uint8_t)EXT2_FT_DIR, "/file"))
    {
        KernelWriteString("  An entry contradicting the inode it names was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyEntryReadings(device))
    {
        succeeded = false;
    }

    /* Nothing may be asked of a null argument. */
    if (Ext2ResolvePath(NULL, superblock, "/", &root) ||
        Ext2ResolvePath(device, superblock, NULL, &root) ||
        Ext2ResolvePath(device, superblock, "/", NULL) ||
        Ext2DirectoryFind(device, superblock, NULL, "file", 4U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, NULL, 4U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "file", 0U, &entry) ||
        (Ext2DirectoryNext(device, superblock, NULL, &entry) != EXT2_DIRECTORY_FAILED))
    {
        KernelWriteString("  A directory operation accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 directories: traversal and path resolution are sound.\n");
    }

    return succeeded;
}

/*
 * Asserts that the superblock of a volume is read as it stands, and that a
 * volume this kernel must not address is refused rather than read hopefully.
 *
 * A filesystem parser fails silently by construction: every field it reads is a
 * number, and a number read from the wrong offset is still a number. A block
 * size taken from the fragment size, a count read as a half where the format
 * stores a word, an offset four bytes adrift — each yields a volume that looks
 * plausible and addresses the wrong blocks for the rest of the machine's life.
 * The assertions below name the value that each field must have, which is the
 * only form of assertion that catches that.
 */
static void KernelVerifyExt2(void)
{
    BlockDevice *device;
    Ext2Superblock superblock;
    bool succeeded = true;

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Volume self-test FAILED.\n");
        return;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock))
    {
        KernelWriteString("  A well-formed volume was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        KernelWriteString("Volume self-test FAILED.\n");
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
        return;
    }

    /*
     * Every field is compared against the value composed above. A parser reading
     * the right number from the wrong offset is the failure this catches, and
     * only naming the values catches it.
     */
    if ((superblock.magic != EXT2_SUPER_MAGIC) || (superblock.revision != EXT2_DYNAMIC_REV) ||
        (superblock.inode_count != KERNEL_VOLUME_INODES) || (superblock.block_count != 128U) ||
        (superblock.reserved_block_count != 6U) ||
        (superblock.free_block_count != KERNEL_VOLUME_FREE_BLOCKS) ||
        (superblock.free_inode_count != KERNEL_VOLUME_FREE_INODES) ||
        (superblock.first_data_block != 1U) ||
        (superblock.blocks_per_group != 8192U) ||
        (superblock.inodes_per_group != KERNEL_VOLUME_INODES) ||
        (superblock.state != EXT2_VALID_FS))
    {
        KernelWriteString("  A field of the superblock was read from the wrong place.\n");
        succeeded = false;
    }

    /* The block size is derived, not stored, and the geometry follows from it. */
    if ((superblock.block_size != 1024U) || (superblock.sectors_per_block != 2U) ||
        (superblock.group_count != 1U) || (Ext2GroupCount(&superblock) != 1U))
    {
        KernelWriteString("  The geometry derived from the superblock is wrong.\n");
        succeeded = false;
    }

    /* The revision 1 fields, including the label, which is padded and not
     * terminated upon the volume. */
    if ((superblock.first_inode != EXT2_GOOD_OLD_FIRST_INODE) ||
        (superblock.inode_size != EXT2_GOOD_OLD_INODE_SIZE) ||
        (superblock.feature_incompatible != EXT2_FEATURE_INCOMPAT_FILETYPE) ||
        (superblock.feature_read_only != EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ||
        (superblock.volume_name[0] != 'o') || (superblock.volume_name[8] != 't') ||
        (superblock.volume_name[9] != '\0'))
    {
        KernelWriteString("  The revision 1 fields were not read correctly.\n");
        succeeded = false;
    }

    /* A volume declaring only features this kernel implements may be written. */
    if (superblock.read_only)
    {
        KernelWriteString("  A volume this kernel fully implements was marked read-only.\n");
        succeeded = false;
    }

    /* The refusals. Each is a volume this kernel must not address as it stands. */
    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_MAGIC, 0x1234U, true))
    {
        KernelWriteString("  A volume bearing no magic number was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_REVISION, 2U, false))
    {
        KernelWriteString("  A volume of an unknown revision was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_LOG_BLOCK_SIZE, 4U, false))
    {
        KernelWriteString("  A block size beyond this kernel was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FIRST_DATA_BLOCK, 0U, false))
    {
        KernelWriteString("  A first data block contradicting the block size was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_BLOCKS_PER_GROUP, 0U, false) ||
        !KernelVolumeRefusedWith(device, EXT2_OFFSET_BLOCK_COUNT, 0U, false))
    {
        KernelWriteString("  A degenerate geometry was accepted.\n");
        succeeded = false;
    }

    /*
     * The group count is derivable from the blocks and from the inodes, and the
     * two must agree. Halving the inodes per group leaves a volume every other
     * rule accepts.
     */
    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_INODES_PER_GROUP, 8U, false))
    {
        KernelWriteString("  A volume whose two group counts disagree was accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FREE_BLOCKS, 1000U, false))
    {
        KernelWriteString("  A volume reporting more free blocks than it holds was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_INODE_SIZE, 100U, true) ||
        !KernelVolumeRefusedWith(device, EXT2_OFFSET_INODE_SIZE, 2048U, true))
    {
        KernelWriteString("  An inode size that is not a power of two within a block was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FIRST_INODE, 2U, false))
    {
        KernelWriteString("  A first usable inode among the reserved ones was accepted.\n");
        succeeded = false;
    }

    /*
     * An incompatible feature this kernel lacks makes the volume unreadable; a
     * read-only compatible one makes it unwritable. The distinction is the whole
     * purpose of the two fields, so both directions are asserted.
     */
    if (!KernelVolumeRefusedWith(device, EXT2_OFFSET_FEATURE_INCOMPAT,
                                 EXT2_FEATURE_INCOMPAT_RECOVER, false))
    {
        KernelWriteString("  A volume requiring an unimplemented feature was accepted.\n");
        succeeded = false;
    }

    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_RO_COMPAT, EXT2_FEATURE_RO_COMPAT_BTREE_DIR);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !superblock.read_only)
    {
        KernelWriteString("  A volume with an unimplemented read-only feature was not "
                          "made read-only.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !superblock.read_only)
    {
        KernelWriteString("  A volume that was not cleanly unmounted was not made "
                          "read-only.\n");
        succeeded = false;
    }

    /* A volume of revision 0 states neither inode size nor first inode. */
    KernelComposeVolume();
    KernelSetVolumeWord(EXT2_OFFSET_REVISION, EXT2_GOOD_OLD_REV);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_RECOVER);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        (superblock.inode_size != EXT2_GOOD_OLD_INODE_SIZE) ||
        (superblock.first_inode != EXT2_GOOD_OLD_FIRST_INODE) ||
        (superblock.feature_incompatible != 0U) || (superblock.volume_name[0] != '\0'))
    {
        KernelWriteString("  A volume of revision 0 was not given its fixed values.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /*
     * The block group descriptor table, read from the volume just restored. It
     * is asserted here rather than in a self-test of its own because it can only
     * be read through a superblock, and this is where a valid one exists.
     */
    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyGroups(device, &superblock))
    {
        succeeded = false;
    }

    /* The inodes, read through the descriptor table just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyInodes(device, &superblock))
    {
        succeeded = false;
    }

    /* The directories, traversed through the inodes just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        !KernelVerifyDirectories(device, &superblock))
    {
        succeeded = false;
    }

    /* The contents of the files, reached through the paths just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyFiles(device, &superblock))
    {
        succeeded = false;
    }

    /* The alteration of a volume, upon the device of memory alone. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyWrites(device, &superblock))
    {
        succeeded = false;
    }

    /* The alteration of a directory, which is what turns an inode into a file
     * somebody can name. */
    KernelRestoreVolume(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        !KernelVerifyDirectoryWrites(device, &superblock))
    {
        succeeded = false;
    }

    KernelRestoreVolume(device);

    /* A device with nowhere to put a superblock, and requests without one. */
    if (Ext2ReadSuperblock(NULL, &superblock) || Ext2ReadSuperblock(device, NULL))
    {
        KernelWriteString("  A degenerate request was accepted.\n");
        succeeded = false;
    }

    (void)BufferInvalidateDevice(device);
    (void)BlockUnregister(device);

    device = BlockRegister("mem1", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 2U,
                           false);

    if ((device == NULL) || Ext2ReadSuperblock(device, &superblock))
    {
        KernelWriteString("  A device too short to hold a superblock was accepted.\n");
        succeeded = false;
    }

    if (device != NULL)
    {
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
    }

    KernelWriteString(succeeded ? "Volume self-test passed.\n" : "Volume self-test FAILED.\n");
}

/*
 * ---------------------------------------------------------------------------
 * The self-test of the virtual filesystem layer, sub-task 5.8.
 *
 * Everything above this point asserts one volume in isolation: a superblock, a
 * table, an inode, a directory, the contents of a file. This asserts the tree
 * those volumes are joined into, and there are three things in it that no test
 * of one volume can reach.
 *
 * The first is identity. A file reached twice must be one file, and not two
 * descriptions of one file that may disagree; the assertion is that a write
 * through one descriptor is seen by the other, and that neither of them, upon
 * closing, leaves a node behind.
 *
 * The second is the mount. A second volume is mounted upon a directory of the
 * first, and the assertions state which volume each path reached — which
 * requires the two to be distinguishable, and is why the second is a copy of the
 * first with one field altered rather than a second composition. Crossing the
 * mount point and returning across it by ".." are the two directions, and the
 * second is the one a layer that matched paths by prefix would get wrong.
 *
 * The third is the record a mount leaves upon a volume. A volume opened for
 * writing is marked as not cleanly unmounted for as long as it is open, and the
 * assertion reads that mark back out of the medium rather than out of the
 * superblock in memory: a mark that never reached the disk would protect
 * nothing, that being the one circumstance — a machine that stopped — the mark
 * exists for.
 * ------------------------------------------------------------------------- */

/* The device names the self-test registers, and the point the second is mounted
 * upon. */
#define KERNEL_VFS_ROOT_DEVICE   "mem0"
#define KERNEL_VFS_SECOND_DEVICE "mem2"
#define KERNEL_VFS_MOUNT_POINT   "/sub"

/*
 * The owner given to the file "/file" upon the second volume alone.
 *
 * The two volumes are otherwise identical, so this is the whole of what
 * distinguishes them, and every assertion below that states which volume a path
 * reached states it by reading this field. A value no composition writes is
 * chosen so that reading it by accident is not possible.
 */
#define KERNEL_VFS_SECOND_UID UINT16_C(0x5A5A)

/* Whether the sequence below has held so far. It is a file-scope variable
 * because the assertion helper sets it, there being some sixty assertions and
 * threading a result through each of them obscuring what is being asserted. */
static bool KernelVfsSucceeded;

/*
 * Asserts one property, naming it and the layer's own diagnosis where it fails.
 *
 * The self-tests of the earlier sub-tasks state each failure with a block of
 * their own because there was no diagnosis to print beside it. There is one
 * here — VfsLastError describes every refusal — so a failure names both what was
 * expected and what the layer said instead, which is the difference between
 * knowing that a test failed and knowing why.
 */
static void KernelVfsRequire(bool condition, const char *statement)
{
    if (condition)
    {
        return;
    }

    KernelWriteString("  ");
    KernelWriteString(statement);
    KernelWriteString(" [");
    KernelWriteString(VfsErrorName(VfsLastErrorCode()));
    KernelWriteString(": ");
    KernelWriteString(VfsLastError());
    KernelWriteString("]\n");
    KernelVfsSucceeded = false;
}

/* A halfword read back out of a store, so that what reached the medium may be
 * asserted rather than what was intended to. */
static uint16_t KernelLoadHalf(const uint8_t *store, size_t offset)
{
    return (uint16_t)((uint16_t)store[offset] | ((uint16_t)store[offset + 1U] << 8));
}

/* A halfword written into an arbitrary store, the composer writing only into the
 * first. */
static void KernelStoreHalfIn(uint8_t *store, size_t offset, uint16_t value)
{
    store[offset] = (uint8_t)(value & 0xFFU);
    store[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

/* Whether a path names the file it should, of the format it should. */
static bool KernelVfsIs(const char *path, uint64_t number, VfsNodeType type)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return false;
    }

    return (attributes.number == number) && (attributes.type == type);
}

/* Whether a path is refused, and refused for the stated reason. Both halves
 * matter: a path refused for the wrong reason is a resolver that reached the
 * wrong conclusion by luck. */
static bool KernelVfsRefusedWith(const char *path, VfsError expected)
{
    VfsAttributes attributes;

    if (VfsStat(path, &attributes))
    {
        return false;
    }

    return VfsLastErrorCode() == expected;
}

/* The size of the file a path names, or UINT64_MAX where it names none. */
static uint64_t KernelVfsSizeOf(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return UINT64_MAX;
    }

    return attributes.size;
}

/* The owner of the file a path names, which is what says which volume it lies
 * upon. */
static uint16_t KernelVfsOwnerOf(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return 0U;
    }

    return attributes.uid;
}

/* The file a path names, or zero where it names none. */
static uint64_t KernelVfsInodeOfPath(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return 0U;
    }

    return attributes.number;
}

/* The number of names a file bears, or zero where the path names none. */
static uint64_t KernelVfsLinksOf(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return 0U;
    }

    return attributes.link_count;
}

/* Whether a directory holds a name, found by reading the directory rather than
 * by resolving the name: the two are different operations and a directory that
 * lists what it cannot resolve is as wrong as one that resolves what it does not
 * list. */
static bool KernelVfsDirectoryHolds(const char *path, const char *name, uint64_t *count)
{
    VfsDirectoryEntry entry;
    bool end = false;
    bool found = false;
    const int descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U);

    if (count != NULL)
    {
        *count = 0U;
    }

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        return false;
    }

    while (VfsReadDirectory(descriptor, &entry, &end) && (!end))
    {
        if (KernelSameString(entry.name, name))
        {
            found = true;
        }

        if (count != NULL)
        {
            ++*count;
        }
    }

    (void)VfsClose(descriptor);
    return found;
}

/*
 * Asserts the resolution of a path: the components, the separators, the entries
 * "." and "..", and the symbolic links.
 */
static void KernelVerifyVfsResolution(void)
{
    char target[VFS_SYMLINK_MAXIMUM + 1U];
    VfsAttributes attributes;

    KernelVfsRequire(KernelVfsIs("/", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "the root did not resolve to the root directory");
    KernelVfsRequire(KernelVfsIs("/file", KERNEL_VOLUME_FILE_INODE, VFS_NODE_REGULAR),
                     "a file in the root did not resolve");
    KernelVfsRequire(KernelVfsIs("/sub", KERNEL_VOLUME_SUB_INODE, VFS_NODE_DIRECTORY),
                     "a subdirectory did not resolve");
    KernelVfsRequire(KernelVfsIs("/sub/inner", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "a file within a subdirectory did not resolve");

    /* The size joins the two halves the format holds it in, and is what bounds
     * every read of the file. */
    KernelVfsRequire(KernelVfsSizeOf("/file") == KERNEL_VOLUME_FILE_SIZE,
                     "the size of a file was not reported as the volume states it");
    KernelVfsRequire(KernelVfsSizeOf("/sub/inner") == KERNEL_VOLUME_INNER_SIZE,
                     "the size of a file within a subdirectory was wrong");

    /*
     * The separators. Repeated ones are equivalent to one, a trailing one
     * asserts a directory, and neither is a curiosity: both arise in every path
     * a person types.
     */
    KernelVfsRequire(KernelVfsIs("//sub//inner", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "repeated separators were not equivalent to one");
    KernelVfsRequire(KernelVfsIs("/sub/", KERNEL_VOLUME_SUB_INODE, VFS_NODE_DIRECTORY),
                     "a trailing separator upon a directory was refused");
    KernelVfsRequire(KernelVfsRefusedWith("/file/", VFS_ERROR_NOT_DIRECTORY),
                     "a trailing separator upon a regular file was accepted");

    /*
     * "." and ".." are ordinary entries of the volume and are resolved by
     * looking rather than by being interpreted. The ".." of the root names the
     * root, which is what the volume says and what a resolver must not
     * second-guess.
     */
    KernelVfsRequire(KernelVfsIs("/.", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "\".\" did not resolve to the directory holding it");
    KernelVfsRequire(KernelVfsIs("/..", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "the \"..\" of the root did not name the root");
    KernelVfsRequire(KernelVfsIs("/sub/..", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "\"..\" did not leave a subdirectory");
    KernelVfsRequire(KernelVfsIs("/sub/../file", KERNEL_VOLUME_FILE_INODE, VFS_NODE_REGULAR),
                     "a path continuing through \"..\" did not resolve");

    /*
     * The symbolic links. A link is followed where the file is wanted and
     * reported as itself where the name is, which is the distinction between
     * acting upon a file and acting upon its name.
     */
    KernelVfsRequire(KernelVfsIs("/link-fast", KERNEL_VOLUME_SUB_INODE, VFS_NODE_DIRECTORY),
                     "a fast symbolic link was not followed");
    KernelVfsRequire(KernelVfsIs("/link-fast/inner", KERNEL_VOLUME_INNER_INODE,
                                 VFS_NODE_REGULAR),
                     "a symbolic link standing within a path was not followed");
    KernelVfsRequire(KernelVfsIs("/link-slow", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "a symbolic link held in a block was not followed");

    KernelVfsRequire(VfsStatLink("/link-fast", &attributes) &&
                         (attributes.number == KERNEL_VOLUME_FAST_LINK_INODE) &&
                         (attributes.type == VFS_NODE_SYMBOLIC_LINK),
                     "a symbolic link was followed where the name was asked for");

    KernelVfsRequire(VfsReadLink("/link-fast", target, sizeof target) &&
                         KernelSameString(target, KERNEL_VOLUME_FAST_LINK_TARGET),
                     "the target of a fast symbolic link was not read");
    KernelVfsRequire(VfsReadLink("/link-slow", target, sizeof target) &&
                         KernelSameString(target, KERNEL_VOLUME_SLOW_LINK_TARGET),
                     "the target of a symbolic link held in a block was not read");
    KernelVfsRequire(!VfsReadLink("/file", target, sizeof target),
                     "the target of something that is not a link was read");

    /* The refusals, each with the reason that distinguishes it from the others. */
    KernelVfsRequire(KernelVfsRefusedWith("/absent", VFS_ERROR_NOT_FOUND),
                     "a name the volume does not hold was resolved");
    KernelVfsRequire(KernelVfsRefusedWith("/file/beyond", VFS_ERROR_NOT_DIRECTORY),
                     "a path continuing through a regular file was resolved");
    KernelVfsRequire(KernelVfsRefusedWith("relative", VFS_ERROR_INVALID),
                     "a relative path was resolved, there being nothing to resolve it "
                     "against");
    KernelVfsRequire(KernelVfsRefusedWith("", VFS_ERROR_INVALID),
                     "an empty path was resolved");

    /*
     * The record whose inode number is zero holds space where a name was
     * removed. The bytes of the name are still lying in it, and a resolver that
     * read them rather than the inode number would report a file that was
     * deleted.
     */
    KernelVfsRequire(KernelVfsRefusedWith("/removed", VFS_ERROR_NOT_FOUND),
                     "a record marked unused was resolved as a name");
}

/*
 * Asserts the open file: that its position advances by what it transferred, that
 * the end of a file is reported by the count and not as a failure, and that two
 * descriptors upon one file have one identity and two positions.
 */
static void KernelVerifyVfsFiles(void)
{
    uint64_t transferred = 0U;
    uint64_t position = 0U;
    VfsAttributes attributes;
    int first;
    int second;

    first = VfsOpen("/sub/inner", VFS_OPEN_READ, 0U);
    KernelVfsRequire(first != VFS_NO_DESCRIPTOR, "a file could not be opened for reading");

    if (first == VFS_NO_DESCRIPTOR)
    {
        return;
    }

    KernelVfsRequire(VfsFileAttributes(first, &attributes) &&
                         (attributes.size == KERNEL_VOLUME_INNER_SIZE) &&
                         (attributes.type == VFS_NODE_REGULAR),
                     "an open file did not describe itself as the volume does");

    /*
     * The whole file, read in one call. The composed contents depend upon the
     * offset, so a read that returned the right count from the wrong block fails
     * here rather than passing.
     */
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, KERNEL_VOLUME_INNER_SIZE,
                             &transferred) &&
                         (transferred == KERNEL_VOLUME_INNER_SIZE) &&
                         KernelFileBufferMatches(0U, KERNEL_VOLUME_INNER_SIZE),
                     "the contents of a file were not read through a descriptor");

    KernelVfsRequire(VfsTell(first, &position) && (position == KERNEL_VOLUME_INNER_SIZE),
                     "the position did not advance by what was read");

    /*
     * The end of the file. Every reader arrives at it, so it is reported by the
     * count rather than by the return value; a layer that reported it as a
     * failure would oblige each caller to treat the conclusion of its work as a
     * fault.
     */
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, 64U, &transferred) &&
                         (transferred == 0U),
                     "reading at the end of a file was reported as a failure");

    /* Seeking, in all three of its origins. */
    KernelVfsRequire(VfsSeek(first, 100, VFS_SEEK_SET, &position) && (position == 100U),
                     "a seek from the beginning did not arrive where it was sent");
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, 16U, &transferred) &&
                         (transferred == 16U) && KernelFileBufferMatches(100U, 16U),
                     "a read after a seek did not begin where the seek left the position");

    KernelVfsRequire(VfsSeek(first, 0, VFS_SEEK_END, &position) &&
                         (position == KERNEL_VOLUME_INNER_SIZE),
                     "a seek to the end did not arrive at the size");
    KernelVfsRequire(VfsSeek(first, -16, VFS_SEEK_CURRENT, &position) &&
                         (position == (KERNEL_VOLUME_INNER_SIZE - 16U)),
                     "a seek backwards from the position was wrong");

    /*
     * A position beyond the end is permitted, writing there being how a sparse
     * file is made; a position before the beginning is refused, there being
     * nothing there to name.
     */
    KernelVfsRequire(VfsSeek(first, 1000000, VFS_SEEK_END, &position),
                     "a seek beyond the end of a file was refused");
    KernelVfsRequire(!VfsSeek(first, -1, VFS_SEEK_SET, &position) &&
                         (VfsLastErrorCode() == VFS_ERROR_INVALID),
                     "a seek before the beginning of a file was accepted");
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, 16U, &transferred) &&
                         (transferred == 0U),
                     "a read beyond the end of a file returned bytes");

    /*
     * Two descriptors upon one file. They must share the file — one node, so
     * that what one alters the other sees — and not share the position, which
     * belongs to the open file and not to the file.
     */
    second = VfsOpen("/sub/inner", VFS_OPEN_READ, 0U);
    KernelVfsRequire(second != VFS_NO_DESCRIPTOR, "a file could not be opened a second time");

    if (second != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsSeek(second, 0, VFS_SEEK_SET, &position) && (position == 0U),
                         "the second descriptor could not be positioned");
        KernelVfsRequire(VfsTell(first, &position) && (position != 0U),
                         "positioning one descriptor moved the position of another upon the "
                         "same file");
        KernelVfsRequire(VfsRead(second, KernelFileBuffer, 32U, &transferred) &&
                             (transferred == 32U) && KernelFileBufferMatches(0U, 32U),
                         "the second descriptor did not read from its own position");
        KernelVfsRequire(VfsClose(second), "a descriptor could not be closed");
    }

    /* A directory is not read as a stream, and a file is not opened as a
     * directory. */
    KernelVfsRequire(VfsOpen("/", VFS_OPEN_READ | VFS_OPEN_WRITE, 0U) == VFS_NO_DESCRIPTOR,
                     "a directory was opened for writing");
    KernelVfsRequire(VfsOpen("/file", VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U) ==
                         VFS_NO_DESCRIPTOR,
                     "a regular file was opened as a directory");
    KernelVfsRequire(VfsOpen("/file", 0U, 0U) == VFS_NO_DESCRIPTOR,
                     "an open asking neither to read nor to write was accepted");
    KernelVfsRequire(VfsOpen("/link-fast", VFS_OPEN_READ | VFS_OPEN_NO_FOLLOW, 0U) ==
                         VFS_NO_DESCRIPTOR,
                     "a symbolic link was opened where the open refused to follow one");

    KernelVfsRequire(VfsClose(first), "a descriptor could not be closed");
    KernelVfsRequire(!VfsClose(first), "a descriptor was closed twice");
}

/* Asserts that a directory is listed, and that what it lists is what resolves. */
static void KernelVerifyVfsDirectories(void)
{
    uint64_t count = 0U;
    uint64_t transferred = 0U;
    int descriptor;

    KernelVfsRequire(KernelVfsDirectoryHolds("/", "file", &count),
                     "a directory listing did not hold a name the directory holds");
    KernelVfsRequire(count == 6U,
                     "the root was not listed as holding six entries: \".\", \"..\", "
                     "\"file\", \"sub\" and the two links");
    KernelVfsRequire(!KernelVfsDirectoryHolds("/", "removed", NULL),
                     "a record marked unused was listed as a name");
    KernelVfsRequire(KernelVfsDirectoryHolds("/", "..", NULL),
                     "\"..\" was not listed, though the volume holds it as an entry");
    KernelVfsRequire(KernelVfsDirectoryHolds("/sub", "inner", &count) && (count == 3U),
                     "a subdirectory was not listed as holding \".\", \"..\" and one file");

    descriptor = VfsOpen("/", VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a directory could not be opened");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(!VfsRead(descriptor, KernelFileBuffer, 16U, &transferred) &&
                             (VfsLastErrorCode() == VFS_ERROR_IS_DIRECTORY),
                         "a directory was read as a stream of bytes");
        KernelVfsRequire(VfsClose(descriptor), "a directory could not be closed");
    }
}

/*
 * Asserts that a volume may be altered through the layer: that a file is
 * created, written, read back, given a second name, truncated and destroyed, and
 * that a file something holds is not destroyed beneath it.
 */
static void KernelVerifyVfsWrites(void)
{
    static const char *const path = "/made";
    uint64_t transferred = 0U;
    uint64_t position = 0U;
    uint64_t root_links;
    int descriptor;

    /* Creating a file, and writing to it. */
    descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                                   VFS_OPEN_EXCLUSIVE,
                         0644U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be created");

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        return;
    }

    for (size_t index = 0U; index < 512U; ++index)
    {
        KernelFileBuffer[index] = KernelFileByteAt((uint64_t)index);
    }

    KernelVfsRequire(VfsWrite(descriptor, KernelFileBuffer, 512U, &transferred) &&
                         (transferred == 512U),
                     "a newly created file could not be written to");
    KernelVfsRequire(VfsTell(descriptor, &position) && (position == 512U),
                     "the position did not advance by what was written");
    KernelVfsRequire(VfsClose(descriptor), "a written file could not be closed");

    KernelVfsRequire(KernelVfsSizeOf(path) == 512U,
                     "the size of a written file was not what was written to it");
    KernelVfsRequire(KernelVfsLinksOf(path) == 1U, "a created file did not bear one name");

    /* Reading it back. The contents were composed from the offset, so a write
     * that reached the wrong block is caught here. */
    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a written file could not be reopened");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        for (size_t index = 0U; index < 512U; ++index)
        {
            KernelFileBuffer[index] = 0U;
        }

        KernelVfsRequire(VfsRead(descriptor, KernelFileBuffer, 512U, &transferred) &&
                             (transferred == 512U) && KernelFileBufferMatches(0U, 512U),
                         "what was read back from a file was not what was written to it");
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    /* An exclusive creation of a file that exists. */
    KernelVfsRequire(VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE,
                             0644U) == VFS_NO_DESCRIPTOR,
                     "an exclusive creation of a file that exists was accepted");
    KernelVfsRequire(VfsLastErrorCode() == VFS_ERROR_EXISTS,
                     "an exclusive creation was refused for the wrong reason");

    /*
     * An appending write goes to the end of the file as it stands and not to
     * where the position is. The position is deliberately left at zero to make
     * the two distinguishable.
     */
    descriptor = VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_APPEND, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be opened to append");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsWrite(descriptor, KernelFileBuffer, 128U, &transferred) &&
                             (transferred == 128U),
                         "an appending write failed");
        KernelVfsRequire(VfsTell(descriptor, &position) && (position == 640U),
                         "an appending write did not go to the end of the file");
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(KernelVfsSizeOf(path) == 640U,
                     "the file did not grow by what was appended to it");

    /* Truncation, downward to nothing and upward into a hole. */
    KernelVfsRequire(VfsTruncate(path, 0U) && (KernelVfsSizeOf(path) == 0U),
                     "a file could not be truncated to nothing");
    KernelVfsRequire(VfsTruncate(path, 4096U) && (KernelVfsSizeOf(path) == 4096U),
                     "a file could not be extended by truncation");

    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsRead(descriptor, KernelFileBuffer, 512U, &transferred) &&
                             (transferred == 512U) && KernelFileBufferIsZero(512U),
                         "the hole a truncation left did not read as zeroes");
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    /* Opening with a truncation discards the contents. */
    descriptor = VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR,
                     "a file could not be opened for truncation");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(KernelVfsSizeOf(path) == 0U,
                     "an open that truncates did not discard the contents");

    /* A second name for one file, and the link count that says how many names
     * lead to it. */
    KernelVfsRequire(VfsLink(path, "/made-again"), "a file could not be given a second name");
    KernelVfsRequire(KernelVfsLinksOf(path) == 2U,
                     "the link count did not rise with the second name");
    KernelVfsRequire(KernelVfsInodeOfPath(path) == KernelVfsInodeOfPath("/made-again"),
                     "the two names did not lead to one file");
    KernelVfsRequire(!VfsLink("/sub", "/sub-again") &&
                         (VfsLastErrorCode() == VFS_ERROR_IS_DIRECTORY),
                     "a directory was given a second name");
    KernelVfsRequire(VfsUnlink("/made-again") && (KernelVfsLinksOf(path) == 1U),
                     "removing one of two names did not leave the file with one");

    /*
     * A file something holds is not destroyed. This kernel keeps no list of
     * files that have no name and are not yet gone, so the alternative to
     * refusing is to free the inode and the blocks beneath a descriptor still
     * reading them.
     */
    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be opened");
    KernelVfsRequire(!VfsUnlink(path) && (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "a file that was open was destroyed");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(VfsUnlink(path), "a file could not be destroyed once nothing held it");
    KernelVfsRequire(KernelVfsRefusedWith(path, VFS_ERROR_NOT_FOUND),
                     "a destroyed file still resolved");

    /*
     * Directories. A new one bears two links — its own "." and the parent's
     * entry — and the parent gains one for the ".." within it. The parent's
     * count is the half that is easy to omit and impossible to see.
     */
    root_links = KernelVfsLinksOf("/");
    KernelVfsRequire(VfsCreateDirectory("/dir", 0755U), "a directory could not be created");
    KernelVfsRequire(KernelVfsInodeOfPath("/dir") != 0U, "a created directory did not resolve");
    KernelVfsRequire(KernelVfsLinksOf("/dir") == 2U,
                     "a new directory did not bear the two links \".\" and its own entry "
                     "give it");
    KernelVfsRequire(KernelVfsLinksOf("/") == (root_links + 1U),
                     "the parent did not gain the link the child's \"..\" holds");
    KernelVfsRequire(KernelVfsIs("/dir/..", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "the \"..\" of a new directory did not name its parent");

    KernelVfsRequire(!VfsCreateDirectory("/dir", 0755U) &&
                         (VfsLastErrorCode() == VFS_ERROR_EXISTS),
                     "a directory that exists was created again");
    KernelVfsRequire(!VfsRemoveDirectory("/sub") &&
                         (VfsLastErrorCode() == VFS_ERROR_NOT_EMPTY),
                     "a directory holding names was removed");
    KernelVfsRequire(!VfsUnlink("/dir") && (VfsLastErrorCode() == VFS_ERROR_IS_DIRECTORY),
                     "a directory was removed as though it were a file");

    KernelVfsRequire(VfsRemoveDirectory("/dir"), "an empty directory could not be removed");
    KernelVfsRequire(KernelVfsLinksOf("/") == root_links,
                     "the parent did not give back the link the removed child held");
    KernelVfsRequire(KernelVfsRefusedWith("/dir", VFS_ERROR_NOT_FOUND),
                     "a removed directory still resolved");
}

/*
 * Asserts the mount: that a second volume covers a directory of the first, that
 * a path crossing the mount point reaches the second volume, that ".." from the
 * root of the second arrives at the parent of the mount point in the first, and
 * that the directory the mount covers reappears when the mount is withdrawn.
 *
 * The two volumes are identical but for the owner of "/file", and every
 * assertion about which volume a path reached is made by reading that field.
 */
static void KernelVerifyVfsMounts(void)
{
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_SECOND_DEVICE, "/file", "ext2", true) &&
                         (VfsLastErrorCode() == VFS_ERROR_NOT_DIRECTORY),
                     "a volume was mounted upon something that is not a directory");
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_ROOT_DEVICE, KERNEL_VFS_MOUNT_POINT, "ext2",
                                     true) &&
                         (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "a device already mounted was mounted a second time");
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_SECOND_DEVICE, "/sub", "minix", true) &&
                         (VfsLastErrorCode() == VFS_ERROR_UNSUPPORTED),
                     "a volume was mounted as a filesystem that is not registered");

    /* Before the mount, the point is the first volume's own directory. */
    KernelVfsRequire(KernelVfsIs(KERNEL_VFS_MOUNT_POINT, KERNEL_VOLUME_SUB_INODE,
                                 VFS_NODE_DIRECTORY),
                     "the mount point was not the first volume's directory before the mount");

    KernelVfsRequire(VfsMountVolume(KERNEL_VFS_SECOND_DEVICE, KERNEL_VFS_MOUNT_POINT, "ext2",
                                    true),
                     "a second volume could not be mounted");
    KernelVfsRequire(VfsMountCount() == 2U, "two mounts were not recorded");

    /*
     * The mount point now names the root of the second volume, and not the
     * directory beneath it. Both are directories and the discriminator is what
     * lies within them.
     */
    KernelVfsRequire(KernelVfsIs(KERNEL_VFS_MOUNT_POINT, EXT2_ROOT_INODE,
                                 VFS_NODE_DIRECTORY),
                     "the mount point did not become the root of the mounted volume");
    KernelVfsRequire(KernelVfsOwnerOf("/sub/file") == KERNEL_VFS_SECOND_UID,
                     "a path crossing the mount point did not reach the second volume");
    KernelVfsRequire(KernelVfsOwnerOf("/file") != KERNEL_VFS_SECOND_UID,
                     "a path not crossing the mount point reached the second volume");

    /* What the mount covers is hidden, entirely, for as long as it stands. */
    KernelVfsRequire(KernelVfsRefusedWith("/sub/inner", VFS_ERROR_NOT_FOUND),
                     "the directory the mount covers was still reachable through it");

    /*
     * Leaving the mounted volume by "..".
     *
     * The ".." of the second volume's root names that root, which is what the
     * volume says; the parent of a mounted root is the directory the mount
     * covers, which only this layer knows. A layer matching paths by prefix
     * would answer this with the second volume's own root and would be wrong in
     * a way nothing else here would catch.
     */
    KernelVfsRequire(KernelVfsOwnerOf("/sub/../file") != KERNEL_VFS_SECOND_UID,
                     "\"..\" from the root of a mounted volume did not leave it");
    KernelVfsRequire(KernelVfsOwnerOf("/sub/../sub/file") == KERNEL_VFS_SECOND_UID,
                     "a path returning across a mount point and crossing it again did not "
                     "reach the second volume");

    /* A read-only mount refuses everything that would alter its volume. */
    KernelVfsRequire(VfsOpen("/sub/file", VFS_OPEN_WRITE, 0U) == VFS_NO_DESCRIPTOR,
                     "a file upon a read-only mount was opened for writing");
    KernelVfsRequire(VfsLastErrorCode() == VFS_ERROR_READ_ONLY,
                     "a read-only mount refused a write for the wrong reason");
    KernelVfsRequire(!VfsCreateDirectory("/sub/new", 0755U) &&
                         (VfsLastErrorCode() == VFS_ERROR_READ_ONLY),
                     "a directory was created upon a read-only mount");

    /* The root may not be withdrawn while a volume stands within it. */
    KernelVfsRequire(!VfsUnmount("/") && (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "the root was withdrawn while a volume was mounted within it");

    KernelVfsRequire(VfsUnmount(KERNEL_VFS_MOUNT_POINT),
                     "a mounted volume could not be withdrawn");
    KernelVfsRequire(VfsMountCount() == 1U, "the withdrawn mount was still recorded");

    /* What the mount covered reappears exactly as it was. */
    KernelVfsRequire(KernelVfsIs(KERNEL_VFS_MOUNT_POINT, KERNEL_VOLUME_SUB_INODE,
                                 VFS_NODE_DIRECTORY),
                     "the covered directory did not reappear when the mount was withdrawn");
    KernelVfsRequire(KernelVfsIs("/sub/inner", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "what the covered directory held did not reappear");
    KernelVfsRequire(!VfsUnmount("/sub") && (VfsLastErrorCode() == VFS_ERROR_NOT_FOUND),
                     "a mount that does not stand was withdrawn");
}

/*
 * The self-test of the virtual filesystem layer.
 *
 * Every write here is made to the two devices of memory and never to a disk the
 * machine carries, for the reason every earlier write self-test is: the volumes
 * upon those belong to whoever booted this kernel.
 */
static void KernelVerifyVfs(void)
{
    BlockDevice *first;
    BlockDevice *second;
    int descriptor;

    KernelVfsSucceeded = true;

    VfsInitialise();

    if (!Ext2VfsInitialise())
    {
        KernelWriteString("  The EXT2 filesystem could not be registered.\n");
        KernelWriteString("Filesystem self-test FAILED.\n");
        return;
    }

    KernelVfsRequire(!Ext2VfsInitialise(),
                     "a filesystem type was registered twice under one name");

    first = BlockRegister(KERNEL_VFS_ROOT_DEVICE, &KernelMemoryDeviceOperations, NULL,
                          BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, false);
    second = BlockRegister(KERNEL_VFS_SECOND_DEVICE, &KernelMemoryDeviceOperations,
                           KernelMemoryDeviceSecondStore, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if ((first == NULL) || (second == NULL))
    {
        KernelWriteString("  The devices of memory could not be registered.\n");
        KernelWriteString("Filesystem self-test FAILED.\n");
        return;
    }

    /*
     * Both volumes are composed before either is mounted, and the second is a
     * copy of the first taken before the first was marked as open. The copy is
     * then given a different owner for "/file", which is the only respect in
     * which the two differ and therefore the only thing an assertion can use to
     * say which of them a path reached.
     */
    (void)BufferInvalidateDevice(first);
    (void)BufferInvalidateDevice(second);
    KernelComposeVolume();

    for (size_t index = 0U; index < sizeof KernelMemoryDeviceSecondStore; ++index)
    {
        KernelMemoryDeviceSecondStore[index] = KernelMemoryDeviceStore[index];
    }

    KernelStoreHalfIn(KernelMemoryDeviceSecondStore,
                      KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_UID),
                      KERNEL_VFS_SECOND_UID);

    (void)BufferInvalidateDevice(first);
    (void)BufferInvalidateDevice(second);

    /* Nothing is mounted, so no absolute path resolves and no mount may be made
     * anywhere but at the root. */
    KernelVfsRequire(!VfsRootIsMounted(), "a root was mounted before anything mounted one");
    KernelVfsRequire(KernelVfsRefusedWith("/", VFS_ERROR_NOT_FOUND),
                     "a path resolved with nothing mounted");
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_ROOT_DEVICE, "/anywhere", "ext2", false) &&
                         (VfsLastErrorCode() == VFS_ERROR_INVALID),
                     "the first mount was made somewhere other than the root");
    KernelVfsRequire(!VfsMountVolume("no-such-device", "/", "ext2", false) &&
                         (VfsLastErrorCode() == VFS_ERROR_NOT_FOUND),
                     "a volume was mounted from a device that does not exist");

    if (!VfsMountVolume(KERNEL_VFS_ROOT_DEVICE, "/", "ext2", false))
    {
        KernelWriteString("  The root volume could not be mounted: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        KernelWriteString("Filesystem self-test FAILED.\n");
        (void)BufferInvalidateDevice(first);
        (void)BlockUnregister(first);
        (void)BufferInvalidateDevice(second);
        (void)BlockUnregister(second);
        return;
    }

    KernelVfsRequire(VfsRootIsMounted(), "the root mount was not recorded");
    KernelVfsRequire(VfsMountCount() == 1U, "one mount was not recorded");

    /*
     * The volume records that this kernel has it open, and the record is read
     * back out of the medium rather than out of the superblock in memory: a mark
     * that had not reached the device would protect nothing, a machine that
     * stopped being the one circumstance it exists for.
     */
    KernelVfsRequire((KernelLoadHalf(KernelMemoryDeviceStore,
                                     EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE) &
                      (uint16_t)EXT2_VALID_FS) == 0U,
                     "a volume mounted for writing was not marked as not cleanly unmounted");
    KernelVfsRequire((KernelLoadHalf(KernelMemoryDeviceStore,
                                     EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE) &
                      (uint16_t)EXT2_ERROR_FS) == 0U,
                     "a volume merely opened for writing was marked as holding errors");
    KernelVfsRequire(KernelLoadHalf(KernelMemoryDeviceStore,
                                    EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_MOUNT_COUNT) == 1U,
                     "the mount count upon the volume was not raised");

    KernelVerifyVfsResolution();
    KernelVerifyVfsFiles();
    KernelVerifyVfsDirectories();
    KernelVerifyVfsWrites();
    KernelVerifyVfsMounts();

    /* The root is not withdrawn while a file upon it is open. */
    descriptor = VfsOpen("/file", VFS_OPEN_READ, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be opened");
    KernelVfsRequire(!VfsUnmount("/") && (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "a volume was withdrawn while a file upon it was open");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(VfsSync(), "the mounts could not be written back");
    KernelVfsRequire(VfsUnmount("/"), "the root volume could not be withdrawn");
    KernelVfsRequire(!VfsRootIsMounted(), "the root was still mounted after being withdrawn");

    /*
     * Nothing was leaked. Every node this sequence took was released, and every
     * descriptor closed. This is the assertion the node cache exists to be held
     * to: a resolution that failed to release what it held would exhaust the
     * table long before a machine had done any real work, and would do so
     * silently until it did.
     */
    KernelVfsRequire(VfsNodesHeld() == 0U,
                     "nodes were left held after everything was closed and withdrawn");
    KernelVfsRequire(VfsOpenFileCount() == 0U, "descriptors were left open");

    /*
     * The volume records that it was cleanly unmounted, and describes itself
     * consistently afterwards. The descriptor table is verified here rather than
     * while the volume was mounted because a mounted volume is marked unclean and
     * is therefore permitted to disagree with itself.
     */
    KernelVfsRequire((KernelLoadHalf(KernelMemoryDeviceStore,
                                     EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE) &
                      (uint16_t)EXT2_VALID_FS) != 0U,
                     "a volume withdrawn cleanly was not marked as cleanly unmounted");

    {
        Ext2Superblock superblock;

        KernelVfsRequire(Ext2ReadSuperblock(first, &superblock) &&
                             Ext2VerifyGroupDescriptors(first, &superblock),
                         "the volume did not describe itself consistently after everything "
                         "the layer did to it");
    }

    (void)BufferInvalidateDevice(first);
    (void)BlockUnregister(first);
    (void)BufferInvalidateDevice(second);
    (void)BlockUnregister(second);

    KernelWriteString(KernelVfsSucceeded ? "Filesystem self-test passed.\n"
                                         : "Filesystem self-test FAILED.\n");
}

/*
 * How many bytes the probe below writes through the filesystem layer.
 *
 * It is deliberately larger than one block of any volume this kernel accepts, so
 * that the write crosses a block boundary and the position of the descriptor is
 * carried across it, which is the whole of what the layer adds to the write of
 * sub-task 5.6.
 */
#define KERNEL_VFS_PROBE_SIZE 5000U

/*
 * Exercises a volume the machine actually carries, through the layer rather than
 * through the format.
 *
 * The discipline is the one every write self-test in this project observes, and
 * it is the discipline of a kernel that may be booted upon somebody else's
 * machine. Nothing is written unless the operator asked for it at the GRUB menu;
 * nothing is created, so a volume that does not already hold the probe file is
 * left exactly as it was; and the file acted upon bears a name nothing else
 * would choose.
 *
 * The volume is then withdrawn and mounted afresh read-only. A mount that is
 * never withdrawn has not been shown to withdraw, and withdrawing it is what
 * writes back the mark that says the volume was cleanly unmounted — so the
 * operator's disk is left clean rather than left claiming to be open, which is
 * both the better outcome and the one that demonstrates both directions of the
 * mark.
 */
static void KernelVfsProbeVolume(void)
{
    static const char *const path = "/oxys-write-test";
    VfsAttributes attributes;
    uint64_t transferred = 0U;
    uint64_t index;
    int descriptor;

    if (!KernelCommandLineHasOption("ext2-write-test"))
    {
        return;
    }

    KernelWriteString("VFS write test: the command line permits writing to the mounted "
                      "volume.\n");

    if (!VfsStat(path, &attributes))
    {
        KernelWriteString("VFS write test: " "/oxys-write-test" " is not present; nothing "
                          "written.\n");
        return;
    }

    if (attributes.type != VFS_NODE_REGULAR)
    {
        KernelWriteString("VFS write test: " "/oxys-write-test" " is not a regular file; "
                          "nothing written.\n");
        return;
    }

    descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE, 0U);

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        KernelWriteString("VFS write test: the file could not be opened: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    /*
     * The contents are derived from the offset, so that a file written from the
     * wrong place is distinguishable from one written correctly when it is
     * examined from outside with `debugfs`.
     */
    for (index = 0U; index < KERNEL_VFS_PROBE_SIZE; index += sizeof KernelFileBuffer)
    {
        uint64_t run = KERNEL_VFS_PROBE_SIZE - index;

        if (run > sizeof KernelFileBuffer)
        {
            run = sizeof KernelFileBuffer;
        }

        for (uint64_t offset = 0U; offset < run; ++offset)
        {
            KernelFileBuffer[offset] = KernelFileByteAt(index + offset);
        }

        if (!VfsWrite(descriptor, KernelFileBuffer, run, &transferred) || (transferred != run))
        {
            KernelWriteString("VFS write test: the file could not be written: ");
            KernelWriteString(VfsLastError());
            KernelWriteString("\n");
            (void)VfsClose(descriptor);
            return;
        }
    }

    /*
     * The file is read back through the same descriptor, which requires the
     * position to be moved: the whole of what a descriptor adds to a write is
     * that it remembers where it is, and reading back without seeking would read
     * from the end of what was just written.
     */
    if (!VfsSeek(descriptor, 0, VFS_SEEK_SET, NULL))
    {
        KernelWriteString("VFS write test: the position could not be moved.\n");
        (void)VfsClose(descriptor);
        return;
    }

    for (index = 0U; index < KERNEL_VFS_PROBE_SIZE; index += sizeof KernelFileBuffer)
    {
        uint64_t run = KERNEL_VFS_PROBE_SIZE - index;

        if (run > sizeof KernelFileBuffer)
        {
            run = sizeof KernelFileBuffer;
        }

        if (!VfsRead(descriptor, KernelFileBuffer, run, &transferred) || (transferred != run) ||
            (!KernelFileBufferMatches(index, run)))
        {
            KernelWriteString("VFS write test: what was read back was not what was "
                              "written.\n");
            (void)VfsClose(descriptor);
            return;
        }
    }

    (void)VfsClose(descriptor);

    if (!VfsSync() || !VfsStat(path, &attributes) ||
        (attributes.size != KERNEL_VFS_PROBE_SIZE))
    {
        KernelWriteString("VFS write test: the volume could not be written back.\n");
        return;
    }

    KernelWriteString("VFS write test: ");
    KernelWriteDecimal(KERNEL_VFS_PROBE_SIZE);
    KernelWriteString(" bytes written to " "/oxys-write-test" " and read back "
                      "identically.\n");

    /* The withdrawal, and the fresh mount that leaves the volume clean. */
    if (!VfsUnmount("/"))
    {
        KernelWriteString("VFS write test: the volume could not be withdrawn: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("VFS write test: the volume was withdrawn and marked cleanly "
                      "unmounted.\n");

    if (!VfsMountRoot("ext2", true))
    {
        KernelWriteString("VFS write test: the volume could not be mounted afresh: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    VfsReport();
}

/*
 * Mounts a volume the machine actually carries at the root, and reports what it
 * holds.
 *
 * It is mounted read-only unless the operator booted the entry of the GRUB menu
 * that permits this kernel to write to their volumes. A kernel that mounted a
 * stranger's disk for writing would mark it as not cleanly unmounted merely by
 * having been booted, and every such disk would then demand a check before its
 * owner could mount it again — which is a real cost imposed for nothing.
 *
 * A machine carrying no volume is not in error. `make verify` runs upon one.
 */
static void KernelMountRootVolume(void)
{
    const bool writable = KernelCommandLineHasOption("ext2-write-test");

    VfsInitialise();

    if (!Ext2VfsInitialise())
    {
        KernelWriteString("VFS: the EXT2 filesystem could not be registered.\n");
        return;
    }

    if (!VfsMountRoot("ext2", !writable))
    {
        KernelWriteString("VFS: no volume was mounted at the root: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    VfsReport();
    VfsReportDirectory("/");
    KernelVfsProbeVolume();
}

/*
 * Moves the cursor of a serial terminal to a column of the current line, the
 * column being counted from one, by the sequence ECMA-48 calls CHA — Cursor
 * Character Absolute, CSI Pn G. The display driver crosses a row boundary upon a
 * backspace by moving the cursor itself, which a terminal at the far end of a
 * serial line will not do upon receiving a backspace; the movement must
 * therefore be described to it.
 *
 * The two devices agree only so far as the terminal is eighty columns wide, the
 * kernel having no way to ask it. A wider or narrower terminal will have wrapped
 * the line elsewhere and the correction will land upon the wrong column of it.
 * The proper remedy is a line discipline that knows the width of its terminal,
 * which belongs to Phase 8.
 */
static void KernelSerialCursorToColumn(size_t column)
{
    /* Two digits suffice for a column of an eighty-column line. */
    char sequence[8];
    size_t index = 0U;
    const size_t number = column + 1U;

    sequence[index] = '\x1B';
    ++index;
    sequence[index] = '[';
    ++index;

    if (number >= 10U)
    {
        sequence[index] = (char)('0' + (unsigned char)(number / 10U));
        ++index;
    }

    sequence[index] = (char)('0' + (unsigned char)(number % 10U));
    ++index;
    sequence[index] = 'G';
    ++index;
    sequence[index] = '\0';

    SerialWriteString(sequence);
}

/*
 * Echoes a backspace upon both devices as an erasure.
 *
 * A backspace moves the cursor without erasing, upon the display and upon a
 * serial terminal alike, so an echo that wrote it alone would leave the
 * character the user meant to delete upon the screen and then overwrite it with
 * whatever was typed next. The erasure is the echo's business, not the driver's:
 * the sequence steps back, writes a space over the character, and steps back
 * again to stand where the character was.
 *
 * The display is driven first, and what it did then determines what is sent to
 * the serial line: the driver may have refused to move, the cursor standing at
 * the erase limit, or it may have crossed into the row above, which is a
 * movement the serial terminal must be told about explicitly.
 */
static void KernelEchoBackspace(void)
{
    size_t row;
    size_t column;
    size_t resulting_row;
    size_t resulting_column;

    VgaCursorPosition(&row, &column);
    VgaWriteString("\b \b");
    VgaCursorPosition(&resulting_row, &resulting_column);

    if ((resulting_row == row) && (resulting_column == column))
    {
        /* The cursor stood at the erase limit; there was nothing to erase. */
        return;
    }

    if (resulting_row == row)
    {
        SerialWriteString("\b \b");
        return;
    }

    /* CUU, CSI A, moves the terminal's cursor up one line without erasing. */
    SerialWriteString("\x1B[A");
    KernelSerialCursorToColumn(resulting_column);
    SerialPutCharacter(' ');
    KernelSerialCursorToColumn(resulting_column);
}

/*
 * Echoes characters from the keyboard and from the serial line upon both output
 * devices, indefinitely.
 *
 * This is the one thing the self-tests cannot establish. They drive the decoder
 * directly, which exercises the whole of scan code set 1 upon a machine at which
 * nobody is typing, but leaves the path from the physical key to the decoder —
 * the controller raising its request line, the interrupt controller routing it,
 * the handler reading the data port — asserted only as configured state. The
 * same is true of the serial receiver, whose self-test can say what becomes of a
 * character that has arrived but not that one arrives. Here both paths are
 * exercised in full, by the only means available: a person, or a virtual machine
 * monitor, actually sending a character.
 *
 * The loop halts the processor between keystrokes rather than spinning. The
 * sequence STI followed immediately by HLT is the correct idiom and not merely a
 * compact one: Intel SDM, Volume 2B, "STI", provides that the effect of the
 * instruction is delayed by one instruction, so the HLT is executed before any
 * interrupt can be taken. Were the order reversed, or a further instruction
 * placed between them, a keystroke arriving in the interval would be serviced
 * and the processor would then halt with nothing left to wake it.
 */
static _Noreturn void KernelEchoLoop(void)
{
    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString("\nEcho loop. Characters typed upon the console or sent "
                      "upon COM1 appear upon both.\n");
    KernelWriteString("A backspace erases, and crosses to the line above.\n");
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * Everything printed up to this point is the kernel's, and everything after
     * it is the user's. The erase limit records the boundary, which is what
     * permits the backspace to cross from one row to the row above: the driver
     * cannot tell the boot log from a line of input, and would otherwise consume
     * the log a character at a time.
     */
    VgaSetEraseLimit();

    for (;;)
    {
        char character;

        __asm__ __volatile__("sti; hlt");

        /*
         * The serial line is a source of characters equally, now that its
         * receiver is interrupt-driven. Echoing them exercises the receive path
         * end to end, which the self-test cannot: it needs a character actually
         * arriving from outside the machine.
         */
        while (SerialReadCharacter(&character))
        {
            const char text[2] = { character, '\0' };

            if (character == '\b')
            {
                KernelEchoBackspace();
            }
            else
            {
                KernelWriteString(text);
            }
        }

        while (KeyboardReadCharacter(&character))
        {
            /* A one-character string, the output routines taking no other form. */
            const char text[2] = { character, '\0' };

            if (character == '\b')
            {
                KernelEchoBackspace();
            }
            else
            {
                KernelWriteString(text);
            }
        }
    }
}

void KernelMain(uint32_t multiboot_information_address, uint32_t multiboot_magic)
{
    /*
     * The serial port is initialised first, so that any subsequent failure is
     * recorded even should the display be unavailable or illegible. A negative
     * result is not an error; it indicates only that no adapter is present.
     */
    (void)SerialInitialise(SERIAL_COM1_PORT);

    VgaInitialise();

    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString(OXYS_SYSTEM_NAME "\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    KernelWriteString("Version " OXYS_VERSION_STRING
                      ", x86_64, long mode active, higher-half kernel.\n");

    /*
     * The magic value is validated a second time here, the first validation
     * having been performed in 32-bit mode by boot/boot.asm. The repetition
     * guards against a transfer of control that bypasses the assembly entry
     * point, and costs nothing measurable.
     */
    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC)
    {
        KernelPanic("The boot loader is not Multiboot2 compliant.");
    }

    KernelWriteString("Multiboot2 magic value verified.\n");

    /*
     * The display is tested first because it is the instrument through which
     * every later test reports, and because it needs nothing that is not already
     * established at this point.
     */
    KernelVerifyVga();

    /*
     * Reduce the Multiboot2 structure to the neutral description upon which the
     * remainder of the kernel depends. A failure here is unrecoverable: without a
     * memory map the physical frame allocator cannot be constructed, and without
     * that the kernel can do nothing further.
     */
    if (!BootInformationParseMultiboot2(multiboot_information_address,
                                        &KernelBootInformation))
    {
        KernelPanic("The Multiboot2 boot information structure could not be parsed.");
    }

    BootInformationReport(&KernelBootInformation);

    PhysicalMemoryInitialise(&KernelBootInformation);
    PhysicalMemoryReport();
    KernelVerifyFrameAllocator();

    PagingInitialise(&KernelBootInformation);
    PagingReport();
    KernelVerifyPaging();

    KernelVirtualInitialise();
    KernelHeapInitialise();
    KernelVerifyAllocators();
    KernelVirtualReport();
    KernelHeapReport();

    FrameReferenceInitialise();
    KernelVerifyReferenceCounting();
    PhysicalMemoryReport();

    /*
     * The table established by boot/boot.asm resides at a low address that
     * sub-task 2.3 unmapped. It must be replaced before any interrupt gate is
     * installed, because delivering an interrupt obliges the processor to read
     * the descriptor named by the gate's selector.
     */
    GdtInitialise();
    GdtReport();

    IdtInitialise();
    InterruptInitialise();
    ExceptionInitialise();
    IdtReport();
    InterruptReport();
    KernelVerifyIdt();
    KernelVerifyInterruptStubs();
    KernelVerifyDispatcher();
    KernelVerifyExceptions();
    KernelVerifyCopyOnWrite();
    KernelVerifyAddressSpaces();

    /*
     * The controllers are remapped only now, after the exception handlers exist.
     * Remapping them earlier would have placed device vectors clear of the
     * exceptions without providing anywhere for them to go.
     */
    PicInitialise();
    KernelVerifyPic();
    PicReport();

    /*
     * The timer is the first device to claim a request line, and therefore the
     * first proof that the whole path from a device to a handler is sound.
     */
    PitInitialise(PIT_DEFAULT_FREQUENCY);
    KernelVerifyPit();
    PitReport();

    /*
     * The keyboard is the last device of Phase 3. A machine without one is not
     * in error, so the return value is recorded rather than acted upon; the
     * report and the self-test both accommodate its absence.
     */
    (void)KeyboardInitialise();
    KernelVerifyKeyboard();
    KeyboardReport();

    /*
     * Phase 4 begins here. The serial adapter was configured in the first
     * instruction of this function, so that a failure anywhere above would be
     * recorded; only now, the interrupt controller existing, can it be promoted
     * from polling to interrupts and become a driver rather than a routine.
     */
    SerialActivateInterrupts();
    KernelVerifySerial();
    SerialReport();
    VgaReport();

    /*
     * The bus is enumerated once every device driven so far is working, so that
     * a failure in the enumeration is reported through channels already proved.
     * Nothing is claimed or configured here; the enumeration only establishes
     * what the machine contains, which the disk driver then searches.
     */
    (void)PciInitialise();
    KernelVerifyPci();
    PciReport();

    /*
     * The disk is the last device of this phase and the first whose failure is
     * silent in the ordinary case: a driver that reads the wrong sector returns
     * data, and data that arrived is indistinguishable from data that is right
     * until something tries to interpret it.
     */
    (void)AtaInitialise();
    KernelVerifyAta();
    AtaReport();

    /*
     * Every disk found presents itself through the generic layer, which is what
     * everything above will address it by. The layer is asserted against a
     * device of memory rather than against a disk: the machine this is verified
     * upon has no disk, and one that has holds data a self-test must not write.
     */
    (void)AtaRegisterBlockDevices();
    KernelVerifyBlock();
    BlockReport();

    /*
     * The cache stands between the block layer and everything that will read a
     * medium. Its storage comes from the kernel heap, so it cannot be prepared
     * until that exists, which it has since Phase 2.
     */
    (void)BufferInitialise();
    KernelVerifyBuffer();
    BufferReport();

    /*
     * Phase 5 begins here. Nothing is mounted: the superblock of any volume the
     * machine actually carries is read and reported, and the parser itself is
     * asserted against a volume composed in memory.
     */
    KernelVerifyExt2();
    KernelReportVolumes();

    /*
     * Sub-task 5.8. The layer is asserted against the two volumes of memory it
     * composes, which is where a mount, a descriptor and a mount point crossing
     * can be stated exactly; only then is a volume the machine actually carries
     * mounted at the root, read-only unless the operator permitted otherwise.
     */
    KernelVerifyVfs();
    KernelMountRootVolume();

    PicReport();
    InterruptReport();
    PagingReport();
    AddressSpaceReport();

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    KernelWriteString("Phase 5 initialisation complete: an EXT2 volume is "
                      "mounted through a virtual filesystem layer.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * With a keyboard the kernel has something to wait for, and waiting for it
     * demonstrates the interrupt path end to end. Without one there is nothing
     * further to do.
     */
    if (KeyboardIsPresent())
    {
        KernelEchoLoop();
    }

    KernelWriteString("No further subsystems are implemented. Halting.\n");

    KernelHalt();
}
