/*
 * File: kernel/include/oxys/cursor.h
 * Purpose: Declares the pointer drawn upon a surface for the mouse of sub-task
 *          6.5: its shape, the pixels it stands upon and the restoration of
 *          them, and the concealment that lets anything else draw while it is
 *          shown.
 * Key definitions: CURSOR_WIDTH, CURSOR_HEIGHT, CursorInitialise,
 *          CursorIsAvailable, CursorShow, CursorHide, CursorIsVisible,
 *          CursorMoveTo, CursorConceal, CursorReveal, CursorX, CursorY,
 *          CursorShapeIsOpaque, CursorShapeIsInterior, CursorDrawCount,
 *          CursorRestoreCount, CursorReport.
 * References:
 *   - docs/design/GRAPHICS.md, Section 26: what the pointer is, seen from the
 *     drawing it is built upon.
 *   - docs/devices/MOUSE.md, Sections 7 and 8: the shape, the save-under, and
 *     every assertion made upon them.
 *   - docs/devices/MOUSE.md, Section 1: the division between the device and its
 *     picture, and why it falls where it does.
 *
 * Why a pointer needs anything more than a bitmap.
 *
 * A pointer is not drawn once. It is drawn, and then it moves, and what was
 * beneath it must reappear — and until sub-task 6.6 there is one surface and no
 * back buffer to redraw the display from. The pixels beneath the pointer are
 * therefore read before it is drawn and written back before it is drawn
 * somewhere else. That store is the substance of this file; the shape is a
 * table.
 *
 * The store is only correct while nothing else draws. If the console prints a
 * line while the pointer is shown, one of two things happens: the console writes
 * over the pointer, and the pointer's next move restores stale pixels over the
 * text; or the pointer is moved first, restoring pixels the console has since
 * legitimately overwritten. Both leave debris on the screen that no assertion
 * would catch, because every pixel involved holds a value something meant to
 * write.
 *
 * CursorConceal and CursorReveal are the answer, and they are a pair rather than
 * a flag because the situations nest: KernelWriteString brackets its console
 * write with them, and a panic raised from within one would otherwise reveal the
 * pointer in the middle of the write that had concealed it. The count is what
 * makes the innermost reveal do nothing and the outermost do the work.
 *
 * From sub-task 6.6 this disappears. With a back buffer the pointer is composited
 * at the end of a frame and no pixels need saving, and this file's save-under
 * becomes the thing that made a pointer possible one sub-task early rather than
 * a mechanism the kernel keeps.
 */

#ifndef OXYS_CURSOR_H
#define OXYS_CURSOR_H

#include <oxys/types.h>
#include <oxys/graphics.h>

/*
 * The pointer's extent, in pixels.
 *
 * Twelve by eighteen is the shape drawn in graphics/cursor.c and is fixed by
 * that table rather than chosen here; the constants are declared so that the
 * save-under store, which is the caller-visible cost of the arrangement, has a
 * size a reader can compute.
 */
#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 18

/*
 * Adopts the surface as the one the pointer is drawn upon, in the two colours
 * given, and places it at the origin. The pointer begins hidden.
 *
 * The colours are supplied rather than named because a pixel value means nothing
 * without the surface's encoding, which belongs to whoever supplied the surface;
 * for the framebuffer that is FramebufferEncode.
 *
 * Returns false where the surface is unusable. Every routine below then does
 * nothing, which is what a machine with no display requires.
 */
bool CursorInitialise(GraphicsSurface *surface, uint32_t outline, uint32_t interior);

/* Whether a usable surface was adopted. */
bool CursorIsAvailable(void);

/*
 * Shows and hides the pointer.
 *
 * Showing draws it and records the pixels beneath; hiding restores them. Both
 * are idempotent: showing a pointer already shown draws nothing further, which
 * matters because a second save would record the pointer's own pixels as though
 * they were the display beneath it, and the display would never come back.
 */
void CursorShow(void);
void CursorHide(void);

/* Whether the pointer is shown. This is what the operator sees, and is not
 * affected by a concealment, which is temporary and reverses itself. */
bool CursorIsVisible(void);

/*
 * Moves the pointer, restoring what was beneath it and saving what is beneath it
 * now. Does nothing visible while the pointer is hidden or concealed, the
 * position being recorded either way.
 *
 * The position is the hot spot — the pixel the pointer points at, which is the
 * tip at its top left — and not the top left of the bitmap by coincidence: the
 * two are the same point for this shape, and a shape whose tip lay elsewhere
 * would subtract the difference here.
 */
void CursorMoveTo(int32_t x, int32_t y);

/*
 * Takes the pointer off the surface for the duration of somebody else's drawing,
 * and puts it back.
 *
 * These nest, and must be paired. See the file header: the save-under is only
 * correct while nothing else draws, and this is how everything else says that it
 * is about to.
 */
void CursorConceal(void);
void CursorReveal(void);

/* The pointer's position. */
int32_t CursorX(void);
int32_t CursorY(void);

/*
 * The shape, one pixel at a time, for the self-test.
 *
 * A pixel is opaque where the pointer covers the display and interior where it
 * is drawn in the interior colour rather than the outline colour. Every interior
 * pixel is opaque; a pixel that were interior and not opaque would be a hole in
 * the shape drawn in the wrong colour, and the self-test asserts that there is
 * none.
 */
bool CursorShapeIsOpaque(int32_t column, int32_t row);
bool CursorShapeIsInterior(int32_t column, int32_t row);

/* The number of times the pointer has been drawn, and the number of times what
 * was beneath it has been put back. They differ by at most one — the once, while
 * it is shown, that it stands upon the surface. */
uint64_t CursorDrawCount(void);
uint64_t CursorRestoreCount(void);

/* Emits a summary of the pointer's state upon both output devices. */
void CursorReport(void);

#endif /* OXYS_CURSOR_H */
