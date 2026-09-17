/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/string/error.c
 * Purpose: Implements ISO/IEC 9899:2011, Section 7.24.6.2 — the mapping of an
 *          error number to a message — for the numbers <errno.h> defines and
 *          for every other value of type int besides.
 * Key functions: strerror.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.24.6.2, paragraph 2: strerror maps the number
 *     in errnum to a message string, and "shall map any value of type int to a
 *     message". A number this library has no name for is therefore not a case to
 *     refuse but a case to answer.
 *   - ISO/IEC 9899:2011, Section 7.24.6.2, paragraph 4: the array returned shall
 *     not be modified by the program and may be overwritten by a subsequent
 *     call. This implementation returns a distinct array for each number and
 *     overwrites nothing, which is permitted and is the stronger promise.
 *   - ISO/IEC 9899:2011, Section 7.5: the numbers, which are in <errno.h>.
 *   - docs/design/LIBC.md, Section 8.5: why this function arrives with the
 *     wrappers and not with the rest of Section 7.24.
 *
 * Why this function is here and not in the same change as the rest of 7.24.
 *
 * Sub-task 7.1 implemented nineteen of the twenty-two functions of Section 7.24
 * and deliberately left this one out, on the ground that a table mapping numbers
 * to messages could not be written before anything produced a number. It would
 * have fixed the spelling of every error message in the system before a single
 * call had a wrapper. The wrappers exist now, the numbers they set are in
 * <errno.h>, and the table is therefore written beside them.
 *
 * Why the messages are arrays rather than string literals.
 *
 * The standard declares strerror as returning char *, and this project compiles
 * with -Wwrite-strings, under which a string literal has type const char[] and
 * returning one as char * is a diagnostic. The choice is therefore between
 * casting away the qualifier at every return — which would be a lie told twenty
 * times — and giving each message an array of its own, which is what the
 * standard's wording contemplates in any case: it speaks of "the array pointed
 * to" and of a program that must not modify it.
 */

#include <errno.h>
#include <string.h>

/*
 * The messages for the numbers derived from the kernel's failure results,
 * indexed by the number itself. Element zero is not a failure and is present so
 * that strerror(0) — which a program reaches by passing an errno nothing has
 * set — answers something true rather than "Unknown error".
 *
 * Each message says what the kernel's interface header says the result means,
 * in the words used there, so that a message and the definition it describes can
 * be compared without the reader having to decide whether two phrasings are the
 * same thing.
 */
static char OxysErrorSyscallMessages[ESRCH + 1][48] = {
    "No error",                          /* 0 */
    "No such system call",               /* ENOSYS */
    "An address the program may not use", /* EFAULT */
    "An argument that cannot be right",  /* EINVAL */
    "No such descriptor",                /* EBADF */
    "No child to collect",               /* ECHILD */
    "No such file, or one that will not load", /* ENOENT */
    "Out of memory",                     /* ENOMEM */
    "A file of that name already",       /* EEXIST */
    "Not a directory",                   /* ENOTDIR */
    "Is a directory",                    /* EISDIR */
    "The directory is not empty",        /* ENOTEMPTY */
    "The volume may not be written",     /* EROFS */
    "The path or a component is too long", /* ENAMETOOLONG */
    "Too many symbolic links",           /* ELOOP */
    "The volume has no room",            /* ENOSPC */
    "Every descriptor is in use",        /* EMFILE */
    "Something is held that this would destroy", /* EBUSY */
    "The operation would cross a volume", /* EXDEV */
    "The filesystem does not offer this", /* ENOTSUP */
    "The volume or its device failed",    /* EIO */
    "The pipe is open for reading by nobody", /* EPIPE */
    "Interrupted by a signal",             /* EINTR */
    "No such process"                      /* ESRCH */
};

/*
 * The three ISO/IEC 9899:2011, Section 7.5, requires, which stand above the
 * range reserved to the kernel's results and are therefore a table of their own
 * rather than a gap of eleven entries in the one above.
 *
 * Nothing in this system sets any of them yet. They are described all the same,
 * because strerror must map any int and a program that received one of these
 * from a library this project has not yet written would otherwise be told the
 * number is unknown.
 */
static char OxysErrorStandardMessages[(ERANGE - EDOM) + 1][48] = {
    "Argument outside the domain of the function", /* EDOM */
    "Invalid multibyte sequence",                  /* EILSEQ */
    "Result outside the range of the type"         /* ERANGE */
};

/*
 * Everything else, which is most of the values of type int.
 *
 * It is one array and not one per number, so two calls with two different
 * unrecognised numbers return the same pointer. That is within Section
 * 7.24.6.2: the standard permits a subsequent call to overwrite what a previous
 * one returned, and this returns the same unchanged array instead.
 */
static char OxysErrorUnknownMessage[] = "Unknown error";

char *strerror(int errnum)
{
    if ((errnum >= 0) && (errnum <= ESRCH))
    {
        return OxysErrorSyscallMessages[errnum];
    }

    if ((errnum >= EDOM) && (errnum <= ERANGE))
    {
        return OxysErrorStandardMessages[errnum - EDOM];
    }

    return OxysErrorUnknownMessage;
}
