/*
 * File: drivers/apic/lapic.c
 * Purpose: Implements the driver for the Local Advanced Programmable Interrupt
 *          Controller: its detection, the mapping of its register page as strong
 *          uncacheable memory, its enabling through the spurious-interrupt vector
 *          register, the local vector table entries this kernel programmes, and
 *          the end-of-interrupt the architecture requires of every handler.
 * Key functions: LocalApicIsSupported, LocalApicInitialise,
 *          LocalApicSignalEndOfInterrupt, LocalApicIdentifier,
 *          LocalApicIsBootstrapProcessor, LocalApicRead, LocalApicWrite,
 *          LocalApicReport.
 * References:
 *   - Intel SDM, Volume 3A, Section 10.4.1: the registers occupy a 4 KiB region
 *     at 0xFEE00000, and "for correct APIC operation, this address space must be
 *     mapped to an area of memory that has been designated as strong
 *     uncacheable (UC)".
 *   - Intel SDM, Volume 3A, Table 10-1: the register offsets, and that every
 *     32-bit register is to be accessed by an aligned 32-bit load or store. An
 *     access of a different width, or one touching bytes 4 to 15 of a register,
 *     is expressly undefined.
 *   - Intel SDM, Volume 3A, Section 10.4.2: CPUID leaf 1, EDX bit 9.
 *   - Intel SDM, Volume 3A, Sections 10.4.3 and 10.4.4: IA32_APIC_BASE, its
 *     global enable at bit 11, its bootstrap flag at bit 8 and its base address
 *     at bits 35:12.
 *   - Intel SDM, Volume 3A, Section 10.4.7.1: the state after reset — every
 *     local vector table entry masked, and the spurious-interrupt vector
 *     register holding 0x000000FF, whose bit 8 is clear, so that the controller
 *     is software-disabled until this driver enables it.
 *   - Intel SDM, Volume 3A, Section 10.5.1 and Figure 10-8: the local vector
 *     table entry.
 *   - Intel SDM, Volume 3A, Section 10.5.2: vectors 16 to 255 are valid; a
 *     vector below 16 is refused by the controller and recorded in the error
 *     status register.
 *   - Intel SDM, Volume 3A, Section 10.8.5: the end-of-interrupt register.
 *   - Intel SDM, Volume 3A, Section 10.8.6: the task priority register blocks
 *     every interrupt of a priority class at or below the value it holds, and a
 *     value of zero blocks none.
 *   - Intel SDM, Volume 3A, Section 10.9: the spurious interrupt, whose handler
 *     must return without an end-of-interrupt because no bit stands in the
 *     in-service register to be reset.
 *   - ACPI Specification 6.5, Section 5.2.12.7: the Local APIC NMI structures,
 *     which say which local interrupt pin of which processor the non-maskable
 *     interrupt is attached to.
 *
 * Why the task priority register is written to zero, and by this driver.
 *
 *   Intel SDM, Volume 3A, Section 10.8.6, provides that the register blocks
 *   every interrupt whose priority class is at or below the value it holds. It
 *   is cleared by a reset, so writing zero appears to be redundant. It is not:
 *   the firmware ran before this kernel and is under no obligation to have left
 *   it as it found it, and a task priority the firmware raised and did not lower
 *   would present as a machine upon which the timer and the keyboard are
 *   correctly programmed and simply never interrupt.
 *
 * Concurrency. Each processor has a local APIC of its own, reached at the same
 * physical address, so every access here is to the executing processor's own
 * controller and no lock can make an access refer to another's. The counters
 * below are written by the two handlers this driver registers, which run upon
 * whichever processor the event occurred at; from sub-task 6.13 they become
 * per-processor quantities.
 */

#include <oxys/lapic.h>
#include <oxys/acpi.h>
#include <oxys/msr.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>
#include <oxys/kernel.h>

/* The mapped register page, and the physical address it was mapped from. */
static volatile uint8_t *LocalApicRegisters;
static PhysicalAddress LocalApicPhysicalBase;
static bool LocalApicEnabled;

/* Accounting. */
static uint64_t LocalApicSpuriousRequests;
static uint64_t LocalApicErrors;
static uint32_t LocalApicErrorStatus;

/*
 * Executes CPUID with the given leaf.
 *
 * The instruction is written out here rather than taken from the compiler's
 * <cpuid.h> because that header is a GCC extension, and PROJECT_GUIDELINES.md,
 * Section 8, admits an extension only with its rationale recorded. There is no
 * rationale for this one: four lines of inline assembly are the whole of what
 * the header would provide. RBX is named as an output rather than clobbered
 * because it is otherwise the compiler's frame or base pointer under some
 * models, and a clobber of it would be rejected.
 */
static void LocalApicCpuId(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                           uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                         : "a"(leaf), "c"(0U));
}

bool LocalApicIsSupported(void)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;

    /* Intel SDM, Volume 3A, Section 10.4.2: leaf 1, EDX bit 9. */
    LocalApicCpuId(1U, &eax, &ebx, &ecx, &edx);

    return (edx & UINT32_C(0x00000200)) != 0U;
}

uint32_t LocalApicRead(uint32_t offset)
{
    if (LocalApicRegisters == NULL)
    {
        return 0U;
    }

    return *(volatile uint32_t *)(const void *)(LocalApicRegisters + offset);
}

void LocalApicWrite(uint32_t offset, uint32_t value)
{
    if (LocalApicRegisters == NULL)
    {
        return;
    }

    *(volatile uint32_t *)(void *)(LocalApicRegisters + offset) = value;
}

void LocalApicSignalEndOfInterrupt(void)
{
    /*
     * The register is write-only and the value written is ignored; Intel SDM,
     * Volume 3A, Figure 10-21, shows the whole of it reserved. Zero is written
     * because a reserved field must be written as zero if it is to remain
     * compatible with whatever a later processor defines there.
     */
    LocalApicWrite(LAPIC_REGISTER_END_OF_INTERRUPT, 0U);
}

/*
 * Receives the spurious vector.
 *
 * Intel SDM, Volume 3A, Section 10.9: the controller delivers this vector when a
 * request it had accepted became masked before it could be dispensed. Nothing
 * stands in the in-service register, so the handler must return without an
 * end-of-interrupt; one issued here would reset the bit of whatever interrupt
 * was genuinely in service and lose it. The condition is counted because a
 * machine producing them steadily is a machine whose masking is racing its
 * delivery, which is worth seeing.
 */
static void LocalApicHandleSpurious(TrapFrame *frame)
{
    (void)frame;
    ++LocalApicSpuriousRequests;
}

/*
 * Receives the error vector.
 *
 * The error status register latches what the controller has detected since it
 * was last read — an illegal vector, a send accept error, a checksum failure.
 * Intel SDM, Volume 3A, Section 10.5.3, requires a write to the register before
 * the read: the write causes the latched value to be updated, and a read without
 * it returns the value latched before the error being reported occurred.
 */
static void LocalApicHandleError(TrapFrame *frame)
{
    (void)frame;

    LocalApicWrite(LAPIC_REGISTER_ERROR_STATUS, 0U);
    LocalApicErrorStatus = LocalApicRead(LAPIC_REGISTER_ERROR_STATUS);
    ++LocalApicErrors;

    LocalApicSignalEndOfInterrupt();
}

/*
 * Programmes the local interrupt pins from the ACPI Local APIC NMI structures.
 *
 * Both pins are masked first and then whichever the firmware declares is given
 * the non-maskable delivery mode. The order matters upon a machine booted by a
 * firmware that left LINT0 configured for the external delivery mode, which is
 * how the 8259A reaches a processor through the local controller: leaving that
 * configuration in place while the 8259A is retired would leave the processor
 * waiting for a vector from a controller that no longer presents one.
 *
 * An entry naming processor 0xFF applies to every processor, per ACPI 6.5,
 * Table 5.28. Entries naming a particular processor are applied here only where
 * they name this one; sub-task 6.14 will apply the remainder as each application
 * processor starts.
 */
static void LocalApicProgrammeLocalPins(void)
{
    const uint8_t identifier = LocalApicIdentifier();

    LocalApicWrite(LAPIC_REGISTER_LVT_LINT0, LAPIC_LVT_MASKED);
    LocalApicWrite(LAPIC_REGISTER_LVT_LINT1, LAPIC_LVT_MASKED);

    for (size_t index = 0U; index < AcpiLocalNmiCount(); ++index)
    {
        const AcpiLocalNmi *const entry = AcpiLocalNmiAt(index);
        uint32_t value = LAPIC_LVT_DELIVERY_NMI;

        if (entry->acpi_uid != UINT32_C(0xFF) &&
            entry->acpi_uid != (uint32_t)identifier)
        {
            continue;
        }

        /*
         * The vector field is ignored for the non-maskable delivery mode, per
         * Intel SDM, Volume 3A, Section 10.5.1, and is left zero. The polarity
         * and trigger mode are taken from the firmware's declaration; a source
         * that conforms to its bus is active high and edge triggered, which is
         * what the zero bits already express.
         */
        if ((entry->flags & ACPI_MPS_INTI_POLARITY_MASK) == ACPI_MPS_INTI_POLARITY_LOW)
        {
            value |= LAPIC_LVT_ACTIVE_LOW;
        }

        if ((entry->flags & ACPI_MPS_INTI_TRIGGER_MASK) == ACPI_MPS_INTI_TRIGGER_LEVEL)
        {
            value |= LAPIC_LVT_LEVEL_TRIGGERED;
        }

        LocalApicWrite(entry->local_interrupt == 0U ? LAPIC_REGISTER_LVT_LINT0
                                                    : LAPIC_REGISTER_LVT_LINT1,
                       value);
    }
}

bool LocalApicInitialise(void)
{
    uint64_t base_register;
    PhysicalAddress base;
    void *mapping;

    LocalApicEnabled = false;

    if (!LocalApicIsSupported())
    {
        KernelWriteString("Local APIC: the processor reports none.\n");
        return false;
    }

    base_register = ReadMsr(IA32_APIC_BASE);
    base = (PhysicalAddress)(base_register & LAPIC_BASE_ADDRESS_MASK);

    /*
     * The MADT's address is preferred where one was declared and differs. A
     * Local APIC Address Override is the firmware stating where it moved the
     * controllers to, and it is the only source that speaks for every processor
     * rather than for the one executing.
     */
    if (AcpiIsAvailable() && AcpiLocalApicAddress() != 0U)
    {
        base = AcpiLocalApicAddress();
    }

    if (base == 0U)
    {
        base = LAPIC_DEFAULT_BASE;
    }

    /*
     * The global enable is set before the page is mapped. Intel SDM, Volume 3A,
     * Section 10.4.3, provides that with bit 11 clear the processor is
     * functionally one without an APIC — the registers do not answer, and upon
     * some processors an access to them raises an invalid-opcode exception.
     * Writing the base back unchanged alongside the flag is deliberate: the
     * register is written whole, and a write that dropped the address the
     * firmware chose would relocate the controllers.
     */
    WriteMsr(IA32_APIC_BASE,
             (base_register & ~LAPIC_BASE_ADDRESS_MASK) | (uint64_t)base |
                 LAPIC_BASE_GLOBAL_ENABLE);

    /*
     * Uncacheable, as Section 10.4.1 requires.
     *
     * The page-level cache-disable flag with the write-through flag clear
     * selects entry 2 of IA32_PAT, which the processor leaves holding UC- and
     * which graphics/framebuffer.c did not disturb, having written only entry 4.
     * Intel SDM, Volume 3A, Table 11-7, gives the effective type of UC- over a
     * region the memory-type range registers call uncacheable as uncacheable,
     * which the register page of a memory-mapped controller invariably is.
     *
     * A cacheable mapping would be the sort of defect that reads correctly and
     * behaves wrongly: a read of the in-service register would be answered from
     * a cache line rather than from the controller, and an end-of-interrupt
     * would sit in a write buffer while the next interrupt waited for it.
     */
    mapping = KernelDeviceMap(base, LAPIC_REGISTER_EXTENT,
                              PAGE_ENTRY_WRITABLE | PAGE_ENTRY_CACHE_DISABLE);

    if (mapping == NULL)
    {
        KernelWriteString("Local APIC: its register page could not be mapped.\n");
        return false;
    }

    LocalApicRegisters = (volatile uint8_t *)mapping;
    LocalApicPhysicalBase = base;

    /* Accept every priority class; see the commentary in the file header. */
    LocalApicWrite(LAPIC_REGISTER_TASK_PRIORITY, 0U);

    /*
     * Mask the entries this kernel does not use before enabling the controller.
     * The timer, the performance counters and the thermal sensor are all masked
     * by a reset, but the firmware ran first and a machine whose firmware left
     * the timer running would begin delivering an unregistered vector the moment
     * the software enable was set.
     */
    LocalApicWrite(LAPIC_REGISTER_LVT_TIMER, LAPIC_LVT_MASKED);
    LocalApicWrite(LAPIC_REGISTER_LVT_PERFORMANCE, LAPIC_LVT_MASKED);

    /*
     * The thermal sensor entry exists only upon processors reporting enough
     * local vector table entries to include it, per Intel SDM, Section 10.4.8;
     * writing a register the implementation does not have is recorded as an
     * illegal register access in the error status register.
     */
    if (LocalApicLocalVectorCount() > 5U)
    {
        LocalApicWrite(LAPIC_REGISTER_LVT_THERMAL, LAPIC_LVT_MASKED);
    }

    InterruptRegisterHandler(LAPIC_SPURIOUS_VECTOR, LocalApicHandleSpurious,
                             "local APIC spurious");
    InterruptRegisterHandler(LAPIC_ERROR_VECTOR, LocalApicHandleError,
                             "local APIC error");

    /*
     * The error entry is programmed before the controller is enabled, so that an
     * error arising from the enabling itself is delivered to a handler rather
     * than to whatever the entry held.
     */
    LocalApicWrite(LAPIC_REGISTER_LVT_ERROR, (uint32_t)LAPIC_ERROR_VECTOR);
    LocalApicWrite(LAPIC_REGISTER_ERROR_STATUS, 0U);
    (void)LocalApicRead(LAPIC_REGISTER_ERROR_STATUS);

    LocalApicProgrammeLocalPins();

    /*
     * The software enable, per Section 10.9. Until bit 8 of this register is set
     * the controller accepts nothing, whatever the local vector table says.
     */
    LocalApicWrite(LAPIC_REGISTER_SPURIOUS_VECTOR,
                   (uint32_t)LAPIC_SPURIOUS_VECTOR | LAPIC_SPURIOUS_SOFTWARE_ENABLE);

    LocalApicEnabled = true;

    return true;
}

bool LocalApicIsEnabled(void)
{
    return LocalApicEnabled;
}

uint8_t LocalApicIdentifier(void)
{
    /* Intel SDM, Volume 3A, Figure 10-6: bits 31:24 in xAPIC mode. */
    return (uint8_t)(LocalApicRead(LAPIC_REGISTER_IDENTIFIER) >> 24);
}

uint32_t LocalApicVersion(void)
{
    return LocalApicRead(LAPIC_REGISTER_VERSION);
}

uint8_t LocalApicLocalVectorCount(void)
{
    const uint32_t version = LocalApicVersion();

    /* Section 10.4.8: the field holds the count less one. */
    return (uint8_t)(((version >> LAPIC_VERSION_MAX_LVT_SHIFT) &
                      LAPIC_VERSION_MAX_LVT_MASK) + 1U);
}

bool LocalApicIsBootstrapProcessor(void)
{
    return (ReadMsr(IA32_APIC_BASE) & LAPIC_BASE_BOOTSTRAP_PROCESSOR) != 0U;
}

PhysicalAddress LocalApicBaseAddress(void)
{
    return LocalApicPhysicalBase;
}

uint64_t LocalApicSpuriousCount(void)
{
    return LocalApicSpuriousRequests;
}

uint64_t LocalApicErrorCount(void)
{
    return LocalApicErrors;
}

uint32_t LocalApicLastErrorStatus(void)
{
    return LocalApicErrorStatus;
}

void LocalApicReport(void)
{
    KernelWriteString("Local APIC: ");

    if (!LocalApicEnabled)
    {
        KernelWriteString("not enabled.\n");
        return;
    }

    KernelWriteString("identifier ");
    KernelWriteDecimal((uint64_t)LocalApicIdentifier());
    KernelWriteString(", version ");
    KernelWriteHexadecimal((uint64_t)(LocalApicVersion() & LAPIC_VERSION_MASK));
    KernelWriteString(", local vector entries ");
    KernelWriteDecimal((uint64_t)LocalApicLocalVectorCount());
    KernelWriteString(", registers at ");
    KernelWriteHexadecimal(LocalApicPhysicalBase);
    KernelWriteString(LocalApicIsBootstrapProcessor()
                          ? ", the bootstrap processor.\n"
                          : ", an application processor.\n");

    KernelWriteString("Local APIC: spurious vector ");
    KernelWriteDecimal((uint64_t)LAPIC_SPURIOUS_VECTOR);
    KernelWriteString(", error vector ");
    KernelWriteDecimal((uint64_t)LAPIC_ERROR_VECTOR);
    KernelWriteString(", LINT0 ");
    KernelWriteHexadecimal((uint64_t)LocalApicRead(LAPIC_REGISTER_LVT_LINT0));
    KernelWriteString(", LINT1 ");
    KernelWriteHexadecimal((uint64_t)LocalApicRead(LAPIC_REGISTER_LVT_LINT1));
    KernelWriteString(".\n");

    KernelWriteString("Local APIC: spurious ");
    KernelWriteDecimal(LocalApicSpuriousRequests);
    KernelWriteString(", errors ");
    KernelWriteDecimal(LocalApicErrors);
    KernelWriteString(", last error status ");
    KernelWriteHexadecimal((uint64_t)LocalApicErrorStatus);
    KernelWriteString(".\n");
}
