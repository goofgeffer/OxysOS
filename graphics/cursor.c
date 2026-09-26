/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: graphics/cursor.c
 * Purpose: Implements the pointer: the shape drawn for this project, the small
 *          surface and coverage mask it is rendered into once, and the
 *          compositor layer through which it appears over everything else.
 * Key functions: CursorInitialise, CursorShow, CursorHide, CursorMoveTo,
 *          CursorIsVisible, CursorX, CursorY, CursorShapeIsOpaque,
 *          CursorShapeIsInterior, CursorReport.
 * References:
 *   - docs/design/COMPOSITOR.md: the shape and the two-mask encoding
 *     of it; Section 27.4: what sub-task 6.6 removed from this file and why.
 *   - docs/devices/MOUSE.md: the pointer as the mouse driver
 *     sees it, and every assertion made upon it.
 *   - PROJECT_GUIDELINES.md, Section 2: no code or artwork is copied. The shape
 *     below was drawn for this project, in the same way and for the same reason
 *     as the face of sub-task 6.4.
 *
 * The shape.
 *
 *   Two bitmaps rather than one, because a pointer needs three states and one
 *   bitmap offers two. A pixel is transparent, or it is the outline, or it is
 *   the interior; the first mask says which pixels the pointer covers at all and
 *   the second, among those, which are interior.
 *
 *   That is what makes a pointer visible upon any background. A white arrow
 *   vanishes upon white and a black one upon black, and an arrow that is white
 *   within a black outline vanishes upon neither. The alternative — one colour
 *   and a fixed background — is a pointer that works upon the boot log and
 *   disappears the moment anything is drawn.
 *
 * What sub-task 6.6 took out of this file.
 *
 *   Until the compositor there was one surface, so what the pointer covered had
 *   to be read back before it was drawn and written again before it was drawn
 *   elsewhere. That store was correct only while nothing else drew, so the rest
 *   of the kernel had to declare that it was about to — KernelWriteString
 *   concealed a pointer it had no business knowing existed, the concealment had
 *   to nest, and the fault screens had to hide what they could not reveal.
 *
 *   None of it remains. The pointer is rendered once into a surface of its own
 *   with a coverage mask beside it, and the compositor composes it over the back
 *   buffer as the changed region is carried to the display. What is beneath the
 *   pointer is never overwritten, so there is nothing to save, nothing to
 *   restore and nothing to declare. What survived is what Section 26.5
 *   predicted would: the shape, the two-mask encoding, and a position that
 *   belongs to the mouse driver and is merely reflected here.
 *
 * Concurrency. The pointer is moved by whoever drains the mouse's event buffer,
 * which is not an interrupt handler. Nothing else touches this file's state, the
 * concealment that two parties shared having gone with the save-under.
 */

#include <oxys/gfx/cursor.h>
#include <oxys/gfx/compositor.h>
#include <oxys/gfx/framebuffer.h>
#include <oxys/gfx/graphics.h>
#include <oxys/kernel.h>

/*
 * The shape is the project owner's pointer since 2026-09-26: art/pointer.h,
 * generated from art/pointer.png, holds the arrow's coverage and a halo's, one
 * byte to a pixel. The arrow is drawn in the interior colour and the halo in
 * the outline colour, so the pointer is still visible upon anything, which is
 * the reason the hand-drawn shape before it had two masks. A pixel covered by
 * both is drawn in the colour of whichever covers more of it.
 */

/*
 * The pointer, rendered once.
 *
 * The two bitmaps above are the shape as a person edits it; these are the shape
 * as the compositor consumes it — a surface of pixels and a byte of coverage
 * beside each. The conversion happens once, at initialisation, because the shape
 * does not change and a compositor that re-derived it from the bitmaps at every
 * presentation would be doing the same arithmetic sixty times a second to reach
 * the same answer.
 *
 * The coverage is a byte and not a bit, so that a shape with a soft edge needs
 * no change here — only a table with values between, which the owner's
 * pointer of 2026-09-26 is.
 */
static uint32_t CursorPixels[CURSOR_HEIGHT * CURSOR_WIDTH];
static uint8_t CursorCoverage[CURSOR_HEIGHT * CURSOR_WIDTH];
static GraphicsSurface CursorImage;
static bool CursorAvailable;

/* The layer the compositor knows the pointer by. */
static size_t CursorLayer = COMPOSITOR_LAYER_NONE;

/* The two colours, encoded for the display. */
static uint32_t CursorOutlineColour;
static uint32_t CursorInteriorColour;

/* Where the pointer is, and whether the operator is meant to see it. */
static int32_t CursorPositionX;
static int32_t CursorPositionY;
static bool CursorVisible;

/* Accounting: how many times the pointer has actually moved, which is not how
 * many packets the mouse sent — a movement that ends where it began costs
 * nothing and is counted as nothing. */
static uint64_t CursorMoves;

/* Whether the pixel at this column and row is covered by the pointer at all. */
bool CursorShapeIsOpaque(int32_t column, int32_t row)
{
    return CursorShapeCoverage(column, row) != 0U;
}

bool CursorShapeIsInterior(int32_t column, int32_t row)
{
    size_t index;

    if ((column < 0) || (column >= CURSOR_WIDTH) || (row < 0) || (row >= CURSOR_HEIGHT))
    {
        return false;
    }

    index = ((size_t)row * CURSOR_WIDTH) + (size_t)column;

    return (PointerBody[index] != 0U) && (PointerBody[index] >= PointerHalo[index]);
}

uint8_t CursorShapeCoverage(int32_t column, int32_t row)
{
    size_t index;
    uint32_t sum;

    if ((column < 0) || (column >= CURSOR_WIDTH) || (row < 0) || (row >= CURSOR_HEIGHT))
    {
        return 0U;
    }

    index = ((size_t)row * CURSOR_WIDTH) + (size_t)column;
    sum = (uint32_t)PointerBody[index] + (uint32_t)PointerHalo[index];

    return (sum > 255U) ? 255U : (uint8_t)sum;
}

/*
 * Renders the shape into the surface the compositor will draw from.
 *
 * A pixel the pointer does not cover is given a coverage of zero and a colour of
 * zero. The colour of an uncovered pixel is never read — the compositor skips it
 * upon the coverage — but it is set all the same, so that a fault in the mask
 * shows as a black rectangle rather than as whatever the array happened to hold,
 * which is the difference between a visible fault and an intermittent one.
 */
static void CursorRender(void)
{
    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            const size_t index = ((size_t)row * CURSOR_WIDTH) + (size_t)column;

            if (!CursorShapeIsOpaque(column, row))
            {
                CursorPixels[index] = 0U;
                CursorCoverage[index] = 0U;
                continue;
            }

            CursorPixels[index] = CursorShapeIsInterior(column, row) ? CursorInteriorColour
                                                                     : CursorOutlineColour;
            CursorCoverage[index] = CursorShapeCoverage(column, row);
        }
    }
}

bool CursorInitialise(uint32_t outline, uint32_t interior)
{
    CursorAvailable = false;
    CursorVisible = false;
    CursorPositionX = 0;
    CursorPositionY = 0;
    CursorMoves = 0U;
    CursorLayer = COMPOSITOR_LAYER_NONE;
    CursorOutlineColour = outline;
    CursorInteriorColour = interior;

    /*
     * The pointer's own surface is four bytes to the pixel whatever the display
     * is, because it is composed and not scanned out: the compositor reads it a
     * pixel at a time through GraphicsPixelAt and writes the result in the
     * display's format. A surface that matched the display would save nothing
     * and would have to be rebuilt if the mode changed.
     */
    if (!GraphicsSurfaceInitialise(&CursorImage, CursorPixels, CURSOR_WIDTH, CURSOR_HEIGHT,
                                   CURSOR_WIDTH * 4U, 4U))
    {
        return false;
    }

    CursorRender();

    if (!CompositorIsActive())
    {
        /*
         * No compositor, so no pointer. This is not a failure of the machine: it
         * is a machine with no framebuffer, or one whose arena could not supply
         * a back buffer, and upon such a machine there is nothing to point at.
         */
        return false;
    }

    CursorLayer = CompositorAddLayer(&CursorImage, CursorCoverage,
                                     CursorPositionX - CURSOR_HOT_X,
                                     CursorPositionY - CURSOR_HOT_Y);

    if (CursorLayer == COMPOSITOR_LAYER_NONE)
    {
        return false;
    }

    CursorAvailable = true;

    return true;
}

bool CursorIsAvailable(void)
{
    return CursorAvailable;
}

bool CursorIsVisible(void)
{
    return CursorAvailable && CursorVisible;
}

const GraphicsSurface *CursorImageSurface(void)
{
    return CursorAvailable ? &CursorImage : NULL;
}

const uint8_t *CursorImageMask(void)
{
    return CursorCoverage;
}

void CursorShow(void)
{
    if (!CursorAvailable || CursorVisible)
    {
        return;
    }

    CursorVisible = true;
    CompositorSetLayerVisible(CursorLayer, true);
}

void CursorHide(void)
{
    if (!CursorAvailable || !CursorVisible)
    {
        return;
    }

    CursorVisible = false;
    CompositorSetLayerVisible(CursorLayer, false);
}

void CursorMoveTo(int32_t x, int32_t y)
{
    if ((x == CursorPositionX) && (y == CursorPositionY))
    {
        /*
         * A movement that ends where it began is not a movement. The mouse
         * reports at a hundred packets a second and a hand at rest still
         * produces them; without this the display would be told that the
         * pointer's rectangle had changed a hundred times a second for a
         * pointer that had not moved.
         */
        return;
    }

    CursorPositionX = x;
    CursorPositionY = y;
    ++CursorMoves;

    if (CursorAvailable)
    {
        /*
         * The compositor marks both where the pointer was and where it now is.
         * That is the whole of what replaced the save-under, and it is one call
         * rather than a read of 216 pixels followed by a write of them.
         */
        CompositorMoveLayer(CursorLayer, x - CURSOR_HOT_X, y - CURSOR_HOT_Y);
    }
}

int32_t CursorX(void)
{
    return CursorPositionX;
}

int32_t CursorY(void)
{
    return CursorPositionY;
}

uint64_t CursorMoveCount(void)
{
    return CursorMoves;
}

void CursorReport(void)
{
    if (!CursorAvailable)
    {
        KernelWriteString("Pointer: no compositor to draw upon.\n");
        return;
    }

    KernelWriteString("Pointer: ");
    KernelWriteString(CursorVisible ? "shown" : "hidden");
    KernelWriteString(" at ");
    KernelWriteDecimal((uint64_t)(uint32_t)CursorPositionX);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)(uint32_t)CursorPositionY);
    KernelWriteString("; ");
    KernelWriteDecimal((uint64_t)CURSOR_WIDTH);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)CURSOR_HEIGHT);
    KernelWriteString(", composited, moves ");
    KernelWriteDecimal(CursorMoves);
    KernelWriteString(".\n");
}
