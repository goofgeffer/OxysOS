/*
 * File: kernel/include/oxys/exceptions.h
 * Purpose: Declares the handlers for the architecture-defined exceptions and the
 *          decoding of the error codes they present.
 * Key definitions: PAGE_FAULT_* error code flags, SELECTOR_ERROR_* flags,
 *          ExceptionInitialise, ExceptionReportState, ExceptionDisposition,
 *          ExceptionDispositionOf, ExceptionCameFromUserMode,
 *          ExceptionOnlyOutsideKernel.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 6.13 and Figure 6-6: the error code presented by an exception
 *     related to a segment selector or an IDT vector.
 *   - Intel SDM, Volume 3A, Section 6.15, "Interrupt 14—Page-Fault Exception
 *     (#PF)" and Figure 6-9: the page fault error code, which has a format
 *     different from that of every other exception.
 *   - Intel SDM, Volume 3A, Section 6.5: the classification of exceptions as
 *     faults, traps and aborts, which determines whether a handler may return.
 */

#ifndef OXYS_EXCEPTIONS_H
#define OXYS_EXCEPTIONS_H

#include <oxys/types.h>
#include <oxys/interrupts.h>

/*
 * The page-fault error code, per Intel SDM, Volume 3A, Section 6.15 and
 * Figure 6-9. Its format is unlike that of every other exception.
 */
#define PAGE_FAULT_PRESENT         UINT64_C(0x0001) /* 0: no translation; 1: protection violation. */
#define PAGE_FAULT_WRITE           UINT64_C(0x0002) /* The access was a write. */
#define PAGE_FAULT_USER            UINT64_C(0x0004) /* The access was made in user mode. */
#define PAGE_FAULT_RESERVED_BIT    UINT64_C(0x0008) /* A reserved bit was set in an entry. */
#define PAGE_FAULT_INSTRUCTION     UINT64_C(0x0010) /* The access was an instruction fetch. */
#define PAGE_FAULT_PROTECTION_KEY  UINT64_C(0x0020) /* A protection-key violation. */
#define PAGE_FAULT_SGX             UINT64_C(0x8000) /* An SGX access-control violation. */

/*
 * The error code presented by the exceptions related to a segment selector,
 * per Intel SDM, Volume 3A, Section 6.13 and Figure 6-6. It resembles a segment
 * selector, but carries three flags in place of the TI field and the requested
 * privilege level.
 */
#define SELECTOR_ERROR_EXTERNAL    UINT64_C(0x0001) /* The event was external to the program. */
#define SELECTOR_ERROR_IDT         UINT64_C(0x0002) /* The index refers to an IDT gate. */
#define SELECTOR_ERROR_LDT         UINT64_C(0x0004) /* The index refers to the LDT, not the GDT. */
#define SELECTOR_ERROR_INDEX_SHIFT 3U
#define SELECTOR_ERROR_INDEX_MASK  UINT64_C(0x1FFF)

/*
 * Registers a handler for every architecture-defined exception, vectors 0 to 31.
 * The interrupt dispatcher must have been initialised beforehand.
 */
/*
 * The vector of the double fault, per Intel SDM, Volume 3A, Section 6.15.
 *
 * It is named because sub-task 6.1 gives it a stack of its own and the number
 * appears in two places thereafter: where the stack is attached, and where the
 * self-test asserts that it was.
 */
#define EXCEPTION_DOUBLE_FAULT UINT8_C(8)

/* The other vectors named below, for the same reason: the disposition table and
 * the fault screens both refer to them, and a bare number in two places is a
 * number that drifts apart. Intel SDM, Volume 3A, Table 6-1. */
#define EXCEPTION_NON_MASKABLE      UINT8_C(2)
#define EXCEPTION_BREAKPOINT        UINT8_C(3)
#define EXCEPTION_OVERFLOW          UINT8_C(4)
#define EXCEPTION_INVALID_TSS       UINT8_C(10)
#define EXCEPTION_SEGMENT_ABSENT    UINT8_C(11)
#define EXCEPTION_PAGE_FAULT        UINT8_C(14)
#define EXCEPTION_ALIGNMENT_CHECK   UINT8_C(17)
#define EXCEPTION_MACHINE_CHECK     UINT8_C(18)

/*
 * What is to be done about an exception.
 *
 * This is the distinction the kernel was missing, and its absence was a real
 * fault rather than an omission: every exception was treated as fatal to the
 * machine, so a divide by zero — the archetypal mistake of a program, and one
 * that must cost that program and nothing else — would have halted the system.
 *
 * The disposition is not a property of the vector alone. It is a property of the
 * vector **and of the privilege level the fault was raised at**, because that is
 * what decides whose mistake it was. The same page fault is a program to be
 * terminated when it comes from privilege level 3 and a kernel that cannot
 * continue when it comes from privilege level 0.
 */
typedef enum ExceptionDisposition
{
    /*
     * The handler removed the cause, or there was never anything to remove.
     * Execution resumes: the traps, and a page fault resolved by copy-on-write.
     */
    EXCEPTION_DISPOSITION_RESUME = 0,

    /*
     * The fault belongs to the program that caused it. That program is to be
     * terminated and the machine is to carry on.
     *
     * **Nothing can produce this yet**, and that is a statement about the state
     * of the kernel and not about the classification. Until sub-task 6.10 runs a
     * program at privilege level 3 there is no code outside the kernel to raise
     * such a fault and no process to terminate, so the classification is made
     * here, asserted here, and acted upon in Phase 7. A fault that reached this
     * disposition today would mean a privilege level 3 that this kernel does not
     * know it has.
     */
    EXCEPTION_DISPOSITION_TERMINATE = 1,

    /*
     * The kernel cannot continue. The machine is halted, and this is the only
     * disposition that draws a fault screen.
     *
     * Three things reach it. An abort — a double fault or a machine check —
     * whatever privilege level it arose at, the architecture permitting no
     * reliable resumption. A non-maskable interrupt, which is hardware
     * announcing a condition rather than a program making a mistake. And **any
     * fault raised within the kernel itself**, because there is then no program
     * to blame and no smaller thing to abandon.
     */
    EXCEPTION_DISPOSITION_FATAL = 2
} ExceptionDisposition;

/*
 * What is to be done about an exception raised upon vector `vector` by code
 * running with code segment selector `cs`.
 *
 * A pure function of its two arguments, which is why it is exposed rather than
 * buried in the handler: it can then be asserted for every vector at both
 * privilege levels without raising a single exception, which is the only way the
 * privilege level 3 half of it can be tested before a privilege level 3 exists.
 */
ExceptionDisposition ExceptionDispositionOf(uint64_t vector, uint64_t cs);

/* Whether the exception was raised outside the kernel, by the privilege level of
 * the code segment selector it was raised with. */
bool ExceptionCameFromUserMode(uint64_t cs);

/*
 * Whether the processor raises this exception **only** outside the kernel, so
 * that it can never be a fault of the kernel's own.
 *
 * At present this is the alignment check alone. Intel SDM, Volume 3A, Section
 * 6.15: #AC is raised only when the current privilege level is 3, `CR0.AM` is
 * set and `RFLAGS.AC` is set — all three, so kernel code cannot produce one
 * however it is written.
 *
 * It exists because such an exception must never be given a fault screen. A
 * screen announces that the machine has stopped; one written for a fault that
 * can only ever belong to a program is a page nobody could see, and its presence
 * in the table would state that the kernel treats as a catastrophe something it
 * does not. The self-test refuses one.
 *
 * The disposition above does not consult this, deliberately. Should such an
 * exception somehow arrive bearing a kernel selector, the machine is doing
 * something the architecture says it cannot, and halting is the right answer to
 * that whatever the vector.
 */
bool ExceptionOnlyOutsideKernel(uint64_t vector);

void ExceptionInitialise(void);

/*
 * Attaches the interrupt stack table entries the exceptions require, the double
 * fault being the only one that presently has one.
 *
 * It is separate from ExceptionInitialise because it must run after the task
 * state segment has been loaded, and the gates are installed before that. The
 * reasons are recorded at the definition and in docs/design/PRIVILEGE.md,
 * Section 4.
 *
 * Returns false where the gate is absent or the entry is not one the
 * architecture provides.
 */
bool ExceptionInstallInterruptStacks(void);

/*
 * Emits a full diagnosis of the processor state: the decoded vector, the decoded
 * error code, every general-purpose register, the control registers, and a
 * portion of the stack. Written to both the console and the serial port.
 */
void ExceptionReportState(const TrapFrame *frame, uint64_t fault_address);

#endif /* OXYS_EXCEPTIONS_H */
