# The Text-Mode Display

**Phase**: 4, sub-task 4.2, of [`PLAN.md`](../project/PLAN.md). The routine upon which this
driver was built belongs to Phase 1, sub-task 1.8. Sub-task 6.2 displaced it; see
Section 1.1.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../drivers/vga/vga.c`](../../drivers/vga/vga.c),
[`../kernel/include/oxys/vga.h`](../../kernel/include/oxys/vga.h).

## 1. What the display is, and what it is for

The display is the console of the machine the kernel is running upon. It is not
the channel the automated tests read — that is the serial adapter, described in
[`SERIAL.md`](SERIAL.md) — and it is not the channel a panic can most be relied
upon to reach. It is the channel a person looking at the machine has, and it is
the only one available upon a machine with no serial adapter attached to
anything.

That distinction governs the whole of the design. The serial driver is written so
that a machine in any condition can report; the display driver is written so that
what it reports can be read, and so that the machine can check that what it
displayed is what it meant to display. The second is harder than it sounds, and
Section 8 explains why.

### 1.1 What sub-task 6.2 did to that

**Since sub-task 6.2 the statement above is conditional.** The kernel asks the
boot loader for a linear framebuffer, and a boot loader that supplies one sets a
graphics mode to do it. In a graphics mode the memory this driver writes to is
not the text buffer, so the driver goes on working correctly and **nothing it
writes appears**.

Nothing here has changed and nothing here is broken. What changed is which device
is displaying the screen. The position is:

| The boot loader left the adapter in | This driver | The screen shows |
| ----------------------------------- | ----------- | ---------------- |
| A text mode | Displays the console, exactly as below | The boot log |
| A graphics mode | Writes to memory nothing displays | The same boot log, drawn by the graphical console of sub-task 6.4 — or the drawing figures, where the command line asked for them |

One consequence follows, and it is recorded rather than worked around.

The self-test of Section 8 is **skipped** in a graphics mode, and says so. Every
assertion it makes reads a character cell back out of the text buffer, and in a
graphics mode none of them means anything; a test that quietly asserted less
would be worse than one that states plainly that it asserted nothing. It also had
to move later in the boot, since which mode the machine is in is stated by the
Multiboot2 boot information and nowhere else.

**It was temporary, and sub-task 6.4 ended it.** There is now a console that
draws text upon a framebuffer, so the operator's channel has returned through
different machinery. `KernelWriteString` writes to this driver, to that console
and to the serial port, unconditionally and without deciding between them; which
of the first two the operator can see is decided by the mode the boot loader left
the adapter in, and neither knows about the other.

The two are deliberately alike where it counts. The console implements the same
four control characters this driver does, with the same meanings and the same
erase limit, so that one diagnostic path does not behave differently upon two
displays — Sections 6 and 7 below are the specification of both. See
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), Section 19.

What was lost between sub-tasks 6.2 and 6.4 was the screen of a machine with no
serial adapter, and under VirtualBox — where this kernel detects none — that was
every readable line; `../project/TESTING.md`, Section 9.1, records it.

The framebuffer that displaced this driver never maps the text buffer, even where
the boot loader reports one. That memory is this driver's, and two mappings of one
device with different memory types and nothing to decide between them would be
worse than either. See [`../design/GRAPHICS.md`](../design/GRAPHICS.md),
Section 7.

## 2. The mode, and the memory it occupies

The mode is the standard VGA colour text mode, mode 3: eighty columns by
twenty-five rows, each cell two bytes, the code point first and the attribute
second, the whole beginning at physical address `0x000B8000` (IBM VGA technical
reference). The buffer is reached at `PhysicalToVirtual(0xB8000)`, the boot-time
paging hierarchy mapping the first gibibyte of physical memory into the higher
half; see [`MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md).

## 3. Which registers the adapter answers upon

The adapter presents two register sets at two addresses, and which of the two is
live is a matter of configuration rather than of assumption. The Miscellaneous
Output Register, written at `0x03C2` and read at `0x03CC`, has as its bit 0 the
I/O Address Select: "If set Color Emulation. Base Address=3Dxh else Mono
Emulation. Base Address=3Bxh".

`VgaInitialise` reads that register and derives from it three things:

| Quantity | Colour configuration | Monochrome configuration |
| -------- | -------------------- | ------------------------ |
| CRT controller index port | `0x03D4` | `0x03B4` |
| Input Status #1 Register | `0x03DA` | `0x03BA` |
| Frame buffer | `0x000B8000` | `0x000B0000` |

No machine this kernel is expected to run upon will answer in the monochrome
configuration. The reason to establish it rather than assume it is that the cost
of being wrong is entirely disproportionate to the cost of one `IN` instruction:
a driver that wrote the cursor location to `0x03D4` upon an adapter decoding
`0x03B4` would leave the cursor stationary at the top left of the screen and
report nothing at all about it. That failure is invisible to the machine, which
is the class of failure this driver is built to eliminate.

## 4. The cursor

The hardware cursor is the block or underline the adapter draws over a cell. It
is not the driver's record of where the next character goes; it is a property of
the CRT controller, which must be told. Four registers are involved, all reached
through the index and data port pair:

| Index | Register | Fields used |
| ----- | -------- | ----------- |
| `0x0A` | Cursor Start | Bits 0 to 4, the first scan line of the cursor within the character cell; bit 5, which "Turns Cursor off if set". |
| `0x0B` | Cursor End | Bits 0 to 4, the last scan line. Bits 5 and 6 are the cursor skew, a delay expressed in character clocks, and are not the driver's business. |
| `0x0E` | Cursor Location High | Bits 15 to 8 of the cursor address. |
| `0x0F` | Cursor Location Low | Bits 7 to 0 of the cursor address. |

Three consequences are worth stating.

**The shape is left as the firmware set it.** `VgaInitialise` asserts only that
the cursor is displayed, by clearing bit 5 of the Cursor Start Register. The scan
lines are not touched, because the shape the firmware chose is the shape known to
be legible upon that machine's own display, and a driver that imposed a range of
scan lines from a table would present an invisible cursor upon an adapter whose
character cell is shorter than the table assumed. `VgaSetCursorShape` exists for
a caller that wants a different shape, and refuses a first scan line below the
last, which would present no cursor at all.

**Only the fields being written are replaced.** `VgaSetCursorShape` reads each
register, masks in bits 0 to 4 and writes the result back, so that it cannot
disable the cursor by accident through bit 5, nor alter the skew.

**The controller can be read back.** `VgaHardwareCursorPosition` reads the two
location registers and reconstructs the position, which is what Section 8 uses to
establish that the driver is addressing the controller at all.

## 5. The attributes, and the sixteenth background colour

The attribute byte holds a four-bit foreground index in bits 0 to 3 and a
background index in bits 4 to 7. Bit 7 is ambiguous by design: in the Attribute
Mode Control Register (index `0x10` of the attribute controller), bit 3 set makes
"Attribute bit 7 ... blinking", and clear makes it "high intensity". The kernel
clears it. A kernel has no use whatever for blinking text, and a real use for the
eight bright background colours that the same bit buys.

Reaching that register is the most awkward access in this driver, and the reason
is historical. The attribute controller's address and data registers share port
`0x03C0`: "The address register is read and written via port 3C0h. The data
register is written to port 3C0h and read from port 3C1h", and an internal
flip-flop decides which of the two a write lands in. The flip-flop is returned to
the address by a read of the Input Status #1 Register, "the data received is not
important". Two further points are not optional:

- Bit 7 of the value written to `0x03C0` is the Palette Address Source, which "is
  set to 0 to load color values to the registers in the internal palette. It is
  set to 1 for normal operation." Writing an index with that bit clear
  disconnects the palette and blanks the display. `VgaWriteAttribute` therefore
  sets it in the index it writes, and leaves it set afterwards.
- The write is read back. A write that arrived while the flip-flop stood at the
  data register would have altered some other register entirely, and the only
  symptom would be a display that a person found wrong. `VgaSetBlinkEnabled`
  returns false and restores the original value if the read-back disagrees.

The failure of that read-back is not treated as fatal by `VgaInitialise`. The
consequence of it is that the eight bright backgrounds blink instead of being
bright, and a display driver that refused to start over such a thing would
deprive the machine of its console to no purpose.

## 6. The control characters

`VgaPutCharacter` implements four control characters, and implements them as
ANSI X3.4-1986 defines them and not as a caller might wish they behaved:

| Character | Effect |
| --------- | ------ |
| LF (`0x0A`) | The active position moves to the first column of the following row, scrolling if it stood upon the last. |
| CR (`0x0D`) | The active position moves to the first column of the current row. |
| HT (`0x09`) | The active position advances to the next multiple of eight columns — to a multiple of eight, not by eight. |
| BS (`0x08`) | The active position moves one position backward. It does not erase. |

The last of these deserves its own section.

## 7. The backspace, and how far back it may go

**The backspace does not erase.** ANSI X3.4-1986 defines it as a movement of the
active position one character position backward, and no more. A caller that means
to erase writes the three-character sequence `BS SP BS`, which steps back, writes
a space over the character and steps back again to stand where the character was.
That sequence erases upon a serial terminal equally, which is why the erasure is
composed by the caller rather than performed by the driver: the two devices would
otherwise need different treatment for the same keystroke.

**The position preceding the first column of a row is the end of the row above.**
This is the part that was previously not implemented. A backspace in the first
column formerly did nothing at all, on the stated grounds that the driver could
not tell a row the user had typed from a row the kernel had printed, and would
otherwise consume the boot log a character at a time.

The objection was sound and the remedy was to supply the missing knowledge rather
than to refuse the movement. The driver now holds an **erase limit**: a position
before which the cursor will not retreat under any backspace. `VgaSetEraseLimit`
records the current cursor position as that limit, and whoever is reading input
calls it where the input is to begin — `KernelEchoLoop` does so immediately after
printing its banner. Everything before the limit is the kernel's and is
unreachable; everything after it is the user's and may be erased.

Within that constraint the backspace behaves as the standard describes:

1. If the cursor stands at or before the limit, nothing happens.
2. If the cursor is not in the first column, it retreats one column.
3. Otherwise it moves to the row above, to the column **immediately after the
   text standing upon that row**. Not to the eightieth column, which a short row
   was never written to and where an erasure would consume a space and leave the
   text untouched; and not onto the last character of the text, which the
   backspace has not reached. A backspace at the beginning of a row consumes the
   separator between the two rows and nothing else, just as a backspace within a
   row consumes one character and nothing else. Pressing it again erases the
   character now standing before the cursor, in the ordinary way.

   A row that is entirely occupied is the exception, and is why the column is
   computed from the contents of the display rather than remembered. Such a row
   did not end because a line feed was written; it ended because the text
   wrapped, and there is no separator between it and the row below to consume.
   The cursor therefore stops upon its final character, which the same backspace
   goes on to erase.
4. If either movement would carry the cursor past the limit — the limit standing
   in the middle of a row it shares with a prompt — the cursor is placed at the
   limit instead.

`VgaScroll` moves the limit up with the text it protects. Where the limit stood
upon the first row, the text it protected has left the display altogether, and
the limit collapses to the origin: nothing that remains was written before it.

### 7.1 The same correction upon a serial terminal

A terminal at the far end of a serial line will not cross a line boundary upon
receiving a backspace, so the two devices would diverge the moment the display
did. `KernelEchoBackspace` drives the display first and lets the outcome decide
what the serial line is told:

| What the display did | What is sent to the serial line |
| -------------------- | ------------------------------- |
| Nothing; the cursor stood at the erase limit. | Nothing. |
| Retreated within the row. | `BS SP BS`. |
| Crossed into the row above. | ECMA-48 CUU (`CSI A`), then CHA (`CSI Pn G`) to the column, a space, then CHA again. |

The two agree only so far as the terminal is eighty columns wide, which the
kernel has no way to ask it. A terminal of another width will have wrapped the
line elsewhere and the correction will land upon the wrong column of it. The
proper remedy is a line discipline that knows the width of its terminal, and it
belongs to Phase 8.

## 8. Verification

### 8.1 Why a display is unusually hard to test

Every other driver in the kernel fails in ways the machine can notice. A display
does not. A control character for which the driver has no case is written into
the frame buffer as whatever glyph the font holds at that code point, and the
cursor advances to the right; a cursor location written to the wrong register
index leaves the cursor where it was; a scroll by two rows instead of one eats a
line of output. In each case the machine runs perfectly and the display is wrong,
and the defect is visible only to somebody reading the screen. That is exactly
how the backspace came to be broken and to stay broken.

`KernelVerifyVga` exists to close that gap, and every property it asserts is
chosen because its failure would otherwise be silent.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The adapter is in its colour configuration and the CRTC index port is `0x03D4`. | Every register access below going to an address nothing decodes. |
| Blinking is disabled. | The attribute write landing in the wrong register through the flip-flop; bright backgrounds blinking. |
| LF returns to the first column; CR does likewise without changing the row; HT advances to a multiple of eight. | A control character being printed as a glyph. |
| The erase limit is recorded where the cursor stood. | A backspace that can walk back through the whole boot log. |
| A backspace at the limit does not move. | The same. |
| A backspace elsewhere retreats by one column. | The defect this sub-task's predecessor fixed. |
| `BS SP BS` erases the cell and restores the cursor. | An erasure that moves the cursor but leaves the character. |
| A backspace in the first column crosses to the position after the text of the row above, leaving that text intact. | The movement not happening; landing upon column 79; or consuming a character along with the separator, so that one keystroke deletes two things. |
| Erasing across the boundary and then continuing stops at the limit. | An off-by-one at the boundary that steps one character into the kernel's output. |
| The hardware cursor holds the position the driver believes it holds. | The cursor location written to the wrong index, or to the wrong port pair. |
| A cursor position outside the display is refused. | A write past the end of the frame buffer. |
| Hiding the cursor is observable in the controller and is reversible. | Bit 5 of the Cursor Start Register being confused with a scan line. |
| An impossible cursor shape is refused. | A first scan line below the last, presenting no cursor. |
| A scroll moves the contents up exactly one row and blanks the last. | A scroll that eats two rows, or that leaves the final row holding a copy of the penultimate one. |

The scroll assertion is made upon the contents of the frame buffer, read back
through `VgaCharacterAt`. It costs one row of the boot log, which leaves the
display for the purpose; the record upon the serial line is unaffected, and that
is the record `make verify` reads.

### 8.2 What the self-test cannot establish

It cannot establish that anything is legible. The frame buffer holding the code
point `0x41` at the cell the driver believes the cursor to be at is the whole of
what the machine can know; whether a letter A appeared upon a monitor is beyond
it. The end-to-end evidence is therefore the same as for the keyboard: a person,
or a virtual machine monitor, typing at the echo loop.

The backspace across a row boundary was exercised both ways for this sub-task:
characters delivered over COM1, and the same characters delivered as scan codes
by the QEMU monitor's `sendkey`. Both produced the identical echo, recorded in
[`TESTING.md`](../project/TESTING.md).

### 8.3 Observed state

`VgaReport` prints the configuration and the accounting at the end of
initialisation:

```
Display adapter: colour configuration, registers at 0x3D4, 80 by 25 characters.
Display adapter: cursor displayed, attribute bit 7 selects a bright background.
Display adapter: written 3344, scrolled 49, cursor at row 24, column 69.
```

## 9. Limitations

1. **Mode 3 only.** The driver does not set the mode; it uses the one the
   firmware left. Setting a mode requires programming the sequencer, the CRT
   controller timing registers, the graphics controller and the attribute
   controller as a set, and nothing in this kernel yet wants a different mode.
2. **No font control.** The character generator is left as the firmware loaded
   it, so the glyph a code point produces is code page 437 upon every machine of
   interest and is not guaranteed.
3. **No colour beyond the sixteen.** The internal palette is not reprogrammed;
   the sixteen indices mean what the firmware says they mean.
4. **No concurrency safety.** The driver has no lock. There is no second thread
   of control that writes to it: the interrupt handlers do not print, save
   through the panic path, which does not return.
5. **A single erase limit.** There is one, not one per line and not a stack of
   them. It suffices for the echo loop and will not suffice for a line discipline
   serving several terminals, which is Phase 8's problem.
6. **No scrollback.** A row that leaves the top of the display is gone. The
   serial log is the record.
7. **The driver may not be the thing displaying the screen.** Since sub-task 6.2
   it writes to the text buffer whether or not the adapter is in a text mode, and
   in a graphics mode that memory is not displayed. It does not detect this and
   does not need to — the writes are harmless and become visible again if a text
   mode is restored — but nothing here should be read as a promise that what it
   writes can be seen. Section 1.1.
8. **The driver cannot set a mode, so it cannot undo this.** Returning to text
   from a graphics mode set by the boot loader means programming the sequencer,
   the CRT controller, the graphics controller and the attribute controller as a
   set, or a VESA BIOS call from real mode, which a long-mode kernel has no means
   of making. Limitation 1 and this one are the same limitation seen from two
   sides.
