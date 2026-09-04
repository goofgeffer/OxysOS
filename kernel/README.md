# `kernel/` — The Kernel Core

**Phase**: 1, sub-tasks 1.7 and 1.8. This directory grows in every subsequent
phase.
**Detailed design**: [`../docs/design/ARCHITECTURE.md`](../docs/design/ARCHITECTURE.md),
[`../docs/design/MEMORY-LAYOUT.md`](../docs/design/MEMORY-LAYOUT.md) and
[`../docs/design/PRIVILEGE.md`](../docs/design/PRIVILEGE.md).

## Purpose

This directory holds the architecture-independent core of the monolithic kernel:
the C entry point, and, as the phases proceed, memory management, the interrupt
dispatcher, the apparatus of a privilege transition, the scheduler, the
system-call layer and the virtual filesystem. All code here executes in 64-bit long mode at a higher-half virtual address, with
paging already enabled — which distinguishes it from `boot/`, and is why the two
are separate.

## Contents

| Path | Description |
| ---- | ----------- |
| `kernel.c` | `KernelMain`, the C entry point: it validates the boot loader handover, initialises every subsystem of Phases 1 to 5 and the privilege apparatus of sub-task 6.1 in dependency order, runs the boot-time self-tests, mounts a volume the machine carries at the root, and enters the keyboard echo loop where a keyboard is present or halts where none is. `KernelPanic`, the unrecoverable-error path. `KernelHalt`, `KernelWriteString`, `KernelWriteDecimal`, `KernelWriteHexadecimal`, `KernelCommandLineHasOption` and `KernelMountRootVolume`. |
| `test/` | The boot-time self-tests, one file per subsystem, and the composed volume they are conducted upon. Described by [`test/README.md`](test/README.md). |
| `include/oxys/verify.h` | The self-test entry points `KernelMain` calls, in the order it calls them; the parsed boot information; and the reading of the boot loader's command line. |
| `include/oxys/testvolume.h` | The fixture: the two memory-backed block devices, the geometry of the composed EXT2 volume, and the routines that address a field of it directly. |
| `cpu/exceptions.c` | The exception handlers, the decoding of both error-code formats, `ExceptionInstallInterruptStacks`, which gives the double fault a stack of its own, and `ExceptionReportState`. |
| `include/oxys/exceptions.h` | The error-code flags of both formats, the vector of the double fault, and the exception interface. |
| `include/oxys/cpu.h` | Accessors for the control registers `CR0`, `CR2`, `CR3` and `CR4`, and for `RFLAGS`, by which a driver determines whether an interrupt it means to wait for could be delivered at all. |
| `cpu/interrupt_stubs.asm` | The 256 per-vector stubs, the common stub, and the table of stub addresses. |
| `cpu/interrupts.c` | `InterruptInitialise`, `InterruptDispatch`, the dispatch table and its registration interface, and the frame reporting routines. |
| `cpu/gdt.c`, `cpu/gdt.asm` | The kernel global descriptor table, the user-mode descriptors at the displacements `SYSCALL` and `SYSRET` derive their selectors by, the run-time construction of the sixteen-byte task state segment descriptor, and the segment reload. |
| `include/oxys/interrupts.h` | The trap frame and the interrupt interface. |
| `include/oxys/gdt.h` | The `LGDT` operand, the kernel, user and task state segment selectors, the requested privilege level a user selector carries, and the interface of the descriptor table. |
| `cpu/idt.c` | The interrupt descriptor table. `IdtInitialise`, `IdtSetGate`, `IdtSetGateStack`, `IdtGateStack`, and the accessors that read the table register back with `SIDT`. |
| `include/oxys/idt.h` | The 64-bit gate descriptor, the `LIDT` operand, the gate types, the attribute bits, and the assignment of an interrupt stack table entry to a gate. |
| `cpu/tss.c` | The task state segment: the stack the processor loads upon a transfer to privilege level 0, the separate stack a double fault is delivered upon, the sixteen-byte descriptor built for it within the global descriptor table, and the loading of the task register. `TssInitialise`, `TssSetKernelStack`, `TssKernelStack`, `TssInterruptStack`, `TssIoMapBase`, `TssAddress`, `TssLimit`, `TssTaskRegister`, `TssReport`. |
| `include/oxys/tss.h` | `TaskStateSegment`, its size asserted at compile time; the interrupt stack table entry the double fault is given; the sizes of the two stacks; and the interface of the segment. |
| `cpu/syscall.c` | The configuration of the fast system-call mechanism: the establishment of processor support by `CPUID`, the selectors written into `IA32_STAR`, the entry point written into `IA32_LSTAR`, the flags cleared by `IA32_FMASK`, the enabling of `IA32_EFER.SCE`, and the derivation of the four selectors the processor computes from `IA32_STAR`. `SyscallInitialise`, `SyscallIsEnabled`, `SyscallStar`, `SyscallLstar`, `SyscallFmask`, `SyscallEntryAddress`, `SyscallDerivedKernelCode`, `SyscallDerivedKernelStack`, `SyscallDerivedUserCode`, `SyscallDerivedUserStack`, `SyscallEntries`, `SyscallObservedCode`, `SyscallObservedStack`, `SyscallObservedFlags`, `SyscallReport`. |
| `cpu/syscall_entry.asm` | `SyscallEntry`, the address `IA32_LSTAR` holds. Provisional in sub-task 6.1: it records the selectors and flags the processor loaded, which exist nowhere else, and returns by restoring `R11` and jumping to `RCX`. Sub-task 6.7 replaces it. |
| `include/oxys/syscall.h` | The named bits of `RFLAGS`, `SYSCALL_FLAG_MASK` composed from them, and the interface by which the configuration is read back from the processor and asserted. |
| `include/oxys/msr.h` | The numbers of the model-specific registers the kernel uses, the `SCE` bit of `IA32_EFER`, and `ReadMsr` and `WriteMsr`. |
| `mm/heap.c` | The kernel heap. `KernelAllocate`, `KernelAllocateZeroed` and `KernelFree`. |
| `mm/vmm.c` | The kernel virtual address allocator. `KernelPagesAllocate` and `KernelPagesFree`. |
| `include/oxys/heap.h` | The interface of the kernel heap. |
| `include/oxys/vmm.h` | The kernel arena constants, the interface of the virtual address allocator, and the mapping of memory the kernel does not own: `KernelDeviceMap` and `KernelDeviceUnmap`. |
| `include/oxys/graphics.h` | The interface of the drawing primitives implemented in `graphics/`: the rectangle, the surface and its clip, and the pixel, line, rectangle and blit operations upon them. |
| `include/oxys/framebuffer.h` | The interface of the framebuffer implemented in `graphics/`: whether a display was supplied and whether it can be drawn upon, its address, extent, pitch and pixel layout, the encoding of a colour into that layout, and whether the mapping is write-combining. |
| `mm/paging.c` | The permanent kernel paging hierarchy and copy-on-write. `PagingInitialise`, `PagingTranslate`, `PagingAddressIsWritable`, `PagingMarkCopyOnWrite`, `PagingResolveCopyOnWriteFault` and the reporting routines. |
| `include/oxys/paging.h` | The paging-structure entry flags and the interface of the paging subsystem. |
| `mm/addrspace.c` | The address space: creation, cloning by copy-on-write, activation and destruction. `AddressSpaceCreate`, `AddressSpaceClone`, `AddressSpaceDestroy`, `AddressSpaceSwitch`, `AddressSpaceMapPage` and the accounting accessors. |
| `include/oxys/addrspace.h` | `AddressSpace`, the division of the root table between the two canonical halves, and the interface of address-space cloning. |
| `mm/pmm.c` | The physical frame allocator and per-frame reference counting. `PhysicalMemoryInitialise`, `FrameAllocate`, `FrameAllocateBelow`, `FrameFree`, `FrameReferenceInitialise`, `FrameReferenceIncrement` and the accounting accessors. |
| `include/oxys/memory.h` | `PAGE_SIZE`, `PAGE_SHIFT`, `LOW_MEMORY_LIMIT` and the alignment helpers. |
| `include/oxys/pmm.h` | The interface of the physical frame allocator. |
| `multiboot2.c` | `BootInformationParseMultiboot2`, which walks the Multiboot2 tag series and reduces it to the neutral description; `BootInformationReport`, which emits that description. |
| `include/oxys/multiboot2.h` | The raw on-memory layout of the Multiboot2 structure and of the tags the kernel consumes, together with the ELF64 section header. |
| `include/oxys/bootinfo.h` | `BootInformation`, the boot-protocol-neutral description of the machine, and its classification of memory regions. |
| `include/oxys/types.h` | The fixed-width integer types, and the distinct address types `PhysicalAddress` and `VirtualAddress`. |
| `include/oxys/kernel.h` | `KERNEL_VIRTUAL_BASE`, the address translation helpers `PhysicalToVirtual` and `VirtualToPhysical`, and the declarations of `KernelMain` and `KernelPanic`. |
| `include/oxys/io.h` | `PortReadByte`, `PortWriteByte` and `IoWait`: the accessors for the x86 programmed input/output address space. |
| `fs/ext2.c` | The EXT2 superblock, block group descriptor table, inode and directory: their reading through the buffer cache, their decoding from the volume's byte order into the processor's, the resolution of a file's block index through the direct and indirect pointers, the traversal of a directory's entries, the resolution of an absolute path to the inode it names, the reading of a file's contents and of a symbolic link's target, the bitmaps and the allocation of blocks and inodes, the writing and truncation of a file, the insertion and removal of directory entries and the creation and destruction of files and directories, and the validation that decides whether a volume may be read, may be written, or may not be addressed at all. `Ext2ReadSuperblock`, `Ext2ReadGroupDescriptor`, `Ext2VerifyGroupDescriptors`, `Ext2ReadInode`, `Ext2InodeBlock`, `Ext2ReadFile`, `Ext2ReadSymbolicLink`, `Ext2DirectoryNext`, `Ext2DirectoryFind`, `Ext2ResolvePath`, `Ext2ResolvePathNoFollow`, `Ext2AllocateBlock`, `Ext2AllocateInode`, `Ext2WriteFile`, `Ext2TruncateFile`, `Ext2WriteInode`, `Ext2DirectoryInsert`, `Ext2DirectoryRemove`, `Ext2CreateFile`, `Ext2CreateDirectory`, `Ext2Link`, `Ext2Unlink`, `Ext2RemoveDirectory`, `Ext2ReportVolume`, `Ext2ReportGroup`, `Ext2ReportInode`, `Ext2ReportDirectory`. |
| `include/oxys/ext2.h` | The on-disk field offsets of the superblock, the block group descriptor, the inode and the directory entry; the feature flags, the file types, the bounds upon a symbolic link and the deletion time recorded for want of a clock; and the parsed `Ext2Superblock`, `Ext2GroupDescriptor`, `Ext2Inode` and `Ext2DirectoryEntry` descriptions. |
| `fs/vfs.c` | The virtual filesystem layer: the registry of filesystem types, the mount table that joins several volumes into one tree, the node cache that gives one file one identity however many callers reach it, the resolution of a path across mount points and through symbolic links, the open file with a position that advances, and the refusals that keep a volume from being withdrawn or a file from being destroyed while something still holds it. `VfsInitialise`, `VfsRegisterFilesystem`, `VfsMountVolume`, `VfsUnmount`, `VfsMountRoot`, `VfsResolve`, `VfsResolveNoFollow`, `VfsNodeRelease`, `VfsOpen`, `VfsClose`, `VfsRead`, `VfsWrite`, `VfsSeek`, `VfsTell`, `VfsReadDirectory`, `VfsStat`, `VfsStatLink`, `VfsTruncate`, `VfsCreateDirectory`, `VfsRemoveDirectory`, `VfsUnlink`, `VfsLink`, `VfsReadLink`, `VfsSync`, `VfsReport`, `VfsReportDirectory`. |
| `fs/ext2_vfs.c` | The binding of the EXT2 implementation to that layer: the operations vector, the translation between the format's `i_mode` and the layer's neutral node type, the packing of a directory traversal into one opaque cookie, the translation of the format's refusals into the layer's codes, and the mark a mount leaves upon a volume it has opened for writing. `Ext2VfsInitialise`. |
| `include/oxys/vfs.h` | `VfsNode`, `VfsMount`, `VfsAttributes`, `VfsDirectoryEntry` and `VfsFilesystemOperations`; the neutral node types and refusal codes; the flags an open takes; and the bounds of the layer's fixed tables. |
| `include/oxys/ext2_vfs.h` | `Ext2VfsInitialise`, and the reason the binding is declared apart from the format it binds. |
| `include/oxys/buffer.h` | The interface of the buffer cache implemented in `drivers/block/`: obtaining, releasing, dirtying and flushing a cached block. |
| `include/oxys/block.h` | The interface of the generic block-device layer implemented in `drivers/block/`: the operations a driver supplies, and the read and write path above them. |
| `include/oxys/ata.h` | The interface of the ATA driver implemented in `drivers/ata/`: the description of a device and the reading and writing of sectors. |
| `include/oxys/pci.h` | The interface of the PCI enumeration implemented in `drivers/pci/`: the configuration accessors, the description of a function, and the searches by class and by identifier. |
| `include/oxys/vga.h` | The interface of the VGA text-mode display driver implemented in `drivers/vga/`: colour, cursor control, scrolling, cell read-back and the erase limit. |
| `include/oxys/serial.h` | The interface of the interrupt-driven COM1 serial driver implemented in `drivers/serial/`, including the line parameters and the accounting the self-test reads. |
| `include/oxys/pic.h` | The interface of the 8259A interrupt controller driver implemented in `drivers/pic/`: the remapped vector bases, the masking of a request line, the claiming of a line by a device driver, and the status registers. |
| `include/oxys/pit.h` | The interface of the interval timer driver implemented in `drivers/pit/`: the clock frequency, the tick counter, the conversion of ticks to elapsed time, and the bounded wait. |
| `include/oxys/keyboard.h` | The interface of the PS/2 keyboard driver implemented in `drivers/keyboard/`: `KeyEvent`, the modifier flags, the buffer capacity, and the reading of events and characters. |

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
| Multiboot2 Specification 2.0 | 3.1.10, 3.6.12 | The framebuffer request tag carried in the image header, and the information tag describing what the boot loader supplied. |
| Intel SDM, Volume 3A | 11.12 | The page attribute table, by which the framebuffer's pages are made write-combining. |
| Intel SDM, Volume 3A | 3.3.7.1, 4.5 | Canonical addressing, and the higher-half translation helpers. |
| Intel SDM, Volume 3A | 3.4.5, 5.8.8, 6.14.4, 8.2.3, 8.7, 20.5.2, Table 2-1 | The segment descriptor format; the selectors `SYSCALL` and `SYSRET` derive from `IA32_STAR`; the unconditional loading of an interrupt stack table entry; the sixteen-byte task state segment descriptor; the 64-bit task state segment; the I/O map base beyond the limit; and `IA32_EFER.SCE`. |
| Intel SDM, Volume 2A, `LTR`, `CPUID` | — | The loading of the task register, and the establishment of processor support for the mechanism. |
| Intel SDM, Volume 2B, `SYSCALL`, `SYSRET` | — | What the transition saves, loads and clears, and that `SYSRET` returns to privilege level 3 unconditionally. |

Full citations are held in [`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## Present limitations

1. `PhysicalToVirtual` and `VirtualToPhysical` translate through the kernel image
   window and are valid only below one gibibyte. For arbitrary physical memory
   use `PhysicalToDirect` and `DirectToPhysical`, which are valid once
   `PagingInitialise` has run. The distinction is explained in
   `docs/design/MEMORY-LAYOUT.md`, Section 9.2.
2. There is no formatted output. `KernelWriteHexadecimal` is a deliberate
   minimum, to be superseded when the C library of Phase 7 exists.
3. The boot-time self-tests are part of the kernel image and are never absent
   from it. There is no configuration that omits them, and none is wanted before
   there is a machine whose image size matters; `test/README.md` records the
   consequences.
4. The frame allocator is not yet safe against concurrent access; from sub-task
   6.13 its bitmap and search hint require a spinlock. Nothing here is yet safe
   against concurrent access. Every structure
   introduced from Phase 2 onward must record its locking discipline in its
   defining file's header, as `docs/design/ARCHITECTURE.md`, Section 1, requires.
