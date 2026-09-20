/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: graphics/window.c
 * Purpose: Implements the window manager of sub-task 9.1: the table of windows,
 *          the stack they are composed in, the focus, the binding of the pointer
 *          to a window by a held button, the frame drawn about each window, and
 *          the routing of keys and movements into the windows' queues.
 * Key functions: WindowManagerInitialise, WindowManagerShutdown, WindowCreate,
 *          WindowLayerOf, WindowSetSession, WindowSession, WindowDrawText,
 *          WindowDestroy, WindowRaise, WindowFocus, WindowMove, WindowInvalidate,
 *          WindowWritePixels, WindowSetOwner, WindowDestroyOwnedBy,
 *          WindowReadEvent, WindowManagerHandleKey, WindowManagerHandleMouse,
 *          WindowManagerCompose, WindowManagerWindowAt, WindowManagerReport.
 * References:
 *   - docs/design/WINDOWS.md: the design, Section 4 the appearance, and
 *     Section 6 every assertion made upon this file paired with what it would
 *     catch.
 *   - X Window System Protocol, X Version 11 Release 7.7, Chapter 11, "Input
 *     Device events": a button press with no grab in progress starts one for
 *     the window that received it, and it ends when every button is released.
 *     The rule is taken as a rule and restated for one window and one pointer;
 *     nothing of that protocol's form is adopted.
 *   - docs/project/INSPIRATIONS.md, Section 3: flat surfaces, depth by
 *     stacking, geometry as the character; and no bevel, ever.
 *
 * The stack is an array of identifiers and not a list.
 *
 *   Sixteen entries, bottom first, with the topmost last. Raising a window is
 *   removing its entry and appending it, which moves at most fifteen words,
 *   and a hit test walks the array from the end. A linked list would be no
 *   shorter to write and would put the stacking order — the one thing a person
 *   sees directly — into pointers that a defect could make circular, where an
 *   array's order is its indices and cannot be.
 *
 * The damage is one rectangle, as the compositor's is.
 *
 *   The region a composition must redraw accumulates as the rectangle enclosing
 *   every change since the last, for the reason docs/design/COMPOSITOR.md,
 *   Section 2.3, gives: it cannot be exhausted. Within that rectangle the ground
 *   and every intersecting window are painted again, bottom to top; a window
 *   that had not changed is repainted where it intersects, which is the cost of
 *   a scheme that keeps no list. It is the compositor's own cost, and the same
 *   rectangle is what the caller carries to the compositor.
 *
 * Concurrency. The table, the stack, the focus and the queues are touched by
 * whoever routes events and by whoever composes — the bootstrap processor's
 * timer tick, which runs with interrupts masked — and, since sub-task 9.2, by
 * a client's system calls, which run upon the bootstrap processor with
 * interrupts masked as well, every user thread being pinned there. Nothing
 * here is locked: the two cannot interleave upon the one processor that
 * reaches either. A user thread upon a second processor is what would make
 * the queue a producer and a consumer upon two processors, and the lock this
 * file would then need is the one docs/design/CONCURRENCY.md, Section 10,
 * limitation 1, lists this file as lacking.
 */

#include <oxys/gfx/window.h>
#include <oxys/gfx/font.h>
#include <oxys/gfx/graphics.h>
#include <oxys/mm/heap.h>
#include <oxys/kernel.h>

/* The close control: a disc of this radius, centred this far in from the
 * frame's right edge, and the square about it that counts as pressing it. */
#define WINDOW_CLOSE_RADIUS 5
#define WINDOW_CLOSE_INSET  14
#define WINDOW_CLOSE_REACH  12

/* The title: the face at twice its size, this far in from the left. */
#define WINDOW_TITLE_SCALE  2
#define WINDOW_TITLE_INSET  8

typedef struct Window
{
    bool in_use;

    /* The frame's top left upon the screen, and the content's extent. */
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    char title[WINDOW_TITLE_CAPACITY + 1U];

    /* The owner's tag, since sub-task 9.2; zero is the kernel's own. */
    uint64_t owner;

    /* The layer it stands in, since sub-task 9.5, and whether it carries a
     * frame — which the layer decides and nothing else may. */
    WindowLayer layer;
    bool decorated;

    /* The content: a tightly packed surface over pixels from the heap. */
    GraphicsSurface surface;
    void *pixels;

    /* The queue: a ring, oldest at `head`, `count` long. */
    WindowEvent events[WINDOW_EVENT_CAPACITY];
    size_t head;
    size_t count;
    uint64_t dropped;
} Window;

static bool WindowActive;
static GraphicsSurface *WindowScreen;
static WindowPalette WindowColours;
static WindowEncodeFunction WindowEncode;

static Window WindowTable[WINDOW_CAPACITY];

/* The stack: identifiers, bottom first. */
static size_t WindowStack[WINDOW_CAPACITY];
static size_t WindowStackCount;

static size_t WindowFocused;

/* The session, of sub-task 9.5: the one owner that may make a root or a panel.
 * Zero is nobody. */
static uint64_t WindowSessionOwner;

/*
 * The window bound to the pointer by a button pressed within its content, and
 * the window being dragged by its title band, with the offset of the pointer
 * from the frame's top left when the drag began. At most one of the two is in
 * force: a press begins whichever the press's position selects, and both end
 * when the last button is released.
 */
static size_t WindowGrabbed;
static size_t WindowDragged;
static int32_t WindowDragOffsetX;
static int32_t WindowDragOffsetY;

/* The pointer's position as last routed, so that a raise or a destruction
 * can be followed by a hit test without waiting for the mouse to move. */
static int32_t WindowPointerX;
static int32_t WindowPointerY;

/* The changed region, and whether there is one. */
static GraphicsRectangle WindowDamage;
static bool WindowDamaged;

static uint64_t WindowRouted;
static uint64_t WindowDiscarded;
static uint64_t WindowCompositions;

/* ------------------------------------------------------------ geometry */

static GraphicsRectangle WindowMakeRectangle(int32_t x, int32_t y, int32_t width,
                                             int32_t height)
{
    GraphicsRectangle rectangle;

    rectangle.x = x;
    rectangle.y = y;
    rectangle.width = width;
    rectangle.height = height;

    return rectangle;
}

static GraphicsRectangle WindowEmptyRectangle(void)
{
    return WindowMakeRectangle(0, 0, 0, 0);
}

static Window *WindowAt(size_t window)
{
    if (!WindowActive || (window >= WINDOW_CAPACITY) || !WindowTable[window].in_use)
    {
        return NULL;
    }

    return &WindowTable[window];
}

/*
 * An undecorated window's frame **is** its content: a root is the whole screen
 * and a panel is a bar, and neither has a band or a border about it. Every
 * routine that asks where a window is asks through these three, so the two
 * kinds differ here and nowhere else.
 */
static GraphicsRectangle WindowFrameOf(const Window *window)
{
    if (!window->decorated)
    {
        return WindowMakeRectangle(window->x, window->y, window->width, window->height);
    }

    return WindowMakeRectangle(window->x, window->y,
                               window->width + (2 * WINDOW_BORDER),
                               window->height + WINDOW_TITLE_HEIGHT + (2 * WINDOW_BORDER));
}

static GraphicsRectangle WindowContentOf(const Window *window)
{
    if (!window->decorated)
    {
        return WindowMakeRectangle(window->x, window->y, window->width, window->height);
    }

    return WindowMakeRectangle(window->x + WINDOW_BORDER,
                               window->y + WINDOW_BORDER + WINDOW_TITLE_HEIGHT,
                               window->width, window->height);
}

/* An undecorated window has no band, and the empty rectangle is what says so:
 * every test against it — the drag, the close — then fails as it should. */
static GraphicsRectangle WindowTitleBandOf(const Window *window)
{
    if (!window->decorated)
    {
        return WindowMakeRectangle(0, 0, 0, 0);
    }

    return WindowMakeRectangle(window->x + WINDOW_BORDER, window->y + WINDOW_BORDER,
                               window->width, WINDOW_TITLE_HEIGHT);
}

/* The square about the close control within which a press closes. */
static GraphicsRectangle WindowCloseReachOf(const Window *window)
{
    const GraphicsRectangle frame = WindowFrameOf(window);

    if (!window->decorated)
    {
        return WindowMakeRectangle(0, 0, 0, 0);
    }

    const int32_t centre_x = frame.x + frame.width - WINDOW_BORDER - WINDOW_CLOSE_INSET;
    const int32_t centre_y = frame.y + WINDOW_BORDER + (WINDOW_TITLE_HEIGHT / 2);

    return WindowMakeRectangle(centre_x - WINDOW_CLOSE_REACH, centre_y - WINDOW_CLOSE_REACH,
                               (2 * WINDOW_CLOSE_REACH) + 1, (2 * WINDOW_CLOSE_REACH) + 1);
}

/*
 * Confines a frame position so that the title band stays reachable: its top
 * never above the screen, its bottom never below it, and WINDOW_REACHABLE_WIDTH
 * pixels of the frame always upon the screen horizontally.
 */
static void WindowConfine(const Window *window, int32_t *x, int32_t *y)
{
    const int32_t screen_width = (int32_t)WindowScreen->width;
    const int32_t screen_height = (int32_t)WindowScreen->height;
    const int32_t frame_width = window->width + (2 * WINDOW_BORDER);
    const int32_t band_height = WINDOW_TITLE_HEIGHT + (2 * WINDOW_BORDER);
    int32_t reach = WINDOW_REACHABLE_WIDTH;

    if (reach > frame_width)
    {
        reach = frame_width;
    }

    if (*x < reach - frame_width)
    {
        *x = reach - frame_width;
    }

    if (*x > screen_width - reach)
    {
        *x = screen_width - reach;
    }

    if (*y < 0)
    {
        *y = 0;
    }

    if (*y > screen_height - band_height)
    {
        *y = screen_height - band_height;
    }
}

/* ------------------------------------------------------------- damage */

static void WindowDamageAdd(GraphicsRectangle region)
{
    const GraphicsRectangle bounded =
        GraphicsRectangleIntersect(region, GraphicsSurfaceBounds(WindowScreen));

    if (GraphicsRectangleIsEmpty(bounded))
    {
        return;
    }

    if (!WindowDamaged)
    {
        WindowDamage = bounded;
        WindowDamaged = true;
        return;
    }

    {
        const int32_t left = (WindowDamage.x < bounded.x) ? WindowDamage.x : bounded.x;
        const int32_t top = (WindowDamage.y < bounded.y) ? WindowDamage.y : bounded.y;
        const int32_t old_right = WindowDamage.x + WindowDamage.width;
        const int32_t new_right = bounded.x + bounded.width;
        const int32_t old_bottom = WindowDamage.y + WindowDamage.height;
        const int32_t new_bottom = bounded.y + bounded.height;
        const int32_t right = (old_right > new_right) ? old_right : new_right;
        const int32_t bottom = (old_bottom > new_bottom) ? old_bottom : new_bottom;

        WindowDamage = WindowMakeRectangle(left, top, right - left, bottom - top);
    }
}

/* -------------------------------------------------------------- queues */

static void WindowEnqueue(size_t window, const WindowEvent *event)
{
    Window *const target = WindowAt(window);

    if (target == NULL)
    {
        ++WindowDiscarded;
        return;
    }

    if (target->count == WINDOW_EVENT_CAPACITY)
    {
        /*
         * The newest is dropped and not the oldest, for the reason the
         * keyboard's buffer gives: the oldest is the beginning of whatever the
         * owner has not yet read, and a ring that overwrote it would hand the
         * owner a release with no press before it.
         */
        ++target->dropped;
        ++WindowDiscarded;
        return;
    }

    target->events[(target->head + target->count) % WINDOW_EVENT_CAPACITY] = *event;
    ++target->count;
    ++WindowRouted;
}

static void WindowEnqueueKind(size_t window, WindowEventKind kind)
{
    WindowEvent event;

    event.kind = kind;
    event.x = 0;
    event.y = 0;
    event.button = 0U;
    event.buttons = 0U;
    event.key.scancode = 0U;
    event.key.character = '\0';
    event.key.modifiers = 0U;
    event.key.pressed = false;
    event.key.extended = false;

    WindowEnqueue(window, &event);
}

/* A pointer event, with the position made relative to the window's content. */
static void WindowEnqueuePointer(size_t window, WindowEventKind kind, int32_t x, int32_t y,
                                 uint8_t button, uint8_t buttons)
{
    const Window *const target = WindowAt(window);
    WindowEvent event;

    if (target == NULL)
    {
        ++WindowDiscarded;
        return;
    }

    {
        const GraphicsRectangle content = WindowContentOf(target);

        event.kind = kind;
        event.x = x - content.x;
        event.y = y - content.y;
        event.button = button;
        event.buttons = buttons;
        event.key.scancode = 0U;
        event.key.character = '\0';
        event.key.modifiers = 0U;
        event.key.pressed = false;
        event.key.extended = false;
    }

    WindowEnqueue(window, &event);
}

/* --------------------------------------------------------------- stack */

static size_t WindowStackPositionOf(size_t window)
{
    for (size_t position = 0U; position < WindowStackCount; ++position)
    {
        if (WindowStack[position] == window)
        {
            return position;
        }
    }

    return WINDOW_NONE;
}

static void WindowStackRemove(size_t window)
{
    const size_t position = WindowStackPositionOf(window);

    if (position == WINDOW_NONE)
    {
        return;
    }

    for (size_t index = position; index + 1U < WindowStackCount; ++index)
    {
        WindowStack[index] = WindowStack[index + 1U];
    }

    --WindowStackCount;
}

/*
 * Puts a window at the top of its own layer, which is what keeps the stack
 * ordered by layer at every moment: the array is searched for the first entry
 * of a higher layer and the window is inserted before it.
 *
 * The ordering is an invariant of the array rather than a rule applied when it
 * is read, because every reader — the hit test, the composition, the focus
 * passed to "the topmost remaining" — walks it in order and would each have to
 * know the rule separately. One insertion knows it instead.
 */
static void WindowStackInsert(size_t window)
{
    const WindowLayer layer = WindowTable[window].layer;
    size_t position = WindowStackCount;

    for (size_t index = 0U; index < WindowStackCount; ++index)
    {
        if (WindowTable[WindowStack[index]].layer > layer)
        {
            position = index;
            break;
        }
    }

    for (size_t index = WindowStackCount; index > position; --index)
    {
        WindowStack[index] = WindowStack[index - 1U];
    }

    WindowStack[position] = window;
    ++WindowStackCount;
}

/* --------------------------------------------------------------- focus */

/*
 * The topmost window that may hold the focus, or WINDOW_NONE — which is every
 * window but a root. A desktop whose last ordinary window closed would
 * otherwise give the keys to the wallpaper.
 */
static size_t WindowTopmostFocusable(void)
{
    for (size_t position = WindowStackCount; position != 0U; --position)
    {
        const size_t identifier = WindowStack[position - 1U];

        if (WindowTable[identifier].layer != WINDOW_LAYER_ROOT)
        {
            return identifier;
        }
    }

    return WINDOW_NONE;
}

/*
 * Passes the focus, telling the loser before the gainer. The order is what
 * an owner that draws its own state upon a focus change needs: a window told
 * it had gained the focus before the other was told it had lost it would, for
 * one composition, have two windows drawn as focused.
 */
static void WindowTransferFocus(size_t window)
{
    const size_t previous = WindowFocused;

    if (previous == window)
    {
        return;
    }

    WindowFocused = window;

    if (WindowAt(previous) != NULL)
    {
        WindowEnqueueKind(previous, WINDOW_EVENT_FOCUS_OUT);
        WindowDamageAdd(WindowTitleBandOf(&WindowTable[previous]));
    }

    if (WindowAt(window) != NULL)
    {
        WindowEnqueueKind(window, WINDOW_EVENT_FOCUS_IN);
        WindowDamageAdd(WindowTitleBandOf(&WindowTable[window]));
    }
}

/* ------------------------------------------------------------- drawing */

static void WindowDrawTitle(const Window *window, GraphicsRectangle band, uint32_t ink,
                            uint32_t paper)
{
    const GraphicsRectangle reach = WindowCloseReachOf(window);
    int32_t x = band.x + WINDOW_TITLE_INSET;
    const int32_t y = band.y + ((WINDOW_TITLE_HEIGHT - (FONT_HEIGHT * WINDOW_TITLE_SCALE)) / 2);

    /*
     * The title is clipped to the band short of the close control, so that a
     * long title stops before it rather than running beneath it. A title that
     * ran beneath the control would leave a person unsure whether the disc was
     * a control or a letter.
     */
    if (!GraphicsPushClip(WindowScreen,
                          WindowMakeRectangle(band.x, band.y, reach.x - band.x, band.height)))
    {
        return;
    }

    for (const char *at = window->title; *at != '\0'; ++at)
    {
        FontDrawGlyphScaled(WindowScreen, x, y, (uint8_t)*at, ink, paper, WINDOW_TITLE_SCALE);
        x += FONT_WIDTH * WINDOW_TITLE_SCALE;
    }

    (void)GraphicsPopClip(WindowScreen);
}

static void WindowDrawFrame(size_t identifier)
{
    const Window *const window = &WindowTable[identifier];
    const GraphicsRectangle frame = WindowFrameOf(window);
    const GraphicsRectangle band = WindowTitleBandOf(window);
    const GraphicsRectangle content = WindowContentOf(window);
    const GraphicsRectangle reach = WindowCloseReachOf(window);
    const bool focused = (identifier == WindowFocused);
    const uint32_t band_colour = focused ? WindowColours.title_focused : WindowColours.title_unfocused;
    const uint32_t ink = focused ? WindowColours.text_focused : WindowColours.text_unfocused;

    /* A root and a panel are their content and nothing else — no band, no
     * border, no control — which is what makes a desktop look like a desktop
     * and not like a window somebody maximised. */
    if (!window->decorated)
    {
        (void)GraphicsBlit(WindowScreen, content.x, content.y, &window->surface,
                           GraphicsSurfaceBounds(&window->surface));

        return;
    }

    GraphicsFillRectangle(WindowScreen, band, band_colour);
    GraphicsDrawRectangle(WindowScreen, frame, WindowColours.border);
    WindowDrawTitle(window, band, ink, band_colour);
    GraphicsFillCircle(WindowScreen, reach.x + WINDOW_CLOSE_REACH, reach.y + WINDOW_CLOSE_REACH,
                       WINDOW_CLOSE_RADIUS, ink);
    (void)GraphicsBlit(WindowScreen, content.x, content.y, &window->surface,
                       GraphicsSurfaceBounds(&window->surface));
}

/* ---------------------------------------------------------- interface */

/* Gives every window back, so that a manager initialised twice, or given up,
 * leaks nothing. */
static void WindowReleaseAll(void)
{
    for (size_t index = 0U; index < WINDOW_CAPACITY; ++index)
    {
        if (WindowTable[index].in_use)
        {
            KernelFree(WindowTable[index].pixels);
            WindowTable[index].in_use = false;
            WindowTable[index].pixels = NULL;
        }
    }

    WindowStackCount = 0U;
    WindowFocused = WINDOW_NONE;
    WindowGrabbed = WINDOW_NONE;
    WindowDragged = WINDOW_NONE;
}

bool WindowManagerInitialise(GraphicsSurface *screen, const WindowPalette *palette,
                             WindowEncodeFunction encode)
{
    if ((screen == NULL) || (palette == NULL) || (screen->pixels == NULL))
    {
        return false;
    }

    WindowReleaseAll();

    WindowScreen = screen;
    WindowColours = *palette;
    WindowEncode = encode;
    WindowActive = true;
    WindowStackCount = 0U;
    WindowFocused = WINDOW_NONE;
    WindowGrabbed = WINDOW_NONE;
    WindowDragged = WINDOW_NONE;
    WindowPointerX = 0;
    WindowPointerY = 0;
    WindowDamaged = false;
    WindowRouted = 0U;
    WindowDiscarded = 0U;
    WindowCompositions = 0U;

    WindowDamageAdd(GraphicsSurfaceBounds(screen));

    return true;
}

void WindowManagerShutdown(void)
{
    if (!WindowActive)
    {
        return;
    }

    WindowReleaseAll();
    WindowScreen = NULL;
    WindowActive = false;
    WindowDamaged = false;
}

bool WindowManagerIsActive(void)
{
    return WindowActive;
}

size_t WindowCreate(int32_t x, int32_t y, int32_t width, int32_t height, const char *title,
                    WindowLayer layer)
{
    size_t identifier = WINDOW_NONE;
    Window *window;
    size_t row_bytes;

    if (!WindowActive || (title == NULL) || (width < WINDOW_MINIMUM_EXTENT) ||
        (height < WINDOW_MINIMUM_EXTENT) || (width > WINDOW_MAXIMUM_EXTENT) ||
        (height > WINDOW_MAXIMUM_EXTENT) || (layer > WINDOW_LAYER_PANEL))
    {
        return WINDOW_NONE;
    }

    for (size_t index = 0U; index < WINDOW_CAPACITY; ++index)
    {
        if (!WindowTable[index].in_use)
        {
            identifier = index;
            break;
        }
    }

    if (identifier == WINDOW_NONE)
    {
        return WINDOW_NONE;
    }

    window = &WindowTable[identifier];
    row_bytes = (size_t)width * WindowScreen->bytes_per_pixel;
    window->pixels = KernelAllocate(row_bytes * (size_t)height);

    if (window->pixels == NULL)
    {
        return WINDOW_NONE;
    }

    if (!GraphicsSurfaceInitialise(&window->surface, window->pixels, (uint32_t)width,
                                   (uint32_t)height, (uint32_t)row_bytes,
                                   WindowScreen->bytes_per_pixel))
    {
        KernelFree(window->pixels);
        window->pixels = NULL;
        return WINDOW_NONE;
    }

    window->in_use = true;
    window->layer = layer;
    window->decorated = (layer == WINDOW_LAYER_NORMAL);
    window->width = width;
    window->height = height;
    window->x = x;
    window->y = y;

    /*
     * A root and a panel are placed where the session puts them and are not
     * confined: the confinement keeps a title band reachable, and neither has
     * one. A root confined would be a root that could not cover the screen,
     * its own height being the screen's.
     */
    if (window->decorated)
    {
        WindowConfine(window, &window->x, &window->y);
    }

    {
        size_t length = 0U;

        while ((title[length] != '\0') && (length < WINDOW_TITLE_CAPACITY))
        {
            window->title[length] = title[length];
            ++length;
        }

        window->title[length] = '\0';
    }

    window->owner = 0U;
    window->head = 0U;
    window->count = 0U;
    window->dropped = 0U;

    GraphicsClear(&window->surface, WindowColours.paper);

    WindowStackInsert(identifier);
    WindowDamageAdd(WindowFrameOf(window));

    /*
     * A root never takes the focus. It is made first, before anything a person
     * would type at, and a desktop whose keys went to the wallpaper because it
     * was the last thing created is a desktop that ignores its first sentence.
     * A panel does take it, a launcher being a thing a person may type into.
     */
    if (layer != WINDOW_LAYER_ROOT)
    {
        WindowTransferFocus(identifier);
    }

    return identifier;
}

WindowLayer WindowLayerOf(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? WINDOW_LAYER_NORMAL : window->layer;
}

void WindowSetSession(uint64_t owner)
{
    WindowSessionOwner = owner;
}

uint64_t WindowSession(void)
{
    return WindowSessionOwner;
}

void WindowDestroy(size_t identifier)
{
    Window *const window = WindowAt(identifier);

    if (window == NULL)
    {
        return;
    }

    WindowDamageAdd(WindowFrameOf(window));
    WindowStackRemove(identifier);

    if (WindowGrabbed == identifier)
    {
        WindowGrabbed = WINDOW_NONE;
    }

    if (WindowDragged == identifier)
    {
        WindowDragged = WINDOW_NONE;
    }

    KernelFree(window->pixels);
    window->pixels = NULL;
    window->in_use = false;
    window->count = 0U;

    /*
     * The focus passes to the topmost window remaining. The destroyed window
     * is told nothing — it has no queue any more — and the gainer is told it
     * gained, so the transfer is made after the table entry is released and
     * not before, or the release would be sent to a queue about to vanish.
     */
    if (WindowFocused == identifier)
    {
        WindowFocused = WINDOW_NONE;

        if (WindowStackCount != 0U)
        {
            WindowTransferFocus(WindowTopmostFocusable());
        }
    }
}

bool WindowExists(size_t identifier)
{
    return WindowAt(identifier) != NULL;
}

void WindowSetOwner(size_t identifier, uint64_t owner)
{
    Window *const window = WindowAt(identifier);

    if (window != NULL)
    {
        window->owner = owner;
    }
}

uint64_t WindowOwner(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? 0U : window->owner;
}

size_t WindowDestroyOwnedBy(uint64_t owner)
{
    size_t destroyed = 0U;

    if (!WindowActive)
    {
        return 0U;
    }

    for (size_t index = 0U; index < WINDOW_CAPACITY; ++index)
    {
        if (WindowTable[index].in_use && (WindowTable[index].owner == owner))
        {
            WindowDestroy(index);
            ++destroyed;
        }
    }

    return destroyed;
}

void WindowRaise(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    if (window == NULL)
    {
        return;
    }

    /*
     * Already at the top of its own layer, where what stands above it belongs
     * to a higher one — which is what a raise means since sub-task 9.5. A
     * raise that went to the top of the whole stack would put an ordinary
     * window over the panel, and the first press upon any window would hide
     * the panel for good.
     */
    {
        const size_t position = WindowStackPositionOf(identifier);

        if ((position == WINDOW_NONE) || (position + 1U == WindowStackCount) ||
            (WindowTable[WindowStack[position + 1U]].layer > window->layer))
        {
            return;
        }
    }

    WindowStackRemove(identifier);
    WindowStackInsert(identifier);
    WindowDamageAdd(WindowFrameOf(window));
}

void WindowFocus(size_t identifier)
{
    if (WindowAt(identifier) == NULL)
    {
        return;
    }

    WindowTransferFocus(identifier);
}

void WindowMove(size_t identifier, int32_t x, int32_t y)
{
    Window *const window = WindowAt(identifier);

    if (window == NULL)
    {
        return;
    }

    WindowConfine(window, &x, &y);

    if ((x == window->x) && (y == window->y))
    {
        return;
    }

    /* Both places, for the reason the compositor's move marks both. */
    WindowDamageAdd(WindowFrameOf(window));
    window->x = x;
    window->y = y;
    WindowDamageAdd(WindowFrameOf(window));
}

GraphicsRectangle WindowFrame(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? WindowEmptyRectangle() : WindowFrameOf(window);
}

GraphicsRectangle WindowContentBounds(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? WindowEmptyRectangle() : WindowContentOf(window);
}

GraphicsSurface *WindowSurface(size_t identifier)
{
    Window *const window = WindowAt(identifier);

    return (window == NULL) ? NULL : &window->surface;
}

void WindowInvalidate(size_t identifier, GraphicsRectangle region)
{
    const Window *const window = WindowAt(identifier);

    if (window == NULL)
    {
        return;
    }

    {
        const GraphicsRectangle content = WindowContentOf(window);
        const GraphicsRectangle upon_screen =
            WindowMakeRectangle(content.x + region.x, content.y + region.y, region.width,
                                region.height);

        WindowDamageAdd(GraphicsRectangleIntersect(upon_screen, content));
    }
}

bool WindowWritePixels(size_t identifier, GraphicsRectangle area, const uint32_t *pixels)
{
    Window *const window = WindowAt(identifier);

    if ((window == NULL) || (pixels == NULL) || (area.width <= 0) || (area.height <= 0) ||
        (area.x < 0) || (area.y < 0) || (area.x > window->width - area.width) ||
        (area.y > window->height - area.height))
    {
        return false;
    }

    /*
     * Pixel by pixel, because each is encoded on the way in and an encoding
     * is per pixel. A client's rectangle is small — the demonstration's
     * largest is a window's whole content, a few hundred thousand pixels —
     * and a copy that converted rows at a time would be a second encoder to
     * keep in step with FramebufferEncode.
     */
    for (int32_t row = 0; row < area.height; ++row)
    {
        const uint32_t *const source = pixels + ((size_t)row * (size_t)area.width);

        for (int32_t column = 0; column < area.width; ++column)
        {
            const uint32_t value = source[column];
            const uint32_t encoded =
                (WindowEncode == NULL)
                    ? value
                    : WindowEncode((uint8_t)((value >> 16) & 0xFFU),
                                   (uint8_t)((value >> 8) & 0xFFU), (uint8_t)(value & 0xFFU));

            GraphicsPutPixel(&window->surface, area.x + column, area.y + row, encoded);
        }
    }

    WindowInvalidate(identifier, area);

    return true;
}

bool WindowDrawText(size_t identifier, int32_t x, int32_t y, const char *text, uint32_t ink,
                    uint32_t paper, int32_t scale)
{
    Window *const window = WindowAt(identifier);
    const uint32_t ink_pixel = (WindowEncode == NULL)
                                   ? ink
                                   : WindowEncode((uint8_t)((ink >> 16) & 0xFFU),
                                                  (uint8_t)((ink >> 8) & 0xFFU),
                                                  (uint8_t)(ink & 0xFFU));
    const uint32_t paper_pixel = (WindowEncode == NULL)
                                     ? paper
                                     : WindowEncode((uint8_t)((paper >> 16) & 0xFFU),
                                                    (uint8_t)((paper >> 8) & 0xFFU),
                                                    (uint8_t)(paper & 0xFFU));
    int32_t at = x;
    int32_t drawn = 0;

    if ((window == NULL) || (text == NULL) || (scale < 1) || (scale > 8))
    {
        return false;
    }

    /*
     * Every glyph is clipped by the content's own surface, so text that runs
     * past the right-hand edge is cut there rather than wrapping or writing
     * into the row beneath — which is what a caller drawing a label into a
     * window too narrow for it means, and is what it would see upon a screen.
     */
    for (const char *character = text; *character != '\0'; ++character)
    {
        FontDrawGlyphScaled(&window->surface, at, y, (uint8_t)*character, ink_pixel, paper_pixel,
                            scale);
        at += (int32_t)FONT_WIDTH * scale;
        ++drawn;
    }

    if (drawn != 0)
    {
        WindowInvalidate(identifier, WindowMakeRectangle(x, y, drawn * (int32_t)FONT_WIDTH * scale,
                                                         (int32_t)FONT_HEIGHT * scale));
    }

    return true;
}

const char *WindowTitle(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? "" : window->title;
}

bool WindowReadEvent(size_t identifier, WindowEvent *event)
{
    Window *const window = WindowAt(identifier);

    if ((window == NULL) || (event == NULL) || (window->count == 0U))
    {
        return false;
    }

    *event = window->events[window->head];
    window->head = (window->head + 1U) % WINDOW_EVENT_CAPACITY;
    --window->count;

    return true;
}

size_t WindowEventsQueued(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? 0U : window->count;
}

uint64_t WindowEventsDropped(size_t identifier)
{
    const Window *const window = WindowAt(identifier);

    return (window == NULL) ? 0U : window->dropped;
}

/* ------------------------------------------------------------- routing */

void WindowManagerHandleKey(const KeyEvent *key)
{
    WindowEvent event;

    if (!WindowActive || (key == NULL))
    {
        return;
    }

    if (WindowAt(WindowFocused) == NULL)
    {
        ++WindowDiscarded;
        return;
    }

    event.kind = WINDOW_EVENT_KEY;
    event.x = 0;
    event.y = 0;
    event.button = 0U;
    event.buttons = 0U;
    event.key = *key;

    WindowEnqueue(WindowFocused, &event);
}

/* The buttons the driver reports, lowest first, for a press or release to be
 * reported one button at a time. */
static uint8_t WindowLowestButton(uint8_t buttons)
{
    if ((buttons & MOUSE_BUTTON_LEFT) != 0U)
    {
        return MOUSE_BUTTON_LEFT;
    }

    if ((buttons & MOUSE_BUTTON_RIGHT) != 0U)
    {
        return MOUSE_BUTTON_RIGHT;
    }

    if ((buttons & MOUSE_BUTTON_MIDDLE) != 0U)
    {
        return MOUSE_BUTTON_MIDDLE;
    }

    return 0U;
}

/*
 * A press with nothing bound: what it lands upon decides what it begins.
 *
 *   The ground: nothing.
 *   A close control: the window is raised and focused and told to close.
 *   A title band: the window is raised and focused and a drag begins.
 *   A content: the window is raised and focused, told of the press, and
 *   bound to the pointer until every button is released.
 */
static void WindowBeginPress(int32_t x, int32_t y, uint8_t button, uint8_t buttons)
{
    const size_t identifier = WindowManagerWindowAt(x, y);
    const Window *window;

    if (identifier == WINDOW_NONE)
    {
        return;
    }

    window = &WindowTable[identifier];

    /*
     * A press upon the root raises nothing and focuses nothing: it is beneath
     * everything already, and taking the focus would mean a person who clicked
     * the wallpaper found their typing going nowhere. It still receives the
     * press, a session being entitled to know that its desktop was clicked.
     */
    if (window->layer != WINDOW_LAYER_ROOT)
    {
        WindowRaise(identifier);
        WindowTransferFocus(identifier);
    }

    if (GraphicsRectangleContains(WindowCloseReachOf(window), x, y))
    {
        WindowEnqueueKind(identifier, WINDOW_EVENT_CLOSE);
        return;
    }

    if (GraphicsRectangleContains(WindowTitleBandOf(window), x, y))
    {
        WindowDragged = identifier;
        WindowDragOffsetX = x - window->x;
        WindowDragOffsetY = y - window->y;
        return;
    }

    WindowGrabbed = identifier;
    WindowEnqueuePointer(identifier, WINDOW_EVENT_BUTTON_PRESS, x, y, button, buttons);
}

void WindowManagerHandleMouse(const MouseEvent *movement)
{
    uint8_t pressed;
    uint8_t released;

    if (!WindowActive || (movement == NULL))
    {
        return;
    }

    pressed = movement->changed & movement->buttons;
    released = movement->changed & (uint8_t)~movement->buttons;

    WindowPointerX = movement->x;
    WindowPointerY = movement->y;

    /*
     * A drag by the title band takes the movement itself and reports it to
     * nobody: the window follows the pointer, and the owner learns where it
     * now stands by asking, if it cares, which it seldom does.
     */
    if (WindowDragged != WINDOW_NONE)
    {
        WindowMove(WindowDragged, movement->x - WindowDragOffsetX,
                   movement->y - WindowDragOffsetY);

        if (movement->buttons == 0U)
        {
            WindowDragged = WINDOW_NONE;
        }

        return;
    }

    /*
     * A window bound by a held button receives everything until the last
     * button is released, the release included, wherever the pointer has gone.
     * A further press while bound goes to the same window, as it would upon a
     * pointer that had not left it.
     */
    if (WindowGrabbed != WINDOW_NONE)
    {
        const size_t bound = WindowGrabbed;

        if ((movement->delta_x != 0) || (movement->delta_y != 0))
        {
            WindowEnqueuePointer(bound, WINDOW_EVENT_POINTER_MOVE, movement->x, movement->y,
                                 0U, movement->buttons);
        }

        if (pressed != 0U)
        {
            WindowEnqueuePointer(bound, WINDOW_EVENT_BUTTON_PRESS, movement->x, movement->y,
                                 WindowLowestButton(pressed), movement->buttons);
        }

        if (released != 0U)
        {
            WindowEnqueuePointer(bound, WINDOW_EVENT_BUTTON_RELEASE, movement->x, movement->y,
                                 WindowLowestButton(released), movement->buttons);
        }

        if (movement->buttons == 0U)
        {
            WindowGrabbed = WINDOW_NONE;
        }

        return;
    }

    if (pressed != 0U)
    {
        WindowBeginPress(movement->x, movement->y, WindowLowestButton(pressed),
                         movement->buttons);
        return;
    }

    /*
     * A release with nothing bound is a button that was pressed before the
     * manager existed, or upon the ground; there is nobody it belongs to.
     */
    if (released != 0U)
    {
        return;
    }

    if ((movement->delta_x != 0) || (movement->delta_y != 0))
    {
        const size_t under = WindowManagerWindowAt(movement->x, movement->y);

        if (under != WINDOW_NONE)
        {
            WindowEnqueuePointer(under, WINDOW_EVENT_POINTER_MOVE, movement->x, movement->y,
                                 0U, movement->buttons);
        }
    }
}

/* --------------------------------------------------------- composition */

GraphicsRectangle WindowManagerCompose(void)
{
    GraphicsRectangle composed;

    if (!WindowActive || !WindowDamaged)
    {
        return WindowEmptyRectangle();
    }

    composed = WindowDamage;

    if (!GraphicsPushClip(WindowScreen, composed))
    {
        return WindowEmptyRectangle();
    }

    /*
     * The ground first and every intersecting window over it, bottom to top:
     * the painter's order, in which whatever is drawn last is on top and
     * nothing needs to know what it covers. A window that does not touch the
     * region is not drawn, and one that does is drawn whole and clipped, the
     * primitives' clip being cheaper than any arithmetic here to avoid it.
     */
    GraphicsClear(WindowScreen, WindowColours.ground);

    for (size_t position = 0U; position < WindowStackCount; ++position)
    {
        const size_t identifier = WindowStack[position];
        const GraphicsRectangle frame = WindowFrameOf(&WindowTable[identifier]);

        if (!GraphicsRectangleIsEmpty(GraphicsRectangleIntersect(frame, composed)))
        {
            WindowDrawFrame(identifier);
        }
    }

    (void)GraphicsPopClip(WindowScreen);

    WindowDamaged = false;
    ++WindowCompositions;

    return composed;
}

GraphicsRectangle WindowManagerDamage(void)
{
    return WindowDamaged ? WindowDamage : WindowEmptyRectangle();
}

GraphicsRectangle WindowManagerScreenBounds(void)
{
    return WindowActive ? GraphicsSurfaceBounds(WindowScreen) : WindowEmptyRectangle();
}

size_t WindowManagerWindowAt(int32_t x, int32_t y)
{
    if (!WindowActive)
    {
        return WINDOW_NONE;
    }

    /* From the top down: the first frame containing the point is the one a
     * person sees there. */
    for (size_t position = WindowStackCount; position != 0U; --position)
    {
        const size_t identifier = WindowStack[position - 1U];

        if (GraphicsRectangleContains(WindowFrameOf(&WindowTable[identifier]), x, y))
        {
            return identifier;
        }
    }

    return WINDOW_NONE;
}

size_t WindowManagerFocused(void)
{
    return (WindowAt(WindowFocused) == NULL) ? WINDOW_NONE : WindowFocused;
}

size_t WindowManagerGrabbed(void)
{
    return WindowGrabbed;
}

size_t WindowManagerStackAt(size_t position)
{
    return (position < WindowStackCount) ? WindowStack[position] : WINDOW_NONE;
}

size_t WindowManagerCount(void)
{
    return WindowStackCount;
}

uint64_t WindowManagerEventsRouted(void)
{
    return WindowRouted;
}

uint64_t WindowManagerEventsDiscarded(void)
{
    return WindowDiscarded;
}

uint64_t WindowManagerCompositionCount(void)
{
    return WindowCompositions;
}

void WindowManagerReport(void)
{
    if (!WindowActive)
    {
        KernelWriteString("Window manager: no screen taken.\n");
        return;
    }

    KernelWriteString("Window manager: screen ");
    KernelWriteDecimal(WindowScreen->width);
    KernelWriteString("x");
    KernelWriteDecimal(WindowScreen->height);
    KernelWriteString(", ");
    KernelWriteDecimal(WindowStackCount);
    KernelWriteString(" window(s), focus ");

    if (WindowManagerFocused() == WINDOW_NONE)
    {
        KernelWriteString("none");
    }
    else
    {
        KernelWriteDecimal(WindowFocused);
    }

    KernelWriteString("; ");
    KernelWriteDecimal(WindowRouted);
    KernelWriteString(" event(s) routed, ");
    KernelWriteDecimal(WindowDiscarded);
    KernelWriteString(" discarded, ");
    KernelWriteDecimal(WindowCompositions);
    KernelWriteString(" composition(s).\n");

    for (size_t position = 0U; position < WindowStackCount; ++position)
    {
        const Window *const window = &WindowTable[WindowStack[position]];

        KernelWriteString("  ");
        KernelWriteDecimal(position);
        KernelWriteString(": window ");
        KernelWriteDecimal(WindowStack[position]);
        KernelWriteString(" \"");
        KernelWriteString(window->title);
        KernelWriteString("\" at ");
        KernelWriteDecimal((uint64_t)(uint32_t)window->x);
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)(uint32_t)window->y);
        KernelWriteString(", content ");
        KernelWriteDecimal((uint64_t)window->width);
        KernelWriteString("x");
        KernelWriteDecimal((uint64_t)window->height);
        KernelWriteString(", ");
        KernelWriteDecimal(window->count);
        KernelWriteString(" queued, ");
        KernelWriteDecimal(window->dropped);
        KernelWriteString(" dropped.\n");
    }
}
