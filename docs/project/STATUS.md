# Present Condition of the System

**Document status**: Living document, revised in every session in which a
functional change is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Where this sits**: [`PLAN.md`](PLAN.md) is the roadmap and the authority upon
what state each sub-task is in. This document says what the system *does* today,
one paragraph to a phase, and where each phase has been observed to work. It was
the opening section of `PLAN.md` until 2026-09-07 and was moved out so that the
roadmap could be scanned.

**The authority where two documents differ** is the design document cited in each
paragraph, which is revised as the design is. Nothing here restates an argument
made there; each paragraph says what stands, and points at the reasoning.

---

## 1. In one sentence

The kernel boots from a Multiboot2 ISO into long mode at a higher-half address,
manages physical and virtual memory with copy-on-write, services interrupts and
exceptions, drives a serial adapter, a display, a keyboard, a mouse, three kinds
of storage controller and the PCI bus, mounts and writes an EXT2 volume through a
virtual filesystem layer, draws upon a composited linear framebuffer, and loads
and runs a statically linked ELF64 program at privilege level 3 — which returns
to the kernel by system call and is ended when it faults.

What it does not yet do is pre-empt that program, run more than one of them, or
start a second processor.

## 2. By phase

**Phase 1 — bootstrapping.** The kernel builds without diagnostics under the full
warning regime, is confirmed Multiboot2 compliant by `grub-file`, and boots under
QEMU and VirtualBox alike, presenting its banner upon the console and COM1.
It has also booted from a USB medium upon real hardware, which closed sub-task
1.12 on 2026-09-07; the machine and the run are
[`TESTING.md`](TESTING.md), Sections 10.1 and 10.2. See
[`../design/BOOT.md`](../design/BOOT.md).

**Phase 2 — memory.** A bitmap allocator governs every physical frame; a permanent
hierarchy maps the kernel text and read-only data without write permission and
the whole of physical memory at `0xFFFF800000000000`; a kernel arena and a slab
heap serve allocations of arbitrary size; every frame carries a reference count
and returns to the allocator only upon its last release; and an address space may
be created, cloned by the copy-on-write discipline, activated and destroyed. The
substrate `fork()` will be built upon in sub-task 6.11 is therefore complete. See
[`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md).

**Phase 3 — interrupts.** The interrupt descriptor table is loaded; a stub for
each of the 256 vectors constructs a uniform trap frame whatever the vector; a
dispatch table routes each vector to a registered handler that may alter the
frame it returns through; every architecture-defined exception has a handler that
decodes its error code and a **disposition** deciding whether the fault is
resumed, costs the program that raised it, or is fatal to the machine; and the
cascaded 8259A pair is remapped clear of the exceptions, with each line masked
until a driver claims it. See
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md).

**Phase 4 — device drivers.** The serial adapter is interrupt-driven and keeps a
polled path it reverts to whenever the interrupt flag is clear, a panic reporting
with interrupts disabled and needing a channel that will drain. The display is a
formal driver reading its configuration rather than assuming it. PCI is
enumerated through bridges rather than swept. Three storage controllers are
driven — ATA in programmed input/output, AHCI by first-party direct memory
access, and an SD host controller for a machine whose system is upon an embedded
MultiMediaCard part — and where none of them can reach the storage a machine
carries, the report says which controller it found and why. A generic block layer
performs every judgement before a driver is reached, and a buffer cache of
sixty-four blocks stands above it, a buffer's identity being the device and the
block number together. See [`../devices/`](../devices/) and
[`../storage/`](../storage/).

**Phase 5 — EXT2.** A volume is read, written and mounted: the superblock, the
group descriptors, the inodes and every level of their block pointers, directory
traversal, path resolution, both forms of symbolic link, allocation from both
bitmaps, writing, truncation, and the creation and destruction of the names that
reach a file. Above it stands a virtual filesystem layer, and three of its
properties are the substance of the work, each being a decision the obvious
alternative gets silently wrong: **a mount is found through the node it covers
and never through a path prefix**; **a file reached twice is one node**, since two
descriptions of one file silently truncate it; and **a volume opened for writing
is marked unclean before anything else is written to it**, a kernel that marked it
upon unmounting recording only the mounts that ended well. The root volume of a
machine this kernel is booted upon is mounted read-only unless the operator chose
the GRUB entry that permits writing. See
[`../storage/EXT2.md`](../storage/EXT2.md) and
[`../storage/VFS.md`](../storage/VFS.md).

**Phase 6 — graphics, system calls, processes, SMP.** Complete as far as sub-task
6.11; 6.12 to 6.15 are the multiprocessing half, none of it begun.

- The apparatus a privilege transition is performed out of stands and has been
  exercised: user-mode descriptors in the order `SYSCALL` and `SYSRET` derive
  their selectors by, a task state segment naming the stack entered from user
  mode and a separate stack for the double fault, and an I/O map base beyond the
  segment limit, which is what denies every port to user mode.
- The kernel asks the boot loader for a linear framebuffer and gives its pages
  the write-combining memory type; draws upon surfaces rather than upon the
  framebuffer by name; renders a bitmap face of ninety-five glyphs drawn for this
  project; carries a console that replays what was written before the framebuffer
  could be mapped; draws a full-screen page for each severe fault, one per fault
  rather than one for all; and composites all of it over a back buffer, after
  which **nothing reads the framebuffer**.
- A `SYSCALL` entry path swaps `GS`, loads a kernel stack from a per-processor
  block, dispatches through a table of seven calls and validates a caller's
  arguments against both the canonical user limit and the paging hierarchy — and
  resolves a copy-on-write fault upon a page it is asked to write, rather than
  refusing an address a fork had protected.
- An ELF64 loader places a statically linked image into an address space, and its
  design is the list of things it refuses to be told.
- A process has an address space of its own and threads with kernel stacks of
  their own beneath guard pages; a switch exchanges six registers and a stack
  pointer; and `IRETQ` descends to privilege level 3.
- **A program may make another program.** `fork` clones its address space by the
  copy-on-write discipline of Phase 2 and gives the child its parent's whole
  register set with `RAX` zeroed; `execve` replaces a process's program with one
  read from a volume; `exit` ends a program upon its own request; and `wait`
  collects what a child ended with. A child runs when its parent waits for it,
  there being one thread of control until the scheduler of sub-task 6.15.

See [`../design/PRIVILEGE.md`](../design/PRIVILEGE.md),
[`../design/GRAPHICS.md`](../design/GRAPHICS.md),
[`../design/EXECUTABLE.md`](../design/EXECUTABLE.md) and
[`../design/PROCESS.md`](../design/PROCESS.md).

## 3. Where it has been observed to work

A sub-task marked *implemented* in [`PLAN.md`](PLAN.md) means the code exists and
builds. It does not mean it has been run everywhere. This table says where each
phase has actually been observed, and is the reason those are separate columns
there.

The physical machine is one machine — the HP Laptop 14-dq0052dx specified in
[`TESTING.md`](TESTING.md), Section 10.1.

| Phase | QEMU | VirtualBox | OVMF (UEFI) | Physical hardware |
| ----- | ---- | ---------- | ----------- | ----------------- |
| 1 Bootstrapping | Yes | Yes | No — no UEFI path until Phase 12 | **Yes**, on one machine — 1.12 |
| 2 Memory | Yes | Yes | — | Reached, not examined |
| 3 Interrupts | Yes | Yes | — | Reached, not examined |
| 4 Device drivers | Yes | Yes, less the serial adapter | — | **Yes, and it found two faults** — see below |
| 5 EXT2 | Yes | Yes | — | Reached, not examined |
| 6 Graphics and processes | Yes | Yes | — | Reached, not examined |

"Reached, not examined" means the kernel ran that far upon the machine — it must
have, the storage report of Phase 4 coming after all of it — but nothing about
those phases was inspected or recorded there. It is not evidence that they are
correct upon real hardware; it is only evidence that they did not stop it.

**Sub-task 1.12 is closed**, upon the criterion the project owner set on
2026-09-07: **one machine, booted from a USB medium, with the boot log read and
recorded**. It is met by the run described above. The criterion is written down
because it was a judgement rather than a deduction — a stricter one, several
machines or a serial capture, would have been equally defensible — and a
sub-task closed against an unrecorded standard cannot be reopened against one.

**No serial capture was possible, and that is part of the result.** The machine
has no 16550 for the kernel to find and no USB stack exists to drive an adapter,
so the log was read from the graphical console of sub-task 6.4 — the same
condition VirtualBox presents. [`TESTING.md`](TESTING.md), Section 10.2, records
what follows from it, including that the automated assertion of Section 1 cannot
be performed there.

Three qualifications, each of which cost something to learn:

**VirtualBox has no serial adapter this kernel detects**, so between sub-tasks
6.2 and 6.4 it had no readable diagnostic output at all — the framebuffer having
displaced the text console and the serial port being absent. Sub-task 6.4's
graphical console is what restored it.
[`TESTING.md`](TESTING.md), Section 9.1.

**The storage drivers are the one part real hardware has changed the design of**,
and the evidence was of failure rather than of success. One machine has run this
kernel — an
**HP Laptop 14-dq0052dx**, Intel Celeron N4120, 4 GB of memory, 64 GB of eMMC
storage and no disk of any other kind, booted from a USB drive — and it reported
no disk. The cause was neither a fault in the driver nor an absent disk: it was a
controller in AHCI mode, whose registers are memory-mapped and which answers at
no I/O port; and then, upon the same machine, no mass-storage controller of any
class whatever, its system being upon an eMMC part. Sub-tasks 4.7 and 4.8 were
added in consequence. The machine is specified in [`TESTING.md`](TESTING.md),
Section 10.1, and the diagnosis is in
[`../storage/DISK.md`](../storage/DISK.md), Sections 2.1 to 2.3.

**`make run-uefi` is expected to fail** and is provided in advance so that the
UEFI work of Phase 12 has an established point of entry. Sub-task 12.7 renders it
functional. [`TESTING.md`](TESTING.md), Section 8.

## 4. How anything here is asserted

There is no test harness and there will be none before Phase 7, there being no
userland to run one in. The kernel therefore asserts its own properties at boot,
in the order the subsystems are initialised, and `make verify` fails if any of
them reports a failure. Thirty-nine assertions presently report.

Those tests are in [`../../kernel/test/`](../../kernel/test/), one file per
subsystem. Each subsystem's design document carries a table pairing every
property its self-test asserts with the silent failure that assertion exists to
catch; that table, and not this document, is where the reasoning lives. See
[`../../kernel/test/README.md`](../../kernel/test/README.md) and
[`TESTING.md`](TESTING.md).

**A self-test is not the same as a probe.** Two routines examine whatever volume
the machine actually carries and assert nothing, there being nothing to assert
about a disk this kernel did not write. Their value is the one thing a composed
fixture cannot supply: a fixture shares this kernel's understanding of the
format, so a misreading of the specification would be composed into it and then
asserted against itself.

## 5. What is known to be absent

These are the limitations that cross subsystem boundaries. Each subsystem's own
design document ends with its particular ones.

| Absent | Arrives at |
| ------ | ---------- |
| Pre-emption. Nothing takes a processor away from a thread that has not given it up. | 6.15 |
| More than one program at a time. `ThreadStart` records the thread to return to in a single variable. | 6.15 |
| `fork`, `execve`, `exit` and `wait`. A program ends because it faulted, not because it asked. | 6.11 |
| File descriptors. A process has no open files; the filesystem layer's table is global. | Phase 7 |
| Any synchronisation whatever. Every shared structure is unguarded, and each says so in its own file's header. | 6.13 |
| A second processor. | 6.14 |
| `CR4.SMEP`, `CR4.SMAP` and `IA32_EFER.NXE`. The user mappings that would be protected now exist. | 13.3 |
| A UEFI boot path. | Phase 12 |
