# `kernel/` — The Kernel Core

**Phase**: 1, sub-tasks 1.7 and 1.8. This directory grows in every subsequent
phase.
**Detailed design**: [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) and
[`../docs/MEMORY-LAYOUT.md`](../docs/MEMORY-LAYOUT.md).

## Purpose

This directory holds the architecture-independent core of the monolithic kernel:
the C entry point, and, as the phases proceed, memory management, the interrupt
dispatcher, the scheduler, the system-call layer and the virtual filesystem. All
code here executes in 64-bit long mode at a higher-half virtual address, with
paging already enabled — which distinguishes it from `boot/`, and is why the two
are separate.

## Contents

| Path | Description |
| ---- | ----------- |
| `kernel.c` | `KernelMain`, the C entry point: it validates the boot loader handover, initialises the early output devices, presents the identification banner and halts. `KernelPanic`, the unrecoverable-error path. `KernelHalt`, `KernelWriteString` and `KernelWriteHexadecimal`. |
| `mm/paging.c` | The permanent kernel paging hierarchy. `PagingInitialise`, `PagingTranslate`, `PagingAddressIsWritable` and the reporting routines. |
| `include/oxys/paging.h` | The paging-structure entry flags and the interface of the paging subsystem. |
| `mm/pmm.c` | The physical frame allocator. `PhysicalMemoryInitialise`, `FrameAllocate`, `FrameAllocateBelow`, `FrameFree` and the accounting accessors. |
| `include/oxys/memory.h` | `PAGE_SIZE`, `PAGE_SHIFT`, `LOW_MEMORY_LIMIT` and the alignment helpers. |
| `include/oxys/pmm.h` | The interface of the physical frame allocator. |
| `multiboot2.c` | `BootInformationParseMultiboot2`, which walks the Multiboot2 tag series and reduces it to the neutral description; `BootInformationReport`, which emits that description. |
| `include/oxys/multiboot2.h` | The raw on-memory layout of the Multiboot2 structure and of the tags the kernel consumes, together with the ELF64 section header. |
| `include/oxys/bootinfo.h` | `BootInformation`, the boot-protocol-neutral description of the machine, and its classification of memory regions. |
| `include/oxys/types.h` | The fixed-width integer types, and the distinct address types `PhysicalAddress` and `VirtualAddress`. |
| `include/oxys/kernel.h` | `KERNEL_VIRTUAL_BASE`, the address translation helpers `PhysicalToVirtual` and `VirtualToPhysical`, and the declarations of `KernelMain` and `KernelPanic`. |
| `include/oxys/io.h` | `PortReadByte`, `PortWriteByte` and `IoWait`: the accessors for the x86 programmed input/output address space. |
| `include/oxys/vga.h` | The interface of the VGA text-mode driver implemented in `drivers/vga/`. |
| `include/oxys/serial.h` | The interface of the COM1 serial driver implemented in `drivers/serial/`. |

## The header corpus

`kernel/include/oxys/` is the kernel's internal header corpus, and is the sole
directory named by `-Ikernel/include` in the `Makefile`. Headers are included as
`<oxys/name.h>`, never by a relative path, so that a file's location does not
determine how it names its dependencies.

Driver interfaces are declared here rather than beside their implementations in
`drivers/`, so that the kernel core depends upon an interface and not upon a
particular driver's directory. The implementations are free to move.

## Entry conditions of `KernelMain`

`KernelMain` is called by `KernelEntryHigh` in `boot/boot.asm`. On entry:

- The processor is in 64-bit long mode.
- Paging is enabled, with the first gibibyte of physical memory mapped both
  identically and at `0xFFFFFFFF80000000`.
- A 64 KiB stack, reserved in `.bss`, is installed, and `RBP` is zero.
- Interrupts are masked; no interrupt descriptor table exists.
- `RDI` holds the physical address of the Multiboot2 information structure and
  `RSI` the Multiboot2 magic value, per the System V AMD64 calling convention.

`KernelMain` does not return. Should it nevertheless do so, the caller halts the
processor permanently.

## Specifications implemented

| Specification | Sections | Applied to |
| ------------- | -------- | ---------- |
| Multiboot2 Specification 2.0 | 3.3, 3.6.1, 3.6.2, 3.6.7, 3.6.8 | The validation of the handover; the tag series and its alignment rule; the ELF sections tag; the memory map tag and its region types. |
| System V ABI, AMD64 supplement | 3.1.2, 3.2.3 | The LP64 data model and the argument registers. |
| ISO/IEC 9899:2011 | 4 ¶6, 6.7.9 ¶4, 7.18, 7.20 | The freestanding environment, constant initialisers, and the fixed-width and boolean types. |
| Intel SDM, Volume 1 | 18.3 | The programmed input/output address space. |
| Intel SDM, Volume 2B, "HLT" | — | The halt instruction and the conditions that resume it. |
| Intel SDM, Volume 3A | 3.3.7.1, 4.5 | Canonical addressing, and the higher-half translation helpers. |

Full citations are held in [`../docs/REFERENCES.md`](../docs/REFERENCES.md).

## Present limitations

1. `PhysicalToVirtual` and `VirtualToPhysical` are valid only for physical
   addresses below one gibibyte, that being the extent of the higher-half
   mapping that the permanent hierarchy establishes. Phase 2, sub-task 2.4,
   introduces the direct physical map and extends their domain to the whole of
   physical memory. Until then `FrameAllocateBelow` must be used for any frame
   the kernel intends to address.
2. There is no formatted output. `KernelWriteHexadecimal` is a deliberate
   minimum, to be superseded when the C library of Phase 7 exists.
4. The frame allocator is not yet safe against concurrent access; from sub-task
   6.9 its bitmap and search hint require a spinlock. Nothing here is yet safe
   against concurrent access. Every structure
   introduced from Phase 2 onward must record its locking discipline in its
   defining file's header, as `docs/ARCHITECTURE.md`, Section 1, requires.
