/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdio/internal.h
 * Purpose: Declares the two counters the formatted conversion keeps in the
 *          census that libc/stdio/stream.c owns, so that the two translation
 *          units of this sub-task share a record without sharing a structure.
 * Key definitions: OxysStreamCountConversion, OxysStreamCountRejection.
 * References:
 *   - libc/include/stream.h: OxysStreamCensus, whose `conversions` and
 *     `rejections` these two increment, and which is where a caller reads them.
 *   - docs/design/LIBC.md, Section 10.2: the division of this sub-task, of which
 *     this header is a consequence rather than a part.
 *
 * Why this is a private header and not two more declarations in <stream.h>.
 *
 * <stream.h> is an interface: a program may include it, and everything in it is
 * something a program may reasonably call. These two are neither — they are how
 * one translation unit of this library tells another that it did something, and
 * a program calling one would be falsifying a census it does not own.
 *
 * It is in libc/stdio/ rather than libc/include/ for exactly that reason: the
 * include root a program is compiled against does not carry it, so the
 * distinction is enforced by the build rather than by a comment asking nobody to
 * call them.
 */

#ifndef OXYS_LIBC_STDIO_INTERNAL_H
#define OXYS_LIBC_STDIO_INTERNAL_H

/* Records that a conversion specification was performed. */
void OxysStreamCountConversion(void);

/* Records that a conversion specification was refused — an unimplemented
 * conversion, or one this library declines to perform. */
void OxysStreamCountRejection(void);

#endif /* OXYS_LIBC_STDIO_INTERNAL_H */
