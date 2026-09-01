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
    size_t live_before;

    /* --- The page allocator. --- */

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
    }

    /* --- The heap. --- */

    live_before = 0U;
    (void)live_before;

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

static bool KernelMemoryDeviceRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    uint8_t *const destination = (uint8_t *)buffer;
    const size_t offset = (size_t)block * BLOCK_SIZE_DEFAULT;

    (void)context;

    for (size_t index = 0U; index < ((size_t)count * BLOCK_SIZE_DEFAULT); ++index)
    {
        destination[index] = KernelMemoryDeviceStore[offset + index];
    }

    return true;
}

static bool KernelMemoryDeviceWrite(void *context, uint64_t block, uint32_t count,
                                    const void *buffer)
{
    const uint8_t *const source = (const uint8_t *)buffer;
    const size_t offset = (size_t)block * BLOCK_SIZE_DEFAULT;

    (void)context;

    for (size_t index = 0U; index < ((size_t)count * BLOCK_SIZE_DEFAULT); ++index)
    {
        KernelMemoryDeviceStore[offset + index] = source[index];
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
            Ext2ReportVolume(&superblock, device->name);
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
static void KernelSetVolumeHalf(size_t offset, uint16_t value)
{
    uint8_t *const field = &KernelMemoryDeviceStore[EXT2_SUPERBLOCK_OFFSET + offset];

    field[0] = (uint8_t)(value & 0xFFU);
    field[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void KernelSetVolumeWord(size_t offset, uint32_t value)
{
    uint8_t *const field = &KernelMemoryDeviceStore[EXT2_SUPERBLOCK_OFFSET + offset];

    field[0] = (uint8_t)(value & 0xFFU);
    field[1] = (uint8_t)((value >> 8) & 0xFFU);
    field[2] = (uint8_t)((value >> 16) & 0xFFU);
    field[3] = (uint8_t)((value >> 24) & 0xFFU);
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

    KernelSetVolumeWord(EXT2_OFFSET_INODE_COUNT, 16U);
    KernelSetVolumeWord(EXT2_OFFSET_BLOCK_COUNT, 128U);
    KernelSetVolumeWord(EXT2_OFFSET_RESERVED_BLOCKS, 6U);
    KernelSetVolumeWord(EXT2_OFFSET_FREE_BLOCKS, 100U);
    KernelSetVolumeWord(EXT2_OFFSET_FREE_INODES, 5U);
    KernelSetVolumeWord(EXT2_OFFSET_FIRST_DATA_BLOCK, 1U);
    KernelSetVolumeWord(EXT2_OFFSET_LOG_BLOCK_SIZE, 0U);
    KernelSetVolumeWord(EXT2_OFFSET_LOG_FRAGMENT_SIZE, 0U);
    KernelSetVolumeWord(EXT2_OFFSET_BLOCKS_PER_GROUP, 8192U);
    KernelSetVolumeWord(EXT2_OFFSET_FRAGS_PER_GROUP, 8192U);
    KernelSetVolumeWord(EXT2_OFFSET_INODES_PER_GROUP, 16U);
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
        (superblock.inode_count != 16U) || (superblock.block_count != 128U) ||
        (superblock.reserved_block_count != 6U) || (superblock.free_block_count != 100U) ||
        (superblock.free_inode_count != 5U) || (superblock.first_data_block != 1U) ||
        (superblock.blocks_per_group != 8192U) || (superblock.inodes_per_group != 16U) ||
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

    PicReport();
    InterruptReport();
    PagingReport();
    AddressSpaceReport();

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    KernelWriteString("Phase 4 initialisation complete; Phase 5 begun to sub-task 5.1.\n");

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
