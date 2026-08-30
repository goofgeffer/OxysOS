/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel. It validates the
 *          state established by the boot loader, initialises the early
 *          diagnostic output devices, presents the system identification banner,
 *          and halts the processor pending the subsystems of subsequent phases.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelWriteString,
 *          KernelWriteHexadecimal, KernelWriteDecimal.
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
 */

#include <oxys/kernel.h>
#include <oxys/bootinfo.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>
#include <oxys/heap.h>
#include <oxys/gdt.h>
#include <oxys/idt.h>
#include <oxys/interrupts.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>
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
                          ? "Exception self-test passed: a read-only page faulted and was resolved.\n"
                          : "Exception self-test FAILED.\n");
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
    InterruptReport();

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    KernelWriteString("Phase 3.4 initialisation complete.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    KernelWriteString("No further subsystems are implemented. Halting.\n");

    KernelHalt();
}
