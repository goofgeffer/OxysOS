/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/gfx/graphics.h
 * Purpose: Declares the two-dimensional drawing primitives: the surface they
 *          draw upon, the rectangle arithmetic they clip against, and the
 *          pixel, line, rectangle and blit operations themselves.
 * Key definitions: GraphicsRectangle, GraphicsSurface, GraphicsSurfaceInitialise,
 *          GraphicsSurfaceFromFramebuffer, GraphicsSetClip, GraphicsResetClip,
 *          GraphicsPutPixel, GraphicsPixelAt, GraphicsFillRectangle,
 *          GraphicsDrawRectangle, GraphicsDrawLine, GraphicsBlit, GraphicsClear,
 *          GraphicsPatternBlock, GraphicsFillCircle.
 * References:
 *   - J. E. Bresenham, "Algorithm for computer control of a digital plotter",
 *     IBM Systems Journal 4(1), 1965: the integer line algorithm implemented in
 *     graphics/draw.c, which uses no division and no floating point.
 *   - J. E. Bresenham, "A linear algorithm for incremental digital display of
 *     circular arcs", Communications of the ACM 20(2), pages 100 to 106, 1977:
 *     the circle algorithm behind GraphicsFillCircle, integer likewise.
 *   - docs/design/DRAWING.md: the design, and every
 *     assertion made upon it.
 *
 * Why a surface, when there is only one framebuffer.
 *
 * A primitive that wrote to the framebuffer directly would be shorter by one
 * argument and worse in three ways. Blit has no meaning without two of them, so
 * it would have to name the framebuffer twice or invent a second thing anyway.
 * Nothing could be asserted upon a machine with no display, whereas a surface in
 * ordinary memory can be drawn upon and then read back pixel by pixel, which is
 * how the whole of the self-test works. And the double buffering of sub-task 6.6
 * is exactly the substitution of one surface for another, which a caller that
 * had named the framebuffer everywhere could not be given.
 *
 * A surface is a rectangle of pixels and nothing more: no ownership of the
 * memory, no allocation, no lifetime. Whoever supplies the pixels keeps them.
 */

#ifndef OXYS_GFX_GRAPHICS_H
#define OXYS_GFX_GRAPHICS_H

#include <oxys/types.h>

/*
 * The greatest coordinate magnitude any primitive will accept.
 *
 * Coordinates are signed because a caller must be able to place a shape
 * partially off the edge — that is what clipping is for — and the natural way to
 * say "half of this rectangle is above the screen" is a negative y. But a line
 * drawn between two coordinates far outside the surface still costs one
 * iteration per step of its longer axis even though it writes nothing, so an
 * unbounded coordinate is an unbounded loop.
 *
 * The bound is far larger than any display and far smaller than the point at
 * which the arithmetic below could overflow, so a coordinate within it is safe
 * by construction rather than by a check at each site. A coordinate outside it is
 * refused, and a refusal is better than a machine that appears to have stopped.
 */
#define GRAPHICS_COORDINATE_LIMIT 32767

/*
 * A rectangle, in pixels, with its origin at the top left of a surface.
 *
 * A width or height of zero or below denotes an empty rectangle. Emptiness is
 * represented rather than refused because it is the ordinary result of an
 * intersection that does not meet, and every primitive must do nothing when
 * given one.
 */
typedef struct GraphicsRectangle
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} GraphicsRectangle;

/*
 * A rectangle of pixels that may be drawn upon.
 *
 * `pitch` is the number of bytes from the start of one row to the start of the
 * next and is not width multiplied by the pixel size; see
 * <oxys/gfx/framebuffer.h>. `bytes_per_pixel` is what a pixel occupies, and pixel
 * values are opaque to everything here: the packing of colour channels into one
 * belongs to whoever supplied the surface, and for the framebuffer that is
 * FramebufferEncode.
 *
 * `clip` is the region within which drawing is permitted. It is always contained
 * within the surface, so writing outside the surface is not a thing a caller can
 * ask for by any argument.
 *
 * The pixels are volatile because a surface may be the framebuffer, which is
 * memory a device reads independently of this kernel. Nothing here is safe
 * against concurrent drawing upon one surface; that requires the lock of
 * sub-task 6.13, which is not taken at this level.
 *
 * `whole_words` records that this surface may be addressed a 32-bit word at a
 * time: its pixels are four bytes, and its base address and its pitch are both
 * multiples of four, so every pixel of it begins upon a word boundary. It is
 * computed once, by GraphicsSurfaceInitialise, because it is a property of the
 * surface and not of any drawing operation, and because a test made once is not
 * a test made in an inner loop.
 *
 * It exists because writing a four-byte pixel as four bytes costs four stores
 * and the loop that generates them, and that was measured to be the greater part
 * of what the console spent — see docs/design/CONSOLE.md. Where it
 * is false every primitive still works, byte at a time; nothing depends upon it
 * for correctness.
 */
/*
 * How deep the clip stack goes.
 *
 * Four is chosen because the nesting that exists is shallow and known: a
 * compositor clips to the region it is presenting, a layer clips to itself, and
 * a caller drawing within that clips to a panel. A fifth would mean somebody has
 * built something this depth was not designed for, and the push refuses rather
 * than overwriting the entry beneath it.
 */
#define GRAPHICS_CLIP_DEPTH 4U

typedef struct GraphicsSurface
{
    volatile uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bytes_per_pixel;
    bool whole_words;
    GraphicsRectangle clip;

    /*
     * The saved clips of GraphicsPushClip, and how many are saved.
     *
     * The stack lives in the surface rather than in the caller because the clip
     * does: a caller that saved and restored it by hand would have to know that
     * it had been narrowed by somebody else in between, and the whole point of a
     * push is that it need not.
     */
    GraphicsRectangle clip_stack[GRAPHICS_CLIP_DEPTH];
    uint32_t clip_depth;
} GraphicsSurface;

/* ------------------------------------------------------------------------------
 * Rectangles.
 * ------------------------------------------------------------------------------ */

/* Whether the rectangle encloses no pixel at all. */
bool GraphicsRectangleIsEmpty(GraphicsRectangle rectangle);

/*
 * The intersection of two rectangles, which is empty where they do not meet.
 * The result is always a valid rectangle: an intersection that does not meet is
 * returned with a width and height of zero rather than with negative extents.
 */
GraphicsRectangle GraphicsRectangleIntersect(GraphicsRectangle first,
                                             GraphicsRectangle second);

/* Whether the point lies within the rectangle. */
bool GraphicsRectangleContains(GraphicsRectangle rectangle, int32_t x, int32_t y);

/* ------------------------------------------------------------------------------
 * Surfaces.
 * ------------------------------------------------------------------------------ */

/*
 * Describes a rectangle of memory as a surface, with the clip set to the whole
 * of it.
 *
 * Returns false where the description is impossible: no pixels, no extent, a
 * pixel of no size, or a pitch narrower than a row. The last is the one worth
 * refusing loudly, a pitch below the occupied width meaning every row overlaps
 * the one before it.
 */
bool GraphicsSurfaceInitialise(GraphicsSurface *surface, void *pixels, uint32_t width,
                               uint32_t height, uint32_t pitch, uint8_t bytes_per_pixel);

/*
 * Describes the framebuffer of sub-task 6.2 as a surface.
 *
 * Returns false where no framebuffer was supplied or where the boot loader left
 * the adapter in a text mode, in which case the surface is left untouched and
 * nothing may be drawn.
 */
bool GraphicsSurfaceFromFramebuffer(GraphicsSurface *surface);

/*
 * Confines drawing to the given region, intersected with the surface.
 *
 * The intersection is what makes this safe: a caller cannot widen the clip
 * beyond the surface by any argument, however large or negative, so no primitive
 * need consider whether the clip it was given is itself sound.
 */
void GraphicsSetClip(GraphicsSurface *surface, GraphicsRectangle region);

/* Restores the clip to the whole surface. */
void GraphicsResetClip(GraphicsSurface *surface);

/*
 * Narrows the clip to the intersection of the present one and the region given,
 * saving what it was. Returns false where the stack is full, in which case the
 * clip is left alone — a push that silently discarded the region beneath it
 * would let drawing escape a boundary a caller believed it had established.
 *
 * The intersection is the point. A push that replaced the clip could widen it,
 * and a caller nesting a panel inside a clipped region would then draw outside
 * the region it was given.
 */
bool GraphicsPushClip(GraphicsSurface *surface, GraphicsRectangle region);

/* Restores the clip saved by the matching push. Returns false where nothing was
 * pushed, the clip then being left alone. */
bool GraphicsPopClip(GraphicsSurface *surface);

/* How many clips are presently saved, which a self-test asserts returns to what
 * it was: a push without a pop leaves every later drawing narrowed. */
uint32_t GraphicsClipDepth(const GraphicsSurface *surface);

/* The clip presently in force. */
GraphicsRectangle GraphicsClip(const GraphicsSurface *surface);

/* The whole surface as a rectangle. */
GraphicsRectangle GraphicsSurfaceBounds(const GraphicsSurface *surface);

/* ------------------------------------------------------------------------------
 * Drawing.
 *
 * No operation below can fail, and none reports anything. A shape that lies
 * wholly outside the clip is not an error; it draws nothing, which is what the
 * caller asked for. A coordinate beyond GRAPHICS_COORDINATE_LIMIT is treated the
 * same way.
 * ------------------------------------------------------------------------------ */

/* Sets one pixel, if it lies within the clip. */
void GraphicsPutPixel(GraphicsSurface *surface, int32_t x, int32_t y, uint32_t colour);

/*
 * Reads one pixel. Returns zero for a coordinate outside the *surface* — note
 * the surface and not the clip, a clip being a restriction upon drawing and not
 * upon reading what is already there.
 *
 * This exists for the self-test. Nothing in a drawing path reads the surface it
 * draws upon, and a framebuffer mapped write-combining is slow to read.
 */
uint32_t GraphicsPixelAt(const GraphicsSurface *surface, int32_t x, int32_t y);

/*
 * Combines a colour into the pixel already at a position, at a coverage of 0 to
 * 255, and clipped as every other write is.
 *
 * A coverage of 255 is exactly GraphicsPutPixel and takes that path; a coverage
 * of 0 writes nothing at all, and writing nothing is not the same as writing
 * what was already there — a surface being read to compose the result, the two
 * differ in cost and, upon a framebuffer, in whether the read happens.
 *
 * The channels are combined apart, through FramebufferDecode, because a packed
 * pixel cannot be interpolated as a whole: the carries would run out of one
 * channel and into the next, so a half-covered red over a blue would produce a
 * green that neither of them contains.
 */
void GraphicsBlendPixel(GraphicsSurface *surface, int32_t x, int32_t y, uint32_t colour,
                        uint8_t coverage);

/*
 * Composites a source surface over a destination at a position, taking each
 * pixel's coverage from a mask of one byte per pixel in the source's own
 * dimensions.
 *
 * A null mask makes every pixel opaque, which is a blit. The mask is what makes
 * a shape of arbitrary outline possible without a colour reserved to mean
 * "absent" — a reserved colour is a colour the shape may not contain, and the
 * pointer contains black and white, which are the two a caller would reach for.
 */
void GraphicsBlendSurface(GraphicsSurface *destination, int32_t x, int32_t y,
                          const GraphicsSurface *source, const uint8_t *mask);

/* Fills the rectangle, clipped. */
void GraphicsFillRectangle(GraphicsSurface *surface, GraphicsRectangle rectangle,
                           uint32_t colour);

/* Draws the one-pixel outline of the rectangle, clipped. The outline lies
 * within the rectangle, so a rectangle of width 1 is a vertical line. */
void GraphicsDrawRectangle(GraphicsSurface *surface, GraphicsRectangle rectangle,
                           uint32_t colour);

/* Fills the whole clip. */
void GraphicsClear(GraphicsSurface *surface, uint32_t colour);

/*
 * Fills the disc of the given radius about the centre, clipped. A radius of
 * zero is one pixel; a negative radius draws nothing.
 *
 * Sub-task 9.1's one addition to the primitives, for the close control of a
 * window's frame, which docs/project/INSPIRATIONS.md wants
 * geometric and not a glyph. The spans are found by Bresenham's circle
 * algorithm and are integer throughout; see docs/design/DRAWING.md.
 */
void GraphicsFillCircle(GraphicsSurface *surface, int32_t centre_x, int32_t centre_y,
                        int32_t radius, uint32_t colour);

/*
 * Writes a block eight pixels wide and `rows` high, each pixel taking one of two
 * colours according to a bit of `pattern` — one byte to a row, bit 7 leftmost.
 *
 * That is exactly the shape of a bitmap glyph, and it is what this is for.
 * Drawing text a pixel at a time was measured to be the greater part of what the
 * console cost: sixty-four clip tests and sixty-four address computations to the
 * character, where the whole block shares one of each. The clip is applied once,
 * to the block, and nothing inside the loops tests anything.
 *
 * Eight wide is fixed because the pattern is a byte and there is no second
 * definition of what a ninth bit would be. `rows` is not fixed, a face of a
 * different height being a table of a different length and nothing else.
 */
void GraphicsPatternBlock(GraphicsSurface *surface, int32_t x, int32_t y,
                          const uint8_t *pattern, int32_t rows, uint32_t ink,
                          uint32_t paper);

/*
 * Draws the line between the two points inclusive, clipped.
 *
 * The pixels drawn are exactly those of the unclipped line that fall within the
 * clip, and this is a stronger promise than it appears; see
 * docs/design/DRAWING.md.
 */
void GraphicsDrawLine(GraphicsSurface *surface, int32_t first_x, int32_t first_y,
                      int32_t second_x, int32_t second_y, uint32_t colour);

/*
 * Copies a rectangle of the source surface to the given position in the
 * destination, clipped by the destination's clip and by both surfaces' extents.
 *
 * The two surfaces must agree upon the size of a pixel; a blit between surfaces
 * that do not is refused rather than performed byte by byte, since the result
 * would be an image of the right size in the wrong colours.
 *
 * The surfaces may be the same, and the regions may overlap. The copy is
 * performed in whichever order leaves the result correct, so this is the
 * operation a console scrolls with.
 *
 * Returns false only where the blit was refused outright: differing pixel sizes,
 * or a surface that has none. A blit that is clipped away entirely returns true,
 * having correctly done nothing.
 */
bool GraphicsBlit(GraphicsSurface *destination, int32_t x, int32_t y,
                  const GraphicsSurface *source, GraphicsRectangle area);

/* Emits a summary of the framebuffer surface upon the console and serial port. */
void GraphicsReport(void);

#endif /* OXYS_GFX_GRAPHICS_H */
