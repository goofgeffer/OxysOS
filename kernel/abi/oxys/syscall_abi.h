/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: kernel/abi/oxys/syscall_abi.h
 * Purpose: Declares the whole of the system-call interface a program is
 *          entitled to know — the register convention, the call numbers, the
 *          results a call may fail with, and the two limits an argument is
 *          judged against — and nothing of the kernel's implementation of it.
 * Key definitions: SYSCALL_ARGUMENT_MAXIMUM, SYSCALL_WRITE, SYSCALL_TICKS,
 *          SYSCALL_VERSION, SYSCALL_FORK, SYSCALL_EXECVE, SYSCALL_EXIT,
 *          SYSCALL_WAIT, SYSCALL_COUNT, SYSCALL_OK, SYSCALL_ENOSYS,
 *          SYSCALL_EFAULT, SYSCALL_EINVAL, SYSCALL_EBADF, SYSCALL_ECHILD,
 *          SYSCALL_ENOENT, SYSCALL_ENOMEM, SYSCALL_PATH_MAXIMUM,
 *          SYSCALL_USER_LIMIT.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
 *     "SYSCALL" and "SYSRET": the instruction places the address of the
 *     following instruction in RCX and RFLAGS in R11, which is why the fourth
 *     argument is not where a C caller would put it.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.3:
 *     the six integer argument registers, from which the convention below
 *     departs in exactly one place.
 *   - docs/design/PRIVILEGE.md, Sections 6 and 7: the dispatch and the
 *     validation these numbers and limits are consumed by.
 *   - docs/design/LIBC.md, Section 2: the division of which this file is one
 *     half, and the licensing obligation that required it.
 *
 * Why this file is separate from <oxys/syscall.h>, and why it is MIT.
 *
 * The kernel is LGPL-3.0-or-later and the C library above it is MIT, and until
 * this file existed both halves of the interface stood in one LGPL header: the
 * numbers and the errors, which a program must know, beside IA32_STAR, the flag
 * mask, the saved register frame and the validation, which are no business of
 * any program. A permissively licensed library cannot cleanly include such a
 * header, and the difficulty is not cured by the position — correct and recorded
 * in LICENSING.md, Section 2 — that *calling* this kernel makes no program a
 * derivative work of it. That paragraph disposes of the calls; it does not
 * dispose of the headers.
 *
 * So the interface is here, under the permissive licence, and may be included by
 * anything: this kernel, the C library of Phase 7, a program written by somebody
 * who has never seen the rest of this repository, and a libc that is not this
 * one. The implementation remains in <oxys/syscall.h>, which includes this file
 * and adds what only the kernel may see. LICENSING.md, Section 2.1, required the
 * division to be made before the wrappers of sub-task 7.2 were written, on the
 * ground that it is easier while the interface is seven calls than after a
 * library depends upon it.
 *
 * Nothing here may acquire a declaration. A function declared in this file would
 * be a function the kernel and every program had to agree existed; the whole
 * value of an interface header is that it defines constants and a convention and
 * commits neither side to a symbol.
 */

#ifndef OXYS_SYSCALL_ABI_H
#define OXYS_SYSCALL_ABI_H

/*
 * ISO/IEC 9899:2011, Section 4, paragraph 6, requires a freestanding
 * implementation to provide <stdint.h>, so this file depends upon nothing that
 * a program compiled without a C library does not already have. It deliberately
 * does not include <oxys/types.h>, which is the kernel's and is licensed as the
 * kernel is.
 */
#include <stdint.h>

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
 * The call number is in RAX and the result returns in RAX. RCX and R11 do not
 * survive the call, the instruction having taken both; every other register
 * does, the entry path saving and restoring the whole set.
 */
#define SYSCALL_ARGUMENT_MAXIMUM 6U

/* The calls this kernel implements. The numbers are its own: there is no library
 * to agree with, and none of these is a POSIX call in anything but spirit. */
#define SYSCALL_WRITE   0U
#define SYSCALL_TICKS   1U
#define SYSCALL_VERSION 2U

/*
 * The four calls of sub-task 6.11, by which a program may make another program,
 * become another program, end, and collect what one of its children ended with.
 *
 * They are numbered after the three that existed rather than interleaved among
 * them, because a number already handed to a program is a number that must not
 * change: the self-test of sub-task 6.10 assembles `write` as call zero by hand,
 * and every program written before this sub-task would call something else if
 * the numbering were rearranged to look tidier. That rule binds harder now that
 * this file is the thing programs are compiled against rather than a header
 * internal to the kernel.
 */
#define SYSCALL_FORK    3U
#define SYSCALL_EXECVE  4U
#define SYSCALL_EXIT    5U
#define SYSCALL_WAIT    6U
#define SYSCALL_COUNT   7U

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
#define SYSCALL_ECHILD         INT64_C(-5)  /* The caller has no children to wait for. */
#define SYSCALL_ENOENT         INT64_C(-6)  /* No such file, or one that will not load. */
#define SYSCALL_ENOMEM         INT64_C(-7)  /* A frame, a table or a slot could not be had. */

/*
 * The greatest length of a path a caller may name, excluding its terminator.
 *
 * A bound is needed before the string is copied, and it must be the copy that is
 * bounded rather than the search for the terminator: a caller may name a page of
 * bytes with no zero in it at all, and a kernel that looked for one before
 * deciding how much to read would walk off the end of the caller's mapping and
 * fault in its own name.
 *
 * It is stated here and not in the kernel's half because a program that composes
 * a path longer than this is a program the kernel will refuse, and the bound it
 * will be refused against is something it is entitled to know before it calls.
 */
#define SYSCALL_PATH_MAXIMUM 255U

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

#endif /* OXYS_SYSCALL_ABI_H */
