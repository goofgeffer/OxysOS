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
 * From sub-task 6.9 a fault taken simultaneously upon two processors would
 * interleave two reports; the diagnostic path will require a lock of its own,
 * one that a handler may take even when the general kernel lock is held.
 */

#include <oxys/exceptions.h>
#include <oxys/interrupts.h>
#include <oxys/cpu.h>
#include <oxys/paging.h>
#include <oxys/kernel.h>

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
    ExceptionReportState(frame, ReadCr2());
    KernelPanic("An unrecoverable processor exception was raised.");
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
    KernelPanic("An unresolved page fault was raised.");
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
