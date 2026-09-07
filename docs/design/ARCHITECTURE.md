# Oxys-OS System Architecture

**Corresponding phase**: All phases. This document is revised whenever a
subsystem is added or its interface altered.

## 1. Design premises

Oxys-OS is a monolithic operating system for the x86_64 architecture. The
monolithic model was selected in preference to a microkernel because the
project's objectives concern the direct exercise of hardware interfaces, and
because the inter-process communication overhead of a microkernel would obscure
rather than illuminate the mechanisms under study.

The following properties are treated as first-class design constraints rather
than as later additions, in accordance with `PROJECT_GUIDELINES.md`, Section 5:

1. **Symmetric multi-processing.** Every kernel data structure introduced from
   Phase 2 onward is designed on the assumption that it will be accessed
   concurrently by several processors. Locking discipline is recorded in the
   header of the structure's defining file at the time the structure is
   introduced, not retrofitted in Phase 6.
2. **Copy-on-write.** The physical frame allocator introduced in Phase 2
   maintains a per-frame reference count from the outset, because retrofitting
   reference counting to an allocator that lacks it would require the
   modification of every consumer. The fault resolution built upon it is
   complete as of sub-task 2.7, and sub-task 2.8 added the address-space cloning
   that creates the shared pages upon which it acts.
3. **Boot-protocol neutrality.** The kernel proper consumes a boot-protocol
   neutral handoff structure. In Phase 1 that structure is the Multiboot2
   information block read directly; in Phase 12 an equivalent structure is
   populated from the UEFI System Table, and the kernel above the handoff layer
   is unchanged.

## 2. Source tree layout

| Directory | Contents | Introduced |
| --------- | -------- | ---------- |
| `boot/` | The Multiboot2 header, the 32-bit entry point, the long-mode transition, and the GRUB configuration. | Phase 1 |
| `kernel/` | The architecture-independent kernel core: entry, memory management, the privilege apparatus, scheduling, system calls, and the virtual filesystem. | Phase 1 |
| `kernel/include/oxys/` | The kernel's internal header corpus. | Phase 1 |
| `kernel/test/` | The boot-time self-tests, one file per subsystem, and the composed volume they are conducted upon. | Phase 2 |
| `drivers/` | Device drivers, one subdirectory per device class. | Phase 1 |
| `libc/` | The minimal C library linked into user programs. | Phase 7 |
| `userland/` | User programs: the utilities and the shell. | Phase 7 |
| `graphics/` | The framebuffer, the drawing primitives, the font and the compositing surface. | Phase 6 (established) |
| `crypto/` | The random-number generator, the hash function and the symmetric cipher. | Phase 10 |
| `net/` | The network protocol stack. | Phase 11 |
| `uefi/` | The UEFI application entry point and the UEFI handoff path. | Phase 12 |
| `docs/` | The documentation corpus, grouped by subject into `project/`, `design/`, `devices/` and `storage/` and indexed by [`docs/README.md`](../README.md). | Phase 1 |

### 2.2 When a subsystem becomes a directory

Four subsystems occupy a directory of their own rather than a file, and the rule
they establish is worth stating once.

| Directory | Was | Is now |
| --------- | --- | ------ |
| `kernel/fs/ext2/` | `kernel/fs/ext2.c`, 4,325 lines | Nine units, 306 to 856 lines, and a private header |
| `kernel/fs/vfs/` | `kernel/fs/vfs.c`, 2,355 lines | Six units, 184 to 532 lines, and a private header |
| `kernel/test/ext2/` | part of `kernel/test/verify_ext2.c`, 2,618 lines | Five chapters and a private header, the entry point remaining above them at 318 lines |
| `drivers/ata/` | `drivers/ata/ata.c`, 1,282 lines | Six units, 160 to 320 lines, and a private header |

A translation unit is divided when it stops being readable as one thing.
`kernel/fs/ext2.c` reached 4,325 lines, and the evidence that it had outgrown
itself was in its own header block: the `Purpose` said it implemented "the
reading and validation of an EXT2 superblock", which had been true when it was
written and described about a twelfth of what the file had become. A header that
no longer describes its file is not a documentation defect to be corrected in
place — it is the file telling you it has become several.

The division is along the lines the *faults* fall upon and not merely the lines
the functions group by: a fault in `path.c` resolves a name to the wrong file, a
fault in `name.c` leaves a volume malformed, a fault in `alloc.c` leaves the
bitmaps disagreeing with the summaries. Each is a different kind of wrongness
with a different way of being found.

The same test applied to the other three. `drivers/ata/` divides at the point
where the *symptoms* diverge: a fault in `port.c` is a timing rule broken and a
status register believed too early, a fault in `channel.c` is a disk this driver
never looked in the right place for, and a fault in `transfer.c` is the wrong
sector returned — and only the last of the three is visible to the caller at all.
`kernel/test/ext2/probe.c` is separated on a line the project had already drawn
in prose: a probe asserts nothing, and `kernel/test/README.md` explains at length
why the two must not be confused. Making that distinction a file boundary means
it can no longer be blurred by accident.

**A division is not a rewrite.** Nothing was reordered, renamed for taste, or
improved in passing. What changed is the set of files the same code lives in, so
that the check afterwards can be mechanical: the function definitions before and
after must be the same set, and every non-comment line must survive but for the
qualifiers the new boundaries force. Anything else in that difference is a defect
introduced by the division, and would be invisible in a diff that also carried
improvements.

Such a directory carries an `internal.h` beside its sources and **not** in
`kernel/include/oxys/`. The public corpus is what a consumer may depend upon;
what the parts of one subsystem share between themselves is not that. Those
declarations were file-scope statics before the division and would be statics
still if C offered any way to share them among a chosen few, and placing the
header beside the implementation is the whole of what records that limit.

### 2.1 The grouping of `docs/`

| Directory | Holds |
| --------- | ----- |
| `docs/project/` | How the work is conducted: the plan, the test procedure and record, the toolchain, the coding standards, the bibliography. |
| `docs/design/` | The kernel itself: this document, the boot sequence, the address space, the interrupts, the privilege transition, and the framebuffer with the drawing above it. |
| `docs/devices/` | One document per device the kernel drives. |
| `docs/storage/` | The stack from a medium to a caller: the disk, the block layer, the buffer cache. |

The grouping is by subject and not by phase, since a document is amended in every
phase that touches its subject. A directory `README.md` describes its directory's
contents locally; these documents describe the system by subject. The two are
complementary and neither replaces the other.

## 3. Present composition

As of the completion of Phase 5 and of sub-tasks 6.1 to 6.10, the system
comprises the following translation units. The list is the `C_SOURCES` and
`ASM_SOURCES` of the `Makefile` and must be revised in the same change as
either.

| Unit | Role |
| ---- | ---- |
| `boot/boot.asm` | The Multiboot2 header; the 32-bit entry point `_start`; CPUID and long-mode feature detection; the construction of the boot-time paging hierarchy; the long-mode transition; the higher-half entry point `KernelEntryHigh`. |
| `kernel/cpu/exceptions.c` | The handlers for the architecture-defined exceptions and the diagnostic report. |
| `kernel/cpu/interrupt_stubs.asm` | The 256 per-vector entry stubs and the common stub that saves the registers and calls the dispatcher. |
| `kernel/cpu/interrupts.c` | The installation of the stubs, the dispatch table and the routing of each vector to its registered handler. |
| `kernel/cpu/gdt.c`, `kernel/cpu/gdt.asm` | The kernel global descriptor table and the reloading of the segment registers. |
| `kernel/cpu/idt.c` | The interrupt descriptor table: its storage, the installation of a gate, the assignment of an interrupt stack table entry to a gate, and the loading of the table. |
| `kernel/cpu/tss.c` | The task state segment: the stacks the processor loads when it needs one it can trust, its descriptor within the global descriptor table, and the loading of the task register. |
| `kernel/cpu/syscall.c` | The configuration of the fast system-call mechanism — `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, `IA32_KERNEL_GS_BASE` and the enabling bit of `IA32_EFER` — and, from sub-task 6.7, the dispatch table, the three calls it holds and the validation of a caller's arguments. |
| `kernel/cpu/syscall_entry.asm` | The entry point `IA32_LSTAR` names. Provisional in sub-task 6.1; replaced at sub-task 6.7 by the path that swaps `GS`, loads the kernel stack from the per-processor block, dispatches, and returns by `SYSRET`. |
| `kernel/exec/elf.c` | The ELF64 loader for statically linked executables: the decoding of the file and program headers, the fifteen refusals an image must survive whole before a page of it is mapped, and the placing of its segments into an address space through the direct physical map. |
| `kernel/proc/process.c` | The process control block, the thread and the saved context: the two tables, the address space a process is given, the kernel stack and guard page a thread is given, the writing of `rsp0` when a thread becomes current, the switch, the descent to privilege level 3 and the termination that returns from it. |
| `kernel/proc/switch.asm` | `ThreadSwitchContext`, which exchanges six registers and a stack pointer; `ThreadTrampoline`, where a thread that has never run begins; and `ThreadEnterUser`, which clears every register and descends to privilege level 3 by `IRETQ`. |
| `graphics/compositor.c` | The compositor: the back buffer that stands in for the framebuffer, the ordered layers composited over it, the damage rectangle that narrows what is carried to the display, and the suspension a fault screen imposes. |
| `graphics/cursor.c` | The pointer: its two-bitmap shape, and the layer the compositor draws it as. |
| `graphics/draw.c` | The two-dimensional primitives upon a surface: rectangle arithmetic and clipping, the pixel, the filled and outlined rectangle, the integer line, and the blit. |
| `graphics/framebuffer.c` | The framebuffer the boot loader supplies: its validation, the write-combining memory type given to its pages, its mapping into the kernel arena, and the description every later phase draws through. |
| `graphics/font.c` | The bitmap face — ninety-five glyphs of eight by eight, drawn for this project — and the drawing of one glyph upon a surface. |
| `graphics/console.c` | The graphical console: a grid of character cells upon the framebuffer, the four control characters, a scroll performed by blitting the surface upon itself, and the replay of what was written before the framebuffer could be mapped. |
| `graphics/faultscreen.c` | The full-screen page a severe fault produces: one screen for each fault, with its own title, colour, account and evidence. |
| `kernel/test/volume.c` | The test fixture: two block devices backed by memory, and an EXT2 volume composed within them. |
| `kernel/test/verify_memory.c` | The self-tests of the frame allocator, the paging hierarchy, the allocators, reference counting, copy-on-write and address spaces. |
| `kernel/test/verify_interrupts.c` | The self-tests of the descriptor table, the stubs and their trap frame, the dispatcher and the exception handlers. |
| `kernel/test/verify_graphics.c` | The self-tests of the drawing primitives, conducted upon a surface in memory. |
| `kernel/test/verify_framebuffer.c` | The self-tests of the framebuffer's description, its mapping, its memory type, and the pattern a person judges. |
| `kernel/test/verify_console.c` | The self-tests of the bitmap face against its own metrics, of a glyph drawn upon a surface against its own bytes, and of the four control characters upon the live console. |
| `kernel/test/verify_faultscreen.c` | The self-tests of the fault screen table: that every severe fault has a screen of its own and that no two of them are alike. |
| `kernel/test/verify_privilege.c` | The self-tests of the descriptors, the task state segment, the interrupt stack table and the system-call configuration. |
| `kernel/test/verify_syscall.c` | The self-tests of the system-call dispatch table and of the validation of a caller's arguments. |
| `kernel/test/verify_elf.c` | The self-tests of the ELF64 loader, upon an image composed in memory so that every field may be made wrong on purpose. |
| `kernel/test/verify_process.c` | The self-tests of the process and thread tables, the per-thread kernel stack and its guard, and the balance of the arena. |
| `kernel/test/verify_usermode.c` | The self-tests of the context switch, and of a program composed, loaded, entered at privilege level 3 and ended. |
| `kernel/test/verify_compositor.c` | The self-tests of the clip stack, the blend, the damage arithmetic and the layer table. |
| `kernel/test/verify_mouse.c` | The self-tests of the mouse's packet decoder, driven without a mouse, and of the pointer upon a surface in memory. |
| `kernel/test/verify_devices.c` | The self-tests of the interrupt controllers, the interval timer, the keyboard, the serial adapter, the display and the bus. |
| `kernel/test/verify_storage.c` | The self-tests of the disk, the block layer and the buffer cache. |
| `kernel/test/verify_ext2.c` | The entry point of the EXT2 self-test: the fixture composed, the superblock and its refusals asserted, and the five chapters below called in turn. |
| `kernel/test/ext2/internal.h` | What those chapters share: their own entry points, the restoration of the fixture between them, and the two helpers more than one judges through. |
| `kernel/test/ext2/format.c` | The superblock, the group descriptors and the inode, and the dozen ways a volume may contradict itself and be refused. |
| `kernel/test/ext2/directory.c` | The directory record, its traversal, and the resolution of a path across symbolic links. |
| `kernel/test/ext2/file.c` | The reading of a file's contents, its holes, and both forms of symbolic link. |
| `kernel/test/ext2/write.c` | Everything that alters a volume: allocation, writing, truncation, and the insertion and removal of names. |
| `kernel/test/ext2/probe.c` | The report upon whatever volume the machine actually carries. **Not a self-test**: it asserts nothing, and the distinction is the reason it is a file of its own. |
| `kernel/test/verify_vfs.c` | The self-tests of the virtual filesystem layer, and the probe of a real volume through it. |
| `kernel/mm/heap.c` | The kernel heap: a slab allocator of eight size classes over the kernel arena. |
| `kernel/mm/vmm.c` | The kernel virtual address allocator, issuing ranges of the kernel arena backed by frames. |
| `kernel/mm/paging.c` | The permanent kernel paging hierarchy: its construction, activation, software translation and copy-on-write fault resolution. |
| `kernel/mm/addrspace.c` | The address space: its creation, its cloning by the copy-on-write discipline, its activation and its destruction. |
| `kernel/mm/pmm.c` | The physical frame allocator: a bitmap of every 4 KiB frame below the highest usable address. |
| `kernel/fs/ext2/internal.h` | What the nine translation units below share with one another and with nothing else: the record of the last refusal, the accounting, the decoders and encoders of the volume's byte order, and the block-level transfer. |
| `kernel/fs/ext2/core.c` | The shared state, the refusals, the decoding and encoding of the stored byte order, the block-level transfer in both directions, and the accounting accessors. |
| `kernel/fs/ext2/superblock.c` | The superblock: its reading, its validation, the geometry derived from it, its writing, and the judgement of whether a volume may be read, written, or addressed at all. |
| `kernel/fs/ext2/group.c` | The block group descriptor table: the geometry of the groups, one descriptor read and written, and the validation of the whole table. |
| `kernel/fs/ext2/inode.c` | The inode: its retrieval and writing, the resolution of a file's block index through every level of indirection, and the allocation of the blocks a file grows into. |
| `kernel/fs/ext2/file.c` | The contents of a file: reading a range of its bytes, the two forms of symbolic link, the writing that extends it, and the truncation that releases what it no longer covers. |
| `kernel/fs/ext2/alloc.c` | The two bitmaps: the testing and setting of a bit, the search for a free one, and the group and superblock summaries kept in step with every allocation. |
| `kernel/fs/ext2/directory.c` | The record a directory is made of: the file types, the decoding and validation of one entry, the traversal, and the search for a name. |
| `kernel/fs/ext2/path.c` | The resolution of an absolute path to the inode it names, across symbolic links and with a bound upon how many may be followed. |
| `kernel/fs/ext2/name.c` | The names themselves: the insertion and removal of a record, and the creation and destruction of files, directories and hard links. |
| `kernel/fs/vfs/internal.h` | What the six units below share: the four fixed tables the layer's whole state lives in, the refusal record and the accounting, the open file, and the resolution and node-cache primitives. |
| `kernel/fs/vfs/vfs.c` | The state itself, the refusals and the names of the error codes, the bounded string primitives, and the accounting and reports. |
| `kernel/fs/vfs/node.c` | The node cache: the identity a file has within the kernel, and why one file must be one node however many callers reach it. |
| `kernel/fs/vfs/path.c` | The resolution of a path: the walk through each component, the crossing of mount points in both directions, and the following of symbolic links. |
| `kernel/fs/vfs/mount.c` | The registry of filesystem types and the mount table that joins several volumes into one tree. |
| `kernel/fs/vfs/file.c` | The open file: the descriptor table, the position that advances, and the reading, writing and seeking above it. |
| `kernel/fs/vfs/namespace.c` | The operations that name a file rather than hold one open: stat, truncate, the creation and removal of names and directories, and the flush. |
| `kernel/fs/ext2_vfs.c` | The binding of the EXT2 implementation to that layer: the operations vector, the translation between the format's mode and the layer's neutral node type, and the mark a mount leaves upon a volume it has open. |
| `kernel/multiboot2.c` | The Multiboot2 parser, reducing the boot loader's structure to the neutral `BootInformation` description. |
| `kernel/kernel.c` | `KernelMain`, which validates the boot loader handover, initialises every subsystem in the dependency order of Section 4, runs the self-tests, mounts a root volume and enters the echo loop. `KernelPanic`, the unrecoverable-error path. |
| `drivers/vga/vga.c` | The VGA text-mode display driver: the control characters, the scrolling, the colour attributes, the hardware cursor and the erase limit that bounds a backspace. |
| `drivers/serial/serial.c` | The interrupt-driven COM1 serial driver used for diagnostics and input. |
| `drivers/pic/pic.c` | The pair of cascaded 8259A interrupt controllers: their remapping, the masking of request lines, the routing of a request to the driver that claims it, and the end-of-interrupt protocol. |
| `drivers/pit/pit.c` | Counter 0 of the 8253 interval timer: the system tick, the elapsed-time conversion and the bounded wait. |
| `drivers/block/buffer.c` | The buffer cache above the block layer: the hash, the recency list, the reference discipline and the write-back policy. |
| `drivers/block/block.c` | The generic block-device layer: the registry of devices that transfer fixed-size blocks, and the validated path through which every caller above reaches a driver. |
| `drivers/ata/internal.h` | What the six units below share: the register and status constants, the table of devices found, the addresses each channel answers at, the accounting, and the register-level discipline. |
| `drivers/ata/ata.c` | The driver's state, its refusals, the initialisation that finds what is present, the device accessors and the binding to the block layer. |
| `drivers/ata/port.c` | The register-level discipline of the task file: the settling delay a selection must be followed by, the two waits every command is bracketed by, and the reset of a channel. |
| `drivers/ata/identify.c` | The identification of whatever stands at one of the four addresses, and the distinction between a device that is absent and one answering a different command set. |
| `drivers/ata/channel.c` | Where each channel actually answers: the base address registers of a controller in native mode, and the classification of storage this driver cannot reach at all. |
| `drivers/ata/transfer.c` | The transfer of sectors: the judgement of a request, both addressing forms, and the cache flush that makes a write durable. |
| `drivers/ata/report.c` | The report, including — for a machine upon which no disk was found — every controller the bus carries and why each was not reached. |
| `drivers/pci/pci.c` | The PCI configuration-space enumeration by access mechanism one: the walk of buses, devices and functions, and the searches by which a driver finds its hardware. |
| `drivers/ps2/ps2.c` | The 8042 controller itself: its configuration byte, written whole by the one module that owns it, and the two device ports it presents. |
| `drivers/keyboard/keyboard.c` | The PS/2 keyboard upon the controller's first port: initialisation, the decoding of scan code set 1, the modifier state and the circular event buffer. |
| `drivers/mouse/mouse.c` | The PS/2 mouse upon the controller's second port: the framing of a packet stream that has none, the nine-bit movement, the single inversion of the vertical sense, and the position the driver keeps. |
| `drivers/ahci/ahci.c` | The AHCI adaptor by first-party direct memory access: the handoff from the firmware, the ports it implements, the command list, and the region descriptors that name a caller's pages to the device. |
| `drivers/sdhci/sdhci.c` | The SD host controller and the card behind it: the card's own command set, the two encodings of its capacity, and the transfer through the buffer data port. |
| `linker.ld` | The link script establishing the higher-half image layout. |

## 4. Subsystem dependency ordering

The phase ordering of `PLAN.md` is dictated by the following dependencies, which
must not be violated.

```
Phase 1  Bootstrapping
   |
   +--> Phase 2  Memory management ------+
   |         ^                           |
   |         | (page-fault delivery)     | (kernel heap)
   |         |                           v
   +--> Phase 3  Interrupts -------------+--> Phase 4  Device drivers
                                                     |
                                                     v
                                            Phase 5  EXT2 filesystem
                                                     |
                                                     v
                Phase 6  Graphics, system calls, processes, SMP
                                                     |
                                                     v
                            Phase 7  Userland and C library
                                                     |
                                                     v
                                          Phase 8  Shell
                                                     |
                          +--------------------------+--------------+
                          v                          v              v
              Phase 9  Desktop           Phase 10  Crypto   Phase 11  Networking
                          |                          |              |
                          +--------------------------+--------------+
                                                     v
                                       Phase 12  UEFI transition
                                                     |
                                                     v
                                       Phase 13  Polish and hardening
```

Phases 2 and 3 are mutually dependent in one particular: the copy-on-write fault
handler of sub-task 2.7 cannot be exercised until the page-fault vector of
Phase 3 is installed. The dependency is resolved by implementing the memory
management structures of sub-tasks 2.1 to 2.6 first, then Phase 3, and finally
returning to sub-tasks 2.7 and 2.8.

**Status.** Phases 2 to 5 are complete, and Phase 6 is complete as far as
sub-task 6.10. The mutual dependency described above has been discharged:
sub-task 3.4 supplied the fault handler, sub-task 2.7 the copy-on-write
resolution beneath it, and sub-task 2.8 the address-space cloning that creates
the shared pages the resolution acts upon. Sub-task 3.5 remapped the interrupt
controllers, so that a device may be heard; 3.6 supplied the first device that
speaks and 3.7 the first that a person operates. Phase 4 supplied the devices
beneath a filesystem and Phase 5 the filesystem itself, which sub-task 5.8
completed by mounting an EXT2 volume through a virtual filesystem layer.

Phase 6 has since established the apparatus a privilege transition is performed
out of (6.1); acquired the linear framebuffer, mapped write-combining (6.2); the
primitives that draw into it (6.3); the font and console that put the boot log
back upon the screen the framebuffer had displaced (6.4); the pointer, which
required the 8042 controller to become a module of its own first (6.5); the
compositor beneath all of it, after which nothing reads the framebuffer (6.6);
the `SYSCALL` entry path, its dispatch table and its argument validation (6.7);
the ELF64 loader (6.8); the process, the thread and the context (6.9); and the
switch and the descent to privilege level 3 (6.10), at which point a program
first ran. Work continues at sub-task 6.11 — `fork()` upon the copy-on-write
substrate of Phase 2, with `execve()`, `exit()` and `wait()` beside it.

## 5. Privilege and address-space model

The kernel occupies the upper half of the canonical 48-bit address space and is
mapped into every address space, so that a system call or an interrupt requires
no change of the page-table root. User processes occupy the lower half. The
detailed layout is recorded in `MEMORY-LAYOUT.md`.

The machinery of the transition between the two privilege levels — the user-mode
descriptors and the order the processor's own arithmetic imposes upon them, the
task state segment holding the stacks the processor loads when it needs one it
can trust, and the three registers that configure `SYSCALL` — was established by
sub-task 6.1 and is recorded in `PRIVILEGE.md`. Sub-task 6.7 supplied the entry
path and the dispatch above it, and sub-task 6.10 the descent itself: a program
is loaded into an address space of its own, entered at privilege level 3 by
`IRETQ`, returned to by `SYSRET` when it makes a system call, and ended when it
faults. What does not yet exist is pre-emption — nothing takes a processor away
from a thread that has not given it up — which is the scheduler of sub-task 6.15.

## 6. Diagnostic policy

Two output paths are maintained from Phase 1 onward. The VGA text console is the
operator-facing path; the COM1 serial port is the machine-readable path, and is
the basis of the automated verification described in `TESTING.md`. Every
diagnostic message of consequence is written to both, so that a failure is
recorded irrespective of which device remains functional.

From sub-task 4.1 the serial path is buffered and carried by interrupt, but it
retains its polled path and reverts to it whenever the interrupt flag is clear.
That is not a fallback for hardware that fails: it is the ordinary path of a
panic, which reports with interrupts disabled and must not be left holding its
message in a buffer that nothing will drain. `docs/devices/SERIAL.md`, Section 4, records
the rule and the single place it is decided.

From sub-task 4.2 the display path is a formal driver equally. It is not the path
the tests read and not the path a panic can most be relied upon to reach; it is
the path a person looking at the machine has, and the property it is built for is
that the machine can verify what it displayed rather than merely that it wrote
something. `docs/devices/DISPLAY.md`, Section 8, records why a display is unusually hard
to test and what is asserted at each boot in consequence.

**From sub-task 6.2 the operator-facing path is addressed two ways, and the policy
above must be read accordingly.** The kernel asks the boot loader for a linear
framebuffer, and a boot loader that supplies one sets a graphics mode to do it;
the display driver then writes to memory the adapter is not displaying. Sub-task
6.4 supplies a console that draws text upon the framebuffer, so from that point
there are **three** output paths and the operator can see whichever of the first
two the boot loader's chosen mode makes visible.

`KernelWriteString` writes to all three unconditionally and decides between none
of them. Deciding there would put knowledge of the display mode into the one
routine that must work before anything has established what the mode is, and the
two screen paths do not know about each other. That routine is also **the only
one permitted to name an output device**: the numeric routines named the display
and the serial port themselves until sub-task 6.4, and the console was in
consequence shown every word of the boot log and not one of its numbers.

Between sub-tasks 6.2 and 6.4 the cost was real and is worth recording: a machine
with no serial adapter this kernel detects — VirtualBox is one — had **no readable
diagnostic output at all**. `docs/design/GRAPHICS.md`, Sections 7 and 19, records
the position, and `docs/project/TESTING.md`, Section 9.1, what it cost the
VirtualBox procedure.

**A fault that the kernel cannot survive leaves the ordinary paths and takes the
display.** Which faults those are is decided by `ExceptionDispositionOf` and set
out in `INTERRUPTS.md`, Section 8.1: an abort, a non-maskable interrupt, a
malformed descriptor table, or any fault the kernel raised within itself. A
divide by zero or an unresolved page fault raised by a program belongs to that
program, costs it alone, and draws nothing — announcing the end of the machine
for one would be a false account of what happened.

Such a fault draws a full-screen page composed for it — its own
title, colour, account of what the processor is reporting, and the evidence that
bears upon it rather than upon the others — and the console is suspended for good
when it does. That is not a duplicate of the report: the report goes to every
path and remains the record; the screen is a summary for a person standing at a
machine that has stopped, who may have no other channel at all.
`docs/design/GRAPHICS.md`, Sections 24 and 25.
