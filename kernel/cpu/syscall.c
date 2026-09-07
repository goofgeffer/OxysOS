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
#include <oxys/memory.h>
#include <oxys/paging.h>
#include <oxys/pit.h>
#include <oxys/tss.h>

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

/*
 * The block GS names within the kernel.
 *
 * One, because there is one processor until sub-task 6.14. It is file-scope
 * rather than allocated because the entry path reaches it before a stack exists
 * and could not have been told where it was.
 */
static SyscallProcessorBlock SyscallBlock;

/*
 * The assembly addresses these fields by number, having no sight of the
 * structure. A field reordered here without the assembly would put the caller's
 * stack pointer where the kernel stack belongs, and the very next instruction
 * would load RSP from it — which is a kernel running upon an address privilege
 * level 3 chose.
 */
_Static_assert(offsetof(SyscallProcessorBlock, kernel_stack) == 0U,
               "The assembly reads the kernel stack at offset 0 of the block.");
_Static_assert(offsetof(SyscallProcessorBlock, user_stack) == 8U,
               "The assembly writes the caller's stack at offset 8 of the block.");

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

    /*
     * The per-processor block, and the register through which the entry path
     * finds it.
     *
     * IA32_KERNEL_GS_BASE is what SWAPGS exchanges GS.base with, and privilege
     * level 3 cannot write it — which is the whole reason the entry path can
     * trust it. GS.base itself is set to zero, that being the value a user
     * program has until something gives it one; SWAPGS exchanges the two, so
     * after it the kernel has its block and the caller's value is held for the
     * return.
     *
     * The kernel stack is the one the task state segment names, which is the
     * same stack an interrupt from privilege level 3 would arrive upon. They are
     * deliberately the same: a system call and an interrupt are both entries to
     * the kernel from a user program, and a kernel that used two stacks for them
     * would have to say which was which at every point that examined one.
     */
    SyscallBlock.kernel_stack = TssKernelStack();
    SyscallBlock.user_stack = 0U;

    WriteMsr(IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&SyscallBlock);
    WriteMsr(IA32_GS_BASE, 0U);

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


/* ------------------------------------------------------------------------------
 * Sub-task 6.7: the per-processor block, the validation, and the dispatch.
 * ------------------------------------------------------------------------------ */


/*
 * The frame the assembly builds by pushing. Its fields must be in the reverse of
 * the push order and there must be no padding, or the dispatcher reads one
 * register and calls it another.
 */
_Static_assert(sizeof(SyscallFrame) == (16U * 8U),
               "The frame is sixteen quadwords, as many as the entry path pushes.");
_Static_assert(offsetof(SyscallFrame, r15) == 0U, "R15 is pushed last and lies lowest.");
_Static_assert(offsetof(SyscallFrame, rax) == (14U * 8U), "RAX carries the call number.");
_Static_assert(offsetof(SyscallFrame, user_stack) == (15U * 8U),
               "The caller's stack is pushed first and lies highest.");

/* Accounting. */
static uint64_t SyscallCalls;
static uint64_t SyscallRefusals;
static uint64_t SyscallFaults;

/* -------------------------------------------------------------- validation */

/*
 * Whether a range lies wholly within what privilege level 3 may name.
 *
 * The wrap test is written as a subtraction and not as a sum. `address + length`
 * overflows for a length near the greatest value, and the sum is then *smaller*
 * than the address — so a range that manifestly runs off the end of the address
 * space would appear to lie within bounds. Comparing the length against what
 * remains below the limit cannot overflow and cannot be fooled.
 */
static bool SyscallRangeIsWithinUserSpace(uint64_t address, uint64_t length)
{
    if ((length == 0U) || (address == 0U))
    {
        return false;
    }

    if (address >= SYSCALL_USER_LIMIT)
    {
        return false;
    }

    return length <= (SYSCALL_USER_LIMIT - address);
}

/*
 * Whether every page of a range is mapped and permitted.
 *
 * Every page, and not the first: a range may begin upon a page that is mapped
 * and end upon one that is not, and a kernel that checked the first byte alone
 * would fault in the middle of a copy it had already promised to complete. The
 * walk steps by pages rather than by bytes, the permissions being a property of
 * the page.
 */
static bool SyscallPagesArePermitted(uint64_t address, uint64_t length, bool writing)
{
    const uint64_t last = address + (length - 1U);

    for (uint64_t page = AlignDown(address, PAGE_SIZE); page <= AlignDown(last, PAGE_SIZE);
         page += PAGE_SIZE)
    {
        if (!PagingAddressIsUser(page))
        {
            return false;
        }

        if (writing && !PagingAddressIsWritable(page))
        {
            return false;
        }
    }

    return true;
}

bool SyscallUserRangeIsReadable(uint64_t address, uint64_t length)
{
    return SyscallRangeIsWithinUserSpace(address, length) &&
           SyscallPagesArePermitted(address, length, false);
}

bool SyscallUserRangeIsWritable(uint64_t address, uint64_t length)
{
    return SyscallRangeIsWithinUserSpace(address, length) &&
           SyscallPagesArePermitted(address, length, true);
}

/* ------------------------------------------------------------------ the calls */

/*
 * The greatest length a single call may transfer.
 *
 * A bound is needed and any bound would do; what must not happen is a caller
 * naming a length of several gibibytes and the kernel walking every page of it
 * before refusing. The validation is proportional to the length, so the length
 * is what must be bounded first.
 */
#define SYSCALL_TRANSFER_MAXIMUM 4096U

/* The descriptors a write may name. There are no files until Phase 7; these are
 * the diagnostic path, which is what a program has to say anything with. */
#define SYSCALL_DESCRIPTOR_OUTPUT 1U
#define SYSCALL_DESCRIPTOR_ERROR  2U

/*
 * Writes a caller's bytes to the diagnostic path.
 *
 * The bytes are copied into the kernel's own buffer before any of them is used,
 * and that is not a convenience. A kernel that read the caller's memory
 * repeatedly — once to validate and once to use — would be reading memory that
 * another processor may have changed in between, so what was validated and what
 * was used need not be the same bytes. Copying once closes that window, and from
 * sub-task 6.14 there is a second processor to open it.
 */
static int64_t SyscallDoWrite(uint64_t descriptor, uint64_t address, uint64_t length)
{
    char buffer[SYSCALL_TRANSFER_MAXIMUM + 1U];
    const char *const source = (const char *)(uintptr_t)address;

    if ((descriptor != SYSCALL_DESCRIPTOR_OUTPUT) && (descriptor != SYSCALL_DESCRIPTOR_ERROR))
    {
        return SYSCALL_EBADF;
    }

    if (length > SYSCALL_TRANSFER_MAXIMUM)
    {
        length = SYSCALL_TRANSFER_MAXIMUM;
    }

    if (!SyscallUserRangeIsReadable(address, length))
    {
        ++SyscallFaults;
        return SYSCALL_EFAULT;
    }

    for (uint64_t index = 0U; index < length; ++index)
    {
        buffer[index] = source[index];
    }

    buffer[length] = '\0';
    KernelWriteString(buffer);

    return (int64_t)length;
}

/* The interval timer's count, which is the only clock this kernel has to offer
 * and needs no argument and no validation. */
static int64_t SyscallDoTicks(void)
{
    return (int64_t)PitTickCount();
}

/*
 * Copies the system's name and version into a caller's buffer.
 *
 * This exists because the validation of a *write* to a caller's memory must be
 * exercised by something, and a call that only reads would leave half the
 * validation asserted by the self-test alone. The length returned is what was
 * copied, not what was asked for.
 */
static int64_t SyscallDoVersion(uint64_t address, uint64_t length)
{
    static const char version[] = OXYS_SYSTEM_NAME " " OXYS_VERSION_STRING;
    char *const destination = (char *)(uintptr_t)address;
    uint64_t copied = 0U;

    if (length == 0U)
    {
        return SYSCALL_EINVAL;
    }

    if (length > SYSCALL_TRANSFER_MAXIMUM)
    {
        length = SYSCALL_TRANSFER_MAXIMUM;
    }

    if (!SyscallUserRangeIsWritable(address, length))
    {
        ++SyscallFaults;
        return SYSCALL_EFAULT;
    }

    while ((copied < (length - 1U)) && (version[copied] != '\0'))
    {
        destination[copied] = version[copied];
        ++copied;
    }

    destination[copied] = '\0';

    return (int64_t)copied;
}

/* ------------------------------------------------------------- the dispatch */

/* A call: what it is named, and how many arguments it reads. The count is
 * recorded so that the report says something a reader can check the caller
 * against; nothing is refused upon it, the registers being there either way. */
typedef struct SyscallEntryDescriptor
{
    const char *name;
    uint8_t arguments;
} SyscallEntryDescriptor;

static const SyscallEntryDescriptor SyscallTable[SYSCALL_COUNT] = {
    { "write", 3U },
    { "ticks", 0U },
    { "version", 2U }
};

bool SyscallNumberIsValid(uint64_t number)
{
    return number < SYSCALL_COUNT;
}

const char *SyscallName(uint64_t number)
{
    return SyscallNumberIsValid(number) ? SyscallTable[number].name : NULL;
}

void SyscallDispatch(SyscallFrame *frame)
{
    if (frame == NULL)
    {
        return;
    }

    /*
     * The number is checked before it is used for anything, and it is unsigned,
     * so a caller passing a negative number passes an enormous one and is
     * refused by the same comparison. A signed number checked only against the
     * upper bound would index the table backwards.
     */
    if (!SyscallNumberIsValid(frame->rax))
    {
        ++SyscallRefusals;
        frame->rax = (uint64_t)SYSCALL_ENOSYS;
        return;
    }

    ++SyscallCalls;

    switch (frame->rax)
    {
    case SYSCALL_WRITE:
        frame->rax = (uint64_t)SyscallDoWrite(frame->rdi, frame->rsi, frame->rdx);
        break;

    case SYSCALL_TICKS:
        frame->rax = (uint64_t)SyscallDoTicks();
        break;

    case SYSCALL_VERSION:
        frame->rax = (uint64_t)SyscallDoVersion(frame->rdi, frame->rsi);
        break;

    default:
        /*
         * Unreachable while the table and the switch agree, and present because
         * they are two lists that must: a call added to one and not the other
         * would otherwise fall through this function and return whatever the
         * caller had in RAX, which is the number it asked for and looks like
         * success.
         */
        ++SyscallRefusals;
        frame->rax = (uint64_t)SYSCALL_ENOSYS;
        break;
    }
}

uint64_t SyscallDispatched(void)
{
    return SyscallCalls;
}

uint64_t SyscallRefused(void)
{
    return SyscallRefusals;
}

uint64_t SyscallFaulted(void)
{
    return SyscallFaults;
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

    KernelWriteString("  ");
    KernelWriteDecimal((uint64_t)SYSCALL_COUNT);
    KernelWriteString(" calls:");

    for (uint64_t number = 0U; number < SYSCALL_COUNT; ++number)
    {
        KernelWriteString(" ");
        KernelWriteDecimal(number);
        KernelWriteString(" ");
        KernelWriteString(SyscallTable[number].name);
        KernelWriteString("/");
        KernelWriteDecimal((uint64_t)SyscallTable[number].arguments);
    }

    KernelWriteString("\n");

    KernelWriteString("  Dispatched ");
    KernelWriteDecimal(SyscallCalls);
    KernelWriteString(", refused ");
    KernelWriteDecimal(SyscallRefusals);
    KernelWriteString(", addresses rejected ");
    KernelWriteDecimal(SyscallFaults);
    KernelWriteString(", entries through SYSCALL ");
    KernelWriteDecimal(SyscallEntryCount);
    KernelWriteString(".\n");
}
