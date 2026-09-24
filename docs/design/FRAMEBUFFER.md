<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Framebuffer

**Phase**: sub-task 6.2 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../graphics/framebuffer.c`](../../graphics/framebuffer.c),
[`../../kernel/include/oxys/gfx/framebuffer.h`](../../kernel/include/oxys/gfx/framebuffer.h),
[`../../kernel/handoff/multiboot2.c`](../../kernel/handoff/multiboot2.c),
[`../../boot/boot.asm`](../../boot/boot.asm), `KernelDeviceMap` in
[`../../kernel/mm/vmm.c`](../../kernel/mm/vmm.c).
**Specifications**: Multiboot2 Specification 2.0, Sections 3.1.10 and 3.6.12;
Intel SDM, Volume 3A, Sections 11.12.2–11.12.4 and Tables 11-7, 11-10, 11-11;
Volume 2A, `CPUID`.

The linear framebuffer the boot loader supplies: how it is asked for, what is
validated about it, how it is mapped and with what memory type, and how a colour
is encoded into it. Drawing is [`DRAWING.md`](DRAWING.md); text is
[`CONSOLE.md`](CONSOLE.md); the back buffer is [`COMPOSITOR.md`](COMPOSITOR.md).

## 1. Asking for one

The Multiboot2 header carries the framebuffer request tag (type 5, Section
3.1.10) with width, height and depth zero (no preference) and **the optional flag
set** ([`BOOT.md`](BOOT.md)). The kernel boots without a framebuffer, so declaring
one required would refuse firmware that offers only text; GRUB also treats a
required framebuffer as an instruction to set a graphics mode in every entry.

**The mode is GRUB's choice.** GRUB 2.12 ignores `gfxpayload` for a Multiboot2
image (asked for `1024x768x32`, it gave 1280×800×32; asked for `text`, it gave
graphics), and `insmod all_video` changes only which driver GRUB picks a mode from.
`grub.cfg` therefore asks for nothing, and the kernel accepts whatever it is handed
and asserts what it was. QEMU gives 1280×800; VirtualBox, 640×480.

## 2. Reading the description

`kernel/handoff/multiboot2.c` reduces the framebuffer information tag (type 8,
Section 3.6.12) to the neutral `BootFramebuffer`, so the kernel depends on a
description of the machine, not on the boot protocol.

| Offset | Field | Bits |
| ------ | ----- | ---- |
| 8 | `framebuffer_addr` | 64 |
| 16 | `pitch` | 32 |
| 20 | `width` | 32 |
| 24 | `height` | 32 |
| 28 | `bpp` | 8 |
| 29 | `framebuffer_type` | 8 |
| 30 | `reserved` | **16** |
| 32 | Colour description | — |

`reserved` is sixteen bits: the specification's diagram is ambiguous, and the
reference `struct multiboot_tag_framebuffer_common` settles it. Reading the colour
description at 31 shifts every channel by one byte: a picture in the wrong colours,
not an error.

**Everything is validated where it is read**; any failure leaves the description
`BOOT_FRAMEBUFFER_NONE`: a zero address, extent or depth, a pitch narrower than a
row, a channel outside the pixel. These values become the base and stride the
drawing loops write through, and a bad one found mid-draw is far from its cause.

**The pitch is not width × bytes per pixel.** It is the distance between row
starts, and a loader may pad rows. Stepping by the occupied width shears the image
down the screen.

## 3. Memory type

An adapter scans the framebuffer out continuously and does not take part in cache
coherency, so its pages cannot be write-back:

| Type | Correct | Fast | |
| ---- | ------- | ---- | --- |
| Write-back | **No** | Yes | A write can sit in a cache line while the screen shows the old value, then appear later for no visible reason. |
| Uncacheable | Yes | No | Each write is a separate transaction. |
| Write-combining | Yes | Yes | Writes are gathered and issued together. |

**Write-combining is selected through PAT entry 4.** A page's memory type is
`IA32_PAT[(PAT << 2) | (PCD << 1) | PWT]`, PAT being bit 7 of a page-table entry.
`FramebufferEstablishWriteCombining` writes `0x01` (WC) to entry 4 by
read-modify-write, leaving the other seven untouched. Entries 0–3 are what every
existing mapping selects, so changing one would retype memory already in use;
entry 4 is reachable only with the PAT bit, which nothing else sets, and defaults
to write-back like entry 0, so no existing mapping can depend on it.

- **Every processor has its own `IA32_PAT`.** Each application processor repeats
  the write (`FramebufferEstablishWriteCombiningOnThisProcessor`, called from
  `SmpApplicationProcessorEntry` before its first diagnostic). Otherwise one page
  would have two memory types, which SDM 11.12.4 leaves undefined, and nothing
  would visibly go wrong.
- **Without PAT** (`CPUID` leaf 1), the framebuffer is mapped cache-disabled:
  correct and slow. `FramebufferWriteCombining` reports which.
- **Bit 7 means two things**: PS (large page) in a directory entry, PAT in a
  table entry. `PAGE_ENTRY_LARGE` and `PAGE_ENTRY_PAT` are both `0x080`. A walk
  that tests bit 7 for a large page must do so only at levels 2 and 3 of the
  hierarchy, not in a page-table entry.

## 4. Mapping

`KernelDeviceMap` maps device memory into the kernel arena without allocating or
freeing any frame: the memory belongs to the device, and the frame allocator must
never hand it out. Device pages are counted apart from allocated ones. An
unaligned physical address is mapped from its page, and the returned pointer keeps
the offset.

A framebuffer of the EGA text type (the adapter left in text mode) is described
and **never mapped**: that memory is the VGA driver's ([`../devices/DISPLAY.md`](../devices/DISPLAY.md)),
and two mappings with different memory types would be two names for one device.

## 5. Encoding a colour

`FramebufferEncode` packs 8-bit red, green and blue into the pixel layout the
loader reported: positions and widths are read, not assumed (`0x00RRGGBB` is
common, not guaranteed). A narrower channel keeps its **high** bits: `0xFF` in five
bits must be `0x1F`; keeping the low bits would make everything dark and banded.

## Verification

`KernelVerifyFramebuffer` in [`../../kernel/test/gfx/framebuffer.c`](../../kernel/test/gfx/framebuffer.c).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A display was described. | The header tag missing or malformed. |
| Address, extent and depth are non-zero; pitch ≥ row width. | An impossible display; overlapping rows. |
| Bytes per pixel rounds bits per pixel up. | A 15-bit pixel given one byte. |
| A text-mode display is not mapped, has no extent, encodes no colour. | A second name for the VGA driver's memory. |
| The mapped extent is pitch × height, inside the kernel arena. | A short mapping; a mapping over other memory. |
| The first and **last** pages translate to the right physical pages. | A loop advancing only the virtual address, repeating the first rows. |
| The mapping is writable; its entry sets PAT and clears PCD and PWT. | A fault on first write; a different memory type. |
| `IA32_PAT` entry 4 is `0x01`; entries 0–3 are `0x06`, `0x04`, `0x07`, `0x00`. | A write-back framebuffer; **existing memory retyped** (the safety, as distinct from the effect). |
| Black encodes to zero, white to non-zero. | Misread channels. |
| The last pixel of the last row is inside the mapping, and a value written there reads back. | A wrong extent; an address nothing decodes. The corner catches a mapping one page short. |

Nothing here can show that the screen displays anything. When the self-test runs
with a framebuffer it paints red, green and blue bands across the top sixteenth of
the screen for a person to judge: wrong channels give wrong colours or order; a
wrong pitch skews the bands; a wrong extent stops them short
([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)).

The log on QEMU q35:

```
Framebuffer: RGB, 1280 by 800 pixels, 32 bits each, pitch 5120 bytes.
  Physical 0xFD000000, mapped at 0xFFFFC00000004000, 4000 KiB, write-combining.
  Red at bit 16 of 8, green at 8 of 8, blue at 0 of 8.
```

## Limitations

1. The MTRRs are not programmed; where firmware marks the framebuffer
   uncacheable, it stays so (SDM Table 11-7): correct and slow.
2. Indexed (palette) framebuffers are refused; the palette is not read.
3. The mode cannot be chosen (Section 1).
4. The framebuffer is never unmapped; `KernelDeviceUnmap` exists but nothing here
   uses it.
