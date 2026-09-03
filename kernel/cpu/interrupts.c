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
 * Status. Sub-task 3.3 introduces the dispatch table and the registration
 * interface implemented here. Sub-task 3.4 supplies the exception handlers
 * proper; until it does, an exception for which no handler is registered
 * remains fatal, for the reason given in the commentary upon InterruptDispatch.
 *
 * Concurrency. The dispatch table is written during initialisation and read
 * thereafter, which is safe without a lock so long as registration precedes the
 * enabling of interrupts. From sub-task 6.8 a handler registered while other
 * processors are running requires the write to be ordered against their reads.
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

/* The number of interrupts dispatched, and a copy of the most recent frame.
 *
 * The copy is a diagnostic aid. It costs 176 bytes of copying per interrupt,
 * which is immaterial at the rates seen so far but should be reconsidered once
 * the timer of sub-task 3.6 begins to fire. */
static uint64_t InterruptDispatchCount;
static TrapFrame InterruptMostRecentFrame;

/* The handler registered for each vector, and its name. A null entry denotes a
 * vector for which no handler is registered. */
static InterruptHandler InterruptHandlerTable[IDT_ENTRY_COUNT];
static const char *InterruptHandlerNames[IDT_ENTRY_COUNT];

/* The number of times each vector has been dispatched. */
static uint64_t InterruptVectorCounts[IDT_ENTRY_COUNT];

/* The number of dispatches that found no handler and were not fatal. */
static uint64_t InterruptUnhandledDispatchCount;

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

    KernelWriteString(" (");
    KernelWriteString(InterruptVectorName(frame->vector));
    KernelWriteString(")");

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

void InterruptRegisterHandler(uint8_t vector, InterruptHandler handler,
                              const char *name)
{
    InterruptHandlerTable[vector] = handler;
    InterruptHandlerNames[vector] = name;
}

void InterruptUnregisterHandler(uint8_t vector)
{
    InterruptHandlerTable[vector] = NULL;
    InterruptHandlerNames[vector] = NULL;
}

InterruptHandler InterruptRegisteredHandler(uint8_t vector)
{
    return InterruptHandlerTable[vector];
}

uint64_t InterruptVectorCount(uint8_t vector)
{
    return InterruptVectorCounts[vector];
}

uint64_t InterruptUnhandledCount(void)
{
    return InterruptUnhandledDispatchCount;
}

const char *InterruptVectorName(uint64_t vector)
{
    if (vector < 32U)
    {
        return InterruptMnemonics[vector];
    }

    return "(user-defined interrupt)";
}

/*
 * Receives control from the common stub and routes the interrupt to the handler
 * registered for its vector.
 *
 * Where no handler is registered the treatment depends upon the vector, and the
 * distinction is not arbitrary. Intel SDM, Volume 3A, Section 6.5, classifies
 * the architecture-defined exceptions as faults, traps and aborts. A fault
 * reports the state before the offending instruction and restarts it upon
 * return, so returning without having removed the cause re-enters the same
 * exception immediately and without end. An unhandled exception is therefore
 * fatal: halting with a diagnosis is the only outcome that yields any
 * information.
 *
 * A vector at or above 32 is not architecture-defined. Nothing about it is
 * restarted, so an unregistered one is counted and ignored. This is the correct
 * treatment of a spurious interrupt, which the 8259A of sub-task 3.5 is known to
 * deliver.
 */
void InterruptDispatch(TrapFrame *frame)
{
    const uint8_t vector = (uint8_t)frame->vector;
    InterruptHandler handler;

    ++InterruptDispatchCount;
    ++InterruptVectorCounts[vector];
    InterruptMostRecentFrame = *frame;

    handler = InterruptHandlerTable[vector];

    if (handler != NULL)
    {
        handler(frame);
        return;
    }

    if (frame->vector < 32U)
    {
        KernelWriteString("\nUnhandled processor exception:\n");
        InterruptReportFrame(frame);
        KernelPanic("An exception was raised for which no handler is registered.");
    }

    ++InterruptUnhandledDispatchCount;
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

    size_t registered = 0U;

    for (size_t vector = 0U; vector < IDT_ENTRY_COUNT; ++vector)
    {
        if (InterruptHandlerTable[vector] != NULL)
        {
            ++registered;
        }
    }

    KernelWriteString("Interrupt stubs: ");
    KernelWriteDecimal((uint64_t)IDT_ENTRY_COUNT);
    KernelWriteString(" installed, of which ");
    KernelWriteDecimal((uint64_t)error_code_vectors);
    KernelWriteString(" receive a processor error code.\n");

    KernelWriteString("Interrupt dispatcher: handlers registered ");
    KernelWriteDecimal((uint64_t)registered);
    KernelWriteString(", dispatched ");
    KernelWriteDecimal(InterruptDispatchCount);
    KernelWriteString(", unhandled ");
    KernelWriteDecimal(InterruptUnhandledDispatchCount);
    KernelWriteString(".\n");
}
