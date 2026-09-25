/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/devices.c
 * Purpose: Phase 4: the timer, the real-time clock, the PS/2 controller, the
 *          keyboard, the terminal, the mouse and the pointer upon the display.
 * Key functions: KernelInitialiseDevices.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order that fixes
 *     where this phase stands in KernelMain, and the order within it.
 *
 * Moved out of kernel/kernel.c on 2026-09-25, unchanged in order, when
 * KernelMain was reduced to the driver that calls one function per phase;
 * kernel/init/internal.h says why.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/dev/pit.h>
#include <oxys/dev/rtc.h>
#include <oxys/dev/ps2.h>
#include <oxys/dev/keyboard.h>
#include <oxys/dev/mouse.h>
#include <oxys/terminal/terminal.h>
#include <oxys/gfx/framebuffer.h>
#include <oxys/gfx/graphics.h>
#include <oxys/gfx/compositor.h>
#include <oxys/gfx/cursor.h>

/*
 * Gives the pointer a display, and tells the mouse how large that display is.
 *
 * Neither is done by either driver, and for the same reason in both directions.
 * The mouse has no idea what it is pointing at, so the bounds are told to it by
 * whoever knows the display; the pointer draws in pixel values, so the encoding
 * of black and white is supplied by whoever knows the framebuffer. This routine
 * is where those two pieces of knowledge meet, and it is in the entry point
 * because the entry point is what establishes both.
 *
 * A machine the boot loader left in a text mode has no surface to draw upon.
 * That is not a failure: the mouse still reports, its events are still decoded,
 * and nothing is drawn.
 */
static void KernelAttachPointer(void)
{
    const GraphicsSurface *const display = CompositorSurface();

    if (display == NULL)
    {
        return;
    }

    MouseSetBounds((int32_t)display->width, (int32_t)display->height);
    MouseSetPosition((int32_t)(display->width / 2U), (int32_t)(display->height / 2U));

    /*
     * Black within white. The two are chosen so that the pointer is visible upon
     * whatever it stands over: a single colour disappears against itself, and the
     * console draws light text upon a dark ground, which either alone would be
     * lost in.
     */
    (void)CursorInitialise(FramebufferEncode(0U, 0U, 0U),
                           FramebufferEncode(255U, 255U, 255U));

    CursorMoveTo(MouseX(), MouseY());
}

void KernelInitialiseDevices(void)
{
    /*
     * The timer is the first device to claim a request line, and therefore the
     * first proof that the whole path from a device to a handler is sound.
     */
    PitInitialise(PIT_DEFAULT_FREQUENCY);
    KernelVerifyPit();
    PitReport();

    /*
     * The real-time clock of sub-task 9.7, read once, after the timer and not
     * before: the time afterwards is this reading advanced by the timer's
     * count, and a reading taken before the timer counted would be advanced
     * from nothing. A machine whose clock reads no date is not in error; it
     * has no time, and `time` says so. The self-test asserts the arithmetic
     * whether or not there is a clock.
     */
    (void)RtcInitialise();
    KernelVerifyRtc();
    RtcReport();

    /*
     * The 8042 controller, before either of the devices upon it.
     *
     * It is one device shared by two drivers, and its configuration byte governs
     * both ports and is written whole. Establishing it here, once, is what allows
     * the keyboard and the mouse to be drivers for their own devices rather than
     * two parties each reconfiguring a controller the other is using.
     */
    (void)Ps2Initialise();
    Ps2Report();

    /*
     * The keyboard is the last device of Phase 3. A machine without one is not
     * in error, so the return value is recorded rather than acted upon; the
     * report and the self-test both accommodate its absence.
     */
    (void)KeyboardInitialise();
    KernelVerifyKeyboard();
    KeyboardReport();

    /*
     * Sub-task 8.1: the terminal, which is the one byte stream a program reads
     * as its standard input. It draws upon the keyboard above and upon the
     * serial adapter, whose receiver is made interrupt-driven later in this
     * sequence; the self-test drives the keyboard decoder and needs neither
     * device present.
     */
    TerminalInitialise();
    KernelVerifyTerminal();
    TerminalReport();

    /*
     * Sub-task 6.5: the mouse upon the controller's second port, and the pointer
     * drawn from it.
     *
     * Both are established here, among the devices, rather than with the drawing
     * of Phase 6 above. The reason is the interrupt flag: this is the last point
     * at which it is still clear, and a decoder self-test that composed packets
     * while a real mouse was delivering its own would be asserting upon a stream
     * it did not compose. The self-tests therefore run before anything sets it,
     * and the pointer is attached to the display afterwards.
     */
    (void)MouseInitialise();
    KernelVerifyMouse();

    /*
     * The pointer is attached before it is asserted, and that order is new at
     * sub-task 6.6.
     *
     * Until then the pointer's self-test composed a surface of its own and drew
     * upon it, so it needed nothing to have been attached. It now asserts the
     * rendering the compositor will actually draw from, which does not exist
     * until the pointer has a layer — and a test that ran first would report,
     * every time and correctly, that there was nothing to look at.
     */
    KernelAttachPointer();
    KernelVerifyCursor();
    MouseReport();
    CursorReport();
}
