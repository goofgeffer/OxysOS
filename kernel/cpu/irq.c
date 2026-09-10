/*
 * File: kernel/cpu/irq.c
 * Purpose: Implements the interrupt request layer: the table of handlers claimed
 *          by line number rather than by vector, the routing of a request to the
 *          driver that claimed it, the signalling of completion at whichever
 *          controller delivered it, and the retirement of the 8259A pair in
 *          favour of the I/O APIC and the Local APIC.
 * Key functions: IrqInitialise, IrqAdoptApic, IrqInstallHandler,
 *          IrqRemoveHandler, IrqRegisteredHandler, IrqMaskLine, IrqUnmaskLine,
 *          IrqLineIsMasked, IrqActiveController, IrqReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 6.2: the vector range the request lines are
 *     placed in.
 *   - Intel SDM, Volume 3A, Section 10.8.5: the Local APIC's end-of-interrupt.
 *   - Intel 8259A datasheet, section "OPERATION COMMAND WORDS (OCWS)": the
 *     non-specific end-of-interrupt, which drivers/pic/pic.c performs.
 *   - ACPI Specification 6.5, Sections 5.2.12.4 and 5.2.12.5: the identity
 *     mapping of the request lines onto the first sixteen global system
 *     interrupts, and the overrides that depart from it.
 *   - docs/design/INTERRUPTS.md, Section 10: the design this file implements.
 *
 * Why the mask state is recorded here as well as in the controller.
 *
 *   A controller's mask register is the truth while that controller is
 *   answering, and this layer reads it rather than its own copy whenever it is
 *   asked. The recorded state exists for one moment only: the instant the 8259A
 *   is retired, at which the old controller's registers are about to be
 *   abandoned and the new controller's have never been written. Something must
 *   carry the answer across, and it must be a record of what each driver asked
 *   for rather than a copy of the old registers — a line the 8259A was
 *   withholding for a reason of its own must not become a line the I/O APIC
 *   withholds for ever.
 *
 * Concurrency. The handler table and the recorded mask state are unsynchronised.
 * Until the interrupt flag is set there is one flow of control; from sub-task
 * 6.13 there is a spinlock the registration of a handler and the change of a
 * mask both require, an interrupt handler and an application processor each
 * being able to enter either. It has not been applied here; sub-task 6.14 is
 * what makes either contended.
 */

#include <oxys/irq.h>
#include <oxys/pic.h>
#include <oxys/lapic.h>
#include <oxys/ioapic.h>
#include <oxys/acpi.h>
#include <oxys/kernel.h>

/* What each line was claimed by, and what its claimant asked for. */
static InterruptHandler IrqHandlerTable[IRQ_LINE_COUNT];
static const char *IrqHandlerNames[IRQ_LINE_COUNT];
static bool IrqLinePermitted[IRQ_LINE_COUNT];

/* Which controller is answering. */
static IrqController IrqControllerInUse = IRQ_CONTROLLER_NONE;

/* Accounting. */
static uint64_t IrqRequestsRouted;
static uint64_t IrqSpuriousRequests;
static uint64_t IrqUnclaimedRequests;

/*
 * Receives every vector the request lines are presented upon, routes the request
 * to the driver that claimed the line, and signals completion.
 *
 * A line with no registered handler is counted and acknowledged rather than
 * treated as fatal. The device is real and its request must be released: under
 * the 8259A an unreleased request withholds every line of lower priority, and
 * under the I/O APIC a level-triggered input whose remote in-service flag is
 * never cleared will present the same request without end. The condition is not
 * an error in the kernel but a device the kernel has not been taught to drive.
 */
static void IrqRoute(TrapFrame *frame)
{
    const uint8_t line = (uint8_t)(frame->vector - (uint64_t)IRQ_VECTOR_BASE);
    InterruptHandler handler;

    /*
     * The 8259A's spurious request is recognised before anything else, and is
     * the one case in which nothing whatever must be acknowledged. The Local
     * APIC has no counterpart here: its spurious request is delivered upon a
     * vector of its own, which drivers/apic/lapic.c registers a handler for, and
     * so never reaches this routine at all.
     */
    if (IrqControllerInUse == IRQ_CONTROLLER_8259A && PicRequestIsSpurious(line))
    {
        ++IrqSpuriousRequests;
        return;
    }

    ++IrqRequestsRouted;

    handler = IrqHandlerTable[line];

    if (handler != NULL)
    {
        handler(frame);
    }
    else
    {
        ++IrqUnclaimedRequests;
    }

    if (IrqControllerInUse == IRQ_CONTROLLER_APIC)
    {
        LocalApicSignalEndOfInterrupt();
    }
    else
    {
        PicSendEndOfInterrupt(line);
    }
}

void IrqInitialise(void)
{
    for (size_t line = 0U; line < IRQ_LINE_COUNT; ++line)
    {
        IrqHandlerTable[line] = NULL;
        IrqHandlerNames[line] = NULL;
        IrqLinePermitted[line] = false;
    }

    IrqRequestsRouted = 0U;
    IrqSpuriousRequests = 0U;
    IrqUnclaimedRequests = 0U;

    PicInitialise();

    for (uint8_t line = 0U; line < (uint8_t)IRQ_LINE_COUNT; ++line)
    {
        InterruptRegisterHandler(IRQ_VECTOR_FOR_LINE(line), IrqRoute,
                                 "device request line");
    }

    IrqControllerInUse = IRQ_CONTROLLER_8259A;
}

uint32_t IrqGlobalInterruptForLine(uint8_t line)
{
    if (line >= IRQ_LINE_COUNT || !AcpiIsAvailable())
    {
        return 0U;
    }

    return AcpiGlobalInterruptForIsaIrq(line);
}

/*
 * Whether a request line has an interrupt input of its own under the APIC.
 *
 * An override does not move a line so much as exchange two of them, and the line
 * displaced is left with nothing. QEMU's tables are the ordinary case: they
 * declare that ISA request 0 — the interval timer — is carried by global system
 * interrupt 2, so input 2 belongs to the timer and request line 2 is carried by
 * no input at all.
 *
 * That is not an anomaly to be worked around; it is what the firmware said, and
 * request line 2 is the cascade of the 8259A, which is not a device line under
 * any controller. But it must be honoured, because the resolution is not
 * injective: line 0 and line 2 both resolve to global interrupt 2, and whichever
 * was programmed second would take the input.
 *
 * The failure that caused this to be written was exactly that. The lines were
 * programmed in ascending order, so line 2 overwrote line 0: the timer's input
 * was left presenting line 2's vector, masked, and the machine lost its tick.
 * The self-test of sub-task 6.12 caught it upon the first run.
 *
 * An explicit declaration therefore wins over the implicit identity mapping of
 * ACPI 6.5, Section 5.2.12.4: a line owns its global interrupt unless some other
 * line was expressly declared to be carried by it.
 */
static bool IrqLineOwnsItsInput(uint8_t line)
{
    const uint32_t global_interrupt = AcpiGlobalInterruptForIsaIrq(line);

    for (size_t index = 0U; index < AcpiOverrideCount(); ++index)
    {
        const AcpiInterruptOverride *const override = AcpiOverrideAt(index);

        if (override->global_interrupt == global_interrupt &&
            override->source != line)
        {
            return false;
        }
    }

    return true;
}

/*
 * Programmes the redirection table entry for one line and applies the mask its
 * claimant asked for.
 *
 * An unclaimed line is programmed as well as a claimed one. The entry costs
 * nothing while it is masked, and programming every line at the moment of
 * adoption means a driver that claims a line afterwards needs only to unmask it,
 * rather than needing this layer to remember whether the entry was ever written.
 */
static bool IrqProgrammeApicLine(uint8_t line)
{
    const uint32_t global_interrupt = AcpiGlobalInterruptForIsaIrq(line);

    if (!IrqLineOwnsItsInput(line))
    {
        return false;
    }

    if (!IoApicRouteGlobalInterrupt(global_interrupt, IRQ_VECTOR_FOR_LINE(line),
                                    LocalApicIdentifier(),
                                    AcpiIsaIrqIsActiveLow(line),
                                    AcpiIsaIrqIsLevelTriggered(line)))
    {
        return false;
    }

    return IoApicSetMask(global_interrupt, !IrqLinePermitted[line]);
}

bool IrqAdoptApic(void)
{
    if (!LocalApicIsEnabled() || IoApicCount() == 0U)
    {
        KernelWriteString("Interrupt requests: the APIC is not available; the 8259A "
                          "pair continues to answer.\n");
        return false;
    }

    /*
     * The 8259A is silenced first. ACPI 6.5, Table 5.20, states the requirement
     * plainly for a machine declaring PCAT_COMPAT: "The 8259 vectors must be
     * disabled (that is, masked) when enabling the ACPI APIC operation." Two
     * controllers presenting the same device upon the same vector would deliver
     * every request twice, and the second delivery would be acknowledged at a
     * controller that had not sent it.
     */
    PicDisable();

    for (uint8_t line = 0U; line < (uint8_t)IRQ_LINE_COUNT; ++line)
    {
        if (!IrqProgrammeApicLine(line))
        {
            /*
             * A line no input carries is reported and left alone. It is not a
             * reason to abandon the adoption: the two causes are a line whose
             * input another line was declared to have taken, and a machine whose
             * first unit begins above zero, and the lines that do have inputs are
             * still better served by them than by a controller that has just been
             * masked.
             *
             * A driver that has claimed such a line is a different matter, and is
             * said so, because its device has just gone silent.
             */
            KernelWriteString("Interrupt requests: no I/O APIC input carries line ");
            KernelWriteDecimal((uint64_t)line);
            KernelWriteString(IrqHandlerTable[line] != NULL
                                  ? ", which a driver has claimed.\n"
                                  : ".\n");
        }
    }

    IrqControllerInUse = IRQ_CONTROLLER_APIC;

    return true;
}

void IrqInstallHandler(uint8_t line, InterruptHandler handler, const char *name)
{
    if (line >= IRQ_LINE_COUNT)
    {
        return;
    }

    IrqHandlerTable[line] = handler;
    IrqHandlerNames[line] = name;
}

void IrqRemoveHandler(uint8_t line)
{
    if (line >= IRQ_LINE_COUNT)
    {
        return;
    }

    IrqMaskLine(line);
    IrqHandlerTable[line] = NULL;
    IrqHandlerNames[line] = NULL;
}

InterruptHandler IrqRegisteredHandler(uint8_t line)
{
    return (line < IRQ_LINE_COUNT) ? IrqHandlerTable[line] : NULL;
}

const char *IrqLineName(uint8_t line)
{
    return (line < IRQ_LINE_COUNT) ? IrqHandlerNames[line] : NULL;
}

void IrqMaskLine(uint8_t line)
{
    if (line >= IRQ_LINE_COUNT)
    {
        return;
    }

    IrqLinePermitted[line] = false;

    if (IrqControllerInUse == IRQ_CONTROLLER_APIC)
    {
        /*
         * The ownership is consulted here as well as at the adoption. Masking
         * line 2 upon a machine whose timer was declared to be carried by global
         * interrupt 2 would otherwise mask the timer, which is the same defect
         * as the one IrqLineOwnsItsInput exists to prevent, arriving by a
         * different route.
         */
        if (IrqLineOwnsItsInput(line))
        {
            (void)IoApicSetMask(AcpiGlobalInterruptForIsaIrq(line), true);
        }
    }
    else
    {
        PicMaskLine(line);
    }
}

void IrqUnmaskLine(uint8_t line)
{
    if (line >= IRQ_LINE_COUNT)
    {
        return;
    }

    IrqLinePermitted[line] = true;

    if (IrqControllerInUse == IRQ_CONTROLLER_APIC)
    {
        if (IrqLineOwnsItsInput(line))
        {
            (void)IoApicSetMask(AcpiGlobalInterruptForIsaIrq(line), false);
        }
    }
    else
    {
        PicUnmaskLine(line);
    }
}

bool IrqLineIsMasked(uint8_t line)
{
    if (line >= IRQ_LINE_COUNT)
    {
        return true;
    }

    if (IrqControllerInUse == IRQ_CONTROLLER_APIC)
    {
        /*
         * A line no input carries is reported as masked, that being the truthful
         * answer: nothing will deliver it. Reading the input another line took
         * would report that line's state under this line's name.
         */
        if (!IrqLineOwnsItsInput(line))
        {
            return true;
        }

        return IoApicGlobalInterruptIsMasked(AcpiGlobalInterruptForIsaIrq(line));
    }

    return PicLineIsMasked(line);
}

IrqController IrqActiveController(void)
{
    return IrqControllerInUse;
}

const char *IrqControllerName(IrqController controller)
{
    switch (controller)
    {
    case IRQ_CONTROLLER_8259A:
        return "the 8259A pair";
    case IRQ_CONTROLLER_APIC:
        return "the I/O APIC and the Local APIC";
    case IRQ_CONTROLLER_NONE:
    default:
        return "none";
    }
}

uint64_t IrqRequestCount(void)
{
    return IrqRequestsRouted;
}

uint64_t IrqSpuriousCount(void)
{
    return IrqSpuriousRequests;
}

uint64_t IrqUnclaimedCount(void)
{
    return IrqUnclaimedRequests;
}

void IrqReport(void)
{
    size_t claimed = 0U;

    for (size_t line = 0U; line < IRQ_LINE_COUNT; ++line)
    {
        if (IrqHandlerTable[line] != NULL)
        {
            ++claimed;
        }
    }

    KernelWriteString("Interrupt requests: answered by ");
    KernelWriteString(IrqControllerName(IrqControllerInUse));
    KernelWriteString(", vectors ");
    KernelWriteDecimal((uint64_t)IRQ_VECTOR_BASE);
    KernelWriteString(" to ");
    KernelWriteDecimal((uint64_t)IRQ_VECTOR_BASE + IRQ_LINE_COUNT - 1U);
    KernelWriteString(", lines claimed ");
    KernelWriteDecimal((uint64_t)claimed);
    KernelWriteString(".\n");

    /*
     * The claimed lines are named individually, and the global system interrupt
     * each is carried by is named with them. A machine whose keyboard has gone
     * silent is diagnosed in one line by a report that says line 1 is claimed by
     * the PS/2 keyboard, carried by global interrupt 1, and unmasked — and by no
     * amount of reading of a count.
     */
    for (uint8_t line = 0U; line < (uint8_t)IRQ_LINE_COUNT; ++line)
    {
        if (IrqHandlerTable[line] == NULL)
        {
            continue;
        }

        KernelWriteString("  Line ");
        KernelWriteDecimal((uint64_t)line);
        KernelWriteString(" (vector ");
        KernelWriteDecimal((uint64_t)IRQ_VECTOR_FOR_LINE(line));
        KernelWriteString("): ");
        KernelWriteString(IrqHandlerNames[line] != NULL ? IrqHandlerNames[line]
                                                        : "unnamed");

        if (IrqControllerInUse == IRQ_CONTROLLER_APIC)
        {
            KernelWriteString(", global interrupt ");
            KernelWriteDecimal((uint64_t)AcpiGlobalInterruptForIsaIrq(line));
        }

        KernelWriteString(IrqLineIsMasked(line) ? ", masked.\n" : ", unmasked.\n");
    }

    KernelWriteString("Interrupt requests: routed ");
    KernelWriteDecimal(IrqRequestsRouted);
    KernelWriteString(", unclaimed ");
    KernelWriteDecimal(IrqUnclaimedRequests);
    KernelWriteString(", spurious ");
    KernelWriteDecimal(IrqSpuriousRequests);
    KernelWriteString(".\n");
}
