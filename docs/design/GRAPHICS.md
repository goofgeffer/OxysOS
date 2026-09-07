# The Framebuffer, the Primitives, the Console, the Fault Screens and the Compositor

**Corresponding phase**: 6, sub-tasks 6.2 to 6.6, which are the whole of the
graphical work that needs no process to exist. Sections 1 to 10 concern the
framebuffer (6.2); Sections 11 to 17 the primitives that draw upon it (6.3);
Sections 18 to 22 the font and the console drawn with them, Section 23 the
measurement that made the console fast enough, and Sections 24 and 25 the fault
screens (6.4); Section 26 the pointer (6.5); and Section 27 the compositor (6.6),
which is where several of the earlier sections' limitations are discharged.
**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.
**Implemented by**: [`../../graphics/framebuffer.c`](../../graphics/framebuffer.c),
[`../../graphics/draw.c`](../../graphics/draw.c),
[`../../graphics/font.c`](../../graphics/font.c),
[`../../graphics/console.c`](../../graphics/console.c),
[`../../graphics/faultscreen.c`](../../graphics/faultscreen.c),
[`../../graphics/cursor.c`](../../graphics/cursor.c),
[`../../graphics/compositor.c`](../../graphics/compositor.c),
[`../../kernel/include/oxys/graphics.h`](../../kernel/include/oxys/graphics.h),
[`../../kernel/include/oxys/framebuffer.h`](../../kernel/include/oxys/framebuffer.h),
[`../../kernel/include/oxys/font.h`](../../kernel/include/oxys/font.h),
[`../../kernel/include/oxys/console.h`](../../kernel/include/oxys/console.h),
[`../../kernel/include/oxys/faultscreen.h`](../../kernel/include/oxys/faultscreen.h),
[`../../kernel/include/oxys/cursor.h`](../../kernel/include/oxys/cursor.h),
[`../../kernel/include/oxys/compositor.h`](../../kernel/include/oxys/compositor.h),
[`../../kernel/multiboot2.c`](../../kernel/multiboot2.c),
[`../../boot/boot.asm`](../../boot/boot.asm),
[`../../kernel/mm/vmm.c`](../../kernel/mm/vmm.c).
**Asserted by**: `KernelVerifyFramebuffer` in
[`../../kernel/test/verify_framebuffer.c`](../../kernel/test/verify_framebuffer.c),
`KernelVerifyGraphics` in
[`../../kernel/test/verify_graphics.c`](../../kernel/test/verify_graphics.c),
`KernelVerifyConsole` in
[`../../kernel/test/verify_console.c`](../../kernel/test/verify_console.c),
`KernelVerifyFaultScreen` in
[`../../kernel/test/verify_faultscreen.c`](../../kernel/test/verify_faultscreen.c),
`KernelVerifyCursor` in
[`../../kernel/test/verify_mouse.c`](../../kernel/test/verify_mouse.c),
and `KernelVerifyCompositing` and `KernelVerifyCompositor` in
[`../../kernel/test/verify_compositor.c`](../../kernel/test/verify_compositor.c).

**Specifications**: Multiboot2 Specification 2.0, Sections 3.1.10 and 3.6.12;
Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
Sections 11.12.2 and 11.12.3, and Tables 11-7, 11-10 and 11-11; Volume 2A,
`CPUID`; J. E. Bresenham, "Algorithm for computer control of a digital plotter",
IBM Systems Journal 4(1), pages 25 to 30, 1965; ANSI X3.4-1986, the printable
range and the four control characters the console interprets; Intel SDM,
Volume 3A, Chapter 6 and Table 6-1, Sections 6.13 and 6.15 with Figures 6-6 and
6-9: the exceptions each fault screen accounts for, and the two forms of error
code they decode.

---

## 1. What this sub-task is

It obtains a framebuffer, decides whether it can be used, maps it so that it may
be written efficiently, and describes it. **It draws nothing.** The primitives
are sub-task 6.3 and the font is 6.4; what is here is the memory they will draw
into and the facts about it they will need.

Three things had to be settled, and each is a decision rather than a
transcription: how the framebuffer is asked for, what memory type its pages are
given, and what happens to the text console that was using the screen already.

## 2. Asking for it

A Multiboot2 boot loader supplies a framebuffer only to an image that says it
wants one. `boot/boot.asm` therefore carries the framebuffer request tag of
Section 3.1.10 in its Multiboot2 header:

| Offset | Field | Value | Why |
| ------ | ----- | ----- | --- |
| 0 | `type` | 5 | The framebuffer request tag. |
| 2 | `flags` | **1** | Bit 0, "optional". See below. |
| 4 | `size` | 20 | |
| 8 | `width` | 0 | No preference. |
| 12 | `height` | 0 | No preference. |
| 16 | `depth` | 0 | No preference. |

**The optional bit is set, and that is the substantive choice here.** Clearing it
declares that the image must not be loaded at all unless a framebuffer can be
supplied, and that is simply untrue of this kernel: it has a text-mode display
driver, it has a serial port, and `FramebufferInitialise` is written to return
false and let the boot proceed. Declaring a requirement the kernel does not have
would refuse to boot upon firmware that can only offer text, for the sake of a
display the kernel can do without.

It had a second consequence, discovered by having got it wrong first. GRUB treats
a *required* framebuffer as an instruction to set a graphics mode whatever else
it is told, so with the bit clear every entry of `boot/grub/grub.cfg` came up in
graphics regardless of what it asked for.

Width, height and depth are zero, which Section 3.1.10 defines as no preference.
A hard-coded mode would be refused outright by firmware that cannot set it,
whereas no preference is a request every boot loader can satisfy with something.

### 2.1 The mode is the boot loader's to choose, and this is not a preference

`gfxpayload` is GRUB's setting for the mode it leaves the machine in. It was
tried, in every entry, at the top level and within, and **GRUB 2.12 ignores it
for a multiboot2 image**: asking for `1024x768x32` yielded 1280×800×32, and
asking for `text` yielded a graphics mode likewise. Loading video drivers
explicitly with `insmod all_video` did change the outcome — to 800×600×24 — but
by changing which driver GRUB selected a mode from, not by honouring anything
asked of it, and the mode it produced was the poorer of the two.

`boot/grub/grub.cfg` therefore selects no mode and says why. **The kernel is
written to accept whatever it is handed and to assert what it was**, which is the
only position that survives a boot loader, a firmware or a machine that behaves
differently — and every one of them will.

## 3. Reading what was supplied

The framebuffer information tag of Section 3.6.12 is reduced in
`kernel/multiboot2.c` to the neutral `BootFramebuffer` of `<oxys/bootinfo.h>`,
as every other tag is: the kernel proper depends upon a description of the
machine, not upon the boot protocol that produced it.

| Offset | Field | Width |
| ------ | ----- | ----- |
| 0 | `type` = 8 | 32 |
| 4 | `size` | 32 |
| 8 | `framebuffer_addr` | 64 |
| 16 | `pitch` | 32 |
| 20 | `width` | 32 |
| 24 | `height` | 32 |
| 28 | `bpp` | 8 |
| 29 | `framebuffer_type` | 8 |
| 30 | `reserved` | **16** |
| 32 | colour description | — |

**`reserved` is sixteen bits and not eight**, and it matters. The prose diagram in
Section 3.6.12 is ambiguous upon the point; the reference implementation's
`struct multiboot_tag_framebuffer_common` settles it, and settles with it that
the colour description begins at offset 32 and is naturally aligned. A kernel
that read the description at 31 would find the red position in the upper byte of
the reserved field and every channel after it displaced by one — which is not an
error but a picture in the wrong colours.

### 3.1 What is validated, and why validation belongs here

Every field is checked before it is recorded, and the description is left
`BOOT_FRAMEBUFFER_NONE` if any check fails: a zero address, a zero extent, a zero
depth, a pitch narrower than a row, a channel lying outside the pixel.

This is not defensive habit. These values become a base address and a stride that
sub-task 6.3 will write through in a loop, and a pitch smaller than a row would
be discovered as a fault in the middle of drawing — a long way, in both code and
time, from the boot loader that supplied it. The place to refuse a bad
description is where the description is read.

**The pitch is the field most worth stating plainly.** It is the number of bytes
from the start of one row to the start of the next, and it is *not* the width
multiplied by the pixel size: a boot loader may pad a row to an alignment its
hardware prefers. Every traversal must step by the pitch. One that stepped by the
occupied width would shear the image progressively down the screen, which looks
like a hardware fault and is not one.

## 4. The memory type, and why it is not the default

A framebuffer is not memory the kernel owns. It is memory an adapter scans out of
continuously, without asking and without participating in cache coherency for
that purpose. That single fact decides how its pages are mapped.

| Type | Correct? | Fast? | |
| ---- | -------- | ----- | --- |
| Write-back | **No** | Yes | A cached write may sit in a cache line while the display shows what memory held before it. |
| Uncacheable | Yes | No | Every write is a separate transaction to the adapter. |
| Write-combining | Yes | Yes | Writes are gathered in a buffer and issued together. |

Write-back is the default for every ordinary mapping and is the one thing this
must not be. The failure it produces is unusually nasty: the write is not lost —
it lands eventually, when something unrelated evicts the line — so the image is
wrong, and then some time later, for no reason connected to anything the operator
did, it is right.

Write-combining is what a framebuffer wants, being written in runs and
essentially never read. It is selected through the page attribute table.

### 4.1 Taking entry 4 of `IA32_PAT`, and only entry 4

A page-table entry selects its memory type by the index
`(PAT << 2) | (PCD << 1) | PWT` into `IA32_PAT`, where PAT is bit 7 of the entry.
`FramebufferEstablishWriteCombining` writes the write-combining encoding `0x01`
into **entry 4**, leaving the other seven as the processor left them.

**The choice of entry 4 is the whole of the safety of this arrangement.** Entries
0 to 3 are those every existing mapping already selects — the paging hierarchy,
the direct map, the kernel image, every page the heap has issued. Altering one of
them would change the memory type of memory already in use, retrospectively, with
none of its users consulted and nothing to report it. Entries 4 to 7 are reached
only by an entry that sets the PAT bit, and nothing in this kernel set that bit
before this sub-task. Entry 4's default is write-back, the same as entry 0, so it
is an entry no existing mapping can even be distinguished by — which is what
makes it free to take.

The write is a read-modify-write for the same reason. Writing a whole constant
would be shorter, and would also decide the memory type of every mapping in the
machine from a value transcribed by hand into this one file.

Where `CPUID` leaf 1 does not report the page attribute table, nothing is written
and the framebuffer is mapped cache-disabled instead: correct, and slow. The
distinction is exposed through `FramebufferWriteCombining` because it is a
property of the machine that a later phase's measurements will have to account
for.

### 4.2 Bit 7 means two things, and this cost an hour

`PAGE_ENTRY_LARGE` and `PAGE_ENTRY_PAT` are the same bit, `0x080`, and both names
are correct. Bit 7 is **PS** in a directory entry, where it says the entry maps a
large page, and **PAT** in a table entry, where it selects a memory type. The two
names exist because the levels are different and a reader of one should not have
to know the other.

The self-test's own page walk got this wrong on its first run: it tested bit 7
for a large page at every level, so the moment the framebuffer's page-table entry
was given the PAT bit, the walk reported it as a large page and refused to read
it. The check is now confined to levels 1 and 2. It is recorded here because the
mistake is available to anyone who walks a hierarchy looking for attributes, and
because it fails in the direction that looks like a mapping fault rather than a
reading fault.

## 5. The mapping

The framebuffer is mapped by `KernelDeviceMap`, added to the kernel virtual
allocator by this sub-task. It differs from `KernelPagesAllocate` in the one way
that matters: **no frame is allocated and none is freed**, because the memory
already exists and belongs to a device.

The frame allocator must never be told of it. A framebuffer handed out as
ordinary memory would be written by whoever received it and displayed by the
adapter at the same time.

Device pages are counted separately from allocated ones, because a report that
conflated the two would suggest the frame allocator had issued memory it never
issued. The physical address need not be page-aligned; the mapping is made from
the page containing it and the returned pointer carries the offset forward, so
the caller addresses exactly the bytes it asked for.

`docs/design/MEMORY-LAYOUT.md`, Section 2, always described the arena as holding
"the kernel heap and device mappings". This is the first device mapping, and the
facility is general: the network adapter of Phase 11 will use it unchanged.

## 6. The colour encoding

`FramebufferEncode` packs three eight-bit channels into a pixel of the
framebuffer's layout, using the positions and widths the boot loader reported.

They are read rather than assumed. `0x00RRGGBB` is a convention of the common
case and not a rule; a kernel that assumed it would write blue where it meant red
upon an adapter that orders the channels otherwise, and would do so with nothing
to report.

A channel narrower than eight bits is reduced by **discarding low-order bits**,
and that direction is the correct one. A five-bit channel given `0xFF` must yield
`0x1F`; taking the top five bits does. Taking the bottom five would yield `0x1F`
for `0x1F` and for `0xFF` alike, and would make the whole range dark and banded.

## 7. The text console, which this sub-task displaces

Requesting a framebuffer causes GRUB to set a graphics mode, and in a graphics
mode the memory at `0xB8000` is no longer the text buffer. The VGA driver of
sub-task 4.2 goes on writing to it and nothing appears.

This is a real cost and it is not disguised. **For two sub-tasks the screen
showed the colour bands the self-test paints and nothing else**, until sub-task
6.4 supplied a console that draws text upon the framebuffer; see Section 19. The whole boot log continues to be
carried by the serial port, which is the path the automated tests read in any
case, so nothing is lost to verification — only to a person looking at the
machine.

Two consequences follow in the code:

**A text-mode framebuffer is described and never mapped.** Where the boot loader
does leave the adapter in a text mode, it reports a framebuffer of the EGA text
kind whose address is the VGA text buffer. That memory is already owned and
already reached by the display driver, and a second mapping of it — with a
different memory type, and nothing to decide which name a later writer should use
— would be two names for one device. It is recorded so that the report can state
what mode the machine is in, and nothing more.

**The display self-test skips rather than adapts.** Every assertion it makes
reads a character cell back out of the text buffer, and in a graphics mode none
of them means anything. It states that it was skipped and why, which is the
treatment the serial and disk tests already give a device that is absent. A test
that quietly asserted less would be worse than one that says plainly it asserted
nothing.

It also had to move. It ran before the Multiboot2 parse until this sub-task,
needing nothing the handover had not supplied; it now runs after, because which
mode the machine is in is stated by the boot information and nowhere else.

## 8. Verification

`KernelVerifyFramebuffer` asserts the description, the mapping and the memory
type, and then writes to the framebuffer and reads it back.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A display was described at all | The request tag of `boot/boot.asm` is absent, malformed, or placed where the boot loader did not look. This is an assertion upon the header, not upon this file. |
| Address non-zero, extent non-zero, depth non-zero | The boot loader described a display that cannot exist. |
| Pitch ≥ the occupied width of a row | The rows overlap; the image would skew progressively down the screen. |
| Bytes per pixel rounds bits per pixel **upward** | A 15-bit pixel would be given one byte instead of the two the hardware stores it in. |
| A text-mode display is **not** mapped, reports no extent, and encodes no colour | The kernel took a second name for memory the display driver owns. |
| The mapped extent is pitch × height | The mapping is short, and the last rows are not addressable. |
| The whole range lies inside the kernel arena | It was mapped over the direct map or the kernel image. |
| The **first** page translates to the reported physical address | The mapping reaches something else entirely. |
| The **last** page translates to the last physical page | A loop advanced the virtual address and not the physical one — every page would point at the same frame, showing the first rows repeated down the screen. |
| The mapping is writable | The first write faults. |
| The page-table entry sets PAT and clears PCD and PWT | It selects an index other than 4, so the memory type is not the one that was configured. |
| `IA32_PAT` entry 4 holds `0x01` | The framebuffer is write-back; the display may lag memory indefinitely. |
| `IA32_PAT` entries 0–3 are `0x06`, `0x04`, `0x07`, `0x00` | **Existing mappings have had their memory type changed beneath them.** This is the assertion that the arrangement is safe, as distinct from effective. |
| Black encodes as zero; white encodes as non-zero | The channel positions or widths were misread. |
| The last pixel of the last row lies within the mapping | The extent is wrong by any amount at all. |
| A pixel written to that last pixel reads back | The mapping reaches no memory the adapter decodes. An address that translated correctly but named a region nothing decodes would accept every write and return ones, or zeroes, or the last value on the bus. |

The corner is used rather than the beginning deliberately: a mapping short by a
page passes every test made upon its start.

### 8.1 The assertion that cannot be made

**Nothing here establishes that anything appeared upon the screen.** A kernel
cannot read its own display back through the eye of whoever is looking at it, and
a framebuffer that is mapped, written and read back correctly may still be
scanned out by nothing at all.

That half of the verification is performed by a person, and the self-test paints
a pattern for them to judge: bands of red, green and blue across the top
sixteenth of the screen. It is composed so that looking at it establishes
something specific. Misread channel positions put the bands in the wrong order or
the wrong colours; a wrong pitch skews them instead of leaving them flat; a wrong
extent stops them short of the right-hand edge.
`docs/project/TESTING.md`, Section 15, records the procedure and the result.

### 8.2 The negative test

The self-test was confirmed capable of failing before it was trusted. The
write-combining encoding was changed to write-back, and the run reported
`entry 4 of IA32_PAT does not hold write-combining, so the framebuffer is
write-back and the display may lag the memory indefinitely` and ended
`Framebuffer self-test FAILED.` The edit was then reverted.

## 9. Observed state

Under QEMU with the q35 machine and the standard VGA adapter:

```
Framebuffer: RGB, 1280 by 800 pixels, 32 bits each, pitch 5120 bytes.
  Physical 0xFD000000, mapped at 0xFFFFC00000004000, 4000 KiB, write-combining.
  Red at bit 16 of 8, green at 8 of 8, blue at 0 of 8.
Framebuffer self-test passed.
Display self-test skipped; the adapter is in a graphics mode, which the framebuffer owns.
```

The pitch is exactly the width times four here, the adapter having asked for no
padding; that is a fact about this adapter and not one to rely upon. The channel
positions describe `0x00RRGGBB`, which is the common case and, again, is read
rather than assumed.

Under VirtualBox the same image is handed **640 by 480**, and the bands appear
correctly there too. That the two hypervisors disagree about the mode is the
useful part of the result: it is the evidence that nothing here is fitted to what
one of them happens to supply. It also leaves VirtualBox with no readable
diagnostic output at all — it has no serial adapter this kernel detects, and now
no console either — which is recorded in `docs/project/TESTING.md`, Section 9.1,
and which sub-task 6.4 ends.

## 10. Limitations of the framebuffer

1. **There was no visible console until sub-task 6.4.** See Section 7: for two
   sub-tasks the serial port carried everything and the screen carried the test
   pattern. Section 19 ends that.
2. **`IA32_PAT` is written upon one processor.** Every processor holds its own,
   and a mapping made here would be write-back upon any processor that had not
   performed the write. Sub-task 6.14 must repeat it upon each application
   processor as it is brought up.
3. **The memory type range registers are not programmed, and may override the
   PAT.** Intel SDM, Volume 3A, Table 11-7: the effective type combines both, and
   the more conservative prevails, so a region an MTRR marks uncacheable stays
   uncacheable however the PAT selects it. What this kernel controls is asserted;
   what the firmware left is not, and a machine whose firmware marks the
   framebuffer uncacheable will be correct and slow.
4. **Indexed framebuffers are recognised and refused.** The palette that follows
   the tag is not read and no colour can be resolved without it. Nothing this
   kernel has met reports one; a machine that did would be reported and left in
   text mode.
5. **The mode cannot be chosen.** See Section 2.1. Selecting one requires either
   a boot loader that honours a request or VESA BIOS mode setting performed by
   the kernel itself, which is a real-mode call this kernel has no means of
   making from long mode.
6. **The framebuffer is never unmapped.** It exists for the life of the machine,
   so `KernelDeviceUnmap` is exercised by nothing here. It is written and
   declared because the facility is general and Phase 11 will need the pair.
7. **Nothing was drawn but the test pattern**, and the pattern was written by
   the self-test rather than by any interface a caller could use. That interface
   is sub-task 6.3, in Section 11.

---

## 11. The primitives (sub-task 6.3)

Sub-task 6.2 supplied memory and facts about it. Sub-task 6.3 supplies the
operations that write into it: a pixel, a line, a filled and an outlined
rectangle, a blit, and the clipping that governs all of them.

## 12. The surface, and why the primitives do not name the framebuffer

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

## 13. Clipping is the memory-safety boundary

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

## 14. The line, and the one place this differs from the usual arrangement

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
per step. Section 16 asserts the promise directly, by drawing the line both ways
and comparing pixel for pixel.

## 15. The blit, and the direction of the copy

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
of sub-task 6.4 will be built on.

## 16. Verification of the primitives

`KernelVerifyGraphics` asserts against a surface composed in memory: 32 by 16
pixels of four bytes, **in rows of 40**.

The pitch exceeds the width deliberately, and the eight pixels of padding on each
row are filled with a sentinel no test ever writes. A primitive that stepped from
row to row by the width instead of the pitch would still write inside the array —
it would simply write the wrong pixels — and every assertion about the image would
then have to be relied upon to notice. The padding turns that into a direct
assertion, made after each operation so that a failure names the operation that
caused it.

### 16.1 Rectangles and the clip

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A rectangle of zero or negative extent is empty | A caller could ask for a region that runs backwards. |
| An intersection that misses is empty **and not negative** | A negative extent passes a `< width` loop bound by doing nothing and fails one computed as an end coordinate. |
| Rectangles that merely touch do not overlap | Every adjacent pair of regions would share a column. |
| A rectangle does not contain the column past its right edge | An off-by-one in the fundamental containment test, which everything else clips with. |
| A clip of `{-1000, -1000, 100000, 100000}` is confined to the surface | The clip could be widened past the surface, and no primitive checks it. |
| A clip wholly outside the surface is empty, and drawing against it writes nothing | Every primitive's cheapest rejection is broken. |

### 16.2 Pixels, fills and outlines

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

### 16.3 The line

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A horizontal line includes both endpoints | The commonest off-by-one here. |
| A line from a point to itself is one pixel, not none | The loop tests its termination before drawing. |
| A diagonal's length is its longer axis, and it passes through its own middle | The error accumulation is wrong. |
| A line drawn backwards lights **the same pixels** | Bresenham accumulates from one end; a careless implementation is not symmetric, and a shape's edges would depend upon the order they were drawn in. |
| **A clipped line lights exactly the pixels the unclipped line lights inside the clip** | Clipping moved the line. This is the promise of Section 14, asserted pixel for pixel: the unclipped line is drawn and its pixels inside the region recorded, the surface is cleared entirely, and the same line is drawn clipped. |
| A coordinate beyond the limit draws nothing | An unbounded loop. |

### 16.4 The blit

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Differing pixel sizes are refused | An image of the right size in the wrong colours. |
| A rectangle arrives whole and in position | The offset arithmetic is wrong. |
| A blit trimmed by the clip takes **the right part** of the source | A shifted image rather than a cropped one. The source is given a distinguishable left and right half so the two are told apart. |
| Moving rows **up** within one surface moves them | The copy read bytes it had already overwritten. |
| Moving rows **down** within one surface moves them | The row order was not reversed; the first row read would smear down the region. |
| A blit clipped away entirely returns success | An operation that correctly did nothing was reported as a refusal. |

### 16.5 What a person judges

The self-test also draws upon the framebuffer, and that figure is judged by eye;
`docs/project/TESTING.md`, Section 16, records what to look for. It is composed so
that looking at it establishes something: a frame around the whole screen, whose
vertical edges lean if the pitch is wrong; a panel with its diagonals crossing,
which meet at the centre only if the line is exact; a second panel drawn through
a clip covering its left half, where the fill and the line must stop dead at the
boundary without the line changing slope; and the first panel blitted below
itself, which must be identical and in the right place.

### 16.6 The negative test

`GraphicsPixelAddress` was changed to step by the width instead of the pitch. Six
assertions fired, in six independent primitives — the fill, the clipped fill, the
clear, the line, the blit and the trimmed blit — each reporting that it had
written into the row padding. The edit was then reverted.

## 17. Limitations of the primitives

1. ~~**There is no clip stack.**~~ Resolved by sub-task 6.6: `GraphicsPushClip`
   and `GraphicsPopClip`, four deep, intersecting rather than replacing. See
   Section 27.5.
2. ~~**There is no blending.**~~ Resolved by sub-task 6.6:
   `GraphicsBlendPixel` and `GraphicsBlendSurface`, combining the channels apart
   through `FramebufferDecode`. The outline's care not to write a corner twice,
   taken in anticipation of this, is now load-bearing. See Section 27.2.
3. **Lines are one pixel wide and unantialiased**, and there are no curves. A
   thicker line is several lines, which nothing needs yet.
4. **The blit does not scale.** Source and destination rectangles are the same
   size by construction.
5. **Nothing is safe against concurrent drawing.** From sub-task 6.13 two
   processors drawing upon one surface require that sub-task's lock. It is not
   taken here: a primitive is far too small a thing to own a lock, and the right
   place is the surface's owner.
6. ~~**No fast path uses the pixel size.**~~ Resolved in the course of sub-task
   6.4, by the measurement of Section 23, which is the specialisation this
   limitation said did not yet exist a justification for. A surface records
   `whole_words` — four bytes to the pixel, a word-aligned base and a pitch that
   is a multiple of four — and where it holds, the pixel, the fill, the pattern
   block and the blit each take a word-wide loop of their own rather than a test
   inside the byte loop. The byte path remains and is what a surface of any other
   depth still uses.

---

## 18. The face (sub-task 6.4)

A framebuffer is pixels, and a boot log is text. Something has to turn one into
the other, and this is it: ninety-five glyphs covering the printable range of
ANSI X3.4-1986, `0x20` to `0x7E`, compiled into the image as `graphics/font.c`.

### 18.1 Why it was drawn rather than obtained

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

### 18.2 The cell, and why it is eight wide

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
character beside it. That is an assertion, not a convention; see Section 21.

Each glyph is eight bytes, one to a row, **most significant bit leftmost**, so a
row of pixels reads left to right exactly as the picture comment beside it is
drawn. The pictures are what make the table legible and they are comments:
nothing checks that a picture agrees with the bytes beside it. Change one and you
must change the other. The self-test asserts the bytes, and no test can tell you
that a letter looks wrong.

### 18.3 The replacement glyph

Every code outside the covered range yields a hollow box rather than nothing.
`FontGlyph` is therefore never `NULL` and its caller dereferences it without
checking, which is the point of it.

A font that drew a blank for a code it did not know would make a run of unmapped
characters indistinguishable from a run of spaces, and the fault would read as
missing output rather than as an unmapped character. The console relies upon this
for the control characters it does not implement: one that nothing meant to emit
appears as a box in the log, where it can be found.

### 18.4 Drawing, and what is not drawn

`FontDrawGlyph` sets only the pixels the glyph defines and leaves the rest of the
cell as it was. The background is the caller's business — a console fills the
cell before calling this; a caller drawing text over an image does not, and gets
the character stencilled upon what was already there. Drawing the background here
would be one pass instead of two and would make it impossible to draw a character
over anything, which is what a cursor does and what sub-task 6.6 duly wanted.

Every pixel goes through `GraphicsPutPixel`, so every pixel is clipped by
Section 13's boundary. A glyph at the edge of a surface is cut off rather than
wrapped or refused, and one wholly outside the clip costs sixty-four rejected
writes and touches nothing. That is the slow way to draw text and it is the
correct one; the fast way needs a clipped span, which is sub-task 6.6's problem
when there is something to measure.

## 19. The console

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

### 19.1 The fault that made the fan-out one place

`KernelWriteHexadecimal` and `KernelWriteDecimal` named `VgaWriteString` and
`SerialWriteString` themselves until this sub-task. The console was therefore
shown every word of the boot log and not one of its numbers — every address,
count and size simply absent from the screen while the same lines on the serial
port were complete.

It read as a formatting error in the messages rather than as a missing output
path, which is why it is written down. Both now emit through `KernelWriteString`,
and that is the only routine in the kernel permitted to name an output device.

### 19.2 The control characters

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
discarded, per Section 18.3.

#### 19.2.1 Crossing to the row above, and the record that makes it possible

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

### 19.3 The scroll, which is the blit paying for itself

Scrolling is one `GraphicsBlit` of the surface upon itself, followed by a fill of
the row exposed at the bottom.

This is the overlapping case that Section 15 chose a copy direction for. The
destination lies above the source, so the rows are taken from the top and nothing
reads a byte the copy has already overwritten. A console scroll is the reason
that direction logic was written, and this is the whole of the payment.

### 19.4 The replay buffer

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

### 19.5 The screen has one owner

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

## 20. Limitations of the font and the console

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
   the echo loop is legible without, and a shell is Phase 8. See Section 27.7.
4. **There are no colours per character.** `ConsoleSetColour` sets the pair used
   from that point onward; there is no attribute stored with a cell, so a scroll
   cannot repaint what it moved and does not need to.
5. **A scroll still moves the whole screen**, but no longer through the
   write-combining mapping: since sub-task 6.6 the blit reads and writes the back
   buffer, which is ordinary cached memory, and only the result is carried out.
   The damage of a scroll is everything, so a screenful is presented; the
   dirty-region scheme of Section 27.3 narrows every *other* operation to the
   cells it touched.
6. ~~**Nothing is buffered off-screen.**~~ Resolved by sub-task 6.6. Drawing goes
   to the back buffer and reaches the display only at a presentation. There is
   still no synchronisation with the adapter's vertical blank; see Section 27.7.
7. **There is no lock**, and no second thread of control writes here: the
   interrupt handlers do not print save through the panic path, which does not
   return. From sub-task 6.13 that ceases to be true and this must take that
   sub-task's lock — the whole of a character, not one pixel of it, being the
   thing that must not interleave.
8. **The picture comments are unchecked.** Nothing asserts that the art beside a
   glyph agrees with its bytes. See Section 18.2.
9. **The console is at most `CONSOLE_MAXIMUM_ROWS` rows tall.** The row lengths
   of Section 19.2.1 are `.bss` and therefore of a fixed size. Five hundred and
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

## 21. Verification of the font and the console

`KernelVerifyConsole` makes thirty-three assertions in three groups. The font and
its drawing are asserted against a surface composed in memory, as the primitives
of Section 16 are, so that the whole of that holds upon a machine with no display.
The control characters are asserted upon the live console, because the position
they move is the console's own and there is no second one to make.

### 21.1 The face

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

### 21.2 Drawing a glyph

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

### 21.3 The control characters, upon the live console

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
| BS at column 0 crosses to the row above **and lands after its text** | Section 19.2.1: the cursor at the edge of the display instead, where the erasure the caller composes is written far away from the text and backspacing over a line separator does nothing a person can see. |
| Three erasures then return to column 0 | The landing was right and the erasure that follows it is not, which is the half a position assertion alone would not reach. |
| BS crossing into a row filled to its last column stops upon that **final character** | The wrapped row treated as one that ended with a line feed. There is no separator there to consume, so the cursor would stop one position past the last character and the erasure would fall off the row. |
| Erasing a filled row returns to column 0 | The same, over the whole width of the display rather than at one position. |

### 21.4 What a person judges

That the log is legible is not something the kernel can assert. The procedure is
in `docs/project/TESTING.md`, Section 17: the whole boot log rendered on the
screen from its first line, **including every number**, which is what Section 19.1
is about.

### 21.5 The negative test

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
`docs/project/TESTING.md`, Section 17.3.

## 22. Observed state of the console

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

## 23. The optimisation, and the measurement that directed it

The console of Section 19 worked and was slow. This section is what was done
about that, and it begins with the measurement rather than with the change,
because the first two things that looked worth optimising were not the things
that cost.

### 23.1 Measuring it

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

### 23.2 What was actually wrong

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

### 23.3 What was done

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
depends upon the fast path for correctness, which is what Section 25.1 asserts.

**`GraphicsPatternBlock` replaced sixty-four clip tests with one.** It draws a
block eight pixels wide and any number of rows high, taking each pixel's colour
from one of two by a bit of a pattern — one byte to a row, most significant bit
leftmost. That is exactly the shape of a bitmap glyph, so the font table is
handed over as it stands, with no copying and no transformation.

**`FontDrawGlyphOpaque` draws the cell and the glyph in one pass.** The console
uses it; `FontDrawGlyph` remains for text drawn over an image, where what is
behind the character must show through. The two must light the same ink pixels
and Section 25.2 asserts that they do, for every glyph in the face — the console
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

### 23.4 The strict-aliasing question, and why the test surfaces changed type

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

## 24. The fault screens

A framebuffer took the operator's view away in sub-task 6.2 and Section 19 gave
it back for the boot log. It did not give it back for the thing a person most
needs to see, which is what happened when the machine stopped.

Every fatal fault has always been reported in full, upon the serial port and upon
the text display. In a graphics mode the text display is not shown, and a serial
adapter is not something most machines have — VirtualBox does not. Such a person
saw the boot log stop, and nothing else.

### 24.1 What draws one, and what does not

**A fault screen is drawn for a fault the kernel cannot survive, and for no
other.** That decision is not made here; it is made by `ExceptionDispositionOf`,
and `docs/design/INTERRUPTS.md`, Section 8.1, is where it is set out.

The distinction was got wrong first and the correction matters. Every exception
was treated as fatal, so a divide by zero — the plainest mistake a program can
make, and one that must cost that program and nothing else — would have halted
the machine and drawn a full-screen page announcing it. A screen that says the
system has stopped, shown for a fault that ought to have ended one program, is
not a cosmetic error: it is a false account of what happened, given to the person
least able to check it.

What reaches this file is therefore only:

- an **abort** — a double fault or a machine check — which the architecture
  permits no resumption from, whatever raised it;
- a **non-maskable interrupt**, which is hardware announcing a condition rather
  than a program erring;
- a **malformed descriptor table**, `#TS` or `#NP`, whose faulty structure is the
  kernel's own however it was reached;
- **any fault the kernel raised within itself**, where there is no program to
  blame and nothing smaller than the machine to abandon;
- a **panic** the kernel raised by its own check.

A divide by zero, an invalid opcode, an unresolved page fault or an alignment
check raised by a program draws nothing here at all. From Phase 7 the program
ends and the machine carries on.

The titles say so. They are `KERNEL PAGE FAULT` and `KERNEL PROTECTION FAULT`,
not `PAGE FAULT` and `GENERAL PROTECTION FAULT`, because by the time one is on
the screen that is what it is.

### 24.2 Why there is more than one screen

Because the faults are not one thing.

A page fault names an address and asks what was meant to be mapped there. A
general-protection fault names a selector, or names nothing at all and is then
about the instruction. An invalid opcode asks what the bytes at the instruction
pointer are. A double fault says that the processor could not deliver something
else, and that **the earlier fault is the one worth finding** — the screen for it
says so in those words, because a reader looking at a double fault's registers is
looking at the wrong fault.

One screen carrying one register dump would present all of that identically and
would tell the reader nothing about which question to ask. So each severe fault
has its own title, its own colour, a sentence saying what the processor is
reporting, a sentence saying what to examine first, and **the evidence that bears
upon that fault and not upon the others**.

| Vector | Screen | Evidence it carries |
| --- | --- | --- |
| 2 NMI | Non-maskable interrupt | The control registers |
| 6 `#UD` | Bad instruction in kernel | The instruction bytes, and the stack |
| 8 `#DF` | Double fault | The stack, and the control registers |
| 10 `#TS` | Malformed task state segment | The selector |
| 11 `#NP` | Descriptor not present | The selector |
| 12 `#SS` | Kernel stack fault | The selector, and the stack |
| 13 `#GP` | Kernel protection fault | The selector, and the instruction bytes |
| 14 `#PF` | Kernel page fault | The faulting address and its cause, and the control registers |
| 18 `#MC` | Machine check | The control registers |
| — | Kernel panic | The message naming the check that failed |

A vector outside that table which is nevertheless fatal receives the general
screen, `UNEXPECTED KERNEL FAULT`. It names the exception from the dispatcher's
own mnemonics, carries the general evidence, and says plainly that this kernel
has no account written for it — adding that most exceptions reaching it are
ordinary mistakes of a program, so arriving there means the kernel made one. That
is what a divide by zero within the kernel gets, and it is the right treatment: a
kernel that divides by zero has a bug, and the useful facts are the instruction,
the stack and the registers, not a lecture about arithmetic.

**`#AC` has no screen and must not have one.** Intel SDM, Volume 3A, Section
6.15: an alignment check requires privilege level 3, `CR0.AM` and `RFLAGS.AC`
together, so kernel code cannot raise one however it is written. A screen for it
would be a page nobody could ever see and a claim that the kernel treats a
program's mistake as the end of the machine. Section 25.3 refuses one.

The panic is separate from all of them deliberately. An exception is the machine
saying something went wrong; a panic is this kernel saying it has found the world
in a state it does not know how to continue from, and the message names the check
that failed rather than any register.

### 24.3 What it must survive

Every routine runs inside a fault handler, upon a machine that has already gone
wrong, and the one thing it must not do is go wrong itself: a fault raised while
drawing a fault screen is a double fault, and one raised while handling that
resets the machine with nothing written anywhere.

**It allocates nothing.** The heap is a thing the fault may have corrupted.

**It reads nothing without asking the paging hierarchy first.** The instruction
bytes and the stack words both go through `PagingTranslate`, and a word that does
not translate is reported as absent rather than fetched. That report is often the
most useful thing on the page: "the stack pointer names memory that is not
mapped" *is* the diagnosis.

**It draws once.** A second call means a fault occurred while the first screen
was being drawn, and overwriting the first would destroy the only account of the
original failure.

**It draws nothing where there is no framebuffer**, that being the case in which
the display driver is already showing the report.

### 24.4 The screen changes hands

The console is suspended when a fault screen begins, and this was not foreseen —
it was found by looking at a screen that had been drawn correctly and displayed
wrongly.

`KernelPanic` follows every fatal exception, and it writes to the diagnostic
path, and the diagnostic path includes the console, and the console is upon this
framebuffer. Its cursor stood at the foot of a screen full of boot log, so each
newline of `KERNEL PANIC: ...` **scrolled the whole framebuffer up by eight
pixels**. Three newlines carried the banner off the top of the display and
shifted the entire layout by three character rows.

`ConsoleSuspend` ends it: the console stops drawing and stops recording, and the
display driver and the serial port go on receiving everything. There is no
resumption, a machine that has drawn a fault screen being one that is halting.

### 24.5 The two demonstrations, which prove different things

`fault-screen=<vector>` composes a trap frame and draws that vector's page. It
proves **the page**: that its text fits the display, that its panels lay out one
beneath another, that its colour and title are its own. It proves nothing about
the processor, and the frame is filled with values no machine would produce —
repeated nibbles, and an obviously artificial address — so that a photograph of
it cannot be filed as evidence of a fault that occurred.

`fault-raise` writes to an unmapped address, which raises a genuine page fault.
That proves **the wiring**: handler, report, screen, end to end. A page fault is
used because it is the one severe fault that can be raised deliberately without
endangering the machine — a double fault is raised by destroying the stack, and a
machine check cannot be asked for at all.

The two are kept apart because they answer different questions and because
confusing them would let a broken handler pass a test of the drawing.

## 25. Verification of the optimisation, the disposition and the fault screens

### 25.1 The word path

A fast path is the most dangerous kind of code to leave unasserted: it runs only
when its own precondition holds, so a fault in it is invisible upon every surface
that does not meet the condition — and the surface the self-tests use and the
surface a person looks at are not the same surface.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A four-byte surface on a word boundary with a word-multiple pitch **is** marked word-addressable | The fast path never runs, and the measurement above was of nothing. |
| A surface whose base and pitch are both odd is **not** | Every row would be written misaligned. |
| A three-byte pixel is **not**, whatever its alignment | A write would spill into the pixel beside it. |
| **The word path and the byte path draw identical pixels** | One of them is wrong and the tests see only the other. Two surfaces differing in nothing but alignment are drawn upon and compared pixel for pixel. |
| A pattern block reproduces its own bits, most significant leftmost | The bit order or the row order is wrong; every character would be mirrored. |
| A pattern block writes the **paper** as well as the ink | A console cell would keep the character drawn there before it. |
| A clipped block draws the bits that survive, not the bits from the start of the pattern | It was shifted to the clip rather than trimmed by it. |
| A block outside the clip, of no rows, or with no pattern writes nothing | The cheapest rejections are broken. |

### 25.2 The two glyph routines agree

For **every glyph in the face**, `FontDrawGlyph` and `FontDrawGlyphOpaque` must
light the same ink pixels. The console changed from one to the other, so no other
test here uses the path every character of the boot log actually goes through; a
difference between them would be a difference nothing else could see. The whole
face is checked rather than a sample, the fault being of exactly the kind that
afflicts one character and no other.

### 25.3 The disposition

The disposition is asserted first, because the screens depend upon it and because
it cannot be exercised any other way: half of it concerns faults raised at
privilege level 3, and there is no code outside the kernel to raise one until
sub-task 6.10. `ExceptionDispositionOf` is a pure function of a vector and a code
segment selector, so it can be asked the question for a privilege level that does
not yet exist.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A privilege level 3 selector is recognised as outside the kernel, and a privilege level 0 one is not | Every kernel fault would be blamed upon a program, or every program's fault upon the kernel. |
| `#BP` and `#OF` resume | A trap would halt the machine, and `INT3` would cease to be usable as a marker. |
| NMI, `#DF`, `#MC`, `#TS`, `#NP` are fatal at **both** privilege levels | An abort or a corrupt descriptor table would be treated as one program's problem, leaving the machine running on a structure known to be wrong. |
| `#DE`, `#BR`, `#UD`, `#SS`, `#GP`, `#PF` and `#AC` **terminate** the program at privilege level 3 | **This is the assertion this section exists for.** Its failure is the machine halting for a mistake that should have cost one program — the fault this kernel actually had. |
| The same seven are **fatal** at privilege level 0 | A fault the kernel raised within itself would be blamed upon a program that does not exist. |
| No vector is treated more leniently within the kernel than outside it | The kernel would survive something a program would not, which is backwards. |
| Every vector has one of the three dispositions | A vector falls through the classification entirely. |
| `#AC` is recognised as raisable only outside the kernel, and `#PF` is not | The rule below would forbid a screen that is needed, or permit one that can never be drawn. |

### 25.4 The fault screen table

This asserts the table and not the drawing, for the reason Section 8.1 gives
about the display generally: whether a page reads well is not something a kernel
can determine about itself. What is asserted is everything a person reading one
screen would not notice.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Every entry has a title, an account and a direction | A screen draws a blank space where the one thing the page was for should be. |
| **No two entries share a title** | A copied row with the vector changed and the identity not. The reader cannot tell the faults apart, which is the whole purpose of having more than one screen. |
| **No two entries share a colour** | The same, at a glance rather than on reading. |
| No two entries claim the same vector | One of them is unreachable. |
| Every vector that is fatal **whatever raised it** has an entry of its own | A deleted entry falls back to the general screen, which still names the vector and so does not look broken — it is merely less useful than it was, silently. |
| **No screen exists for a fault that is never fatal** | A page nobody could ever see, and a claim that the kernel treats as the end of the machine something that costs one program. |
| **No screen exists for a fault the processor raises only outside the kernel** | The same, argued from the architecture rather than from the disposition: `#AC` needs all of privilege level 3, `CR0.AM` and `RFLAGS.AC`, so it can never be the kernel's. |
| Every character of every sentence is one the font can draw | Text that renders as replacement boxes. This caught a real fault: an em dash reached a string literal, which in UTF-8 is three bytes none of which the face covers, and the sentence rendered with three boxes in the middle of it — visible only to somebody looking at the page, at the worst possible moment. |
| There is an entry for a panic the kernel raises itself | It would be shown a screen written for a processor exception that did not occur. |
| Every title fits a **640-pixel** display at the scale used there | The title runs off the edge upon VirtualBox and not upon QEMU, so whichever machine the person judging the screens did not use is where it is broken. |
| The evidence flags name only panels that exist | A screen carries no evidence and looks exactly like one meant to carry none. |
| No screen has been drawn when the self-test runs | A real fault later in the boot would find the display taken and draw nothing — invisible precisely when it matters. |
| An index past the end yields nothing | The table is read past its own end. |

Nothing here draws, and that is deliberate: drawing would set the flag recording
a screen as shown, and a real fault later in the same boot would then be refused
the display by the test meant to protect it.

### 25.5 The negative tests

**The word path.** The alignment conditions were removed from `whole_words`, so
that every four-byte surface claimed the fast path. The run reported `a surface
whose base and pitch are both odd was marked as addressable by words, so every
row would be written misaligned` and ended `Graphics self-test FAILED.`

**The pattern block.** It was made to skip its clear bits, as the transparent
glyph does. Three assertions fired in the graphics self-test and a fourth in the
console self-test, the last naming the code point at which the two glyph routines
first disagreed — `the two glyph routines disagree, at code 0x20`.

**A duplicated screen.** Two entries were given the same title and colour, as a
copied row would be. The run reported `two fault screens share a title, at vectors
0xA and 0xB` and `two fault screens share a colour, at vectors 0xA and 0xB`.

**A deleted screen.** The machine-check entry was removed. The run reported `a
fault that threatens the kernel has no screen of its own, at vector 0x12` — the
fault this catches being precisely the one that would otherwise look like nothing
at all.

**Every fault made fatal**, which is what this kernel did before the disposition
existed. The privilege-level test was removed from `ExceptionDispositionOf`. The
run named all seven vectors in turn — `a program's own fault would halt the
machine rather than the program, at vector 0x0`, and the same for `0x5`, `0x6`,
`0xC`, `0xD`, `0xE` and `0x11`. Vector 0 is the divide by zero, which is the
example the fault was reported with.

**A screen for a fault that can never be the kernel's.** The alignment check was
given an entry again. The run reported `a screen exists for a fault the processor
raises only outside the kernel, at vector 0x11`. Worth recording that the weaker
form of this rule — asking merely whether the vector is ever fatal — did **not**
catch it, `#AC` being nominally fatal from a kernel selector; the architectural
fact had to be stated before the assertion had any force.

**An undrawable character.** An em dash was put back into one screen's text. The
run reported `a fault screen's text holds a character the font cannot draw, at
vector 0x8, code 0xE2` — the first byte of its UTF-8 encoding.

Every edit was reverted.

---

## 26. The pointer of sub-task 6.5

The device that moves the pointer is documented in
[`../devices/MOUSE.md`](../devices/MOUSE.md), which also carries the shape and
every assertion made upon it — and, as history, the save-under that sub-task 6.6
removed. What belongs here is what the pointer says about the drawing this
document describes.

*This section was written at sub-task 6.5, when there was one surface and no back
buffer. Section 27 is what became of it, and the parts superseded there are
marked where they stand rather than deleted: the argument is why the compositor
was necessary.*

### 26.1 It is the first thing that composites

Everything drawn until now has been drawn once and left. The console writes a
glyph; the fault screen fills a page; neither has any obligation to what was
underneath, because nothing was. A pointer is the first object that must be
**removable** — drawn over whatever is there, and then taken away leaving that
whatever intact.

With one surface and no back buffer there is exactly one way to do it: read the
pixels back before drawing and write them again afterwards. That is what
`graphics/cursor.c` does, and it is the first use in this kernel of
`GraphicsPixelAt` outside a self-test.

Section 6 declared that routine to exist "for the self-test", and noted that
nothing in a drawing path reads the surface it draws upon. That is no longer
true, and the reason it stopped being true is worth keeping: a compositor reads.
The remark stands as written for Sections 6 to 25, and this is where it ends.

### 26.2 What the surface abstraction bought

The pointer draws through `GraphicsPutPixel` and `GraphicsPixelAt` and names no
framebuffer. Two consequences follow directly, and both were the argument for
the abstraction in Section 11:

- The pointer is asserted upon a surface composed in memory, pixel by pixel,
  upon a machine with no display — including the assertion that it writes nothing
  into the row padding, which a framebuffer could not be asked.
- Sub-task 6.6 substitutes a back buffer for the framebuffer and the pointer
  needs no change to be composited into it instead.

### 26.3 The cost, and why it is bounded

Reading is the expensive half. The framebuffer is mapped write-combining and
reads from write-combining memory are uncached — the same fact that made the
scroll the costly operation in Section 23.2. The pointer is 12 by 18, so a move
costs 216 uncached reads and up to 216 writes.

Three things keep that from mattering:

1. **A move is per movement, not per packet.** The echo loop drains the mouse's
   event buffer and calls `CursorMoveTo` once. A hundred packets a second
   describe a path the eye cannot follow, and drawing the intermediate positions
   would pay the cost a hundred times to show nothing.
2. **A move to where the pointer already is costs nothing.** A stationary mouse
   still sends button packets at the full rate; without the early return the
   pointer would be erased and redrawn a hundred times a second for as long as
   nobody moved it.
3. **The pointer is shown only once the boot log is finished.** Each of those
   several hundred lines would otherwise conceal and reveal it — reading the
   pixels back each time — for a pointer nobody is yet moving.

### 26.4 The screen now has three owners, and the rule is unchanged

Section 19.5 established that the screen has one owner at a time: the console
holds it, the drawing figures take it, and the fault screens take it for good.
The pointer does not take it. It is a fourth party that draws **over** whoever
holds it and must get out of the way when they draw.

`CursorConceal` and `CursorReveal` are that arrangement, and
`KernelWriteString` is where it is applied, being already the one routine
permitted to name an output device — the same fan-out point Section 19.1
established for the same kind of reason. The pair is counted rather than a flag
because a panic raised from within a write nests inside it.

The fault screens do not conceal the pointer; they hide it. Concealment implies
a reveal, and there will be none: the machine has stopped, and the last thing
drawn upon the display should be the fault screen alone.

### 26.5 What 6.6 removed

*Written before sub-task 6.6 and left as it was written, the answer following.*

> The save-under is a single-surface expedient and is expected to disappear. With
> a back buffer the display is redrawn each frame and the pointer is composited
> last, so nothing needs saving, nothing needs concealing, and
> `KernelWriteString` stops knowing that a pointer exists.
>
> What survives into 6.6 is the shape, the two-mask encoding of it, and the
> position — which belongs to the mouse driver and never belonged here. That is
> the test of whether this division was drawn in the right place, and it will be
> answered one sub-task from now rather than argued here.

**It was answered exactly so.** `CursorConceal`, `CursorReveal`, the concealment
count, the save-under store and its coordinates are gone; `KernelWriteString` no
longer names the pointer; and what remains in `graphics/cursor.c` is the two
bitmaps, the conversion of them into a surface and a mask, and a position it
reflects from the mouse driver. The division was drawn in the right place, and
the prediction is left standing above so that a reader may check the claim rather
than take it. See Section 27.4.

## 27. The compositor of sub-task 6.6

Four sub-tasks deferred something here, and all four deferred the same thing.

| Deferred by | What was deferred | Section |
| ----------- | ----------------- | ------- |
| 6.3 | A clip **stack**; a caller nesting regions saved and restored the clip by hand | 17.1 |
| 6.3 | **Blending**; every colour was opaque and a pixel was written, not combined | 17.2 |
| 6.4 | A **dirty-region** scheme; a scroll redrew the whole screen | 20.5 |
| 6.4 | **Double buffering**; drawing was visible as it happened | 20.6 |
| 6.4 | A **text cursor** the console could remove again | 20.3 |
| 6.5 | The **save-under**, and the concealment that made it safe | 26.5 |
| 6.4 | The framebuffer **read-back** of a scroll, which is the expensive half | 23.3 |

They are one thing because they have one cause: **the kernel drew directly upon
the framebuffer**, so anything that had to appear over something else had to
remember what was beneath it, and anything that wanted to read what was there had
to read it back through a mapping in which reads are uncached.

A back buffer removes the cause rather than the symptoms. The display is composed
in ordinary memory and carried to the adapter; what is beneath a thing is simply
still there.

### 27.1 What was substituted for what

`ConsoleInitialise` asked `GraphicsSurfaceFromFramebuffer` for a surface. It now
asks `CompositorSurface`. That is the whole of the change to the console's
drawing, and it is one line, because **a surface owns nothing**: it describes
memory somebody else supplied, and every primitive, every glyph and the scroll
itself go wherever it points without knowing which they got.

That was the argument for the abstraction in Section 11, made four sub-tasks
before there was anything to spend it on. This is the sub-task that collects.

Where there is no compositor — no framebuffer, or an arena that cannot supply the
pages — the console takes the framebuffer as it did before and behaves exactly as
it did before. A machine with no display is not a machine with a broken display.

### 27.2 The layers are composited at presentation, not into the buffer

A layer drawn *into* the back buffer would have to be undone before the next
frame, which is the save-under again under a different name.

Compositing during the copy costs nothing extra. The pixels of the changed region
are being written to the framebuffer anyway; a pixel a layer covers is simply
composed differently on its way out. The back buffer is never disturbed, so what
a layer covered needs no restoring — it was never overwritten.

**Nothing reads the framebuffer.** The colour beneath a layer comes from the back
buffer, which is ordinary write-back memory. The framebuffer is written and never
read, which is the whole of the gain over Section 23.2's arrangement, where a
scroll read four megabytes back through a mapping in which reads are uncached.

A layer is a surface, a position, and an optional **mask of one coverage byte per
pixel**. The mask is what makes a shape of arbitrary outline possible without a
colour reserved to mean "absent" — a reserved colour is a colour the shape may
not contain, and the pointer contains black and white, which are the two any
caller would reach for. The coverage is a byte and not a bit so that a shape with
a soft edge needs no change here, only a table with values between.

### 27.3 The damage, and why it is one rectangle

Regions accumulate as the rectangle enclosing them, not as a list.

Two cells at opposite corners of the screen therefore present the whole screen,
which is this scheme's cost. What it buys is that **no amount of drawing can
exhaust it**. A list needs a bound, and a compositor that had run out of entries
would have to present everything anyway — so the fragmenting scheme's worst case
is this scheme's ordinary one, and this one has no bookkeeping.

A presentation empties the damage. A presentation with nothing damaged writes
nothing and is not an error: it is what the echo loop does between keystrokes.

**A movement must mark both places.** A layer that marked only where it had
arrived would leave its previous appearance standing until something else
happened to change those pixels — which upon a pointer crossing a static screen
is a trail, and is precisely the fault the save-under was written to prevent.
Getting this wrong reintroduces the bug the sub-task exists to remove.

### 27.4 What this took out of the pointer

The pointer of sub-task 6.5 kept the 216 pixels it covered and put them back
before each move. That store was correct only while nothing else drew, so the
rest of the kernel had to declare that it was about to: `KernelWriteString`
concealed a pointer it had no business knowing existed, the concealment had to
nest, and the fault screens had to hide what they could not reveal.

None of it remains. `CursorConceal`, `CursorReveal`, the concealment count, the
save-under store and its coordinates are all gone, and `KernelWriteString` has
stopped knowing that a pointer exists. The pointer is rendered **once** into a
surface of its own with a coverage mask beside it, and a movement is one call
that marks two rectangles.

Section 26.5 predicted that what would survive was "the shape, the two-mask
encoding of it, and the position — which belongs to the mouse driver and never
belonged here", and said the prediction was the test of whether the division had
been drawn in the right place. It survived exactly that and nothing else.

### 27.5 Verification

Two groups, and the division is deliberate. The clip stack and the blending are
asserted **against surfaces composed in memory**, as every primitive since
sub-task 6.3 has been, so the whole of that holds upon a machine with no display.
The compositor itself cannot be: it owns one back buffer and one framebuffer and
there is no second of either to compose.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A push **intersects** the clip in force and does not replace it | A push that could widen the clip, so that a caller nesting a panel within a region it was handed draws outside that region |
| A pop restores what the matching push saved, and a pop with nothing pushed is refused | An underflow taking a rectangle from beyond the array, after which every drawing is clipped to whatever that held |
| The stack is bounded and a push beyond it is refused | An overflow overwriting the entry beneath, so that a pop restores the wrong region |
| A reset abandons the saved clips | A later pop narrowing the surface to a region long since finished with |
| Full coverage is exactly an opaque write; no coverage writes nothing | Everything drawn before this sub-task depends upon the first being unchanged; the second is the case where a read of the destination must not happen |
| The channels are combined **apart** | A packed pixel interpolated whole carries out of one channel into the next: red over blue produces a green neither contains |
| A masked surface arrives as the shape the mask describes | The mask indexed by the destination's pitch rather than the source's width, which shears the shape by the difference each row |
| Damage accumulates as the rectangle enclosing both regions | A compositor keeping only the latest leaves the earlier standing upon the display for ever |
| Damage outside the buffer is discarded | A region widened to cover pixels that do not exist, and a presentation that walks off the end of the buffer |
| A presentation empties the damage, and one with nothing damaged carries nothing | Every presentation writing the whole screen, which is the dirty-region scheme not working at all |
| The layer table refuses more than its capacity, and a layer with no surface | A layer written past the end of the table |

**The self-test gives back every layer it takes.** The first form of it did not,
and the consequence was a machine that booted with no pointer and reported no
failure: the table was exhausted by the test, `CursorInitialise` could not get a
layer, and every routine in the pointer then correctly did nothing. A self-test
that consumes a bounded resource must return it, or it is testing a machine
nobody else will ever run.

### 27.6 What only looking establishes

That the pointer leaves no trail, that text beneath it is intact after it has
passed, and that a fault screen is not carried away by the next presentation.
The procedure is in [`../project/TESTING.md`](../project/TESTING.md), Section 20.

The last of those is worth stating as a rule rather than a case. **A fault screen
draws straight upon the framebuffer**, the machine having stopped and the back
buffer holding a boot log that is no longer what should be on the screen — so
`FaultScreenBegin` suspends the compositor, permanently. Without that the next
`KernelWriteString`, and the panic path makes several, would carry the back
buffer over the top of the page just drawn. That is the fault of Section 24's
console scrolling the screen out from under a fault screen, arriving again by a
different route, and it is the reason there is no resumption: everything that
suspends the display has stopped the machine.

### 27.7 Limitations

1. **One damaged rectangle.** Section 27.3. Two changes at opposite corners
   present everything between them.
2. **No text cursor yet.** Section 20.3 named this sub-task as the one with the
   compositing needed to remove a text cursor again, and the compositing now
   exists — a cursor is a layer, and a layer is removed by hiding it. Nothing
   yet has a use for one: the echo loop does not need it, and a shell is Phase 8.
3. **The presentation is synchronous.** It happens where it is asked for and
   nothing waits for the adapter's vertical blank, so a large presentation can be
   seen to arrive. There is nothing to synchronise against without an interrupt
   from the adapter, which the Multiboot2 framebuffer does not offer.
4. **Layers are composited pixel by pixel.** The back buffer is copied by words
   and the layers are not, because a layer has a mask and a mask is per pixel.
   The pointer is 216 pixels, so this has nothing yet to buy.
5. **The back buffer is the size of the display and is never resized.** The mode
   is fixed by the boot loader and this kernel never changes it.
6. **Nothing is safe against concurrent drawing.** From sub-task 6.13 two
   processors may draw at once, and the back buffer, the damage and the layer
   table all become the spinlock's business.
