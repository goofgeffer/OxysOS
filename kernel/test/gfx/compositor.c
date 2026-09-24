/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/gfx/compositor.c
 * Purpose: Asserts the compositing of sub-task 6.6: the clip stack, the
 *          blending of one colour into another, the accumulation of the region
 *          that has changed, and the composition of layers over a back buffer.
 * Key functions: KernelVerifyCompositing, KernelVerifyCompositor.
 * References:
 *   - docs/design/COMPOSITOR.md: the design of the compositor, and
 *     Section 27.5, this file's assertions paired with what each would catch.
 *
 * Two groups, and the division between them is deliberate.
 *
 *   The clip stack and the blending are asserted **against surfaces composed in
 *   memory**, as every drawing primitive since sub-task 6.3 has been, so that
 *   the whole of that holds upon a machine with no display. The compositor
 *   itself cannot be: it owns one back buffer and one framebuffer and there is
 *   no second of either to compose. What is asserted there is what can be —
 *   the damage arithmetic, the layer table, and that a presentation empties the
 *   damage — and the rest is judged by looking at the screen.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/gfx/compositor.h>
#include <oxys/gfx/framebuffer.h>
#include <oxys/gfx/graphics.h>

/*
 * The surfaces the compositing primitives are asserted upon.
 *
 * The pitch exceeds the width and the padding holds a sentinel, which is the
 * arrangement of kernel/test/verify_graphics.c and exists for the same reason: a
 * primitive that computed an address from the width instead of the pitch writes
 * into the padding and is caught rather than merely being wrong.
 */
#define KERNEL_COMPOSITE_WIDTH  16U
#define KERNEL_COMPOSITE_HEIGHT 12U
#define KERNEL_COMPOSITE_PITCH  (KERNEL_COMPOSITE_WIDTH + 3U)

static uint32_t KernelCompositeStore[KERNEL_COMPOSITE_HEIGHT * KERNEL_COMPOSITE_PITCH];
/*
 * The composited source is deliberately **narrower** than the destination.
 *
 * Equal widths were the first arrangement and they made the stride assertion
 * unfalsifiable: an index taken from the destination's width and one taken from
 * the source's are the same number, so the fault the assertion names could not
 * be produced. Seven by five against sixteen by twelve makes them differ at
 * every row after the first.
 */
#define KERNEL_SOURCE_WIDTH  7U
#define KERNEL_SOURCE_HEIGHT 5U

static uint32_t KernelCompositeSource[KERNEL_SOURCE_HEIGHT * KERNEL_SOURCE_WIDTH];
static uint8_t KernelCompositeMask[KERNEL_SOURCE_HEIGHT * KERNEL_SOURCE_WIDTH];

#define KERNEL_COMPOSITE_SENTINEL UINT32_C(0xA5A5A5A5)

static bool KernelCompositeSucceeded;

static void KernelCompositeRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelCompositeSucceeded = false;
    }
}

/* Composes the destination: the visible area cleared, the padding a sentinel. */
static void KernelCompositePrepare(GraphicsSurface *surface, uint32_t colour)
{
    for (size_t index = 0U;
         index < (KERNEL_COMPOSITE_HEIGHT * KERNEL_COMPOSITE_PITCH); ++index)
    {
        KernelCompositeStore[index] = KERNEL_COMPOSITE_SENTINEL;
    }

    (void)GraphicsSurfaceInitialise(surface, KernelCompositeStore, KERNEL_COMPOSITE_WIDTH,
                                    KERNEL_COMPOSITE_HEIGHT, KERNEL_COMPOSITE_PITCH * 4U,
                                    4U);
    GraphicsClear(surface, colour);
}

/* Whether the padding beyond every row is still the sentinel. */
static bool KernelCompositePaddingIsIntact(void)
{
    for (uint32_t row = 0U; row < KERNEL_COMPOSITE_HEIGHT; ++row)
    {
        for (uint32_t column = KERNEL_COMPOSITE_WIDTH; column < KERNEL_COMPOSITE_PITCH;
             ++column)
        {
            if (KernelCompositeStore[(row * KERNEL_COMPOSITE_PITCH) + column] !=
                KERNEL_COMPOSITE_SENTINEL)
            {
                return false;
            }
        }
    }

    return true;
}

/*
 * Asserts the clip stack and the blending, upon surfaces composed in memory.
 *
 * Both were promised to this sub-task by the limitations of Section 17: the clip
 * had no stack, so a caller nesting regions had to save and restore it by hand,
 * and every colour was opaque.
 */
void KernelVerifyCompositing(void)
{
    GraphicsSurface surface;
    GraphicsSurface source;
    const GraphicsRectangle whole = { 0, 0, KERNEL_COMPOSITE_WIDTH,
                                      KERNEL_COMPOSITE_HEIGHT };
    const GraphicsRectangle half = { 0, 0, KERNEL_COMPOSITE_WIDTH / 2U,
                                     KERNEL_COMPOSITE_HEIGHT };
    const GraphicsRectangle quarter = { 0, 0, KERNEL_COMPOSITE_WIDTH / 4U,
                                        KERNEL_COMPOSITE_HEIGHT };
    const GraphicsRectangle right = { (int32_t)(KERNEL_COMPOSITE_WIDTH / 2U), 0,
                                      KERNEL_COMPOSITE_WIDTH / 2U,
                                      KERNEL_COMPOSITE_HEIGHT };

    KernelCompositeSucceeded = true;

    KernelWriteString("Compositing: asserting the clip stack and the blend.\n");

    /* --- The clip stack. --- */

    KernelCompositePrepare(&surface, 0U);

    KernelCompositeRequire(GraphicsClipDepth(&surface) == 0U,
                           "a fresh surface has a clip already pushed");

    KernelCompositeRequire(GraphicsPushClip(&surface, half),
                           "a push upon an empty stack was refused");
    KernelCompositeRequire(GraphicsClip(&surface).width == (KERNEL_COMPOSITE_WIDTH / 2U),
                           "a push did not narrow the clip");

    /*
     * A push intersects; it does not replace. A push that replaced the clip
     * could widen it, and a caller nesting a panel within a region it had been
     * handed would then draw outside that region — which is the one thing the
     * clip exists to prevent.
     */
    KernelCompositeRequire(GraphicsPushClip(&surface, whole),
                           "a second push was refused");
    KernelCompositeRequire(GraphicsClip(&surface).width == (KERNEL_COMPOSITE_WIDTH / 2U),
                           "a push to a wider region widened the clip");

    KernelCompositeRequire(GraphicsPopClip(&surface), "a pop with two pushed was refused");
    KernelCompositeRequire(GraphicsClip(&surface).width == (KERNEL_COMPOSITE_WIDTH / 2U),
                           "a pop did not restore the clip the push had saved");

    KernelCompositeRequire(GraphicsPopClip(&surface), "a pop with one pushed was refused");
    KernelCompositeRequire(GraphicsClip(&surface).width == KERNEL_COMPOSITE_WIDTH,
                           "the last pop did not restore the whole surface");

    /*
     * A pop with nothing pushed is refused and changes nothing. Left to
     * underflow, it would take a rectangle from beyond the array and every later
     * drawing would be clipped to whatever that held.
     */
    KernelCompositeRequire(!GraphicsPopClip(&surface),
                           "a pop upon an empty stack was accepted");
    KernelCompositeRequire(GraphicsClip(&surface).width == KERNEL_COMPOSITE_WIDTH,
                           "a refused pop altered the clip");

    /* The stack is bounded, and a push beyond it is refused rather than
     * overwriting the entry beneath. */
    {
        uint32_t pushed = 0U;

        while (GraphicsPushClip(&surface, whole))
        {
            ++pushed;

            if (pushed > (GRAPHICS_CLIP_DEPTH + 2U))
            {
                break;
            }
        }

        KernelCompositeRequire(pushed == GRAPHICS_CLIP_DEPTH,
                               "the clip stack accepted more than its depth");
    }

    /* A reset abandons the stack. A reset that left it standing would let a
     * later pop narrow the surface to a region long since finished with. */
    GraphicsResetClip(&surface);
    KernelCompositeRequire(GraphicsClipDepth(&surface) == 0U,
                           "a reset left saved clips upon the stack");

    /* Drawing obeys the pushed clip, and the padding is untouched throughout. */
    KernelCompositePrepare(&surface, 0U);
    (void)GraphicsPushClip(&surface, quarter);
    GraphicsFillRectangle(&surface, whole, FramebufferEncode(255U, 255U, 255U));
    (void)GraphicsPopClip(&surface);

    KernelCompositeRequire(
        GraphicsPixelAt(&surface, (int32_t)(KERNEL_COMPOSITE_WIDTH / 4U), 0) == 0U,
        "a fill escaped the clip a push had established");
    KernelCompositeRequire(KernelCompositePaddingIsIntact(),
                           "a clipped fill wrote into the row padding");

    /* --- The blend. --- */

    KernelCompositePrepare(&surface, FramebufferEncode(0U, 0U, 0U));

    /*
     * Full coverage is exactly an opaque write and no coverage is no write at
     * all. The two ends are asserted because they are the cases a blend is
     * likeliest to get subtly wrong, and because everything drawn before this
     * sub-task relies upon the first of them being unchanged.
     */
    GraphicsBlendPixel(&surface, 0, 0, FramebufferEncode(255U, 255U, 255U), 255U);
    KernelCompositeRequire(GraphicsPixelAt(&surface, 0, 0) ==
                               FramebufferEncode(255U, 255U, 255U),
                           "full coverage did not write the colour");

    GraphicsBlendPixel(&surface, 1, 0, FramebufferEncode(255U, 255U, 255U), 0U);
    KernelCompositeRequire(GraphicsPixelAt(&surface, 1, 0) == FramebufferEncode(0U, 0U, 0U),
                           "no coverage wrote something");

    /*
     * Half coverage of white over black is a grey, and the assertion is that it
     * lies between rather than at either end — the exact value depending upon
     * the channel widths the display reports, which are the boot loader's to
     * choose and not this test's to assume.
     */
    {
        uint8_t red = 0U;
        uint8_t green = 0U;
        uint8_t blue = 0U;

        GraphicsBlendPixel(&surface, 2, 0, FramebufferEncode(255U, 255U, 255U), 128U);
        FramebufferDecode(GraphicsPixelAt(&surface, 2, 0), &red, &green, &blue);

        KernelCompositeRequire((red > 64U) && (red < 192U),
                               "half coverage of white over black is not a middling grey");
        KernelCompositeRequire((red == green) && (green == blue),
                               "a blend of grey over grey produced a colour");
    }

    /*
     * The channels are combined apart. A packed pixel interpolated as a whole
     * carries out of one channel and into the next, so red over blue would
     * produce a green that neither contains — which upon a grey display is
     * invisible and upon a colour one is a fault nobody could explain.
     */
    {
        uint8_t red = 0U;
        uint8_t green = 0U;
        uint8_t blue = 0U;

        GraphicsPutPixel(&surface, 3, 0, FramebufferEncode(0U, 0U, 255U));
        GraphicsBlendPixel(&surface, 3, 0, FramebufferEncode(255U, 0U, 0U), 128U);
        FramebufferDecode(GraphicsPixelAt(&surface, 3, 0), &red, &green, &blue);

        KernelCompositeRequire(green < 32U, "a blend of red over blue produced green");
        KernelCompositeRequire((red > 64U) && (blue > 64U),
                               "a blend of red over blue lost one of them");
    }

    /* A blend is clipped as every other write is. */
    KernelCompositePrepare(&surface, 0U);
    (void)GraphicsPushClip(&surface, right);
    GraphicsBlendPixel(&surface, 0, 0, FramebufferEncode(255U, 255U, 255U), 128U);
    (void)GraphicsPopClip(&surface);
    KernelCompositeRequire(GraphicsPixelAt(&surface, 0, 0) == 0U,
                           "a blend escaped the clip");

    /* --- A masked surface composited over another. --- */

    KernelCompositePrepare(&surface, FramebufferEncode(0U, 0U, 0U));

    /*
     * A diagonal, and not alternating pixels.
     *
     * Alternating pixels was the first form of this and it asserted nothing: the
     * source is sixteen wide, so every row begins upon the same parity and a
     * mask indexed by any even stride produces the same picture. A diagonal
     * shears the moment the stride is wrong, which is the fault being looked
     * for. The test was corrected, not the code — but a test that cannot fail is
     * worse than none, because it is counted among those that pass.
     */
    for (uint32_t row = 0U; row < KERNEL_SOURCE_HEIGHT; ++row)
    {
        for (uint32_t column = 0U; column < KERNEL_SOURCE_WIDTH; ++column)
        {
            const size_t index = (row * KERNEL_SOURCE_WIDTH) + column;

            KernelCompositeSource[index] = FramebufferEncode(255U, 255U, 255U);
            KernelCompositeMask[index] = (row == column) ? 255U : 0U;
        }
    }

    (void)GraphicsSurfaceInitialise(&source, KernelCompositeSource, KERNEL_SOURCE_WIDTH,
                                    KERNEL_SOURCE_HEIGHT, KERNEL_SOURCE_WIDTH * 4U, 4U);

    GraphicsBlendSurface(&surface, 0, 0, &source, KernelCompositeMask);

    /*
     * The diagonal arrives as a diagonal. This is the assertion that the mask is
     * indexed by the **source's** width — seven — and not the destination's
     * sixteen or its pitch of nineteen. An index taken from any of the others
     * walks through the mask at the wrong rate and produces a pattern that is
     * plausible and wrong.
     */
    KernelCompositeRequire(GraphicsPixelAt(&surface, 0, 0) ==
                               FramebufferEncode(255U, 255U, 255U),
                           "a covered pixel was not composited");
    KernelCompositeRequire(GraphicsPixelAt(&surface, 1, 0) == FramebufferEncode(0U, 0U, 0U),
                           "an uncovered pixel was composited");
    KernelCompositeRequire(GraphicsPixelAt(&surface, 1, 1) ==
                               FramebufferEncode(255U, 255U, 255U),
                           "the mask was indexed by the wrong stride");
    KernelCompositeRequire(GraphicsPixelAt(&surface, 0, 1) == FramebufferEncode(0U, 0U, 0U),
                           "the mask was indexed by the destination's width");
    KernelCompositeRequire(KernelCompositePaddingIsIntact(),
                           "a composited surface wrote into the row padding");

    KernelWriteString(KernelCompositeSucceeded
                          ? "Compositing: the clip stack and the blend are sound.\n"
                          : "Compositing self-test FAILED.\n");
}

/*
 * Asserts the compositor itself: the damage arithmetic and the layer table.
 *
 * There is one back buffer and one framebuffer and no second of either to
 * compose, so what is asserted here is what does not need a second: that damage
 * accumulates as the enclosing rectangle, that a presentation empties it, that a
 * layer moved reports both where it was and where it is, and that the table
 * refuses more layers than it holds.
 *
 * The composition itself is judged by looking at the screen, and the procedure
 * for that is in docs/project/TESTING.md.
 */
void KernelVerifyCompositor(void)
{
    GraphicsRectangle damage;

    KernelCompositeSucceeded = true;

    if (!CompositorIsActive())
    {
        KernelWriteString("Compositor self-test: no back buffer; nothing to assert.\n");
        return;
    }

    KernelWriteString("Compositor: asserting the damage and the layers.\n");

    /* Whatever stood before this test is carried out, so that the assertions
     * below begin from nothing rather than from the boot log's last line. */
    CompositorPresent();

    KernelCompositeRequire(GraphicsRectangleIsEmpty(CompositorDamage()),
                           "a presentation left damage behind");

    /* --- Damage accumulates as the rectangle enclosing both. --- */

    {
        const GraphicsRectangle first = { 4, 4, 2U, 2U };
        const GraphicsRectangle second = { 20, 30, 2U, 2U };

        CompositorInvalidate(first);
        damage = CompositorDamage();

        KernelCompositeRequire((damage.x == 4) && (damage.y == 4) && (damage.width == 2U) &&
                                   (damage.height == 2U),
                               "one damaged region was not itself");

        CompositorInvalidate(second);
        damage = CompositorDamage();

        /*
         * The union, not the second alone and not a list. Two cells at opposite
         * corners present the whole screen, which is this scheme's cost; a
         * compositor that kept only the latest would leave the earlier one
         * standing upon the display for ever.
         */
        KernelCompositeRequire((damage.x == 4) && (damage.y == 4) && (damage.width == 18U) &&
                                   (damage.height == 28U),
                               "two damaged regions did not accumulate as the rectangle "
                               "enclosing both");
    }

    /* Damage outside the buffer is discarded rather than widening the region to
     * cover pixels that do not exist. */
    {
        const GraphicsRectangle outside = { -100, -100, 4U, 4U };
        const GraphicsRectangle before = CompositorDamage();

        CompositorInvalidate(outside);
        damage = CompositorDamage();

        KernelCompositeRequire((damage.x == before.x) && (damage.y == before.y) &&
                                   (damage.width == before.width),
                               "damage outside the buffer was accumulated");
    }

    CompositorPresent();
    KernelCompositeRequire(GraphicsRectangleIsEmpty(CompositorDamage()),
                           "the damage was not emptied by the presentation");

    /* A presentation with nothing changed writes nothing and is not an error:
     * it is what the echo loop does between keystrokes. */
    {
        const uint64_t pixels = CompositorPixelsPresented();

        CompositorPresent();
        KernelCompositeRequire(CompositorPixelsPresented() == pixels,
                               "a presentation with nothing changed carried pixels");
    }

    /* --- The whole buffer is a region like any other. --- */

    CompositorInvalidateAll();
    damage = CompositorDamage();
    KernelCompositeRequire((damage.width == (int32_t)CompositorSurface()->width) &&
                               (damage.height == (int32_t)CompositorSurface()->height) &&
                               (damage.x == 0) && (damage.y == 0),
                           "invalidating everything did not name the whole buffer");
    CompositorPresent();

    /* --- The layer table refuses more than it holds. --- */

    {
        const size_t before = CompositorLayerCount();
        size_t taken[COMPOSITOR_LAYER_CAPACITY];
        size_t added = 0U;
        size_t handle;

        while ((added < COMPOSITOR_LAYER_CAPACITY) &&
               ((handle = CompositorAddLayer(CompositorSurface(), NULL, 0, 0)) !=
                COMPOSITOR_LAYER_NONE))
        {
            taken[added] = handle;
            ++added;
        }

        KernelCompositeRequire((before + added) == COMPOSITOR_LAYER_CAPACITY,
                               "the layer table accepted more or fewer than its capacity");
        KernelCompositeRequire(CompositorAddLayer(CompositorSurface(), NULL, 0, 0) ==
                                   COMPOSITOR_LAYER_NONE,
                               "a layer beyond the table's capacity was accepted");
        KernelCompositeRequire(CompositorAddLayer(NULL, NULL, 0, 0) ==
                                   COMPOSITOR_LAYER_NONE,
                               "a layer with no surface was accepted");

        /*
         * Everything this test took is given back.
         *
         * A self-test that exhausted the table would leave the pointer without a
         * layer, and the machine would boot with no pointer and no failure
         * reported — which is precisely what the first form of this test did.
         */
        for (size_t index = 0U; index < added; ++index)
        {
            CompositorRemoveLayer(taken[index]);
        }

        KernelCompositeRequire(CompositorLayerCount() == before,
                               "the layers this test took were not given back");

        /* Withdrawing a layer that is not there is refused rather than counted,
         * which a repeated removal would otherwise make negative. */
        CompositorRemoveLayer(COMPOSITOR_LAYER_CAPACITY);
        KernelCompositeRequire(CompositorLayerCount() == before,
                               "withdrawing a layer that is not there altered the count");
    }

    KernelWriteString(KernelCompositeSucceeded
                          ? "Compositor self-test passed.\n"
                          : "Compositor self-test FAILED.\n");
}
