# `graphics/` — The Display

**Phase**: 6, sub-tasks 6.2 to 6.6. This directory is created by sub-task 6.2 and
grows through the four that follow it.
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
arithmetic upon the memory at that address. The one exception, the mouse of
sub-task 6.5, is a PS/2 device and will live in `drivers/` beside the keyboard
that shares its controller.

## Contents

| Path | Description |
| ---- | ----------- |
| `draw.c` | Sub-task 6.3. The two-dimensional primitives upon a surface: the rectangle arithmetic every one of them clips with, the pixel, the filled and outlined rectangle, Bresenham's line, and the blit — including the overlapping case a console scrolls with. `GraphicsRectangleIsEmpty`, `GraphicsRectangleIntersect`, `GraphicsRectangleContains`, `GraphicsSurfaceInitialise`, `GraphicsSurfaceFromFramebuffer`, `GraphicsSurfaceBounds`, `GraphicsSetClip`, `GraphicsResetClip`, `GraphicsClip`, `GraphicsPutPixel`, `GraphicsPixelAt`, `GraphicsFillRectangle`, `GraphicsDrawRectangle`, `GraphicsClear`, `GraphicsDrawLine`, `GraphicsBlit`, `GraphicsReport`. |
| `framebuffer.c` | Sub-task 6.2. Acquires the framebuffer described in the Multiboot2 boot information, gives its pages the write-combining memory type through the page attribute table, maps them into the kernel arena, and describes what was obtained. `FramebufferInitialise`, `FramebufferIsPresent`, `FramebufferIsGraphical`, `FramebufferAddress`, `FramebufferWidth`, `FramebufferHeight`, `FramebufferPitch`, `FramebufferBitsPerPixel`, `FramebufferBytesPerPixel`, `FramebufferByteCount`, `FramebufferFormat`, `FramebufferEncode`, `FramebufferWriteCombining`, `FramebufferReport`. |

The interfaces are declared in
[`../kernel/include/oxys/framebuffer.h`](../kernel/include/oxys/framebuffer.h) and
[`../kernel/include/oxys/graphics.h`](../kernel/include/oxys/graphics.h), with the
rest of the kernel's header corpus, so that a consumer depends upon an interface
and not upon this directory.

**Nothing here draws upon the framebuffer by name.** Every primitive takes a
surface, of which the framebuffer is one; a surface composed in ordinary memory
is another, and is what the self-tests are conducted upon. That is what lets the
drawing be asserted pixel by pixel on a machine with no display, and what will
let sub-task 6.6 hand the same code a back buffer instead.

## Specifications implemented

| Specification | Sections | Applied to |
| ------------- | -------- | ---------- |
| Multiboot2 Specification 2.0 | 3.1.10, 3.6.12 | The framebuffer request tag carried in the image header, and the information tag describing what the boot loader supplied. |
| Intel SDM, Volume 3A | 11.12.2, 11.12.3, Tables 11-7, 11-10, 11-11 | The page attribute table: its eight entries, the index a page-table entry selects by `(PAT << 2) \| (PCD << 1) \| PWT`, the write-combining encoding, and the combination with the memory type range registers. |
| Intel SDM, Volume 2A, `CPUID` | — | Leaf 1, EDX bit 16: whether the page attribute table exists at all. |
| J. E. Bresenham, IBM Systems Journal 4(1), 1965 | — | The integer line algorithm of `draw.c`, which decides each step from an accumulated error and uses no division and no floating point. |

Full citations are held in
[`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## Present limitations

The complete list is `docs/design/GRAPHICS.md`, Section 10. The three that govern
what can be built next:

1. **There is no text.** The primitives draw shapes; the font that draws
   characters is sub-task 6.4. There is also no blending, no clip stack, no
   scaling blit and no curve.
2. **There is no visible console until sub-task 6.4.** Requesting a framebuffer
   puts the adapter in a graphics mode, so the VGA text driver writes to memory
   nothing displays. The serial port carries the whole boot log meanwhile.
3. **The mode cannot be chosen.** GRUB selects it and ignores what it is asked
   for; the kernel accepts whatever it is handed and asserts what it was.
