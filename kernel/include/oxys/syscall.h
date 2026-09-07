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

/* ------------------------------------------------------------------------------
 * Sub-task 6.7: the entry path, the dispatch table and the validation.
 * ------------------------------------------------------------------------------ */

/*
 * The per-processor block GS names within the kernel.
 *
 * SYSCALL performs no stack switch: RSP is still the caller's when the first
 * instruction of the entry path runs, so the path must find a kernel stack using
 * nothing but a register the caller could not have set. SWAPGS is that register:
 * it exchanges GS.base with IA32_KERNEL_GS_BASE, which privilege level 3 cannot
 * write, so the first instruction of the path can reach this block and nothing
 * the caller does can redirect it.
 *
 * Two fields, and both are needed before a stack exists. The kernel stack is
 * where to go; the scratch is where the caller's stack pointer is put while
 * there is nowhere else to put it — no register may be destroyed and no memory
 * addressed, at that moment, except through GS.
 *
 * Sub-task 6.13 replaces this with the per-processor area proper. These two
 * fields are the beginning of it and their offsets are asserted, because the
 * assembly addresses them by number.
 */
typedef struct SyscallProcessorBlock
{
    uint64_t kernel_stack; /* Offset 0: the top of this processor's kernel stack. */
    uint64_t user_stack;   /* Offset 8: the caller's RSP, while it is homeless. */
} SyscallProcessorBlock;

/*
 * The registers as the entry path saved them, in the order it pushed them.
 *
 * The whole set is saved and not merely the arguments, because everything here
 * belongs to the caller and SYSRET restores none of it. A register the kernel
 * used and did not put back is a register a user program finds changed for no
 * reason it can see, which is the least debuggable class of fault there is.
 *
 * RCX and R11 are the exception in the other direction: the instruction itself
 * put the return address in the first and the caller's flags in the second, so
 * these two fields are not the caller's values but the mechanism's, and SYSRET
 * consumes them.
 */
typedef struct SyscallFrame
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11; /* The caller's RFLAGS, placed here by SYSCALL. */
    uint64_t r10; /* The fourth argument. */
    uint64_t r9;  /* The sixth. */
    uint64_t r8;  /* The fifth. */
    uint64_t rbp;
    uint64_t rdi; /* The first argument. */
    uint64_t rsi; /* The second. */
    uint64_t rdx; /* The third. */
    uint64_t rcx; /* The return address, placed here by SYSCALL. */
    uint64_t rbx;
    uint64_t rax; /* The call number on entry; the result on return. */
    uint64_t user_stack;
} SyscallFrame;

/*
 * Where the arguments are, and why the fourth is not where a C caller would put
 * it.
 *
 * The System V AMD64 convention passes the first six integer arguments in RDI,
 * RSI, RDX, RCX, R8 and R9. SYSCALL destroys RCX — it puts the return address
 * there — so the fourth argument moves to R10 and everything else stands. This
 * is the convention Linux adopted and it is adopted here for the same reason:
 * there is no other register the instruction leaves alone.
 *
 * The call number is in RAX and the result returns in RAX.
 */
#define SYSCALL_ARGUMENT_MAXIMUM 6U

/* The calls this kernel implements. The numbers are its own: there is no library
 * to agree with, and none of these is a POSIX call in anything but spirit. */
#define SYSCALL_WRITE   0U
#define SYSCALL_TICKS   1U
#define SYSCALL_VERSION 2U
#define SYSCALL_COUNT   3U

/*
 * The results a call may fail with.
 *
 * They are negative so that a caller may distinguish a failure from a length or
 * a count without a second register, which is the convention every kernel of
 * this shape uses. The numbers are this kernel's own and are not POSIX's: there
 * is no C library yet to agree with, and inventing agreement with one that does
 * not exist would be inventing a compatibility nobody had tested.
 */
#define SYSCALL_OK             INT64_C(0)
#define SYSCALL_ENOSYS         INT64_C(-1)  /* No such call. */
#define SYSCALL_EFAULT         INT64_C(-2)  /* An address the caller may not use. */
#define SYSCALL_EINVAL         INT64_C(-3)  /* An argument that cannot be right. */
#define SYSCALL_EBADF          INT64_C(-4)  /* No such descriptor. */

/*
 * The boundary between what a user may name and what it may not.
 *
 * Every address at or above this belongs to the kernel. It is the lowest address
 * of the higher half, so the test is the sign bit of the canonical address and
 * costs one comparison — and it is made before the page tables are consulted,
 * because a kernel address that happens to be mapped and marked user would
 * otherwise be accepted by the walk alone.
 */
#define SYSCALL_USER_LIMIT UINT64_C(0x0000800000000000)

/*
 * Whether a range of addresses may be read from, or written to, on behalf of a
 * caller at privilege level 3.
 *
 * Four things are checked and each admits a distinct attack. A length of zero is
 * refused as meaningless rather than accepted as a range of nothing; a range
 * that wraps past the end of the address space is refused, since the sum a
 * careless check performs would be smaller than either operand and would appear
 * to lie within bounds; every byte must lie below SYSCALL_USER_LIMIT; and every
 * page of it must be mapped, marked accessible to privilege level 3, and — for a
 * write — writable.
 *
 * The page walk is per page and not per range, because a range may span a page
 * that is mapped and one that is not, and a check of the first byte alone is a
 * check of the first byte alone.
 */
bool SyscallUserRangeIsReadable(uint64_t address, uint64_t length);
bool SyscallUserRangeIsWritable(uint64_t address, uint64_t length);

/*
 * Whether a call number names an implemented call.
 *
 * Exposed because the bound and the table must agree, and because a number
 * beyond the table is the first thing a hostile caller will try: the table is
 * indexed by it, so a bound that were wrong by one would call whatever function
 * pointer lay after the array.
 */
bool SyscallNumberIsValid(uint64_t number);

/* The name of a call, for the report and the self-test; null beyond the table. */
const char *SyscallName(uint64_t number);

/*
 * Performs the call the frame describes and places the result in its RAX.
 *
 * This is what the entry path calls once it has a stack. It is an ordinary
 * function of an ordinary structure, which is what allows the whole of the
 * dispatch and the validation to be asserted without executing SYSCALL at all —
 * and there is no user program to execute one until sub-task 6.10.
 */
void SyscallDispatch(SyscallFrame *frame);

/* Accounting. */
uint64_t SyscallDispatched(void);
uint64_t SyscallRefused(void);
uint64_t SyscallFaulted(void);

/* Emits the configuration upon the console and the serial port. */
void SyscallReport(void);

#endif /* OXYS_SYSCALL_H */
