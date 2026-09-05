/*
 * File: graphics/cursor.c
 * Purpose: Implements the pointer of sub-task 6.5: the shape drawn for this
 *          project, the store of the pixels it stands upon, the restoration of
 *          them as it moves, and the nested concealment by which anything else
 *          may draw while it is shown.
 * Key functions: CursorInitialise, CursorShow, CursorHide, CursorMoveTo,
 *          CursorConceal, CursorReveal, CursorIsVisible, CursorX, CursorY,
 *          CursorShapeIsOpaque, CursorShapeIsInterior, CursorReport.
 * References:
 *   - docs/devices/MOUSE.md, Sections 7 and 8: the design of the pointer, the
 *     save-under, and every assertion made upon them.
 *   - docs/design/GRAPHICS.md, Section 26: the same, seen from the drawing it
 *     is built upon.
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
 * The save-under, and its one real limitation.
 *
 *   There is one surface until sub-task 6.6, so what the pointer covers must be
 *   read back before it is drawn and written again before it is drawn elsewhere.
 *   Reading is the expensive half: the framebuffer is mapped write-combining and
 *   reads from write-combining memory are uncached, which is the same fact that
 *   made the console's scroll the costly operation in Section 23. It is 216
 *   pixels and is paid once per movement, not once per packet — see CursorMoveTo.
 *
 *   The store is correct only while nothing else draws upon the surface, and
 *   nothing here can detect a violation: every pixel involved holds a value that
 *   something meant to write. CursorConceal and CursorReveal are how the rest of
 *   the kernel declares that it is about to draw, and KernelWriteString — which
 *   is already the one routine permitted to name an output device — is where the
 *   declaration is actually made.
 *
 * Concurrency. The pointer is moved by whoever drains the mouse's event buffer,
 * which is not an interrupt handler, and is concealed by whoever writes to the
 * console, which may be one. The concealment count is therefore the field a
 * handler and the main flow both touch; from sub-task 6.13 it requires the
 * spinlock governing the surface, together with the drawing it guards.
 */

#include <oxys/cursor.h>
#include <oxys/graphics.h>
#include <oxys/kernel.h>

/*
 * The pointer, twelve wide and eighteen high, drawn for this project.
 *
 * Each row is a bit per column with column 0 in bit 11, which is the same
 * convention the font of sub-task 6.4 uses for its glyphs: the leftmost pixel is
 * the most significant bit, so that the number as written reads left to right in
 * the order the pixels are drawn.
 *
 * The picture beside each row is the row, and is the thing to edit; the number
 * is what the row was transcribed to. A row whose number and picture disagree is
 * a fault the self-test cannot see, both being data — which is why they are kept
 * adjacent rather than in a table of numbers with a comment somewhere above.
 *
 *   O denotes the outline, W the interior, and a full stop a pixel the pointer
 *   does not cover.
 */
static const uint16_t CursorOpacity[CURSOR_HEIGHT] = {
    0x800U, /* O...........  */
    0xC00U, /* OO..........  */
    0xE00U, /* OWO.........  */
    0xF00U, /* OWWO........  */
    0xF80U, /* OWWWO.......  */
    0xFC0U, /* OWWWWO......  */
    0xFE0U, /* OWWWWWO.....  */
    0xFF0U, /* OWWWWWWO....  */
    0xFF8U, /* OWWWWWWWO...  */
    0xFFCU, /* OWWWWWWWWO..  */
    0xFFEU, /* OWWWWWWWWWO.  */
    0xFFFU, /* OWWWWWOOOOOO  */
    0xFE0U, /* OWWOWWO.....  */
    0xEF0U, /* OWO.OWWO....  */
    0xCF0U, /* OO..OWWO....  */
    0x878U, /* O....OWWO...  */
    0x078U, /* .....OWWO...  */
    0x038U  /* ......OOO...  */
};

static const uint16_t CursorInterior[CURSOR_HEIGHT] = {
    0x000U, /* O...........  */
    0x000U, /* OO..........  */
    0x400U, /* OWO.........  */
    0x600U, /* OWWO........  */
    0x700U, /* OWWWO.......  */
    0x780U, /* OWWWWO......  */
    0x7C0U, /* OWWWWWO.....  */
    0x7E0U, /* OWWWWWWO....  */
    0x7F0U, /* OWWWWWWWO...  */
    0x7F8U, /* OWWWWWWWWO..  */
    0x7FCU, /* OWWWWWWWWWO.  */
    0x7C0U, /* OWWWWWOOOOOO  */
    0x6C0U, /* OWWOWWO.....  */
    0x460U, /* OWO.OWWO....  */
    0x060U, /* OO..OWWO....  */
    0x030U, /* O....OWWO...  */
    0x030U, /* .....OWWO...  */
    0x000U  /* ......OOO...  */
};

/*
 * The shape must fit the word its rows are held in. Twelve columns in sixteen
 * bits is not close to the limit, but a shape widened to seventeen would
 * silently lose its rightmost column rather than failing to build.
 */
_Static_assert(CURSOR_WIDTH <= 16, "A cursor row is held in a 16-bit word.");

/* The surface the pointer is drawn upon, and whether one was adopted. */
static GraphicsSurface *CursorTarget;
static bool CursorAvailable;

/* The two colours, encoded for that surface. */
static uint32_t CursorOutlineColour;
static uint32_t CursorInteriorColour;

/* Where the pointer is, and whether the operator is meant to see it. */
static int32_t CursorPositionX;
static int32_t CursorPositionY;
static bool CursorVisible;

/*
 * The depth of concealment. Zero means nothing is drawing; above zero the
 * pointer is off the surface however visible it is meant to be.
 *
 * It is a count and not a flag because the situations nest, and a flag would let
 * the inner reveal put the pointer back in the middle of the outer party's
 * drawing. See <oxys/cursor.h>.
 */
static uint32_t CursorConcealment;

/*
 * The pixels the pointer presently stands upon, and where they came from.
 *
 * `CursorSavedValid` is not redundant with the visibility: the pointer may be
 * visible and concealed, in which case nothing is saved and nothing must be put
 * back. Restoring an unsaved store would paint eighteen rows of whatever the
 * array last held.
 */
static uint32_t CursorSaved[CURSOR_HEIGHT][CURSOR_WIDTH];
static int32_t CursorSavedX;
static int32_t CursorSavedY;
static bool CursorSavedValid;

/* Accounting. */
static uint64_t CursorDraws;
static uint64_t CursorRestores;

/* Whether the pixel at this column and row is covered by the pointer at all. */
bool CursorShapeIsOpaque(int32_t column, int32_t row)
{
    if ((column < 0) || (column >= CURSOR_WIDTH) || (row < 0) || (row >= CURSOR_HEIGHT))
    {
        return false;
    }

    return (CursorOpacity[row] & (uint16_t)(1U << (CURSOR_WIDTH - 1 - column))) != 0U;
}

/* Whether it is drawn in the interior colour rather than the outline colour. */
bool CursorShapeIsInterior(int32_t column, int32_t row)
{
    if ((column < 0) || (column >= CURSOR_WIDTH) || (row < 0) || (row >= CURSOR_HEIGHT))
    {
        return false;
    }

    return (CursorInterior[row] & (uint16_t)(1U << (CURSOR_WIDTH - 1 - column))) != 0U;
}

/*
 * Records the pixels the pointer is about to cover.
 *
 * The whole rectangle is saved and not merely the opaque pixels within it. The
 * saving is a read of the surface either way, and a rectangle is one loop with
 * no test in it, where a shape would be a loop that consulted the mask twice —
 * once here and once in the restore — and would have to agree with itself both
 * times about a shape that may have been edited in between.
 *
 * Pixels outside the surface are read as zero by GraphicsPixelAt and written
 * back by GraphicsPutPixel to nowhere, the clip refusing them. A pointer half
 * off the edge therefore needs no special case here.
 */
static void CursorSaveUnder(int32_t x, int32_t y)
{
    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            CursorSaved[row][column] = GraphicsPixelAt(CursorTarget, x + column, y + row);
        }
    }

    CursorSavedX = x;
    CursorSavedY = y;
    CursorSavedValid = true;
}

/* Puts back what was saved, and forgets it. */
static void CursorRestoreUnder(void)
{
    if (!CursorSavedValid)
    {
        return;
    }

    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            GraphicsPutPixel(CursorTarget, CursorSavedX + column, CursorSavedY + row,
                             CursorSaved[row][column]);
        }
    }

    CursorSavedValid = false;
    ++CursorRestores;
}

/* Draws the shape at the position, having saved what is beneath it. */
static void CursorDrawAt(int32_t x, int32_t y)
{
    CursorSaveUnder(x, y);

    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        const uint16_t opacity = CursorOpacity[row];
        const uint16_t interior = CursorInterior[row];

        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            const uint16_t bit = (uint16_t)(1U << (CURSOR_WIDTH - 1 - column));

            if ((opacity & bit) == 0U)
            {
                continue;
            }

            GraphicsPutPixel(CursorTarget, x + column, y + row,
                             ((interior & bit) != 0U) ? CursorInteriorColour
                                                      : CursorOutlineColour);
        }
    }

    ++CursorDraws;
}

/* Whether the pointer ought to be standing upon the surface at this moment. */
static bool CursorShouldBeDrawn(void)
{
    return CursorAvailable && CursorVisible && (CursorConcealment == 0U);
}

bool CursorInitialise(GraphicsSurface *surface, uint32_t outline, uint32_t interior)
{
    CursorTarget = NULL;
    CursorAvailable = false;
    CursorVisible = false;
    CursorConcealment = 0U;
    CursorSavedValid = false;
    CursorPositionX = 0;
    CursorPositionY = 0;
    CursorDraws = 0U;
    CursorRestores = 0U;

    if ((surface == NULL) || (surface->pixels == NULL) || (surface->width == 0U) ||
        (surface->height == 0U))
    {
        return false;
    }

    CursorTarget = surface;
    CursorOutlineColour = outline;
    CursorInteriorColour = interior;
    CursorAvailable = true;

    return true;
}

bool CursorIsAvailable(void)
{
    return CursorAvailable;
}

bool CursorIsVisible(void)
{
    return CursorVisible;
}

void CursorShow(void)
{
    if (!CursorAvailable || CursorVisible)
    {
        return;
    }

    CursorVisible = true;

    if (CursorConcealment == 0U)
    {
        CursorDrawAt(CursorPositionX, CursorPositionY);
    }
}

void CursorHide(void)
{
    if (!CursorAvailable || !CursorVisible)
    {
        return;
    }

    CursorVisible = false;
    CursorRestoreUnder();
}

void CursorMoveTo(int32_t x, int32_t y)
{
    if (!CursorAvailable)
    {
        return;
    }

    /*
     * A movement to where the pointer already is costs nothing. This is not a
     * refinement: the mouse reports at a hundred packets a second and a hand at
     * rest still produces the button packets, so without this the pointer would
     * be erased and redrawn — 432 uncached reads and 216 writes — a hundred times
     * a second for as long as nobody moved it.
     */
    if ((x == CursorPositionX) && (y == CursorPositionY))
    {
        return;
    }

    if (CursorShouldBeDrawn())
    {
        /*
         * The position is advanced before the repair and not after, which is
         * what makes the saved coordinates load-bearing rather than a copy of
         * the position: the pixels are put back where they came from, which by
         * this point is no longer where the pointer is.
         *
         * Written the other way round the two would agree at every restore, and
         * a restore that used the position instead of the saved coordinates
         * would be indistinguishable from a correct one — until some later
         * caller moved the pointer and repaired afterwards, at which point it
         * would paint a rectangle of stale pixels over whatever stood at the new
         * position and leave the old one holding an arrow for ever.
         */
        CursorPositionX = x;
        CursorPositionY = y;
        CursorRestoreUnder();
        CursorDrawAt(x, y);

        return;
    }

    CursorPositionX = x;
    CursorPositionY = y;
}

void CursorConceal(void)
{
    if (!CursorAvailable)
    {
        return;
    }

    /*
     * Only the outermost concealment takes the pointer off the surface. The
     * count is incremented first so that the test reads as the state after this
     * call rather than before it.
     */
    ++CursorConcealment;

    if ((CursorConcealment == 1U) && CursorVisible)
    {
        CursorRestoreUnder();
    }
}

void CursorReveal(void)
{
    if (!CursorAvailable || (CursorConcealment == 0U))
    {
        return;
    }

    --CursorConcealment;

    if ((CursorConcealment == 0U) && CursorVisible)
    {
        CursorDrawAt(CursorPositionX, CursorPositionY);
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

uint64_t CursorDrawCount(void)
{
    return CursorDraws;
}

uint64_t CursorRestoreCount(void)
{
    return CursorRestores;
}

void CursorReport(void)
{
    KernelWriteString("Pointer: ");

    if (!CursorAvailable)
    {
        KernelWriteString("no surface; nothing is drawn.\n");
        return;
    }

    KernelWriteString(CursorVisible ? "shown" : "hidden");
    KernelWriteString(" at ");
    KernelWriteDecimal((uint64_t)(uint32_t)CursorPositionX);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)(uint32_t)CursorPositionY);
    KernelWriteString("; drawn ");
    KernelWriteDecimal(CursorDraws);
    KernelWriteString(", restored ");
    KernelWriteDecimal(CursorRestores);
    KernelWriteString(", concealment ");
    KernelWriteDecimal((uint64_t)CursorConcealment);
    KernelWriteString(".\n");
}
