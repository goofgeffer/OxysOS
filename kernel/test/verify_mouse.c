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
#include <oxys/framebuffer.h>
#include <oxys/irq.h>
#include <oxys/ps2.h>
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
        if (IrqRegisteredHandler(MOUSE_IRQ) == NULL)
        {
            KernelWriteString("  The mouse did not claim its request line.\n");
            succeeded = false;
        }

        if (IrqLineIsMasked(MOUSE_IRQ))
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

/*
 * Asserts the pointer: that its shape is well formed, and that the surface and
 * coverage the compositor draws from agree with the bitmaps they came from.
 *
 * This changed shape at sub-task 6.6 and the change is the point. Until then the
 * pointer drew itself upon a surface and kept the pixels beneath it, so what
 * there was to assert was the drawing and the restoration — a trail left behind,
 * a background overwritten, a concealment that failed to nest. None of that
 * exists now: the pointer is rendered once and composited, so what is left to
 * assert is the rendering, and the compositing is asserted where it lives, in
 * KernelVerifyCompositor.
 *
 * The rendering is worth its own assertions because it is a conversion between
 * two representations of the same thing, and those are where a shape silently
 * loses a column or gains one.
 */
void KernelVerifyCursor(void)
{
    const GraphicsSurface *image;
    const uint8_t *mask;
    bool succeeded = true;

    /* --- The shape is well formed, whatever it is drawn into. --- */

    /*
     * Every interior pixel is also opaque. The two masks are separate data and
     * nothing but this makes them agree: an interior pixel that were not opaque
     * would be a hole in the shape drawn in the interior colour, which is
     * invisible in the picture comments because each is written beside its own
     * row and neither shows the other.
     */
    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            if (CursorShapeIsInterior(column, row) && !CursorShapeIsOpaque(column, row))
            {
                KernelWriteString("  The shape has an interior pixel it does not "
                                  "cover.\n");
                succeeded = false;
            }
        }
    }

    /* The hot spot is part of the shape. A pointer whose tip is transparent
     * points at a pixel it does not draw, and cannot be aimed. */
    if (!CursorShapeIsOpaque(0, 0))
    {
        KernelWriteString("  The hot spot is not part of the shape.\n");
        succeeded = false;
    }

    /* A shape wholly opaque or wholly transparent is not a pointer, and either
     * is what a mask read with the wrong bit order produces. */
    {
        uint32_t opaque = 0U;

        for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
        {
            for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
            {
                if (CursorShapeIsOpaque(column, row))
                {
                    ++opaque;
                }
            }
        }

        if ((opaque == 0U) || (opaque == (CURSOR_WIDTH * CURSOR_HEIGHT)))
        {
            KernelWriteString("  The shape covers everything or nothing.\n");
            succeeded = false;
        }
    }

    /* --- What the compositor will actually draw. --- */

    image = CursorImageSurface();
    mask = CursorImageMask();

    if (image == NULL)
    {
        /*
         * No compositor, so no pointer, so nothing rendered. Upon a machine the
         * boot loader left in a text mode this is the correct outcome and not a
         * failure; the shape above was asserted regardless, being a table.
         */
        KernelWriteString(succeeded ? "Pointer self-test: shape sound; no display to "
                                      "render upon.\n"
                                    : "Pointer self-test FAILED.\n");
        return;
    }

    if ((image->width != CURSOR_WIDTH) || (image->height != CURSOR_HEIGHT))
    {
        KernelWriteString("  The rendered pointer is not the size of the shape.\n");
        succeeded = false;
    }

    /*
     * The rendering agrees with the bitmaps, pixel for pixel.
     *
     * A covered pixel must have full coverage and the colour its interior bit
     * chooses; an uncovered pixel must have none. The two failures this catches
     * are opposite and both silent: a mask taken from the wrong bitmap gives a
     * pointer that is a solid rectangle, and an interior test inverted gives one
     * drawn inside out, which upon a black background looks almost right.
     */
    for (int32_t row = 0; row < CURSOR_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CURSOR_WIDTH; ++column)
        {
            const size_t index = ((size_t)row * CURSOR_WIDTH) + (size_t)column;
            const bool opaque = CursorShapeIsOpaque(column, row);
            const uint32_t pixel = GraphicsPixelAt(image, column, row);

            if (opaque != (mask[index] == 255U))
            {
                KernelWriteString("  The coverage does not follow the shape.\n");
                succeeded = false;
                break;
            }

            if (!opaque)
            {
                continue;
            }

            if (pixel != (CursorShapeIsInterior(column, row)
                              ? FramebufferEncode(255U, 255U, 255U)
                              : FramebufferEncode(0U, 0U, 0U)))
            {
                KernelWriteString("  A rendered pixel is not the colour its bit "
                                  "chooses.\n");
                succeeded = false;
                break;
            }
        }
    }

    /* --- Showing, hiding and moving are the layer's, and are idempotent. --- */

    {
        const bool was_visible = CursorIsVisible();
        const int32_t saved_x = CursorX();
        const int32_t saved_y = CursorY();
        const uint64_t moves = CursorMoveCount();

        CursorHide();
        CursorShow();
        CursorShow();

        if (!CursorIsVisible())
        {
            KernelWriteString("  Showing the pointer twice left it hidden.\n");
            succeeded = false;
        }

        CursorMoveTo(saved_x + 3, saved_y + 5);

        if ((CursorX() != (saved_x + 3)) || (CursorY() != (saved_y + 5)))
        {
            KernelWriteString("  The pointer did not move where it was sent.\n");
            succeeded = false;
        }

        /*
         * A movement that ends where it began is not a movement. Without this
         * the display would be told the pointer's rectangle had changed a
         * hundred times a second for a hand at rest, the mouse reporting whether
         * or not it has moved.
         */
        CursorMoveTo(CursorX(), CursorY());

        if (CursorMoveCount() != (moves + 1U))
        {
            KernelWriteString("  A movement to where the pointer already stood was "
                              "counted.\n");
            succeeded = false;
        }

        /* Left as it was found, so that the operator's pointer is where the
         * mouse thinks it is. */
        CursorMoveTo(saved_x, saved_y);

        if (!was_visible)
        {
            CursorHide();
        }
    }

    KernelWriteString(succeeded ? "Pointer self-test passed.\n"
                                : "Pointer self-test FAILED.\n");
}
