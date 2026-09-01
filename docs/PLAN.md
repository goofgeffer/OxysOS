# Oxys-OS Development Plan

**Document status**: Living document. This file is the single source of truth for
task tracking and shall be updated in every session in which a functional change
is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Target architecture**: x86_64.
**Boot protocol**: Multiboot2 (legacy BIOS, GRUB) initially; native UEFI added in Phase 12.
**Kernel model**: Monolithic.

## Current status

This section states the present condition of the work. The chronological account
of how that condition was reached is the Revision History at the foot of this
document.

**Phase 1 is complete but for sub-task 1.12.** The kernel builds without
diagnostics under `-Wall -Wextra -Werror`, is confirmed Multiboot2 compliant by
`grub-file`, and boots under both QEMU and VirtualBox, where it presents its
banner upon the VGA text console and the COM1 serial port alike. Sub-task 1.12,
boot from a physical USB medium, remains open.

**Phase 2 is complete.** The Multiboot2 information structure is parsed into a
boot-protocol-neutral `BootInformation` description; a bitmap allocator governs
every physical frame and reserves those that are not free; a permanent paging
hierarchy maps the kernel text and read-only data without write permission and
the whole of physical memory at `0xFFFF800000000000`; a kernel virtual arena and
a slab heap above it serve allocations of arbitrary size; every frame carries a
reference count and is returned to the allocator only upon the release of its
last reference; the page-fault handler resolves a copy-on-write fault by
duplicating a shared frame, or by restoring write permission where the frame has
but one referrer; and an address space may be created, cloned by the
copy-on-write discipline, activated and destroyed. The substrate upon which
`fork()` is built in sub-task 6.6 is therefore complete.

**Phase 3 is complete.** The interrupt descriptor table is loaded, a stub is
installed for each of the 256 vectors and constructs a uniform
trap frame whatever the vector, a dispatch table routes each vector to a
registered handler that may alter the frame to which control returns, and every
architecture-defined exception has a handler that decodes its error code and
emits a full diagnosis. The pair of cascaded 8259A controllers is remapped to
vectors 32 to 47, clear of the architecture-defined exceptions it would otherwise
be indistinguishable from; every request line begins masked; a routing layer
delivers a request to the driver that claims its line and signals the
end-of-interrupt to both controllers upon its return; and a spurious request is
recognised by the absence of its bit from the in-service register and deliberately
left unacknowledged. Counter 0 of the 8253 interval timer is programmed as a rate
generator at 1000.152 Hz, claims the controller's IR0 line and maintains a
monotonic tick counter, from which elapsed time and a bounded wait are derived;
it is the first device to claim a line, and therefore the first demonstration
that the whole path from a device to its driver is sound. The PS/2 keyboard is
initialised through the 8042 controller, whose translation of scan code set 2
into set 1 is established rather than assumed; scancodes are decoded into key
events carrying the character, the modifiers and the distinction between a
depression and a release, and delivered through a circular buffer of 128 events
whose overrun is counted rather than silent. The interrupt flag may now be set
safely. A minimal kernel global descriptor table was established in the course of
this work, for the reasons given in `docs/INTERRUPTS.md`, Section 5; it is
recorded against sub-task 6.1, which it partly discharges.

Every property above is asserted at each boot by a self-test, no test harness
being available before Phase 7. The procedure is `make verify`, described in
`docs/TESTING.md`.

At the completion of Phase 3 the kernel enters an echo loop rather than halting,
printing every character typed. This is the project's first end-to-end exercise
of a device: a physical keystroke traverses the controller, the interrupt
controller, the handler and the decoder, and emerges as a character.

The display driver implements the backspace, which it had not, and its cursor
movements are asserted at each boot by a self-test of their own.

**Phase 4 is in progress.** Sub-task 4.1 is complete: the serial routine of Phase 1 is
now a formal driver. Its line parameters — signalling rate, word length, parity
and stop bits — are configurable, and a configuration the adapter cannot express
is refused rather than approximated. It claims IR4, buffers both directions, and
services the adapter's interrupt sources until none remains pending. It retains
its polled path, which is what carries a panic: the interrupt flag is clear at
that moment, and a driver that queued the message would halt without transmitting
it. The receive path is exercised end to end by the echo loop, which now drains
the serial line as well as the keyboard.

Sub-task 4.2 is complete: the display routine of Phase 1 is now a formal driver
likewise. Which registers the adapter answers upon is read from the Miscellaneous
Output Register rather than assumed; the hardware cursor may be positioned,
hidden, shaped and read back out of the CRT controller; blinking is disabled
through the attribute controller, so that bit 7 of the attribute brightens the
background and all sixteen colours are available as one; and the frame buffer may
be read back, which is what allows the machine to assert what it displayed.

The backspace now crosses from one row into the row above and stops immediately
after the text standing there, having consumed the separator between the rows and
nothing else. That movement was previously refused because the driver
could not distinguish a line of input from the boot log and would have consumed
the log a character at a time. The remedy was to supply the missing knowledge
rather than to withhold the movement: the driver holds an **erase limit**, a
position before which no backspace may retreat, and whoever reads the input sets
it where the input begins. The serial terminal is told of the same movement by
the ECMA-48 sequences CUU and CHA, a backspace not expressing it.

Sub-task 4.3 is complete: the PCI configuration space is enumerated by access
mechanism one. Every device driven before it was found by knowing where it is,
those addresses being inherited from the IBM Personal Computer; no device
introduced afterwards may be assumed in the same way, which is why the
enumeration precedes the disk driver rather than following it. Buses are reached
through the host bridge and through each PCI-to-PCI bridge found, rather than
swept; the queue is explicit rather than the call stack and each bus is visited
once. Nothing is claimed or configured — the enumeration establishes only what
the machine contains and where each part of it answers.

Sub-task 4.4 is complete: an ATA driver in programmed input/output mode resets
both channels, identifies the four devices they may carry, and reads and writes
sectors by 28-bit and 48-bit addressing. It is the first driver whose failures
are silent in the ordinary case — a driver that reads the wrong sector returns
data, and data that arrived cannot be distinguished from data that is correct
until something interprets it — and it is the first that can destroy something.
The self-test therefore asserts the relationship between sectors rather than
merely that a read succeeded, and it writes only when the operator has booted the
GRUB entry that permits it, restoring the sector afterwards.

**The next work is sub-task 4.5**, the generic block-device abstraction layer.

## Legend

| Marker | Meaning |
| ------ | ------- |
| `[ ]`  | Not commenced. |
| `[~]`  | In progress. |
| `[x]`  | Completed and verified by test. |

---

## Phase 1 — Bootstrapping and Early Output

**Objective**: Produce a bootable ISO image containing a Multiboot2-compliant
ELF64 kernel that enters long mode, relocates to the higher half of the virtual
address space, and emits identifying output.

**Specifications**: Multiboot2 Specification 2.0; Intel SDM Volume 3A,
Chapters 2, 4 and 9; System V ABI for AMD64.

- [x] 1.1 Verify the cross-compilation toolchain (`x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`, `xorriso`) is present and functional.
- [x] 1.2 Author the linker script `linker.ld` defining a higher-half kernel at virtual base `0xFFFFFFFF80000000` with a physical load address of `0x00100000`.
- [x] 1.3 Author `boot/boot.asm`: Multiboot2 header, 32-bit protected-mode entry point, Multiboot2 magic validation, CPUID and long-mode feature detection.
- [x] 1.4 Construct the boot-time paging hierarchy (PML4, two PDPTs, one PD) providing a 1 GiB identity map and a coincident 1 GiB higher-half map using 2 MiB pages.
- [x] 1.5 Perform the long-mode transition (CR4.PAE, IA32_EFER.LME, CR0.PG) and load a 64-bit GDT.
- [x] 1.6 Transfer control to the higher-half 64-bit kernel entry point and establish the kernel stack.
- [x] 1.7 Implement a minimal VGA text-mode output routine and a minimal COM1 serial output routine for early diagnostics.
- [x] 1.8 Implement `KernelMain`, which clears the screen and prints the string "Oxys-OS".
- [x] 1.9 Author the `Makefile` with the targets `all`, `clean`, `iso`, `run-qemu`, `run-vbox` and `run-uefi`.
- [x] 1.10 Generate the ISO image and verify boot under QEMU.
- [x] 1.11 Verify boot under VirtualBox.
- [ ] 1.12 Verify boot on physical hardware from a USB medium.

---

## Phase 2 — Memory Management (including Copy-on-Write)

**Objective**: Establish complete control of physical and virtual memory, and
provide the copy-on-write primitives upon which `fork()` will later depend.

**Specifications**: Intel SDM Volume 3A, Chapter 4 (Paging) and Section 6.15
(Page-Fault Exception); Multiboot2 Specification, Section 3.6 (memory map tag).

- [x] 2.1 Parse the Multiboot2 information structure and extract the memory map (tag type 6) and the ELF section headers (tag type 9).
- [x] 2.2 Implement a physical frame allocator (bitmap) covering all usable regions, reserving the kernel image, the Multiboot2 structures and the low 1 MiB.
- [x] 2.3 Construct a permanent kernel page-table hierarchy, replacing the boot-time tables and removing the low identity map.
- [x] 2.4 Implement a direct physical map region for kernel access to arbitrary frames.
- [x] 2.5 Implement a kernel virtual-address-space allocator and a general-purpose kernel heap (slab allocator over a buddy-style page allocator).
- [x] 2.6 Implement per-frame reference counting as the substrate for shared pages.
- [x] 2.7 Implement the page-fault handler dispatch path (dependent upon Phase 3) and the copy-on-write fault resolution routine.
- [x] 2.8 Implement address-space cloning that marks writable user pages read-only and increments frame reference counts.

---

## Phase 3 — Interrupts, Exceptions and Keyboard Input

**Objective**: Install a complete interrupt descriptor table, service processor
exceptions, and accept keyboard input.

**Specifications**: Intel SDM Volume 3A, Chapter 6; Intel 8259A datasheet;
IBM PS/2 controller documentation.

- [x] 3.1 Define the IDT and the 64-bit interrupt-gate descriptor format; load it with `lidt`.
- [x] 3.2 Author assembly stubs for vectors 0–255, normalising the presence or absence of a processor-pushed error code.
- [x] 3.3 Implement a C interrupt dispatcher operating on a formal trap frame structure.
- [x] 3.4 Implement exception handlers with register and stack diagnostics emitted over the serial port.
- [x] 3.5 Remap the 8259A PIC to vectors 32–47 and implement end-of-interrupt signalling.
- [x] 3.6 Implement the Programmable Interval Timer as the initial timer source.
- [x] 3.7 Implement the PS/2 keyboard driver: controller initialisation, scancode set 1 translation, modifier state and a circular input buffer.

---

## Phase 4 — Basic Device Drivers

**Objective**: Provide the device support required by the filesystem and by
subsequent user-facing subsystems.

**Specifications**: PC16550D UART datasheet; ATA/ATAPI Command Set (ACS-3);
PCI Local Bus Specification 3.0.

- [x] 4.1 Promote the early serial routine to a formal, interrupt-driven COM1 driver with configurable line parameters.
- [x] 4.2 Promote the early VGA routine to a formal text-mode driver with scrolling, cursor control and colour attributes.
- [x] 4.3 Implement PCI configuration-space enumeration by the legacy I/O port mechanism, with device and class identification.
- [x] 4.4 Implement an ATA PIO driver: bus reset, `IDENTIFY DEVICE`, 28-bit and 48-bit LBA sector read and write.
- [ ] 4.5 Define a generic block-device abstraction layer above the ATA driver.
- [ ] 4.6 Implement a buffer cache for block devices.

---

## Phase 5 — EXT2 Filesystem

**Objective**: Mount, read and write an EXT2 volume.

**Specifications**: The Second Extended File System (Poirier); Linux kernel
documentation, `Documentation/filesystems/ext2.rst`.

- [ ] 5.1 Parse the superblock and validate the EXT2 magic number and revision level.
- [ ] 5.2 Parse the block-group descriptor table.
- [ ] 5.3 Implement inode retrieval and the resolution of direct, singly, doubly and triply indirect block pointers.
- [ ] 5.4 Implement directory-entry traversal and absolute path resolution.
- [ ] 5.5 Implement file reading.
- [ ] 5.6 Implement block and inode allocation, file writing, extension and truncation.
- [ ] 5.7 Implement directory creation and entry insertion and removal.
- [ ] 5.8 Define a virtual filesystem layer and mount an EXT2 root volume.

---

## Phase 6 — System Calls, Process Management and Symmetric Multi-Processing

**Objective**: Execute user-mode programs, schedule them across multiple
processors, and expose kernel services by system call.

**Specifications**: Intel SDM Volume 3A, Chapters 5, 8 and 11, and Volume 2B
(`SYSCALL`/`SYSRET`); System V ABI for AMD64; Intel MultiProcessor
Specification 1.4; ACPI Specification 6.5 (MADT).

- [~] 6.1 Install the GDT and TSS required for privilege transition; configure IA32_STAR, IA32_LSTAR and IA32_FMASK. *(The kernel GDT and its null, code and data descriptors were established early, in Phase 3; what remains is the user-mode descriptors, the task state segment and the system-call MSRs.)*
- [ ] 6.2 Implement the `SYSCALL` entry path, the system-call dispatch table and argument validation.
- [ ] 6.3 Implement the ELF64 loader for statically linked executables.
- [ ] 6.4 Define the process control block, the address-space descriptor and the thread structure.
- [ ] 6.5 Implement context switching and the initial transition to user mode via `IRETQ`.
- [ ] 6.6 Implement `fork()` upon the Phase 2 copy-on-write substrate, together with `execve()`, `exit()` and `wait()`.
- [ ] 6.7 Parse the ACPI MADT; initialise the Local APIC and the I/O APIC; retire the 8259A PIC.
- [ ] 6.8 Implement application-processor bring-up by INIT-SIPI-SIPI and a real-mode trampoline.
- [ ] 6.9 Implement spinlocks, per-CPU data areas and inter-processor interrupts, including TLB shootdown.
- [ ] 6.10 Implement a multiprocessor-aware round-robin scheduler with per-CPU run queues and processor affinity.

---

## Phase 7 — Userland and Minimal C Library

**Objective**: Provide the runtime environment in which user programs execute.

**Specifications**: ISO/IEC 9899:2011; System V ABI for AMD64.

- [ ] 7.1 Implement the freestanding string and memory functions (`<string.h>`).
- [ ] 7.2 Implement system-call wrappers for the complete kernel interface.
- [ ] 7.3 Implement a user-space heap allocator (`malloc`, `free`, `realloc`) above `brk`/`mmap`.
- [ ] 7.4 Implement buffered input and output (`<stdio.h>`) and formatted conversion.
- [ ] 7.5 Author the C runtime startup object (`crt0`) and the static-linking procedure for user programs.
- [ ] 7.6 Implement the utilities `ls`, `cat`, `echo`, `mkdir` and `rm`.
- [ ] 7.7 Construct an initial ramdisk containing the utilities and mount it as the early root.

---

## Phase 8 — Shell

**Objective**: Provide an interactive command interpreter.

- [ ] 8.1 Implement line editing with history.
- [ ] 8.2 Implement the tokeniser and the command parser.
- [ ] 8.3 Implement built-in commands (`cd`, `exit`, `export`, `pwd`).
- [ ] 8.4 Implement external program execution by `fork()` and `execve()`.
- [ ] 8.5 Implement input and output redirection.
- [ ] 8.6 Implement pipelines.
- [ ] 8.7 Implement job control, process groups and terminal signal delivery.

---

## Phase 9 — Graphical User Interface

**Objective**: Provide a graphical display and a window system.

**Specifications**: VESA BIOS Extensions 3.0; UEFI Specification 2.10,
Section 12.9 (Graphics Output Protocol); Multiboot2 Specification, Section 3.6.12
(framebuffer information tag).

- [ ] 9.1 Request a linear framebuffer by the Multiboot2 framebuffer tag and map it into kernel space.
- [ ] 9.2 Implement 2D primitives: pixel, line, rectangle, blit and clipping.
- [ ] 9.3 Implement a bitmap font renderer.
- [ ] 9.4 Implement a PS/2 mouse driver and a cursor.
- [ ] 9.5 Implement a compositing surface abstraction and double buffering.
- [ ] 9.6 Implement a stacking window manager with focus and event routing.
- [ ] 9.7 Implement the client protocol by which user processes create and draw windows.
- [ ] 9.8 Implement demonstration applications: a terminal emulator window and a clock.

---

## Phase 10 — Cryptography

**Objective**: Provide primitives for random-number generation, hashing and
symmetric encryption.

**Specifications**: FIPS 180-4 (SHA-256); FIPS 197 (AES); NIST SP 800-38A
(modes of operation); NIST SP 800-90A (deterministic random bit generators);
Intel SDM Volume 2B (`RDRAND`, `RDSEED`).

- [ ] 10.1 Implement an entropy pool seeded from `RDSEED`/`RDRAND` where available and from timer jitter otherwise.
- [ ] 10.2 Implement a cryptographically secure deterministic random bit generator.
- [ ] 10.3 Implement SHA-256 with the FIPS 180-4 test vectors.
- [ ] 10.4 Implement AES-128 and AES-256 with the FIPS 197 test vectors.
- [ ] 10.5 Implement CBC and CTR modes of operation.
- [ ] 10.6 Expose the primitives to user space by system call and by a `/dev/random` device node.

---

## Phase 11 — Networking

**Objective**: Provide a functional TCP/IP stack.

**Specifications**: IEEE 802.3; RFC 826 (ARP); RFC 791 (IP); RFC 792 (ICMP);
RFC 768 (UDP); RFC 9293 (TCP); RFC 2131 (DHCP); Realtek RTL8139 or Intel 8254x
datasheet.

- [ ] 11.1 Implement an Ethernet controller driver (RTL8139 or Intel E1000) with descriptor rings and interrupt handling.
- [ ] 11.2 Define the network buffer structure and the protocol layering framework.
- [ ] 11.3 Implement Ethernet frame transmission and reception.
- [ ] 11.4 Implement ARP with a resolution cache.
- [ ] 11.5 Implement IPv4, including fragmentation and reassembly, and a routing table.
- [ ] 11.6 Implement ICMP echo request and reply.
- [ ] 11.7 Implement UDP.
- [ ] 11.8 Implement TCP: the state machine, sequence-number handling, retransmission and flow control.
- [ ] 11.9 Implement the BSD-style socket system-call interface.
- [ ] 11.10 Implement DHCP client configuration and the `ping` utility.

---

## Phase 12 — UEFI Transition

**Objective**: Boot the identical kernel by native UEFI as well as by legacy
BIOS.

**Specifications**: UEFI Specification 2.10; Microsoft PE/COFF Specification;
ACPI Specification 6.5.

- [ ] 12.1 Establish a PE32+ build path for a UEFI application image.
- [ ] 12.2 Implement the UEFI entry point and parse the System Table.
- [ ] 12.3 Retrieve the memory map, the ACPI RSDP and the Graphics Output Protocol framebuffer by Boot Services.
- [ ] 12.4 Define a boot-protocol-neutral handoff structure consumed by the kernel, populated identically from Multiboot2 or from UEFI.
- [ ] 12.5 Invoke `ExitBootServices` and transfer control to the kernel.
- [ ] 12.6 Integrate UEFI Runtime Services: time and variable access.
- [ ] 12.7 Produce a hybrid ISO image bootable by both BIOS and UEFI, and verify under OVMF.

---

## Phase 13 — Polish, Optimisation and Final Hardening

**Objective**: Prepare the system for release.

**Specifications**: Intel SDM Volume 3A, Chapters 4 and 5 (SMEP, SMAP, NX).

- [ ] 13.1 Profile interrupt latency, context-switch cost and filesystem throughput.
- [ ] 13.2 Optimise the scheduler for fairness and the block layer for read-ahead.
- [ ] 13.3 Enable NX, SMEP and SMAP; enforce write-exclusive-or-execute in kernel mappings.
- [ ] 13.4 Implement kernel stack guard pages and stack-canary protection.
- [ ] 13.5 Implement kernel address-space layout randomisation, if feasible.
- [ ] 13.6 Complete and review the whole of the `docs/` corpus.
- [ ] 13.7 Test on a minimum of three distinct physical machines, including UEFI systems.
- [ ] 13.8 Extend the userland utility set and produce the final release image.

---

## Revision History

The rows are ordered with the most recent first.

| Date | Phase affected | Summary |
| ---- | -------------- | ------- |
| 2026-08-31 | Phase 4 | Sub-task 4.1 completed. The serial routine of Phase 1 is now an interrupt-driven driver claiming IR4. The line parameters are computed from a signalling rate rather than stated as a divisor, and the rate realised is reported beside the rate requested, the division being truncating. The driver keeps both modes and needs both: it begins polled, because it serves the diagnostics of an initialisation that precedes the interrupt controller, and it falls back upon polling wherever the interrupt flag is clear, which includes every panic — a driver that assumed interrupts were available would queue a panic message and halt without transmitting it. The transmitter interrupt is enabled only while characters are waiting, the condition it reports being a level and not an event; an adapter with nothing to send holds it asserted permanently, and a driver that left it enabled would be seized by an interrupt no service could dismiss. A writer that fills the transmit buffer waits for room, a diagnostic channel that discards its output being worse than a slow one, while a receiver that fills the receive buffer discards the newest character and counts it, having no one to wait for. `ReadRflags` was added to `kernel/include/oxys/cpu.h`, which had named it in its header without providing it. `docs/SERIAL.md` added and records the design. |
| 2026-09-01 | Phase 4 | Sub-task 4.2 completed. The display routine of Phase 1 is now a formal driver. The register configuration is read from the Miscellaneous Output Register rather than assumed, the cost of one input instruction being far below the cost of writing the cursor location to an address nothing decodes. The hardware cursor may be positioned, hidden, shaped and read back; the shape is left as the firmware established it, that shape being the one known to be legible upon the machine's own display. Blinking is disabled through the attribute controller, whose shared address and data port is reached through a flip-flop and whose every write is therefore read back, a write that arrived in the wrong state having altered some other register with no symptom the machine could detect. The backspace now crosses into the row above and stops upon the last character standing there, bounded by an erase limit that records where the input began; the objection recorded against this movement in sub-task 4.1 — that the driver could not tell a line of input from the boot log — was answered by supplying that knowledge rather than by withholding the movement. `KernelEchoBackspace` tells a serial terminal of the same movement by ECMA-48 CUU and CHA. `docs/DISPLAY.md` added and records the design. |
| 2026-09-01 | Phase 4 | The backspace across a row boundary corrected. It stopped upon the last character of the row above, so that the erasing sequence the callers compose consumed both the separator between the rows and that character: one keystroke deleted two things. It now stops immediately after the text, consuming the separator alone, exactly as a backspace within a row consumes one character alone. A row that is entirely occupied remains the exception, having ended by wrapping rather than by a line feed and having no separator to consume. |
| 2026-09-01 | Phase 4 | Sub-task 4.3 completed. The PCI configuration space is enumerated by access mechanism one. The walk reaches buses through bridges rather than sweeping all 256, holds the queue of buses awaiting a scan explicitly rather than in the call stack, and records each bus as visited so that malformed hardware cannot send it around a cycle. A function that is not there returns all ones rather than failing, which is how absence is detected and also why the self-test asserts that particular devices were found: an enumerator with its address arithmetic wrong reports an empty machine, and an empty report is exactly what a machine with no devices produces. `docs/PCI.md` added and records the design. |
| 2026-09-01 | Phase 4 | Sub-task 4.4 completed. An ATA driver in programmed input/output mode. Both addressing modes are implemented: the 28-bit form keeps four bits of the address in the register that selects the device, and the 48-bit form writes each register twice, high-order byte first, the device retaining the previous content in a hidden half. A count register of zero means the greatest count the mode allows, so the limits are 256 and 65536 sectors rather than 255 and 65535. A write is followed by a cache flush within the same sequence, a device that has accepted data without committing it reporting success and losing it, the loss appearing only upon a later read. The driver's only clock is the read of an I/O port: the interval timer counts by interrupt and the interrupt flag is clear throughout initialisation. This is the first driver that can destroy something, so the self-test reads unconditionally and writes only upon an option given at the GRUB menu, restoring the sector it wrote and verifying the restoration. `docs/DISK.md` added and records the design. |
| 2026-08-31 | Phase 3 | The backspace key repaired. `VgaPutCharacter` had no case for it, so the character fell through to the default and was written into the frame buffer as whatever glyph the adapter's font holds at code point 0x08, the cursor then advancing rightward; the key appeared to do nothing useful. The driver now implements the backspace as ANSI X3.4-1986 defines it, a movement one column to the left that erases nothing, and it stops in the first column rather than wrapping, because nothing records whether a row ended by wrapping or by a line feed and a wrap would therefore let a backspace destroy output the user never typed. The erasure belongs to the caller: `KernelEchoLoop` writes `"\b \b"`, which erases upon a serial terminal equally, the serial driver having transmitted the raw character to no visible effect. A display self-test was added, the driver having had none, with a `VgaCursorPosition` accessor for it to read; the failure mode it guards is silent to the machine and visible only to a person reading the screen, which is how this defect survived. |
| 2026-08-31 | Phase 3 | Sub-task 3.7 completed, and Phase 3 with it. The 8042 controller is initialised, its configuration byte written a second time after the controller self-test because that test resets the controller upon some implementations. The translation of scan code set 2 into set 1 is set explicitly rather than assumed: a PS/2 keyboard powers up in set 2, and a driver that assumed set 1 would work upon most machines and elsewhere deliver plausible characters that were simply the wrong ones. Scancodes are decoded into key events, retaining releases and the codes of keys that produce no character, since a later window system needs both. Capitals lock is a latch toggled upon depression alone, and combines with shift as an exclusive disjunction for letters while leaving every other key alone. The buffer's indices are free-running and masked, so that their difference is the occupancy directly; an overrun discards the newest event and counts it. Every wait upon the controller is bounded, so that a machine without one proceeds rather than hanging. The kernel now enters an echo loop in place of halting, which exercises the interrupt path end to end; keystrokes driven from the QEMU monitor were echoed correctly. `docs/KEYBOARD.md` added and records the design. |
| 2026-08-31 | Phase 3 | Sub-task 3.6 completed. Counter 0 of the 8253 interval timer is programmed as a rate generator, mode 2 being preferred to mode 3 because the square wave mode decrements by two and so admits only even divisors, and nothing here has any interest in the shape of the waveform. A divisor of 1193 realises 1000.152 Hz against the 1000 Hz requested, and elapsed time is converted by the frequency realised rather than the one requested, an error of a known size that does not announce itself being worse than a coarse clock. The divisor is confirmed from within the machine by latching the counter and asserting that no reading exceeds it, there being no second clock to check the first against. The bounded wait exists so that a timer which never fires reports itself instead of hanging. `docs/TIME.md` added and records the design. |
| 2026-08-31 | Phase 3 | Sub-task 3.5 completed. The cascaded 8259A pair is remapped to vectors 32 to 47, the vectors the firmware leaves them presenting having collided exactly with the architecture-defined exceptions, so that a timer tick was indistinguishable from a double fault. ICW1 clears the interrupt mask register, so every line is masked immediately after the sequence rather than before it. A routing layer owns the end-of-interrupt, signalling both controllers for a slave line, because the protocol belongs to the controller and the cost of a driver forgetting it is the permanent silencing of every lower-priority line. A spurious request upon IR7 or IR15 is recognised by the absence of its bit from the in-service register and left unacknowledged. The remapping is established by setting the interrupt flag with every line masked: had it failed, the running interval timer would have delivered a double fault at once. `docs/INTERRUPTS.md`, Section 9, records the design. |
| 2026-08-31 | Phase 2 | Sub-task 2.8 completed, and Phase 2 with it. An address space may be created, cloned, activated and destroyed. A clone duplicates the paging structures of the lower half and shares the frames they map, withdrawing write permission and setting the copy-on-write flag in both hierarchies and recording a reference for the new holder; the higher half is shared with the kernel by copying the root entries, so the kernel is mapped identically in every address space. Paging now distinguishes the active hierarchy from the kernel's, a walk performed in software being obliged to follow the one the processor follows. `docs/MEMORY-LAYOUT.md`, Section 14, records the design. |
| 2026-08-31 | — | This document reviewed and corrected. The status section had retained the claim that sub-tasks 2.7 and 2.8 could not proceed, which the completion of Phase 3 had discharged, and had accumulated as a chronological narrative duplicating this table; it is now a statement of present condition alone. Sub-task 1.11 is recorded as passed upon the project owner's own testing, and `docs/TESTING.md` amended to agree. The rows of this table were placed in order. |
| 2026-08-30 | Phase 2 | Sub-task 2.7 completed. Copy-on-write fault resolution, using bit 9 of the page-table entry, which Intel SDM Table 4-19 records as ignored by the processor. A shared frame is duplicated through the direct map and one reference released; a frame with a single referrer is made writable without a copy. The page-fault handler attempts resolution before reporting. |
| 2026-08-30 | Phase 3 | Sub-task 3.4 completed. Handlers for all thirty-two architecture-defined exceptions, with decoding of both error-code formats, a full register and control-register dump, and a bounded stack reproduction. `CR0.WP` is now set in `PagingInitialise`, without which the read-only kernel mappings were advisory only. The negative paging test deferred from sub-task 2.3 was performed and passed. |
| 2026-08-30 | Phase 3 | Sub-task 3.3 completed. A dispatch table of 256 entries with a registration interface. An unregistered architecture-defined exception remains fatal until sub-task 3.4; an unregistered vector above that range is counted and ignored, which is the correct treatment of a spurious interrupt. A default breakpoint handler is registered, the breakpoint being a trap and therefore safe to resume from. |
| 2026-08-30 | Phase 3 | Sub-task 3.2 completed. A stub for each of the 256 vectors, normalising the vector number and the presence of a processor error code into a uniform trap frame. A minimal kernel global descriptor table was established in the same work: the table loaded by `boot/boot.asm` lay at an address unmapped by sub-task 2.3, and interrupt delivery faulted upon reading it. `docs/INTERRUPTS.md` added. |
| 2026-08-30 | Phase 3 | Sub-task 3.1 completed. The interrupt descriptor table and the 64-bit gate descriptor are defined and the table loaded with `LIDT`. Every gate is initially absent. Verified by reading the table register back with `SIDT`. |
| 2026-08-30 | — | The project guidelines were consolidated under `PROJECT_GUIDELINES.md` and the text adapted to that name: the framing addressed to an assistant is now addressed to a contributor, and the document now describes itself as guidelines. The root directory recorded in Sections 1 and 9 was corrected to `~/oxys-os`. The documentation index was relocated from `docs/` to the repository root as `README.md`, becoming the front page of the repository, with every relative link corrected. Every reference to the guidelines document was updated across the source, the documentation and the agent definitions. |
| 2026-08-30 | Phase 2 | Sub-task 2.6 completed. Per-frame reference counting is established over a 255 KiB table allocated from the kernel heap. `FrameFree` releases one reference and returns a frame to the allocator only upon the last. Sub-tasks 2.7 and 2.8 are deferred until Phase 3 provides a page-fault handler. |
| 2026-08-30 | Phase 2 | Sub-task 2.5 completed. The kernel virtual address allocator issues ranges of the 32 TiB arena, backing each page with a frame; the kernel heap is a slab allocator over it, with eight size classes and a whole-page path for larger requests. Both pass a boot-time self-test. |
| 2026-08-30 | Phase 2 | Sub-task 2.4 completed. The direct physical map covers the whole of usable physical memory at `0xFFFF800000000000` with 2 MiB pages. Paging structures are no longer confined to the first gibibyte. |
| 2026-08-30 | Phase 2 | Sub-task 2.3 completed. The permanent kernel paging hierarchy is constructed from four allocated frames and activated; the kernel text and read-only data are mapped read-only; the low identity mapping is removed. Verified by a boot-time self-test that walks the hierarchy in software. |
| 2026-08-30 | Phase 2 | Sub-task 2.2 completed. A bitmap physical frame allocator, with a boot-time self-test. 131039 frames governed under QEMU with 512 MiB, of which 288 are reserved: 256 for the low mebibyte, 27 for the kernel image, 4 for the bitmap and 1 for the boot information structure. |
| 2026-08-30 | Phase 2 | Sub-task 2.1 completed. The Multiboot2 information structure is parsed into the neutral `BootInformation` description: the memory map, the ELF section count, the boot loader name and the command line. The kernel and boot information extents are recorded for the frame allocator to reserve. |
| 2026-08-30 | Phase 1 | `PROJECT_GUIDELINES.md` amended at the project owner's request by the addition of Section 10, requiring directory-level documentation. `boot/README.md`, `kernel/README.md` and `drivers/README.md` created accordingly, and `docs/README.md`, since relocated to the repository root, extended to index them. |
| 2026-08-30 | Phase 1 | Project initialised. Directory structure, documentation corpus, boot code, kernel entry, VGA and serial output, build system and ISO generation completed. Boot verified under QEMU. |
