<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
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
| `kernel/` | The kernel core: entry, memory management, scheduling, the block layer, the virtual filesystem, and the boot-protocol handoff. It was described here as architecture-independent while `kernel/cpu/` sat within it, which it plainly was not; what is architecture-independent is this directory **less** `arch/`, and that is now a statement about the tree rather than about the prose. | Phase 1 |
| `kernel/include/oxys/` | The kernel's internal header corpus, grouped to mirror the source tree: `arch/`, `mm/`, `proc/`, `exec/`, `acpi/`, `block/`, `fs/`, `boot/`, `dev/`, `gfx/`, `terminal/` and `test/`, with `types.h` and `kernel.h` at the root because they belong to no subsystem. Section 2.5 records the grouping and what it buys. | Phase 1 |
| `kernel/abi/oxys/` | A **second include root**: the system-call interface a program is entitled to — the call numbers, the failure results, the register convention and the two limits an argument is judged against — held apart from the corpus above it and licensed permissively so that the `MIT` C library may include it without including the kernel. It holds constants and never a declaration. | Phase 7 (sub-task 7.1) |
| `kernel/arch/x86_64/` | Everything in the kernel that could not survive a change of processor, in six subdirectories: `cpu/`, `interrupt/`, `syscall/`, `smp/`, `mm/` and `proc/`. The headers of these subsystems are **not** here; they are in the corpus above, under `oxys/arch/`, which mirrors this tree without naming a processor. [`kernel/arch/README.md`](../../kernel/arch/README.md) states the test for admitting a file, and Section 2.4 below records the boundary. | Phase 1, gathered here at the sub-task 7.3 review |
| `kernel/test/` | The boot-time self-tests, one file per subsystem, and the composed volume they are conducted upon. | Phase 2 |
| `kernel/handoff/` | The boot-protocol handoff layer: the reading of whatever structure the boot loader left, reduced to the neutral `BootInformation` of `<oxys/boot/bootinfo.h>`. It carries its own private header rather than one in the corpus above, because the wire format of a boot protocol is exactly what design premise 3 forbids anything above it to know. Multiboot2 is its one member; the UEFI equivalent joins it in Phase 12. | Phase 1 |
| `kernel/acpi/` | The reading of the firmware's ACPI description tables. | Phase 6 (sub-task 6.12) |
| `kernel/block/` | The generic block-device layer and the buffer cache above it: the layer a storage driver registers into, and the cache the filesystems read through. Above `drivers/` and below `kernel/fs/`, and in neither. | Phase 4 (sub-tasks 4.5 and 4.6) |
| `kernel/terminal/` | The terminal input path: one byte stream, drawn from the keyboard and the serial line, that a program's `read` of descriptor 0 delivers — the keyboard's keys translated to the sequences a terminal sends, and nothing echoed or assembled. Above `drivers/` and below the system call, and in neither. | Phase 8 (sub-task 8.1) |
| `drivers/` | Device drivers, one subdirectory per device class. | Phase 1 |
| `libc/` | The minimal C library linked into user programs. `libc/include/` is its header root and `libc/string/` its first material. | Phase 7 (sub-task 7.1) |
| `userland/` | User programs: the utilities of Phase 7, and from sub-task 8.1 the shell — whose tokeniser and parser are compiled into the kernel image as well, under `SHELL_SOURCES`, for the self-test to assert. | Phases 7 and 8 |
| `graphics/` | The framebuffer, the drawing primitives, the font and the compositing surface. | Phase 6 (established) |
| `crypto/` | The random-number generator, the hash function and the symmetric cipher. | Phase 10 |
| `net/` | The network protocol stack. | Phase 11 |
| `uefi/` | The UEFI application entry point and the UEFI handoff path. | Phase 12 |
| `docs/` | The documentation corpus, grouped by subject into `project/`, `design/`, `devices/` and `storage/` and indexed by [`docs/README.md`](../README.md). | Phase 1 |

### 2.1 The grouping of `docs/`

| Directory | Holds |
| --------- | ----- |
| `docs/project/` | How the work is conducted: the plan, the test procedure and record, the toolchain, the coding standards, the bibliography. |
| `docs/design/` | The kernel itself: this document, the boot sequence, the address space, the interrupts, the privilege transition, and the framebuffer with the drawing above it. |
| `docs/devices/` | One document per device the kernel drives. |
| `docs/storage/` | The stack from a medium to a caller: the disk, the block layer, the buffer cache. |

The grouping is by subject and not by phase, since a document is amended in every
phase that touches its subject. A directory `README.md` describes its directory's
contents locally; these documents describe the system by subject. The two are
complementary and neither replaces the other.

### 2.2 When a subsystem becomes a directory

Four subsystems occupy a directory of their own rather than a file, and the rule
they establish is worth stating once.

| Directory | Was | Is now |
| --------- | --- | ------ |
| `kernel/fs/ext2/` | `kernel/fs/ext2.c`, 4,325 lines | Nine units, 306 to 856 lines, and a private header |
| `kernel/fs/vfs/` | `kernel/fs/vfs.c`, 2,355 lines | Six units, 184 to 532 lines, and a private header |
| `kernel/test/storage/ext2/` | part of `kernel/test/verify_ext2.c`, 2,618 lines | Five chapters and a private header, the entry point remaining above them at 318 lines |
| `drivers/ata/` | `drivers/ata/ata.c`, 1,282 lines | Six units, 160 to 320 lines, and a private header |

A translation unit is divided when it stops being readable as one thing.
`kernel/fs/ext2.c` reached 4,325 lines, and the evidence that it had outgrown
itself was in its own header block: the `Purpose` said it implemented "the
reading and validation of an EXT2 superblock", which had been true when it was
written and described about a twelfth of what the file had become. A header that
no longer describes its file is not a documentation defect to be corrected in
place — it is the file telling you it has become several.

The division is along the lines the *faults* fall upon and not merely the lines
the functions group by: a fault in `path.c` resolves a name to the wrong file, a
fault in `name.c` leaves a volume malformed, a fault in `alloc.c` leaves the
bitmaps disagreeing with the summaries. Each is a different kind of wrongness
with a different way of being found.

The same test applied to the other three. `drivers/ata/` divides at the point
where the *symptoms* diverge: a fault in `port.c` is a timing rule broken and a
status register believed too early, a fault in `channel.c` is a disk this driver
never looked in the right place for, and a fault in `transfer.c` is the wrong
sector returned — and only the last of the three is visible to the caller at all.
`kernel/test/storage/ext2/probe.c` is separated on a line the project had already drawn
in prose: a probe asserts nothing, and `kernel/test/README.md` explains at length
why the two must not be confused. Making that distinction a file boundary means
it can no longer be blurred by accident.

**A division is not a rewrite.** Nothing was reordered, renamed for taste, or
improved in passing. What changed is the set of files the same code lives in, so
that the check afterwards can be mechanical: the function definitions before and
after must be the same set, and every non-comment line must survive but for the
qualifiers the new boundaries force. Anything else in that difference is a defect
introduced by the division, and would be invisible in a diff that also carried
improvements.

Such a directory carries an `internal.h` beside its sources and **not** in
`kernel/include/oxys/`. The public corpus is what a consumer may depend upon;
what the parts of one subsystem share between themselves is not that. Those
declarations were file-scope statics before the division and would be statics
still if C offered any way to share them among a chosen few, and placing the
header beside the implementation is the whole of what records that limit.

### 2.3 When a subsystem is in the wrong directory

Section 2.2 is about a file that outgrew itself. This one is about a file that
was never where it belonged, which is a different defect and has a different
symptom: not a header that stopped describing its file, but a **directory whose
`README.md` stopped describing its contents**.

Three relocations were made at one review, at the project owner's direction.
Nothing was rewritten: a move is not a division and a division is not a rewrite,
so the same check applies — what changed is the path a file is at, and every
other line of it must survive.

| Was | Is now | The claim it falsified |
| --- | ------ | ---------------------- |
| `drivers/block/block.c`, `drivers/block/buffer.c` | `kernel/block/` | `drivers/README.md`: "one subdirectory per device class", and "a driver implements an interface declared in `kernel/include/oxys/`; it does not export declarations of its own" |
| `kernel/multiboot2.c`, `kernel/include/oxys/multiboot2.h` | `kernel/handoff/` | Section 1, premise 3: nothing above the handoff layer knows which boot protocol it was booted by |
| `kernel/include/oxys/testvolume.h` | `kernel/test/volume.h` | Section 2.2: what the parts of one subsystem share between themselves does not go in the public corpus |

**The block layer.** `drivers/README.md` already carried the test, and had
already applied it once: the framebuffer is not a driver, "because nothing
programs it". Applied to `block.c` and `buffer.c` the same test excludes them
just as plainly — a registry with four validations, and a hash table with a
recency list. Neither holds a register, a port address or a timing rule, and
neither cites a hardware specification, which every genuine driver in that
directory opens by doing. The relation was inverted besides: a driver implements
an interface this corpus declares, and `block.c` *declares* `<oxys/block/block.h>`,
which `ata/ata.c`, `ahci/ahci.c` and `sdhci/sdhci.c` register into. A directory
that excludes the framebuffer and admits a hash table is not applying a rule.

**The handoff.** `kernel/include/oxys/multiboot2.h` had exactly one consumer in
the entire tree — the file beside which it now sits. It holds one boot protocol's
wire format, and premise 3 of Section 1 is that everything above the handoff
layer consumes `<oxys/boot/bootinfo.h>` instead and cannot tell which protocol
supplied it. Leaving the wire format in the public corpus advertised, to every
future subsystem, a dependency the premise forbids; the corpus is where a
consumer looks for what it may use. Phase 12 adds the UEFI handoff, and it now
has a directory to be added to rather than a decision to be made under the
pressure of adding it.

**The fixture.** `testvolume.h` had ten consumers and every one of them was under
`kernel/test/`. Its sibling fixture had kept its header locally as
`kernel/test/program.h` from the day it was written, so two fixtures of identical
role sat in two different corpora, and the rule of Section 2.2 decided which of
them was wrong.

The common lesson is that **a directory's `README.md` is a claim about what is in
it**, and `PROJECT_GUIDELINES.md`, Section 10, is what makes each directory carry
one. Where a file contradicts that claim, one of the two is wrong, and the cheaper
repair — amending the prose to admit the exception — is the one that costs
something later: it spends the rule. Each of these three was found by reading a
`README.md` against `git ls-files` and asking which of the two to believe.

### 2.4 The architecture boundary

Sections 2.2 and 2.3 are about one file at a time. This one is about a line drawn
through the whole kernel, at the project owner's direction, and it is the only
structural change here that was not forced by a document contradicting itself.

`kernel/arch/x86_64/` gathers what could not survive a change of processor.
[`kernel/arch/README.md`](../../kernel/arch/README.md) states the test and
applies it file by file; what belongs here is why the line is worth drawing at
all, and what it does not yet achieve.

| Was | Is now | The subject |
| --- | ------ | ----------- |
| `kernel/cpu/gdt.*`, `idt.c`, `tss.c`, `percpu.c`, `spinlock.c` | `kernel/arch/x86_64/cpu/` | What the processor loads, and the state each core keeps |
| `kernel/cpu/exceptions.c`, `interrupts.c`, `irq.c`, `interrupt_stubs.asm` | `kernel/arch/x86_64/interrupt/` | Delivery and dispatch |
| `kernel/cpu/syscall.c`, `syscall_entry.asm` | `kernel/arch/x86_64/syscall/` | The privilege boundary |
| `kernel/cpu/smp.c`, `smp_trampoline.asm`, `ipi.c` | `kernel/arch/x86_64/smp/` | More than one processor |
| `kernel/mm/paging.c`, `addrspace.c`, `shootdown.c` | `kernel/arch/x86_64/mm/` | The paging hierarchy and what depends upon its shape |
| `kernel/proc/switch.asm` | `kernel/arch/x86_64/proc/` | The context switch |

**This is not preparation for a port.** `PLAN.md` has thirteen phases and none of
them is one; Phase 12 changes the boot protocol and not the processor. A
directory justified by a port that is not planned would be exactly the
speculative structure Section 8 of `PROJECT_GUIDELINES.md` discourages, and the
justification is a different one.

It is that **this kernel contains two kinds of claim, and they fail differently.**
A defect in `mm/pmm.c` is a mistake about an algorithm: the bitmap and the
reference counts disagree, and the argument that they should not is one you can
follow on paper. A defect in `arch/x86_64/cpu/tss.c` is a mistake about a
manual — a field at the wrong offset, a segment one byte short, a register named
to an instruction not defined upon it. The second kind is not found by reasoning,
because the reasoning is somebody else's and is in Intel's manual; it is found by
checking the citation. Section 2 of `PROJECT_GUIDELINES.md` requires every such
assertion to carry one, and the distinction between a file that owes citations
and a file that does not was, until this change, held in the head of whoever was
reading. It is now a path.

The `cpu/` and `smp/` division within it is worth stating because it is not
obvious: `cpu/` is about **a** processor and `smp/` about **several**. The ticket
spinlock is in `cpu/` although it exists for contention, because the other half
of its job — masking interrupts for as long as it is held — is owed on a machine
with one core, and `CONCURRENCY.md`, Section 3, is about that half.

**Two subsystems are now split across both trees**, which is the cost of the line
and not a defect in it. `kernel/mm/` keeps the frame allocator, the address-range
allocator and the heap; `kernel/arch/x86_64/mm/` takes the four-level hierarchy
and the two files whose correctness depends on its shape. `kernel/proc/` keeps
the process table and the scheduler; the context switch is six registers and an
`IRETQ`, and is here. A reader looking for "memory management" now looks in two
places, and the compensation is that they can tell which of the two they are in.

**What this did not at first achieve.** The headers did not move with the
implementations. `<oxys/paging.h>` described a four-level hierarchy and
`<oxys/tss.h>` a 104-byte segment, and both sat in `kernel/include/oxys/` beside
`<oxys/vfs.h>`, which described nothing of the sort — so the implementations
made the distinction and the interfaces did not. This section recorded that as
the boundary's one loose end, and **Section 2.5 closed it**: the corpus is now
grouped the way this tree is, and the interface to anything here is reached
through `<oxys/arch/...>`. What the path names is `arch`, which is a claim about
portability; what it still does not name is `x86_64`, which would be a claim
about this processor and is the dependency a second include root would have
advertised.

`kernel/kernel.c`, `proc/sched.c` and `proc/process.c` each retain a handful of
instructions that are plainly x86 — `sti; hlt` in the idle loop, `cli; hlt` in
the termination guard, a `CR3` in a comment about why a switch does what it does.
They were left where they are because extracting three instructions into an
architecture shim would cost a layer of indirection to buy a boundary nothing is
pressing against.

**And the boundary is crossed more widely than that, which this section first
understated.** It said the line was drawn at the file and three files sat
slightly on the wrong side. Measured rather than asserted, the portable core
includes `<oxys/arch/...>` headers **fourteen times**, across five files:
`proc/process.c` six, `proc/sched.c` three, `exec/elf.c` three, and `mm/vmm.c`
and `acpi/acpi.c` one each. `kernel/arch/README.md` lists them and grades them,
the shallow ones — a spinlock, which every processor has in some form — apart
from the deep, such as `process.c` writing `rsp0` into a task state segment,
which is not a facility another processor has a different version of but a
question it would not ask.

So the honest statement of what this division achieves is the weaker one:
**it groups the kernel by what a defect in it is answerable to, and it does not
isolate the portable part.** The grouping is worth having on that ground alone —
it is the ground Section 2.4 opens with — but the stronger claim is not yet
earned and should not be made until the number above comes down.

`tools/check-docs.sh`, Section 8, is what keeps it honest from here. The fourteen
crossings are recorded with a reason apiece and checked in both directions, so a
fifteenth fails `make lint` and so does an entry left behind when a crossing is
removed. The debt can now only change deliberately, which is the property the
prose alone never had — and which `drivers/` lacking it for three phases, at
Section 2.3, is the cautionary case for.

### 2.5 The grouping of the header corpus and of the self-tests

Two directories had stayed flat while everything around them acquired a shape,
and both were reorganised at the project owner's direction.

**`kernel/include/oxys/` was fifty-four headers in one directory.** It is now
grouped to mirror the source tree, so that the interface to a subsystem sits
where the subsystem does:

| Under `oxys/` | Holds | Mirrors |
| ------------- | ----- | ------- |
| *(root)* | `types.h`, `kernel.h` | Nothing: these two are universal, included from every corner, and belong to no subsystem. |
| `arch/cpu/`, `arch/interrupt/`, `arch/syscall/`, `arch/smp/`, `arch/mm/` | The interfaces of the processor-bound subsystems | `kernel/arch/x86_64/` |
| `mm/` | `memory.h`, `pmm.h`, `vmm.h`, `heap.h` | `kernel/mm/` |
| `proc/`, `exec/`, `acpi/`, `block/`, `fs/`, `test/` | One group each | The kernel directory of the same name |
| `boot/` | `bootinfo.h`, the neutral description everything above the handoff consumes | `kernel/handoff/` |
| `dev/`, `dev/storage/` | The driver interfaces | `drivers/` |
| `gfx/` | The framebuffer, the drawing, the console, the compositor | `graphics/` |

The gain is the one Section 2.4 asked for and could not have. A header's path is
now a claim about it, and the claim a reader most needs is the portability one:
`<oxys/arch/mm/paging.h>` announces that what it describes is answerable to
Intel's manual, and `<oxys/mm/pmm.h>` announces that what it describes is not.
Two headers that had sat side by side, indistinguishable, now sort into different
directories on exactly the line the implementations were sorted on.

`arch/` and not `arch/x86_64/`, deliberately. The consumer's `#include` says that
a thing is processor-bound; it does not say which processor, because it must not
have to change if that ever answered differently.

**`kernel/test/` was twenty-four files named `verify_*.c` in one directory.** The
prefix was doing the work a directory should do — it existed to say "this is a
self-test" in a directory where nothing else was — and twenty-four files sharing
one prefix sort as a single undifferentiated block, which is the arrangement a
reader has to read all of to search any of. They are now grouped by the subsystem
they assert and the prefix is dropped, `kernel/test/arch/syscall.c` saying what
`kernel/test/verify_syscall.c` said with one word fewer and a shape besides:

| Directory | Asserts |
| --------- | ------- |
| `arch/` | `interrupts.c`, `privilege.c`, `syscall.c`, `usermode.c`, `apic.c`, `smp.c` |
| `mm/` | `memory.c` |
| `proc/` | `process.c`, `sched.c`, `lifecycle.c` |
| `exec/` | `elf.c` |
| `storage/` | `stack.c`, `ext2.c` with its five chapters beneath, `vfs.c` |
| `gfx/` | `framebuffer.c`, `graphics.c`, `console.c`, `compositor.c`, `faultscreen.c` |
| `dev/` | `devices.c`, `mouse.c` |
| `libc/` | `string.c`, `wrappers.c`, `heap.c`, `stdio.c`, `startup.c`, `utilities.c`, `line.c` |
| `terminal/` | `terminal.c` |
| `shell/` | `parser.c` |

The **function** names did not change. `KernelVerifySyscall` is still
`KernelVerifySyscall`, because `<oxys/test/verify.h>` declares it and `kernel.c`
calls it by name in the order the tests run; the prefix earns its place there,
where the symbols of every subsystem do share one namespace. It was only in the
file names that it was redundant, and only there that it was dropped.

**One consequence was a simplification rather than a rename.** Three self-tests
are compiled against the C library's include root, and the `Makefile` named them
in three explicit rules because — its note said — a pattern over `kernel/test/`
would put every future self-test in reach of the userland's headers whether or
not it asserted the userland. That was true of `kernel/test/` and is not true of
`kernel/test/libc/`, whose membership *is* the exception: a file is in it exactly
when it asserts the C library. The three rules are now one pattern whose scope
and whose exception are the same set, and a fourth such test is added by putting
it in the directory rather than by remembering to add a fourth rule.

## 3. Present composition

As of the completion of Phase 5 and of sub-tasks 6.1 to 6.13, the system
comprises the following translation units. The list is the `C_SOURCES` and
`ASM_SOURCES` of the `Makefile` and must be revised in the same change as
either.

| Unit | Role |
| ---- | ---- |
| `boot/boot.asm` | The Multiboot2 header; the 32-bit entry point `_start`; CPUID and long-mode feature detection; the construction of the boot-time paging hierarchy; the long-mode transition; the higher-half entry point `KernelEntryHigh`. |
| `boot/trampoline.asm` | The real-mode trampoline an application processor begins executing on answering a startup inter-processor interrupt: real mode with `CS` normalised, 32-bit protected mode, and 64-bit long mode upon the kernel's own paging hierarchy, with the parameter block the bootstrap processor fills in. Assembled to a flat binary at a fixed origin, not linked. |
| `kernel/arch/x86_64/cpu/gdt.c`, `kernel/arch/x86_64/cpu/gdt.asm` | The kernel global descriptor table and the reloading of the segment registers. |
| `kernel/arch/x86_64/cpu/idt.c` | The interrupt descriptor table: its storage, the installation of a gate, the assignment of an interrupt stack table entry to a gate, and the loading of the table. |
| `kernel/arch/x86_64/cpu/tss.c` | The task state segment: the stacks the processor loads when it needs one it can trust, its descriptor within the global descriptor table, and the loading of the task register. |
| `kernel/arch/x86_64/cpu/percpu.c` | The per-processor data areas: their static allocation, the establishment of the executing processor's own, the segment base it is reached through and the repair of that base after a segment reload, and the counted interrupt-disable every critical section is built upon. |
| `kernel/arch/x86_64/cpu/spinlock.c` | The ticket spinlock: the locked fetch-and-add that issues a ticket, the bounded wait to be served, the release that admits the next arrival, and the two checks that turn the silent misuses of a lock into a report. |
| `kernel/arch/x86_64/interrupt/interrupt_stubs.asm` | The 256 per-vector entry stubs and the common stub that saves the registers and calls the dispatcher. |
| `kernel/arch/x86_64/interrupt/interrupts.c` | The installation of the stubs, the dispatch table and the routing of each vector to its registered handler. |
| `kernel/arch/x86_64/interrupt/irq.c` | The interrupt request layer: the handlers claimed by request line rather than by vector, the routing of a request to the driver that claimed it, the signalling of completion at whichever controller delivered it, and the retirement of the 8259A pair in favour of the APIC. |
| `kernel/arch/x86_64/interrupt/exceptions.c` | The handlers for the architecture-defined exceptions and the diagnostic report. |
| `kernel/arch/x86_64/syscall/syscall.c` | The configuration of the fast system-call mechanism — `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, `IA32_KERNEL_GS_BASE` and the enabling bit of `IA32_EFER` — and, from sub-task 6.7, the dispatch table and the validation of a caller's arguments; the table holds three calls from 6.7 and seven from sub-task 6.11, which adds `fork`, `execve`, `exit` and `wait`. |
| `kernel/arch/x86_64/syscall/syscall_entry.asm` | The entry point `IA32_LSTAR` names. Provisional in sub-task 6.1; replaced at sub-task 6.7 by the path that swaps `GS`, loads the kernel stack from the per-processor area, dispatches, and returns by `SYSRET`. |
| `kernel/arch/x86_64/smp/ipi.c` | The inter-processor interrupt layer: the composition of a command for each of the three audiences a sender may address, the accounting, and the handler by which a panicking processor stops the others. |
| `kernel/arch/x86_64/smp/smp.c` | The bring-up of the application processors: the proving and mapping of the low page the trampoline requires, the resources each processor is given before it is started, the INIT-startup-startup sequence and the bounded waits around it, and the C entry point a started processor arrives at and parks in. |
| `kernel/arch/x86_64/smp/smp_trampoline.asm` | Carries the assembled trampoline into the kernel image with `incbin`, and gives its two ends the symbols the C takes the size from. |
| `kernel/acpi/acpi.c` | The firmware's ACPI description tables: the discovery and validation of the Root System Description Pointer, the walk of the RSDT or XSDT, and the parse of the Multiple APIC Description Table. |
| `kernel/exec/elf.c` | The ELF64 loader for statically linked executables: the decoding of the file and program headers, the fifteen refusals an image must survive whole before a page of it is mapped, and the placing of its segments into an address space through the direct physical map. |
| `kernel/proc/process.c` | The process control block, the thread and the saved context: the two tables, the address space a process is given, the kernel stack and guard page a thread is given, the writing of `rsp0` when a thread becomes current, the switch, the descent to privilege level 3, the termination that returns from it, and — from sub-task 6.11 — `fork`, `execve`, `exit` and `wait`. |
| `kernel/terminal/terminal.c` | The terminal input path of sub-task 8.1: the queue, the poll that drains the keyboard's events and the serial adapter's characters into it, the translation of a key event to the bytes a terminal would send, and the wait a `read` of descriptor 0 makes upon it. |
| `kernel/arch/x86_64/proc/switch.asm` | `ThreadSwitchContext`, which exchanges six registers and a stack pointer; `ThreadTrampoline`, where a thread that has never run begins; `ThreadEnterUser`, which clears every register and descends to privilege level 3 by `IRETQ`; and `ThreadResumeUser`, which descends with a whole saved register set restored, as a thread made by `fork` requires. |
| `graphics/compositor.c` | The compositor: the back buffer that stands in for the framebuffer, the ordered layers composited over it, the damage rectangle that narrows what is carried to the display, and the suspension a fault screen imposes. |
| `graphics/cursor.c` | The pointer: its two-bitmap shape, and the layer the compositor draws it as. |
| `graphics/draw.c` | The two-dimensional primitives upon a surface: rectangle arithmetic and clipping, the pixel, the filled and outlined rectangle, the integer line, and the blit. |
| `graphics/framebuffer.c` | The framebuffer the boot loader supplies: its validation, the write-combining memory type given to its pages, its mapping into the kernel arena, and the description every later phase draws through. |
| `graphics/font.c` | The bitmap face — ninety-five glyphs of eight by eight, drawn for this project — and the drawing of one glyph upon a surface. |
| `graphics/console.c` | The graphical console: a grid of character cells upon the framebuffer, the four control characters, a scroll performed by blitting the surface upon itself, and the replay of what was written before the framebuffer could be mapped. |
| `graphics/faultscreen.c` | The full-screen page a severe fault produces: one screen for each fault, with its own title, colour, account and evidence. |
| `kernel/test/volume.c` | The test fixture: two block devices backed by memory, and an EXT2 volume composed within them. |
| `kernel/test/mm/memory.c` | The self-tests of the frame allocator, the paging hierarchy, the allocators, reference counting, copy-on-write and address spaces. |
| `kernel/test/arch/interrupts.c` | The self-tests of the descriptor table, the stubs and their trap frame, the dispatcher and the exception handlers. |
| `kernel/test/gfx/graphics.c` | The self-tests of the drawing primitives, conducted upon a surface in memory. |
| `kernel/test/gfx/framebuffer.c` | The self-tests of the framebuffer's description, its mapping, its memory type, and the pattern a person judges. |
| `kernel/test/gfx/console.c` | The self-tests of the bitmap face against its own metrics, of a glyph drawn upon a surface against its own bytes, and of the four control characters upon the live console. |
| `kernel/test/gfx/faultscreen.c` | The self-tests of the fault screen table: that every severe fault has a screen of its own and that no two of them are alike. |
| `kernel/test/arch/privilege.c` | The self-tests of the descriptors, the task state segment, the interrupt stack table and the system-call configuration. |
| `kernel/test/arch/syscall.c` | The self-tests of the system-call dispatch table and of the validation of a caller's arguments. |
| `kernel/test/exec/elf.c` | The self-tests of the ELF64 loader, upon an image composed in memory so that every field may be made wrong on purpose. |
| `kernel/test/proc/process.c` | The self-tests of the process and thread tables, the per-thread kernel stack and its guard, and the balance of the arena. |
| `kernel/test/arch/usermode.c` | The self-tests of the context switch, and of a program composed, loaded, entered at privilege level 3 and ended. |
| `kernel/test/proc/lifecycle.c` | The self-tests of `fork`, `execve`, `exit` and `wait`: a process cloned and examined without running, and a program that forks twice, replaces one child with a program read from a volume, and collects what each ended with. |
| `kernel/test/gfx/compositor.c` | The self-tests of the clip stack, the blend, the damage arithmetic and the layer table. |
| `kernel/test/dev/mouse.c` | The self-tests of the mouse's packet decoder, driven without a mouse, and of the pointer upon a surface in memory. |
| `kernel/test/dev/devices.c` | The self-tests of the interrupt controllers, the interval timer, the keyboard, the serial adapter, the display and the bus. |
| `kernel/test/storage/stack.c` | The self-tests of the disk, the block layer and the buffer cache. |
| `kernel/test/storage/ext2.c` | The entry point of the EXT2 self-test: the fixture composed, the superblock and its refusals asserted, and the five chapters below called in turn. |
| `kernel/test/storage/ext2/internal.h` | What those chapters share: their own entry points, the restoration of the fixture between them, and the two helpers more than one judges through. |
| `kernel/test/storage/ext2/format.c` | The superblock, the group descriptors and the inode, and the dozen ways a volume may contradict itself and be refused. |
| `kernel/test/storage/ext2/directory.c` | The directory record, its traversal, and the resolution of a path across symbolic links. |
| `kernel/test/storage/ext2/file.c` | The reading of a file's contents, its holes, and both forms of symbolic link. |
| `kernel/test/storage/ext2/write.c` | Everything that alters a volume: allocation, writing, truncation, and the insertion and removal of names. |
| `kernel/test/storage/ext2/probe.c` | The report upon whatever volume the machine actually carries. **Not a self-test**: it asserts nothing, and the distinction is the reason it is a file of its own. |
| `kernel/test/storage/vfs.c` | The self-tests of the virtual filesystem layer, and the probe of a real volume through it. |
| `kernel/test/arch/apic.c` | The self-tests of the ACPI parse, the Local APIC, the I/O APIC, and the routing of the request lines through them once the 8259A pair has been retired. |
| `kernel/test/arch/smp.c` | The self-tests of the per-processor area, the spinlock, the inter-processor interrupt and the shootdown — the first two asserting internal state, since upon one processor a lock that does not lock behaves like one that does, and the last two asserting behaviour by an interrupt the processor sends to itself. |
| `kernel/mm/heap.c` | The kernel heap: a slab allocator of eight size classes over the kernel arena. |
| `kernel/mm/vmm.c` | The kernel virtual address allocator, issuing ranges of the kernel arena backed by frames. |
| `kernel/arch/x86_64/mm/paging.c` | The permanent kernel paging hierarchy: its construction, activation, software translation and copy-on-write fault resolution. |
| `kernel/arch/x86_64/mm/shootdown.c` | The translation-lookaside-buffer shootdown: the publication of the address whose translation has become stale, the interrupt that tells the other processors to discard it, the acknowledgement each makes, and the bounded wait for all of them. |
| `kernel/arch/x86_64/mm/addrspace.c` | The address space: its creation, its cloning by the copy-on-write discipline, its activation and its destruction. |
| `kernel/mm/pmm.c` | The physical frame allocator: a bitmap of every 4 KiB frame below the highest usable address. |
| `kernel/fs/ext2/internal.h` | What the nine translation units below share with one another and with nothing else: the record of the last refusal, the accounting, the decoders and encoders of the volume's byte order, and the block-level transfer. |
| `kernel/fs/ext2/core.c` | The shared state, the refusals, the decoding and encoding of the stored byte order, the block-level transfer in both directions, and the accounting accessors. |
| `kernel/fs/ext2/superblock.c` | The superblock: its reading, its validation, the geometry derived from it, its writing, and the judgement of whether a volume may be read, written, or addressed at all. |
| `kernel/fs/ext2/group.c` | The block group descriptor table: the geometry of the groups, one descriptor read and written, and the validation of the whole table. |
| `kernel/fs/ext2/inode.c` | The inode: its retrieval and writing, the resolution of a file's block index through every level of indirection, and the allocation of the blocks a file grows into. |
| `kernel/fs/ext2/file.c` | The contents of a file: reading a range of its bytes, the two forms of symbolic link, the writing that extends it, and the truncation that releases what it no longer covers. |
| `kernel/fs/ext2/alloc.c` | The two bitmaps: the testing and setting of a bit, the search for a free one, and the group and superblock summaries kept in step with every allocation. |
| `kernel/fs/ext2/directory.c` | The record a directory is made of: the file types, the decoding and validation of one entry, the traversal, and the search for a name. |
| `kernel/fs/ext2/path.c` | The resolution of an absolute path to the inode it names, across symbolic links and with a bound upon how many may be followed. |
| `kernel/fs/ext2/name.c` | The names themselves: the insertion and removal of a record, and the creation and destruction of files, directories and hard links. |
| `kernel/fs/vfs/internal.h` | What the six units below share: the four fixed tables the layer's whole state lives in, the refusal record and the accounting, the open file, and the resolution and node-cache primitives. |
| `kernel/fs/vfs/vfs.c` | The state itself, the refusals and the names of the error codes, the bounded string primitives, and the accounting and reports. |
| `kernel/fs/vfs/node.c` | The node cache: the identity a file has within the kernel, and why one file must be one node however many callers reach it. |
| `kernel/fs/vfs/path.c` | The resolution of a path: the walk through each component, the crossing of mount points in both directions, and the following of symbolic links. |
| `kernel/fs/vfs/mount.c` | The registry of filesystem types and the mount table that joins several volumes into one tree. |
| `kernel/fs/vfs/file.c` | The open file: the descriptor table, the position that advances, and the reading, writing and seeking above it. |
| `kernel/fs/vfs/namespace.c` | The operations that name a file rather than hold one open: stat, truncate, the creation and removal of names and directories, and the flush. |
| `kernel/fs/ext2_vfs.c` | The binding of the EXT2 implementation to that layer: the operations vector, the translation between the format's mode and the layer's neutral node type, and the mark a mount leaves upon a volume it has open. |
| `kernel/handoff/multiboot2.c` | The Multiboot2 parser, reducing the boot loader's structure to the neutral `BootInformation` description. |
| `kernel/kernel.c` | `KernelMain`, which validates the boot loader handover, initialises every subsystem in the dependency order of Section 4, runs the self-tests, mounts a root volume and enters the echo loop. `KernelPanic`, the unrecoverable-error path. |
| `drivers/vga/vga.c` | The VGA text-mode display driver: the control characters, the scrolling, the colour attributes, the hardware cursor and the erase limit that bounds a backspace. |
| `drivers/serial/serial.c` | The interrupt-driven COM1 serial driver used for diagnostics and input. |
| `drivers/pic/pic.c` | The pair of cascaded 8259A interrupt controllers: their remapping, the masking of request lines, the recognition of a spurious request, the end-of-interrupt protocol, and the silencing of the pair when the APIC supersedes it. |
| `drivers/apic/lapic.c` | The Local APIC: its detection, the mapping of its register page as uncacheable memory, its two enables, the local vector table entries this kernel programmes, and the end-of-interrupt every handler owes it. |
| `drivers/apic/ioapic.c` | The I/O APIC: the indirect register pair its registers are reached through, and the redirection table entry that decides what vector an interrupt input presents and to which processor. |
| `drivers/pit/pit.c` | Counter 0 of the 8253 interval timer: the system tick, the elapsed-time conversion and the bounded wait. |
| `drivers/ata/internal.h` | What the six units below share: the register and status constants, the table of devices found, the addresses each channel answers at, the accounting, and the register-level discipline. |
| `drivers/ata/ata.c` | The driver's state, its refusals, the initialisation that finds what is present, the device accessors and the binding to the block layer. |
| `drivers/ata/port.c` | The register-level discipline of the task file: the settling delay a selection must be followed by, the two waits every command is bracketed by, and the reset of a channel. |
| `drivers/ata/identify.c` | The identification of whatever stands at one of the four addresses, and the distinction between a device that is absent and one answering a different command set. |
| `drivers/ata/channel.c` | Where each channel actually answers: the base address registers of a controller in native mode, and the classification of storage this driver cannot reach at all. |
| `drivers/ata/transfer.c` | The transfer of sectors: the judgement of a request, both addressing forms, and the cache flush that makes a write durable. |
| `drivers/ata/report.c` | The report, including — for a machine upon which no disk was found — every controller the bus carries and why each was not reached. |
| `drivers/pci/pci.c` | The PCI configuration-space enumeration by access mechanism one: the walk of buses, devices and functions, and the searches by which a driver finds its hardware. |
| `drivers/ps2/ps2.c` | The 8042 controller itself: its configuration byte, written whole by the one module that owns it, and the two device ports it presents. |
| `drivers/keyboard/keyboard.c` | The PS/2 keyboard upon the controller's first port: initialisation, the decoding of scan code set 1, the modifier state and the circular event buffer. |
| `drivers/mouse/mouse.c` | The PS/2 mouse upon the controller's second port: the framing of a packet stream that has none, the nine-bit movement, the single inversion of the vertical sense, and the position the driver keeps. |
| `drivers/ahci/ahci.c` | The AHCI adaptor by first-party direct memory access: the handoff from the firmware, the ports it implements, the command list, and the region descriptors that name a caller's pages to the device. |
| `drivers/sdhci/sdhci.c` | The SD host controller and the card behind it: the card's own command set, the two encodings of its capacity, and the transfer through the buffer data port. |
| `drivers/ramdisk/ramdisk.c` | The extent of physical memory a boot module occupies, presented to the block layer as a device. The one driver here that converses with nothing: a transfer is a copy and cannot fail. It is what makes the initial ramdisk of sub-task 7.7 readable by the code that reads a disk. |
| `kernel/block/block.c` | The generic block-device layer: the registry of devices that transfer fixed-size blocks, and the validated path through which every caller above reaches a driver. |
| `kernel/block/buffer.c` | The buffer cache above the block layer: the hash, the recency list, the reference discipline and the write-back policy. |
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

**Status.** Phases 2 to 5 are complete, and Phase 6 is complete as far as
sub-task 6.10. The mutual dependency described above has been discharged:
sub-task 3.4 supplied the fault handler, sub-task 2.7 the copy-on-write
resolution beneath it, and sub-task 2.8 the address-space cloning that creates
the shared pages the resolution acts upon. Sub-task 3.5 remapped the interrupt
controllers, so that a device may be heard; 3.6 supplied the first device that
speaks and 3.7 the first that a person operates. Phase 4 supplied the devices
beneath a filesystem and Phase 5 the filesystem itself, which sub-task 5.8
completed by mounting an EXT2 volume through a virtual filesystem layer.

Phase 6 has since established the apparatus a privilege transition is performed
out of (6.1); acquired the linear framebuffer, mapped write-combining (6.2); the
primitives that draw into it (6.3); the font and console that put the boot log
back upon the screen the framebuffer had displaced (6.4); the pointer, which
required the 8042 controller to become a module of its own first (6.5); the
compositor beneath all of it, after which nothing reads the framebuffer (6.6);
the `SYSCALL` entry path, its dispatch table and its argument validation (6.7);
the ELF64 loader (6.8); the process, the thread and the context (6.9); and the
switch and the descent to privilege level 3 (6.10), at which point a program
first ran. Work continues at sub-task 6.11 — `fork()` upon the copy-on-write
substrate of Phase 2, with `execve()`, `exit()` and `wait()` beside it.

### 4.1 The two orderings chosen against the obvious one

Most of the phase order follows from the diagram above without argument. Two
places do not, and both were changed on 2026-09-03 at the project owner's
decision. The arguments are recorded here rather than in
[`../project/PLAN.md`](../project/PLAN.md), which states the order and not the
reasoning for it.

#### Why sub-tasks 6.2 to 6.6 are in Phase 6 and not in Phase 9

The framebuffer and the drawing above it were sub-tasks 9.1 to 9.5, and
`PROJECT_GUIDELINES.md`, Section 5, placed the whole of the graphical work after
the shell. They were moved forward, and the split is along a real line rather
than an arbitrary one: **nothing in 6.2 to 6.6 depends upon a process existing.**
A framebuffer is memory the boot loader describes and this kernel maps;
primitives, a font and a compositing surface are arithmetic upon that memory; and
a mouse is another device upon the 8042 controller, whose second port the
keyboard driver of sub-task 3.7 already leaves alone. Every one of them is
written, exercised and asserted with the machinery Phases 2 to 5 already provide.

What genuinely does need processes is the half that stays in Phase 9: a window
manager has nothing to manage, and a client protocol has no client, until there
is something to run. That division is why this is a split and not a wholesale
reordering, and it is why Phase 9 is no longer "graphics" but the desktop as a
thing a person uses — the window system, the services that maintain it, and the
configuration they read.

Two things are gained and one is given up. The diagnostic path acquires a console
that is not eighty by twenty-five characters of text, and every phase from here
to the end reports through it; and the choice between the VESA path and the UEFI
Graphics Output Protocol is forced now, while Phase 12 can still be shaped around
it, rather than in Phase 9 when it can no longer be. **What is given up** is that
the surface abstraction of sub-task 6.6 is designed before any user-mode client
exists to design it against, so its interface is a judgement rather than a
response. That is recorded so that the judgement is revisited at sub-task 9.2 and
not merely inherited.

#### Why sub-task 6.13 precedes 6.14

These two stood in the opposite order, and the order was wrong. Sub-task 6.14
starts processors; sub-task 6.13 supplies the locks without which nothing they
touch is safe. Every shared structure this kernel has — the frame allocator's
bitmap and search hint, the heap, the buffer cache, the mount and node tables of
the filesystem layer, the interrupt dispatch table — was unsynchronised then and
remains so, save the diagnostic channel 6.14 locked; each says so in its own
file's header.

Bringing a second processor up before the locks existed would have produced a
milestone that the testing mandate requires to be bootable and testable and that
could be neither: it would either park the new processors immediately, in which
case nothing is demonstrated, or let them run, in which case the machine is
corrupt in a way no assertion here would catch.

**The order chosen produced the first of those two, deliberately.** 6.14 does
park its processors — but it parks them against locks that exist, and it
demonstrates them by the one thing a parked processor can still do: answer an
interrupt, and record having answered it in an area only it writes. That is
`KernelVerifyApplicationProcessors`, and it is the assertion the wrong order
could not have produced. See [`SMP.md`](SMP.md), Section 8.

The reordering costs nothing, because everything in 6.13 can be exercised upon
one processor. A spinlock's uncontended acquire and release, and the per-CPU data
area reached through `GS`, are single-processor mechanisms outright. An
inter-processor interrupt sent to one's own Local APIC is delivered like any
other, so the shootdown handler may be made to run and the invalidation it
performs observed — the same device as sub-task 6.1's execution of `SYSCALL` from
privilege level 0, where a mechanism is exercised in full although the condition
it exists for has not yet arrived.

**The reordering was borne out.** Sub-task 6.13 closed on 2026-09-09 and every
one of its mechanisms was exercised upon the one processor, the shootdown against
a mapping the self-test made stale on purpose. It also found two defects that
would otherwise have surfaced during the bring-up itself, which is the hardest
moment there is to diagnose one: a segment reload destroys `GS.base`, and the
interrupt entry path performed no `SWAPGS`. Both had been harmless only because
nothing in the kernel read `GS`, and both are recorded in
[`CONCURRENCY.md`](CONCURRENCY.md), Sections 3.3 and 3.4.

## 5. Privilege and address-space model

The kernel occupies the upper half of the canonical 48-bit address space and is
mapped into every address space, so that a system call or an interrupt requires
no change of the page-table root. User processes occupy the lower half. The
detailed layout is recorded in `MEMORY-LAYOUT.md`.

The machinery of the transition between the two privilege levels — the user-mode
descriptors and the order the processor's own arithmetic imposes upon them, the
task state segment holding the stacks the processor loads when it needs one it
can trust, and the three registers that configure `SYSCALL` — was established by
sub-task 6.1 and is recorded in `PRIVILEGE.md`. Sub-task 6.7 supplied the entry
path and the dispatch above it, and sub-task 6.10 the descent itself: a program
is loaded into an address space of its own, entered at privilege level 3 by
`IRETQ`, returned to by `SYSRET` when it makes a system call, and ended when it
faults. What does not yet exist is pre-emption — nothing takes a processor away
from a thread that has not given it up. Sub-task 6.15 supplies it for threads
upon a run queue; a *program* is not yet placed upon one.

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
something. `docs/devices/DISPLAY.md`, Section 8, records why a display is unusually hard
to test and what is asserted at each boot in consequence.

**From sub-task 6.2 the operator-facing path is addressed two ways, and the policy
above must be read accordingly.** The kernel asks the boot loader for a linear
framebuffer, and a boot loader that supplies one sets a graphics mode to do it;
the display driver then writes to memory the adapter is not displaying. Sub-task
6.4 supplies a console that draws text upon the framebuffer, so from that point
there are **three** output paths and the operator can see whichever of the first
two the boot loader's chosen mode makes visible.

`KernelWriteString` writes to all three unconditionally and decides between none
of them. Deciding there would put knowledge of the display mode into the one
routine that must work before anything has established what the mode is, and the
two screen paths do not know about each other. That routine is also **the only
one permitted to name an output device**: the numeric routines named the display
and the serial port themselves until sub-task 6.4, and the console was in
consequence shown every word of the boot log and not one of its numbers.

Between sub-tasks 6.2 and 6.4 the cost was real and is worth recording: a machine
with no serial adapter this kernel detects — VirtualBox is one — had **no readable
diagnostic output at all**. `docs/design/FRAMEBUFFER.md`, Section 7, and `docs/design/CONSOLE.md`, Section 2, record
the position, and `docs/project/TESTING.md`, Section 4.1, what it cost the
VirtualBox procedure.

**A fault that the kernel cannot survive leaves the ordinary paths and takes the
display.** Which faults those are is decided by `ExceptionDispositionOf` and set
out in `INTERRUPTS.md`, Section 8.1: an abort, a non-maskable interrupt, a
malformed descriptor table, or any fault the kernel raised within itself. A
divide by zero or an unresolved page fault raised by a program belongs to that
program, costs it alone, and draws nothing — announcing the end of the machine
for one would be a false account of what happened.

Such a fault draws a full-screen page composed for it — its own
title, colour, account of what the processor is reporting, and the evidence that
bears upon it rather than upon the others — and the console is suspended for good
when it does. That is not a duplicate of the report: the report goes to every
path and remains the record; the screen is a summary for a person standing at a
machine that has stopped, who may have no other channel at all.
`docs/design/FAULTSCREEN.md`, Sections 1 and 2.
