<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Status

What the system does today, where it has been observed to do it, and what is
known to be missing. Progress by sub-task is [`PLAN.md`](PLAN.md); how each
subsystem works is its design document.

| | |
| - | - |
| Phases complete | 1 to 8; Phase 9 to sub-task 9.7 |
| Latest release | `Oxys 1 Beta`, 2026-09-24 — [`RELEASE-1-BETA.md`](RELEASE-1-BETA.md) |
| Next release | `Oxys 1`, at about sub-task 11.10 — [`VERSIONING.md`](VERSIONING.md) |
| Self-test assertions | 78 |

The kernel asserts its own properties at boot, and `make verify` fails if any
reports a failure or the completion banner is absent. The assertions are in
[`../../kernel/test/`](../../kernel/test/); each design document pairs its
assertions with the failures they catch.

## 1. What works

| Area | Today | Documentation |
| ---- | ----- | ------------- |
| Boot | Multiboot2 through GRUB into long mode at a higher-half address; a quiet boot to the desktop by default, and menu entries for diagnostics, the shell alone and the write probe. | [`BOOT.md`](../design/BOOT.md) |
| Memory | A bitmap frame allocator, the permanent paging hierarchy, a direct physical map, a kernel heap, per-frame reference counts and copy-on-write. | [`MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md) |
| Interrupts | The IDT, 256 stubs, a dispatcher, exceptions with a disposition (a faulting program ends; the kernel stops only for its own faults), the Local APIC and I/O APIC programmed from ACPI. | [`INTERRUPTS.md`](../design/INTERRUPTS.md), [`APIC.md`](../devices/APIC.md) |
| Devices | Serial (16550), VGA text, PS/2 keyboard and mouse, PCI, the interval timer, the real-time clock. | [`devices/`](../devices/README.md) |
| Storage | ATA PIO, AHCI and SD host controller drivers; a block layer and buffer cache; EXT2 read and write; a VFS joining volumes into one tree; the initial ramdisk as root; a persistent `/etc` upon a disk labelled `oxys-etc`. | [`storage/`](../storage/README.md) |
| Processors | Every processor the firmware declares is started; per-processor run queues, a ticket spinlock, IPIs and TLB shootdown; user threads run upon the bootstrap processor. | [`SMP.md`](../design/SMP.md), [`SCHEDULER.md`](../design/SCHEDULER.md) |
| Programs | ELF64 programs at privilege level 3; `fork`, `execve`, `exit`, `wait`; 44 system calls; signals, process groups and job control. | [`PROCESS.md`](../design/PROCESS.md), [`PRIVILEGE.md`](../design/PRIVILEGE.md) |
| C library | Strings, heap, buffered I/O, formatted output, `<time.h>`, the configuration parser, the line editor, the terminal grid, the icon and image formats. | [`LIBC.md`](../design/LIBC.md) |
| Shell | Line editing with history, pipelines, redirection, built-ins, job control; utilities in `/bin`. | [`SHELL.md`](../design/SHELL.md) |
| Graphics | A composited framebuffer, 2D primitives, the bitmap face and console, fault screens, the pointer. | [`FRAMEBUFFER.md`](../design/FRAMEBUFFER.md), [`DRAWING.md`](../design/DRAWING.md), [`CONSOLE.md`](../design/CONSOLE.md), [`FAULTSCREEN.md`](../design/FAULTSCREEN.md), [`COMPOSITOR.md`](../design/COMPOSITOR.md) |
| Desktop | A stacking window manager with minimise and full screen; `init` supervising the session; a panel with a launcher, a list of windows and a clock; a background; a terminal, a file manager and a text viewer. | [`WINDOWS.md`](../design/WINDOWS.md), [`SESSION.md`](../design/SESSION.md), [`UTILITIES.md`](../design/UTILITIES.md) |
| Configuration | `/etc/system.conf`, `/etc/desktop.conf`, `/etc/session.conf`; the shipped copies at `/share/defaults/etc`; the launcher reads its file again at every opening. | [`CONFIG.md`](../design/CONFIG.md), [`PERSIST.md`](../storage/PERSIST.md) |

## 2. Where it has been observed

Every boot runs every self-test, so a clean boot is evidence for every phase at
once. The environments and how they are driven are [`TESTING.md`](TESTING.md);
each run is a row of [`TESTING-RECORD.md`](TESTING-RECORD.md).

| Phases | QEMU | VirtualBox | Bochs | OVMF (UEFI) | Physical hardware |
| ------ | ---- | ---------- | ----- | ----------- | ----------------- |
| 1 | Yes | Yes | Yes | No UEFI path until Phase 12 | Yes, one machine (sub-task 1.12) |
| 2, 3, 5, 6 | Yes | Yes | Yes | — | Reached, not examined |
| 4 | Yes | Yes | Yes | — | Yes: it found the AHCI and eMMC cases that became sub-tasks 4.7 and 4.8 |
| 7, 8 | Yes, driven over the serial line | Yes, driven at the PS/2 keyboard | Yes, to the prompt | — | Not run |
| 9 | Yes, driven through the monitor and captured | Yes, at 640 by 480 | Yes, at 1024 by 768 | — | Not run |

- **The physical machine** is an HP Laptop 14-dq0052dx (Celeron N4120, eMMC,
  no serial port); its log is read from the graphical console.
  [`TESTING.md`](TESTING.md).
- **Bochs** needs the source-tree build configured for x86-64 and SMP; the
  default build on `PATH` cannot run this kernel. [`TESTING.md`](TESTING.md).
- **Machine-dependent paths not yet taken** by any run: the XSDT, the Local
  APIC address override, a second I/O APIC, more than two processors, and any
  interrupt source override QEMU does not declare.

## 3. Known gaps

The gaps that cross subsystems. Each design document lists its own.

| Missing | Expected |
| ------- | -------- |
| User programs upon any processor but the bootstrap one: the allocators, process tables and filesystem layer are unsynchronised beyond the run queues and the process and thread tables. [`CONCURRENCY.md`](../design/CONCURRENCY.md) | Not scheduled |
| A canonical terminal mode: `stdin` is raw. [`SHELL.md`](../design/SHELL.md) | When a program needs it |
| A reaper for kernel threads: a finished kernel thread's stack is held until the machine stops. | Not scheduled |
| Thread migration, work stealing and priorities. | Phase 13 |
| A resize of a window by hand. [`WINDOWS.md`](../design/WINDOWS.md) | Not scheduled |
| A persistent `/home`: needs partition tables, which the block layer does not read. [`PERSIST.md`](../storage/PERSIST.md) | Not scheduled |
| USB of any kind, and so USB storage. | Not scheduled |
| A time zone, and a way to set the clock. [`TIME.md`](../devices/TIME.md) | Not scheduled |
| The settings application. | 9.8 |
| `CR4.SMEP`, `CR4.SMAP` and `IA32_EFER.NXE`. | 13.3 |
| A UEFI boot path. | Phase 12 |
| Cryptography and networking. | Phases 10 and 11 |
