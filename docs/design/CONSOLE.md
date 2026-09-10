# The Font and the Console

**Corresponding phase**: 6, sub-task 6.4 — the bitmap face drawn for this
project, the graphical console drawn with it, and the measurement that made that
console fast enough to keep.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.

**Implemented by**: [`../../graphics/font.c`](../../graphics/font.c),
[`../../graphics/console.c`](../../graphics/console.c),
[`../../kernel/include/oxys/font.h`](../../kernel/include/oxys/font.h),
[`../../kernel/include/oxys/console.h`](../../kernel/include/oxys/console.h).

**Asserted by**: `KernelVerifyConsole` in
[`../../kernel/test/verify_console.c`](../../kernel/test/verify_console.c). The
word path of Section 6 is additionally asserted by `KernelVerifyGraphics`, and
those two assertions are recorded in
[`FAULTSCREEN.md`](FAULTSCREEN.md), Sections 2.1 and 2.2, with the run they were
written for.

**Specifications**: ANSI X3.4-1986, the printable range and the four control
characters the console interprets.

**Where this sits**: the third of the five documents the graphical work of
sub-tasks 6.2 to 6.6 is divided into. [`GRAPHICS.md`](GRAPHICS.md) is the index
and records why they are five.

---

## 1. The face (sub-task 6.4)

A framebuffer is pixels, and a boot log is text. Something has to turn one into
the other, and this is it: ninety-five glyphs covering the printable range of
ANSI X3.4-1986, `0x20` to `0x7E`, compiled into the image as `graphics/font.c`.

### 1.1 Why it was drawn rather than obtained

`PROJECT_GUIDELINES.md`, Section 2, prohibits transcribing anybody else's source,
and a font is exactly the kind of asset that is easy to lift and hard to notice
having lifted. The IBM code page 437 face is in a hundred repositories and would
have been quicker to copy than to draw. It was drawn instead, pixel by pixel, and
that is why the face is a plain one.

The alternative that would have been original — reading the font the firmware
loaded into plane 2 of the VGA character generator, which is the machine's own
data rather than anybody's source — was considered and rejected for two reasons.
The boot loader has already set a graphics mode by the time this kernel runs, so
the planes may no longer hold it; and Phase 12 boots under UEFI, where there is
no VGA character generator at all. A font compiled into the image works in both
cases and in every case after them.

### 1.2 The cell, and why it is eight wide

Eight columns by eight rows. The width is eight because a row of a glyph is then
exactly one byte: a wider cell needs either two bytes to the row or a bit field
spanning bytes, and neither buys a console anything.

The metrics the whole table is drawn to:

| | |
| --- | --- |
| Ink | columns 0 to 5 |
| Spacing | columns 6 and 7, always clear |
| Capitals and digits | rows 0 to 6, baseline at row 6 |
| Lowercase | rows 2 to 6 |
| Descenders (`g j p q y`) | reach row 7 |

The two clear columns are the whole of the spacing between one character and the
next. A console therefore draws at a stride of exactly `FONT_WIDTH` and has no
gap of its own to manage, and a glyph that used those columns would touch the
character beside it. That is an assertion, not a convention; see Section 4.

Each glyph is eight bytes, one to a row, **most significant bit leftmost**, so a
row of pixels reads left to right exactly as the picture comment beside it is
drawn. The pictures are what make the table legible and they are comments:
nothing checks that a picture agrees with the bytes beside it. Change one and you
must change the other. The self-test asserts the bytes, and no test can tell you
that a letter looks wrong.

### 1.3 The replacement glyph

Every code outside the covered range yields a hollow box rather than nothing.
`FontGlyph` is therefore never `NULL` and its caller dereferences it without
checking, which is the point of it.

A font that drew a blank for a code it did not know would make a run of unmapped
characters indistinguishable from a run of spaces, and the fault would read as
missing output rather than as an unmapped character. The console relies upon this
for the control characters it does not implement: one that nothing meant to emit
appears as a box in the log, where it can be found.

### 1.4 Drawing, and what is not drawn

`FontDrawGlyph` sets only the pixels the glyph defines and leaves the rest of the
cell as it was. The background is the caller's business — a console fills the
cell before calling this; a caller drawing text over an image does not, and gets
the character stencilled upon what was already there. Drawing the background here
would be one pass instead of two and would make it impossible to draw a character
over anything, which is what a cursor does and what sub-task 6.6 duly wanted.

Every pixel goes through `GraphicsPutPixel`, so every pixel is clipped by
[`DRAWING.md`](DRAWING.md), Section 3's boundary. A glyph at the edge of a surface is cut off rather than
wrapped or refused, and one wholly outside the clip costs sixty-four rejected
writes and touches nothing. That is the slow way to draw text and it is the
correct one; the fast way needs a clipped span, which is sub-task 6.6's problem
when there is something to measure.

## 2. The console

`graphics/console.c` is a grid of character cells drawn upon the framebuffer:
`width / FONT_WIDTH` columns by `height / FONT_HEIGHT` rows, taken from a
`GraphicsSurface` describing the framebuffer, and nothing else. It is what ends
the cost Section 7 recorded — that for two sub-tasks the screen showed a test
pattern and the boot log went to the serial port alone.

It does not supersede the text-mode driver of sub-task 4.2. `KernelWriteString`
writes to the display, the console and the serial port unconditionally, and which
of the first two the operator can see is decided by the mode the boot loader left
the adapter in. Deciding between them in that routine would put knowledge of the
display mode in the one function that must work before anything has established
what the mode is.

### 2.1 The fault that made the fan-out one place

`KernelWriteHexadecimal` and `KernelWriteDecimal` named `VgaWriteString` and
`SerialWriteString` themselves until this sub-task. The console was therefore
shown every word of the boot log and not one of its numbers — every address,
count and size simply absent from the screen while the same lines on the serial
port were complete.

It read as a formatting error in the messages rather than as a missing output
path, which is why it is written down. Both now emit through `KernelWriteString`,
and that is the only routine in the kernel permitted to name an output device.

### 2.2 The control characters

The four of ANSI X3.4-1986 that the text-mode driver implements, given the same
meanings deliberately, so that one diagnostic path does not behave differently
upon two displays. `docs/devices/DISPLAY.md`, Sections 6 and 7, is the other half
of this.

| | |
| --- | --- |
| LF | to the first column of the following row, scrolling upon the last |
| CR | to the first column of the current row |
| HT | to the next multiple of eight columns — to a multiple, not by eight |
| BS | one position backward; **does not erase**, and will not pass the erase limit |

The tabulation is the one worth stating twice. Columns of text separated by
tabulations line up only if every one of them lands upon the same grid, whatever
the length of what preceded it; advancing by eight lines nothing up with
anything.

The backspace does not erase because an erasure is composed by its caller from
backspace, space, backspace — which is what it must be upon a serial terminal, so
it is what it is here. The erase limit is the position a backspace may not
retreat past, and it exists for the reason the text-mode driver has one: an echo
loop must not let a person backspace over the prompt, or over output the kernel
wrote and they did not type.

**The limit moves with a scroll.** A limit left at a fixed row would come to mark
a different character once the text beneath it had moved, and a backspace would
then be permitted to erase output it was meant to protect — or refused where it
should have been allowed.

Every other control character is drawn as the replacement glyph rather than
discarded, per Section 1.3.

#### 2.2.1 Crossing to the row above, and the record that makes it possible

A backspace standing in the first column of a row consumes the separator between
that row and the one above, and the cursor belongs **after the text of the row
above** — where the next character written upon that row would go. It does not
belong at the right-hand edge of the display.

The edge is where this console put it until sub-task 6.4 was corrected, and the
consequence was not subtle. The caller composes an erasure from backspace, space,
backspace; with the cursor at the far edge the space was written a hundred and
fifty columns away from the text, so a person pressing backspace over a line
separator saw the characters they meant to delete stay exactly where they were,
and the cursor stride away from them. Backspacing within a line worked, which is
what made it look like a fault in the crossing rather than in the landing.

The text-mode driver answers the same question by reading the characters back out
of text memory — `VgaFirstFreeColumn` scans the row for its last non-blank cell.
A console drawn upon a framebuffer has no characters to read back, only pixels,
so what text memory would have told it is recorded as it goes: `ConsoleRowEnd`
holds, for each row, the column at which that row's text ended when the cursor
last left it downward.

The record cannot go stale, and the reason is the order in which it is used. It
is written when a row is left and read when a row is re-entered from below; a row
whose text was shortened by an erasure must be left again before it can be
re-entered again, and the leaving rewrites the entry.

**A row filled to its last column is the exception.** Such a row did not end
because a line feed was written but because the text wrapped, and there is no
separator between it and the row below to consume. The cursor therefore stops
upon the final character, which the same backspace goes on to erase. This is why
the value stored may be the column count itself: the count says "wrapped" and any
lesser value says "ended", and the two want different landings.

**The record moves with a scroll**, for the same reason the erase limit does. A
row length left describing a fixed row would describe different text once the
text beneath it had moved.

The record is `.bss`, so it has a fixed size, and `CONSOLE_MAXIMUM_ROWS` is that
size: 512 rows of an eight-pixel face is a display 4096 pixels tall. A taller
framebuffer is not refused — the console occupies the topmost rows of it and
leaves the remainder black, which is a display that is short rather than a
console that is wrong.

### 2.3 The scroll, which is the blit paying for itself

Scrolling is one `GraphicsBlit` of the surface upon itself, followed by a fill of
the row exposed at the bottom.

This is the overlapping case that [`DRAWING.md`](DRAWING.md), Section 5, chose a copy direction for. The
destination lies above the source, so the rows are taken from the top and nothing
reads a byte the copy has already overwritten. A console scroll is the reason
that direction logic was written, and this is the whole of the payment.

### 2.4 The replay buffer

The framebuffer cannot be acquired until the kernel virtual arena exists, because
the mapping comes out of it, and by then some nineteen hundred bytes of boot log
have already been written. A console started at that point would begin part way
through the boot, and the messages it dropped — the handover, the memory map —
are exactly the ones worth seeing when a machine will not boot.

So `ConsoleWriteCharacter` records into a fixed 4 KiB `.bss` buffer until there is
something to draw upon, and `ConsoleInitialise` replays it. The capacity is fixed
and what does not fit is dropped **with the drop counted and reported**: a replay
that silently began part way through would look exactly like a boot that began
part way through. A buffer that reallocated would need the heap, which does not
exist that early either.

The replay cannot re-enter the buffer, and the reason is the order of two
statements rather than a flag: `ConsoleActive` is set true before the replay loop,
and the buffer is appended to only while it is false.

### 2.5 The screen has one owner

The console clears the framebuffer and replays the log over it. The figures the
self-tests of sub-tasks 6.2 and 6.3 paint live on that same framebuffer. They
cannot both have it.

The console wins by default, because a screen is for reading. The figures are
drawn only when the boot loader's command line carries `graphics-figure`, and the
console is then not started at all — whoever wants to look at the figures asks for
them and gives up the log for that boot. `boot/grub/grub.cfg` carries a second
entry, **Oxys-OS (graphics figures)**, that passes it.

This is also why the console is started *after* the drawing self-tests rather
than before: started first, it would be drawn over by the figures and the log
would be unreadable. Started after, it erases them.

`ConsoleReport` distinguishes the two silences accordingly. "Not started; the
command line asked for the drawing figures" is not the same statement as "none;
the adapter is in a text mode", and reporting the second in both cases would have
the kernel deny having a framebuffer three lines after describing one in detail.

## 3. Limitations of the font and the console

1. **The face covers ASCII and nothing else.** No accented letters, no box
   drawing, no code point above `0x7E`. Everything outside the range is a hollow
   box. A wider repertoire is a larger table and a different lookup, and nothing
   yet emits anything outside it.
2. **The cell is fixed at eight by eight** and cannot be scaled. At 1280 by 800
   that is 160 by 100 characters, which is small on a large display; a doubled
   cell is a different drawing routine, not a parameter.
3. **There is no text cursor drawn.** The position is tracked and reported and
   nothing marks it upon the screen. Sub-task 6.6 supplied the compositing needed
   to remove one again — a cursor is a layer — but nothing yet has a use for it:
   the echo loop is legible without, and a shell is Phase 8. See [`COMPOSITOR.md`](COMPOSITOR.md), Section 2.7.
4. **There are no colours per character.** `ConsoleSetColour` sets the pair used
   from that point onward; there is no attribute stored with a cell, so a scroll
   cannot repaint what it moved and does not need to.
5. **A scroll still moves the whole screen**, but no longer through the
   write-combining mapping: since sub-task 6.6 the blit reads and writes the back
   buffer, which is ordinary cached memory, and only the result is carried out.
   The damage of a scroll is everything, so a screenful is presented; the
   dirty-region scheme of [`COMPOSITOR.md`](COMPOSITOR.md), Section 2.3, narrows every *other* operation to the
   cells it touched.
6. ~~**Nothing is buffered off-screen.**~~ Resolved by sub-task 6.6. Drawing goes
   to the back buffer and reaches the display only at a presentation. There is
   still no synchronisation with the adapter's vertical blank; see [`COMPOSITOR.md`](COMPOSITOR.md), Section 2.7.
7. **There is no lock**, and no second thread of control writes here: the
   interrupt handlers do not print save through the panic path, which does not
   return. From sub-task 6.14 that ceases to be true and this must take the lock
   sub-task 6.13 built — the whole of a character, not one pixel of it, being the
   thing that must not interleave. The lock exists; it is not taken here.
8. **The picture comments are unchecked.** Nothing asserts that the art beside a
   glyph agrees with its bytes. See Section 1.2.
9. **The console is at most `CONSOLE_MAXIMUM_ROWS` rows tall.** The row lengths
   of Section 2.2.1 are `.bss` and therefore of a fixed size. Five hundred and
   twelve rows is a display 4096 pixels tall, beyond anything a boot loader
   hands this kernel; a taller framebuffer keeps its remainder black rather than
   being refused.
10. **A carriage return does not restore the row's recorded length.** The length
   is the column at which the cursor left the row, so text overwritten after a
   carriage return shortens what a later backspace will cross up to, where the
   text-mode driver — which scans the characters themselves — would find the
   longer text still standing. Nothing in this kernel writes a carriage return
   without a line feed after it, and a line discipline that did belongs with
   Phase 8.

## 4. Verification of the font and the console

`KernelVerifyConsole` makes thirty-three assertions in three groups. The font and
its drawing are asserted against a surface composed in memory, as the primitives
of [`DRAWING.md`](DRAWING.md), Section 6, are, so that the whole of that holds upon a machine with no display.
The control characters are asserted upon the live console, because the position
they move is the console's own and there is no second one to make.

### 4.1 The face

These are assertions upon a table authored by hand, which is exactly why they are
worth making: a font is data, so the compiler checks nothing about it, and the
plausible faults all produce a font that is merely wrong to look at.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| The font covers its first and last code and neither neighbour | The range and the table have drifted apart; the last glyph is unreachable or one past the end is read. |
| `FontGlyph` is never `NULL`, for `0x00` and `0xFF` | The drawing routine dereferences it without checking. This is the assertion that it need not. |
| The replacement glyph is **not** blank | A run of unmapped characters would be indistinguishable from a run of spaces, and the fault would read as missing output. |
| The space **is** blank | Ink in the space streaks every gap between words — the one glyph whose fault is visible everywhere at once. |
| No glyph draws into columns 6 or 7 | It touches the character beside it, and a console drawing at a stride of the cell width has nowhere to put a gap of its own. |
| Exactly one glyph is blank | A glyph omitted from the table is a blank cell where a character should be, and is otherwise reported by nothing. |
| **No two glyphs are identical** | What a copy-and-paste leaves behind. It is invisible in a picture comment that was pasted along with it, and its consequence is that one letter is silently drawn as another — which a reader reads straight past. |

### 4.2 Drawing a glyph

A 16 by 16 surface with the glyph drawn at (4, 4), so that ink escaping the cell
in any direction lands in the margin rather than off the surface, where clipping
would hide it from the assertion.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Every pixel of `'A'` matches the glyph's own bytes | The bit order or the row order is wrong. |
| The margin around the cell is untouched | Reversed bit order would still light eight pixels a row and would still be a picture — merely a mirrored one, which is invisible in the symmetric letters. This is what catches it. |
| A pixel the glyph does not set keeps the background it was given | The glyph filled its own cell, and text can no longer be drawn over an image. |
| An unmapped code lights pixels | It drew nothing rather than the replacement glyph. |
| A glyph drawn off the surface does not appear on the far side of it | The clip was applied to the cell rather than to each pixel, or not at all. |

### 4.3 The control characters, upon the live console

Only characters that draw nothing are used — CR, HT and BS — so the boot log this
is written into is not disturbed by the test of it, and the position is left at
the first column of a fresh line afterwards.

The two assertions about crossing to the row above are the exception and must
be: a landing cannot be judged without text upon the row above to land after.
They write upon a fresh line, assert, erase what they wrote, and leave the line
blank, so the log is as it would have been. **Their absence is what let a real
fault ship**: every assertion here used characters that draw nothing, so the one
case that needs a character drawn was the one case never exercised.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| The console's extent fits within the framebuffer, and is not zero | A row or column past the end of the mapping; or a division by zero at the first tabulation. |
| CR returns to column 0 and does not change the row | It was implemented as a line feed, which is what a terminal setting hides. |
| HT from column 0 lands on column 8, **and from column 8 lands on 16** | The second is the case a careless implementation gets wrong by standing still, or by advancing *by* eight from an arbitrary column so that nothing lines up. |
| BS moves exactly one position | It erased as well, or moved two. |
| BS at the erase limit does not move | An echo loop can erase the prompt, or output the kernel wrote. |
| BS at column 0 with the limit there does not cross to the row above | The limit is not consulted on the row-crossing path — the path that would eat the previous line of the log. |
| BS at column 0 crosses to the row above **and lands after its text** | Section 2.2.1: the cursor at the edge of the display instead, where the erasure the caller composes is written far away from the text and backspacing over a line separator does nothing a person can see. |
| Three erasures then return to column 0 | The landing was right and the erasure that follows it is not, which is the half a position assertion alone would not reach. |
| BS crossing into a row filled to its last column stops upon that **final character** | The wrapped row treated as one that ended with a line feed. There is no separator there to consume, so the cursor would stop one position past the last character and the erasure would fall off the row. |
| Erasing a filled row returns to column 0 | The same, over the whole width of the display rather than at one position. |

### 4.4 What a person judges

That the log is legible is not something the kernel can assert. The procedure is
in `docs/project/TESTING-GRAPHICS.md`, Section 3: the whole boot log rendered on the
screen from its first line, **including every number**, which is what Section 2.1
is about.

### 4.5 The negative test

Glyph `0x4F`, `'O'`, was overwritten with the bytes of glyph `0x30`, `'0'` —
precisely the copy-and-paste this is meant to catch, and one that leaves a
perfectly plausible-looking table because the picture comment beside it was left
alone. The run reported

```
  two glyphs are identical, at codes 0x30 and 0x4F
Console self-test FAILED.
```

and `make verify` failed on it. The edit was then reverted.

Three further negative tests were made when the backspace was corrected. Putting
the cursor back at the right-hand edge upon a crossing — the fault as it was
reported — gave `a backspace crossing to the row above did not land after its
text` and `three erasures did not return to the first column`. Dropping the
filled-row exception gave `a backspace crossing into a filled row did not stop
upon its final character` and the erasure assertion beside it. Removing the shift
of the row lengths upon a scroll passed `make verify` — the console at 1280 by
800 has a hundred rows and has not scrolled by the time the self-test runs — and
was confirmed instead at the echo loop, by scrolling the display past a hundred
and thirty lines and then backspacing over a line separator: the text stood
unerased, exactly as it had upon the machine that reported this. See
`docs/project/TESTING-GRAPHICS.md`, Section 3.3.

## 5. Observed state of the console

Under QEMU with the q35 machine and the standard VGA adapter, at 1280 by 800:

```
Console: 160 by 100 characters of 8 by 8 pixels.
Console: written 1932, scrolled 0, cursor at row 37, column 60.
Console: 1903 bytes replayed from before the console existed.
Console self-test passed.
```

Nineteen hundred bytes replayed and nothing dropped, against a capacity of four
kibibytes. The log has not reached a hundred rows by the time the console reports
itself, so nothing has scrolled at that point.

Under VirtualBox, whose boot loader chooses 640 by 480:

```
Console: 80 by 60 characters of 8 by 8 pixels.
Console: written 1875, scrolled 0, cursor at row 41, column 60.
Console: 1847 bytes replayed from before the console existed.
Console self-test passed.
```

Eighty by sixty does not hold the boot log, so that machine scrolls where QEMU
does not, and the screen at the end of the boot is the evidence that Section
19.3's copy direction is right: text that had been moved up dozens of times was
legible and unsmeared.

That is the result worth having from VirtualBox. It had **no readable diagnostic
output at all** — no serial adapter this kernel detects, and since sub-task 6.2 no
text mode either — and it now shows the boot log on the screen.

---

## 6. The optimisation, and the measurement that directed it

The console of Section 2 worked and was slow. This section is what was done
about that, and it begins with the measurement rather than with the change,
because the first two things that looked worth optimising were not the things
that cost.

### 6.1 Measuring it

The interval timer is useless here: interrupts are disabled for most of the
boot, and **seventeen ticks elapse in the whole of it**. `RDTSC` was used
instead. The figures below are from QEMU's interpreter, so they are proportional
to instructions executed rather than to cycles on any real processor; that is the
right measure for this, the fault being instruction count and not memory latency.

| | Before | After | |
| --- | ---: | ---: | ---: |
| The console, over the whole boot log (9,001 characters) | 952,248 | 254,245 | **3.7×** |
| One full-screen clear, 1280 × 800 × 4 | 77,647 | 9,884 | **7.9×** |
| One full-screen scroll | 40,398 | 11,086 | **3.6×** |
| Four thousand characters | 1,059,136 | 283,243 | **3.7×** |
| The whole boot | 6,254,178 | 5,588,320 | 11% |

*(Thousands of `RDTSC` units.)*

The console was **15.2% of the entire boot** and is now 4.5%.

### 6.2 What was actually wrong

Three things, and all three were the same thing: the primitives were written for
clarity at the pixel, and a console addresses pixels a quarter of a million times
a second.

**A four-byte pixel was written as four bytes.** `GraphicsStorePixel` looped
`bytes_per_pixel` times, shifting and masking a byte out of the colour each time.
That is four stores and a loop where one 32-bit store would do, and it is the
innermost operation in the system: it was measured at some nineteen cycles a
byte.

**A glyph was drawn one pixel at a time.** `FontDrawGlyph` called
`GraphicsPutPixel` sixty-four times, and each call tested the clip and recomputed
the address from the row and the pitch. The whole eight-by-eight cell shares one
clip test and one address per row.

**A console cell was written twice.** The console filled the cell with the
background and then drew the glyph over it — 256 byte-stores for the fill, and
then up to 64 pixels written again. Every pixel of every character was addressed
twice and clipped twice.

### 6.3 What was done

**`GraphicsSurface` gained `whole_words`.** It records that a surface may be
addressed a 32-bit word at a time: the pixel is four bytes, the base address is a
multiple of four, and **the pitch is a multiple of four**. All three are
required, and the third is the one that is easy to forget — a base that is
aligned and a pitch that is not puts every odd row out of alignment, which is
hard to see in a test and which the machines that care about alignment will care
about. It is computed once by `GraphicsSurfaceInitialise`, because it is a
property of the surface and a test made once is not a test made in an inner loop.

Where it holds, the fill, the blit and the pattern block below run word-wide
loops. Where it does not, the byte loops run and the result is identical; nothing
depends upon the fast path for correctness, which is what [`FAULTSCREEN.md`](FAULTSCREEN.md), Section 2.1, asserts.

**`GraphicsPatternBlock` replaced sixty-four clip tests with one.** It draws a
block eight pixels wide and any number of rows high, taking each pixel's colour
from one of two by a bit of a pattern — one byte to a row, most significant bit
leftmost. That is exactly the shape of a bitmap glyph, so the font table is
handed over as it stands, with no copying and no transformation.

**`FontDrawGlyphOpaque` draws the cell and the glyph in one pass.** The console
uses it; `FontDrawGlyph` remains for text drawn over an image, where what is
behind the character must show through. The two must light the same ink pixels
and [`FAULTSCREEN.md`](FAULTSCREEN.md), Section 2.2, asserts that they do, for every glyph in the face — the console
having changed which of them it goes through, and no other test using the path
the console uses.

**The blit copies words.** Both surfaces must permit it, because a word-aligned
destination reached from an unaligned source would need the bytes reassembled
across word boundaries, which is more work than the byte loop it replaced. A
scroll is what this pays for: it reads the whole framebuffer back, and a
framebuffer is write-combining, where **reads are uncached**, so the read is the
expensive half and a quarter as many of them is the whole of the gain.

Removing the read altogether needs a back buffer, which is sub-task 6.6. That is
the remaining factor and it is a larger one than anything here; it is not
attempted now because a back buffer is a compositing decision and not an
optimisation.

### 6.4 The strict-aliasing question, and why the test surfaces changed type

Writing a four-byte pixel as one `uint32_t` store means accessing memory through
an lvalue of a type it may not have. For the framebuffer this is sound: the
mapping has no declared type, and reading it back through `uint8_t` — which
`GraphicsPixelAt` does — is permitted for a character type whatever the object.

The **test surfaces** were the problem. They were declared `static uint8_t[]`,
which fixes their type for their whole life, and writing a `uint32_t` into one is
undefined however well it appears to work at `-O2`. They are now declared as
arrays of `uint32_t` and cast where a byte-wise helper wants them, which is sound
in both directions and which also guarantees the four-byte alignment the word
path requires. A byte array guarantees neither.

