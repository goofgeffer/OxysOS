/*
 * File: kernel/cpu/interrupts.c
 * Purpose: Installs the 256 interrupt stubs into the descriptor table and
 *          receives control from them, recording the frame and reporting it.
 * Key functions: InterruptInitialise, InterruptDispatch,
 *          InterruptVectorPushesErrorCode, InterruptReport,
 *          InterruptReportFrame, InterruptCount, InterruptLastFrame.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Table 6-1: the mnemonics and the error-code column reproduced below.
 *   - Intel SDM, Volume 3A, Section 6.12.1 and Figure 6-4: the frame the
 *     processor pushes in 64-bit mode.
 *   - Intel SDM, Volume 3A, Section 6.5: exceptions are classified as faults,
 *     traps or aborts. A fault reports the state before the instruction and the
 *     instruction is restarted upon return; a trap reports the state after it.
 *     The distinction governs whether returning from a handler that has taken no
 *     corrective action re-enters the same exception.
 *
 * Status. The dispatcher here is provisional and belongs to sub-task 3.2, whose
 * purpose is the stubs and the uniform frame. Sub-task 3.3 replaces it with a
 * dispatch table and registered handlers, and sub-task 3.4 supplies the
 * exception handlers proper.
 */

#include <oxys/interrupts.h>
#include <oxys/idt.h>
#include <oxys/kernel.h>

/*
 * The addresses of the stubs, defined in kernel/cpu/interrupt_stubs.asm.
 */
extern const uint64_t InterruptStubTable[IDT_ENTRY_COUNT];

/*
 * The frame is twenty-two quadwords: fifteen general-purpose registers, the
 * vector, the error code, and the five the processor pushes. A discrepancy here
 * would mean the structure had acquired padding or a member, and every field the
 * dispatcher read would be displaced.
 */
_Static_assert(sizeof(TrapFrame) == (22U * 8U),
               "The trap frame must be exactly twenty-two quadwords.");

_Static_assert(TRAP_FRAME_REGISTER_COUNT == 15U,
               "The common stub preserves fifteen general-purpose registers.");

/* The number of interrupts dispatched, and a copy of the most recent frame. */
static uint64_t InterruptDispatchCount;
static TrapFrame InterruptMostRecentFrame;

/*
 * The mnemonics of the architecture-defined exceptions, per Intel SDM,
 * Volume 3A, Table 6-1. A vector with no assigned mnemonic is named as reserved.
 */
static const char *const InterruptMnemonics[32] = {
    "#DE Divide Error",
    "#DB Debug Exception",
    "NMI Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR BOUND Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun (reserved)",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "(Intel reserved)",
    "#MF x87 Floating-Point Error",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point Exception",
    "#VE Virtualisation Exception",
    "#CP Control Protection Exception",
    "(Intel reserved)",
    "(Intel reserved)",
    "(Intel reserved)",
    "(Intel reserved)",
    "(Intel reserved)",
    "(Intel reserved)",
    "(Intel reserved)",
    "#VC VMM Communication Exception",
    "#SX Security Exception",
    "(Intel reserved)"
};

bool InterruptVectorPushesErrorCode(uint64_t vector)
{
    /*
     * Per Intel SDM, Volume 3A, Table 6-1. Vectors 21, 29 and 30 are included
     * for the reasons set out in the header comment of
     * kernel/cpu/interrupt_stubs.asm; this function and the assembly must agree,
     * and the self-test compares them.
     */
    switch (vector)
    {
    case 8U:  /* #DF, error code always zero. */
    case 10U: /* #TS */
    case 11U: /* #NP */
    case 12U: /* #SS */
    case 13U: /* #GP */
    case 14U: /* #PF */
    case 17U: /* #AC, error code always zero. */
    case 21U: /* #CP */
    case 29U: /* #VC */
    case 30U: /* #SX */
        return true;
    default:
        return false;
    }
}

void InterruptInitialise(void)
{
    for (size_t vector = 0U; vector < IDT_ENTRY_COUNT; ++vector)
    {
        /*
         * Every gate is an interrupt gate at descriptor privilege level zero.
         * The gate type clears the interrupt flag upon entry, so a handler is
         * not itself interrupted before it has saved its state. Privilege level
         * zero means user code cannot raise these vectors with INT n; the
         * vectors that user code is to be permitted, such as a system call, are
         * given level three when they are introduced in Phase 6.
         */
        IdtSetGate((uint8_t)vector,
                   InterruptStubTable[vector],
                   (uint8_t)(IDT_ATTRIBUTE_PRESENT | IDT_ATTRIBUTE_DPL_0 |
                             IDT_GATE_TYPE_INTERRUPT));
    }
}

void InterruptReportFrame(const TrapFrame *frame)
{
    KernelWriteString("  vector ");
    KernelWriteDecimal(frame->vector);

    if (frame->vector < 32U)
    {
        KernelWriteString(" (");
        KernelWriteString(InterruptMnemonics[frame->vector]);
        KernelWriteString(")");
    }

    KernelWriteString(", error code ");
    KernelWriteHexadecimal(frame->error_code);
    KernelWriteString("\n");

    KernelWriteString("  RIP ");
    KernelWriteHexadecimal(frame->rip);
    KernelWriteString("  CS ");
    KernelWriteHexadecimal(frame->cs);
    KernelWriteString("  RFLAGS ");
    KernelWriteHexadecimal(frame->rflags);
    KernelWriteString("\n");

    KernelWriteString("  RSP ");
    KernelWriteHexadecimal(frame->rsp);
    KernelWriteString("  SS ");
    KernelWriteHexadecimal(frame->ss);
    KernelWriteString("  RAX ");
    KernelWriteHexadecimal(frame->rax);
    KernelWriteString("\n");
}

void InterruptDispatch(TrapFrame *frame)
{
    ++InterruptDispatchCount;
    InterruptMostRecentFrame = *frame;

    /*
     * A fault reports the state before the faulting instruction and restarts it
     * upon return, per Intel SDM, Volume 3A, Section 6.5. Returning from one
     * without having removed its cause therefore re-enters it immediately and
     * without end. Until sub-task 3.4 supplies handlers that can either correct
     * the condition or report it usefully, an exception is fatal.
     *
     * The breakpoint and overflow exceptions are excepted because they are traps
     * rather than faults: they report the state after the instruction, so
     * returning resumes at the instruction following and does not re-enter.
     * INT3 is consequently usable to exercise this path, which is what the
     * self-test of sub-task 3.2 does.
     */
    if (frame->vector < 32U && frame->vector != 3U && frame->vector != 4U)
    {
        KernelWriteString("\nUnhandled processor exception:\n");
        InterruptReportFrame(frame);
        KernelPanic("An exception was raised for which no handler is installed.");
    }
}

uint64_t InterruptCount(void)
{
    return InterruptDispatchCount;
}

const TrapFrame *InterruptLastFrame(void)
{
    return &InterruptMostRecentFrame;
}

void InterruptReport(void)
{
    size_t error_code_vectors = 0U;

    for (size_t vector = 0U; vector < IDT_ENTRY_COUNT; ++vector)
    {
        if (InterruptVectorPushesErrorCode((uint64_t)vector))
        {
            ++error_code_vectors;
        }
    }

    KernelWriteString("Interrupt stubs: ");
    KernelWriteDecimal((uint64_t)IDT_ENTRY_COUNT);
    KernelWriteString(" installed, of which ");
    KernelWriteDecimal((uint64_t)error_code_vectors);
    KernelWriteString(" receive a processor error code.\n");
}
