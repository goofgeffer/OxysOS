/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/gfx/window.h
 * Purpose: Declares the window manager of sub-task 9.1: a table of windows upon
 *          one screen surface, the order they stack in, the one of them that
 *          holds the keyboard, and the routing of every key and every movement
 *          of the mouse to the window it belongs to.
 * Key definitions: WINDOW_CAPACITY, WINDOW_NONE, WindowEvent, WindowEventKind,
 *          WindowLayer, WindowManagerInitialise, WindowCreate, WindowDestroy,
 *          WindowRaise, WindowSetSession, WindowSession, WindowDrawText,
 *          WindowFocus, WindowMove, WindowSurface, WindowInvalidate,
 *          WindowReadEvent, WindowManagerHandleKey, WindowManagerHandleMouse,
 *          WindowManagerCompose, WindowManagerWindowAt, WindowManagerReport,
 *          WindowSetOwner, WindowDestroyOwnedBy, WindowWritePixels,
 *          WindowManagerShutdown, WindowEncodeFunction, WindowMinimise,
 *          WindowRestore, WindowSetFull, WindowIsMinimised, WindowIsFull,
 *          WindowManagerWorkArea.
 * References:
 *   - docs/design/WINDOWS.md: the design, the appearance it commits to, and
 *     every assertion made upon it.
 *   - X Window System Protocol, X Version 11 Release 7.7, Chapter 11, "Input
 *     Device events", and the Glossary entries "Input focus" and "Stacking
 *     order": the three notions this manager has — that keyboard input has one
 *     scope, that sibling windows obscure one another in an order, and that a
 *     button pressed in a window binds the pointer to that window until every
 *     button is released — are the ones that document defines, and they are
 *     taken as notions and not as an interface. No request, event or type of
 *     that protocol is adopted; docs/project/INSPIRATIONS.md, Section 5.
 *   - docs/project/INSPIRATIONS.md, Section 3: the appearance the frame is
 *     designed against, and the idiom it is required not to have.
 *
 * Why the manager is in the kernel, and what that decides.
 *
 *   Sub-task 9.2 puts the programs that own windows on the far side of a
 *   protocol; this sub-task has no such program, so the windows here are owned
 *   by whatever kernel code created them, and the demonstration of
 *   kernel/test/gfx/windows.c is the only owner. What is decided now, and will
 *   not change when a client arrives, is the shape of the thing on this side of
 *   the protocol: a window is a rectangle of pixels the owner draws into and a
 *   queue of events the owner drains, and the manager's whole business is to
 *   compose the rectangles in their order and to put each event into the right
 *   queue. A protocol carries those two things across a boundary; it does not
 *   change what they are.
 *
 * What a window is.
 *
 *   A content surface, owned by the manager and drawn into by the owner, and a
 *   frame the manager draws around it: a title band above and a one-pixel border
 *   about the whole. The owner names the content's size and the frame's position
 *   and never draws the frame — a frame the owner drew would be a frame every
 *   owner drew differently, and the one thing a person recognises a window by
 *   is that they all have the same one. The frame is flat, by
 *   docs/project/INSPIRATIONS.md, Section 3, and the reason that section gives.
 *
 * Where an event goes.
 *
 *   A key goes to the window that holds the focus, and to no other. The focus is
 *   given to a window when it is created and when a button is pressed within it,
 *   and passes to the topmost window remaining when its holder is destroyed.
 *
 *   A movement of the pointer goes to the window under the pointer — the topmost
 *   whose frame contains it — with the position translated so that the owner
 *   reads it against the content's own origin. A button pressed within a
 *   window's content raises that window, gives it the focus, and binds the
 *   pointer to it: until every button is released, every movement and every
 *   release goes to that window wherever the pointer has gone since. Without
 *   that, a drag begun in one window and carried over another would end in the
 *   other, and the first would never learn that the button it saw pressed was
 *   released.
 *
 *   A button pressed in a window's title band is the manager's and not the
 *   owner's: it raises and focuses the window and begins a drag that moves it,
 *   or — upon the close control — puts a close event into the window's queue;
 *   or, since 2026-09-23, upon the full-screen or the minimise control beside
 *   it, makes the window full or hides it, which lose nothing and so are the
 *   manager's to do rather than the owner's to be asked.
 *   The manager destroys nothing upon a close. What a close means is the
 *   owner's decision, an editor with an unsaved file being the standing example
 *   of a window that must be asked and not removed.
 *
 * Nothing here presents. The manager composes into the surface it was given and
 * says which region it changed; carrying that region to the display is the
 * caller's, so that the same code composes into a surface in memory for the
 * self-test as into the compositor's back buffer for the screen.
 */

#ifndef OXYS_GFX_WINDOW_H
#define OXYS_GFX_WINDOW_H

#include <oxys/types.h>
#include <oxys/gfx/graphics.h>
#include <oxys/dev/keyboard.h>
#include <oxys/dev/mouse.h>

/*
 * How many windows may exist at once.
 *
 * Sixteen, and a fixed table with a refusal beyond it, for the reason the
 * compositor's layer table is fixed: the manager must be honest about a bound
 * rather than grow until an allocation fails somewhere nothing expects it to.
 * The desktop of sub-task 9.5 and the utilities of 9.7 are a handful of windows;
 * a program that opens sixteen has a defect this bound turns into a refusal.
 */
#define WINDOW_CAPACITY 16U

/* The value WindowCreate returns when it cannot make a window, and the value
 * WindowManagerWindowAt and WindowManagerFocused return when there is none. */
#define WINDOW_NONE ((size_t)-1)

/* The longest title kept, in characters, the terminator not counted. A longer
 * title is cut and not refused: a title is a label, not an identity. */
#define WINDOW_TITLE_CAPACITY 31U

/* How many events a window's queue holds before the newest is dropped. */
#define WINDOW_EVENT_CAPACITY 32U

/*
 * The frame's geometry, in pixels. The title band holds the face of sub-task
 * 6.4 drawn at twice its size — sixteen pixels — with four above and four below,
 * and the border is one pixel, depth being expressed by stacking and not by
 * thickness.
 */
#define WINDOW_TITLE_HEIGHT 24
#define WINDOW_BORDER       1

/*
 * The three layers a window may stand in, of sub-task 9.5, ordered from the
 * bottom of the screen upward. A window stacks among its own layer and never
 * outside it: raising the topmost ordinary window does not put it over the
 * panel, and nothing a program does puts it under the root.
 *
 * Without the layers a panel is an ordinary window that the next press
 * anywhere puts behind something, and a root is an ordinary window that the
 * next raise buries the desktop beneath. Both are things a person would call
 * broken and neither can be expressed by an order alone.
 *
 * **The root and the panel carry no frame.** A root is the whole screen and a
 * panel is a bar; a title band and a close control upon either would be a
 * control for closing the desktop. The layer decides it, rather than a second
 * argument, because a decorated root and an undecorated ordinary window are
 * both things nobody has asked for and neither should be reachable.
 */
typedef enum WindowLayer
{
    WINDOW_LAYER_ROOT = 0,   /* Beneath everything: the desktop itself. */
    WINDOW_LAYER_NORMAL = 1, /* The programs' windows. */
    WINDOW_LAYER_PANEL = 2   /* Above everything: the panel and what it opens. */
} WindowLayer;

/* The smallest content a window may have, and the largest. The bound above is
 * the coordinate limit of the primitives; the one below is a window a person
 * can still find. */
#define WINDOW_MINIMUM_EXTENT 16
#define WINDOW_MAXIMUM_EXTENT 4096

/*
 * The width of a frame that must remain upon the screen when a window is moved,
 * so that the title band — and with it the means of moving it back — can always
 * be reached. A window dragged wholly off the screen is a window nobody can
 * recover without knowing its name.
 */
#define WINDOW_REACHABLE_WIDTH 48

/* What an event is. */
typedef enum WindowEventKind
{
    /* A key was pressed or released while this window held the focus. */
    WINDOW_EVENT_KEY = 1,

    /* The pointer moved over this window, or anywhere while a button pressed
     * within it was still held. */
    WINDOW_EVENT_POINTER_MOVE,

    /* A button was pressed within the content, or released while this window
     * held the pointer. */
    WINDOW_EVENT_BUTTON_PRESS,
    WINDOW_EVENT_BUTTON_RELEASE,

    /* This window gained or lost the focus. */
    WINDOW_EVENT_FOCUS_IN,
    WINDOW_EVENT_FOCUS_OUT,

    /* The close control was pressed. The owner decides what that means. */
    WINDOW_EVENT_CLOSE,

    /* The content was given a new extent — by full screen, or by leaving it —
     * and the owner must draw it again: `x` and `y` carry the new width and
     * height. What stood in the content was kept where it fitted, and the rest
     * is the paper colour; neither is what the owner would have drawn. */
    WINDOW_EVENT_RESIZE,

    /* Sent to a root: the set of ordinary windows, their states or the focus
     * among them changed, and a session listing them should look again. */
    WINDOW_EVENT_WINDOWS
} WindowEventKind;

/*
 * One event, as the owner reads it.
 *
 * `x` and `y` are the pointer's position relative to the content's top left,
 * for the three pointer events; they may lie outside the content, and below
 * zero, while the pointer is bound to this window by a held button. `button`
 * names the one button a press or a release concerns and `buttons` the set held
 * afterwards, as the mouse driver's own event distinguishes them. `key` is the
 * keyboard driver's event entire, for a key event, so that an owner wanting a
 * release or a key that produces no character has it.
 */
typedef struct WindowEvent
{
    WindowEventKind kind;
    int32_t x;
    int32_t y;
    uint8_t button;
    uint8_t buttons;
    KeyEvent key;
} WindowEvent;

/*
 * The colours the manager draws with, supplied rather than named because a
 * pixel value means nothing without an encoding; for the display that is
 * FramebufferEncode, and for the self-test's surface it is whatever the test
 * says. docs/design/WINDOWS.md, Section 4, records the palette the entry point
 * supplies and why each colour is what it is.
 */
/*
 * How a client's pixel — 0x00RRGGBB, since sub-task 9.2 — becomes the screen's.
 * Supplied to WindowManagerInitialise beside the palette, for the reason the
 * palette is supplied: the manager draws in pixel values and does not know the
 * encoding. Null means the screen's pixel is the client's, unchanged, which is
 * what the self-test's surface wants and what lets it assert exact values.
 */
typedef uint32_t (*WindowEncodeFunction)(uint8_t red, uint8_t green, uint8_t blue);

typedef struct WindowPalette
{
    uint32_t ground;          /* The screen where no window stands. */
    uint32_t paper;           /* A window's content, as created. */
    uint32_t border;          /* The one-pixel border about a frame. */
    uint32_t title_focused;   /* The title band of the window holding the focus. */
    uint32_t title_unfocused; /* The title band of every other window. */
    uint32_t text_focused;    /* The title and the close control upon the first. */
    uint32_t text_unfocused;  /* The same upon the second. */
} WindowPalette;

/*
 * Takes a screen: every window is destroyed, the table emptied, and the whole
 * of the surface marked as changed, so that the first composition paints the
 * ground. Returns false where the surface is null or has no pixels.
 *
 * It may be called again with another surface, which is how the self-test
 * conducts the manager upon a surface in memory before the entry point gives it
 * the compositor's back buffer.
 */
bool WindowManagerInitialise(GraphicsSurface *screen, const WindowPalette *palette,
                             WindowEncodeFunction encode);

/*
 * Gives the screen up: every window is destroyed and the manager is inactive
 * until initialised again. The self-test calls it so that the manager does not
 * stand holding the test's surface — where a client's window, made through the
 * calls of sub-task 9.2 upon an entry that gave the shell the screen, would be
 * drawn into memory nobody displays and refused by nothing.
 */
void WindowManagerShutdown(void);

/* Whether a screen has been taken. Every routine below does nothing, or
 * returns WINDOW_NONE, until one has. */
bool WindowManagerIsActive(void);

/*
 * Makes a window whose frame's top left is at (x, y) upon the screen and whose
 * content is `width` by `height`, titled as given, filled with the paper
 * colour, placed upon the top of its layer, and given the focus — save a root,
 * which never takes it.
 *
 * Returns WINDOW_NONE where the table is full, the extent is outside the bounds
 * above, or the heap cannot supply the content. An ordinary window is not
 * refused for lying partly off the screen — the frame is confined as WindowMove
 * confines a moved one — and a root or a panel is not confined at all, having
 * no band to keep reachable and being placed by the one program that knows
 * where they belong.
 */
size_t WindowCreate(int32_t x, int32_t y, int32_t width, int32_t height, const char *title,
                    WindowLayer layer);

/* The layer a window stands in. WINDOW_LAYER_NORMAL for one that does not
 * exist, that being the layer a caller asking about nothing means least by. */
WindowLayer WindowLayerOf(size_t window);

/*
 * The session, of sub-task 9.5: the one owner that may make a root or a panel.
 *
 * The manager keeps a number and attaches no meaning to it, as it does for a
 * window's owner; graphics/client.c sets it to the process that claimed the
 * session and refuses the two layers to every other. Zero is nobody, which is
 * what it is before a session has claimed it and after the claimant has ended.
 */
void WindowSetSession(uint64_t owner);
uint64_t WindowSession(void);

/*
 * Draws text into a window's content with the system face, of sub-task 9.5:
 * `text` at (x, y) in the content's coordinates, each glyph enlarged by
 * `scale`, in the two colours given — which are a client's 0x00RRGGBB and are
 * encoded here as a blitted pixel is. The region drawn is marked as changed.
 *
 * It exists because the face is a system resource and there is exactly one of
 * it: the window manager already draws every title with it, and a system in
 * which each program carries its own face is a system whose text does not match
 * itself. The face is also the kernel's under the kernel's licence and `libc/`
 * is under another, so a copy in the library would be a relicensing this
 * project may not perform; docs/design/SESSION.md, Section 4.
 *
 * Returns false where the window does not exist or the text is unreadable.
 * Characters the face does not cover are drawn as nothing rather than refused,
 * a label with one odd character in it being a label a person can still read.
 */
bool WindowDrawText(size_t window, int32_t x, int32_t y, const char *text, uint32_t ink,
                    uint32_t paper, int32_t scale);

/*
 * Destroys a window, giving its content back to the heap, marking where it
 * stood as changed, and passing the focus to the topmost window remaining if it
 * held it. A window bound to the pointer by a held button is unbound.
 */
void WindowDestroy(size_t window);

/* Whether the identifier names a window that exists. */
bool WindowExists(size_t window);

/*
 * The owner, since sub-task 9.2: a tag the client layer sets to the process's
 * identifier, zero being the kernel's own. The manager attaches no meaning to
 * it beyond WindowDestroyOwnedBy, which destroys every window carrying the tag
 * and returns how many — what a process's ending calls, so that a program
 * which ended without destroying its windows leaves none standing upon the
 * screen with nobody to drain their queues.
 */
void WindowSetOwner(size_t window, uint64_t owner);
uint64_t WindowOwner(size_t window);
size_t WindowDestroyOwnedBy(uint64_t owner);

/* Places the window upon the top of the stack. */
void WindowRaise(size_t window);

/*
 * Gives the window the focus, telling the previous holder it has lost it and
 * this one that it has gained it, in that order, and marking both title bands
 * as changed. Giving the focus to the window that holds it does nothing.
 */
void WindowFocus(size_t window);

/*
 * Moves the frame's top left to (x, y), confined so that WINDOW_REACHABLE_WIDTH
 * pixels of the title band remain upon the screen and the band's top is never
 * above it. Both where the window was and where it now is are marked changed.
 */
void WindowMove(size_t window, int32_t x, int32_t y);

/*
 * Minimise and full screen, of 2026-09-23. Each is refused, returning false,
 * for a window that does not exist or carries no frame — a root or a panel
 * minimised would be a desktop that vanished with nothing to bring it back.
 *
 * WindowMinimise hides the window: it is not composed, not hit, and gives up
 * the focus and the pointer, but keeps its content, its queue and its owner.
 * WindowRestore shows it again if it was hidden, and raises and focuses it
 * whether it was or not, which is what choosing a window from a list means.
 *
 * WindowSetFull gives the window the work area — the screen less the panel
 * across its top — keeping the frame's title band, so that the control that
 * made it full is still there to undo it; and gives it back the position and
 * extent it had before when undone. The content is a new surface of the new
 * extent, what stood in the old one is kept where it fits, and the owner is
 * sent WINDOW_EVENT_RESIZE to draw it again. False, with nothing changed,
 * where the heap cannot supply the larger content.
 *
 * Every one of the three tells the roots, WINDOW_EVENT_WINDOWS.
 */
bool WindowMinimise(size_t window);
bool WindowRestore(size_t window);
bool WindowSetFull(size_t window, bool full);
bool WindowIsMinimised(size_t window);
bool WindowIsFull(size_t window);

/* The screen less any panel standing across the whole width of its top: what
 * a full window is given. */
GraphicsRectangle WindowManagerWorkArea(void);

/* The frame's rectangle upon the screen, and the content's. */
GraphicsRectangle WindowFrame(size_t window);
GraphicsRectangle WindowContentBounds(size_t window);

/* The content surface the owner draws into, or null. Drawing into it changes
 * nothing upon the screen until the region is invalidated and composed. */
GraphicsSurface *WindowSurface(size_t window);

/* Records that a region of the content, in the content's coordinates, has
 * changed and must be composed again. */
void WindowInvalidate(size_t window, GraphicsRectangle region);

/*
 * Writes a rectangle of pixels into the content, since sub-task 9.2: `area` in
 * the content's coordinates, `pixels` row by row and tightly packed, each
 * 0x00RRGGBB, encoded for the screen on the way in, and the area marked as
 * changed. Refused, with nothing written, where the area is empty or reaches
 * outside the content — a client's rectangle is not clipped, because a client
 * that asked for more than its window has made a mistake it should hear of.
 */
bool WindowWritePixels(size_t window, GraphicsRectangle area, const uint32_t *pixels);

/* The title, as kept. */
const char *WindowTitle(size_t window);

/*
 * Removes the oldest event from the window's queue. Returns false, leaving the
 * argument untouched, where the queue is empty or the window does not exist.
 */
bool WindowReadEvent(size_t window, WindowEvent *event);

/* How many events the window's queue holds, and how many it has dropped. */
size_t WindowEventsQueued(size_t window);
uint64_t WindowEventsDropped(size_t window);

/*
 * Routes a key: into the queue of the window holding the focus, or nowhere,
 * counted, where no window does.
 */
void WindowManagerHandleKey(const KeyEvent *event);

/*
 * Routes a movement of the mouse, with whatever buttons it changed, by the
 * rules the header of this file sets out: to the window bound by a held button,
 * else to the window under the pointer; a press in a title band to the manager
 * itself. A movement over the ground goes nowhere and is not counted as
 * dropped, the ground being nobody's.
 */
void WindowManagerHandleMouse(const MouseEvent *event);

/*
 * Composes every window that intersects the changed region into the screen, in
 * stacking order over the ground, and returns that region — empty where nothing
 * had changed — so that the caller may carry it to the display. The changed
 * region is empty afterwards.
 */
GraphicsRectangle WindowManagerCompose(void);

/* The region presently awaiting composition, which the self-test asserts about. */
GraphicsRectangle WindowManagerDamage(void);

/* The screen's bounds, or an empty rectangle where no screen has been taken. */
GraphicsRectangle WindowManagerScreenBounds(void);

/* The topmost window whose frame contains the point, or WINDOW_NONE. */
size_t WindowManagerWindowAt(int32_t x, int32_t y);

/* The window holding the focus, or WINDOW_NONE. */
size_t WindowManagerFocused(void);

/* The window bound to the pointer by a held button, or WINDOW_NONE. */
size_t WindowManagerGrabbed(void);

/* The window at a position in the stack, counted from the bottom, or
 * WINDOW_NONE beyond the top; and how many windows there are. */
size_t WindowManagerStackAt(size_t position);
size_t WindowManagerCount(void);

/* Accounting, for the report and for the self-test. */
uint64_t WindowManagerEventsRouted(void);
uint64_t WindowManagerEventsDiscarded(void);
uint64_t WindowManagerCompositionCount(void);

/* Emits the screen's geometry, the stack, the focus and the accounting. */
void WindowManagerReport(void);

#endif /* OXYS_GFX_WINDOW_H */
