/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel. It validates the
 *          state established by the boot loader, initialises in dependency order
 *          every subsystem of Phases 1 to 3, asserts the properties of each by a
 *          boot-time self-test, and then either enters the keyboard echo loop,
 *          where a keyboard is present, or halts the processor where none is.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelKeyboardEcho,
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
     */
    if (frame->rip < (uint64_t)(uintptr_t)&KernelMain ||
        frame->rip >= (uint64_t)KERNEL_VIRTUAL_BASE + UINT64_C(0x40000000))
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
 * Echoes typed characters upon both output devices, indefinitely.
 *
 * This is the one thing the self-tests cannot establish. They drive the decoder
 * directly, which exercises the whole of scan code set 1 upon a machine at which
 * nobody is typing, but leaves the path from the physical key to the decoder —
 * the controller raising its request line, the interrupt controller routing it,
 * the handler reading the data port — asserted only as configured state. Here
 * that path is exercised in full, by the only means available: a person, or a
 * virtual machine monitor, actually pressing a key.
 *
 * The loop halts the processor between keystrokes rather than spinning. The
 * sequence STI followed immediately by HLT is the correct idiom and not merely a
 * compact one: Intel SDM, Volume 2B, "STI", provides that the effect of the
 * instruction is delayed by one instruction, so the HLT is executed before any
 * interrupt can be taken. Were the order reversed, or a further instruction
 * placed between them, a keystroke arriving in the interval would be serviced
 * and the processor would then halt with nothing left to wake it.
 */
static _Noreturn void KernelKeyboardEcho(void)
{
    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString("\nKeyboard echo. Type upon the console; characters appear "
                      "here and upon COM1.\n");
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    for (;;)
    {
        char character;

        __asm__ __volatile__("sti; hlt");

        while (KeyboardReadCharacter(&character))
        {
            /* A one-character string, the output routines taking no other form. */
            const char text[2] = { character, '\0' };

            KernelWriteString(text);
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

    PicReport();
    InterruptReport();
    PagingReport();
    AddressSpaceReport();

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    KernelWriteString("Phase 3 initialisation complete.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * With a keyboard the kernel has something to wait for, and waiting for it
     * demonstrates the interrupt path end to end. Without one there is nothing
     * further to do.
     */
    if (KeyboardIsPresent())
    {
        KernelKeyboardEcho();
    }

    KernelWriteString("No further subsystems are implemented. Halting.\n");

    KernelHalt();
}
