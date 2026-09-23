/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/gfx/mark.c
 * Purpose: Asserts the mark of art/logo.h — the invariant its mixing relies
 *          upon, in every byte of the table; that it is a disc upon nothing and
 *          has an edge that is neither; its reduction; and the colours at either
 *          end of its coverage.
 * Key functions: KernelVerifyMark.
 * References:
 *   - art/logo.h: the table and the three functions that draw it.
 *   - art/README.md: the command that made the table.
 *   - docs/design/SESSION.md, Section 3.2: where it is drawn, and every
 *     assertion here paired with the silent failure it would catch.
 *
 * Why a generated table is asserted at all.
 *
 *   The table is regenerated whenever somebody draws the mark again, by a
 *   command run by hand, and every failure of that command compiles: an ink
 *   channel wider than its coverage, a ground that was not removed, a
 *   threshold that made the edge all or nothing. None stops the boot; each is
 *   a picture a person would have to notice was wrong.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <logo.h>

static bool VerifyMarkSucceeded;

static void VerifyMarkRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyMarkSucceeded = false;
    }
}

/* ------------------------------------------------------------- the table */

static void VerifyMarkTable(void)
{
    bool ink_within = true;
    bool disc = false;
    bool ink = false;
    bool edge = false;

    for (unsigned index = 0U; index < (unsigned)(LOGO_EXTENT * LOGO_EXTENT); ++index)
    {
        const unsigned covered = (unsigned)LogoCoverage[index] >> 4;
        const unsigned inked = (unsigned)LogoCoverage[index] & 0x0FU;

        ink_within = ink_within && (inked <= covered);
        disc = disc || ((covered == LOGO_LEVELS) && (inked == 0U));
        ink = ink || (inked == LOGO_LEVELS);
        edge = edge || ((covered > 0U) && (covered < LOGO_LEVELS));
    }

    /*
     * LogoMix takes the ink from the coverage in unsigned arithmetic, so a byte
     * with more ink than coverage is a colour from the far end of the range —
     * a speck of some other colour upon the edge of the disc, one pixel wide,
     * wherever the generator's rounding went the wrong way.
     */
    VerifyMarkRequire(ink_within, "a pixel of the mark has more ink than coverage");
    VerifyMarkRequire(disc && ink, "the mark lacks either its disc or its figure");
    VerifyMarkRequire(edge, "no pixel of the mark is partly covered, so its edge was generated "
                            "as a staircase");

    /* The corners are outside a disc. A table covered there is the artwork's
     * white ground kept, which draws the mark as a square. */
    VerifyMarkRequire((LogoCoverage[0] == 0U) &&
                          (LogoCoverage[(LOGO_EXTENT * LOGO_EXTENT) - 1] == 0U),
                      "a corner of the mark is covered, so its ground was not removed");
}

/* ---------------------------------------------------------- the sampling */

static void VerifyMarkSampling(void)
{
    const int middle = LOGO_EXTENT / 2;
    unsigned covered;
    unsigned inked;
    unsigned sum = 0U;

    /* One to one, a pixel is its byte of the table. */
    LogoSample(middle, middle, LOGO_EXTENT, &covered, &inked);
    VerifyMarkRequire(covered == ((((unsigned)LogoCoverage[(middle * LOGO_EXTENT) + middle] >> 4) *
                                   LOGO_FULL) /
                                  LOGO_LEVELS),
                      "the mark drawn one to one was not its own table");

    /*
     * Halved, a pixel is the average of the four beneath it. A reduction that
     * took one of the four instead would pass every other assertion here and
     * draw the staircase again upon every screen narrower than 1024.
     */
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 2; ++column)
        {
            sum += (unsigned)LogoCoverage[((middle + row) * LOGO_EXTENT) + middle + column] >> 4;
        }
    }

    LogoSample(middle / 2, middle / 2, LOGO_EXTENT / 2, &covered, &inked);
    VerifyMarkRequire(covered == (((sum * LOGO_FULL) + ((4U * LOGO_LEVELS) / 2U)) /
                                  (4U * LOGO_LEVELS)),
                      "the mark reduced by half was not the average of the pixels beneath");

    /*
     * Enlarged by two, of 2026-09-23, the pixels of a row are not in equal
     * pairs: somewhere along the middle row, which crosses the edge of the disc
     * and the figure, two screen pixels over one pixel of the table differ.
     * Enlargement by repetition makes every pair equal, which is the staircase
     * of the enlargement's size upon a window made full; and no pixel of the
     * enlargement has more ink than coverage.
     */
    {
        bool interpolated = false;
        bool within = true;

        for (int column = 0; column < (2 * LOGO_EXTENT); column += 2)
        {
            unsigned first_covered;
            unsigned first_inked;
            unsigned second_covered;
            unsigned second_inked;

            LogoSample(column, LOGO_EXTENT, 2 * LOGO_EXTENT, &first_covered, &first_inked);
            LogoSample(column + 1, LOGO_EXTENT, 2 * LOGO_EXTENT, &second_covered, &second_inked);

            interpolated = interpolated || (first_covered != second_covered) ||
                           (first_inked != second_inked);
            within = within && (first_inked <= first_covered) && (second_inked <= second_covered);
        }

        VerifyMarkRequire(interpolated, "the mark enlarged by two repeated its pixels in pairs");
        VerifyMarkRequire(within, "the mark enlarged has a pixel with more ink than coverage");
    }

    LogoSample(LOGO_EXTENT, 0, LOGO_EXTENT, &covered, &inked);
    VerifyMarkRequire((covered == 0U) && (inked == 0U), "a position outside the mark was covered");
}

/* ------------------------------------------------------------ the mixing */

static void VerifyMarkMixing(void)
{
    /*
     * The ends are exact. A mix a level off at no coverage tints the whole
     * square about the mark a shade from the ground, which is a box drawn
     * round the mark upon the boot screen by nobody's intention.
     */
    VerifyMarkRequire(LogoMix(233U, 201U, 58U, 0U, 0U) == 233U,
                      "the mark at no coverage was not the ground");
    VerifyMarkRequire(LogoMix(233U, 201U, 58U, LOGO_FULL, 0U) == 201U,
                      "the mark wholly covered and without ink was not the disc");
    VerifyMarkRequire(LogoMix(233U, 201U, 58U, LOGO_FULL, LOGO_FULL) == 58U,
                      "the mark wholly inked was not the ink");
    VerifyMarkRequire(LogoMixPacked(UINT32_C(0x00E9BA3C), UINT32_C(0x00C9A060),
                                    UINT32_C(0x003A280E), 0U, 0U) == UINT32_C(0x00E9BA3C),
                      "the packed mix at no coverage was not the ground in every channel");
}

void KernelVerifyMark(void)
{
    VerifyMarkSucceeded = true;

    KernelWriteString("Mark: asserting the table of art/logo.h, its sampling and its mixing.\n");

    VerifyMarkTable();
    VerifyMarkSampling();
    VerifyMarkMixing();

    KernelWriteString(VerifyMarkSucceeded
                          ? "Mark self-test passed: no pixel inked beyond its coverage, a disc "
                            "with a figure and a smooth edge upon nothing, sampled one to one, "
                            "averaged when halved and interpolated when doubled, and mixed exactly "
                            "at either end.\n"
                          : "Mark self-test FAILED.\n");
}
