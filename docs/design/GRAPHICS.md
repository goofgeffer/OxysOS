# The Framebuffer

**Corresponding phase**: 6, sub-task 6.2, which opens the graphical work.
**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.
**Implemented by**: [`../../graphics/framebuffer.c`](../../graphics/framebuffer.c),
[`../../kernel/include/oxys/framebuffer.h`](../../kernel/include/oxys/framebuffer.h),
[`../../kernel/multiboot2.c`](../../kernel/multiboot2.c),
[`../../boot/boot.asm`](../../boot/boot.asm),
[`../../kernel/mm/vmm.c`](../../kernel/mm/vmm.c).
**Asserted by**: `KernelVerifyFramebuffer` in
[`../../kernel/test/verify_framebuffer.c`](../../kernel/test/verify_framebuffer.c).

**Specifications**: Multiboot2 Specification 2.0, Sections 3.1.10 and 3.6.12;
Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
Sections 11.12.2 and 11.12.3, and Tables 11-7, 11-10 and 11-11; Volume 2A,
`CPUID`.

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

This is a real cost and it is not disguised. **Until sub-task 6.4 supplies a
console that can draw text upon a framebuffer, the screen shows the colour bands
the self-test paints and nothing else.** The whole boot log continues to be
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

## 10. Present limitations

1. **There is no visible console until sub-task 6.4.** See Section 7. The serial
   port carries everything; the screen carries the test pattern.
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
7. **Nothing is drawn but the test pattern**, and the pattern is written by the
   self-test rather than by any interface a caller could use. That interface is
   sub-task 6.3.
