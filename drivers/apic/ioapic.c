/*
 * File: drivers/apic/ioapic.c
 * Purpose: Implements the driver for the I/O Advanced Programmable Interrupt
 *          Controller: the mapping of each unit the ACPI tables declare, the
 *          indirect register pair its registers are reached through, and the
 *          programming of the redirection table entry that decides what vector
 *          an interrupt input presents, to which processor, and how.
 * Key functions: IoApicInitialise, IoApicRouteGlobalInterrupt, IoApicSetMask,
 *          IoApicGlobalInterruptIsMasked, IoApicEntryValue, IoApicDescribe,
 *          IoApicReport.
 * References:
 *   - Intel 82093AA I/O APIC datasheet (order number 290566-001), Section 3.1:
 *     IOREGSEL at offset 0x00 selects a register by its 8-bit address; IOWIN at
 *     offset 0x10 reads and writes the register so selected. "Memory references
 *     to this register are mapped to the APIC register specified by the contents
 *     of the IOREGSEL Register."
 *   - 82093AA datasheet, Section 3.2.2: IOAPICVER, whose bits 23:16 hold "the
 *     entry number (0 being the lowest entry) of the highest entry in the I/O
 *     Redirection Table. The value is equal to the number of interrupt input
 *     pins for the IOAPIC minus one."
 *   - 82093AA datasheet, Section 3.2.4: the redirection table entries at indices
 *     0x10 upward, two registers to an entry, and the meaning of every field.
 *   - Intel SDM, Volume 3A, Section 10.5.2: the local APIC treats a vector below
 *     16 as illegal and records it in the error status register, so a
 *     redirection entry must never be given one.
 *   - ACPI Specification 6.5, Sections 5.2.12.3 and 5.2.12.4: which unit carries
 *     which global system interrupts, and the identity mapping of the 8259A
 *     request lines onto the first sixteen of them.
 *
 * Why the register page is mapped uncacheable and accessed 32 bits at a time.
 *
 *   The pair IOREGSEL and IOWIN is a stateful interface: a write to the first
 *   decides what the second refers to. A cached read of IOWIN could be answered
 *   without the controller being consulted at all, which would return the value
 *   of whichever register was selected when the line was filled. The mapping is
 *   therefore cache-disabled, and every access is a single aligned 32-bit load
 *   or store, as the datasheet's register widths require.
 *
 * Concurrency. The select-then-access sequence is not atomic, and two flows of
 * control performing it upon one unit would interleave into an access of the
 * wrong register. There is one flow of control, so it is presently safe; the
 * spinlock of sub-task 6.13 exists and this sequence has not been brought under
 * it — the same obligation the 8259A's mask registers carry, and for the same
 * reason, and discharged at the same moment: sub-task 6.14.
 */

#include <oxys/ioapic.h>
#include <oxys/acpi.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>
#include <oxys/kernel.h>

/* One mapped unit. */
typedef struct IoApicUnit
{
    volatile uint8_t *registers;
    PhysicalAddress address;
    uint8_t identifier;
    uint32_t interrupt_base;
    uint32_t input_count;
} IoApicUnit;

static IoApicUnit IoApicUnits[ACPI_IO_APIC_MAXIMUM];
static size_t IoApicUnitTotal;

/*
 * Reads and writes one register of a unit.
 *
 * The index is written to IOREGSEL and the value transferred through IOWIN. The
 * two are always performed together and never left half done, because the
 * selection persists in the controller: a routine that selected a register and
 * returned without using it would change the meaning of the next access
 * somewhere else entirely.
 */
static uint32_t IoApicReadRegister(const IoApicUnit *unit, uint8_t index)
{
    *(volatile uint32_t *)(void *)(unit->registers + IOAPIC_SELECT_OFFSET) =
        (uint32_t)index;

    return *(volatile uint32_t *)(const void *)(unit->registers + IOAPIC_WINDOW_OFFSET);
}

static void IoApicWriteRegister(const IoApicUnit *unit, uint8_t index, uint32_t value)
{
    *(volatile uint32_t *)(void *)(unit->registers + IOAPIC_SELECT_OFFSET) =
        (uint32_t)index;
    *(volatile uint32_t *)(void *)(unit->registers + IOAPIC_WINDOW_OFFSET) = value;
}

/* The unit carrying a global system interrupt, or NULL where none does. */
static IoApicUnit *IoApicUnitFor(uint32_t global_interrupt)
{
    for (size_t index = 0U; index < IoApicUnitTotal; ++index)
    {
        IoApicUnit *const unit = &IoApicUnits[index];

        if (global_interrupt >= unit->interrupt_base &&
            (global_interrupt - unit->interrupt_base) < unit->input_count)
        {
            return unit;
        }
    }

    return NULL;
}

/* The index of the low register of the redirection entry for an input. */
static uint8_t IoApicRedirectionIndex(const IoApicUnit *unit, uint32_t global_interrupt)
{
    const uint32_t input = global_interrupt - unit->interrupt_base;

    return (uint8_t)(IOAPIC_INDEX_REDIRECTION_BASE + (input * 2U));
}

static uint64_t IoApicReadEntry(const IoApicUnit *unit, uint32_t global_interrupt)
{
    const uint8_t index = IoApicRedirectionIndex(unit, global_interrupt);
    const uint32_t low = IoApicReadRegister(unit, index);
    const uint32_t high = IoApicReadRegister(unit, (uint8_t)(index + 1U));

    return (uint64_t)low | ((uint64_t)high << 32);
}

/*
 * Writes a whole redirection entry, high half first.
 *
 * The order is the whole of the safety here. The mask lives in the low half, so
 * writing the low half last means the entry becomes deliverable only once its
 * destination and its trigger mode are already in place. The reverse order has a
 * window — short, and therefore reproducible nowhere — in which the input is
 * unmasked and directed at whatever the entry held before.
 */
static void IoApicWriteEntry(const IoApicUnit *unit, uint32_t global_interrupt,
                             uint64_t value)
{
    const uint8_t index = IoApicRedirectionIndex(unit, global_interrupt);

    IoApicWriteRegister(unit, (uint8_t)(index + 1U), (uint32_t)(value >> 32));
    IoApicWriteRegister(unit, index, (uint32_t)(value & UINT64_C(0xFFFFFFFF)));
}

bool IoApicInitialise(void)
{
    IoApicUnitTotal = 0U;

    if (!AcpiIsAvailable())
    {
        KernelWriteString("I/O APIC: no ACPI tables, so none can be found.\n");
        return false;
    }

    for (size_t index = 0U; index < AcpiIoApicCount(); ++index)
    {
        const AcpiIoApic *const declaration = AcpiIoApicAt(index);
        IoApicUnit *unit;
        void *mapping;
        uint32_t version;

        if (IoApicUnitTotal >= ACPI_IO_APIC_MAXIMUM)
        {
            break;
        }

        mapping = KernelDeviceMap(declaration->address, IOAPIC_REGISTER_EXTENT,
                                  PAGE_ENTRY_WRITABLE | PAGE_ENTRY_CACHE_DISABLE);

        if (mapping == NULL)
        {
            KernelWriteString("I/O APIC: a unit's registers could not be mapped.\n");
            continue;
        }

        unit = &IoApicUnits[IoApicUnitTotal];
        unit->registers = (volatile uint8_t *)mapping;
        unit->address = declaration->address;
        unit->interrupt_base = declaration->interrupt_base;

        /*
         * The identifier and the input count are read from the hardware rather
         * than taken from the table. The table's identifier is what the firmware
         * believes; the register is what the unit will actually answer to, and
         * the input count has no counterpart in the table at all — ACPI records
         * only where a unit's inputs begin, Section 5.2.12.3 referring the
         * reader to this register for how many there are.
         */
        unit->identifier = (uint8_t)((IoApicReadRegister(unit, IOAPIC_INDEX_IDENTIFIER) >>
                                      IOAPIC_IDENTIFIER_SHIFT) & IOAPIC_IDENTIFIER_MASK);

        version = IoApicReadRegister(unit, IOAPIC_INDEX_VERSION);
        unit->input_count = ((version >> IOAPIC_MAX_REDIRECTION_SHIFT) &
                             IOAPIC_MAX_REDIRECTION_MASK) + 1U;

        if (unit->input_count > IOAPIC_INPUT_MAXIMUM)
        {
            /*
             * A unit answering with more inputs than the datasheet admits is not
             * answering: the commonest cause is a mapping that does not reach
             * the controller at all, in which case the read returned whatever
             * the bus drives upon an unclaimed address.
             */
            KernelWriteString("I/O APIC: a unit reports an implausible input count; "
                              "it is ignored.\n");
            KernelDeviceUnmap(mapping, IOAPIC_REGISTER_EXTENT);
            continue;
        }

        /* Every input begins masked; see the commentary upon IoApicInitialise. */
        for (uint32_t input = 0U; input < unit->input_count; ++input)
        {
            IoApicWriteEntry(unit, unit->interrupt_base + input,
                             IOAPIC_REDIRECTION_MASKED);
        }

        ++IoApicUnitTotal;
    }

    if (IoApicUnitTotal == 0U)
    {
        KernelWriteString("I/O APIC: the tables declare none that could be used.\n");
        return false;
    }

    return true;
}

size_t IoApicCount(void)
{
    return IoApicUnitTotal;
}

uint32_t IoApicInputCount(void)
{
    uint32_t total = 0U;

    for (size_t index = 0U; index < IoApicUnitTotal; ++index)
    {
        total += IoApicUnits[index].input_count;
    }

    return total;
}

bool IoApicDescribe(size_t index, uint8_t *identifier, PhysicalAddress *address,
                    uint32_t *interrupt_base, uint32_t *input_count)
{
    if (index >= IoApicUnitTotal)
    {
        return false;
    }

    *identifier = IoApicUnits[index].identifier;
    *address = IoApicUnits[index].address;
    *interrupt_base = IoApicUnits[index].interrupt_base;
    *input_count = IoApicUnits[index].input_count;

    return true;
}

bool IoApicRouteGlobalInterrupt(uint32_t global_interrupt, uint8_t vector,
                                uint8_t destination, bool active_low,
                                bool level_triggered)
{
    IoApicUnit *const unit = IoApicUnitFor(global_interrupt);
    uint64_t entry;

    if (unit == NULL)
    {
        return false;
    }

    /*
     * Intel SDM, Volume 3A, Section 10.5.2: the local APIC refuses a vector
     * below 16 and records the refusal in its error status register. Programming
     * one here would produce a device that never interrupts and an error count
     * that rises, which is a considerably harder thing to read than a refusal.
     */
    if (vector < 16U)
    {
        return false;
    }

    entry = (uint64_t)vector | IOAPIC_REDIRECTION_DELIVERY_FIXED |
            IOAPIC_REDIRECTION_MASKED |
            ((uint64_t)destination << IOAPIC_REDIRECTION_DESTINATION_SHIFT);

    if (active_low)
    {
        entry |= IOAPIC_REDIRECTION_ACTIVE_LOW;
    }

    if (level_triggered)
    {
        entry |= IOAPIC_REDIRECTION_LEVEL_TRIGGERED;
    }

    /*
     * The destination mode is left physical, the flag for logical mode being
     * absent from the value above. Physical mode names one processor by the
     * identifier of its local APIC, which is exactly what this kernel wants
     * while there is one processor; the logical modes exist to distribute
     * interrupts across a set, and choosing one before there is a set to choose
     * from would be a decision made without its reason.
     */
    IoApicWriteEntry(unit, global_interrupt, entry);

    return true;
}

bool IoApicSetMask(uint32_t global_interrupt, bool masked)
{
    IoApicUnit *const unit = IoApicUnitFor(global_interrupt);
    uint64_t entry;

    if (unit == NULL)
    {
        return false;
    }

    entry = IoApicReadEntry(unit, global_interrupt);

    if (masked)
    {
        entry |= IOAPIC_REDIRECTION_MASKED;
    }
    else
    {
        entry &= ~IOAPIC_REDIRECTION_MASKED;
    }

    IoApicWriteEntry(unit, global_interrupt, entry);

    return true;
}

bool IoApicGlobalInterruptIsMasked(uint32_t global_interrupt)
{
    const IoApicUnit *const unit = IoApicUnitFor(global_interrupt);

    if (unit == NULL)
    {
        return true;
    }

    return (IoApicReadEntry(unit, global_interrupt) & IOAPIC_REDIRECTION_MASKED) != 0U;
}

bool IoApicEntryValue(uint32_t global_interrupt, uint64_t *value)
{
    const IoApicUnit *const unit = IoApicUnitFor(global_interrupt);

    if (unit == NULL)
    {
        return false;
    }

    *value = IoApicReadEntry(unit, global_interrupt);

    return true;
}

void IoApicReport(void)
{
    KernelWriteString("I/O APIC: ");

    if (IoApicUnitTotal == 0U)
    {
        KernelWriteString("none.\n");
        return;
    }

    KernelWriteDecimal((uint64_t)IoApicUnitTotal);
    KernelWriteString(" unit(s), ");
    KernelWriteDecimal((uint64_t)IoApicInputCount());
    KernelWriteString(" interrupt inputs between them.\n");

    for (size_t index = 0U; index < IoApicUnitTotal; ++index)
    {
        const IoApicUnit *const unit = &IoApicUnits[index];

        KernelWriteString("  Unit ");
        KernelWriteDecimal((uint64_t)unit->identifier);
        KernelWriteString(" at ");
        KernelWriteHexadecimal(unit->address);
        KernelWriteString(", version ");
        KernelWriteHexadecimal((uint64_t)(IoApicReadRegister(unit, IOAPIC_INDEX_VERSION) &
                                          IOAPIC_VERSION_MASK));
        KernelWriteString(", global interrupts ");
        KernelWriteDecimal((uint64_t)unit->interrupt_base);
        KernelWriteString(" to ");
        KernelWriteDecimal((uint64_t)(unit->interrupt_base + unit->input_count - 1U));
        KernelWriteString(".\n");
    }

    /*
     * Every entry that is not masked is named. A redirection table is the one
     * place where a wrong answer is entirely invisible from above: an input
     * routed to a vector nothing registered, or to a processor that is not
     * running, produces a device that is silent and a kernel that reports
     * nothing at all.
     */
    for (size_t index = 0U; index < IoApicUnitTotal; ++index)
    {
        const IoApicUnit *const unit = &IoApicUnits[index];

        for (uint32_t input = 0U; input < unit->input_count; ++input)
        {
            const uint32_t global_interrupt = unit->interrupt_base + input;
            const uint64_t entry = IoApicReadEntry(unit, global_interrupt);

            if ((entry & IOAPIC_REDIRECTION_MASKED) != 0U)
            {
                continue;
            }

            KernelWriteString("  Global interrupt ");
            KernelWriteDecimal((uint64_t)global_interrupt);
            KernelWriteString(" presents vector ");
            KernelWriteDecimal(entry & IOAPIC_REDIRECTION_VECTOR_MASK);
            KernelWriteString(" to processor ");
            KernelWriteDecimal(entry >> IOAPIC_REDIRECTION_DESTINATION_SHIFT);
            KernelWriteString((entry & IOAPIC_REDIRECTION_ACTIVE_LOW) != 0U
                                  ? ", active low"
                                  : ", active high");
            KernelWriteString((entry & IOAPIC_REDIRECTION_LEVEL_TRIGGERED) != 0U
                                  ? ", level triggered.\n"
                                  : ", edge triggered.\n");
        }
    }
}
