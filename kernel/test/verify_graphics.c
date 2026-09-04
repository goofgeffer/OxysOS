/*
 * File: kernel/test/verify_graphics.c
 * Purpose: Asserts the two-dimensional primitives of sub-task 6.3 — the
 *          rectangle arithmetic, the clip, the pixel, the filled and outlined
 *          rectangles, the line and the blit — against a surface composed in
 *          ordinary memory, so that every assertion holds upon a machine with no
 *          display at all.
 * Key functions: KernelVerifyGraphics.
 * References:
 *   - docs/design/GRAPHICS.md, Section 16: every assertion below, paired with
 *     the silent failure it catches.
 *   - J. E. Bresenham, IBM Systems Journal 4(1), 1965: the line whose exactness
 *     under clipping is asserted here.
 *
 * Why the surface is in memory and not the framebuffer.
 *
 * A primitive is judged by which pixels it set and which it left alone, and that
 * is a question the framebuffer cannot answer well: it may not exist, it is slow
 * to read back through a write-combining mapping, and its extent is whatever the
 * boot loader chose rather than a size chosen to make an assertion sharp.
 *
 * The surface below is a small array whose every pixel is read back. Its width
 * is deliberately not its pitch, so that any routine computing an address as
 * width times the pixel size instead of by the pitch writes into the padding and
 * is caught; and the padding is filled with a sentinel that no test ever writes,
 * so that a stray write into it is detected wherever it came from.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/graphics.h>
#include <oxys/framebuffer.h>

static bool KernelGraphicsSucceeded;

static void KernelGraphicsRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelGraphicsSucceeded = false;
    }
}

/*
 * The test surface: 32 by 16 pixels of four bytes, in rows of 40 pixels.
 *
 * The pitch exceeds the width by eight pixels and that is the point of it. A
 * primitive that stepped from row to row by the width rather than by the pitch
 * would still write inside this array — it would simply write the wrong pixels —
 * and every assertion upon the image would then have to be relied upon to catch
 * it. The padding turns that into a direct assertion instead.
 */
#define KERNEL_SURFACE_WIDTH   32U
#define KERNEL_SURFACE_HEIGHT  16U
#define KERNEL_SURFACE_STRIDE  40U
#define KERNEL_SURFACE_DEPTH   4U
#define KERNEL_SURFACE_PITCH   (KERNEL_SURFACE_STRIDE * KERNEL_SURFACE_DEPTH)

/* A colour no test writes, so that finding it proves nothing touched the byte
 * and finding anything else in the padding proves something did. */
#define KERNEL_SURFACE_SENTINEL UINT32_C(0xA5A5A5A5)

static uint8_t KernelSurfaceStore[KERNEL_SURFACE_PITCH * KERNEL_SURFACE_HEIGHT];
static uint8_t KernelSecondStore[KERNEL_SURFACE_PITCH * KERNEL_SURFACE_HEIGHT];

static GraphicsSurface KernelSurface;
static GraphicsSurface KernelSecondSurface;

/* Fills the whole store, padding included, with the sentinel. */
static void KernelSurfaceReset(uint8_t *store)
{
    for (size_t index = 0U; index < (KERNEL_SURFACE_PITCH * KERNEL_SURFACE_HEIGHT);
         index += 4U)
    {
        store[index + 0U] = (uint8_t)(KERNEL_SURFACE_SENTINEL & 0xFFU);
        store[index + 1U] = (uint8_t)((KERNEL_SURFACE_SENTINEL >> 8U) & 0xFFU);
        store[index + 2U] = (uint8_t)((KERNEL_SURFACE_SENTINEL >> 16U) & 0xFFU);
        store[index + 3U] = (uint8_t)((KERNEL_SURFACE_SENTINEL >> 24U) & 0xFFU);
    }
}

/*
 * Whether the padding beyond each row still holds the sentinel.
 *
 * This is the assertion that no primitive addressed a row by the width instead
 * of by the pitch, and it is made after every test rather than once at the end,
 * so that a failure names the operation that caused it.
 */
static bool KernelPaddingIsIntact(const uint8_t *store)
{
    for (uint32_t row = 0U; row < KERNEL_SURFACE_HEIGHT; ++row)
    {
        for (uint32_t column = KERNEL_SURFACE_WIDTH; column < KERNEL_SURFACE_STRIDE;
             ++column)
        {
            const size_t at = (row * KERNEL_SURFACE_PITCH) + (column * KERNEL_SURFACE_DEPTH);
            uint32_t value = 0U;

            for (uint32_t index = 0U; index < 4U; ++index)
            {
                value |= (uint32_t)store[at + index] << (index * 8U);
            }

            if (value != KERNEL_SURFACE_SENTINEL)
            {
                return false;
            }
        }
    }

    return true;
}

/* The number of pixels of the surface holding the given colour. */
static uint32_t KernelCountColour(const GraphicsSurface *surface, uint32_t colour)
{
    uint32_t count = 0U;

    for (int32_t y = 0; y < (int32_t)surface->height; ++y)
    {
        for (int32_t x = 0; x < (int32_t)surface->width; ++x)
        {
            if (GraphicsPixelAt(surface, x, y) == colour)
            {
                ++count;
            }
        }
    }

    return count;
}

/* Asserts the rectangle arithmetic every primitive clips with. */
static void KernelVerifyGraphicsRectangles(void)
{
    const GraphicsRectangle a = { 10, 10, 20, 20 };
    const GraphicsRectangle b = { 20, 20, 20, 20 };
    const GraphicsRectangle apart = { 100, 100, 5, 5 };
    const GraphicsRectangle degenerate = { 0, 0, 0, 10 };
    const GraphicsRectangle negative = { 5, 5, -3, 10 };
    GraphicsRectangle meeting;

    KernelGraphicsRequire(!GraphicsRectangleIsEmpty(a), "a rectangle of extent is empty");
    KernelGraphicsRequire(GraphicsRectangleIsEmpty(degenerate),
                          "a rectangle of no width is not empty");
    KernelGraphicsRequire(GraphicsRectangleIsEmpty(negative),
                          "a rectangle of negative width is not empty, so a caller could "
                          "ask for a region that runs backwards");

    meeting = GraphicsRectangleIntersect(a, b);
    KernelGraphicsRequire((meeting.x == 20) && (meeting.y == 20) && (meeting.width == 10) &&
                              (meeting.height == 10),
                          "the intersection of two overlapping rectangles is wrong");

    meeting = GraphicsRectangleIntersect(a, apart);
    KernelGraphicsRequire(GraphicsRectangleIsEmpty(meeting),
                          "rectangles that do not meet intersect to something");

    /*
     * An intersection that misses must be empty and must not be negative. A
     * negative extent would pass a loop bound of "less than width" by doing
     * nothing, and would fail one computed as an end coordinate.
     */
    KernelGraphicsRequire((meeting.width >= 0) && (meeting.height >= 0),
                          "an empty intersection has a negative extent");

    /* Touching edges do not overlap: a rectangle ending at x is disjoint from
     * one beginning at x, or every adjacent pair of regions would share a
     * column. */
    {
        const GraphicsRectangle left = { 0, 0, 10, 10 };
        const GraphicsRectangle right = { 10, 0, 10, 10 };

        KernelGraphicsRequire(GraphicsRectangleIsEmpty(GraphicsRectangleIntersect(left, right)),
                              "rectangles that merely touch are treated as overlapping");
    }

    KernelGraphicsRequire(GraphicsRectangleContains(a, 10, 10),
                          "a rectangle does not contain its own origin");
    KernelGraphicsRequire(!GraphicsRectangleContains(a, 30, 10),
                          "a rectangle contains the column one past its right edge");
    KernelGraphicsRequire(!GraphicsRectangleContains(a, 9, 10),
                          "a rectangle contains the column before its left edge");
}

/* Asserts the description of a surface and the confinement of its clip. */
static void KernelVerifyGraphicsSurface(void)
{
    GraphicsSurface scratch;
    GraphicsRectangle clip;

    KernelGraphicsRequire(!GraphicsSurfaceInitialise(&scratch, KernelSurfaceStore, 32U, 16U,
                                                     32U * 4U - 1U, 4U),
                          "a pitch narrower than one row was accepted, so every row would "
                          "overlap the one before it");
    KernelGraphicsRequire(!GraphicsSurfaceInitialise(&scratch, NULL, 32U, 16U, 128U, 4U),
                          "a surface with no pixels was accepted");
    KernelGraphicsRequire(!GraphicsSurfaceInitialise(&scratch, KernelSurfaceStore, 0U, 16U,
                                                     128U, 4U),
                          "a surface of no width was accepted");
    KernelGraphicsRequire(!GraphicsSurfaceInitialise(&scratch, KernelSurfaceStore, 32U, 16U,
                                                     128U, 0U),
                          "a surface whose pixels have no size was accepted");

    /*
     * The clip cannot be widened past the surface by any argument. This is the
     * property that lets every primitive treat the clip as sound without
     * inspecting it, so it is asserted with the most hostile arguments available:
     * a region far larger than the surface, and one placed at a negative origin.
     */
    {
        const GraphicsRectangle enormous = { -1000, -1000, 100000, 100000 };

        GraphicsSetClip(&KernelSurface, enormous);
        clip = GraphicsClip(&KernelSurface);

        KernelGraphicsRequire((clip.x == 0) && (clip.y == 0) &&
                                  (clip.width == (int32_t)KERNEL_SURFACE_WIDTH) &&
                                  (clip.height == (int32_t)KERNEL_SURFACE_HEIGHT),
                              "a clip larger than the surface was not confined to it");
    }

    {
        const GraphicsRectangle outside = { 500, 500, 10, 10 };

        GraphicsSetClip(&KernelSurface, outside);
        KernelGraphicsRequire(GraphicsRectangleIsEmpty(GraphicsClip(&KernelSurface)),
                              "a clip wholly outside the surface is not empty");

        /* Drawing against an empty clip must do nothing at all. */
        KernelSurfaceReset(KernelSurfaceStore);
        GraphicsClear(&KernelSurface, 0x111111U);
        GraphicsDrawLine(&KernelSurface, 0, 0, 31, 15, 0x111111U);
        GraphicsPutPixel(&KernelSurface, 5, 5, 0x111111U);
        KernelGraphicsRequire(KernelCountColour(&KernelSurface, 0x111111U) == 0U,
                              "drawing against an empty clip wrote pixels");
    }

    GraphicsResetClip(&KernelSurface);
    clip = GraphicsClip(&KernelSurface);
    KernelGraphicsRequire((clip.width == (int32_t)KERNEL_SURFACE_WIDTH) &&
                              (clip.height == (int32_t)KERNEL_SURFACE_HEIGHT),
                          "the clip was not restored to the whole surface");
}

/* Asserts the pixel, the fill and the outline. */
static void KernelVerifyGraphicsRectangleDrawing(void)
{
    const uint32_t ink = 0x00FF00U;
    const uint32_t ground = 0x000010U;

    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsResetClip(&KernelSurface);

    /* A pixel is set where it is asked for and nowhere else. */
    GraphicsPutPixel(&KernelSurface, 7, 3, ink);
    KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 7, 3) == ink,
                          "a pixel was not set where it was asked for");
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 1U,
                          "setting one pixel changed more than one");
    KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                          "setting a pixel wrote into the row padding, so the address was "
                          "computed from the width and not the pitch");

    /* A pixel outside the surface writes nothing and reads as zero. */
    GraphicsPutPixel(&KernelSurface, -1, 3, ink);
    GraphicsPutPixel(&KernelSurface, 32, 3, ink);
    GraphicsPutPixel(&KernelSurface, 7, 16, ink);
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 1U,
                          "a pixel outside the surface was written");
    KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, -1, 3) == 0U,
                          "a pixel outside the surface read as something");
    KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                          "a pixel outside the surface wrote into the padding");

    /* A fill covers exactly its own area. */
    KernelSurfaceReset(KernelSurfaceStore);
    {
        const GraphicsRectangle box = { 4, 2, 10, 5 };

        GraphicsFillRectangle(&KernelSurface, box, ground);
        KernelGraphicsRequire(KernelCountColour(&KernelSurface, ground) == 50U,
                              "a filled rectangle covers the wrong number of pixels");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 4, 2) == ground &&
                                  GraphicsPixelAt(&KernelSurface, 13, 6) == ground,
                              "a filled rectangle does not reach its own corners");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 14, 6) != ground &&
                                  GraphicsPixelAt(&KernelSurface, 4, 7) != ground,
                              "a filled rectangle extends one pixel past its extent");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "a filled rectangle wrote into the row padding");
    }

    /*
     * A fill that straddles the edge is clipped to what fits, and the count is
     * asserted rather than merely the absence of a fault: a fill that silently
     * dropped the whole rectangle because part of it was outside would look the
     * same as one that clipped correctly, from the point of view of memory.
     */
    KernelSurfaceReset(KernelSurfaceStore);
    {
        const GraphicsRectangle straddling = { -5, -5, 10, 10 };

        GraphicsFillRectangle(&KernelSurface, straddling, ink);
        KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 25U,
                              "a rectangle straddling the top left corner was not clipped "
                              "to the 5 by 5 that remains");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 0, 0) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 4, 4) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 5, 5) != ink,
                              "the clipped rectangle is in the wrong place");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "a clipped fill wrote into the row padding");
    }

    /* The outline is hollow, and its corners are present. */
    KernelSurfaceReset(KernelSurfaceStore);
    {
        const GraphicsRectangle box = { 2, 2, 6, 4 };

        GraphicsDrawRectangle(&KernelSurface, box, ink);

        /* 6 by 4 has 2*(6+4) - 4 = 16 pixels of outline. */
        KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 16U,
                              "an outlined rectangle has the wrong number of pixels, so "
                              "its corners are doubled or its edges are short");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 2, 2) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 7, 2) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 2, 5) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 7, 5) == ink,
                              "an outlined rectangle is missing a corner");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 4, 3) != ink,
                              "an outlined rectangle is filled");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "an outlined rectangle wrote into the row padding");
    }

    /* A clear fills the clip and not the surface, which is what makes it usable
     * for erasing one region of a screen. */
    KernelSurfaceReset(KernelSurfaceStore);
    {
        const GraphicsRectangle region = { 8, 4, 4, 4 };

        GraphicsSetClip(&KernelSurface, region);
        GraphicsClear(&KernelSurface, ink);
        GraphicsResetClip(&KernelSurface);

        KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 16U,
                              "a clear filled something other than the clip");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "a clear wrote into the row padding");
    }
}

/* Asserts the line, and in particular that clipping does not move it. */
static void KernelVerifyGraphicsLine(void)
{
    const uint32_t ink = 0xFF00FFU;

    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsResetClip(&KernelSurface);

    /* A horizontal line covers exactly the span it names, inclusive of both
     * ends. An off-by-one at either end is the commonest fault here. */
    GraphicsDrawLine(&KernelSurface, 3, 8, 12, 8, ink);
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 10U,
                          "a horizontal line does not include both of its endpoints");
    KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 3, 8) == ink &&
                              GraphicsPixelAt(&KernelSurface, 12, 8) == ink &&
                              GraphicsPixelAt(&KernelSurface, 13, 8) != ink,
                          "a horizontal line is displaced");

    /* A single point is a line of one pixel, not of none. */
    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsDrawLine(&KernelSurface, 5, 5, 5, 5, ink);
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 1U,
                          "a line between one point and itself is not one pixel");

    /* A vertical line, and a diagonal, whose length is its longer axis. */
    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsDrawLine(&KernelSurface, 6, 1, 6, 14, ink);
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 14U,
                          "a vertical line has the wrong length");

    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsDrawLine(&KernelSurface, 0, 0, 9, 9, ink);
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 10U,
                          "a diagonal line has the wrong length");
    KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 4, 4) == ink,
                          "a diagonal line does not pass through its own middle");

    /* A line is the same line drawn in either direction. Bresenham accumulates
     * its error from one end, so a careless implementation is not symmetric. */
    {
        uint32_t forwards;
        uint32_t backwards;
        bool identical = true;

        KernelSurfaceReset(KernelSurfaceStore);
        GraphicsDrawLine(&KernelSurface, 1, 2, 30, 13, ink);
        forwards = KernelCountColour(&KernelSurface, ink);

        for (int32_t y = 0; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            for (int32_t x = 0; x < (int32_t)KERNEL_SURFACE_WIDTH; ++x)
            {
                if (GraphicsPixelAt(&KernelSurface, x, y) == ink)
                {
                    GraphicsPutPixel(&KernelSurface, x, y, 0x010101U);
                }
            }
        }

        GraphicsDrawLine(&KernelSurface, 30, 13, 1, 2, ink);
        backwards = KernelCountColour(&KernelSurface, ink);

        /* Every pixel of the reversed line must land upon one of the first,
         * which now holds the marker rather than the ink. */
        for (int32_t y = 0; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            for (int32_t x = 0; x < (int32_t)KERNEL_SURFACE_WIDTH; ++x)
            {
                if (GraphicsPixelAt(&KernelSurface, x, y) == 0x010101U)
                {
                    identical = false;
                }
            }
        }

        KernelGraphicsRequire(forwards == backwards,
                              "a line drawn backwards has a different length");
        KernelGraphicsRequire(identical,
                              "a line drawn backwards lights different pixels, so the "
                              "error is accumulated asymmetrically");
    }

    /*
     * The property the header promises: a clipped line lights exactly those
     * pixels of the unclipped line that fall within the clip.
     *
     * The line is drawn once unclipped and its pixels counted within a region;
     * then the surface is cleared, the clip set to that region, and the same line
     * drawn again. The two counts must agree, and every pixel of the second must
     * coincide with one of the first.
     *
     * An implementation that clipped by moving the endpoints would pass a count
     * and fail the coincidence, the recomputed error lighting a neighbouring
     * pixel here and there. That is the failure this exists to catch, and it is
     * invisible until two clipped regions meet along a seam.
     */
    {
        const GraphicsRectangle region = { 8, 4, 12, 8 };
        static bool expected[KERNEL_SURFACE_HEIGHT][KERNEL_SURFACE_WIDTH];
        uint32_t within = 0U;
        uint32_t clipped = 0U;
        bool coincide = true;

        /*
         * The unclipped line is drawn first and the pixels it lights *inside the
         * region* are recorded. The surface is then cleared entirely before the
         * clipped line is drawn, so that what remains afterwards is the work of
         * the second drawing alone. Comparing the two upon one surface would
         * leave the first line's pixels outside the region standing, and they
         * would be counted against the second.
         */
        KernelSurfaceReset(KernelSurfaceStore);
        GraphicsResetClip(&KernelSurface);
        GraphicsDrawLine(&KernelSurface, -4, -2, 34, 17, ink);

        for (int32_t y = 0; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            for (int32_t x = 0; x < (int32_t)KERNEL_SURFACE_WIDTH; ++x)
            {
                const bool lit = (GraphicsPixelAt(&KernelSurface, x, y) == ink) &&
                                 GraphicsRectangleContains(region, x, y);

                expected[y][x] = lit;

                if (lit)
                {
                    ++within;
                }
            }
        }

        KernelSurfaceReset(KernelSurfaceStore);
        GraphicsSetClip(&KernelSurface, region);
        GraphicsDrawLine(&KernelSurface, -4, -2, 34, 17, ink);
        GraphicsResetClip(&KernelSurface);

        for (int32_t y = 0; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            for (int32_t x = 0; x < (int32_t)KERNEL_SURFACE_WIDTH; ++x)
            {
                const bool lit = (GraphicsPixelAt(&KernelSurface, x, y) == ink);

                if (lit)
                {
                    ++clipped;
                }

                if (lit != expected[y][x])
                {
                    coincide = false;
                }
            }
        }

        KernelGraphicsRequire(within > 0U,
                              "the line under test does not cross the clip region at all, "
                              "so this assertion proves nothing");
        KernelGraphicsRequire(clipped == within,
                              "a clipped line lights a different number of pixels than the "
                              "unclipped line does inside the clip");
        KernelGraphicsRequire(coincide,
                              "a clipped line lights different pixels than the unclipped "
                              "line does, so clipping moved the line");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "a line wrote into the row padding");
    }

    /* A line whose coordinates exceed the bound is refused rather than drawn,
     * an unbounded loop being worse than a refusal. */
    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsDrawLine(&KernelSurface, 0, 0, GRAPHICS_COORDINATE_LIMIT + 1, 8, ink);
    KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 0U,
                          "a line with a coordinate beyond the limit was drawn");
}

/* Asserts the blit, including the overlapping case a console scrolls with. */
static void KernelVerifyGraphicsBlit(void)
{
    const uint32_t ink = 0x00ABCDU;
    const GraphicsRectangle whole = { 0, 0, (int32_t)KERNEL_SURFACE_WIDTH,
                                      (int32_t)KERNEL_SURFACE_HEIGHT };

    KernelSurfaceReset(KernelSurfaceStore);
    KernelSurfaceReset(KernelSecondStore);
    GraphicsResetClip(&KernelSurface);
    GraphicsResetClip(&KernelSecondSurface);

    /* A blit between surfaces of different depths is refused, not converted. */
    {
        GraphicsSurface shallow;

        if (GraphicsSurfaceInitialise(&shallow, KernelSecondStore, 8U, 8U, 64U, 2U))
        {
            KernelGraphicsRequire(
                !GraphicsBlit(&KernelSurface, 0, 0, &shallow, whole),
                "a blit between surfaces of different pixel size was performed, which "
                "would give an image of the right size in the wrong colours");
        }
    }

    /* A rectangle copied between two surfaces arrives whole and in the right
     * place. */
    {
        const GraphicsRectangle box = { 1, 1, 4, 3 };

        GraphicsFillRectangle(&KernelSecondSurface, box, ink);
        KernelGraphicsRequire(GraphicsBlit(&KernelSurface, 10, 6, &KernelSecondSurface, box),
                              "a blit between two surfaces was refused");
        KernelGraphicsRequire(KernelCountColour(&KernelSurface, ink) == 12U,
                              "a blit copied the wrong number of pixels");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 10, 6) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 13, 8) == ink &&
                                  GraphicsPixelAt(&KernelSurface, 14, 8) != ink,
                              "a blit placed the rectangle in the wrong position");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore) &&
                                  KernelPaddingIsIntact(KernelSecondStore),
                              "a blit wrote into the row padding");
    }

    /*
     * A blit trimmed by the destination's clip takes correspondingly less of the
     * source, and takes the right part of it.
     *
     * This is the assertion that the source is trimmed by exactly what the
     * destination lost. An implementation that clipped the destination without
     * adjusting the source would copy the correct number of pixels from the
     * wrong place, which is a shifted image rather than a missing one.
     */
    KernelSurfaceReset(KernelSurfaceStore);
    KernelSurfaceReset(KernelSecondStore);
    {
        const GraphicsRectangle source_box = { 0, 0, 8, 8 };

        /* A source whose left half differs from its right. */
        for (int32_t y = 0; y < 8; ++y)
        {
            for (int32_t x = 0; x < 8; ++x)
            {
                GraphicsPutPixel(&KernelSecondSurface, x, y,
                                 (x < 4) ? 0x111111U : 0x222222U);
            }
        }

        /* Placed so that its left half falls off the left edge. */
        (void)GraphicsBlit(&KernelSurface, -4, 2, &KernelSecondSurface, source_box);

        KernelGraphicsRequire(KernelCountColour(&KernelSurface, 0x111111U) == 0U,
                              "the part of the source that fell outside the destination "
                              "was copied anyway");
        KernelGraphicsRequire(KernelCountColour(&KernelSurface, 0x222222U) == 32U,
                              "the surviving half of a trimmed blit is the wrong size");
        KernelGraphicsRequire(GraphicsPixelAt(&KernelSurface, 0, 2) == 0x222222U,
                              "a trimmed blit took the wrong part of the source, so the "
                              "image is shifted rather than cropped");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "a trimmed blit wrote into the row padding");
    }

    /*
     * The overlapping copy, which is what a console scroll is.
     *
     * Each row of the surface is filled with its own row number as a colour, and
     * the whole is then moved up by two rows within the one surface. Row n must
     * afterwards hold what row n+2 held. A copy performed in the wrong direction
     * would smear the first row it read down the whole region, and the assertion
     * that catches that is the one upon a row well below the top.
     */
    KernelSurfaceReset(KernelSurfaceStore);
    GraphicsResetClip(&KernelSurface);
    {
        bool moved = true;

        for (int32_t y = 0; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            const GraphicsRectangle row = { 0, y, (int32_t)KERNEL_SURFACE_WIDTH, 1 };

            GraphicsFillRectangle(&KernelSurface, row, (uint32_t)(0x1000U + y));
        }

        {
            const GraphicsRectangle from = { 0, 2, (int32_t)KERNEL_SURFACE_WIDTH,
                                             (int32_t)KERNEL_SURFACE_HEIGHT - 2 };

            KernelGraphicsRequire(GraphicsBlit(&KernelSurface, 0, 0, &KernelSurface, from),
                                  "an overlapping blit within one surface was refused");
        }

        for (int32_t y = 0; y < ((int32_t)KERNEL_SURFACE_HEIGHT - 2); ++y)
        {
            if (GraphicsPixelAt(&KernelSurface, 5, y) != (uint32_t)(0x1000U + y + 2))
            {
                moved = false;
            }
        }

        KernelGraphicsRequire(moved,
                              "an overlapping blit upward did not move the rows, so the "
                              "copy read bytes it had already overwritten");
    }

    /* The same downward, which is the direction that fails if the row order is
     * not reversed. */
    KernelSurfaceReset(KernelSurfaceStore);
    {
        bool moved = true;

        for (int32_t y = 0; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            const GraphicsRectangle row = { 0, y, (int32_t)KERNEL_SURFACE_WIDTH, 1 };

            GraphicsFillRectangle(&KernelSurface, row, (uint32_t)(0x2000U + y));
        }

        {
            const GraphicsRectangle from = { 0, 0, (int32_t)KERNEL_SURFACE_WIDTH,
                                             (int32_t)KERNEL_SURFACE_HEIGHT - 3 };

            (void)GraphicsBlit(&KernelSurface, 0, 3, &KernelSurface, from);
        }

        for (int32_t y = 3; y < (int32_t)KERNEL_SURFACE_HEIGHT; ++y)
        {
            if (GraphicsPixelAt(&KernelSurface, 5, y) != (uint32_t)(0x2000U + y - 3))
            {
                moved = false;
            }
        }

        KernelGraphicsRequire(moved,
                              "an overlapping blit downward smeared, so the rows were not "
                              "copied from the bottom");
        KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                              "an overlapping blit wrote into the row padding");
    }

    /* A blit wholly outside the destination is not an error; it copies nothing
     * and reports success, having done what was asked. */
    KernelSurfaceReset(KernelSurfaceStore);
    KernelGraphicsRequire(GraphicsBlit(&KernelSurface, 500, 500, &KernelSecondSurface, whole),
                          "a blit clipped away entirely was reported as a refusal");
    KernelGraphicsRequire(KernelPaddingIsIntact(KernelSurfaceStore),
                          "a blit clipped away entirely wrote something");
}

/*
 * Draws upon the framebuffer itself, so that the primitives are seen to work
 * upon the surface they exist for and not only upon an array.
 *
 * Nothing is asserted here beyond what the memory surface already established;
 * the framebuffer may not exist, and reading it back through a write-combining
 * mapping proves less than reading an array. What this produces is the figure a
 * person judges, described in docs/project/TESTING.md, Section 16.
 */
static void KernelVerifyGraphicsUponTheScreen(void)
{
    GraphicsSurface screen;
    GraphicsRectangle panel;
    int32_t width;
    int32_t height;

    if (!GraphicsSurfaceFromFramebuffer(&screen))
    {
        return;
    }

    width = (int32_t)screen.width;
    height = (int32_t)screen.height;

    /* A frame around the whole screen, which shows at a glance whether the
     * extent and the pitch are right: a wrong pitch makes the vertical edges
     * lean. */
    {
        const GraphicsRectangle border = { 0, 0, width, height };

        GraphicsDrawRectangle(&screen, border, FramebufferEncode(255U, 255U, 255U));
    }

    /* A panel, and a cross drawn corner to corner within it. The diagonals must
     * meet exactly at its centre. */
    panel.x = width / 8;
    panel.y = height / 4;
    panel.width = width / 4;
    panel.height = height / 4;

    GraphicsFillRectangle(&screen, panel, FramebufferEncode(20U, 20U, 60U));
    GraphicsDrawRectangle(&screen, panel, FramebufferEncode(120U, 120U, 255U));
    GraphicsDrawLine(&screen, panel.x, panel.y, panel.x + panel.width - 1,
                     panel.y + panel.height - 1, FramebufferEncode(255U, 200U, 0U));
    GraphicsDrawLine(&screen, panel.x + panel.width - 1, panel.y, panel.x,
                     panel.y + panel.height - 1, FramebufferEncode(255U, 200U, 0U));

    /*
     * A second panel drawn through a clip that covers only its left half, with
     * a line crossing the whole of it.
     *
     * This is the figure that shows clipping working: the fill and the line stop
     * dead at the clip boundary, and the line's slope does not change where it
     * stops. A line clipped by moving its endpoints would meet the boundary at a
     * slightly different height.
     */
    {
        const GraphicsRectangle second = { width / 2, height / 4, width / 4, height / 4 };
        const GraphicsRectangle half = { second.x, second.y, second.width / 2,
                                         second.height };

        GraphicsSetClip(&screen, half);
        GraphicsFillRectangle(&screen, second, FramebufferEncode(60U, 20U, 20U));
        GraphicsDrawLine(&screen, second.x, second.y + second.height - 1,
                         second.x + second.width - 1, second.y,
                         FramebufferEncode(255U, 255U, 255U));
        GraphicsResetClip(&screen);
    }

    /* The blit: the first panel copied below itself. The copy must be identical
     * and must be where it was put. */
    {
        const GraphicsRectangle taken = { panel.x, panel.y, panel.width, panel.height };

        (void)GraphicsBlit(&screen, panel.x, panel.y + panel.height + (height / 16),
                           &screen, taken);
    }
}

void KernelVerifyGraphics(void)
{
    KernelGraphicsSucceeded = true;

    if (!GraphicsSurfaceInitialise(&KernelSurface, KernelSurfaceStore, KERNEL_SURFACE_WIDTH,
                                   KERNEL_SURFACE_HEIGHT, KERNEL_SURFACE_PITCH,
                                   KERNEL_SURFACE_DEPTH) ||
        !GraphicsSurfaceInitialise(&KernelSecondSurface, KernelSecondStore,
                                   KERNEL_SURFACE_WIDTH, KERNEL_SURFACE_HEIGHT,
                                   KERNEL_SURFACE_PITCH, KERNEL_SURFACE_DEPTH))
    {
        KernelWriteString("Graphics self-test FAILED: the test surfaces could not be "
                          "described.\n");
        return;
    }

    KernelVerifyGraphicsRectangles();
    KernelVerifyGraphicsSurface();
    KernelVerifyGraphicsRectangleDrawing();
    KernelVerifyGraphicsLine();
    KernelVerifyGraphicsBlit();
    KernelVerifyGraphicsUponTheScreen();

    KernelWriteString(KernelGraphicsSucceeded ? "Graphics self-test passed.\n"
                                              : "Graphics self-test FAILED.\n");
}
