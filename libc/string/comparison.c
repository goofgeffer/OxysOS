/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/string/comparison.c
 * Purpose: Implements the comparison functions of ISO/IEC 9899:2011, Section
 *          7.24.4 — the one that compares objects and the two that compare
 *          strings.
 * Key functions: memcmp, strcmp, strncmp.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.24.4.1 (memcmp), 7.24.4.2 (strcmp) and
 *     7.24.4.4 (strncmp): each compares the bytes **as unsigned char** and
 *     returns a value greater than, equal to or less than zero.
 *   - ISO/IEC 9899:2011, Section 6.2.5, paragraph 15: whether plain char is
 *     signed is implementation-defined. Upon x86_64 with this toolchain it is,
 *     which is what makes the paragraph below a correctness matter rather than
 *     a pedantry.
 *   - docs/design/LIBC.md: the assertions, each paired with the
 *     failure it would catch.
 *
 * The one thing these three can get wrong invisibly.
 *
 * The standard requires the comparison to be performed upon `unsigned char`. An
 * implementation that compared plain char would agree with a correct one for
 * every byte below 128 and disagree for every byte above it, reporting 0x80 as
 * *less* than 0x01. Every ASCII string in this system would compare correctly,
 * so nothing would go wrong until the first byte of a UTF-8 sequence, a
 * binary buffer or a hash was compared — at which point a sort would be subtly
 * wrong and nothing would fault. The self-test asserts exactly this pair of
 * bytes for that reason.
 *
 * The difference is returned as `int` after conversion, which cannot overflow:
 * both operands are in the range 0 to 255, so the difference lies between -255
 * and 255 and is representable in int upon every conforming implementation.
 */

#include <string.h>

int memcmp(const void *left, const void *right, size_t n)
{
    const unsigned char *const first = (const unsigned char *)left;
    const unsigned char *const second = (const unsigned char *)right;

    for (size_t index = 0U; index < n; ++index)
    {
        if (first[index] != second[index])
        {
            return (int)first[index] - (int)second[index];
        }
    }

    return 0;
}

int strcmp(const char *left, const char *right)
{
    const unsigned char *const first = (const unsigned char *)left;
    const unsigned char *const second = (const unsigned char *)right;
    size_t index = 0U;

    /* The terminator ends the loop by inequality where the strings differ in
     * length, and by equality where they do not — so no separate test for the
     * end of either string is needed, and none may be added: a string that ran
     * out would then be compared past its own terminator. */
    while ((first[index] == second[index]) && (first[index] != '\0'))
    {
        ++index;
    }

    return (int)first[index] - (int)second[index];
}

int strncmp(const char *left, const char *right, size_t n)
{
    const unsigned char *const first = (const unsigned char *)left;
    const unsigned char *const second = (const unsigned char *)right;

    for (size_t index = 0U; index < n; ++index)
    {
        if (first[index] != second[index])
        {
            return (int)first[index] - (int)second[index];
        }

        /* Both are equal here, so testing one for the terminator tests both.
         * Stopping is required and not an optimisation: the standard compares
         * "not more than n characters", and characters after a terminator are
         * not part of either string. */
        if (first[index] == '\0')
        {
            return 0;
        }
    }

    return 0;
}
