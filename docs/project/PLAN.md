# Oxys-OS Development Plan

**Document status**: Living document. This file is the single source of truth for
task tracking and shall be updated in every session in which a functional change
is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Target architecture**: x86_64.
**Boot protocol**: Multiboot2 (legacy BIOS, GRUB) initially; native UEFI added in Phase 12.
**Kernel model**: Monolithic.

## Current status

This section states the present condition of the work, one paragraph to a phase.
The chronological account is the Revision History at the foot of this document;
the reasoning behind any decision named here is in the design document cited,
which is revised as the design is and is the authority where the two differ.

**Phase 1 is complete but for sub-task 1.12.** The kernel builds without
diagnostics under the full regime, is confirmed Multiboot2 compliant by
`grub-file`, and boots under QEMU and VirtualBox alike, presenting its banner
upon the VGA console and COM1. Sub-task 1.12, boot from a physical USB medium,
remains open. See [`../design/BOOT.md`](../design/BOOT.md).

**Phase 2 is complete.** A bitmap allocator governs every physical frame; a
permanent hierarchy maps the kernel text and read-only data without write
permission and the whole of physical memory at `0xFFFF800000000000`; a kernel
arena and a slab heap serve allocations of arbitrary size; every frame carries a
reference count and returns to the allocator only upon its last release; and an
address space may be created, cloned by the copy-on-write discipline, activated
and destroyed. The substrate `fork()` is built upon in sub-task 6.11 is therefore
complete. See [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md).

**Phase 3 is complete.** The interrupt descriptor table is loaded; a stub for
each of the 256 vectors constructs a uniform trap frame whatever the vector; a
dispatch table routes each vector to a registered handler that may alter the
frame it returns through; every architecture-defined exception has a handler that
decodes its error code; and the cascaded 8259A pair is remapped clear of the
exceptions, with each line masked until a driver claims it. See
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md).

**Phase 4 is complete.** The serial adapter is interrupt-driven and keeps a
polled path it reverts to whenever the interrupt flag is clear, a panic reporting
with interrupts disabled and needing a channel that will drain. The display is a
formal driver reading its configuration rather than assuming it. PCI is
enumerated through bridges rather than swept. An ATA driver transfers sectors by
both addressing forms. A generic block layer performs every judgement before a
driver is reached, and a buffer cache of sixty-four blocks stands above it, a
buffer's identity being the device and the block number together. See
[`../devices/`](../devices/) and [`../storage/`](../storage/).

**Phase 5 is complete.** An EXT2 volume is read, written and mounted: the
superblock, the group descriptors, the inodes and every level of their block
pointers, directory traversal, path resolution, both forms of symbolic link,
allocation from both bitmaps, writing, truncation, and the creation and
destruction of the names that reach a file. Above it stands a virtual filesystem
layer, and three of its properties are the substance of the work, each being a
decision the obvious alternative gets silently wrong: **a mount is found through
the node it covers and never through a path prefix**; **a file reached twice is
one node**, since two descriptions of one file silently truncate it; and **a
volume opened for writing is marked unclean before anything else is written to
it**, a kernel that marked it upon unmounting recording only the mounts that
ended well. The root volume of a machine this kernel is booted upon is mounted
read-only unless the operator chose the GRUB entry that permits writing. See
[`../storage/EXT2.md`](../storage/EXT2.md) and
[`../storage/VFS.md`](../storage/VFS.md).

**Phase 6 has begun; sub-task 6.1 is complete.** The apparatus a privilege
transition is performed out of now stands, and has been exercised rather than
merely built. The global descriptor table holds the user-mode descriptors, whose
order is fixed by the arithmetic `SYSCALL` and `SYSRET` derive their selectors
by and not by any preference of this kernel's. A task state segment names the
stack loaded upon entry from user mode and, in its first interrupt stack table
entry, a separate stack the double fault is delivered upon; its I/O map base lies
beyond the segment limit, which is what denies every port to user mode.
`IA32_STAR`, `IA32_LSTAR` and `IA32_FMASK` are written before `IA32_EFER.SCE` is
set. Both the interrupt stack table and the transition itself are exercised —
`SYSCALL` being executable from privilege level 0, where it raises no privilege
but performs every other part of the transition — so the mechanism is proved with
no user program in existence.

**Sub-task 6.2 is complete.** The image asks the boot loader for a linear
framebuffer, and what it is given is validated, mapped and described. Two
decisions are the substance of it. The pages are **write-combining and not
write-back**, through entry 4 of `IA32_PAT` — an entry chosen because every
existing mapping in the kernel selects one of the first four, so taking it
changes the memory type of nothing already in use. Write-back would have been
the default and is the one type that is wrong here: a cached write may sit in a
line while the adapter displays what memory held before it, so the image is
wrong and then, for no reason connected to anything, right. And **the mode is
the boot loader's to choose**: GRUB 2.12 ignores `gfxpayload` for a multiboot2
image, which was established rather than assumed, so the kernel accepts whatever
it is handed and asserts what it was. The cost is that the adapter is now in a
graphics mode with no console upon it: the screen shows the self-test's colour
bands until sub-task 6.4, and the serial port carries the boot log as it always
did.

**Sub-task 6.3 is complete.** The primitives draw upon a `GraphicsSurface` and
not upon the framebuffer, which is one surface among them: blit has no meaning
with a single surface, the double buffering of sub-task 6.6 is the substitution
of one for another, and — the reason that mattered most in practice — a surface
composed in ordinary memory can be read back pixel by pixel, so the whole of the
self-test holds upon a machine with no display. **Clipping is treated as the
memory-safety boundary it is** rather than as a convenience: it is implemented
once, a clip is always confined to its surface so no argument can widen it, and
each shape is clipped once before a loop that then tests nothing. The line is
Bresenham's and is clipped **per pixel rather than at its endpoints**, which is
slower and is the only way to keep the promise that a clipped line lights exactly
the pixels the unclipped line would: the algorithm accumulates its error from the
start, so moving the start moves the line by a pixel here and there, and that
shows as a kink where two clipped regions meet along a seam. The next work is
sub-task 6.4, the font and the console that ends the blank screen. See
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md) and
[`../design/GRAPHICS.md`](../design/GRAPHICS.md).

**The testing arrangement**, which is not a phase and governs every one of them:
there is no test harness and there will be none before Phase 7, so the kernel
asserts its own properties at boot, in the order the subsystems are initialised.
Those tests are in `kernel/test/`, one file per subsystem, and `make verify`
fails if any of them reports a failure. See
[`../../kernel/test/README.md`](../../kernel/test/README.md) and
[`TESTING.md`](TESTING.md).

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
- [x] 5.8 Define a virtual filesystem layer and mount an EXT2 root volume.

---

## Phase 6 — Graphics, System Calls, Process Management and Symmetric Multi-Processing

**Objective**: Present a graphical display, execute user-mode programs, schedule
them across multiple processors, and expose kernel services by system call.

**Specifications**: Intel SDM Volume 3A, Chapters 5, 8 and 11, and Volume 2B
(`SYSCALL`/`SYSRET`); System V ABI for AMD64; Intel MultiProcessor
Specification 1.4; ACPI Specification 6.5 (MADT); Multiboot2 Specification,
Section 3.6.12 (framebuffer information tag); VESA BIOS Extensions 3.0.

- [x] 6.1 Install the GDT and TSS required for privilege transition; configure IA32_STAR, IA32_LSTAR and IA32_FMASK. *(The kernel GDT and its null, code and data descriptors were established early, in Phase 3; what remained was the user-mode descriptors, the task state segment and the system-call MSRs. Designed in `docs/design/PRIVILEGE.md`.)*
- [x] 6.2 Request a linear framebuffer by the Multiboot2 framebuffer tag and map it into kernel space. *(Designed in `docs/design/GRAPHICS.md`. The mode is the boot loader's to choose and GRUB ignores what it is asked for, so the kernel accepts whatever it is handed; the pages are given the write-combining memory type through entry 4 of `IA32_PAT`.)*
- [x] 6.3 Implement 2D primitives: pixel, line, rectangle, blit and clipping. *(Upon a surface rather than upon the framebuffer, so that they may be asserted in memory upon a machine with no display. Clipping is treated as the memory-safety boundary it is; the line is clipped per pixel so that clipping does not move it. Designed in `docs/design/GRAPHICS.md`, Sections 11 to 17.)*
- [ ] 6.4 Implement a bitmap font renderer, and a graphical console above it that the diagnostic path may write to.
- [ ] 6.5 Implement a PS/2 mouse driver upon the second device port of the 8042, and a cursor.
- [ ] 6.6 Implement a compositing surface abstraction and double buffering.
- [ ] 6.7 Implement the `SYSCALL` entry path, the system-call dispatch table and argument validation.
- [ ] 6.8 Implement the ELF64 loader for statically linked executables.
- [ ] 6.9 Define the process control block, the address-space descriptor and the thread structure.
- [ ] 6.10 Implement context switching and the initial transition to user mode via `IRETQ`.
- [ ] 6.11 Implement `fork()` upon the Phase 2 copy-on-write substrate, together with `execve()`, `exit()` and `wait()`.
- [ ] 6.12 Parse the ACPI MADT; initialise the Local APIC and the I/O APIC; retire the 8259A PIC.
- [ ] 6.13 Implement spinlocks, per-CPU data areas and inter-processor interrupts, including TLB shootdown. *(Ordered before the bring-up that needs them; see the note below.)*
- [ ] 6.14 Implement application-processor bring-up by INIT-SIPI-SIPI and a real-mode trampoline.
- [ ] 6.15 Implement a multiprocessor-aware round-robin scheduler with per-CPU run queues and processor affinity.

**Why 6.2 to 6.6 are here.** The framebuffer and the drawing above it were
sub-tasks 9.1 to 9.5 until 2026-09-03, and `PROJECT_GUIDELINES.md`, Section 5,
placed the whole of the graphical work after the shell. They are moved here at
the project owner's decision, and the split is along a real line rather than an
arbitrary one: **nothing in 6.2 to 6.6 depends upon a process existing.** A
framebuffer is memory the boot loader describes and this kernel maps; primitives,
a font and a compositing surface are arithmetic upon that memory; and a mouse is
another device upon the 8042 controller, whose second port the keyboard driver of
sub-task 3.7 already leaves alone. Every one of them is written, exercised and
asserted with the machinery Phases 2 to 5 already provide.

What genuinely does need processes is the half that stays in Phase 9: a window
manager has nothing to manage, and a client protocol has no client, until there
is something to run. That division is why this is a split and not a wholesale
reordering.

Two things are gained and one is given up. The diagnostic path acquires a console
that is not eighty by twenty-five characters of text, and every phase from here
to the end reports through it; and the choice between the VESA path and the UEFI
Graphics Output Protocol is forced now, while Phase 12 can still be shaped around
it, rather than in Phase 9 when it can no longer be. What is given up is that the
surface abstraction of sub-task 6.6 is designed before any user-mode client
exists to design it against, so its interface is a judgement rather than a
response. That is recorded here so that the judgement is revisited in sub-task
9.2 and not merely inherited.

**Why 6.13 precedes 6.14.** These two stood in the opposite order until
2026-09-03, and the order was wrong. Sub-task 6.14 starts processors; sub-task
6.13 supplies the locks without which nothing they touch is safe. Every shared
structure this kernel has — the frame allocator's bitmap and search hint, the
heap, the buffer cache, the mount and node tables of the filesystem layer, the
interrupt dispatch table — is presently unsynchronised, and each says so in its
own file's header. Bringing a second processor up before the locks exist would
produce a milestone that the testing mandate requires to be bootable and
testable, and that cannot be either: it would either park the new processors
immediately, in which case nothing is demonstrated, or let them run, in which
case the machine is corrupt in a way no assertion here would catch.

The reordering costs nothing, because everything in 6.8 can be exercised upon
one processor. A spinlock's uncontended acquire and release, and the per-CPU
data area reached through `GS`, are single-processor mechanisms outright. An
inter-processor interrupt sent to one's own Local APIC is delivered like any
other, so the shootdown handler may be made to run and the invalidation it
performs observed — the same device as sub-task 6.1's execution of `SYSCALL`
from privilege level 0, where the mechanism is exercised in full although the
condition it exists for has not yet arrived.

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

## Phase 9 — The Desktop, its System Services and its Configuration

**Objective**: Build a usable desktop upon the graphics of Phase 6: the window
system that arranges it, the long-lived processes that maintain it, the
configuration those processes read, and the applications a person operates it
with.

**Specifications**: IEEE Std 1003.1-2017 (process lifetime, signals and the
descriptors a service inherits); System V ABI for AMD64; UEFI Specification 2.10,
Section 12.9 (Graphics Output Protocol), where Phase 12 supplies the framebuffer
in place of Phase 6.

Phase 6 supplied the framebuffer, the primitives, the font, the pointer and the
compositing surface — everything that can be built without a process to own it.
What remains is everything that cannot, which is why this phase follows the
shell and not the framebuffer.

- [ ] 9.1 Implement a stacking window manager with focus and event routing.
- [ ] 9.2 Implement the client protocol by which user processes create, draw and receive events upon windows. *(The surface interface of sub-task 6.6 is revisited here against its first real client, and revised if it does not survive one.)*
- [ ] 9.3 Implement `init`: the first user process, the supervision of the services below it, and the orderly shutdown of both.
- [ ] 9.4 Define the system configuration format, its parser, and the `/etc` hierarchy the services and the desktop read at start.
- [ ] 9.5 Implement the session: the desktop root, the panel, the launcher, and the ownership of the display that decides who may draw upon it.
- [ ] 9.6 Implement a terminal emulator window hosting the Phase 8 shell.
- [ ] 9.7 Implement the utilities the desktop is not usable without: a file manager, a text viewer and a clock.
- [ ] 9.8 Implement the settings application, by which the configuration of sub-task 9.4 is edited rather than hand-written.

**What this phase is now.** Until 2026-09-03 this was the whole of the graphical
work, from the framebuffer upward. Its first five sub-tasks were moved to Phase 6
at the project owner's decision, upon the division recorded there: what needs no
process was brought forward, and what does was left here. The phase is therefore
no longer "graphics" — it is the desktop as a thing a person uses, and the
system processes and configuration without which a window system is a
demonstration rather than an environment.

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

This table is an **index**, not an account. One row per change, in reverse order
of commit, saying what changed and pointing at the two places that hold the
detail: the commit, which records the change as it was made, and the design
document, which records the reasoning and is revised as the design is.

Rows are kept to about sixty words. They were not always: by sub-task 5.8 a
single row had reached 1,564 words in one table cell, restating a design document
that already existed and a commit message that already said the same thing, and
this document had become 17,000 words of which the roadmap was 2,000. Three
copies of an argument do not agree with each other for long.

| Date | Phase | Change | Commit | Design |
| ---- | ----- | ------ | ------ | ------ |
| 2026-09-03 | Phase 6 | Sub-task 6.3: the 2D primitives — pixel, line, rectangle, blit and clipping — upon a surface rather than upon the framebuffer, so that they are asserted in memory against a surface whose pitch exceeds its width and whose padding holds a sentinel. Clipping is the memory-safety boundary and is implemented once; the line is clipped per pixel so that clipping cannot displace it. | *(this change)* | [`../design/GRAPHICS.md`](../design/GRAPHICS.md), Sections 11 to 17 |
| 2026-09-03 | Phase 6 | Sub-task 6.2: the linear framebuffer. The image carries the Multiboot2 request tag, optional bit set because the kernel can boot without one; what is supplied is validated, mapped by a new `KernelDeviceMap` that allocates no frame, and given the write-combining memory type through entry 4 of `IA32_PAT`, an entry no existing mapping selects. GRUB ignores `gfxpayload`, so the mode is accepted rather than chosen, and the text console is displaced until sub-task 6.4. | `cdf74b0` | [`../design/GRAPHICS.md`](../design/GRAPHICS.md) |
| 2026-09-03 | Phases 6, 9 | Phase 9 split at the project owner's decision. Its first five sub-tasks — the framebuffer, the primitives, the font, the mouse and the compositing surface — become sub-tasks 6.2 to 6.6, none of them needing a process to exist; the window manager, the client protocol and the desktop services they support remain in Phase 9, which is now the desktop rather than graphics. Old 6.2 to 6.10 renumbered to 6.7 to 6.15, and every reference across 35 files with them. `PROJECT_GUIDELINES.md` §5 amended accordingly. | *(this change)* | [`PLAN.md`](PLAN.md), Phases 6 and 9 |
| 2026-09-03 | Phase 6 | Sub-tasks 6.8 and 6.9 exchanged — their numbers that day; they are 6.13 and 6.14 since the renumbering above — so that spinlocks and per-CPU data precede the application-processor bring-up rather than following it. In the old order a milestone started processors against a kernel whose every shared structure was unsynchronised, and could be neither demonstrated nor asserted. Every reference to either number, in code and documentation alike, updated with it. | `c3befd8` | [`PLAN.md`](PLAN.md), Phase 6 |
| 2026-09-03 | All | The boot-time self-tests moved out of `kernel.c` into `kernel/test/`, one file per subsystem. `kernel.c` fell from 9,050 lines to 708. `make verify` now fails when a self-test reports a failure, which it previously could not see. This revision history rewritten as an index rather than a third account of each change. | `8e778e2` | [`../../kernel/test/README.md`](../../kernel/test/README.md), [`TESTING.md`](TESTING.md) §1 |
| 2026-09-03 | Phase 6 | Sub-task 6.1: the apparatus of a privilege transition. User-mode descriptors ordered by the arithmetic `SYSCALL` and `SYSRET` derive their selectors by; a task state segment with a stack for the double fault; `IA32_STAR`, `IA32_LSTAR` and `IA32_FMASK`. The interrupt stack table and the transition are exercised, not merely inspected. | `1903603` | [`../design/PRIVILEGE.md`](../design/PRIVILEGE.md) |
| 2026-09-02 | Phase 5 | Sub-task 5.8: the virtual filesystem layer, and an EXT2 root volume mounted through it. A mount is found through the node it covers and never through a path prefix; a file reached twice is one node; a volume opened for writing is marked unclean before anything else is written to it. | `f83f498` | [`../storage/VFS.md`](../storage/VFS.md) |
| 2026-09-02 | Phase 5 | **A defect in sub-task 5.7 found by `e2fsck`**, not by any assertion this kernel makes. `i_dtime` is overloaded — a deletion time, or the link to the next inode on the orphan list, distinguished by magnitude — so the constant 1 recorded for want of a clock made every freed inode appear orphaned. Now `UINT32_MAX`. | `f83f498` | [`../storage/VFS.md`](../storage/VFS.md) §11.1 |
| 2026-09-02 | Phase 5 | Sub-task 5.7: names, and the creation of files. Directory entries inserted and removed, link counts maintained on both sides, and an inode freed only with its last name. | `ebc5987` | [`../storage/EXT2.md`](../storage/EXT2.md) |
| 2026-09-02 | Phase 5 | Sub-task 5.6: allocation, file writing and truncation. Both bitmaps, the group summaries kept in step with them, and a write that leaves a hole where it skipped. | `c550424` | [`../storage/EXT2.md`](../storage/EXT2.md) |
| 2026-09-02 | Phase 5 | Sub-task 5.5: file reading and symbolic links. Both forms of link — the target held within the inode and the target held in a block — distinguished as the volume itself records the distinction. | `4e453f1` | [`../storage/EXT2.md`](../storage/EXT2.md) |
| 2026-09-02 | Phase 2 | `KernelPagesFree` now establishes that the whole range released lies within the arena, not merely its first page. A base at the last page with a count of 2^33 satisfied every check and corrupted the free list. | `08fcaeb` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) §11.7 |
| 2026-09-02 | Phase 2 | Three integer-wrap defects corrected in the sub-task 2.5 allocators, found by review rather than by failure. Each computed a bound from a product that could wrap, so the guard could not trust the value it tested. Counts and sizes are now bounded before any arithmetic is performed upon them. | `00d5472` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) §§10.4, 11.4 |
| 2026-09-02 | Phase 5 | Sub-task 5.4: directory traversal and path resolution. A directory is an ordinary file, so the work is entirely the validation of what its bytes claim. | `c9421d4` | [`../storage/EXT2.md`](../storage/EXT2.md) |
| 2026-09-02 | Phase 5 | Sub-task 5.3: inode retrieval and block-pointer resolution, through the direct, indirect, doubly and triply indirect pointers. | `50fcf28` | [`../storage/EXT2.md`](../storage/EXT2.md) |
| 2026-09-02 | Phase 5 | Sub-task 5.2: the block group descriptor table, read and validated. | `665a66d` | [`../storage/EXT2.md`](../storage/EXT2.md) |
| 2026-09-01 | Phase 1 | The `LOAD` segments of the image separated by permission — an accepted condition since Phase 1, discharged ahead of sub-task 13.3. The linker had inferred the segments and given each the union of what it held, so read-only data was mapped writable. | `d52828d` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| 2026-09-01 | All | `INSPIRATIONS.md` added at the project owner's request, recording ToaruOS as the principal inspiration and stating expressly what is not taken from it. | `ad6da48` | [`INSPIRATIONS.md`](INSPIRATIONS.md) |
| 2026-09-01 | Phase 5 | Sub-task 5.1: the EXT2 superblock, read through the buffer cache and decoded field by field from a buffer of bytes rather than by overlaying a structure. | `a000311` | [`../storage/EXT2.md`](../storage/EXT2.md) §3 |
| 2026-09-01 | All | `docs/` reorganised into four groups at the project owner's request: `project/`, `design/`, `devices/` and `storage/`. The grouping is by subject and not by phase. | `50e72e6` | [`../README.md`](../README.md) |
| 2026-09-01 | Phase 4 | Sub-task 4.6: the buffer cache, completing Phase 4. A buffer's identity is the device and the block number together. | `6a9c176` | [`../storage/BUFFER.md`](../storage/BUFFER.md) |
| 2026-09-01 | Phase 4 | Sub-task 4.5: the generic block-device layer. Its purpose is the judgement it performs before a driver is reached. | `eea928c` | [`../storage/BLOCK.md`](../storage/BLOCK.md) |
| 2026-09-01 | Phase 4 | Sub-task 4.4: the ATA driver in programmed input/output mode, in both the 28-bit and 48-bit addressing forms. | `792c9e5` | [`../storage/DISK.md`](../storage/DISK.md) |
| 2026-09-01 | Phase 4 | Sub-task 4.3: PCI enumeration by access mechanism one. Buses are reached through bridges rather than swept, and each is recorded as visited so malformed hardware cannot induce a cycle. | `90c32aa` | [`../devices/PCI.md`](../devices/PCI.md) |
| 2026-09-01 | Phase 4 | The backspace across a row boundary corrected: it consumed the separator and the character before it, so one keystroke deleted two things. | `bfbbf09` | [`../devices/DISPLAY.md`](../devices/DISPLAY.md) |
| 2026-09-01 | Phase 4 | Sub-task 4.2: the formal text-mode display driver. The register configuration is read from the Miscellaneous Output Register rather than assumed. | `9f168cd` | [`../devices/DISPLAY.md`](../devices/DISPLAY.md) |
| 2026-08-31 | Phase 4 | Sub-task 4.1: the interrupt-driven serial driver, claiming IR4. It keeps a polled path as well, and needs both: a panic reports with interrupts disabled and must not leave its message in a buffer nothing will drain. | `f0722ce` | [`../devices/SERIAL.md`](../devices/SERIAL.md) §4 |
| 2026-08-31 | Phase 3 | The backspace key repaired. `VgaPutCharacter` had no case for it, so it was written into the frame buffer as whatever glyph stands at code point 0x08. | `96dc484` | [`../devices/DISPLAY.md`](../devices/DISPLAY.md) |
| 2026-08-31 | Phase 3 | Sub-task 3.7: the PS/2 keyboard, completing Phase 3. The controller's configuration byte is written a second time after its self-test, that test resetting the controller upon some implementations. | `32ee3b4` | [`../devices/KEYBOARD.md`](../devices/KEYBOARD.md) |
| 2026-08-31 | Phase 3 | Sub-task 3.6: the interval timer as a rate generator. Mode 2 rather than mode 3, the square-wave mode decrementing by two and so admitting only even divisors. | `28264d7` | [`../devices/TIME.md`](../devices/TIME.md) |
| 2026-08-31 | Phase 3 | Sub-task 3.5: the 8259A pair remapped to vectors 32–47. The vectors the firmware leaves them presenting collide exactly with the architecture-defined exceptions, so a timer tick was indistinguishable from a double fault. | `421ac38` | [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md) §9 |
| 2026-08-31 | Phase 2 | Sub-task 2.8: address-space cloning, completing Phase 2. A clone shares the frames of the lower half, withdrawing write permission in both hierarchies and recording a reference for the new holder. | `8b05d27` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| 2026-08-31 | — | This document reviewed and corrected: the status section had accumulated as a chronological narrative duplicating this table, and is now a statement of present condition alone. | `e235f6f` | — |
| 2026-08-30 | Phase 3 | Sub-task 3.4: the exception handlers and their diagnostics. `CR0.WP` set in `PagingInitialise`, without which the read-only kernel mappings were advisory only. | `80ba170` | [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md) §8 |
| 2026-08-30 | Phase 3 | Sub-task 3.3: the interrupt dispatcher, a table of 256 entries with a registration interface. | `b56ec8b` | [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md) §7 |
| 2026-08-30 | Phase 3 | Sub-task 3.2: a stub for each of the 256 vectors, normalising the vector number and the presence of an error code into a uniform trap frame. A kernel global descriptor table was established in the same work, the boot table lying at an address sub-task 2.3 had unmapped. | `35eaaa6` | [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md) §§3–5 |
| 2026-08-30 | Phase 3 | Sub-task 3.1: the interrupt descriptor table and the 64-bit gate descriptor, loaded with `LIDT` and read back with `SIDT`. | `6e8bccd` | [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md) §2 |
| 2026-08-30 | Phase 2 | Sub-task 2.7: copy-on-write fault resolution, using bit 9 of the page-table entry, which Intel SDM Table 4-19 records as ignored by the processor. | `c574fb2` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| 2026-08-30 | — | The project guidelines consolidated under `PROJECT_GUIDELINES.md` and the documentation index relocated to the repository root. | `48c678e` | [`../../PROJECT_GUIDELINES.md`](../../PROJECT_GUIDELINES.md) |
| 2026-08-30 | Phase 2 | Sub-task 2.6: per-frame reference counting over a 255 KiB table. Sub-tasks 2.7 and 2.8 deferred until Phase 3 provided a page-fault handler. | `ed48893` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| 2026-08-30 | Phase 2 | Sub-task 2.5: the kernel virtual address allocator over the 32 TiB arena, and a slab heap above it. | `cfe5bad` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) §§10–11 |
| 2026-08-30 | Phase 2 | Sub-task 2.4: the direct physical map at `0xFFFF800000000000` with 2 MiB pages. Paging structures are no longer confined to the first gibibyte. | `aff2abc` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) §9 |
| 2026-08-30 | Phase 2 | Sub-task 2.3: the permanent kernel paging hierarchy constructed and activated, the text and read-only data mapped read-only, and the low identity mapping removed. | `d4c98ba` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| 2026-08-30 | Phase 2 | Sub-task 2.2: the bitmap physical frame allocator. 131,039 frames governed under QEMU with 512 MiB, of which 288 are reserved. | `b098c95` | [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| 2026-08-30 | Phase 2 | Sub-task 2.1: the Multiboot2 information structure parsed into the boot-protocol-neutral `BootInformation` description. | `7c379e5` | [`../design/BOOT.md`](../design/BOOT.md) |
| 2026-08-30 | Phase 1 | `PROJECT_GUIDELINES.md` amended at the project owner's request by the addition of Section 10, requiring directory-level documentation. | `d97fa4d` | [`../../PROJECT_GUIDELINES.md`](../../PROJECT_GUIDELINES.md) §10 |
| 2026-08-30 | Phase 1 | Project initialised. Directory structure, documentation corpus, boot code, kernel entry, VGA and serial output, build system and ISO generation. Boot verified under QEMU. | `64c4c42` | [`../design/BOOT.md`](../design/BOOT.md) |
