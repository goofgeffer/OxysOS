/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/verify_syscall.c
 * Purpose: Asserts the system-call dispatch and the validation of a caller's
 *          arguments, without executing SYSCALL.
 * Key functions: KernelVerifySyscall.
 * References:
 *   - docs/design/PRIVILEGE.md, Section 9: the entry path, the table and the
 *     validation; Section 9.5, these assertions paired with what each catches.
 *
 * Why nothing here executes SYSCALL.
 *
 *   The entry path returns by SYSRET, and SYSRET returns to privilege level 3
 *   unconditionally. A self-test that executed SYSCALL from the kernel would not
 *   return to the kernel: it would arrive in user mode with no user mapping to
 *   execute in. The alternative was a branch in the entry path that returned
 *   differently for a caller the kernel trusts, and that is a test hook in the
 *   one path where a test hook cannot be told from a privilege-escalation bug.
 *
 *   What that leaves is not small. The dispatcher is an ordinary function of an
 *   ordinary structure, so the table, the refusals and the whole of the argument
 *   validation are asserted here directly — and the validation is the part that
 *   decides whether a hostile caller can make the kernel read or write memory it
 *   chose. The transition itself is executed for the first time at sub-task
 *   6.10, where there is a user program to execute it.
 *
 * What "a user address" means in a kernel with no user program.
 *
 *   The validation asks two questions of an address: is it below the boundary,
 *   and is its page mapped and marked accessible to privilege level 3. The first
 *   is arithmetic and is asserted upon numbers. The second needs a page that is
 *   really mapped that way, so the test makes one — a frame, mapped at a low
 *   address with the user bit — and takes it away again afterwards. That page is
 *   the only user-accessible mapping this kernel has ever had, and asserting
 *   against it is asserting against the thing itself rather than against a
 *   description of it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/syscall.h>
#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/memory.h>

/*
 * Where the composed user page is mapped.
 *
 * Low, arbitrary, and page aligned. It must be somewhere no other mapping is,
 * and the low canonical half is empty in the kernel's own address space — which
 * is exactly why it is the half a user program will be given.
 */
#define KERNEL_SYSCALL_USER_PAGE UINT64_C(0x0000000040000000)

static bool KernelSyscallSucceeded;

static void KernelSyscallRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelSyscallSucceeded = false;
    }
}

/* A frame as the entry path would have built it, with the call number and the
 * six argument registers set and everything else zero. */
static SyscallFrame KernelSyscallFrame(uint64_t number, uint64_t first, uint64_t second,
                                       uint64_t third)
{
    SyscallFrame frame;
    uint8_t *const bytes = (uint8_t *)&frame;

    for (size_t index = 0U; index < sizeof frame; ++index)
    {
        bytes[index] = 0U;
    }

    frame.rax = number;
    frame.rdi = first;
    frame.rsi = second;
    frame.rdx = third;

    return frame;
}

/* Dispatches a composed frame and returns what the dispatcher put in RAX. */
static int64_t KernelSyscallInvoke(uint64_t number, uint64_t first, uint64_t second,
                                   uint64_t third)
{
    SyscallFrame frame = KernelSyscallFrame(number, first, second, third);

    SyscallDispatch(&frame);

    return (int64_t)frame.rax;
}

void KernelVerifySyscall(void)
{
    PhysicalAddress frame_address;
    bool mapped = false;

    KernelSyscallSucceeded = true;

    KernelWriteString("System call: asserting the dispatch and the validation.\n");

    /* --- The table, and what lies beyond it. --- */

    KernelSyscallRequire(SyscallNumberIsValid(0U) &&
                             SyscallNumberIsValid(SYSCALL_COUNT - 1U),
                         "a call within the table was called invalid");

    /*
     * The number indexes the table, so a bound wrong by one calls whatever
     * function follows the array. It is unsigned, so a caller passing a negative
     * number passes an enormous one and meets the same comparison — which is why
     * there is only one comparison.
     */
    KernelSyscallRequire(!SyscallNumberIsValid(SYSCALL_COUNT),
                         "the number one beyond the table was accepted");
    KernelSyscallRequire(!SyscallNumberIsValid(UINT64_MAX),
                         "the greatest number was accepted, so a negative one would be");

    KernelSyscallRequire(SyscallName(SYSCALL_WRITE) != NULL,
                         "a call within the table has no name");
    KernelSyscallRequire(SyscallName(SYSCALL_COUNT) == NULL,
                         "a call beyond the table has a name");

    KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_COUNT, 0U, 0U, 0U) == SYSCALL_ENOSYS,
                         "a call beyond the table was not refused");
    KernelSyscallRequire(KernelSyscallInvoke(UINT64_MAX, 0U, 0U, 0U) == SYSCALL_ENOSYS,
                         "a call of the greatest number was not refused");

    /* --- The arithmetic of a range, upon numbers alone. --- */

    /*
     * A range that wraps past the end of the address space.
     *
     * The address must be **below** the limit for this to assert anything: an
     * address above it is refused by the bounds test before the arithmetic is
     * reached, so a wrapping range built from one exercises nothing. That was
     * the first form of this assertion and it caught the damage it names not at
     * all. Eight bytes below the limit with a length of everything is a range
     * whose sum is smaller than its address, which is what makes it look like a
     * range of nothing rather than a range of the whole machine.
     */
    KernelSyscallRequire(
        !SyscallUserRangeIsReadable(SYSCALL_USER_LIMIT - 8U, UINT64_MAX),
        "a range wrapping past the end of the address space was accepted");
    KernelSyscallRequire(
        !SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE, UINT64_MAX - 16U),
        "a length that wraps from a legitimate address was accepted");

    /* An address in the kernel's own half, whatever its mapping says. */
    KernelSyscallRequire(!SyscallUserRangeIsReadable(DIRECT_MAP_BASE, 8U),
                         "an address in the kernel's half was accepted");
    KernelSyscallRequire(!SyscallUserRangeIsReadable(SYSCALL_USER_LIMIT, 8U),
                         "the first address above the user limit was accepted");

    /*
     * A range that begins below the limit and ends above it. This is the case a
     * check of the starting address alone admits, and it is how a caller reaches
     * the kernel's memory using an address that is itself legitimate.
     */
    KernelSyscallRequire(!SyscallUserRangeIsReadable(SYSCALL_USER_LIMIT - 4U, 64U),
                         "a range beginning below the limit and ending above it was "
                         "accepted");

    KernelSyscallRequire(!SyscallUserRangeIsReadable(0U, 8U),
                         "the null address was accepted");
    KernelSyscallRequire(!SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE, 0U),
                         "a range of no length was accepted");

    /* An address below the limit that is simply not mapped. */
    KernelSyscallRequire(!SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE, 8U),
                         "an unmapped user address was accepted before it was mapped");

    /* --- A page that really is accessible to privilege level 3. --- */

    frame_address = FrameAllocate();

    if (frame_address != 0U)
    {
        PagingMapKernelPage(KERNEL_SYSCALL_USER_PAGE, frame_address,
                            PAGE_ENTRY_WRITABLE | PAGE_ENTRY_USER);
        mapped = true;
    }

    if (!mapped)
    {
        KernelWriteString("  No frame was available; the mapped half of the validation "
                          "was not asserted.\n");
    }
    else
    {
        KernelSyscallRequire(SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE, 8U),
                             "a mapped, user-accessible page was refused");
        KernelSyscallRequire(SyscallUserRangeIsWritable(KERNEL_SYSCALL_USER_PAGE, 8U),
                             "a writable user page was refused for writing");

        /*
         * A range that begins upon the mapped page and ends upon the one after
         * it, which is not mapped. A kernel that checked the first byte alone
         * would begin a copy it had promised to finish and fault in the middle
         * of it — with the caller's bytes half written and the kernel in a
         * handler it has no way to return from.
         */
        KernelSyscallRequire(
            !SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE + PAGE_SIZE - 4U, 64U),
            "a range straddling a mapped page and an unmapped one was accepted");

        /* The last byte of the page is within it, and the first byte beyond is
         * not: the boundary itself, from both sides. */
        KernelSyscallRequire(
            SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE + PAGE_SIZE - 1U, 1U),
            "the last byte of a mapped page was refused");
        KernelSyscallRequire(
            !SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE + PAGE_SIZE, 1U),
            "the first byte beyond a mapped page was accepted");

        /* --- The calls themselves, against that page. --- */

        {
            char *const page = (char *)(uintptr_t)KERNEL_SYSCALL_USER_PAGE;

            /*
             * A write of bytes the caller may read. The result is the length
             * transferred, and nothing is emitted that would disturb the log: a
             * single space.
             */
            page[0] = ' ';
            KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_WRITE, 1U,
                                                     KERNEL_SYSCALL_USER_PAGE, 1U) == 1,
                                 "a write of one legitimate byte did not report one");

            /* A descriptor that names nothing is refused before the buffer is
             * even looked at. */
            KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_WRITE, 7U,
                                                     KERNEL_SYSCALL_USER_PAGE, 1U) ==
                                     SYSCALL_EBADF,
                                 "a write to a descriptor that does not exist was "
                                 "attempted");

            /*
             * A write from the kernel's own memory. This is the assertion the
             * whole validation exists for: a caller that named a kernel address
             * would otherwise have the kernel print its own memory back to it.
             */
            KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_WRITE, 1U, DIRECT_MAP_BASE,
                                                     16U) == SYSCALL_EFAULT,
                                 "a write from the kernel's own memory was performed");

            /* The version, into a buffer the caller may write. */
            page[0] = '\0';
            KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_VERSION,
                                                     KERNEL_SYSCALL_USER_PAGE, 64U, 0U) > 0,
                                 "the version was not copied into a writable user "
                                 "buffer");
            KernelSyscallRequire(page[0] == 'O',
                                 "the version copied does not begin with the system's "
                                 "name");

            /* And into one it may not. */
            KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_VERSION, DIRECT_MAP_BASE, 64U,
                                                     0U) == SYSCALL_EFAULT,
                                 "the version was written into the kernel's own memory");
            KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_VERSION,
                                                     KERNEL_SYSCALL_USER_PAGE, 0U, 0U) ==
                                     SYSCALL_EINVAL,
                                 "a version buffer of no length was accepted");
        }

        PagingUnmapKernelPage(KERNEL_SYSCALL_USER_PAGE);
        FrameFree(frame_address);

        /*
         * And it is gone. The mapping was the only user-accessible page this
         * kernel has ever had; leaving it standing would leave the kernel with a
         * page privilege level 3 could reach, established by a self-test, for
         * the rest of the boot.
         */
        KernelSyscallRequire(!SyscallUserRangeIsReadable(KERNEL_SYSCALL_USER_PAGE, 8U),
                             "the page composed by this test was left mapped");
    }

    /* --- A call that needs no argument at all. --- */

    KernelSyscallRequire(KernelSyscallInvoke(SYSCALL_TICKS, 0U, 0U, 0U) >= 0,
                         "the tick count was reported as a failure");

    KernelWriteString(KernelSyscallSucceeded
                          ? "System call self-test passed.\n"
                          : "System call self-test FAILED.\n");
}
