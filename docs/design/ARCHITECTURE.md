# Oxys-OS System Architecture

**Corresponding phase**: All phases. This document is revised whenever a
subsystem is added or its interface altered.

## 1. Design premises

Oxys-OS is a monolithic operating system for the x86_64 architecture. The
monolithic model was selected in preference to a microkernel because the
project's objectives concern the direct exercise of hardware interfaces, and
because the inter-process communication overhead of a microkernel would obscure
rather than illuminate the mechanisms under study.

The following properties are treated as first-class design constraints rather
than as later additions, in accordance with `PROJECT_GUIDELINES.md`, Section 5:

1. **Symmetric multi-processing.** Every kernel data structure introduced from
   Phase 2 onward is designed on the assumption that it will be accessed
   concurrently by several processors. Locking discipline is recorded in the
   header of the structure's defining file at the time the structure is
   introduced, not retrofitted in Phase 6.
2. **Copy-on-write.** The physical frame allocator introduced in Phase 2
   maintains a per-frame reference count from the outset, because retrofitting
   reference counting to an allocator that lacks it would require the
   modification of every consumer. The fault resolution built upon it is
   complete as of sub-task 2.7, and sub-task 2.8 added the address-space cloning
   that creates the shared pages upon which it acts.
3. **Boot-protocol neutrality.** The kernel proper consumes a boot-protocol
   neutral handoff structure. In Phase 1 that structure is the Multiboot2
   information block read directly; in Phase 12 an equivalent structure is
   populated from the UEFI System Table, and the kernel above the handoff layer
   is unchanged.

## 2. Source tree layout

| Directory | Contents | Introduced |
| --------- | -------- | ---------- |
| `boot/` | The Multiboot2 header, the 32-bit entry point, the long-mode transition, and the GRUB configuration. | Phase 1 |
| `kernel/` | The architecture-independent kernel core: entry, memory management, scheduling, system calls, and the virtual filesystem. | Phase 1 |
| `kernel/include/oxys/` | The kernel's internal header corpus. | Phase 1 |
| `kernel/test/` | The boot-time self-tests, one file per subsystem, and the composed volume they are conducted upon. | Phase 2 |
| `drivers/` | Device drivers, one subdirectory per device class. | Phase 1 |
| `libc/` | The minimal C library linked into user programs. | Phase 7 |
| `userland/` | User programs: the utilities and the shell. | Phase 7 |
| `graphics/` | The framebuffer, the drawing primitives, the font and the compositing surface. | Phase 6 |
| `crypto/` | The random-number generator, the hash function and the symmetric cipher. | Phase 10 |
| `net/` | The network protocol stack. | Phase 11 |
| `uefi/` | The UEFI application entry point and the UEFI handoff path. | Phase 12 |
| `docs/` | The documentation corpus, grouped by subject into `project/`, `design/`, `devices/` and `storage/` and indexed by [`docs/README.md`](../README.md). | Phase 1 |

### 2.1 The grouping of `docs/`

| Directory | Holds |
| --------- | ----- |
| `docs/project/` | How the work is conducted: the plan, the test procedure and record, the toolchain, the coding standards, the bibliography. |
| `docs/design/` | The kernel itself: this document, the boot sequence, the address space, the interrupts. |
| `docs/devices/` | One document per device the kernel drives. |
| `docs/storage/` | The stack from a medium to a caller: the disk, the block layer, the buffer cache. |

The grouping is by subject and not by phase, since a document is amended in every
phase that touches its subject. A directory `README.md` describes its directory's
contents locally; these documents describe the system by subject. The two are
complementary and neither replaces the other.

## 3. Present composition

As of the completion of Phase 5 and of sub-task 6.1, the system comprises
the following translation units.

| Unit | Role |
| ---- | ---- |
| `boot/boot.asm` | The Multiboot2 header; the 32-bit entry point `_start`; CPUID and long-mode feature detection; the construction of the boot-time paging hierarchy; the long-mode transition; the higher-half entry point `KernelEntryHigh`. |
| `kernel/cpu/exceptions.c` | The handlers for the architecture-defined exceptions and the diagnostic report. |
| `kernel/cpu/interrupt_stubs.asm` | The 256 per-vector entry stubs and the common stub that saves the registers and calls the dispatcher. |
| `kernel/cpu/interrupts.c` | The installation of the stubs, the dispatch table and the routing of each vector to its registered handler. |
| `kernel/cpu/gdt.c`, `kernel/cpu/gdt.asm` | The kernel global descriptor table and the reloading of the segment registers. |
| `kernel/cpu/idt.c` | The interrupt descriptor table: its storage, the installation of a gate, the assignment of an interrupt stack table entry to a gate, and the loading of the table. |
| `kernel/cpu/tss.c` | The task state segment: the stacks the processor loads when it needs one it can trust, its descriptor within the global descriptor table, and the loading of the task register. |
| `kernel/cpu/syscall.c` | The configuration of the fast system-call mechanism: `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK` and the enabling bit of `IA32_EFER`. |
| `kernel/cpu/syscall_entry.asm` | The entry point `IA32_LSTAR` names. Provisional in sub-task 6.1; replaced by the dispatch path of sub-task 6.7. |
| `kernel/test/volume.c` | The test fixture: two block devices backed by memory, and an EXT2 volume composed within them. |
| `kernel/test/verify_memory.c` | The self-tests of the frame allocator, the paging hierarchy, the allocators, reference counting, copy-on-write and address spaces. |
| `kernel/test/verify_interrupts.c` | The self-tests of the descriptor table, the stubs and their trap frame, the dispatcher and the exception handlers. |
| `kernel/test/verify_privilege.c` | The self-tests of the descriptors, the task state segment, the interrupt stack table and the system-call configuration. |
| `kernel/test/verify_devices.c` | The self-tests of the interrupt controllers, the interval timer, the keyboard, the serial adapter, the display and the bus. |
| `kernel/test/verify_storage.c` | The self-tests of the disk, the block layer and the buffer cache. |
| `kernel/test/verify_ext2.c` | The self-tests of the EXT2 format, and the report upon a real volume. |
| `kernel/test/verify_vfs.c` | The self-tests of the virtual filesystem layer, and the probe of a real volume through it. |
| `kernel/mm/heap.c` | The kernel heap: a slab allocator of eight size classes over the kernel arena. |
| `kernel/mm/vmm.c` | The kernel virtual address allocator, issuing ranges of the kernel arena backed by frames. |
| `kernel/mm/paging.c` | The permanent kernel paging hierarchy: its construction, activation, software translation and copy-on-write fault resolution. |
| `kernel/mm/addrspace.c` | The address space: its creation, its cloning by the copy-on-write discipline, its activation and its destruction. |
| `kernel/mm/pmm.c` | The physical frame allocator: a bitmap of every 4 KiB frame below the highest usable address. |
| `kernel/fs/ext2.c` | The EXT2 volume: the reading, decoding and validation of its structures, the resolution of a path within it, the reading and writing of a file, and the creation and destruction of the names that reach one. |
| `kernel/fs/vfs.c` | The virtual filesystem layer: the registry of filesystem types, the mount table that joins several volumes into one tree, the node cache that gives one file one identity, the resolution of a path across mount points, and the open file with a position that advances. |
| `kernel/fs/ext2_vfs.c` | The binding of the EXT2 implementation to that layer: the operations vector, the translation between the format's mode and the layer's neutral node type, and the mark a mount leaves upon a volume it has open. |
| `kernel/multiboot2.c` | The Multiboot2 parser, reducing the boot loader's structure to the neutral `BootInformation` description. |
| `kernel/kernel.c` | `KernelMain`, which validates the boot loader handover, initialises every subsystem in the dependency order of Section 4, runs the self-tests, mounts a root volume and enters the echo loop. `KernelPanic`, the unrecoverable-error path. |
| `drivers/vga/vga.c` | The VGA text-mode display driver: the control characters, the scrolling, the colour attributes, the hardware cursor and the erase limit that bounds a backspace. |
| `drivers/serial/serial.c` | The interrupt-driven COM1 serial driver used for diagnostics and input. |
| `drivers/pic/pic.c` | The pair of cascaded 8259A interrupt controllers: their remapping, the masking of request lines, the routing of a request to the driver that claims it, and the end-of-interrupt protocol. |
| `drivers/pit/pit.c` | Counter 0 of the 8253 interval timer: the system tick, the elapsed-time conversion and the bounded wait. |
| `drivers/block/buffer.c` | The buffer cache above the block layer: the hash, the recency list, the reference discipline and the write-back policy. |
| `drivers/block/block.c` | The generic block-device layer: the registry of devices that transfer fixed-size blocks, and the validated path through which every caller above reaches a driver. |
| `drivers/ata/ata.c` | The ATA driver in programmed input/output mode: the reset of a channel, the identification of its devices, and sector transfer by 28-bit and 48-bit addressing. |
| `drivers/pci/pci.c` | The PCI configuration-space enumeration by access mechanism one: the walk of buses, devices and functions, and the searches by which a driver finds its hardware. |
| `drivers/keyboard/keyboard.c` | The 8042 controller and the PS/2 keyboard: initialisation, the decoding of scan code set 1, the modifier state and the circular event buffer. |
| `linker.ld` | The link script establishing the higher-half image layout. |

## 4. Subsystem dependency ordering

The phase ordering of `PLAN.md` is dictated by the following dependencies, which
must not be violated.

```
Phase 1  Bootstrapping
   |
   +--> Phase 2  Memory management ------+
   |         ^                           |
   |         | (page-fault delivery)     | (kernel heap)
   |         |                           v
   +--> Phase 3  Interrupts -------------+--> Phase 4  Device drivers
                                                     |
                                                     v
                                            Phase 5  EXT2 filesystem
                                                     |
                                                     v
                Phase 6  Graphics, system calls, processes, SMP
                                                     |
                                                     v
                            Phase 7  Userland and C library
                                                     |
                                                     v
                                          Phase 8  Shell
                                                     |
                          +--------------------------+--------------+
                          v                          v              v
              Phase 9  Desktop           Phase 10  Crypto   Phase 11  Networking
                          |                          |              |
                          +--------------------------+--------------+
                                                     v
                                       Phase 12  UEFI transition
                                                     |
                                                     v
                                       Phase 13  Polish and hardening
```

Phases 2 and 3 are mutually dependent in one particular: the copy-on-write fault
handler of sub-task 2.7 cannot be exercised until the page-fault vector of
Phase 3 is installed. The dependency is resolved by implementing the memory
management structures of sub-tasks 2.1 to 2.6 first, then Phase 3, and finally
returning to sub-tasks 2.7 and 2.8.

**Status.** Phases 2 to 5 are complete, and Phase 6 has begun. The dependency
described above has been discharged: sub-task 3.4 supplied the fault handler,
sub-task 2.7 the copy-on-write resolution beneath it, and sub-task 2.8 the
address-space cloning that creates the shared pages the resolution acts upon.
Sub-task 3.5 has since remapped the interrupt controllers, so that a device may
be heard; sub-task 3.6 supplied the first device that speaks; and sub-task 3.7
the first that a person operates. Phase 4 supplied the devices beneath a
filesystem and Phase 5 the filesystem itself, which sub-task 5.8 completed by
mounting an EXT2 volume through a virtual filesystem layer. Sub-task 6.1 has
since established the apparatus a privilege transition is performed out of, and
exercised it; work continues at sub-task 6.2, which acquires the linear
framebuffer the boot loader can supply, and thence to sub-task 6.7, whose system
calls are the operations of that layer with a user's arguments copied in.

## 5. Privilege and address-space model

The kernel occupies the upper half of the canonical 48-bit address space and is
mapped into every address space, so that a system call or an interrupt requires
no change of the page-table root. User processes occupy the lower half. The
detailed layout is recorded in `MEMORY-LAYOUT.md`.

The machinery of the transition between the two privilege levels — the user-mode
descriptors and the order the processor's own arithmetic imposes upon them, the
task state segment holding the stacks the processor loads when it needs one it
can trust, and the three registers that configure `SYSCALL` — was established by
sub-task 6.1 and is recorded in `PRIVILEGE.md`. Everything above privilege level
0 remains prospective until sub-task 6.10: the apparatus stands and has been
exercised, but no program yet runs in it.

## 6. Diagnostic policy

Two output paths are maintained from Phase 1 onward. The VGA text console is the
operator-facing path; the COM1 serial port is the machine-readable path, and is
the basis of the automated verification described in `TESTING.md`. Every
diagnostic message of consequence is written to both, so that a failure is
recorded irrespective of which device remains functional.

From sub-task 4.1 the serial path is buffered and carried by interrupt, but it
retains its polled path and reverts to it whenever the interrupt flag is clear.
That is not a fallback for hardware that fails: it is the ordinary path of a
panic, which reports with interrupts disabled and must not be left holding its
message in a buffer that nothing will drain. `docs/devices/SERIAL.md`, Section 4, records
the rule and the single place it is decided.

From sub-task 4.2 the display path is a formal driver equally. It is not the path
the tests read and not the path a panic can most be relied upon to reach; it is
the path a person looking at the machine has, and the property it is built for is
that the machine can verify what it displayed rather than merely that it wrote
something. `docs/devices/DISPLAY.md`, Section 7, records why a display is unusually hard
to test and what is asserted at each boot in consequence.
