# Oxys-OS

Oxys-OS is a monolithic, Unix-like operating system for x86_64, written from
scratch in ISO C11 and NASM assembly. It targets real hardware and is tested
under QEMU and VirtualBox.

The conventions binding upon all work in this repository are set out in
[`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md). In accordance with its
Section 2, no code change is final until the documents affected by it have been
updated.

## Documentation

The design documentation resides in [`docs/`](docs/).

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](docs/PLAN.md) | The thirteen-phase roadmap and the task tracker. This is the single source of truth for progress. |
| [`ARCHITECTURE.md`](docs/ARCHITECTURE.md) | The overall structure of the system, the source tree layout, and the subsystem dependency ordering. |
| [`BOOT.md`](docs/BOOT.md) | The boot sequence, from the GRUB handover to the invocation of `KernelMain`. |
| [`MEMORY-LAYOUT.md`](docs/MEMORY-LAYOUT.md) | The physical and virtual address space layout, and the boot-time paging hierarchy. |
| [`TOOLCHAIN.md`](docs/TOOLCHAIN.md) | The cross-compilation toolchain, its construction, and the build system. |
| [`CODING-STANDARDS.md`](docs/CODING-STANDARDS.md) | The mandatory conventions of style, naming, documentation and compiler diagnostics. |
| [`TESTING.md`](docs/TESTING.md) | The test procedure under QEMU, VirtualBox, OVMF and physical hardware. |
| [`REFERENCES.md`](docs/REFERENCES.md) | The bibliography of authoritative specifications consulted by the project. |

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

The directories `libc/`, `userland/`, `graphics/`, `crypto/`, `net/` and `uefi/`
are presently empty and acquire their documents when material is first placed
within them.

## Document conventions

Every document states, in its opening section, the phase of `docs/PLAN.md` to which
it corresponds and the specifications upon which it depends. Assertions of
hardware or protocol behaviour carry a citation to a numbered section of a
specification listed in `docs/REFERENCES.md`.
