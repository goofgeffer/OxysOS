/*
 * File: kernel/include/oxys/tss.h
 * Purpose: Declares the task state segment, which in 64-bit mode holds no task
 *          state at all but the stack pointers the processor loads when it
 *          raises its privilege or takes an exception through the interrupt
 *          stack table.
 * Key definitions: TaskStateSegment, TSS_IST_DOUBLE_FAULT, TssInitialise,
 *          TssSetKernelStack, TssKernelStack, TssInterruptStack,
 *          TssTaskRegister, TssReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 8.7 (Task Management in 64-Bit Mode) and Figure 8-11: the 64-bit
 *     task state segment holds RSP0 to RSP2, the seven interrupt stack table
 *     pointers and the I/O map base, and hardware task switching does not exist
 *     in 64-bit mode, so the segment is read and never written by the processor.
 *   - Intel SDM, Volume 3A, Section 6.14.2 (64-Bit Mode Stack Frame) and
 *     Section 6.14.4: upon a transfer to a numerically lower privilege level the
 *     processor loads RSP from the RSPn field for the new level; upon a transfer
 *     through a gate whose IST index is non-zero it loads RSP from that entry
 *     unconditionally, whatever the privilege change.
 *   - Intel SDM, Volume 3A, Section 8.2.3 and Figure 8-4: the TSS descriptor in
 *     64-bit mode occupies sixteen bytes, twice an ordinary descriptor, the base
 *     address being 64 bits wide. Its type is 9 while available and 11 once the
 *     task register has been loaded.
 *   - Intel SDM, Volume 3A, Section 20.5.2 (I/O Permission Bit Map): a map base
 *     beyond the segment limit means the map is absent, and every I/O access
 *     from a privilege level above IOPL then raises a general-protection
 *     exception.
 *   - Intel SDM, Volume 2A, "LTR": loads the task register with a selector for
 *     an available TSS descriptor, and marks that descriptor busy.
 */

#ifndef OXYS_TSS_H
#define OXYS_TSS_H

#include <oxys/types.h>

/*
 * The task state segment, per Intel SDM, Volume 3A, Figure 8-11.
 *
 * Every field the processor reads is at a fixed offset, and the structure
 * requires the packed attribute because it does not follow the alignment the C
 * implementation would otherwise impose: `reserved0` is four bytes and `rsp0`
 * eight, so a conforming layout would insert four bytes of padding between them
 * and every field thereafter would be read from the wrong offset. ISO C provides
 * no means of suppressing padding; refer to PROJECT_GUIDELINES.md, Section 8,
 * and to docs/project/CODING-STANDARDS.md, Section 7.
 *
 * The reserved fields are named and retained rather than elided, so that the
 * structure is comparable field by field against the figure it reproduces.
 */
typedef struct __attribute__((packed)) TaskStateSegment
{
    uint32_t reserved0;

    /*
     * The stack pointers loaded upon a transfer to privilege level 0, 1 and 2.
     * Only RSP0 is meaningful here: this kernel uses two privilege levels, and
     * the two intermediate levels exist in the architecture and not in the
     * design.
     */
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;

    uint64_t reserved1;

    /*
     * The interrupt stack table. Entry 0 of this array is IST1, the table being
     * numbered from one in the gate descriptor and an index of zero there
     * meaning that no entry is selected. The off-by-one is the architecture's
     * and is stated here because it is the one thing about this structure that
     * is easy to get wrong and produces a stack pointer of zero.
     */
    uint64_t ist[7];

    uint64_t reserved2;
    uint16_t reserved3;

    /*
     * The offset from the base of this segment at which the I/O permission bit
     * map begins. Set beyond the segment's limit, which the architecture defines
     * as the absence of a map: every I/O instruction executed at a privilege
     * level above IOPL then raises a general-protection exception, which is what
     * this kernel wants of a user program.
     */
    uint16_t io_map_base;
} TaskStateSegment;

_Static_assert(sizeof(TaskStateSegment) == 104,
               "The 64-bit task state segment must be exactly 104 bytes; padding "
               "would move every field the processor reads.");

/*
 * The entry of the interrupt stack table given to the double fault, expressed as
 * the gate descriptor expresses it — numbered from one.
 *
 * The double fault is the one exception that must not be delivered upon the
 * stack that was in use, because the commonest reason for a double fault is that
 * the stack was the thing that went wrong. A processor unable to push an
 * exception frame raises a triple fault, which is not an exception at all but a
 * shutdown, and the machine resets with nothing reported. This project has
 * already met one; the account is in docs/design/INTERRUPTS.md, Section 5.
 */
#define TSS_IST_DOUBLE_FAULT 1U

/* The stacks the segment names, in bytes. */
#define TSS_KERNEL_STACK_SIZE       16384U
#define TSS_INTERRUPT_STACK_SIZE    16384U

/*
 * Fills the segment, installs its descriptor in the global descriptor table and
 * loads the task register. The global descriptor table must already have been
 * established.
 */
void TssInitialise(void);

/*
 * Sets the stack the processor loads upon a transfer to privilege level 0.
 *
 * There is one stack until sub-task 6.4, there being one thread of control. From
 * that point each thread has a kernel stack of its own and the scheduler sets
 * this field as part of every switch: the field says where the *next* entry to
 * the kernel will place its frame, so a stale value would deliver a system call
 * upon a stack another thread is already using.
 */
void TssSetKernelStack(uint64_t stack_top);

/* The values presently held, for a report and for the self-test. */
uint64_t TssKernelStack(void);
uint64_t TssInterruptStack(unsigned int entry);
uint16_t TssIoMapBase(void);

/* The address of the segment, and its limit, for comparison against the
 * descriptor the processor reads. */
const TaskStateSegment *TssAddress(void);
uint32_t TssLimit(void);

/* The task register read back from the processor with STR, which is the only
 * evidence that the selector reached it. */
uint16_t TssTaskRegister(void);

/* Emits a summary of the segment upon the console and the serial port. */
void TssReport(void);

#endif /* OXYS_TSS_H */
