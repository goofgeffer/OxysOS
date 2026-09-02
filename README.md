# Oxys-OS

Oxys-OS is a monolithic, Unix-like operating system for x86_64, written from
scratch in ISO C11 and NASM assembly. It targets real hardware and is tested
under QEMU and VirtualBox.

The conventions binding upon all work in this repository are set out in
[`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md). In accordance with its
Section 2, no code change is final until the documents affected by it have been
updated.

## Documentation

The design documentation resides in [`docs/`](docs/), grouped by subject into
four directories. [`docs/README.md`](docs/README.md) is its index.

### [`docs/project/`](docs/project/) — how the work is conducted

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](docs/project/PLAN.md) | The thirteen-phase roadmap and the task tracker. This is the single source of truth for progress. |
| [`TESTING.md`](docs/project/TESTING.md) | The test procedure under QEMU, VirtualBox, OVMF and physical hardware, and the record of every test performed. |
| [`TOOLCHAIN.md`](docs/project/TOOLCHAIN.md) | The cross-compilation toolchain, its construction, and the build system. |
| [`CODING-STANDARDS.md`](docs/project/CODING-STANDARDS.md) | The mandatory conventions of style, naming, documentation and compiler diagnostics. |
| [`REFERENCES.md`](docs/project/REFERENCES.md) | The bibliography of authoritative specifications consulted by the project. |
| [`INSPIRATIONS.md`](docs/project/INSPIRATIONS.md) | The systems this project takes its character from — ToaruOS principally, SerenityOS and BSD besides — and the line between an inspiration and a source of code. |

### [`docs/design/`](docs/design/) — the kernel itself

| Document | Subject |
| -------- | ------- |
| [`ARCHITECTURE.md`](docs/design/ARCHITECTURE.md) | The overall structure of the system, the source tree layout, and the subsystem dependency ordering. |
| [`BOOT.md`](docs/design/BOOT.md) | The boot sequence, from the GRUB handover to the invocation of `KernelMain`. |
| [`MEMORY-LAYOUT.md`](docs/design/MEMORY-LAYOUT.md) | The physical and virtual address space layout, the paging hierarchy, and the allocators above it. |
| [`INTERRUPTS.md`](docs/design/INTERRUPTS.md) | The interrupt descriptor table, the stubs and the dispatcher, the exception handlers, and the 8259A interrupt controllers. |

### [`docs/devices/`](docs/devices/) — the hardware the kernel drives

| Document | Subject |
| -------- | ------- |
| [`TIME.md`](docs/devices/TIME.md) | The kernel's time sources: the programmable interval timer, the system tick, and what remains to be added. |
| [`DISPLAY.md`](docs/devices/DISPLAY.md) | The VGA text-mode display: the register configuration, the cursor, the attributes, the control characters and how far a backspace may retreat. |
| [`SERIAL.md`](docs/devices/SERIAL.md) | The 16550 serial adapter: the line parameters, the two output modes, the buffering discipline and the interrupt service. |
| [`KEYBOARD.md`](docs/devices/KEYBOARD.md) | The 8042 controller, scan code set 1 and its translation, the modifier discipline and the input buffer. |
| [`PCI.md`](docs/devices/PCI.md) | The PCI bus: configuration space access mechanism one, the walk of buses and functions, and what the enumeration records. |

### [`docs/storage/`](docs/storage/) — the path from a medium to a caller

| Document | Subject |
| -------- | ------- |
| [`DISK.md`](docs/storage/DISK.md) | The ATA disk in programmed input/output mode: the registers, the two addressing modes, and why a disk driver's failures are silent. |
| [`BLOCK.md`](docs/storage/BLOCK.md) | The generic block-device layer: what a device is, what the layer refuses before a driver is reached, and why it is tested against memory. |
| [`BUFFER.md`](docs/storage/BUFFER.md) | The buffer cache: how a block is found, what is discarded when the store is full, and when a modified block reaches its device. |
| [`EXT2.md`](docs/storage/EXT2.md) | The EXT2 volume: the superblock, its decoding, and which volumes this kernel refuses to address. |

## Directory-level documentation

`PROJECT_GUIDELINES.md`, Section 10, requires every high-level directory holding
material to carry at least a `README.md` of its own. Those documents describe
their directory's contents locally; the documents in `docs/` describe the system
by subject. The two are complementary.

| Document | Subject |
| -------- | ------- |
| [`boot/README.md`](boot/README.md) | The boot directory: the Multiboot2 header, the entry point and the GRUB configuration. |
| [`kernel/README.md`](kernel/README.md) | The kernel core and its internal header corpus. |
| [`drivers/README.md`](drivers/README.md) | The device drivers, one subdirectory per device class. |
| [`docs/README.md`](docs/README.md) | The documentation corpus itself: what the four groups hold, and the form every document takes. Each group carries a `README.md` of its own. |

The directories `libc/`, `userland/`, `graphics/`, `crypto/`, `net/` and `uefi/`
are presently empty and acquire their documents when material is first placed
within them.

## Document conventions

Every document states, in its opening section, the phase of `docs/project/PLAN.md` to which
it corresponds and the specifications upon which it depends. Assertions of
hardware or protocol behaviour carry a citation to a numbered section of a
specification listed in `docs/project/REFERENCES.md`.
