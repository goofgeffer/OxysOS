/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/gfx/cursor.h
 * Purpose: Declares the pointer: its shape, the rendered surface and coverage
 *          mask the compositor draws it from, and the position it reflects from
 *          the mouse driver.
 * Key definitions: CURSOR_WIDTH, CURSOR_HEIGHT, CursorInitialise,
 *          CursorIsAvailable, CursorShow, CursorHide, CursorIsVisible,
 *          CursorMoveTo, CursorX, CursorY, CursorShapeIsOpaque,
 *          CursorShapeIsInterior, CursorImageSurface, CursorImageMask,
 *          CursorMoveCount, CursorReport.
 * References:
 *   - docs/design/COMPOSITOR.md: what the pointer is, seen from the
 *     drawing it is built upon; Section 27.4, what sub-task 6.6 removed from it.
 *   - docs/devices/MOUSE.md: the shape and every assertion
 *     made upon it.
 *   - docs/devices/MOUSE.md: the division between the device and its
 *     picture, and why it falls where it does.
 *
 * Why a pointer needs anything more than a bitmap, and why it no longer does.
 *
 * A pointer is not drawn once: it is drawn, and then it moves, and what was
 * beneath it must reappear. Until sub-task 6.6 there was one surface and no back
 * buffer to redraw from, so the pixels beneath the pointer were read before it
 * was drawn and written back before it was drawn elsewhere. That store was the
 * substance of this file and the shape was a table.
 *
 * The store was correct only while nothing else drew. If the console printed a
 * line while the pointer was shown, one of two things happened: the console
 * wrote over the pointer, and the pointer's next move restored stale pixels over
 * the text; or the pointer moved first, restoring pixels the console had since
 * legitimately overwritten. Both left debris no assertion would catch, every
 * pixel involved holding a value something meant to write. A nested concealment
 * was the answer, and it obliged KernelWriteString to know that a pointer
 * existed.
 *
 * **Sub-task 6.6 removed all of it.** The pointer is rendered once into a
 * surface of its own, with a byte of coverage beside each pixel, and the
 * compositor composes it over the back buffer as the changed region is carried
 * to the display. What is beneath it is never overwritten, so nothing is saved,
 * nothing is restored, and nothing needs to declare that it is about to draw.
 * The save-under is what made a pointer possible one sub-task early; it was
 * never a mechanism the kernel meant to keep.
 */

#ifndef OXYS_GFX_CURSOR_H
#define OXYS_GFX_CURSOR_H

#include <oxys/types.h>
#include <oxys/gfx/graphics.h>

/*
 * The pointer's extent, in pixels.
 *
 * Twelve by eighteen is the shape drawn in graphics/cursor.c and is fixed by
 * that table rather than chosen here; the constants are declared so that the
 * rendered surface and its coverage mask, which the self-test reads, have a size
 * a reader can compute.
 */
#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 18

/*
 * Renders the shape in the two colours given and registers it as a compositor
 * layer, at the origin and hidden.
 *
 * The colours are supplied rather than named because a pixel value means nothing
 * without an encoding; for the display that is FramebufferEncode.
 *
 * Returns false where there is no compositor to draw upon, or where it will take
 * no further layer. Every routine below then does nothing, which is what a
 * machine with no display requires — and is not a failure of the machine.
 */
bool CursorInitialise(uint32_t outline, uint32_t interior);

/* Whether the pointer has a layer and may be shown. */
bool CursorIsAvailable(void);

/*
 * Shows and hides the pointer. Both are idempotent.
 *
 * Since sub-task 6.6 these are a layer's visibility and nothing more. There is
 * no concealment to nest and no store to keep: what is beneath the pointer is
 * in the back buffer, undisturbed, because the pointer was never drawn into it.
 */
void CursorShow(void);
void CursorHide(void);

/* Whether the pointer is shown. */
bool CursorIsVisible(void);

/*
 * Moves the pointer. A movement that ends where it began does nothing at all.
 *
 * The position is the hot spot — the pixel the pointer points at, which is the
 * tip at its top left — and not the top left of the bitmap by coincidence: the
 * two are the same point for this shape, and a shape whose tip lay elsewhere
 * would subtract the difference here.
 */
void CursorMoveTo(int32_t x, int32_t y);

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

/*
 * The rendered pointer and its coverage, for the self-test.
 *
 * These are what the compositor consumes, and asserting upon them asserts the
 * thing that is actually drawn rather than the bitmaps it was derived from — the
 * two being separated by exactly the conversion most likely to be wrong.
 */
const GraphicsSurface *CursorImageSurface(void);
const uint8_t *CursorImageMask(void);

/* How many times the pointer has actually moved, which is not how many packets
 * the mouse sent: a movement ending where it began is counted as nothing. */
uint64_t CursorMoveCount(void);

/* Emits a summary of the pointer's state upon both output devices. */
void CursorReport(void);

#endif /* OXYS_GFX_CURSOR_H */
