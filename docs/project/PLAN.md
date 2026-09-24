<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Development Plan

The roadmap of Oxys-OS: every sub-task of the thirteen phases and its state.
This is the single source of truth for progress (`PROJECT_GUIDELINES.md`,
Section 5). What works today is [`STATUS.md`](STATUS.md); how each subsystem
works is its design document; when each change was made is
[`HISTORY.md`](HISTORY.md).

| | |
| - | - |
| Target | x86_64, monolithic kernel |
| Boot | Multiboot2 through GRUB (BIOS); UEFI in Phase 12 |
| Complete | Phases 1 to 8; Phase 9 to sub-task 9.7 |
| Next | Sub-task 9.8, the settings application, which closes Phase 9 |
| Releases | `Oxys 1 Alpha` at 8.7 (cut); `Oxys 1 Beta` at 9.7 (awaiting the owner's cut); `Oxys 1` at about 11.10 — [`VERSIONING.md`](VERSIONING.md) |

**State** is one of `Planned`, `In progress` or `Implemented`. A sub-task is
`Implemented` only when the self-test named in **Asserted by** passes under
`make verify`; the names are functions under
[`../../kernel/test/`](../../kernel/test/) or the user programs that the
self-test runs. A change of state is made in the commit that causes it.

## Phase 1 — Bootstrapping and Early Output

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 1.1 | Verify the cross-compilation toolchain (`x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`, `xorriso`) is present and functional. | Implemented | `make toolcheck` |
| 1.2 | Author the linker script `linker.ld` defining a higher-half kernel at virtual base `0xFFFFFFFF80000000` with a physical load address of `0x00100000`. | Implemented | — |
| 1.3 | Author `boot/boot.asm`: Multiboot2 header, 32-bit protected-mode entry point, Multiboot2 magic validation, CPUID and long-mode feature detection. | Implemented | `grub-file`, at each link |
| 1.4 | Construct the boot-time paging hierarchy (PML4, two PDPTs, one PD) providing a 1 GiB identity map and a coincident 1 GiB higher-half map using 2 MiB pages. | Implemented | — |
| 1.5 | Perform the long-mode transition (CR4.PAE, IA32_EFER.LME, CR0.PG) and load a 64-bit GDT. | Implemented | — |
| 1.6 | Transfer control to the higher-half 64-bit kernel entry point and establish the kernel stack. | Implemented | — |
| 1.7 | Implement a minimal VGA text-mode output routine and a minimal COM1 serial output routine for early diagnostics. | Implemented | `dev/devices.c` |
| 1.8 | Implement `KernelMain`, which clears the screen and prints the string "Oxys-OS". | Implemented | `make verify` banner |
| 1.9 | Author the `Makefile` with the targets `all`, `clean`, `iso`, `run-qemu`, `run-vbox` and `run-uefi`. | Implemented | — |
| 1.10 | Generate the ISO image and verify boot under QEMU. | Implemented | `make verify` |
| 1.11 | Verify boot under VirtualBox. | Implemented | [`TESTING.md`](TESTING.md) §4 |
| 1.12 | Verify boot on physical hardware from a USB medium. | Implemented | [`TESTING.md`](TESTING.md) §5.1–5.2 |

## Phase 2 — Memory Management (including Copy-on-Write)

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 2.1 | Parse the Multiboot2 information structure and extract the memory map (tag type 6) and the ELF section headers (tag type 9). | Implemented | `mm/memory.c` (indirect) |
| 2.2 | Implement a physical frame allocator (bitmap) covering all usable regions, reserving the kernel image, the Multiboot2 structures and the low 1 MiB. | Implemented | `mm/memory.c` |
| 2.3 | Construct a permanent kernel page-table hierarchy, replacing the boot-time tables and removing the low identity map. | Implemented | `mm/memory.c` |
| 2.4 | Implement a direct physical map region for kernel access to arbitrary frames. | Implemented | `mm/memory.c` |
| 2.5 | Implement a kernel virtual-address-space allocator and a general-purpose kernel heap (slab allocator over a buddy-style page allocator). | Implemented | `mm/memory.c` |
| 2.6 | Implement per-frame reference counting as the substrate for shared pages. | Implemented | `mm/memory.c` |
| 2.7 | Implement the page-fault handler dispatch path (dependent upon Phase 3) and the copy-on-write fault resolution routine. | Implemented | `mm/memory.c` |
| 2.8 | Implement address-space cloning that marks writable user pages read-only and increments frame reference counts. | Implemented | `mm/memory.c` |

## Phase 3 — Interrupts, Exceptions and Keyboard Input

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 3.1 | Define the IDT and the 64-bit interrupt-gate descriptor format; load it with `lidt`. | Implemented | `arch/interrupts.c` |
| 3.2 | Author assembly stubs for vectors 0–255, normalising the presence or absence of a processor-pushed error code. | Implemented | `arch/interrupts.c` |
| 3.3 | Implement a C interrupt dispatcher operating on a formal trap frame structure. | Implemented | `arch/interrupts.c` |
| 3.4 | Implement exception handlers with register and stack diagnostics emitted over the serial port. | Implemented | `arch/interrupts.c`, `gfx/faultscreen.c` |
| 3.5 | Remap the 8259A PIC to vectors 32–47 and implement end-of-interrupt signalling. | Implemented | `dev/devices.c` |
| 3.6 | Implement the Programmable Interval Timer as the initial timer source. | Implemented | `dev/devices.c` |
| 3.7 | Implement the PS/2 keyboard driver: controller initialisation, scancode set 1 translation, modifier state and a circular input buffer. | Implemented | `dev/devices.c` |

## Phase 4 — Basic Device Drivers

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 4.1 | Promote the early serial routine to a formal, interrupt-driven COM1 driver with configurable line parameters. | Implemented | `dev/devices.c` |
| 4.2 | Promote the early VGA routine to a formal text-mode driver with scrolling, cursor control and colour attributes. | Implemented | `dev/devices.c` |
| 4.3 | Implement PCI configuration-space enumeration by the legacy I/O port mechanism, with device and class identification. | Implemented | `dev/devices.c` |
| 4.4 | Implement an ATA PIO driver: bus reset, `IDENTIFY DEVICE`, 28-bit and 48-bit LBA sector read and write. | Implemented | `storage/stack.c` |
| 4.5 | Define a generic block-device abstraction layer above the ATA driver. | Implemented | `storage/stack.c` |
| 4.6 | Implement a buffer cache for block devices. | Implemented | `storage/stack.c` |
| 4.7 | Implement an AHCI driver, so that a machine whose firmware presents its SATA controller in AHCI mode has a disk at all. | Implemented | `storage/stack.c` |
| 4.8 | Implement an SD host controller driver, so that a machine whose system is upon an embedded MultiMediaCard part has storage at all. | Implemented | `storage/stack.c` |

## Phase 5 — EXT2 Filesystem

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 5.1 | Parse the superblock and validate the EXT2 magic number and revision level. | Implemented | `ext2/format.c` |
| 5.2 | Parse the block-group descriptor table. | Implemented | `ext2/format.c` |
| 5.3 | Implement inode retrieval and the resolution of direct, singly, doubly and triply indirect block pointers. | Implemented | `ext2/format.c`, `ext2/file.c` |
| 5.4 | Implement directory-entry traversal and absolute path resolution. | Implemented | `ext2/directory.c` |
| 5.5 | Implement file reading. | Implemented | `ext2/file.c` |
| 5.6 | Implement block and inode allocation, file writing, extension and truncation. | Implemented | `ext2/write.c` |
| 5.7 | Implement directory creation and entry insertion and removal. | Implemented | `ext2/write.c` |
| 5.8 | Define a virtual filesystem layer and mount an EXT2 root volume. | Implemented | `storage/vfs.c` |

## Phase 6 — Graphics, System Calls, Process Management and Symmetric Multi-Processing

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 6.1 | Install the GDT and TSS required for privilege transition; configure IA32_STAR, IA32_LSTAR and IA32_FMASK. | Implemented | `arch/privilege.c` (the configuration; the instruction is executed only by user programs, [`PRIVILEGE.md`](../design/PRIVILEGE.md)) |
| 6.2 | Request a linear framebuffer by the Multiboot2 framebuffer tag and map it into kernel space. | Implemented | `gfx/framebuffer.c` |
| 6.3 | Implement 2D primitives: pixel, line, rectangle, blit and clipping. | Implemented | `gfx/graphics.c` |
| 6.4 | Implement a bitmap font renderer, and a graphical console above it that the diagnostic path may write to. | Implemented | `gfx/console.c`, `gfx/faultscreen.c` |
| 6.5 | Implement a PS/2 mouse driver upon the second device port of the 8042, and a cursor. | Implemented | `dev/mouse.c` |
| 6.6 | Implement a compositing surface abstraction and double buffering. | Implemented | `gfx/compositor.c` |
| 6.7 | Implement the `SYSCALL` entry path, the system-call dispatch table and argument validation. | Implemented | `arch/syscall.c` |
| 6.8 | Implement the ELF64 loader for statically linked executables. | Implemented | `exec/elf.c` |
| 6.9 | Define the process control block, the address-space descriptor and the thread structure. | Implemented | `proc/process.c` |
| 6.10 | Implement context switching and the initial transition to user mode via `IRETQ`. | Implemented | `arch/usermode.c` |
| 6.11 | Implement `fork()` upon the Phase 2 copy-on-write substrate, together with `execve()`, `exit()` and `wait()`. | Implemented | `proc/lifecycle.c` |
| 6.12 | Parse the ACPI MADT; initialise the Local APIC and the I/O APIC; retire the 8259A PIC. | Implemented | `arch/apic.c`, `dev/devices.c` |
| 6.13 | Implement spinlocks, per-CPU data areas and inter-processor interrupts, including TLB shootdown. | Implemented | `arch/smp.c` |
| 6.14 | Implement application-processor bring-up by INIT-SIPI-SIPI and a real-mode trampoline. | Implemented | `arch/smp.c` |
| 6.15 | Implement a multiprocessor-aware round-robin scheduler with per-CPU run queues and processor affinity. | Implemented | `proc/sched.c` |

## Phase 7 — Userland and Minimal C Library

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 7.1 | Implement the freestanding string and memory functions (`<string.h>`). | Implemented | `libc/string.c` |
| 7.2 | Implement system-call wrappers for the complete kernel interface. | Implemented | `libc/wrappers.c` |
| 7.3 | Implement a user-space heap allocator (`malloc`, `free`, `realloc`) above `brk`/`mmap`. | Implemented | `libc/heap.c` |
| 7.4 | Implement buffered input and output (`<stdio.h>`) and formatted conversion. | Implemented | `libc/stdio/` |
| 7.5 | Author the C runtime startup object (`crt0`) and the static-linking procedure for user programs. | Implemented | `userland/startup-check/` |
| 7.6 | Implement the utilities `ls`, `cat`, `echo`, `mkdir` and `rm`. | Implemented | `userland/` |
| 7.7 | Construct an initial ramdisk containing the utilities and mount it as the early root. | Implemented | `kernel/test/storage/initrd.c` |

## Phase 8 — Shell

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 8.1 | Implement line editing with history. | Implemented | `KernelVerifyTerminal`, `KernelVerifyLine` |
| 8.2 | Implement the tokeniser and the command parser. | Implemented | `KernelVerifyShell` |
| 8.3 | Implement built-in commands (`cd`, `exit`, `export`, `pwd`). | Implemented | `KernelVerifyDirectory`, `KernelVerifyShell` |
| 8.4 | Implement external program execution by `fork()` and `execve()`. | Implemented | `KernelVerifyShell`, `KernelVerifyDirectory` |
| 8.5 | Implement input and output redirection. | Implemented | `KernelVerifyUtilities`, `KernelVerifyShell` |
| 8.6 | Implement pipelines. | Implemented | `KernelVerifyVfs`, `KernelVerifyUtilities`, `KernelVerifyShell` |
| 8.7 | Implement job control, process groups and terminal signal delivery. `Oxys 1 Alpha` was cut here: [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md). | Implemented | `KernelVerifySignals`, `KernelVerifyShell` |

## Phase 9 — The Desktop, its System Services and its Configuration

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 9.1 | Implement a stacking window manager with focus and event routing. | Implemented | `KernelVerifyWindows`, `KernelVerifyCircle` |
| 9.2 | Implement the client protocol by which user processes create, draw and receive events upon windows. | Implemented | `KernelVerifyClients`, `window-check` |
| 9.3 | Implement `init`: the first user process, the supervision of the services below it, and the orderly shutdown of both. | Implemented | `KernelVerifyInit`, `init-check` |
| 9.4 | Define the system configuration format, its parser, and the `/etc` hierarchy the services and the desktop read at start. | Implemented | `KernelVerifyConfig`, `config-check` |
| 9.5 | Implement the session: the desktop root, the panel, the launcher, and the ownership of the display that decides who may draw upon it. | Implemented | `KernelVerifyWindows`, `window-check` |
| 9.6 | Implement a terminal emulator window hosting the Phase 8 shell. | Implemented | `KernelVerifyTerm`, `poll-check` |
| 9.7 | Implement the utilities the desktop is not usable without: a file manager, a text viewer and a clock. `Oxys 1 Beta` is fixed here. | Implemented | `KernelVerifyRtc`, `KernelVerifyTime`, `signal-check` |
| 9.8 | Implement the settings application, by which the configuration of 9.4 is edited rather than hand-written. | Planned | — |

## Phase 10 — Cryptography

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 10.1 | Implement an entropy pool seeded from `RDSEED`/`RDRAND` where available and from timer jitter otherwise. | Planned | — |
| 10.2 | Implement a cryptographically secure deterministic random bit generator. | Planned | — |
| 10.3 | Implement SHA-256 with the FIPS 180-4 test vectors. | Planned | — |
| 10.4 | Implement AES-128 and AES-256 with the FIPS 197 test vectors. | Planned | — |
| 10.5 | Implement CBC and CTR modes of operation. | Planned | — |
| 10.6 | Expose the primitives to user space by system call and by a `/dev/random` device node. | Planned | — |

## Phase 11 — Networking

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 11.1 | Implement an Ethernet controller driver (RTL8139 or Intel E1000) with descriptor rings and interrupt handling. | Planned | — |
| 11.2 | Define the network buffer structure and the protocol layering framework. | Planned | — |
| 11.3 | Implement Ethernet frame transmission and reception. | Planned | — |
| 11.4 | Implement ARP with a resolution cache. | Planned | — |
| 11.5 | Implement IPv4, including fragmentation and reassembly, and a routing table. | Planned | — |
| 11.6 | Implement ICMP echo request and reply. | Planned | — |
| 11.7 | Implement UDP. | Planned | — |
| 11.8 | Implement TCP: the state machine, sequence-number handling, retransmission and flow control. | Planned | — |
| 11.9 | Implement the BSD-style socket system-call interface. | Planned | — |
| 11.10 | Implement DHCP client configuration and the `ping` utility. `Oxys 1` is cut at or about here. | Planned | — |

## Phase 12 — UEFI Transition

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 12.1 | Establish a PE32+ build path for a UEFI application image. | Planned | — |
| 12.2 | Implement the UEFI entry point and parse the System Table. | Planned | — |
| 12.3 | Retrieve the memory map, the ACPI RSDP and the Graphics Output Protocol framebuffer by Boot Services. | Planned | — |
| 12.4 | Define a boot-protocol-neutral handoff structure consumed by the kernel, populated identically from Multiboot2 or from UEFI. | Planned | — |
| 12.5 | Invoke `ExitBootServices` and transfer control to the kernel. | Planned | — |
| 12.6 | Integrate UEFI Runtime Services: time and variable access. | Planned | — |
| 12.7 | Produce a hybrid ISO image bootable by both BIOS and UEFI, and verify under OVMF. | Planned | — |

## Phase 13 — Polish, Optimisation and Final Hardening

| Sub-task | Deliverable | State | Asserted by |
| -------- | ----------- | ----- | ----------- |
| 13.1 | Profile interrupt latency, context-switch cost and filesystem throughput. | Planned | — |
| 13.2 | Optimise the scheduler for fairness and the block layer for read-ahead. | Planned | — |
| 13.3 | Enable NX, SMEP and SMAP; enforce write-exclusive-or-execute in kernel mappings. | Planned | — |
| 13.4 | Implement kernel stack guard pages and stack-canary protection. | Planned | — |
| 13.5 | Implement kernel address-space layout randomisation, if feasible. | Planned | — |
| 13.6 | Complete and review the whole of the `docs/` corpus. | Planned | — |
| 13.7 | Test on a minimum of three distinct physical machines, including UEFI systems. | Planned | — |
| 13.8 | Extend the userland utility set and produce the final release image. | Planned | — |

## Beyond the thirteen phases — self-hosting

**The objective**: a machine running Oxys-OS checks out this repository, builds
it and produces a bootable image of Oxys-OS, with no other operating system
involved. This is a direction, not a fourteenth phase: it has no sub-tasks yet.
It is written down so that the work leading to it is not foreclosed by
accident.

### Requirements

| Requirement | Where it stands |
| ----------- | --------------- |
| A writable filesystem with directories and a mountable root | Phase 5. Present. |
| Processes that fork, execute a program from a volume and are collected | Sub-task 6.11. Present. |
| A scheduler, so that a build runs beside other work | Sub-task 6.15. Present; user threads are confined to the bootstrap processor until the locks of [`CONCURRENCY.md`](../design/CONCURRENCY.md) are applied. A parallel build is what will first want them. |
| A C library a compiler can be built against | Phase 7, and larger than the utilities need. [`LIBC.md`](../design/LIBC.md) |
| A shell, job control and pipes | Phase 8. Present. |
| A C compiler, an assembler and a linker running upon Oxys-OS | Porting work, not scheduled. |
| An editor and a working utility set | Sub-task 13.8. |
| Storage, memory and time enough to compile the kernel on the machine itself | Open; bears on Phase 13 and on the hardware targeted. |

### The compiler is ported, not written

Decided by the project owner. A compiler good enough to compile this kernel is
a larger undertaking than the thirteen phases together, and one that is not
good enough does not reach the objective; porting reaches it, at the cost of
depending on code this project did not write. `PROJECT_GUIDELINES.md`,
Sections 2 and 8, state the rules for ports. Consequences:

- The C library of Phase 7 is sized for a compiler, not for `ls` and `cat`.
- The compiler, assembler and linker are ports. Which ones is decided when the
  library's cost is known; the owner has named `tinycc` as an acceptable
  candidate, without narrowing the choice to it.
- Each port lives in a directory of its own under its own licence, recorded in
  [`../../LICENSING.md`](../../LICENSING.md) before it is committed.

### What must not be foreclosed

1. **The build assumes a POSIX shell, not Linux.** The `Makefile` and its
   scripts keep to what a POSIX shell provides, so that running them upon
   Oxys-OS finds bugs rather than requiring a rewrite.
2. **System calls take the shapes real programs expect**, not the shape the
   first self-test found convenient.
3. **The filesystem stays writable and correct under load.** A build writes
   many files quickly; [`BUFFER.md`](../storage/BUFFER.md) names the cache as
   the first structure that will want a sleeping lock.
4. **Each self-hosting obstacle is named in the document that owns it**, as a
   limitation, not collected here.
