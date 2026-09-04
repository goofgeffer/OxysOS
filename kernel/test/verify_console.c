/*
 * File: kernel/test/verify_console.c
 * Purpose: Asserts the bitmap font of sub-task 6.4 and the console drawn with
 *          it: that every glyph obeys the metrics the face is drawn to, that no
 *          two glyphs are the same, that a glyph reaches the surface as the
 *          bytes say it should, and that the four control characters move the
 *          active position as ANSI X3.4-1986 defines them.
 * Key functions: KernelVerifyConsole.
 * References:
 *   - docs/design/GRAPHICS.md, Section 21: every assertion below, paired with
 *     the silent failure it catches.
 *   - ANSI X3.4-1986: LF, CR, HT and BS.
 *   - docs/devices/DISPLAY.md, Section 6: the same four characters as the
 *     text-mode driver implements them, which this console must agree with.
 *
 * The font is asserted against a surface in memory, as the primitives of
 * sub-task 6.3 are, so that the whole of it holds upon a machine with no
 * display. The console's control characters are asserted upon the live console,
 * because the position they move is the console's own and there is no second one
 * to make; only characters that draw nothing are used, so the boot log is not
 * disturbed by the test of it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/console.h>
#include <oxys/font.h>
#include <oxys/graphics.h>
#include <oxys/framebuffer.h>

static bool KernelConsoleSucceeded;

static void KernelConsoleRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelConsoleSucceeded = false;
    }
}

/* A surface just large enough to hold one glyph with a margin, so that a glyph
 * drawn outside its cell is visible as a pixel in the margin. */
#define KERNEL_GLYPH_SURFACE_WIDTH  16U
#define KERNEL_GLYPH_SURFACE_HEIGHT 16U
#define KERNEL_GLYPH_SURFACE_PITCH  (KERNEL_GLYPH_SURFACE_WIDTH * 4U)

/*
 * The store is declared as words and not as bytes, and that is a correctness
 * matter rather than a convenience.
 *
 * From the optimisation of sub-task 6.4 the primitives write a four-byte pixel
 * as one 32-bit store where the surface permits it. An object declared as an
 * array of `uint8_t` has that as its type for the whole of its life, and writing
 * through a `uint32_t` lvalue into it is undefined however well it appears to
 * work; declared as words, both accesses are sound — a word through its own
 * type, and a byte through a character type, which may alias anything.
 *
 * It also guarantees the four-byte alignment the word path requires, which a
 * byte array does not.
 */
static uint32_t
    KernelGlyphStore[(KERNEL_GLYPH_SURFACE_PITCH * KERNEL_GLYPH_SURFACE_HEIGHT) / 4U];
static GraphicsSurface KernelGlyphSurface;

/* Where the ink of one glyph is recorded so that the two drawing routines may be
 * compared cell for cell. It is a file-scope object rather than a local because
 * the stack of the early boot is not the place for one, small as it is. */
static bool KernelOpaqueExpected[FONT_HEIGHT][FONT_WIDTH];

/*
 * Asserts the face against the metrics it is documented to be drawn to.
 *
 * These are assertions upon a table that was authored by hand, and that is
 * exactly why they are worth making. A font is data, so a compiler checks
 * nothing about it; the plausible faults are a glyph transposed with its
 * neighbour, a row omitted so that everything after it is shifted, and a
 * copy-and-paste that left two characters identical. None of those produces a
 * fault — they produce a font that is wrong to look at, which no test can
 * report and which the reader of a boot log will blame upon the boot log.
 */
static void KernelVerifyFont(void)
{
    uint32_t blank_glyphs = 0U;

    KernelConsoleRequire(!FontCovers((uint8_t)(FONT_FIRST_CODE - 1U)),
                         "the font claims to cover the code below its first");
    KernelConsoleRequire(FontCovers((uint8_t)FONT_FIRST_CODE),
                         "the font does not cover its own first code");
    KernelConsoleRequire(FontCovers((uint8_t)FONT_LAST_CODE),
                         "the font does not cover its own last code");
    KernelConsoleRequire(!FontCovers((uint8_t)(FONT_LAST_CODE + 1U)),
                         "the font claims to cover the code above its last");

    /* Never NULL, for any code at all. A drawing routine dereferences this
     * without checking, because this is the assertion that it need not. */
    KernelConsoleRequire((FontGlyph(0x00U) != NULL) && (FontGlyph(0xFFU) != NULL),
                         "the font returned no glyph for a code outside its range");

    /*
     * The replacement glyph is not blank. A font that drew nothing for a code it
     * did not know would make a run of unmapped characters indistinguishable
     * from a run of spaces, and the fault would look like missing output.
     */
    {
        uint8_t ink = 0U;

        for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
        {
            ink |= FontGlyphRow(0x01U, row);
        }

        KernelConsoleRequire(ink != 0U,
                             "the replacement glyph is blank, so an unmapped character is "
                             "indistinguishable from a space");
    }

    /* The space is blank. A space with ink in it would streak every gap between
     * words, which is the one glyph whose fault is visible everywhere at once. */
    {
        uint8_t ink = 0U;

        for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
        {
            ink |= FontGlyphRow(0x20U, row);
        }

        KernelConsoleRequire(ink == 0U, "the space glyph is not blank");
    }

    for (uint32_t code = FONT_FIRST_CODE; code <= FONT_LAST_CODE; ++code)
    {
        uint8_t ink = 0U;
        uint8_t spacing = 0U;

        for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
        {
            const uint8_t bits = FontGlyphRow((uint8_t)code, row);

            ink |= bits;
            spacing |= (uint8_t)(bits & 0x03U);
        }

        if (ink == 0U)
        {
            ++blank_glyphs;
        }

        /*
         * The two rightmost columns are the spacing between one character and
         * the next, and every glyph must leave them clear. A glyph that used
         * them would touch the character beside it, and a console drawing at a
         * stride of exactly the cell width has nowhere to put a gap of its own.
         */
        if (spacing != 0U)
        {
            KernelWriteString("  a glyph draws into the two columns reserved for "
                              "spacing, at code ");
            KernelWriteHexadecimal(code);
            KernelWriteString("\n");
            KernelConsoleSucceeded = false;
            break;
        }
    }

    /* Only the space is blank. A glyph omitted from the table would be a blank
     * cell where a character should be, and would otherwise be reported by
     * nothing. */
    KernelConsoleRequire(blank_glyphs == 1U,
                         "some glyph other than the space is blank, so a character is "
                         "missing from the face");

    /*
     * No two glyphs are identical.
     *
     * This is the assertion worth having in a hand-drawn table. A duplicate is
     * what a copy-and-paste leaves behind, it is invisible in a picture comment
     * that was pasted along with it, and its consequence is that one letter is
     * silently drawn as another — which a reader will read straight past.
     */
    {
        bool duplicated = false;

        for (uint32_t first = FONT_FIRST_CODE; first <= FONT_LAST_CODE && !duplicated;
             ++first)
        {
            for (uint32_t second = first + 1U; second <= FONT_LAST_CODE; ++second)
            {
                bool same = true;

                for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
                {
                    if (FontGlyphRow((uint8_t)first, row) !=
                        FontGlyphRow((uint8_t)second, row))
                    {
                        same = false;
                        break;
                    }
                }

                if (same)
                {
                    KernelWriteString("  two glyphs are identical, at codes ");
                    KernelWriteHexadecimal(first);
                    KernelWriteString(" and ");
                    KernelWriteHexadecimal(second);
                    KernelWriteString("\n");
                    KernelConsoleSucceeded = false;
                    duplicated = true;
                    break;
                }
            }
        }
    }
}

/* Asserts that a glyph reaches a surface as its bytes say it should. */
static void KernelVerifyGlyphDrawing(void)
{
    const uint32_t ink = 0x00FFFFFFU;
    bool matched = true;
    bool margin_clean = true;

    if (!GraphicsSurfaceInitialise(&KernelGlyphSurface, KernelGlyphStore,
                                   KERNEL_GLYPH_SURFACE_WIDTH, KERNEL_GLYPH_SURFACE_HEIGHT,
                                   KERNEL_GLYPH_SURFACE_PITCH, 4U))
    {
        KernelConsoleRequire(false, "the glyph test surface could not be described");
        return;
    }

    GraphicsResetClip(&KernelGlyphSurface);
    GraphicsClear(&KernelGlyphSurface, 0U);

    /* Drawn at (4, 4), so that a glyph escaping its cell in any direction lands
     * in the margin rather than off the surface, where it would be clipped away
     * and so hidden from this assertion. */
    FontDrawGlyph(&KernelGlyphSurface, 4, 4, (uint8_t)'A', ink);

    for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
    {
        const uint8_t bits = FontGlyphRow((uint8_t)'A', row);

        for (uint8_t column = 0U; column < FONT_WIDTH; ++column)
        {
            const bool expected = (bits & (uint8_t)(0x80U >> column)) != 0U;
            const uint32_t got =
                GraphicsPixelAt(&KernelGlyphSurface, 4 + (int32_t)column, 4 + (int32_t)row);

            if (expected != (got == ink))
            {
                matched = false;
            }
        }
    }

    KernelConsoleRequire(matched,
                         "a glyph drawn upon a surface does not match its own bytes, so "
                         "the bit order or the row order is wrong");

    /*
     * The margin around the cell is untouched. The bit order is the thing this
     * catches: a glyph drawn with the least significant bit leftmost would still
     * light eight pixels a row and would still be a picture, merely a mirrored
     * one — and mirroring is invisible in the symmetric letters.
     */
    for (int32_t y = 0; y < (int32_t)KERNEL_GLYPH_SURFACE_HEIGHT; ++y)
    {
        for (int32_t x = 0; x < (int32_t)KERNEL_GLYPH_SURFACE_WIDTH; ++x)
        {
            const bool inside = (x >= 4) && (x < (4 + (int32_t)FONT_WIDTH)) && (y >= 4) &&
                                (y < (4 + (int32_t)FONT_HEIGHT));

            if (!inside && (GraphicsPixelAt(&KernelGlyphSurface, x, y) != 0U))
            {
                margin_clean = false;
            }
        }
    }

    KernelConsoleRequire(margin_clean, "a glyph drew outside its own cell");

    /* Only the glyph's pixels are written: the cell's background is the caller's
     * business, so a glyph drawn twice over different backgrounds must leave the
     * unset pixels as they were. */
    GraphicsClear(&KernelGlyphSurface, 0x00202020U);
    FontDrawGlyph(&KernelGlyphSurface, 4, 4, (uint8_t)'A', ink);
    KernelConsoleRequire(GraphicsPixelAt(&KernelGlyphSurface, 4, 4) == 0x00202020U,
                         "a glyph filled its own background, so it cannot be drawn over an "
                         "image");

    /* An unmapped code draws the replacement glyph rather than nothing. */
    GraphicsClear(&KernelGlyphSurface, 0U);
    FontDrawGlyph(&KernelGlyphSurface, 4, 4, 0x01U, ink);
    {
        uint32_t lit = 0U;

        for (int32_t y = 4; y < (4 + (int32_t)FONT_HEIGHT); ++y)
        {
            for (int32_t x = 4; x < (4 + (int32_t)FONT_WIDTH); ++x)
            {
                if (GraphicsPixelAt(&KernelGlyphSurface, x, y) == ink)
                {
                    ++lit;
                }
            }
        }

        KernelConsoleRequire(lit > 0U,
                             "an unmapped code drew nothing rather than the replacement "
                             "glyph");
    }

    /* A glyph is clipped by the surface rather than wrapping or faulting. */
    GraphicsClear(&KernelGlyphSurface, 0U);
    FontDrawGlyph(&KernelGlyphSurface, -4, -4, (uint8_t)'A', ink);
    FontDrawGlyph(&KernelGlyphSurface, 100, 100, (uint8_t)'A', ink);
    KernelConsoleRequire(GraphicsPixelAt(&KernelGlyphSurface, 15, 15) == 0U,
                         "a glyph drawn off the surface wrapped to the far side of it");

    /*
     * The transparent glyph and the opaque one must light **the same ink
     * pixels**, for every glyph in the face.
     *
     * This is the assertion the optimisation of Section 23 required. The console
     * was changed from filling a cell and drawing a glyph over it to drawing both
     * in one pass, and the two routines now stand side by side: the transparent
     * one is what the earlier tests above assert against the font's own bytes,
     * and the opaque one is what every character of the boot log actually goes
     * through. A difference between them would be a difference no other test
     * here could see, because no other test uses the path the console uses.
     *
     * Every code is checked rather than a sample, the whole face costing two
     * hundred drawings upon a surface in memory and the fault being of exactly
     * the kind that afflicts one character and no other.
     */
    {
        bool agreed = true;

        for (uint32_t code = FONT_FIRST_CODE; code <= FONT_LAST_CODE && agreed; ++code)
        {
            GraphicsClear(&KernelGlyphSurface, 0U);
            FontDrawGlyph(&KernelGlyphSurface, 4, 4, (uint8_t)code, ink);

            for (int32_t y = 4; y < (4 + (int32_t)FONT_HEIGHT); ++y)
            {
                for (int32_t x = 4; x < (4 + (int32_t)FONT_WIDTH); ++x)
                {
                    KernelOpaqueExpected[y - 4][x - 4] =
                        GraphicsPixelAt(&KernelGlyphSurface, x, y) == ink;
                }
            }

            GraphicsClear(&KernelGlyphSurface, 0U);
            FontDrawGlyphOpaque(&KernelGlyphSurface, 4, 4, (uint8_t)code, ink, 0x00303030U);

            for (int32_t y = 4; y < (4 + (int32_t)FONT_HEIGHT); ++y)
            {
                for (int32_t x = 4; x < (4 + (int32_t)FONT_WIDTH); ++x)
                {
                    const uint32_t got = GraphicsPixelAt(&KernelGlyphSurface, x, y);
                    const bool wanted = KernelOpaqueExpected[y - 4][x - 4];

                    if (got != (wanted ? ink : 0x00303030U))
                    {
                        agreed = false;
                    }
                }
            }

            if (!agreed)
            {
                KernelWriteString("  the two glyph routines disagree, at code ");
                KernelWriteHexadecimal(code);
                KernelWriteString("\n");
                KernelConsoleSucceeded = false;
            }
        }
    }

    /* The opaque glyph is clipped as the transparent one is. The console draws
     * every character through it, so a cell at the edge of a screen whose extent
     * is not a whole number of cells must be cut off and not wrapped. */
    GraphicsClear(&KernelGlyphSurface, 0U);
    FontDrawGlyphOpaque(&KernelGlyphSurface, -4, -4, (uint8_t)'A', ink, 0x00303030U);
    FontDrawGlyphOpaque(&KernelGlyphSurface, 100, 100, (uint8_t)'A', ink, 0x00303030U);
    KernelConsoleRequire(GraphicsPixelAt(&KernelGlyphSurface, 15, 15) == 0U,
                         "an opaque glyph drawn off the surface wrapped to the far side "
                         "of it");
}

/*
 * Asserts the control characters upon the live console.
 *
 * Only characters that draw nothing are used — carriage return, tabulation and
 * backspace — so the boot log this is written into is not disturbed by the test
 * of it. The position is left at the first column of a fresh line afterwards, so
 * that the line following this test begins where it would have begun anyway.
 */
static void KernelVerifyConsoleControl(void)
{
    uint32_t column;

    if (!ConsoleIsActive())
    {
        return;
    }

    KernelConsoleRequire((ConsoleColumns() * FONT_WIDTH) <= FramebufferWidth(),
                         "the console claims more columns than the framebuffer holds");
    KernelConsoleRequire((ConsoleRows() * FONT_HEIGHT) <= FramebufferHeight(),
                         "the console claims more rows than the framebuffer holds");
    KernelConsoleRequire((ConsoleColumns() > 0U) && (ConsoleRows() > 0U),
                         "the console has no extent");

    /* A carriage return returns to the first column of the row it is upon, and
     * does not change the row. */
    {
        const uint32_t row = ConsoleRow();

        ConsoleWriteCharacter('\r');
        KernelConsoleRequire(ConsoleColumn() == 0U,
                             "a carriage return did not return to the first column");
        KernelConsoleRequire(ConsoleRow() == row,
                             "a carriage return changed the row");
    }

    /*
     * A tabulation advances to the next multiple of eight columns, not onward by
     * eight. The distinction is the whole of what a tabulation is for, and the
     * test is made from column zero and again from a column that is already a
     * multiple, which is the case a careless implementation gets wrong by
     * standing still.
     */
    ConsoleWriteCharacter('\t');
    column = ConsoleColumn();
    KernelConsoleRequire((column % 8U) == 0U,
                         "a tabulation did not land upon a multiple of eight columns");
    KernelConsoleRequire(column == 8U,
                         "a tabulation from column zero did not advance to column eight");

    ConsoleWriteCharacter('\t');
    KernelConsoleRequire(ConsoleColumn() == 16U,
                         "a tabulation from a multiple of eight did not advance to the "
                         "next one");

    /* A backspace moves one position back and does not erase; the assertion that
     * it does not erase is that the position moved by exactly one. */
    ConsoleWriteCharacter('\b');
    KernelConsoleRequire(ConsoleColumn() == 15U,
                         "a backspace did not move exactly one position");

    /*
     * The erase limit. A backspace may not retreat past it, which is what keeps
     * an echo loop from erasing a prompt it did not write.
     */
    ConsoleSetEraseLimit();
    ConsoleWriteCharacter('\b');
    KernelConsoleRequire(ConsoleColumn() == 15U,
                         "a backspace retreated past the erase limit");

    ConsoleWriteCharacter('\r');
    ConsoleSetEraseLimit();
    {
        const uint32_t row = ConsoleRow();

        ConsoleWriteCharacter('\b');
        KernelConsoleRequire((ConsoleColumn() == 0U) && (ConsoleRow() == row),
                             "a backspace at the first column crossed to the row above "
                             "although the limit stood there");
    }

    /* Leave the position where the next line of the log expects it. */
    ConsoleWriteCharacter('\r');
    ConsoleSetEraseLimit();
}

void KernelVerifyConsole(void)
{
    KernelConsoleSucceeded = true;

    KernelVerifyFont();
    KernelVerifyGlyphDrawing();
    KernelVerifyConsoleControl();

    KernelWriteString(KernelConsoleSucceeded ? "Console self-test passed.\n"
                                             : "Console self-test FAILED.\n");
}
