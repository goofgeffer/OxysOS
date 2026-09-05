# `graphics/` — The Display

**Phase**: 6, sub-tasks 6.2 to 6.6. This directory is created by sub-task 6.2 and
grows through the four that follow it; 6.2 to 6.5 are done.
**Detailed design**: [`../docs/design/GRAPHICS.md`](../docs/design/GRAPHICS.md).

## Purpose

This directory holds everything that puts pixels on a screen: the framebuffer the
boot loader hands over, the primitives that draw into it, the font that renders
text upon it, and the compositing surface that arranges what is drawn.

**None of it needs a process to exist**, and that is why it is in Phase 6 rather
than in Phase 9 with the rest of the graphical work. A framebuffer is memory the
boot loader describes and this kernel maps; primitives, a font and a surface are
arithmetic upon that memory. What does need processes — the window manager, the
client protocol, the desktop — stays in Phase 9, after the shell.
`docs/project/PLAN.md` records the division under Phase 6.

This is not a device driver directory. `drivers/` holds code that programs
hardware through its registers; the framebuffer is not programmed at all. The
boot loader sets the mode and hands over an address, and everything here is
arithmetic upon the memory at that address.

The mouse of sub-task 6.5 shows where the line falls. The **device** is in
`drivers/mouse/`, being a real PS/2 device upon the controller the keyboard
shares; its **picture** — the shape, the pixels beneath it, the restoring of them
— is `cursor.c` here, being arithmetic upon memory that would be identical if the
position came from somewhere else entirely.

## Contents

| Path | Description |
| ---- | ----------- |
| `draw.c` | Sub-task 6.3. The two-dimensional primitives upon a surface: the rectangle arithmetic every one of them clips with, the pixel, the filled and outlined rectangle, Bresenham's line, and the blit — including the overlapping case a console scrolls with. `GraphicsRectangleIsEmpty`, `GraphicsRectangleIntersect`, `GraphicsRectangleContains`, `GraphicsSurfaceInitialise`, `GraphicsSurfaceFromFramebuffer`, `GraphicsSurfaceBounds`, `GraphicsSetClip`, `GraphicsResetClip`, `GraphicsClip`, `GraphicsPutPixel`, `GraphicsPixelAt`, `GraphicsFillRectangle`, `GraphicsDrawRectangle`, `GraphicsClear`, `GraphicsPatternBlock`, `GraphicsDrawLine`, `GraphicsBlit`, `GraphicsReport`. |
| `font.c` | Sub-task 6.4. The bitmap face — ninety-five glyphs of eight by eight covering the printable ASCII range, **drawn for this project rather than obtained**, with a picture comment beside each — and three ways of drawing one: transparent, opaque, and enlarged for a banner. `FontCovers`, `FontGlyph`, `FontGlyphRow`, `FontDrawGlyph`, `FontDrawGlyphOpaque`, `FontDrawGlyphScaled`. |
| `console.c` | Sub-task 6.4. The graphical console: a grid of character cells upon the framebuffer, the four control characters of ANSI X3.4-1986 as the text-mode driver implements them, a scroll performed by blitting the surface upon itself, and a buffer that replays what was written before the framebuffer could be mapped. `ConsoleInitialise`, `ConsoleIsActive`, `ConsoleWriteCharacter`, `ConsoleWriteString`, `ConsoleSetColour`, `ConsoleColumns`, `ConsoleRows`, `ConsoleColumn`, `ConsoleRow`, `ConsoleSetEraseLimit`, `ConsoleReport`. |
| `faultscreen.c` | Sub-task 6.4. The full-screen page a fault the kernel cannot survive produces — which faults those are is `ExceptionDispositionOf`'s decision, not this file's, a fault belonging to a program drawing nothing here: a table of screens, one for each fault, each with its own title, colour, account of what the processor is reporting, direction as to what to examine first, and evidence panels chosen for that fault. Runs inside a fault handler, so it allocates nothing, reads no address without asking the paging hierarchy, and draws once. `FaultScreenShowException`, `FaultScreenShowPanic`, `FaultScreenDemonstrate`, `FaultScreenWasDrawn`, `FaultScreenEntryCount`, `FaultScreenEntryAt`. |
| `cursor.c` | Sub-task 6.5. The pointer: a shape of two bitmaps, drawn for this project, that says of each pixel whether the pointer covers it and, if so, in which of two colours — three states, because an arrow of one colour vanishes against itself. It keeps the pixels beneath it and puts them back as it moves, there being one surface and no back buffer until 6.6, and offers a **counted** conceal and reveal by which anything else may draw. `CursorInitialise`, `CursorIsAvailable`, `CursorShow`, `CursorHide`, `CursorIsVisible`, `CursorMoveTo`, `CursorConceal`, `CursorReveal`, `CursorX`, `CursorY`, `CursorShapeIsOpaque`, `CursorShapeIsInterior`, `CursorDrawCount`, `CursorRestoreCount`, `CursorReport`. |
| `framebuffer.c` | Sub-task 6.2. Acquires the framebuffer described in the Multiboot2 boot information, gives its pages the write-combining memory type through the page attribute table, maps them into the kernel arena, and describes what was obtained. `FramebufferInitialise`, `FramebufferIsPresent`, `FramebufferIsGraphical`, `FramebufferAddress`, `FramebufferWidth`, `FramebufferHeight`, `FramebufferPitch`, `FramebufferBitsPerPixel`, `FramebufferBytesPerPixel`, `FramebufferByteCount`, `FramebufferFormat`, `FramebufferEncode`, `FramebufferWriteCombining`, `FramebufferReport`. |

The interfaces are declared in
[`../kernel/include/oxys/framebuffer.h`](../kernel/include/oxys/framebuffer.h),
[`../kernel/include/oxys/graphics.h`](../kernel/include/oxys/graphics.h),
[`../kernel/include/oxys/font.h`](../kernel/include/oxys/font.h) and
[`../kernel/include/oxys/console.h`](../kernel/include/oxys/console.h),
[`../kernel/include/oxys/faultscreen.h`](../kernel/include/oxys/faultscreen.h) and
[`../kernel/include/oxys/cursor.h`](../kernel/include/oxys/cursor.h),
with the
rest of the kernel's header corpus, so that a consumer depends upon an interface
and not upon this directory.

**Nothing here draws upon the framebuffer by name.** Every primitive takes a
surface, of which the framebuffer is one; a surface composed in ordinary memory
is another, and is what the self-tests are conducted upon. That is what lets the
drawing be asserted pixel by pixel on a machine with no display, and what will
let sub-task 6.6 hand the same code a back buffer instead.

The pointer is where that stopped being an argument and became a return. It reads
the surface it draws upon — the first thing outside a self-test to do so, because
a compositor reads — and it is asserted upon a surface in memory including the
assertion that a pointer at the edge writes nothing into the row padding, which
no framebuffer could be asked.

## Specifications implemented

| Specification | Sections | Applied to |
| ------------- | -------- | ---------- |
| Multiboot2 Specification 2.0 | 3.1.10, 3.6.12 | The framebuffer request tag carried in the image header, and the information tag describing what the boot loader supplied. |
| Intel SDM, Volume 3A | 11.12.2, 11.12.3, Tables 11-7, 11-10, 11-11 | The page attribute table: its eight entries, the index a page-table entry selects by `(PAT << 2) \| (PCD << 1) \| PWT`, the write-combining encoding, and the combination with the memory type range registers. |
| Intel SDM, Volume 2A, `CPUID` | — | Leaf 1, EDX bit 16: whether the page attribute table exists at all. |
| J. E. Bresenham, IBM Systems Journal 4(1), 1965 | — | The integer line algorithm of `draw.c`, which decides each step from an accumulated error and uses no division and no floating point. |
| ANSI X3.4-1986 | — | The printable range `0x20` to `0x7E` the font of `font.c` covers, and the four control characters `console.c` interprets — the same four, given the same meanings, as the text-mode driver. |

Full citations are held in
[`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## Present limitations

The complete lists are `docs/design/GRAPHICS.md`, Sections 10, 17 and 20. The
three that govern what can be built next:

1. **There is no blending, no clip stack, no scaling blit and no curve.** Every
   colour is opaque and a pixel is written rather than combined; alpha belongs
   with the compositing of sub-task 6.6.
2. **The console is plain.** No cursor is drawn, there are no colours per
   character, a scroll redraws the whole screen, and nothing is buffered
   off-screen. All four are answered by sub-task 6.6 and none is felt at the rate
   a boot log is written.
3. **The mode cannot be chosen.** GRUB selects it and ignores what it is asked
   for; the kernel accepts whatever it is handed and asserts what it was.

## What was measured

The console was measured, after it worked, at **15.2% of the whole boot**, and
the cause was not where it had been guessed at. Section 23 of the design document
records the figures, what was actually wrong, and the three changes that brought
it to 4.5%. The remaining factor is the framebuffer read a scroll performs, and
removing it needs the back buffer of sub-task 6.6.
