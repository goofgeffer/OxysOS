<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Drawing Primitives

**Corresponding phase**: 6, sub-task 6.3 — the surface, the clipping that bounds
every write to it, and the pixel, rectangle, line and blit drawn within that
bound.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.

**Implemented by**: [`../../graphics/draw.c`](../../graphics/draw.c),
[`../../kernel/include/oxys/gfx/graphics.h`](../../kernel/include/oxys/gfx/graphics.h).

**Asserted by**: `KernelVerifyGraphics` in
[`../../kernel/test/gfx/graphics.c`](../../kernel/test/gfx/graphics.c).

**Specifications**: J. E. Bresenham, "Algorithm for computer control of a digital
plotter", IBM Systems Journal 4(1), pages 25 to 30, 1965.

**Where this sits**: the second of the five documents the graphical work of
sub-tasks 6.2 to 6.6 is divided into. [`GRAPHICS.md`](GRAPHICS.md) is the index
and records why they are five. The measurement that later specialised these
primitives for the console is [`CONSOLE.md`](CONSOLE.md), Section 6, and it is
recorded there because the console is what it was measured upon.

---

## 1. The primitives (sub-task 6.3)

Sub-task 6.2 supplied memory and facts about it. Sub-task 6.3 supplies the
operations that write into it: a pixel, a line, a filled and an outlined
rectangle, a blit, and the clipping that governs all of them.

## 2. The surface, and why the primitives do not name the framebuffer

Every primitive takes a `GraphicsSurface`: a rectangle of pixels, with a pitch, a
pixel size and a clip. The framebuffer is one such surface and is not privileged
among them.

Writing to the framebuffer directly would have been shorter by one argument and
worse in three ways.

**Blit has no meaning with one surface.** It copies from somewhere to somewhere,
so a framebuffer-only design would have had to name the framebuffer twice or
invent a second thing anyway — which is the surface, arrived at by a longer road.

**Nothing could be asserted without a display.** A primitive is judged by which
pixels it set and which it left alone. The framebuffer answers that badly: it may
not exist, it is slow to read through a write-combining mapping, and its size is
whatever the boot loader chose. A surface in ordinary memory answers it exactly,
and that is what the whole of the self-test is built upon.

**The double buffering of sub-task 6.6 is the substitution of one surface for
another.** A caller that had named the framebuffer everywhere could not be handed
a back buffer instead.

A surface owns nothing. It is a description of memory somebody else supplied,
with no allocation and no lifetime, which is what lets the same code draw into a
framebuffer, into a `.bss` array and, later, into a window's backing store.

## 3. Clipping is the memory-safety boundary

Every routine here computes a byte offset into a surface and writes to it. A
shape that escapes its bounds does not draw in the wrong place — it writes into
whatever the arena mapped next. **Clipping is therefore not a convenience and not
an optimisation; it is the boundary that makes the whole graphical stack safe**,
and everything else follows from treating it that way.

Two decisions come out of it.

**It is implemented once.** `GraphicsRectangleIntersect` is the only place the
arithmetic lives. `GraphicsSetClip` intersects whatever it is given with the
surface, so a clip is *always* within its surface and no caller can widen it by
any argument, however large or negative. That is what lets every primitive treat
the clip as sound without inspecting it.

**The shape is clipped once, and the surviving span is then written with no test
in the loop at all.** This is faster than testing each pixel, but speed is not
why it is written this way: the bound is computed once, from arithmetic checked
once, in a place a reader can look at. A loop that tested each pixel would be
correct only for as long as every one of its tests stayed correct.

Coordinates are signed, because placing a shape half off the edge is the ordinary
case and the natural way to say it is a negative origin. They are bounded by
`GRAPHICS_COORDINATE_LIMIT`, because a line between two very distant points costs
one iteration per step of its longer axis even when it writes nothing: an
unbounded coordinate is an unbounded loop. A coordinate outside the bound is
refused, and a refusal is better than a machine that appears to have stopped.

## 4. The line, and the one place this differs from the usual arrangement

The line is Bresenham's: integer throughout, no division, no floating point —
which `PROJECT_GUIDELINES.md`, Section 8, prohibits in the kernel in any case.

**The clip is tested per pixel rather than applied to the endpoints first, and
that is a deliberate departure from the usual arrangement.** Clipping the
endpoints and drawing between the clipped ones is what most implementations do,
and it is faster. It also draws a *different line*.

The algorithm chooses at each step from an error accumulated since the start.
Begin at a different start and a different error accumulates, and here and there
a different pixel is lit. The visible result is that a shape crossing the edge of
a clip is displaced by a pixel where it crosses — invisible until two clipped
regions meet along a seam and the line running through them has a kink in it.

So the header promises something stronger: **the pixels drawn are exactly those
of the unclipped line that fall within the clip.** The cost is two comparisons
per step. Section 6 asserts the promise directly, by drawing the line both ways
and comparing pixel for pixel.

## 5. The blit, and the direction of the copy

`GraphicsBlit` copies a rectangle between surfaces, or within one.

**A blit between differing pixel sizes is refused, not performed.** Copying byte
by byte across depths would produce an image of exactly the right size in
entirely the wrong colours, which looks like a fault in the drawing and is a
fault in the caller.

**The destination's clip may trim the copy, and the source must then be trimmed
by exactly as much.** Trimming one without the other copies the right number of
pixels from the wrong place — a shifted image rather than a cropped one, which is
the harder of the two to notice.

**Where the surfaces are the same and the regions overlap, the direction of the
copy decides whether it is correct.** Copying forwards through an overlap reads
bytes the copy has already overwritten, and the image smears in the direction of
the move. Rows are therefore taken from the bottom where the destination lies
below the source, and each row from its right where the destination lies to the
right on the same row.

This is not a corner case to be tidy about. **Scrolling is exactly the overlapping
case** — the whole screen moved up by one row of text — and it is what the console
of sub-task 6.4 is built upon.

## 6. Verification of the primitives

`KernelVerifyGraphics` asserts against a surface composed in memory: 32 by 16
pixels of four bytes, **in rows of 40**.

The pitch exceeds the width deliberately, and the eight pixels of padding on each
row are filled with a sentinel no test ever writes. A primitive that stepped from
row to row by the width instead of the pitch would still write inside the array —
it would simply write the wrong pixels — and every assertion about the image would
then have to be relied upon to notice. The padding turns that into a direct
assertion, made after each operation so that a failure names the operation that
caused it.

### 6.1 Rectangles and the clip

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A rectangle of zero or negative extent is empty | A caller could ask for a region that runs backwards. |
| An intersection that misses is empty **and not negative** | A negative extent passes a `< width` loop bound by doing nothing and fails one computed as an end coordinate. |
| Rectangles that merely touch do not overlap | Every adjacent pair of regions would share a column. |
| A rectangle does not contain the column past its right edge | An off-by-one in the fundamental containment test, which everything else clips with. |
| A clip of `{-1000, -1000, 100000, 100000}` is confined to the surface | The clip could be widened past the surface, and no primitive checks it. |
| A clip wholly outside the surface is empty, and drawing against it writes nothing | Every primitive's cheapest rejection is broken. |

### 6.2 Pixels, fills and outlines

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Setting one pixel changes exactly one | The address arithmetic overlaps neighbours. |
| A pixel outside the surface writes nothing and reads as zero | The boundary is not enforced on either path. |
| A fill covers exactly its area, reaches its corners, and stops one short of its extent | The commonest off-by-one, in both directions. |
| A rectangle straddling the top-left corner leaves exactly the 5×5 that remains | A fill that dropped the whole rectangle because part fell outside would look identical from the point of view of memory. |
| An outline of 6×4 is exactly 16 pixels | Doubled corners, or short edges. Doubling is harmless for an opaque colour and ceased to be harmless at sub-task 6.6, which admitted blending: a corner written twice is blended twice and is the wrong colour. |
| An outline is hollow | It is a fill. |
| `GraphicsClear` fills the clip, not the surface | It could not be used to erase one region of a screen. |
| **The padding is intact after every one of these** | A row was addressed by the width instead of the pitch. |

### 6.3 The line

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A horizontal line includes both endpoints | The commonest off-by-one here. |
| A line from a point to itself is one pixel, not none | The loop tests its termination before drawing. |
| A diagonal's length is its longer axis, and it passes through its own middle | The error accumulation is wrong. |
| A line drawn backwards lights **the same pixels** | Bresenham accumulates from one end; a careless implementation is not symmetric, and a shape's edges would depend upon the order they were drawn in. |
| **A clipped line lights exactly the pixels the unclipped line lights inside the clip** | Clipping moved the line. This is the promise of Section 4, asserted pixel for pixel: the unclipped line is drawn and its pixels inside the region recorded, the surface is cleared entirely, and the same line is drawn clipped. |
| A coordinate beyond the limit draws nothing | An unbounded loop. |

### 6.4 The blit

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Differing pixel sizes are refused | An image of the right size in the wrong colours. |
| A rectangle arrives whole and in position | The offset arithmetic is wrong. |
| A blit trimmed by the clip takes **the right part** of the source | A shifted image rather than a cropped one. The source is given a distinguishable left and right half so the two are told apart. |
| Moving rows **up** within one surface moves them | The copy read bytes it had already overwritten. |
| Moving rows **down** within one surface moves them | The row order was not reversed; the first row read would smear down the region. |
| A blit clipped away entirely returns success | An operation that correctly did nothing was reported as a refusal. |

### 6.5 What a person judges

The self-test also draws upon the framebuffer, and that figure is judged by eye;
`docs/project/TESTING-GRAPHICS.md`, Section 2, records what to look for. It is composed so
that looking at it establishes something: a frame around the whole screen, whose
vertical edges lean if the pitch is wrong; a panel with its diagonals crossing,
which meet at the centre only if the line is exact; a second panel drawn through
a clip covering its left half, where the fill and the line must stop dead at the
boundary without the line changing slope; and the first panel blitted below
itself, which must be identical and in the right place.

### 6.6 The negative test

`GraphicsPixelAddress` was changed to step by the width instead of the pitch. Six
assertions fired, in six independent primitives — the fill, the clipped fill, the
clear, the line, the blit and the trimmed blit — each reporting that it had
written into the row padding. The edit was then reverted.

## 7. Limitations of the primitives

1. ~~**There is no clip stack.**~~ Resolved by sub-task 6.6: `GraphicsPushClip`
   and `GraphicsPopClip`, four deep, intersecting rather than replacing. See
   [`COMPOSITOR.md`](COMPOSITOR.md), Section 2.5.
2. ~~**There is no blending.**~~ Resolved by sub-task 6.6:
   `GraphicsBlendPixel` and `GraphicsBlendSurface`, combining the channels apart
   through `FramebufferDecode`. The outline's care not to write a corner twice,
   taken in anticipation of this, is now load-bearing. See [`COMPOSITOR.md`](COMPOSITOR.md), Section 2.2.
3. **Lines are one pixel wide and unantialiased**, and there are no curves. A
   thicker line is several lines, which nothing needs yet.
4. **The blit does not scale.** Source and destination rectangles are the same
   size by construction.
5. **Nothing is safe against concurrent drawing.** Two processors drawing upon
   one surface require the lock sub-task 6.13 built, and since sub-task 6.14
   there are two processors. That lock is still not taken here, and should not
   be: a primitive is far too small a thing to own a lock, and the right place is
   the surface's owner — [`COMPOSITOR.md`](COMPOSITOR.md), limitation 6, is where
   that obligation is recorded and where 6.14 discharged the part of it that the
   ordinary diagnostic path represents. **The change that widens a user thread's
   affinity mask is what makes the
   remainder contended**, by giving a second processor something to draw.
6. ~~**No fast path uses the pixel size.**~~ Resolved in the course of sub-task
   6.4, by the measurement of [`CONSOLE.md`](CONSOLE.md), Section 6, which is the specialisation this
   limitation said did not yet exist a justification for. A surface records
   `whole_words` — four bytes to the pixel, a word-aligned base and a pitch that
   is a multiple of four — and where it holds, the pixel, the fill, the pattern
   block and the blit each take a word-wide loop of their own rather than a test
   inside the byte loop. The byte path remains and is what a surface of any other
   depth still uses.

---

