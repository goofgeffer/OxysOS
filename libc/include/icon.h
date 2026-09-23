/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/icon.h
 * Purpose: Declares the icon of sub-task 9.6 — a small picture a program reads
 *          from a file at run time and draws into a window — and the seam
 *          between reading the bytes, which only a program at privilege level 3
 *          may do, and understanding them, which anything may.
 * Key definitions: OxysIcon, ICON_EXTENT_MAXIMUM, ICON_BYTES_MAXIMUM,
 *          ICON_NOTHING, ICON_MAGIC, ICON_VERSION, OxysIconParse, OxysIconAt,
 *          OxysIconWidth, OxysIconHeight, OxysIconRead.
 * References:
 *   - docs/design/SESSION.md, Section 8: what icons are for, where they live,
 *     and why they are files rather than a header compiled in.
 *   - art/README.md: the format, and the one command that makes a file of it
 *     from a picture somebody drew.
 *   - kernel/abi/oxys/syscall_abi.h: the 0x00RRGGBB a client's pixel is, which
 *     is what a parsed icon holds, so that drawing one is a blit and not a
 *     conversion.
 *
 * Why a file and not a header.
 *
 *   The mark of art/logo.h is compiled into the kernel and into the session
 *   because both draw it before there is a filesystem to read and because there
 *   is exactly one of it. An icon is the opposite of both: there is one per
 *   program, the set grows whenever somebody adds an entry to a launcher, and
 *   nothing draws one before `/` is mounted. A picture compiled in is a picture
 *   that needs a rebuild of the system to change, and a launcher whose entries
 *   are read from `/etc/session.conf` at start cannot have its pictures fixed
 *   at compile time without the two disagreeing the first time somebody edits
 *   the file.
 *
 * The format, in one paragraph.
 *
 *   Eight bytes of header — `OXIC`, a version, a width, a height, a reserved
 *   byte — and then one 32-bit little-endian pixel per position, row by row.
 *   A pixel is the `0x00RRGGBB` the window protocol carries, or ICON_NOTHING
 *   for a position the icon does not cover, which is what lets an icon sit upon
 *   whatever is behind it. There is no compression and no palette: an icon is
 *   at most a few kilobytes, and a format a person can read with `xxd` is a
 *   format that can be checked by looking.
 */

#ifndef OXYS_ICON_H
#define OXYS_ICON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The largest icon this holds, in each direction.
 *
 * Thirty-two is twice the sixteen a launcher's row is drawn with, which is the
 * one thing that draws icons today; a larger one is refused rather than
 * truncated, a picture silently missing its right-hand half being worse than a
 * picture that did not appear. The structure is the bound made of memory: four
 * kilobytes of pixels, which a program may hold several of.
 */
#define ICON_EXTENT_MAXIMUM 32U

/* The header, and the largest file that can be an icon of that extent. */
#define ICON_HEADER_BYTES 8U
#define ICON_BYTES_MAXIMUM \
    (ICON_HEADER_BYTES + (ICON_EXTENT_MAXIMUM * ICON_EXTENT_MAXIMUM * 4U))

/*
 * A position the icon does not cover.
 *
 * A client's pixel is `0x00RRGGBB` and its top byte is zero, so a top byte of
 * 0xFF is a value no pixel can be. That is why the sentinel is a colour and not
 * a second array: a mask would double the file and give the caller two things
 * to keep in step.
 */
#define ICON_NOTHING UINT32_C(0xFF000000)

/* What the first four bytes are, and the version the fifth carries. */
#define ICON_MAGIC   "OXIC"
#define ICON_VERSION 1U

/*
 * One icon, parsed.
 *
 * Concurrency. None. It is a program's own memory, touched by that program
 * alone, and this system has no threads within a process.
 */
typedef struct OxysIcon
{
    uint32_t width;
    uint32_t height;
    uint32_t pixel[ICON_EXTENT_MAXIMUM * ICON_EXTENT_MAXIMUM];
} OxysIcon;

/*
 * Reads `count` bytes as an icon. False where they are not one, and the icon is
 * left empty rather than half filled: a caller that drew a refused icon would
 * draw whatever the last one left behind.
 *
 * Every field is judged — the magic, the version, an extent of zero or beyond
 * the bound, and a length that is not exactly the header and the pixels the
 * extent calls for. The last is what catches a file truncated in the middle,
 * which is the failure a length nobody checks turns into a picture of whatever
 * followed it in memory.
 */
bool OxysIconParse(OxysIcon *icon, const void *bytes, size_t count);

/* The pixel at a position, or ICON_NOTHING outside the icon — so that a caller
 * drawing a square larger than the icon needs no bounds of its own. */
uint32_t OxysIconAt(const OxysIcon *icon, uint32_t x, uint32_t y);

uint32_t OxysIconWidth(const OxysIcon *icon);
uint32_t OxysIconHeight(const OxysIcon *icon);

/*
 * Reads an icon from a file. The one function here that touches a descriptor,
 * and the reason this is two translation units: everything above is asserted
 * without a filesystem, as the line editor's editing and the configuration's
 * parsing are.
 */
bool OxysIconRead(OxysIcon *icon, const char *path);

#endif /* OXYS_ICON_H */
