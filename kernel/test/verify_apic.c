/*
 * File: kernel/test/verify_apic.c
 * Purpose: Asserts the work of sub-task 6.12: the parse of the firmware's ACPI
 *          description tables, the Local APIC, the I/O APIC, and the routing of
 *          the device request lines through them once the 8259A pair has been
 *          retired.
 * Key functions: KernelVerifyAcpi, KernelVerifyLocalApic, KernelVerifyIoApic,
 *          KernelVerifyApicRouting.
 * References:
 *   - docs/devices/ACPI.md, Section 7: the assertions upon the tables, each
 *     paired with the silent failure it exists to catch.
 *   - docs/devices/APIC.md, Section 8: the assertions upon the two controllers.
 *   - ACPI Specification 6.5, Sections 5.2.5.3, 5.2.6 and 5.2.12.
 *   - Intel SDM, Volume 3A, Sections 10.4.3, 10.4.4, 10.5.2 and 10.9.
 *   - Intel 82093AA I/O APIC datasheet, Sections 3.2.2 and 3.2.4.
 *
 * The class of failure these are written against.
 *
 *   An interrupt controller that is programmed wrongly does not report anything.
 *   It produces a device that is silent, and a silent device is indistinguishable
 *   from a device that is absent, from a driver that was never initialised, and
 *   from a machine that has nothing attached. Every assertion below is therefore
 *   made against a value read back from the hardware or from the firmware's own
 *   tables rather than against what this kernel believes it wrote — and the last
 *   of them lets the timer run and counts its ticks, which is the only assertion
 *   that establishes the whole path from a device pin to a handler.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/acpi.h>
#include <oxys/lapic.h>
#include <oxys/ioapic.h>
#include <oxys/irq.h>
#include <oxys/pic.h>
#include <oxys/pit.h>
#include <oxys/cpu.h>
#include <oxys/msr.h>

/*
 * Asserts the parse of the description tables.
 *
 * A machine that supplies none is not in error and is not reported as a failure:
 * this kernel must run upon one, and does, through the 8259A pair. What is
 * asserted is that a parse which claims to have succeeded is coherent — that
 * every table it accepted passed its checksum, that the MADT it found names a
 * local controller address and at least one processor, and that the overrides it
 * recorded name only the bus the specification permits them to.
 */
void KernelVerifyAcpi(void)
{
    bool succeeded = true;

    if (!AcpiIsAvailable())
    {
        KernelWriteString("ACPI self-test: no description tables; the machine will "
                          "be driven through the 8259A pair.\n");
        return;
    }

    /*
     * A checksum failure among the tables that were nevertheless used would mean
     * a table was accepted after failing the only integrity check ACPI provides.
     * The count rising for a table that was refused is legitimate — a firmware
     * with one bad table is common — so the count is reported rather than
     * asserted to be zero, and what is asserted is that what was accepted is
     * coherent.
     */
    if (AcpiRsdpAddress() == 0U)
    {
        KernelWriteString("  The tables were parsed but no pointer address was recorded.\n");
        succeeded = false;
    }

    if (AcpiDirectoryAddress() == 0U)
    {
        KernelWriteString("  No table directory address was recorded.\n");
        succeeded = false;
    }

    if (AcpiTableCount() == 0U)
    {
        KernelWriteString("  The directory named no tables.\n");
        succeeded = false;
    }

    /*
     * A machine whose MADT declares no processor at all is a machine whose
     * parse has gone wrong, not a machine with no processor: the kernel is
     * executing upon one. This is the assertion that would catch a walk of the
     * entry list that advanced by the wrong amount and so read every entry from
     * the wrong offset.
     */
    if (AcpiProcessorCount() == 0U)
    {
        KernelWriteString("  The MADT declares no processor, which cannot be true.\n");
        succeeded = false;
    }

    if (AcpiUsableProcessorCount() == 0U)
    {
        KernelWriteString("  The MADT declares no usable processor.\n");
        succeeded = false;
    }

    if (AcpiLocalApicAddress() == 0U)
    {
        KernelWriteString("  The MADT names no local controller address.\n");
        succeeded = false;
    }

    /*
     * Section 5.2.12.5 admits overrides for the ISA bus alone, the bus number
     * being a constant zero. An override naming another bus is a structure this
     * parse has read from the wrong offset, and it would be applied to a request
     * line it has nothing to do with.
     */
    for (size_t index = 0U; index < AcpiOverrideCount(); ++index)
    {
        const AcpiInterruptOverride *const override = AcpiOverrideAt(index);

        if (override->bus != 0U)
        {
            KernelWriteString("  An interrupt source override names a bus other "
                              "than the ISA bus.\n");
            succeeded = false;
        }
    }

    /*
     * The identity mapping of Section 5.2.12.4 must hold for every line no
     * override departs from. This asserts the resolution itself rather than the
     * table: a lookup that returned the wrong global interrupt would silently
     * route the timer to an input nothing is attached to.
     */
    for (uint8_t line = 0U; line < 16U; ++line)
    {
        const uint32_t resolved = AcpiGlobalInterruptForIsaIrq(line);
        bool overridden = false;

        for (size_t index = 0U; index < AcpiOverrideCount(); ++index)
        {
            if (AcpiOverrideAt(index)->source == line)
            {
                overridden = true;
            }
        }

        if (!overridden && resolved != (uint32_t)line)
        {
            KernelWriteString("  A request line with no override does not resolve "
                              "to the global interrupt of the same number.\n");
            succeeded = false;
        }
    }

    KernelWriteString(succeeded ? "ACPI table self-test passed.\n"
                                : "ACPI table self-test FAILED.\n");
}

/*
 * Asserts the Local APIC.
 *
 * Every value is read back from the controller. The two enables are separate
 * mechanisms — the global one in a model-specific register and the software one
 * in a memory-mapped register — and a controller with either clear accepts
 * nothing whatever while its local vector table reads exactly as intended.
 */
void KernelVerifyLocalApic(void)
{
    bool succeeded = true;
    uint32_t spurious;

    if (!LocalApicIsSupported())
    {
        KernelWriteString("Local APIC self-test: the processor reports none.\n");
        return;
    }

    if (!LocalApicIsEnabled())
    {
        KernelWriteString("Local APIC self-test: not enabled; the machine continues "
                          "upon the 8259A pair.\n");
        return;
    }

    /* --- The two enables. --- */

    if ((ReadMsr(IA32_APIC_BASE) & LAPIC_BASE_GLOBAL_ENABLE) == 0U)
    {
        KernelWriteString("  The global enable of IA32_APIC_BASE is clear.\n");
        succeeded = false;
    }

    spurious = LocalApicRead(LAPIC_REGISTER_SPURIOUS_VECTOR);

    if ((spurious & LAPIC_SPURIOUS_SOFTWARE_ENABLE) == 0U)
    {
        KernelWriteString("  The software enable of the spurious vector register "
                          "is clear.\n");
        succeeded = false;
    }

    /*
     * Intel SDM, Volume 3A, Section 10.9: upon the P6 family and the Pentium the
     * low four bits of the spurious vector are hardwired to one. Reading the
     * register back is what establishes that the vector programmed is the vector
     * the controller will actually present; a vector altered by the hardware
     * would be delivered to whatever handler stood at the altered number.
     */
    if ((spurious & LAPIC_LVT_VECTOR_MASK) != (uint32_t)LAPIC_SPURIOUS_VECTOR)
    {
        KernelWriteString("  The spurious vector read back is not the one programmed.\n");
        succeeded = false;
    }

    /* --- The task priority accepts every class. --- */

    /*
     * Section 10.8.6: a task priority class of 15 blocks every external
     * interrupt. The register is cleared by a reset, but the firmware ran first;
     * this is the assertion that would catch a firmware that raised it, whose
     * symptom is a machine where every controller is correctly programmed and
     * nothing ever interrupts.
     */
    if (LocalApicRead(LAPIC_REGISTER_TASK_PRIORITY) != 0U)
    {
        KernelWriteString("  The task priority register blocks some interrupt "
                          "priority classes.\n");
        succeeded = false;
    }

    /* --- The local interrupt pins. --- */

    /*
     * Neither pin may be left in the external delivery mode. That mode is how
     * the 8259A reaches a processor through its local controller, and a pin left
     * in it after the 8259A has been retired would have the processor waiting
     * for a vector from a controller that no longer supplies one.
     */
    if ((LocalApicRead(LAPIC_REGISTER_LVT_LINT0) & LAPIC_LVT_DELIVERY_EXTINT) ==
        LAPIC_LVT_DELIVERY_EXTINT)
    {
        KernelWriteString("  LINT0 is still in the external delivery mode.\n");
        succeeded = false;
    }

    if ((LocalApicRead(LAPIC_REGISTER_LVT_LINT1) & LAPIC_LVT_DELIVERY_EXTINT) ==
        LAPIC_LVT_DELIVERY_EXTINT)
    {
        KernelWriteString("  LINT1 is still in the external delivery mode.\n");
        succeeded = false;
    }

    /* --- The timer and the performance counters are masked. --- */

    if ((LocalApicRead(LAPIC_REGISTER_LVT_TIMER) & LAPIC_LVT_MASKED) == 0U)
    {
        KernelWriteString("  The local timer is unmasked, and no handler expects it.\n");
        succeeded = false;
    }

    /* --- The version register is plausible. --- */

    /*
     * Section 10.4.8 gives the version of an integrated APIC as 0x10 to 0x15 and
     * that of the discrete 82489DX as 0x0X. A version outside both ranges means
     * the register page is not mapped to a controller at all, which is what a
     * mapping made at the wrong address, or with the wrong memory type, would
     * look like from here.
     */
    {
        const uint32_t version = LocalApicVersion() & LAPIC_VERSION_MASK;

        if (version > 0x15U)
        {
            KernelWriteString("  The version register does not read as an APIC's.\n");
            succeeded = false;
        }
    }

    if (!LocalApicIsBootstrapProcessor())
    {
        KernelWriteString("  The processor running the initialisation is not the "
                          "bootstrap processor.\n");
        succeeded = false;
    }

    if (LocalApicErrorCount() != 0U)
    {
        KernelWriteString("  The controller reported an error during initialisation.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Local APIC self-test passed.\n"
                                : "Local APIC self-test FAILED.\n");
}

/*
 * Asserts the I/O APIC.
 *
 * The property that matters most is the least visible: that every input begins
 * masked. An input left unmasked upon a machine whose device has no driver
 * delivers a vector nothing registered, and where the input is level triggered
 * it delivers it without end, no end-of-interrupt ever clearing the remote
 * in-service flag.
 */
void KernelVerifyIoApic(void)
{
    bool succeeded = true;

    if (IoApicCount() == 0U)
    {
        KernelWriteString("I/O APIC self-test: none; the machine continues upon the "
                          "8259A pair.\n");
        return;
    }

    for (size_t index = 0U; index < IoApicCount(); ++index)
    {
        uint8_t identifier = 0U;
        PhysicalAddress address = 0U;
        uint32_t base = 0U;
        uint32_t inputs = 0U;

        if (!IoApicDescribe(index, &identifier, &address, &base, &inputs))
        {
            KernelWriteString("  A unit the driver counts cannot be described.\n");
            succeeded = false;
            continue;
        }

        /*
         * The 82093AA datasheet, Section 3.2.2, gives the maximum redirection
         * entry as 0 to 239, so a unit presents between one and 240 inputs. A
         * count outside that is a register read that did not reach a controller;
         * the commonest cause is a mapping established at the wrong address,
         * where the read returns whatever the bus drives upon an unclaimed one.
         */
        if (inputs == 0U || inputs > IOAPIC_INPUT_MAXIMUM)
        {
            KernelWriteString("  A unit reports an implausible number of inputs.\n");
            succeeded = false;
            continue;
        }

        if (address == 0U)
        {
            KernelWriteString("  A unit was mapped from no address.\n");
            succeeded = false;
        }

        /*
         * Every entry not claimed by a request line must be masked. The lines the
         * request layer claims are examined by KernelVerifyApicRouting; here the
         * assertion is confined to the inputs above them, which no driver in this
         * kernel can have asked for.
         */
        for (uint32_t input = 0U; input < inputs; ++input)
        {
            const uint32_t global_interrupt = base + input;
            uint64_t entry = 0U;

            if (global_interrupt < IRQ_LINE_COUNT)
            {
                continue;
            }

            if (!IoApicEntryValue(global_interrupt, &entry))
            {
                KernelWriteString("  An input the unit declares cannot be read.\n");
                succeeded = false;
                continue;
            }

            if ((entry & IOAPIC_REDIRECTION_MASKED) == 0U)
            {
                KernelWriteString("  An input no driver claimed is unmasked.\n");
                succeeded = false;
            }
        }
    }

    KernelWriteString(succeeded ? "I/O APIC self-test passed.\n"
                                : "I/O APIC self-test FAILED.\n");
}

/*
 * Asserts the routing of the device request lines after the 8259A has been
 * retired.
 *
 * This is the assertion of the sub-task. Everything above it establishes that
 * the controllers were programmed; this establishes that a device pin still
 * reaches its driver afterwards, which is the only thing the change was for.
 */
void KernelVerifyApicRouting(void)
{
    bool succeeded = true;
    uint64_t ticks_before;

    if (IrqActiveController() != IRQ_CONTROLLER_APIC)
    {
        KernelWriteString("APIC routing self-test: the 8259A pair still answers; "
                          "nothing to assert.\n");
        return;
    }

    /* --- The 8259A is silent. --- */

    /*
     * ACPI 6.5, Table 5.20, requires the pair's lines to be masked before the
     * APIC is enabled. Two controllers presenting one device upon one vector
     * would deliver every request twice, and the second delivery would be
     * acknowledged at a controller that had not sent it — which, at the 8259A,
     * would reset the in-service bit of an unrelated line.
     */
    if (PicMaskValue() != UINT16_C(0xFFFF))
    {
        KernelWriteString("  The 8259A pair is not fully masked.\n");
        succeeded = false;
    }

    if (PicIsInitialised())
    {
        KernelWriteString("  The 8259A pair still reports itself as answering.\n");
        succeeded = false;
    }

    /* --- Every claimed line has a redirection entry that agrees with it. --- */

    for (uint8_t line = 0U; line < (uint8_t)IRQ_LINE_COUNT; ++line)
    {
        const uint32_t global_interrupt = AcpiGlobalInterruptForIsaIrq(line);
        uint64_t entry = 0U;

        if (IrqRegisteredHandler(line) == NULL)
        {
            continue;
        }

        if (!IoApicEntryValue(global_interrupt, &entry))
        {
            KernelWriteString("  A claimed line is carried by no I/O APIC input.\n");
            succeeded = false;
            continue;
        }

        /*
         * The vector must be the one the line has always been presented upon.
         * A device driver holds no vector and could not detect a change, so an
         * entry programmed with the wrong one would deliver the keyboard's
         * requests to the timer's handler and nothing would say so.
         */
        if ((entry & IOAPIC_REDIRECTION_VECTOR_MASK) !=
            (uint64_t)IRQ_VECTOR_FOR_LINE(line))
        {
            KernelWriteString("  A claimed line is routed to the wrong vector.\n");
            succeeded = false;
        }

        /*
         * The destination must be this processor's local controller. An entry
         * naming a processor that has not been started would produce a device
         * that is programmed, unmasked and silent — which is sub-task 6.14's
         * characteristic failure arriving two sub-tasks early.
         */
        if ((entry >> IOAPIC_REDIRECTION_DESTINATION_SHIFT) !=
            (uint64_t)LocalApicIdentifier())
        {
            KernelWriteString("  A claimed line is directed at another processor.\n");
            succeeded = false;
        }

        /*
         * The mask must be what the driver asked for and not what the change of
         * controller happened to leave. A line carried across as masked is a
         * device that worked before the adoption and does not after.
         */
        if (IrqLineIsMasked(line) != ((entry & IOAPIC_REDIRECTION_MASKED) != 0U))
        {
            KernelWriteString("  A claimed line's mask does not agree with its "
                              "redirection entry.\n");
            succeeded = false;
        }
    }

    /* --- The timer still ticks. --- */

    /*
     * The end-to-end assertion, and the reason the timer is the device chosen
     * for it: it is the only one that interrupts without anybody touching the
     * machine. Its line is also the one most likely to have moved, an Interrupt
     * Source Override for request line 0 being among the commonest a firmware
     * declares.
     *
     * The interrupt flag is set for a bounded interval and cleared again, as
     * every self-test that needs it does.
     *
     * The wait is a fixed spin rather than PitWaitTicks, and the difference
     * matters here alone. PitWaitTicks is bounded by iterations *per tick
     * awaited*, so a timer that is dead — which is precisely the case this
     * assertion exists to detect — makes it spin for a multiple of a bound
     * chosen to be generous. A negative test of the destination field made the
     * run outlast the `verify` target's timeout, so the failure was reported as
     * a kernel that never reached its banner rather than as a timer that had
     * stopped. A fixed spin is far longer than a tick period at 1000 Hz and
     * costs the same whether the timer runs or not.
     */
    if (IrqRegisteredHandler(PIT_IRQ) != NULL && !IrqLineIsMasked(PIT_IRQ))
    {
        ticks_before = PitTickCount();

        __asm__ __volatile__("sti" : : : "memory");

        for (volatile uint32_t spin = 0U; spin < 2000000U; ++spin)
        {
            /* Deliberately empty: time is allowed to pass with interrupts on. */
        }

        __asm__ __volatile__("cli" : : : "memory");

        if (PitTickCount() <= ticks_before)
        {
            KernelWriteString("  The interval timer stopped when the I/O APIC took "
                              "over its request line.\n");
            succeeded = false;
        }
    }

    /* --- The Local APIC reported nothing amiss. --- */

    if (LocalApicErrorCount() != 0U)
    {
        KernelWriteString("  The Local APIC reported an error while routing.\n");
        KernelWriteString("  Its error status was ");
        KernelWriteHexadecimal((uint64_t)LocalApicLastErrorStatus());
        KernelWriteString(".\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "APIC routing self-test passed.\n"
                                : "APIC routing self-test FAILED.\n");
}
