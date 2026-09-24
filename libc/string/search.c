/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/string/search.c
 * Purpose: Implements the search functions of ISO/IEC 9899:2011, Section 7.24.5
 *          — finding a byte, a span of bytes, a substring, and the tokens a
 *          string divides into.
 * Key functions: memchr, strchr, strcspn, strpbrk, strrchr, strspn, strstr,
 *          strtok.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.24.5.1 (memchr), 7.24.5.2 (strchr),
 *     7.24.5.3 (strcspn), 7.24.5.4 (strpbrk), 7.24.5.5 (strrchr), 7.24.5.6
 *     (strspn), 7.24.5.7 (strstr) and 7.24.5.8 (strtok).
 *   - docs/design/LIBC.md: the assertions, each paired with the
 *     failure it would catch.
 *
 * Two conversions, and they are not the same one.
 *
 * memchr converts its `int` argument to `unsigned char` and compares against
 * the object's bytes. strchr and strrchr convert theirs to `char`. The standard
 * says so in each case, and the distinction matters at exactly one place: the
 * terminator. strchr(s, 0) and strrchr(s, 0) must find the terminator and
 * return a pointer to it, which is why the loops below test the terminator
 * *after* comparing rather than before.
 *
 * The searched-for byte is compared as unsigned char throughout, for the reason
 * libc/string/comparison.c gives at length: a comparison performed through plain
 * char agrees with a correct one for every byte below 128 and for none above it,
 * upon a machine whose plain char is signed.
 */

#include <string.h>

/*
 * <stdbool.h> is one of the four headers ISO/IEC 9899:2011, Section 4,
 * paragraph 6, requires a freestanding implementation to provide, so the helper
 * below may return a bool without this file depending upon a hosted library.
 */
#include <stdbool.h>

void *memchr(const void *object, int c, size_t n)
{
    const unsigned char *const bytes = (const unsigned char *)object;
    const unsigned char wanted = (unsigned char)c;

    for (size_t index = 0U; index < n; ++index)
    {
        if (bytes[index] == wanted)
        {
            /*
             * The const is cast away deliberately and the standard requires it:
             * 7.24.5 declares these functions to take a pointer to const and
             * return a pointer that is not const, C having no way to express
             * the qualifier being carried through. The object itself is not
             * modified here.
             */
            return (void *)(const void *)&bytes[index];
        }
    }

    return NULL;
}

char *strchr(const char *string, int c)
{
    const unsigned char *const bytes = (const unsigned char *)string;
    const unsigned char wanted = (unsigned char)(char)c;
    size_t index = 0U;

    for (;;)
    {
        if (bytes[index] == wanted)
        {
            return (char *)(const char *)&string[index];
        }

        if (bytes[index] == '\0')
        {
            return NULL;
        }

        ++index;
    }
}

char *strrchr(const char *string, int c)
{
    const unsigned char *const bytes = (const unsigned char *)string;
    const unsigned char wanted = (unsigned char)(char)c;
    const char *found = NULL;
    size_t index = 0U;

    /*
     * The search runs forwards and keeps the last match rather than running
     * backwards from the end. Both are correct; this one reads the string once
     * instead of twice, and — more to the point — a backward search must first
     * find the end, which is a second loop with its own off-by-one in the one
     * case that matters, the terminator itself.
     */
    for (;;)
    {
        if (bytes[index] == wanted)
        {
            found = &string[index];
        }

        if (bytes[index] == '\0')
        {
            break;
        }

        ++index;
    }

    return (char *)found;
}

/*
 * Whether a byte appears in a set given as a string.
 *
 * The set is scanned for each byte of the subject, which is the product of the
 * two lengths. A 256-entry table would make it linear and is what a library
 * sized for a compiler will eventually want; it is not built here because the
 * table costs a quarter of a kilobyte of stack in functions that are presently
 * called upon sets of two or three bytes. docs/design/LIBC.md
 * limitation 1, records the measurement that would justify changing it.
 *
 * The terminator is deliberately not a member of any set: the loop stops before
 * comparing it, so strcspn(s, "") is the length of s rather than zero.
 */
static bool StringByteIsInSet(unsigned char byte, const char *set)
{
    const unsigned char *const members = (const unsigned char *)set;

    for (size_t index = 0U; members[index] != '\0'; ++index)
    {
        if (members[index] == byte)
        {
            return true;
        }
    }

    return false;
}

size_t strspn(const char *string, const char *accept)
{
    const unsigned char *const bytes = (const unsigned char *)string;
    size_t length = 0U;

    while ((bytes[length] != '\0') && StringByteIsInSet(bytes[length], accept))
    {
        ++length;
    }

    return length;
}

size_t strcspn(const char *string, const char *reject)
{
    const unsigned char *const bytes = (const unsigned char *)string;
    size_t length = 0U;

    while ((bytes[length] != '\0') && !StringByteIsInSet(bytes[length], reject))
    {
        ++length;
    }

    return length;
}

char *strpbrk(const char *string, const char *accept)
{
    const unsigned char *const bytes = (const unsigned char *)string;

    for (size_t index = 0U; bytes[index] != '\0'; ++index)
    {
        if (StringByteIsInSet(bytes[index], accept))
        {
            return (char *)(const char *)&string[index];
        }
    }

    return NULL;
}

/*
 * The naive search, and the case that is not naive.
 *
 * An empty needle matches at the beginning: the standard defines the result as
 * a pointer to the haystack. The loop below already produces that for a haystack
 * that is not empty — the inner comparison never runs, so the needle's
 * terminator is reached at offset zero and the first position matches — and the
 * guard is there for the case it does not: **an empty needle in an empty
 * haystack**, where the outer loop performs no iteration at all and the function
 * would return null.
 *
 * The distinction is written out because it was got wrong here first. The guard
 * was added with a comment claiming it was what made *every* empty needle work,
 * and the self-test asserted the empty needle against a haystack that was not
 * empty — so deleting the guard altogether changed no result the test could see.
 * docs/design/LIBC.md records the negative test that found it and
 * the assertion that now covers it.
 *
 * The search is the product of the two lengths in the worst case. A
 * Boyer-Moore or Knuth-Morris-Pratt search would be linear and would want a
 * table proportional to the needle; docs/design/LIBC.md limitation
 * 1, records that this is deliberate and what would justify revisiting it.
 */
char *strstr(const char *haystack, const char *needle)
{
    if (needle[0] == '\0')
    {
        return (char *)(const char *)haystack;
    }

    for (size_t start = 0U; haystack[start] != '\0'; ++start)
    {
        size_t offset = 0U;

        while ((needle[offset] != '\0') &&
               (haystack[start + offset] == needle[offset]))
        {
            ++offset;
        }

        if (needle[offset] == '\0')
        {
            return (char *)(const char *)&haystack[start];
        }
    }

    return NULL;
}

/*
 * Where strtok is between calls.
 *
 * The standard requires the position to be kept by the function itself, which
 * is what makes strtok the one function in this header that two threads may not
 * call at once and that a caller may not interleave two scans with. It is a
 * property of the interface and not of this implementation; strtok_s and
 * strtok_r exist elsewhere precisely because of it, and neither is invented
 * here under a standard name. docs/design/LIBC.md.
 */
static char *StringTokenPosition;

char *strtok(char *restrict string, const char *restrict separators)
{
    char *start;
    char *end;

    /* A null subject continues the previous scan. A scan that has finished
     * stays finished, and every further call returns null. */
    if (string == NULL)
    {
        string = StringTokenPosition;

        if (string == NULL)
        {
            return NULL;
        }
    }

    start = string + strspn(string, separators);

    if (*start == '\0')
    {
        StringTokenPosition = NULL;
        return NULL;
    }

    end = start + strcspn(start, separators);

    if (*end == '\0')
    {
        /* The last token. The next call must return null rather than rescanning
         * a string whose terminator it would find immediately — which is the
         * same answer by accident, until the caller passes a different string
         * and the stale position sends the scan into it. */
        StringTokenPosition = NULL;
    }
    else
    {
        /* The separator is replaced by a terminator, which is why strtok may
         * not be given a string literal or any object the caller may not
         * write. */
        *end = '\0';
        StringTokenPosition = end + 1;
    }

    return start;
}
