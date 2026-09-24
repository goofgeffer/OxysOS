<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `docs/design/` — The Kernel, the Library and the Desktop

How each subsystem works and why. Devices are [`../devices/`](../devices/README.md);
the storage stack is [`../storage/`](../storage/README.md). Listed in the order the
phases build them, which is the order to read them in.

| Document | Subject | Phase |
| -------- | ------- | ----- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The structure of the system, the source tree, and why the phases are ordered as they are. | All |
| [`BOOT.md`](BOOT.md) | From GRUB to `KernelMain`: the Multiboot2 header, long mode, the higher half. | 1 |
| [`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md) | Address spaces, paging, the frame allocator, the arena, the heap, reference counts, copy-on-write. | 2 |
| [`INTERRUPTS.md`](INTERRUPTS.md) | The IDT, stubs and trap frame, the dispatcher, exceptions, the 8259A, the request layer. | 3, 6.12 |
| [`PRIVILEGE.md`](PRIVILEGE.md) | Descriptors, the TSS, `SYSCALL`, the entry path, argument validation, the call table. | 6.1, 6.7 |
| [`FRAMEBUFFER.md`](FRAMEBUFFER.md) | The framebuffer: request, validation, write-combining, colour encoding. | 6.2 |
| [`DRAWING.md`](DRAWING.md) | Surfaces, clipping, and the primitives. | 6.3 |
| [`CONSOLE.md`](CONSOLE.md) | The system face and the graphical console. | 6.4 |
| [`FAULTSCREEN.md`](FAULTSCREEN.md) | The page drawn when the kernel stops. | 6.4 |
| [`COMPOSITOR.md`](COMPOSITOR.md) | The back buffer, layers, damage, clip stack and blending. | 6.5, 6.6 |
| [`EXECUTABLE.md`](EXECUTABLE.md) | The ELF64 loader. | 6.8 |
| [`PROCESS.md`](PROCESS.md) | Processes, threads, the switch, `fork`, `execve`, `exit`, `wait`, signals. | 6.9–6.11, 8.7 |
| [`CONCURRENCY.md`](CONCURRENCY.md) | Spinlocks, per-processor areas, IPIs, shootdown; what is still unsynchronised. | 6.13 |
| [`SMP.md`](SMP.md) | Starting the application processors. | 6.14 |
| [`SCHEDULER.md`](SCHEDULER.md) | Run queues, the quantum, affinity, sleeping and waking. | 6.15 |
| [`LIBC.md`](LIBC.md) | The C library: strings, wrappers, heap, stdio, startup, the filesystem calls. | 7 |
| [`SHELL.md`](SHELL.md) | The terminal, the line editor, the shell and its utilities. | 8 |
| [`WINDOWS.md`](WINDOWS.md) | The window manager and its client protocol. | 9.1, 9.2 |
| [`INIT.md`](INIT.md) | `init`, orphans, `power`, the boot and power screens. | 9.3 |
| [`CONFIG.md`](CONFIG.md) | The configuration format and `/etc`. | 9.4 |
| [`SESSION.md`](SESSION.md) | The session: layers, root, panel, launcher, icons, background. | 9.5 |
| [`TERMINAL.md`](TERMINAL.md) | The terminal emulator, `poll`, and the shell without a terminal. | 9.6 |
| [`UTILITIES.md`](UTILITIES.md) | The file manager, the text viewer and the clock. | 9.7 |

`LIBC.md` and `SHELL.md` describe userland, not the kernel; they are here because
each is the other side of an interface this group documents.
