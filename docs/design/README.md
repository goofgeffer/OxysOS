# `docs/design/` — The Kernel Itself

How the machine is brought up, and how it is arranged once it is. These five
documents describe the parts of the kernel that no device driver may assume the
absence of.

| Document | Subject | Phase |
| -------- | ------- | ----- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The structure of the system, the source tree file by file, and the subsystem dependency ordering that fixes the order of the phases. | All |
| [`BOOT.md`](BOOT.md) | The boot sequence: the Multiboot2 header, the GRUB handover, the entry into long mode, and the transfer to `KernelMain` in the higher half. | 1 |
| [`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md) | The physical and virtual address spaces, the permanent paging hierarchy, the frame allocator, the kernel arena, the heap, reference counting and copy-on-write. | 2 |
| [`INTERRUPTS.md`](INTERRUPTS.md) | The interrupt descriptor table, the 256 stubs and the uniform trap frame, the dispatcher, the exception handlers, and the pair of 8259A controllers with their routing and end-of-interrupt protocol. | 3 |
| [`PRIVILEGE.md`](PRIVILEGE.md) | The apparatus a privilege transition is performed out of: the user-mode descriptors and the order the processor's arithmetic imposes upon them, the task state segment with its trusted stacks and its interrupt stack table, and the three registers that configure `SYSCALL`. | 6 |

Read them in that order if you are new to the project: each depends upon the one
before it, and the dependency is the reason the phases are numbered as they are.
