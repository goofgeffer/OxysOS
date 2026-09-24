<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Font and the Graphical Console

**Phase**: sub-task 6.4 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../graphics/font.c`](../../graphics/font.c),
[`../../graphics/console.c`](../../graphics/console.c), and their headers in
[`../../kernel/include/oxys/gfx/`](../../kernel/include/oxys/gfx/).
**Specifications**: ANSI X3.4-1986 (the printable range and the control
characters).

The bitmap face drawn for this project, and the console that draws the boot log
with it on the framebuffer. The same face is the one programs draw text with
([`SESSION.md`](SESSION.md)); the high-resolution mark and icons are separate
([`SESSION.md`](SESSION.md)).

## 1. The face

Ninety-five glyphs, `0x20` to `0x7E`, compiled into the image.

- **Drawn for this project**, pixel by pixel. `PROJECT_GUIDELINES.md`, Section 2,
  forbids transcription, and the code page 437 face is easy to lift unnoticed.
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
2. The cell is fixed at 8×8, small on a large display.
3. No text cursor is drawn on the console.
4. No per-cell colour; `ConsoleSetColour` applies from then on.
5. A scroll changes the whole screen, so a whole screen is presented.
6. The picture comments beside the glyphs are unchecked.
7. The row-end record is the column where the cursor left the row: text rewritten
   after a CR shortens it. Nothing writes a CR without an LF.
8. No lock of its own: `KernelWriteString` holds one for a whole string, which is
   the right granularity. The fault screen takes none ([`FAULTSCREEN.md`](FAULTSCREEN.md)).
