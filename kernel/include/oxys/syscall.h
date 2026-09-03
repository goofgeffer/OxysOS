/*
 * File: kernel/include/oxys/syscall.h
 * Purpose: Declares the configuration of the fast system-call mechanism: the
 *          three model-specific registers that fix the selectors, the entry
 *          point and the flags cleared upon entry, and the accessors by which
 *          the configuration may be read back and asserted.
 * Key definitions: SYSCALL_FLAG_MASK, SyscallInitialise, SyscallIsEnabled,
 *          SyscallEntryAddress, SyscallStar, SyscallLstar, SyscallFmask,
 *          SyscallDerivedKernelCode, SyscallDerivedUserCode, SyscallEntries,
 *          SyscallReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
 *     "SYSCALL" and "SYSRET": SYSCALL saves the address of the following
 *     instruction in RCX and RFLAGS in R11, loads RIP from IA32_LSTAR, loads CS
 *     and SS from IA32_STAR, and clears in RFLAGS every bit set in IA32_FMASK.
 *     SYSRET performs the inverse and returns to privilege level 3
 *     unconditionally.
 *   - Intel SDM, Volume 3A, Section 5.8.8 (Fast System Calls in 64-Bit Mode):
 *     the derivation of the four selectors from the two IA32_STAR holds, and the
 *     requirement that the descriptors stand at those fixed displacements.
 *   - Intel SDM, Volume 3A, Table 2-1: IA32_EFER bit 0, SCE, without which
 *     SYSCALL raises an invalid-opcode exception.
 *   - Intel SDM, Volume 1, Section 3.4.3: the flags of RFLAGS, from which the
 *     mask below is composed.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.1:
 *     the direction flag is required to be clear at a function's entry, which is
 *     among the reasons it appears in the mask.
 */

#ifndef OXYS_SYSCALL_H
#define OXYS_SYSCALL_H

#include <oxys/types.h>
#include <oxys/cpu.h>

/*
 * The bits SYSCALL clears in RFLAGS upon entry, being those the kernel must not
 * inherit from whoever called it.
 *
 * Each is named rather than a constant being written out, because each is here
 * for a reason of its own:
 *
 *   IF   Interrupts must be off. SYSCALL performs no stack switch: RSP is still
 *        the caller's when the first instruction of the handler runs, and an
 *        interrupt delivered upon a user stack while executing at privilege
 *        level 0 is the whole of the attack this bit prevents. It is the one
 *        bit in this mask whose omission is a hole rather than a nuisance.
 *   TF   The trap flag would single-step the kernel on behalf of a user that
 *        set it, delivering a debug exception at every instruction of the
 *        handler.
 *   DF   The direction flag must be clear at a function's entry by the ABI, and
 *        a kernel that inherited it set would run its string operations
 *        backwards.
 *   NT   The nested-task flag alters what IRET does; a kernel entered with it
 *        set and returning by IRET would attempt a task switch.
 *   AC   Alignment checking, which combined with CR4.SMAP is what makes a
 *        supervisor access to a user page fault. A user that set AC could not be
 *        permitted to disarm that in the kernel.
 *   IOPL Both bits, so that the handler runs at an I/O privilege level of zero
 *        whatever the caller's was.
 */
#define RFLAGS_CARRY              UINT64_C(0x00000001)
#define RFLAGS_PARITY             UINT64_C(0x00000004)
#define RFLAGS_AUXILIARY          UINT64_C(0x00000010)
#define RFLAGS_ZERO               UINT64_C(0x00000040)
#define RFLAGS_SIGN               UINT64_C(0x00000080)
#define RFLAGS_TRAP               UINT64_C(0x00000100)
#define RFLAGS_DIRECTION          UINT64_C(0x00000400)
#define RFLAGS_OVERFLOW           UINT64_C(0x00000800)
#define RFLAGS_IO_PRIVILEGE_LEVEL UINT64_C(0x00003000)
#define RFLAGS_NESTED_TASK        UINT64_C(0x00004000)
#define RFLAGS_ALIGNMENT_CHECK    UINT64_C(0x00040000)

#define SYSCALL_FLAG_MASK                                                        \
    (RFLAGS_TRAP | RFLAGS_INTERRUPT_ENABLE | RFLAGS_DIRECTION |                  \
     RFLAGS_IO_PRIVILEGE_LEVEL | RFLAGS_NESTED_TASK | RFLAGS_ALIGNMENT_CHECK)

/*
 * Enables the mechanism and writes the three registers that configure it. The
 * global descriptor table must already hold the descriptors IA32_STAR names.
 *
 * Returns false where the processor does not report support for the mechanism,
 * in which case nothing is written and SYSCALL remains an invalid opcode. Every
 * processor capable of long mode supports it, so a false return means the
 * machine is not one this kernel can run user programs upon at all.
 */
bool SyscallInitialise(void);

/* Whether IA32_EFER.SCE is set, read back from the register. */
bool SyscallIsEnabled(void);

/* The configuration, read back from the processor rather than from memory, so
 * that a self-test asserts what the processor holds. */
uint64_t SyscallStar(void);
uint64_t SyscallLstar(void);
uint64_t SyscallFmask(void);

/* The address of the entry point, as this kernel means to have installed it. */
uint64_t SyscallEntryAddress(void);

/*
 * The selectors the processor will derive from IA32_STAR, computed here by the
 * same arithmetic the processor performs.
 *
 * They are derived rather than restated so that the self-test asserts the
 * consequence of the configuration and not the configuration itself: a global
 * descriptor table whose user descriptors stood in the wrong order would satisfy
 * an assertion upon IA32_STAR and fail here, which is the failure that would
 * otherwise appear as a general-protection exception at the first return to user
 * mode in sub-task 6.10.
 */
uint16_t SyscallDerivedKernelCode(void);
uint16_t SyscallDerivedKernelStack(void);
uint16_t SyscallDerivedUserCode(void);
uint16_t SyscallDerivedUserStack(void);

/*
 * What the entry point observed the last time it was entered, and how many times
 * it has been entered.
 *
 * These exist for the self-test of this sub-task. Nothing executes SYSCALL yet
 * but that test, there being no user program until sub-task 6.10, and the values
 * the processor loads can be established in no other way: they are loaded by the
 * instruction and are gone by the time it returns.
 */
uint64_t SyscallEntries(void);
uint16_t SyscallObservedCode(void);
uint16_t SyscallObservedStack(void);
uint64_t SyscallObservedFlags(void);

/* Emits the configuration upon the console and the serial port. */
void SyscallReport(void);

#endif /* OXYS_SYSCALL_H */
