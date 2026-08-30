/*
 * File: kernel/include/oxys/exceptions.h
 * Purpose: Declares the handlers for the architecture-defined exceptions and the
 *          decoding of the error codes they present.
 * Key definitions: PAGE_FAULT_* error code flags, SELECTOR_ERROR_* flags,
 *          ExceptionInitialise, ExceptionReportState.
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
void ExceptionInitialise(void);

/*
 * Emits a full diagnosis of the processor state: the decoded vector, the decoded
 * error code, every general-purpose register, the control registers, and a
 * portion of the stack. Written to both the console and the serial port.
 */
void ExceptionReportState(const TrapFrame *frame, uint64_t fault_address);

#endif /* OXYS_EXCEPTIONS_H */
