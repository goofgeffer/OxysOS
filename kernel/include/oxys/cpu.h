/*
 * File: kernel/include/oxys/cpu.h
 * Purpose: Provides accessors for the processor control registers, which record
 *          the paging and protection state and, in the case of CR2, the linear
 *          address of a page fault.
 * Key definitions: ReadCr0, ReadCr2, ReadCr3, ReadCr4, ReadRflags.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 2.5 (Control Registers): CR0 holds the system control flags, CR2
 *     the page-fault linear address, CR3 the paging-structure base, and CR4 the
 *     architectural extension flags.
 *   - Intel SDM, Volume 3A, Section 6.15, "Interrupt 14—Page-Fault Exception":
 *     the processor loads CR2 with the linear address that generated the
 *     exception.
 */

#ifndef OXYS_CPU_H
#define OXYS_CPU_H

#include <oxys/types.h>

/* CR0 flags of present interest, per Intel SDM, Volume 3A, Section 2.5. */
#define CR0_PROTECTION_ENABLE UINT64_C(0x00000001)
#define CR0_WRITE_PROTECT     UINT64_C(0x00010000)
#define CR0_PAGING            UINT64_C(0x80000000)

/* CR4 flags of present interest. */
#define CR4_PHYSICAL_ADDRESS_EXTENSION UINT64_C(0x00000020)
#define CR4_SUPERVISOR_EXECUTE_PREVENTION UINT64_C(0x00100000)
#define CR4_SUPERVISOR_ACCESS_PREVENTION  UINT64_C(0x00200000)

static inline uint64_t ReadCr0(void)
{
    uint64_t value;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(value));

    return value;
}

/*
 * Reads the linear address that caused the most recent page fault.
 *
 * Intel SDM, Volume 3A, Section 6.15, warns that a further page fault may occur
 * during execution of the page-fault handler, and that the handler should
 * therefore save CR2 before a second fault can occur. Every handler here reads
 * it as its first action for that reason.
 */
static inline uint64_t ReadCr2(void)
{
    uint64_t value;

    __asm__ __volatile__("mov %%cr2, %0" : "=r"(value));

    return value;
}

/*
 * Writes CR0. Used to set the write-protect flag, which governs whether a
 * supervisor-mode write to a read-only page raises a page fault.
 */
static inline void WriteCr0(uint64_t value)
{
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline uint64_t ReadCr3(void)
{
    uint64_t value;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));

    return value;
}

static inline uint64_t ReadCr4(void)
{
    uint64_t value;

    __asm__ __volatile__("mov %%cr4, %0" : "=r"(value));

    return value;
}

#endif /* OXYS_CPU_H */
