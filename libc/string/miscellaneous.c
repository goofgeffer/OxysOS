/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/string/miscellaneous.c
 * Purpose: Implements the two functions ISO/IEC 9899:2011, Section 7.24.6,
 *          groups under no heading of their own — the fill and the length.
 * Key functions: memset, strlen.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.24.6.1 (memset) and 7.24.6.3 (strlen).
 *   - ISO/IEC 9899:2011, Section 7.24.6.2 (strerror), which is not here; the
 *     header records why, and docs/design/LIBC.md, Section 4, at length.
 *   - docs/design/LIBC.md, Section 5: the assertions, each paired with the
 *     failure it would catch.
 *
 * These two are together because the standard puts them together, and the
 * grouping is worth keeping even though they have nothing else in common: a
 * reader looking for the whole of Section 7.24 in this directory finds it
 * divided exactly as the standard divides it, and can tell at a glance that
 * 7.24.6.2 is the only thing missing.
 */

#include <string.h>

/*
 * The conversion is to `unsigned char` and not to `char`, and the distinction is
 * visible: memset(buffer, 0xFF, n) must fill with 0xFF, which is what the
 * conversion below produces and what a conversion through a signed char would
 * also produce here, by a route that is implementation-defined rather than
 * required. The standard states the unsigned conversion; it is written out.
 */
void *memset(void *object, int c, size_t n)
{
    unsigned char *const bytes = (unsigned char *)object;
    const unsigned char value = (unsigned char)c;

    for (size_t index = 0U; index < n; ++index)
    {
        bytes[index] = value;
    }

    return object;
}

size_t strlen(const char *string)
{
    size_t length = 0U;

    while (string[length] != '\0')
    {
        ++length;
    }

    return length;
}
