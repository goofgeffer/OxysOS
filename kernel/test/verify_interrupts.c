/*
 * File: kernel/test/verify_interrupts.c
 * Purpose: Asserts the interrupt apparatus of Phase 3: the descriptor table
 *          and the gates within it, the 256 stubs and the uniform trap frame
 *          they construct, the routing of a vector to its registered handler,
 *          and the exception handlers together with the fault they resolve.
 * Key functions: KernelVerifyIdt, KernelVerifyInterruptStubs,
 *          KernelVerifyDispatcher, KernelVerifyExceptions.
 * References:
   - docs/design/INTERRUPTS.md, Section 6: the table pairing every assertion
 *     with the silent failure it catches.
 *   - Intel SDM, Volume 3A, Section 6.12.1 and Figure 6-4: the stack frame the
 *     processor constructs, against which the frame the stubs build is checked.
 *
 * INT3 is used to raise an interrupt deliberately because the breakpoint
 * exception is a trap and not a fault: it reports the state after the
 * instruction, so returning resumes at the one following. A fault would be
 * restarted and would re-enter without end.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/idt.h>
#include <oxys/interrupts.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>
#include <oxys/memory.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>

/*
 * Confirms that the interrupt descriptor table was loaded as intended.
 *
 * The table register is read back with SIDT rather than trusting the value that
 * was written to it. A LIDT that silently failed, or an operand corrupted by
 * padding the compiler inserted, would produce a table register that does not
 * describe the table, and nothing else would reveal it until the first interrupt
 * escalated to a reset.
 */
void KernelVerifyIdt(void)
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
void KernelVerifyInterruptStubs(void)
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
void KernelVerifyDispatcher(void)
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
void KernelVerifyExceptions(void)
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
