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
 *          SYSCALL_WAIT, SYSCALL_BRK, SYSCALL_OPEN, SYSCALL_CLOSE,
 *          SYSCALL_READ, SYSCALL_READDIR, SYSCALL_MKDIR, SYSCALL_UNLINK,
 *          SYSCALL_CHDIR, SYSCALL_GETCWD, SYSCALL_DUP2, SYSCALL_RMDIR, SYSCALL_PIPE,
 *          SYSCALL_WAITPID, SYSCALL_KILL, SYSCALL_SIGACTION, SYSCALL_SIGRETURN,
 *          SYSCALL_GETPID, SYSCALL_GETPGID, SYSCALL_SETPGID, SYSCALL_TCGROUP,
 *          SYSCALL_LINK, SYSCALL_PROCINFO, SyscallProcessInformation,
 *          SYSCALL_WAIT_NO_HANG, SYSCALL_WAIT_UNTRACED, SYSCALL_STATUS_MAKE,
 *          SYSCALL_STATUS_KIND, SYSCALL_STATUS_NUMBER, the SYSCALL_SIG numbers,
 *          SYSCALL_SIGNAL_DEFAULT, SYSCALL_SIGNAL_IGNORE, SYSCALL_EINTR, SYSCALL_ESRCH,
 *          SYSCALL_OPEN_WRITE, SYSCALL_OPEN_CREATE, SYSCALL_OPEN_TRUNCATE,
 *          SYSCALL_OPEN_APPEND,
 *          SYSCALL_COUNT, SYSCALL_OK, SYSCALL_ENOSYS, SYSCALL_EFAULT,
 *          SYSCALL_EINVAL, SYSCALL_EBADF, SYSCALL_ECHILD, SYSCALL_ENOENT,
 *          SYSCALL_ENOMEM, SYSCALL_EEXIST, SYSCALL_ENOTDIR, SYSCALL_EISDIR,
 *          SYSCALL_ENOTEMPTY, SYSCALL_EROFS, SYSCALL_ENAMETOOLONG,
 *          SYSCALL_ELOOP, SYSCALL_ENOSPC, SYSCALL_EMFILE, SYSCALL_EBUSY,
 *          SYSCALL_EXDEV, SYSCALL_ENOTSUP, SYSCALL_EIO, SYSCALL_EPIPE,
 *          SYSCALL_PATH_MAXIMUM,
 *          SYSCALL_USER_LIMIT, SYSCALL_BREAK_QUERY, SYSCALL_OPEN_READ,
 *          SYSCALL_OPEN_DIRECTORY, SYSCALL_NAME_MAXIMUM, SyscallEntryType,
 *          SyscallDirectoryEntry, SYSCALL_DESCRIPTOR_INPUT,
 *          SYSCALL_DESCRIPTOR_OUTPUT, SYSCALL_DESCRIPTOR_ERROR,
 *          SYSCALL_DESCRIPTOR_FIRST, SYSCALL_ARGUMENT_COUNT_MAXIMUM,
 *          SYSCALL_ARGUMENT_BYTES_MAXIMUM.
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
 * Why this file is separate from <oxys/arch/syscall/syscall.h>, and why it is MIT.
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
 * one. The implementation remains in <oxys/arch/syscall/syscall.h>, which includes this file
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

/*
 * The call of sub-task 7.3, by which a program asks for memory.
 *
 * It moves the *break* — the address one past the last byte of the region a
 * program may use for a heap — and returns where the break stands afterwards.
 * An argument of SYSCALL_BREAK_QUERY moves nothing and reports where it stands
 * now, which is how a program discovers its heap before it has grown one.
 *
 * It is numbered eighth for the reason the four above are numbered after the
 * three that preceded them: a number already handed to a program is a number
 * that must not change.
 *
 * **This call returns the new break and not the old one, and a failure is a
 * negative result.** Kernels of this lineage traditionally return the break as
 * it stands whether or not the request succeeded, which obliges every caller to
 * compare the result against what it asked for and to make a second call to find
 * out what it has. A caller that omits the comparison — and the comparison is
 * easy to omit, the result being a plausible address either way — believes it
 * owns memory that was never mapped, and discovers otherwise at some later
 * instruction that touches it. Every other call of this kernel reports a failure
 * as a negative result, and this one does too.
 */
#define SYSCALL_BRK     7U

/*
 * The six calls of sub-task 7.6, by which a program reaches the filesystem.
 *
 * They are numbered ninth to fourteenth, after the eight that existed, for the
 * reason recorded above: a number already handed to a program is a number that
 * must not change.
 *
 * **There is no call here that creates or writes a file.** `open` accepts the
 * read flags below and nothing else, and `write` still reaches the two
 * diagnostic descriptors alone. The utilities of this sub-task read, list and
 * remove; the first thing that needs to write to a file is the shell's output
 * redirection at sub-task 8.5, and a call whose only caller is a future one is
 * a call nothing asserts. docs/design/LIBC.md, Section 12.7, limitation 2.
 */
#define SYSCALL_OPEN    8U
#define SYSCALL_CLOSE   9U
#define SYSCALL_READ    10U
#define SYSCALL_READDIR 11U
#define SYSCALL_MKDIR   12U
#define SYSCALL_UNLINK  13U

/*
 * The two calls of sub-task 8.3, by which a program changes and asks its
 * working directory. Numbered fifteenth and sixteenth, after the fourteen,
 * for the reason recorded above.
 *
 * **Every relative path every call accepts is resolved against the working
 * directory from this sub-task**, and not only these two: `open`, `mkdir`,
 * `unlink`, `execve` and `chdir` itself. It is done in the one place a path
 * is copied out of a program's memory, so that no call can forget. A path
 * that is relative and, joined to the working directory, exceeds
 * SYSCALL_PATH_MAXIMUM is refused as ENAMETOOLONG, the bound being upon the
 * absolute path the kernel resolves and not upon what the program typed.
 *
 * `getcwd` copies the directory, terminated, into a buffer of the length
 * given, and returns the length copied excluding the terminator; a buffer too
 * small is ENAMETOOLONG, which is the nearest of this kernel's results to the
 * ERANGE IEEE Std 1003.1-2017 names, and the C library's wrapper reports it
 * as ERANGE.
 */
#define SYSCALL_CHDIR   14U
#define SYSCALL_GETCWD  15U

/*
 * The two calls of sub-task 8.5. `dup2` makes one descriptor name what another
 * names — IEEE Std 1003.1-2017's — which is how a redirection reaches a
 * program: the shell opens the file in the child and places it at 0 or 1
 * before `execve`. `rmdir` removes an empty directory, which nothing could do
 * since 7.6. Numbered seventeenth and eighteenth for the reason recorded above.
 */
#define SYSCALL_DUP2    16U
#define SYSCALL_RMDIR   17U

/*
 * The one call of sub-task 8.6. `pipe` makes a bounded byte queue with an end
 * that reads and an end that writes — IEEE Std 1003.1-2017's `pipe()`, the
 * read end at `descriptors[0]` and the write end at `descriptors[1]` — and
 * is what the shell's `|` is made of: the writer's 1 and the reader's 0 are
 * the two ends, placed by `dup2` in each child before `execve`. Numbered
 * nineteenth for the reason recorded above.
 *
 * A read of an empty pipe sleeps until a writer writes or the last writer
 * closes, upon which it reports zero; a write to a full pipe sleeps until a
 * reader reads; and a write to a pipe held open for reading by nobody is
 * EPIPE. Every write a program can make — SYSCALL_TRANSFER_MAXIMUM bytes at
 * most — is delivered in one piece.
 */
#define SYSCALL_PIPE    18U

/*
 * The eight calls of sub-task 8.7, by which a program governs the others and
 * is told what became of them: job control, process groups and the signals
 * the terminal delivers. Numbered twentieth to twenty-seventh for the reason
 * recorded above.
 *
 * `waitpid` is `wait` with a choice: `pid` names one child, -1 any, and a
 * number below -1 any child of the process group whose identifier is its
 * negation; `options` may hold SYSCALL_WAIT_NO_HANG, upon which a call that
 * would sleep returns 0 instead, and SYSCALL_WAIT_UNTRACED, upon which a child
 * that has stopped is reported — once per stop — as an ended one would be. The
 * status is the encoding below, and no longer the number `exit` was given.
 *
 * `kill` sends a signal to a process, or with a negative `pid` to every process
 * of a group; a signal of 0 sends nothing and reports whether the target
 * exists. `sigaction` sets a signal's disposition — SYSCALL_SIGNAL_DEFAULT,
 * SYSCALL_SIGNAL_IGNORE or the address of a handler — together with the
 * restorer the handler returns through, and reports the old disposition; a
 * handler is entered with the signal's number as its argument, upon the
 * program's own stack below what it was using, and returns into the restorer,
 * which calls `sigreturn` to put the interrupted context back. SIGKILL and
 * SIGSTOP may be neither caught nor ignored.
 *
 * `getpid` and `getpgid` report identifiers, the latter of the process named
 * or of the caller for 0; `setpgid` puts a process (0 for the caller) into a
 * group (0 for a group of its own identifier); and `tcgroup` reads the
 * terminal's foreground process group, or sets it where `group` is not 0 — the
 * group control-C and control-Z are delivered to, and the only group whose
 * members may read the terminal without being stopped by SIGTTIN.
 */
#define SYSCALL_WAITPID   19U
#define SYSCALL_KILL      20U
#define SYSCALL_SIGACTION 21U
#define SYSCALL_SIGRETURN 22U
#define SYSCALL_GETPID    23U
#define SYSCALL_GETPGID   24U
#define SYSCALL_SETPGID   25U
#define SYSCALL_TCGROUP   26U

/*
 * The two calls added on 2026-09-16, beside sub-task 8.7, for two utilities a
 * person asked for. `link` makes a second name for a file upon the same volume
 * — IEEE Std 1003.1-2017's `link()` — which with `unlink` is how `mv` renames
 * within a volume; a second name upon another volume is EXDEV. `procinfo`
 * fills a SyscallProcessInformation for the process at a table index, so that
 * `ps` can walk the table: it returns 1 where the slot holds a process, 0
 * where it is empty, and EINVAL beyond the table — the index being a slot and
 * not an identifier, so that a walk from 0 upward sees every process once.
 */
#define SYSCALL_LINK      27U
#define SYSCALL_PROCINFO  28U
#define SYSCALL_COUNT     29U

/* What `procinfo` reports of one process. The state is one of
 * SYSCALL_PROCESS_STATE_*, and the name is what the process was created as —
 * the program's name where `execve` replaced it. */
#define SYSCALL_PROCESS_NAME_MAXIMUM 31U
#define SYSCALL_PROCESS_CAPACITY     64U

#define SYSCALL_PROCESS_STATE_READY   1U
#define SYSCALL_PROCESS_STATE_RUNNING 2U
#define SYSCALL_PROCESS_STATE_BLOCKED 3U
#define SYSCALL_PROCESS_STATE_STOPPED 4U
#define SYSCALL_PROCESS_STATE_EXITED  5U

typedef struct SyscallProcessInformation
{
    uint64_t id;
    uint64_t parent;
    uint64_t group;
    uint64_t state;
    uint64_t pages;   /* Pages mapped for the program: image, stack and heap. */
    char name[SYSCALL_PROCESS_NAME_MAXIMUM + 1U];
} SyscallProcessInformation;

/* The options of `waitpid`. */
#define SYSCALL_WAIT_NO_HANG  UINT64_C(0x1)
#define SYSCALL_WAIT_UNTRACED UINT64_C(0x2)

/*
 * What `wait` and `waitpid` report, since sub-task 8.7: a kind in bits 8 to 15
 * and a number in bits 0 to 7 — the code a program gave `exit`, reduced to
 * eight bits, or the signal that ended or stopped it. Until 8.7 the status was
 * the quadword `exit` was given, or the negated vector of a fault, which could
 * not say that a program had been stopped and could not tell a signal from a
 * vector; docs/design/PROCESS.md, Section 19, limitation 11, had recorded that
 * the encoding was owed and belonged with the C library that must agree with it.
 * A fault is reported as the signal it corresponds to — SIGSEGV for a page or
 * protection fault, SIGILL for an invalid opcode, SIGFPE for a divide error,
 * SIGBUS for an alignment check, SIGTRAP for a breakpoint or a debug exception.
 */
#define SYSCALL_STATUS_KIND_EXITED    UINT64_C(0)
#define SYSCALL_STATUS_KIND_SIGNALLED UINT64_C(1)
#define SYSCALL_STATUS_KIND_STOPPED   UINT64_C(2)

#define SYSCALL_STATUS_MAKE(kind, number) (((kind) << 8) | ((uint64_t)(number) & 0xFFU))
#define SYSCALL_STATUS_KIND(status)       ((((uint64_t)(status)) >> 8) & 0xFFU)
#define SYSCALL_STATUS_NUMBER(status)     (((uint64_t)(status)) & 0xFFU)

/*
 * The signals, of sub-task 8.7, numbered as the x86 System V and Linux
 * conventions number them so that a person who knows `kill -9` finds it here.
 * IEEE Std 1003.1-2017 fixes the names and the default actions and leaves the
 * numbers to the implementation.
 */
#define SYSCALL_SIGHUP    1U  /* Terminates. */
#define SYSCALL_SIGINT    2U  /* Terminates; control-C at the terminal. */
#define SYSCALL_SIGQUIT   3U  /* Terminates. */
#define SYSCALL_SIGILL    4U  /* Terminates; an invalid opcode. */
#define SYSCALL_SIGTRAP   5U  /* Terminates; a breakpoint or debug exception. */
#define SYSCALL_SIGABRT   6U  /* Terminates. */
#define SYSCALL_SIGBUS    7U  /* Terminates; an alignment check. */
#define SYSCALL_SIGFPE    8U  /* Terminates; a divide error. */
#define SYSCALL_SIGKILL   9U  /* Terminates, and cannot be caught or ignored. */
#define SYSCALL_SIGUSR1   10U /* Terminates. */
#define SYSCALL_SIGSEGV   11U /* Terminates; a page or general protection fault. */
#define SYSCALL_SIGUSR2   12U /* Terminates. */
#define SYSCALL_SIGPIPE   13U /* Terminates; a write to a pipe with no reader. */
#define SYSCALL_SIGALRM   14U /* Terminates. */
#define SYSCALL_SIGTERM   15U /* Terminates. */
#define SYSCALL_SIGCHLD   17U /* Ignored; a child ended or stopped. */
#define SYSCALL_SIGCONT   18U /* Continues a stopped process; otherwise ignored. */
#define SYSCALL_SIGSTOP   19U /* Stops, and cannot be caught or ignored. */
#define SYSCALL_SIGTSTP   20U /* Stops; control-Z at the terminal. */
#define SYSCALL_SIGTTIN   21U /* Stops; a background read of the terminal. */
#define SYSCALL_SIGTTOU   22U /* Stops; reserved, nothing sends it yet. */
#define SYSCALL_SIGNAL_MAXIMUM 31U

/* The two dispositions that are not a handler's address. */
#define SYSCALL_SIGNAL_DEFAULT UINT64_C(0)
#define SYSCALL_SIGNAL_IGNORE  UINT64_C(1)

/*
 * The argument that asks where the break stands rather than moving it.
 *
 * Zero is not an address a program could ever want its break at — the lowest
 * page of every address space is deliberately unmapped, so that a null pointer
 * dereference faults — which is what makes it free to carry a second meaning
 * without an argument of its own to say which meaning was intended.
 */
#define SYSCALL_BREAK_QUERY UINT64_C(0)

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
 * The thirteen of sub-task 7.6, which are the refusals the filesystem layer
 * makes, carried out to a program one for one.
 *
 * They are not reduced to the seven above, and that is the whole reason there
 * are thirteen of them, a fourteenth at 8.6 and a fifteenth at 8.7. `VfsError` distinguishes
 * seventeen causes; a kernel that
 * collapsed them into `ENOENT` and `EINVAL` would tell a program that a
 * directory it could not remove was not there, and a person reading that
 * diagnostic would look for the file rather than for the entries still within
 * it. The correspondence is one to one and is written in one place —
 * `SyscallFromVfsError` in kernel/arch/x86_64/syscall/syscall.c — so that a
 * cause added to that enumeration has exactly one place to be forgotten, and
 * the switch there has no default.
 */
#define SYSCALL_EEXIST         INT64_C(-8)  /* A file of that name already. */
#define SYSCALL_ENOTDIR        INT64_C(-9)  /* A component of the path is not a directory. */
#define SYSCALL_EISDIR         INT64_C(-10) /* A directory where a file was required. */
#define SYSCALL_ENOTEMPTY      INT64_C(-11) /* A directory holding more than "." and "..". */
#define SYSCALL_EROFS          INT64_C(-12) /* The mount, or the volume, may not be written. */
#define SYSCALL_ENAMETOOLONG   INT64_C(-13) /* A path or a component beyond the bounds. */
#define SYSCALL_ELOOP          INT64_C(-14) /* Symbolic links followed beyond the depth bound. */
#define SYSCALL_ENOSPC         INT64_C(-15) /* The volume has no room. */
#define SYSCALL_EMFILE         INT64_C(-16) /* Every descriptor is in use. */
#define SYSCALL_EBUSY          INT64_C(-17) /* Something held that the operation would destroy. */
#define SYSCALL_EXDEV          INT64_C(-18) /* An operation confined to one volume was not. */
#define SYSCALL_ENOTSUP        INT64_C(-19) /* The filesystem does not offer the operation. */
#define SYSCALL_EIO            INT64_C(-20) /* The volume or the device beneath it failed. */

/* The one of sub-task 8.6: a pipe written that nobody holds open for reading.
 * IEEE Std 1003.1-2017 sends SIGPIPE as well, which arrives with the signals of
 * 8.7; until then the result is the whole of what a writer is told. */
#define SYSCALL_EPIPE          INT64_C(-21) /* The pipe is open for reading by nobody. */

/* The two of sub-task 8.7. A call that slept and was woken by a signal reports
 * EINTR rather than the thing it was waiting for, so that the signal is acted
 * upon before the program is told anything else; ESRCH is a process, or a
 * group, that does not exist. */
#define SYSCALL_EINTR          INT64_C(-22) /* A signal arrived while the call slept. */
#define SYSCALL_ESRCH          INT64_C(-23) /* No such process or process group. */

/*
 * How a program opens a file: six of the flags the filesystem layer offers,
 * two of them since sub-task 7.6 and four since 8.5, when the shell's
 * redirection became the first thing that needed to create or write a file.
 *
 * The kernel refuses any bit outside this set rather than masking it away. A
 * program that asked for a flag this kernel does not carry and was given a
 * file opened without it would discover the difference at some later call,
 * for a reason having nothing to do with that call.
 *
 * READ or WRITE must be given: an open that asked for neither could do
 * nothing. CREATE without WRITE, and TRUNCATE or APPEND without WRITE, are
 * refused as EINVAL, being requests to change a file without opening it for
 * change. The values are the filesystem layer's own, asserted equal to them
 * in kernel/arch/x86_64/syscall/syscall.c.
 */
#define SYSCALL_OPEN_READ      UINT64_C(0x0001)
#define SYSCALL_OPEN_WRITE     UINT64_C(0x0002) /* Since 8.5: writing, and the position advances. */
#define SYSCALL_OPEN_CREATE    UINT64_C(0x0004) /* Since 8.5: create the file if it is absent. */
#define SYSCALL_OPEN_TRUNCATE  UINT64_C(0x0010) /* Since 8.5: discard the contents upon opening. */
#define SYSCALL_OPEN_APPEND    UINT64_C(0x0020) /* Since 8.5: every write goes to the end. */
#define SYSCALL_OPEN_DIRECTORY UINT64_C(0x0040) /* Refuse anything but a directory. */

/*
 * The three descriptors every program begins with, and the first one an open
 * may be given.
 *
 * They are the numbers every system of this shape uses, and they are fixed here
 * rather than left to the C library because the kernel refuses a `write` to
 * anything else: the two halves must agree, and this file is where they do.
 *
 * **Descriptor 0 is the terminal, since sub-task 8.1.** A `read` of it delivers
 * the bytes typed at the keyboard or received upon the serial line, as they
 * were typed and without echo, and waits until there is at least one. It was
 * reserved and named before anything read it, so that the first call to do so
 * did not have to renumber the other two — and it did not.
 */
#define SYSCALL_DESCRIPTOR_INPUT  0
#define SYSCALL_DESCRIPTOR_OUTPUT 1
#define SYSCALL_DESCRIPTOR_ERROR  2
#define SYSCALL_DESCRIPTOR_FIRST  3

/*
 * The greatest length of one name within a directory, excluding its terminator.
 *
 * It is the filesystem layer's own bound, restated here because the structure
 * below carries a name and a program must be able to declare one of the right
 * size. The two are asserted to agree in kernel/arch/x86_64/syscall/syscall.c,
 * where both headers are visible; this file must not include the kernel's.
 */
#define SYSCALL_NAME_MAXIMUM 255U

/*
 * What kind of file a directory entry names.
 *
 * These are the filesystem layer's `VfsNodeType` values, renumbered onto nothing
 * — they are the same numbers — but restated here because a program may not
 * include the kernel's headers, and asserted equal to them where both are
 * visible. UNKNOWN is not an absence of information the caller may ignore: a
 * filesystem is entitled to declare no type in its directory entries, and a
 * program that needs the type of such a name must ask the file itself.
 */
typedef enum SyscallEntryType
{
    SYSCALL_TYPE_UNKNOWN = 0,
    SYSCALL_TYPE_REGULAR = 1,
    SYSCALL_TYPE_DIRECTORY = 2,
    SYSCALL_TYPE_SYMBOLIC_LINK = 3,
    SYSCALL_TYPE_CHARACTER_DEVICE = 4,
    SYSCALL_TYPE_BLOCK_DEVICE = 5,
    SYSCALL_TYPE_FIFO = 6,
    SYSCALL_TYPE_SOCKET = 7
} SyscallEntryType;

/*
 * One entry of a directory, as `readdir` hands it to a program.
 *
 * This is the one structure the kernel and a program must agree upon byte for
 * byte, and it is therefore here rather than in either's own headers — which is
 * exactly the reason this file exists. The note at the head of this file
 * forbids a *declaration*, because a declaration commits both sides to a symbol;
 * a definition of a shared layout commits them to nothing but the agreement
 * itself, which is what an interface header is for.
 *
 * The name is terminated here, which it is not upon an EXT2 volume. The `type`
 * field holds a `SyscallEntryType` and is a fixed-width integer rather than the
 * enumeration, because the width of an enumeration is the implementation's
 * choice and this structure crosses a privilege boundary between two
 * compilations.
 */
typedef struct SyscallDirectoryEntry
{
    uint64_t number; /* The filesystem's identifier for the file. */
    uint32_t type;   /* A SyscallEntryType. */
    uint32_t reserved; /* Zero. Present so the name begins at a fixed offset. */
    char name[SYSCALL_NAME_MAXIMUM + 1U];
} SyscallDirectoryEntry;

/*
 * What `execve` will accept of the two vectors the System V ABI puts upon a new
 * program's stack: how many strings, and how many bytes of them in total.
 *
 * Both bounds are the program's to know before it calls, for the reason
 * SYSCALL_PATH_MAXIMUM is. The kernel copies every string out of the caller's
 * memory *before* it destroys the address space they stand in, so the copy needs
 * a bound and the bound is the kernel's stack.
 *
 * The count bounds each vector separately and the byte count bounds the two
 * together, terminators included. A program that exceeds either is refused with
 * EINVAL and keeps running, which is what an `execve` that fails must do.
 */
#define SYSCALL_ARGUMENT_COUNT_MAXIMUM 16U
#define SYSCALL_ARGUMENT_BYTES_MAXIMUM 2048U

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
