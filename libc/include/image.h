/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/image.h
 * Purpose: Declares the image of Phase 9 — a picture larger than an icon,
 *          which a program reads from a file at run time and draws scaled to
 *          whatever it must cover — and the seam between reading the bytes and
 *          understanding them.
 * Key definitions: OxysImage, OxysImageScaler, IMAGE_MAGIC, IMAGE_VERSION,
 *          IMAGE_HEADER_BYTES, IMAGE_EXTENT_MAXIMUM, OxysImageParse,
 *          OxysImageWidth, OxysImageHeight, OxysImageScalerBegin,
 *          OxysImageScalerRow, OxysImageRead.
 * References:
 *   - docs/design/SESSION.md: what an image is for, where it lives,
 *     and why the background is a file and not a header compiled in.
 *   - art/README.md: the one command that makes a file of this format.
 *   - libc/include/icon.h: the smaller format beside it, and why the two are
 *     two.
 *
 * Why a second format beside the icon's.
 *
 *   An icon is a few kilobytes held whole, one pixel to four bytes, and a
 *   format a person can check with `xxd`. A background drawn at the resolution
 *   of the artwork is three million pixels; held the icon's way it is twelve
 *   megabytes, which is six times the ramdisk. What makes it small is that a
 *   drawing is flat colour — the background of 2026-09-23 is twelve thousand
 *   runs of one colour — so the pixels are stored as runs. Folding that into
 *   the icon format would give every icon a decoder it does not need, and
 *   every reader of icons a length it cannot check by multiplying.
 *
 * The format.
 *
 *   Twelve bytes of header — `OXIM`, a version, three reserved zero bytes, and
 *   the width and the height as 16-bit little-endian values — and then runs,
 *   row by row: a 16-bit little-endian count of at least one, and a 32-bit
 *   little-endian pixel, the `0x00RRGGBB` the window protocol carries. **A run
 *   never crosses the end of a row**, so every row is its own runs summing to
 *   exactly the width, and a parser can refuse a file whose rows do not; a run
 *   allowed to cross would let a single miscounted run shift every row after it
 *   sideways by the error, which draws, and draws a picture sheared from that
 *   row down. There is no transparency: a background has nothing behind it, and
 *   a pixel whose top byte is not zero is refused.
 *
 * Why a scaler and not an array of pixels.
 *
 *   The picture is drawn to cover a screen whose size it cannot know, and it is
 *   never held decoded: OxysImageScalerRow produces one row of the screen at a
 *   time, reading forward through the runs, so the memory a program spends is
 *   one row of the picture and one of the screen, whatever the size of either.
 */

#ifndef OXYS_IMAGE_H
#define OXYS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The largest image this reads, in each direction, and the largest screen row
 * a scaler produces. It is the window manager's own bound on a window's extent,
 * WINDOW_MAXIMUM_EXTENT, so that nothing a window can be is something a scaler
 * cannot fill.
 */
#define IMAGE_EXTENT_MAXIMUM 4096U

#define IMAGE_HEADER_BYTES 12U
#define IMAGE_RUN_BYTES    6U

#define IMAGE_MAGIC   "OXIM"
#define IMAGE_VERSION 1U

/*
 * One image, parsed. It holds no pixels: `runs` points into the bytes the
 * caller parsed, which must outlive it. Parsing walks every run once to judge
 * the file, so that the scaler may trust it and need refuse nothing.
 */
typedef struct OxysImage
{
    uint32_t width;
    uint32_t height;
    const uint8_t *runs;
    size_t run_count;
} OxysImage;

/*
 * Reads `count` bytes as an image. False where they are not one, and the image
 * is left empty. Every field is judged — the magic, the version, the reserved
 * bytes, an extent of zero or beyond the bound, a run of zero, a run crossing
 * the end of its row, a pixel with a top byte, and bytes left over after the
 * last row or missing before it.
 */
bool OxysImageParse(OxysImage *image, const void *bytes, size_t count);

uint32_t OxysImageWidth(const OxysImage *image);
uint32_t OxysImageHeight(const OxysImage *image);

/*
 * The state of one scaling: the image fitted to cover `width` by `height`,
 * preserving its proportions, centred, and cut where it overhangs — rather
 * than stretched, which would make the drawing's disc an ellipse upon every
 * screen of another shape than the drawing's, or letterboxed, which would put
 * bars of some colour the drawing never had at two of its edges.
 *
 * It is large — a row of the image and three sums for every column of the
 * screen — and a program keeps it static. Its fields are the library's.
 */
typedef struct OxysImageScaler
{
    const OxysImage *image;
    uint32_t width;
    uint32_t height;

    /* The part of the image that is shown, in the image's pixels. */
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;

    /* The next row of the screen to produce. */
    uint32_t row;

    /* Where the reading stands: the next run, and the image row it begins,
     * and which image row `decoded` holds. */
    size_t next_run;
    uint32_t next_row;
    uint32_t decoded_row;
    bool decoded_valid;

    /* The first image column beneath each screen column, and one past the
     * last: computed once, since every row of the screen shares them. */
    uint32_t column_start[IMAGE_EXTENT_MAXIMUM + 1U];

    uint32_t decoded[IMAGE_EXTENT_MAXIMUM];
    uint64_t sum[3][IMAGE_EXTENT_MAXIMUM];
} OxysImageScaler;

/*
 * Begins drawing the image to cover `width` by `height`. False where the image
 * is empty or the extent is zero or beyond IMAGE_EXTENT_MAXIMUM.
 */
bool OxysImageScalerBegin(OxysImageScaler *scaler, const OxysImage *image, uint32_t width,
                          uint32_t height);

/*
 * Writes the next row of the screen, `width` pixels of `0x00RRGGBB`, into
 * `pixels`. False once every row has been produced. Each pixel is the average
 * of the image's pixels beneath it where the image is reduced, and the one
 * pixel beneath it where it is enlarged.
 */
bool OxysImageScalerRow(OxysImageScaler *scaler, uint32_t *pixels);

/*
 * Reads an image from a file into the caller's buffer, and parses it there.
 * The buffer is the caller's because the image points into it for as long as
 * it is used, and because every translation unit of this library is linked into
 * the kernel as well, where a static buffer of a background's size would be a
 * megabyte of the kernel spent upon nothing. A file that does not fit is
 * refused, not truncated.
 */
bool OxysImageRead(OxysImage *image, const char *path, uint8_t *buffer, size_t capacity);

#endif /* OXYS_IMAGE_H */
