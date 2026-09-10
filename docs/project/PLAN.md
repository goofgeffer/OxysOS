# Oxys-OS Development Plan

**Document status**: Living document. This file is the single source of truth for
task tracking and shall be updated in every session in which a functional change
is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Target architecture**: x86_64.
**Boot protocol**: Multiboot2 (legacy BIOS, GRUB) initially; native UEFI added in Phase 12.
**Kernel model**: Monolithic.

## What is being built

A monolithic, Unix-like operating system for x86_64, written from scratch in ISO
C11 and NASM assembly, in thirteen phases ordered by dependency. Each phase is
divided into atomic sub-tasks, and every milestone must be bootable and testable.

**The long-term objective is that Oxys-OS should build Oxys-OS.** That lies
beyond the thirteen phases and is recorded here so that the work leading to it is
not quietly foreclosed; the section [Beyond the thirteen
phases](#beyond-the-thirteen-phases--self-hosting) sets out what it means, what
it depends upon, and the one decision it cannot be planned without.

## Where we are
**Phases 1 to 5 are complete.** Sub-task 1.12 closed on 2026-09-07: the kernel
has booted from a USB medium upon real hardware, and the boot log was read there.
**Phase 6 is complete**: a statically linked ELF64 program is loaded into an
address space of its own, entered at privilege level 3, returned to by `SYSRET`
when it makes a system call, and ended when it faults or when it asks — and it
may make a child of itself upon the copy-on-write substrate of Phase 2, replace
that child's program with one read from a volume, and collect what it ended with.
Since sub-task 6.12 the machine's own interrupt controllers are the Local APIC
and the I/O APIC, programmed from what the firmware's ACPI tables declare; the
8259A pair is masked and retired. Since sub-task 6.13 each processor holds a
per-processor area of its own, reached through `GS`; there is a ticket spinlock
that masks interrupts for as long as it is held; one processor can interrupt
another; and a paging-structure change is announced by a
translation-lookaside-buffer shootdown that waits to be acknowledged. Since
sub-task 6.14 every processor the firmware declares usable is started by an
INIT-startup-startup sequence into a real-mode trampoline and carried into
64-bit mode upon the kernel's own paging hierarchy. **Since sub-task 6.15 those
processors have work**: each holds a run queue of its own with a lock of its own,
rotates round-robin between the threads upon it, and is taken back by a local
timer calibrated against the interval timer when a ten-millisecond quantum
expires.

**Next: Phase 7** — the userland and the minimal C library.

**Two things Phase 6 leaves for it to inherit.** The scheduler's affinity mask
names the bootstrap processor alone for every user thread, because the
allocators, the process tables and the filesystem layer a system call reaches are
still unsynchronised; [`../design/SCHEDULER.md`](../design/SCHEDULER.md),
Section 4, records that this is a limitation written as a value in a field rather
than a scheduling decision, and
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10, limitation 1,
enumerates what is outstanding. And the bootstrap processor is not itself a
scheduled thread: it executes `KernelMain` as a flow of control, joining the
rotation only while something has adopted a thread for it.

For what the system does today, and where it has been observed to do it, see
[`STATUS.md`](STATUS.md). For how it came to be that way, see
[`HISTORY.md`](HISTORY.md).

## How to read this document

**A sub-task carries three independent facts, and none of them implies another.**
One checkbox used to carry all three, which meant it could not distinguish a
sub-task whose code exists from one that has been run, or either from one that
some assertion actually covers.

| Column | What it says | What it does **not** say |
| ------ | ------------ | ------------------------ |
| **State** | Whether the code exists. `Planned`, `In progress`, or `Implemented`. | Nothing about whether it works, or where it has been run. |
| **Asserted by** | Which boot-time self-test covers the sub-task, if any. `—` means no assertion of its own — a statement of fact, not a defect: some sub-tasks are not the kind of thing an assertion can reach. `(indirect)` means no assertion names the sub-task, but an assertion elsewhere would fail if it were wrong. | Nothing about whether that assertion is sufficient. |
| **Verified** | Recorded per phase in [`STATUS.md`](STATUS.md), Section 3, because the evidence is per environment — QEMU, VirtualBox, OVMF, physical hardware — and not per sub-task. | — |

So `Implemented` means the code exists and builds under the full diagnostic
regime. It does not mean tested, and it does not mean verified upon any
particular machine.

Sub-task 6.1 is the case that shows why the columns are separate. It is
`Implemented`, and it has been since the phase opened; but its self-test stopped
executing `SYSCALL` when sub-task 6.7 replaced the entry point, so what it
asserts today is narrower than what it asserted then. One marker could not have
shown that change at all. Sub-task 1.12 was the other such case until it was
closed: everything around it was implemented and nothing about it was, the
sub-task being the verification itself.

## The thirteen phases

| Phase | Subject | State |
| ----- | ------- | ----- |
| [1](#phase-1--bootstrapping-and-early-output) | Bootstrapping and early output | Implemented |
| [2](#phase-2--memory-management-including-copy-on-write) | Memory management, including copy-on-write | Implemented |
| [3](#phase-3--interrupts-exceptions-and-keyboard-input) | Interrupts, exceptions and keyboard input | Implemented |
| [4](#phase-4--basic-device-drivers) | Basic device drivers | Implemented |
| [5](#phase-5--ext2-filesystem) | EXT2 filesystem | Implemented |
| [6](#phase-6--graphics-system-calls-process-management-and-symmetric-multi-processing) | Graphics, system calls, processes, SMP | Implemented |
| [7](#phase-7--userland-and-minimal-c-library) | Userland and minimal C library | Planned |
| [8](#phase-8--shell) | Shell | Planned |
| [9](#phase-9--the-desktop-its-system-services-and-its-configuration) | The desktop, its services and its configuration | Planned |
| [10](#phase-10--cryptography) | Cryptography | Planned |
| [11](#phase-11--networking) | Networking | Planned |
| [12](#phase-12--uefi-transition) | UEFI transition | Planned |
| [13](#phase-13--polish-optimisation-and-final-hardening) | Polish, optimisation and final hardening | Planned |

The ordering is dictated by dependency, which
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4, sets out —
including the two places where the order was deliberately chosen against the
obvious one, and why.

---

## Phase 1 — Bootstrapping and Early Output

**Objective**: Produce a bootable ISO image containing a Multiboot2-compliant
ELF64 kernel that enters long mode, relocates to the higher half of the virtual
address space, and emits identifying output.

**Specifications**: Multiboot2 Specification 2.0; Intel SDM Volume 3A,
Chapters 2, 4 and 9; System V ABI for AMD64.
**Design**: [`../design/BOOT.md`](../design/BOOT.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 1.1 | Verify the cross-compilation toolchain (`x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`, `xorriso`) is present and functional. | Implemented | `make toolcheck` |
| 1.2 | Author the linker script `linker.ld` defining a higher-half kernel at virtual base `0xFFFFFFFF80000000` with a physical load address of `0x00100000`. | Implemented | — |
| 1.3 | Author `boot/boot.asm`: Multiboot2 header, 32-bit protected-mode entry point, Multiboot2 magic validation, CPUID and long-mode feature detection. | Implemented | `grub-file`, at each link |
| 1.4 | Construct the boot-time paging hierarchy (PML4, two PDPTs, one PD) providing a 1 GiB identity map and a coincident 1 GiB higher-half map using 2 MiB pages. | Implemented | — |
| 1.5 | Perform the long-mode transition (CR4.PAE, IA32_EFER.LME, CR0.PG) and load a 64-bit GDT. | Implemented | — |
| 1.6 | Transfer control to the higher-half 64-bit kernel entry point and establish the kernel stack. | Implemented | — |
| 1.7 | Implement a minimal VGA text-mode output routine and a minimal COM1 serial output routine for early diagnostics. | Implemented | `verify_devices.c` |
| 1.8 | Implement `KernelMain`, which clears the screen and prints the string "Oxys-OS". | Implemented | `make verify` banner |
| 1.9 | Author the `Makefile` with the targets `all`, `clean`, `iso`, `run-qemu`, `run-vbox` and `run-uefi`. | Implemented | — |
| 1.10 | Generate the ISO image and verify boot under QEMU. | Implemented | `make verify` |
| 1.11 | Verify boot under VirtualBox. | Implemented | [`TESTING.md`](TESTING.md) §4 |
| 1.12 | Verify boot on physical hardware from a USB medium. | Implemented | [`TESTING.md`](TESTING.md) §5.1–5.2 |

---

## Phase 2 — Memory Management (including Copy-on-Write)

**Objective**: Establish complete control of physical and virtual memory, and
provide the copy-on-write primitives upon which `fork()` will later depend.

**Specifications**: Intel SDM Volume 3A, Chapter 4 (Paging) and Section 6.15
(Page-Fault Exception); Multiboot2 Specification, Sections 3.6.7 (ELF-Symbols
tag) and 3.6.8 (memory map tag).
**Design**: [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md).

Sub-tasks 2.7 and 2.8 were deferred until Phase 3 supplied a page-fault handler;
that is the one mutual dependency in the whole ordering, and
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4, records how
it was discharged.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 2.1 | Parse the Multiboot2 information structure and extract the memory map (tag type 6) and the ELF section headers (tag type 9). | Implemented | `verify_memory.c` (indirect) |
| 2.2 | Implement a physical frame allocator (bitmap) covering all usable regions, reserving the kernel image, the Multiboot2 structures and the low 1 MiB. | Implemented | `verify_memory.c` |
| 2.3 | Construct a permanent kernel page-table hierarchy, replacing the boot-time tables and removing the low identity map. | Implemented | `verify_memory.c` |
| 2.4 | Implement a direct physical map region for kernel access to arbitrary frames. | Implemented | `verify_memory.c` |
| 2.5 | Implement a kernel virtual-address-space allocator and a general-purpose kernel heap (slab allocator over a buddy-style page allocator). | Implemented | `verify_memory.c` |
| 2.6 | Implement per-frame reference counting as the substrate for shared pages. | Implemented | `verify_memory.c` |
| 2.7 | Implement the page-fault handler dispatch path (dependent upon Phase 3) and the copy-on-write fault resolution routine. | Implemented | `verify_memory.c` |
| 2.8 | Implement address-space cloning that marks writable user pages read-only and increments frame reference counts. | Implemented | `verify_memory.c` |

---

## Phase 3 — Interrupts, Exceptions and Keyboard Input

**Objective**: Install a complete interrupt descriptor table, service processor
exceptions, and accept keyboard input.

**Specifications**: Intel SDM Volume 3A, Chapter 6; Intel 8259A datasheet;
IBM PS/2 controller documentation.
**Design**: [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md),
[`../devices/TIME.md`](../devices/TIME.md),
[`../devices/KEYBOARD.md`](../devices/KEYBOARD.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 3.1 | Define the IDT and the 64-bit interrupt-gate descriptor format; load it with `lidt`. | Implemented | `verify_interrupts.c` |
| 3.2 | Author assembly stubs for vectors 0–255, normalising the presence or absence of a processor-pushed error code. | Implemented | `verify_interrupts.c` |
| 3.3 | Implement a C interrupt dispatcher operating on a formal trap frame structure. | Implemented | `verify_interrupts.c` |
| 3.4 | Implement exception handlers with register and stack diagnostics emitted over the serial port. | Implemented | `verify_interrupts.c`, `verify_faultscreen.c` |
| 3.5 | Remap the 8259A PIC to vectors 32–47 and implement end-of-interrupt signalling. | Implemented | `verify_devices.c` |
| 3.6 | Implement the Programmable Interval Timer as the initial timer source. | Implemented | `verify_devices.c` |
| 3.7 | Implement the PS/2 keyboard driver: controller initialisation, scancode set 1 translation, modifier state and a circular input buffer. | Implemented | `verify_devices.c` |

---

## Phase 4 — Basic Device Drivers

**Objective**: Provide the device support required by the filesystem and by
subsequent user-facing subsystems.

**Specifications**: PC16550D UART datasheet; ATA/ATAPI Command Set (ACS-3);
PCI Local Bus Specification 3.0; Serial ATA Advanced Host Controller Interface
1.3.1; SD Host Controller Simplified Specification 4.20.
**Design**: [`../devices/`](../devices/) and [`../storage/`](../storage/).

Sub-tasks 4.7 and 4.8 were added after the phase was otherwise complete, both at
the project owner's report from a real machine that this kernel found no disk
upon. The diagnosis and the reasoning are in
[`../storage/DISK.md`](../storage/DISK.md), Sections 2.1 to 2.3; the short of it
is that "no disk" had three indistinguishable causes and only one of them was
the absence of a disk.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 4.1 | Promote the early serial routine to a formal, interrupt-driven COM1 driver with configurable line parameters. | Implemented | `verify_devices.c` |
| 4.2 | Promote the early VGA routine to a formal text-mode driver with scrolling, cursor control and colour attributes. | Implemented | `verify_devices.c` |
| 4.3 | Implement PCI configuration-space enumeration by the legacy I/O port mechanism, with device and class identification. | Implemented | `verify_devices.c` |
| 4.4 | Implement an ATA PIO driver: bus reset, `IDENTIFY DEVICE`, 28-bit and 48-bit LBA sector read and write. | Implemented | `verify_storage.c` |
| 4.5 | Define a generic block-device abstraction layer above the ATA driver. | Implemented | `verify_storage.c` |
| 4.6 | Implement a buffer cache for block devices. | Implemented | `verify_storage.c` |
| 4.7 | Implement an AHCI driver, so that a machine whose firmware presents its SATA controller in AHCI mode has a disk at all. *(Added 2026-09-04.)* | Implemented | `verify_storage.c` |
| 4.8 | Implement an SD host controller driver, so that a machine whose system is upon an embedded MultiMediaCard part has storage at all. *(Added 2026-09-06.)* | Implemented | `verify_storage.c` |

---

## Phase 5 — EXT2 Filesystem

**Objective**: Mount, read and write an EXT2 volume.

**Specifications**: The Second Extended File System (Poirier); Linux kernel
documentation, `Documentation/filesystems/ext2.rst`.
**Design**: [`../storage/EXT2.md`](../storage/EXT2.md) and the two it heads,
[`../storage/VFS.md`](../storage/VFS.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 5.1 | Parse the superblock and validate the EXT2 magic number and revision level. | Implemented | `ext2/format.c` |
| 5.2 | Parse the block-group descriptor table. | Implemented | `ext2/format.c` |
| 5.3 | Implement inode retrieval and the resolution of direct, singly, doubly and triply indirect block pointers. | Implemented | `ext2/format.c`, `ext2/file.c` |
| 5.4 | Implement directory-entry traversal and absolute path resolution. | Implemented | `ext2/directory.c` |
| 5.5 | Implement file reading. | Implemented | `ext2/file.c` |
| 5.6 | Implement block and inode allocation, file writing, extension and truncation. | Implemented | `ext2/write.c` |
| 5.7 | Implement directory creation and entry insertion and removal. | Implemented | `ext2/write.c` |
| 5.8 | Define a virtual filesystem layer and mount an EXT2 root volume. | Implemented | `verify_vfs.c` |

---

## Phase 6 — Graphics, System Calls, Process Management and Symmetric Multi-Processing

**Objective**: Present a graphical display, execute user-mode programs, schedule
them across multiple processors, and expose kernel services by system call.

**Specifications**: Intel SDM Volume 3A, Chapters 5, 8 and 10, and Volume 2B
(`SYSCALL`/`SYSRET`); System V ABI for AMD64; Intel 82093AA I/O APIC datasheet;
Intel MultiProcessor Specification 1.4; ACPI Specification 6.5 (the RSDP, the
table directory and the MADT); Multiboot2 Specification, Sections 3.6.12
(framebuffer information tag), 3.6.16 and 3.6.17 (the ACPI pointer tags); VESA
BIOS Extensions 3.0.
**Design**: [`../design/PRIVILEGE.md`](../design/PRIVILEGE.md),
[`../design/GRAPHICS.md`](../design/GRAPHICS.md) and the five it indexes,
[`../design/EXECUTABLE.md`](../design/EXECUTABLE.md),
[`../design/PROCESS.md`](../design/PROCESS.md),
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md),
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md), Section 10,
[`../devices/MOUSE.md`](../devices/MOUSE.md),
[`../devices/ACPI.md`](../devices/ACPI.md),
[`../devices/APIC.md`](../devices/APIC.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 6.1 | Install the GDT and TSS required for privilege transition; configure IA32_STAR, IA32_LSTAR and IA32_FMASK. | Implemented | `verify_privilege.c` — **partial**, see note (a) |
| 6.2 | Request a linear framebuffer by the Multiboot2 framebuffer tag and map it into kernel space. | Implemented | `verify_framebuffer.c` |
| 6.3 | Implement 2D primitives: pixel, line, rectangle, blit and clipping. | Implemented | `verify_graphics.c` |
| 6.4 | Implement a bitmap font renderer, and a graphical console above it that the diagnostic path may write to. | Implemented | `verify_console.c`, `verify_faultscreen.c` |
| 6.5 | Implement a PS/2 mouse driver upon the second device port of the 8042, and a cursor. | Implemented | `verify_mouse.c` |
| 6.6 | Implement a compositing surface abstraction and double buffering. | Implemented | `verify_compositor.c` |
| 6.7 | Implement the `SYSCALL` entry path, the system-call dispatch table and argument validation. | Implemented | `verify_syscall.c` |
| 6.8 | Implement the ELF64 loader for statically linked executables. | Implemented | `verify_elf.c` |
| 6.9 | Define the process control block, the address-space descriptor and the thread structure. | Implemented | `verify_process.c` |
| 6.10 | Implement context switching and the initial transition to user mode via `IRETQ`. | Implemented | `verify_usermode.c` |
| 6.11 | Implement `fork()` upon the Phase 2 copy-on-write substrate, together with `execve()`, `exit()` and `wait()`. | Implemented | `verify_lifecycle.c` |
| 6.12 | Parse the ACPI MADT; initialise the Local APIC and the I/O APIC; retire the 8259A PIC. | Implemented | `verify_apic.c`, `verify_devices.c` — see note (b) |
| 6.13 | Implement spinlocks, per-CPU data areas and inter-processor interrupts, including TLB shootdown. | Implemented | `verify_smp.c` — see note (c) |
| 6.14 | Implement application-processor bring-up by INIT-SIPI-SIPI and a real-mode trampoline. | Implemented | `verify_smp.c` — see note (d) |
| 6.15 | Implement a multiprocessor-aware round-robin scheduler with per-CPU run queues and processor affinity. | Implemented | `verify_sched.c` — see note (e) |

**(a)** Sub-task 6.1's self-test executed `SYSCALL` until sub-task 6.7 replaced
the entry point with one returning by `SYSRET`, which returns to privilege level
3 unconditionally. That assertion is recorded as lost rather than disguised, and
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), Section 9.4, says why no test
hook was added to recover it. The configuration is asserted still; the
instruction is now executed only by a user program.

**(b)** Sub-task 6.12 is asserted by four routines in `verify_apic.c` — the ACPI
parse, the Local APIC, the I/O APIC and the routing after the adoption — and by
`KernelVerifyIrq` in `verify_devices.c`, which asserts the routing layer while
the 8259A pair still answers. The division is deliberate: the same path is
asserted under each controller, so a failure says which of them broke it.

**(c)** Sub-task 6.13 is asserted by four routines in `verify_smp.c`. The first
two — the per-processor area and the spinlock — assert internal state and not
behaviour, because upon a machine with one processor a lock that does not lock
behaves exactly like one that does. The last two are behavioural: an interrupt a
processor sends to itself is delivered like any other, so the whole shootdown
path is exercised, against a mapping the test makes stale on purpose. **Only one
of the locks has been applied**; see note (d).

**(d)** Sub-task 6.14 is asserted by `KernelVerifyApplicationProcessors`, the
fifth routine in `verify_smp.c`. **A count is not the assertion**: a kernel that
incremented a variable and started nobody would produce the same count, the same
report and the same banner. What only a running processor can produce is an
acknowledgement to an interrupt it was sent, so the substance of the test is a
shootdown broadcast with each target's own service count read out afterwards —
the mechanism of 6.13 doing, at last, the thing it was built for. It also checks
what each started processor read out of its own task register, descriptor table
registers and control registers, `CR0.WP` among them, whose absence upon one
processor nothing else in this kernel would ever report; and upon a machine with
one processor it asserts the other side, that nobody was started and that the
kernel says which condition declined it.

**The one lock 6.14 applies is the diagnostic channel**, in `KernelWriteString`,
because that is the whole of what a started processor touches — a started
processor has nothing to run and is parked in a halt loop until 6.15. Every other
structure in
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10, limitation 1,
is still unsynchronised; see note (e) for what 6.15 did and did not change.

**(e)** Sub-task 6.15 is asserted by `KernelVerifyScheduler` in
`verify_sched.c`. **A count of admissions is not evidence that anything ran**: a
scheduler that enqueued four threads and gave none of them a processor produces
the same admissions, the same queue lengths and the same report. So the fixture
is four kernel threads that do work and record it, and the assertions are made
against what they recorded — that every thread completed its rounds, upon a
processor it names itself, having been given the processor at least once; that
the slices across the fixture exceed the number of threads, which is the rotation
visible from outside; and that a quantum expired, which is the one thing a
voluntary yield cannot demonstrate.

Two of those assertions were got wrong first and the corrections are recorded in
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7. **The locks are
still not applied beyond two of them** — the run queues, which this sub-task
creates, and the process and thread tables, which it makes contended. A user
thread's affinity mask names the bootstrap processor alone for exactly that
reason, and Section 4 of that document says so; widening it is the work of the
sub-task that locks the allocators and the filesystem layer.

**Why 6.2 to 6.6 sit here rather than in Phase 9**, and **why 6.13 precedes
6.14**: both orderings were chosen against the obvious one, and both arguments
are in [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4.1.

---

## Phase 7 — Userland and Minimal C Library

**Objective**: Provide the runtime environment in which user programs execute.

**Specifications**: ISO/IEC 9899:2011; System V ABI for AMD64.

Phase 7 is also where the filesystem layer's open file table becomes per-process
and `fork` must decide what a child inherits; a process has no file descriptors
before it. See [`../storage/VFS.md`](../storage/VFS.md), limitation 2.

**Sub-task 7.2 carries an obligation from the licensing.** The userland is `MIT`
and the kernel `LGPL-3.0-or-later`, so a C library cannot include a header that
mixes the user-visible interface with the kernel's implementation of it — and
`kernel/include/oxys/syscall.h` presently does. It must be divided before the
wrappers are written. See [`../../LICENSING.md`](../../LICENSING.md), Section 2.1.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 7.1 | Implement the freestanding string and memory functions (`<string.h>`). | Planned | — |
| 7.2 | Implement system-call wrappers for the complete kernel interface. | Planned | — |
| 7.3 | Implement a user-space heap allocator (`malloc`, `free`, `realloc`) above `brk`/`mmap`. | Planned | — |
| 7.4 | Implement buffered input and output (`<stdio.h>`) and formatted conversion. | Planned | — |
| 7.5 | Author the C runtime startup object (`crt0`) and the static-linking procedure for user programs. | Planned | — |
| 7.6 | Implement the utilities `ls`, `cat`, `echo`, `mkdir` and `rm`. | Planned | — |
| 7.7 | Construct an initial ramdisk containing the utilities and mount it as the early root. | Planned | — |

---

## Phase 8 — Shell

**Objective**: Provide an interactive command interpreter.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 8.1 | Implement line editing with history. | Planned | — |
| 8.2 | Implement the tokeniser and the command parser. | Planned | — |
| 8.3 | Implement built-in commands (`cd`, `exit`, `export`, `pwd`). | Planned | — |
| 8.4 | Implement external program execution by `fork()` and `execve()`. | Planned | — |
| 8.5 | Implement input and output redirection. | Planned | — |
| 8.6 | Implement pipelines. | Planned | — |
| 8.7 | Implement job control, process groups and terminal signal delivery. | Planned | — |

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
What remains is everything that cannot, which is why this phase follows the shell
and not the framebuffer. The division of the graphical work between Phase 6 and
this phase is argued in [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md),
Section 4.1, which records what it cost.

**The appearance of what 9.5 to 9.8 present** is not decided here, but the
preference it is to be designed against is written down:
[`INSPIRATIONS.md`](INSPIRATIONS.md), Section 3 — a contemporary playful
minimalism, modernist and playfully geometric. A retro-styled desktop is
expressly not wanted, that section stating why the prohibition is written down
rather than left implied.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 9.1 | Implement a stacking window manager with focus and event routing. | Planned | — |
| 9.2 | Implement the client protocol by which user processes create, draw and receive events upon windows. *(The surface interface of 6.6 is revisited here against its first real client.)* | Planned | — |
| 9.3 | Implement `init`: the first user process, the supervision of the services below it, and the orderly shutdown of both. | Planned | — |
| 9.4 | Define the system configuration format, its parser, and the `/etc` hierarchy the services and the desktop read at start. | Planned | — |
| 9.5 | Implement the session: the desktop root, the panel, the launcher, and the ownership of the display that decides who may draw upon it. | Planned | — |
| 9.6 | Implement a terminal emulator window hosting the Phase 8 shell. | Planned | — |
| 9.7 | Implement the utilities the desktop is not usable without: a file manager, a text viewer and a clock. | Planned | — |
| 9.8 | Implement the settings application, by which the configuration of 9.4 is edited rather than hand-written. | Planned | — |

---

## Phase 10 — Cryptography

**Objective**: Provide primitives for random-number generation, hashing and
symmetric encryption.

**Specifications**: FIPS 180-4 (SHA-256); FIPS 197 (AES); NIST SP 800-38A
(modes of operation); NIST SP 800-90A (deterministic random bit generators);
Intel SDM Volume 2B (`RDRAND`, `RDSEED`).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 10.1 | Implement an entropy pool seeded from `RDSEED`/`RDRAND` where available and from timer jitter otherwise. | Planned | — |
| 10.2 | Implement a cryptographically secure deterministic random bit generator. | Planned | — |
| 10.3 | Implement SHA-256 with the FIPS 180-4 test vectors. | Planned | — |
| 10.4 | Implement AES-128 and AES-256 with the FIPS 197 test vectors. | Planned | — |
| 10.5 | Implement CBC and CTR modes of operation. | Planned | — |
| 10.6 | Expose the primitives to user space by system call and by a `/dev/random` device node. | Planned | — |

---

## Phase 11 — Networking

**Objective**: Provide a functional TCP/IP stack.

**Specifications**: IEEE 802.3; RFC 826 (ARP); RFC 791 (IP); RFC 792 (ICMP);
RFC 768 (UDP); RFC 9293 (TCP); RFC 2131 (DHCP); Realtek RTL8139 or Intel 8254x
datasheet.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 11.1 | Implement an Ethernet controller driver (RTL8139 or Intel E1000) with descriptor rings and interrupt handling. | Planned | — |
| 11.2 | Define the network buffer structure and the protocol layering framework. | Planned | — |
| 11.3 | Implement Ethernet frame transmission and reception. | Planned | — |
| 11.4 | Implement ARP with a resolution cache. | Planned | — |
| 11.5 | Implement IPv4, including fragmentation and reassembly, and a routing table. | Planned | — |
| 11.6 | Implement ICMP echo request and reply. | Planned | — |
| 11.7 | Implement UDP. | Planned | — |
| 11.8 | Implement TCP: the state machine, sequence-number handling, retransmission and flow control. | Planned | — |
| 11.9 | Implement the BSD-style socket system-call interface. | Planned | — |
| 11.10 | Implement DHCP client configuration and the `ping` utility. | Planned | — |

---

## Phase 12 — UEFI Transition

**Objective**: Boot the identical kernel by native UEFI as well as by legacy
BIOS.

**Specifications**: UEFI Specification 2.10; Microsoft PE/COFF Specification;
ACPI Specification 6.5.

`make run-uefi` exists already and is expected to fail; it is provided in advance
so that this phase has an established point of entry. Sub-task 12.7 renders it
functional. See [`TESTING.md`](TESTING.md), Section 3.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 12.1 | Establish a PE32+ build path for a UEFI application image. | Planned | — |
| 12.2 | Implement the UEFI entry point and parse the System Table. | Planned | — |
| 12.3 | Retrieve the memory map, the ACPI RSDP and the Graphics Output Protocol framebuffer by Boot Services. | Planned | — |
| 12.4 | Define a boot-protocol-neutral handoff structure consumed by the kernel, populated identically from Multiboot2 or from UEFI. | Planned | — |
| 12.5 | Invoke `ExitBootServices` and transfer control to the kernel. | Planned | — |
| 12.6 | Integrate UEFI Runtime Services: time and variable access. | Planned | — |
| 12.7 | Produce a hybrid ISO image bootable by both BIOS and UEFI, and verify under OVMF. | Planned | — |

---

## Phase 13 — Polish, Optimisation and Final Hardening

**Objective**: Prepare the system for release.

**Specifications**: Intel SDM Volume 3A, Chapters 4 and 5 (SMEP, SMAP, NX).

Sub-task 13.3 is depended upon by name from two earlier documents:
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), limitation 8, and
[`../design/EXECUTABLE.md`](../design/EXECUTABLE.md), limitation 4. The user
mappings those protections exist for came into being at sub-task 6.10.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 13.1 | Profile interrupt latency, context-switch cost and filesystem throughput. | Planned | — |
| 13.2 | Optimise the scheduler for fairness and the block layer for read-ahead. | Planned | — |
| 13.3 | Enable NX, SMEP and SMAP; enforce write-exclusive-or-execute in kernel mappings. | Planned | — |
| 13.4 | Implement kernel stack guard pages and stack-canary protection. | Planned | — |
| 13.5 | Implement kernel address-space layout randomisation, if feasible. | Planned | — |
| 13.6 | Complete and review the whole of the `docs/` corpus. | Planned | — |
| 13.7 | Test on a minimum of three distinct physical machines, including UEFI systems. | Planned | — |
| 13.8 | Extend the userland utility set and produce the final release image. | Planned | — |

---

## Beyond the thirteen phases — self-hosting

**The objective**: a machine running Oxys-OS can check out this repository, build
it, and produce a bootable image of Oxys-OS — with no other operating system
involved at any step. The project is self-hosting when the cross-compiler of
`PROJECT_GUIDELINES.md`, Section 3, is no longer the only way to make it.

This is a statement of direction and not a fourteenth phase. It has no sub-tasks,
no ordering and no assertions, because it cannot honestly have them until the
decision in Section B below is taken. It is written down so that the work leading
towards it is not foreclosed by accident — a system call omitted, a filesystem
left read-only, a toolchain assumption baked into the build — which is the way
this goal is usually lost.

### A. What it actually requires

Self-hosting is not one piece of work. It is the point at which several
independent lines of work happen to meet, and most of them are already on the
roadmap for their own reasons.

| Requirement | Where it stands |
| ----------- | --------------- |
| A filesystem that can be written to, with directories and a mountable root | **Phase 5.** Present. |
| Processes that can fork, execute a program from a volume, and be collected | **Phase 6**, sub-task 6.11. Present. |
| A scheduler, so that a build runs while other work does | **Phase 6**, sub-task 6.15. Present, though a user thread is confined to the bootstrap processor until the locks of [`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10, limitation 1, are applied. A parallel build is what will first want that. |
| A C library a compiler can be built against | **Phase 7**, and this is the requirement Phase 7's present scope does *not* meet. A minimal libc is enough for `ls` and `cat`; it is nowhere near enough for a compiler. |
| A shell, a job-control model, and pipes | **Phase 8.** Planned. A build system is a program that runs programs. |
| An assembler and a linker | **Not on the roadmap.** Nothing yet plans for either. |
| A C compiler that runs upon Oxys-OS | **Not on the roadmap**, and Section B is why. |
| A text editor, and enough of a utility set to work in | **Phase 13**, sub-task 13.8, in outline only. |
| Storage, memory and time enough to compile a kernel on the machine itself | An open question. It bears on Phase 13's optimisation work and on what hardware the final image targets. |

Three of those rows have nothing behind them, and the honest summary is that
self-hosting is presently **further away than the thirteen phases are long**.
Recording it is worth doing anyway: several of the rows above are cheaper to get
right the first time than to retrofit, and the ones that are not yet planned are
easier to plan for if it is known they are coming.

### B. The decision this cannot be planned without

**Does Oxys-OS write its own C compiler, or port one?**

The two answers lead to entirely different projects, and neither is obviously
right.

*Writing one* is what `PROJECT_GUIDELINES.md`, Section 2, points at: the project
is built from scratch, no external code is copied, and a compiler written here
would be continuous with everything else in the repository. It is also, plainly,
a larger undertaking than the thirteen phases combined if the target is a C11
compiler good enough to compile this kernel — and a compiler that cannot compile
this kernel does not achieve the objective at all.

*Porting one* — a small existing C compiler, or GCC or LLVM — reaches the
objective far sooner and brings a large body of third-party code into the
project. Section 8 of the guidelines forbids third-party code **inside the kernel
proper**, and a userland toolchain is not the kernel; but the project's identity
in Section 1 is "built entirely from scratch", and a ported compiler sits
uncomfortably against that whether or not the letter of the rule permits it.

**This is the project owner's decision and has not been taken.** It is recorded
as an open question rather than resolved by whoever writes the next document,
because the answer determines whether Phase 7's C library is sized for utilities
or for a compiler — and that is a decision made long before anybody starts
writing either.

Whichever way it goes, `PROJECT_GUIDELINES.md`, Section 5, will need amending to
say so, and Section 7 of that document requires the reason to be recorded in the
commit that makes the amendment.

### C. What to avoid foreclosing in the meantime

These cost nothing now and are expensive to retrofit:

1. **The build must not assume the host is Linux.** `Makefile` and the scripts
   beside it should keep to what a POSIX shell provides, so that the day they are
   run under an Oxys-OS shell is a day of finding bugs rather than of rewriting.
2. **System calls should be added in the shapes a real program expects**, and
   not in whatever shape the self-test that first needed them found convenient.
   A compiler wants files, directories, pipes and processes; each is far cheaper
   to get right at the moment it is introduced.
3. **The filesystem must stay writable and must stay correct under a load it has
   never seen.** A build writes many files, of many sizes, quickly.
   [`../storage/BUFFER.md`](../storage/BUFFER.md) records that the buffer cache
   is the first structure that will want a lock it may sleep upon; a build is the
   workload that will demonstrate it.
4. **The `docs/` corpus should keep saying which parts are self-hosting
   obstacles**, in the file that owns each, rather than accumulating the list
   here. A limitation named in one place is one somebody can find.

---

## Where the rest of the detail lives

| Question | Document |
| -------- | -------- |
| What does the system do today, and where has it been observed to work? | [`STATUS.md`](STATUS.md) |
| How did it come to be that way? | [`HISTORY.md`](HISTORY.md) |
| Why is a subsystem built the way it is? | [`../design/`](../design/), [`../devices/`](../devices/), [`../storage/`](../storage/) |
| Why are the phases in this order? | [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4 |
| How is any of it tested, and what was the result? | [`TESTING.md`](TESTING.md) |
| What are the conventions the work is bound by? | [`../../PROJECT_GUIDELINES.md`](../../PROJECT_GUIDELINES.md) |
