<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `boot/` — Bootstrapping and Boot Loader Configuration

**Phase**: 1, sub-tasks 1.3 to 1.6 and 1.9; and 6.14, which placed
`trampoline.asm` here for the reason the whole directory exists.
**Detailed design**: [`../docs/design/BOOT.md`](../docs/design/BOOT.md), and
[`../docs/design/SMP.md`](../docs/design/SMP.md) for the trampoline.

## Purpose

This directory holds everything that executes between the boot loader's handover
and the first instruction of the C kernel: the Multiboot2 header by which GRUB
recognises the image, the 32-bit protected-mode entry point, the construction of
the boot-time paging hierarchy, the transition to 64-bit long mode, the transfer
of control to the higher half, and the GRUB configuration embedded in the ISO. It
also holds the trampoline of sub-task 6.14, which is the same journey made a
second time, by an application processor, long after the kernel is running.

Code in this directory is unique in one respect: most of it executes **before
paging is enabled**, and is therefore linked at its physical load address rather
than at a higher-half virtual address. This constraint governs its structure and
is the reason it cannot simply be merged into `kernel/`.

`trampoline.asm` shares that property and takes it furthest. A processor
answering a startup inter-processor interrupt begins in 16-bit real mode, so the
code must execute from a physical page below the first mebibyte and every address
it names must be that page's. NASM's `org` directive states that origin and is
available only in the flat binary format, so the file is assembled with
`nasm -f bin` to `build/trampoline.bin` and is **not** a member of `ASM_SOURCES`.
`kernel/arch/x86_64/smp/smp_trampoline.asm` embeds the result with `incbin` between two
symbols the C of `kernel/arch/x86_64/smp/smp.c` takes the size from, and the `Makefile`'s
dependency upon `$(TRAMPOLINE_BINARY)` is what guarantees the image cannot be out
of step with the source it came from: the link fails if it was not assembled.

## Contents

| File | Description |
| ---- | ----------- |
| `boot.asm` | The Multiboot2 header, carrying the framebuffer request tag of sub-task 6.2 alongside the terminating tag; the entry point `_start`; `CPUID` and long-mode feature detection; `BootBuildPageTables`; `BootEnableLongMode`; the 64-bit trampoline `BootLongModeEntry`; the higher-half entry point `KernelEntryHigh`; and, in `.boot.data`, the boot GDT and the boot-time paging structures. |
| `trampoline.asm` | The real-mode trampoline of sub-task 6.14, which an application processor begins executing when it answers a startup inter-processor interrupt. It carries that processor from 16-bit real mode through 32-bit protected mode into 64-bit long mode upon the kernel's own paging hierarchy, and hands it to `SmpApplicationProcessorEntry`. It holds `SmpTrampolineParameters`, the block the bootstrap processor fills in before each start. **It is assembled to a flat binary and not to an object file**, for the reason the note beneath **Purpose** gives; `kernel/arch/x86_64/smp/smp_trampoline.asm` embeds the result in the kernel image. |
| `grub/grub.cfg` | The GRUB configuration embedded within the ISO image, defining the boot menu entries. Staged into the image by the `iso` target of the `Makefile`. Since sub-task 7.7 every entry also carries a `module2` line loading `/boot/initrd.img` under the name `initrd`, which is the root filesystem; see `../docs/storage/INITRD.md`. Since the change of 2026-09-15 recorded in `../docs/project/HISTORY.md` the default entry boots with a quiet display — the boot log goes to the serial line alone until the shell starts — and the "diagnostics" entry shows the log upon the screen; it replaced the "serial console diagnostics" entry, whose option nothing read. Since sub-task 9.1 the default entry gives the screen to the window manager and the shell runs upon the serial line; the "Shell-only" entry gives the shell the screen as the default did before, and the "diagnostics" entry is named "Shell Diagnostics", both at the project owner's direction — `../docs/design/WINDOWS.md`. Since sub-task 9.3 the default entry is quiet from before the banner and shows a boot screen from the moment the compositor exists until the desktop composes over it, where it had shown the banner and then nothing; the other two entries are unchanged — `../docs/design/INIT.md`. |

## Sequence of execution

```
GRUB  --->  _start  --->  BootLongModeEntry  --->  KernelEntryHigh  --->  KernelMain
            32-bit        64-bit, identity      64-bit, higher half   (kernel/kernel.c)
            paging off    mapped low memory
```

The intermediate trampoline exists because the far jump that enters 64-bit mode
encodes a 32-bit offset and cannot name an address in the upper half of the
address space. `docs/design/BOOT.md` sets out the reasoning.

The application processors of sub-task 6.14 make the same journey a second time,
from a different starting point and into a kernel that is already running:

```
Startup IPI  --->  SmpTrampolineReal  --->  SmpTrampolineProtected  --->  SmpTrampolineLongMode  --->  SmpApplicationProcessorEntry
vector 0x08        16-bit real,             32-bit, paging off           64-bit, kernel CR3          (kernel/arch/x86_64/smp/smp.c)
                   CS normalised to 0
```

`docs/design/SMP.md` sets out each step and what it would cost to
omit it.

## Specifications implemented

| Specification | Sections | Applied to |
| ------------- | -------- | ---------- |
| Multiboot2 Specification 2.0 | 3.1.1, 3.1.2, 3.1.3, 3.1.10 | The header layout, the magic fields, the terminating tag, and the framebuffer request tag carried since sub-task 6.2. |
| Multiboot2 Specification 2.0 | 3.3 | The machine state relied upon at entry: `EAX`, `EBX`, the segment registers, the A20 gate, `CR0` and `EFLAGS`. |
| Intel SDM, Volume 2A, "CPUID" | — | Feature detection: `EFLAGS.ID`, and leaf `0x80000001` `EDX` bit 29. |
| Intel SDM, Volume 3A | 4.1.2, Table 4-14 | The control-register sequence entering IA-32e mode. |
| Intel SDM, Volume 3A | 4.5, Figure 4-8, Table 4-15 | The four-level paging hierarchy, the index decomposition and the entry flags. |
| Intel SDM, Volume 3A | 3.4.5, Figure 3-8 | The segment descriptor format and the `L` flag of a 64-bit code segment. |
| Intel SDM, Volume 3A | 8.4.3, 8.4.4.1 | `trampoline.asm`: the address a processor begins at for a startup interrupt carrying vector `VV`, and the sequence that sends it. |
| Intel SDM, Volume 3A | 9.1.4, Table 9-1 | `trampoline.asm`: the processor state after a reset, which is what it must start from. |
| Intel SDM, Volume 3A | 9.9.1 | `trampoline.asm`: the descriptor table loaded before `CR0.PE`, and the far jump that follows. |
| Intel SDM, Volume 3A | 2.5 | `trampoline.asm`: `CR0.WP`, which is per processor and must be set again here. |
| GNU GRUB Manual | 6, 16.192 | The configuration file, and the module providing the `multiboot2` and `module2` commands. The second number was `16.3.16` until sub-task 7.7 and named nothing in the current manual; `../docs/project/REFERENCES.md` records the correction. |

Full citations are held in [`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## Diagnostics

A failure before the display driver exists cannot report itself by ordinary
means. The failure paths in `boot.asm` therefore write a single character
directly to the VGA text buffer at physical `0x000B8000` and halt.

| Character | Meaning |
| --------- | ------- |
| `M` | `EAX` did not hold `0x36D76289`; the loader is not Multiboot2 compliant. |
| `C` | The `CPUID` instruction is unavailable. |
| `L` | The processor does not implement the Intel 64 architecture. |

The character appears in white upon a red background in the upper-left corner of
an otherwise unmodified display.

## Constraints upon changes to this directory

1. The Multiboot2 header must remain within the first 32768 bytes of the image
   and 8-byte aligned. `linker.ld` guarantees this by placing
   `.multiboot_header` first; that ordering must not be disturbed.
2. Code in `.boot.text` executes with paging disabled and must not reference a
   higher-half symbol.
3. `KernelEntryHigh` executes with paging enabled and must not be reached by any
   path that encodes its address in fewer than 64 bits.
4. The boot-time paging structures are emitted as initialised zero data rather
   than reserved in `.bss`, so that they occupy a defined physical location
   before the loader has zeroed anything.
5. `.boot.text` and `.boot.data` occupy separate program headers, one readable
   and executable and the other readable and writable. Code placed in
   `.boot.data`, or data placed in `.boot.text`, would defeat that division; a
   new boot-time table belongs in `.boot.data` and new boot-time code in
   `.boot.text`. See [`../docs/design/BOOT.md`](../docs/design/BOOT.md).
6. `trampoline.asm` is assembled at a fixed origin that must equal
   `SMP_TRAMPOLINE_ADDRESS` in `kernel/include/oxys/arch/smp/smp.h`, and its parameter
   block must match `SmpTrampolineParameters` in that same header. Neither
   agreement is checked by any compiler: the magic value at the head of the block
   is what proves them at run time, and a change to either side that does not
   change the other is caught there or not at all. The image must also fit one
   4 KiB page, which the file asserts at assembly time with `%error`.
