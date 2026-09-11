/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/string/copying.c
 * Purpose: Implements the copying functions of ISO/IEC 9899:2011, Sections
 *          7.24.2 and 7.24.3 — the two that move bytes and the four that move
 *          strings.
 * Key functions: memcpy, memmove, strcpy, strncpy, strcat, strncat.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.24.2.1 (memcpy), 7.24.2.2 (memmove),
 *     7.24.2.3 (strcpy), 7.24.2.4 (strncpy), 7.24.3.1 (strcat) and 7.24.3.2
 *     (strncat): the behaviour of each, including the return value, which in
 *     every case here is the destination.
 *   - ISO/IEC 9899:2011, Section 6.7.3.1: the meaning of `restrict`, which is
 *     what permits memcpy to copy forwards without asking whether the objects
 *     overlap.
 *   - docs/design/LIBC.md, Section 3: the group these six belong to, and
 *     Section 5, which pairs each assertion of the self-test with the failure it
 *     would catch.
 *
 * Every byte here is moved through `unsigned char`, which is the only type the
 * standard permits an object's representation to be examined as (Section
 * 6.5, paragraph 7). A copy written through `char` would be a copy whose
 * behaviour depended upon whether the compiler's plain char is signed; x86_64's
 * is, and no byte would move differently today, which is exactly what makes the
 * mistake survive.
 */

#include <string.h>

void *memcpy(void *restrict destination, const void *restrict source, size_t n)
{
    unsigned char *const out = (unsigned char *)destination;
    const unsigned char *const in = (const unsigned char *)source;

    for (size_t index = 0U; index < n; ++index)
    {
        out[index] = in[index];
    }

    return destination;
}

/*
 * The direction is chosen, and choosing it is the whole of this function.
 *
 * The standard requires memmove to behave as though the bytes were copied first
 * into a temporary object and then into the destination, so a caller may move a
 * buffer over itself. No temporary is needed to achieve that: a forward copy is
 * already correct when the destination lies below the source, and a backward
 * copy is correct when it lies above. The only case that can go wrong is
 * overlapping regions copied in the wrong direction, which silently duplicates
 * the first byte across the overlap — a failure that produces a plausible
 * result rather than a fault, which is why the self-test asserts both
 * directions against a pattern in which every byte is distinguishable.
 *
 * The pointers are compared after conversion to `unsigned char *`. Comparing
 * pointers into distinct objects is unspecified by Section 6.5.8, paragraph 5;
 * but a caller whose objects are distinct gets a correct copy whichever branch
 * an unspecified comparison selects, so no behaviour depends upon the answer in
 * the case where the standard declines to give one.
 */
void *memmove(void *destination, const void *source, size_t n)
{
    unsigned char *const out = (unsigned char *)destination;
    const unsigned char *const in = (const unsigned char *)source;

    if (out < in)
    {
        for (size_t index = 0U; index < n; ++index)
        {
            out[index] = in[index];
        }
    }
    else if (out > in)
    {
        for (size_t index = n; index > 0U; --index)
        {
            out[index - 1U] = in[index - 1U];
        }
    }

    return destination;
}

char *strcpy(char *restrict destination, const char *restrict source)
{
    size_t index = 0U;

    /* The terminator is copied by the assignment that ends the loop, and the
     * loop ends because that assignment's value is zero. A separate write
     * afterwards would be a second place to forget it. */
    while ((destination[index] = source[index]) != '\0')
    {
        ++index;
    }

    return destination;
}

/*
 * The function whose interface is the trap.
 *
 * Two things about it are the standard's and are implemented exactly rather than
 * improved upon. It pads: where the source is shorter than n, the remainder of
 * the destination is filled with null bytes, which costs n writes for a
 * one-byte string. And it does not terminate: where the source is n bytes or
 * longer, no terminator is written at all, so the destination is not a string
 * and every later reader of it runs off the end.
 *
 * Neither is repaired here. A strncpy that terminated would be a function with
 * a standard name and non-standard behaviour, which is worse than either the
 * standard function or a differently-named one; docs/design/LIBC.md, Section 6,
 * limitation 2, records the bounded copy that ought to exist beside it and why
 * it is not being invented in this sub-task.
 */
char *strncpy(char *restrict destination, const char *restrict source, size_t n)
{
    size_t index = 0U;

    while ((index < n) && (source[index] != '\0'))
    {
        destination[index] = source[index];
        ++index;
    }

    while (index < n)
    {
        destination[index] = '\0';
        ++index;
    }

    return destination;
}

char *strcat(char *restrict destination, const char *restrict source)
{
    size_t end = 0U;
    size_t index = 0U;

    while (destination[end] != '\0')
    {
        ++end;
    }

    while ((destination[end + index] = source[index]) != '\0')
    {
        ++index;
    }

    return destination;
}

/*
 * Unlike strncpy, this one always terminates, and it does not pad.
 *
 * The two bounded functions of this header therefore differ in both of the
 * respects a reader would expect them to share, and the difference is the
 * standard's. n bounds the bytes taken *from the source*; the destination
 * receives at most n + 1.
 */
char *strncat(char *restrict destination, const char *restrict source, size_t n)
{
    size_t end = 0U;
    size_t index = 0U;

    while (destination[end] != '\0')
    {
        ++end;
    }

    while ((index < n) && (source[index] != '\0'))
    {
        destination[end + index] = source[index];
        ++index;
    }

    destination[end + index] = '\0';

    return destination;
}
