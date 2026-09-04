/*
 * File: kernel/include/oxys/font.h
 * Purpose: Declares the bitmap font: its metrics, the range of code points it
 *          covers, and the drawing of one glyph upon a surface.
 * Key definitions: FONT_WIDTH, FONT_HEIGHT, FONT_FIRST_CODE, FONT_LAST_CODE,
 *          FONT_GLYPH_COUNT, FontCovers, FontGlyph, FontGlyphRow, FontDrawGlyph,
 *          FontDrawGlyphOpaque, FontDrawGlyphScaled.
 * References:
 *   - ANSI X3.4-1986: the printable range 0x20 to 0x7E the font covers.
 *   - docs/design/GRAPHICS.md, Section 18: the face, its metrics, and why it was
 *     drawn rather than obtained.
 *
 * The font is compiled into the image. It is not read from the firmware and not
 * loaded from a volume: the boot loader has set a graphics mode before this
 * kernel runs, so the VGA character generator may no longer hold anything, and
 * Phase 12 boots under UEFI where there is no character generator at all.
 */

#ifndef OXYS_FONT_H
#define OXYS_FONT_H

#include <oxys/types.h>
#include <oxys/graphics.h>

/*
 * The cell, in pixels. Eight by eight, and the width is eight because a row of
 * a glyph is exactly one byte: a wider cell would need either two bytes a row or
 * a bit field spanning bytes, and neither buys anything a console wants.
 *
 * The ink occupies six of the eight columns and seven or eight of the rows; the
 * remainder is the spacing between one character and the next, so a console
 * draws characters at a stride of exactly FONT_WIDTH with no gap of its own.
 */
#define FONT_WIDTH  8U
#define FONT_HEIGHT 8U

/* The range covered, and the number of glyphs in it. */
#define FONT_FIRST_CODE  0x20U
#define FONT_LAST_CODE   0x7EU
#define FONT_GLYPH_COUNT (FONT_LAST_CODE - FONT_FIRST_CODE + 1U)

/* Whether the font defines a glyph for the code, as against substituting the
 * replacement glyph for it. */
bool FontCovers(uint8_t code);

/*
 * The eight rows of the glyph for a code, most significant bit leftmost.
 *
 * Never NULL. A code the font does not cover yields the replacement glyph, a
 * hollow box, so that an unmapped character is visibly present rather than
 * indistinguishable from a space.
 */
const uint8_t *FontGlyph(uint8_t code);

/* One row of that glyph, or zero for a row beyond the cell. */
uint8_t FontGlyphRow(uint8_t code, uint8_t row);

/*
 * Draws the glyph with its top left corner at the given position, setting only
 * the pixels the glyph defines and leaving the rest of the cell as it was.
 *
 * The background is the caller's business. A console fills the cell before
 * calling this; a caller drawing text over an image does not, and gets the
 * character stencilled upon what was already there.
 *
 * Every pixel is clipped by the surface, so a glyph at an edge is cut off rather
 * than wrapped, and one wholly outside the clip draws nothing.
 */
void FontDrawGlyph(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                   uint32_t colour);

/*
 * Draws the glyph and the whole of its cell in one pass: every pixel of the
 * eight by eight becomes either the ink or the paper.
 *
 * This is what a console wants and FontDrawGlyph is not. A console fills the
 * cell and then draws the glyph over it, which writes every pixel of the cell
 * twice and clips each of them separately; measured, that was the greater part
 * of what a character cost. Here the cell is clipped a row at a time and each
 * pixel is written once.
 *
 * Both exist because they answer different questions. Use this where the cell
 * has a background of its own — a console, a menu, a label upon a solid panel.
 * Use FontDrawGlyph where what is behind the character must show through, which
 * is text over an image and is what a cursor is drawn with.
 */
void FontDrawGlyphOpaque(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                         uint32_t ink, uint32_t paper);

/*
 * Draws the glyph with each of its pixels enlarged to a square of `scale` by
 * `scale`, so that a face of one size can title a page as well as fill a line
 * of it.
 *
 * There is no second face and no attempt to smooth what enlarging produces: a
 * pixel becomes a square and the letter becomes a blocky version of itself. That
 * is the honest result of having one bitmap face, and it is legible, which is
 * the whole requirement — this exists for the banner of a fault screen, read
 * once by somebody whose machine has just stopped.
 *
 * A scale of one is the same drawing as FontDrawGlyphOpaque and is permitted, so
 * that a caller may compute the scale from the width of the display without
 * having to branch upon the result.
 */
void FontDrawGlyphScaled(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                         uint32_t ink, uint32_t paper, int32_t scale);

#endif /* OXYS_FONT_H */
