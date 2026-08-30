/*
 * File: kernel/include/oxys/gdt.h
 * Purpose: Declares the kernel global descriptor table, which replaces the
 *          table established in boot/boot.asm with one residing in the higher
 *          half of the address space.
 * Key definitions: GdtRegister, GDT_KERNEL_CODE_SELECTOR,
 *          GDT_KERNEL_DATA_SELECTOR, GdtInitialise, GdtBase, GdtLimit.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 3.5.1 (Segment Descriptor Tables): the GDTR holds a base address
 *     and a limit, the limit being one less than the size of the table.
 *   - Intel SDM, Volume 3A, Section 3.4.5 and Figure 3-8: the segment descriptor
 *     format, including the L flag that designates a 64-bit code segment.
 *   - Intel SDM, Volume 2A, "LGDT/LIDT": the two instructions share an operand
 *     format, a 16-bit limit followed by a 64-bit base with no padding.
 *   - Intel SDM, Volume 3A, Section 3.4.2: loading a segment selector causes the
 *     processor to set the accessed bit in the corresponding descriptor. The
 *     table must therefore reside in writable memory.
 *
 * Why this exists before Phase 6. The table loaded by boot/boot.asm resides in
 * the .boot section, at a low physical address which was reachable only through
 * the identity mapping. Sub-task 2.3 removed that mapping. Nothing required the
 * table thereafter, so the omission went unnoticed until sub-task 3.2 installed
 * interrupt gates: delivering an interrupt obliges the processor to read the
 * descriptor named by the gate's selector, and that read faulted upon an
 * unmapped address, escalating to a double fault and a reset.
 *
 * A minimal table is therefore established here. Phase 6, sub-task 6.1, extends
 * it with the user-mode descriptors and the task state segment required for
 * privilege transition.
 */

#ifndef OXYS_GDT_H
#define OXYS_GDT_H

#include <oxys/types.h>

/*
 * The selectors, being byte offsets into the table. They retain the values used
 * by boot/boot.asm so that the code segment in force does not change when the
 * table is replaced.
 */
#define GDT_NULL_SELECTOR         UINT16_C(0x00)
#define GDT_KERNEL_CODE_SELECTOR  UINT16_C(0x08)
#define GDT_KERNEL_DATA_SELECTOR  UINT16_C(0x10)

/* The number of descriptors presently defined. */
#define GDT_ENTRY_COUNT 3U

/*
 * The operand of the LGDT instruction. It shares its format with that of LIDT,
 * and requires the packed attribute for the same reason: the 64-bit base would
 * otherwise be aligned to an eight-byte boundary and the processor would read it
 * from the wrong offset. Refer to PROJECT_GUIDELINES.md, Section 8.
 */
typedef struct __attribute__((packed)) GdtRegister
{
    uint16_t limit;
    uint64_t base;
} GdtRegister;

_Static_assert(sizeof(GdtRegister) == 10,
               "The LGDT operand must be exactly 10 bytes; padding would corrupt it.");

/*
 * Constructs the kernel table, loads it with LGDT, and reloads every segment
 * register including CS so that no cached descriptor from the boot table remains
 * in force.
 */
void GdtInitialise(void);

/* The base address presently loaded, read back from the processor with SGDT. */
uint64_t GdtBase(void);

/* The limit presently loaded, read back from the processor with SGDT. */
uint16_t GdtLimit(void);

/* The address of the table, for comparison against the value read back. */
const uint64_t *GdtTableAddress(void);

/* Emits a summary of the table upon the console and the serial port. */
void GdtReport(void);

#endif /* OXYS_GDT_H */
