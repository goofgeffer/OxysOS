<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Testing: the Graphical Work

**Authority**: `PROJECT_GUIDELINES.md`, Section 2, the testing mandate.

**What is here**: the verification of sub-tasks 6.2 to 6.6 — the framebuffer, the
drawing primitives, the font and the console, the measurement and the
optimisation it directed, the fault screens, the compositor and the pointer.

These are apart from the rest because their evidence is of a different kind.
**Most of what matters here cannot be asserted by the kernel at all**: a
framebuffer written perfectly may be scanned out by nothing, a console may be
correct and illegible, and a pointer may leave a trail that every assertion in
memory passes. So each section below pairs what the self-test asserts with what a
person has to look at, and says which of the two found what.

**The designs** these assert are
[`../design/FRAMEBUFFER.md`](../design/FRAMEBUFFER.md),
[`../design/DRAWING.md`](../design/DRAWING.md),
[`../design/CONSOLE.md`](../design/CONSOLE.md),
[`../design/FAULTSCREEN.md`](../design/FAULTSCREEN.md) and
[`../design/COMPOSITOR.md`](../design/COMPOSITOR.md), which
[`../design/GRAPHICS.md`](../design/GRAPHICS.md) indexes.

**The other three**: [`TESTING.md`](TESTING.md) is how the machine is tested and
in which environments; [`TESTING-SYSTEM.md`](TESTING-SYSTEM.md) is everything
that is not graphical; [`TESTING-RECORD.md`](TESTING-RECORD.md) is the dated
record. `TESTING.md` records why there are four.

---

## 1. Verification of the framebuffer

The description, the mapping and the memory type of the framebuffer acquired by
sub-task 6.2 are asserted at every boot by `KernelVerifyFramebuffer`. Each
assertion, and the silent failure it catches, is tabulated in
[`../design/FRAMEBUFFER.md`](../design/FRAMEBUFFER.md), Section 8.

**One thing the kernel cannot assert about a display is that anything appeared
upon it.** A framebuffer that is mapped, written and read back correctly may
still be scanned out by nothing at all. That half of the verification is
performed by a person, and the self-test paints a pattern for them to judge:
bands of red, green and blue across the top sixteenth of the screen, and a single
white pixel in the very last position of the last row.

### 1.1 Capturing the pattern

**The second menu entry must be selected.** From sub-task 6.4 the console owns
the screen and would erase the pattern within the same boot, so the figures are
painted only when the command line carries `graphics-figure` — which the entry
**Oxys-OS (graphics figures)** passes, and which suppresses the console for that
boot. The assertions of both self-tests run either way; it is only the drawing
that this governs. See [`../design/CONSOLE.md`](../design/CONSOLE.md),
Section 2.5.

The entry is reached by sending a keystroke to the boot menu through the QEMU
monitor:

```sh
( sleep 2;  echo "sendkey down"; sleep 0.3; echo "sendkey ret"; \
  sleep 12; echo "screendump /tmp/oxys-fb.ppm"; \
  sleep 3;  echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

A capture that shows text rather than the figures is a capture of the default
entry: the keystroke arrived before the menu was drawn, or after the three-second
timeout had elapsed.

The image is a binary PPM, whose header states the mode the boot loader chose.
Four things are read from it, and each establishes something the kernel cannot
establish for itself:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The bands are **red, then green, then blue**, left to right | The channel positions or widths were misread; the kernel would be writing blue where it meant red, with nothing to report it. |
| They are **flat**, not sloping | The pitch is wrong. A traversal stepping by the occupied width rather than the pitch shears the image progressively down the screen. |
| They reach the **right-hand edge** | The width or the pitch is short. |
| The **last pixel of the last row is white** | The mapping is short by less than a page — an amount every assertion made upon the start of the range would pass. |

The bands occupy the top sixteenth of the screen; the remainder is black, being
memory nothing has written.

### 1.2 The negative test

To confirm the self-test can fail, change the memory type written into the page
attribute table from write-combining to write-back:

```sh
sed -i 's/FRAMEBUFFER_PAT_ENTRY_WC   UINT64_C(0x01)/FRAMEBUFFER_PAT_ENTRY_WC   UINT64_C(0x06)/'     graphics/framebuffer.c
make verify
```

The run must report `entry 4 of IA32_PAT does not hold write-combining, so the
framebuffer is write-back and the display may lag the memory indefinitely` and
end `Framebuffer self-test FAILED.` Restore the file afterwards.

### 1.3 What the display self-test does now

It is skipped. Requesting a framebuffer causes the boot loader to set a graphics
mode, and every assertion the display test makes reads a character cell back out
of the text buffer at `0xB8000`, which in a graphics mode is not the text buffer.
The expected line is:

```
Display self-test skipped; the adapter is in a graphics mode, which the framebuffer owns.
```

A run in which it is *not* skipped is a run in which the boot loader left the
adapter in a text mode, and the display test then applies as it always did. Both
are correct; which occurs is the boot loader's decision. See
[`../design/FRAMEBUFFER.md`](../design/FRAMEBUFFER.md), Sections 2.1 and 7.

## 2. Verification of the drawing primitives

The primitives of sub-task 6.3 are asserted at every boot by
`KernelVerifyGraphics`, **against a surface composed in memory and not against
the framebuffer**. Every assertion, and the silent failure it catches, is
tabulated in [`../design/DRAWING.md`](../design/DRAWING.md), Section 6.

The test surface is 32 by 16 pixels of four bytes in rows of 40. The pitch
exceeds the width deliberately: a primitive that stepped from row to row by the
width rather than the pitch would still write inside the array, merely writing
the wrong pixels, so the eight pixels of padding on each row hold a sentinel that
no test ever writes and the padding is checked after each operation. A failure
therefore names the operation that caused it.

Because the surface is in memory, **all of this holds upon a machine with no
display at all**, which is the reason the primitives take a surface rather than
drawing upon the framebuffer by name.

### 2.1 The figure a person judges

The self-test also draws upon the framebuffer, and that part is judged by eye.
Capture it as in Section 1.1, **which from sub-task 6.4 means booting the
Oxys-OS (graphics figures) entry**; the default entry gives the screen to the
console instead. Four things are drawn, and each shows something different:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| A one-pixel frame around the **whole** screen, on all four edges | The extent or the pitch is wrong. A wrong pitch makes the vertical edges lean rather than run straight. |
| A panel with its two diagonals **crossing exactly at its centre** | The line is not exact; an error accumulated wrongly puts the crossing off-centre. |
| The second panel's fill and line **stopping dead at the clip boundary**, with the line's slope unchanged where it stops | Clipping is not confining the fill, or — the subtler fault — the line was clipped by moving its endpoints, which meets the boundary at a slightly different height. |
| The first panel **copied below itself, identically** | The blit is displaced, or takes the wrong part of the source. |

The frame is one pixel wide, so it is easily lost when a captured image is
scaled down; sample the corner pixels rather than trusting the eye at reduced
size.

### 2.2 The negative test

To confirm the self-test can fail, make the primitives address a row by the
surface's width instead of its pitch — the fault the padding sentinel exists to
catch:

```sh
sed -i 's/(uint64_t)(uint32_t)y \* surface->pitch/(uint64_t)(uint32_t)y * surface->width * surface->bytes_per_pixel/' \
    graphics/draw.c
make verify
```

The run must report `wrote into the row padding` from **six** independent
primitives — the fill, the clipped fill, the clear, the line, the blit and the
trimmed blit — and end `Graphics self-test FAILED.` Restore the file afterwards.

## 3. Verification of the font and the console

The font and the console of sub-task 6.4 are asserted at every boot by
`KernelVerifyConsole` — thirty-three assertions in three groups, each tabulated
against the silent failure it catches in
[`../design/CONSOLE.md`](../design/CONSOLE.md), Section 4.

The face and its drawing are asserted **against a surface composed in memory**,
as the primitives of Section 2 are, so that the whole of that holds upon a
machine with no display. The four control characters are asserted upon the live
console, because the position they move is the console's own and there is no
second one to make; only characters that draw nothing are used — CR, HT and BS —
so the boot log the test is written into is not disturbed by the test of it.

The exception is the pair of assertions about a backspace crossing to the row
above, added when that path was found to be wrong. A landing cannot be judged
without text upon the row above to land after, so those write upon a fresh line,
assert, erase what they wrote and leave the line blank. Their absence is what let
the fault ship: every assertion here used characters that draw nothing, so the
one case that needs a character drawn was the one case never exercised.

The assertions worth naming here are the ones a compiler cannot make about a
table authored by hand: that **no two glyphs are identical**, that exactly one
glyph is blank, and that no glyph draws into the two columns reserved for the
spacing between characters.

### 3.1 The half a person judges

That the log is legible is not something the kernel can assert, for the reason
Section 1 gives: a framebuffer written correctly may be scanned out by nothing.
Boot the **default** menu entry — the console owns the screen there — and look at
it:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The log begins at its **first line**, `Oxys-OS`, at the top of the screen | The replay buffer is not being replayed, and the screen begins part way through the boot. |
| **Every number is present** — addresses, counts, sizes | `KernelWriteHexadecimal` or `KernelWriteDecimal` is naming an output device itself rather than emitting through `KernelWriteString`. This reads as a formatting error in the messages and is a missing output path; see [`../design/CONSOLE.md`](../design/CONSOLE.md), Section 2.1. |
| Letters are upright and not mirrored, and words have gaps between them | The bit order is reversed, or a glyph draws into its spacing columns. |
| Text that has **scrolled** is unsmeared | The blit copied in the wrong direction and read bytes it had already overwritten. Visible only upon a display too short to hold the log, which is VirtualBox's 640 by 480 and not QEMU's 1280 by 800. |
| The echo loop's backspace stops at the prompt | The erase limit is not set, or did not move with a scroll. |

A screendump serves for the first four:

```sh
( sleep 11; echo "screendump /tmp/oxys-console.ppm"; sleep 3; echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

### 3.2 The negative test

To confirm the self-test can fail, duplicate a glyph — precisely the
copy-and-paste the assertion exists for, and one that leaves a plausible-looking
table because the picture comment beside it is not touched:

```sh
# Give 'O' (0x4F) the bytes of '0' (0x30).
sed -i "/0x4F  'O'/{n;s/.*/    { 0x78, 0x84, 0x8C, 0x94, 0xA4, 0xC4, 0x78, 0x00 },/}" \
    graphics/font.c
make verify
```

The run must report

```
  two glyphs are identical, at codes 0x30 and 0x4F
Console self-test FAILED.
```

and `make verify` must itself fail, the harness having gained the assertion upon
`FAILED` recorded in [`TESTING.md`](TESTING.md), Section 1. Restore the file afterwards — the correct bytes
for `'O'` are `0x78, 0x84, 0x84, 0x84, 0x84, 0x84, 0x78, 0x00`, and the picture
comment beneath the line states them.

### 3.3 The backspace across a line separator

The fault was reported from a real machine: backspacing over letters worked and
backspacing over a line separator did not. The cause was the console putting the
cursor at the right-hand edge of the display when it crossed to the row above,
so the erasure its caller composes — `BS SP BS` — wrote its space a hundred and
fifty columns away from the text. See
[`../design/CONSOLE.md`](../design/CONSOLE.md), Section 2.2.1.

It is reproduced by driving the echo loop from outside, which needs the serial
line for input rather than a log file. A socket serves for both, with the monitor
upon a second one so that the screen can be captured while the machine still
stands at the loop:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -display none -no-reboot \
    -serial unix:/tmp/oxys-serial.sock,server=on,wait=off \
    -monitor unix:/tmp/oxys-monitor.sock,server=on,wait=off
```

Wait for `A backspace erases, and crosses to the line above.` upon the serial
socket, then send `ab`, a line feed, `cd`, a line feed, and three `0x08` bytes,
a quarter of a second apart so that each is processed as a keystroke would be.
Then `screendump` upon the monitor socket.

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The three backspaces leave `ab` alone upon the screen | The line separator, then `d`, then `c` were each consumed. Both lines standing intact is the fault as reported. |
| Ten backspaces leave neither line, and the banner above them intact | The erase limit still holds across a row crossing — the assertion that keeps an echo loop from eating the boot log. |
| After a hundred and thirty line feeds, the same sequence still erases | The row lengths moved with the text when the display scrolled. |

The third is the case the boot-time self-test cannot reach. The console at 1280
by 800 is a hundred rows tall and has not scrolled by the time the self-test
runs, so removing the shift of the row lengths upon a scroll leaves `make verify`
passing; it is caught here and nowhere else.

### 3.4 The negative tests of the backspace

Each was applied to `graphics/console.c`, confirmed and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The cursor put at the right-hand edge upon crossing, which is the fault as it was reported. | `a backspace crossing to the row above did not land after its text` and `three erasures did not return to the first column`. |
| The filled-row exception dropped, so that a wrapped row is treated as one that ended with a line feed. | `a backspace crossing into a filled row did not stop upon its final character` and the erasure assertion beside it. |
| The shift of the row lengths upon a scroll removed. | `make verify` **passed**, for the reason given in Section 3.3. Confirmed instead at the echo loop: after a hundred and thirty line feeds, `abc` followed by a line feed and three backspaces left `abc` standing untouched. |

## 4. Verification of the drawing optimisation

The primitives gained a path that writes a four-byte pixel as one 32-bit store,
and the console gained one that draws a character cell and its glyph in a single
pass. Both are asserted by `KernelVerifyGraphics` and `KernelVerifyConsole`, and
both are tabulated in [`../design/FAULTSCREEN.md`](../design/FAULTSCREEN.md),
Sections 2.1 and 2.2.

**A fast path is the most dangerous kind of code to leave unasserted.** It runs
only when its own precondition holds, so a fault in it is invisible upon every
surface that does not meet the condition — and the surface the self-tests use is
not the surface a person looks at. The test therefore draws upon **two surfaces
differing in nothing but their alignment** and requires them to produce identical
pixels, and it compares the two glyph routines for every glyph in the face.

### 4.1 Measuring it again

The figures in [`../design/CONSOLE.md`](../design/CONSOLE.md), Section 6.1, were obtained with `RDTSC`,
not with the interval timer: interrupts are disabled for most of the boot and
only seventeen ticks elapse in the whole of it. To repeat the measurement, time
the operations from `KernelMain` after `PitInitialise` and read the counter
directly; the ratios are what matter, QEMU's interpreter making the absolute
figures proportional to instructions executed rather than to cycles.

### 4.2 The negative tests

Two, because the optimisation has two halves.

```sh
# The alignment conditions removed, so every four-byte surface claims the fast path.
sed -i 's/    surface->whole_words = (bytes_per_pixel == 4U) \&\&/    surface->whole_words = (bytes_per_pixel == 4U); \/\//' \
    graphics/draw.c
```

The run must report `a surface whose base and pitch are both odd was marked as
addressable by words, so every row would be written misaligned` and end
`Graphics self-test FAILED.`

The second is applied by hand: make `GraphicsPatternBlock` skip its clear bits,
as the transparent glyph does, by replacing the word store in its inner loop with
`if ((bits & bit) != 0U) { word[column] = ink; }`. Three assertions must fire in
the graphics self-test and a fourth in the console self-test, the last naming the
code point at which the two glyph routines first disagree. Restore the file
afterwards.

## 5. Verification of the fault screens

`KernelVerifyFaultScreen` asserts two things at every boot, and the first governs
the second.

**What is to be done about each exception.** `ExceptionDispositionOf` classifies
every vector, at both privilege levels, as resumed, terminating the program that
raised it, or fatal to the kernel — and **only the last draws a screen**. A
divide by zero raised by a program must cost that program and nothing else; the
kernel treated every exception as fatal until sub-task 6.4, so it would have
halted the machine and announced it, which is a false account of what happened
given to the person least able to check it. The classification is asserted rather
than the behaviour because half of it concerns privilege level 3, where no code
runs until sub-task 6.10, and `ExceptionDispositionOf` is a pure function that
can be asked about a privilege level that does not yet exist.

**The table of screens**: that every fault which threatens the kernel whatever
raised it has a screen of its own, that **no two share a title or a colour**,
that every title fits a 640-pixel display, that every character is one the font
can draw, and that **no screen exists for a fault that can never be the
kernel's**. Each assertion and the silent failure it catches is tabulated in
[`../design/FAULTSCREEN.md`](../design/FAULTSCREEN.md), Sections 2.3 and 2.4.

It asserts the table and not the drawing, for the reason Section 1 gives about
the display generally. **Nothing in it draws**, deliberately: drawing would set
the flag that records a screen as having been shown, and a real fault later in
the same boot would then find the display taken and draw nothing.

### 5.1 Looking at the screens — withdrawn, and what stands in its place

**There is no longer a way to look at a fault screen without causing the fault.**
Two GRUB entries did it — one drawing a composed frame for any vector, one
raising a genuine page fault — and both were removed at the project owner's
direction, along with the routine that composed the frame and the command-line
parser that read the vector. `../design/FAULTSCREEN.md`, Section 1.5, records
what they were and the distinction between them, which is still worth knowing.

What this costs, stated so that nobody looks for a procedure that is not here:

| Screen | How it could be looked at | How it can be looked at now |
| ------ | ------------------------ | --------------------------- |
| Page fault | Either entry | By causing a real one, which means editing the kernel |
| Double fault, machine check, malformed task state segment, and the rest | The composed frame only | Not at all without editing the kernel |

The screens **were** judged by eye when they were written, against every item in
the table that follows, and that judgement is recorded in
[`TESTING-RECORD.md`](TESTING-RECORD.md). The table is kept because it is the
list of things to look for whenever a screen is next changed, and because a
screen altered without being looked at is the defect
`../design/FAULTSCREEN.md`, Section 1.3, exists to describe. Anybody changing one
must restore a way of drawing it and repeat this list; the cheapest is to call
`FaultScreenShowException` from a temporary edit to `KernelMain` and revert it.

What to look for:

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| A coloured banner with the fault's title at several times life size, **not clipped at the top** | The banner is shorter than the title, or something scrolled the framebuffer after the screen was drawn. This is exactly the fault [`../design/FAULTSCREEN.md`](../design/FAULTSCREEN.md), Section 1.3, records. |
| The mnemonic and vector beneath the title | The general screen was drawn, meaning the table has no entry for this vector. |
| Panels that differ **between faults** — an address for a page fault, a decoded selector for a general protection fault, instruction bytes for an invalid opcode | The evidence flags are not being consulted, and every fault is being given the same page. |
| The instruction bytes reproduced as real values, or an explicit statement that the address is unmapped | The bytes are being read without asking the paging hierarchy, which would raise a second fault. |
| Nothing written over the page afterwards | The console was not suspended. |
| Every character drawn, with **no replacement boxes** in the prose | Text outside the printable ASCII the face covers. An em dash in a string literal is three UTF-8 bytes and renders as three boxes. |
| At **640 by 480**: the title still fits, paragraphs re-wrap, and the footer is still on the screen | The layout was fitted to 1280 pixels. |

### 5.2 The negative tests

Four, and the first is the one that matters most: it reproduces the fault this
section was written because of.

**Every fault made fatal.** Remove the privilege-level test from
`ExceptionDispositionOf` in `kernel/arch/x86_64/interrupt/exceptions.c`, so that everything falls
through to `EXCEPTION_DISPOSITION_FATAL`. The run must name all seven program
faults in turn, beginning `a program's own fault would halt the machine rather
than the program, at vector 0x0` — vector 0 being the divide by zero.

**A duplicated identity.**

```sh
sed -i 's/{ 11U, "DESCRIPTOR NOT PRESENT",/{ 11U, "MALFORMED TASK STATE SEGMENT",/' \
    graphics/faultscreen.c
make verify
```

The run must report `two fault screens share a title, at vectors 0xA and 0xB`.

**A deleted screen.** Delete the `{ 18U, "MACHINE CHECK", ... }` row. The run
must report `a fault that threatens the kernel has no screen of its own, at
vector 0x12` — the fault this catches being the one that would otherwise look
like nothing at all, the general screen still naming the vector.

**A screen that can never be drawn.** Add an entry for vector 17, the alignment
check. The run must report `a screen exists for a fault the processor raises only
outside the kernel, at vector 0x11`. Note that the weaker rule — asking merely
whether the vector is ever fatal — does not catch this, `#AC` being nominally
fatal from a kernel selector; the architectural fact that it requires privilege
level 3 has to be stated before the assertion has any force.

Restore the files afterwards. Each run must end `Fault disposition and screen
self-test FAILED.`


## 6. Verification of the compositor

`KernelVerifyCompositing` and `KernelVerifyCompositor` run at every boot. The
first asserts the clip stack and the blend upon surfaces composed in memory, so
it holds upon a machine with no display; the second asserts the damage
arithmetic and the layer table, which is what can be asserted of a compositor
that owns one back buffer and one framebuffer and has no second of either to
compose. Both are tabulated against the failure each would catch in
[`../design/COMPOSITOR.md`](../design/COMPOSITOR.md), Section 2.5.

### 6.1 What only looking establishes

Three things, and all three are what the sub-task exists for.

| What to look for | What its absence would mean |
| ---------------- | --------------------------- |
| The boot log appears at all, from its first line | The console is drawing into a back buffer nothing carries out, or into the framebuffer while presentations copy the back buffer over it |
| The pointer is drawn **over** the text, its black outline cutting into the letters beneath | The layer is composited under the base, or not at all |
| After the pointer has crossed the screen, **the text it passed over is intact and there is no trail** | [`../design/COMPOSITOR.md`](../design/COMPOSITOR.md), Section 2.3: a layer that marks only where it has arrived leaves its previous appearance standing. This is the fault the save-under existed to prevent, and reintroducing it would undo the sub-task |
| A fault screen stays on the screen | [`../design/COMPOSITOR.md`](../design/COMPOSITOR.md), Section 2.6: the compositor must be suspended, or the next `KernelWriteString` carries the back buffer over the page |

The pointer is driven from the monitor rather than by hand, so that the path is
repeatable:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -display none -no-reboot \
    -serial unix:/tmp/oxys-serial.sock,server=on,wait=off \
    -monitor unix:/tmp/oxys-monitor.sock,server=on,wait=off
```

Wait upon the serial socket for `A backspace erases, and crosses to the line
above.`, then send `mouse_move 20 12` to the monitor a dozen times, a quarter of
a second apart, and `screendump` afterwards. The pointer should stand at the far
end of the path and nothing should mark the path itself.

A trail is easier to measure than to see: a screendump in which no row holds a
run of six or more white pixels except at the pointer's final position is a
screen with one pointer upon it.

### 6.2 The negative tests of the compositor

Each was applied, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| A push replacing the clip in force rather than intersecting it. | `a push to a wider region widened the clip` |
| The mask indexed by the destination's width rather than the source's. | `the mask was indexed by the wrong stride` and `the mask was indexed by the destination's width` — but only after the test was corrected; see below. |
| `CompositorMoveLayer` marking only where the layer has arrived and not where it was. | **Nothing at all** from the self-test, and a trail upon the screen: thirteen pointers along the path of one. Measured rather than admired — a screendump in which four rows hold a run of six or more white pixels holds one pointer, and this one had fifty-two. |
| The layers taken by the self-test not given back. | Nothing from the self-test, and a machine that boots **with no pointer** and reports no failure: the table was exhausted, `CursorInitialise` was refused a layer, and every routine in the pointer then correctly did nothing. |

**Two of the four are invisible to every assertion available**, and both were
found by looking at the screen. That is what Section 6.1 is for, and it is why
the compositor's verification is not finished when `make verify` passes.

The second is worth recording for what it revealed about the test rather than
the code. Applied to the test as first written it **passed**: the composited
source and the destination were both sixteen pixels wide, so an index taken from
either was the same number and the fault the assertion named could not be
produced. The source is now seven wide against a destination of sixteen with a
pitch of nineteen, so the three strides differ at every row after the first. An
assertion that cannot fail is worse than no assertion, because it is counted
among those that pass.

## 7. Verification of the mouse and the pointer

Neither self-test needs a mouse, and that is what makes them run at every boot
upon every machine.

**The decoder is driven directly.** `MouseProcessByte` is exposed for the same
reason `KeyboardProcessScancode` is: the decoding of a movement packet is not a
property of the 8042, and a byte arriving by any route decodes identically. So
`KernelVerifyMouse` composes packets — of whichever length the device negotiated
— and asserts the nine-bit sign extension, the inversion of the vertical sense,
the confinement of the position at all four edges, the naming of button
transitions, the discarding of an overflowed movement, the refusal and counting
of a byte that cannot begin a packet, the recovery of the stream afterwards, the
abandonment of a partial packet by a flush, and the behaviour of a full buffer.

**The pointer is asserted upon a surface composed in memory**, whose pitch
exceeds its width and whose padding holds a sentinel. That is what permits the
assertion that a pointer at the edge writes nothing outside the surface, which a
framebuffer could not be asked. It also asserts what a lazier test would omit:
that the **transparent** pixels of the shape still hold the background, without
which a pointer drawn as a solid rectangle would pass.

Every assertion and the silent failure it catches is tabulated in
[`../devices/MOUSE.md`](../devices/MOUSE.md), Sections 8.1 and 8.2.

### 7.1 What is skipped, and why that is not a gap

The assertions that need a device — that one answered, that IR12 was claimed and
unmasked, that the controller agrees the second port is usable — are made only
where `MouseIsPresent`. A machine may genuinely have no mouse, and the driver is
required to discover that **without blocking**; reaching the skip at all is
evidence that it did, every wait upon the controller being bounded.

### 7.2 The negative tests

Apply each, run `make verify`, then revert it.

```sh
# The device's vertical sign forwarded rather than inverted.
sed -i 's|movement_y = -MouseExtendMovement|movement_y = MouseExtendMovement|' \
    drivers/mouse/mouse.c

# The magnitude sign-extended as eight bits rather than nine.
sed -i 's|return (int32_t)magnitude - (negative ? 256 : 0);|(void)negative;\n    return (int32_t)(int8_t)magnitude;|' \
    drivers/mouse/mouse.c
```

For the pointer, change `CursorRestoreUnder` in `graphics/cursor.c` to put the
pixels back at `CursorPositionX`/`CursorPositionY` rather than at
`CursorSavedX`/`CursorSavedY`; or move the `CursorSaveUnder` call in
`CursorDrawAt` from before the drawing loop to after it.

Each run must end `Mouse self-test FAILED.` or `Pointer self-test FAILED.`

### 7.3 A caveat for headless capture

A keystroke injected through the QEMU monitor **before the controller has been
initialised** makes `Ps2Initialise` fail, and the machine then reports no
controller, no keyboard and no mouse. The byte lands in the output buffer after
the single drain at the start of the sequence and is read as the answer to a
command that follows.

It is an artefact of injecting into an emulated controller whose ports are
disabled — a real machine does not scan a disabled port — and it is recorded here
because it is an easy way to spend an hour diagnosing a driver that is working.
When capturing a screenshot, either let GRUB's timeout elapse or wait for the
boot to finish before drawing conclusions from the report.


## 8. Verification of the window manager

The self-test of sub-task 9.1 conducts the manager upon a screen composed in
memory and reads the stacking order from the pixel where two windows overlap;
[`../design/WINDOWS.md`](../design/WINDOWS.md), Section 6, pairs each of its
assertions with the failure it would catch. What follows is what only looking
establishes, and how it is looked at.

**Boot the default entry.** Three windows stand upon a dark ground: *Oxys*,
*Pointer* and *Keys*, the last with its band in blue — it holds the focus. The
pointer is at the centre of the screen. Since sub-task 9.2 the three are drawn
by `/bin/windows` at privilege level 3, so everything below is also a judgement
upon the client protocol: a program that could not blit, or that never woke
for an event, would show a window that never changes. Judge:

1. **Typing** adds a tile per character to the Keys window and nowhere else,
   and a backspace removes the last. Press the Pointer window and type again:
   nothing arrives anywhere, the Pointer window ignoring keys, and the blue has
   moved to its band.
2. **The pointer** moved into the Pointer window makes a disc follow it. Hold a
   button: the disc grows. Drag out of the window with the button held: the
   disc goes out of sight while the window still receives — the program's
   event has coordinates beyond the content — and no other window reacts.
   Release, and the pointer is the other windows' again. (At 9.1 the window
   printed the coordinates as well; a program cannot draw text yet.)
3. **A drag** by a band moves the window under the hand with no trail where it
   was and no lag a person can see, and the dragged window is on top of whatever
   it crosses from the moment it is pressed. Drag it to every edge: the band
   never leaves the screen.
4. **The disc** at the right of a band closes the window: it vanishes whole, the
   ground and whatever it covered are repainted beneath, and the blue passes to
   the topmost window remaining. Close all three: the ground is bare, the
   pointer still moves, and nothing is drawn.

**Under QEMU without a person**, the monitor drives all of it: `sendkey` for the
keys, `mouse_move` and `mouse_button` for the pointer, and `screendump` after
each step. That is how the runs of 2026-09-17 in
[`TESTING-RECORD.md`](TESTING-RECORD.md) were made, and the captures are what
was judged. `mouse_move` is relative and the pointer begins at the centre, so a
script's arithmetic must follow the windows it has moved: the first such script
pressed where a window it had dragged now stood rather than upon the disc it
meant, which was the script's defect and is recorded there.

**Under VirtualBox** `keyboardputscancode` types and `screenshotpng` captures;
there is no way to move the mouse from `VBoxManage`, so the pointer and the
drag are judged at the console when a person is at it. **Under Bochs** with
`nogui` nothing can be operated and the evidence is the log: the manager's
report after the banner names the three windows at their places.

**The two entries that give the shell the screen** are judged by the absence:
*Shell-only* shows the banner and a prompt with the pointer following the
mouse, *Shell Diagnostics* the boot log first, and neither draws a window.

## 9. Verification of the boot screen, the desktop `init` starts, and the power screen

The self-tests of sub-task 9.3 assert the adoption of orphans and the two calls;
[`../design/INIT.md`](../design/INIT.md), Section 6, pairs each with what it
would catch. What only looking establishes is the three pages and the
supervision, and all four are judged from one boot of the default entry.

1. **The boot screen** stands from very early in the boot until the desktop
   appears: the mark, `OXYS-OS`, the version, and `starting the desktop`, upon
   the slate ground. There is no text of the boot log upon the screen at any
   point, and no black screen between the two. Boot the **Shell-only** and
   **Shell Diagnostics** entries to see the other half of the rule: neither
   draws it, and each shows what it showed before.
2. **The desktop is `init`'s.** At the prompt — the serial line's, upon the
   default entry — `ps` shows `init`, and shows `windows` carrying `init`'s
   identifier as its parent. That is the supervision, before anything is done
   to it.
3. **It is started again.** `kill -9` upon the desktop's identifier, and within
   a second the three windows are back; `ps` shows a new `windows`, still
   `init`'s child. Closing all three windows by their discs does the same
   thing by the other route.
4. **The power screen.** `shutdown` replaces the desktop with the mark,
   `OXYS-OS` and `it is now safe to turn off the machine`, and the machine
   stops; `shutdown -r` shows `RESTARTING` and the machine restarts, which
   under QEMU with `-no-reboot` is QEMU exiting and under VirtualBox and Bochs
   is the machine booting again.

**Under QEMU without a person** the monitor drives the pointer and the captures
and the serial socket types at the shell, which is where the shell is upon the
default entry — `sendkey` would reach the window manager, which holds the
keyboard there, and not the shell. That is how the runs of 2026-09-17 in
[`TESTING-RECORD.md`](TESTING-RECORD.md) were made.

## 10. Verification that the desktop's configuration is obeyed

The self-test of sub-task 9.4 asserts the format and that the shipped files
carry the keys the programs read;
[`../design/CONFIG.md`](../design/CONFIG.md), Section 6, pairs each assertion
with what it would catch. What only looking establishes is that a setting
changed in a file changes what is drawn.

1. **Change the accent.** In `/etc/desktop.conf` set `accent` to something no
   part of the desktop uses — `230, 120, 40` is what the run of 2026-09-18
   used — rebuild the image, and boot the default entry. The discs the desktop
   draws in its own windows are that colour.
2. **The frame does not change**, and that is the half of the assertion a
   person is most likely to skip. The title band of the focused window stays
   blue: the frame's colours are the window manager's, drawn by the kernel,
   and the file says so where a person will read it. A change that altered the
   band too would mean the desktop had been given the frame to draw, which is
   sub-task 9.1's boundary gone.
3. **Change the scale.** `scale = 1` draws the windows and their contents at
   the face's own size upon a screen where `0` would have chosen two; `scale =
   9` is refused and the default stands, which is the bound a person cannot
   draw a window outside of.
4. **Break a line.** Put `run` before any section, or leave a bracket unclosed,
   and the faults are printed upon the serial line with their line numbers
   while everything else in the file still takes effect — which is the format's
   central promise and the one thing a single screenshot cannot show.

## 11. Verification of the session

The self-tests of sub-task 9.5 assert the layers, the focus rule, the text and
the claim; [`../design/SESSION.md`](../design/SESSION.md), Section 6, pairs each
with what it would catch. What only looking establishes is the desktop.

1. **The desktop with nothing running.** Boot the default entry: the root
   carries the mark and the wordmark the boot screen drew — the same figure and
   the same ground, so that the hand-over is not a flash — and a panel stands
   across the top with `OXYS` at its left.
2. **The launcher.** Press the panel's button: a menu opens beneath it with one
   row per `[launch]` block of `/etc/session.conf`. Press the button again, or
   press the root, and it closes. Press a row and that program starts.
3. **The layers, which is the one that matters.** With a program's windows upon
   the screen, drag one upward by its title band: it passes **under** the
   panel. Press each of the windows in turn to raise them: none of them ever
   covers the panel, and none of them ever goes beneath the root. A build in
   which the raise is not confined shows the opposite at the first press, and
   the panel does not come back.
4. **The root takes no keys.** Press the root, then type: nothing goes anywhere,
   and the window that held the focus still holds it — a desktop whose keys went
   to the wallpaper would swallow the first sentence a person typed.
5. **Upon a small screen.** VirtualBox's 640 by 480 draws at scale one, and the
   panel, the launcher and the mark must all still appear: it is the screen upon
   which the panel was once refused for being shorter than a window may be, and
   the failure showed as a bare ground and a line in the log from `init`.
