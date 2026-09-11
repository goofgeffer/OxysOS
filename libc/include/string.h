/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/string.h
 * Purpose: Declares the string and memory handling functions of ISO/IEC
 *          9899:2011, Section 7.24, as this C library implements them.
 * Key definitions: memcpy, memmove, memcmp, memchr, memset, strcpy, strncpy,
 *          strcat, strncat, strcmp, strncmp, strchr, strrchr, strspn, strcspn,
 *          strpbrk, strstr, strtok, strlen.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.24: the whole of this header. Each function
 *     below cites the subsection that defines it.
 *   - ISO/IEC 9899:2011, Section 4, paragraph 6: a freestanding implementation
 *     provides <stddef.h>, from which size_t and NULL are taken rather than
 *     being defined here a second time.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.1.2:
 *     the LP64 model, in which size_t is 64 bits.
 *   - docs/design/LIBC.md, Sections 3 and 4: what is implemented here, what is
 *     not, and the reason in each case.
 *
 * Three functions of Section 7.24 are absent, and each is absent for a reason
 * rather than by oversight.
 *
 *   strcoll and strxfrm compare and transform according to the current locale.
 *   There is no locale in this system and no <locale.h> to establish one; both
 *   would therefore be strcmp and a bounded copy wearing another name, which is
 *   an agreement with a standard this library could not yet keep. They arrive
 *   with the locale.
 *
 *   strerror maps an integer to a message, and the integers it would map are
 *   the failure results of <oxys/syscall_abi.h> as the wrappers of sub-task 7.2
 *   will present them through errno. The table belongs beside the thing that
 *   sets errno and not here, where it would fix the spelling of every error
 *   message before a single call had a wrapper.
 *
 * Nothing here allocates, and nothing here calls the kernel. These are the
 * functions a freestanding program may use before it has a heap, a descriptor or
 * a process, which is why they are sub-task 7.1 and everything else in Phase 7
 * comes after them.
 */

#ifndef OXYS_LIBC_STRING_H
#define OXYS_LIBC_STRING_H

#include <stddef.h>

/* -------------------------------------------------------------------------
 * ISO/IEC 9899:2011, Section 7.24.2: copying.
 * ------------------------------------------------------------------------- */

/*
 * Copies n bytes from source to destination. The objects must not overlap: the
 * parameters are declared `restrict` by 7.24.2.1, and an overlapping call is
 * undefined behaviour rather than a slow path this implementation takes.
 * memmove is what a caller with overlapping objects must use.
 */
void *memcpy(void *restrict destination, const void *restrict source, size_t n);

/* Copies n bytes as though through a temporary buffer, so that the objects may
 * overlap. ISO/IEC 9899:2011, Section 7.24.2.2. */
void *memmove(void *destination, const void *source, size_t n);

/* Copies the string at source, its terminator included. 7.24.2.3. */
char *strcpy(char *restrict destination, const char *restrict source);

/*
 * Copies at most n bytes of the string at source, padding with null bytes to n
 * if the string is shorter. 7.24.2.4.
 *
 * **It does not terminate a destination it fills.** That is the standard's
 * behaviour and not a defect here, and it is the reason this function is the
 * wrong one for almost every use it is reached for.
 */
char *strncpy(char *restrict destination, const char *restrict source, size_t n);

/* Appends the string at source to the one at destination. 7.24.3.1. */
char *strcat(char *restrict destination, const char *restrict source);

/* Appends at most n bytes of source, and always terminates. 7.24.3.2. */
char *strncat(char *restrict destination, const char *restrict source, size_t n);

/* -------------------------------------------------------------------------
 * ISO/IEC 9899:2011, Section 7.24.4: comparison.
 * ------------------------------------------------------------------------- */

/*
 * Compares n bytes of two objects. 7.24.4.1.
 *
 * The bytes are compared as `unsigned char`, which the standard requires and
 * which is the whole of the difference between a correct implementation and one
 * that reports a byte above 127 as less than a byte below it upon a machine
 * whose plain char is signed. x86_64's is.
 */
int memcmp(const void *left, const void *right, size_t n);

/* Compares two strings, as unsigned char, to their first difference. 7.24.4.2. */
int strcmp(const char *left, const char *right);

/* The same, bounded to n bytes. 7.24.4.4. */
int strncmp(const char *left, const char *right, size_t n);

/* -------------------------------------------------------------------------
 * ISO/IEC 9899:2011, Section 7.24.5: search.
 * ------------------------------------------------------------------------- */

/* The first occurrence of c, converted to unsigned char, within the first n
 * bytes of an object; null where there is none. 7.24.5.1. */
void *memchr(const void *object, int c, size_t n);

/* The first occurrence of c, converted to char, in a string — the terminator
 * included, so strchr(s, 0) finds the end of s. 7.24.5.2. */
char *strchr(const char *string, int c);

/* The length of the initial segment of `string` consisting of bytes not in
 * `reject`. 7.24.5.3. */
size_t strcspn(const char *string, const char *reject);

/* The first byte of `string` that is also in `accept`; null where there is
 * none. 7.24.5.4. */
char *strpbrk(const char *string, const char *accept);

/* The last occurrence of c in a string, the terminator included. 7.24.5.5. */
char *strrchr(const char *string, int c);

/* The length of the initial segment of `string` consisting only of bytes in
 * `accept`. 7.24.5.6. */
size_t strspn(const char *string, const char *accept);

/* The first occurrence of `needle` within `haystack`; `haystack` itself where
 * `needle` is empty. 7.24.5.7. */
char *strstr(const char *haystack, const char *needle);

/*
 * Breaks a string into tokens separated by any byte of `separators`. 7.24.5.8.
 *
 * The first call of a sequence passes the string and each subsequent call passes
 * null. The position is kept in a static object, which makes this function the
 * only one in this header that is neither re-entrant nor safe to call from two
 * threads at once — a property of the interface the standard defines, recorded
 * in docs/design/LIBC.md, Section 6, limitation 3, rather than repaired here
 * under the same name.
 */
char *strtok(char *restrict string, const char *restrict separators);

/* -------------------------------------------------------------------------
 * ISO/IEC 9899:2011, Section 7.24.6: miscellaneous.
 * ------------------------------------------------------------------------- */

/* Sets n bytes of an object to c, converted to unsigned char. 7.24.6.1. */
void *memset(void *object, int c, size_t n);

/* The number of bytes before a string's terminator. 7.24.6.3. */
size_t strlen(const char *string);

#endif /* OXYS_LIBC_STRING_H */
