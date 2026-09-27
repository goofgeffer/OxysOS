/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: graphics/face.c
 * Purpose: Draws the desktop's face, Inter, from the coverage tables of
 *          fonts/inter/face.h.
 * Key functions: FaceCoverage, FaceDrawGlyph, FaceTextWidth, FaceDrawText.
 * References:
 *   - docs/design/CONSOLE.md: the two faces and where each is drawn.
 *   - LICENSING.md, Section 4: Inter, under the SIL Open Font License 1.1,
 *     which permits it to be bundled with software under any licence.
 *
 * Scales one to four are drawn from tables rendered at exactly their size,
 * which is where a small glyph's shape is decided. Scales five to eight are
 * sampled bilinearly from the scale-four table: magnifying a smooth glyph
 * stays smooth, and four more tables would cost half a mebibyte of image for
 * sizes only a large label uses.
 */

#include <oxys/syscall_abi.h>
#include <oxys/gfx/face.h>
#include <oxys/gfx/graphics.h>

#include <face.h>

_Static_assert(FACE_FIRST_CODE == 0x20U, "the face begins at the space");
_Static_assert(FACE_GLYPHS == 95U, "the face covers the printable ASCII range");
_Static_assert(FACE_SIZES == (uint32_t)FACE_TABLE_SCALE_MAXIMUM, "a table for each exact scale");
_Static_assert((FACE_WIDTH == (int)SYSCALL_WINDOW_TEXT_ADVANCE) &&
                   (FACE_HEIGHT == (int)SYSCALL_WINDOW_TEXT_HEIGHT),
               "the cell a program lays text out by is the cell drawn");

/* The largest table's cell, from which larger scales are sampled. */
#define FACE_LARGEST_WIDTH  (FACE_WIDTH * FACE_TABLE_SCALE_MAXIMUM)
#define FACE_LARGEST_HEIGHT (FACE_HEIGHT * FACE_TABLE_SCALE_MAXIMUM)

/* One pixel of an exact table, which the caller has bounded. */
static uint8_t FaceTable(uint32_t glyph, int32_t scale, int32_t column, int32_t row)
{
    const uint32_t at = (uint32_t)((row * FACE_WIDTH * scale) + column);

    switch (scale)
    {
    case 1:
        return FaceCell1[glyph][at];
    case 2:
        return FaceCell2[glyph][at];
    case 3:
        return FaceCell3[glyph][at];
    default:
        return FaceCell4[glyph][at];
    }
}

/*
 * A pixel's centre in the largest table, in 256ths along an axis `source`
 * pixels long, for a cell `cell` pixels long; clamped, so the edge pixels
 * sample the edge.
 */
static int32_t FaceSource(int32_t position, int32_t cell, int32_t source)
{
    const int32_t at = ((((2 * position) + 1) * source * 256) / (2 * cell)) - 128;
    const int32_t last = (source - 1) * 256;

    return (at < 0) ? 0 : ((at > last) ? last : at);
}

uint8_t FaceCoverage(uint8_t code, int32_t scale, int32_t column, int32_t row)
{
    uint32_t glyph;
    int32_t u;
    int32_t v;
    int32_t u0;
    int32_t v0;
    int32_t u1;
    int32_t v1;
    int32_t fu;
    int32_t fv;
    int32_t top;
    int32_t bottom;

    if ((scale < 1) || (scale > 8) || (column < 0) || (row < 0) ||
        (column >= FACE_WIDTH * scale) || (row >= FACE_HEIGHT * scale) ||
        (code < FACE_FIRST_CODE) || (code >= FACE_FIRST_CODE + FACE_GLYPHS))
    {
        return 0U;
    }

    glyph = (uint32_t)code - FACE_FIRST_CODE;

    if (scale <= FACE_TABLE_SCALE_MAXIMUM)
    {
        return FaceTable(glyph, scale, column, row);
    }

    u = FaceSource(column, FACE_WIDTH * scale, FACE_LARGEST_WIDTH);
    v = FaceSource(row, FACE_HEIGHT * scale, FACE_LARGEST_HEIGHT);
    u0 = u >> 8;
    v0 = v >> 8;
    fu = u & 0xFF;
    fv = v & 0xFF;
    u1 = (u0 + 1 < FACE_LARGEST_WIDTH) ? (u0 + 1) : u0;
    v1 = (v0 + 1 < FACE_LARGEST_HEIGHT) ? (v0 + 1) : v0;

    top = ((int32_t)FaceTable(glyph, FACE_TABLE_SCALE_MAXIMUM, u0, v0) * (256 - fu)) +
          ((int32_t)FaceTable(glyph, FACE_TABLE_SCALE_MAXIMUM, u1, v0) * fu);
    bottom = ((int32_t)FaceTable(glyph, FACE_TABLE_SCALE_MAXIMUM, u0, v1) * (256 - fu)) +
             ((int32_t)FaceTable(glyph, FACE_TABLE_SCALE_MAXIMUM, u1, v1) * fu);

    return (uint8_t)(((top * (256 - fv)) + (bottom * fv) + 32768) >> 16);
}

/* Whether the face has a glyph for the code. */
static bool FaceCovers(uint8_t code)
{
    return (code >= FACE_FIRST_CODE) && (code < FACE_FIRST_CODE + FACE_GLYPHS);
}

/*
 * A glyph's proportional advance, in sixteenths of a pixel, and the offset of
 * its cell from the pen, in pixels. Scales past the tables take the scale-four
 * metrics in proportion, as their coverage is sampled from the scale-four cell.
 * A code the face does not cover advances by a whole cell and stands at the pen.
 */
static int32_t FaceAdvanceOf(uint8_t code, int32_t scale)
{
    if (!FaceCovers(code))
    {
        return FACE_WIDTH * scale * 16;
    }

    if (scale <= FACE_TABLE_SCALE_MAXIMUM)
    {
        return (int32_t)FaceAdvance[scale - 1][code - FACE_FIRST_CODE];
    }

    return ((int32_t)FaceAdvance[FACE_TABLE_SCALE_MAXIMUM - 1][code - FACE_FIRST_CODE] * scale) /
           FACE_TABLE_SCALE_MAXIMUM;
}

static int32_t FaceOffsetOf(uint8_t code, int32_t scale)
{
    if (!FaceCovers(code))
    {
        return 0;
    }

    if (scale <= FACE_TABLE_SCALE_MAXIMUM)
    {
        return (int32_t)FaceOffset[scale - 1][code - FACE_FIRST_CODE];
    }

    return ((int32_t)FaceOffset[FACE_TABLE_SCALE_MAXIMUM - 1][code - FACE_FIRST_CODE] * scale) /
           FACE_TABLE_SCALE_MAXIMUM;
}

/* A glyph's ink alone, blended over what is there; a box for an unknown code. */
static void FaceInkGlyph(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                         uint32_t ink, int32_t scale)
{
    const int32_t width = FACE_WIDTH * scale;
    const int32_t height = FACE_HEIGHT * scale;

    if (!FaceCovers(code))
    {
        /* A hollow box an inset within the cell, as the 8-by-8 face draws. */
        const GraphicsRectangle box = { x + scale, y + scale, width - (2 * scale),
                                        height - (3 * scale) };

        GraphicsDrawRectangle(surface, box, ink);
        return;
    }

    for (int32_t row = 0; row < height; ++row)
    {
        for (int32_t column = 0; column < width; ++column)
        {
            GraphicsBlendPixel(surface, x + column, y + row, ink,
                               FaceCoverage(code, scale, column, row));
        }
    }
}

int32_t FaceTextWidth(const char *text, int32_t scale, bool proportional)
{
    int32_t pen = 0;

    if ((text == NULL) || (scale < 1) || (scale > 8))
    {
        return 0;
    }

    for (const char *at = text; *at != '\0'; ++at)
    {
        pen += proportional ? FaceAdvanceOf((uint8_t)*at, scale) : (FACE_WIDTH * scale * 16);
    }

    return (pen + 15) / 16;
}

int32_t FaceDrawText(GraphicsSurface *surface, int32_t x, int32_t y, const char *text,
                     uint32_t ink, uint32_t paper, int32_t scale, bool proportional)
{
    const int32_t width = FaceTextWidth(text, scale, proportional);
    int32_t pen = 0;

    if ((surface == NULL) || (width == 0))
    {
        return width;
    }

    if (!proportional)
    {
        for (const char *at = text; *at != '\0'; ++at)
        {
            FaceDrawGlyph(surface, x + (pen / 16), y, (uint8_t)*at, ink, paper, scale);
            pen += FACE_WIDTH * scale * 16;
        }

        return width;
    }

    {
        const GraphicsRectangle run = { x, y, width, FACE_HEIGHT * scale };

        GraphicsFillRectangle(surface, run, paper);
    }

    return FaceInkText(surface, x, y, text, ink, scale);
}

int32_t FaceInkText(GraphicsSurface *surface, int32_t x, int32_t y, const char *text,
                    uint32_t ink, int32_t scale)
{
    const int32_t width = FaceTextWidth(text, scale, true);
    int32_t pen = 0;

    if ((surface == NULL) || (width == 0))
    {
        return width;
    }

    for (const char *at = text; *at != '\0'; ++at)
    {
        const uint8_t code = (uint8_t)*at;

        FaceInkGlyph(surface, x + ((pen + 8) / 16) + FaceOffsetOf(code, scale), y, code, ink,
                     scale);
        pen += FaceAdvanceOf(code, scale);
    }

    return width;
}

void FaceDrawGlyph(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code, uint32_t ink,
                   uint32_t paper, int32_t scale)
{
    const int32_t width = FACE_WIDTH * scale;
    const int32_t height = FACE_HEIGHT * scale;
    const GraphicsRectangle whole = { x, y, width, height };

    if ((surface == NULL) || (scale < 1) || (scale > 8))
    {
        return;
    }

    GraphicsFillRectangle(surface, whole, paper);
    FaceInkGlyph(surface, x, y, code, ink, scale);
}
