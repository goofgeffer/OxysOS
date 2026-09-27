<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Faces and the Graphical Console

**Phase**: sub-task 6.4 of [`../project/PLAN.md`](../project/PLAN.md); the desktop's face, 9.9.
**Source**: [`../../graphics/font.c`](../../graphics/font.c), [`../../graphics/face.c`](../../graphics/face.c),
[`../../graphics/console.c`](../../graphics/console.c), and their headers in
[`../../kernel/include/oxys/gfx/`](../../kernel/include/oxys/gfx/).
**Specifications**: ANSI X3.4-1986 (the printable range and the control
characters).

The bitmap face drawn for this project, the console that draws the boot log with
it on the framebuffer, and the desktop's face, Inter, which window titles and
programs draw text with ([`SESSION.md`](SESSION.md)). The high-resolution mark
and icons are separate ([`SESSION.md`](SESSION.md)).

## 1. The 8-by-8 face

Ninety-five glyphs, `0x20` to `0x7E`, compiled into the image.

- **Drawn for this project**, pixel by pixel. The code page 437 face circulates
  under no stated licence, so it cannot be recorded in `LICENSING.md` as a
  third-party work must be (`PROJECT_GUIDELINES.md`, Section 2).
  Reading the firmware's font from VGA plane 2 is not possible either: the loader
  has already set a graphics mode, and UEFI (Phase 12) has no VGA at all.
- **Eight by eight.** A glyph row is one byte, **most significant bit leftmost**,
  so each row reads left to right like the picture comment beside it. The comments
  are unchecked; change a glyph's bytes and its picture together.

| Metric | Value |
| ------ | ----- |
| Ink | Columns 0–5 |
| Spacing | Columns 6–7, always clear |
| Capitals and digits | Rows 0–6, baseline row 6 |
| Lower case | Rows 2–6 |
| Descenders (`g j p q y`) | Reach row 7 |

The two clear columns are all the spacing there is, so a console draws at a stride
of exactly `FONT_WIDTH`.

- **Every code outside the range draws a hollow box**, so `FontGlyph` never
  returns `NULL`, and an unmapped character is visible in the log rather than
  looking like a space.
- **`FontDrawGlyph` sets only the ink**, so text can be stencilled over an image;
  **`FontDrawGlyphOpaque`** draws the cell's background and ink in one pass, which
  the console uses. Both must light the same ink ([`FAULTSCREEN.md`](FAULTSCREEN.md)
  asserts it for every glyph).

## 2. The console

A grid of `width / FONT_WIDTH` by `height / FONT_HEIGHT` cells on a surface
describing the framebuffer (160 by 100 at 1280×800; 80 by 60 at 640×480).

`KernelWriteString` writes every diagnostic to the text display, this console and
the serial port, and is the only routine that names an output device (numbers
included: formatting routines emit through it). Which console is visible depends on
the mode the loader chose; neither knows of the other.

| Character | Effect |
| --------- | ------ |
| LF | First column of the next row, scrolling at the bottom. |
| CR | First column of this row. |
| HT | To the next multiple of eight columns (so columns line up). |
| BS | One position back; **does not erase**; stops at the erase limit. |
| FF | Clear; cursor and erase limit to the top left; the whole surface marked changed. |

Other control characters draw the replacement box. The meanings, the erase limit
and the backspace rules are the text display's
([`../devices/DISPLAY.md`](../devices/DISPLAY.md)), kept identical so one
diagnostic path behaves alike on both.

**Crossing to the row above.** A backspace in column 0 lands just after the text of
the row above. The text display reads that column back from its cells; a
framebuffer has only pixels, so `ConsoleRowEnd` records, per row, the column where
its text ended when the cursor left it downward. The record cannot go stale: it is
written when a row is left and read when it is re-entered from below, and a row
shortened by erasure must be left again first. A value equal to the column count
means the row **wrapped** (no separator), and the cursor lands on its last
character. The record and the erase limit both move with a scroll. The record is
`.bss` with `CONSOLE_MAXIMUM_ROWS` (512) rows; a taller framebuffer keeps its
remainder black.

**Scrolling** is one `GraphicsBlit` of the surface onto itself upwards (the
overlapping case [`DRAWING.md`](DRAWING.md) orders the copy for), then a fill of the
new bottom row. It reads and writes the back buffer, not the framebuffer
([`COMPOSITOR.md`](COMPOSITOR.md)).

**The replay buffer.** The framebuffer can be mapped only once the kernel arena
exists, after nearly two thousand bytes of log. `ConsoleWriteCharacter` records
into a fixed 4 KiB `.bss` buffer until then, and `ConsoleInitialise` replays it;
overflow is dropped **and counted**, since a replay silently starting midway looks
like a boot that started midway. `ConsoleActive` is set before the replay loop and
the buffer is appended to only while it is false, so the replay cannot re-enter it.

**The screen has one owner.** The drawing self-tests paint figures on the same
framebuffer. The console wins unless the command line carries `graphics-figure`
(the GRUB entry *Oxys-OS (graphics figures)*), in which case it is not started and
the figures stay. It starts after the drawing tests so that it erases them.
`ConsoleReport` says which of "not started, figures requested" and "none, text
mode" applies.

## 3. Speed

The console is a quarter of a million pixel writes a second during boot, so three
paths are specialised (measured with `RDTSC`: the console fell from about 15 % of
the boot to about 4.5 %):

- **Word-wide stores** where the surface's `whole_words` holds (four-byte pixels,
  word-aligned base, pitch a multiple of four; the pitch condition keeps odd rows
  aligned). The byte loops remain and give identical results.
- **`GraphicsPatternBlock`**: an 8-pixel-wide block, each pixel one of two colours
  by a bit of a one-byte-per-row pattern, i.e. a glyph; one clip test per block
  instead of 64. The font table is used as it stands.
- **Opaque glyphs**, so each cell is written once, not filled and then drawn over.

Test surfaces are declared as `uint32_t` arrays: writing a `uint32_t` into a
`uint8_t` array is undefined behaviour however well it seems to work, while the
framebuffer mapping has no declared type and may be read back through `uint8_t`.

## 4. The desktop's face

The desktop draws its text in **Inter**, a typeface written elsewhere and used
under the SIL Open Font License 1.1 ([`../../LICENSING.md`](../../LICENSING.md),
Section 4); the boot log, the console and the fault screen keep the 8-by-8 face,
because they draw before the desktop exists and when the machine is failing, and
the less they depend upon the better.

- **Rendered at build preparation, not parsed at boot.** [`../../tools/face.sh`](../../tools/face.sh)
  renders the font once, with ImageMagick, into
  [`../../fonts/inter/face.h`](../../fonts/inter/face.h), which is committed. The
  kernel carries no font parser, and no build depends on ImageMagick.
- **Grid cells of five by eight units of the scale**
  (`SYSCALL_WINDOW_TEXT_ADVANCE` by `SYSCALL_WINDOW_TEXT_HEIGHT`), so a program
  lays columns of text out by multiplication. A square cell, the 8-by-8
  face's, left Inter's narrower letters standing apart; five units is near the
  proportion of a monospaced face. Each glyph is centred by its ink, and the few
  wider than the cell (`W`, `M`, `m`, `w`, `@`) are narrowed to fit rather than
  cut. The baseline is three-quarters down the cell and the size 0.82 of its
  height, which leaves room for descenders.
- **One byte of coverage a pixel**, drawn by `FaceDrawGlyph` in
  [`../../graphics/face.c`](../../graphics/face.c): the paper over the cell,
  then the ink blended at each pixel's coverage through `GraphicsBlendPixel`,
  whose channels are mixed apart. Both window titles and `window_text` draw with
  it.
- **Exact tables for scales one to four** (182 KiB), where a small glyph's shape
  is decided; **scales five to eight are sampled bilinearly** from the
  scale-four table, since magnifying a smooth glyph stays smooth and four more
  tables would cost half a mebibyte for sizes only a large label uses.
- **Proportional text** (`FaceDrawText`, titles and the flag of `window_text`)
  moves the pen by Inter's own advance, in sixteenths of a pixel so rounding
  does not accumulate, and places each glyph's cell so its first inked column
  falls where the font puts the ink; both are generated with the tables. The
  paper covers the run first and each glyph's ink is only blended, so a glyph
  that overhangs its advance does not erase its neighbour. A glyph narrowed to
  fit the grid's cell advances by the font's advance less what it was narrowed
  by.
- **A code outside `0x20` to `0x7E` draws a hollow box**, as the 8-by-8 face
  does, so an unmapped character is visible.

## Verification

`KernelVerifyConsole` in [`../../kernel/test/gfx/console.c`](../../kernel/test/gfx/console.c).
The face and glyph drawing are asserted on memory surfaces; control characters on
the live console, using only characters that draw nothing except where a landing
needs text above it (that text is written and erased again).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The font covers its first and last codes and neither neighbour. | Range and table drifted apart. |
| `FontGlyph` is never `NULL` (tested at `0x00` and `0xFF`); the box is not blank. | A dereference of `NULL`; unmapped text looking like spaces. |
| Space is blank; no glyph inks columns 6–7; exactly one glyph is blank. | Streaks between words; touching characters; an omitted glyph. |
| **No two glyphs are identical.** | A copy-and-paste drawing one letter as another. |
| `'A'` drawn at (4, 4) on a 16×16 surface matches its bytes, with the margin untouched. | Wrong bit or row order; mirrored glyphs. |
| An unset pixel keeps its background; an unmapped code lights pixels; a glyph off the surface does not wrap. | Glyphs filling their cell; blank unknowns; missing clip. |
| The console's extent fits the framebuffer and is not zero. | Rows past the mapping; division by zero. |
| CR returns to column 0 on the same row; HT goes 0 → 8 → 16; BS moves one. | CR as LF; tabs that stand still or add eight. |
| BS at the limit, and at column 0 with the limit there, does not move. | Erasing the prompt or the previous line. |
| BS at column 0 lands after the row above's text; three erasures reach column 0. | The cursor at the display's edge, erasing far from the text. |
| Into a full row, BS stops on its last character; erasing it returns to column 0 and then to that character. | A wrapped row treated as ended; a stray character left at the edge of the next prompt. |
| Inter's space covers nothing at every scale; its `H` is fully covered somewhere at every scale from two, the sampled scales included; nothing is covered outside the cell, beyond scale eight or outside the codes (`KernelVerifyWindows`). | A table index off by a row or a glyph; a sampler reading past its table. |
| A glyph drawn in a window leaves ink in the content (`KernelVerifyWindows`, `window-check`). | Text that draws nothing. |
| Grid text is five units a character; proportional `il` is narrower than the grid; drawing proportional text returns the width measuring it gave; transparent text keeps what lay beneath where it has no ink and inks where it has, and is refused on the grid; an unknown flag is refused (`KernelVerifyWindows`, `window-check`). | A measure that disagrees with the drawing, which centres every label wrongly; a name on the desktop standing on a stripe of paper. |

`FAULTSCREEN.md` asserts the fast paths against the slow ones. That the log is
legible, numbers included, is judged by eye
([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)). The log reports
the grid, characters written, rows scrolled, and bytes replayed:

```
Console: 160 by 100 characters of 8 by 8 pixels.
Console: 1903 bytes replayed from before the console existed.
```

## Limitations

1. ASCII only; everything else is a box.
2. The console's cell is fixed at 8×8, small on a large display.
3. No text cursor is drawn on the console.
4. No per-cell colour; `ConsoleSetColour` applies from then on.
5. A scroll changes the whole screen, so a whole screen is presented.
6. The picture comments beside the glyphs are unchecked.
7. The row-end record is the column where the cursor left the row: text rewritten
   after a CR shortens it. Nothing writes a CR without an LF.
8. No lock of its own: `KernelWriteString` holds one for a whole string, which is
   the right granularity. The fault screen takes none ([`FAULTSCREEN.md`](FAULTSCREEN.md)).
9. Proportional text has no kerning, and W, M, m, w and @ keep the narrowing
   the grid's cell needs in it too; at scale one (eight pixels high) text is soft.
10. That a glyph looks right is judged by eye; the self-test asserts coverage,
    not shape.
