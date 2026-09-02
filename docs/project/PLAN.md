# Oxys-OS Development Plan

**Document status**: Living document. This file is the single source of truth for
task tracking and shall be updated in every session in which a functional change
is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Target architecture**: x86_64.
**Boot protocol**: Multiboot2 (legacy BIOS, GRUB) initially; native UEFI added in Phase 12.
**Kernel model**: Monolithic.

## Current status

This section states the present condition of the work. The chronological account
of how that condition was reached is the Revision History at the foot of this
document.

**Phase 1 is complete but for sub-task 1.12.** The kernel builds without
diagnostics under `-Wall -Wextra -Werror`, is confirmed Multiboot2 compliant by
`grub-file`, and boots under both QEMU and VirtualBox, where it presents its
banner upon the VGA text console and the COM1 serial port alike. Sub-task 1.12,
boot from a physical USB medium, remains open.

**Phase 2 is complete.** The Multiboot2 information structure is parsed into a
boot-protocol-neutral `BootInformation` description; a bitmap allocator governs
every physical frame and reserves those that are not free; a permanent paging
hierarchy maps the kernel text and read-only data without write permission and
the whole of physical memory at `0xFFFF800000000000`; a kernel virtual arena and
a slab heap above it serve allocations of arbitrary size; every frame carries a
reference count and is returned to the allocator only upon the release of its
last reference; the page-fault handler resolves a copy-on-write fault by
duplicating a shared frame, or by restoring write permission where the frame has
but one referrer; and an address space may be created, cloned by the
copy-on-write discipline, activated and destroyed. The substrate upon which
`fork()` is built in sub-task 6.6 is therefore complete.

**Phase 3 is complete.** The interrupt descriptor table is loaded, a stub is
installed for each of the 256 vectors and constructs a uniform
trap frame whatever the vector, a dispatch table routes each vector to a
registered handler that may alter the frame to which control returns, and every
architecture-defined exception has a handler that decodes its error code and
emits a full diagnosis. The pair of cascaded 8259A controllers is remapped to
vectors 32 to 47, clear of the architecture-defined exceptions it would otherwise
be indistinguishable from; every request line begins masked; a routing layer
delivers a request to the driver that claims its line and signals the
end-of-interrupt to both controllers upon its return; and a spurious request is
recognised by the absence of its bit from the in-service register and deliberately
left unacknowledged. Counter 0 of the 8253 interval timer is programmed as a rate
generator at 1000.152 Hz, claims the controller's IR0 line and maintains a
monotonic tick counter, from which elapsed time and a bounded wait are derived;
it is the first device to claim a line, and therefore the first demonstration
that the whole path from a device to its driver is sound. The PS/2 keyboard is
initialised through the 8042 controller, whose translation of scan code set 2
into set 1 is established rather than assumed; scancodes are decoded into key
events carrying the character, the modifiers and the distinction between a
depression and a release, and delivered through a circular buffer of 128 events
whose overrun is counted rather than silent. The interrupt flag may now be set
safely. A minimal kernel global descriptor table was established in the course of
this work, for the reasons given in `docs/design/INTERRUPTS.md`, Section 5; it is
recorded against sub-task 6.1, which it partly discharges.

Every property above is asserted at each boot by a self-test, no test harness
being available before Phase 7. The procedure is `make verify`, described in
`docs/project/TESTING.md`.

At the completion of Phase 3 the kernel enters an echo loop rather than halting,
printing every character typed. This is the project's first end-to-end exercise
of a device: a physical keystroke traverses the controller, the interrupt
controller, the handler and the decoder, and emerges as a character.

The display driver implements the backspace, which it had not, and its cursor
movements are asserted at each boot by a self-test of their own.

**Phase 4 is complete.** Sub-task 4.1: the serial routine of Phase 1 is
now a formal driver. Its line parameters — signalling rate, word length, parity
and stop bits — are configurable, and a configuration the adapter cannot express
is refused rather than approximated. It claims IR4, buffers both directions, and
services the adapter's interrupt sources until none remains pending. It retains
its polled path, which is what carries a panic: the interrupt flag is clear at
that moment, and a driver that queued the message would halt without transmitting
it. The receive path is exercised end to end by the echo loop, which now drains
the serial line as well as the keyboard.

Sub-task 4.2 is complete: the display routine of Phase 1 is now a formal driver
likewise. Which registers the adapter answers upon is read from the Miscellaneous
Output Register rather than assumed; the hardware cursor may be positioned,
hidden, shaped and read back out of the CRT controller; blinking is disabled
through the attribute controller, so that bit 7 of the attribute brightens the
background and all sixteen colours are available as one; and the frame buffer may
be read back, which is what allows the machine to assert what it displayed.

The backspace now crosses from one row into the row above and stops immediately
after the text standing there, having consumed the separator between the rows and
nothing else. That movement was previously refused because the driver
could not distinguish a line of input from the boot log and would have consumed
the log a character at a time. The remedy was to supply the missing knowledge
rather than to withhold the movement: the driver holds an **erase limit**, a
position before which no backspace may retreat, and whoever reads the input sets
it where the input begins. The serial terminal is told of the same movement by
the ECMA-48 sequences CUU and CHA, a backspace not expressing it.

Sub-task 4.3 is complete: the PCI configuration space is enumerated by access
mechanism one. Every device driven before it was found by knowing where it is,
those addresses being inherited from the IBM Personal Computer; no device
introduced afterwards may be assumed in the same way, which is why the
enumeration precedes the disk driver rather than following it. Buses are reached
through the host bridge and through each PCI-to-PCI bridge found, rather than
swept; the queue is explicit rather than the call stack and each bus is visited
once. Nothing is claimed or configured — the enumeration establishes only what
the machine contains and where each part of it answers.

Sub-task 4.4 is complete: an ATA driver in programmed input/output mode resets
both channels, identifies the four devices they may carry, and reads and writes
sectors by 28-bit and 48-bit addressing. It is the first driver whose failures
are silent in the ordinary case — a driver that reads the wrong sector returns
data, and data that arrived cannot be distinguished from data that is correct
until something interprets it — and it is the first that can destroy something.
The self-test therefore asserts the relationship between sectors rather than
merely that a read succeeded, and it writes only when the operator has booted the
GRUB entry that permits it, restoring the sector afterwards.

Sub-task 4.5 is complete: a generic block-device layer stands above the disk
drivers. A driver registers a device by supplying two operations, a context and a
geometry, and every request is judged before a driver is reached, so that the
four tests each driver would otherwise repeat are written once. It is asserted
against a device made of memory: the machine `make verify` runs upon has no disk,
and a machine that has one holds data a self-test must not write to.

Sub-task 4.6 is complete: a buffer cache of sixty-four blocks stands above the
block layer, found through a hash of the device and the block number, discarded
in least recently used order, and written back rather than through. A buffer a
caller is holding is never taken from them.

**Phase 4 is therefore complete.**

**Phase 5 has begun.** Sub-task 5.1 is complete: the superblock of an EXT2 volume
is read through the buffer cache, decoded field by field from the volume's byte
order into the processor's, and validated. Every other structure of the format is
found by arithmetic upon it, so a superblock read wrongly yields no error but a
filesystem that addresses the wrong blocks confidently for as long as the machine
runs; the sub-task is accordingly as much validation as parsing. A volume
declaring an incompatible feature this kernel lacks is refused, one declaring a
read-only compatible feature it lacks is accepted read-only, and the group count
is derived twice from independent fields and the two required to agree.

Sub-task 5.2 is complete: the block group descriptor table is read and
validated. It is the structure every other structure of the filesystem is found
through — an inode is reached by dividing its number into a group, reading that
group's descriptor for the block its inode table begins at, and indexing into it
— and a descriptor is six numbers, each plausible wherever it is read from. The
individual checks are joined by one statement about the table as a whole: the
groups must account for every free block and every free inode the superblock
reports.

Sub-task 5.3 is complete: an inode is retrieved by number and the index of a
block within a file is resolved to a block of the volume through however many
levels of indirection it requires. Locating an inode is three pieces of
arithmetic upon numbers beginning at one and indices beginning at zero, and
every plausible mistake in it lands upon another valid inode of the same volume;
resolving a block pointer fails the same way. A zero entry is a hole and not the
end of the file, at any of the three levels.

Sub-task 5.4 is complete: a directory is traversed and an absolute path is
resolved to the inode it names. An inode describes a file entirely without
naming it, and a directory is what supplies the name — an ordinary file, with
ordinary blocks resolved by sub-task 5.3, whose data happens to be a linked list
of entries. It is the first structure of the volume whose contents are variable
rather than fixed: an entry is as long as its record length says and the next
begins wherever that lands, so every mistake in reading one is self-propagating
and what comes out of it is fragments of real names rather than an error. The
two readings of the two bytes at offset 6 — a name length of sixteen bits, or one
of eight followed by a file type — are distinguished by the incompatible feature
flag and by nothing else, that being the one place in the format where the same
bytes have two lawful meanings and the wrong choice produces no diagnostic of its
own. Records naming inode 0 are passed over rather than reported, holes are
passed over rather than read as entries of no length, and `.` and `..` are
resolved as the ordinary entries every EXT2 directory holds upon the volume
rather than being interpreted. Corroborated against `mke2fs` images: a root of
46 entries in two blocks listed with the inode numbers and offsets `debugfs`
gives, a path of three components resolved through five lookups to the inode
`debugfs` names, and a root of 9003 entries in 530 blocks counted exactly,
which required the traversal to cross both the direct-to-indirect and the
indirect-to-doubly-indirect boundaries without losing or repeating a record.

Sub-task 5.5 is complete: the contents of a file are read, and a symbolic link
is followed. Reading is the shortest piece of work in the phase precisely because
the locating was done properly before it — the whole of it is the arithmetic of a
byte range against a block size, and one call per block to the resolver of
sub-task 5.3. Two things it does are decisions rather than mechanism. A hole
reads as zeroes rather than being refused, the contents of a file at a hole being
zeroes by definition and a reader being unable to distinguish one from a block
written with zeroes, which is the point of a hole. And the end of a file is
reported by the count and not by the return value, since every reader arrives at
the end and a kernel reporting it as an error would oblige each of them to treat
the conclusion of its work as a fault.

A symbolic link holds its target in one of two places, and which of them is
decided by the sectors the inode declares and not by the size of the target: the
two agree upon every link a filesystem creates, and part when the inode carries
an extended attribute block, which `i_blocks` counts and which is not data. With
targets readable, the resolver of sub-task 5.4 now follows a link wherever it
stands, resolving a relative target against the directory holding the link and
bounding the depth against a link that names itself, which the format permits and
cannot prevent. A second entry point returns a link standing last unresolved,
which is the distinction between acting upon a file and acting upon its name.

This work exposed a defect in sub-task 5.3: `Ext2ReadInode` validated all fifteen
words of `i_block` as block numbers, which they are for every file but a fast
symbolic link, where they are the target's text. Every such link upon every real
volume was refused, and the diagnosis named a block pointer that was not one.

Sub-task 5.6 is complete: a volume may be altered. The bitmaps are read — the
first structures of the format this kernel needed for something other than
reading a file, every earlier sub-task having read the free counts and believed
them — and blocks and inodes are allocated from them and returned to them. A
file may be written within itself, extended, grown through every level of the
indirection, and truncated, with the blocks and the blocks of pointers freed as
they cease to be needed.

Three disciplines govern all of it, and they are the substance of the sub-task
rather than its preamble. A volume this kernel judged unsafe to write is refused
in one place rather than by each caller remembering. A resource is marked used
before anything refers to it, because the two orders fail differently at the one
moment that matters: this one leaks a block, and the reverse shares a block
between two files, and a leak is a cost where sharing is a fault that spreads.
And nothing reaches the medium until the cache writes it back, so a sequence of
writes is not atomic and this kernel does not pretend it is — that is what a
journal provides and what EXT2 has not got.

Setting a bitmap bit already set, or clearing one already clear, is refused
rather than performed. Neither announces itself at the moment it occurs, and the
second is precisely what allows one block to be given to two files.

Sub-task 5.7 is complete: a file may be named. Sub-task 5.6 could write a file
and not name one — an inode allocated and filled that no directory named, and so
reachable by no path, the only record of its existence being a bit in the inode
bitmap. This closes that gap, and is the first work in which the inode table and
the directories must be kept in step with one another.

A name is inserted into the slack a removal left behind: a record longer than the
name it holds is shortened to what it needs and the new record laid in the
remainder, and only when no block has room is a block added. A name is removed by
lengthening the record before it, or, where it is the first of its block, by
marking it unused where it stands — which is why a traversal must pass over such
a record, the two facts being one fact seen from each end. Nothing is ever moved,
so creating and removing a name repeatedly reuses one piece of space rather than
growing the directory.

A directory is not merely an inode of a different mode. It must hold "." and
".." before any path resolves through it, its own link count is two rather than
one, and its parent's link count rises by one for the ".." standing in the child.
The last of those is easy to omit and impossible to see: a parent short by one
may be freed while a child still names it, and nothing reports that until the
freed blocks are given away. A file is destroyed only when its last name goes,
the link count being what says how many paths reach it, and the name is removed
before the inode is freed so that a machine stopping between them leaks an inode
rather than leaving a name that leads to one already reissued.

This work also added a refusal to sub-task 5.3: destroying a file leaves its mode
and its block pointers exactly as they were, so nothing distinguishes a destroyed
inode from a live one but a link count of zero and a deletion time, and
`Ext2ReadInode` now refuses one bearing both.

**The next work is sub-task 5.8**, the virtual filesystem layer and the mounting
of an EXT2 root volume, which completes the phase.

## Legend

| Marker | Meaning |
| ------ | ------- |
| `[ ]`  | Not commenced. |
| `[~]`  | In progress. |
| `[x]`  | Completed and verified by test. |

---

## Phase 1 — Bootstrapping and Early Output

**Objective**: Produce a bootable ISO image containing a Multiboot2-compliant
ELF64 kernel that enters long mode, relocates to the higher half of the virtual
address space, and emits identifying output.

**Specifications**: Multiboot2 Specification 2.0; Intel SDM Volume 3A,
Chapters 2, 4 and 9; System V ABI for AMD64.

- [x] 1.1 Verify the cross-compilation toolchain (`x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`, `xorriso`) is present and functional.
- [x] 1.2 Author the linker script `linker.ld` defining a higher-half kernel at virtual base `0xFFFFFFFF80000000` with a physical load address of `0x00100000`.
- [x] 1.3 Author `boot/boot.asm`: Multiboot2 header, 32-bit protected-mode entry point, Multiboot2 magic validation, CPUID and long-mode feature detection.
- [x] 1.4 Construct the boot-time paging hierarchy (PML4, two PDPTs, one PD) providing a 1 GiB identity map and a coincident 1 GiB higher-half map using 2 MiB pages.
- [x] 1.5 Perform the long-mode transition (CR4.PAE, IA32_EFER.LME, CR0.PG) and load a 64-bit GDT.
- [x] 1.6 Transfer control to the higher-half 64-bit kernel entry point and establish the kernel stack.
- [x] 1.7 Implement a minimal VGA text-mode output routine and a minimal COM1 serial output routine for early diagnostics.
- [x] 1.8 Implement `KernelMain`, which clears the screen and prints the string "Oxys-OS".
- [x] 1.9 Author the `Makefile` with the targets `all`, `clean`, `iso`, `run-qemu`, `run-vbox` and `run-uefi`.
- [x] 1.10 Generate the ISO image and verify boot under QEMU.
- [x] 1.11 Verify boot under VirtualBox.
- [ ] 1.12 Verify boot on physical hardware from a USB medium.

---

## Phase 2 — Memory Management (including Copy-on-Write)

**Objective**: Establish complete control of physical and virtual memory, and
provide the copy-on-write primitives upon which `fork()` will later depend.

**Specifications**: Intel SDM Volume 3A, Chapter 4 (Paging) and Section 6.15
(Page-Fault Exception); Multiboot2 Specification, Section 3.6 (memory map tag).

- [x] 2.1 Parse the Multiboot2 information structure and extract the memory map (tag type 6) and the ELF section headers (tag type 9).
- [x] 2.2 Implement a physical frame allocator (bitmap) covering all usable regions, reserving the kernel image, the Multiboot2 structures and the low 1 MiB.
- [x] 2.3 Construct a permanent kernel page-table hierarchy, replacing the boot-time tables and removing the low identity map.
- [x] 2.4 Implement a direct physical map region for kernel access to arbitrary frames.
- [x] 2.5 Implement a kernel virtual-address-space allocator and a general-purpose kernel heap (slab allocator over a buddy-style page allocator).
- [x] 2.6 Implement per-frame reference counting as the substrate for shared pages.
- [x] 2.7 Implement the page-fault handler dispatch path (dependent upon Phase 3) and the copy-on-write fault resolution routine.
- [x] 2.8 Implement address-space cloning that marks writable user pages read-only and increments frame reference counts.

---

## Phase 3 — Interrupts, Exceptions and Keyboard Input

**Objective**: Install a complete interrupt descriptor table, service processor
exceptions, and accept keyboard input.

**Specifications**: Intel SDM Volume 3A, Chapter 6; Intel 8259A datasheet;
IBM PS/2 controller documentation.

- [x] 3.1 Define the IDT and the 64-bit interrupt-gate descriptor format; load it with `lidt`.
- [x] 3.2 Author assembly stubs for vectors 0–255, normalising the presence or absence of a processor-pushed error code.
- [x] 3.3 Implement a C interrupt dispatcher operating on a formal trap frame structure.
- [x] 3.4 Implement exception handlers with register and stack diagnostics emitted over the serial port.
- [x] 3.5 Remap the 8259A PIC to vectors 32–47 and implement end-of-interrupt signalling.
- [x] 3.6 Implement the Programmable Interval Timer as the initial timer source.
- [x] 3.7 Implement the PS/2 keyboard driver: controller initialisation, scancode set 1 translation, modifier state and a circular input buffer.

---

## Phase 4 — Basic Device Drivers

**Objective**: Provide the device support required by the filesystem and by
subsequent user-facing subsystems.

**Specifications**: PC16550D UART datasheet; ATA/ATAPI Command Set (ACS-3);
PCI Local Bus Specification 3.0.

- [x] 4.1 Promote the early serial routine to a formal, interrupt-driven COM1 driver with configurable line parameters.
- [x] 4.2 Promote the early VGA routine to a formal text-mode driver with scrolling, cursor control and colour attributes.
- [x] 4.3 Implement PCI configuration-space enumeration by the legacy I/O port mechanism, with device and class identification.
- [x] 4.4 Implement an ATA PIO driver: bus reset, `IDENTIFY DEVICE`, 28-bit and 48-bit LBA sector read and write.
- [x] 4.5 Define a generic block-device abstraction layer above the ATA driver.
- [x] 4.6 Implement a buffer cache for block devices.

---

## Phase 5 — EXT2 Filesystem

**Objective**: Mount, read and write an EXT2 volume.

**Specifications**: The Second Extended File System (Poirier); Linux kernel
documentation, `Documentation/filesystems/ext2.rst`.

- [x] 5.1 Parse the superblock and validate the EXT2 magic number and revision level.
- [x] 5.2 Parse the block-group descriptor table.
- [x] 5.3 Implement inode retrieval and the resolution of direct, singly, doubly and triply indirect block pointers.
- [x] 5.4 Implement directory-entry traversal and absolute path resolution.
- [x] 5.5 Implement file reading.
- [x] 5.6 Implement block and inode allocation, file writing, extension and truncation.
- [x] 5.7 Implement directory creation and entry insertion and removal.
- [ ] 5.8 Define a virtual filesystem layer and mount an EXT2 root volume.

---

## Phase 6 — System Calls, Process Management and Symmetric Multi-Processing

**Objective**: Execute user-mode programs, schedule them across multiple
processors, and expose kernel services by system call.

**Specifications**: Intel SDM Volume 3A, Chapters 5, 8 and 11, and Volume 2B
(`SYSCALL`/`SYSRET`); System V ABI for AMD64; Intel MultiProcessor
Specification 1.4; ACPI Specification 6.5 (MADT).

- [~] 6.1 Install the GDT and TSS required for privilege transition; configure IA32_STAR, IA32_LSTAR and IA32_FMASK. *(The kernel GDT and its null, code and data descriptors were established early, in Phase 3; what remains is the user-mode descriptors, the task state segment and the system-call MSRs.)*
- [ ] 6.2 Implement the `SYSCALL` entry path, the system-call dispatch table and argument validation.
- [ ] 6.3 Implement the ELF64 loader for statically linked executables.
- [ ] 6.4 Define the process control block, the address-space descriptor and the thread structure.
- [ ] 6.5 Implement context switching and the initial transition to user mode via `IRETQ`.
- [ ] 6.6 Implement `fork()` upon the Phase 2 copy-on-write substrate, together with `execve()`, `exit()` and `wait()`.
- [ ] 6.7 Parse the ACPI MADT; initialise the Local APIC and the I/O APIC; retire the 8259A PIC.
- [ ] 6.8 Implement application-processor bring-up by INIT-SIPI-SIPI and a real-mode trampoline.
- [ ] 6.9 Implement spinlocks, per-CPU data areas and inter-processor interrupts, including TLB shootdown.
- [ ] 6.10 Implement a multiprocessor-aware round-robin scheduler with per-CPU run queues and processor affinity.

---

## Phase 7 — Userland and Minimal C Library

**Objective**: Provide the runtime environment in which user programs execute.

**Specifications**: ISO/IEC 9899:2011; System V ABI for AMD64.

- [ ] 7.1 Implement the freestanding string and memory functions (`<string.h>`).
- [ ] 7.2 Implement system-call wrappers for the complete kernel interface.
- [ ] 7.3 Implement a user-space heap allocator (`malloc`, `free`, `realloc`) above `brk`/`mmap`.
- [ ] 7.4 Implement buffered input and output (`<stdio.h>`) and formatted conversion.
- [ ] 7.5 Author the C runtime startup object (`crt0`) and the static-linking procedure for user programs.
- [ ] 7.6 Implement the utilities `ls`, `cat`, `echo`, `mkdir` and `rm`.
- [ ] 7.7 Construct an initial ramdisk containing the utilities and mount it as the early root.

---

## Phase 8 — Shell

**Objective**: Provide an interactive command interpreter.

- [ ] 8.1 Implement line editing with history.
- [ ] 8.2 Implement the tokeniser and the command parser.
- [ ] 8.3 Implement built-in commands (`cd`, `exit`, `export`, `pwd`).
- [ ] 8.4 Implement external program execution by `fork()` and `execve()`.
- [ ] 8.5 Implement input and output redirection.
- [ ] 8.6 Implement pipelines.
- [ ] 8.7 Implement job control, process groups and terminal signal delivery.

---

## Phase 9 — Graphical User Interface

**Objective**: Provide a graphical display and a window system.

**Specifications**: VESA BIOS Extensions 3.0; UEFI Specification 2.10,
Section 12.9 (Graphics Output Protocol); Multiboot2 Specification, Section 3.6.12
(framebuffer information tag).

- [ ] 9.1 Request a linear framebuffer by the Multiboot2 framebuffer tag and map it into kernel space.
- [ ] 9.2 Implement 2D primitives: pixel, line, rectangle, blit and clipping.
- [ ] 9.3 Implement a bitmap font renderer.
- [ ] 9.4 Implement a PS/2 mouse driver and a cursor.
- [ ] 9.5 Implement a compositing surface abstraction and double buffering.
- [ ] 9.6 Implement a stacking window manager with focus and event routing.
- [ ] 9.7 Implement the client protocol by which user processes create and draw windows.
- [ ] 9.8 Implement demonstration applications: a terminal emulator window and a clock.

---

## Phase 10 — Cryptography

**Objective**: Provide primitives for random-number generation, hashing and
symmetric encryption.

**Specifications**: FIPS 180-4 (SHA-256); FIPS 197 (AES); NIST SP 800-38A
(modes of operation); NIST SP 800-90A (deterministic random bit generators);
Intel SDM Volume 2B (`RDRAND`, `RDSEED`).

- [ ] 10.1 Implement an entropy pool seeded from `RDSEED`/`RDRAND` where available and from timer jitter otherwise.
- [ ] 10.2 Implement a cryptographically secure deterministic random bit generator.
- [ ] 10.3 Implement SHA-256 with the FIPS 180-4 test vectors.
- [ ] 10.4 Implement AES-128 and AES-256 with the FIPS 197 test vectors.
- [ ] 10.5 Implement CBC and CTR modes of operation.
- [ ] 10.6 Expose the primitives to user space by system call and by a `/dev/random` device node.

---

## Phase 11 — Networking

**Objective**: Provide a functional TCP/IP stack.

**Specifications**: IEEE 802.3; RFC 826 (ARP); RFC 791 (IP); RFC 792 (ICMP);
RFC 768 (UDP); RFC 9293 (TCP); RFC 2131 (DHCP); Realtek RTL8139 or Intel 8254x
datasheet.

- [ ] 11.1 Implement an Ethernet controller driver (RTL8139 or Intel E1000) with descriptor rings and interrupt handling.
- [ ] 11.2 Define the network buffer structure and the protocol layering framework.
- [ ] 11.3 Implement Ethernet frame transmission and reception.
- [ ] 11.4 Implement ARP with a resolution cache.
- [ ] 11.5 Implement IPv4, including fragmentation and reassembly, and a routing table.
- [ ] 11.6 Implement ICMP echo request and reply.
- [ ] 11.7 Implement UDP.
- [ ] 11.8 Implement TCP: the state machine, sequence-number handling, retransmission and flow control.
- [ ] 11.9 Implement the BSD-style socket system-call interface.
- [ ] 11.10 Implement DHCP client configuration and the `ping` utility.

---

## Phase 12 — UEFI Transition

**Objective**: Boot the identical kernel by native UEFI as well as by legacy
BIOS.

**Specifications**: UEFI Specification 2.10; Microsoft PE/COFF Specification;
ACPI Specification 6.5.

- [ ] 12.1 Establish a PE32+ build path for a UEFI application image.
- [ ] 12.2 Implement the UEFI entry point and parse the System Table.
- [ ] 12.3 Retrieve the memory map, the ACPI RSDP and the Graphics Output Protocol framebuffer by Boot Services.
- [ ] 12.4 Define a boot-protocol-neutral handoff structure consumed by the kernel, populated identically from Multiboot2 or from UEFI.
- [ ] 12.5 Invoke `ExitBootServices` and transfer control to the kernel.
- [ ] 12.6 Integrate UEFI Runtime Services: time and variable access.
- [ ] 12.7 Produce a hybrid ISO image bootable by both BIOS and UEFI, and verify under OVMF.

---

## Phase 13 — Polish, Optimisation and Final Hardening

**Objective**: Prepare the system for release.

**Specifications**: Intel SDM Volume 3A, Chapters 4 and 5 (SMEP, SMAP, NX).

- [ ] 13.1 Profile interrupt latency, context-switch cost and filesystem throughput.
- [ ] 13.2 Optimise the scheduler for fairness and the block layer for read-ahead.
- [ ] 13.3 Enable NX, SMEP and SMAP; enforce write-exclusive-or-execute in kernel mappings.
- [ ] 13.4 Implement kernel stack guard pages and stack-canary protection.
- [ ] 13.5 Implement kernel address-space layout randomisation, if feasible.
- [ ] 13.6 Complete and review the whole of the `docs/` corpus.
- [ ] 13.7 Test on a minimum of three distinct physical machines, including UEFI systems.
- [ ] 13.8 Extend the userland utility set and produce the final release image.

---

## Revision History

The rows are ordered with the most recent first.

| Date | Phase affected | Summary |
| ---- | -------------- | ------- |
| 2026-09-02 | Phase 5 | Sub-task 5.7 completed. A file may be named. Until now a file could be written and not named — an inode allocated and filled that no directory named, reachable by no path, its existence recorded only by a bit in the inode bitmap — and this is the first work in which the inode table and the directories must be kept in step with one another. A name is inserted into the slack a removal left: a record longer than the name it holds is shortened to what it actually needs and the new record laid in the remainder, an unused record is taken whole if it is long enough, and only when no block has room is a block allocated and the directory extended, the new block holding one record spanning its whole length so that the ordinary search then finds room in it. Nothing is ever moved, which is the property the linked list exists to provide and the reason sixty-four insertions and removals of the same name consume no blocks at all. A name is removed by lengthening the record before it to cover it, the removed bytes staying where they are until an insertion takes them; where the record is the first of its block there is nothing before it to lengthen, so it is marked unused where it stands by having its inode number set to zero — which is the format's own discipline and precisely why a traversal must pass over such a record rather than reading the name still lying in it, the two being one fact seen from each end. A name already present is refused, a directory holding one name twice making the path to it ambiguous rather than merely untidy; "." and ".." may not be removed, a directory without them no longer knowing itself or its parent. A directory is not an inode of a different mode: it must hold "." and "..", the second record running to the end of its block; its own link count is two, its own entry and the parent's; and the parent's link count rises by one for the ".." standing in the child. That last is easy to omit and impossible to see — a parent short by one may be freed while a child still names it, and nothing reports it until the freed blocks are given away — and it is checked against the maximum before anything is allocated, so a refusal leaves no half-made directory. A file is destroyed only when its last name goes, the link count being what says how many paths reach it; the name is removed before the inode is freed, so a machine stopping between them leaks an inode a check reclaims rather than leaving a name that leads to an inode already reissued, and within the destruction the blocks go before the inode, which would otherwise leak them with nothing left to say which they were. A fast symbolic link is not truncated, the words of `i_block` being its target and not pointers. A directory may not be given a second name, two paths to one directory making a cycle in what the format requires to be a tree, and one that is not empty is not removed, everything within it becoming reachable by no path. A refusal was added to sub-task 5.3 in the course of this: destroying a file leaves its mode and its block pointers exactly as they were, nothing overwriting them and a recovery tool reading them for that reason, so nothing distinguishes a destroyed inode from a live one but a link count of zero and a deletion time; `Ext2ReadInode` now refuses one bearing both, since no name leads to such an inode and reading it would serve another file's data under the dead file's name. It was found by the self-test asserting that an inode freed with its last name could no longer be read, and discovering that it could. The composed volume grew from sixteen inodes to thirty-two, its table accordingly from two blocks to four and its data blocks two blocks later, there being creations to perform and nothing to perform them with; two assertions that had stated the old free counts now derive them. Corroborated upon a real volume: within one boot from the GRUB entry that permits writing, the kernel created a directory, created a file within it, wrote to that file and removed both, and `e2fsck -fn` afterwards reported no errors — Pass 2 checking the directory structure this kernel split and joined, Pass 3 the connectivity of the "." and ".." it wrote, and Pass 4 the reference counts it raised and lowered. The volume reported the same file count as before the test, so the two inodes created were genuinely returned. `docs/storage/EXT2.md` gained Section 13 and Section 14.11, its verification and limitations being renumbered. |
| 2026-09-02 | Phase 5 | Sub-task 5.6 completed. A volume may be altered. The two bitmaps are read — one bit per block and per inode, 1 meaning used, the first of a group being bit 0 of byte 0 and the ninth bit 0 of byte 1, which is the least significant bit first and not the order a diagram of a byte suggests — and they are the first structures of the format this kernel has needed for anything but reading a file: until now the free counts were read and believed. Blocks and inodes are allocated and freed, a file is written within itself, extended, grown through every level of the indirection and truncated, and the superblock, the group descriptors and the inodes are written back in the volume's own order by encoders that are the exact inverses of the parsers. Three disciplines govern all of it and are the substance of the work. A volume judged unsafe to write is refused once, in one place. A resource is marked used before anything refers to it: the order matters at the single moment a machine stops between the two writes, and the two orders fail differently — this one leaks a block, which costs space until a check reclaims it, and the reverse shares a block between two files, which is corruption that spreads and which reads correctly for both of them until one writes. And nothing reaches the medium until the cache writes it back, so allocating one block touches four structures that cannot be made atomic; this kernel does not pretend otherwise, that being what a journal provides and what EXT2 has not got, ext3 being ext2 with one added. The ordering discipline does not close the window but chooses which side of it the damage falls on. Setting a bitmap bit already set or clearing one already clear is refused rather than performed, the second being exactly what permits two owners of one block. Every structure is read before it is written and only the fields this kernel parses are altered, so that a journal identifier, a hash seed or the extensions of a 256-byte inode are not destroyed by a kernel that does not understand them. A block of pointers is zeroed as it is allocated, an unzeroed one being read as pointers to real blocks belonging to real files; a data block is zeroed when it is newly allocated and the write does not cover the whole of it, the remainder being otherwise the previous owner's data appearing as this file's contents — and the allocation reports whether it allocated rather than leaving the caller to infer it from the offsets, inferring it being how a caller gets that wrong. The search for a free block is bounded by the group's true extent and not by the size of the bitmap, the last group being short and the bits beyond it being set by whatever made the volume. A group whose descriptor claims free blocks its bitmap does not hold refuses the allocation rather than continuing in another group, which would leave the contradiction for the next caller. Truncation frees a table of pointers only when nothing remains in it, and retains without walking any subtree lying wholly below the new size; truncation upward allocates nothing, the file growing by a hole. The self-test is the first in this project that writes, and asserts the volume rather than the operation: counts are read back from the volume and not from memory, a file truncated to nothing and rewritten must take back exactly the blocks it gave up, a write into an unoccupied entry of the doubly indirect block must take two blocks and not one, and `Ext2VerifyGroupDescriptors` must still pass afterwards. Restoring the composed volume between sequences required reversing the order every earlier self-test uses — the cache emptied first and the composition second — since `BufferInvalidateDevice` writes dirty buffers back before discarding them and would otherwise flush the test's writes onto the volume just composed. Corroborated by the strongest evidence available: a real `mke2fs` image was written from the GRUB entry that permits it, and `e2fsck -fn` afterwards reported no errors through all five passes, Pass 5 checking the group summary information this kernel maintained; the 8192 bytes written matched the expected pattern byte for byte when extracted with `debugfs`, and the other file in the image was untouched. `docs/storage/EXT2.md` gained Section 12 and Sections 13.9 and 13.10, its verification and limitations being renumbered. |
| 2026-09-02 | Phase 5 | Sub-task 5.5 completed. The contents of a file are read, and a symbolic link is read and followed. Reading is the shortest piece of work in the phase because the locating was done properly before it: the whole of it is the arithmetic of a byte range against a block size — `index = offset / block_size`, `within = offset % block_size`, `take = min(block_size - within, remaining)`, which makes the first and last blocks of a range partial and every block between them whole without those three cases being written separately — and one call per block to the pointer resolution of sub-task 5.3, which knows how many levels of indirection were needed and does not say. Two of its properties are decisions rather than mechanism, and both are recorded because the opposite choice is defensible until it is examined. A hole reads as zeroes rather than being refused: the contents of a file at a hole are zeroes by definition rather than by accident, a reader cannot distinguish a hole from a block written with zeroes, and a kernel that refused would be unable to read most of the files a system holds. And the end of a file is reported by the count rather than by the return value: every reader arrives at the end, it is how reading concludes, and reporting it as an error would oblige each caller to treat the conclusion of its work as a fault and would leave it unable to distinguish that from a volume it could not read. A directory is refused outright, its bytes being entries that are read by traversing it. A symbolic link holds its target either within the inode, in the sixty bytes that would otherwise be fifteen block pointers, or in blocks like any other file; which of the two is decided by the sectors the inode declares less those of an extended attribute block, and not by the size of the target, the two agreeing upon every link a filesystem creates and parting exactly when such a block is present, where a test upon the size would read the target out of pointers to a block that exists. With targets readable, the resolver of sub-task 5.4 now follows a link wherever it stands, by resolving the target to an inode and continuing the original path from there rather than by splicing the target into the path, which needs no buffer for the spliced result; a relative target is resolved against the directory holding the link, which is the whole of the difference between a relative target and an absolute one; and the depth is bounded at eight, the format permitting a link that names itself and being unable to prevent one. `Ext2ResolvePathNoFollow` returns a link standing last unresolved, which is the distinction POSIX draws between acting upon a file and acting upon its name, and a trailing separator overrides it, a path asserting a directory asking for what the link names. This work exposed a defect in sub-task 5.3: `Ext2ReadInode` validated all fifteen words of `i_block` as block numbers, which they are for every file but a fast symbolic link, where they are text — the target `sub` read as a pointer is the word `0x00627573`, a block some millions beyond the end of any volume — so every fast symbolic link upon every real volume was refused, with a diagnosis naming a block pointer that was not one. The validation is now skipped for such an inode. Corroborated against an `mke2fs` image: a file's first sixteen bytes matched `xxd` upon the host byte for byte, a four-byte target was read from within its inode and a seventy-one-byte target from a block, `debugfs` reporting `Blockcount` 0 and 2 respectively, and a path passing through a link resolved to the inode and the contents `debugfs` states. `docs/storage/EXT2.md` gained Section 11 and Sections 12.5 and 12.8, its verification and limitations being renumbered. |
| 2026-09-02 | Phase 2 | `KernelPagesFree` now establishes that the whole range released lies within the arena, and not merely its first page. It validated the base address, the alignment and the mapping of every page, none of which prevents a base at the arena's last page from being given a count of 2^33: that satisfies both the base test and the capacity bound added earlier today while describing a range sweeping the 32 TiB above the arena. The base being known to lie within the arena, the test is the subtraction `page_count > (KERNEL_ARENA_BASE + KERNEL_ARENA_SIZE - base) / PAGE_SIZE`, whose difference lies in `(0, KERNEL_ARENA_SIZE]` and therefore cannot wrap, and which performs no multiplication at all; it subsumes the capacity bound, that being this same test for a base at the arena's first page, so the release path now applies this one alone while the allocation path — having no base to measure from — keeps the capacity bound. This corrected no silent corruption: the release loop met an unmapped page above the arena and panicked, so neither the accounting nor the free list was reached. It was nonetheless worth stating. The panic named the wrong error, reporting an unmapped page observed partway through a range rather than the range being wrong; it refused after having already unmapped pages and returned frames, which the fatality of a panic makes moot by luck rather than by design; and it held only for as long as nothing is mapped above the arena, that space being presently unassigned but expressly reserved by Section 2 of `docs/design/MEMORY-LAYOUT.md`. On the day something is placed there the loop would stop panicking and would instead unmap and free pages belonging to it, complete, and insert the range into the free list — an incidental protection depending upon a region being empty is a coincidence with an expiry date rather than a protection. The refusal itself is not asserted by the self-test and cannot be, every impossible argument to this function being fatal and no means of surviving a panic existing before the harness of Phase 7; what is asserted is the admit direction, a legitimate multi-page range being allocated, written, read back, released, reissued from the free list, released again, and the arena's pages in use required to return to exactly what they were, so that a bound inverted or off by one panics upon a legitimate range rather than passing silently. `docs/design/MEMORY-LAYOUT.md` gained Section 10.5. |
| 2026-09-02 | Phase 2 | Three integer-wrap defects corrected in the allocators of sub-task 2.5, found by review rather than by failure. `KernelPagesAllocate` bounded the request by comparing `ArenaBumpPointer + (page_count * PAGE_SIZE)` against the end of the arena, and that product is a 64-bit unsigned quantity: 2^52 pages multiply to exactly zero, so the bound became the bump pointer itself, and 2^38 pages give a product that carries the sum past the top of the address space and truncates to `0x0003C00000000000`, which compares below the arena's end. The guard computed a value the guard could not trust. This is defined behaviour and not undefined — unsigned arithmetic wraps by the standard — so it is a defect of logic rather than of conformance, which is why a compiler configured to refuse a great deal did not refuse it. The damage was not the request, which the mapping loop refused anyway upon exhausting physical memory; it was what the wrapped arithmetic left behind, the bump pointer standing in the lower half so that the next allocation would have been served from user address space and reported as a success, and a bogus range inserted into the free list by the unwinding, to be handed to somebody else long afterwards. `KernelPagesFree` had the same gap and could underflow the accounting and corrupt the free list likewise. Both entry points now refuse a count above `ARENA_PAGE_CAPACITY`, the pages the arena would hold were it empty, before any arithmetic is performed upon it; a count so bounded cannot wrap any product or sum below, so every later site is safe by construction rather than by a test repeated at each. The third defect was in the heap and was the worst of them: `AlignUp(size + sizeof(HeapPageHeader), PAGE_SIZE)` wraps for a size within `sizeof(HeapPageHeader) + PAGE_SIZE - 1` of `SIZE_MAX`, yielding a page count of one or two, so the allocation succeeded and returned a valid pointer to two pages for a request of very nearly the whole address space — no error reported, and the arena bound above cannot catch it, the count having already been made small. Such a size is now refused before the addition. None of the three was reachable from any existing caller, every one of which passes a small constant or a count derived from an internal structure; they are corrected now because Phase 6 is where sizes cease to be this kernel's own. The self-test asserts each refusal and, because a request of 2^52 pages returned NULL before the checks existed as well, asserts additionally that the pages in use are unchanged and that an ordinary allocation made afterwards still comes from within the arena — which is the assertion that distinguishes the two states. `docs/design/MEMORY-LAYOUT.md` gained Sections 10.4, 11.4 and 11.7. |
| 2026-09-02 | Phase 5 | Sub-task 5.4 completed. A directory is traversed entry by entry and an absolute path is resolved to the inode it names. The traversal rests entirely upon `Ext2InodeBlock` and adds nothing to it but an interpretation of the bytes, a directory being an ordinary file; what the work consists of is the validation, since a directory is the first structure of the volume whose contents are variable rather than fixed and every mistake in reading a record is therefore self-propagating through the rest of its block. Each rule the specification states is applied where the record is read: a record length below eight cannot be advanced past and the traversal would not terminate; one that is not a multiple of four leaves every record after it unaligned; one reaching beyond its block contradicts the rule that no entry spans two; and a name length above `rec_len - 8` reads the name out of the record that follows. The two readings of offset 6 are decided by `EXT2_FEATURE_INCOMPAT_FILETYPE` alone and not by the revision, a revision 1 volume being free to omit the feature: this is the one place in the format where the same two bytes have two lawful meanings and the wrong one produces no diagnostic, the entry `.` reading as a name of `1 + 256 × 2 = 513` bytes under the sixteen-bit interpretation, and the self-test presents the same bytes under both. A record naming inode 0 holds space and not a name and is passed over — which is also how the interior nodes of an indexed directory are disguised, so a linear traversal reads one correctly — and a block the directory never had allocated is passed over likewise, a hole reading as zeroes and a record length of zero being unadvanceable. `.` and `..` are not interpreted: every EXT2 directory holds them upon the volume as ordinary entries, the `..` of the root naming the root, so the ordinary lookup resolves them and interpreting them here would be second-guessing the volume. A lookup is given the address and length of a component rather than a terminated string, so a component is matched where it stands in the path; a name is matched by its whole length, a comparison stopping at the shorter of the two resolving a path to the wrong file. The file type an entry declares is checked against the format of the inode it names, the two being written at different times by different code. A name holding the separator or a null byte is refused although the format permits it, such a name being reachable by no path and equal to its own prefix once terminated. A symbolic link is returned as it stands as the last component and refused within a path, following one requiring sub-task 5.5. The composed volume of the self-test gained a subdirectory and a file within it, and with them a named `KERNEL_VOLUME_UNUSED_INODE`: the assertion that an unfilled inode is refused had reached its subject by adding one to the file's inode number, and the inodes in use grow as the volume acquires structure. The root directory of every volume the machine carries is now listed at boot and one path resolved upon it. `docs/storage/EXT2.md` gained Sections 10, 11.4 and 11.6. |
| 2026-09-01 | Phase 1 | The `LOAD` segments of the image separated by permission, an accepted condition since Phase 1 discharged ahead of sub-task 13.3. The linker had been left to infer the segments, and it packs sections into as few as it can and gives each the union of the permissions of what it holds; the image accordingly presented two segments, both readable, writable and executable. `linker.ld` now declares five in a `PHDRS` block. What made the division possible was separating the boot code from the boot data: they were one section because both are linked at their physical addresses and both are finished with before the permanent tables of Phase 2 are built, but the entry code is executed and never written and the paging structures are written and never executed, and nothing but their common lifetime placed them together. Each output section also states its own alignment as well as its address, without which a segment inherits the largest alignment among its input sections — 16 or 32 bytes — and its file offset ceases to be congruent to its address modulo a page, which is the congruence that lets a loader map the image rather than copy it. Five segments result, each page-aligned in address and file offset, none both writable and executable. This is the image's declaration of its permissions and not an enforcement of them: GRUB copies segments and does not apply their flags, and the mappings that enforce anything are the ones `PagingInitialise` builds. Sub-task 13.3 retains the enforcement work — NX, SMEP and SMAP — and is no longer answerable for this. `docs/design/BOOT.md`, Section 8, records the design. |
| 2026-09-01 | Phase 5 | Sub-task 5.3 completed. An inode is retrieved by number and a block index within a file is resolved to a block of the volume. The group is `(number - 1) / s_inodes_per_group` and the index within it the remainder, numbers beginning at one and indices at zero; every plausible mistake in that arithmetic — the subtraction omitted, the block size used where the inode size belongs, the group's first block taken for its inode table — lands upon another inode of the same volume, which is a valid inode belonging to a different file, and nothing in the machine can tell. The fifteen pointers of `i_block` are decomposed by the pointers a block holds, and one loop walks all three levels by dividing the offset by the span of an entry at each. A zero entry is a hole and not an end: the original implementation terminated the list upon one, but in a sparse file it means a block that was never allocated, and a zero pointer block is a hole occupying its whole subtree, so the same function serves every level and reads nothing when it meets one. The index is deliberately not checked against the file's size, a caller walking allocated blocks having no size to bound itself by. The high half of a regular file's size is joined from the field a revision 0 volume calls `i_dir_acl`, and only for a regular file — upon a directory those bytes mean something else, and joining them regardless would give a directory a size of gigabytes. An inode with no format and no links is refused, the zeroes past the end of a table being a valid encoding of nothing. Corroborated against two `mke2fs` images whose root directories reach the indirect and the doubly indirect blocks, the resolved blocks matching `debugfs` exactly, index 268 of the second having been reached through the doubly indirect block at 1054 and the indirect block at 1055. `docs/storage/EXT2.md` gained Sections 9 and 10.3. |
| 2026-09-01 | Phase 5 | Sub-task 5.2 completed. The block group descriptor table is read and validated. The table begins upon the block after the one the superblock lies within, which is `s_first_data_block + 1` whatever the block size, and a descriptor is located by its position within the table rather than within a block of it, the table occupying several blocks upon any sizeable volume. A descriptor is six numbers and every one of them is plausible wherever it is read from, so the validation is what the work consists of: the three structures must lie within the volume, no two may begin upon the same block, the inode table must end within the volume as well as begin within it — its length is stored nowhere and follows from the inode size — and no count may exceed what the group holds, the last group being short whenever the volume is not an exact multiple of the group size. The statement the table makes as a whole is that the groups account for every free block and every free inode of the volume, and a table read at the wrong offset or one descriptor short yields descriptors that are individually plausible and a sum that is not; it is asserted, but only upon a volume marked cleanly unmounted, a volume that was not being permitted to disagree with itself. Bytes are read from a block through the cache in the quantity wanted — 32 for a descriptor — rather than by copying a block onto the stack. Corroborated against two `mke2fs` images, whose group lines match `dumpe2fs` in full. `docs/storage/EXT2.md` gained Sections 8 and 9.2. |
| 2026-09-01 | All | `docs/project/INSPIRATIONS.md` added at the project owner's request. It records ToaruOS as the principal inspiration, in the architecture of a wholly original system of this scope and in the style of graphical environment Phase 9 intends, and states expressly that the theme of ToaruOS is not taken — the structure of a graphical environment is inherited, its appearance is not. SerenityOS is recorded for its retro graphical idiom, as a preference of taste for a phase not yet begun rather than as a decision already made. BSD is recorded for the coherence of a system whose kernel, userland and documentation are maintained as one thing, a practice this project already follows. The document exists chiefly to hold a line: `PROJECT_GUIDELINES.md`, Section 2, forbids the transcription of any reference implementation, and an inspiration is a reason for a decision and never a source for one. |
| 2026-08-31 | Phase 4 | Sub-task 4.1 completed. The serial routine of Phase 1 is now an interrupt-driven driver claiming IR4. The line parameters are computed from a signalling rate rather than stated as a divisor, and the rate realised is reported beside the rate requested, the division being truncating. The driver keeps both modes and needs both: it begins polled, because it serves the diagnostics of an initialisation that precedes the interrupt controller, and it falls back upon polling wherever the interrupt flag is clear, which includes every panic — a driver that assumed interrupts were available would queue a panic message and halt without transmitting it. The transmitter interrupt is enabled only while characters are waiting, the condition it reports being a level and not an event; an adapter with nothing to send holds it asserted permanently, and a driver that left it enabled would be seized by an interrupt no service could dismiss. A writer that fills the transmit buffer waits for room, a diagnostic channel that discards its output being worse than a slow one, while a receiver that fills the receive buffer discards the newest character and counts it, having no one to wait for. `ReadRflags` was added to `kernel/include/oxys/cpu.h`, which had named it in its header without providing it. `docs/devices/SERIAL.md` added and records the design. |
| 2026-09-01 | Phase 4 | Sub-task 4.2 completed. The display routine of Phase 1 is now a formal driver. The register configuration is read from the Miscellaneous Output Register rather than assumed, the cost of one input instruction being far below the cost of writing the cursor location to an address nothing decodes. The hardware cursor may be positioned, hidden, shaped and read back; the shape is left as the firmware established it, that shape being the one known to be legible upon the machine's own display. Blinking is disabled through the attribute controller, whose shared address and data port is reached through a flip-flop and whose every write is therefore read back, a write that arrived in the wrong state having altered some other register with no symptom the machine could detect. The backspace now crosses into the row above and stops upon the last character standing there, bounded by an erase limit that records where the input began; the objection recorded against this movement in sub-task 4.1 — that the driver could not tell a line of input from the boot log — was answered by supplying that knowledge rather than by withholding the movement. `KernelEchoBackspace` tells a serial terminal of the same movement by ECMA-48 CUU and CHA. `docs/devices/DISPLAY.md` added and records the design. |
| 2026-09-01 | Phase 4 | The backspace across a row boundary corrected. It stopped upon the last character of the row above, so that the erasing sequence the callers compose consumed both the separator between the rows and that character: one keystroke deleted two things. It now stops immediately after the text, consuming the separator alone, exactly as a backspace within a row consumes one character alone. A row that is entirely occupied remains the exception, having ended by wrapping rather than by a line feed and having no separator to consume. |
| 2026-09-01 | Phase 4 | Sub-task 4.3 completed. The PCI configuration space is enumerated by access mechanism one. The walk reaches buses through bridges rather than sweeping all 256, holds the queue of buses awaiting a scan explicitly rather than in the call stack, and records each bus as visited so that malformed hardware cannot send it around a cycle. A function that is not there returns all ones rather than failing, which is how absence is detected and also why the self-test asserts that particular devices were found: an enumerator with its address arithmetic wrong reports an empty machine, and an empty report is exactly what a machine with no devices produces. `docs/devices/PCI.md` added and records the design. |
| 2026-09-01 | Phase 4 | Sub-task 4.4 completed. An ATA driver in programmed input/output mode. Both addressing modes are implemented: the 28-bit form keeps four bits of the address in the register that selects the device, and the 48-bit form writes each register twice, high-order byte first, the device retaining the previous content in a hidden half. A count register of zero means the greatest count the mode allows, so the limits are 256 and 65536 sectors rather than 255 and 65535. A write is followed by a cache flush within the same sequence, a device that has accepted data without committing it reporting success and losing it, the loss appearing only upon a later read. The driver's only clock is the read of an I/O port: the interval timer counts by interrupt and the interrupt flag is clear throughout initialisation. This is the first driver that can destroy something, so the self-test reads unconditionally and writes only upon an option given at the GRUB menu, restoring the sector it wrote and verifying the restoration. `docs/storage/DISK.md` added and records the design. |
| 2026-09-01 | Phase 4 | Sub-task 4.5 completed. A generic block-device layer: a driver registers a device by supplying a read and a write operation, an opaque context and a geometry, and callers above address it by name and block number. The layer's purpose is the judgement it performs before a driver is reached — no driver is called with a null buffer, a count of zero, a range outside the device or a write to a read-only device — and its range check is a subtraction rather than an addition, a 64-bit block number near its greatest value making the obvious sum wrap so that a range wholly outside the device would be accepted. The adaptor presenting an ATA disk lives in the ATA driver, so that the dependency runs one way. The layer is asserted against a device made of memory, the machine the verification runs upon having no disk and a machine that has one holding data a self-test must not write to. The bound of the saved instruction pointer in the interrupt stub self-test was corrected to the linker's own text symbols; it had been the address of `KernelMain`, which asserted only that the compiler had placed that function before every other, and adding a function above it in the file was enough to fail the test with nothing wrong. `docs/storage/BLOCK.md` added and records the design. |
| 2026-09-01 | Phase 4 | Sub-task 4.6 completed, and with it Phase 4. A buffer cache of sixty-four blocks above the block layer. A buffer's identity is the device and the block number together, block zero of one device and block zero of another being different blocks; the device forms part of the hash as well as of the comparison. Eviction is by least recent use, and a buffer a caller holds is passed over rather than taken — the request is refused instead, handing the same storage to two callers being a defect that appears as corruption somewhere else entirely. The policy is write-back and not write-through, so that a filesystem updating an inode three times performs one sector write and not three; the obligation that follows is that an eviction writes back before it reuses the storage, a dropped dirty block losing a write already reported as successful. The first implementation linked its entries by a routine that begins with a detachment, and detaching an entry not in the list wrote nothing over both of its ends: each call emptied the list, and sixty-three of the sixty-four buffers were unreachable with no symptom but slowness. `docs/storage/BUFFER.md` added and records the design, that defect included. |
| 2026-09-01 | All | `docs/` reorganised into four groups at the project owner's request: `project/` for how the work is conducted, `design/` for the kernel itself, `devices/` for the hardware it drives, and `storage/` for the stack from a medium to a caller. The grouping is by subject and not by phase, a document being amended in every phase that touches its subject. Each group carries a `README.md` of its own and `docs/README.md` indexes them. Every cross-reference was rewritten and every link verified to resolve, in the documents, in the directory `README.md`s, in the source comments that cite a document by path, and in `PROJECT_GUIDELINES.md`. |
| 2026-09-01 | Phase 5 | Sub-task 5.1 completed. The EXT2 superblock is read through the buffer cache — the first caller above it, and the natural one, the superblock being the block a filesystem reads most often. It is decoded field by field from a buffer of bytes rather than by laying a structure over them: overlaying would require packing the structure, a compiler extension admitted only where no conforming alternative exists, and would make the format's little-endian order a property of the processor the code happens to be compiled for rather than a stated fact about the format. The field offsets are named in the header, so that the self-test composes a volume from the same names the parser reads; a test restating them would agree with a mistaken parser as readily as with a correct one. Validation is the bulk of the work, every other structure of the format being found by arithmetic upon this one: a superblock read wrongly produces no error but a filesystem that addresses the wrong blocks confidently. The strongest check derives the group count twice, from the block count and from the inode count, and refuses a volume whose two answers disagree. An incompatible feature this kernel lacks refuses the volume; a read-only compatible one, or a volume not cleanly unmounted, makes it read-only. `docs/storage/EXT2.md` added and records the design; `docs/project/CODING-STANDARDS.md` gained Section 7.1 upon the decoding of on-disk structures. |
| 2026-08-31 | Phase 3 | The backspace key repaired. `VgaPutCharacter` had no case for it, so the character fell through to the default and was written into the frame buffer as whatever glyph the adapter's font holds at code point 0x08, the cursor then advancing rightward; the key appeared to do nothing useful. The driver now implements the backspace as ANSI X3.4-1986 defines it, a movement one column to the left that erases nothing, and it stops in the first column rather than wrapping, because nothing records whether a row ended by wrapping or by a line feed and a wrap would therefore let a backspace destroy output the user never typed. The erasure belongs to the caller: `KernelEchoLoop` writes `"\b \b"`, which erases upon a serial terminal equally, the serial driver having transmitted the raw character to no visible effect. A display self-test was added, the driver having had none, with a `VgaCursorPosition` accessor for it to read; the failure mode it guards is silent to the machine and visible only to a person reading the screen, which is how this defect survived. |
| 2026-08-31 | Phase 3 | Sub-task 3.7 completed, and Phase 3 with it. The 8042 controller is initialised, its configuration byte written a second time after the controller self-test because that test resets the controller upon some implementations. The translation of scan code set 2 into set 1 is set explicitly rather than assumed: a PS/2 keyboard powers up in set 2, and a driver that assumed set 1 would work upon most machines and elsewhere deliver plausible characters that were simply the wrong ones. Scancodes are decoded into key events, retaining releases and the codes of keys that produce no character, since a later window system needs both. Capitals lock is a latch toggled upon depression alone, and combines with shift as an exclusive disjunction for letters while leaving every other key alone. The buffer's indices are free-running and masked, so that their difference is the occupancy directly; an overrun discards the newest event and counts it. Every wait upon the controller is bounded, so that a machine without one proceeds rather than hanging. The kernel now enters an echo loop in place of halting, which exercises the interrupt path end to end; keystrokes driven from the QEMU monitor were echoed correctly. `docs/devices/KEYBOARD.md` added and records the design. |
| 2026-08-31 | Phase 3 | Sub-task 3.6 completed. Counter 0 of the 8253 interval timer is programmed as a rate generator, mode 2 being preferred to mode 3 because the square wave mode decrements by two and so admits only even divisors, and nothing here has any interest in the shape of the waveform. A divisor of 1193 realises 1000.152 Hz against the 1000 Hz requested, and elapsed time is converted by the frequency realised rather than the one requested, an error of a known size that does not announce itself being worse than a coarse clock. The divisor is confirmed from within the machine by latching the counter and asserting that no reading exceeds it, there being no second clock to check the first against. The bounded wait exists so that a timer which never fires reports itself instead of hanging. `docs/devices/TIME.md` added and records the design. |
| 2026-08-31 | Phase 3 | Sub-task 3.5 completed. The cascaded 8259A pair is remapped to vectors 32 to 47, the vectors the firmware leaves them presenting having collided exactly with the architecture-defined exceptions, so that a timer tick was indistinguishable from a double fault. ICW1 clears the interrupt mask register, so every line is masked immediately after the sequence rather than before it. A routing layer owns the end-of-interrupt, signalling both controllers for a slave line, because the protocol belongs to the controller and the cost of a driver forgetting it is the permanent silencing of every lower-priority line. A spurious request upon IR7 or IR15 is recognised by the absence of its bit from the in-service register and left unacknowledged. The remapping is established by setting the interrupt flag with every line masked: had it failed, the running interval timer would have delivered a double fault at once. `docs/design/INTERRUPTS.md`, Section 9, records the design. |
| 2026-08-31 | Phase 2 | Sub-task 2.8 completed, and Phase 2 with it. An address space may be created, cloned, activated and destroyed. A clone duplicates the paging structures of the lower half and shares the frames they map, withdrawing write permission and setting the copy-on-write flag in both hierarchies and recording a reference for the new holder; the higher half is shared with the kernel by copying the root entries, so the kernel is mapped identically in every address space. Paging now distinguishes the active hierarchy from the kernel's, a walk performed in software being obliged to follow the one the processor follows. `docs/design/MEMORY-LAYOUT.md`, Section 14, records the design. |
| 2026-08-31 | — | This document reviewed and corrected. The status section had retained the claim that sub-tasks 2.7 and 2.8 could not proceed, which the completion of Phase 3 had discharged, and had accumulated as a chronological narrative duplicating this table; it is now a statement of present condition alone. Sub-task 1.11 is recorded as passed upon the project owner's own testing, and `docs/project/TESTING.md` amended to agree. The rows of this table were placed in order. |
| 2026-08-30 | Phase 2 | Sub-task 2.7 completed. Copy-on-write fault resolution, using bit 9 of the page-table entry, which Intel SDM Table 4-19 records as ignored by the processor. A shared frame is duplicated through the direct map and one reference released; a frame with a single referrer is made writable without a copy. The page-fault handler attempts resolution before reporting. |
| 2026-08-30 | Phase 3 | Sub-task 3.4 completed. Handlers for all thirty-two architecture-defined exceptions, with decoding of both error-code formats, a full register and control-register dump, and a bounded stack reproduction. `CR0.WP` is now set in `PagingInitialise`, without which the read-only kernel mappings were advisory only. The negative paging test deferred from sub-task 2.3 was performed and passed. |
| 2026-08-30 | Phase 3 | Sub-task 3.3 completed. A dispatch table of 256 entries with a registration interface. An unregistered architecture-defined exception remains fatal until sub-task 3.4; an unregistered vector above that range is counted and ignored, which is the correct treatment of a spurious interrupt. A default breakpoint handler is registered, the breakpoint being a trap and therefore safe to resume from. |
| 2026-08-30 | Phase 3 | Sub-task 3.2 completed. A stub for each of the 256 vectors, normalising the vector number and the presence of a processor error code into a uniform trap frame. A minimal kernel global descriptor table was established in the same work: the table loaded by `boot/boot.asm` lay at an address unmapped by sub-task 2.3, and interrupt delivery faulted upon reading it. `docs/design/INTERRUPTS.md` added. |
| 2026-08-30 | Phase 3 | Sub-task 3.1 completed. The interrupt descriptor table and the 64-bit gate descriptor are defined and the table loaded with `LIDT`. Every gate is initially absent. Verified by reading the table register back with `SIDT`. |
| 2026-08-30 | — | The project guidelines were consolidated under `PROJECT_GUIDELINES.md` and the text adapted to that name: the framing addressed to an assistant is now addressed to a contributor, and the document now describes itself as guidelines. The root directory recorded in Sections 1 and 9 was corrected to `~/oxys-os`. The documentation index was relocated from `docs/` to the repository root as `README.md`, becoming the front page of the repository, with every relative link corrected. Every reference to the guidelines document was updated across the source, the documentation and the agent definitions. |
| 2026-08-30 | Phase 2 | Sub-task 2.6 completed. Per-frame reference counting is established over a 255 KiB table allocated from the kernel heap. `FrameFree` releases one reference and returns a frame to the allocator only upon the last. Sub-tasks 2.7 and 2.8 are deferred until Phase 3 provides a page-fault handler. |
| 2026-08-30 | Phase 2 | Sub-task 2.5 completed. The kernel virtual address allocator issues ranges of the 32 TiB arena, backing each page with a frame; the kernel heap is a slab allocator over it, with eight size classes and a whole-page path for larger requests. Both pass a boot-time self-test. |
| 2026-08-30 | Phase 2 | Sub-task 2.4 completed. The direct physical map covers the whole of usable physical memory at `0xFFFF800000000000` with 2 MiB pages. Paging structures are no longer confined to the first gibibyte. |
| 2026-08-30 | Phase 2 | Sub-task 2.3 completed. The permanent kernel paging hierarchy is constructed from four allocated frames and activated; the kernel text and read-only data are mapped read-only; the low identity mapping is removed. Verified by a boot-time self-test that walks the hierarchy in software. |
| 2026-08-30 | Phase 2 | Sub-task 2.2 completed. A bitmap physical frame allocator, with a boot-time self-test. 131039 frames governed under QEMU with 512 MiB, of which 288 are reserved: 256 for the low mebibyte, 27 for the kernel image, 4 for the bitmap and 1 for the boot information structure. |
| 2026-08-30 | Phase 2 | Sub-task 2.1 completed. The Multiboot2 information structure is parsed into the neutral `BootInformation` description: the memory map, the ELF section count, the boot loader name and the command line. The kernel and boot information extents are recorded for the frame allocator to reserve. |
| 2026-08-30 | Phase 1 | `PROJECT_GUIDELINES.md` amended at the project owner's request by the addition of Section 10, requiring directory-level documentation. `boot/README.md`, `kernel/README.md` and `drivers/README.md` created accordingly, and `docs/README.md`, since relocated to the repository root, extended to index them. |
| 2026-08-30 | Phase 1 | Project initialised. Directory structure, documentation corpus, boot code, kernel entry, VGA and serial output, build system and ISO generation completed. Boot verified under QEMU. |
