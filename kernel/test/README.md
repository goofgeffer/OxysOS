<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `kernel/test/` — The Boot-Time Self-Tests

**Phase**: introduced in Phase 6, sub-task 6.1, as a reorganisation of tests
written from Phase 2 onward. This directory grows with every subsequent phase.
**Detailed design**: [`../../docs/project/TESTING.md`](../../docs/project/TESTING.md);
each subsystem's own design document holds the table pairing its assertions with
the failure each would catch.

## Purpose

This directory holds the tests the kernel runs upon itself at boot, and the
fixture they are conducted upon.

**There is no test harness, and there will be none until Phase 7 has produced a
userland to run one in.** So the tests are not a separate program: they are part
of the kernel image, executed by `KernelMain` in the order the subsystems are
initialised, because a test cannot run before the thing it asserts exists. That
sequence in `KernelMain` is the subsystem dependency ordering of
`docs/design/ARCHITECTURE.md`, Section 4, made executable.

**Sub-task 7.1 is the first sign that the arrangement is temporary**, and the
first strain upon it. `verify_string.c` asserts the C library and not the kernel,
and it is here anyway, because here is the only place in this project where
anything can be executed at all. Its subject sits at the end of the sequence
rather than within it: it depends upon no subsystem, and no subsystem depends
upon it.

**Sub-task 7.2 is the second strain and a sharper one.** `verify_wrappers.c`
asserts a library whose central instruction the kernel **cannot execute at all**:
`SYSCALL` works at any privilege level, but the `SYSRET` that ends the kernel's
handling of it returns to privilege level 3 unconditionally, so a kernel that
called a wrapper would leave its own entry path as a user program. Half of that
test is therefore an ordinary call and half of it is a program — composed here,
loaded, and run at privilege level 3 with the library's own bytes copied into
it. It sits at the end of the sequence too, but for the opposite reason to
`verify_string.c`: not because it depends upon nothing, but because it depends
upon almost everything.

**Sub-task 7.3 is the third, and it is the one that produced an answer rather
than a strain.** `verify_heap.c` asserts an allocator and the system call beneath
it, and those are two different kinds of thing: the allocator is ordinary C that
runs anywhere, and the call is the one thing this kernel cannot make. So the
sub-task was built with the seam named — `libc/include/heap.h` declares the
single function through which the allocator obtains memory, and a second by which
a caller may supply some — and the test gives the allocator a region directly and
exercises the whole of its policy, while a composed program at privilege level 3
exercises `brk`. **Neither half is a reconstruction of what it tests**, and no
test hook was added to the library to make it possible.

**A third file arrived with it that asserts nothing.** `program.c` is the
composer the last two tests build their programs with, factored out of
`verify_wrappers.c` when this sub-task needed a second one. Two copies of an
instruction encoder is two places for a byte to be wrong, and the second copy
would have been wrong in a way the first one's assertions could not see.

Every test reports its own verdict and returns. **None halts the machine, and
none may.** A kernel that stopped at the first failed assertion would report one
failure where it might have reported nine, and the run that matters most is the
one where several things are broken at once. `make verify` reads the verdicts out
of the serial log and fails if any of them says `FAILED`;
`docs/project/TESTING.md`, Section 1, records what that target asserts and why
both of its assertions are necessary.

## Why these are here and not in `kernel.c`

Until sub-task 6.1 they were in `kernel.c`, which had grown to 9,050 lines of
which the entry point was the last 250. Every sub-task of every phase edited that
one file, and `KernelMain` — the thing a reader opens `kernel.c` to find — stood
at line 8,797.

The tests are now one file per subsystem, declared by
[`../include/oxys/verify.h`](../include/oxys/verify.h), which is the only thing
`kernel.c` needs to know about them. `kernel.c` fell to 708 lines at that change
and is again what its own header block says it is; it grows by a few lines per
sub-task as subsystems are added to the initialisation, which is the only thing
that ought to make it grow at all.

Nothing was rewritten in the move: the assertions, their order and their wording
are as they were, and the serial output after the change differs from the output
before it only in the size of the kernel image and in two counters that jitter
between runs.

## Contents

| Path | Description |
| ---- | ----------- |
| `volume.h`, `volume.c` | The fixture. Two block devices backed by arrays in `.bss`, and a complete EXT2 volume composed within them byte by byte, so that every storage and filesystem assertion holds upon a machine with no disk. The second volume is a copy of the first with the owner of one file altered, so that an assertion can state which volume a path reached. |
| `verify_memory.c` | Phase 2: the physical frame allocator, the paging hierarchy, the virtual address allocator and the heap, per-frame reference counting, the resolution of a copy-on-write fault, and the cloning of an address space. |
| `verify_interrupts.c` | Phase 3: the descriptor table and its gates, the 256 stubs and the uniform trap frame they construct, the dispatcher's routing, and the exception handlers. |
| `verify_faultscreen.c` | Phase 6, sub-task 6.4: what is to be done about each exception — resumed, terminating the program that raised it, or fatal to the kernel — asserted for every vector at **both privilege levels**, which was possible six sub-tasks before anything ran at privilege level 3 because the classification is a pure function of a vector and a selector. And the table of fault screens — that every severe fault has one of its own, that **no two share a title or a colour**, that every title fits the narrowest display this kernel has been handed, and that none has been drawn yet, a screen drawn early leaving a real fault later in the boot with nothing to display. It asserts the table and never draws. |
| `verify_compositor.c` | Phase 6: the clip stack and the blend, upon surfaces composed in memory; and the compositor's damage arithmetic and layer table. |
| `verify_console.c` | Phase 6, sub-task 6.4: the bitmap face against the metrics it was drawn to — no glyph in the spacing columns, one blank glyph, a replacement glyph that is not blank, and **no two glyphs identical**, which is what a copy-and-paste leaves behind and what a pasted picture comment hides — the drawing of a glyph against its own bytes upon a surface in memory, and the four control characters upon the live console. |
| `verify_graphics.c` | Phase 6, sub-task 6.3: the rectangle arithmetic, the confinement of the clip, and the pixel, fill, outline, line and blit — asserted against a surface composed in memory whose pitch exceeds its width, so that a primitive addressing a row by the width writes into padding that holds a sentinel and is caught by name. |
| `verify_framebuffer.c` | Phase 6, sub-task 6.2: that the boot loader honoured the framebuffer request tag, that what it described is self-consistent, that the mapping reaches both ends of the physical memory the adapter scans out of, and that entry 4 of `IA32_PAT` holds write-combining while entries 0 to 3 are untouched. It also paints the pattern a person judges. |
| `verify_usermode.c` | Phase 6: the exchange of one thread for another, and a program of twenty-nine bytes composed, loaded, entered at privilege level 3, and ended by the fault it raised for itself. |
| `verify_lifecycle.c` | Phase 6: `fork`, `execve`, `exit` and `wait` — a process cloned and examined without running anything, and a program that forks twice, replaces one child with a program read from a volume, lets the other end by faulting, and collects what each ended with. |
| `verify_process.c` | Phase 6: the process and thread tables, the per-thread kernel stack and its guard, and that the arena returns to what it held. |
| `verify_elf.c` | Phase 6: the ELF64 loader, upon an image composed in memory so that every field may be made wrong on purpose. |
| `verify_syscall.c` | Phase 6: the system-call dispatch table and the validation of a caller's arguments, without executing SYSCALL. |
| `verify_privilege.c` | Phase 6, sub-task 6.1: the user-mode descriptors and their ordering, the task state segment, the interrupt stack table exercised rather than inspected, and the `SYSCALL` configuration asserted as configuration. It executed `SYSCALL` until sub-task 6.7, whose entry path returns by `SYSRET` and so does not come back to the kernel; that assertion is recorded as lost rather than disguised, and `docs/design/PRIVILEGE.md`, Section 9.4, says why no test hook was added to recover it. |
| `verify_mouse.c` | Phase 6, sub-task 6.5: the mouse's packet decoder, driven directly so that the framing, the nine-bit sign extension, the inverted vertical sense, the confinement of the position and the behaviour of a full buffer are all asserted **without a mouse and without anybody moving one**; and the pointer, upon a surface composed in memory, including that its transparent pixels leave the background alone — without which a pointer drawn as a solid rectangle would pass — and that a pointer at the edge writes nothing into the row padding. |
| `verify_devices.c` | Phases 3 and 4: the 8259A controllers, the request layer above them, the interval timer, the PS/2 keyboard, the 16550 serial adapter, the VGA display, and PCI enumeration. |
| `verify_apic.c` | Phase 6, sub-task 6.12: the parse of the firmware's ACPI tables, the Local APIC, the I/O APIC, and the routing of the device request lines through them once the 8259A pair has been retired. The first three assert what was programmed, every value being read back from the hardware; the fourth lets the interval timer run and counts its ticks, which is the only assertion that establishes the whole path from a device pin to a handler. A controller programmed wrongly reports nothing — it produces a device that is silent, and a silent device is indistinguishable from an absent one. |
| `verify_smp.c` | Phase 6, sub-tasks 6.13 and 6.14: the per-processor data area and the segment base it is reached through, the ticket spinlock and the counted interrupt-disable beneath it, the inter-processor interrupt, the translation-lookaside-buffer shootdown built upon that, and the application processors the shootdown is finally broadcast to. **Upon a machine with one processor a lock that does not lock behaves exactly like a lock that does**, so the first two assert internal state — the tickets, the owner, the counted depth, the interrupt flag — rather than behaviour. The last three assert behaviour: an interrupt a processor sends to itself is delivered like any other; the shootdown test rewrites a page-table entry by hand and invalidates nothing, so that the handler is required to be what repairs a genuinely stale translation; and `KernelVerifyApplicationProcessors` broadcasts a shootdown and reads each target's own service count afterwards, which is the one quantity a kernel that started nobody cannot fabricate. It also checks what each started processor read out of its own task register, descriptor table registers and control registers — `CR0.WP` among them, whose absence upon one processor nothing else in this kernel would ever report — and, upon a machine with one processor, asserts the other side: that nobody was started and that the kernel says which condition declined it. |
| `verify_sched.c` | Phase 6, sub-task 6.15: the per-processor run queues, the affinity that decides which of them a thread may join, the round-robin rotation, and the local timer that takes a processor back when a quantum expires. **A count of admissions is not evidence that anything ran**, so the fixture is four kernel threads that do work and record it, and the assertions are made against what they recorded. Two of them were got wrong first: the rotation was asserted as "slices at least rounds", which is false for a thread that yields into an empty queue; and the fixture originally yielded after every round and did no work, so it completed in microseconds, no two threads were ever runnable at once, and no quantum ever expired. `docs/design/SCHEDULER.md`, Section 7, records both. |
| `verify_string.c` | Phase 7, sub-task 7.1, and **the first file here whose subject is not the kernel**: the nineteen string and memory functions of ISO/IEC 9899:2011, Section 7.24, that `libc/` implements. They are freestanding — they call nothing and depend upon nothing but the C language — so the kernel can host them, and `make verify` is the only thing in this project that can execute anything at all. It is also one of the three files here compiled against the C library's include root, by an explicit rule in the `Makefile`, so that no kernel source can reach `<string.h>` without that rule being edited. The assertions are built around the three failures that give right answers for the inputs anybody tests with: **the signed byte** (a comparison through plain `char` is correct below 128 and wrong above it, and nothing faults), **the byte just past the end** (asserted by a sentinel margin around every destination, `0x5A` because `0x00` and `0xFF` are values these functions legitimately write), and **the empty case**. Its own negative test found a defect in itself; `../../docs/design/LIBC.md`, Section 5.1, records it. |
| `verify_wrappers.c` | Phase 7, sub-task 7.2, and the first of two files here whose subject the kernel cannot call: the C library's system-call wrappers. `SYSCALL` executes at any privilege level, but `SYSRET` returns to privilege level 3 unconditionally, so there is no arrangement in which this kernel calls `OxysWrite` and survives. The test is therefore two tests. The translation of a kernel result into an `errno` is on this side of the instruction and is asserted by calling it — every failure result to its own name, a result beyond the reserved range and `INT64_MIN` to `ENOSYS`, and `errno` never touched by a call that succeeded nor left at zero by one that failed. The invocation is not, and is asserted by **copying the bytes `libc/syscall/invoke.asm` ships** into a program composed here and running them at privilege level 3: seven calls whose results the program sums, ended with a status that carries that sum exactly and the interval timer's count beneath a scale no boot reaches. The second file here compiled against the C library's include root, by a rule of its own in the `Makefile`. `../../docs/design/LIBC.md`, Section 8.7, records the negative test that found its first version worthless. |
| `verify_heap.c` | Phase 7, sub-task 7.3: the C library's heap, and the `brk` system call beneath it. **Two tests again, and the division is the sub-task's rather than the test's.** The allocator's policy calls nothing that can fail outside the C language, so it is given a sixty-four kibibyte region by `OxysHeapAdopt` — a documented interface and not a test hook — and exercised directly: alignment, disjointness proved by writing rather than by comparing addresses, splitting, both directions of coalescing, every path of `realloc`, a `calloc` upon a block deliberately soiled first, and the product that wraps. The single strongest assertion is that **releasing everything leaves the heap as one block of exactly its original size**, which no defect in the size arithmetic can hide from. `brk` cannot be called here at all, and is asserted by a composed program that asks for its break, is refused when it reads there, grows the heap by a page, has the kernel write into that page, reads it back into the log, gives the page up and is refused again — with the kernel's own counts of growths, shrinks and pages checked afterwards, independently of anything the program said. The third file here compiled against the C library's include root. `../../docs/design/LIBC.md`, Section 9.7, records the fourteen negative tests, of which one found a limitation and one found code that did nothing. |
| `program.h`, `program.c` | **Not a self-test.** The composer the two tests above build their programs with: the ELF64 file header, the program headers, and the seven instruction forms such a program needs, each cited to Intel's Volume 2. It exists because there is no compiler to produce a user program until sub-task 7.5 and two tests now need one, and because two copies of an instruction encoder is two places for a byte to be wrong. Every write goes through one bounds check and a refusal is recorded rather than reported at the call site, so a program is composed as a sequence of statements and checked once — a composer that overran its array would otherwise write into whatever the linker placed next, and the failure would surface in an unrelated subsystem long afterwards. |
| `verify_storage.c` | Phase 4: the ATA, AHCI and SD host controller drivers, the generic block layer, and the buffer cache. |
| `verify_ext2.c` | Phase 5: the entry point of the EXT2 self-test. It composes the fixture, asserts the superblock and every refusal a malformed one must meet, and calls in turn the five chapters in `ext2/`, restoring the volume between those that alter it. It was 2,618 lines until the chapters were divided out of it; `../../docs/design/ARCHITECTURE.md`, Section 2.2, records why. |
| `ext2/internal.h` | What those chapters share: their own entry points, `KernelRestoreVolume`, and the two helpers more than one of them judges through. `<oxys/verify.h>` still declares `KernelVerifyExt2` and `KernelReportVolumes` alone, which is the whole of what `KernelMain` knows about any of it. |
| `ext2/format.c` | The superblock, the block group descriptor table and the inode, and the dozen ways a volume may contradict itself — each made wrong on purpose and the volume re-read. |
| `ext2/directory.c` | The directory record and its traversal, and the resolution of a path across symbolic links. Entries are compared by name and by inode number rather than counted, a traversal returning the right number of the wrong entries being what a count cannot see. |
| `ext2/file.c` | The reading of a file's contents through every level of indirection, its holes, and both forms of symbolic link. Every assertion is about *which* block was read. |
| `ext2/write.c` | Everything that alters a volume: allocation from both bitmaps, writing, truncation, and the insertion and removal of names — together with the summaries that must agree with the bitmaps afterwards. |
| `ext2/probe.c` | **Not a self-test.** `KernelReportVolumes`, which examines whatever volume the machine actually carries and asserts nothing. It is a file of its own so that the distinction below cannot be blurred by accident. |
| `verify_vfs.c` | Sub-task 5.8: the virtual filesystem layer, its mounts and its node identity; and `KernelVfsProbeVolume`, which exercises a real volume through the layer. |

## The headers, and which corpus each belongs to

[`../include/oxys/verify.h`](../include/oxys/verify.h) is the one header here
that is in the kernel's public corpus, and it is there because `kernel.c` calls
what it declares: the entry points, in the order they are called, together with
the two things `kernel.c` supplies to the tests — the parsed boot information,
and whether the boot loader's command line names a given option.

[`volume.h`](volume.h) and [`program.h`](program.h) are **not** in that corpus
and are included from beside their implementations, by the rule
[`../../docs/design/ARCHITECTURE.md`](../../docs/design/ARCHITECTURE.md),
Section 2.2, states: the public corpus is what a consumer may depend upon, and
what the parts of one subsystem share between themselves is not that.
`volume.h` declares the fixture — the two block devices, the composed volume's
geometry, and the routines that address a field of it directly — and every file
that includes it is in this directory: the storage tests, which need a device,
and the EXT2 and virtual filesystem tests, which need a volume. `program.h`
declares the composer described above, and has the same three consumers of its
own.

`volume.h` was `../include/oxys/testvolume.h` until the review that moved it.
Nothing outside this directory had ever included it, so the fixture's geometry
was reachable from the kernel proper for no reason but its address; putting it
beside `volume.c` — where `program.h` had sat beside `program.c` all along —
makes the two fixtures consistent and puts the limit where it can be seen.

## The distinction between a test and a probe

Two routines here are not self-tests and are named so that they cannot be
mistaken for one: `KernelReportVolumes` and `KernelVfsProbeVolume`. They examine
whatever volume the machine actually carries and **assert nothing**, there being
nothing to assert about a disk this kernel did not write.

There is a third thing a self-test cannot do, and sub-task 6.2 met it: **nothing
here can establish that anything appeared upon the screen.** A kernel cannot read
its own display back through the eye of whoever is looking at it. The framebuffer
test therefore paints a pattern composed so that looking at it establishes
something — misread channel positions put the bands in the wrong colours, a wrong
pitch skews them, a wrong extent stops them short — and
`docs/project/TESTING-GRAPHICS.md`, Section 1, records the procedure by which a person
judges it.

Their value is the one thing a composed fixture cannot supply. The composed
volume shares this kernel's understanding of the format, so a misreading of the
specification would be composed into it and then asserted against itself. The
probes produce a volume that a tool outside this kernel — `e2fsck`, `debugfs`,
`dumpe2fs` — can judge, and that is how the defect in the recorded deletion time
described in `docs/storage/VFS.md`, Section 11.1, was found: every assertion in
this directory passed, and the volume was nevertheless wrong.

The probes that write are selected by the boot loader's command line and never
run by default. A kernel that wrote to a stranger's disk merely by having been
booted would impose a real cost for nothing. The GRUB entries that set those
options are in [`../../boot/grub/grub.cfg`](../../boot/grub/grub.cfg).

## Specifications implemented

Each test cites the specification of the subsystem it asserts, in its own file
header. The corpus is enumerated in
[`../../docs/project/REFERENCES.md`](../../docs/project/REFERENCES.md).

## Present limitations

1. **Every test runs at every boot.** There is no means of selecting one, and no
   need of one yet; the whole corpus costs a fraction of a second. When it ceases
   to, selection belongs on the boot loader's command line beside the write
   probes.
2. **A test cannot assert a refusal that panics.** Several routines in the kernel
   treat an impossible argument as unrecoverable, and no means of surviving a
   panic exists before the test harness of Phase 7. Where that is so, the test
   asserts the admitting direction and says that it does.
3. **Nothing here is safe against concurrent execution, and it does not need to
   be.** The fixture is a pair of static arrays and the tests write to them. The
   corpus runs upon the bootstrap processor and upon no other: a processor
   started by sub-task 6.14 is parked in a halt loop and reaches nothing here,
   and `KernelVerifyApplicationProcessors` — which is the one routine that
   *observes* another processor — only reads areas that processor wrote and
   sends it an interrupt. Sub-task 6.15 is what could change this, by running a
   test upon a processor that has work; the corpus must then run upon one
   processor by construction, or be given a fixture per processor, and the
   per-processor area sub-task 6.13 built is what the second of those would be
   held in.
4. **The fixture is one volume geometry**: 1024-byte blocks, one block group, 32
   inodes. A volume with several groups is exercised only by the probes against
   real images, which `docs/project/TESTING-SYSTEM.md`, Sections 5 and 6, records
   at both block sizes this kernel accepts.
