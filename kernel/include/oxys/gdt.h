/*
 * File: kernel/include/oxys/gdt.h
 * Purpose: Declares the kernel global descriptor table, which replaces the
 *          table established in boot/boot.asm with one residing in the higher
 *          half of the address space.
 * Key definitions: GdtRegister, GDT_KERNEL_CODE_SELECTOR,
 *          GDT_KERNEL_DATA_SELECTOR, GDT_USER_CODE32_SELECTOR,
 *          GDT_USER_DATA_SELECTOR, GDT_USER_CODE_SELECTOR, GDT_TSS_SELECTOR,
 *          GdtInitialise, GdtInstallTaskStateSegment, GdtDescriptorAt, GdtBase,
 *          GdtLimit.
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
 *   - Intel SDM, Volume 3A, Section 5.8.8 (Fast System Calls in 64-Bit Mode):
 *     the selectors SYSCALL and SYSRET derive from IA32_STAR, which fix the
 *     order of the descriptors below.
 *   - Intel SDM, Volume 3A, Section 8.2.3 and Figure 8-4: the sixteen-byte task
 *     state segment descriptor of 64-bit mode.
 *
 * Why this exists before Phase 6. The table loaded by boot/boot.asm resides in
 * the .boot section, at a low physical address which was reachable only through
 * the identity mapping. Sub-task 2.3 removed that mapping. Nothing required the
 * table thereafter, so the omission went unnoticed until sub-task 3.2 installed
 * interrupt gates: delivering an interrupt obliges the processor to read the
 * descriptor named by the gate's selector, and that read faulted upon an
 * unmapped address, escalating to a double fault and a reset.
 *
 * A minimal table was therefore established here. Sub-task 6.1 has since
 * extended it with the user-mode descriptors and the task state segment required
 * for privilege transition; the order those descriptors stand in is not a matter
 * of taste and is explained at the selectors below.
 */

#ifndef OXYS_GDT_H
#define OXYS_GDT_H

#include <oxys/types.h>

/*
 * The selectors, being byte offsets into the table. The first three retain the
 * values used by boot/boot.asm so that the code segment in force does not change
 * when the table is replaced.
 *
 * The order of what follows is dictated by SYSCALL and SYSRET and is not a
 * matter of arrangement. IA32_STAR holds two selectors and the processor derives
 * four from them by adding fixed displacements, per Intel SDM, Volume 3A,
 * Section 5.8.8:
 *
 *   SYSCALL:  CS = STAR[47:32]          SS = STAR[47:32] + 8
 *   SYSRET:   CS = STAR[63:48] + 16     SS = STAR[63:48] + 8   (64-bit operand)
 *             CS = STAR[63:48]          SS = STAR[63:48] + 8   (32-bit operand)
 *
 * So the kernel code descriptor must be followed immediately by the kernel data
 * descriptor, which it is; and the user descriptors must stand in the order
 * 32-bit code, data, 64-bit code, with nothing between them.
 *
 * GDT_USER_CODE32_SELECTOR is the consequence. This kernel does not support
 * compatibility mode and will never load that descriptor, but its *slot* cannot
 * be omitted: it is what STAR[63:48] names, and removing it would move the
 * 64-bit user code descriptor to an offset SYSRET has no way to express. The
 * descriptor is therefore present, correct, and unused.
 */
#define GDT_NULL_SELECTOR        UINT16_C(0x00)
#define GDT_KERNEL_CODE_SELECTOR UINT16_C(0x08)
#define GDT_KERNEL_DATA_SELECTOR UINT16_C(0x10)
#define GDT_USER_CODE32_SELECTOR UINT16_C(0x18)
#define GDT_USER_DATA_SELECTOR   UINT16_C(0x20)
#define GDT_USER_CODE_SELECTOR   UINT16_C(0x28)
#define GDT_TSS_SELECTOR         UINT16_C(0x30)

/*
 * The requested privilege level a selector carries in its low two bits.
 *
 * A user selector is loaded with an RPL of 3, and SYSRET forces those bits set
 * whatever IA32_STAR holds, per Intel SDM, Volume 3A, Section 5.8.8. The
 * constant is named so that the self-test may assert the value the processor
 * will derive rather than restating it.
 */
#define GDT_REQUESTED_PRIVILEGE_USER UINT16_C(0x03)

/*
 * The number of quadwords the table occupies.
 *
 * Six ordinary descriptors of eight bytes, and the task state segment
 * descriptor, which occupies sixteen in 64-bit mode because its base address is
 * 64 bits wide; the descriptor accordingly consumes two of these slots and the
 * selector after GDT_TSS_SELECTOR is not available.
 */
#define GDT_ENTRY_COUNT 8U

/* How many of those slots hold a descriptor a selector may name. */
#define GDT_DESCRIPTOR_COUNT 7U

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

/*
 * Writes the sixteen-byte task state segment descriptor into the table.
 *
 * It is installed after the table has been loaded, which is lawful: the
 * processor re-reads the descriptor when LTR names it, holding nothing of the
 * table between one reference and the next. It is separate from GdtInitialise
 * because the segment it describes belongs to kernel/cpu/tss.c, and a table that
 * composed the descriptor itself would have to know the segment's size.
 */
void GdtInstallTaskStateSegment(uint64_t base, uint32_t limit);

/* One quadword of the table, so that a self-test may assert the descriptors
 * rather than merely the register that names them. */
uint64_t GdtDescriptorAt(size_t index);

/* The base address presently loaded, read back from the processor with SGDT. */
uint64_t GdtBase(void);

/* The limit presently loaded, read back from the processor with SGDT. */
uint16_t GdtLimit(void);

/* The address of the table, for comparison against the value read back. */
const uint64_t *GdtTableAddress(void);

/* Emits a summary of the table upon the console and the serial port. */
void GdtReport(void);

#endif /* OXYS_GDT_H */
