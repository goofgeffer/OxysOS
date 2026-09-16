/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/syscall/syscall.c
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
 * kernel/arch/x86_64/syscall/syscall_entry.asm.
 *
 * Concurrency. Every register written here is per-processor. Sub-task 6.14 must
 * repeat this configuration upon each application processor as it is brought up,
 * with the same values; a processor that entered user mode without it would find
 * SYSCALL an invalid opcode.
 */

#include <oxys/arch/syscall/syscall.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/arch/cpu/msr.h>
#include <oxys/arch/cpu/gdt.h>
#include <oxys/kernel.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/mm/paging.h>
#include <oxys/dev/pit.h>
#include <oxys/arch/cpu/tss.h>
#include <oxys/proc/process.h>
#include <oxys/fs/vfs.h>
#include <oxys/terminal/terminal.h>

/* Defined in kernel/arch/x86_64/syscall/syscall_entry.asm. */
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
 * The block GS names within the kernel is the executing processor's own area.
 *
 * It was a structure of this file's own from sub-task 6.7 until sub-task 6.13,
 * which replaced it with the per-processor area <oxys/arch/cpu/percpu.h> declares — the
 * same two fields at the same two offsets, and the rest of what a processor owns
 * after them. The offsets the entry path addresses by number are asserted in
 * kernel/arch/x86_64/cpu/percpu.c, beside the structure they belong to.
 */

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
     * The stack the entry path will switch to.
     *
     * It is the one the task state segment names, which is the same stack an
     * interrupt from privilege level 3 would arrive upon. They are deliberately
     * the same: a system call and an interrupt are both entries to the kernel
     * from a user program, and a kernel that used two stacks for them would have
     * to say which was which at every point that examined one.
     *
     * The area itself, and GS.base, were established by PerCpuInitialise long
     * before this — a spinlock reaches the area, and locks are taken from the
     * first allocation onwards. What is written here is the one field that could
     * not be known then, the task state segment not having existed.
     *
     * The registers are settled by SyscallEstablishKernelGsBase rather than
     * written directly, because the invariant they express has a direction: the
     * kernel is executing at this moment, so the area belongs in GS.base and
     * IA32_KERNEL_GS_BASE holds what a user program would have. Writing them the
     * other way round — which this function did until sub-task 6.13, when
     * nothing in the kernel read GS — leaves the kernel executing with a segment
     * base of zero, and the first per-processor access after it reaching for
     * address sixteen.
     */
    SyscallSetKernelStack(TssKernelStack());
    SyscallEstablishKernelGsBase();

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

        /*
         * A page the kernel means to write that is not writable may still be
         * writable to its owner: `fork` withdraws write permission from every
         * shared page of both hierarchies, so a buffer a program passed to a
         * call before forking is read-only afterwards and yet is the program's
         * to write.
         *
         * The fault is therefore resolved here rather than provoked. Provoking
         * it is not an option: CR0.WP has been set since sub-task 3.4, so the
         * kernel's own write to such a page raises a page fault at privilege
         * level 0, and a fault at privilege level 0 is a panic. Refusing the
         * call instead would be worse than either, being a call that fails for a
         * reason the caller cannot see and cannot correct — it would touch the
         * page itself to correct it, which is the very thing it asked the kernel
         * to do.
         */
        if (writing && !PagingAddressIsWritable(page) &&
            !PagingResolveCopyOnWriteFault(page))
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

bool SyscallCopyUserString(uint64_t address, char *destination, size_t capacity)
{
    const char *const source = (const char *)(uintptr_t)address;

    if ((destination == NULL) || (capacity == 0U))
    {
        return false;
    }

    destination[0] = '\0';

    for (size_t index = 0U; index < capacity; ++index)
    {
        /*
         * One byte validated, then one byte read, and in that order for every
         * byte of the string.
         *
         * The length is not known until the terminator is found, so there is no
         * range to validate in advance: a caller may name the last byte of a
         * mapped page and the string may continue onto a page that is not
         * mapped. Validating each byte as it is reached is what makes that case
         * a refusal rather than a page fault raised by the kernel upon itself.
         */
        if (!SyscallUserRangeIsReadable(address + (uint64_t)index, 1U))
        {
            return false;
        }

        destination[index] = source[index];

        if (source[index] == '\0')
        {
            return true;
        }
    }

    /* No terminator within the capacity. The bytes copied are discarded rather
     * than terminated at the bound: a path silently shortened would name a
     * different file, and acting upon the wrong file is worse than refusing. */
    destination[0] = '\0';

    return false;
}

/* ------------------------------------------------- the state a transition owes */

void SyscallSetKernelStack(uint64_t top)
{
    PerCpuCurrent()->kernel_stack = top;
}

/*
 * The two boundaries, and why the registers are written rather than exchanged.
 *
 * Both obtain the area first and write the registers afterwards. That order is
 * load-bearing in the second of them: the area is reached through GS.base, and
 * the second thing that function does is clear GS.base, so a version that
 * cleared first would have nothing left to read the area through.
 *
 * docs/design/PROCESS.md, Section 12, records why a context switch writes these
 * registers instead of exchanging them: which of the two holds the area depends
 * upon how the kernel was entered and not upon which thread is running, and a
 * switch cannot tell those apart.
 */
void SyscallEstablishKernelGsBase(void)
{
    PerCpu *const area = PerCpuCurrent();

    WriteMsr(IA32_GS_BASE, (uint64_t)(uintptr_t)area);
    WriteMsr(IA32_KERNEL_GS_BASE, 0U);
}

void SyscallEstablishUserGsBase(void)
{
    PerCpu *const area = PerCpuCurrent();

    WriteMsr(IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)area);
    WriteMsr(IA32_GS_BASE, 0U);
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

/*
 * The descriptors a write may name: the diagnostic path, and nothing else.
 *
 * The numbers moved to <oxys/syscall_abi.h> at sub-task 7.6, because a program
 * that opens a file has to know which numbers it will never be given. They are
 * used here by those names.
 *
 * **A write still reaches no file**, which is the one place where the
 * descriptors of 7.6 stop short: `open` accepts the read flags alone, so there
 * is no descriptor a program could write to even if this accepted one. See
 * docs/design/LIBC.md, Section 12.7, limitation 2.
 */

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

/* ------------------------------------------- the four calls of sub-task 6.11 */

/*
 * Makes a child of the calling process.
 *
 * The frame is passed on rather than its fields, because the child needs the
 * whole of it: the address the parent will return to, the stack it will return
 * upon, and every register it is entitled to find unchanged. The dispatcher
 * writes this function's result into the parent's RAX afterwards, so the copy
 * the child keeps is taken before the parent's own return value exists — which
 * is what leaves the child's RAX free to be set to zero.
 */
static int64_t SyscallDoFork(const SyscallFrame *frame)
{
    Process *const parent = ProcessCurrent();
    Process *child;

    if (parent == NULL)
    {
        return SYSCALL_EINVAL;
    }

    child = ProcessFork(parent, frame);

    if (child == NULL)
    {
        return SYSCALL_ENOMEM;
    }

    return (int64_t)child->id;
}

/*
 * Reduces an absolute path to its canonical form, in place: no `.` component,
 * no `..` component, no repeated or trailing separator, and `/` for the root.
 * `..` at the root stays at the root, as IEEE Std 1003.1-2017, Section 4.13,
 * has it. Lexical, and therefore what every shell's `pwd -L` reports; a
 * symbolic link in the path is not followed, which Section 8 of
 * docs/design/SHELL.md records.
 *
 * It is applied to the working directory alone. A path a call is given is
 * resolved by the filesystem layer, which reads `.` and `..` off the volume
 * and follows a link where one stands; the working directory is stored and
 * reported, and a stored `/bin/../bin/.` would be reported as typed.
 */
static void SyscallCanonicalisePath(char *path)
{
    size_t read = 0U;
    size_t write = 0U;

    while (path[read] != '\0')
    {
        size_t length = 0U;

        /* Skip the separators before a component. */
        while (path[read] == '/')
        {
            ++read;
        }

        while ((path[read + length] != '\0') && (path[read + length] != '/'))
        {
            ++length;
        }

        if (length == 0U)
        {
            break;
        }

        if ((length == 1U) && (path[read] == '.'))
        {
            read += length;
            continue;
        }

        if ((length == 2U) && (path[read] == '.') && (path[read + 1U] == '.'))
        {
            /* Back over the last component written, if any. */
            while ((write > 0U) && (path[write - 1U] != '/'))
            {
                --write;
            }

            if (write > 0U)
            {
                --write;
            }

            read += length;
            continue;
        }

        path[write++] = '/';

        for (size_t index = 0U; index < length; ++index)
        {
            path[write++] = path[read + index];
        }

        read += length;
    }

    if (write == 0U)
    {
        path[write++] = '/';
    }

    path[write] = '\0';
}

/*
 * Copies a path out of a caller's memory and makes it absolute.
 *
 * A path that does not begin with `/` is the caller's working directory, a
 * separator, and the path — since sub-task 8.3, and here, in the one function
 * every call that takes a path copies through, so that no call resolves a
 * relative path differently from another or forgets to. The bound is upon the
 * absolute result and not upon what was typed: a relative path that fits the
 * copy and does not fit once joined is ENAMETOOLONG, which is the same
 * refusal an absolute path of that length receives.
 *
 * And it says which of the two things went wrong where something did.
 *
 * `SyscallCopyUserString` returns one `false` for two causes — memory the caller
 * may not read, and a string longer than the room given — and every call that
 * used it before this sub-task reported EFAULT for both. That was wrong and a
 * program written by this sub-task found it: a path of three hundred characters,
 * entirely within the program's own memory, was refused as an address the
 * program may not use. A person reading that diagnostic looks for a pointer
 * defect, and there is none.
 *
 * The two are told apart by asking whether the first byte was readable, which it
 * was if the refusal was about length. That is one extra page walk upon a path
 * that has already failed, and it buys the difference between "this address is
 * not yours" and "this path is too long" — which are the only two things the
 */
static int64_t SyscallCopyUserPath(uint64_t address, char *destination, size_t capacity)
{
    const Process *const process = ProcessCurrent();

    if (!SyscallCopyUserString(address, destination, capacity))
    {
        if (SyscallUserRangeIsReadable(address, 1U))
        {
            return SYSCALL_ENAMETOOLONG;
        }

        ++SyscallFaults;

        return SYSCALL_EFAULT;
    }

    if ((destination[0] != '/') && (process != NULL))
    {
        char joined[SYSCALL_PATH_MAXIMUM + 1U];
        size_t used = 0U;
        const char *const directory = process->working_directory;

        while (directory[used] != '\0')
        {
            joined[used] = directory[used];
            ++used;
        }

        /* The root already ends in its separator; every other directory needs
         * one before the path. */
        if ((used == 0U) || (joined[used - 1U] != '/'))
        {
            if (used >= SYSCALL_PATH_MAXIMUM)
            {
                return SYSCALL_ENAMETOOLONG;
            }

            joined[used++] = '/';
        }

        for (size_t index = 0U; destination[index] != '\0'; ++index)
        {
            if (used >= SYSCALL_PATH_MAXIMUM)
            {
                return SYSCALL_ENAMETOOLONG;
            }

            joined[used++] = destination[index];
        }

        joined[used] = '\0';

        if (used >= capacity)
        {
            return SYSCALL_ENAMETOOLONG;
        }

        for (size_t index = 0U; index <= used; ++index)
        {
            destination[index] = joined[index];
        }
    }

    return SYSCALL_OK;
}

/*
 * Copies one of `execve`'s two vectors out of a caller's memory into the block
 * the new program's stack will be built from.
 *
 * Every string is copied **before** the caller's address space is destroyed, and
 * that is the whole reason this function exists rather than the vectors being
 * read where they stand. `execve` replaces an address space; a kernel that read
 * `argv[1]` after the replacement would read whatever the new program has at
 * that address, which is a fault if it is lucky and the new program's own data
 * if it is not — and the second is a program started with arguments nobody
 * wrote.
 *
 * A null vector is not a failure: it is an empty one, which is what a program
 * passing no arguments has to hand.
 *
 * Returns a SYSCALL_ result. EFAULT for memory that is not the caller's to read,
 * and EINVAL where either bound of <oxys/syscall_abi.h> is exceeded — both
 * before the point of no return, so a program refused here keeps running.
 */
static int64_t SyscallCopyUserVector(uint64_t vector, ProcessArguments *arguments,
                                     uint32_t *displacement, uint32_t *count)
{
    *count = 0U;

    if (vector == 0U)
    {
        return SYSCALL_OK;
    }

    for (;;)
    {
        const uint64_t slot = vector + ((uint64_t)*count * sizeof(uint64_t));
        uint64_t string;
        size_t capacity;
        size_t length = 0U;

        if (!SyscallUserRangeIsReadable(slot, sizeof(uint64_t)))
        {
            return SYSCALL_EFAULT;
        }

        string = *(const uint64_t *)(uintptr_t)slot;

        if (string == 0U)
        {
            return SYSCALL_OK;
        }

        if (*count >= PROCESS_ARGUMENT_COUNT_MAXIMUM)
        {
            return SYSCALL_EINVAL;
        }

        /*
         * The copy is bounded by what is left of the block and not by
         * SYSCALL_PATH_MAXIMUM, so that sixteen short arguments and one long one
         * are both accommodated by the same two kibibytes rather than by a
         * per-string bound that would have to be guessed at.
         */
        capacity = (size_t)(PROCESS_ARGUMENT_BYTES_MAXIMUM - arguments->storage_used);

        if (capacity == 0U)
        {
            return SYSCALL_EINVAL;
        }

        if (!SyscallCopyUserString(string, &arguments->storage[arguments->storage_used],
                                   capacity))
        {
            /*
             * Two causes arrive here and they are not the same: memory the
             * caller may not read, and a string longer than the room left. The
             * first is EFAULT and the second EINVAL, and they are told apart by
             * asking whether the first byte was readable — which it was, if the
             * refusal was about length.
             */
            return SyscallUserRangeIsReadable(string, 1U) ? SYSCALL_EINVAL : SYSCALL_EFAULT;
        }

        displacement[*count] = arguments->storage_used;

        while (arguments->storage[arguments->storage_used + length] != '\0')
        {
            ++length;
        }

        arguments->storage_used += (uint32_t)(length + 1U);
        ++(*count);
    }
}

static int64_t SyscallDoExecve(uint64_t path_address, uint64_t argument_vector,
                               uint64_t environment_vector)
{
    Process *const process = ProcessCurrent();
    char path[SYSCALL_PATH_MAXIMUM + 1U];
    ProcessArguments arguments;
    int64_t copied;

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    arguments.storage_used = 0U;

    copied = SyscallCopyUserVector(argument_vector, &arguments, arguments.argument,
                                   &arguments.argument_count);

    if (copied != SYSCALL_OK)
    {
        if (copied == SYSCALL_EFAULT)
        {
            ++SyscallFaults;
        }

        return copied;
    }

    copied = SyscallCopyUserVector(environment_vector, &arguments, arguments.environment,
                                   &arguments.environment_count);

    if (copied != SYSCALL_OK)
    {
        if (copied == SYSCALL_EFAULT)
        {
            ++SyscallFaults;
        }

        return copied;
    }

    copied = SyscallCopyUserPath(path_address, path, sizeof path);

    if (copied != SYSCALL_OK)
    {
        return copied;
    }

    /*
     * Upon success this does not return: the caller is already executing the new
     * program at privilege level 3. Its result is therefore always a refusal,
     * and it is passed on as it stands rather than reduced to one — a program
     * told that its file does not exist, when the machine had in fact run out of
     * memory, would look for the fault in the one place it is not.
     */
    return ProcessExecute(process, path, &arguments);
}

/* Ends the calling program. Does not return; the result exists so that the
 * switch below has one, and so that a path that somehow came back would report
 * something a reader could recognise as impossible. */
static int64_t SyscallDoExit(uint64_t status)
{
    ProcessExit((int64_t)status);

    return SYSCALL_EINVAL;
}

/*
 * Collects a child that has ended, running it first if it has not yet run.
 *
 * The caller's buffer is validated before the child is run and not afterwards.
 * Running the child is what produces the status, and a status produced and then
 * found to have nowhere to go would be a child collected and its outcome
 * discarded — the one loss in this call that nothing could recover from.
 */
static int64_t SyscallDoWait(uint64_t status_address)
{
    Process *const parent = ProcessCurrent();
    int64_t status = 0;
    uint64_t collected;

    if (parent == NULL)
    {
        return SYSCALL_EINVAL;
    }

    if ((status_address != 0U) &&
        !SyscallUserRangeIsWritable(status_address, (uint64_t)sizeof(int64_t)))
    {
        ++SyscallFaults;

        return SYSCALL_EFAULT;
    }

    collected = ProcessWait(parent, &status);

    if (collected == 0U)
    {
        return SYSCALL_ECHILD;
    }

    if (status_address != 0U)
    {
        uint8_t *const destination = (uint8_t *)(uintptr_t)status_address;

        /* Byte by byte, in the order the architecture stores an integer, so that
         * nothing here depends upon the caller's buffer being aligned for a
         * quadword. A caller may name any address it owns. */
        for (uint64_t index = 0U; index < (uint64_t)sizeof(int64_t); ++index)
        {
            destination[index] = (uint8_t)(((uint64_t)status >> (index * 8U)) & 0xFFU);
        }
    }

    return (int64_t)collected;
}

/* ------------------------------------------------- the call of sub-task 7.3 */

/*
 * Moves the caller's break, or reports where it stands.
 *
 * No user address is dereferenced here, so there is nothing to validate: the
 * argument is an address the kernel is asked to *make* valid, not one it is
 * asked to read. What bounds it is ProcessSetBreak, which refuses anything below
 * where the heap begins or beyond PROCESS_BREAK_MAXIMUM above it — so a caller
 * cannot name a kernel address, the maximum being far below SYSCALL_USER_LIMIT
 * and the start being an address the kernel itself chose.
 *
 * SYSCALL_BREAK_QUERY reports rather than moves. It is the first call a heap
 * makes, there being no other way for a program to learn where its own heap
 * begins: the address is derived from the image the loader placed, and a program
 * has no view of its own program headers.
 */
static int64_t SyscallDoBrk(uint64_t requested)
{
    Process *const process = ProcessCurrent();

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    if (requested == SYSCALL_BREAK_QUERY)
    {
        const uint64_t established = ProcessBreak(process);

        /*
         * A process with no heap at all reports a refusal rather than zero. Zero
         * is not an address, so a program told it could produce no heap in any
         * arrangement it can distinguish from one that has yet to grow.
         */
        return (established == 0U) ? SYSCALL_ENOMEM : (int64_t)established;
    }

    return ProcessSetBreak(process, requested);
}

/* ------------------------------------------ the six calls of sub-task 7.6 */

/*
 * The filesystem layer's refusal, as the result a program receives.
 *
 * The switch has no `default`, deliberately. `VfsError` is an enumeration and
 * this function must answer for every one of its values; a default would answer
 * for the ones nobody had thought about, and would keep on answering as the
 * enumeration grew. Without one the compiler reports an unhandled value, which
 * is the only notice of that omission that cannot be missed —
 * PROJECT_GUIDELINES.md, Section 4, requires -Wall -Wextra -Werror, under which
 * the report is a failure to build.
 *
 * VFS_ERROR_NONE becomes EINVAL rather than success: this is called only upon a
 * path that has already failed, so a layer reporting no error at all is a layer
 * that failed without saying why, and reporting success for it would turn a
 * failed call into one that appeared to have worked.
 */
static int64_t SyscallFromVfsError(VfsError error)
{
    switch (error)
    {
    case VFS_ERROR_NONE:
        return SYSCALL_EINVAL;
    case VFS_ERROR_NOT_FOUND:
        return SYSCALL_ENOENT;
    case VFS_ERROR_EXISTS:
        return SYSCALL_EEXIST;
    case VFS_ERROR_NOT_DIRECTORY:
        return SYSCALL_ENOTDIR;
    case VFS_ERROR_IS_DIRECTORY:
        return SYSCALL_EISDIR;
    case VFS_ERROR_NOT_EMPTY:
        return SYSCALL_ENOTEMPTY;
    case VFS_ERROR_READ_ONLY:
        return SYSCALL_EROFS;
    case VFS_ERROR_INVALID:
        return SYSCALL_EINVAL;
    case VFS_ERROR_TOO_LONG:
        return SYSCALL_ENAMETOOLONG;
    case VFS_ERROR_TOO_MANY_LINKS:
        return SYSCALL_ELOOP;
    case VFS_ERROR_NO_SPACE:
        return SYSCALL_ENOSPC;
    case VFS_ERROR_NO_RESOURCE:
        return SYSCALL_EMFILE;
    case VFS_ERROR_BUSY:
        return SYSCALL_EBUSY;
    case VFS_ERROR_CROSSES_MOUNT:
        return SYSCALL_EXDEV;
    case VFS_ERROR_UNSUPPORTED:
        return SYSCALL_ENOTSUP;
    case VFS_ERROR_MEDIUM:
        return SYSCALL_EIO;
    }

    /*
     * Unreachable while the switch above covers the enumeration, and present
     * because a value outside it may still arrive: an enumerated type may hold
     * any value of its underlying type, and a caller passing one that names no
     * member would otherwise fall off the end of a function that returns a
     * value, which ISO/IEC 9899:2011, Section 6.9.1, paragraph 12, makes
     * undefined the moment the result is used.
     */
    return SYSCALL_EINVAL;
}

/* The whole of the failure path of the six calls below: the layer's reason,
 * translated, with the mount having been checked first. A program that calls
 * before a root is mounted is told there is no such file, which is true of every
 * path it can name. */
static int64_t SyscallFilesystemRefusal(void)
{
    return SyscallFromVfsError(VfsLastErrorCode());
}

/*
 * The agreement between what a program may see of a directory entry and what
 * the filesystem layer holds, checked where both headers are visible.
 *
 * <oxys/syscall_abi.h> may not include the kernel's, so it restates the bound
 * and the eight kinds. These are the only lines in the system where the two
 * statements can be compared, and a divergence in either would otherwise appear
 * as a program reading a truncated name or calling a regular file a directory.
 */
_Static_assert(SYSCALL_NAME_MAXIMUM == VFS_NAME_MAXIMUM,
               "A program's bound upon a name is not the filesystem layer's.");
_Static_assert((int)SYSCALL_TYPE_UNKNOWN == (int)VFS_NODE_UNKNOWN,
               "SYSCALL_TYPE_UNKNOWN does not name VFS_NODE_UNKNOWN.");
_Static_assert((int)SYSCALL_TYPE_REGULAR == (int)VFS_NODE_REGULAR,
               "SYSCALL_TYPE_REGULAR does not name VFS_NODE_REGULAR.");
_Static_assert((int)SYSCALL_TYPE_DIRECTORY == (int)VFS_NODE_DIRECTORY,
               "SYSCALL_TYPE_DIRECTORY does not name VFS_NODE_DIRECTORY.");
_Static_assert((int)SYSCALL_TYPE_SYMBOLIC_LINK == (int)VFS_NODE_SYMBOLIC_LINK,
               "SYSCALL_TYPE_SYMBOLIC_LINK does not name VFS_NODE_SYMBOLIC_LINK.");
_Static_assert((int)SYSCALL_TYPE_CHARACTER_DEVICE == (int)VFS_NODE_CHARACTER_DEVICE,
               "SYSCALL_TYPE_CHARACTER_DEVICE does not name VFS_NODE_CHARACTER_DEVICE.");
_Static_assert((int)SYSCALL_TYPE_BLOCK_DEVICE == (int)VFS_NODE_BLOCK_DEVICE,
               "SYSCALL_TYPE_BLOCK_DEVICE does not name VFS_NODE_BLOCK_DEVICE.");
_Static_assert((int)SYSCALL_TYPE_FIFO == (int)VFS_NODE_FIFO,
               "SYSCALL_TYPE_FIFO does not name VFS_NODE_FIFO.");
_Static_assert((int)SYSCALL_TYPE_SOCKET == (int)VFS_NODE_SOCKET,
               "SYSCALL_TYPE_SOCKET does not name VFS_NODE_SOCKET.");
_Static_assert((int)SYSCALL_OPEN_READ == (int)VFS_OPEN_READ,
               "SYSCALL_OPEN_READ is not the layer's VFS_OPEN_READ.");
_Static_assert((int)SYSCALL_OPEN_DIRECTORY == (int)VFS_OPEN_DIRECTORY,
               "SYSCALL_OPEN_DIRECTORY is not the layer's VFS_OPEN_DIRECTORY.");

/*
 * Opens a file for reading and returns a descriptor of the calling process.
 *
 * The flags are checked against the pair the interface offers and any other bit
 * is refused, rather than masked away. A program that asked to create a file and
 * silently received one opened for reading would find out at its first write, in
 * a call reporting something about a descriptor and nothing about what it asked
 * for.
 *
 * The descriptor is adopted into the process's table *after* the file is opened,
 * and the file is closed again where the table is full — because the table being
 * full is the one failure that can happen with an open file already in hand, and
 * an open file nobody holds a number for is a descriptor of the machine's that
 * nothing can ever close.
 */
static int64_t SyscallDoOpen(uint64_t path_address, uint64_t flags)
{
    Process *const process = ProcessCurrent();
    char path[SYSCALL_PATH_MAXIMUM + 1U];
    int file;
    int64_t descriptor;
    int64_t copied;

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    if ((flags & ~(SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY)) != 0U)
    {
        return SYSCALL_EINVAL;
    }

    if ((flags & SYSCALL_OPEN_READ) == 0U)
    {
        return SYSCALL_EINVAL;
    }

    copied = SyscallCopyUserPath(path_address, path, sizeof path);

    if (copied != SYSCALL_OK)
    {
        return copied;
    }

    file = VfsOpen(path, (uint32_t)flags, 0U);

    if (file == VFS_NO_DESCRIPTOR)
    {
        return SyscallFilesystemRefusal();
    }

    descriptor = ProcessAdoptDescriptor(process, file);

    if (descriptor < 0)
    {
        (void)VfsClose(file);

        return descriptor;
    }

    return descriptor;
}

/* Releases a descriptor and the open file beneath it. A number naming nothing is
 * EBADF and not success: a program that closed the same descriptor twice has
 * lost track of what it holds, and the second close would otherwise appear to
 * have worked upon a number that may since have been given to another file. */
static int64_t SyscallDoClose(uint64_t descriptor)
{
    Process *const process = ProcessCurrent();

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    if (!ProcessReleaseDescriptor(process, (int64_t)descriptor))
    {
        return SYSCALL_EBADF;
    }

    return SYSCALL_OK;
}

/*
 * Reads bytes from a descriptor into a caller's buffer.
 *
 * The bytes are read into the kernel's own buffer and copied out afterwards,
 * which is the reverse of what `write` does and is done for the same reason: the
 * filesystem layer must not be handed an address in a caller's memory, because
 * nothing below this function validates one and a path that faulted there would
 * fault with a volume's locks held.
 *
 * A read of zero bytes is refused as EINVAL rather than answered with zero,
 * because zero is what this call returns at the end of a file and a program
 * cannot tell the two apart.
 *
 * **Descriptor 0 is the terminal, since sub-task 8.1**, and a read of it does
 * not reach the filesystem layer at all. It waits until at least one byte has
 * been typed and then delivers what has been — up to the length asked for and
 * never nothing, so that a program can tell the terminal from a file at its end
 * by the one property that distinguishes them: a file that has run out stays
 * run out, and a terminal has more the moment somebody types. Nothing here
 * echoes and nothing assembles a line; docs/design/SHELL.md, Section 2, says
 * why the program does both.
 *
 * The caller's buffer is judged before the wait and not after it. The wait may
 * be long — it is however long a person takes to press a key — and a buffer
 * that was writable when the call was made is the buffer the program asked to
 * receive into; judging it afterwards would change nothing about what is
 * written and would make the refusal arrive after the keystroke was consumed.
 */
static int64_t SyscallDoRead(uint64_t descriptor, uint64_t address, uint64_t length)
{
    Process *const process = ProcessCurrent();
    char buffer[SYSCALL_TRANSFER_MAXIMUM];
    char *const destination = (char *)(uintptr_t)address;
    int file;
    uint64_t read = 0U;

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    if (length == 0U)
    {
        return SYSCALL_EINVAL;
    }

    if (length > SYSCALL_TRANSFER_MAXIMUM)
    {
        length = SYSCALL_TRANSFER_MAXIMUM;
    }

    if (descriptor == SYSCALL_DESCRIPTOR_INPUT)
    {
        if (!SyscallUserRangeIsWritable(address, length))
        {
            ++SyscallFaults;

            return SYSCALL_EFAULT;
        }

        TerminalWaitForInput();
        read = (uint64_t)TerminalRead(buffer, (size_t)length);

        for (uint64_t index = 0U; index < read; ++index)
        {
            destination[index] = buffer[index];
        }

        return (int64_t)read;
    }

    file = ProcessDescriptorFile(process, (int64_t)descriptor);

    if (file == VFS_NO_DESCRIPTOR)
    {
        return SYSCALL_EBADF;
    }

    if (length > SYSCALL_TRANSFER_MAXIMUM)
    {
        length = SYSCALL_TRANSFER_MAXIMUM;
    }

    /* The caller's buffer is judged before the file is read and not afterwards.
     * A read that succeeded and then found nowhere to put its bytes would have
     * advanced the file's position over data the program never received. */
    if (!SyscallUserRangeIsWritable(address, length))
    {
        ++SyscallFaults;

        return SYSCALL_EFAULT;
    }

    if (!VfsRead(file, buffer, length, &read))
    {
        return SyscallFilesystemRefusal();
    }

    for (uint64_t index = 0U; index < read; ++index)
    {
        destination[index] = buffer[index];
    }

    return (int64_t)read;
}

/*
 * Reads one entry of a directory into a caller's buffer.
 *
 * Returns 1 where an entry was placed, and 0 at the end of the directory. It is
 * not the count of bytes: an entry is a structure of a fixed size and a caller
 * that received its length would learn nothing it did not already know, whereas
 * the end of a directory is the one thing it cannot otherwise discover.
 *
 * The entry is composed in the kernel's own storage and copied out whole, so a
 * caller never sees a structure half filled — which is what it would see if the
 * layer wrote into its memory and then failed.
 */
static int64_t SyscallDoReadDirectory(uint64_t descriptor, uint64_t address)
{
    Process *const process = ProcessCurrent();
    VfsDirectoryEntry source;
    SyscallDirectoryEntry entry;
    uint8_t *const destination = (uint8_t *)(uintptr_t)address;
    const uint8_t *const bytes = (const uint8_t *)&entry;
    int file;
    bool end = false;

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    file = ProcessDescriptorFile(process, (int64_t)descriptor);

    if (file == VFS_NO_DESCRIPTOR)
    {
        return SYSCALL_EBADF;
    }

    if (!SyscallUserRangeIsWritable(address, sizeof entry))
    {
        ++SyscallFaults;

        return SYSCALL_EFAULT;
    }

    if (!VfsReadDirectory(file, &source, &end))
    {
        return SyscallFilesystemRefusal();
    }

    if (end)
    {
        return 0;
    }

    entry.number = source.number;
    entry.type = (uint32_t)source.type;
    entry.reserved = 0U;

    for (size_t index = 0U; index < sizeof entry.name; ++index)
    {
        entry.name[index] = source.name[index];
    }

    /* The name is terminated whatever the layer returned. The structure crosses
     * a privilege boundary, and a name without a terminator is a program reading
     * past the end of its own buffer on the kernel's authority. */
    entry.name[sizeof entry.name - 1U] = '\0';

    for (size_t index = 0U; index < sizeof entry; ++index)
    {
        destination[index] = bytes[index];
    }

    return 1;
}

/*
 * Creates a directory.
 *
 * The permissions are the caller's, masked to the twelve bits a mode holds. No
 * file mode creation mask is applied, this system having none: a program that
 * wants 0755 asks for 0755. IEEE Std 1003.1-2017 has `mkdir` reduce the mode by
 * the process's mask, and the mask belongs with the credentials this kernel does
 * not yet have — docs/design/PROCESS.md, Section 14.
 */
static int64_t SyscallDoMakeDirectory(uint64_t path_address, uint64_t permissions)
{
    char path[SYSCALL_PATH_MAXIMUM + 1U];
    int64_t copied;

    if (ProcessCurrent() == NULL)
    {
        return SYSCALL_EINVAL;
    }

    copied = SyscallCopyUserPath(path_address, path, sizeof path);

    if (copied != SYSCALL_OK)
    {
        return copied;
    }

    if (!VfsCreateDirectory(path, (uint16_t)(permissions & UINT64_C(0x0FFF))))
    {
        return SyscallFilesystemRefusal();
    }

    return SYSCALL_OK;
}

/* Removes a name. A directory is refused with EISDIR by the layer beneath, which
 * is what lets `rm` report what IEEE Std 1003.1-2017 requires it to report for an
 * operand naming a directory without -r. There is no call that removes a
 * directory; docs/design/LIBC.md, Section 12.7, limitation 3. */
static int64_t SyscallDoUnlink(uint64_t path_address)
{
    char path[SYSCALL_PATH_MAXIMUM + 1U];
    int64_t copied;

    if (ProcessCurrent() == NULL)
    {
        return SYSCALL_EINVAL;
    }

    copied = SyscallCopyUserPath(path_address, path, sizeof path);

    if (copied != SYSCALL_OK)
    {
        return copied;
    }

    if (!VfsUnlink(path))
    {
        return SyscallFilesystemRefusal();
    }

    return SYSCALL_OK;
}

/*
 * Changes the caller's working directory, of sub-task 8.3.
 *
 * The path is copied and made absolute by the copier above, reduced to its
 * canonical form, and then established to name a directory before it is
 * stored: a working directory that named a file would make every relative
 * path fail with ENOTDIR at some later call, which is the wrong place for the
 * refusal. ENOENT and ENOTDIR are the filesystem layer's own, carried out as
 * every other refusal is.
 */
static int64_t SyscallDoChangeDirectory(uint64_t path_address)
{
    Process *const process = ProcessCurrent();
    char path[SYSCALL_PATH_MAXIMUM + 1U];
    VfsAttributes attributes;
    int64_t copied;

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    copied = SyscallCopyUserPath(path_address, path, sizeof path);

    if (copied != SYSCALL_OK)
    {
        return copied;
    }

    SyscallCanonicalisePath(path);

    if (!VfsStat(path, &attributes))
    {
        return SyscallFilesystemRefusal();
    }

    if (attributes.type != VFS_NODE_DIRECTORY)
    {
        return SYSCALL_ENOTDIR;
    }

    for (size_t index = 0U; index <= PROCESS_PATH_MAXIMUM; ++index)
    {
        process->working_directory[index] = path[index];

        if (path[index] == '\0')
        {
            break;
        }
    }

    return SYSCALL_OK;
}

/*
 * Copies the caller's working directory, terminated, into its buffer, and
 * returns the length copied excluding the terminator. A buffer too small is
 * ENAMETOOLONG, the interface header having no ERANGE; the C library reports
 * it as ERANGE, which is what IEEE Std 1003.1-2017 names.
 */
static int64_t SyscallDoGetWorkingDirectory(uint64_t address, uint64_t capacity)
{
    const Process *const process = ProcessCurrent();
    char *const destination = (char *)(uintptr_t)address;
    size_t length = 0U;

    if (process == NULL)
    {
        return SYSCALL_EINVAL;
    }

    while (process->working_directory[length] != '\0')
    {
        ++length;
    }

    if (capacity == 0U)
    {
        return SYSCALL_EINVAL;
    }

    if (capacity < length + 1U)
    {
        return SYSCALL_ENAMETOOLONG;
    }

    if (!SyscallUserRangeIsWritable(address, length + 1U))
    {
        ++SyscallFaults;

        return SYSCALL_EFAULT;
    }

    for (size_t index = 0U; index <= length; ++index)
    {
        destination[index] = process->working_directory[index];
    }

    return (int64_t)length;
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
    { "version", 2U },
    { "fork", 0U },
    { "execve", 3U },
    { "exit", 1U },
    { "wait", 1U },
    { "brk", 1U },
    { "open", 2U },
    { "close", 1U },
    { "read", 3U },
    { "readdir", 2U },
    { "mkdir", 2U },
    { "unlink", 1U },
    { "chdir", 1U },
    { "getcwd", 2U }
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

    case SYSCALL_FORK:
        frame->rax = (uint64_t)SyscallDoFork(frame);
        break;

    case SYSCALL_EXECVE:
        frame->rax = (uint64_t)SyscallDoExecve(frame->rdi, frame->rsi, frame->rdx);
        break;

    case SYSCALL_EXIT:
        frame->rax = (uint64_t)SyscallDoExit(frame->rdi);
        break;

    case SYSCALL_WAIT:
        frame->rax = (uint64_t)SyscallDoWait(frame->rdi);
        break;

    case SYSCALL_BRK:
        frame->rax = (uint64_t)SyscallDoBrk(frame->rdi);
        break;

    case SYSCALL_OPEN:
        frame->rax = (uint64_t)SyscallDoOpen(frame->rdi, frame->rsi);
        break;

    case SYSCALL_CLOSE:
        frame->rax = (uint64_t)SyscallDoClose(frame->rdi);
        break;

    case SYSCALL_READ:
        frame->rax = (uint64_t)SyscallDoRead(frame->rdi, frame->rsi, frame->rdx);
        break;

    case SYSCALL_READDIR:
        frame->rax = (uint64_t)SyscallDoReadDirectory(frame->rdi, frame->rsi);
        break;

    case SYSCALL_MKDIR:
        frame->rax = (uint64_t)SyscallDoMakeDirectory(frame->rdi, frame->rsi);
        break;

    case SYSCALL_UNLINK:
        frame->rax = (uint64_t)SyscallDoUnlink(frame->rdi);
        break;

    case SYSCALL_CHDIR:
        frame->rax = (uint64_t)SyscallDoChangeDirectory(frame->rdi);
        break;

    case SYSCALL_GETCWD:
        frame->rax = (uint64_t)SyscallDoGetWorkingDirectory(frame->rdi, frame->rsi);
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
