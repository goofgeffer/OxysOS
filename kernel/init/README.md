<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `kernel/init/` — The Phases of the Boot

**Phase**: introduced on 2026-09-25, after `Oxys 1 Beta`, as a reorganisation of
`KernelMain`. Nothing here is new code; every statement was moved from
`kernel/kernel.c`.
**Detailed design**: [`../../docs/design/ARCHITECTURE.md`](../../docs/design/ARCHITECTURE.md),
Section 4, the dependency order the phases are called in.

## Purpose

This directory holds the boot sequence, one file for each phase, in the order
`KernelMain` calls them.

**Why the boot was divided.** Until 2026-09-25, `KernelMain` was one function of
eleven hundred lines, and `kernel.c` held twenty-six hundred. The order of
initialisation is the substance of the boot, and it could be read only by
scrolling through all of it. A change to one phase was a change to the file
every other phase lived in. This is the arrangement `kernel/test/` reached at
sub-task 6.1 for the self-tests, applied to what calls them. `KernelMain` is now
the list of phases, with a comment at each point where the order is not the
order of the phases' numbers.

**What was not changed.** The statements were moved, not reordered. The order
within each phase, and the comments that give its reasons, are what they were.
The self-tests each phase runs are the ones it ran. A reordering would have been
a second change hidden inside the first, and the boot's order is exactly where
such a change breaks something silently: a test run before its subsystem exists
reads zeroes and may pass. Two edits were made in passing. The echo loop's
comment, which had drifted two functions away from the loop, is back above it.
A call made twice in succession on one path of the session is now made once.

**What stayed in `kernel.c`.** What every phase and every subsystem writes
through: the diagnostic channel, the display's quiet and its mode, the command
line, the panic and the halt, the boot and power screens, and the power call.
None of these is a step of the boot; each is a service the boot uses.

## Contents

| Path | Description |
| ---- | ----------- |
| `internal.h` | The phase functions, in call order, and what the phases share with `kernel.c` and with each other: the display mode, the halt, the boot screen, the desktop-entry test and the root mount. Included by nothing outside `kernel.c` and this directory. |
| `early.c` | `KernelInitialiseEarly`: the serial line and the text display, the second check of the Multiboot2 magic, the banner, the per-processor area, the parse of the boot information, and the test of the display every later test reports through. |
| `memory.c` | `KernelInitialiseMemory`: Phase 2, the frame allocator, the paging hierarchy, the arena and the heap, and the growing table. `KernelInitialiseFrameReferences`: per-frame reference counting, called after the display. |
| `display.c` | `KernelInitialiseDisplay`: the framebuffer, the drawing primitives, the compositor and the console on it, the boot screen, and the window manager asserted on a screen in memory. |
| `interrupts.c` | `KernelInitialiseInterrupts`: Phase 3, the descriptor tables, the task state segment, the system call entry, the exceptions and the 8259A request layer; and the copy-on-write and address-space tests that needed the fault handler. |
| `devices.c` | `KernelInitialiseDevices`: Phase 4, the timer, the real-time clock, the PS/2 controller, the keyboard, the terminal, the mouse and the pointer. `KernelAttachPointer` gives the pointer its display. |
| `processes.c` | `KernelInitialiseProcesses`: the privilege transition and system calls, the ELF loader, processes and threads, the context switch, the descent to privilege level 3, and the serial line moved onto interrupts. |
| `processors.c` | `KernelInitialiseProcessors`: sub-tasks 6.12 to 6.15, the firmware tables and both APICs, the per-processor facilities, the scheduler and the application processors. |
| `storage.c` | `KernelInitialiseStorage`: Phase 5, the bus, the three disk controllers, the block layer and ramdisk, the buffer cache, EXT2 and the filesystem layer. `KernelMountRootVolume`: the ramdisk at the root, the persistent `/etc`, and the machine's volume at `/mnt`. |
| `userland.c` | `KernelVerifyUserland`: Phases 7 to 9 asserted from the root, from `fork` to the calendar, with the root mounted where the first test that needs it stands. |
| `session.c` | `KernelEnterSession`: the completion banner, then the screen given to the window manager and `init`, to the shell, or to the echo loop. It does not return. |

## Present limitations

1. **Each phase is still a long straight sequence.** The division is by phase,
   not by subsystem: `processors.c` and `userland.c` are each more than a
   hundred lines of calls and the comments that order them. A further division
   would split steps whose order depends on each other across files, which is
   the opposite of the purpose.
2. **The phases share state through `kernel.c`**, the display mode above all,
   rather than each owning its own. The mode is set by the session and read by
   the tick, and has no better home until the display service is a subsystem of
   its own.
