/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/errno.h
 * Purpose: Declares the error reporting of ISO/IEC 9899:2011, Section 7.5 — the
 *          modifiable lvalue `errno` and the numbers a library function may set
 *          it to — for a system whose only present source of failure is a system
 *          call.
 * Key definitions: errno, OxysErrnoAddress, ENOSYS, EFAULT, EINVAL, EBADF,
 *          ECHILD, ENOENT, ENOMEM, EEXIST, ENOTDIR, EISDIR, ENOTEMPTY, EROFS,
 *          ENAMETOOLONG, ELOOP, ENOSPC, EMFILE, EBUSY, EXDEV, ENOTSUP, EIO,
 *          EPIPE, EINTR, ESRCH,
 *          EDOM, EILSEQ, ERANGE,
 *          OXYS_ERRNO_SYSCALL_LIMIT.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.5, paragraph 2: <errno.h> defines EDOM,
 *     EILSEQ and ERANGE, which expand to integer constant expressions of type
 *     int with distinct positive values and are suitable for use in #if
 *     directives; and errno, which expands to a modifiable lvalue of type int
 *     and thread local storage duration.
 *   - ISO/IEC 9899:2011, Section 7.5, footnote 201: the macro errno need not be
 *     the identifier of an object and may expand to a modifiable lvalue
 *     resulting from a function call. That footnote is why this header is
 *     written as it is; see below.
 *   - ISO/IEC 9899:2011, Section 7.5, paragraph 3: errno is zero at program
 *     startup and is never set to zero by any library function.
 *   - ISO/IEC 9899:2011, Section 7.5, paragraph 4: an implementation may define
 *     further macros beginning with E and an uppercase letter, which is what
 *     permits the twenty below that ISO C does not name.
 *   - kernel/abi/oxys/syscall_abi.h: the failure results these numbers are the
 *     names of, included below so that the correspondence is checked by the
 *     compiler rather than by a reader.
 *   - docs/design/LIBC.md, Section 8: the design of this header and of the
 *     translation that is the only thing in this library that writes errno.
 *
 * Why errno is a function call and not a variable.
 *
 * Section 7.5 requires errno to have thread local storage duration. This system
 * has no userland threads, so the object below is one object and serves the one
 * thread there is — but which object it is must not be visible to a caller,
 * because the moment a program has two threads every one of them must be given
 * its own and no program that already reads errno may need recompiling to say
 * so. Footnote 201 exists for exactly this, and a library that exported a plain
 * `extern int errno` would have published the wrong thing: callers would resolve
 * one symbol at link time, and the change to per-thread storage would be a
 * change to the interface rather than to the implementation.
 *
 * Why the numbers are these numbers.
 *
 * They are the failure results of <oxys/syscall_abi.h> negated, so that the
 * translation in libc/syscall/result.c is an arithmetic derivation and not a
 * table. A table is a second list that must agree with a first, and the way it
 * fails is that somebody adds a failure result and forgets the other half —
 * whereupon a program reports the wrong cause and nothing faults. The static
 * assertions below are what make the derivation checked: renumber a result in
 * the kernel's interface and this header fails to compile, which is the only
 * report of that class of defect that cannot be missed.
 *
 * Numbers 1 to OXYS_ERRNO_SYSCALL_LIMIT are reserved to that derivation, and the
 * three ISO C requires stand above it. They are held apart deliberately: if
 * EDOM were 8 then the eighth failure result this kernel acquires would arrive
 * in a program's errno wearing the name of a domain error.
 */

#ifndef OXYS_LIBC_ERRNO_H
#define OXYS_LIBC_ERRNO_H

#include <oxys/syscall_abi.h>

/*
 * The address of the calling thread's errno.
 *
 * It is never null. A program has no reason to call this and should write
 * `errno`; it is declared because the macro below expands to a call of it, and
 * a macro that expanded to a call of something undeclared would be a macro that
 * only worked where the definition happened to be visible.
 */
int *OxysErrnoAddress(void);

/*
 * ISO/IEC 9899:2011, Section 7.5, paragraph 2. A program that defines an
 * identifier with this name, or suppresses this definition to reach an object,
 * has undefined behaviour — which is the standard's way of saying that the
 * storage is the implementation's business and not the program's.
 */
#define errno (*OxysErrnoAddress())

/*
 * The greatest number reserved to the derivation from the kernel's failure
 * results.
 *
 * Nothing in this system produces a value near it. It is thirty-one so that the
 * reservation is plainly a range rather than a coincidence of the twenty results
 * that exist today, and so that the translation has a bound to refuse beyond —
 * a kernel that returned a failure this library has no name for must not be
 * allowed to put an arbitrary integer into a program's errno.
 */
#define OXYS_ERRNO_SYSCALL_LIMIT 31

/*
 * The failure results of <oxys/syscall_abi.h>, negated.
 *
 * Each is the name of exactly one result and of nothing else. They are not
 * POSIX's numbers and do not claim to be: the interface header records that the
 * results are this kernel's own, and a library that renumbered them here to
 * agree with a standard it does not implement would be asserting a
 * compatibility nobody has tested.
 */
#define ENOSYS 1 /* No such call. */
#define EFAULT 2 /* An address the caller may not use. */
#define EINVAL 3 /* An argument that cannot be right. */
#define EBADF  4 /* No such descriptor. */
#define ECHILD 5 /* The caller has no children to wait for. */
#define ENOENT 6 /* No such file, or one that will not load. */
#define ENOMEM 7 /* A frame, a table or a slot could not be had. */

/* The thirteen of sub-task 7.6, which are the filesystem layer's refusals
 * carried out to a program. They are derived exactly as the seven above are, and
 * are asserted below by the same means. */
#define EEXIST       8  /* A file of that name already. */
#define ENOTDIR      9  /* A component of the path is not a directory. */
#define EISDIR       10 /* A directory where a file was required. */
#define ENOTEMPTY    11 /* A directory holding more than "." and "..". */
#define EROFS        12 /* The mount, or the volume, may not be written. */
#define ENAMETOOLONG 13 /* A path or a component beyond the bounds. */
#define ELOOP        14 /* Symbolic links followed beyond the depth bound. */
#define ENOSPC       15 /* The volume has no room. */
#define EMFILE       16 /* Every descriptor is in use. */
#define EBUSY        17 /* Something held that the operation would destroy. */
#define EXDEV        18 /* An operation confined to one volume was not. */
#define ENOTSUP      19 /* The filesystem does not offer the operation. */
#define EIO          20 /* The volume or the device beneath it failed. */

/* The one of sub-task 8.6, the pipe's refusal: written, and held open for
 * reading by nobody. IEEE Std 1003.1-2017 sends SIGPIPE beside it, which
 * arrives with the signals of 8.7. */
#define EPIPE        21 /* The pipe is open for reading by nobody. */

/* The two of sub-task 8.7. */
#define EINTR        22 /* A signal arrived while the call slept. */
#define ESRCH        23 /* No such process or process group. */

/* The three ISO/IEC 9899:2011, Section 7.5, paragraph 2, requires, above the
 * reserved range for the reason given at the head of this file. Nothing in this
 * system sets any of them yet: there is no mathematical library to report a
 * domain or range error and no multibyte conversion to report an invalid
 * sequence. They are defined because the standard requires the macros to exist,
 * not because something writes them. */
#define EDOM   32
#define EILSEQ 33
#define ERANGE 34

/*
 * The correspondence, checked by the compiler.
 *
 * Each of these fails to compile if a failure result is renumbered in
 * <oxys/syscall_abi.h> without this header following it. That is the whole
 * purpose of including that header here, and the alternative — a comment saying
 * the two agree — is what every such pair of lists has instead of this.
 */
_Static_assert(ENOSYS == -SYSCALL_ENOSYS, "ENOSYS does not name SYSCALL_ENOSYS.");
_Static_assert(EFAULT == -SYSCALL_EFAULT, "EFAULT does not name SYSCALL_EFAULT.");
_Static_assert(EINVAL == -SYSCALL_EINVAL, "EINVAL does not name SYSCALL_EINVAL.");
_Static_assert(EBADF == -SYSCALL_EBADF, "EBADF does not name SYSCALL_EBADF.");
_Static_assert(ECHILD == -SYSCALL_ECHILD, "ECHILD does not name SYSCALL_ECHILD.");
_Static_assert(ENOENT == -SYSCALL_ENOENT, "ENOENT does not name SYSCALL_ENOENT.");
_Static_assert(ENOMEM == -SYSCALL_ENOMEM, "ENOMEM does not name SYSCALL_ENOMEM.");
_Static_assert(EEXIST == -SYSCALL_EEXIST, "EEXIST does not name SYSCALL_EEXIST.");
_Static_assert(ENOTDIR == -SYSCALL_ENOTDIR, "ENOTDIR does not name SYSCALL_ENOTDIR.");
_Static_assert(EISDIR == -SYSCALL_EISDIR, "EISDIR does not name SYSCALL_EISDIR.");
_Static_assert(ENOTEMPTY == -SYSCALL_ENOTEMPTY,
               "ENOTEMPTY does not name SYSCALL_ENOTEMPTY.");
_Static_assert(EROFS == -SYSCALL_EROFS, "EROFS does not name SYSCALL_EROFS.");
_Static_assert(ENAMETOOLONG == -SYSCALL_ENAMETOOLONG,
               "ENAMETOOLONG does not name SYSCALL_ENAMETOOLONG.");
_Static_assert(ELOOP == -SYSCALL_ELOOP, "ELOOP does not name SYSCALL_ELOOP.");
_Static_assert(ENOSPC == -SYSCALL_ENOSPC, "ENOSPC does not name SYSCALL_ENOSPC.");
_Static_assert(EMFILE == -SYSCALL_EMFILE, "EMFILE does not name SYSCALL_EMFILE.");
_Static_assert(EBUSY == -SYSCALL_EBUSY, "EBUSY does not name SYSCALL_EBUSY.");
_Static_assert(EXDEV == -SYSCALL_EXDEV, "EXDEV does not name SYSCALL_EXDEV.");
_Static_assert(ENOTSUP == -SYSCALL_ENOTSUP, "ENOTSUP does not name SYSCALL_ENOTSUP.");
_Static_assert(EIO == -SYSCALL_EIO, "EIO does not name SYSCALL_EIO.");
_Static_assert(EPIPE == -SYSCALL_EPIPE, "EPIPE does not name SYSCALL_EPIPE.");
_Static_assert(EINTR == -SYSCALL_EINTR, "EINTR does not name SYSCALL_EINTR.");
_Static_assert(ESRCH == -SYSCALL_ESRCH, "ESRCH does not name SYSCALL_ESRCH.");

/* And that the reservation still holds. A failure result more negative than the
 * limit would be translated to ENOSYS rather than to its own name, silently. */
_Static_assert(-SYSCALL_ESRCH <= OXYS_ERRNO_SYSCALL_LIMIT,
               "A failure result lies beyond the range reserved for one.");
_Static_assert(EDOM > OXYS_ERRNO_SYSCALL_LIMIT,
               "The numbers ISO C requires overlap the kernel's failure results.");

#endif /* OXYS_LIBC_ERRNO_H */
