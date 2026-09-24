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
first strain upon it. `libc/string.c` asserts the C library and not the kernel,
and it is here anyway, because here is the only place in this project where
anything can be executed at all. Its subject sits at the end of the sequence
rather than within it: it depends upon no subsystem, and no subsystem depends
upon it.

**Sub-task 7.2 is the second strain and a sharper one.** `libc/wrappers.c`
asserts a library whose central instruction the kernel **cannot execute at all**:
`SYSCALL` works at any privilege level, but the `SYSRET` that ends the kernel's
handling of it returns to privilege level 3 unconditionally, so a kernel that
called a wrapper would leave its own entry path as a user program. Half of that
test is therefore an ordinary call and half of it is a program — composed here,
loaded, and run at privilege level 3 with the library's own bytes copied into
it. It sits at the end of the sequence too, but for the opposite reason to
`libc/string.c`: not because it depends upon nothing, but because it depends
upon almost everything.

**Sub-task 7.3 is the third, and it is the one that produced an answer rather
than a strain.** `libc/heap.c` asserts an allocator and the system call beneath
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
`libc/wrappers.c` when this sub-task needed a second one. Two copies of an
instruction encoder is two places for a byte to be wrong, and the second copy
would have been wrong in a way the first one's assertions could not see.

**Sub-task 7.4 is the fourth, and it is where the strain returned in a new
place.** `libc/stdio.c` asserts a buffering policy and a conversion engine, both
of which are ordinary C, and a pair of transfers of which one is a system call.
The seam was named before the code was written — `libc/include/stream.h` — so the
policy could be exercised against a region of memory; but unlike sub-task 7.3
there is no composed program that can assert the other half, because a program
composed byte by byte cannot call `printf`. **That half waits for sub-task 7.5**,
which builds a program from source, and the gap is therefore one sub-task wide
and not one phase wide. It is also the first test placed here by the directory
pattern rather than by a rule of its own in the `Makefile`, that pattern having
been written at the previous reorganisation for exactly this case.

**Sub-task 7.5 is the fifth, and it is where a test stopped needing the
composer.** `libc/startup.c` runs a program built by a compiler, so the
instructions it asserts are the ones a toolchain produced rather than the ones
somebody wrote into an array — and the half of sub-task 7.4 that could not be
asserted from inside this kernel is asserted by that program calling `printf`.
`program.c` remains, because the tests of 7.2 and 7.3 still use it and because a
program composed by hand is the only kind that can be made deliberately
malformed.

**It is also the first test here whose subject reports for itself.** The program
makes its own assertions and ends with the number that failed; this directory's
file checks that number. The division is not duplication: the program can reach
what only a program can reach, and the kernel can tell a program that printed
nothing from a program that passed.

**Sub-task 7.6 is the sixth, and it is where a test began asserting what a
program *did*.** `libc/utilities.c` runs eight programs upon a volume the kernel
composed, and then looks at the volume: that `mkdir` left a directory where there
was none, that `rm` removed a name and left its neighbours alone, that no program
ended holding a descriptor of the machine's. That is evidence of a kind no
earlier test here had, because no earlier program had done anything but print.

**It is also where the limit of the arrangement was found and worked around.** A
negative test removed the copy at the end of the `read` system call, so that the
call reported a count and delivered no bytes, and `make verify` reported nothing
at all — `cat` wrote a buffer it had never been given and exited with zero.
Nothing in this kernel captures what a program printed, so a program whose whole
output is text proves only that it did not fault. The answer is
`userland/file-check`, which *compares*: a file of known contents read byte for
byte, a directory of known entries listed, and twenty refusals asserted by the
**name** of the failure rather than by its sign. `../../docs/design/LIBC.md`,
Sections 12.4 and 12.6.

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
[`../include/oxys/test/verify.h`](../include/oxys/test/verify.h), which is the only thing
`kernel.c` needs to know about them. `kernel.c` fell to 708 lines at that change
and is again what its own header block says it is; it grows by a few lines per
sub-task as subsystems are added to the initialisation, which is the only thing
that ought to make it grow at all.

Nothing was rewritten in the move: the assertions, their order and their wording
are as they were, and the serial output after the change differs from the output
before it only in the size of the kernel image and in two counters that jitter
between runs.

## Why they are grouped, and why the `verify_` prefix went

They were twenty-four files named `verify_*.c` in this one directory until the
review that grouped them. The prefix was doing a directory's work — announcing
"this is a self-test" in a directory where nothing else was one — and the cost of
paying for it twice was that twenty-four files sorted as a single block with no
shape, which is the arrangement a reader has to read all of in order to search
any of. They are now in a subdirectory named for the subsystem each asserts, and
`arch/syscall.c` says what `verify_syscall.c` said with one word fewer.

**The function names did not change.** `KernelVerifySyscall` is still
`KernelVerifySyscall`. The prefix is redundant in a file name, because the
directory already says it; it is not redundant in a symbol, because C has one
namespace and `<oxys/test/verify.h>` declares these beside everything else in the
kernel. It was dropped only where it was repeating something.

The grouping mirrors the kernel's own tree, so a subsystem and the assertions
about it are found the same way: `kernel/arch/x86_64/syscall/` is asserted by
`kernel/test/arch/syscall.c`, `kernel/mm/` by `kernel/test/mm/`, and so on.
`libc/` is the one group whose subject is not the kernel at all, and it is also
the one the `Makefile` treats specially — see
[the section on the headers](#the-headers-and-which-corpus-each-belongs-to)
below and `../../docs/design/ARCHITECTURE.md`, Section 2.5.

## Contents

| Path | Description |
| ---- | ----------- |
| `volume.h`, `volume.c` | The fixture. Two block devices backed by arrays in `.bss`, and a complete EXT2 volume composed within them byte by byte, so that every storage and filesystem assertion holds upon a machine with no disk. The second volume is a copy of the first with the owner of one file altered, so that an assertion can state which volume a path reached. |
| `program.h`, `program.c` | **Not a self-test.** The composer the two tests above build their programs with: the ELF64 file header, the program headers, and the seven instruction forms such a program needs, each cited to Intel's Volume 2. It exists because there is no compiler to produce a user program until sub-task 7.5 and two tests now need one, and because two copies of an instruction encoder is two places for a byte to be wrong. Every write goes through one bounds check and a refusal is recorded rather than reported at the call site, so a program is composed as a sequence of statements and checked once — a composer that overran its array would otherwise write into whatever the linker placed next, and the failure would surface in an unrelated subsystem long afterwards. |
| `mm/memory.c` | Phase 2: the physical frame allocator, the paging hierarchy, the virtual address allocator and the heap, per-frame reference counting, the resolution of a copy-on-write fault, and the cloning of an address space. |
| `arch/interrupts.c` | Phase 3: the descriptor table and its gates, the 256 stubs and the uniform trap frame they construct, the dispatcher's routing, and the exception handlers. |
| `arch/privilege.c` | Phase 6, sub-task 6.1: the user-mode descriptors and their ordering, the task state segment, the interrupt stack table exercised rather than inspected, and the `SYSCALL` configuration asserted as configuration. It executed `SYSCALL` until sub-task 6.7, whose entry path returns by `SYSRET` and so does not come back to the kernel; that assertion is recorded as lost rather than disguised, and `docs/design/PRIVILEGE.md`, Section 9.4, says why no test hook was added to recover it. |
| `arch/syscall.c` | Phase 6: the system-call dispatch table and the validation of a caller's arguments, without executing SYSCALL. |
| `arch/usermode.c` | Phase 6: the exchange of one thread for another, and a program of twenty-nine bytes composed, loaded, entered at privilege level 3, and ended by the fault it raised for itself. |
| `arch/apic.c` | Phase 6, sub-task 6.12: the parse of the firmware's ACPI tables, the Local APIC, the I/O APIC, and the routing of the device request lines through them once the 8259A pair has been retired. The first three assert what was programmed, every value being read back from the hardware; the fourth lets the interval timer run and counts its ticks, which is the only assertion that establishes the whole path from a device pin to a handler. A controller programmed wrongly reports nothing — it produces a device that is silent, and a silent device is indistinguishable from an absent one. |
| `arch/smp.c` | Phase 6, sub-tasks 6.13 and 6.14: the per-processor data area and the segment base it is reached through, the ticket spinlock and the counted interrupt-disable beneath it, the inter-processor interrupt, the translation-lookaside-buffer shootdown built upon that, and the application processors the shootdown is finally broadcast to. **Upon a machine with one processor a lock that does not lock behaves exactly like a lock that does**, so the first two assert internal state — the tickets, the owner, the counted depth, the interrupt flag — rather than behaviour. The last three assert behaviour: an interrupt a processor sends to itself is delivered like any other; the shootdown test rewrites a page-table entry by hand and invalidates nothing, so that the handler is required to be what repairs a genuinely stale translation; and `KernelVerifyApplicationProcessors` broadcasts a shootdown and reads each target's own service count afterwards, which is the one quantity a kernel that started nobody cannot fabricate. It also checks what each started processor read out of its own task register, descriptor table registers and control registers — `CR0.WP` among them, whose absence upon one processor nothing else in this kernel would ever report — and, upon a machine with one processor, asserts the other side: that nobody was started and that the kernel says which condition declined it. |
| `exec/elf.c` | Phase 6: the ELF64 loader, upon an image composed in memory so that every field may be made wrong on purpose. |
| `proc/process.c` | Phase 6: the process and thread tables, the per-thread kernel stack and its guard, and that the arena returns to what it held. |
| `proc/sched.c` | Phase 6, sub-task 6.15: the per-processor run queues, the affinity that decides which of them a thread may join, the round-robin rotation, and the local timer that takes a processor back when a quantum expires. **A count of admissions is not evidence that anything ran**, so the fixture is four kernel threads that do work and record it, and the assertions are made against what they recorded. Two of them were got wrong first: the rotation was asserted as "slices at least rounds", which is false for a thread that yields into an empty queue; and the fixture originally yielded after every round and did no work, so it completed in microseconds, no two threads were ever runnable at once, and no quantum ever expired. `docs/design/SCHEDULER.md`, Section 7, records both. |
| `proc/lifecycle.c` | Phase 6: `fork`, `execve`, `exit` and `wait` — a process cloned and examined without running anything, and a program that forks twice, replaces one child with a program read from a volume, lets the other end by faulting, and collects what each ended with. |
| `proc/directory.c` | Phase 8, sub-tasks 8.3 and 8.4: the working directory, asserted by `dir-check` — which since 8.4 also asserts that a parent's stack survives a child's `execve`, the assertion that isolated the entry-path defect of `SHELL.md`, Section 16.3 — at privilege level 3 upon the root the machine booted with — that a process begins at the root, that `chdir` moves it and `getcwd` reports it canonically, that a relative path is resolved against it by a call that is neither, that a child of `fork` inherits it, and that each refusal is the named one. The kernel asserts the field before the program runs and the program asserts the call. It runs after the root is mounted, as `storage/initrd.c` does, because the program changes into `/bin`. `../../docs/design/SHELL.md`, Section 14. |
| `proc/directory_image.asm` | `dir-check`, carried in the image for the test above. |
| `proc/signal.c` | Phase 8, sub-task 8.7: the signals, the process groups and `waitpid`. The pending set, the dispositions, the default actions and the vectors are asserted upon a process that never runs — the rules a program can only see the consequence of — and then `signal-check` is run at privilege level 3, with children reached on both delivery paths: one that computes and is reached by the timer's interrupt, one asleep upon a pipe and reached by the wake. Since sub-task 9.7 it also asserts `alarm` — SIGALRM ending a `pause`, once; the remainder of a replaced alarm; none in a child of `fork` — and `time`. `../../docs/design/PROCESS.md`, Section 18.5; `../../docs/devices/TIME.md`, Section 10.3. |
| `proc/signal_image.asm` | `signal-check`, carried in the image for `proc/signal.c`. |
| `shell/programs_image.asm` | `env-check`, carried in the image for `shell/parser.c` to write onto the root. |
| `gfx/framebuffer.c` | Phase 6, sub-task 6.2: that the boot loader honoured the framebuffer request tag, that what it described is self-consistent, that the mapping reaches both ends of the physical memory the adapter scans out of, and that entry 4 of `IA32_PAT` holds write-combining while entries 0 to 3 are untouched. It also paints the pattern a person judges. |
| `gfx/graphics.c` | Phase 6, sub-task 6.3: the rectangle arithmetic, the confinement of the clip, and the pixel, fill, outline, line and blit — asserted against a surface composed in memory whose pitch exceeds its width, so that a primitive addressing a row by the width writes into padding that holds a sentinel and is caught by name. |
| `gfx/console.c` | Phase 6, sub-task 6.4: the bitmap face against the metrics it was drawn to — no glyph in the spacing columns, one blank glyph, a replacement glyph that is not blank, and **no two glyphs identical**, which is what a copy-and-paste leaves behind and what a pasted picture comment hides — the drawing of a glyph against its own bytes upon a surface in memory, and the four control characters upon the live console. |
| `gfx/compositor.c` | Phase 6: the clip stack and the blend, upon surfaces composed in memory; and the compositor's damage arithmetic and layer table. |
| `gfx/faultscreen.c` | Phase 6, sub-task 6.4: what is to be done about each exception — resumed, terminating the program that raised it, or fatal to the kernel — asserted for every vector at **both privilege levels**, which was possible six sub-tasks before anything ran at privilege level 3 because the classification is a pure function of a vector and a selector. And the table of fault screens — that every severe fault has one of its own, that **no two share a title or a colour**, that every title fits the narrowest display this kernel has been handed, and that none has been drawn yet, a screen drawn early leaving a real fault later in the boot with nothing to display. It asserts the table and never draws. |
| `gfx/windows.c` | Phase 9, sub-task 9.1: the window manager upon a screen composed in memory — the stack read from the pixel where two windows overlap, the focus, the routing of keys and movements, the binding by a held button, the drag, the close control, since 2026-09-23 the minimise and full-screen controls, the notice to the root and the panel that does not take the focus, the confinement and the two bounds — and the disc of `graphics/draw.c`. Until sub-task 9.2 it also held the demonstration the default entry presents, which is a program now. |
| `gfx/client.c` | Phase 9, sub-task 9.2: the client protocol, asserted by running `window-check` at privilege level 3 upon a manager holding a screen composed in memory, with a kernel thread — the hand — that yields until the program sleeps for an event, reads the pixels it blitted, injects the key it waits for and wakes it; then that the program's ending took its window and left the kernel's own. |
| `gfx/client_image.asm` | `window-check`, carried in the image for `gfx/client.c`. |
| `gfx/mark.c` | Phase 9, beside sub-task 9.6: the mark of `art/logo.h` — that no pixel of its table has more ink than coverage, which the mixing subtracts unsigned; that it has a disc, a figure, an edge and uncovered corners; that it is its own table one to one and the average of four when halved; and that the mix is exactly the ground, the disc and the ink at either end. |
| `proc/init.c` | Phase 9, sub-task 9.3: the adoption of orphans by `init`, asserted upon fixture processes composed in the table, and the two calls `init` rests upon — `power`, refused every process but `init`, and `pause` — asserted by running `init-check` at privilege level 3. It runs **last** of the corpus, which is both the order the machine has and what keeps it from moving the tick the shell's job-control session depends upon; `../../docs/design/INIT.md`, Section 6.2. |
| `proc/init_image.asm` | `init-check`, carried in the image for `proc/init.c`. |
| `libc/config.c` | Phase 9, sub-task 9.4: the configuration format, asserted upon text composed in memory — every rule, and every fault recorded against the line it stands upon — and then `config-check` run at privilege level 3 for the half that opens a file, which also asserts that the files `/etc` ships say what `init` and the desktop read. |
| `libc/config_image.asm` | `config-check`, carried in the image for `libc/config.c`. |
| `libc/icon.c` | Phase 9, sub-task 9.6: the icon format upon bytes composed in memory — every field and every refusal, the composition of version 2 upon a colour (opacity-weighted, so that a reduction brings no black from `ICON_NOTHING`), and the centring of an icon that is not square — and then the file the ramdisk ships, read through the VFS: forty-eight square, with a transparent, an opaque and a partly transparent pixel. |
| `libc/image.c` | 2026-09-23: the image format of `libc/include/image.h` upon bytes composed in memory — every field and refusal, a run crossing the end of its row refused though the file is exactly the length of two rows, a reduction averaged, a cover cut to its middle with rows in order, an enlargement repeated — and then the background the ramdisk ships, read through the VFS, of the drawing's extent, scaled to every row of a 1280 by 800 screen and more than one colour. |
| `libc/time.c` | Sub-task 9.7: `gmtime_r` of `<time.h>` upon 1970, a leap day, 2026-09-23 and 2100, against the date, weekday and day of the year the build host's `date -u` gives, and its refusal of a time before 1970. |
| `dev/devices.c` | Phases 3 and 4: the 8259A controllers, the request layer above them, the interval timer, the PS/2 keyboard, the 16550 serial adapter, the VGA display, and PCI enumeration. |
| `dev/mouse.c` | Phase 6, sub-task 6.5: the mouse's packet decoder, driven directly so that the framing, the nine-bit sign extension, the inverted vertical sense, the confinement of the position and the behaviour of a full buffer are all asserted **without a mouse and without anybody moving one**; and the pointer, upon a surface composed in memory, including that its transparent pixels leave the background alone — without which a pointer drawn as a solid rectangle would pass — and that a pointer at the edge writes nothing into the row padding. |
| `dev/rtc.c` | Sub-task 9.7: the real-time clock's decoding in both data modes and both hour modes — twelve o'clock in each half of the day — its refusal of bytes that are not a date, and days and seconds since 1970 against values the build host's `date -u` gave; then that the clock this machine carries read a time after 2000. |
| `storage/stack.c` | Phase 4: the ATA, AHCI and SD host controller drivers, the generic block layer, and the buffer cache. |
| `storage/ext2.c` | Phase 5: the entry point of the EXT2 self-test. It composes the fixture, asserts the superblock and every refusal a malformed one must meet, and calls in turn the five chapters in `ext2/`, restoring the volume between those that alter it. It was 2,618 lines until the chapters were divided out of it; `../../docs/design/ARCHITECTURE.md`, Section 2.2, records why. |
| `storage/ext2/internal.h` | What those chapters share: their own entry points, `KernelRestoreVolume`, and the two helpers more than one of them judges through. `<oxys/test/verify.h>` still declares `KernelVerifyExt2` and `KernelReportVolumes` alone, which is the whole of what `KernelMain` knows about any of it. |
| `storage/ext2/format.c` | The superblock, the block group descriptor table and the inode, and the dozen ways a volume may contradict itself — each made wrong on purpose and the volume re-read. |
| `storage/ext2/directory.c` | The directory record and its traversal, and the resolution of a path across symbolic links. Entries are compared by name and by inode number rather than counted, a traversal returning the right number of the wrong entries being what a count cannot see. |
| `storage/ext2/file.c` | The reading of a file's contents through every level of indirection, its holes, and both forms of symbolic link. Every assertion is about *which* block was read. |
| `storage/ext2/write.c` | Everything that alters a volume: allocation from both bitmaps, writing, truncation, and the insertion and removal of names — together with the summaries that must agree with the bitmaps afterwards. |
| `storage/ext2/probe.c` | **Not a self-test.** `KernelReportVolumes`, which examines whatever volume the machine actually carries and asserts nothing. It is a file of its own so that the distinction below cannot be blurred by accident. |
| `storage/vfs.c` | Sub-task 5.8: the virtual filesystem layer, its mounts and its node identity; and `KernelVfsProbeVolume`, which exercises a real volume through the layer. Since 8.6, `KernelVerifyVfsPipes`: the pipe from a caller that cannot sleep, which is everything about it but the sleeping. |
| `storage/persist.c` | 2026-09-23: the persistent `/etc` upon the second device of memory, run from within `storage/vfs.c` — found by its whole label and not by a prefix, marked clean when left open, one file seeded and one kept, a file closed upon it found upon the medium and not only in the buffer cache, a name made or removed upon it leaving no dirty buffer, and released clean. It searches only for `oxys-selftest`, so that a machine booted with its real `/etc` disk is never written upon by the test. |
| `storage/initrd.c` | Phase 7, sub-task 7.7, and **the only test here that composes nothing.** Every other file in this directory builds the thing it asserts; this one's subject is the root filesystem the machine actually booted with, which does not exist until `KernelMountRootVolume` has run — so it runs *after* that call rather than among the self-tests, and a test that mounted a ramdisk for itself would establish that a ramdisk can be mounted and say nothing about whether this kernel mounted one. Four groups: that the module arrived and became a device whose geometry is its extent, under a root mounted upon *that* device; that each of the five utilities and the shell in `/bin` is **byte for byte** the copy `libc/utilities_image.asm` — or, for the shell, `libc/line_image.asm` — embedded in this image, the two having been one file at build time, so any difference is something the module, the direct map, the device, the cache, an indirect block or a recorded length did; that `/bin/echo` read from the root loads and runs at privilege level 3 and ends with zero; and that a file may be created upon the root, read back identically and removed. It asserts nothing about what `echo` printed, for the reason `libc/utilities.c` asserts nothing about what `cat` printed. `../../docs/storage/INITRD.md`, Section 7. |
| `libc/string.c` | Phase 7, sub-task 7.1, and **the first file here whose subject is not the kernel**: the nineteen string and memory functions of ISO/IEC 9899:2011, Section 7.24, that `libc/` implements. They are freestanding — they call nothing and depend upon nothing but the C language — so the kernel can host them, and `make verify` is the only thing in this project that can execute anything at all. It is also one of the five files here compiled against the C library's include root, by the `Makefile` pattern over this directory, so that no kernel source can reach `<string.h>` without that rule being edited. The assertions are built around the three failures that give right answers for the inputs anybody tests with: **the signed byte** (a comparison through plain `char` is correct below 128 and wrong above it, and nothing faults), **the byte just past the end** (asserted by a sentinel margin around every destination, `0x5A` because `0x00` and `0xFF` are values these functions legitimately write), and **the empty case**. Its own negative test found a defect in itself; `../../docs/design/LIBC.md`, Section 5.1, records it. |
| `libc/wrappers.c` | Phase 7, sub-task 7.2, and the first of two files here whose subject the kernel cannot call: the C library's system-call wrappers. `SYSCALL` executes at any privilege level, but `SYSRET` returns to privilege level 3 unconditionally, so there is no arrangement in which this kernel calls `OxysWrite` and survives. The test is therefore two tests. The translation of a kernel result into an `errno` is on this side of the instruction and is asserted by calling it — every failure result to its own name, a result beyond the reserved range and `INT64_MIN` to `ENOSYS`, and `errno` never touched by a call that succeeded nor left at zero by one that failed. The invocation is not, and is asserted by **copying the bytes `libc/syscall/invoke.asm` ships** into a program composed here and running them at privilege level 3: seven calls whose results the program sums, ended with a status that carries that sum exactly and the interval timer's count beneath a scale no boot reaches. The second file here compiled against the C library's include root, by a rule of its own in the `Makefile`. `../../docs/design/LIBC.md`, Section 8.7, records the negative test that found its first version worthless. |
| `libc/heap.c` | Phase 7, sub-task 7.3: the C library's heap, and the `brk` system call beneath it. **Two tests again, and the division is the sub-task's rather than the test's.** The allocator's policy calls nothing that can fail outside the C language, so it is given a sixty-four kibibyte region by `OxysHeapAdopt` — a documented interface and not a test hook — and exercised directly: alignment, disjointness proved by writing rather than by comparing addresses, splitting, both directions of coalescing, every path of `realloc`, a `calloc` upon a block deliberately soiled first, and the product that wraps. The single strongest assertion is that **releasing everything leaves the heap as one block of exactly its original size**, which no defect in the size arithmetic can hide from. `brk` cannot be called here at all, and is asserted by a composed program that asks for its break, is refused when it reads there, grows the heap by a page, has the kernel write into that page, reads it back into the log, gives the page up and is refused again — with the kernel's own counts of growths, shrinks and pages checked afterwards, independently of anything the program said. The third file here compiled against the C library's include root. `../../docs/design/LIBC.md`, Section 9.7, records the fourteen negative tests, of which one found a limitation and one found code that did nothing. |
| `libc/stdio.c` | Phase 7, sub-task 7.4: the C library's buffered streams and its formatted conversion. **The first file here whose subject the kernel can assert only in part, and the part it cannot is not left unasserted but deferred one sub-task.** Every stream opened here has a region of memory for a device — `OxysStreamOpenMemoryWrite` and `OxysStreamOpenMemoryRead`, a documented interface and not a test hook — so the whole of the buffering and the whole of the conversion run inside this kernel: the three buffering modes and the moment each empties, a buffer of one byte whose *second* byte is a sentinel, `fwrite` counting whole elements, the sticky error indicator, one character of pushback that clears the end-of-file indicator, a partial line at end-of-file that is kept, and twenty-one conversions compared against their expected characters **and their expected length**. Nothing here may write to `stdout` or `stderr`; the last assertion made is that no byte reached the system, so a change that made one is reported rather than suffered as a reset. `stdin` is read, because its source is ordinary C that reports end-of-file and asserting that it reports an *end* and not an *error* is worth doing. The fourth file here compiled against the C library's include root, and the first added by the directory pattern rather than by a rule of its own. `../../docs/design/LIBC.md`, Section 10.8, records the twenty negative tests, of which four found gaps in these assertions and two found things no assertion here can defend. |
| `libc/startup.c` | Phase 7, sub-task 7.5, and **the first test here whose subject was built rather than composed**. Every program this project had run before it was assembled byte by byte by `program.c`; this one is compiled, linked against the C library's archive, embedded in the image by `libc/startup_image.asm` and loaded from there. That difference is the substance of the test: it asserts the toolchain, the linker script, the startup object, the archive, and every translation unit of the library compiled with a program's flags rather than the kernel's. The assertions are in two places on purpose — the program asserts what only a program can reach (what stands upon its stack, `malloc` obtaining memory from the break, `printf` reaching a descriptor, `exit` calling what `atexit` registered before it flushes) and **ends with the number that failed**, while this file asserts that the image is an ELF this loader accepts, is not absurdly larger than the code within it, is entered at its first instruction, ran, ended, and **ended with a status of zero**. The last is the one the test rests upon: the program's own reporting depends upon the machinery under test, so a test whose only evidence was output would read silence as success. `../../docs/design/LIBC.md`, Section 11.7, records the fifteen negative tests, of which three found a false claim in the documentation rather than a defect in the code. |
| `libc/utilities.c` | Phase 7, sub-task 7.6, and **the first test here that asserts a program by what it did rather than by what it reported**. It composes the EXT2 volume of `volume.h` in memory, mounts it as the root, writes `arg-check` onto it, builds a small tree of files and directories through the filesystem layer — so that a program which fails is not also the thing that prepared the ground — and then runs eight programs at privilege level 3. The assertions fall into four groups: the descriptor table, called directly from within the kernel because two of its properties (that it is emptied and not zeroed, and that a process ending while holding a descriptor gives it back) cannot be reached by a program at all; the argument vector, by `arg-check` run directly and by `exec-check` becoming it through `execve`; the six filesystem calls, by `file-check`, which is the only thing here that compares bytes and error *names* rather than statuses; and the five utilities, by the status each ended with and by what the volume holds afterwards. **Every positive case is paired with a negative one**, a status of zero being the weakest evidence a program can offer. What it cannot assert is what a program printed, which is why the three `-check` programs exist: `../../docs/design/LIBC.md`, Section 12.7, limitation 1, and Section 12.6, where the negative test that proved `file-check` necessary is recorded. Since sub-task 8.5 `file-check` also asserts writing, appending, truncation, `dup2` sharing a position and a child inheriting a descriptor, and `cat` with no operand is given a line upon the terminal to copy; since 8.6 the pipe, twelve kibibytes of it crossing from a child. |
| `libc/line.c` | Phase 8, sub-task 8.1, and **the first test here that asserts what a program would have printed.** The line editor writes through a function it is given, so the test gives it one that appends to an array and compares the bytes every editing key produces — the character echoed, the tail redrawn, the backspaces that return the cursor, the recalled line drawn over the old one — and then asserts the history's order, its ring and its draft. It then places a session of seventy-one bytes upon the terminal's queue and runs `line-check` at privilege level 3, which reads the session through descriptor 0 one byte at a time and compares the lines it edits into. A negative test removed the backspaces after an insertion and `line-check` still passed: the captured output is the only assertion in this project that can see a display that is wrong while the line is right. `../../docs/design/SHELL.md`, Section 5. |
| `libc/line_image.asm` | The two programs of sub-task 8.1 carried in the image: `line-check`, which `libc/line.c` runs, and `sh`, which `storage/initrd.c` compares against `/bin/sh`. |
| `terminal/terminal.c` | Phase 8, sub-task 8.1: the terminal input path. The keyboard decoder is driven with scancodes, as `dev/devices.c` drives it, and the bytes the terminal delivers are compared against what a terminal would send — a character, a control character, the seven control sequences of the cursor, home, end and delete keys — and the queue's order, its bound and its flush are asserted by injection. Nothing here needs a keyboard present. |
| `shell/parser.c` | Phase 8, sub-tasks 8.2 to 8.7: the shell's tokeniser and parser, since 8.3 its assignment words, variables and expansion, since 8.4 the programs it runs — a third session invoking `env-check`, written onto the root for the purpose — and since 8.5 its redirections, a fourth session whose files the test reads back — and since 8.6 its pipelines, a fifth session whose files and status the test reads back, the whole of `/bin/sh` among what crossed a pipe — and since 8.7 its job control, a sixth session with control-C and control-Z among its lines, `cat` interrupted, stopped, continued and killed — asserted against the translation units the shell ships — compiled into this image as the C library's are — with some fifty lines of known tokens and structure: every operator longest first, the `io_number`, the three quotings with the quotes kept, the comment, the subset of the grammar parsed and the remainder refused by name, every bound, and what is incomplete rather than wrong. The shell is then run at privilege level 3 upon a session of eight lines that continues a command across a quote and across a pipe, and is asserted to consume it and end with zero. `../../docs/design/SHELL.md`, Section 9. Since 2026-09-24 a session runs `micro` upon a file, appends a blank line and a text line and saves — the file must hold all three lines, which a refused write of nothing once cut at the blank one, and no `.micro-save` file may be left beside it. |

## The headers, and which corpus each belongs to

[`../include/oxys/test/verify.h`](../include/oxys/test/verify.h) is the one header here
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

`KernelVfsProbeVolume` takes the mount point and the path it is to act upon since
sub-task 7.7, and had both written into it before. The initial ramdisk now holds
the root, so the machine's own volume is mounted at `/mnt` where there is a
ramdisk and at `/` where there is not — and a probe that named a path from the
root would have gone on printing "not present" for ever, which is a diagnostic
that stops saying anything rather than a test that fails.

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
