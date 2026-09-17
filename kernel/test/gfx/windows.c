/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/gfx/windows.c
 * Purpose: Asserts the window manager of sub-task 9.1 upon a screen composed
 *          in memory — the stack, the focus, the routing of keys and movements,
 *          the binding of the pointer by a held button, the drag, the close
 *          control, the confinement of a move, the bounds of the table and of a
 *          queue — and the disc primitive the frame is drawn with. Until
 *          sub-task 9.2 it also held the demonstration the default boot entry
 *          presents; that is a program now, userland/windows/main.c, run at
 *          privilege level 3 through the client protocol.
 * Key functions: KernelVerifyWindows, KernelVerifyCircle.
 * References:
 *   - docs/design/WINDOWS.md, Section 6: every assertion here paired with the
 *     silent failure it would catch.
 *   - docs/design/DRAWING.md, Section 4.1: the disc, and its assertions.
 *
 * The screen is composed in memory, as every graphical assertion since
 * sub-task 6.3 has been, and for the reason the compositor's self-test gives
 * for what it could not do: the manager composes into whatever surface it is
 * given, so it is given one whose every pixel can be read back, and the
 * stacking order is asserted by reading the pixel where two windows overlap
 * rather than by asking the manager which it believes is on top. The pitch
 * exceeds the width and the padding holds a sentinel, so a composition that
 * escaped the surface is caught rather than merely being wrong.
 *
 * The self-test gives back every window it takes, for the reason the
 * compositor's does: the table is bounded, and a test that left it full
 * would leave the client self-test after it with nothing to make its windows
 * from and no failure reported.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/gfx/window.h>
#include <oxys/gfx/graphics.h>

#define KERNEL_SCREEN_WIDTH  160U
#define KERNEL_SCREEN_HEIGHT 120U
#define KERNEL_SCREEN_PITCH  (KERNEL_SCREEN_WIDTH + 5U)
#define KERNEL_SCREEN_SENTINEL UINT32_C(0x5A5A5A5A)

static uint32_t KernelScreenStore[KERNEL_SCREEN_HEIGHT * KERNEL_SCREEN_PITCH];

/*
 * Seven colours no two of which are alike, and two more for the contents,
 * so that a pixel read back names the thing that drew it.
 */
static const WindowPalette KernelTestPalette = {
    UINT32_C(0x00111111), UINT32_C(0x00222222), UINT32_C(0x00333333),
    UINT32_C(0x00444444), UINT32_C(0x00555555), UINT32_C(0x00666666),
    UINT32_C(0x00777777)
};

#define KERNEL_FILL_A UINT32_C(0x00AAAAAA)
#define KERNEL_FILL_B UINT32_C(0x00BBBBBB)

static bool KernelWindowSucceeded;

static void KernelWindowRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelWindowSucceeded = false;
    }
}

static void KernelScreenPrepare(GraphicsSurface *surface)
{
    for (size_t index = 0U; index < (KERNEL_SCREEN_HEIGHT * KERNEL_SCREEN_PITCH); ++index)
    {
        KernelScreenStore[index] = KERNEL_SCREEN_SENTINEL;
    }

    (void)GraphicsSurfaceInitialise(surface, KernelScreenStore, KERNEL_SCREEN_WIDTH,
                                    KERNEL_SCREEN_HEIGHT, KERNEL_SCREEN_PITCH * 4U, 4U);
}

static bool KernelScreenPaddingIsIntact(void)
{
    for (uint32_t row = 0U; row < KERNEL_SCREEN_HEIGHT; ++row)
    {
        for (uint32_t column = KERNEL_SCREEN_WIDTH; column < KERNEL_SCREEN_PITCH; ++column)
        {
            if (KernelScreenStore[(row * KERNEL_SCREEN_PITCH) + column] != KERNEL_SCREEN_SENTINEL)
            {
                return false;
            }
        }
    }

    return true;
}

static uint32_t KernelScreenPixel(int32_t x, int32_t y)
{
    return KernelScreenStore[((uint32_t)y * KERNEL_SCREEN_PITCH) + (uint32_t)x];
}

static bool KernelRectangleEquals(GraphicsRectangle rectangle, int32_t x, int32_t y,
                                  int32_t width, int32_t height)
{
    return (rectangle.x == x) && (rectangle.y == y) && (rectangle.width == width) &&
           (rectangle.height == height);
}

/* Whether `outer` contains the whole of `inner`. */
static bool KernelRectangleCovers(GraphicsRectangle outer, GraphicsRectangle inner)
{
    return (inner.x >= outer.x) && (inner.y >= outer.y) &&
           (inner.x + inner.width <= outer.x + outer.width) &&
           (inner.y + inner.height <= outer.y + outer.height);
}

/* A movement of the mouse, as the driver would report it. */
static MouseEvent KernelMouse(int32_t x, int32_t y, int16_t delta_x, int16_t delta_y,
                              uint8_t buttons, uint8_t changed)
{
    MouseEvent event;

    event.x = x;
    event.y = y;
    event.delta_x = delta_x;
    event.delta_y = delta_y;
    event.delta_wheel = 0;
    event.buttons = buttons;
    event.changed = changed;

    return event;
}

static KeyEvent KernelKey(char character)
{
    KeyEvent event;

    event.scancode = 0x10U;
    event.character = character;
    event.modifiers = 0U;
    event.pressed = true;
    event.extended = false;

    return event;
}

/* Reads the next event of a window and says whether it is of the kind. */
static bool KernelNextEventIs(size_t window, WindowEventKind kind, WindowEvent *event)
{
    return WindowReadEvent(window, event) && (event->kind == kind);
}

static void KernelFillContent(size_t window, uint32_t colour)
{
    GraphicsSurface *const surface = WindowSurface(window);

    if (surface != NULL)
    {
        GraphicsClear(surface, colour);
        WindowInvalidate(window, GraphicsSurfaceBounds(surface));
    }
}

void KernelVerifyWindows(void)
{
    GraphicsSurface screen;
    WindowEvent event;
    size_t a;
    size_t b;
    GraphicsRectangle composed;

    KernelWindowSucceeded = true;
    KernelWriteString("Window manager: asserting the stack, the focus and the routing.\n");

    KernelScreenPrepare(&screen);
    KernelWindowRequire(WindowManagerInitialise(&screen, &KernelTestPalette, NULL),
                        "the manager refused a surface in memory");

    /* --- Creation, the stack and the focus. --- */

    a = WindowCreate(10, 10, 60, 40, "A");
    b = WindowCreate(40, 30, 60, 40, "B");

    KernelWindowRequire((a != WINDOW_NONE) && (b != WINDOW_NONE) && (a != b),
                        "two windows could not be made, or were made the same");
    KernelWindowRequire((WindowManagerStackAt(0U) == a) && (WindowManagerStackAt(1U) == b) &&
                            (WindowManagerCount() == 2U),
                        "the second window made does not stand above the first");
    KernelWindowRequire(WindowManagerFocused() == b,
                        "the window made last does not hold the focus");
    KernelWindowRequire(KernelRectangleEquals(WindowFrame(a), 10, 10, 62, 66),
                        "the frame is not the content plus the band and the border");
    KernelWindowRequire(KernelRectangleEquals(WindowContentBounds(a), 11, 35, 60, 40),
                        "the content does not stand beneath the band, within the border");
    KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_FOCUS_IN, &event) &&
                            KernelNextEventIs(a, WINDOW_EVENT_FOCUS_OUT, &event) &&
                            (WindowEventsQueued(a) == 0U),
                        "the first window was not told it gained and then lost the focus");
    KernelWindowRequire(KernelNextEventIs(b, WINDOW_EVENT_FOCUS_IN, &event) &&
                            (WindowEventsQueued(b) == 0U),
                        "the second window was not told it gained the focus");

    /* --- Composition: the order is read from the pixels. --- */

    KernelFillContent(a, KERNEL_FILL_A);
    KernelFillContent(b, KERNEL_FILL_B);
    composed = WindowManagerCompose();

    KernelWindowRequire(KernelRectangleEquals(composed, 0, 0, (int32_t)KERNEL_SCREEN_WIDTH,
                                              (int32_t)KERNEL_SCREEN_HEIGHT),
                        "the first composition did not cover the whole screen");
    KernelWindowRequire(GraphicsRectangleIsEmpty(WindowManagerDamage()),
                        "a composition left the changed region standing");
    KernelWindowRequire(KernelScreenPixel(50, 60) == KERNEL_FILL_B,
                        "where two windows overlap the lower one shows");
    KernelWindowRequire(KernelScreenPixel(20, 40) == KERNEL_FILL_A,
                        "a window's content is not drawn where nothing covers it");
    KernelWindowRequire(KernelScreenPixel(150, 110) == KernelTestPalette.ground,
                        "the ground is not drawn where no window stands");
    KernelWindowRequire(KernelScreenPixel(45, 33) == KernelTestPalette.title_focused,
                        "the focused window's band is not in the focused colour");
    KernelWindowRequire(KernelScreenPixel(15, 13) == KernelTestPalette.title_unfocused,
                        "an unfocused window's band is not in the unfocused colour");
    KernelWindowRequire(KernelScreenPixel(10, 10) == KernelTestPalette.border,
                        "the frame's corner is not the border colour");
    KernelWindowRequire(KernelScreenPaddingIsIntact(), "a composition wrote into the padding");

    /* --- The hit test. --- */

    KernelWindowRequire(WindowManagerWindowAt(50, 60) == b,
                        "the hit test in the overlap does not name the upper window");
    KernelWindowRequire(WindowManagerWindowAt(20, 40) == a,
                        "the hit test does not find a window where it alone stands");
    KernelWindowRequire(WindowManagerWindowAt(150, 110) == WINDOW_NONE,
                        "the hit test finds a window upon the ground");

    /* --- A press raises, focuses, is delivered, and binds the pointer. --- */

    {
        const MouseEvent press = KernelMouse(20, 40, 0, 0, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_LEFT);

        WindowManagerHandleMouse(&press);
    }

    KernelWindowRequire((WindowManagerStackAt(1U) == a) && (WindowManagerFocused() == a),
                        "a press in the lower window did not raise and focus it");
    KernelWindowRequire(KernelNextEventIs(b, WINDOW_EVENT_FOCUS_OUT, &event) &&
                            (WindowEventsQueued(b) == 0U),
                        "the window that lost the focus to a press was not told");
    KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_FOCUS_IN, &event),
                        "the window pressed in was not told it gained the focus");
    KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_BUTTON_PRESS, &event) &&
                            (event.x == 9) && (event.y == 5) &&
                            (event.button == MOUSE_BUTTON_LEFT) &&
                            (event.buttons == MOUSE_BUTTON_LEFT),
                        "the press did not reach the window in its content's coordinates");
    KernelWindowRequire(WindowManagerGrabbed() == a,
                        "a press in a window's content did not bind the pointer to it");

    composed = WindowManagerCompose();
    KernelWindowRequire(KernelRectangleCovers(composed, WindowFrame(a)),
                        "raising a window did not mark the whole of its frame as changed");
    KernelWindowRequire(KernelScreenPixel(50, 60) == KERNEL_FILL_A,
                        "after a raise the raised window is not drawn over the other");

    /* --- The binding: everything goes to the bound window until release. --- */

    {
        const MouseEvent move = KernelMouse(90, 80, 70, 40, MOUSE_BUTTON_LEFT, 0U);
        const MouseEvent release = KernelMouse(90, 80, 0, 0, 0U, MOUSE_BUTTON_LEFT);
        const MouseEvent after = KernelMouse(95, 85, 5, 5, 0U, 0U);

        WindowManagerHandleMouse(&move);
        KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_POINTER_MOVE, &event) &&
                                (event.x == 79) && (event.y == 45),
                            "a movement while bound did not go to the bound window");
        KernelWindowRequire(WindowEventsQueued(b) == 0U,
                            "a movement while bound reached the window under the pointer");

        WindowManagerHandleMouse(&release);
        KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_BUTTON_RELEASE, &event) &&
                                (event.button == MOUSE_BUTTON_LEFT) && (event.buttons == 0U),
                            "the release did not go to the window that saw the press");
        KernelWindowRequire(WindowManagerGrabbed() == WINDOW_NONE,
                            "releasing the last button did not unbind the pointer");

        WindowManagerHandleMouse(&after);
        KernelWindowRequire(KernelNextEventIs(b, WINDOW_EVENT_POINTER_MOVE, &event) &&
                                (event.x == 54) && (event.y == 30),
                            "after the release a movement did not go to the window beneath it");
        KernelWindowRequire(WindowEventsQueued(a) == 0U,
                            "after the release the formerly bound window still received");
    }

    /* --- A key goes to the focus and nowhere else. --- */

    {
        const KeyEvent key = KernelKey('k');

        WindowManagerHandleKey(&key);
        KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_KEY, &event) &&
                                (event.key.character == 'k') && event.key.pressed,
                            "a key did not reach the window holding the focus");
        KernelWindowRequire(WindowEventsQueued(b) == 0U,
                            "a key reached a window not holding the focus");
    }

    /* --- A drag by the title band moves the window and tells nobody. --- */

    {
        const MouseEvent press = KernelMouse(30, 20, 0, 0, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_LEFT);
        const MouseEvent move = KernelMouse(40, 30, 10, 10, MOUSE_BUTTON_LEFT, 0U);
        const MouseEvent release = KernelMouse(40, 30, 0, 0, 0U, MOUSE_BUTTON_LEFT);

        WindowManagerHandleMouse(&press);
        WindowManagerHandleMouse(&move);
        WindowManagerHandleMouse(&release);

        KernelWindowRequire(KernelRectangleEquals(WindowFrame(a), 20, 20, 62, 66),
                            "a drag by the band did not move the window by the pointer's movement");
        KernelWindowRequire(WindowEventsQueued(a) == 0U,
                            "a drag by the band was reported to the window as if in its content");
        composed = WindowManagerCompose();
        KernelWindowRequire(KernelRectangleCovers(composed, GraphicsRectangleIntersect(
                                (GraphicsRectangle){ 10, 10, 72, 76 },
                                GraphicsSurfaceBounds(&screen))),
                            "a move did not mark both where the window was and where it is");
        KernelWindowRequire(KernelScreenPixel(12, 12) == KernelTestPalette.ground,
                            "a moved window's old frame still stands upon the screen");
    }

    /* --- The close control asks; it does not destroy. --- */

    {
        const MouseEvent press = KernelMouse(67, 33, 0, 0, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_LEFT);
        const MouseEvent release = KernelMouse(67, 33, 0, 0, 0U, MOUSE_BUTTON_LEFT);

        WindowManagerHandleMouse(&press);
        WindowManagerHandleMouse(&release);

        KernelWindowRequire(KernelNextEventIs(a, WINDOW_EVENT_CLOSE, &event) &&
                                (WindowEventsQueued(a) == 0U),
                            "the close control did not deliver a close event, alone");
        KernelWindowRequire(WindowExists(a) && (WindowManagerGrabbed() == WINDOW_NONE),
                            "the close control destroyed the window, or bound the pointer");
    }

    /* --- A move is confined so that the band stays reachable. --- */

    WindowMove(a, -1000, -1000);
    KernelWindowRequire(KernelRectangleEquals(WindowFrame(a), -14, 0, 62, 66),
                        "a window moved off the top left was not held where its band is reachable");
    WindowMove(a, 1000, 1000);
    KernelWindowRequire(KernelRectangleEquals(WindowFrame(a), 112, 94, 62, 66),
                        "a window moved off the bottom right was not held where its band is reachable");

    /* --- Destruction passes the focus; the last leaves none. --- */

    WindowDestroy(a);
    KernelWindowRequire((WindowManagerFocused() == b) && (WindowManagerCount() == 1U),
                        "destroying the focused window did not pass the focus to the one beneath");
    KernelWindowRequire(KernelNextEventIs(b, WINDOW_EVENT_FOCUS_IN, &event),
                        "the window the focus passed to was not told");
    KernelWindowRequire(!WindowExists(a) && (WindowSurface(a) == NULL),
                        "a destroyed window still exists");

    composed = WindowManagerCompose();
    KernelWindowRequire(KernelRectangleCovers(composed, (GraphicsRectangle){ 112, 94, 48, 26 }),
                        "destroying a window did not mark where it stood as changed");

    WindowDestroy(b);
    KernelWindowRequire((WindowManagerFocused() == WINDOW_NONE) && (WindowManagerCount() == 0U),
                        "destroying the last window left a focus or a stack");

    {
        const uint64_t discarded = WindowManagerEventsDiscarded();
        const KeyEvent key = KernelKey('x');

        WindowManagerHandleKey(&key);
        KernelWindowRequire(WindowManagerEventsDiscarded() == discarded + 1U,
                            "a key with no window to take it was not counted as discarded");
    }

    /* --- The table is bounded, and the queue is bounded. --- */

    {
        size_t made[WINDOW_CAPACITY];
        size_t count = 0U;

        for (size_t index = 0U; index < WINDOW_CAPACITY; ++index)
        {
            made[index] = WindowCreate(0, 0, 16, 16, "n");

            if (made[index] != WINDOW_NONE)
            {
                ++count;
            }
        }

        KernelWindowRequire(count == WINDOW_CAPACITY,
                            "the table did not take as many windows as its capacity");
        KernelWindowRequire(WindowCreate(0, 0, 16, 16, "over") == WINDOW_NONE,
                            "the table took a window beyond its capacity");
        KernelWindowRequire(WindowCreate(0, 0, 8, 8, "small") == WINDOW_NONE,
                            "a window below the least extent was made");

        for (size_t index = 0U; index < WINDOW_CAPACITY; ++index)
        {
            WindowDestroy(made[index]);
        }

        KernelWindowRequire(WindowManagerCount() == 0U,
                            "the windows made to fill the table were not all given back");
    }

    {
        const size_t c = WindowCreate(0, 0, 16, 16, "queue");
        const KeyEvent key = KernelKey('q');

        (void)KernelNextEventIs(c, WINDOW_EVENT_FOCUS_IN, &event);

        for (size_t index = 0U; index < WINDOW_EVENT_CAPACITY + 8U; ++index)
        {
            WindowManagerHandleKey(&key);
        }

        KernelWindowRequire((WindowEventsQueued(c) == WINDOW_EVENT_CAPACITY) &&
                                (WindowEventsDropped(c) == 8U),
                            "a full queue did not drop the newest and count it");

        WindowDestroy(c);
    }

    KernelWindowRequire(WindowManagerCount() == 0U, "the self-test left a window behind");
    KernelWindowRequire(KernelScreenPaddingIsIntact(), "something wrote into the padding");

    /* The manager gives the test's surface up, since sub-task 9.2: the client
     * self-test takes it again for its own run, and the entry point gives the
     * manager the real screen or nothing. */
    WindowManagerShutdown();
    KernelWindowRequire(!WindowManagerIsActive() && (WindowCreate(0, 0, 16, 16, "x") == WINDOW_NONE),
                        "a manager that gave its screen up still makes windows");

    KernelWriteString(KernelWindowSucceeded ? "Window manager self-test passed.\n"
                                            : "Window manager self-test FAILED.\n");
}

/* ------------------------------------------------------------ the disc */

#define KERNEL_DISC_WIDTH  32U
#define KERNEL_DISC_HEIGHT 32U
#define KERNEL_DISC_PITCH  (KERNEL_DISC_WIDTH + 3U)

static uint32_t KernelDiscStore[KERNEL_DISC_HEIGHT * KERNEL_DISC_PITCH];

static bool KernelDiscPixel(int32_t x, int32_t y)
{
    return KernelDiscStore[((uint32_t)y * KERNEL_DISC_PITCH) + (uint32_t)x] == 1U;
}

/*
 * Asserts the disc upon a surface in memory: that it is symmetric in both
 * axes, that it reaches exactly the radius along each axis and no further,
 * that its diagonal is where the integer algorithm places it, and that one
 * clipped at the edge writes nothing beyond the surface.
 */
void KernelVerifyCircle(void)
{
    GraphicsSurface surface;
    bool symmetric = true;
    bool bounded = true;

    KernelWindowSucceeded = true;
    KernelWriteString("Disc: asserting the filled circle.\n");

    for (size_t index = 0U; index < (KERNEL_DISC_HEIGHT * KERNEL_DISC_PITCH); ++index)
    {
        KernelDiscStore[index] = 0U;
    }

    (void)GraphicsSurfaceInitialise(&surface, KernelDiscStore, KERNEL_DISC_WIDTH,
                                    KERNEL_DISC_HEIGHT, KERNEL_DISC_PITCH * 4U, 4U);

    GraphicsFillCircle(&surface, 16, 16, 5, 1U);

    KernelWindowRequire(KernelDiscPixel(16, 16), "the centre of the disc is not filled");
    KernelWindowRequire(KernelDiscPixel(21, 16) && KernelDiscPixel(11, 16) &&
                            KernelDiscPixel(16, 21) && KernelDiscPixel(16, 11),
                        "the disc does not reach its radius along the axes");
    KernelWindowRequire(!KernelDiscPixel(22, 16) && !KernelDiscPixel(10, 16) &&
                            !KernelDiscPixel(16, 22) && !KernelDiscPixel(16, 10),
                        "the disc reaches beyond its radius along an axis");
    KernelWindowRequire(KernelDiscPixel(19, 20) && !KernelDiscPixel(20, 20),
                        "the diagonal is not where the integer algorithm places it");

    /* Row and column zero have no mirror within the surface and are skipped;
     * the disc lies nowhere near them in any case. */
    for (int32_t y = 1; y < (int32_t)KERNEL_DISC_HEIGHT; ++y)
    {
        for (int32_t x = 1; x < (int32_t)KERNEL_DISC_WIDTH; ++x)
        {
            const bool set = KernelDiscPixel(x, y);

            if ((set != KernelDiscPixel(32 - x, y)) || (set != KernelDiscPixel(x, 32 - y)))
            {
                symmetric = false;
            }

            if (set && ((x < 11) || (x > 21) || (y < 11) || (y > 21)))
            {
                bounded = false;
            }
        }
    }

    KernelWindowRequire(symmetric, "the disc is not symmetric about its centre");
    KernelWindowRequire(bounded, "the disc has a pixel outside its bounding square");

    for (size_t index = 0U; index < (KERNEL_DISC_HEIGHT * KERNEL_DISC_PITCH); ++index)
    {
        KernelDiscStore[index] = 0U;
    }

    GraphicsFillCircle(&surface, 0, 0, 6, 1U);

    {
        bool padding_clean = true;

        for (uint32_t row = 0U; row < KERNEL_DISC_HEIGHT; ++row)
        {
            for (uint32_t column = KERNEL_DISC_WIDTH; column < KERNEL_DISC_PITCH; ++column)
            {
                if (KernelDiscStore[(row * KERNEL_DISC_PITCH) + column] != 0U)
                {
                    padding_clean = false;
                }
            }
        }

        KernelWindowRequire(KernelDiscPixel(0, 0) && KernelDiscPixel(6, 0) && padding_clean,
                            "a disc at the corner was not clipped to the surface");
    }

    KernelWriteString(KernelWindowSucceeded ? "Disc self-test passed.\n"
                                            : "Disc self-test FAILED.\n");
}
