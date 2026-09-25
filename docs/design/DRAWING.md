<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Drawing Primitives

**Phase**: sub-task 6.3 of [`../project/PLAN.md`](../project/PLAN.md); the disc
from 9.1.
**Source**: [`../../graphics/draw.c`](../../graphics/draw.c),
[`../../kernel/include/oxys/gfx/graphics.h`](../../kernel/include/oxys/gfx/graphics.h).
**Specifications**: J. E. Bresenham, "Algorithm for computer control of a digital
plotter", *IBM Systems Journal* 4(1), 25–30, 1965; J. E. Bresenham, "A linear
algorithm for incremental digital display of circular arcs", *CACM* 20(2),
100–106, 1977.

The surface and what is drawn on it: pixel, filled and outlined rectangle, line,
disc, pattern block and blit, all bounded by one clip. Blending and the clip stack
belong to the compositor and are described in [`COMPOSITOR.md`](COMPOSITOR.md);
the fast word-wide paths were justified by the measurement in
[`CONSOLE.md`](CONSOLE.md).

## 1. The surface

Every primitive takes a `GraphicsSurface`: base, width, height, pitch, pixel size,
clip. The framebuffer is one surface among others, which is necessary because:

- a **blit** needs two surfaces;
- **assertions** need a surface in ordinary memory, of known size, cheap to read
  (the framebuffer may not exist and is slow to read through write-combining);
- **double buffering** ([`COMPOSITOR.md`](COMPOSITOR.md)) is handing the same code
  a back buffer instead.

A surface owns nothing: it describes memory someone else supplied, so the same
code draws into the framebuffer, a `.bss` array, or a window's content.

`whole_words` is set when pixels are four bytes, the base is word-aligned and the
pitch is a multiple of four; then the pixel, fill, pattern block and blit use
word-wide loops. Other depths use the byte path.

## 2. Clipping is the memory-safety boundary

Every primitive computes a byte offset and writes there; a shape that escaped its
bounds would write into whatever is mapped next. So:

- **The arithmetic lives in one place**, `GraphicsRectangleIntersect`.
  `GraphicsSetClip` intersects its argument with the surface, so a clip is always
  inside its surface and no argument can widen it.
- **A shape is clipped once**, and the surviving span is written with no test in
  the loop. The bound is computed once, where it can be read, rather than by a
  per-pixel test that must stay right in every loop.
- **Coordinates are signed** (a negative origin is the natural way to place a shape
  half off the edge) and **bounded** by `GRAPHICS_COORDINATE_LIMIT`: a line between
  distant points iterates once per step of its longer axis even when drawing
  nothing, so an unbounded coordinate is an unbounded loop. Beyond the bound, a
  shape is refused.

## 3. The line

Bresenham's algorithm: integers only (`PROJECT_GUIDELINES.md`, Section 8, forbids
floating point in the kernel).

**The clip is tested per pixel, not applied to the endpoints.** The algorithm's
choices depend on an error accumulated from the start point; clipping the
endpoints starts elsewhere and lights different pixels, so a line crossing the
seam between two clipped regions would kink. The promise is: **the pixels drawn
are exactly those of the unclipped line that lie within the clip**, at a cost of
two comparisons per step.

## 4. The disc

`GraphicsFillCircle`, written for a window's close control, which has been a cross
since 2026-09-25 ([`WINDOWS.md`](WINDOWS.md)),
is a stack of spans, one per row, each a rectangle fill, so the clip is applied by
the fill and no pixel is tested. Each half-width comes from Bresenham's 1977 circle
algorithm, walking the arc from the top to the diagonal with an integer measure of
distance from the true circle; the eight-fold symmetry supplies the other rows. The
row on the diagonal comes from both reflections and is simply filled twice with one
colour, rather than special-cased.

## 5. The blit

`GraphicsBlit` copies a rectangle between surfaces or within one.

- **Different pixel sizes are refused**: a byte copy would give the right size in
  the wrong colours.
- **If the destination clip trims the copy, the source is trimmed by exactly as
  much**; otherwise the result is shifted rather than cropped.
- **Overlap within one surface decides the direction.** Copying forwards reads
  bytes already overwritten and smears the image. Rows run bottom-up when the
  destination is below the source, and right-to-left when it is to the right on
  the same row. Scrolling is exactly this case.

## Verification

`KernelVerifyGraphics` in [`../../kernel/test/gfx/graphics.c`](../../kernel/test/gfx/graphics.c)
draws on a memory surface of 32 by 16 four-byte pixels **in rows of 40**. The eight
padding pixels per row hold a sentinel no test writes, and are checked after every
operation: a primitive stepping by width instead of pitch still writes inside the
array, only to the wrong pixels, and the padding makes that a direct assertion.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| An empty or negative rectangle is empty; a missed intersection is empty **and not negative**. | A backwards region; a negative extent that passes one loop bound and fails another. |
| Touching rectangles do not overlap; a rectangle does not contain the column past its edge. | Adjacent regions sharing a column; the containment test everything clips with, off by one. |
| A clip of `{-1000, -1000, 100000, 100000}` is confined to the surface; a clip outside it is empty and nothing draws. | A widened clip; a broken cheapest rejection. |
| One pixel set changes one pixel; a pixel outside writes nothing and reads zero. | Overlapping address arithmetic; an unenforced boundary. |
| A fill covers exactly its area, corners included, stopping one short of its extent. | Off by one either way. |
| A rectangle straddling the corner leaves exactly the 5×5 inside. | A rectangle dropped entirely because part was outside. |
| A 6×4 outline is exactly 16 pixels and hollow. | Doubled corners (wrong under blending); a fill. |
| `GraphicsClear` fills the clip, not the surface. | An erase that cannot target a region. |
| A horizontal line includes both ends; a point-to-itself line is one pixel. | Off by one; termination tested before drawing. |
| A diagonal is as long as its longer axis and passes through its middle. | Wrong error accumulation. |
| A line drawn backwards lights the same pixels. | Edges depending on drawing order. |
| **A clipped line lights exactly the unclipped line's pixels inside the clip** (drawn both ways and compared). | Clipping moving the line. |
| A coordinate past the limit draws nothing. | An unbounded loop. |
| A blit between pixel sizes is refused; a rectangle arrives whole and in place. | Wrong colours; wrong offsets. |
| A blit trimmed by the clip takes the right part of a source with distinct halves. | A shifted image. |
| Moving rows up and down within one surface both work; a blit clipped away entirely succeeds. | Smearing; a correct no-op reported as a refusal. |
| **The padding is intact after all of these.** | Width used for pitch. |

`KernelVerifyCircle` in [`../../kernel/test/gfx/windows.c`](../../kernel/test/gfx/windows.c):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The centre is filled; each axis reaches exactly the radius. | A wrong initial error term: a disc of the wrong radius. |
| For radius 5, `(3, 4)` is filled and `(4, 4)` is not. | A step on the wrong sign: square corners. |
| The disc is symmetric in both axes, with nothing outside its bounding square. | A reflection with a wrong sign; a span from the wrong reflection. |
| A disc at the surface's corner writes nothing into the padding. | A span escaping the surface. |

The self-test also draws a figure on the framebuffer for a person to judge
([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)): a frame whose
edges lean if the pitch is wrong; crossed diagonals that meet at the centre only if
the line is exact; a panel clipped down its middle, where fill and line stop dead
without the line changing slope; and a blitted copy that must match.

## Limitations

1. Lines are one pixel wide and not antialiased; the only curve is the filled disc.
2. The blit does not scale.
3. No lock. A primitive is too small to own one; the surface's owner must
   ([`COMPOSITOR.md`](COMPOSITOR.md)).
