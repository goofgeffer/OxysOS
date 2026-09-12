/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/syscall.h
 * Purpose: Declares the C library's system-call wrappers — one for each of the
 *          eight calls <oxys/syscall_abi.h> numbers — together with the raw
 *          invocation they are built upon and the translation that turns a
 *          kernel result into a library result and an errno.
 * Key definitions: OxysSyscallInvoke0, OxysSyscallInvoke1, OxysSyscallInvoke2,
 *          OxysSyscallInvoke3, OxysSyscallResult, OxysWrite, OxysTicks,
 *          OxysVersion, OxysFork, OxysExecve, OxysExit, OxysWait, OxysBrk,
 *          OxysSbrk.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the call numbers, the failure results and
 *     the register convention. This header declares symbols; that one declares
 *     none, and the division is the licensing one recorded in
 *     docs/design/LIBC.md, Section 2.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
 *     "SYSCALL": the instruction places the address of the following
 *     instruction in RCX and RFLAGS in R11, so neither survives a call.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.3:
 *     the first six integer arguments arrive in RDI, RSI, RDX, RCX, R8 and R9,
 *     which is what the raw invocations below shift by one place.
 *   - ISO/IEC 9899:2011, Section 7.5: the errno these wrappers set, and the rule
 *     that no library function sets it to zero.
 *   - docs/design/LIBC.md, Section 8: the design of this sub-task, what is
 *     asserted of it and what is not.
 *
 * Why the names are this project's and not POSIX's.
 *
 * Five of the first seven calls have a POSIX name that means very nearly this — write,
 * fork, execve, _exit, wait — and none of the five means exactly it. This
 * execve refuses an argument vector because there is no convention yet fixed for
 * where a program finds one; this wait takes no process identifier and no option
 * flags; this write reaches two diagnostic descriptors and no file. A function
 * bearing a standard name and behaving otherwise is worse than either the
 * standard function or a differently named one, which is the same judgement
 * docs/design/LIBC.md, Section 4, records about strlcpy and strdup. The POSIX
 * spellings arrive when the semantics do, and the names here comply meanwhile
 * with PROJECT_GUIDELINES.md, Section 4, under which a global function is
 * PascalCase.
 *
 * How a wrapper reports a failure.
 *
 * The kernel returns a negative result. A wrapper returns -1 and sets errno to
 * that result's name, which is the convention every C library above a kernel of
 * this shape uses and the reason a program need not know the kernel's numbers to
 * read them. Anything not negative is returned as it stands and errno is left
 * alone; this library sets errno upon failure and at no other time, which
 * Section 7.5, paragraph 3, permits a library to promise and does not require.
 */

#ifndef OXYS_LIBC_SYSCALL_H
#define OXYS_LIBC_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#include <oxys/syscall_abi.h>

/* -------------------------------------------------------------------------
 * The raw invocation.
 * ------------------------------------------------------------------------- */

/*
 * Executes SYSCALL with the given call number and arguments, and returns what
 * the kernel put in RAX — untranslated, so a failure arrives here as the
 * negative result the kernel chose and not as -1.
 *
 * There is one of these for each number of arguments a call of this kernel
 * actually takes, which is none to three. Four, five and six are not written:
 * the register convention reserves R10, R8 and R9 for them, no call uses one,
 * and a wrapper nothing calls is a wrapper nothing asserts. They arrive with the
 * first call that needs them, by which time there will be something to assert
 * them against.
 *
 * They are assembly and not inline assembly, which is a deliberate departure
 * from the kernel's practice. Two reasons, and the second is the larger. First,
 * PROJECT_GUIDELINES.md, Section 8, prohibits a GCC-specific extension that has
 * not been justified, and a translation unit of NASM is not an extension of the
 * C language at all. Second, these routines contain no memory reference, no
 * relative displacement and no relocation, so the bytes the assembler emits mean
 * the same thing at every address — which is what allows the boot-time self-test
 * to copy them into a program's own address space and run them at privilege
 * level 3. Inline assembly would have produced the same instructions inside a
 * function the compiler was free to place a prologue, a stack frame and a
 * reference to a kernel address in, and no assertion could then have been made
 * upon the code this library actually ships.
 */
int64_t OxysSyscallInvoke0(uint64_t number);
int64_t OxysSyscallInvoke1(uint64_t number, uint64_t first);
int64_t OxysSyscallInvoke2(uint64_t number, uint64_t first, uint64_t second);
int64_t OxysSyscallInvoke3(uint64_t number, uint64_t first, uint64_t second,
                           uint64_t third);

/*
 * Translates a kernel result into a library result.
 *
 * A result that is not negative is returned unchanged and errno is not touched.
 * A negative one leaves -1 in the caller's hands and its name in errno.
 *
 * It is a function of its own, and exported, for a reason that is not tidiness:
 * it is the only part of this sub-task that can be executed by anything other
 * than a program at privilege level 3, so it is the only part the kernel's
 * boot-time self-test can assert directly. See docs/design/LIBC.md, Section 8.4.
 */
int64_t OxysSyscallResult(int64_t result);

/* -------------------------------------------------------------------------
 * The seven calls of sub-task 7.2.
 * ------------------------------------------------------------------------- */

/*
 * Writes bytes to a diagnostic descriptor — 1 the output, 2 the error — and
 * returns how many were written, which may be fewer than were asked for: the
 * kernel bounds a single transfer, and a caller that means to write more must
 * call again from where this one stopped.
 *
 * Returns -1 with errno set to EBADF for any other descriptor, and to EFAULT
 * for a range this program may not read. There are no files yet, which is why
 * there is no descriptor to open one with.
 */
int64_t OxysWrite(int descriptor, const void *buffer, size_t length);

/* The interval timer's count since the kernel started it. It cannot fail, and
 * it is the only clock this system offers a program. */
int64_t OxysTicks(void);

/*
 * Copies the system's name and version into the caller's buffer and returns the
 * number of bytes copied, excluding the terminator the kernel always writes.
 *
 * A capacity of zero is refused with EINVAL rather than treated as a request for
 * nothing, there being no room even for the terminator.
 */
int64_t OxysVersion(char *buffer, size_t capacity);

/*
 * Makes a child of the calling program, which resumes at the same place with a
 * result of zero. The parent receives the child's identifier.
 *
 * Returns -1 with errno set to ENOMEM where the frames, the tables or the slot
 * could not be had.
 */
int64_t OxysFork(void);

/*
 * Replaces the calling program with one read from a volume. Upon success it does
 * not return, the caller already executing the new program.
 *
 * The two vectors must both be null. The kernel refuses anything else with
 * EINVAL rather than discarding it, because there is no convention yet fixed for
 * where a program finds its arguments upon its stack and a program that passed
 * some and found none would have no way to tell that they had been thrown away.
 * They are parameters of this wrapper all the same, so that the day the
 * convention exists the interface does not change under every caller.
 */
int64_t OxysExecve(const char *path, char *const argument_vector[],
                   char *const environment_vector[]);

/* Ends the calling program with the given status. It does not return: there is
 * nothing for it to return to. */
_Noreturn void OxysExit(int64_t status);

/*
 * Collects a child that has ended and returns its identifier, placing what it
 * ended with in `status` unless that is null.
 *
 * Returns -1 with errno set to ECHILD where the caller has no child to collect,
 * and to EFAULT where the status has nowhere this program may write it.
 */
int64_t OxysWait(int64_t *status);

/* -------------------------------------------------------------------------
 * The eighth call, of sub-task 7.3, and the classic spelling above it.
 * ------------------------------------------------------------------------- */

/*
 * Moves the program's break — the address one past the last byte of its heap —
 * to the given address, and returns where it stands afterwards.
 *
 * A null argument reports the break rather than moving it, which is how a
 * program discovers its own heap before it has grown one. That is the kernel's
 * SYSCALL_BREAK_QUERY, spelled here as the null pointer because a null pointer is
 * what a C caller has to hand and the two are the same value.
 *
 * Returns -1 with errno set to ENOMEM where the address is beyond what the
 * kernel will map, or where a page of it could not be had; and to EINVAL where
 * it lies below the heap's first byte. **Upon failure the break has not moved**,
 * including a growth that ran out of frames half way: the kernel withdraws what
 * it mapped rather than reporting a heap that is part there.
 */
int64_t OxysBrk(void *address);

/*
 * Moves the break by a relative amount and returns where it stood **before** the
 * move, which is the address of the memory just obtained.
 *
 * This is the operation an allocator actually wants, and it is built here from
 * OxysBrk rather than given a call of its own: the kernel has no need to know
 * that a caller thinks in increments, and a second call number would be a second
 * thing to agree about for no gain. An increment of zero reports the break
 * without moving it.
 *
 * Returns (void *)-1 with errno set, for the reasons OxysBrk does. That is the
 * failure value this function has to use rather than a null pointer: zero is a
 * plausible answer for nothing at all, whereas no break this kernel establishes
 * can be the greatest representable address.
 */
void *OxysSbrk(intptr_t increment);

#endif /* OXYS_LIBC_SYSCALL_H */
