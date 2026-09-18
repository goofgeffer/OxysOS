/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/syscall/calls.c
 * Purpose: The twenty-nine system-call wrappers, one for each call
 *          <oxys/syscall_abi.h> numbers: the arguments named rather than
 *          numbered, and the result translated into the convention a C program
 *          expects.
 * Key functions: OxysWrite, OxysTicks, OxysVersion, OxysFork, OxysExecve,
 *          OxysOpen, OxysClose, OxysRead, OxysReadDirectory,
 *          OxysMakeDirectory, OxysUnlink, OxysChangeDirectory,
 *          OxysGetWorkingDirectory, OxysDuplicate, OxysRemoveDirectory, OxysPipe,
 *          OxysWaitFor, OxysKill, OxysSignalAction, OxysGetProcessId,
 *          OxysGetProcessGroup, OxysSetProcessGroup, OxysTerminalGroup,
 *          OxysLink, OxysProcessInformation, OxysWindowCreate,
 *          OxysWindowDestroy, OxysWindowMove, OxysWindowBlit, OxysWindowEvent,
 *          OxysWindowScreen, OxysPower, OxysPause,
 *          OxysExit, OxysWait, OxysBrk, OxysSbrk.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the call numbers and what each call means.
 *   - kernel/arch/x86_64/syscall/syscall.c: the kernel's half, which is what these agree with.
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
 * The two vectors are passed on rather than dropped, which they have been since
 * this wrapper was written and which now means something.
 *
 * Until sub-task 7.6 the kernel refused any vector that was not null, and this
 * wrapper could therefore have taken a path alone and passed two zeroes. It did
 * not, on the ground that a refusal is the kernel's to make and not the
 * library's to conceal. The kernel accepts them now, and the wrapper did not
 * have to change — which is the whole return upon having declined to narrow the
 * interface to what the kernel of the day happened to implement.
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

/* ---------------------------------------------------------- sub-task 7.6 */

int64_t OxysOpen(const char *path, uint64_t flags, uint16_t permissions)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_OPEN, (uint64_t)(uintptr_t)path,
                                                flags, (uint64_t)permissions));
}

int64_t OxysClose(int descriptor)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_CLOSE,
                                                (uint64_t)descriptor));
}

int64_t OxysRead(int descriptor, void *buffer, size_t length)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_READ, (uint64_t)descriptor,
                                                (uint64_t)(uintptr_t)buffer,
                                                (uint64_t)length));
}

/*
 * The third place a reader would guess wrong, and it is about the result rather
 * than the arguments.
 *
 * This returns 1, 0 or -1 and not a count of bytes, so the translation below
 * does exactly what it does everywhere else and the meaning of a non-negative
 * result is this call's own. It is written out here because "read" in the name
 * invites the assumption that the number is a length.
 */
int64_t OxysReadDirectory(int descriptor, SyscallDirectoryEntry *entry)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_READDIR,
                                                (uint64_t)descriptor,
                                                (uint64_t)(uintptr_t)entry));
}

int64_t OxysMakeDirectory(const char *path, uint16_t permissions)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_MKDIR,
                                                (uint64_t)(uintptr_t)path,
                                                (uint64_t)permissions));
}

int64_t OxysUnlink(const char *path)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_UNLINK,
                                                (uint64_t)(uintptr_t)path));
}

int64_t OxysChangeDirectory(const char *path)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_CHDIR,
                                                (uint64_t)(uintptr_t)path));
}

int64_t OxysGetWorkingDirectory(char *buffer, size_t capacity)
{
    const int64_t result = OxysSyscallInvoke2(SYSCALL_GETCWD,
                                              (uint64_t)(uintptr_t)buffer,
                                              (uint64_t)capacity);

    /* The one translation this file makes, for the reason the header gives:
     * the standard names ERANGE for a buffer too small, and the kernel has no
     * such result. */
    if (result == SYSCALL_ENAMETOOLONG)
    {
        errno = ERANGE;

        return -1;
    }

    return OxysSyscallResult(result);
}

int64_t OxysDuplicate(int from, int to)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_DUP2, (uint64_t)from, (uint64_t)to));
}

int64_t OxysRemoveDirectory(const char *path)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_RMDIR,
                                                (uint64_t)(uintptr_t)path));
}

int64_t OxysPipe(int descriptors[2])
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_PIPE,
                                                (uint64_t)(uintptr_t)descriptors));
}

/* ---------------------------------------------------------- sub-task 8.7 */

int64_t OxysWaitFor(int64_t pid, int64_t *status, uint64_t options)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_WAITPID, (uint64_t)pid,
                                                (uint64_t)(uintptr_t)status, options));
}

int64_t OxysKill(int64_t pid, int signal)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_KILL, (uint64_t)pid,
                                                (uint64_t)signal));
}

int64_t OxysSignalAction(int signal, uint64_t disposition, uint64_t restorer)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_SIGACTION, (uint64_t)signal,
                                                disposition, restorer));
}

int64_t OxysGetProcessId(void)
{
    return OxysSyscallResult(OxysSyscallInvoke0(SYSCALL_GETPID));
}

int64_t OxysGetProcessGroup(int64_t pid)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_GETPGID, (uint64_t)pid));
}

int64_t OxysSetProcessGroup(int64_t pid, int64_t group)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_SETPGID, (uint64_t)pid,
                                                (uint64_t)group));
}

int64_t OxysTerminalGroup(int64_t group)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_TCGROUP, (uint64_t)group));
}

/* ---------------------------------------------------- 2026-09-16, beside 8.7 */

int64_t OxysLink(const char *existing, const char *name)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_LINK, (uint64_t)(uintptr_t)existing,
                                                (uint64_t)(uintptr_t)name));
}

int64_t OxysProcessInformation(uint64_t index, SyscallProcessInformation *information)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_PROCINFO, index,
                                                (uint64_t)(uintptr_t)information));
}

/* ------------------------------------------------------------ sub-task 9.2 */

int64_t OxysWindowCreate(const SyscallWindowRectangle *geometry, const char *title)
{
    return OxysSyscallResult(OxysSyscallInvoke2(SYSCALL_WINDOW_CREATE,
                                                (uint64_t)(uintptr_t)geometry,
                                                (uint64_t)(uintptr_t)title));
}

int64_t OxysWindowDestroy(int64_t window)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_WINDOW_DESTROY, (uint64_t)window));
}

int64_t OxysWindowMove(int64_t window, int32_t x, int32_t y)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_WINDOW_MOVE, (uint64_t)window,
                                                (uint64_t)(int64_t)x, (uint64_t)(int64_t)y));
}

int64_t OxysWindowBlit(int64_t window, const SyscallWindowRectangle *area,
                       const uint32_t *pixels)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_WINDOW_BLIT, (uint64_t)window,
                                                (uint64_t)(uintptr_t)area,
                                                (uint64_t)(uintptr_t)pixels));
}

int64_t OxysWindowEvent(int64_t window, SyscallWindowEvent *event, uint64_t flags)
{
    return OxysSyscallResult(OxysSyscallInvoke3(SYSCALL_WINDOW_EVENT, (uint64_t)window,
                                                (uint64_t)(uintptr_t)event, flags));
}

int64_t OxysWindowScreen(SyscallWindowRectangle *geometry)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_WINDOW_SCREEN,
                                                (uint64_t)(uintptr_t)geometry));
}

/* ------------------------------------------------------------ sub-task 9.3 */

int64_t OxysPower(uint64_t action)
{
    return OxysSyscallResult(OxysSyscallInvoke1(SYSCALL_POWER, action));
}

int64_t OxysPause(void)
{
    return OxysSyscallResult(OxysSyscallInvoke0(SYSCALL_PAUSE));
}
