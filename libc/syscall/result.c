/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/syscall/result.c
 * Purpose: Translates a kernel result into a library result, and holds the
 *          errno it sets — this being the only thing in this library that
 *          writes errno at all.
 * Key functions: OxysErrnoAddress, OxysSyscallResult.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.5, paragraph 2: errno has type int and
 *     thread local storage duration, and footnote 201 permits the macro to
 *     expand to an lvalue resulting from a function call — which is why the
 *     object below is reached through a function and is not itself exported.
 *   - ISO/IEC 9899:2011, Section 7.5, paragraph 3: errno is zero at program
 *     startup and is never set to zero by a library function. Both properties
 *     are asserted; see kernel/test/verify_wrappers.c.
 *   - kernel/abi/oxys/syscall_abi.h: the failure results this translates, which
 *     are negative so that a caller may tell a failure from a length without a
 *     second register.
 *   - docs/design/LIBC.md: the translation, and the bound it
 *     refuses beyond.
 *
 * Why this is a function of its own.
 *
 * Every wrapper in calls.c is an invocation followed by this, and it could
 * therefore have been a line repeated seven times. It is not, for a reason that
 * is about testing rather than about repetition: SYSCALL cannot be executed by
 * anything but a program at privilege level 3, so nothing in this library can be
 * run by the kernel's boot-time self-test except the part that is on this side
 * of the instruction. Holding that part apart is what gives this sub-task an
 * assertion that runs today, rather than one deferred until sub-task 7.5 links
 * the first program.
 */

#include <errno.h>
#include <syscall.h>

/*
 * The one errno there is.
 *
 * Section 7.5 requires thread local storage duration, and this object does not
 * have it: there are no userland threads to give it to, and a _Thread_local
 * object would need a thread-local storage block that nothing in this system
 * allocates. The storage is therefore one object today and reached through a
 * function so that it can stop being one without any program that reads errno
 * being affected — the caller resolves OxysErrnoAddress, not the object, so the
 * day a thread has an errno of its own this file changes and nothing else does.
 * docs/design/LIBC.md.
 *
 * Its initial value is zero, as Section 7.5, paragraph 3, requires of the
 * initial thread at program startup. It is an object of static storage duration
 * with no initialiser, which the standard gives that value; writing the zero out
 * would place it in the initialised data of every program that links this.
 */
static int OxysErrnoObject;

int *OxysErrnoAddress(void)
{
    return &OxysErrnoObject;
}

/*
 * The translation.
 *
 * Three things happen here and each has a reason a reader should be able to
 * check.
 *
 * A result that is not negative is returned exactly as it stands, and errno is
 * not touched. Section 7.5, paragraph 3, would permit a library to set errno
 * upon a successful call; this one does not, and says so in <syscall.h>, because
 * a program that clears errno, calls a wrapper that succeeds and then finds
 * errno set has been told a failure that did not happen.
 *
 * A negative result within the reserved range becomes its own name. The
 * arithmetic is the whole of the mapping: <errno.h> defines each number as the
 * negation of the kernel's result and asserts it at compile time, so there is no
 * table here to fall out of agreement with the kernel's.
 *
 * A negative result outside that range becomes ENOSYS. Nothing this kernel
 * returns is outside it — the static assertions in <errno.h> say so — which
 * makes this branch a guard against a kernel that has changed and a library that
 * has not. It must not simply negate: an arbitrary value would put a number into
 * a program's errno that names nothing, and INT64_MIN has no positive
 * counterpart at all, so negating it would be the undefined behaviour
 * PROJECT_GUIDELINES.md, Section 8, forbids. ENOSYS is the answer because it is
 * the one failure that means precisely what is known in that case: the system
 * did not perform the call, and the library cannot say why.
 */
int64_t OxysSyscallResult(int64_t result)
{
    if (result >= 0)
    {
        return result;
    }

    if (result >= -(int64_t)OXYS_ERRNO_SYSCALL_LIMIT)
    {
        errno = (int)(-result);
    }
    else
    {
        errno = ENOSYS;
    }

    return -1;
}
