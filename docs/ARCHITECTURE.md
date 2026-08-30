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
   modification of every consumer.
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
| `drivers/` | Device drivers, one subdirectory per device class. | Phase 1 |
| `libc/` | The minimal C library linked into user programs. | Phase 7 |
| `userland/` | User programs: the utilities and the shell. | Phase 7 |
| `graphics/` | The framebuffer, the drawing primitives and the window manager. | Phase 9 |
| `crypto/` | The random-number generator, the hash function and the symmetric cipher. | Phase 10 |
| `net/` | The network protocol stack. | Phase 11 |
| `uefi/` | The UEFI application entry point and the UEFI handoff path. | Phase 12 |
| `docs/` | The documentation corpus indexed by the repository `README.md`. | Phase 1 |

## 3. Present composition

As of the completion of Phase 1 the system comprises the following translation
units.

| Unit | Role |
| ---- | ---- |
| `boot/boot.asm` | The Multiboot2 header; the 32-bit entry point `_start`; CPUID and long-mode feature detection; the construction of the boot-time paging hierarchy; the long-mode transition; the higher-half entry point `KernelEntryHigh`. |
| `kernel/mm/heap.c` | The kernel heap: a slab allocator of eight size classes over the kernel arena. |
| `kernel/mm/vmm.c` | The kernel virtual address allocator, issuing ranges of the kernel arena backed by frames. |
| `kernel/mm/paging.c` | The permanent kernel paging hierarchy: its construction, activation and software translation. |
| `kernel/mm/pmm.c` | The physical frame allocator: a bitmap of every 4 KiB frame below the highest usable address. |
| `kernel/multiboot2.c` | The Multiboot2 parser, reducing the boot loader's structure to the neutral `BootInformation` description. |
| `kernel/kernel.c` | `KernelMain`, which validates the boot loader handover, initialises the early output devices, presents the identification banner and halts. `KernelPanic`, the unrecoverable-error path. |
| `drivers/vga/vga.c` | The VGA colour text-mode output driver. |
| `drivers/serial/serial.c` | The polled COM1 serial output driver used for diagnostics. |
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
                    Phase 6  System calls, processes, SMP
                                                     |
                                                     v
                            Phase 7  Userland and C library
                                                     |
                                                     v
                                          Phase 8  Shell
                                                     |
                          +--------------------------+--------------+
                          v                          v              v
                  Phase 9  GUI            Phase 10  Crypto   Phase 11  Networking
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

**Status.** Sub-tasks 2.1 to 2.6 are complete. Work has therefore moved to
Phase 3, and sub-tasks 2.7 and 2.8 will be taken up once sub-task 3.4 provides a
fault handler capable of reporting, and then of resolving, a page fault.

## 5. Privilege and address-space model

The kernel occupies the upper half of the canonical 48-bit address space and is
mapped into every address space, so that a system call or an interrupt requires
no change of the page-table root. User processes occupy the lower half. The
detailed layout is recorded in `MEMORY-LAYOUT.md`.

## 6. Diagnostic policy

Two output paths are maintained from Phase 1 onward. The VGA text console is the
operator-facing path; the COM1 serial port is the machine-readable path, and is
the basis of the automated verification described in `TESTING.md`. Every
diagnostic message of consequence is written to both, so that a failure is
recorded irrespective of which device remains functional.
