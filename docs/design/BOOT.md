<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Boot Sequence

**Phase**: sub-tasks 1.3 to 1.6 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../boot/boot.asm`](../../boot/boot.asm),
[`../../linker.ld`](../../linker.ld),
[`../../boot/grub/grub.cfg`](../../boot/grub/grub.cfg).
**Specifications**: Multiboot2 Specification 2.0, Sections 3.1, 3.3 and 3.6;
Intel SDM, Volume 3A, Sections 3.4.5, 4.1.2 and 4.5; Volume 2A, `CPUID`.

How control passes from the firmware to `KernelMain`: the Multiboot2 header
GRUB loads the image by, the checks made before leaving 32-bit mode, the
boot-time paging hierarchy, the entry into long mode, and the jump to the
higher half.

## 1. Overview

```
Firmware (BIOS)
      v
GRUB 2, Multiboot2      Loads the ELF64 image at physical 0x00100000 and
      |                 establishes the machine state of Multiboot2 Section 3.3.
      v
_start                  32-bit protected mode, paging disabled.
      |                 Establishes a stack; preserves EAX and EBX; checks the
      |                 magic value, CPUID and Intel 64; builds the boot-time
      |                 paging hierarchy; sets CR4.PAE, IA32_EFER.LME, CR0.PG;
      |                 loads the 64-bit GDT and far-jumps.
      v
BootLongModeEntry       64-bit mode, identity-mapped low memory.
      |                 Loads the data selectors; jumps through a 64-bit register.
      v
KernelEntryHigh         64-bit mode, higher-half addresses.
      |                 Establishes the kernel stack; clears RBP; passes the
      |                 information address in RDI and the magic in RSI.
      v
KernelMain              C, System V AMD64 calling convention.
```

## 2. The Multiboot2 header

The header is the section `.multiboot_header`, which `linker.ld` places first in
the image, so that it lies within the first 32,768 bytes and on an 8-byte
boundary (Multiboot2, Section 3.1).

| Offset | Field | Value | Multiboot2 |
| ------ | ----- | ----- | ---------- |
| 0 | `magic` | `0xE85250D6` | 3.1.2 |
| 4 | `architecture` | `0`, i386 protected mode | 3.1.2 |
| 8 | `header_length` | 48 | 3.1.2 |
| 12 | `checksum` | Makes the sum of the first four fields zero, modulo 2^32 | 3.1.2 |
| 16 | framebuffer tag | Type 5, flags 1, size 20; width, height, depth 0 | 3.1.10 |
| 36 | padding | Four bytes, to align the next tag on 8 bytes | 3.1.3 |
| 40 | end tag | Type 0, flags 0, size 8 | 3.1.3 |

- **The framebuffer tag** asks the loader for a linear framebuffer; zero width,
  height and depth mean no preference. Its **optional flag is set**, because the
  kernel boots without one: the text display, the serial port and a framebuffer
  initialisation that returns false are all there. Clearing it would declare the
  image unloadable without a framebuffer, which is untrue.
  [`FRAMEBUFFER.md`](FRAMEBUFFER.md).
- **No module-alignment tag** (Section 3.1.11). The initial ramdisk is read
  through the direct map at byte granularity, and `FrameMarkRange` reserves
  every frame a module touches, so alignment changes nothing. A tag that changes
  no behaviour is a tag someone will one day reason from.

## 3. What the loader supplies

The kernel reads these tags of the boot information structure:

| Tag | Multiboot2 | Used for |
| --- | ---------- | -------- |
| Command line | 3.6.1 | The boot options of [`CONFIG.md`](CONFIG.md) and [`INIT.md`](INIT.md). |
| Boot loader name | 3.6.2 | The boot report. |
| Module | 3.6.6 | The initial ramdisk ([`../storage/INITRD.md`](../storage/INITRD.md)). |
| ELF sections | 3.6.7 | The boot report. |
| Memory map | 3.6.8 | The frame allocator ([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md)). |
| Framebuffer | 3.6.12 | [`FRAMEBUFFER.md`](FRAMEBUFFER.md). |
| ACPI RSDP, old and new | 3.6.16, 3.6.17 | [`../devices/ACPI.md`](../devices/ACPI.md). |

**Modules are found by name, never by position.** `grub.cfg` loads
`/boot/initrd.img` under the name `initrd`. A kernel that took the first module
would load the wrong one, silently, the day a second is added, since every
module is just a range of bytes. Up to `BOOT_MODULE_MAXIMUM` (4) modules are
recorded, and any beyond are reported; recording only one would leave no set to
choose from by name.

**Module extents are reserved** from the frame allocator with the kernel image,
the boot information and the frame bitmap: the memory map reports them as
available (Section 3.6.8).

## 4. The machine state at entry

Multiboot2, Section 3.3, guarantees the following, and the kernel relies on each.

| Element | State | Use |
| ------- | ----- | --- |
| `EAX` | `0x36D76289` | Checked by `_start` and again by `KernelMain`. |
| `EBX` | Physical address of the boot information | Preserved and passed to `KernelMain`. |
| `CS` | 32-bit execute/read, base 0, limit 4 GiB | Code runs before the kernel's GDT exists. |
| `DS`, `ES`, `FS`, `GS`, `SS` | 32-bit read/write, base 0, limit 4 GiB | Flat data access. |
| A20 gate | Enabled | No A20 code is needed. |
| `CR0` | `PE` set, `PG` clear | Long mode may set `PG` directly. |
| `EFLAGS` | `VM` and `IF` clear | Interrupts are masked while there is no IDT. |

Everything else is undefined. In particular there is no stack, so `_start`
sets one before its first `call`.

## 5. Feature checks

Before the long-mode transition, `_start` makes three checks. A failure writes
one character to the VGA text buffer at physical `0xB8000`, which is addressable
while paging is off, and halts.

| Check | Method | Character |
| ----- | ------ | --------- |
| Multiboot2 handover | `EAX` equals `0x36D76289` | `M` |
| `CPUID` present | `EFLAGS.ID` (bit 21) can be toggled | `C` |
| Intel 64 present | Leaf `0x80000000` reports at least `0x80000001`, and leaf `0x80000001` sets `EDX` bit 29 | `L` |

## 6. The boot-time paging hierarchy

Long mode cannot be entered with paging off, so a hierarchy is built first,
mapping the first gibibyte of physical memory twice:

1. **At linear 0.** When `CR0.PG` is set the instruction pointer still holds a
   low address, and the next fetch must succeed.
2. **At `0xFFFFFFFF80000000`**, where the kernel is linked.

The permanent hierarchy built in sub-task 2.3 ([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md))
replaces this one and drops the identity mapping. The boot structures are then
unreferenced; their frames lie inside the kernel image and stay reserved. The
twenty kibibytes are not reclaimed, since freeing memory still in use is the
larger risk.

## 7. The long-mode transition

In the order of Intel SDM, Volume 3A, Section 4.1.2:

1. `MOV CR3, BootPml4`.
2. Set `CR4.PAE` (bit 5); four-level paging requires it.
3. Set `IA32_EFER.LME` (bit 8 of MSR `0xC0000080`).
4. Set `CR0.PG` (bit 31). The processor is now in IA-32e compatibility mode,
   because `CS.L` is clear.
5. `LGDT` a table of a null descriptor, 64-bit code at `0x08` and data at
   `0x10`.
6. `JMP 0x08:BootLongModeEntry`, which loads a code segment with `L` set and
   enters 64-bit mode.

## 8. The jump to the higher half

The far jump carries a 32-bit offset and cannot reach the upper half, so
`BootLongModeEntry` sits in the identity-mapped boot section, loads the data
selectors, and jumps through a register:

```
mov rax, KernelEntryHigh
jmp rax
```

`KernelEntryHigh` runs at its higher-half address. It sets the 64 KiB kernel
stack, clears `RBP` to end the frame-pointer chain, puts the information address
in `RDI` and the magic in `RSI`, and calls `KernelMain`. If `KernelMain` returns,
the processor halts with interrupts masked.

## 9. The program headers

A segment has one set of permissions. Left alone, the linker packs sections into
as few segments as it can with the union of their permissions, which produces a
segment both writable and executable. `linker.ld` therefore states the segments
in a `PHDRS` block:

| Segment | Flags | Holds |
| ------- | ----- | ----- |
| `boot` | `r-x` | The Multiboot2 header and the 32-bit entry code (`.boot`). |
| `bootdata` | `rw-` | The boot GDT, the preserved loader values, the boot stack, the boot paging structures (`.boot.data`). |
| `text` | `r-x` | The 64-bit kernel code. |
| `rodata` | `r--` | Constants, string literals, read-only tables. |
| `data` | `rw-` | Initialised data, then `.bss`. |

Every output section carries `ALIGN(4K)` twice: before the colon it sets the
section's address; after it, the section's alignment, which becomes the
segment's. Without the second, a segment inherits the largest input alignment
(16 or 32 bytes) and its file offset stops being congruent to its address modulo
a page, as `p_align` requires; a loader could then only copy the segment, not map
it with its permissions.

These flags are a declaration, not an enforcement: GRUB copies segments without
applying them, and the mappings that enforce permissions are the ones
`PagingInitialise` builds ([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md)). The
declaration is for the mapping loader of Phase 12 and for every tool that reads
the image.

## Verification

Boot is verified by reaching the rest of the self-test: `make verify` fails if
the completion banner is absent, and nothing reaches the banner without every
stage above. `KernelMain` checks the magic value again before using `EBX`.

| Property | The failure it would catch |
| -------- | -------------------------- |
| The serial log begins with the boot report, naming the loader and the command line. | A header GRUB rejects, or a lost `EBX`. |
| `readelf -l build/oxys.elf` lists five `LOAD` segments, none `RWE`, each page-aligned in address and offset. | A section placed in the wrong segment. |
| The initial ramdisk self-test ([`../storage/INITRD.md`](../storage/INITRD.md)) finds the module named `initrd`. | A module found by position, or its frames handed out. |

## Limitations

1. The boot-time paging structures are not reclaimed (Section 6).
2. The failure characters `M`, `C` and `L` are shown only on a VGA text
   display; a machine without one halts silently.
3. There is no UEFI path until Phase 12.
