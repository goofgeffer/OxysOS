/*
 * File: kernel/include/oxys/msr.h
 * Purpose: Provides access to the model-specific registers, and names those the
 *          kernel presently writes: the extended feature enable register, and
 *          the three registers that configure the SYSCALL and SYSRET pair.
 * Key definitions: ReadMsr, WriteMsr, IA32_EFER, IA32_STAR, IA32_LSTAR,
 *          IA32_FMASK, EFER_SYSTEM_CALL_EXTENSIONS.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 10.4 (Model-Specific Registers) and Volume 4, Chapter 2: a
 *     model-specific register is addressed by a 32-bit number in ECX, and its
 *     value is carried in EDX:EAX, the high half in EDX.
 *   - Intel SDM, Volume 2B, "RDMSR" and "WRMSR": both are privileged, and WRMSR
 *     raises a general-protection exception where the value is not one the
 *     register admits — a non-canonical address in IA32_LSTAR, for instance.
 *   - Intel SDM, Volume 3A, Section 2.2.1 and Table 2-1: IA32_EFER at
 *     0xC0000080, bit 0 being SCE, the system-call extensions enable, which must
 *     be set before SYSCALL is anything but an invalid opcode.
 *   - Intel SDM, Volume 3A, Section 5.8.8: IA32_STAR at 0xC0000081, IA32_LSTAR
 *     at 0xC0000082 and IA32_FMASK at 0xC0000084, and what each contributes to
 *     the transition.
 *   - AMD64 Architecture Programmer's Manual, Volume 2, Section 6.1: the same
 *     three registers, under the names STAR, LSTAR and SFMASK. The mechanism is
 *     AMD's; Intel's manual documents the same numbers and the same behaviour.
 */

#ifndef OXYS_MSR_H
#define OXYS_MSR_H

#include <oxys/types.h>

/* The extended feature enable register. */
#define IA32_EFER UINT32_C(0xC0000080)

/*
 * The three registers that configure SYSCALL and SYSRET.
 *
 * IA32_CSTAR is named although it is not written: it holds the entry point taken
 * when SYSCALL is executed from compatibility mode, which this kernel does not
 * support. It is recorded here so that a reader does not suppose the omission
 * accidental.
 */
#define IA32_STAR  UINT32_C(0xC0000081)
#define IA32_LSTAR UINT32_C(0xC0000082)
#define IA32_CSTAR UINT32_C(0xC0000083)
#define IA32_FMASK UINT32_C(0xC0000084)

/*
 * The segment-base registers. None is written yet; IA32_KERNEL_GS_BASE is what
 * SWAPGS exchanges with GS.base, and is how the system-call entry path of
 * sub-task 6.2 will find the kernel's per-processor data with nothing but a
 * register the user could not have set.
 */
#define IA32_FS_BASE        UINT32_C(0xC0000100)
#define IA32_GS_BASE        UINT32_C(0xC0000101)
#define IA32_KERNEL_GS_BASE UINT32_C(0xC0000102)

/* Flags of IA32_EFER, per Intel SDM, Volume 3A, Table 2-1. */
#define EFER_SYSTEM_CALL_EXTENSIONS UINT64_C(0x0001)
#define EFER_LONG_MODE_ENABLE       UINT64_C(0x0100)
#define EFER_LONG_MODE_ACTIVE       UINT64_C(0x0400)
#define EFER_NO_EXECUTE_ENABLE      UINT64_C(0x0800)

/*
 * Reads a model-specific register.
 *
 * The value arrives in two halves because the instruction predates the 64-bit
 * general registers: EDX holds the high half and EAX the low, and each is zero
 * extended into its 64-bit register, so the two must be joined explicitly rather
 * than read as one quantity.
 */
static inline uint64_t ReadMsr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));

    return ((uint64_t)high << 32) | (uint64_t)low;
}

/*
 * Writes a model-specific register.
 *
 * The memory clobber is declared because a write here alters how the processor
 * itself behaves — enabling SYSCALL, or moving the address it transfers to — and
 * the compiler must not move a memory reference across it upon the assumption
 * that the instruction touches nothing.
 */
static inline void WriteMsr(uint32_t msr, uint64_t value)
{
    const uint32_t low = (uint32_t)(value & UINT64_C(0xFFFFFFFF));
    const uint32_t high = (uint32_t)(value >> 32);

    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

#endif /* OXYS_MSR_H */
