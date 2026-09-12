<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `kernel/arch/` — What Could Not Survive a Change of Processor

**Phase**: none of its own. Every file here was written in the phase its subject
belongs to and moved here afterwards; the move is recorded in
[`../../docs/design/ARCHITECTURE.md`](../../docs/design/ARCHITECTURE.md),
Section 2.4.
**Detailed design**: the design documents of the subsystems themselves —
[`INTERRUPTS.md`](../../docs/design/INTERRUPTS.md),
[`PRIVILEGE.md`](../../docs/design/PRIVILEGE.md),
[`MEMORY-LAYOUT.md`](../../docs/design/MEMORY-LAYOUT.md),
[`CONCURRENCY.md`](../../docs/design/CONCURRENCY.md),
[`SMP.md`](../../docs/design/SMP.md) and
[`PROCESS.md`](../../docs/design/PROCESS.md). None of them moved, because none of
them is about a directory.

## Purpose

This directory holds the kernel code that is tied to the processor it runs upon.
`x86_64/` is its one member and, on the roadmap of
[`../../docs/project/PLAN.md`](../../docs/project/PLAN.md), will remain so:
thirteen phases and not one of them is a port. **This directory is therefore not
preparing for a second architecture, and claiming otherwise would be the kind of
speculative structure `PROJECT_GUIDELINES.md`, Section 8, exists to discourage.**

What it is for is that the boundary is real whether or not it is ever crossed.
Some of this kernel is a statement about algorithms — a bitmap of free frames, a
first-fit free list, a round-robin queue — and some of it is a statement about
one manufacturer's manual: that a task state segment is 104 bytes, that `SYSRET`
returns to the selector `IA32_STAR` implies, that a translation is discarded by
`INVLPG`. The second kind is where a hardware specification is the authority and
where `PROJECT_GUIDELINES.md`, Section 2, requires a citation for every assertion
made. Having it in one subtree means that question — *is this file answerable to
a manual?* — has an answer you can see in a path.

## The test for admitting a file

A file belongs here when **a correct port of it to another processor would share
no lines with it**. Not when it merely mentions the architecture: almost every
file in this kernel cites the Intel manual somewhere, because that is where the
page size and the memory model are defined.

Applied to the two subsystems that are now split across both trees:

| Stays in `kernel/` | Comes here | Why |
| ------------------ | ---------- | --- |
| `mm/pmm.c` | — | A bitmap of frames and a per-frame reference count. The page size is a constant it is given. |
| `mm/vmm.c`, `mm/heap.c` | — | An address-range allocator and a first-fit heap. Neither reads a paging structure. |
| — | `arch/x86_64/mm/paging.c` | Four levels, nine bits each, and the entry flags of Intel SDM Volume 3A, Table 4-15. Nothing of it survives a different hierarchy. |
| — | `arch/x86_64/mm/addrspace.c` | Clones that hierarchy and reloads `CR3`. |
| — | `arch/x86_64/mm/shootdown.c` | `INVLPG`, an inter-processor interrupt, and an end-of-interrupt owed to the Local APIC. |
| `proc/process.c`, `proc/sched.c` | — | The process table, the run queues, the affinity mask and the quantum: policy, and portable. |
| — | `arch/x86_64/proc/switch.asm` | Six named registers, a stack pointer, and `IRETQ`. |

## The six subdirectories of `x86_64/`

| Directory | Holds | Where its detail is |
| --------- | ----- | ------------------- |
| `cpu/` | What the processor is made to load, and the state it keeps for each core: the global descriptor table, the interrupt descriptor table, the task state segments, the per-processor areas reached through the `GS` base, and the ticket spinlock built upon them. | `PRIVILEGE.md`, `CONCURRENCY.md` |
| `interrupt/` | Delivery and dispatch: the 256 per-vector stubs, the dispatcher, the exception handlers, and the request layer through which a driver claims a line. | `INTERRUPTS.md` |
| `syscall/` | The privilege boundary: the model-specific registers that configure `SYSCALL`, the entry path they name, the dispatch table and the validation of a caller's arguments. | `PRIVILEGE.md` |
| `smp/` | More than one processor: the bring-up sequence, the trampoline a started processor begins in real mode upon, and the inter-processor interrupt. | `SMP.md`, `CONCURRENCY.md` |
| `mm/` | The paging hierarchy and everything whose correctness depends upon its shape. | `MEMORY-LAYOUT.md` |
| `proc/` | The context switch itself, which is a list of registers. | `PROCESS.md` |

The division between `cpu/` and `smp/` is the one worth stating: `cpu/` is about
**a** processor and `smp/` is about **several**. A spinlock is in `cpu/` and not
`smp/` because a single-processor kernel still takes it — it masks interrupts,
and that is the half of its job which has nothing to do with a second core.

## The headers are not here, but they say the same thing

The declarations stay in [`../include/oxys/`](../include/oxys/) with the whole of
the corpus — there is no second include root under this directory, and there must
not be one. [`../../drivers/README.md`](../../drivers/README.md) states the rule
for a device driver, "the kernel core therefore depends upon the interface and
never upon a driver's location", and it answers this case too.

What **did** change is that the corpus is now grouped the way this directory is,
so the interfaces make the same distinction the implementations do:

| Implementation | Interface |
| -------------- | --------- |
| `arch/x86_64/cpu/gdt.c` | `<oxys/arch/cpu/gdt.h>` |
| `arch/x86_64/interrupt/irq.c` | `<oxys/arch/interrupt/irq.h>` |
| `arch/x86_64/mm/paging.c` | `<oxys/arch/mm/paging.h>` |
| `mm/pmm.c` | `<oxys/mm/pmm.h>` |

**This was recorded as the boundary's one loose end and is no longer one.** When
`arch/x86_64/` was first established the headers were left flat, and
`ARCHITECTURE.md`, Section 2.4, said plainly that the corpus therefore could not
tell you which of its headers was portable: `<oxys/paging.h>` described a
four-level hierarchy and sat beside `<oxys/vfs.h>`, which described nothing of
the sort. Grouping the corpus closed it.

Note what the include path says and what it does not. It names `arch`, which is a
statement about portability, and it does not name `x86_64`, which would be a
statement about *this* processor and is the dependency the rule above forbids
advertising. A port would resolve `<oxys/arch/cpu/gdt.h>` to a different file by
putting an architecture's include directory ahead of the shared one; no consumer's
source would change, which is the property being protected.
