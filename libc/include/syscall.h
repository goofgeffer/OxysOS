/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/syscall.h
 * Purpose: Declares the C library's system-call wrappers — one for each of the
 *          twenty-seven calls <oxys/syscall_abi.h> numbers — together with the raw
 *          invocation they are built upon and the translation that turns a
 *          kernel result into a library result and an errno.
 * Key definitions: OxysSyscallInvoke0, OxysSyscallInvoke1, OxysSyscallInvoke2,
 *          OxysSyscallInvoke3, OxysSyscallResult, OxysOpen, OxysClose,
 *          OxysRead, OxysReadDirectory, OxysMakeDirectory, OxysUnlink,
 *          OxysChangeDirectory, OxysGetWorkingDirectory, OxysDuplicate,
 *          OxysRemoveDirectory, OxysPipe, OxysWaitFor, OxysKill,
 *          OxysSignalAction, OxysGetProcessId, OxysGetProcessGroup,
 *          OxysSetProcessGroup, OxysTerminalGroup, OxysSignalRestorer,
 *          OxysWrite, OxysTicks,
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
 * Most of these calls have a POSIX name that means very nearly this — write,
 * fork, execve, _exit, wait, open, close, read, mkdir, unlink — and not one of
 * them means exactly it. This wait takes no process identifier and no option
 * flags; this write reaches two diagnostic descriptors and no file; this open
 * cannot create a file; this mkdir applies no file mode creation mask; this
 * readdir is not `readdir()` at all, taking a descriptor rather than a `DIR *`
 * and returning three results rather than a pointer. A function
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
 * for a range this program may not read. **A descriptor this program opened is
 * refused too**, since sub-task 7.6 as before it: `OxysOpen` opens for reading
 * alone, so there is no descriptor a write could sensibly reach.
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
 * **Since sub-task 7.6 both vectors are accepted.** They were refused until then
 * for want of a convention about where a program finds them; the convention is
 * now the System V ABI's own — the strings at the top of the new stack, the
 * pointers below them, the argument count at the stack pointer — and `_start`
 * reads it. Either may be null, which is an empty vector.
 *
 * The kernel copies every string out of this program's memory before it destroys
 * the address space they stand in, and refuses with EINVAL where either vector
 * holds more than SYSCALL_ARGUMENT_COUNT_MAXIMUM strings or the two together
 * more than SYSCALL_ARGUMENT_BYTES_MAXIMUM bytes. Both refusals arrive before
 * anything is destroyed, so a program refused here carries on running.
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

/* -------------------------------------------------------------------------
 * The six calls of sub-task 7.6, by which a program reaches the filesystem.
 * ------------------------------------------------------------------------- */

/*
 * Opens a file and returns a descriptor of this program's own.
 *
 * `flags` must include SYSCALL_OPEN_READ or SYSCALL_OPEN_WRITE and may include
 * SYSCALL_OPEN_DIRECTORY, which refuses anything that is not one, and — since
 * sub-task 8.5, when the shell's redirection became the first thing that
 * needed to write a file — SYSCALL_OPEN_CREATE, SYSCALL_OPEN_TRUNCATE and
 * SYSCALL_OPEN_APPEND, each of which needs WRITE beside it. No other bit is
 * accepted: a program that asks for a flag this interface does not name is
 * refused with EINVAL rather than quietly given something else. `permissions`
 * is the mode a created file is given, 0644 being the usual; it is recorded
 * and not enforced, this system checking no permission bit.
 *
 * **A directory is opened by this and read by OxysReadDirectory, not by
 * OxysRead.** A read of a descriptor naming a directory fails with EISDIR, which
 * is what lets `cat` report the right thing about one.
 *
 * Returns -1 with errno set to ENOENT for a path that leads nowhere, ENOTDIR
 * where a component of it is not a directory or where DIRECTORY was asked and
 * the path names something else, EMFILE where this program or the machine has no
 * descriptor left, ENAMETOOLONG beyond SYSCALL_PATH_MAXIMUM, and EFAULT for a
 * path this program may not read.
 */
int64_t OxysOpen(const char *path, uint64_t flags, uint16_t permissions);

/*
 * Releases a descriptor and the open file beneath it.
 *
 * Returns -1 with errno set to EBADF where the descriptor names nothing this
 * program holds — which includes a descriptor already closed. That is not
 * pedantry: a program that closes twice has lost track of what it holds, and the
 * number may since have been given to a different file.
 */
int64_t OxysClose(int descriptor);

/*
 * Reads up to `length` bytes from a descriptor and returns how many were read,
 * which is zero at the end of the file.
 *
 * Fewer bytes than were asked for is not a failure and is the ordinary case: the
 * kernel bounds a single transfer, so a program that means to read a whole file
 * must call again until this returns zero.
 *
 * A length of zero is refused with EINVAL rather than answered with zero,
 * because zero is what the end of a file returns and the two must be
 * distinguishable.
 *
 * Returns -1 with errno set to EBADF for a descriptor this program does not
 * hold, EISDIR for one naming a directory, and EFAULT for a buffer this program
 * may not write.
 */
int64_t OxysRead(int descriptor, void *buffer, size_t length);

/*
 * Reads one entry of an open directory.
 *
 * Returns 1 where an entry was placed in `entry`, 0 at the end of the directory,
 * and -1 with errno set otherwise — ENOTDIR for a descriptor that does not name
 * a directory, EBADF for one this program does not hold, EFAULT for an entry
 * this program may not write.
 *
 * Three results and not two, because "no more entries" and "something went
 * wrong" are different things and a program that treated them alike would stop
 * listing a directory upon a medium failure and report that it had finished.
 *
 * Every entry is returned, including "." and "..". Filtering is the caller's:
 * IEEE Std 1003.1-2017 has `ls` hide names beginning with a period unless -a is
 * given, and a kernel that hid them could not be asked for them.
 */
int64_t OxysReadDirectory(int descriptor, SyscallDirectoryEntry *entry);

/*
 * Creates a directory with the given permission bits.
 *
 * No file mode creation mask is applied, this system having none: the bits asked
 * for are the bits the directory gets. IEEE Std 1003.1-2017 has `mkdir()` reduce
 * the mode by the process's mask, and the mask belongs with credentials this
 * kernel does not yet have.
 *
 * Returns -1 with errno set to EEXIST where something of that name is already
 * there, ENOENT where the parent is not, ENOTDIR where a component of the path
 * is not a directory, EROFS for a volume that may not be written, and ENOSPC
 * where it has no room.
 */
int64_t OxysMakeDirectory(const char *path, uint16_t permissions);

/*
 * Removes a name.
 *
 * Returns -1 with errno set to EISDIR where the path names a directory: there is
 * no call in this interface that removes one, and a directory is refused rather
 * than removed by some other means. ENOENT where there is nothing of that name,
 * EROFS for a volume that may not be written.
 */
int64_t OxysUnlink(const char *path);

/*
 * Changes the working directory, of sub-task 8.3 — the directory every
 * relative path this program names is resolved against, by the kernel, in
 * every call that takes a path.
 *
 * Returns -1 with errno set to ENOENT where the path names nothing, ENOTDIR
 * where it names something that is not a directory, and ENAMETOOLONG where
 * the path, made absolute, exceeds SYSCALL_PATH_MAXIMUM.
 */
int64_t OxysChangeDirectory(const char *path);

/*
 * Copies the working directory, terminated, into `buffer`, and returns its
 * length excluding the terminator.
 *
 * Returns -1 with errno set to ERANGE where the buffer cannot hold it. The
 * kernel reports that as ENAMETOOLONG, having no ERANGE among its results, and
 * this is the one wrapper that translates a result rather than passing it
 * through: IEEE Std 1003.1-2017 names ERANGE for exactly this, and a program
 * written to the standard should be told what the standard says.
 */
int64_t OxysGetWorkingDirectory(char *buffer, size_t capacity);

/*
 * Makes `to` name what `from` names, of sub-task 8.5 — `dup2` of IEEE Std
 * 1003.1-2017. Whatever `to` named is closed; the two then share one open file
 * and one position until both are closed. Returns -1 with errno set to EBADF
 * where `from` names nothing, or where a number below SYSCALL_DESCRIPTOR_FIRST
 * that names the terminal or the diagnostic path is asked to become a number
 * above them, the kernel's paths not being files.
 */
int64_t OxysDuplicate(int from, int to);

/*
 * Removes an empty directory, of sub-task 8.5.
 *
 * Returns -1 with errno set to ENOENT where there is nothing of that name,
 * ENOTDIR where it is not a directory, ENOTEMPTY where it holds more than `.`
 * and `..`, EBUSY where it is a mount point or the working directory of the
 * filesystem layer's own resolution, and EROFS for a volume that may not be
 * written.
 */
int64_t OxysRemoveDirectory(const char *path);

/*
 * Makes a pipe, of sub-task 8.6 — `pipe()` of IEEE Std 1003.1-2017: the read
 * end at `descriptors[0]` and the write end at `descriptors[1]`, both new
 * numbers above the three. A read of an empty pipe waits for a write or for
 * the last write end to close, upon which it returns 0; a write to a full pipe
 * waits for a read; a write to a pipe held open for reading by nobody fails
 * with EPIPE. Returns -1 with errno set to EMFILE where every pipe, every open
 * file or every descriptor of the caller's is in use, and EFAULT where the
 * array may not be written.
 */
int64_t OxysPipe(int descriptors[2]);

/*
 * The seven of sub-task 8.7.
 *
 * OxysWaitFor is `waitpid`: `pid` names one child, -1 any, and a number below
 * -1 any child of the group its negation names; `options` may hold
 * SYSCALL_WAIT_NO_HANG and SYSCALL_WAIT_UNTRACED; the status is in the
 * encoding of <oxys/syscall_abi.h>, read with SYSCALL_STATUS_KIND and
 * SYSCALL_STATUS_NUMBER. Returns the child's identifier, 0 where nothing is to
 * be reported and the caller declined to wait, or -1 with errno ECHILD or
 * EINTR. OxysWait above is OxysWaitFor(-1, status, 0).
 *
 * OxysKill sends a signal — 0 to test for existence — to a process, or with a
 * negative `pid` to a group; -1 with ESRCH or EINVAL. OxysSignalAction sets a
 * signal's disposition and the restorer, and returns the previous disposition;
 * <signal.h>'s `signal` is the form a program should use. The three that
 * follow report the caller's identifier, a process's group and set one, and
 * OxysTerminalGroup reads the terminal's foreground group, or sets it where
 * `group` is not 0.
 */
int64_t OxysWaitFor(int64_t pid, int64_t *status, uint64_t options);
int64_t OxysKill(int64_t pid, int signal);
int64_t OxysSignalAction(int signal, uint64_t disposition, uint64_t restorer);
int64_t OxysGetProcessId(void);
int64_t OxysGetProcessGroup(int64_t pid);
int64_t OxysSetProcessGroup(int64_t pid, int64_t group);
int64_t OxysTerminalGroup(int64_t group);

/* The restorer, of libc/syscall/invoke.asm: what a handler returns into. */
void OxysSignalRestorer(void);

#endif /* OXYS_LIBC_SYSCALL_H */
