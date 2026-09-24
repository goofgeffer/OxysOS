<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Text-Mode Display

**Phase**: sub-task 4.2 of [`../project/PLAN.md`](../project/PLAN.md); the first
routine is sub-task 1.7.
**Source**: [`../../drivers/vga/vga.c`](../../drivers/vga/vga.c),
[`../../kernel/include/oxys/dev/vga.h`](../../kernel/include/oxys/dev/vga.h).
**Specifications**: IBM VGA Technical Reference (mode 3; the Miscellaneous
Output, CRT controller and attribute controller registers); ANSI X3.4-1986 (the
control characters); ECMA-48 (CUU, CHA, ED, CUP).

The VGA colour text console: 80 by 25 cells at physical `0xB8000`. It is the
channel a person at the machine has, and the only one on a machine with no
serial line. The design aims at output a person can read and the machine can
check.

## 1. When it is displayed

The kernel asks GRUB for a linear framebuffer. If GRUB sets a graphics mode, this
driver's memory is no longer displayed; it keeps working and nothing appears.

| GRUB left the adapter in | The screen shows |
| ------------------------ | ---------------- |
| A text mode | This console. |
| A graphics mode | The graphical console ([`../design/CONSOLE.md`](../design/CONSOLE.md)), or the desktop. |

`KernelWriteString` writes to this driver, the graphical console and the serial
port unconditionally; neither console knows of the other. The two implement the
same control characters with the same meanings and the same erase limit, so one
diagnostic path behaves alike on both: Sections 5 and 6 specify both. The
framebuffer never maps this driver's memory, so one device is never mapped twice
with different memory types. The self-test is **skipped**, and says so, in a
graphics mode, where reading cells back means nothing.

## 2. The adapter's addresses

The Miscellaneous Output Register (written at `0x03C2`, read at `0x03CC`), bit 0
(I/O Address Select), says which register set is live:

| Quantity | Colour (bit 0 set) | Monochrome |
| -------- | ------------------ | ---------- |
| CRT controller index port | `0x03D4` | `0x03B4` |
| Input Status #1 | `0x03DA` | `0x03BA` |
| Text memory | `0xB8000` | `0xB0000` |

`VgaInitialise` reads it rather than assuming colour. One `IN` costs nothing; a
cursor written to a port nothing decodes stays at the top left, silently. Text
memory is reached through the direct map, `PhysicalToVirtual(0xB8000)`
([`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md)).

## 3. The cursor

The hardware cursor is a property of the CRT controller:

| Index | Register | Fields used |
| ----- | -------- | ----------- |
| `0x0A` | Cursor Start | Bits 0–4 first scan line; bit 5 turns the cursor off. |
| `0x0B` | Cursor End | Bits 0–4 last scan line (bits 5–6 are skew, untouched). |
| `0x0E`, `0x0F` | Cursor Location High, Low | The cell address. |

- **The shape is the firmware's.** `VgaInitialise` only clears bit 5. A shape from
  a table could be invisible on an adapter whose cells are shorter.
  `VgaSetCursorShape` changes it on request, refusing a first line below the last.
- **Only the fields written are replaced**, by read-modify-write, so bit 5 and the
  skew survive.
- **The location is read back** (`VgaHardwareCursorPosition`), which is how the
  self-test knows the controller is being addressed at all.

## 4. Attributes

Bits 0–3 of a cell's attribute are the foreground, bits 4–7 the background. Bit 7
means blink or bright background according to bit 3 of the Attribute Mode Control
Register (attribute index `0x10`); the kernel chooses bright backgrounds.

The attribute controller's address and data share port `0x03C0`, alternated by a
flip-flop that a read of Input Status #1 resets to "address". Two things are not
optional:

- **Bit 7 of the index written (Palette Address Source) stays set.** Clear, it
  disconnects the palette and blanks the display.
- **The write is read back.** If the flip-flop was wrong, some other register
  changed; `VgaSetBlinkEnabled` then restores the original and returns false.
  `VgaInitialise` carries on regardless: the cost is blinking backgrounds, not a
  reason to lose the console.

## 5. Control characters

`VgaPutCharacter` implements them as ANSI X3.4-1986 defines them:

| Character | Effect |
| --------- | ------ |
| LF `0x0A` | First column of the next row, scrolling at the bottom. |
| CR `0x0D` | First column of this row. |
| HT `0x09` | To the next multiple of eight columns. |
| BS `0x08` | One position back. **It does not erase.** |
| FF `0x0C` | New page: clear the screen; the position and the erase limit go to the top left. For the shell's `clear`; the diagnostic path sends ECMA-48 `ED 2` and `CUP` to the serial line instead. |

To erase, a caller writes `BS SP BS`, which erases on a serial terminal too; the
erasure is therefore the caller's composition, not the driver's.

## 6. Backspace across rows, and the erase limit

The driver cannot tell a row the user typed from one the kernel printed, and a
held backspace must not eat the boot log. So it keeps an **erase limit**, a
position the cursor never retreats past. `VgaSetEraseLimit` records the current
position; whoever reads input calls it where input begins (the echo loop after
its banner; `KernelMain` before starting the shell, whose line editor never
backspaces past its prompt anyway).

1. At or before the limit, a backspace does nothing.
2. Not in the first column: back one column.
3. In the first column: to the row above, **just after the text on that row**.
   Not column 79, where a short row was never written; not onto the last
   character, which the backspace has not reached. The backspace consumes the row
   separator and nothing else. A **full** row is the exception: it wrapped rather
   than ended, so there is no separator, and the cursor lands on its last
   character. The column is therefore computed from the cells, not remembered.
4. A movement that would pass the limit stops at it.

`VgaScroll` moves the limit up with the text it protects; if that text scrolls off
the top, the limit collapses to the origin. The graphical console keeps its own
record of each row's length, having no cells to read back.

**On the serial line**, a terminal will not cross rows on a backspace, so
`KernelEchoBackspace` drives the display first and tells the serial line what
happened:

| The display | The serial line is sent |
| ----------- | ----------------------- |
| Did nothing (at the limit) | Nothing. |
| Moved back within the row | `BS SP BS`. |
| Crossed into the row above | CUU (`CSI A`), CHA (`CSI Pn G`) to the column, a space, CHA again. |

This agrees only with an 80-column terminal; the kernel cannot ask a terminal its
width.

## Verification

`KernelVerifyVga` in [`../../kernel/test/dev/devices.c`](../../kernel/test/dev/devices.c).
A display fails silently: an unhandled control character prints a glyph, a cursor
written to the wrong index stays put, a scroll by two eats a line. Each assertion
catches one such failure.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Colour configuration, CRT index at `0x03D4`. | Every register access going nowhere. |
| Blinking is disabled. | The flip-flop landing the write elsewhere. |
| LF, CR and HT move as defined. | A control character printed as a glyph. |
| The erase limit is recorded; a backspace at it does not move. | Backspace walking back through the boot log. |
| A backspace elsewhere retreats one column; `BS SP BS` erases and restores. | No movement; a character left standing. |
| A backspace in column 0 goes to just after the row above's text, which stays intact. | Landing on column 79, or eating a character with the separator. |
| Erasing across the boundary stops at the limit. | An off-by-one into kernel output. |
| The hardware cursor is where the driver thinks; an off-screen position is refused. | Wrong index or port; a write past text memory. |
| Hiding the cursor is visible in the controller and reversible; an impossible shape is refused. | Bit 5 confused with a scan line. |
| A scroll moves contents up exactly one row and blanks the last. | A two-row scroll; a duplicated last row. |

The self-test cannot know that anything is legible on a monitor; that is checked
by typing at the machine ([`../project/TESTING.md`](../project/TESTING.md)). The
log reports:

```
Display adapter: colour configuration, registers at 0x3D4, 80 by 25 characters.
Display adapter: cursor displayed, attribute bit 7 selects a bright background.
```

## Limitations

1. Mode 3 only, as the firmware left it; the driver cannot set a mode, so it
   cannot return to text from a graphics mode GRUB set.
2. The character generator is the firmware's (code page 437 in practice).
3. The palette is the firmware's sixteen colours.
4. No lock; only the diagnostic path writes, and the panic path does not return.
5. One erase limit.
6. No scrollback; the serial log is the record.
7. In a graphics mode, writes go to memory nothing displays, undetected.
