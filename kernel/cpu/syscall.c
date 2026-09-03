/*
 * File: kernel/cpu/syscall.c
 * Purpose: Configures the fast system-call mechanism: enables it in IA32_EFER,
 *          writes the selectors into IA32_STAR, the entry point into IA32_LSTAR
 *          and the flag mask into IA32_FMASK, and reads all four back so that
 *          the configuration may be asserted rather than assumed.
 * Key functions: SyscallInitialise, SyscallIsEnabled, SyscallStar, SyscallLstar,
 *          SyscallFmask, SyscallDerivedKernelCode, SyscallDerivedUserCode,
 *          SyscallReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 5.8.8: the derivation of the four selectors from IA32_STAR.
 *   - Intel SDM, Volume 2B, "SYSCALL" and "SYSRET".
 *   - Intel SDM, Volume 3A, Table 2-1: IA32_EFER.SCE.
 *   - Intel SDM, Volume 2A, "CPUID": leaf 0x80000001, bit 11 of EDX reports
 *     SYSCALL and SYSRET. The leaf must be confirmed to exist before it is
 *     read, by comparing the greatest extended leaf the processor supports.
 *
 * Design note. Nothing here dispatches anything. Sub-task 6.1 establishes only
 * that the transition is *configured*: that the processor knows which
 * descriptors to load, where to transfer, and what to clear from the flags. The
 * entry path, the dispatch table and the validation of a user's arguments are
 * sub-task 6.7, and the entry point installed here is a placeholder that
 * sub-task 6.7 replaces; the reasons it is shaped as it is are recorded in
 * kernel/cpu/syscall_entry.asm.
 *
 * Concurrency. Every register written here is per-processor. Sub-task 6.14 must
 * repeat this configuration upon each application processor as it is brought up,
 * with the same values; a processor that entered user mode without it would find
 * SYSCALL an invalid opcode.
 */

#include <oxys/syscall.h>
#include <oxys/msr.h>
#include <oxys/gdt.h>
#include <oxys/kernel.h>

/* Defined in kernel/cpu/syscall_entry.asm. */
extern void SyscallEntry(void);

/*
 * What the entry point observed, written by the assembly routine.
 *
 * The storage is declared here rather than there so that its type and its
 * initial value are stated in C, the assembly holding only the instructions that
 * write it.
 */
uint16_t SyscallObservedCodeSelector;
uint16_t SyscallObservedStackSelector;
uint64_t SyscallObservedFlagsValue;
uint64_t SyscallEntryCount;

/* Whether the configuration was written. */
static bool SyscallConfigured;

/*
 * Whether the processor reports the mechanism.
 *
 * The extended leaf must be established to exist before it is read: a processor
 * that does not implement it returns the contents of the highest leaf it does
 * implement, which is arbitrary data that may well have bit 11 of EDX set.
 */
static bool SyscallIsSupported(void)
{
    uint32_t highest = 0U;
    uint32_t unused_b = 0U;
    uint32_t unused_c = 0U;
    uint32_t features = 0U;

    __asm__ __volatile__("cpuid"
                         : "=a"(highest), "=b"(unused_b), "=c"(unused_c), "=d"(features)
                         : "a"(UINT32_C(0x80000000)));

    if (highest < UINT32_C(0x80000001))
    {
        return false;
    }

    __asm__ __volatile__("cpuid"
                         : "=a"(highest), "=b"(unused_b), "=c"(unused_c), "=d"(features)
                         : "a"(UINT32_C(0x80000001)));

    /* Bit 11 of EDX: SYSCALL and SYSRET are available. */
    return (features & (UINT32_C(1) << 11)) != 0U;
}

bool SyscallInitialise(void)
{
    uint64_t star;

    if (!SyscallIsSupported())
    {
        return false;
    }

    /*
     * The selectors are written before the mechanism is enabled. The order
     * matters at one moment and in one direction: between enabling SYSCALL and
     * configuring where it transfers to, the instruction is valid and IA32_LSTAR
     * is whatever it held — zero upon a processor freshly reset. Nothing in this
     * kernel executes SYSCALL, so the window is theoretical; the order costs
     * nothing and the reverse would be a transfer to address zero at privilege
     * level 0.
     *
     * IA32_STAR[47:32] is the selector pair SYSCALL loads and [63:48] the pair
     * SYSRET derives from; the low 32 bits are the entry point of the mechanism
     * as it existed in 32-bit mode and are ignored in 64-bit mode.
     */
    star = ((uint64_t)GDT_KERNEL_CODE_SELECTOR << 32) |
           ((uint64_t)GDT_USER_CODE32_SELECTOR << 48);

    WriteMsr(IA32_STAR, star);
    WriteMsr(IA32_LSTAR, (uint64_t)(uintptr_t)&SyscallEntry);
    WriteMsr(IA32_FMASK, SYSCALL_FLAG_MASK);

    /*
     * IA32_CSTAR is the entry point taken when SYSCALL is executed from
     * compatibility mode. This kernel supports no such mode and installs no
     * descriptor a program could enter it through, so the register is left as it
     * stands rather than being given an address that would suggest the path
     * exists.
     */

    WriteMsr(IA32_EFER, ReadMsr(IA32_EFER) | EFER_SYSTEM_CALL_EXTENSIONS);

    SyscallConfigured = true;
    return true;
}

bool SyscallIsEnabled(void)
{
    return (ReadMsr(IA32_EFER) & EFER_SYSTEM_CALL_EXTENSIONS) != 0U;
}

uint64_t SyscallStar(void)
{
    return ReadMsr(IA32_STAR);
}

uint64_t SyscallLstar(void)
{
    return ReadMsr(IA32_LSTAR);
}

uint64_t SyscallFmask(void)
{
    return ReadMsr(IA32_FMASK);
}

uint64_t SyscallEntryAddress(void)
{
    return (uint64_t)(uintptr_t)&SyscallEntry;
}

/*
 * The four selectors, derived from IA32_STAR by the arithmetic Section 5.8.8
 * states the processor performs. The requested privilege level is forced to 3
 * upon the two SYSRET loads, which is done here for the same reason.
 */
uint16_t SyscallDerivedKernelCode(void)
{
    return (uint16_t)((SyscallStar() >> 32) & UINT64_C(0xFFFF));
}

uint16_t SyscallDerivedKernelStack(void)
{
    return (uint16_t)(SyscallDerivedKernelCode() + 8U);
}

uint16_t SyscallDerivedUserCode(void)
{
    const uint16_t base = (uint16_t)((SyscallStar() >> 48) & UINT64_C(0xFFFF));

    return (uint16_t)((base + 16U) | GDT_REQUESTED_PRIVILEGE_USER);
}

uint16_t SyscallDerivedUserStack(void)
{
    const uint16_t base = (uint16_t)((SyscallStar() >> 48) & UINT64_C(0xFFFF));

    return (uint16_t)((base + 8U) | GDT_REQUESTED_PRIVILEGE_USER);
}

uint64_t SyscallEntries(void)
{
    return SyscallEntryCount;
}

uint16_t SyscallObservedCode(void)
{
    return SyscallObservedCodeSelector;
}

uint16_t SyscallObservedStack(void)
{
    return SyscallObservedStackSelector;
}

uint64_t SyscallObservedFlags(void)
{
    return SyscallObservedFlagsValue;
}

void SyscallReport(void)
{
    if (!SyscallConfigured)
    {
        KernelWriteString("System call: not configured; the processor does not report "
                          "SYSCALL.\n");
        return;
    }

    KernelWriteString("System call: SYSCALL enabled, entry at ");
    KernelWriteHexadecimal(SyscallLstar());
    KernelWriteString(".\n");

    KernelWriteString("  IA32_STAR ");
    KernelWriteHexadecimal(SyscallStar());
    KernelWriteString(" derives CS ");
    KernelWriteHexadecimal((uint64_t)SyscallDerivedKernelCode());
    KernelWriteString(" and SS ");
    KernelWriteHexadecimal((uint64_t)SyscallDerivedKernelStack());
    KernelWriteString(" upon entry, CS ");
    KernelWriteHexadecimal((uint64_t)SyscallDerivedUserCode());
    KernelWriteString(" and SS ");
    KernelWriteHexadecimal((uint64_t)SyscallDerivedUserStack());
    KernelWriteString(" upon return.\n");

    KernelWriteString("  IA32_FMASK ");
    KernelWriteHexadecimal(SyscallFmask());
    KernelWriteString("; the interrupt flag is ");
    KernelWriteString(((SyscallFmask() & RFLAGS_INTERRUPT_ENABLE) != 0U)
                          ? "cleared upon entry.\n"
                          : "NOT cleared upon entry.\n");

    KernelWriteString("  Entries so far ");
    KernelWriteDecimal(SyscallEntryCount);
    KernelWriteString(".\n");
}
