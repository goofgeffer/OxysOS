/*
 * File: kernel/include/oxys/idt.h
 * Purpose: Declares the interrupt descriptor table, the 64-bit gate descriptor
 *          defined by the architecture, and the operations by which a gate is
 *          installed and the table loaded into the processor.
 * Key definitions: IdtGateDescriptor, IdtRegister, IDT_ENTRY_COUNT,
 *          IDT_GATE_TYPE_INTERRUPT, IDT_GATE_TYPE_TRAP, IdtInitialise,
 *          IdtSetGate, IdtLimit, IdtBase.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 6.10 (Interrupt Descriptor Table) and Section 6.14.1 (64-Bit Mode
 *     IDT): in 64-bit mode the index into the table is formed by scaling the
 *     vector number by 16, each descriptor therefore occupying sixteen bytes.
 *   - Intel SDM, Volume 3A, Figure 6-8 (64-Bit IDT Gate Descriptors): the layout
 *     reproduced in the structure below.
 *   - Intel SDM, Volume 3A, Section 6.14.1: only 64-bit interrupt and trap gates
 *     are valid in IA-32e mode; a legacy 32-bit gate type referenced there
 *     generates a general-protection exception.
 *   - Intel SDM, Volume 3A, Section 6.2: vector numbers 0 to 31 are reserved for
 *     architecture-defined exceptions and interrupts; 32 to 255 are available
 *     for external devices and software.
 */

#ifndef OXYS_IDT_H
#define OXYS_IDT_H

#include <oxys/types.h>

/*
 * The table holds an entry for every vector the processor can present. The
 * architecture permits 256, per Intel SDM, Volume 3A, Section 6.2.
 */
#define IDT_ENTRY_COUNT 256U

/*
 * Gate types valid in IA-32e mode, occupying the low four bits of the type and
 * attributes byte.
 *
 * An interrupt gate clears the interrupt flag upon entry, so the handler runs
 * with maskable interrupts disabled. A trap gate leaves the flag unchanged.
 * Every gate installed by this kernel is an interrupt gate: a handler that could
 * be interrupted before it has saved its state would corrupt that state.
 */
#define IDT_GATE_TYPE_INTERRUPT UINT8_C(0x0E)
#define IDT_GATE_TYPE_TRAP      UINT8_C(0x0F)

/* Attribute bits of the type and attributes byte. */
#define IDT_ATTRIBUTE_PRESENT   UINT8_C(0x80)
#define IDT_ATTRIBUTE_DPL_0     UINT8_C(0x00)
#define IDT_ATTRIBUTE_DPL_3     UINT8_C(0x60)

/*
 * The code segment selector through which every handler is entered. It is the
 * 64-bit code descriptor of the table established in boot/boot.asm.
 */
#define IDT_KERNEL_CODE_SELECTOR UINT16_C(0x08)

/*
 * The 64-bit interrupt gate descriptor, per Intel SDM, Volume 3A, Figure 6-8.
 *
 * The layout is fixed by the processor and the fields must appear in this order
 * and at these widths. The structure requires no packing attribute: every member
 * is naturally aligned at its offset, so the compiler inserts no padding, and
 * the assertion below confirms that on every build.
 */
typedef struct IdtGateDescriptor
{
    /* Bits 15:0 of the address of the handler. */
    uint16_t offset_low;
    /* The code segment selector loaded into CS upon entry. */
    uint16_t selector;
    /*
     * Bits 2:0 select an entry of the interrupt stack table in the task state
     * segment; zero selects the legacy stack-switching behaviour. Bits 7:3 are
     * reserved and must be zero. The interrupt stack table is not used until the
     * task state segment is established in Phase 6, sub-task 6.1.
     */
    uint8_t interrupt_stack_table;
    /*
     * The present bit, the descriptor privilege level, a zero bit, and the gate
     * type, in descending order of significance.
     */
    uint8_t type_and_attributes;
    /* Bits 31:16 of the address of the handler. */
    uint16_t offset_middle;
    /* Bits 63:32 of the address of the handler. */
    uint32_t offset_high;
    /* Reserved; must be zero. */
    uint32_t reserved;
} IdtGateDescriptor;

_Static_assert(sizeof(IdtGateDescriptor) == 16,
               "A 64-bit IDT gate descriptor must be exactly 16 bytes.");

/*
 * The operand of the LIDT instruction: a 16-bit limit followed immediately by a
 * 64-bit base address, with no padding between them.
 *
 * This structure DOES require the packed attribute. The 64-bit base would
 * otherwise be aligned to an eight-byte boundary, inserting six bytes of padding
 * after the limit, and the processor would read the base from the wrong offset.
 * The use of a compiler extension is required here because ISO C provides no
 * means of suppressing padding, and the layout is dictated by the hardware.
 * Refer to PROJECT_GUIDELINES.md, Section 8.
 */
typedef struct __attribute__((packed)) IdtRegister
{
    uint16_t limit;
    uint64_t base;
} IdtRegister;

_Static_assert(sizeof(IdtRegister) == 10,
               "The LIDT operand must be exactly 10 bytes; padding would corrupt it.");

/*
 * Clears every gate to a non-present state and loads the table into the
 * processor. Until gates are installed, any interrupt or exception presented
 * will reference a descriptor whose present bit is clear, which raises a
 * general-protection exception; that in turn finds no handler and escalates.
 * The table is therefore loaded but not useful until sub-task 3.2 installs the
 * stubs.
 */
void IdtInitialise(void);

/*
 * Installs a gate for one vector.
 *
 * vector: the vector number, which must be below IDT_ENTRY_COUNT.
 * handler: the address of the routine entered.
 * type_and_attributes: the gate type combined with the present bit and the
 *     descriptor privilege level.
 */
void IdtSetGate(uint8_t vector, uint64_t handler, uint8_t type_and_attributes);

/* The limit presently loaded, read back from the processor with SIDT. */
uint16_t IdtLimit(void);

/* The base address presently loaded, read back from the processor with SIDT. */
uint64_t IdtBase(void);

/* The address of the table, for comparison against the value read back. */
const IdtGateDescriptor *IdtTableAddress(void);

/* Emits a summary of the table upon the console and the serial port. */
void IdtReport(void);

#endif /* OXYS_IDT_H */
