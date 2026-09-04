/*
 * File: kernel/cpu/exceptions.c
 * Purpose: Implements the handlers for the architecture-defined exceptions,
 *          together with the diagnostic report that each fatal exception emits
 *          before the machine is halted.
 * Key functions: ExceptionInitialise, ExceptionReportState,
 *          ExceptionPageFaultHandler, ExceptionFatalHandler,
 *          ExceptionBreakpointHandler, ExceptionDecodePageFaultError,
 *          ExceptionDecodeSelectorError.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 6.5: exceptions are faults, traps or aborts. A fault reports the
 *     state before the offending instruction and restarts it upon return; a trap
 *     reports the state after it; an abort permits no reliable resumption. This
 *     classification determines which handlers here may return.
 *   - Intel SDM, Volume 3A, Section 6.13 and Figure 6-6: the selector-form error
 *     code, and the rule that the handler must remove the error code before
 *     returning, IRET not popping it.
 *   - Intel SDM, Volume 3A, Section 6.15 and Figure 6-9: the page-fault error
 *     code and the loading of CR2 with the faulting linear address.
 *   - Intel SDM, Volume 3A, Section 2.5: the control registers reported below.
 *
 * Concurrency. These handlers write to the shared output devices without a lock.
 * From sub-task 6.13 a fault taken simultaneously upon two processors would
 * interleave two reports; the diagnostic path will require a lock of its own,
 * one that a handler may take even when the general kernel lock is held.
 */

#include <oxys/exceptions.h>
#include <oxys/interrupts.h>
#include <oxys/cpu.h>
#include <oxys/paging.h>
#include <oxys/idt.h>
#include <oxys/tss.h>
#include <oxys/kernel.h>
#include <oxys/faultscreen.h>

/* The number of quadwords of stack reproduced in a diagnostic report. */
#define EXCEPTION_STACK_WORDS 8U

/*
 * Emits one named 64-bit value, three to a line, so that a full register dump
 * occupies five lines rather than fifteen.
 */
static void ExceptionReportRegister(const char *name, uint64_t value)
{
    KernelWriteString(name);
    KernelWriteString(" ");
    KernelWriteHexadecimal(value);
    KernelWriteString("  ");
}

/*
 * Decodes the page-fault error code, whose format is unlike that of every other
 * exception. Refer to Intel SDM, Volume 3A, Figure 6-9.
 */
static void ExceptionDecodePageFaultError(uint64_t error_code)
{
    KernelWriteString("    cause: ");

    /*
     * The present flag distinguishes the two fundamentally different causes. A
     * clear flag means no translation existed; a set flag means a translation
     * existed and the access violated its permissions. The remedies differ
     * entirely, which is why this is stated first.
     */
    KernelWriteString((error_code & PAGE_FAULT_PRESENT) != 0U
                          ? "protection violation"
                          : "page not present");

    KernelWriteString((error_code & PAGE_FAULT_WRITE) != 0U ? ", write" : ", read");
    KernelWriteString((error_code & PAGE_FAULT_USER) != 0U
                          ? ", user mode"
                          : ", supervisor mode");

    if ((error_code & PAGE_FAULT_INSTRUCTION) != 0U)
    {
        KernelWriteString(", instruction fetch");
    }

    if ((error_code & PAGE_FAULT_RESERVED_BIT) != 0U)
    {
        KernelWriteString(", reserved bit set in a paging-structure entry");
    }

    if ((error_code & PAGE_FAULT_PROTECTION_KEY) != 0U)
    {
        KernelWriteString(", protection-key violation");
    }

    if ((error_code & PAGE_FAULT_SGX) != 0U)
    {
        KernelWriteString(", SGX access-control violation");
    }

    KernelWriteString("\n");
}

/*
 * Decodes the selector-form error code presented by #TS, #NP, #SS and #GP, per
 * Intel SDM, Volume 3A, Section 6.13.
 */
static void ExceptionDecodeSelectorError(uint64_t error_code)
{
    KernelWriteString("    cause: ");

    /*
     * Section 6.13 provides that a null error code, all bits clear save possibly
     * EXT, indicates that the fault was not caused by a reference to a specific
     * segment, or that a null selector was referenced.
     */
    if ((error_code & ~SELECTOR_ERROR_EXTERNAL) == 0U)
    {
        KernelWriteString("no specific segment, or a null selector");
    }
    else
    {
        KernelWriteString("selector index ");
        KernelWriteDecimal((error_code >> SELECTOR_ERROR_INDEX_SHIFT) &
                           SELECTOR_ERROR_INDEX_MASK);

        if ((error_code & SELECTOR_ERROR_IDT) != 0U)
        {
            KernelWriteString(" in the IDT");
        }
        else if ((error_code & SELECTOR_ERROR_LDT) != 0U)
        {
            KernelWriteString(" in the LDT");
        }
        else
        {
            KernelWriteString(" in the GDT");
        }
    }

    if ((error_code & SELECTOR_ERROR_EXTERNAL) != 0U)
    {
        KernelWriteString(", raised during delivery of an external event");
    }

    KernelWriteString("\n");
}

void ExceptionReportState(const TrapFrame *frame, uint64_t fault_address)
{
    KernelWriteString("\n=== PROCESSOR EXCEPTION ===\n");

    KernelWriteString("  vector ");
    KernelWriteDecimal(frame->vector);
    KernelWriteString(": ");
    KernelWriteString(InterruptVectorName(frame->vector));
    KernelWriteString("\n");

    KernelWriteString("  error code ");
    KernelWriteHexadecimal(frame->error_code);
    KernelWriteString("\n");

    if (frame->vector == 14U)
    {
        ExceptionDecodePageFaultError(frame->error_code);
        KernelWriteString("    faulting linear address ");
        KernelWriteHexadecimal(fault_address);
        KernelWriteString("\n");
    }
    else if (InterruptVectorPushesErrorCode(frame->vector))
    {
        ExceptionDecodeSelectorError(frame->error_code);
    }

    KernelWriteString("  RIP ");
    KernelWriteHexadecimal(frame->rip);
    KernelWriteString("  CS ");
    KernelWriteHexadecimal(frame->cs);
    KernelWriteString("  RFLAGS ");
    KernelWriteHexadecimal(frame->rflags);
    KernelWriteString("\n  RSP ");
    KernelWriteHexadecimal(frame->rsp);
    KernelWriteString("  SS ");
    KernelWriteHexadecimal(frame->ss);
    KernelWriteString("\n");

    ExceptionReportRegister("  RAX", frame->rax);
    ExceptionReportRegister("RBX", frame->rbx);
    ExceptionReportRegister("RCX", frame->rcx);
    KernelWriteString("\n");
    ExceptionReportRegister("  RDX", frame->rdx);
    ExceptionReportRegister("RSI", frame->rsi);
    ExceptionReportRegister("RDI", frame->rdi);
    KernelWriteString("\n");
    ExceptionReportRegister("  RBP", frame->rbp);
    ExceptionReportRegister("R8 ", frame->r8);
    ExceptionReportRegister("R9 ", frame->r9);
    KernelWriteString("\n");
    ExceptionReportRegister("  R10", frame->r10);
    ExceptionReportRegister("R11", frame->r11);
    ExceptionReportRegister("R12", frame->r12);
    KernelWriteString("\n");
    ExceptionReportRegister("  R13", frame->r13);
    ExceptionReportRegister("R14", frame->r14);
    ExceptionReportRegister("R15", frame->r15);
    KernelWriteString("\n");

    ExceptionReportRegister("  CR0", ReadCr0());
    ExceptionReportRegister("CR2", fault_address);
    KernelWriteString("\n");
    ExceptionReportRegister("  CR3", ReadCr3());
    ExceptionReportRegister("CR4", ReadCr4());
    KernelWriteString("\n");

    /*
     * Reproduce a portion of the stack. The pointer is checked against the
     * kernel's address space first: a fault taken with a corrupt stack pointer
     * is precisely the case in which a report is most wanted, and reading
     * through the bad pointer would raise a second fault and lose the report
     * entirely.
     */
    if (frame->rsp >= KERNEL_VIRTUAL_BASE && PagingTranslate(frame->rsp) != 0U)
    {
        const uint64_t *stack = (const uint64_t *)(uintptr_t)frame->rsp;

        KernelWriteString("  stack:\n");

        for (size_t index = 0U; index < EXCEPTION_STACK_WORDS; ++index)
        {
            const VirtualAddress address = frame->rsp + (index * 8U);

            if (PagingTranslate(address) == 0U)
            {
                break;
            }

            KernelWriteString("    ");
            KernelWriteHexadecimal(address);
            KernelWriteString(": ");
            KernelWriteHexadecimal(stack[index]);
            KernelWriteString("\n");
        }
    }
    else
    {
        KernelWriteString("  stack: not reproduced; the stack pointer is unmapped.\n");
    }

    KernelWriteString("===========================\n");
}

bool ExceptionCameFromUserMode(uint64_t cs)
{
    /*
     * The low two bits of a code segment selector are its requested privilege
     * level, and for the selector the processor pushed they are the privilege
     * level the code was actually running at. Anything above zero is outside the
     * kernel.
     */
    return (cs & UINT64_C(0x03)) != 0U;
}

bool ExceptionOnlyOutsideKernel(uint64_t vector)
{
    /*
     * The alignment check, and nothing else. Intel SDM, Volume 3A, Section 6.15:
     * it requires privilege level 3, CR0.AM and RFLAGS.AC together, so no
     * arrangement of kernel code can raise one.
     */
    return vector == EXCEPTION_ALIGNMENT_CHECK;
}

ExceptionDisposition ExceptionDispositionOf(uint64_t vector, uint64_t cs)
{
    /*
     * The aborts, first and unconditionally.
     *
     * Intel SDM, Volume 3A, Section 6.5 classifies these as aborts: they permit
     * no reliable resumption, and the state they report may not describe where
     * the error occurred. The privilege level is irrelevant to them. A double
     * fault means the processor could not deliver an earlier exception, which is
     * a statement about the machine and not about a program; a machine check is
     * the hardware reporting a fault in itself.
     */
    if ((vector == EXCEPTION_DOUBLE_FAULT) || (vector == EXCEPTION_MACHINE_CHECK))
    {
        return EXCEPTION_DISPOSITION_FATAL;
    }

    /*
     * A non-maskable interrupt is not a program's mistake either. It is memory
     * parity, a watchdog, or a hardware error the platform could not defer, and
     * terminating whatever happened to be running when it arrived would blame
     * the wrong thing.
     */
    if (vector == EXCEPTION_NON_MASKABLE)
    {
        return EXCEPTION_DISPOSITION_FATAL;
    }

    /*
     * The two traps that may be resumed from. A trap reports the state *after*
     * the instruction, so returning does not re-enter it; see the handlers
     * below, which is where they are actually dealt with.
     */
    if ((vector == EXCEPTION_BREAKPOINT) || (vector == EXCEPTION_OVERFLOW))
    {
        return EXCEPTION_DISPOSITION_RESUME;
    }

    /*
     * An invalid task state segment and an absent segment are faults in the
     * descriptor tables, and the descriptor tables are the kernel's own data.
     * Whoever tripped over one, the structure that is wrong was built here, so
     * terminating the program that happened to reach it would leave the machine
     * running with the same malformed descriptor and the next program would meet
     * it too.
     */
    if ((vector == EXCEPTION_INVALID_TSS) || (vector == EXCEPTION_SEGMENT_ABSENT))
    {
        return EXCEPTION_DISPOSITION_FATAL;
    }

    /*
     * Everything else is decided by where it happened, and this is the whole of
     * the distinction.
     *
     * A divide error, an invalid opcode, a general-protection fault, an
     * unresolved page fault, an alignment check: each of these is a mistake made
     * by the code that raised it. Raised at privilege level 3 the mistake
     * belongs to a program, and the program is what should end. Raised at
     * privilege level 0 the mistake was the kernel's own, and there is no
     * smaller thing to abandon than the machine.
     *
     * A vector this kernel does not expect at all — one Intel reserves, or one
     * introduced by an extension it does not implement — reaches the same place
     * and is fatal when it comes from the kernel, for the same reason: a kernel
     * that has met an exception it has no account of does not know what state it
     * is in.
     */
    if (ExceptionCameFromUserMode(cs))
    {
        return EXCEPTION_DISPOSITION_TERMINATE;
    }

    return EXCEPTION_DISPOSITION_FATAL;
}

/*
 * The treatment of a fault that belongs to the program that caused it.
 *
 * There are no programs. Until sub-task 6.10 runs code at privilege level 3
 * there is nothing outside the kernel to raise such a fault and nothing to
 * terminate, so reaching this means the machine is at a privilege level this
 * kernel does not know it has — which is itself a condition it cannot continue
 * from, and is reported as one rather than being quietly ignored.
 *
 * From Phase 7 this becomes the ordinary path: the process is destroyed, its
 * address space released, and the scheduler picks another. The machine is not
 * halted and no screen is drawn.
 */
static void ExceptionTerminateProgram(TrapFrame *frame)
{
    KernelWriteString("\nA fault was raised outside the kernel, at privilege level ");
    KernelWriteDecimal(frame->cs & UINT64_C(0x03));
    KernelWriteString(".\n");
    KernelWriteString("This fault belongs to the program that caused it, and from Phase 7 "
                      "that program\nwould be terminated and the machine would carry on. "
                      "There are no programs yet,\nso there is nothing to terminate and "
                      "this cannot be continued from.\n");
}

/*
 * The handler for every exception from which no recovery is presently possible.
 *
 * Most architecture-defined exceptions are faults, which restart the offending
 * instruction upon return. Returning without having removed the cause would
 * re-enter the same exception without end, so the machine is halted after the
 * state has been reported.
 */
static void ExceptionFatalHandler(TrapFrame *frame)
{
    const ExceptionDisposition disposition =
        ExceptionDispositionOf(frame->vector, frame->cs);

    ExceptionReportState(frame, ReadCr2());

    /*
     * Only a fault the kernel cannot survive draws a screen.
     *
     * The screen says, in effect, that the system has stopped, and it must not
     * be shown for a fault that ought to have cost one program and no more. A
     * divide by zero in a user program is the plainest example: it is that
     * program's mistake, its consequence is that program's death, and a
     * full-screen page announcing the end of the machine would be a lie about
     * what happened.
     */
    if (disposition != EXCEPTION_DISPOSITION_FATAL)
    {
        ExceptionTerminateProgram(frame);
        KernelPanic("A fault outside the kernel was raised before any program exists.");
        return;
    }

    /*
     * The screen is drawn after the report and not instead of it. The report is
     * the record and goes to every diagnostic path; the screen is a summary
     * composed for this particular exception, for a person standing at a machine
     * whose adapter the boot loader put into a graphics mode and who can
     * therefore read neither the text console nor a serial port they do not
     * have. Where there is no framebuffer this does nothing at all.
     */
    FaultScreenShowException(frame, ReadCr2());
    KernelPanic("An unrecoverable processor exception was raised within the kernel.");
}

/*
 * The handler for the page-fault exception.
 *
 * CR2 is read as the first action. Intel SDM, Volume 3A, Section 6.15, warns
 * that a further page fault may occur during execution of this handler and that
 * the register must be saved before that can happen.
 *
 * A write to a page that is present is the only fault that can be a
 * copy-on-write fault, and resolution is attempted for exactly that case. The
 * two conditions are tested here rather than left to the resolution routine so
 * that the far commoner faults - an absent page, or a read - do not walk the
 * paging structures at all.
 *
 * Where the fault is resolved the handler returns, and the offending instruction
 * is restarted against the corrected mapping. Any other fault is reported and
 * the machine halted, since a fault whose cause has not been removed would
 * re-enter without end.
 */
static void ExceptionPageFaultHandler(TrapFrame *frame)
{
    const uint64_t fault_address = ReadCr2();

    if ((frame->error_code & PAGE_FAULT_WRITE) != 0U &&
        (frame->error_code & PAGE_FAULT_PRESENT) != 0U &&
        PagingResolveCopyOnWriteFault(fault_address))
    {
        return;
    }

    ExceptionReportState(frame, fault_address);

    /*
     * An unresolved page fault raised outside the kernel is the program's, and
     * from Phase 7 costs the program alone. Raised within the kernel it is a
     * mapping this kernel failed to make or a pointer it computed wrongly, and
     * there is nothing smaller than the machine to abandon.
     */
    if (ExceptionDispositionOf(frame->vector, frame->cs) !=
        EXCEPTION_DISPOSITION_FATAL)
    {
        ExceptionTerminateProgram(frame);
        KernelPanic("A page fault outside the kernel was raised before any program "
                    "exists.");
        return;
    }

    FaultScreenShowException(frame, fault_address);
    KernelPanic("An unresolved page fault was raised within the kernel.");
}

/*
 * The handler for the breakpoint exception.
 *
 * The breakpoint is a trap: it reports the state after the INT3 instruction, so
 * returning resumes at the instruction following and does not re-enter. It is
 * therefore recorded and execution continues, which makes INT3 usable as a
 * diagnostic marker in kernel code that has no debugger attached.
 */
static void ExceptionBreakpointHandler(TrapFrame *frame)
{
    KernelWriteString("Breakpoint at RIP ");
    KernelWriteHexadecimal(frame->rip);
    KernelWriteString(".\n");
}

/*
 * The handler for the overflow exception, which is likewise a trap and so may
 * be resumed from.
 */
static void ExceptionOverflowHandler(TrapFrame *frame)
{
    KernelWriteString("Overflow trap at RIP ");
    KernelWriteHexadecimal(frame->rip);
    KernelWriteString(".\n");
}

void ExceptionInitialise(void)
{
    /*
     * Every architecture-defined vector receives the fatal handler by default,
     * so that no exception can reach the dispatcher's own unregistered path and
     * be reported with less detail than is available here.
     */
    for (uint8_t vector = 0U; vector < 32U; ++vector)
    {
        InterruptRegisterHandler(vector, ExceptionFatalHandler,
                                 InterruptVectorName(vector));
    }

    /* The two traps from which execution may safely resume. */
    InterruptRegisterHandler(3U, ExceptionBreakpointHandler, "breakpoint");
    InterruptRegisterHandler(4U, ExceptionOverflowHandler, "overflow");

    /* The page fault has its own handler, the error code and CR2 requiring
     * treatment that the general report does not provide. */
    InterruptRegisterHandler(14U, ExceptionPageFaultHandler, "page fault");
}

bool ExceptionInstallInterruptStacks(void)
{
    /*
     * The double fault is delivered upon a stack of its own.
     *
     * It is raised when the processor cannot deliver an earlier exception, and
     * by far the commonest reason it cannot is that the stack was the thing that
     * went wrong — exhausted, unmapped, or pointing at nothing. Delivering it
     * upon that same stack means the processor cannot push its frame either, and
     * a fault taken while delivering a double fault is not a third exception but
     * a shutdown: the machine resets with nothing written anywhere.
     *
     * This project has already met one. The account is in
     * docs/design/INTERRUPTS.md, Section 5, where an unmapped descriptor table
     * turned a general-protection exception into a triple fault and a reboot
     * loop, and the cause had to be found from QEMU's own trace because the
     * kernel emitted nothing. With an entry of the interrupt stack table the
     * same sequence would have produced a double fault upon a sound stack, and
     * the handler would have printed the frame.
     *
     * This is applied after every gate has been installed and after the task
     * state segment has been loaded, and it is a separate step for that reason:
     * the index selects an entry of a segment, and a segment that did not yet
     * exist would furnish a stack pointer of zero.
     */
    return IdtSetGateStack(EXCEPTION_DOUBLE_FAULT, (uint8_t)TSS_IST_DOUBLE_FAULT);
}
