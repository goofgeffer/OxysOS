/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/gfx/compositor.h
 * Purpose: Declares the compositor: a back buffer standing in for the
 *          framebuffer, an ordered list of layers composited over it, the region
 *          of it that has changed, and the presentation that carries that region
 *          to the display.
 * Key definitions: COMPOSITOR_LAYER_CAPACITY, CompositorInitialise,
 *          CompositorIsActive, CompositorSurface, CompositorAddLayer,
 *          CompositorMoveLayer, CompositorSetLayerVisible, CompositorInvalidate,
 *          CompositorInvalidateAll, CompositorPresent, CompositorSuspend,
 *          CompositorReport.
 * References:
 *   - docs/design/COMPOSITOR.md: the design of this layer and the four
 *     things sub-tasks 6.2 to 6.5 deferred to it.
 *   - docs/design/DRAWING.md: a surface owns nothing and describes
 *     memory somebody else supplied, which is what allows the back buffer to be
 *     substituted for the framebuffer without a caller knowing.
 *   - docs/design/CONSOLE.md: reads from the write-combining
 *     framebuffer mapping are uncached and are the expensive half of everything
 *     drawn. The back buffer is ordinary memory, so a scroll reads it at cache
 *     speed and nothing ever reads the framebuffer again.
 */

#ifndef OXYS_GFX_COMPOSITOR_H
#define OXYS_GFX_COMPOSITOR_H

#include <oxys/types.h>
#include <oxys/gfx/graphics.h>

/*
 * How many layers may stand over the back buffer at once.
 *
 * Four, because the layers that exist are the pointer and whatever a shell will
 * want, and a fixed table with a refusal beyond it is honest where a growing one
 * would need an allocator this must work without — the compositor is prepared
 * before the heap is asked for anything of consequence.
 */
#define COMPOSITOR_LAYER_CAPACITY 4U

/* The value CompositorAddLayer returns when it cannot take another layer. */
#define COMPOSITOR_LAYER_NONE ((size_t)-1)

/*
 * Prepares the back buffer and takes the display.
 *
 * The buffer is the framebuffer's width and height with a pitch of its own —
 * tightly packed, the padding a boot loader asks of an adapter being the
 * adapter's business and not this buffer's.
 *
 * Returns false where there is no framebuffer, or where the arena cannot supply
 * the pages. Neither is fatal: everything above draws through CompositorSurface,
 * which then reports no surface, and the machine carries its diagnostics upon
 * the serial port as it did before there was a display at all.
 */
bool CompositorInitialise(void);

/* Whether the back buffer exists and may be drawn into. */
bool CompositorIsActive(void);

/*
 * The surface everything draws into: the back buffer, never the framebuffer.
 *
 * Returns null where the compositor is not active. A caller that held the
 * framebuffer's own surface instead would have its drawing overwritten by the
 * next presentation, which copies the back buffer over the whole changed region.
 */
GraphicsSurface *CompositorSurface(void);

/*
 * Adds a layer: a surface composited over the back buffer at presentation, with
 * an optional mask of one coverage byte per pixel in the surface's dimensions.
 *
 * Layers are composited in the order they were added, so the last added is the
 * topmost. Returns COMPOSITOR_LAYER_NONE where the table is full or the
 * arguments name nothing.
 *
 * The surface and the mask are borrowed and not copied: they must outlive the
 * layer, exactly as a surface's pixels must outlive the surface.
 */
size_t CompositorAddLayer(const GraphicsSurface *surface, const uint8_t *mask, int32_t x,
                          int32_t y);

/*
 * Withdraws a layer, freeing its place in the table and marking what it covered
 * as changed. A layer withdrawn without that would leave its last appearance
 * standing upon the display.
 */
void CompositorRemoveLayer(size_t layer);

/*
 * Moves a layer, and marks both where it was and where it now is as changed.
 *
 * Both, and that is the whole of what replaces the save-under of sub-task 6.5: a
 * layer that marked only its new position would leave its old one standing upon
 * the display until something else happened to change those pixels.
 */
void CompositorMoveLayer(size_t layer, int32_t x, int32_t y);

/* Shows or hides a layer, marking the region it occupies as changed. */
void CompositorSetLayerVisible(size_t layer, bool visible);

/* Whether a layer is presently shown. */
bool CompositorLayerIsVisible(size_t layer);

/*
 * Records that a region of the back buffer has changed and must reach the
 * display at the next presentation.
 *
 * The regions accumulate as their bounding rectangle rather than as a list. Two
 * cells at opposite corners of the screen therefore present the whole screen,
 * which is the cost of a scheme that cannot fragment; what it buys is that no
 * amount of drawing can exhaust it, and a compositor that ran out of dirty
 * regions would have to present everything anyway.
 */
void CompositorInvalidate(GraphicsRectangle region);

/* Records that the whole back buffer has changed. */
void CompositorInvalidateAll(void);

/*
 * Carries the changed region to the display: the back buffer first, then every
 * visible layer that intersects it, composited in order.
 *
 * Nothing is read from the framebuffer. The colour beneath a layer is taken from
 * the back buffer, which is ordinary cached memory, and only the result is
 * written out — so a presentation costs one write per changed pixel and no reads
 * from the mapping where reads are uncached.
 *
 * The changed region is empty afterwards. A presentation with nothing changed
 * writes nothing and is not an error: it is what the echo loop does between
 * keystrokes.
 */
void CompositorPresent(void);

/*
 * Stops presenting, permanently.
 *
 * A fault screen draws straight upon the framebuffer, the machine having stopped
 * and the back buffer being no longer worth trusting. Without this the next
 * KernelWriteString — and the panic path makes several — would carry the back
 * buffer over the top of the page just drawn, which is the fault of sub-task
 * 6.4's console scrolling the screen out from under a fault screen, arriving
 * again by a different route.
 *
 * There is deliberately no resumption. Everything that suspends the display has
 * stopped the machine.
 */
void CompositorSuspend(void);

/* Accounting, for the report and for the self-test. */
uint64_t CompositorPresentCount(void);
uint64_t CompositorPixelsPresented(void);
size_t CompositorLayerCount(void);

/* The region presently awaiting presentation, which a self-test asserts about. */
GraphicsRectangle CompositorDamage(void);

/* Emits the back buffer's geometry, its layers and the accounting. */
void CompositorReport(void);

#endif /* OXYS_GFX_COMPOSITOR_H */
