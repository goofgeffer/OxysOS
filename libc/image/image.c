/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/image/image.c
 * Purpose: Judges the bytes of an image file, and draws a parsed image to
 *          cover a rectangle of any size a row at a time. It opens nothing and
 *          reads nothing.
 * Key functions: OxysImageParse, OxysImageWidth, OxysImageHeight,
 *          OxysImageScalerBegin, OxysImageScalerRow.
 * References:
 *   - libc/include/image.h: the format, and why it is runs.
 *   - docs/design/SESSION.md: what reads images and when.
 *
 * Concurrency. None; a scaler is one program's memory.
 */

#include <image.h>
#include <string.h>

/* Little-endian values from bytes, written out rather than cast, because the
 * file's order is the file's and not this processor's. */
static uint32_t ImageHalf(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

static uint32_t ImageWord(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

bool OxysImageParse(OxysImage *image, const void *bytes, size_t count)
{
    const uint8_t *const source = bytes;
    uint32_t width;
    uint32_t height;
    size_t at = IMAGE_HEADER_BYTES;
    size_t runs = 0U;

    if (image == NULL)
    {
        return false;
    }

    /* Emptied first, so that a refusal leaves nothing of the image before. */
    image->width = 0U;
    image->height = 0U;
    image->runs = NULL;
    image->run_count = 0U;

    if ((source == NULL) || (count < IMAGE_HEADER_BYTES) ||
        (memcmp(source, IMAGE_MAGIC, 4U) != 0) || (source[4] != (uint8_t)IMAGE_VERSION) ||
        (source[5] != 0U) || (source[6] != 0U) || (source[7] != 0U))
    {
        return false;
    }

    width = ImageHalf(&source[8]);
    height = ImageHalf(&source[10]);

    if ((width == 0U) || (height == 0U) || (width > IMAGE_EXTENT_MAXIMUM) ||
        (height > IMAGE_EXTENT_MAXIMUM))
    {
        return false;
    }

    /*
     * Every run is read once here, so that the scaler may trust what it walks.
     * A row must be exactly its runs: one that fell short and let the next
     * row's first run finish it would shear the picture from that row down,
     * and would draw.
     */
    for (uint32_t row = 0U; row < height; ++row)
    {
        uint32_t filled = 0U;

        while (filled < width)
        {
            uint32_t length;

            if ((count - at) < IMAGE_RUN_BYTES)
            {
                return false;
            }

            length = ImageHalf(&source[at]);

            if ((length == 0U) || (length > (width - filled)) ||
                ((ImageWord(&source[at + 2U]) & UINT32_C(0xFF000000)) != 0U))
            {
                return false;
            }

            filled += length;
            at += IMAGE_RUN_BYTES;
            ++runs;
        }
    }

    /* Bytes after the last row are a file this does not understand. */
    if (at != count)
    {
        return false;
    }

    image->width = width;
    image->height = height;
    image->runs = &source[IMAGE_HEADER_BYTES];
    image->run_count = runs;

    return true;
}

uint32_t OxysImageWidth(const OxysImage *image)
{
    return (image == NULL) ? 0U : image->width;
}

uint32_t OxysImageHeight(const OxysImage *image)
{
    return (image == NULL) ? 0U : image->height;
}

bool OxysImageScalerBegin(OxysImageScaler *scaler, const OxysImage *image, uint32_t width,
                          uint32_t height)
{
    if ((scaler == NULL) || (image == NULL) || (image->width == 0U) || (image->height == 0U) ||
        (width == 0U) || (height == 0U) || (width > IMAGE_EXTENT_MAXIMUM) ||
        (height > IMAGE_EXTENT_MAXIMUM))
    {
        return false;
    }

    scaler->image = image;
    scaler->width = width;
    scaler->height = height;

    /*
     * Cover: the image is scaled by whichever of the two ratios is the larger,
     * so that it reaches both edges of the screen, and what overhangs the other
     * two is cut equally from each side. Compared as products rather than as
     * quotients, so that no ratio is rounded before it is compared.
     */
    if (((uint64_t)width * image->height) >= ((uint64_t)height * image->width))
    {
        scaler->crop_width = image->width;
        scaler->crop_height =
            (uint32_t)(((uint64_t)image->width * height + (width / 2U)) / width);

        if (scaler->crop_height == 0U)
        {
            scaler->crop_height = 1U;
        }

        if (scaler->crop_height > image->height)
        {
            scaler->crop_height = image->height;
        }
    }
    else
    {
        scaler->crop_height = image->height;
        scaler->crop_width =
            (uint32_t)(((uint64_t)image->height * width + (height / 2U)) / height);

        if (scaler->crop_width == 0U)
        {
            scaler->crop_width = 1U;
        }

        if (scaler->crop_width > image->width)
        {
            scaler->crop_width = image->width;
        }
    }

    scaler->crop_x = (image->width - scaler->crop_width) / 2U;
    scaler->crop_y = (image->height - scaler->crop_height) / 2U;

    for (uint32_t column = 0U; column <= width; ++column)
    {
        scaler->column_start[column] =
            scaler->crop_x +
            (uint32_t)(((uint64_t)column * scaler->crop_width) / width);
    }

    scaler->row = 0U;
    scaler->next_run = 0U;
    scaler->next_row = 0U;
    scaler->decoded_row = 0U;
    scaler->decoded_valid = false;

    return true;
}

/* Decodes image rows forward until `decoded` holds `row`. The scaler asks for
 * rows in an order that never goes back, so nothing is ever decoded twice. */
static void ImageDecodeTo(OxysImageScaler *scaler, uint32_t row)
{
    const OxysImage *const image = scaler->image;

    if (scaler->decoded_valid && (scaler->decoded_row == row))
    {
        return;
    }

    while (scaler->next_row <= row)
    {
        uint32_t filled = 0U;

        while (filled < image->width)
        {
            const uint8_t *const run = &image->runs[scaler->next_run * IMAGE_RUN_BYTES];
            const uint32_t length = ImageHalf(run);
            const uint32_t pixel = ImageWord(&run[2]);

            for (uint32_t index = 0U; index < length; ++index)
            {
                scaler->decoded[filled + index] = pixel;
            }

            filled += length;
            ++scaler->next_run;
        }

        scaler->decoded_row = scaler->next_row;
        scaler->decoded_valid = true;
        ++scaler->next_row;
    }
}

bool OxysImageScalerRow(OxysImageScaler *scaler, uint32_t *pixels)
{
    uint32_t first;
    uint32_t last;

    if ((scaler == NULL) || (pixels == NULL) || (scaler->image == NULL) ||
        (scaler->row >= scaler->height))
    {
        return false;
    }

    first = scaler->crop_y +
            (uint32_t)(((uint64_t)scaler->row * scaler->crop_height) / scaler->height);
    last = scaler->crop_y +
           (uint32_t)(((uint64_t)(scaler->row + 1U) * scaler->crop_height) / scaler->height);

    /* Enlarged, a screen row lies within one image row, which is repeated. */
    if (last <= first)
    {
        last = first + 1U;
    }

    for (uint32_t column = 0U; column < scaler->width; ++column)
    {
        scaler->sum[0][column] = 0U;
        scaler->sum[1][column] = 0U;
        scaler->sum[2][column] = 0U;
    }

    for (uint32_t row = first; row < last; ++row)
    {
        ImageDecodeTo(scaler, row);

        for (uint32_t column = 0U; column < scaler->width; ++column)
        {
            uint32_t from = scaler->column_start[column];
            uint32_t to = scaler->column_start[column + 1U];

            if (to <= from)
            {
                to = from + 1U;
            }

            for (uint32_t at = from; at < to; ++at)
            {
                const uint32_t pixel = scaler->decoded[at];

                scaler->sum[0][column] += (pixel >> 16) & 0xFFU;
                scaler->sum[1][column] += (pixel >> 8) & 0xFFU;
                scaler->sum[2][column] += pixel & 0xFFU;
            }
        }
    }

    for (uint32_t column = 0U; column < scaler->width; ++column)
    {
        uint32_t from = scaler->column_start[column];
        uint32_t to = scaler->column_start[column + 1U];
        uint64_t area;

        if (to <= from)
        {
            to = from + 1U;
        }

        area = (uint64_t)(to - from) * (uint64_t)(last - first);

        pixels[column] = (uint32_t)(((scaler->sum[0][column] + (area / 2U)) / area) << 16) |
                         (uint32_t)(((scaler->sum[1][column] + (area / 2U)) / area) << 8) |
                         (uint32_t)((scaler->sum[2][column] + (area / 2U)) / area);
    }

    ++scaler->row;

    return true;
}
