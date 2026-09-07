# `docs/design/` — The Kernel Itself

How the machine is brought up, and how it is arranged once it is. These eight
documents describe the parts of the kernel that no device driver may assume the
absence of.

| Document | Subject | Implementation | Phase |
| -------- | ------- | -------------- | ----- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The structure of the system, the source tree file by file, and the subsystem dependency ordering that fixes the order of the phases. | — | All |
| [`BOOT.md`](BOOT.md) | The boot sequence: the Multiboot2 header, the GRUB handover, the entry into long mode, and the transfer to `KernelMain` in the higher half. | [`../../boot/boot.asm`](../../boot/boot.asm) | 1 |
| [`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md) | The physical and virtual address spaces, the permanent paging hierarchy, the frame allocator, the kernel arena, the heap, reference counting and copy-on-write. | [`../../kernel/mm/`](../../kernel/mm/) | 2 |
| [`INTERRUPTS.md`](INTERRUPTS.md) | The interrupt descriptor table, the 256 stubs and the uniform trap frame, the dispatcher, the exception handlers and their dispositions, and the pair of 8259A controllers with their routing and end-of-interrupt protocol. | [`../../kernel/cpu/`](../../kernel/cpu/) | 3 |
| [`PRIVILEGE.md`](PRIVILEGE.md) | The apparatus a privilege transition is performed out of: the user-mode descriptors and the order the processor's arithmetic imposes upon them, the task state segment with its trusted stacks and its interrupt stack table, and the three registers that configure `SYSCALL` — and, from sub-task 6.7, the entry path, the dispatch table and the validation of a caller's arguments. | [`../../kernel/cpu/syscall.c`](../../kernel/cpu/syscall.c), [`../../kernel/cpu/tss.c`](../../kernel/cpu/tss.c) | 6.1, 6.7 |
| [`GRAPHICS.md`](GRAPHICS.md) | The framebuffer: how it is asked for, why its pages are write-combining rather than write-back, the primitives that draw upon it, the font and console that put the boot log back on the screen it displaced, the page a severe fault draws there, the pointer, and the compositor that put a back buffer beneath all of it. | [`../../graphics/`](../../graphics/) | 6.2 to 6.6 |
| [`EXECUTABLE.md`](EXECUTABLE.md) | The ELF64 loader for statically linked executables: what a file is refused for, and how its segments reach an address space. | [`../../kernel/exec/elf.c`](../../kernel/exec/elf.c) | 6.8 |
| [`PROCESS.md`](PROCESS.md) | The process control block, the thread structure and the saved context: what a program is while it runs, what runs within it, the stacks each is given, the switch between them, and the descent to privilege level 3. | [`../../kernel/proc/`](../../kernel/proc/) | 6.9, 6.10 |

They are listed in the order the phases build them, and that is the order to read
them in if you are new to the project: each depends upon the ones before it, and
the dependency is the reason the phases are numbered as they are.
`ARCHITECTURE.md`, Section 4, is where that ordering is set out as a whole.
