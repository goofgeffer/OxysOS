<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Compositor

**Phase**: sub-tasks 6.5 and 6.6 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../graphics/compositor.c`](../../graphics/compositor.c),
[`../../graphics/cursor.c`](../../graphics/cursor.c), the clip stack and blending in
[`../../graphics/draw.c`](../../graphics/draw.c); headers in
[`../../kernel/include/oxys/gfx/`](../../kernel/include/oxys/gfx/).
**Specifications**: none beyond those of [`FRAMEBUFFER.md`](FRAMEBUFFER.md).

The back buffer everything is drawn into, the layers composited over it on the way
to the screen, and the damage that decides how much of it is carried out. **Nothing
reads the framebuffer.** The windows drawn into the back buffer are
[`WINDOWS.md`](WINDOWS.md); the pointer's device is
[`../devices/MOUSE.md`](../devices/MOUSE.md).

## 1. Why a back buffer

Drawing directly on the framebuffer means anything drawn over something else must
save and restore what was beneath it, and anything that reads the screen reads
through a write-combining mapping where reads are uncached (a scroll read four
megabytes that way). Composing in ordinary memory and carrying the result to the
adapter removes both: what is beneath a thing is still there.

`CompositorSurface` is an ordinary `GraphicsSurface`, so the console, the fault
screen's primitives and the window manager draw into it without knowing
([`DRAWING.md`](DRAWING.md)). Without a compositor (no framebuffer, or no pages for
the buffer) the console draws on the framebuffer directly and behaves the same.

## 2. Layers

A layer is a surface, a position, a visibility, and an optional **coverage mask of
one byte per pixel**; at most `COMPOSITOR_LAYER_CAPACITY` (4) exist. Layers are
composited **during presentation**, not into the back buffer: the pixels of the
changed region are being written out anyway, and a covered pixel is simply
composed differently on the way. The back buffer is never disturbed, so nothing
needs restoring when a layer moves.

- **A mask, not a transparent colour.** A reserved colour is one the shape may not
  contain, and the pointer contains black and white. Coverage is a byte, so a soft
  edge needs only different mask values.
- **The pointer is a layer**, rendered once from its two bitmaps into a surface and
  a mask ([`../devices/MOUSE.md`](../devices/MOUSE.md)). Moving it is one call that
  marks two rectangles.

## 3. Damage

Changes accumulate as **one rectangle** enclosing them. Two changes at opposite
corners present everything between, which is the cost; the gain is that no amount
of drawing can overflow the record, and a list's overflow would have to present
everything anyway.

- A presentation (`CompositorPresent`) empties the damage; with nothing damaged it
  writes nothing.
- **A moving layer marks both where it was and where it went.** Marking only the
  destination leaves the old image standing: a pointer trail.
- Damage outside the buffer is discarded.

## 4. Clip stack and blending

Both live in `draw.c` and are used by the compositor and by the window manager.

- **`GraphicsPushClip` intersects** with the clip in force (never replaces), four
  deep; `GraphicsPopClip` restores. A nested panel cannot draw outside the region
  its caller was given.
- **`GraphicsBlendPixel` and `GraphicsBlendSurface`** combine each channel apart
  (through `FramebufferDecode`): a packed pixel interpolated whole carries between
  channels. Full coverage is exactly an opaque write; zero coverage does not read
  the destination. This is why an outline never writes a corner twice.

## 5. The fault screen

A fault screen draws straight on the framebuffer, and `FaultScreenBegin` calls
`CompositorSuspend`, permanently. Otherwise the next diagnostic write would present
the back buffer (a boot log) over the page just drawn. There is no resumption,
because everything that suspends the display has stopped the machine
([`FAULTSCREEN.md`](FAULTSCREEN.md)).

## 6. Concurrency

`KernelWriteString` holds a spinlock for a whole call, and a call is exactly one
presentation on the ordinary path; a processor drawing between the reading of the
damage and its clearing would otherwise have its work discarded. The window
manager presents from the tick on the bootstrap processor
([`WINDOWS.md`](WINDOWS.md)). The fault screen takes no lock, by decision.

## Verification

`KernelVerifyCompositing` and `KernelVerifyCompositor` in
[`../../kernel/test/gfx/compositor.c`](../../kernel/test/gfx/compositor.c). The
clip stack and blending are asserted on memory surfaces; the compositor on the real
one, since there is no second back buffer.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A push intersects rather than replaces. | A nested region drawing outside its parent. |
| A pop restores its push; a pop with nothing pushed is refused. | An underflow clipping everything to garbage. |
| A push beyond the bound is refused; a reset abandons saved clips. | An overflow; a later pop narrowing to a stale region. |
| Full coverage equals an opaque write; zero coverage writes nothing. | Everything opaque changed; a needless destination read. |
| Channels are blended apart. | Red over blue producing green. |
| A masked surface arrives as the mask's shape. | The mask indexed by the wrong pitch, shearing the shape. |
| Damage accumulates as the enclosing rectangle; outside damage is discarded. | An earlier change left on screen; a presentation past the buffer. |
| A presentation empties the damage; an empty one carries nothing. | Every presentation writing the whole screen. |
| The layer table refuses beyond its capacity, and a layer with no surface. | Writes past the table. |
| **The test returns every layer it takes.** | A machine booting with no pointer: the table exhausted by the test, the pointer unable to get a layer. |

By eye ([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)): the
pointer leaves no trail; text under it survives its passing; a fault screen is not
carried away by the next presentation.

## Limitations

1. One damage rectangle.
2. Presentation is synchronous, with no wait for vertical blank (the Multiboot2
   framebuffer offers no interrupt to wait on); a large presentation can be seen
   arriving.
3. Layers are composited pixel by pixel (the mask is per pixel); the back buffer is
   copied by words.
4. The back buffer is the display's size and is never resized; the mode never
   changes.
5. No text-cursor layer on the console.
