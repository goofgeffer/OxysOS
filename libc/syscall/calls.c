/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/syscall/calls.c
 * Purpose: The eight system-call wrappers, one for each call
 *          <oxys/syscall_abi.h> numbers: the arguments named rather than
 *          numbered, and the result translated into the convention a C program
 *          expects.
 * Key functions: OxysWrite, OxysTicks, OxysVersion, OxysFork, OxysExecve,
 *          OxysExit, OxysWait, OxysBrk, OxysSbrk.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the call numbers and what each call means.
 *   - kernel/cpu/syscall.c: the kernel's half, which is what these agree with.
 *   - ISO/IEC 9899:2011, Section 7.5: the errno OxysSyscallResult sets.
 *   - docs/design/LIBC.md, Section 8.3: what each wrapper does with its
 *     arguments, and the two places it does something a reader would not guess.
 *
 * What a wrapper is for, given that it is four lines.
 *
 * Every function here is a cast of its arguments, an invocation and a
 * translation, and a program could perform all three itself. What it could not
 * do itself is get them right once. The kernel's interface is eight numbers and
 * a register convention; a program that wrote out the invocation at each call
 * site would repeat that convention at every one of them, and the way that fails
 * is not that the program stops working but that one call site puts an argument
 * in the wrong place and reports a plausible failure for ever. These wrappers
 * are the place where the number and the order of the arguments are written
 * down once, and the type of each is the compiler's business thereafter.
 *
 * The casts to uint64_t are unavoidable and are not hiding anything. The
 * invocation takes machine words because that is what the register convention
 * transfers; the parameters above them have the types the call actually means,
 * which is the point of the wrapper. Each cast below is from a type that fits,
 * and the two that could lose something — a pointer and a signed status —
 * cannot: a pointer converts to uint64_t and back without loss under the LP64
 * model of the System V ABI, Section 3.1.2, and a signed status is carried as
 * its two's complement representation and read back as one by the kernel.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall.h>

int64_t OxysWrite(int descriptor, const void *buffer, size_t length)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_WRITE, (uint64_t)descriptor,
                                                (uint64_t)(uintptr_t)buffer,
                                                (uint64_t)length));
}

int64_t OxysTicks(void)
{
    return OxysSyscallResult(OxysSyscallInvoke0(SYSCALL_TICKS));
}

int64_t OxysVersion(char *buffer, size_t capacity)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_VERSION,
                                                (uint64_t)(uintptr_t)buffer,
                                                (uint64_t)capacity));
}

int64_t OxysFork(void)
{
    return OxysSyscallResult(OxysSyscallInvoke0(SYSCALL_FORK));
}

/*
 * The two vectors are passed on rather than dropped, and this is the first of
 * the two places a reader would guess wrong.
 *
 * The kernel refuses any vector that is not null, and this wrapper could
 * therefore have taken a path alone and passed two zeroes. It does not, because
 * the refusal is the kernel's to make and not the library's to conceal: a
 * program that passes arguments must be told that they were not accepted, and a
 * wrapper that quietly passed null in their place would turn a refusal into a
 * program running with no arguments and no way to discover why.
 */
int64_t OxysExecve(const char *path, char *const argument_vector[],
                   char *const environment_vector[])
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_EXECVE,
                                                (uint64_t)(uintptr_t)path,
                                                (uint64_t)(uintptr_t)argument_vector,
                                                (uint64_t)(uintptr_t)environment_vector));
}

/*
 * The loop after the invocation is the second, and it is not dead code in the
 * sense a reader might take it for.
 *
 * SYSCALL_EXIT does not return: the kernel ends the thread that made the call
 * and resumes something else, so the instruction after the invocation is never
 * executed. The function is declared _Noreturn all the same, which is what lets
 * a caller's compiler know that the code after a call to it is unreachable — and
 * a _Noreturn function that a compiler can see falling off its end is a
 * diagnostic in every compiler this project is built with. The loop is what
 * makes the declaration true by construction rather than by the kernel keeping
 * a promise the compiler cannot check.
 */
_Noreturn void OxysExit(int64_t status)
{
    (void)OxysSyscallInvoke1(SYSCALL_EXIT, (uint64_t)status);

    for (;;)
    {
    }
}

int64_t OxysWait(int64_t *status)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_WAIT,
                                                (uint64_t)(uintptr_t)status));
}

/* ---------------------------------------------------------- sub-task 7.3 */

int64_t OxysBrk(void *address)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_BRK,
                                                (uint64_t)(uintptr_t)address));
}

/*
 * The relative form, built from the absolute one, and the arithmetic between
 * them is the whole of what it adds.
 *
 * Three things are done in an order that is not free, and each of them answers a
 * way this function is commonly written wrongly.
 *
 * **The break is read before it is moved.** The caller is owed the address of the
 * memory it has just obtained, which is where the break stood *before* the
 * growth; a function that moved first and subtracted afterwards would be right
 * only so long as the kernel granted exactly what was asked, and this kernel is
 * entitled to refuse.
 *
 * **The sum is checked before it is made.** A caller may pass an increment that
 * carries the break past the greatest representable address, and the sum would
 * then wrap to a small one — which the kernel would refuse as being below the
 * heap's first byte, reporting EINVAL for what is in truth a request too large
 * to be met. The check is written as a comparison against what remains, for the
 * reason the kernel's own range check is: a sum that has already overflowed
 * cannot be tested for having overflowed.
 *
 * **A negative increment is a shrink and not an error.** It is the only way a
 * program gives memory back, and the same bound applies from the other side: an
 * increment more negative than the break's distance above zero would wrap
 * upward.
 */
void *OxysSbrk(intptr_t increment)
{
    const int64_t established = OxysBrk(NULL);
    uint64_t wanted;

    if (established < 0)
    {
        return (void *)(intptr_t)-1;
    }

    if (increment == 0)
    {
        return (void *)(uintptr_t)established;
    }

    if (increment > 0)
    {
        if ((uint64_t)increment > (UINT64_MAX - (uint64_t)established))
        {
            errno = ENOMEM;

            return (void *)(intptr_t)-1;
        }

        wanted = (uint64_t)established + (uint64_t)increment;
    }
    else
    {
        /*
         * The magnitude is taken by negating the value as an unsigned quantity
         * rather than as a signed one. Negating INTPTR_MIN is undefined
         * behaviour — there is no positive counterpart of it — and the
         * conversion to uint64_t followed by an unsigned negation is defined for
         * every value by ISO/IEC 9899:2011, Section 6.3.1.3, paragraph 2, and
         * Section 6.2.5, paragraph 9.
         */
        const uint64_t magnitude = (uint64_t)0 - (uint64_t)increment;

        if (magnitude > (uint64_t)established)
        {
            errno = EINVAL;

            return (void *)(intptr_t)-1;
        }

        wanted = (uint64_t)established - magnitude;
    }

    if (OxysBrk((void *)(uintptr_t)wanted) < 0)
    {
        return (void *)(intptr_t)-1;
    }

    return (void *)(uintptr_t)established;
}
