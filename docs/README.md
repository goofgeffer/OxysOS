# Oxys-OS Documentation Corpus

This directory contains the design documentation of Oxys-OS. In accordance with
`CLAUDE.md`, Section 2, no code change is final until the documents affected by
it have been updated.

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](PLAN.md) | The thirteen-phase roadmap and the task tracker. This is the single source of truth for progress. |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The overall structure of the system, the source tree layout, and the subsystem dependency ordering. |
| [`BOOT.md`](BOOT.md) | The boot sequence, from the GRUB handover to the invocation of `KernelMain`. |
| [`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md) | The physical and virtual address space layout, and the boot-time paging hierarchy. |
| [`TOOLCHAIN.md`](TOOLCHAIN.md) | The cross-compilation toolchain, its construction, and the build system. |
| [`CODING-STANDARDS.md`](CODING-STANDARDS.md) | The mandatory conventions of style, naming, documentation and compiler diagnostics. |
| [`TESTING.md`](TESTING.md) | The test procedure under QEMU, VirtualBox, OVMF and physical hardware. |
| [`REFERENCES.md`](REFERENCES.md) | The bibliography of authoritative specifications consulted by the project. |

## Directory-level documentation

`CLAUDE.md`, Section 10, requires every high-level directory holding material to
carry at least a `README.md` of its own. Those documents describe their
directory's contents locally; the documents in this corpus describe the system by
subject. The two are complementary.

| Document | Subject |
| -------- | ------- |
| [`../boot/README.md`](../boot/README.md) | The boot directory: the Multiboot2 header, the entry point and the GRUB configuration. |
| [`../kernel/README.md`](../kernel/README.md) | The kernel core and its internal header corpus. |
| [`../drivers/README.md`](../drivers/README.md) | The device drivers, one subdirectory per device class. |

The directories `libc/`, `userland/`, `graphics/`, `crypto/`, `net/` and `uefi/`
are presently empty and acquire their documents when material is first placed
within them.

## Document conventions

Every document states, in its opening section, the phase of `PLAN.md` to which
it corresponds and the specifications upon which it depends. Assertions of
hardware or protocol behaviour carry a citation to a numbered section of a
specification listed in `REFERENCES.md`.
