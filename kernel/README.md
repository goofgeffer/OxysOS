<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
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
| `kernel.c` | `KernelMain`, the C entry point: it validates the boot loader handover, initialises every subsystem of Phases 1 to 5 and of sub-tasks 6.1 to 6.10 in dependency order, runs the boot-time self-tests, mounts the initial ramdisk at the root and a volume the machine carries at `/mnt`, and — from sub-task 8.1 — reads `/bin/sh` off the root and runs it at privilege level 3, starting it again when it ends at the end of its input; where there is no root, it enters the keyboard echo loop of Phase 3 where a keyboard is present, or halts where none is. `KernelRunShell`. `KernelPanic`, the unrecoverable-error path. `KernelHalt`, `KernelWriteString`, `KernelWriteDecimal`, `KernelWriteHexadecimal`, `KernelCommandLineHasOption` and `KernelMountRootVolume`. `KernelWriteString` is the only routine in the kernel permitted to name an output device; the reason is `docs/design/ARCHITECTURE.md`, Section 6. |
| `test/` | The boot-time self-tests, one file per subsystem, and the composed volume they are conducted upon. Described by [`test/README.md`](test/README.md). |
| `abi/` | **A second include root, and not part of the corpus below.** It holds the system-call interface a program is entitled to, under the permissive licence so that the `MIT` C library may include it without including the kernel, and holds constants and never a declaration. Described by [`abi/README.md`](abi/README.md); the division that created it is `../docs/design/LIBC.md`, Section 2. |
| `arch/x86_64/` | **Everything here that could not survive a change of processor.** Six subdirectories — `cpu/`, `interrupt/`, `syscall/`, `smp/`, `mm/` and `proc/` — and the rule for admitting a file to one of them is [`arch/README.md`](arch/README.md). The per-file detail stays in the table below rather than being restated there. Note what the directory does **not** contain: the headers. They stay in `include/oxys/` — but under `include/oxys/arch/`, mirroring this tree, so the interface makes the same portability distinction the implementation does without any consumer's `#include` naming a processor. `../docs/design/ARCHITECTURE.md`, Sections 2.4 and 2.5. |
| `include/oxys/test/verify.h` | The self-test entry points `KernelMain` calls, in the order it calls them; the parsed boot information; and the reading of the boot loader's command line. |
| `arch/x86_64/proc/switch.asm` | The transfers a privilege boundary requires: `ThreadSwitchContext`, which saves six registers and a stack pointer and resumes another thread through the return address its stack carries; `ThreadTrampoline`, where a thread that has never run begins; `ThreadEnterUser`, which clears every register and descends to privilege level 3 by `IRETQ`; and — from sub-task 6.11 — `ThreadResumeUser`, which descends with a whole saved register set restored, as a thread made by `fork` requires. |
| `proc/process.c` | The process control block and the thread structure: the tables, the creation of a process with an address space of its own and a thread with a kernel stack of its own beneath a guard page, the writing of `rsp0` when a thread becomes current, the record of what a process has had loaded into it, and — from sub-task 6.10 — the switch, the start of a thread at privilege level 3, and the termination that returns from one. `ProcessInitialise`, `ProcessCreate`, `ProcessDestroy`, `ThreadCreate`, `ThreadDestroy`, `ThreadSetCurrent`, `ThreadCurrent`, `ProcessCreateUserStack`, `ProcessRecordImage`, `ThreadAdoptCurrent`, `ThreadCreateKernel`, `ThreadSwitchTo`, `ThreadStart`, `ThreadTerminateCurrent`, `ThreadTrampolineEntry`, `ProcessReport`, and — from sub-task 6.11 — `ProcessFork`, `ProcessExecute`, `ProcessExit`, `ProcessWait`, `ProcessCurrent`. |
| `proc/sched.c` | The multiprocessor round-robin scheduler of sub-task 6.15: the per-processor run queues and the lock each carries, the affinity that decides which queue a thread may join, the choice of the shortest eligible queue at admission, the local timer that takes a processor back when a quantum expires, and the idle thread each processor falls back to. `SchedulerInitialise`, `SchedulerStartOnThisProcessor`, `SchedulerDetachThisProcessor`, `SchedulerEnterIdle`, `SchedulerAdmit`, `SchedulerYield`, `SchedulerBlockCurrent`, `SchedulerSetAffinity`, `SchedulerAffinity`, `SchedulerQueueLength`, `SchedulerRunningOn`, `SchedulerIsRunning`, `SchedulerReport`. |
| `terminal/terminal.c` | The terminal input path of sub-task 8.1: one queue of bytes, filled by polling the keyboard's events and the serial adapter's received characters when a reader asks, with the cursor, home, end and delete keys translated to the control sequences a terminal would send (ECMA-48, Section 5.4 and Sections 8.3.18 to 8.3.22; xterm for Home, End and Delete) and the control key collapsed onto a letter as a terminal collapses it. It is what a program's `read` of descriptor 0 reaches, and it echoes nothing and assembles nothing — a raw terminal, which is the only kind a line editor can work upon. `TerminalInitialise`, `TerminalInject`, `TerminalPoll`, `TerminalRead`, `TerminalWaitForInput`, `TerminalHasInput`, `TerminalFlush`, `TerminalReport`. `../docs/design/SHELL.md`, Section 2. |
| `include/oxys/terminal/terminal.h` | The interface above, the queue's capacity, and the two reasons recorded at its head: why the keys become the terminal's own sequences rather than a code of this kernel's, and why it is not a line discipline. |
| `include/oxys/proc/process.h` | `Process`, `Thread` and `ThreadContext`, whose six callee-saved registers and stack pointer are the whole of a context; the capacities of the two tables; the kernel stack and its guard in pages; the placement and extent of a process's user stack; and — from sub-task 6.11 — the saved user context a forked child resumes upon, together with `ProcessFork`, `ProcessExecute`, `ProcessExit`, `ProcessWait` and `ProcessCurrent`. |
| `include/oxys/exec/elf.h` | The ELF64 file and program header layouts, the classes, encodings, machine and type values a file is checked against, the segment flags, and `ElfImage`, `ElfResult` and the loader's interface. |
| `acpi/acpi.c` | The reading of the firmware's ACPI description tables: the discovery and validation of the Root System Description Pointer, the walk of the RSDT or the XSDT, and the parse of the Multiple APIC Description Table into the processors, I/O APICs, interrupt source overrides and non-maskable interrupt connections the APIC drivers are programmed from. Every table is mapped for the duration of its parse and unmapped afterwards, so nothing retains a pointer into memory the firmware declared reclaimable. `AcpiInitialise`, `AcpiIsAvailable`, `AcpiLocalApicAddress`, `AcpiDualPicPresent`, `AcpiProcessorCount`, `AcpiIoApicCount`, `AcpiOverrideCount`, `AcpiLocalNmiCount`, `AcpiGlobalInterruptForIsaIrq`, `AcpiIsaIrqIsActiveLow`, `AcpiIsaIrqIsLevelTriggered`, `AcpiReport`. |
| `exec/elf.c` | The ELF64 loader for statically linked executables: the decoding of a file header and its program headers, the validation an image must survive before a page of it is mapped, and the placing of its segments into an address space through the direct physical map. `ElfDecode`, `ElfValidate`, `ElfLoad`, `ElfLoadFile`, `ElfResultName`, `ElfSegmentAt`, `ElfReport`. |
| `arch/x86_64/interrupt/exceptions.c` | The exception handlers, the decoding of both error-code formats, `ExceptionInstallInterruptStacks`, which gives the double fault a stack of its own, and `ExceptionReportState`. |
| `include/oxys/arch/interrupt/exceptions.h` | The error-code flags of both formats, the vector of the double fault, and the exception interface. |
| `include/oxys/arch/cpu/cpu.h` | Accessors for the control registers `CR0`, `CR2`, `CR3` and `CR4`, and for `RFLAGS`, by which a driver determines whether an interrupt it means to wait for could be delivered at all. |
| `arch/x86_64/interrupt/interrupt_stubs.asm` | The 256 per-vector stubs, the common stub, and the table of stub addresses. |
| `arch/x86_64/interrupt/interrupts.c` | `InterruptInitialise`, `InterruptDispatch`, the dispatch table and its registration interface, and the frame reporting routines. |
| `arch/x86_64/interrupt/irq.c` | The interrupt request layer of sub-task 6.12: the table of handlers claimed by request line rather than by vector, the routing of a request to the driver that claimed it, the signalling of completion at whichever controller delivered it, and the retirement of the 8259A pair in favour of the APIC. It is what a device driver claims a line through, and the one place that knows which controller is answering. `IrqInitialise`, `IrqAdoptApic`, `IrqInstallHandler`, `IrqRemoveHandler`, `IrqRegisteredHandler`, `IrqLineName`, `IrqMaskLine`, `IrqUnmaskLine`, `IrqLineIsMasked`, `IrqActiveController`, `IrqControllerName`, `IrqGlobalInterruptForLine`, `IrqRequestCount`, `IrqSpuriousCount`, `IrqUnclaimedCount`, `IrqReport`. |
| `arch/x86_64/cpu/gdt.c`, `arch/x86_64/cpu/gdt.asm` | The kernel global descriptor table, the user-mode descriptors at the displacements `SYSCALL` and `SYSRET` derive their selectors by, the run-time construction of the sixteen-byte task state segment descriptors — **one for each processor** since sub-task 6.14, `LTR` marking a descriptor busy and refusing one already so marked — the segment reload, and `GdtLoadOnThisProcessor`, by which a started processor adopts the table this kernel already built rather than composing a second one. |
| `include/oxys/arch/interrupt/interrupts.h` | The trap frame and the interrupt interface. |
| `include/oxys/arch/cpu/gdt.h` | The `LGDT` operand, the kernel, user and task state segment selectors, the requested privilege level a user selector carries, and the interface of the descriptor table. |
| `arch/x86_64/cpu/idt.c` | The interrupt descriptor table. `IdtInitialise`, `IdtSetGate`, `IdtSetGateStack`, `IdtGateStack`, and the accessors that read the table register back with `SIDT`. |
| `include/oxys/arch/cpu/idt.h` | The 64-bit gate descriptor, the `LIDT` operand, the gate types, the attribute bits, and the assignment of an interrupt stack table entry to a gate. |
| `arch/x86_64/cpu/tss.c` | The task state segments, one to a processor since sub-task 6.14: the stack the processor loads upon a transfer to privilege level 0, the separate stack a double fault is delivered upon, the sixteen-byte descriptor built for each within the global descriptor table, and the loading of the task register. `RSP0` names the stack of whatever runs upon *that* processor, so a shared segment would deliver two system calls upon one stack. `TssInitialise`, `TssInitialiseProcessor`, `TssSetKernelStack`, `TssKernelStack`, `TssInterruptStack`, `TssIoMapBase`, `TssAddress`, `TssLimit`, `TssTaskRegister`, `TssReport`. |
| `include/oxys/arch/cpu/tss.h` | `TaskStateSegment`, its size asserted at compile time; the interrupt stack table entry the double fault is given; the sizes of the two stacks; and the interface of the segment. |
| `arch/x86_64/syscall/syscall.c` | The configuration of the fast system-call mechanism — the establishment of processor support by `CPUID`, the selectors written into `IA32_STAR`, the entry point written into `IA32_LSTAR`, the flags cleared by `IA32_FMASK`, the per-processor block written into `IA32_KERNEL_GS_BASE`, the enabling of `IA32_EFER.SCE`, and the derivation of the four selectors the processor computes from `IA32_STAR` — and, from sub-task 6.7, the dispatch: the table of calls, the refusal of a number the table does not hold, and the validation of a caller's ranges against both the canonical user limit and the paging hierarchy — which from sub-task 6.11 resolves a copy-on-write fault upon a page it is asked to write rather than refusing an address a fork had protected. That sub-task also adds `fork`, `execve`, `exit` and `wait` to the table, the settling of the two segment-base registers at each boundary, and the second record of the current thread's kernel stack that the entry path reads. `SyscallInitialise`, `SyscallIsEnabled`, `SyscallStar`, `SyscallLstar`, `SyscallFmask`, `SyscallEntryAddress`, `SyscallDerivedKernelCode`, `SyscallDerivedKernelStack`, `SyscallDerivedUserCode`, `SyscallDerivedUserStack`, `SyscallUserRangeIsReadable`, `SyscallCopyUserString`, `SyscallSetKernelStack`, `SyscallEstablishKernelGsBase`, `SyscallEstablishUserGsBase`, `SyscallUserRangeIsWritable`, `SyscallNumberIsValid`, `SyscallName`, `SyscallDispatch`, `SyscallDispatched`, `SyscallRefused`, `SyscallFaulted`, `SyscallEntries`, `SyscallObservedCode`, `SyscallObservedStack`, `SyscallObservedFlags`, `SyscallReport`. |
| `arch/x86_64/cpu/percpu.c` | The per-processor data areas of sub-task 6.13: their static allocation, the establishment of the executing processor's own, the segment base it is reached through and the repair of that base after a segment reload, and the counted interrupt-disable every critical section is built upon. `PerCpuInitialise`, `PerCpuEstablishSegmentBase`, `PerCpuAt`, `PerCpuOnlineCount`, `PerCpuIsEstablished`, `PerCpuPushInterruptState`, `PerCpuPopInterruptState`, `PerCpuCriticalDepth`, `PerCpuLocksHeld`, `PerCpuReport`. |
| `arch/x86_64/cpu/spinlock.c` | The ticket spinlock of sub-task 6.13: the locked fetch-and-add that issues a ticket, the bounded wait to be served, the release that admits the next arrival, and the two checks that turn the silent misuses of a lock into a report rather than a machine that stops. `SpinlockInitialise`, `SpinlockAcquire`, `SpinlockRelease`, `SpinlockTryAcquire`, `SpinlockIsHeld`, `SpinlockIsHeldByThisProcessor`, `SpinlockName`, `SpinlockAcquisitionCount`, `SpinlockContentionCount`, `SpinlockWaiterCount`, `SpinlockReport`. |
| `arch/x86_64/smp/smp.c` | The bring-up of the application processors, sub-task 6.14: the proving of the low page the trampoline requires, the placement of the assembled image within it and the identity mapping that stands for the duration of the bring-up alone, the stacks and task state segment each processor is given before it is started rather than after, the INIT-startup-startup sequence of Intel SDM Volume 3A Section 8.4.4.1 with the delays measured by the interval timer under a masked interrupt flag, the bounded wait for each processor to answer, and the C entry point a started processor arrives at — which loads the kernel's tables, claims an area, records what it actually holds, and parks. `SmpInitialise`, `SmpApplicationProcessorEntry`, `SmpRecordAt`, `SmpProcessorsStarted`, `SmpProcessorsRefused`, `SmpStartupFailureCount`, `SmpIsMultiprocessor`, `SmpDeclinedReason`, `SmpReport`. |
| `arch/x86_64/smp/smp_trampoline.asm` | Carries the assembled image of `boot/trampoline.asm` into the kernel as read-only data with `incbin`, and gives its two ends the symbols `cpu/smp.c` takes the size from. It exists because the trampoline is a flat binary assembled at a fixed origin and cannot be linked as an ordinary object; `boot/README.md` sets out why. |
| `arch/x86_64/smp/ipi.c` | The inter-processor interrupt layer of sub-task 6.13: the composition of a command for each of the three audiences a sender may address, the accounting of what was sent and what arrived, and the handler by which a panicking processor stops the others before it prints. `IpiInitialise`, `IpiIsAvailable`, `IpiSendToOthers`, `IpiSendToSelf`, `IpiSendToProcessor`, `IpiHaltOtherProcessors`, `IpiNoteReceived`, `IpiSentCount`, `IpiReceivedCount`, `IpiFailedSendCount`, `IpiHaltRequestCount`, `IpiReport`. |
| `arch/x86_64/syscall/syscall_entry.asm` | `SyscallEntry`, the address `IA32_LSTAR` holds. Provisional in sub-task 6.1 and replaced at 6.7 by the real path, whose first three instructions are the whole of its security and none of which may be moved: `SWAPGS`, because until it has run no kernel state is addressable; the caller's stack stored into the per-processor block, because there is nowhere else to put it; and only then the kernel stack, because a path that pushed first would be writing to the caller's stack at privilege level 0. It returns by `SYSRET`. |
| `include/oxys/arch/syscall/syscall.h` | The named bits of `RFLAGS` and `SYSCALL_FLAG_MASK` composed from them; `SyscallFrame`, the registers the entry path saves; the interface by which the configuration is read back from the processor and asserted; the dispatch and the validation of a caller's arguments; and `SyscallCopyUserString`, `SyscallSetKernelStack` and the two routines that settle the segment bases at a boundary. **The call numbers, the error values and the two limits are no longer here**: sub-task 7.1 moved them to `abi/oxys/syscall_abi.h`, which this header includes, so every consumer of it sees what it always saw. |
| `include/oxys/arch/cpu/msr.h` | The numbers of the model-specific registers the kernel uses, the `SCE` bit of `IA32_EFER`, and `ReadMsr` and `WriteMsr`. |
| `mm/heap.c` | The kernel heap. `KernelAllocate`, `KernelAllocateZeroed` and `KernelFree`. |
| `mm/vmm.c` | The kernel virtual address allocator. `KernelPagesAllocate` and `KernelPagesFree`. |
| `include/oxys/mm/heap.h` | The interface of the kernel heap. |
| `include/oxys/mm/vmm.h` | The kernel arena constants, the interface of the virtual address allocator, and the mapping of memory the kernel does not own: `KernelDeviceMap` and `KernelDeviceUnmap`. |
| `include/oxys/gfx/graphics.h` | The interface of the drawing primitives implemented in `graphics/`: the rectangle, the surface and its clip, and the pixel, line, rectangle and blit operations upon them. |
| `include/oxys/gfx/framebuffer.h` | The interface of the framebuffer implemented in `graphics/`: whether a display was supplied and whether it can be drawn upon, its address, extent, pitch and pixel layout, the encoding of a colour into that layout, and whether the mapping is write-combining. |
| `include/oxys/gfx/font.h` | The interface of the bitmap font implemented in `graphics/`: the cell's metrics, the range of code points covered, and the drawing of one glyph upon a surface. |
| `include/oxys/gfx/console.h` | The interface of the graphical console implemented in `graphics/`: its extent and active position in characters, the writing of a character or a string, the colours drawn in, and the erase limit a backspace may not retreat past. |
| `include/oxys/gfx/faultscreen.h` | The interface of the graphical fault screens implemented in `graphics/`: the table of screens, one for each severe fault, and the two entry points by which one is drawn — for a processor exception, and for a panic the kernel raises itself. |
| `include/oxys/gfx/compositor.h` | The interface of the compositor implemented in `graphics/`: the back buffer offered as a surface, the ordered layers composited over it, the damage rectangle and its accumulation, the presentation that carries the damage to the display, and the suspension a fault screen imposes permanently. |
| `include/oxys/gfx/cursor.h` | The interface of the pointer implemented in `graphics/`: its two-bitmap shape and the three states that shape encodes, its position and bounds, and the surface and mask by which the compositor draws it as a layer. |
| `arch/x86_64/mm/paging.c` | The permanent kernel paging hierarchy and copy-on-write. `PagingInitialise`, `PagingTranslate`, `PagingAddressIsWritable`, `PagingMarkCopyOnWrite`, `PagingResolveCopyOnWriteFault` and the reporting routines. |
| `include/oxys/arch/mm/paging.h` | The paging-structure entry flags and the interface of the paging subsystem, including the distinction sub-task 6.13 introduced between an invalidation announced to every processor and one performed only here. |
| `include/oxys/arch/cpu/percpu.h` | The per-processor area, the bound upon how many exist, and `PerCpuCurrent` — one load, of the third quadword of the area, through a segment base only privilege level 0 can have written. |
| `include/oxys/arch/cpu/spinlock.h` | The ticket spinlock, its static initialiser, and the interface above it. |
| `include/oxys/arch/smp/ipi.h` | The two vectors this kernel reserves for one processor to address another, and the three ways an audience is named. |
| `include/oxys/arch/mm/shootdown.h` | The translation-lookaside-buffer shootdown: the broadcast, the self-directed form the single-processor demonstration needs, and the counters. |
| `arch/x86_64/mm/addrspace.c` | The address space: creation, cloning by copy-on-write, activation and destruction. `AddressSpaceCreate`, `AddressSpaceClone`, `AddressSpaceDestroy`, `AddressSpaceSwitch`, `AddressSpaceMapPage` and the accounting accessors. |
| `include/oxys/arch/mm/addrspace.h` | `AddressSpace`, the division of the root table between the two canonical halves, and the interface of address-space cloning. |
| `mm/pmm.c` | The physical frame allocator and per-frame reference counting. `PhysicalMemoryInitialise`, `FrameAllocate`, `FrameAllocateBelow`, `FrameFree`, `FrameReferenceInitialise`, `FrameReferenceIncrement` and the accounting accessors. |
| `arch/x86_64/mm/shootdown.c` | The translation-lookaside-buffer shootdown of sub-task 6.13: the publication of the address whose translation has become stale, the interrupt that tells the other processors to discard it, the acknowledgement each makes, and the bounded wait for all of them. `ShootdownInitialise`, `ShootdownBroadcast`, `ShootdownToSelf`, `ShootdownRequestCount`, `ShootdownServiceCount`, `ShootdownAbandonedCount`, `ShootdownLastAddress`, `ShootdownReport`. |
| `include/oxys/mm/memory.h` | `PAGE_SIZE`, `PAGE_SHIFT`, `LOW_MEMORY_LIMIT` and the alignment helpers. |
| `include/oxys/mm/pmm.h` | The interface of the physical frame allocator. |
| `handoff/multiboot2.c` | `BootInformationParseMultiboot2`, which walks the Multiboot2 tag series and reduces it to the neutral description; `BootInformationReport`, which emits that description. |
| `handoff/multiboot2.h` | The raw on-memory layout of the Multiboot2 structure and of the tags the kernel consumes, together with the ELF64 section header. **It is a private header and deliberately not in the corpus below**: the structures in it are one boot protocol's wire format, and design premise 3 is that nothing above the handoff layer knows which protocol it was booted by. Everything above consumes `include/oxys/boot/bootinfo.h` instead. |
| `include/oxys/boot/bootinfo.h` | `BootInformation`, the boot-protocol-neutral description of the machine, and its classification of memory regions. |
| `include/oxys/types.h` | The fixed-width integer types, and the distinct address types `PhysicalAddress` and `VirtualAddress`. |
| `include/oxys/kernel.h` | `KERNEL_VIRTUAL_BASE`, the address translation helpers `PhysicalToVirtual` and `VirtualToPhysical`, and the declarations of `KernelMain` and `KernelPanic`. |
| `include/oxys/dev/io.h` | `PortReadByte`, `PortWriteByte` and `IoWait`: the accessors for the x86 programmed input/output address space. |
| `fs/ext2/` | The EXT2 implementation, divided into nine translation units and a private header. It was one file of 4,325 lines until it was divided; `../docs/design/ARCHITECTURE.md`, Section 2.2, records why, and along which lines. The public interface is unchanged and is `<oxys/fs/ext2.h>`. |
| `fs/ext2/internal.h` | What those units share with one another and with nothing else: the record of the last refusal and the accounting beside it, the decoders and encoders of the volume's stored byte order, the block-level transfer, and the two seams between modules — `Ext2TruncateBlocks` and the directory entry codec. Beside the implementation and not in `include/oxys/`, because nothing outside `fs/ext2/` may depend upon it. |
| `fs/ext2/core.c` | The shared state and the primitives above it. `Ext2Refuse` and its six siblings, `Ext2ReadHalf`, `Ext2ReadWord`, `Ext2ReadText`, `Ext2WriteHalf`, `Ext2WriteWord`, `Ext2ReadBytes`, `Ext2WriteBytes`, `Ext2ZeroBlock`, `Ext2BlockExists`, `Ext2Writable`, `Ext2FillZero`, `Ext2PointersPerBlock`, `Ext2SectorsPerBlock`, `Ext2LastError` and the accounting accessors. |
| `fs/ext2/superblock.c` | The superblock read, validated, written and reported, and the geometry every other module computes from. `Ext2ReadSuperblock`, `Ext2WriteSuperblock`, `Ext2ReportVolume`, `Ext2VolumesRead`, `Ext2VolumesRefused`. |
| `fs/ext2/group.c` | The block group descriptor table. `Ext2GroupCount`, `Ext2GroupDescriptorBlock`, `Ext2GroupDescriptorBlocks`, `Ext2InodeTableBlocks`, `Ext2GroupFirstBlock`, `Ext2GroupBlockCount`, `Ext2ReadGroupDescriptor`, `Ext2WriteGroupDescriptor`, `Ext2VerifyGroupDescriptors`, `Ext2ReportGroup`. |
| `fs/ext2/inode.c` | The inode and the block pointers that name a file's blocks, through every level of indirection. `Ext2ReadInode`, `Ext2WriteInode`, `Ext2InodeBlock`, `Ext2InodeBlockAllocate`, `Ext2InodeBlockCount`, `Ext2InodeIsDirectory`, `Ext2InodeIsRegular`, `Ext2InodeIsSymbolicLink`, `Ext2ReportInode`. |
| `fs/ext2/file.c` | The contents of a file. `Ext2ReadFile`, `Ext2ReadSymbolicLink`, `Ext2InodeIsFastSymbolicLink`, `Ext2WriteFile`, `Ext2TruncateFile`, `Ext2TruncateBlocks`. |
| `fs/ext2/alloc.c` | The block and inode bitmaps, and the summaries kept in step with them. `Ext2BlockInUse`, `Ext2InodeInUse`, `Ext2AllocateBlock`, `Ext2FreeBlock`, `Ext2AllocateInode`, `Ext2FreeInode`. |
| `fs/ext2/directory.c` | The record a directory is made of, and its traversal. `Ext2FileTypeOfMode`, `Ext2FileTypeName`, `Ext2VolumeStatesFileType`, `Ext2ReadEntryHeader`, `Ext2ReadEntryName`, `Ext2DirectoryOpen`, `Ext2DirectoryNext`, `Ext2DirectoryFind`, `Ext2ReportDirectoryEntry`, `Ext2ReportDirectory`. |
| `fs/ext2/path.c` | The resolution of an absolute path across symbolic links. `Ext2ResolvePath`, `Ext2ResolvePathNoFollow`. |
| `fs/ext2/name.c` | The names by which a file is reached, and the files themselves. `Ext2DirectoryInsert`, `Ext2DirectoryRemove`, `Ext2DirectoryIsEmpty`, `Ext2CreateFile`, `Ext2CreateDirectory`, `Ext2Link`, `Ext2Unlink`, `Ext2RemoveDirectory`. |
| `include/oxys/fs/ext2.h` | The on-disk field offsets of the superblock, the block group descriptor, the inode and the directory entry; the feature flags, the file types, the bounds upon a symbolic link and the deletion time recorded for want of a clock; and the parsed `Ext2Superblock`, `Ext2GroupDescriptor`, `Ext2Inode` and `Ext2DirectoryEntry` descriptions. |
| `fs/vfs/` | The virtual filesystem layer, divided into six translation units and a private header. It was one file of 2,355 lines; `../docs/design/ARCHITECTURE.md`, Section 2.2, records why and along which lines. The public interface is unchanged and is `<oxys/fs/vfs.h>`. |
| `fs/vfs/internal.h` | What those units share: the four fixed tables the layer's whole state lives in, `VfsFile`, the refusal record and the accounting, and the resolution and node-cache primitives. Beside the implementation and not in `include/oxys/`, because nothing outside `fs/vfs/` may depend upon it. |
| `fs/vfs/vfs.c` | The state itself, the refusals every operation records through, the names of the error codes and node types, the bounded string primitives, and the accounting. `VfsSetError`, `VfsLastError`, `VfsLastErrorCode`, `VfsErrorName`, `VfsNodeTypeName`, `VfsReport`, `VfsReportDirectory`. |
| `fs/vfs/node.c` | The node cache, which gives one file one identity however many callers reach it. `VfsNodeHold`, `VfsNodeAcquire`, `VfsNodeRelease`, `VfsNodesHeld`, `VfsNodeAttributes`. |
| `fs/vfs/path.c` | The resolution of a path across mount points and through symbolic links, and the resolution of a path's parent for the operations that alter a directory. `VfsWalk`, `VfsResolve`, `VfsResolveNoFollow`, `VfsResolveParent`, `VfsPathLength`, `VfsWritable`. |
| `fs/vfs/mount.c` | The registry of filesystem types and the mount table that joins several volumes into one tree, with the refusals that keep a volume from being withdrawn while something still holds it. `VfsInitialise`, `VfsRegisterFilesystem`, `VfsMountVolume`, `VfsUnmount`, `VfsMountRoot`, `VfsMountAt`, `VfsMountCount`, `VfsRootIsMounted`. |
| `fs/vfs/file.c` | The open file, and the position that advances. `VfsOpen`, `VfsClose`, `VfsRead`, `VfsWrite`, `VfsSeek`, `VfsTell`, `VfsReadDirectory`, `VfsFileAttributes`, `VfsOpenFileCount`. |
| `fs/vfs/namespace.c` | The operations that name a file rather than hold one open. `VfsStat`, `VfsStatLink`, `VfsTruncate`, `VfsCreateDirectory`, `VfsRemoveDirectory`, `VfsUnlink`, `VfsLink`, `VfsReadLink`, `VfsSync`. |
| `fs/ext2_vfs.c` | The binding of the EXT2 implementation to that layer: the operations vector, the translation between the format's `i_mode` and the layer's neutral node type, the packing of a directory traversal into one opaque cookie, the translation of the format's refusals into the layer's codes, and the mark a mount leaves upon a volume it has opened for writing. `Ext2VfsInitialise`. |
| `include/oxys/fs/vfs.h` | `VfsNode`, `VfsMount`, `VfsAttributes`, `VfsDirectoryEntry` and `VfsFilesystemOperations`; the neutral node types and refusal codes; the flags an open takes; and the bounds of the layer's fixed tables. |
| `include/oxys/fs/ext2_vfs.h` | `Ext2VfsInitialise`, and the reason the binding is declared apart from the format it binds. |
| `block/` | The generic block-device layer and the buffer cache above it. It is **not** in `../drivers/`, and the reason is the test that directory's own `README.md` applies to the framebuffer: nothing here programs anything. A driver implements an interface this corpus declares; these two declare the interface a driver registers *into*, which is the opposite relation. `../docs/design/ARCHITECTURE.md`, Section 2.3, records the move. |
| `block/block.c` | The registry of devices that transfer fixed-size blocks, and the validated read and write path through which every caller above reaches a driver. `BlockRegister`, `BlockUnregister`, `BlockRead`, `BlockWrite`, `BlockDeviceAt`, `BlockFindByName`, `BlockReport`. |
| `block/buffer.c` | The buffer cache above that layer: the hash by which a held block is found, the recency list by which one is chosen for eviction, the reference count that protects a buffer still in use, and the write-back discipline. `BufferInitialise`, `BufferGet`, `BufferRelease`, `BufferMarkDirty`, `BufferFlush`, `BufferSync`, `BufferInvalidateDevice`, `BufferReport`. |
| `include/oxys/block/buffer.h` | The interface of the buffer cache implemented in `block/`: obtaining, releasing, dirtying and flushing a cached block. |
| `include/oxys/block/block.h` | The interface of the generic block-device layer implemented in `block/`: the operations a driver supplies, and the read and write path above them. |
| `include/oxys/dev/storage/ata.h` | The interface of the ATA driver implemented in `drivers/ata/`: the description of a device and the reading and writing of sectors. |
| `include/oxys/dev/storage/ahci.h` | The interface of the AHCI driver implemented in `drivers/ahci/`: the host and port register displacements, the command header and frame information structure layouts, and the description of a port and its attached device. |
| `include/oxys/dev/storage/sdhci.h` | The interface of the SD host controller driver implemented in `drivers/sdhci/`: the controller's registers, the card's own command set, the two encodings of a card's capacity, and the description of the card found. |
| `include/oxys/dev/storage/ramdisk.h` | The interface of the ramdisk driver implemented in `drivers/ramdisk/`: the name the initial ramdisk is registered under, the name of the module it arrives in, and the registration and report. Sub-task 7.7; `../docs/storage/INITRD.md`. |
| `include/oxys/dev/pci.h` | The interface of the PCI enumeration implemented in `drivers/pci/`: the configuration accessors, the description of a function, and the searches by class and by identifier. |
| `include/oxys/dev/vga.h` | The interface of the VGA text-mode display driver implemented in `drivers/vga/`: colour, cursor control, scrolling, cell read-back and the erase limit. |
| `include/oxys/dev/serial.h` | The interface of the interrupt-driven COM1 serial driver implemented in `drivers/serial/`, including the line parameters and the accounting the self-test reads. |
| `include/oxys/dev/pic.h` | The interface of the 8259A interrupt controller driver implemented in `drivers/pic/`: the remapped vector bases, the masking of a request line, the claiming of a line by a device driver, and the status registers. |
| `include/oxys/dev/pit.h` | The interface of the interval timer driver implemented in `drivers/pit/`: the clock frequency, the tick counter, the conversion of ticks to elapsed time, and the bounded wait. |
| `include/oxys/dev/ps2.h` | The interface of the 8042 controller module implemented in `drivers/ps2/`: the command and status ports, the two device ports, and the read-modify-write of the configuration byte that governs both — declared apart from either device driver because the byte admits only one owner. |
| `include/oxys/dev/keyboard.h` | The interface of the PS/2 keyboard driver implemented in `drivers/keyboard/`: `KeyEvent`, the modifier flags, the buffer capacity, and the reading of events and characters. |
| `include/oxys/dev/mouse.h` | The interface of the PS/2 mouse driver implemented in `drivers/mouse/`: `MouseEvent`, the button flags, the bounds the driver confines its position within, and the reading of events. |

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
4. **Every processor schedules; three structures are locked.** Sub-task 6.13
   built the ticket spinlock, the per-processor area beneath it, the
   inter-processor interrupt and the shootdown; 6.14 started the processors that
   make any of it necessary; 6.15 gave each a run queue and a timer.
   `docs/design/CONCURRENCY.md`, `docs/design/SMP.md` and
   `docs/design/SCHEDULER.md` are the designs. What is locked is the diagnostic
   channel (in `KernelWriteString`), each run queue, and the claim of a slot in
   the process and thread tables (`ProcessTableLock`). Everything else here is
   still unsynchronised — the frame allocator's bitmap and search hint, the
   arena, the heap and the dispatch table all still say so in their own headers —
   and is still safe for a narrower reason than before: **what runs upon an
   application processor is a kernel thread, and a user thread's affinity mask
   names the bootstrap processor alone.** That mask is the state of these locks
   written as a value in a field; widening it and shrinking that list are one
   change. Every structure introduced from Phase 2 onward must record its locking
   discipline in its defining file's header, as
   `docs/design/ARCHITECTURE.md`, Section 1, requires.
