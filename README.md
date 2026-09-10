# Oxys-OS

[![CI](https://github.com/goofgeffer/OxysOS/actions/workflows/ci.yml/badge.svg)](https://github.com/goofgeffer/OxysOS/actions/workflows/ci.yml)
[![Language](https://img.shields.io/badge/C-C11-blue?logo=c)](docs/project/CODING-STANDARDS.md)
[![Assembly](https://img.shields.io/badge/asm-NASM-6E4C13)](boot/)
[![Architecture](https://img.shields.io/badge/arch-x86__64-lightgrey)](docs/design/ARCHITECTURE.md)
[![Boot](https://img.shields.io/badge/boot-Multiboot2-lightgrey)](docs/design/BOOT.md)
[![Kernel licence](https://img.shields.io/badge/kernel-LGPL--3.0--or--later-green)](LICENSES/)
[![Userland licence](https://img.shields.io/badge/userland-MIT-green)](LICENSES/)
[![Docs licence](https://img.shields.io/badge/docs-CC0--1.0-green)](LICENSES/)

Oxys-OS is a monolithic, Unix-like operating system for x86_64, written from
scratch in ISO C11 and NASM assembly. It targets real hardware and is tested
under QEMU and VirtualBox.

The conventions binding upon all work in this repository are set out in
[`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md). In accordance with its
Section 2, no code change is final until the documents affected by it have been
updated. [`CONTRIBUTING.md`](CONTRIBUTING.md) is the working procedure that
follows from them.

## Building

Upon a Debian or Ubuntu machine with nothing installed:

```sh
./build_all.sh
```

That installs the host packages, builds the `x86_64-elf` cross-toolchain from
source, builds the ISO and runs the verification under QEMU. The first run takes
twenty to forty minutes, nearly all of it the compiler; there is no package for
it, which is why it is built rather than installed.

The three stages are separately invocable, and the `make` targets are the usual
way to work once the toolchain stands:

| Command | What it does |
| ------- | ------------ |
| [`./build_deps.sh`](build_deps.sh) | Installs the host packages the build and the verification require. |
| [`./build_toolchain.sh`](build_toolchain.sh) | Builds binutils and GCC for `x86_64-elf` into `~/opt/cross`. Exits at once if they are there already. |
| [`./build_all.sh`](build_all.sh) | The three stages above in order, then `make iso` and `make verify`. |
| `make verify` | Boots the ISO under QEMU with no display and asserts upon the serial output. **This is the gate**: `PROJECT_GUIDELINES.md`, Section 2, requires it to pass before a change is final. |
| `make clang-check` | Compiles every translation unit with a second compiler, for its diagnostics alone. Builds nothing. |

The toolchain is not placed upon the default `PATH`.
[`docs/project/TOOLCHAIN.md`](docs/project/TOOLCHAIN.md) states the whole of the
build system, the flags and their justifications, and the workflow that runs the
same verification upon GitHub.

## Licensing

Three licences apply, one to each kind of thing here.
[`LICENSING.md`](LICENSING.md) is the map and the texts are in
[`LICENSES/`](LICENSES/).

| Path | Licence |
| ---- | ------- |
| `boot/`, `kernel/`, `drivers/`, `graphics/` | `LGPL-3.0-or-later` |
| `libc/`, `userland/` | `MIT` |
| `docs/`, the `README.md` files, this one included | `CC0-1.0` |
| `Makefile`, `build_*.sh`, `.gitignore`, `.gitattributes`, `.github/` | `CC0-1.0` |

**Using this kernel's services through the system-call interface does not make a
program a derivative work of it.** `LICENSING.md`, Section 2, states the position
and its one outstanding consequence.

## Documentation

The design documentation resides in [`docs/`](docs/), grouped by subject into
four directories. [`docs/README.md`](docs/README.md) is its index.

`docs/project/PLAN.md` states what is done and what is not; it is the single
source of truth for progress, and nothing below restates it.
`docs/project/STATUS.md` states what the system does today.

### [`docs/project/`](docs/project/) — how the work is conducted

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](docs/project/PLAN.md) | The thirteen-phase roadmap and the task tracker: where the work stands and what comes next. This is the single source of truth for progress. |
| [`STATUS.md`](docs/project/STATUS.md) | The present condition of the system, one paragraph to a phase, and which environments each phase has been observed to work in. |
| [`HISTORY.md`](docs/project/HISTORY.md) | The revision history: one row per change, pointing at the commit and the design document that hold the detail. |
| [`VERSIONING.md`](docs/project/VERSIONING.md) | What a release gets called. One repository, many releases: `Oxys 1`, `Oxys 1.1`, `Oxys Aetos` — which is `Oxys 4` with a name — and editions such as `Oxys Aetos Workspace Edition`, which are varieties of a release rather than successors to it. Nothing has been released yet; the scheme is written before the first one rather than under the pressure of it. |
| [`TESTING.md`](docs/project/TESTING.md) | The test procedure and the four environments — QEMU, VirtualBox, OVMF and physical hardware — with what each is good for that the others are not. |
| [`TESTING-SYSTEM.md`](docs/project/TESTING-SYSTEM.md) | What the verification of each non-graphical subsystem establishes, and the deliberately-inserted defect that confirmed the assertion was worth making. |
| [`TESTING-GRAPHICS.md`](docs/project/TESTING-GRAPHICS.md) | The same for the graphical work, which is apart because most of what matters there cannot be asserted by the kernel and has to be looked at. |
| [`TESTING-RECORD.md`](docs/project/TESTING-RECORD.md) | The dated record of every test performed, with its outcome. |
| [`TOOLCHAIN.md`](docs/project/TOOLCHAIN.md) | The cross-compilation toolchain, its construction, the build system, and the workflow that runs the verification upon GitHub. |
| [`CODING-STANDARDS.md`](docs/project/CODING-STANDARDS.md) | The mandatory conventions of style, naming, documentation and compiler diagnostics. |
| [`REFERENCES.md`](docs/project/REFERENCES.md) | The bibliography of authoritative specifications consulted by the project. |
| [`INSPIRATIONS.md`](docs/project/INSPIRATIONS.md) | The systems this project takes its character from — ToaruOS principally, BSD besides — the appearance the desktop is intended to have, and the line between an inspiration and a source of code. |

### [`docs/design/`](docs/design/) — the kernel itself

| Document | Subject |
| -------- | ------- |
| [`ARCHITECTURE.md`](docs/design/ARCHITECTURE.md) | The overall structure of the system, the source tree layout, and the subsystem dependency ordering. |
| [`BOOT.md`](docs/design/BOOT.md) | The boot sequence, from the GRUB handover to the invocation of `KernelMain`. |
| [`MEMORY-LAYOUT.md`](docs/design/MEMORY-LAYOUT.md) | The physical and virtual address space layout, the paging hierarchy, and the allocators above it. |
| [`INTERRUPTS.md`](docs/design/INTERRUPTS.md) | The interrupt descriptor table, the stubs and the dispatcher, the exception handlers, and the 8259A interrupt controllers. |
| [`PRIVILEGE.md`](docs/design/PRIVILEGE.md) | The apparatus of a privilege transition: the user-mode descriptors, the task state segment and its trusted stacks, the registers that configure `SYSCALL`, and the entry path, dispatch table and argument validation built upon them. |
| [`GRAPHICS.md`](docs/design/GRAPHICS.md) | The index of the five documents that describe the graphical work of sub-tasks 6.2 to 6.6, and why it is five documents rather than one. |
| [`FRAMEBUFFER.md`](docs/design/FRAMEBUFFER.md) | The framebuffer: how it is asked for, what is validated about it, why its pages are write-combining, how it is mapped, and what became of the text console that had the screen. |
| [`DRAWING.md`](docs/design/DRAWING.md) | The drawing primitives: the surface they name instead of the framebuffer, the clipping that is a memory-safety boundary rather than a convenience, and the pixel, rectangle, line and blit. |
| [`CONSOLE.md`](docs/design/CONSOLE.md) | The bitmap font drawn for this project, the console that draws the boot log with it, and the measurement that found the console slow and the specialisation that fixed it. |
| [`FAULTSCREEN.md`](docs/design/FAULTSCREEN.md) | The screen each severe fault draws when the machine stops — one page per fault rather than one for all — and what a screen must survive to be drawn at all. |
| [`COMPOSITOR.md`](docs/design/COMPOSITOR.md) | The pointer, and the compositor that put a back buffer beneath all of it — after which nothing reads the framebuffer. |
| [`EXECUTABLE.md`](docs/design/EXECUTABLE.md) | The ELF64 loader: a piece of the kernel that does what an untrusted document tells it to, so its design is the list of things it refuses to be told. |
| [`PROCESS.md`](docs/design/PROCESS.md) | The process, the thread and the saved context; the switch that exchanges one for another, and the descent to privilege level 3 by which a program first ran. |
| [`CONCURRENCY.md`](docs/design/CONCURRENCY.md) | The ticket spinlock that masks interrupts for as long as it is held, the per-processor area it is built upon and the segment base that reaches it, the interrupt one processor sends to another, and the shootdown that makes a paging-structure change true everywhere — all of it built, and exercised, before there is a second processor. |

### [`docs/devices/`](docs/devices/) — the hardware the kernel drives

| Document | Subject |
| -------- | ------- |
| [`TIME.md`](docs/devices/TIME.md) | The kernel's time sources: the programmable interval timer, the system tick, and what remains to be added. |
| [`DISPLAY.md`](docs/devices/DISPLAY.md) | The VGA text-mode display: the register configuration, the cursor, the attributes, the control characters, how far a backspace may retreat, and how sub-task 6.2 displaced it — until sub-task 6.4 gave the framebuffer a console of its own. |
| [`SERIAL.md`](docs/devices/SERIAL.md) | The 16550 serial adapter: the line parameters, the two output modes, the buffering discipline and the interrupt service. |
| [`KEYBOARD.md`](docs/devices/KEYBOARD.md) | The 8042 controller, scan code set 1 and its translation, the modifier discipline and the input buffer. |
| [`MOUSE.md`](docs/devices/MOUSE.md) | The PS/2 mouse: why the controller became a module of its own, the framing of a packet stream that has no framing, the nine bits a movement actually occupies, and the pointer that keeps the pixels beneath it. |
| [`PCI.md`](docs/devices/PCI.md) | The PCI bus: configuration space access mechanism one, the walk of buses and functions, and what the enumeration records. |

### [`docs/storage/`](docs/storage/) — the path from a medium to a caller

| Document | Subject |
| -------- | ------- |
| [`DISK.md`](docs/storage/DISK.md) | The ATA disk in programmed input/output mode: the registers, the two addressing modes, why a disk driver's failures are silent, and what storage this driver cannot reach and how it says so rather than reporting no disk. |
| [`AHCI.md`](docs/storage/AHCI.md) | The AHCI adaptor by first-party direct memory access: the handoff from the firmware, the ports, the command list, and the region descriptors that name a caller's own pages to the device. |
| [`SDCARD.md`](docs/storage/SDCARD.md) | The SD card and the embedded MultiMediaCard: the host controller upon the bus, the second command set of the card behind it, and the two encodings of a card's capacity. |
| [`BLOCK.md`](docs/storage/BLOCK.md) | The generic block-device layer: what a device is, what the layer refuses before a driver is reached, and why it is tested against memory. |
| [`BUFFER.md`](docs/storage/BUFFER.md) | The buffer cache: how a block is found, what is discarded when the store is full, and when a modified block reaches its device. |
| [`EXT2.md`](docs/storage/EXT2.md) | The EXT2 volume's structures: the superblock, the block group descriptor table and the inode, their decoding, and which volumes this kernel refuses to address. Section 10 enumerates every limitation of the EXT2 support, the two documents below included. |
| [`EXT2-FILES.md`](docs/storage/EXT2-FILES.md) | What is done with those structures: the directory and the resolution of a path through it, the reading of a file, the writing and truncation of one, and the names by which a file is reached. |
| [`EXT2-VERIFICATION.md`](docs/storage/EXT2-VERIFICATION.md) | The eleven self-tests of the EXT2 implementation, six of which assert against a volume `mke2fs` produced rather than one this kernel composed. |
| [`VFS.md`](docs/storage/VFS.md) | The virtual filesystem layer: the mount found through the node it covers, the file that is one node however many callers reach it, and the mark a mount leaves upon a volume it has open for writing. |

## Directory-level documentation

`PROJECT_GUIDELINES.md`, Section 10, requires every high-level directory holding
material to carry at least a `README.md` of its own. Those documents describe
their directory's contents locally; the documents in `docs/` describe the system
by subject. The two are complementary.

| Document | Subject |
| -------- | ------- |
| [`boot/README.md`](boot/README.md) | The boot directory: the Multiboot2 header, the entry point and the GRUB configuration. |
| [`kernel/README.md`](kernel/README.md) | The kernel core and its internal header corpus. |
| [`kernel/test/README.md`](kernel/test/README.md) | The boot-time self-tests, one file per subsystem, and the composed volume they are conducted upon. |
| [`drivers/README.md`](drivers/README.md) | The device drivers, one subdirectory per device class. |
| [`graphics/README.md`](graphics/README.md) | The framebuffer and the drawing above it. |
| [`docs/README.md`](docs/README.md) | The documentation corpus itself: what the four groups hold, and the form every document takes. Each group carries a `README.md` of its own. |

The directories `libc/`, `userland/`, `crypto/`, `net/` and `uefi/` are presently
empty and acquire their documents when material is first placed within them.
`graphics/` was among them until sub-task 6.2. `LICENSES/` holds licence texts
alone and carries no `README.md`; [`LICENSING.md`](LICENSING.md) describes it.
`.github/` likewise carries none: it holds the configuration by which GitHub
runs the verification, not material of the system, and
[`docs/project/TOOLCHAIN.md`](docs/project/TOOLCHAIN.md), Section 10, describes
it.

## Root documents

| Document | Subject |
| -------- | ------- |
| [`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md) | The conventions binding upon all work here. Amended only by explicit decision of the project owner. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | The working procedure those conventions imply: the order of work, what a change must carry before it is complete, and the standards a test, a document and a commit are held to. |
| [`LICENSING.md`](LICENSING.md) | Which licence applies to which path, why the three were chosen, and where the boundary between the kernel and a program running upon it falls. |
| [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) | The standard of conduct expected, what is not acceptable, how to report a concern and what follows from one — and why blunt criticism of the work is expected rather than forbidden. |
| [`SECURITY.md`](SECURITY.md) | The one security boundary this system actually has, what is in force defending it, what is not in force and when it is due, and how to report a defect that crosses it. |

## Document conventions

Every document states, in its opening section, the phase of `docs/project/PLAN.md` to which
it corresponds and the specifications upon which it depends. Assertions of
hardware or protocol behaviour carry a citation to a numbered section of a
specification listed in `docs/project/REFERENCES.md`.
