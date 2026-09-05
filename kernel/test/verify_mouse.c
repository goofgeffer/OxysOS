/*
 * File: kernel/test/verify_mouse.c
 * Purpose: Asserts the PS/2 mouse driver and the pointer drawn from it, being
 *          sub-task 6.5: the decoding and framing of a movement packet, the
 *          nine-bit sign extension, the inversion of the vertical sense, the
 *          confinement of the position, and the pointer's save-under, its
 *          restoration and its nested concealment.
 * Key functions: KernelVerifyMouse, KernelVerifyCursor.
 * References:
 *   - docs/devices/MOUSE.md, Section 8: each assertion below, paired with the
 *     silent failure it catches.
 *   - docs/design/GRAPHICS.md, Section 26: the pointer, seen from the drawing
 *     it is built upon.
 *   - docs/project/TESTING.md, Section 21: the record of these tests.
 *
 * Neither test needs a mouse, and that is the point of both.
 *
 * The decoder is driven through MouseProcessByte, exactly as the keyboard's is
 * driven through KeyboardProcessScancode, so the packet arithmetic is asserted
 * upon a machine at which nobody is moving anything. What cannot be asserted
 * that way — that the device answered, that the line was claimed — is checked
 * against the driver's own report of what it found, and is skipped where it
 * found nothing.
 *
 * The pointer is asserted upon a surface composed in memory rather than upon the
 * framebuffer, for the reason Section 11 of the design gives: a surface in
 * ordinary memory can be read back pixel by pixel, and the framebuffer of a
 * machine with no display cannot be read back at all.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/mouse.h>
#include <oxys/cursor.h>
#include <oxys/graphics.h>
#include <oxys/pic.h>
#include <oxys/ps2.h>

/*
 * The surface the pointer is asserted upon.
 *
 * Its pitch exceeds its width, and the padding beyond each row holds a sentinel,
 * so that a draw which computed an address from the width instead of the pitch
 * writes into the padding and is caught. This is the arrangement of
 * kernel/test/verify_graphics.c and the reason for it is the same.
 *
 * The store is declared as words rather than as bytes because words are what is
 * written into it. An object declared uint8_t[] has that type for its whole
 * life, and writing a uint32_t through a pointer into it is undefined; declaring
 * it as words and taking a byte pointer where a byte pointer is wanted is sound
 * in both directions, and guarantees the alignment the word path requires.
 */
#define KERNEL_CURSOR_SURFACE_WIDTH  40U
#define KERNEL_CURSOR_SURFACE_HEIGHT 32U
#define KERNEL_CURSOR_SURFACE_PITCH  (KERNEL_CURSOR_SURFACE_WIDTH + 3U)

static uint32_t KernelCursorStore[KERNEL_CURSOR_SURFACE_HEIGHT * KERNEL_CURSOR_SURFACE_PITCH];

/* The pixel values used by the pointer test. They are arbitrary and distinct;
 * nothing about the encoding of a real framebuffer is assumed. */
#define KERNEL_CURSOR_BACKGROUND UINT32_C(0x00112233)
#define KERNEL_CURSOR_OUTLINE    UINT32_C(0x00000001)
#define KERNEL_CURSOR_INTERIOR   UINT32_C(0x00FFFFFE)
#define KERNEL_CURSOR_SENTINEL   UINT32_C(0xA5A5A5A5)

/* The bounds the decoder is exercised within. Small, so that a movement can
 * reach an edge in one packet and the clamping is provoked rather than hoped
 * for. */
#define KERNEL_MOUSE_BOUND_WIDTH  200
#define KERNEL_MOUSE_BOUND_HEIGHT 100

/*
 * Feeds one packet to the decoder, of whatever length the device negotiated.
 *
 * The wheel byte is supplied always and consumed only where the length is four,
 * so that one routine serves both devices and a test written for a wheel mouse
 * does not silently feed a three-byte decoder a byte and a third of a packet.
 */
static void KernelMousePacket(uint8_t flags, uint8_t x, uint8_t y, uint8_t wheel)
{
    MouseProcessByte(flags);
    MouseProcessByte(x);
    MouseProcessByte(y);

    if (MousePacketLengthInUse() == 4U)
    {
        MouseProcessByte(wheel);
    }
}

/* The first byte of a packet with no buttons, no signs and no overflow. */
#define KERNEL_MOUSE_FLAGS_PLAIN UINT8_C(0x08)

/*
 * Composes the surface the pointer is asserted upon: the visible area a uniform
 * background, the padding beyond each row a sentinel.
 */
static void KernelCursorPrepareSurface(GraphicsSurface *surface)
{
    for (size_t index = 0U;
         index < (KERNEL_CURSOR_SURFACE_HEIGHT * KERNEL_CURSOR_SURFACE_PITCH); ++index)
    {
        KernelCursorStore[index] = KERNEL_CURSOR_SENTINEL;
    }

    (void)GraphicsSurfaceInitialise(surface, KernelCursorStore, KERNEL_CURSOR_SURFACE_WIDTH,
                                    KERNEL_CURSOR_SURFACE_HEIGHT,
                                    KERNEL_CURSOR_SURFACE_PITCH * 4U, 4U);

    GraphicsClear(surface, KERNEL_CURSOR_BACKGROUND);
}

/* Whether every pixel of the visible area holds the background, and every word
 * of the padding still holds the sentinel. */
static bool KernelCursorSurfaceIsClean(const GraphicsSurface *surface)
{
    for (uint32_t row = 0U; row < KERNEL_CURSOR_SURFACE_HEIGHT; ++row)
    {
        for (uint32_t column = 0U; column < KERNEL_CURSOR_SURFACE_WIDTH; ++column)
        {
            if (GraphicsPixelAt(surface, (int32_t)column, (int32_t)row) !=
                KERNEL_CURSOR_BACKGROUND)
            {
                return false;
            }
        }

        for (uint32_t pad = KERNEL_CURSOR_SURFACE_WIDTH; pad < KERNEL_CURSOR_SURFACE_PITCH;
             ++pad)
        {
            if (KernelCursorStore[(row * KERNEL_CURSOR_SURFACE_PITCH) + pad] !=
                KERNEL_CURSOR_SENTINEL)
            {
                return false;
            }
        }
    }

    return true;
}

/* Whether the padding is untouched, asked on its own where the visible area is
 * expected to hold a drawing. */
static bool KernelCursorPaddingIsIntact(void)
{
    for (uint32_t row = 0U; row < KERNEL_CURSOR_SURFACE_HEIGHT; ++row)
    {
        for (uint32_t pad = KERNEL_CURSOR_SURFACE_WIDTH; pad < KERNEL_CURSOR_SURFACE_PITCH;
             ++pad)
        {
            if (KernelCursorStore[(row * KERNEL_CURSOR_SURFACE_PITCH) + pad] !=
                KERNEL_CURSOR_SENTINEL)
            {
                return false;
            }
        }
    }

    return true;
}

/*
 * Whether the pointer stands at this position, judged pixel by pixel against the
 * shape: every opaque pixel holds its colour and every transparent one still
 * holds the background.
 *
 * The transparent half is the half worth having. A pointer drawn as a solid
 * rectangle would satisfy any test that looked only at the pixels it should have
 * drawn, and would be a twelve by eighteen block obliterating the display around
 * the arrow.
 */
static bool KernelCursorIsDrawnAt(const GraphicsSurface *surface, int32_t x, int32_t y)
{
    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            const int32_t at_x = x + column;
            const int32_t at_y = y + row;
            uint32_t expected;

            if ((at_x < 0) || (at_x >= (int32_t)KERNEL_CURSOR_SURFACE_WIDTH) || (at_y < 0) ||
                (at_y >= (int32_t)KERNEL_CURSOR_SURFACE_HEIGHT))
            {
                continue;
            }

            if (!CursorShapeIsOpaque(column, row))
            {
                expected = KERNEL_CURSOR_BACKGROUND;
            }
            else
            {
                expected = CursorShapeIsInterior(column, row) ? KERNEL_CURSOR_INTERIOR
                                                              : KERNEL_CURSOR_OUTLINE;
            }

            if (GraphicsPixelAt(surface, at_x, at_y) != expected)
            {
                return false;
            }
        }
    }

    return true;
}

void KernelVerifyMouse(void)
{
    MouseEvent event;
    const uint64_t framing_before = MouseFramingErrorCount();
    const uint64_t overflow_before = MouseMovementOverflowCount();
    const uint64_t discarded_before = MouseOverflowCount();
    bool succeeded = true;

    KernelWriteString("Verifying the PS/2 mouse.\n");

    /* --- What can only be asserted where a device answered. --- */

    if (MouseIsPresent())
    {
        if (PicRegisteredHandler(MOUSE_IRQ) == NULL)
        {
            KernelWriteString("  The mouse did not claim its request line.\n");
            succeeded = false;
        }

        if (PicLineIsMasked(MOUSE_IRQ))
        {
            KernelWriteString("  The mouse's request line is masked.\n");
            succeeded = false;
        }

        /*
         * A mouse that answered but whose port the controller never declared
         * usable would mean the two modules disagree about which port was
         * configured, which is exactly the fault that giving the controller one
         * owner was meant to remove.
         */
        if (!Ps2PortIsUsable(PS2_PORT_SECOND))
        {
            KernelWriteString("  A mouse answered upon a port the controller calls "
                              "unusable.\n");
            succeeded = false;
        }
    }
    else
    {
        /*
         * Not a failure. A machine may genuinely have no mouse, and the driver is
         * required to discover that without blocking; reaching this line at all
         * is evidence that it did. The decoder below is asserted regardless,
         * being arithmetic and not a device.
         */
        KernelWriteString("  No mouse was found; the device assertions are skipped.\n");
    }

    if ((MousePacketLengthInUse() != 3U) && (MousePacketLengthInUse() != 4U))
    {
        KernelWriteString("  The packet length is neither three bytes nor four.\n");
        succeeded = false;
    }

    if (!MouseHasWheel() && (MousePacketLengthInUse() != 3U))
    {
        KernelWriteString("  A device with no wheel is sending four-byte packets.\n");
        succeeded = false;
    }

    /* --- Begin from a known state. --- */

    MouseSetBounds(KERNEL_MOUSE_BOUND_WIDTH, KERNEL_MOUSE_BOUND_HEIGHT);
    MouseFlush();
    MouseSetPosition(100, 50);

    if (MouseHasEvent() || MouseReadEvent(&event))
    {
        KernelWriteString("  The buffer is not empty after a flush.\n");
        succeeded = false;
    }

    if ((MouseX() != 100) || (MouseY() != 50))
    {
        KernelWriteString("  The pointer was not placed where it was put.\n");
        succeeded = false;
    }

    /* --- A plain packet moves the pointer by what it carries. --- */

    KernelMousePacket(KERNEL_MOUSE_FLAGS_PLAIN, 10U, 4U, 0U);

    if (!MouseReadEvent(&event))
    {
        KernelWriteString("  A complete packet produced no event.\n");
        succeeded = false;
    }
    else
    {
        if ((event.delta_x != 10) || (event.delta_y != -4))
        {
            /*
             * The vertical sense is the assertion here. A device measures upward
             * as positive and a display downward, so a driver that forwarded the
             * device's sign gives a pointer that moves the wrong way vertically
             * and the right way horizontally — which looks like a broken mouse
             * rather than a broken driver, and is diagnosed as one.
             */
            KernelWriteString("  A movement was decoded with the wrong sense or "
                              "magnitude.\n");
            succeeded = false;
        }

        if ((event.x != 110) || (event.y != 46))
        {
            KernelWriteString("  The position did not follow the movement.\n");
            succeeded = false;
        }

        if ((event.buttons != 0U) || (event.changed != 0U))
        {
            KernelWriteString("  A packet with no button held reported one.\n");
            succeeded = false;
        }
    }

    /* --- The movement is nine bits, not eight. --- */

    /*
     * A magnitude of 255 with the sign bit set is -1, and a magnitude of 0 with
     * the sign bit set is -256. The second is the one that matters: a driver
     * that treated the magnitude as a signed char would decode it as 0 and lose
     * the movement entirely, silently, and only for the largest movements a hand
     * can make in one report period.
     */
    MouseSetPosition(150, 60);
    KernelMousePacket((uint8_t)(KERNEL_MOUSE_FLAGS_PLAIN | 0x10U), 0xFFU, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.delta_x != -1))
    {
        KernelWriteString("  A movement of minus one was not decoded as such.\n");
        succeeded = false;
    }

    MouseSetPosition(150, 60);
    KernelMousePacket((uint8_t)(KERNEL_MOUSE_FLAGS_PLAIN | 0x10U), 0x00U, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.delta_x != -256))
    {
        KernelWriteString("  A movement of minus 256 was decoded as zero.\n");
        succeeded = false;
    }
    else if (event.x != 0)
    {
        KernelWriteString("  A movement past the left edge did not stop at it.\n");
        succeeded = false;
    }

    /* --- The position is confined, at both edges of both axes. --- */

    MouseSetPosition(KERNEL_MOUSE_BOUND_WIDTH - 2, KERNEL_MOUSE_BOUND_HEIGHT - 2);
    KernelMousePacket((uint8_t)(KERNEL_MOUSE_FLAGS_PLAIN | 0x20U), 100U, 0x80U, 0U);

    if (!MouseReadEvent(&event))
    {
        KernelWriteString("  A packet at the far corner produced no event.\n");
        succeeded = false;
    }
    else if ((event.x != (KERNEL_MOUSE_BOUND_WIDTH - 1)) ||
             (event.y != (KERNEL_MOUSE_BOUND_HEIGHT - 1)))
    {
        /*
         * The last legal coordinate is one less than the extent. An off-by-one
         * here puts the pointer's hot spot one pixel outside the display, where
         * the clip silently declines to draw it and the pointer appears to stick
         * short of the edge.
         */
        KernelWriteString("  The pointer was not confined to the bounds.\n");
        succeeded = false;
    }

    /* --- A button transition is reported once, and named. --- */

    MouseFlush();
    KernelMousePacket((uint8_t)(KERNEL_MOUSE_FLAGS_PLAIN | MOUSE_BUTTON_LEFT), 0U, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.buttons != MOUSE_BUTTON_LEFT) ||
        (event.changed != MOUSE_BUTTON_LEFT))
    {
        KernelWriteString("  A button depression was not reported as a change.\n");
        succeeded = false;
    }

    KernelMousePacket((uint8_t)(KERNEL_MOUSE_FLAGS_PLAIN | MOUSE_BUTTON_LEFT), 0U, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.changed != 0U))
    {
        KernelWriteString("  A button held reported a change a second time.\n");
        succeeded = false;
    }

    KernelMousePacket(KERNEL_MOUSE_FLAGS_PLAIN, 0U, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.buttons != 0U) ||
        (event.changed != MOUSE_BUTTON_LEFT))
    {
        KernelWriteString("  A button release was not reported as a change.\n");
        succeeded = false;
    }

    if (MouseButtons() != 0U)
    {
        KernelWriteString("  A released button is still recorded as held.\n");
        succeeded = false;
    }

    /* --- An overflowed movement is discarded and counted, its buttons kept. --- */

    MouseSetPosition(70, 40);
    KernelMousePacket((uint8_t)(KERNEL_MOUSE_FLAGS_PLAIN | 0x40U | MOUSE_BUTTON_RIGHT), 0x7FU,
                      0x7FU, 0U);

    if (!MouseReadEvent(&event))
    {
        KernelWriteString("  An overflowed packet produced no event.\n");
        succeeded = false;
    }
    else if ((event.delta_x != 0) || (event.delta_y != 0) || (event.x != 70) ||
             (event.y != 40))
    {
        /*
         * The device is saying the magnitude it sent is the low bits of a larger
         * movement. Using it would jump the pointer somewhere arbitrary; standing
         * still is the better failure and is the one this asserts.
         */
        KernelWriteString("  An overflowed movement was used rather than discarded.\n");
        succeeded = false;
    }
    else if (event.buttons != MOUSE_BUTTON_RIGHT)
    {
        KernelWriteString("  An overflowed packet lost the buttons in the same byte.\n");
        succeeded = false;
    }

    if (MouseMovementOverflowCount() != (overflow_before + 1U))
    {
        KernelWriteString("  An overflowed movement was not counted.\n");
        succeeded = false;
    }

    /* --- A byte that cannot begin a packet is refused, and the stream recovers. --- */

    MouseFlush();

    /*
     * Bit 3 is set in every first byte the device sends, so a byte without it
     * cannot be one. The recovery is what is asserted: a driver that has lost the
     * framing keeps working and reports movements taken from button bytes for
     * ever, and no value it produces is one the device could not have sent.
     */
    MouseProcessByte(0x00U);
    MouseProcessByte(0x01U);

    if (MouseHasEvent())
    {
        KernelWriteString("  A byte that cannot begin a packet was accepted.\n");
        succeeded = false;
    }

    if (MouseFramingErrorCount() != (framing_before + 2U))
    {
        KernelWriteString("  A mis-framed byte was discarded without being counted.\n");
        succeeded = false;
    }

    MouseSetPosition(90, 45);
    KernelMousePacket(KERNEL_MOUSE_FLAGS_PLAIN, 5U, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.delta_x != 5) || (event.x != 95))
    {
        KernelWriteString("  The decoder did not recover after a mis-framed byte.\n");
        succeeded = false;
    }

    /* --- A flush abandons a packet only partly received. --- */

    /*
     * Without this the next byte to arrive is appended to a fragment whose
     * remaining bytes were discarded, which is precisely the mis-framing the
     * always-set bit exists to prevent — introduced by the routine meant to
     * restore a known state.
     */
    MouseProcessByte(KERNEL_MOUSE_FLAGS_PLAIN);
    MouseProcessByte(3U);
    MouseFlush();

    MouseSetPosition(90, 45);
    KernelMousePacket(KERNEL_MOUSE_FLAGS_PLAIN, 7U, 0U, 0U);

    if (!MouseReadEvent(&event) || (event.delta_x != 7))
    {
        KernelWriteString("  A flush did not abandon a partly received packet.\n");
        succeeded = false;
    }

    /* --- A full buffer discards the newest event, and counts it. --- */

    MouseFlush();

    for (uint32_t index = 0U; index < (MOUSE_BUFFER_CAPACITY + 4U); ++index)
    {
        KernelMousePacket(KERNEL_MOUSE_FLAGS_PLAIN, 1U, 0U, 0U);
    }

    if (MouseOverflowCount() != (discarded_before + 4U))
    {
        KernelWriteString("  A full buffer did not count what it discarded.\n");
        succeeded = false;
    }

    {
        uint32_t drained = 0U;

        while (MouseReadEvent(&event))
        {
            ++drained;
        }

        if (drained != MOUSE_BUFFER_CAPACITY)
        {
            KernelWriteString("  The buffer did not hold exactly its capacity.\n");
            succeeded = false;
        }
    }

    /*
     * The state the operator's mouse will be used in is restored. The counters
     * are not restored and cannot be: they record what this test did, and the
     * report printed afterwards names those figures so that a reader is not left
     * to wonder why a machine nobody touched has decoded seventy packets.
     */
    MouseFlush();
    MouseSetPosition(0, 0);

    KernelWriteString(succeeded ? "Mouse self-test passed.\n" : "Mouse self-test FAILED.\n");
}

void KernelVerifyCursor(void)
{
    static GraphicsSurface surface;
    bool succeeded = true;
    bool interior_seen = false;
    bool opaque_seen = false;

    KernelWriteString("Verifying the pointer.\n");

    /* --- The shape is well formed. --- */

    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            const bool opaque = CursorShapeIsOpaque(column, row);
            const bool interior = CursorShapeIsInterior(column, row);

            opaque_seen = opaque_seen || opaque;
            interior_seen = interior_seen || interior;

            /*
             * A pixel that were interior and not opaque is a hole in the shape
             * drawn in the interior colour: the draw consults the opacity mask
             * and never reaches it, so the interior mask would carry a pixel
             * nothing renders, and the two tables would have drifted apart with
             * no symptom at all.
             */
            if (interior && !opaque)
            {
                KernelWriteString("  The shape has an interior pixel that is not "
                                  "opaque.\n");
                succeeded = false;
            }
        }
    }

    if (!opaque_seen || !interior_seen)
    {
        KernelWriteString("  The shape is empty, or has no interior.\n");
        succeeded = false;
    }

    /* The hot spot is the pixel the pointer points at, and must be part of the
     * pointer; a transparent tip is a pointer that indicates a pixel it does not
     * cover. */
    if (!CursorShapeIsOpaque(0, 0))
    {
        KernelWriteString("  The hot spot is not part of the shape.\n");
        succeeded = false;
    }

    /* --- The pointer draws, and what was beneath it comes back. --- */

    KernelCursorPrepareSurface(&surface);

    if (!CursorInitialise(&surface, KERNEL_CURSOR_OUTLINE, KERNEL_CURSOR_INTERIOR))
    {
        KernelWriteString("  The pointer refused a surface composed in memory.\n");
        KernelWriteString("Pointer self-test FAILED.\n");
        return;
    }

    CursorMoveTo(4, 3);
    CursorShow();

    if (!KernelCursorIsDrawnAt(&surface, 4, 3))
    {
        KernelWriteString("  The pointer was not drawn where it was shown.\n");
        succeeded = false;
    }

    if (!KernelCursorPaddingIsIntact())
    {
        KernelWriteString("  Drawing the pointer wrote into the row padding.\n");
        succeeded = false;
    }

    /*
     * Showing a pointer already shown must draw nothing further. Were it to draw
     * again it would first save what is beneath it, which is now the pointer
     * itself, and the display beneath would be gone for good.
     */
    CursorShow();
    CursorHide();

    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  Hiding the pointer did not restore what was beneath it.\n");
        succeeded = false;
    }

    /* --- A movement restores the old position and draws the new. --- */

    CursorMoveTo(4, 3);
    CursorShow();
    CursorMoveTo(20, 11);

    if (!KernelCursorIsDrawnAt(&surface, 20, 11))
    {
        KernelWriteString("  The pointer was not drawn at the position it moved to.\n");
        succeeded = false;
    }

    /*
     * The pixels at the old position must be the background again. A driver that
     * drew before restoring, or that restored to the new position rather than the
     * saved one, leaves a trail of arrows behind the pointer — which is the
     * commonest way this is got wrong and needs nobody to describe it twice.
     */
    if (GraphicsPixelAt(&surface, 4, 3) != KERNEL_CURSOR_BACKGROUND)
    {
        KernelWriteString("  The pointer left a trail where it had been.\n");
        succeeded = false;
    }

    CursorHide();

    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  The surface was not clean after a move and a hide.\n");
        succeeded = false;
    }

    /* --- A pointer at the edge draws no pixel outside the surface. --- */

    CursorMoveTo((int32_t)KERNEL_CURSOR_SURFACE_WIDTH - 3,
                 (int32_t)KERNEL_CURSOR_SURFACE_HEIGHT - 3);
    CursorShow();

    if (!KernelCursorPaddingIsIntact())
    {
        KernelWriteString("  A pointer at the edge wrote into the row padding.\n");
        succeeded = false;
    }

    if (!KernelCursorIsDrawnAt(&surface, (int32_t)KERNEL_CURSOR_SURFACE_WIDTH - 3,
                               (int32_t)KERNEL_CURSOR_SURFACE_HEIGHT - 3))
    {
        KernelWriteString("  A pointer at the edge did not draw the part that fits.\n");
        succeeded = false;
    }

    CursorHide();

    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  A pointer clipped at the edge did not restore cleanly.\n");
        succeeded = false;
    }

    /* --- Concealment nests. --- */

    CursorMoveTo(10, 6);
    CursorShow();

    CursorConceal();

    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  Concealing the pointer did not take it off the surface.\n");
        succeeded = false;
    }

    CursorConceal();
    CursorReveal();

    /*
     * One reveal of two concealments must do nothing. A flag rather than a count
     * would put the pointer back here, in the middle of the drawing that the
     * outer concealment was protecting, and the save-under would then record the
     * half-finished drawing as the display beneath.
     */
    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  An inner reveal put the pointer back too early.\n");
        succeeded = false;
    }

    CursorReveal();

    if (!KernelCursorIsDrawnAt(&surface, 10, 6))
    {
        KernelWriteString("  The outermost reveal did not put the pointer back.\n");
        succeeded = false;
    }

    if (!CursorIsVisible())
    {
        KernelWriteString("  A concealment altered whether the pointer is shown.\n");
        succeeded = false;
    }

    /*
     * A move made while concealed must leave the surface untouched and take
     * effect when the pointer returns. The alternative is a pointer that draws
     * itself in the middle of somebody else's drawing.
     */
    CursorConceal();
    CursorMoveTo(2, 20);

    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  A move made while concealed drew upon the surface.\n");
        succeeded = false;
    }

    CursorReveal();

    if (!KernelCursorIsDrawnAt(&surface, 2, 20))
    {
        KernelWriteString("  A move made while concealed was not honoured.\n");
        succeeded = false;
    }

    CursorHide();

    if (!KernelCursorSurfaceIsClean(&surface))
    {
        KernelWriteString("  The surface was not clean at the end of the test.\n");
        succeeded = false;
    }

    /*
     * Every draw is matched by a restore, save the one standing upon the surface
     * while the pointer is shown — and the pointer is hidden here, so they must
     * be equal. A leak either way is a save-under that has been taken twice or
     * put back twice, and the second is how a stale rectangle of pixels comes to
     * be painted over something legitimate.
     */
    if (CursorDrawCount() != CursorRestoreCount())
    {
        KernelWriteString("  The pointer was drawn and restored a different number of "
                          "times.\n");
        succeeded = false;
    }

    /*
     * The surface composed above goes out of scope with this test, so the
     * pointer is detached from it. Whoever wants a pointer upon the display
     * initialises it afresh against the framebuffer.
     */
    (void)CursorInitialise(NULL, 0U, 0U);

    KernelWriteString(succeeded ? "Pointer self-test passed.\n"
                                : "Pointer self-test FAILED.\n");
}
