/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/icon/icon.c
 * Purpose: Turns the bytes of an icon file into pixels a program may draw, and
 *          refuses everything that is not one. It opens nothing and reads
 *          nothing.
 * Key functions: OxysIconParse, OxysIconAt, OxysIconWidth, OxysIconHeight,
 *          OxysIconCompose.
 * References:
 *   - libc/include/icon.h: the format, and why an icon is a file.
 *   - docs/design/SESSION.md: what reads these and when.
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

    /* Every version from the oldest to this one is read alike: a pixel of
     * version 1 is 0x00 or 0xFF in its top byte, which version 2 reads as it
     * was meant. A later one is refused, since what it adds is unknown here. */
    if ((source[4] < (uint8_t)ICON_VERSION_OLDEST) || (source[4] > (uint8_t)ICON_VERSION))
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

uint32_t OxysIconCompose(const OxysIcon *icon, uint32_t x, uint32_t y, uint32_t extent,
                         uint32_t paper)
{
    uint32_t side;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t first_x;
    uint32_t last_x;
    uint32_t first_y;
    uint32_t last_y;
    uint64_t opacity = 0U;
    uint64_t channel[3] = {0U, 0U, 0U};
    uint64_t whole;
    uint32_t pixel = 0U;

    if ((icon == NULL) || (icon->width == 0U) || (icon->height == 0U) || (extent == 0U) ||
        (x >= extent) || (y >= extent))
    {
        return ICON_COLOUR(paper);
    }

    /* The icon's square, and where the icon stands within it. */
    side = (icon->width > icon->height) ? icon->width : icon->height;
    offset_x = (side - icon->width) / 2U;
    offset_y = (side - icon->height) / 2U;

    first_x = (x * side) / extent;
    last_x = ((x + 1U) * side) / extent;
    first_y = (y * side) / extent;
    last_y = ((y + 1U) * side) / extent;

    if (last_x <= first_x)
    {
        last_x = first_x + 1U;
    }

    if (last_y <= first_y)
    {
        last_y = first_y + 1U;
    }

    /*
     * Each colour weighted by how opaque it is, so that the colour a wholly
     * transparent pixel happens to carry — black, for ICON_NOTHING — adds
     * nothing. Averaging the colours alone would darken every edge by the
     * black of the nothing beside it, which is a fringe about the picture.
     */
    for (uint32_t row = first_y; row < last_y; ++row)
    {
        for (uint32_t column = first_x; column < last_x; ++column)
        {
            uint32_t value = ICON_NOTHING;
            uint32_t opaque;

            if ((column >= offset_x) && (row >= offset_y))
            {
                value = OxysIconAt(icon, column - offset_x, row - offset_y);
            }

            opaque = ICON_TRANSPARENT - ICON_TRANSPARENCY(value);
            opacity += opaque;

            for (uint32_t index = 0U; index < 3U; ++index)
            {
                channel[index] += (uint64_t)((value >> (8U * index)) & 0xFFU) * opaque;
            }
        }
    }

    whole = (uint64_t)(last_x - first_x) * (uint64_t)(last_y - first_y) * ICON_TRANSPARENT;

    for (uint32_t index = 0U; index < 3U; ++index)
    {
        const uint64_t behind = (paper >> (8U * index)) & 0xFFU;
        const uint64_t mixed = (channel[index] + (behind * (whole - opacity)) + (whole / 2U)) / whole;

        pixel |= (uint32_t)mixed << (8U * index);
    }

    return pixel;
}
