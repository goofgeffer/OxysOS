<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Design Documentation

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 7. No change to this
project is complete until the documents affected by it have been brought up to
date in the same change; the documentation is part of the codebase and not a
description of it.

The documents here describe the system **by subject**. The `README.md` of each
source directory describes that directory's contents **locally**. The two are
complementary, and neither replaces the other.

## The four groups

| Directory | Holds | Read it when |
| --------- | ----- | ------------ |
| [`project/`](project/) | How the work is conducted: the plan, the tests, the toolchain, the standards, the bibliography. | You are about to make a change, or want to know what is done and what is not. |
| [`design/`](design/) | The kernel itself: its architecture, its boot, its address space, its interrupts, the apparatus of a privilege transition, the framebuffer and the drawing upon it, the executable and the process that runs one, the processors it all runs upon — and, from sub-task 7.1, the interface it presents to a program, the C library built upon that, and — from sub-task 8.1 — the shell and the terminal it reads. | You want to know how the machine is brought up, how it is arranged once it is, and what a program standing upon it is given. |
| [`devices/`](devices/) | The hardware the kernel drives, one document per device. | You are working upon a driver, or want to know what a device does and why the driver treats it so. |
| [`storage/`](storage/) | The path from a medium to a caller: the disk, the block layer above it, the cache above that, the filesystem above that — and, from sub-task 7.7, the initial ramdisk that puts the whole of it to work at every boot. | You are working anywhere between a sector and a file. |

## Contents

### [`project/`](project/)

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](project/PLAN.md) | The thirteen-phase roadmap and the task tracker: where the work stands and what comes next. The single source of truth for progress, and the document every other one cites its phase from. |
| [`STATUS.md`](project/STATUS.md) | The present condition of the system, one paragraph to a phase, and which environments each phase has been observed to work in. |
| [`HISTORY.md`](project/HISTORY.md) | The revision history: one row per change, pointing at the commit and the design document that hold the detail. |
| [`VERSIONING.md`](project/VERSIONING.md) | The release naming scheme: one repository, many releases — the ordinal, the point beneath it, the alpha and beta that precede the first ordinal and recur for no later one, the name that stands for an ordinal, and the edition that is a variety rather than a successor — the record of releases, which is empty, and the three that are planned. |
| [`TESTING.md`](project/TESTING.md) | The verification procedure and the environments it is carried out in. |
| [`TESTING-SYSTEM.md`](project/TESTING-SYSTEM.md) | What the test of each non-graphical subsystem establishes, and the negative test that confirmed it. |
| [`TESTING-GRAPHICS.md`](project/TESTING-GRAPHICS.md) | The same for the graphical work, where most of what matters must be looked at rather than asserted. |
| [`TESTING-RECORD.md`](project/TESTING-RECORD.md) | The dated record of every test performed, with its outcome. |
| [`BUILDS.md`](project/BUILDS.md) | The register of every image produced. Its record is [`builds.tsv`](project/builds.tsv) — twelve columns, three fixed vocabularies, append-only — and the document is a generated view of it that `make lint` keeps honest. |
| [`TOOLCHAIN.md`](project/TOOLCHAIN.md) | The cross-compilation toolchain, the build system, and the continuous integration workflow. |
| [`CODING-STANDARDS.md`](project/CODING-STANDARDS.md) | Style, naming, documentation, the diagnostic regime, and the register of compiler extensions relied upon. |
| [`REFERENCES.md`](project/REFERENCES.md) | Every specification the project relies upon, with the sections relied upon named. |
| [`INSPIRATIONS.md`](project/INSPIRATIONS.md) | The systems this project takes its character from, what is taken from each, what is not, and the appearance Phase 9 is to be designed against. |

### [`design/`](design/)

| Document | Subject |
| -------- | ------- |
| [`ARCHITECTURE.md`](design/ARCHITECTURE.md) | The structure of the system, the source tree, and the dependency ordering that fixes the phases. |
| [`BOOT.md`](design/BOOT.md) | From the GRUB handover to `KernelMain`. |
| [`MEMORY-LAYOUT.md`](design/MEMORY-LAYOUT.md) | The physical and virtual address spaces, the paging hierarchy, and the allocators above it. |
| [`INTERRUPTS.md`](design/INTERRUPTS.md) | The descriptor table, the stubs, the dispatcher, the exceptions, the 8259A controllers, and the layer through which a driver claims a request line whichever controller is answering. |
| [`PRIVILEGE.md`](design/PRIVILEGE.md) | The user-mode descriptors, the task state segment and its trusted stacks, the registers that configure `SYSCALL`, and the entry path, dispatch table and argument validation above them. |
| [`GRAPHICS.md`](design/GRAPHICS.md) | An index of the five documents below, which are the graphical work of sub-tasks 6.2 to 6.6, and the argument for its being five. |
| [`FRAMEBUFFER.md`](design/FRAMEBUFFER.md) | The framebuffer and its memory type: how it is asked for, what is validated, why write-combining, and what became of the text console. |
| [`DRAWING.md`](design/DRAWING.md) | The surface and the clipping that bounds every write to it, and the primitives drawn within that bound. |
| [`CONSOLE.md`](design/CONSOLE.md) | The bitmap font and the graphical console drawn with it, and the measurement that made the console fast enough to keep. |
| [`FAULTSCREEN.md`](design/FAULTSCREEN.md) | The page a severe fault draws when the machine stops — one for each fault, not one for all. |
| [`COMPOSITOR.md`](design/COMPOSITOR.md) | The pointer and the compositor beneath all of the above, after which nothing reads the framebuffer. |
| [`EXECUTABLE.md`](design/EXECUTABLE.md) | The ELF64 loader for statically linked executables: the list of things it refuses to be told by an untrusted document, and how a segment reaches an address space. |
| [`PROCESS.md`](design/PROCESS.md) | The process control block, the thread and the saved context; the switch that exchanges one thread for another, the descent to privilege level 3, and the four calls by which a program makes another program, becomes another program, ends, and collects what a child ended with. |
| [`CONCURRENCY.md`](design/CONCURRENCY.md) | The ticket spinlock, the per-processor area beneath it, the interrupt one processor sends to another, and the translation-lookaside-buffer shootdown built upon that — the mechanisms a second processor cannot safely exist without, built before there was one; [`SMP.md`](design/SMP.md) is what started them. |
| [`SMP.md`](design/SMP.md) | The bring-up of the application processors: the INIT-startup-startup protocol, the real-mode trampoline that carries a processor from a reset into 64-bit mode upon the kernel's own hierarchy, and the rule that a starting processor allocates nothing. |
| [`SCHEDULER.md`](design/SCHEDULER.md) | The per-processor run queues, the round-robin rotation, the affinity that decides which queue a thread may join, and the local timer whose rate the kernel measures rather than assumes. |
| [`LIBC.md`](design/LIBC.md) | The C library, and the division of the system-call header that had to precede it: the interface a program is entitled to, held apart from the kernel's implementation of it so that a permissively licensed library may include one without the other; the nineteen string and memory functions of ISO/IEC 9899:2011, Section 7.24, that this library implements and the three it does not; and why a userland library is presently compiled into the kernel image and asserted by a boot-time self-test. |
| [`SHELL.md`](design/SHELL.md) | The shell of Phase 8, one section per sub-task: the terminal the kernel assembles from the keyboard and the serial line so that a program can read what a person types, the line editor and its history above it, and the shell that prompts with them — sub-task 8.1 so far. |

### [`devices/`](devices/)

| Document | Subject | Driver |
| -------- | ------- | ------ |
| [`TIME.md`](devices/TIME.md) | The interval timer and the system tick. | `drivers/pit/` |
| [`DISPLAY.md`](devices/DISPLAY.md) | The VGA text-mode display, and how sub-task 6.2 displaced it. | `drivers/vga/` |
| [`SERIAL.md`](devices/SERIAL.md) | The 16550 serial adapter. | `drivers/serial/` |
| [`KEYBOARD.md`](devices/KEYBOARD.md) | The PS/2 keyboard, and the 8042 controller it is reached through. | `drivers/ps2/`, `drivers/keyboard/` |
| [`MOUSE.md`](devices/MOUSE.md) | The PS/2 mouse, and the pointer drawn from it. | `drivers/mouse/`, `graphics/cursor.c` |
| [`PCI.md`](devices/PCI.md) | The PCI bus enumeration. | `drivers/pci/` |
| [`ACPI.md`](devices/ACPI.md) | The firmware's description tables, and the one of them the kernel reads. | `kernel/acpi/` |
| [`APIC.md`](devices/APIC.md) | The Local APIC and the I/O APIC, which retire the 8259A pair. | `drivers/apic/` |

### [`storage/`](storage/)

| Document | Subject | Implementation |
| -------- | ------- | -------------- |
| [`DISK.md`](storage/DISK.md) | The ATA disk in programmed input/output mode, and what storage the driver cannot reach and how it says so. | `drivers/ata/` |
| [`AHCI.md`](storage/AHCI.md) | The AHCI disk by first-party direct memory access: the handoff from the firmware, the ports, and the command list. | `drivers/ahci/` |
| [`SDCARD.md`](storage/SDCARD.md) | The SD card and the embedded MultiMediaCard, and the host controller they are reached through. | `drivers/sdhci/` |
| [`INITRD.md`](storage/INITRD.md) | The initial ramdisk: the EXT2 image built beside the kernel, the Multiboot2 module that carries it, the device it becomes and the root it is mounted as. | `drivers/ramdisk/ramdisk.c` |
| [`BLOCK.md`](storage/BLOCK.md) | The generic block-device layer. | `kernel/block/block.c` |
| [`BUFFER.md`](storage/BUFFER.md) | The buffer cache. | `kernel/block/buffer.c` |
| [`EXT2.md`](storage/EXT2.md) | The EXT2 volume's structures: its superblock, its group descriptors and its inodes — and, in Section 10, every limitation of this kernel's EXT2 support. |
| [`EXT2-FILES.md`](storage/EXT2-FILES.md) | What is done with those structures: the directories, the resolution of a path, the reading and writing of a file, and the creation and destruction of names. |
| [`EXT2-VERIFICATION.md`](storage/EXT2-VERIFICATION.md) | The eleven self-tests of the EXT2 implementation, six of them against a volume this kernel did not compose. | `kernel/fs/ext2/` |
| [`VFS.md`](storage/VFS.md) | The virtual filesystem layer: the mount, the node, the open file, and the one tree that several volumes are joined into. | `kernel/fs/vfs/`, `kernel/fs/ext2_vfs.c` |

## The form of a document

Each document opens by stating the phase and sub-task of
[`project/PLAN.md`](project/PLAN.md) it belongs to, the authority under which it
is written, and the files that implement what it describes. Each ends with a
statement of its limitations, so that what has not been done is as legible as
what has.

Every assertion about hardware carries a citation to a specification registered
in [`project/REFERENCES.md`](project/REFERENCES.md). Where a specification is not
publicly distributed, the document says so and records that its details were
taken from two independent renderings and cross-verified.

Each document also contains a table pairing every property its subject's
boot-time self-test asserts with the silent failure that assertion would catch.
That is the project's substitute for a test harness, which cannot exist before
the userland of Phase 7.

## When a document becomes several

[`design/ARCHITECTURE.md`](design/ARCHITECTURE.md), Section 2.2, states the rule
for a translation unit that has stopped being readable as one thing. **The same
rule governs a document**, and three of them met it on 2026-09-09.

| Was | Is now | The evidence that it had become several |
| --- | ------ | --------------------------------------- |
| `design/GRAPHICS.md`, 1,663 lines | An index plus [`FRAMEBUFFER.md`](design/FRAMEBUFFER.md), [`DRAWING.md`](design/DRAWING.md), [`CONSOLE.md`](design/CONSOLE.md), [`FAULTSCREEN.md`](design/FAULTSCREEN.md), [`COMPOSITOR.md`](design/COMPOSITOR.md) | Its own opening block had become a table of contents, enumerating which sections belonged to which sub-task because there was no other way to find the third of five subjects. |
| `storage/EXT2.md`, 1,562 lines | [`EXT2.md`](storage/EXT2.md), [`EXT2-FILES.md`](storage/EXT2-FILES.md), [`EXT2-VERIFICATION.md`](storage/EXT2-VERIFICATION.md) | Its verification chapter alone was 463 lines and eleven subsections, none of which anybody reads while reading about the format. |
| `project/TESTING.md`, 1,757 lines | [`TESTING.md`](project/TESTING.md), [`TESTING-SYSTEM.md`](project/TESTING-SYSTEM.md), [`TESTING-GRAPHICS.md`](project/TESTING-GRAPHICS.md), [`TESTING-RECORD.md`](project/TESTING-RECORD.md) | Three quarters were per-subsystem chapters nobody reads in sequence, and one eighth was a dated table everybody scrolled past them to reach. |

**The line the division falls upon is the reader's question**, which is the
document's form of "the lines the faults fall upon". *How is the framebuffer
asked for?* and *why does the pointer leave no trail?* are not the same question
and were not in the same document by accident.

**A division is not a rewrite.** Nothing was reordered within a document,
reworded, or improved in passing. Every line of each original stands in exactly
one of its pieces, and each division was checked mechanically: the pieces
rejoined differ from the original in **heading lines alone**, save the
cross-references that the renumbering obliged, which were rewritten in the same
change and each of which was then confirmed to name a section that exists.

Three things the divisions made necessary, each recorded where it happened:

1. **`design/FAULTSCREEN.md` acquired a limitations section**, the original
   having none for the fault screens and this document requiring every document
   to end with one. Its items are gathered from statements already in the text.
2. **`storage/EXT2.md` keeps the limitations of all three EXT2 documents.** They
   are properties of the format and of this kernel's handling of it as a whole;
   several bear upon two documents at once and one refers to another by number.
3. **`project/TESTING.md`'s sections were regrouped by subject**, so its four
   pieces do not each hold a contiguous run of the original. Within each piece
   the sections stand in their original relative order.
