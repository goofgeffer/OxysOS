/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/stdlib.h
 * Purpose: Declares the memory management functions of ISO/IEC 9899:2011,
 *          Section 7.22.3, as this library implements them — and states, at its
 *          head, which of that subsection is absent and what it is waiting for.
 * Key definitions: malloc, calloc, realloc, free.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.22.3: the memory management functions, their
 *     alignment guarantee, and the rule that the order and contiguity of
 *     successive allocations is unspecified.
 *   - ISO/IEC 9899:2011, Section 6.2.8, paragraph 2: a fundamental alignment is
 *     one no stricter than _Alignof(max_align_t), which upon this architecture
 *     is sixteen. That is the alignment every pointer returned here satisfies.
 *   - ISO/IEC 9899:2011, Section 7.5: the errno these functions set upon
 *     failure, which the standard permits and does not require.
 *   - docs/design/LIBC.md, Section 9: the design of the allocator beneath these
 *     four names, what is asserted of it and what is not.
 *
 * What of <stdlib.h> is here, and what is not.
 *
 * This header is not <stdlib.h> as a hosted implementation provides it. ISO/IEC
 * 9899:2011, Section 4, paragraph 6, does not require a freestanding
 * implementation to provide the header at all, and this library provides the
 * part of it sub-task 7.3 is: the four memory management functions of Section
 * 7.22.3 and nothing else. Absent are the string conversions of 7.22.1, the
 * pseudo-random sequence of 7.22.2, the communication with the environment of
 * 7.22.4, the searching and sorting of 7.22.5, the integer arithmetic of 7.22.6
 * and the multibyte conversions of 7.22.7 and 7.22.8. Each arrives with the
 * sub-task that needs it, and none of them is needed by a heap.
 *
 * The types <stdlib.h> is required to define — size_t, wchar_t, div_t, ldiv_t,
 * lldiv_t — are likewise not all here. size_t is obtained from <stddef.h>, which
 * a freestanding implementation must provide; the division types belong to
 * functions this header does not declare, and declaring a type for a function
 * that does not exist would be an interface promising something.
 *
 * **aligned_alloc is deliberately absent.** Section 7.22.3.1 requires it to
 * honour any alignment the implementation supports, and every extended alignment
 * is stricter than the sixteen bytes every block here already has. Honouring one
 * means returning a pointer that is not at a fixed displacement from its own
 * block header — which is the invariant `free` finds a block by, and the one
 * thing in this allocator that everything else depends upon. It arrives when
 * something asks for it, and what it will cost is recorded in
 * docs/design/LIBC.md, Section 9.6, rather than discovered then.
 *
 * Why these four keep their standard names, when every other global function in
 * this repository is PascalCase.
 *
 * PROJECT_GUIDELINES.md, Section 4, fixes PascalCase for a global function, and
 * <syscall.h> observes it: the seven calls are OxysWrite and not write, because
 * none of them means quite what the POSIX name means. These four are the
 * opposite case. Their names are fixed by ISO/IEC 9899:2011 and they mean
 * exactly what that standard says they mean; a program calling `malloc` is
 * entitled to find this one, and a compiler is entitled to assume the semantics
 * of it. Spelling them otherwise would produce a library no conforming program
 * could use, which is a worse outcome than a convention bent where a standard
 * already decides the question.
 */

#ifndef OXYS_LIBC_STDLIB_H
#define OXYS_LIBC_STDLIB_H

#include <stddef.h>

/*
 * 7.22.3.4: allocates space for an object of the given size, whose value is
 * indeterminate, and returns a pointer to it or a null pointer.
 *
 * **A request of zero bytes returns a distinct pointer and not a null one.**
 * Section 7.22.3, paragraph 1, makes the choice implementation-defined and this
 * library takes the pointer, because the alternative is unusable: a null pointer
 * returned for a zero-sized request cannot be told apart from a failure, and
 * every correct program checks for null. The pointer returned may be freed and
 * must not be used to access an object, which is what that paragraph requires of
 * it.
 *
 * Upon failure errno is set to ENOMEM. ISO C does not require that of malloc and
 * Section 7.5, paragraph 3, permits it; it is done because every other way this
 * library reports a failure sets errno, and a caller should not have to know
 * which functions are the exception.
 */
void *malloc(size_t size);

/*
 * 7.22.3.2: allocates space for an array of `count` objects of `size` bytes
 * each, initialised to all bits zero.
 *
 * The product is checked for overflow before it is formed. That check is the
 * whole reason this function is not written as a malloc and a memset by its
 * callers: a product that wraps produces a small allocation for a large request,
 * the caller writes the elements it asked for, and the write runs off the end of
 * a block the allocator believes is smaller than it is. It is the commonest
 * arithmetic fault in C that has a name.
 */
void *calloc(size_t count, size_t size);

/*
 * 7.22.3.5: changes the size of an existing allocation, preserving its contents
 * up to the lesser of the two sizes.
 *
 * A null pointer makes this malloc. **A failure leaves the old allocation
 * untouched and returns a null pointer**, as paragraph 3 requires — which is why
 * the idiom `p = realloc(p, n)` loses the only reference to the old object and
 * is a leak rather than a shorthand.
 */
void *realloc(void *pointer, size_t size);

/*
 * 7.22.3.3: releases an allocation. A null pointer is no action.
 *
 * The standard makes it undefined behaviour to pass anything that did not come
 * from one of the three functions above, or to pass the same pointer twice. This
 * library defines both as a refusal: the block is examined for the mark the
 * allocator wrote into it, and one that does not carry it is left alone and
 * counted. That is a courtesy and not a guarantee — a pointer into the middle of
 * a block may carry another block's mark — and it exists because a heap that
 * corrupts itself silently is the most expensive failure a C program has.
 */
void free(void *pointer);

#endif /* OXYS_LIBC_STDLIB_H */
