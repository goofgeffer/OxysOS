/*
 * File: graphics/draw.c
 * Purpose: Implements the two-dimensional drawing primitives upon a surface:
 *          rectangle arithmetic, the setting of a pixel, filled and outlined
 *          rectangles, the integer line, and the blit.
 * Key functions: GraphicsRectangleIsEmpty, GraphicsRectangleIntersect,
 *          GraphicsRectangleContains, GraphicsSurfaceInitialise,
 *          GraphicsSurfaceFromFramebuffer, GraphicsSetClip, GraphicsResetClip,
 *          GraphicsPutPixel, GraphicsPixelAt, GraphicsFillRectangle,
 *          GraphicsDrawRectangle, GraphicsClear, GraphicsDrawLine, GraphicsBlit.
 * References:
 *   - J. E. Bresenham, "Algorithm for computer control of a digital plotter",
 *     IBM Systems Journal 4(1), pages 25 to 30, 1965: the line algorithm, which
 *     decides each step from an accumulated integer error and uses no division.
 *   - PROJECT_GUIDELINES.md, Section 8: floating point is prohibited in the
 *     kernel, which is why every calculation here is integer.
 *   - docs/design/GRAPHICS.md, Sections 11 to 16: the design and its limits.
 *
 * The rule that governs this file.
 *
 * Clipping is not a convenience and is not an optimisation. Every one of these
 * routines computes a byte offset into a surface and writes to it, so a shape
 * that escapes its bounds writes into whatever the arena mapped next. Clipping is
 * therefore the memory-safety boundary of the whole graphical stack, and it is
 * implemented once, in the intersection below, rather than being restated by
 * every primitive.
 *
 * The shape is intersected with the clip first, and the surviving span is then
 * written with no test in the loop at all. That is faster than testing each
 * pixel, but the reason it is written this way is that the bound is computed
 * once, from arithmetic that has been checked once, in a place a reader can look
 * at. A loop that tested each pixel would be correct only for as long as every
 * one of its tests remained correct.
 *
 * Concurrency. Nothing here is safe against two processors drawing upon one
 * surface. From sub-task 6.13 that requires the lock which that sub-task
 * introduces; it is not taken here, a primitive being far too small a thing to
 * own a lock.
 */

#include <oxys/graphics.h>
#include <oxys/framebuffer.h>
#include <oxys/kernel.h>

/* ------------------------------------------------------------------------------
 * Rectangles.
 *
 * The edges are computed in 64 bits throughout. The inputs are bounded by
 * GRAPHICS_COORDINATE_LIMIT, so a sum of two of them cannot approach the range
 * of a 32-bit integer either; the wider type is used because a rectangle may
 * also be built from a surface's unsigned extent, and mixing a signed origin
 * with an unsigned extent in 32 bits is where such arithmetic goes wrong.
 * ------------------------------------------------------------------------------ */

bool GraphicsRectangleIsEmpty(GraphicsRectangle rectangle)
{
    return (rectangle.width <= 0) || (rectangle.height <= 0);
}

GraphicsRectangle GraphicsRectangleIntersect(GraphicsRectangle first,
                                             GraphicsRectangle second)
{
    const GraphicsRectangle empty = { 0, 0, 0, 0 };
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (GraphicsRectangleIsEmpty(first) || GraphicsRectangleIsEmpty(second))
    {
        return empty;
    }

    left = (first.x > second.x) ? first.x : second.x;
    top = (first.y > second.y) ? first.y : second.y;

    right = (int64_t)first.x + first.width;
    if (((int64_t)second.x + second.width) < right)
    {
        right = (int64_t)second.x + second.width;
    }

    bottom = (int64_t)first.y + first.height;
    if (((int64_t)second.y + second.height) < bottom)
    {
        bottom = (int64_t)second.y + second.height;
    }

    if ((right <= left) || (bottom <= top))
    {
        return empty;
    }

    {
        const GraphicsRectangle result = { (int32_t)left, (int32_t)top,
                                           (int32_t)(right - left),
                                           (int32_t)(bottom - top) };
        return result;
    }
}

bool GraphicsRectangleContains(GraphicsRectangle rectangle, int32_t x, int32_t y)
{
    if (GraphicsRectangleIsEmpty(rectangle))
    {
        return false;
    }

    return (x >= rectangle.x) && (y >= rectangle.y) &&
           ((int64_t)x < ((int64_t)rectangle.x + rectangle.width)) &&
           ((int64_t)y < ((int64_t)rectangle.y + rectangle.height));
}

/* Whether a coordinate is one this file will act upon at all. */
static bool GraphicsCoordinateIsSound(int64_t value)
{
    return (value >= -GRAPHICS_COORDINATE_LIMIT) && (value <= GRAPHICS_COORDINATE_LIMIT);
}

/* ------------------------------------------------------------------------------
 * Surfaces.
 * ------------------------------------------------------------------------------ */

GraphicsRectangle GraphicsSurfaceBounds(const GraphicsSurface *surface)
{
    const GraphicsRectangle bounds = { 0, 0, (int32_t)surface->width,
                                       (int32_t)surface->height };
    return bounds;
}

bool GraphicsSurfaceInitialise(GraphicsSurface *surface, void *pixels, uint32_t width,
                               uint32_t height, uint32_t pitch, uint8_t bytes_per_pixel)
{
    if ((surface == NULL) || (pixels == NULL) || (width == 0U) || (height == 0U) ||
        (bytes_per_pixel == 0U))
    {
        return false;
    }

    /*
     * The extent must be describable in the signed coordinates every primitive
     * uses. A surface wider than the coordinate limit could hold pixels no
     * argument to any routine here could name, and the arithmetic that clipped
     * against it would be comparing quantities of different kinds.
     */
    if ((width > (uint32_t)GRAPHICS_COORDINATE_LIMIT) ||
        (height > (uint32_t)GRAPHICS_COORDINATE_LIMIT))
    {
        return false;
    }

    /* A pitch below the occupied width would make every row overlap the one
     * before it: the second row would begin inside the first. */
    if ((uint64_t)pitch < ((uint64_t)width * bytes_per_pixel))
    {
        return false;
    }

    surface->pixels = (volatile uint8_t *)pixels;
    surface->width = width;
    surface->height = height;
    surface->pitch = pitch;
    surface->bytes_per_pixel = bytes_per_pixel;
    surface->clip = GraphicsSurfaceBounds(surface);

    return true;
}

bool GraphicsSurfaceFromFramebuffer(GraphicsSurface *surface)
{
    if (!FramebufferIsGraphical())
    {
        return false;
    }

    return GraphicsSurfaceInitialise(surface, (void *)FramebufferAddress(),
                                     FramebufferWidth(), FramebufferHeight(),
                                     FramebufferPitch(), FramebufferBytesPerPixel());
}

void GraphicsSetClip(GraphicsSurface *surface, GraphicsRectangle region)
{
    surface->clip = GraphicsRectangleIntersect(region, GraphicsSurfaceBounds(surface));
}

void GraphicsResetClip(GraphicsSurface *surface)
{
    surface->clip = GraphicsSurfaceBounds(surface);
}

GraphicsRectangle GraphicsClip(const GraphicsSurface *surface)
{
    return surface->clip;
}

/* The address of a pixel. The caller has already established that the
 * coordinates lie within the surface; this performs no checking, because every
 * caller of it is a loop whose bounds were computed from the clip. */
static volatile uint8_t *GraphicsPixelAddress(const GraphicsSurface *surface, int32_t x,
                                              int32_t y)
{
    return surface->pixels + ((uint64_t)(uint32_t)y * surface->pitch) +
           ((uint64_t)(uint32_t)x * surface->bytes_per_pixel);
}

/* Writes one pixel of the surface's depth. The bytes are written in increasing
 * order, which is the order the little-endian pixel is stored in. */
static void GraphicsStorePixel(const GraphicsSurface *surface, volatile uint8_t *at,
                               uint32_t colour)
{
    for (uint8_t index = 0U; index < surface->bytes_per_pixel; ++index)
    {
        at[index] = (uint8_t)((colour >> (index * 8U)) & 0xFFU);
    }
}

void GraphicsPutPixel(GraphicsSurface *surface, int32_t x, int32_t y, uint32_t colour)
{
    if (!GraphicsRectangleContains(surface->clip, x, y))
    {
        return;
    }

    GraphicsStorePixel(surface, GraphicsPixelAddress(surface, x, y), colour);
}

uint32_t GraphicsPixelAt(const GraphicsSurface *surface, int32_t x, int32_t y)
{
    const volatile uint8_t *at;
    uint32_t colour = 0U;

    if (!GraphicsRectangleContains(GraphicsSurfaceBounds(surface), x, y))
    {
        return 0U;
    }

    at = GraphicsPixelAddress(surface, x, y);

    for (uint8_t index = 0U; index < surface->bytes_per_pixel; ++index)
    {
        colour |= (uint32_t)at[index] << (index * 8U);
    }

    return colour;
}

void GraphicsFillRectangle(GraphicsSurface *surface, GraphicsRectangle rectangle,
                           uint32_t colour)
{
    const GraphicsRectangle area = GraphicsRectangleIntersect(rectangle, surface->clip);

    if (GraphicsRectangleIsEmpty(area))
    {
        return;
    }

    /*
     * The clip has been applied once, above. Nothing within these loops tests
     * anything: every coordinate they produce was proved to lie within the
     * surface by the intersection, and the surface's own pitch was proved to
     * cover a row when it was described.
     */
    for (int32_t row = 0; row < area.height; ++row)
    {
        volatile uint8_t *at = GraphicsPixelAddress(surface, area.x, area.y + row);

        for (int32_t column = 0; column < area.width; ++column)
        {
            GraphicsStorePixel(surface, at, colour);
            at += surface->bytes_per_pixel;
        }
    }
}

void GraphicsClear(GraphicsSurface *surface, uint32_t colour)
{
    GraphicsFillRectangle(surface, surface->clip, colour);
}

void GraphicsDrawRectangle(GraphicsSurface *surface, GraphicsRectangle rectangle,
                           uint32_t colour)
{
    GraphicsRectangle edge;

    if (GraphicsRectangleIsEmpty(rectangle))
    {
        return;
    }

    /*
     * The outline is four filled rectangles, each of which is clipped by the
     * fill. Drawing it as four fills rather than as four runs of pixels means the
     * clipping is performed by the routine that already does it correctly, and
     * that a rectangle whose top edge alone is visible costs one clipped fill and
     * three that intersect to nothing.
     *
     * The left and right edges are shortened by the two horizontal edges so that
     * the corners are written once rather than twice. Writing a corner twice is
     * harmless for an opaque colour and would not be once sub-task 6.6 admits
     * blending.
     */
    edge.x = rectangle.x;
    edge.y = rectangle.y;
    edge.width = rectangle.width;
    edge.height = 1;
    GraphicsFillRectangle(surface, edge, colour);

    if (rectangle.height == 1)
    {
        return;
    }

    edge.y = (int32_t)((int64_t)rectangle.y + rectangle.height - 1);
    GraphicsFillRectangle(surface, edge, colour);

    edge.x = rectangle.x;
    edge.y = rectangle.y + 1;
    edge.width = 1;
    edge.height = rectangle.height - 2;

    if (edge.height > 0)
    {
        GraphicsFillRectangle(surface, edge, colour);

        if (rectangle.width > 1)
        {
            edge.x = (int32_t)((int64_t)rectangle.x + rectangle.width - 1);
            GraphicsFillRectangle(surface, edge, colour);
        }
    }
}

void GraphicsDrawLine(GraphicsSurface *surface, int32_t first_x, int32_t first_y,
                      int32_t second_x, int32_t second_y, uint32_t colour)
{
    int32_t x = first_x;
    int32_t y = first_y;
    int32_t delta_x;
    int32_t delta_y;
    int32_t step_x;
    int32_t step_y;
    int32_t error;

    if (!GraphicsCoordinateIsSound(first_x) || !GraphicsCoordinateIsSound(first_y) ||
        !GraphicsCoordinateIsSound(second_x) || !GraphicsCoordinateIsSound(second_y))
    {
        return;
    }

    if (GraphicsRectangleIsEmpty(surface->clip))
    {
        return;
    }

    delta_x = (second_x > first_x) ? (second_x - first_x) : (first_x - second_x);
    delta_y = (second_y > first_y) ? (second_y - first_y) : (first_y - second_y);
    step_x = (first_x < second_x) ? 1 : -1;
    step_y = (first_y < second_y) ? 1 : -1;
    error = delta_x - delta_y;

    /*
     * Bresenham, with the clip tested per pixel rather than applied to the
     * endpoints first.
     *
     * Clipping the endpoints and then drawing between the clipped ones is the
     * usual arrangement and is faster, and it draws a different line. The
     * algorithm's choice at each step depends upon an error accumulated from the
     * start, so beginning at a different start accumulates a different error and
     * lights, here and there, a different pixel. The visible result is that a
     * shape crossing the edge of a clip is displaced by one pixel where it
     * crosses — which is invisible until two clipped regions meet along a seam
     * and the line through them has a kink in it.
     *
     * Testing each pixel keeps the promise the header makes: the pixels drawn are
     * exactly those of the unclipped line that fall within the clip. The cost is
     * two comparisons per step, and the loop is bounded because the coordinates
     * are, which is what GRAPHICS_COORDINATE_LIMIT is for.
     */
    for (;;)
    {
        if (GraphicsRectangleContains(surface->clip, x, y))
        {
            GraphicsStorePixel(surface, GraphicsPixelAddress(surface, x, y), colour);
        }

        if ((x == second_x) && (y == second_y))
        {
            break;
        }

        /*
         * The doubled error is the whole of the algorithm: it compares the
         * accumulated error against half a step in each axis without ever
         * dividing, which is what makes this exact in integers.
         */
        {
            const int32_t doubled = error * 2;

            if (doubled > -delta_y)
            {
                error -= delta_y;
                x += step_x;
            }

            if (doubled < delta_x)
            {
                error += delta_x;
                y += step_y;
            }
        }
    }
}

bool GraphicsBlit(GraphicsSurface *destination, int32_t x, int32_t y,
                  const GraphicsSurface *source, GraphicsRectangle area)
{
    GraphicsRectangle taken;
    GraphicsRectangle placed;
    int64_t offset_x;
    int64_t offset_y;
    bool backwards_rows;
    bool backwards_columns;

    if ((destination == NULL) || (source == NULL) || (destination->pixels == NULL) ||
        (source->pixels == NULL))
    {
        return false;
    }

    /*
     * Both surfaces must agree upon the size of a pixel.
     *
     * This is refused rather than converted. A blit between depths is a colour
     * conversion, and performing one byte by byte would produce an image of
     * exactly the right size in entirely the wrong colours — which looks like a
     * fault in the drawing and is a fault in the caller.
     */
    if (destination->bytes_per_pixel != source->bytes_per_pixel)
    {
        return false;
    }

    if (!GraphicsCoordinateIsSound(x) || !GraphicsCoordinateIsSound(y))
    {
        return false;
    }

    /* What may be read, and then what may be written; the second is the first
     * moved to its destination and clipped there. */
    taken = GraphicsRectangleIntersect(area, GraphicsSurfaceBounds(source));

    if (GraphicsRectangleIsEmpty(taken))
    {
        return true;
    }

    offset_x = (int64_t)x + (taken.x - area.x);
    offset_y = (int64_t)y + (taken.y - area.y);

    if (!GraphicsCoordinateIsSound(offset_x) || !GraphicsCoordinateIsSound(offset_y))
    {
        return false;
    }

    {
        const GraphicsRectangle wanted = { (int32_t)offset_x, (int32_t)offset_y,
                                           taken.width, taken.height };
        placed = GraphicsRectangleIntersect(wanted, destination->clip);
    }

    if (GraphicsRectangleIsEmpty(placed))
    {
        return true;
    }

    /* The clip may have trimmed the destination, and the source must be trimmed
     * by exactly as much or the copy would be displaced. */
    taken.x += placed.x - (int32_t)offset_x;
    taken.y += placed.y - (int32_t)offset_y;
    taken.width = placed.width;
    taken.height = placed.height;

    /*
     * The direction of the copy.
     *
     * Where the two surfaces are one and the regions overlap, copying forwards
     * would read bytes the copy had already overwritten, and the image would
     * smear in the direction of the move. The rows are therefore taken from the
     * bottom where the destination lies below the source, and each row is copied
     * from its right where the destination lies to the right upon the same row.
     *
     * This is what a console scrolls with in sub-task 6.4, and scrolling is
     * precisely the overlapping case: the whole screen moved up by one row of
     * text.
     */
    backwards_rows = (destination->pixels == source->pixels) && (placed.y > taken.y);
    backwards_columns = (destination->pixels == source->pixels) &&
                        (placed.y == taken.y) && (placed.x > taken.x);

    for (int32_t row = 0; row < placed.height; ++row)
    {
        const int32_t source_row = backwards_rows ? (placed.height - 1 - row) : row;
        const volatile uint8_t *from =
            GraphicsPixelAddress(source, taken.x, taken.y + source_row);
        volatile uint8_t *to =
            GraphicsPixelAddress(destination, placed.x, placed.y + source_row);
        const uint64_t span = (uint64_t)placed.width * destination->bytes_per_pixel;

        if (backwards_columns)
        {
            for (uint64_t index = span; index > 0U; --index)
            {
                to[index - 1U] = from[index - 1U];
            }
        }
        else
        {
            for (uint64_t index = 0U; index < span; ++index)
            {
                to[index] = from[index];
            }
        }
    }

    return true;
}

void GraphicsReport(void)
{
    GraphicsSurface surface;

    if (!GraphicsSurfaceFromFramebuffer(&surface))
    {
        KernelWriteString("Graphics: no surface; the framebuffer is not one that may "
                          "be drawn upon.\n");
        return;
    }

    KernelWriteString("Graphics: surface ");
    KernelWriteDecimal((uint64_t)surface.width);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)surface.height);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)surface.bytes_per_pixel);
    KernelWriteString(" bytes per pixel, pitch ");
    KernelWriteDecimal((uint64_t)surface.pitch);
    KernelWriteString(", clip the whole of it.\n");
}
