# `boot/` — Bootstrapping and Boot Loader Configuration

**Phase**: 1, sub-tasks 1.3 to 1.6 and 1.9.
**Detailed design**: [`../docs/design/BOOT.md`](../docs/design/BOOT.md).

## Purpose

This directory holds everything that executes between the boot loader's handover
and the first instruction of the C kernel: the Multiboot2 header by which GRUB
recognises the image, the 32-bit protected-mode entry point, the construction of
the boot-time paging hierarchy, the transition to 64-bit long mode, the transfer
of control to the higher half, and the GRUB configuration embedded in the ISO.

Code in this directory is unique in one respect: most of it executes **before
paging is enabled**, and is therefore linked at its physical load address rather
than at a higher-half virtual address. This constraint governs its structure and
is the reason it cannot simply be merged into `kernel/`.

## Contents

| File | Description |
| ---- | ----------- |
| `boot.asm` | The Multiboot2 header; the entry point `_start`; `CPUID` and long-mode feature detection; `BootBuildPageTables`; `BootEnableLongMode`; the 64-bit trampoline `BootLongModeEntry`; the higher-half entry point `KernelEntryHigh`; the boot GDT and the boot-time paging structures. |
| `grub/grub.cfg` | The GRUB configuration embedded within the ISO image, defining the boot menu entries. Staged into the image by the `iso` target of the `Makefile`. |

## Sequence of execution

```
GRUB  --->  _start  --->  BootLongModeEntry  --->  KernelEntryHigh  --->  KernelMain
            32-bit        64-bit, identity      64-bit, higher half   (kernel/kernel.c)
            paging off    mapped low memory
```

The intermediate trampoline exists because the far jump that enters 64-bit mode
encodes a 32-bit offset and cannot name an address in the upper half of the
address space. `docs/design/BOOT.md`, Section 7, sets out the reasoning.

## Specifications implemented

| Specification | Sections | Applied to |
| ------------- | -------- | ---------- |
| Multiboot2 Specification 2.0 | 3.1.1, 3.1.2, 3.1.3 | The header layout, the magic fields and the terminating tag. |
| Multiboot2 Specification 2.0 | 3.3 | The machine state relied upon at entry: `EAX`, `EBX`, the segment registers, the A20 gate, `CR0` and `EFLAGS`. |
| Intel SDM, Volume 2A, "CPUID" | — | Feature detection: `EFLAGS.ID`, and leaf `0x80000001` `EDX` bit 29. |
| Intel SDM, Volume 3A | 4.1.2, Table 4-14 | The control-register sequence entering IA-32e mode. |
| Intel SDM, Volume 3A | 4.5, Figure 4-8, Table 4-15 | The four-level paging hierarchy, the index decomposition and the entry flags. |
| Intel SDM, Volume 3A | 3.4.5, Figure 3-8 | The segment descriptor format and the `L` flag of a 64-bit code segment. |
| GNU GRUB Manual | 6, 16.3.16 | The configuration file and the `multiboot2` command. |

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
