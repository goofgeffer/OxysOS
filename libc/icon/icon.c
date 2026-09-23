/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/icon/icon.c
 * Purpose: Turns the bytes of an icon file into pixels a program may draw, and
 *          refuses everything that is not one. It opens nothing and reads
 *          nothing.
 * Key functions: OxysIconParse, OxysIconAt, OxysIconWidth, OxysIconHeight.
 * References:
 *   - libc/include/icon.h: the format, and why an icon is a file.
 *   - docs/design/SESSION.md, Section 8: what reads these and when.
 *
 * Concurrency. None; see the header.
 */

#include <icon.h>
#include <string.h>

/* One little-endian 32-bit value from four bytes. Written out rather than cast,
 * because a cast would read the bytes in the order this processor happens to
 * use and the file's order is the file's. */
static uint32_t IconWord(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

bool OxysIconParse(OxysIcon *icon, const void *bytes, size_t count)
{
    const uint8_t *const source = bytes;
    uint32_t width;
    uint32_t height;
    size_t expected;

    if (icon == NULL)
    {
        return false;
    }

    /* Emptied first, so that a refusal leaves nothing of whatever was here
     * before. A caller that drew a refused icon would otherwise draw the last
     * one it read. */
    icon->width = 0U;
    icon->height = 0U;

    if ((source == NULL) || (count < ICON_HEADER_BYTES))
    {
        return false;
    }

    if (memcmp(source, ICON_MAGIC, 4U) != 0)
    {
        return false;
    }

    if (source[4] != (uint8_t)ICON_VERSION)
    {
        return false;
    }

    width = source[5];
    height = source[6];

    if ((width == 0U) || (height == 0U) || (width > ICON_EXTENT_MAXIMUM) ||
        (height > ICON_EXTENT_MAXIMUM))
    {
        return false;
    }

    /*
     * The length must be exactly the header and the pixels the extent calls
     * for. A file longer than that is a file this does not understand, and a
     * file shorter is one truncated in the middle — which without this check
     * becomes a picture of whatever followed it in memory.
     */
    expected = ICON_HEADER_BYTES + ((size_t)width * (size_t)height * 4U);

    if (count != expected)
    {
        return false;
    }

    for (uint32_t y = 0U; y < height; ++y)
    {
        for (uint32_t x = 0U; x < width; ++x)
        {
            const size_t at = ICON_HEADER_BYTES + (((size_t)y * (size_t)width + x) * 4U);

            icon->pixel[(y * ICON_EXTENT_MAXIMUM) + x] = IconWord(&source[at]);
        }
    }

    icon->width = width;
    icon->height = height;

    return true;
}

uint32_t OxysIconAt(const OxysIcon *icon, uint32_t x, uint32_t y)
{
    if ((icon == NULL) || (x >= icon->width) || (y >= icon->height))
    {
        return ICON_NOTHING;
    }

    return icon->pixel[(y * ICON_EXTENT_MAXIMUM) + x];
}

uint32_t OxysIconWidth(const OxysIcon *icon)
{
    return (icon == NULL) ? 0U : icon->width;
}

uint32_t OxysIconHeight(const OxysIcon *icon)
{
    return (icon == NULL) ? 0U : icon->height;
}
