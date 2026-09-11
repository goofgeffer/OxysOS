/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/verify_wrappers.c
 * Purpose: Asserts the work of sub-task 7.2 — the C library's system-call
 *          wrappers — in the two halves it divides into: the translation of a
 *          kernel result into an errno, which runs here; and the invocation
 *          itself, which cannot run here and is therefore copied into a program
 *          and run at privilege level 3.
 * Key functions: KernelVerifyWrappers.
 * References:
 *   - docs/design/LIBC.md, Section 8.4: the table pairing every property
 *     asserted below with the silent failure that assertion exists to catch.
 *   - ISO/IEC 9899:2011, Section 7.5, paragraph 3: errno is never set to zero by
 *     a library function, which is asserted below because it is the property a
 *     natural implementation loses first.
 *   - ISO/IEC 9899:2011, Section 7.24.6.2: strerror maps any value of type int
 *     to a message.
 *   - kernel/abi/oxys/syscall_abi.h: the numbers and the failure results both
 *     halves are written against.
 *
 * Why half of this test is a program and not a call.
 *
 *   SYSCALL cannot be executed by this kernel. The instruction works at any
 *   privilege level, but the SYSRET that ends the kernel's handling of it
 *   returns to privilege level 3 unconditionally — so a kernel that called a
 *   wrapper would enter its own entry path and leave it as a user program, upon
 *   a stack and in an address space that are not a user program's. There is no
 *   arrangement in which the kernel calls OxysWrite and survives.
 *
 *   So the wrappers are asserted where they run. libc/syscall/invoke.asm holds
 *   no memory reference and no relocation, which makes its bytes mean the same
 *   thing wherever they are placed; this file copies them out of the kernel
 *   image into a program composed for the purpose, assembles a driver around
 *   them by hand, and runs the result at privilege level 3. What is asserted is
 *   therefore the code this library actually ships, and not a reconstruction of
 *   it or a description of what it ought to do.
 *
 * What the program reports, and why it reports it that way.
 *
 *   A program at privilege level 3 has two channels to this test: what it
 *   writes, and the status it ends with. Only the status can be asserted
 *   mechanically, so the program adds up what every one of its calls returned
 *   and ends with that sum — and the sum is composed so that no single result
 *   can be lost without changing it.
 *
 *   The first version of this test ended with one result instead of the sum, and
 *   the negative test found it worthless. libc/syscall/invoke.asm was altered to
 *   lose the third argument of a three-argument call, which is the defect this
 *   whole arrangement exists to catch, and **the test still passed**: the
 *   corrupted length reached a call whose result the status did not depend upon,
 *   and the only trace was a missing newline in the log that nothing was
 *   asserting. docs/design/LIBC.md, Section 8.7, records it. The sum below is
 *   what that failure bought.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/process.h>
#include <oxys/elf.h>
#include <oxys/memory.h>
#include <oxys/pit.h>
#include <oxys/syscall.h>

#include <errno.h>
#include <string.h>
#include <syscall.h>

static bool VerifyWrappersSucceeded;

static void VerifyWrappersRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyWrappersSucceeded = false;
    }
}

/* -------------------------------------------------------------------------
 * 1. The translation, which is the half that can be called.
 * ------------------------------------------------------------------------- */

/*
 * Asserts one failure result: that it produces -1, that it names the errno it
 * should, and that it does so from whatever errno held before.
 *
 * The previous value is set to something no failure result maps to, so that an
 * implementation which left errno alone and returned -1 — the commonest way to
 * write this wrong — is caught by the number rather than passing because errno
 * happened to hold the right thing already.
 */
static void VerifyWrappersFailure(int64_t result, int expected, const char *statement)
{
    errno = ERANGE;

    VerifyWrappersRequire(OxysSyscallResult(result) == -1, statement);
    VerifyWrappersRequire(errno == expected, statement);
}

static void VerifyWrappersTranslation(void)
{
    /*
     * A result that is not negative is returned exactly, and errno is not
     * touched.
     *
     * Both halves matter and the second is the one that is lost silently.
     * ISO/IEC 9899:2011, Section 7.5, paragraph 3, permits a library to set
     * errno upon a call that succeeded; this library promises not to, and a
     * program that cleared errno, called a wrapper that succeeded and found
     * errno set would be told a failure that did not happen.
     */
    errno = EDOM;
    VerifyWrappersRequire(OxysSyscallResult(0) == 0,
                          "a result of zero was not returned unchanged");
    VerifyWrappersRequire(errno == EDOM,
                          "a successful call altered errno");

    errno = EDOM;
    VerifyWrappersRequire(OxysSyscallResult(4096) == 4096,
                          "a length was not returned unchanged");
    VerifyWrappersRequire(errno == EDOM,
                          "a call that returned a length altered errno");

    /*
     * The largest value a call may return. A translation that tested the sign by
     * casting to a narrower type would pass every assertion above and fail here,
     * INT64_MAX being negative in thirty-two bits.
     */
    errno = EDOM;
    VerifyWrappersRequire(OxysSyscallResult(INT64_MAX) == INT64_MAX,
                          "the greatest result was not returned unchanged");
    VerifyWrappersRequire(errno == EDOM,
                          "the greatest result altered errno");

    /* Each failure result names itself and nothing else. Seven assertions and
     * not one, because a table or a derivation can be wrong for one entry. */
    VerifyWrappersFailure(SYSCALL_ENOSYS, ENOSYS, "ENOSYS was not translated");
    VerifyWrappersFailure(SYSCALL_EFAULT, EFAULT, "EFAULT was not translated");
    VerifyWrappersFailure(SYSCALL_EINVAL, EINVAL, "EINVAL was not translated");
    VerifyWrappersFailure(SYSCALL_EBADF, EBADF, "EBADF was not translated");
    VerifyWrappersFailure(SYSCALL_ECHILD, ECHILD, "ECHILD was not translated");
    VerifyWrappersFailure(SYSCALL_ENOENT, ENOENT, "ENOENT was not translated");
    VerifyWrappersFailure(SYSCALL_ENOMEM, ENOMEM, "ENOMEM was not translated");

    /*
     * A failure this library has no name for.
     *
     * Nothing this kernel returns is outside the reserved range — <errno.h>
     * asserts that at compile time — so this is the guard against a kernel that
     * has changed and a library that has not. A translation that simply negated
     * would put 32 into errno here, which is EDOM: a program would be told that
     * a system call had reported a mathematical domain error.
     */
    VerifyWrappersFailure(-(int64_t)OXYS_ERRNO_SYSCALL_LIMIT - 1, ENOSYS,
                          "a failure beyond the reserved range was not refused");

    /*
     * The one value that cannot be negated at all.
     *
     * Negating INT64_MIN is undefined behaviour, which PROJECT_GUIDELINES.md,
     * Section 8, forbids relying upon. A translation that negated before
     * checking the range would do it here, and the defect would not announce
     * itself: the commonest result upon this architecture is the value negated
     * back to itself, which is negative, which would then be cast to int and
     * stored in errno as zero — and errno set to zero is the one thing Section
     * 7.5 says a library function never does.
     */
    VerifyWrappersFailure(INT64_MIN, ENOSYS,
                          "the least representable result was not refused");

    /* And errno is never left zero by any of it. */
    VerifyWrappersRequire(errno != 0, "a translation left errno at zero");
}

/* -------------------------------------------------------------------------
 * 2. strerror, which arrives with the wrappers because the numbers do.
 * ------------------------------------------------------------------------- */

static void VerifyWrappersMessages(void)
{
    static const int numbers[] = { 0,      ENOSYS, EFAULT, EINVAL, EBADF,
                                   ECHILD, ENOENT, ENOMEM, EDOM,   EILSEQ,
                                   ERANGE };
    const size_t count = sizeof numbers / sizeof numbers[0];

    /*
     * Every number this system can produce has a message, and no two of them
     * are the same message.
     *
     * The second half is what catches the defect this function is prone to: a
     * table with an entry omitted shifts every message after it by one, so every
     * number still answers something and every one of them answers wrongly. A
     * check that each message is non-empty would pass. Distinctness would not:
     * a shifted table has a duplicate at the end, where the last entry is
     * repeated or the unknown message appears for a number that has one.
     */
    for (size_t index = 0U; index < count; ++index)
    {
        const char *const message = strerror(numbers[index]);

        VerifyWrappersRequire(message != NULL, "strerror returned nothing");

        if (message == NULL)
        {
            continue;
        }

        VerifyWrappersRequire(message[0] != '\0',
                              "strerror returned an empty message");

        for (size_t other = 0U; other < index; ++other)
        {
            VerifyWrappersRequire(strcmp(message, strerror(numbers[other])) != 0,
                                  "two error numbers share a message");
        }
    }

    /*
     * ISO/IEC 9899:2011, Section 7.24.6.2, paragraph 2: strerror shall map any
     * value of type int. These three are the boundaries a table-driven
     * implementation walks off — one below the first entry, one above the last,
     * and the extremes of the type — and each of them would be an index into
     * something that is not the table.
     */
    VerifyWrappersRequire(strerror(-1)[0] != '\0',
                          "a negative error number produced no message");
    VerifyWrappersRequire(strerror(OXYS_ERRNO_SYSCALL_LIMIT)[0] != '\0',
                          "an unassigned error number produced no message");
    VerifyWrappersRequire(strerror(INT32_MAX)[0] != '\0',
                          "the greatest error number produced no message");
    VerifyWrappersRequire(strerror(INT32_MIN)[0] != '\0',
                          "the least error number produced no message");

    /* And the message a program actually reaches: the one for what the last
     * failure set. A library whose strerror and whose errno disagreed would pass
     * every assertion above. */
    errno = EDOM;
    (void)OxysSyscallResult(SYSCALL_EBADF);
    VerifyWrappersRequire(strcmp(strerror(errno), strerror(EBADF)) == 0,
                          "strerror of a failed call did not describe that failure");
}

/* -------------------------------------------------------------------------
 * 3. The invocation, which must be run by a program.
 * ------------------------------------------------------------------------- */

/*
 * The boundaries of the block this test copies, and the offset of each entry
 * point within it.
 *
 * They are the aliases libc/syscall/invoke.asm declares beside each routine, and
 * not the routines' own names. A name declared to this file as an array of bytes
 * and to libc/syscall/calls.c as a function would be two incompatible
 * declarations of one object across two translation units; the aliases mean this
 * file never declares a function as anything else.
 */
extern const uint8_t OxysSyscallInvokeBegin[];
extern const uint8_t OxysSyscallInvokeEnd[];
extern const uint8_t OxysSyscallInvokeBytes0[];
extern const uint8_t OxysSyscallInvokeBytes1[];
extern const uint8_t OxysSyscallInvokeBytes2[];
extern const uint8_t OxysSyscallInvokeBytes3[];

/*
 * The program's address space, and where each thing stands in it.
 *
 * The text page holds two things that must not overlap: the driver this file
 * assembles, which begins at the entry point, and the library's invocation
 * block, which is copied to a fixed displacement within the same page so that
 * the driver's call displacements can be computed before either is written.
 * VERIFY_WRAPPERS_INVOKE_AT is that displacement and is asserted to be beyond
 * the driver's last byte, because a driver that grew into the block would
 * overwrite the very code this test exists to run.
 */
#define VERIFY_WRAPPERS_TEXT_ADDRESS UINT64_C(0x0000000000401000)
#define VERIFY_WRAPPERS_DATA_ADDRESS UINT64_C(0x0000000000402000)
#define VERIFY_WRAPPERS_TEXT_OFFSET  0x1000U
#define VERIFY_WRAPPERS_DATA_OFFSET  0x2000U
#define VERIFY_WRAPPERS_IMAGE_BYTES  0x3000U
#define VERIFY_WRAPPERS_TEXT_BYTES   0x0200U
#define VERIFY_WRAPPERS_DATA_BYTES   0x0100U
#define VERIFY_WRAPPERS_INVOKE_AT    0x0100U

/* Within the data page: the buffer the version is copied into, and the single
 * newline byte the program writes after it so that the log's next line begins
 * where a reader expects. */
#define VERIFY_WRAPPERS_BUFFER_AT   0x00U
#define VERIFY_WRAPPERS_BUFFER_SIZE 64U
#define VERIFY_WRAPPERS_NEWLINE_AT  0x40U

/*
 * The capacity the program asks the version call for a second time, and it is
 * deliberately too small.
 *
 * A capacity larger than the string is not a capacity the result depends upon:
 * the kernel copies the whole string and returns its length whatever room was
 * offered, so a call that lost its second argument entirely would return exactly
 * the same number — which is how the negative test of a two-argument invocation
 * first passed against a defective one. A capacity of eight is smaller than the
 * string, so the result is the capacity less one and could have come from
 * nowhere else. docs/design/LIBC.md, Section 8.7.
 */
#define VERIFY_WRAPPERS_SHORT_CAPACITY 8U

/* The descriptor the program offers deliberately, which is neither of the two
 * the kernel accepts, so that the failure path is exercised by a call that must
 * fail rather than by one that might. */
#define VERIFY_WRAPPERS_BAD_DESCRIPTOR 99U

/*
 * The scale that holds the program's two numbers apart within one status.
 *
 * A million ticks is a thousand seconds of running, which no boot of this kernel
 * reaches; the assertion below checks that the count observed is well beneath it
 * rather than assuming so, because the day it is not the two numbers would
 * silently run into one another.
 */
#define VERIFY_WRAPPERS_STATUS_SCALE 1000000

static uint8_t VerifyWrappersImage[VERIFY_WRAPPERS_IMAGE_BYTES];
static uint64_t VerifyWrappersDriverEnd;

static void VerifyWrappersPut16(uint64_t at, uint16_t value)
{
    VerifyWrappersImage[at] = (uint8_t)(value & 0xFFU);
    VerifyWrappersImage[at + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

static void VerifyWrappersPut32(uint64_t at, uint32_t value)
{
    for (uint64_t index = 0U; index < 4U; ++index)
    {
        VerifyWrappersImage[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void VerifyWrappersPut64(uint64_t at, uint64_t value)
{
    for (uint64_t index = 0U; index < 8U; ++index)
    {
        VerifyWrappersImage[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void VerifyWrappersProgramHeader(uint64_t at, uint32_t flags, uint64_t file_offset,
                                        uint64_t address, uint64_t size)
{
    VerifyWrappersPut32(at + 0U, ELF_SEGMENT_LOAD);
    VerifyWrappersPut32(at + 4U, flags);
    VerifyWrappersPut64(at + 8U, file_offset);
    VerifyWrappersPut64(at + 16U, address);
    VerifyWrappersPut64(at + 24U, address);
    VerifyWrappersPut64(at + 32U, size);
    VerifyWrappersPut64(at + 40U, size);
    VerifyWrappersPut64(at + 48U, PAGE_SIZE);
}

/* ------------------------------------------------- the driver, by hand */

/*
 * The five instruction forms the driver needs, each emitted at a cursor the
 * caller advances.
 *
 * They are written out rather than being a table of opcodes because a table
 * would have to be read against the manual anyway, and a reader checking this
 * against Intel's Volume 2 wants the mnemonic beside the byte.
 */

/* mov r32, imm32 — B8+rd. The 32-bit form zero-extends to the full register,
 * which is why a call number and a descriptor need no REX prefix. */
static uint64_t VerifyWrappersMoveImmediate32(uint64_t at, uint8_t reg, uint32_t value)
{
    VerifyWrappersImage[at] = (uint8_t)(0xB8U + reg);
    VerifyWrappersPut32(at + 1U, value);

    return at + 5U;
}

/* mov r64, imm64 — REX.W + B8+rd, which is the only form that can name an
 * address in a program's data page. */
static uint64_t VerifyWrappersMoveImmediate64(uint64_t at, uint8_t reg, uint64_t value)
{
    VerifyWrappersImage[at] = 0x48U;
    VerifyWrappersImage[at + 1U] = (uint8_t)(0xB8U + reg);
    VerifyWrappersPut64(at + 2U, value);

    return at + 10U;
}

/* mov r/m64, r64 — REX.W + 89 /r, with both operands registers. */
static uint64_t VerifyWrappersMoveRegister(uint64_t at, uint8_t destination,
                                           uint8_t source)
{
    VerifyWrappersImage[at] = 0x48U;
    VerifyWrappersImage[at + 1U] = 0x89U;
    VerifyWrappersImage[at + 2U] = (uint8_t)(0xC0U | (source << 3) | destination);

    return at + 3U;
}

/*
 * call rel32 — E8 cd, the displacement being relative to the address of the
 * instruction after the call. Both addresses are within the one page, so the
 * displacement is small and the arithmetic is done in the image's own offsets.
 */
static uint64_t VerifyWrappersCall(uint64_t at, uint64_t target_offset)
{
    const int64_t displacement = (int64_t)target_offset - (int64_t)(at + 5U);

    VerifyWrappersImage[at] = 0xE8U;
    VerifyWrappersPut32(at + 1U, (uint32_t)(int32_t)displacement);

    return at + 5U;
}

/* add r/m64, r64 — REX.W + 01 /r. */
static uint64_t VerifyWrappersAddRegister(uint64_t at, uint8_t destination,
                                          uint8_t source)
{
    VerifyWrappersImage[at] = 0x48U;
    VerifyWrappersImage[at + 1U] = 0x01U;
    VerifyWrappersImage[at + 2U] = (uint8_t)(0xC0U | (source << 3) | destination);

    return at + 3U;
}

/* imul r64, r/m64, imm32 — REX.W + 69 /r id, the three-operand form, which is
 * what scales the accumulator without a second register to hold the multiplier
 * in. */
static uint64_t VerifyWrappersMultiplyImmediate(uint64_t at, uint8_t reg,
                                                uint32_t value)
{
    VerifyWrappersImage[at] = 0x48U;
    VerifyWrappersImage[at + 1U] = 0x69U;
    VerifyWrappersImage[at + 2U] = (uint8_t)(0xC0U | (reg << 3) | reg);
    VerifyWrappersPut32(at + 3U, value);

    return at + 7U;
}

/* The register numbers of the encoding, which are not the order a reader would
 * guess: RAX 0, RCX 1, RDX 2, RBX 3, RSP 4, RBP 5, RSI 6, RDI 7. */
#define VERIFY_WRAPPERS_RAX 0U
#define VERIFY_WRAPPERS_RCX 1U
#define VERIFY_WRAPPERS_RDX 2U
#define VERIFY_WRAPPERS_RBX 3U
#define VERIFY_WRAPPERS_RBP 5U
#define VERIFY_WRAPPERS_RSI 6U
#define VERIFY_WRAPPERS_RDI 7U

/*
 * Composes the whole image: the ELF header, the two program headers, the
 * library's invocation block copied in verbatim, and the driver that calls it.
 *
 * The driver, in the order it executes:
 *
 *   1. ticks()                      through OxysSyscallInvoke0. Kept in RBP.
 *   2. version(buffer, 64)          through OxysSyscallInvoke2, returning the
 *                                   length copied. Begins the sum in RBX.
 *   3. write(1, buffer, that length) through OxysSyscallInvoke3 — so the system's
 *                                   name appears in the log only if the result of
 *                                   step 2 came back, and only if the third
 *                                   argument of a three-argument call arrives
 *                                   where the kernel reads it. Added to the sum.
 *   4. write(1, newline, 1)         through OxysSyscallInvoke3, returning 1.
 *                                   Added to the sum.
 *   5. version(buffer, 8)           through OxysSyscallInvoke2 again, with a
 *                                   capacity smaller than the string, returning
 *                                   seven. Added to the sum. See the note upon
 *                                   VERIFY_WRAPPERS_SHORT_CAPACITY for why this
 *                                   call exists and step 2 does not replace it.
 *   6. write(99, buffer, 1)         through OxysSyscallInvoke3, which must fail
 *                                   with EBADF. Added to the sum.
 *   7. exit(sum * scale + ticks)    through OxysSyscallInvoke1.
 *
 * The status of step 6 is the whole mechanical result of this test, and it
 * carries two independent numbers in one integer.
 *
 *   **The sum is exact and the kernel knows it**: twice the length of the version
 *   string, plus one, plus seven, less four. Every call but the first contributes
 *   to it, so a
 *   result that did not come back, or came back wrong, changes it — and changes
 *   it by a number the kernel can see rather than by one that might be within
 *   some tolerance.
 *
 *   **The tick count is bounded and not exact**, this processor having observed
 *   it either side of the run and nothing being able to say what it was in
 *   between. It is therefore kept in the low digits, beneath a scale no boot
 *   reaches — a million ticks is a thousand seconds — so that the uncertainty in
 *   it cannot absorb an error in the sum. That separation is the point of the
 *   multiplication: added together the two would be one number with a tolerance,
 *   and an off-by-one in the sum would hide inside the tolerance of the count.
 *
 * RBX and RBP carry their values across four system calls apiece, which asserts
 * in passing something nothing else here does: that the kernel's entry path
 * restores the registers it is not returning in. A path that did not would leave
 * the arithmetic of step 6 operating upon whatever the kernel last had there.
 */
static void VerifyWrappersCompose(void)
{
    const uint64_t block = (uint64_t)(OxysSyscallInvokeEnd - OxysSyscallInvokeBegin);
    const uint64_t invoke0 =
        VERIFY_WRAPPERS_TEXT_OFFSET + VERIFY_WRAPPERS_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes0 - OxysSyscallInvokeBegin);
    const uint64_t invoke1 =
        VERIFY_WRAPPERS_TEXT_OFFSET + VERIFY_WRAPPERS_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes1 - OxysSyscallInvokeBegin);
    const uint64_t invoke2 =
        VERIFY_WRAPPERS_TEXT_OFFSET + VERIFY_WRAPPERS_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes2 - OxysSyscallInvokeBegin);
    const uint64_t invoke3 =
        VERIFY_WRAPPERS_TEXT_OFFSET + VERIFY_WRAPPERS_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes3 - OxysSyscallInvokeBegin);
    const uint64_t buffer = VERIFY_WRAPPERS_DATA_ADDRESS + VERIFY_WRAPPERS_BUFFER_AT;
    const uint64_t newline = VERIFY_WRAPPERS_DATA_ADDRESS + VERIFY_WRAPPERS_NEWLINE_AT;
    uint64_t at = VERIFY_WRAPPERS_TEXT_OFFSET;

    for (uint64_t index = 0U; index < VERIFY_WRAPPERS_IMAGE_BYTES; ++index)
    {
        VerifyWrappersImage[index] = 0U;
    }

    VerifyWrappersImage[0] = 0x7FU;
    VerifyWrappersImage[1] = 'E';
    VerifyWrappersImage[2] = 'L';
    VerifyWrappersImage[3] = 'F';
    VerifyWrappersImage[4] = (uint8_t)ELF_CLASS_64;
    VerifyWrappersImage[5] = (uint8_t)ELF_DATA_LITTLE_ENDIAN;
    VerifyWrappersImage[6] = (uint8_t)ELF_VERSION_CURRENT;

    VerifyWrappersPut16(16U, (uint16_t)ELF_TYPE_EXECUTABLE);
    VerifyWrappersPut16(18U, (uint16_t)ELF_MACHINE_X86_64);
    VerifyWrappersPut32(20U, ELF_VERSION_CURRENT);
    VerifyWrappersPut64(24U, VERIFY_WRAPPERS_TEXT_ADDRESS);
    VerifyWrappersPut64(32U, ELF_HEADER_BYTES);
    VerifyWrappersPut16(52U, (uint16_t)ELF_HEADER_BYTES);
    VerifyWrappersPut16(54U, (uint16_t)ELF_PROGRAM_HEADER_BYTES);
    VerifyWrappersPut16(56U, 2U);

    VerifyWrappersProgramHeader(ELF_HEADER_BYTES,
                                ELF_SEGMENT_READ | ELF_SEGMENT_EXECUTE,
                                VERIFY_WRAPPERS_TEXT_OFFSET, VERIFY_WRAPPERS_TEXT_ADDRESS,
                                VERIFY_WRAPPERS_TEXT_BYTES);
    VerifyWrappersProgramHeader(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES,
                                ELF_SEGMENT_READ | ELF_SEGMENT_WRITE,
                                VERIFY_WRAPPERS_DATA_OFFSET, VERIFY_WRAPPERS_DATA_ADDRESS,
                                VERIFY_WRAPPERS_DATA_BYTES);

    /* The library's own bytes, verbatim. This is the copy the whole arrangement
     * exists for: nothing here reassembles the instructions, and a change to
     * invoke.asm is a change to what this program executes. */
    for (uint64_t index = 0U; index < block; ++index)
    {
        VerifyWrappersImage[VERIFY_WRAPPERS_TEXT_OFFSET + VERIFY_WRAPPERS_INVOKE_AT +
                            index] = OxysSyscallInvokeBegin[index];
    }

    /* 1. ticks(), kept in RBP across everything that follows. */
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_TICKS);
    at = VerifyWrappersCall(at, invoke0);
    at = VerifyWrappersMoveRegister(at, VERIFY_WRAPPERS_RBP, VERIFY_WRAPPERS_RAX);

    /* 2. version(buffer, 64), which begins the sum in RBX and supplies the
     * length the next call writes. Both are taken from RAX before anything else
     * is loaded, that being the one register the call left something in. */
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_VERSION);
    at = VerifyWrappersMoveImmediate64(at, VERIFY_WRAPPERS_RSI, buffer);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDX, VERIFY_WRAPPERS_BUFFER_SIZE);
    at = VerifyWrappersCall(at, invoke2);
    at = VerifyWrappersMoveRegister(at, VERIFY_WRAPPERS_RCX, VERIFY_WRAPPERS_RAX);
    at = VerifyWrappersMoveRegister(at, VERIFY_WRAPPERS_RBX, VERIFY_WRAPPERS_RAX);

    /* 3. write(1, buffer, what version returned). */
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_WRITE);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RSI, 1U);
    at = VerifyWrappersMoveImmediate64(at, VERIFY_WRAPPERS_RDX, buffer);
    at = VerifyWrappersCall(at, invoke3);
    at = VerifyWrappersAddRegister(at, VERIFY_WRAPPERS_RBX, VERIFY_WRAPPERS_RAX);

    /* 4. write(1, newline, 1). The length is an immediate one, so a call that
     * lost its third argument returns something other than one here — which is
     * the defect the first version of this test could not see. */
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_WRITE);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RSI, 1U);
    at = VerifyWrappersMoveImmediate64(at, VERIFY_WRAPPERS_RDX, newline);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RCX, 1U);
    at = VerifyWrappersCall(at, invoke3);
    at = VerifyWrappersAddRegister(at, VERIFY_WRAPPERS_RBX, VERIFY_WRAPPERS_RAX);

    /* 5. version(buffer, 8), a capacity smaller than the string, so the result
     * is the capacity less one and nothing else could have produced it. */
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_VERSION);
    at = VerifyWrappersMoveImmediate64(at, VERIFY_WRAPPERS_RSI, buffer);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDX,
                                       VERIFY_WRAPPERS_SHORT_CAPACITY);
    at = VerifyWrappersCall(at, invoke2);
    at = VerifyWrappersAddRegister(at, VERIFY_WRAPPERS_RBX, VERIFY_WRAPPERS_RAX);

    /* 6. write(99, buffer, 1), which must fail with EBADF. */
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_WRITE);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RSI,
                                       VERIFY_WRAPPERS_BAD_DESCRIPTOR);
    at = VerifyWrappersMoveImmediate64(at, VERIFY_WRAPPERS_RDX, buffer);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RCX, 1U);
    at = VerifyWrappersCall(at, invoke3);
    at = VerifyWrappersAddRegister(at, VERIFY_WRAPPERS_RBX, VERIFY_WRAPPERS_RAX);

    /* 7. exit(sum * scale + ticks). */
    at = VerifyWrappersMultiplyImmediate(at, VERIFY_WRAPPERS_RBX,
                                         VERIFY_WRAPPERS_STATUS_SCALE);
    at = VerifyWrappersAddRegister(at, VERIFY_WRAPPERS_RBX, VERIFY_WRAPPERS_RBP);
    at = VerifyWrappersMoveRegister(at, VERIFY_WRAPPERS_RSI, VERIFY_WRAPPERS_RBX);
    at = VerifyWrappersMoveImmediate32(at, VERIFY_WRAPPERS_RDI, (uint32_t)SYSCALL_EXIT);
    at = VerifyWrappersCall(at, invoke1);

    /* Not reached: the call above does not return. It is here so that a kernel
     * which somehow returned from exit ends this program upon a fault that
     * belongs to it, rather than executing the zeroes that follow. */
    VerifyWrappersImage[at] = 0x0FU;
    VerifyWrappersImage[at + 1U] = 0x0BU;
    at += 2U;

    VerifyWrappersDriverEnd = at - VERIFY_WRAPPERS_TEXT_OFFSET;

    /* The data page: the buffer is left as zeroes for the kernel to fill, and
     * the newline is the one byte the program supplies itself. */
    VerifyWrappersImage[VERIFY_WRAPPERS_DATA_OFFSET + VERIFY_WRAPPERS_NEWLINE_AT] =
        (uint8_t)'\n';
}

/* -------------------------------------------------------------------------
 * The test itself.
 * ------------------------------------------------------------------------- */

void KernelVerifyWrappers(void)
{
    Process *process;
    Thread *boot;
    Thread *thread;
    ElfImage image;
    uint64_t stack;
    uint64_t dispatched_before;
    uint64_t ticks_before;
    uint64_t ticks_after;
    int64_t observed_ticks;
    int64_t observed_sum;
    int64_t expected_sum;
    size_t version_length;
    const uint64_t terminations_before = ProcessTerminationCount();
    const uint64_t block = (uint64_t)(OxysSyscallInvokeEnd - OxysSyscallInvokeBegin);

    VerifyWrappersSucceeded = true;

    KernelWriteString("Wrappers: asserting the C library's system-call wrappers.\n");

    VerifyWrappersTranslation();
    VerifyWrappersMessages();

    /*
     * The sum the program must end with, computed here from the same string the
     * kernel's version call copies.
     *
     * It is derived and not written out, because a constant written out is a
     * constant that stops being right the day the version string changes — and
     * the failure would be a self-test reporting that a correct program returned
     * the wrong thing. The kernel's call copies at most one byte fewer than the
     * capacity it is given, which is what the bound below expresses.
     */
    version_length = strlen(OXYS_SYSTEM_NAME " " OXYS_VERSION_STRING);

    if (version_length > (VERIFY_WRAPPERS_BUFFER_SIZE - 1U))
    {
        version_length = VERIFY_WRAPPERS_BUFFER_SIZE - 1U;
    }

    expected_sum = (2 * (int64_t)version_length) + 1 +
                   (int64_t)(VERIFY_WRAPPERS_SHORT_CAPACITY - 1U) + SYSCALL_EBADF;

    /* The block must exist and must fit where the driver expects it. Both are
     * properties of another file, which is exactly why they are asserted here
     * rather than assumed: invoke.asm may grow, and a block that overran its
     * displacement would be executed as whatever the driver's last bytes are. */
    VerifyWrappersRequire(block > 0U, "the library's invocation block is empty");
    VerifyWrappersRequire(
        (VERIFY_WRAPPERS_INVOKE_AT + block) <= VERIFY_WRAPPERS_TEXT_BYTES,
        "the library's invocation block does not fit in the program's text");

    VerifyWrappersCompose();

    VerifyWrappersRequire(VerifyWrappersDriverEnd <= VERIFY_WRAPPERS_INVOKE_AT,
                          "the composed driver runs into the invocation block");

    process = ProcessCreate("wrappers", NULL);
    VerifyWrappersRequire(process != NULL, "a process could not be created");

    if (process == NULL)
    {
        KernelWriteString("Wrapper self-test FAILED.\n");
        return;
    }

    if (ElfLoad(&process->space, VerifyWrappersImage, VERIFY_WRAPPERS_IMAGE_BYTES,
                &image) != ELF_OK)
    {
        ProcessDestroy(process);
        KernelWriteString("  The composed program did not load. FAILED.\n");
        KernelWriteString("Wrapper self-test FAILED.\n");
        return;
    }

    ProcessRecordImage(process, &image);
    stack = ProcessCreateUserStack(process);
    VerifyWrappersRequire(stack != 0U, "the program was given no stack");

    boot = ThreadAdoptCurrent("boot");
    thread = ThreadCreate(process, image.entry, stack);
    VerifyWrappersRequire(thread != NULL, "a thread could not be created");

    if ((boot == NULL) || (thread == NULL) || (stack == 0U))
    {
        ProcessDestroy(process);

        if (boot != NULL)
        {
            ThreadDestroy(boot);
        }

        KernelWriteString("Wrapper self-test FAILED.\n");
        return;
    }

    dispatched_before = SyscallDispatched();
    ticks_before = PitTickCount();

    /*
     * The program runs, and what it writes appears in the log between this line
     * and the next: the system's name, fetched by a call and written by another,
     * both of them made through the bytes this library ships.
     */
    KernelWriteString("  A program at privilege level 3 reports, through the "
                      "library's own invocation: ");

    VerifyWrappersRequire(ThreadStart(thread), "the program could not be started");

    ticks_after = PitTickCount();

    VerifyWrappersRequire(ThreadCurrent() == boot,
                          "the kernel did not resume the thread that started the "
                          "program");

    /*
     * Six calls and not five or seven.
     *
     * A count rather than a floor, because the failure this catches is a call
     * that did not happen: an invocation whose displacement was wrong would call
     * into the middle of another routine, which upon this block is still a valid
     * instruction sequence and would still return. The number of calls that
     * reached the dispatcher is what distinguishes that from six correct ones.
     */
    VerifyWrappersRequire((SyscallDispatched() - dispatched_before) == 7U,
                          "the program did not make exactly the seven calls it was "
                          "composed to make");

    VerifyWrappersRequire(ProcessTerminationCount() == (terminations_before + 1U),
                          "the program was not ended");
    VerifyWrappersRequire(process->state == PROCESS_EXITED,
                          "the program's process was not marked as ended");

    /*
     * The status, which is the whole mechanical result: a sum the kernel knows
     * exactly, scaled, plus a tick count it knows the bounds of.
     *
     * The sum is what every call after the first returned — the length of the
     * version string twice over, once from the call that produced it and once
     * from the call that wrote it; one for the newline; and EBADF for the
     * descriptor that had to be refused. A program that lost any one of those, or
     * received a different one, produces a different sum, and the difference is
     * not within any tolerance because there is none.
     *
     * The separation is checked before it is relied upon. A tick count that had
     * reached the scale would have carried into the sum, and the assertion would
     * then be reporting upon a number that is two numbers added together.
     */
    observed_sum = process->exit_status / VERIFY_WRAPPERS_STATUS_SCALE;
    observed_ticks = process->exit_status % VERIFY_WRAPPERS_STATUS_SCALE;

    VerifyWrappersRequire((int64_t)ticks_after < VERIFY_WRAPPERS_STATUS_SCALE,
                          "the tick count has reached the scale that holds it apart "
                          "from the sum");

    VerifyWrappersRequire(observed_sum == expected_sum,
                          "the program's calls did not return what they had to "
                          "return");

    VerifyWrappersRequire(observed_ticks >= (int64_t)ticks_before,
                          "the tick count the program read precedes the run");
    VerifyWrappersRequire(observed_ticks <= (int64_t)ticks_after,
                          "the tick count the program read follows the run");

    ProcessDestroy(process);
    ThreadDestroy(boot);

    KernelWriteString(VerifyWrappersSucceeded ? "Wrapper self-test passed.\n"
                                              : "Wrapper self-test FAILED.\n");
}
