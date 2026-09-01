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
| [`design/`](design/) | The kernel itself: its architecture, its boot, its address space, its interrupts. | You want to know how the machine is brought up and how it is arranged once it is. |
| [`devices/`](devices/) | The hardware the kernel drives, one document per device. | You are working upon a driver, or want to know what a device does and why the driver treats it so. |
| [`storage/`](storage/) | The path from a medium to a caller: the disk, the block layer above it, the cache above that. | You are working anywhere between a sector and a filesystem. |

## Contents

### [`project/`](project/)

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](project/PLAN.md) | The thirteen-phase roadmap and the task tracker. The single source of truth for progress, and the document every other one cites its phase from. |
| [`TESTING.md`](project/TESTING.md) | The verification procedure and the record of every test performed, with its date and its outcome. |
| [`TOOLCHAIN.md`](project/TOOLCHAIN.md) | The cross-compilation toolchain and the build system. |
| [`CODING-STANDARDS.md`](project/CODING-STANDARDS.md) | Style, naming, documentation, the diagnostic regime, and the register of compiler extensions relied upon. |
| [`REFERENCES.md`](project/REFERENCES.md) | Every specification the project relies upon, with the sections relied upon named. |

### [`design/`](design/)

| Document | Subject |
| -------- | ------- |
| [`ARCHITECTURE.md`](design/ARCHITECTURE.md) | The structure of the system, the source tree, and the dependency ordering that fixes the phases. |
| [`BOOT.md`](design/BOOT.md) | From the GRUB handover to `KernelMain`. |
| [`MEMORY-LAYOUT.md`](design/MEMORY-LAYOUT.md) | The physical and virtual address spaces, the paging hierarchy, and the allocators above it. |
| [`INTERRUPTS.md`](design/INTERRUPTS.md) | The descriptor table, the stubs, the dispatcher, the exceptions and the 8259A controllers. |

### [`devices/`](devices/)

| Document | Subject | Driver |
| -------- | ------- | ------ |
| [`TIME.md`](devices/TIME.md) | The interval timer and the system tick. | `drivers/pit/` |
| [`DISPLAY.md`](devices/DISPLAY.md) | The VGA text-mode display. | `drivers/vga/` |
| [`SERIAL.md`](devices/SERIAL.md) | The 16550 serial adapter. | `drivers/serial/` |
| [`KEYBOARD.md`](devices/KEYBOARD.md) | The 8042 controller and the PS/2 keyboard. | `drivers/keyboard/` |
| [`PCI.md`](devices/PCI.md) | The PCI bus enumeration. | `drivers/pci/` |

### [`storage/`](storage/)

| Document | Subject | Implementation |
| -------- | ------- | -------------- |
| [`DISK.md`](storage/DISK.md) | The ATA disk in programmed input/output mode. | `drivers/ata/` |
| [`BLOCK.md`](storage/BLOCK.md) | The generic block-device layer. | `drivers/block/block.c` |
| [`BUFFER.md`](storage/BUFFER.md) | The buffer cache. | `drivers/block/buffer.c` |

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
