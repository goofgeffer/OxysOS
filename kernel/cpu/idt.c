/*
 * File: kernel/cpu/idt.c
 * Purpose: Implements the interrupt descriptor table: the storage for the 256
 *          gate descriptors, the installation of a gate, and the loading of the
 *          table into the processor.
 * Key functions: IdtInitialise, IdtSetGate, IdtSetGateStack, IdtGateStack,
 *          IdtLoad, IdtLimit, IdtBase,
 *          IdtTableAddress, IdtReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 6.10: the interrupt descriptor table and the IDTR register.
 *   - Intel SDM, Volume 3A, Section 6.14.1: in 64-bit mode the index is the
 *     vector scaled by 16; only 64-bit interrupt and trap gates are valid.
 *   - Intel SDM, Volume 3A, Figure 6-8: the gate descriptor layout.
 *   - Intel SDM, Volume 2A, "LGDT/LIDT" and "SGDT/SIDT": the instructions that
 *     load and store the table register, and the format of their operand.
 *
 * Concurrency. The table is loaded once per processor. Each application
 * processor started in sub-task 6.8 must execute LIDT for itself, since IDTR is
 * a per-processor register, but they may all reference this one table.
 */

#include <oxys/idt.h>
#include <oxys/kernel.h>

/*
 * The table itself. Intel SDM, Volume 3A, Section 6.10, recommends that the
 * table be aligned on an eight-byte boundary so that no descriptor spans a cache
 * line unnecessarily. Sixteen-byte alignment is used, which is the natural
 * alignment of a descriptor and satisfies that recommendation.
 */
static IdtGateDescriptor IdtTable[IDT_ENTRY_COUNT] __attribute__((aligned(16)));

/* The operand most recently supplied to LIDT, retained for reporting. */
static IdtRegister IdtLoadedRegister;

/*
 * Loads the table into the processor. The limit is one less than the size of the
 * table in bytes, being the offset of its last valid byte, per Intel SDM,
 * Volume 3A, Section 6.10.
 */
static void IdtLoad(void)
{
    IdtLoadedRegister.limit = (uint16_t)(sizeof(IdtTable) - 1U);
    IdtLoadedRegister.base = (uint64_t)(uintptr_t)&IdtTable[0];

    __asm__ __volatile__("lidt %0" : : "m"(IdtLoadedRegister) : "memory");
}

void IdtSetGate(uint8_t vector, uint64_t handler, uint8_t type_and_attributes)
{
    IdtGateDescriptor *gate = &IdtTable[vector];

    gate->offset_low = (uint16_t)(handler & UINT64_C(0xFFFF));
    gate->selector = IDT_KERNEL_CODE_SELECTOR;
    gate->interrupt_stack_table = 0U;
    gate->type_and_attributes = type_and_attributes;
    gate->offset_middle = (uint16_t)((handler >> 16) & UINT64_C(0xFFFF));
    gate->offset_high = (uint32_t)((handler >> 32) & UINT64_C(0xFFFFFFFF));
    gate->reserved = 0U;
}

void IdtInitialise(void)
{
    /*
     * Clear every descriptor. A cleared descriptor has its present bit clear,
     * which is the correct initial state: a vector for which no handler has been
     * installed must not transfer control to arbitrary memory.
     */
    for (size_t vector = 0U; vector < IDT_ENTRY_COUNT; ++vector)
    {
        IdtTable[vector].offset_low = 0U;
        IdtTable[vector].selector = 0U;
        IdtTable[vector].interrupt_stack_table = 0U;
        IdtTable[vector].type_and_attributes = 0U;
        IdtTable[vector].offset_middle = 0U;
        IdtTable[vector].offset_high = 0U;
        IdtTable[vector].reserved = 0U;
    }

    IdtLoad();
}

bool IdtSetGateStack(uint8_t vector, uint8_t interrupt_stack_table_entry)
{
    IdtGateDescriptor *const gate = &IdtTable[vector];

    /*
     * Bits 2:0 of the field select the entry; bits 7:3 are reserved and must be
     * zero, so a value above seven is refused rather than truncated. A truncated
     * index would select a different entry of the table, which is a valid stack
     * pointer belonging to another exception and would then be used by both.
     */
    if (interrupt_stack_table_entry > 7U)
    {
        return false;
    }

    /*
     * A gate that is not present names no handler, so no stack can usefully be
     * attached to it: the assignment would be lost the moment the gate was
     * installed, IdtSetGate clearing this field.
     */
    if ((gate->type_and_attributes & IDT_ATTRIBUTE_PRESENT) == 0U)
    {
        return false;
    }

    gate->interrupt_stack_table = interrupt_stack_table_entry;
    return true;
}

uint8_t IdtGateStack(uint8_t vector)
{
    return (uint8_t)(IdtTable[vector].interrupt_stack_table & UINT8_C(0x07));
}

/*
 * Reads the table register back from the processor with SIDT. This is the only
 * means of confirming that LIDT took effect, and is used by the self-test rather
 * than trusting the value that was written.
 */
static void IdtStore(IdtRegister *destination)
{
    __asm__ __volatile__("sidt %0" : "=m"(*destination) : : "memory");
}

uint16_t IdtLimit(void)
{
    IdtRegister stored;

    IdtStore(&stored);

    return stored.limit;
}

uint64_t IdtBase(void)
{
    IdtRegister stored;

    IdtStore(&stored);

    return stored.base;
}

const IdtGateDescriptor *IdtTableAddress(void)
{
    return &IdtTable[0];
}

void IdtReport(void)
{
    size_t present_count = 0U;

    for (size_t vector = 0U; vector < IDT_ENTRY_COUNT; ++vector)
    {
        if ((IdtTable[vector].type_and_attributes & IDT_ATTRIBUTE_PRESENT) != 0U)
        {
            ++present_count;
        }
    }

    KernelWriteString("Interrupt descriptor table: base ");
    KernelWriteHexadecimal(IdtBase());
    KernelWriteString(", limit ");
    KernelWriteHexadecimal((uint64_t)IdtLimit());
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)present_count);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)IDT_ENTRY_COUNT);
    KernelWriteString(" gates present.\n");
}
