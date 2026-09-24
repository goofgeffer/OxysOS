<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Present Condition of the System

**Document status**: Living document, revised in every session in which a
functional change is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Where this sits**: [`PLAN.md`](PLAN.md) is the roadmap and the authority upon
what state each sub-task is in. This document says what the system *does* today,
one paragraph to a phase, and where each phase has been observed to work.

**The authority where two documents differ** is the design document cited in each
paragraph, which is revised as the design is. Nothing here restates an argument
made there; each paragraph says what stands, and points at the reasoning.

---

## 1. In one sentence

The kernel boots from a Multiboot2 ISO into long mode at a higher-half address,
manages physical and virtual memory with copy-on-write, services interrupts and
exceptions through the machine's own Local APIC and I/O APIC, drives a serial
adapter, a display, a keyboard, a mouse, three kinds of storage controller and
the PCI bus, mounts and writes an EXT2 volume through a virtual filesystem layer,
draws upon a composited linear framebuffer, and loads and runs a statically
linked ELF64 program at privilege level 3 — which returns to the kernel by system
call, may make a child of itself and collect what it ended with, and is ended
when it faults or when it asks — and, since sub-task 9.1, arranges windows upon
that framebuffer, one of them holding the keyboard and the one beneath the
pointer receiving the mouse. **`Oxys 1 Alpha`, the first release, was cut from
this state on 2026-09-16**: [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md).

**And it runs programs a person would recognise.** Since sub-task 7.6 the kernel
carries fourteen system calls rather than eight (eighteen since sub-task 8.5, nineteen since 8.6, twenty-seven since 8.7, twenty-nine with `link` and `procinfo` added after it, thirty-five with the six window calls of 9.2, thirty-seven with `power` and `pause` of 9.3) — `open`, `close`, `read`,
`readdir`, `mkdir` and `unlink` joining them — each process holds a descriptor
table of its own, and `execve` carries the argument and environment vectors it
had refused since Phase 6. Above those stand `ls`, `cat`, `echo`, `mkdir` and
`rm`, built from source, linked against the C library's archive, and run at
privilege level 3 upon a mounted EXT2 volume.

**And since sub-task 7.7 those programs are *somewhere*.** The ISO carries an
initial ramdisk — an EXT2 image built beside the kernel by `mke2fs` — which GRUB
places in memory as a Multiboot2 module, the frame allocator reserves, the
ramdisk driver presents as the block device `ram0`, and the kernel mounts at the
root before anything else. `/bin/echo` is a file upon it. A volume the machine
carries is mounted at `/mnt`.

**And since sub-task 8.1 a person can type at it.** When the boot finishes the
kernel reads `/bin/sh` off the ramdisk and runs it at privilege level 3; it
prompts, edits a line with the cursor keys, Home, End, Delete and the control
characters of every shell of this lineage, keeps a history of thirty-two lines
the arrow keys walk, and — there being no tokeniser until sub-task 8.2 — answers
each line with a statement that nothing runs it. Beneath it is the thing that had
to exist first: a terminal, one byte stream drawn from the keyboard and the
serial line that a `read` of descriptor 0 waits upon, with the keyboard's keys
translated to the sequences a terminal sends. `stdin` reads.

**And since sub-task 8.6 it runs two programs at once.** `ls | wc` is two
children of the shell with a pipe between them — a page of buffer inside the
kernel, an open file with no node beneath it — the writer asleep while it is
full and the reader asleep while it is empty, each woken by the other. A child
of `fork` joins the scheduler's run queue at the fork and runs when its parent
sleeps or is pre-empted at privilege level 3; `wait` sleeps until a child ends.
The bootstrap processor is still the only one a user thread runs upon, and a
user thread is never pre-empted inside the kernel, which is what keeps the
unsynchronised structures beneath a system call unentered by two threads.

**And since sub-task 8.7 a person can stop what runs.** Control-C ends the
foreground job and control-Z stops it — the terminal turns the two bytes into
SIGINT and SIGTSTP to its foreground process group, by whoever polls next, the
timer tick included — and `jobs`, `fg`, `bg` and `kill` govern what `&` left
running. A signal is a bit the target acts upon on its own way out of the
kernel: ignored, terminating, stopping, or entering a handler on the program's
own stack; `waitpid` reports a child's ending or its stop by kind and number.

**It pre-empts, and it schedules across processors.** Since sub-task 6.15 each
processor holds a run queue of its own with a lock of its own, rotates
round-robin between the threads upon it, and is taken back by a local APIC timer
— calibrated against the interval timer, because the architecture states no rate
for it — when a ten-millisecond quantum expires. A thread is placed upon the
shortest queue its affinity permits, and stays there.

What it does not yet do is run a **user** program upon anything but the bootstrap
processor. The allocators, the process tables and the filesystem layer a system
call reaches are still unsynchronised, and a user thread's affinity mask names
processor 0 alone for exactly that reason — a limitation written as a value in a
field rather than as a rule somebody must remember. Two locks are applied: the
diagnostic channel of sub-task 6.14, and the process and thread tables, which
6.15 made contended.

**A program may now ask for memory.** Since sub-task 7.3 the kernel carries an
eighth system call, `brk`, which moves the boundary of a program's heap — mapping
a zeroed, writable page for each one gained and releasing the frame of each one
given back — and the C library carries `malloc`, `calloc`, `realloc` and `free`
above it. Nothing in this system allocates yet; the heap exists for the things
that will.

**A program may now write to a stream.** Since sub-task 7.4 the C library carries
the buffered input and output of ISO/IEC 9899:2011, Section 7.21 — `stdin`,
`stdout` and `stderr`, the three buffering modes, the transfers of Sections
7.21.7 and 7.21.8, and one conversion engine beneath `printf`, `fprintf`,
`snprintf` and the five other formatted-output names. Nothing in this system
calls them yet, the buffers being static because a stream must be usable before a
heap has been grown. **Nothing could *read* a stream until sub-task 8.1**: 7.6's
`read` reads a file through a descriptor `open` gave out, and the call that
reads the console arrived with the shell — so `stdin` is now the terminal, raw,
and `cat` with no operand still reports the absence rather than copying
keystrokes; [`../design/SHELL.md`](../design/SHELL.md), Section 2.

**Sub-task 7.5 stands at the top of the phase, and it is where the library stops
being something only the kernel can run.** A program is now built rather than
composed: `libc/crt/crt0.asm` takes the argument count, the argument vector and
the environment vector from the stack the kernel prepared, calls `main` with
them, and passes what `main` returned to `exit`; `libc/user.ld` places a program
at four mebibytes with one page-aligned segment per permission; and the same C
library sources are compiled a second time — with a program's flags rather than
the kernel's — into an archive a program links against.

**The kernel gained the half of the process-entry contract it had never had.**
The System V ABI, Section 3.4.1, puts the argument count at the stack pointer,
and this kernel had left that pointer one byte past the last mapped byte of the
stack. Every program composed by hand ignored it; the first conforming `_start`
faults upon it. The frame is six eightbytes, every one of them zero, and since a
stack's pages are already zeroed the whole of the change is where the stack
pointer begins.

**Four limitations closed at once.** The C library has now run at privilege level
3, which nothing in it had; the heap has obtained memory from the break through
`malloc`, which joins two halves that had only been asserted apart; a stream has
reached a descriptor through `printf`, which is the half of sub-task 7.4 the
kernel could not assert at all; and every translation unit has been compiled a
second time, under a second code model, which is a genuine second verification
rather than a formality.

**`exit` calls what `atexit` registered and then flushes**, in that order and not
the other, because a registered function that writes a diagnostic writes it into
a buffer. The program asserts the order from inside itself: it leaves a partial
line in the buffer as `main` returns, and a registered function checks that the
line is still there.

**Since sub-task 7.6 there are programs a person would recognise.** `ls`, `cat`,
`echo`, `mkdir` and `rm` are built by that procedure, linked against that
archive, and run at privilege level 3 upon a mounted EXT2 volume — listing it,
copying files out of it, creating directories within it and removing names from
it. Each is IEEE Std 1003.1-2017's utility as far as this system reaches, and
each refuses the options it does not implement rather than accepting them and
doing nothing. [`../design/LIBC.md`](../design/LIBC.md), Section 12.3.

**A program may now reach the filesystem, and may be given arguments.** The
kernel carries **fourteen** system calls — eighteen since 8.5, nineteen since 8.6, twenty-seven since 8.7, twenty-nine after it — the eight it had, and `open`, `close`,
`read`, `readdir`, `mkdir` and `unlink`, each a validation of a caller's
arguments and then a call of the filesystem layer that has existed since Phase 5.
Each process holds a descriptor table of its own, so that the numbers a program
sees are small and its own and it cannot reach another's open file by guessing
one. And `execve` accepts both vectors, which it had refused since Phase 6 for
want of a convention about where a program finds them: the convention is the
System V ABI's, the strings are copied out of the caller's memory before the
address space they stand in is destroyed, and `_start` has read them since 7.5.

**The count of failure results grew from seven to twenty**, which is not
bookkeeping. Three of the five utilities act upon `errno` and not upon the sign
of a result — `ls` prints an operand reporting `ENOTDIR`, `rm -f` treats `ENOENT`
as success, `mkdir -p` treats `EEXIST` as success — so a kernel that collapsed
the filesystem layer's fifteen causes into two would make all three do the wrong
thing, and each would still exit with a plausible status.

**What none of this asserts is what a program printed.** Nothing in the kernel
captures the diagnostic path, so three of the eight programs exist to turn what
would otherwise be printed-only into a status: `arg-check` compares the vector it
was given, `exec-check` carries a vector across an `execve`, and `file-check`
reads a file of known contents byte for byte and asserts twenty refusals by name.
The last of those was written because a negative test proved it had to, and it
then found a defect that had stood since sub-task 6.11.

## 2. By phase

**Phase 1 — bootstrapping.** The kernel builds without diagnostics under the full
warning regime, is confirmed Multiboot2 compliant by `grub-file`, and boots under
QEMU and VirtualBox alike, presenting its banner upon the console and COM1.
It has also booted from a USB medium upon real hardware, which closed sub-task
1.12 on 2026-09-07; the machine and the run are
[`TESTING.md`](TESTING.md), Sections 5.1 and 5.2. See
[`../design/BOOT.md`](../design/BOOT.md).

**Phase 2 — memory.** A bitmap allocator governs every physical frame; a permanent
hierarchy maps the kernel text and read-only data without write permission and
the whole of physical memory at `0xFFFF800000000000`; a kernel arena and a slab
heap serve allocations of arbitrary size; every frame carries a reference count
and returns to the allocator only upon its last release; and an address space may
be created, cloned by the copy-on-write discipline, activated and destroyed. The
substrate `fork()` will be built upon in sub-task 6.11 is therefore complete. See
[`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md).

**Phase 3 — interrupts.** The interrupt descriptor table is loaded; a stub for
each of the 256 vectors constructs a uniform trap frame whatever the vector; a
dispatch table routes each vector to a registered handler that may alter the
frame it returns through; every architecture-defined exception has a handler that
decodes its error code and a **disposition** deciding whether the fault is
resumed, costs the program that raised it, or is fatal to the machine; and a
device driver claims a request line through one controller-neutral layer that
knows which interrupt controller is answering — the cascaded 8259A pair, remapped
clear of the exceptions, until sub-task 6.12 retires it in favour of the APIC.
See [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md).

**Phase 4 — device drivers.** The serial adapter is interrupt-driven and keeps a
polled path it reverts to whenever the interrupt flag is clear, a panic reporting
with interrupts disabled and needing a channel that will drain. The display is a
formal driver reading its configuration rather than assuming it. PCI is
enumerated through bridges rather than swept. Three storage controllers are
driven — ATA in programmed input/output, AHCI by first-party direct memory
access, and an SD host controller for a machine whose system is upon an embedded
MultiMediaCard part — and where none of them can reach the storage a machine
carries, the report says which controller it found and why. A generic block layer
performs every judgement before a driver is reached, and a buffer cache of
sixty-four blocks stands above it, a buffer's identity being the device and the
block number together. See [`../devices/`](../devices/) and
[`../storage/`](../storage/).

**Phase 5 — EXT2.** A volume is read, written and mounted: the superblock, the
group descriptors, the inodes and every level of their block pointers, directory
traversal, path resolution, both forms of symbolic link, allocation from both
bitmaps, writing, truncation, and the creation and destruction of the names that
reach a file. Above it stands a virtual filesystem layer, and three of its
properties are the substance of the work, each being a decision the obvious
alternative gets silently wrong: **a mount is found through the node it covers
and never through a path prefix**; **a file reached twice is one node**, since two
descriptions of one file silently truncate it; and **a volume opened for writing
is marked unclean before anything else is written to it**, a kernel that marked it
upon unmounting recording only the mounts that ended well. The root volume of a
machine this kernel is booted upon is mounted read-only unless the operator chose
the GRUB entry that permits writing. See
[`../storage/EXT2.md`](../storage/EXT2.md) with the two documents it heads, and
[`../storage/VFS.md`](../storage/VFS.md).

**Phase 6 — graphics, system calls, processes, SMP.** Complete.


- The apparatus a privilege transition is performed out of stands and has been
  exercised: user-mode descriptors in the order `SYSCALL` and `SYSRET` derive
  their selectors by, a task state segment naming the stack entered from user
  mode and a separate stack for the double fault, and an I/O map base beyond the
  segment limit, which is what denies every port to user mode.
- The kernel asks the boot loader for a linear framebuffer and gives its pages
  the write-combining memory type; draws upon surfaces rather than upon the
  framebuffer by name; renders a bitmap face of ninety-five glyphs drawn for this
  project; carries a console that replays what was written before the framebuffer
  could be mapped; draws a full-screen page for each severe fault, one per fault
  rather than one for all; and composites all of it over a back buffer, after
  which **nothing reads the framebuffer**.
- A `SYSCALL` entry path swaps `GS`, loads a kernel stack from a per-processor
  block, dispatches through a table of twenty-nine calls and validates a caller's
  arguments against both the canonical user limit and the paging hierarchy — and
  resolves a copy-on-write fault upon a page it is asked to write, rather than
  refusing an address a fork had protected.
- An ELF64 loader places a statically linked image into an address space, and its
  design is the list of things it refuses to be told.
- A process has an address space of its own and threads with kernel stacks of
  their own beneath guard pages; a switch exchanges six registers and a stack
  pointer; and `IRETQ` descends to privilege level 3.
- **A program may make another program.** `fork` clones its address space by the
  copy-on-write discipline of Phase 2 and gives the child its parent's whole
  register set with `RAX` zeroed; `execve` replaces a process's program with one
  read from a volume; `exit` ends a program upon its own request; and `wait`
  collects what a child ended with. A child runs when its parent waits for it,
  the bootstrap processor having one thread of control; sub-task 6.15 rotates
  threads upon a run queue, and no program has been placed upon one.
- **The machine's own interrupt controllers are in use.** The firmware's ACPI
  tables are found, checksummed and read; the Multiple APIC Description Table
  says where the Local APIC and the I/O APIC are, which processors exist, and
  which of the sixteen ISA request lines have been moved. The Local APIC is
  enabled at both of its two separate enables and completes every interrupt; the
  I/O APIC's redirection table carries each claimed line to the vector it has
  always had; and **the 8259A pair is masked and retired**. A device driver
  observed none of this: it claims a line number through one layer, and that
  layer is the only thing that knows which controller is answering.
- **The mechanisms a second processor needs exist, and there is one.** Each
  processor holds an area of its own, reached in one instruction through
  `GS.base` — a register privilege level 3 cannot write — which the system-call
  path already used for its kernel stack and which now holds the whole of what a
  processor owns. Above it stands a ticket spinlock that admits its waiters in
  arrival order, masks interrupts for as long as it is held, counts the nesting so
  that the flag is restored when the outermost section is left, and panics rather
  than hangs upon the two misuses a lock cannot otherwise report. One processor
  may interrupt another through the local controller's command register, and the
  memory manager uses that to announce a paging-structure change: a
  translation-lookaside-buffer shootdown, waited for until every target has
  acknowledged, because Intel SDM Section 4.10.4.4 permits an invalidation to be
  deferred only while no processor can use the stale translation.
- **Every processor the firmware declares usable is started.** Sub-task 6.14
  sends each an INIT, waits ten milliseconds, sends a startup interrupt naming a
  real-mode trampoline copied to a low page proved available by the Multiboot2
  memory map, and waits for the acknowledgement the trampoline writes on
  reaching 64-bit mode upon the kernel's own paging hierarchy. The started
  processor then loads the kernel's descriptor tables, claims an area, loads a
  task state segment of its own — one per processor, `LTR` refusing a descriptor
  already marked busy — configures its four system-call registers and its own
  local controller, records what it actually holds in each of those registers,
  and parks in a halt loop. The trampoline's identity mapping is removed
  afterwards by the first shootdown this kernel has ever had a target for.
  **A started processor has nothing to run**: it answers inter-processor
  interrupts and halts, which is its whole contribution until 6.15.
- **Three locks are applied, and each was applied where it became necessary.**
  The diagnostic channel, in `KernelWriteString`, covers the four unsynchronised
  structures a parked processor can reach — the text display's cursor, the
  console's rows, the serial transmit buffer and the compositor's back buffer —
  because one function reaches all four and a whole line is what must not
  interleave. Sub-task 6.15 added two more: a lock per run queue, and
  `ProcessTableLock`, which makes the claim of a slot in the process and thread
  tables atomic now that each application processor claims one as it comes
  online. Every other structure that needs a lock says so in its own file's
  header and is safe still, nothing reaching it.
- **Every processor has work, and is taken back when it has had enough.** Since
  sub-task 6.15 each holds a run queue with a lock of its own and rotates
  round-robin between the threads upon it. Pre-emption is a local APIC timer, one
  per processor because the local vector table is per processor, at a rate this
  kernel measures against the interval timer rather than assumes — the
  architecture states none. A thread is placed once, upon the shortest queue its
  affinity permits, and stays there. **A user thread's affinity names the
  bootstrap processor alone**, which is the state of the remaining locks written
  as a value in a field rather than as a rule somebody must remember.

See [`../design/PRIVILEGE.md`](../design/PRIVILEGE.md),
[`../design/GRAPHICS.md`](../design/GRAPHICS.md) and the five documents it indexes,
[`../design/EXECUTABLE.md`](../design/EXECUTABLE.md),
[`../design/PROCESS.md`](../design/PROCESS.md),
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md),
[`../design/SMP.md`](../design/SMP.md),
[`../design/SCHEDULER.md`](../design/SCHEDULER.md),
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md), Section 10,
[`../devices/ACPI.md`](../devices/ACPI.md) and
[`../devices/APIC.md`](../devices/APIC.md).

**Phase 7 — userland and the C library.** Complete. Sub-task 7.1 stands: the
nineteen functions of ISO/IEC 9899:2011, Section 7.24, that do not require a
locale or an `errno`, in [`../../libc/`](../../libc/) under the userland's
permissive licence, divided into four translation units as the standard divides
its own subsections. They are implemented exactly as specified rather than as a
reader might expect — `strncpy` pads and does not terminate, `strchr` finds the
terminator, `strtok` keeps its position in a static object — and every byte is
examined through `unsigned char`, which is the one property whose loss would be
invisible: a comparison through plain `char` is correct for every byte below 128
and wrong for every byte above it, upon a machine whose plain `char` is signed.

**Sub-task 7.2 stands beside it**: a wrapper for each of the seven calls the
kernel implements, the `SYSCALL` instruction beneath them in a translation unit
of NASM, the `errno` of ISO/IEC 9899:2011, Section 7.5, and the `strerror` that
7.1 had left for it — twenty of Section 7.24's twenty-two functions now, the two
absent being the locale's. A wrapper returns `-1` and sets `errno` where the
kernel refused, and returns what the kernel returned where it did not; the
numbers in `errno` are the kernel's failure results negated, and `_Static_assert`
holds the two halves together, so renumbering a result in the kernel's interface
fails the build rather than misreporting a cause.

**The instruction cannot be executed by this kernel**, `SYSRET` returning to
privilege level 3 unconditionally. So the wrappers are asserted where they run:
the invocation contains no relocation, and the boot-time self-test copies the
bytes the library ships into a program composed for the purpose and runs them at
privilege level 3. The log carries a line no part of the kernel composed — the
system's name, fetched by one call and written by another.

**Sub-task 7.3 stands above both**: `malloc`, `calloc`, `realloc` and `free` of
ISO/IEC 9899:2011, Section 7.22.3, over a first-fit allocator upon an
address-ordered free list of thirty-two-byte-headed blocks, which splits a block
too large and joins a released one to whichever of its neighbours lies against
it. Every pointer it returns is aligned for any type with a fundamental
alignment; `malloc(0)` returns a distinct pointer rather than a null one, a null
being indistinguishable from a failure; `calloc` refuses a count and a size whose
product would wrap, which is the one arithmetic fault here with a security
consequence; and `free` of something that is not an allocation is **defined as a
refusal** rather than left undefined, the header carrying an eight-byte mark that
a stray pointer will not accidentally hold.

**Beneath it the kernel gained its eighth system call**, `brk` — the first added
since Phase 6. A program's break begins one guard page above its image and moves
by a call that maps a zeroed, writable, user-accessible frame per page it gains
and releases the frames of every page it gives back. It returns the new break and
reports a failure as a negative result, which the traditional call of that name
does not; and a growth that cannot be completed is undone entire, because a heap
that is part there is a heap whose owner discovers the fact at whichever byte it
happens to touch first.

**The two halves are asserted apart because the sub-task is two things.** The
policy is ordinary C and is exercised directly against a region the self-test
gives it; the call beneath it cannot be executed by this kernel, and is asserted
by a program at privilege level 3 which asks for its break, is refused when it
reads there, grows the heap by a page, has the kernel write the system's name
into that page, reads it back into the log, gives the page up, and is refused
again. Nothing in this system allocates yet — sub-task 7.4 buffers into static
storage, a stream having to be usable before a heap has been grown.

**Sub-task 7.4 stands above that**: the buffered stream of ISO/IEC 9899:2011,
Section 7.21, and the formatted conversion above it. A `FILE` is an incomplete
type, so no program can depend upon what is inside one; `stdin`, `stdout` and
`stderr` exist from the first statement of a program, initialised statically
rather than by anything that has to run first. `stdout` is **line** buffered and
`stderr` **un**buffered, which paragraph 7 of Section 7.21.3 requires of the
second and permits of the first — a fully buffered `stdout` loses the last
partial line whenever a program faults, and that is the line worth having. A
buffer is emptied when it is already full rather than after the byte that fills
it, one character of pushback is guaranteed and clears the end-of-file indicator,
a partial line at end-of-file is returned rather than discarded, and a stream
that has reported its end does not ask its source again.

**One conversion engine stands beneath eight standard names.** `printf`,
`fprintf`, `snprintf` and the rest differ only in where a character goes, so a
conversion cannot be right in one of them and wrong in another. It performs the
five flags, the field width and the precision — each as a digit string or as an
asterisk — the seven length modifiers, and `d`, `i`, `o`, `u`, `x`, `X`, `c`,
`s`, `p` and `%%`. It counts what it produced and not what it stored, which is
what makes `snprintf` with a size of zero a way to measure a result. **A
conversion it does not implement is refused and the refusal is reported**: every
floating conversion, which this system cannot perform at all, and `%n`, which is
refused deliberately — it is the only conversion that writes through a pointer
the format string selects, and a program that hands a received string to `printf`
is a defect this library can decline to arm.

**What can read a stream is nothing.** This kernel has a `read` since sub-task
7.6 and it reads a *file*, reached through a descriptor `open` gave out; no call
reads a console, so the source beneath `stdin` reports end-of-file — an *end*
and not an *error*, which is the distinction every program reading it depends
upon. The buffering above it is real, is exercised against streams whose device
is a region of memory, and is correct on the day a call that reads exists; what
changes then is one function of six lines.

**Sub-task 7.5 stands above all of it, and it is what ends the arrangement every
sub-task before it worked under.** There is now a `crt0`, a linker script, an
archive and a user-mode compilation: `libc/crt/crt0.asm` takes the argument count,
the argument vector and the environment vector from the stack the System V ABI,
Section 3.4.1, describes, calls `main`, and passes what `main` returned to
`exit`; `libc/user.ld` places a program at four mebibytes with one page-aligned
segment per permission; and the C library's own sources are compiled a second
time, without the kernel's code model, into `build/user/liboxys.a`.

**The kernel's half was missing and nothing had noticed.** The ABI puts the
argument count at the stack pointer, and this kernel left that pointer one byte
past the last mapped byte of a stack — which every program composed by hand
ignored, and upon which the first conforming `_start` faults. A stack now begins
six eightbytes lower, and since its pages are already zeroed those eightbytes are
already the frame the ABI names.

**The first program built by this procedure is the thing that asserts it.** It
runs at privilege level 3, prints through the library's own `printf`, makes its
own assertions about what stands upon its stack, about the string functions and
the wrappers, about `malloc` obtaining memory from the break, and about `exit`
calling what `atexit` registered before it flushes — and ends with the number of
them that failed, which the kernel checks independently of anything printed.

**`getenv` was absent and a program had no environment** until sub-task 8.4, this
kernel's `execve` having accepted neither vector until 7.6 and no program having
exported one until the shell did; there is no dynamic linking of any kind; and the three
segments' permissions are given by the linker script and asserted by nothing,
because an address space still cannot be asked what it maps.

**The library is compiled twice, and that is what sub-task 7.5 changed.** It is
still compiled into the kernel image, where the boot-time self-tests assert it —
`make verify` remaining the only thing in this project that can execute anything
at all — and it is now also compiled into an archive that a program links
against, with a program's flags rather than the kernel's. The kernel does not
call any of it and is compiled without the C library's include root in reach, so
that it cannot begin to.

**The system-call header was divided in the same sub-task**, which is a licensing
obligation rather than a tidying: the interface a program is entitled to is now
`kernel/abi/oxys/syscall_abi.h` under the permissive licence, in a second include
root of its own, and the kernel's configuration, frame, dispatch and validation
stay behind. Nothing changed in the move. See
[`../design/LIBC.md`](../design/LIBC.md) and
[`../../LICENSING.md`](../../LICENSING.md), Section 2.1.

**Sub-task 7.7 closes the phase, and it is what makes any of the above findable.**
There is a root filesystem: an EXT2 image of two mebibytes built beside the kernel
by `mke2fs`, carried in the ISO, placed in memory by GRUB as a Multiboot2 module
named `initrd`, reserved from the frame allocator, presented to the block layer as
the device `ram0`, and mounted at `/` before anything else. It holds the five
utilities under `/bin` and an empty `/mnt`. Every program Phase 7 had run before
it came from one of two places — embedded in the kernel image, or written onto a
volume the kernel composed in an array — and both are a self-test's apparatus.

**It adds no filesystem code at all**, and that is the measure of Phase 5 rather
than of this sub-task: the mount is `VfsMountVolume(…, "ext2", …)` and the code
that reads a program off the ramdisk is the code that reads a file off a disk.
What is new is the module tag of Multiboot2, Section 3.6.6; the reservation of
the frames the module occupies, without which the allocator would issue them and
the root would decay under load; a block driver that converses with nothing; and
the decision about which volume is the root.

**The root is chosen by name and mounted for writing.** A kernel that took
whichever device registered first would mount a stranger's disk at the root upon a
machine that carries one, so `ram0` is named; a volume the machine carries is
mounted at `/mnt` instead, read-only unless the operator asked otherwise at the
GRUB menu. The ramdisk itself is writable, because the rule that protects a disk
— that it belongs to somebody and must not be marked unclean merely by being
booted — does not reach a volume this build made and the machine forgets when it
is switched off.

**The image is produced by an implementation this project did not write**, which
is the point of it: e2fsprogs composed the volume the kernel mounts at every boot
in every environment, so Phase 5's superblock, group descriptor, inode, directory
and file-block readers are put against something that shares none of their
assumptions before the banner is printed. The self-test then compares each
utility, byte for byte, against the copy of the same program embedded in the
kernel image — one file at build time, so any difference is something the path
between them did — and reads, loads and runs one of them at privilege level 3.
[`../storage/INITRD.md`](../storage/INITRD.md).

**Nothing pivots.** The ramdisk is the root and stays the root; exchanging it for
a volume upon a disk needs a working directory and a way to move a mount, and the
first of those is sub-task 8.3's.

**Phase 8 — the shell, complete; `Oxys 1 Alpha` cut at its close on 2026-09-16.** Sub-task 8.1 is complete, and it is three
things where its line names one. **The terminal**: `kernel/terminal/terminal.c`,
a queue of a kibibyte filled by polling the keyboard's events and the serial
adapter's characters when a reader asks, the keyboard's cursor, home, end and
delete keys translated to the control sequences of ECMA-48 and xterm, and the
control key collapsed onto a letter as a terminal collapses it. A `read` of
descriptor 0 halts the processor, interrupts enabled, until at least one byte is
queued, and delivers what is — never nothing. It is raw: nothing echoes and
nothing assembles a line. **The line editor**: `libc/line/`, behind
`<line.h>`, which parses the grammar of ECMA-48, Section 5.4, whatever the final
byte, redraws every edit with printable characters, spaces and backspaces alone,
and keeps a ring of thirty-two lines with the draft preserved across a recall.
**The shell**: `/bin/sh`, started by `KernelRunShell` when the boot finishes and
started again when it ends at the end of its input.

**The editor is the first thing here whose output is asserted.** It writes
through a function it is given, and the self-test gives it one that appends to
an array; so the display an editing key produces is compared byte for byte, and
the one negative test that mattered — an insertion that redraws the tail and
does not backspace over it — was caught by that alone, `line-check` passing
because the line was right and only the screen was wrong.
[`../design/SHELL.md`](../design/SHELL.md), Section 5.

**`stdin` reads, and two tests stopped reading it.** `OxysStreamFill` is now
`OxysRead`, as the note it replaced said it would one day be; the kernel's stdio
self-test and `startup-check` both read `stdin` to assert it was at its end, and
both would now execute `SYSCALL` — the first fatally, the second waiting for a
person in the middle of `make verify` — so both stop, and the end-of-file
discipline is asserted upon a memory stream instead.

**What it is not.** A shell: nothing is parsed, expanded or run. A canonical
terminal: `fgets` upon `stdin` delivers keystrokes and `cat` with no operand
still reports the absence. A wait that yields: the `read` halts the processor,
which is right while one program runs upon the bootstrap processor's own flow of
control and wrong the moment there are two. Section 6 of the design document
counts eight.

**Sub-task 8.2 is complete: the tokeniser and the parser.** `userland/sh/lexer.c`
is Section 2.3 of IEEE Std 1003.1-2017 — the operators longest first, the
`io_number`, the comment, the three quotings of Section 2.2 with the quotes
kept upon the word — and `userland/sh/parser.c` is the subset of Section
2.10's grammar the phase acts upon: a list of pipelines of simple commands,
each with its words in order and its redirections with their descriptors, the
conditions `&&` and `||`, the separators `;` and `&`, and `!`. Compound
commands, functions, the here-document and subshells are refused by name;
reserved words are reserved only in command position; nothing is expanded and
nothing allocates. A command that ends inside a quote or after an operator is
continued upon a `> ` prompt and parsed again whole. **The shell describes what
it parsed**, every word unquoted and every redirection named, because there is
nothing yet to run it — and both grammar units are compiled into the kernel
image and asserted there against fifty lines, the shell then being run upon a
session at privilege level 3. [`../design/SHELL.md`](../design/SHELL.md),
Sections 8 to 10.

**Sub-task 8.3 is complete: the built-ins, and the working directory beneath
them.** Every process now holds a working directory — `/` at creation,
inherited across `fork`, kept across `execve` — and two calls, `chdir` and
`getcwd`, move and report it; every path-taking call resolves a relative path
against it in `SyscallCopyUserPath`, the one function they all copy through.
`chdir` stores the canonical form and refuses a file. The shell has variables
in a fixed table, the assignment word `NAME=value`, the expansion of `$NAME`,
`${NAME}` and `$?` in one pass with quote removal, and the four built-ins:
`cd` (with `-`, `HOME`, `PWD` and `OLDPWD`), `pwd`, `export` (listing as
`export NAME=value`) and `exit [n]`. `exit` and `export` are special, `cd` and
`pwd` are not. A command that is not a built-in is answered with 127; `&&`,
`||`, `!` and `$?` act upon the statuses. `ls` with no operand lists `.`.
**The evidence is a status**: `dir-check` ends with its count of failures, and
the shell, run upon a session, ends with the 137 that only every built-in
working composes. [`../design/SHELL.md`](../design/SHELL.md), Sections 11 to 15.

**Sub-task 8.4 is complete: programs run from the prompt.** A command that is
not a built-in is sought upon `PATH` — `/bin` when unset — forked, executed
with the exported variables as its environment, and waited for; `env-check`,
written onto the root by the self-test, finds its arguments with their quotes
removed and its exported variable and not the assigned one; `cat` of nothing
is 1, a name not found is 127, and `echo` upon the default `PATH` is 0.
`getenv` is in `<stdlib.h>`. **The first program the shell ran found a defect
in the system-call entry path** that had stood since 6.7: the caller's stack
pointer was saved in the per-processor block and restored from there, and a
child's `SYSCALL` inside the parent's `wait` overwrote it; a child that exited
from the cloned stack had the same pointer, which is why five sub-tasks never
saw it. It is restored from the per-thread frame now. 8.5's redirection, 8.6's
pipeline and 8.7's job control were each refused or
named rather than pretended until they arrived. [`../design/SHELL.md`](../design/SHELL.md), Sections 16
to 18.

**Sub-task 8.5 is complete: redirection, and the writable file beneath it.**
`open` takes WRITE, CREATE, TRUNCATE and APPEND and a mode; `write` reaches a
file; an open file of the filesystem layer counts its holders, so a child of
`fork` inherits every descriptor, `execve` keeps them, and `dup2` — the
seventeenth call — makes two numbers of one file and one position; `rmdir` is
the eighteenth. The shell performs a command's redirections in the child in
the order written, every operator of Section 2.7 but the here-document, and
`file-check` and a fourth session whose files the kernel reads back assert
them. With the writable file came `touch`, `cp` and `rmdir` in `/bin`, `cat`
copying its standard input, and the built-ins `help`, `true`, `false` and
`unset`; the shell greets nobody, `help` being the built-in for that. A
redirection upon a built-in is still named and not performed.
[`../design/SHELL.md`](../design/SHELL.md), Sections 19 to 21.

**Sub-task 8.6 is complete: pipelines, and two programs running at once.** The
shell runs `a | b | c` as one child per command with a pipe between each pair,
the ends placed by `dup2` before the command's own redirections and closed
wherever they are not needed, every child collected and the status the last
one's or its inverse after `!`; a built-in in a pipeline runs in the child. The
pipe — the nineteenth call — is an open file of the filesystem layer with no
node beneath it and a page of buffer: a reader sleeps while it is empty, a
writer while it is full, a reader whose writers have gone reads the end of the
file, and a writer whose readers have gone is told `EPIPE`, the twenty-first
failure result. Beneath it a child of `fork` joins the run queue at the fork,
`wait` sleeps upon a wait channel, the thread to return to is the started
thread's own field, and the counted interrupt-disable travels with a thread
across a switch; a user thread is pre-empted at privilege level 3 alone; and
the bootstrap processor has an idle thread of its own. `wc` joins `/bin`, and
`help` is a list of every command, one to a line. `file-check` sends twelve
kibibytes through a pipe from a child, the kernel asserts the pipe from a
caller that cannot sleep, and a fifth session of the shell carries `/bin/sh`
through `cat | wc -c`. Beside the sub-task, at the
project owner's request, the line editor `micro` — the first thing that changes
a file rather than writes one — and `clear`, one form feed the displays clear
upon; `SHELL.md`, Sections 25 and 26.
[`../design/SHELL.md`](../design/SHELL.md), Sections 22 to 24;
[`../design/PROCESS.md`](../design/PROCESS.md), Section 17;
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 9.

**Sub-task 8.7 is complete: job control, process groups and the terminal's
signals — and Phase 8 with it; `Oxys 1 Alpha` was cut the same day,
[`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md).** A signal is a bit in the target, set by the sender and
acted upon by the target on its way out of the kernel: ignored, terminating,
stopping until SIGCONT, or entering a handler upon the program's own stack
that returns through a two-instruction restorer; SIGKILL and SIGSTOP cannot
be given a disposition; a call a signal interrupted is made again where no
handler was entered. The terminal holds a foreground process group, turns
control-C and control-Z at the head of its queue into SIGINT and SIGTSTP to
it — by whoever polls next, the bootstrap processor's tick included — and
stops a background reader with SIGTTIN inside its `read`. `waitpid` names a
child, declines to sleep and reports a stop, with the status an encoding of
kind and number; a process's descriptors are released when it ends. The shell
runs every pipeline as a job in a group of its own, `&` leaves one running,
and `jobs`, `fg`, `bg` and `kill` govern them. `<signal.h>` in the C library;
`signal-check` and a sixth shell session assert it. Eight calls, twenty-seven
in all, and after the sub-task `head`, `tail`, `grep`, `sort`, `mv` and `ps`
with the `link` and `procinfo` calls, twenty-nine. [`../design/PROCESS.md`](../design/PROCESS.md), Section 18;
[`../design/SHELL.md`](../design/SHELL.md), Sections 28 and 29;
[`../design/LIBC.md`](../design/LIBC.md), Section 13.

**Phase 9 — the desktop, its system services and its configuration.** Begun:
sub-tasks 9.1, 9.2 and 9.3 are complete, all on 2026-09-17, 9.4 on 2026-09-18
and 9.5 on 2026-09-20; the three after them are not. The
window manager holds a fixed table of sixteen windows upon the compositor's back
buffer, each a content surface its owner draws into and a queue of events its
owner drains, with a frame the manager draws: a flat band, a one-pixel border, a
title, and a disc for the close control — the one curve the primitives now have.
The stack is an array walked from the top for a hit test; the window holding the
focus takes every key and is drawn in the one colour the palette reserves; a
movement goes to the window beneath the pointer, in that window's coordinates;
a press raises and focuses its window and binds the pointer to it until every
button is up, so that a drag ends where it began; a press in the band drags the
window or, upon the disc, asks it to close, and the manager destroys nothing.
It is serviced from the bootstrap processor's tick beside the terminal, which
gives up the keyboard for it. **What the default entry boots changed**: it now
gives the window manager the screen, the keyboard and the mouse, with the shell
upon the serial line and three windows a person can operate — the mark, the
pointer's position with a disc that follows it, and the characters typed — and
the **Shell-only** and **Shell Diagnostics** entries give the shell the screen
as every entry did through Phase 8. **Since 9.2 a process owns a window**:
six calls — thirty-five in all — by which it creates one, moves it, carries a
rectangle of `0x00RRGGBB` pixels into it, asks the screen's size, and reads its
events one at a time from one window or any of its own, sleeping until one
arrives. A window is the process's and `EBADF` to every other, and goes with
the process at its ending. The demonstration is that process, `/bin/windows`,
launched beside the shell and drawing everything it shows from privilege level
3; `window-check` asserts the protocol with a kernel thread reading its pixels
and waking it. [`../design/WINDOWS.md`](../design/WINDOWS.md);
[`../design/DRAWING.md`](../design/DRAWING.md), Section 4.1.

**And since 9.3 there is a first user process.** The kernel starts one program,
`/bin/init`, and tells itself which process that is; `init` starts the desktop,
starts it again whenever it ends, and collects every orphan — a process that
ends now gives its children to `init` rather than leaving them in the table for
the machine's life, which `PROCESS.md` had recorded as owed since 8.7. The
machine can be stopped: `power` halts it or restarts it through the keyboard
controller's reset line, and **only `init` may call it** — `shutdown` finds
`init` by name in the process table and asks it by a signal, so that the desktop
is stopped before the machine is. `pause` was added for an `init` that has
nothing to collect and must not spin. And the default entry's screen is a boot
screen from the moment there is a back buffer until the desktop composes over
it, where it had shown a banner and then nothing; the power screen is its
counterpart. [`../design/INIT.md`](../design/INIT.md).

**And since 9.4 it is told what to run rather than knowing.** `/etc/system.conf`
holds a `[service]` block for each program `init` starts — its path, whether it
is restarted, and whether it needs a display, which is what keeps the desktop
off the entries that give the shell the screen — and `/etc/desktop.conf` holds
the scale and the accent the desktop draws with. The format is a line at a time,
so that a line which cannot be read costs that line and not the file: the faults
are kept with their line numbers and reported, and the parse carries on. The
parser is in the C library, its parsing apart from the file it reads as the line
editor's editing is apart from the terminal. A service that ends five times in a
row is given up on, there being no clock to make that a rate.
[`../design/CONFIG.md`](../design/CONFIG.md).

**And since 9.5 there is a desktop.** A window now stands in one of three
layers — a root beneath everything, the programs' windows, a panel above
everything — and stacks among its own layer and never outside it, so that a
panel cannot be buried by a press nor a root raised over the windows it is
beneath. One program at a time may claim the display, and only that one may make
a root or a panel; every other is refused for being the wrong program. A program
may draw text with the system's one face, which stays in the kernel because the
library is under another licence. `/bin/session` is that program: it paints the
root with the mark the boot screen draws, holds a panel across the top, and
opens a launcher whose entries come from `/etc/session.conf` — from which the
window demonstration and the shell are started.
[`../design/SESSION.md`](../design/SESSION.md).

**And since 2026-09-21 it looks like something**, at the project owner's
direction: a yellow ground, a lighter and more orange yellow for the panel and
for the band of the window holding the focus, a dark brown ink, and the owner's
own mark in place of the ring of discs this project drew for itself. The mark
and the palette are one bitmap and one header in [`../../art/`](../../art/),
public domain so that the kernel — which is LGPL — and the session — which is
MIT — may both draw them, which is what makes the boot screen and the desktop
the same picture rather than two.

The demonstration `/bin/windows` draws takes its paper, its ink and its accent
from that same header, and `/etc/desktop.conf` ships `accent = system` to name
it from a file rather than repeat it. The program held three numbers of its own
until then, and by that day they were a blue from the scheme before this one —
which is what a program looks like when the system changes its mind and the
program does not.

The demonstration's own window draws the mark itself since 2026-09-21, where it
had drawn a ring of discs this project made before there was artwork. The ring
outlived its reason by a day: the boot screen and the desktop carried the
owner's mark while the window a person actually opens carried the stand-in.

**And a machine that loses its userland now says so upon its own screen.** The
desktop entry quiets the console and gives the framebuffer to the window
manager; every path that then gave up — no `init`, no shell, a shell that
faulted — wrote its reason to the serial line while the screen kept a boot
screen nothing was drawing, and the echo loop beneath took out a row of the mark
for each line. The screen is handed back first now, and cleared: the reason
stands at the top of an empty screen with a prompt or an echo loop under it.
[`../design/INIT.md`](../design/INIT.md), Section 5.3.

**And since sub-task 9.6 a person may type at this system's own screen.**
`/bin/terminal` is a window with an unmodified `/bin/sh` beneath it upon a pair
of pipes: the shell edits its line, echoes and prompts exactly as it does upon
the serial line, and does not know it is in a window. What decides where a
character stands is a grid in the C library, asserted by the kernel's self-test
without a window at all. Two things were added for it. **`poll`** — a program
with a window and a pipe to wait upon had no way to wait upon both, and sleeping
in either alone is deafness to the other. And a **refusal**: `tcgroup` from a
process whose standard input is not the terminal is now `ENOTTY`, which is what
stops a shell in a window taking the terminal from the shell at the keyboard.
That shell therefore has no job control — no `fg`, no `bg`, no control-Z — and
control-C is a signal the emulator sends to the group the shell leads. It draws
white upon black, at the project owner's direction, which is the one thing in
this system that does not take its colours from `art/palette.h`: a window of
text read for minutes at a time is not the problem a label upon a panel is.
[`../design/TERMINAL.md`](../design/TERMINAL.md).

**And the launcher draws pictures**, since the same sub-task. An entry of
`/etc/session.conf` may carry an `icon`, which is **a path to a file** the
session reads once at start: four bytes of magic, a version, an extent, and one
`0xTTRRGGBB` pixel per position, the top byte how transparent it is — zero for
opaque, 0xFF for a position the picture does not cover, and since 2026-09-23
(version 2) anything between. A file and not a header, because there is one
icon per program and the set grows whenever somebody edits that file — a
picture compiled in would need the system rebuilt to change. The parsing and
the fitting of an icon to the slot are in the C library and are asserted upon
bytes composed in memory; the file the ramdisk ships is then read and put
through the same parser, because a picture converted at the wrong size, with its
transparency flattened, or with its edge thresholded parses perfectly and draws
wrongly. The terminal's icon is forty-eight pixels square, drawn one to one.
[`../design/SESSION.md`](../design/SESSION.md), Section 8.

**The mark is drawn smooth**, since 2026-09-23: `art/logo.h` is 192 pixels of
coverage rather than ninety-six of three states, drawn one to one upon every
screen of 1024 or wider and averaged upon VirtualBox's 640 by 480, its edge
mixed with the ground. Observed under QEMU, VirtualBox and Bochs.
[`../design/SESSION.md`](../design/SESSION.md), Section 3.2.

**The desktop carries a background, and a window may be minimised and made
full**, since 2026-09-23. The background is a file upon the system's own
filesystem, `/share/backgrounds/background.oxim`, named by `/etc/session.conf`
and scaled by the session to cover whatever screen it has; it is in a
run-length format of its own, `libc/include/image.h`, because the drawing is
three million pixels and seventy kilobytes as runs. Each frame carries a
full-screen and a minimise control beside the close; full gives the window the
screen below the panel and sends its owner the new extent, which the terminal
answers with more rows and columns and the demonstration with a larger mark,
now interpolated; minimise hides it, and the panel lists every window so that
a hidden one can be brought back. The panel no longer takes the focus. Two
calls were added, `window_state` and `window_list`. Observed under QEMU,
VirtualBox and Bochs.
[`../design/SESSION.md`](../design/SESSION.md), Sections 9 and 10;
[`../design/WINDOWS.md`](../design/WINDOWS.md), Section 13.

**The desktop has its utilities, and the kernel knows the date**, since sub-task
9.7. `Files` upon the launcher lists a directory — directories first; a press
selects and a second opens, a directory in the same window and a file in
`/bin/view`, which wraps the text to its window and again when the window is
made full, keeping the top row. The panel carries the time, `HH:MM`, woken at
each minute by an alarm; `/bin/date` prints it with the seconds. The date comes
from the real-time clock, read at every `time` call in either of its data modes
and hour modes; `alarm` sends SIGALRM at a time and is served from the tick.
Observed under QEMU, VirtualBox and Bochs. [`../design/UTILITIES.md`](../design/UTILITIES.md);
[`../devices/TIME.md`](../devices/TIME.md), Section 10.

**`/etc` survives a restart**, since 2026-09-23, upon a machine carrying a disk
labelled `oxys-etc`: it is mounted over the ramdisk's `/etc` at start, seeded
with the files it lacks, written back when a file upon it is closed, and
released clean at `shutdown`; a volume left open by a machine stopped without
one is marked clean and mounted writable. Without such a disk `/etc` is the
ramdisk's, as before. Observed under QEMU, VirtualBox and Bochs.
[`../storage/PERSIST.md`](../storage/PERSIST.md).

**The same day found and fixed a defect a person could reach from the
launcher.** Two processes reading the terminal at once stopped the machine:
each kept the other runnable, neither reached the halt that lets the timer tick
poll the devices, and nothing arrived for either. A read judged the foreground
group once on the way in and never again; it is one loop now, and a reader that
loses the terminal is stopped by SIGTTIN as sub-task 8.7 always meant it to be.
[`../design/SHELL.md`](../design/SHELL.md), Section 2.6.

## 3. Where it has been observed to work

A sub-task marked *implemented* in [`PLAN.md`](PLAN.md) means the code exists and
builds. It does not mean it has been run everywhere. This table says where each
phase has actually been observed, and is the reason those are separate columns
there.

The physical machine is one machine — the HP Laptop 14-dq0052dx specified in
[`TESTING.md`](TESTING.md), Section 5.1.

| Phase | QEMU | VirtualBox | Bochs | OVMF (UEFI) | Physical hardware |
| ----- | ---- | ---------- | ----- | ----------- | ----------------- |
| 1 Bootstrapping | Yes | Yes | Yes — 7.2 | No — no UEFI path until Phase 12 | **Yes**, on one machine — 1.12 |
| 2 Memory | Yes | Yes | Yes — 7.2 | — | Reached, not examined |
| 3 Interrupts | Yes | Yes | Yes — 7.2 | — | Reached, not examined |
| 4 Device drivers | Yes | Yes, the serial adapter included since 7.2 | Yes — 7.2 | — | **Yes, and it found two faults** — see below |
| 5 EXT2 | Yes | Yes | Yes — 7.2 | — | Reached, not examined |
| 6 Graphics and processes | Yes | Yes | Yes — 7.2 | — | Reached, not examined |
| 6.12 The APIC | Yes | **Yes — 7.2** | Yes — 7.2 | — | **Not yet run** |
| 6.13 Concurrency | Yes | **Yes — 7.2** | Yes — 7.2 | — | **Not yet run** |
| 6.14 Application processors | Yes | **Yes — 7.2** | Yes — 7.2 | — | **Not yet run** |
| 6.15 The scheduler | Yes | **Yes — 7.2** | Yes — 7.2 | — | **Not yet run** |
| 7.1 The C library's string functions | Yes | **Yes — 7.2** | Yes — 7.2 | — | **Not yet run** |
| 7.2 The system-call wrappers | Yes | **Yes** | **Yes** | — | **Not yet run** |
| 7.3 The heap and `brk` | Yes | **Yes** | **Yes** | — | **Not yet run** |
| 7.4 The buffered streams | Yes | **Yes** | **Yes** | — | **Not yet run** |
| 7.5 The runtime startup object | Yes | **Yes** | **Yes** | — | **Not yet run** |
| 7.6 The utilities and the filesystem calls | Yes | **Yes** | **Yes** | — | **Not yet run** |
| 7.7 The initial ramdisk | Yes | **Yes** | **No** — the `bochs` upon the `PATH` had reverted to a default build for the fifth sub-task running and cannot execute long mode | — | **Not yet run** |
| 8.1 The terminal, the line editor and the shell | Yes, and driven over the serial line | **Yes, and driven at the PS/2 keyboard** | **Yes — 8.1**, to the prompt | — | **Not yet run** |
| 8.2 The tokeniser and the parser | Yes, and driven over the serial line | **Yes** | **Yes — 8.2**, to the prompt | — | **Not yet run** |
| 8.3 The built-ins and the working directory | Yes, and driven over the serial line | **Yes** | **Yes — 8.3**, to the prompt | — | **Not yet run** |
| 8.4 Programs run from the prompt | Yes, and driven over the serial line | **Yes** | **Yes — 8.4**, to the prompt | — | **Not yet run** |
| 8.5 Redirection, and the writable file | Yes, and driven over the serial line | **Yes** | **Yes — 8.5**, to the prompt | — | **Not yet run** |
| 8.6 Pipelines, and two programs at once | Yes, and driven over the serial line | **Yes, and driven at the PS/2 keyboard** | **Yes — 8.6**, to the prompt | — | **Not yet run** |
| 8.7 Job control, process groups and the terminal's signals | Yes, and driven over the serial line with control-C and control-Z | **Yes, and driven at the PS/2 keyboard** | **Yes — 8.7**, to the prompt | — | **Not yet run** |
| 9.1 The window manager | Yes, at 1280 by 800, the mouse and the keyboard driven through the monitor and the screen captured at each step | **Yes**, at 640 by 480, typed at the PS/2 keyboard and captured | **Yes — 9.1**, at 1024 by 768, to the three windows and the prompt upon the serial line | — | **Not yet run** |
| 9.2 The client protocol | Yes, the demonstration program driven through the monitor and captured at each step | **Yes**, at 640 by 480, the program's windows placed by the screen it asked for | **Yes — 9.2**, `window-check` passed and the prompt reached | — | **Not yet run** |
| 9.3 `init`, the shutdown and the boot screen | Yes: the boot screen captured, the desktop started by `init`, the desktop killed and started again, `shutdown` halting and `shutdown -r` restarting | **Yes**, the desktop started by `init` at 640 by 480 | **Yes — 9.3**, seventy assertions and the prompt reached | — | **Not yet run** |
| 9.4 The configuration and `/etc` | Yes: the desktop's accent changed in `/etc/desktop.conf` and seen to change, and `init` seen to give up on a service whose program is missing | **Yes**, seventy-one assertions | **Yes — 9.4**, seventy-one assertions and the prompt reached | — | **Not yet run** |
| 9.5 The session | Yes: the root and panel captured, the launcher opened and a program started from it, and a window dragged upward seen to pass under the panel | **Yes**, at 640 by 480 — and it is where the panel's height was found to fall below the least extent a window may have | **Yes — 9.5**, seventy-one assertions and the prompt reached | — | **Not yet run** |
| 9.6 The terminal emulator | Yes: the launcher opened with the terminal's icon beside its name, `Terminal` chosen, and `ls` and `echo hi` typed at the window with their output drawn in it | **Yes**, at 640 by 480, the boot reaching the desktop | **Yes — 9.6**, seventy-five assertions and the prompt reached | — | **Not yet run** |
| 9.7 The file manager, the text viewer and the clock | Yes: `Files` opened from the launcher, `/etc` entered and `system.conf` opened in the viewer, paged and made full with the text wrapped again; the panel's clock read the host's minute and turned over by itself | **Yes**, at 640 by 480, the boot reaching the desktop | **Yes — 9.7**, seventy-seven assertions and the prompt reached | — | **Not yet run** |

**The rows marked "— 7.2" were all established by two boots of one image**,
because a boot runs every self-test in the corpus and a clean one is therefore
evidence about every phase at once. Both reported 55 assertions passed or sound
and no verdict of `FAILED`: VirtualBox 7.2.0 with two processors, and Bochs 3.1
likewise. **The same is true of sub-task 7.3's row**, established by the boots of
the heap image: every row above it was re-confirmed by those boots and none is
restated here, a clean boot being evidence about the whole corpus and not about
the sub-task that prompted it. [`TESTING-RECORD.md`](TESTING-RECORD.md) holds
what each run reported.

**Sub-task 7.4's image was run in all three environments.** It
booted under QEMU, under VirtualBox 7.2.0 configured as
`make run-vbox` configures it, and under Bochs 3.1 built with `--enable-x86-64`
and `--enable-smp` — fifty-seven assertions passed or sound in each, and no
verdict of `FAILED` in any. The Bochs installed upon this machine was the default
build again, reporting no processor above `atom_n270`, exactly as
[`TESTING.md`](TESTING.md), Section 4A, says it will be; it was rebuilt from
source with the configuration recorded there.

**Sub-task 7.5's image was run in all three environments likewise.** It
booted under QEMU, under VirtualBox 7.2.0 and under Bochs 3.1, with fifty-nine
assertions passed or sound and no verdict of `FAILED` in any — and in each of the
three the log carries lines written **by a program**, through the C library's own
`printf`, at privilege level 3. The Bochs binary had reverted to a default build
for the third sub-task running and was rebuilt again;
[`TESTING.md`](TESTING.md), Section 4A.

**Sub-task 7.6's image was run in all three likewise**, with sixty assertions
passed or sound and no verdict of `FAILED` in any. In each of the three the log
carries the output of **eight programs** run at privilege level 3 upon an EXT2
volume this kernel composed — a directory listed, two files copied out of it, a
directory created and a name removed — and in each, `arg-check` and `file-check`
report zero failed comparisons of their own. Bochs raised no architectural
objection; the only entries in its own log are the PC speaker declining
`/dev/console`. **The Bochs binary had reverted to a default build for the
fourth sub-task running** and was rebuilt again, which is beginning to be a
property of the environment rather than an accident;
[`TESTING.md`](TESTING.md), Section 4A.

**Sub-task 7.7's image was run under QEMU and under VirtualBox 7.2.0**, with
sixty-one assertions passed or sound and no verdict of `FAILED` in either. In
both, the root filesystem is the initial ramdisk, the five utilities stand in
`/bin` byte for byte as they were built, and the log carries a line written by
`/bin/echo` — read off that filesystem, loaded, and entered at privilege level 3.
It was run under QEMU a second way, with an EXT2 disk attached and the `EXT2
write self-test` entry selected, to establish that a machine carrying a volume
still reaches it: the volume was mounted at `/mnt`, written through the
filesystem layer, read back identically, withdrawn and mounted afresh read-only,
and `e2fsck -fn` upon the image afterwards reported no error.

**Sub-task 8.1's image was run in all three environments, and typed at in two
of them.** Sixty-three assertions passed or sound and no verdict of `FAILED` in
any; in each, `line-check` read a session of seventy-one bytes through
descriptor 0 at privilege level 3 and reported zero failures, and the boot ended
at the shell's prompt rather than at the echo loop. Under QEMU the shell was
driven over the serial line by a script — a line typed, edited with Home, Right,
Delete and End, recalled with the arrow keys, and the shell ended with control-D
and started again. Under VirtualBox 7.2.0 it was driven at the **PS/2 keyboard**
by `VBoxManage controlvm keyboardputscancode`, so the path from a scancode
through the 8042, the decoder, the terminal's translation and the `read` to the
editor's redraw upon the framebuffer console was exercised whole, and a
screenshot shows `oxys$ hxi` drawn where `hi`, Left, `x` had been typed. Bochs
3.1 — the `bochs` upon the `PATH` a default build for the sixth sub-task running,
the source-tree build of Section 4A used instead — reached the prompt with every test
clean and was not typed at, its serial channel being a file.

**It was not run under Bochs, and the reason is the one this project has recorded
four times.** The `bochs` upon the `PATH` had reverted to a default build again —
for the fifth sub-task running — and a default build offers no processor model
above `atom_n270` and refuses `count=2` outright, every model in its list being
32-bit. It cannot execute long mode and therefore cannot run this kernel at all.
The position is stated rather than a run being claimed;
[`TESTING.md`](TESTING.md), Section 4A, holds the configuration a usable build
needs, and [`TESTING-RECORD.md`](TESTING-RECORD.md) holds the dated row.

**Four of Bochs's self-tests failed on the first attempt and none of them was
the kernel's.** The `bochsrc` named the system BIOS where it should have named
the VGA BIOS, so the machine had no VBE for GRUB to honour the framebuffer
request through; the display, framebuffer and compositing tests were asserting
against hardware that was not there. Corrected, the run is clean.
[`TESTING.md`](TESTING.md), Section 4A, records the symptom set, because it is a
failure that looks exactly like a regression.

**The build register was cleared on 2026-09-13**, at the project owner's
direction: every row removed, every archived image deleted, and no build recorded
again until `Oxys 1 Alpha` at sub-task 8.7. The runs described above happened and
their results stand — [`TESTING-RECORD.md`](TESTING-RECORD.md) holds what each
reported — but there is no longer a numbered image to point at, which is why none
of the paragraphs above names one. **Numbering restarts at 1**, so the numbers
`TESTING-RECORD.md` cites will one day name different images and the date in each
row is what tells them apart. [`BUILDS.md`](BUILDS.md) records the decision and
what the restart costs.

**The VirtualBox run carried its whole boot log over the serial adapter**, 6,927
bytes by interrupt, so the automated assertion is available there and not only
under QEMU; [`TESTING.md`](TESTING.md), Section 4.1. **Bochs needs a build of its
own**: configured without `--enable-x86-64` it presents a processor with no long
mode and cannot run this kernel at all, which is what the rebuild described in
Section 4A of that document exists to avoid.

**Sub-task 6.12 has its own row because it is the change most likely to differ by
machine.** Everything it does is programmed from tables the firmware wrote, and
no two firmwares write the same tables. Several paths this kernel now contains
have never been taken by any run: the XSDT, no machine having yet presented one;
the Local APIC Address Override; a second I/O APIC; and every interrupt source
override but the five QEMU declares. They are written from the specification and
asserted only so far as a machine that does not exercise them permits.

Nothing here has been run anywhere but QEMU, and it is recorded as such rather
than assumed from a sibling row.

**Sub-task 6.13 has its own row for a different reason.** What it changed that a
machine could disagree about is the segment base: `GS.base` now holds a structure
the kernel reads on every lock, and the interrupt entry path exchanges it
conditionally upon the privilege level it was entered from. A firmware or a
virtual machine that differs in how it leaves those registers, or a processor
whose `CPUID` initial APIC identifier differs from what its local controller
reports, would show itself here and nowhere else. Under QEMU every assertion
passes and the shootdown was observed to repair a genuinely stale translation.

**Sub-task 6.14 has its own row for the same kind of reason, and a stronger one.**
Everything it does depends upon what the firmware declares and upon how the
processors actually answer, and neither is the same on two machines. QEMU is
started with `-smp cores=2` and declares two processors; a machine that declares
more exercises paths no run has taken, and a machine whose firmware reserves the
low page the trampoline requires exercises the refusal rather than the bring-up.
The startup protocol's ten-millisecond delay, the second startup interrupt sent
only where the first was unanswered, and the hundred-millisecond bound upon the
wait are all timing against real silicon under QEMU and timing against an
emulator's approximation of it here. Under QEMU one application processor starts,
comes online, and answers a shootdown from within its own handler.

**Sub-task 6.15 has its own row because its quantum is a measurement.** The local
timer's rate is not stated by the architecture — Intel SDM, Volume 3A, Section
10.5.4, gives it as the bus clock or core crystal divided by the divide
configuration register — so this kernel measures it against the interval timer at
every boot. Under QEMU that measurement lands at about 62,600 counts per
millisecond at divide-by-sixteen, which is the 1 GHz bus clock QEMU presents; a
real machine will produce a different figure, and a machine whose interval timer
is inaccurate will produce a wrong one. The refusals are what stand between a bad
measurement and a quantum computed from it: the kernel declines to start any
timer and says so, and runs unpre-empted rather than upon an invented rate. That
path has been exercised — it is what the first run of this sub-task did — but not
upon hardware.

**Sub-task 7.1 has a row although nothing about it is machine-dependent**, and
saying why is the point of recording it. These are nineteen freestanding
functions: they call nothing and depend upon nothing but the C language, so a
machine has almost no way to disagree with them. What has never been run
anywhere is the code **as a program will use it** — compiled with the flags a
user program requires rather than the kernel's, and executed at privilege level
3. That is sub-task 7.5, and it is a genuine second verification rather than a
formality. [`../design/LIBC.md`](../design/LIBC.md), Section 6, limitation 4.

"Reached, not examined" means the kernel ran that far upon the machine — it must
have, the storage report of Phase 4 coming after all of it — but nothing about
those phases was inspected or recorded there. It is not evidence that they are
correct upon real hardware; it is only evidence that they did not stop it.

**Sub-task 1.12 is closed**, upon the criterion the project owner set on
2026-09-07: **one machine, booted from a USB medium, with the boot log read and
recorded**. It is met by the run described above. The criterion is written down
because it was a judgement rather than a deduction — a stricter one, several
machines or a serial capture, would have been equally defensible — and a
sub-task closed against an unrecorded standard cannot be reopened against one.

**No serial capture was possible, and that is part of the result.** The machine
has no 16550 for the kernel to find and no USB stack exists to drive an adapter,
so the log was read from the graphical console of sub-task 6.4 — the same
condition VirtualBox presents. [`TESTING.md`](TESTING.md), Section 5.2, records
what follows from it, including that the automated assertion of Section 1 cannot
be performed there.

Three qualifications, each of which cost something to learn:

**VirtualBox has no serial adapter this kernel detects**, so between sub-tasks
6.2 and 6.4 it had no readable diagnostic output at all — the framebuffer having
displaced the text console and the serial port being absent. Sub-task 6.4's
graphical console is what restored it.
[`TESTING.md`](TESTING.md), Section 4.1.

**The storage drivers are the one part real hardware has changed the design of**,
and the evidence was of failure rather than of success. One machine has run this
kernel — an
**HP Laptop 14-dq0052dx**, Intel Celeron N4120, 4 GB of memory, 64 GB of eMMC
storage and no disk of any other kind, booted from a USB drive — and it reported
no disk. The cause was neither a fault in the driver nor an absent disk: it was a
controller in AHCI mode, whose registers are memory-mapped and which answers at
no I/O port; and then, upon the same machine, no mass-storage controller of any
class whatever, its system being upon an eMMC part. Sub-tasks 4.7 and 4.8 were
added in consequence. The machine is specified in [`TESTING.md`](TESTING.md),
Section 5.1, and the diagnosis is in
[`../storage/DISK.md`](../storage/DISK.md), Sections 2.1 to 2.3.

**`make run-uefi` is expected to fail** and is provided in advance so that the
UEFI work of Phase 12 has an established point of entry. Sub-task 12.7 renders it
functional. [`TESTING.md`](TESTING.md), Section 3.

## 4. How anything here is asserted

There is no test harness and there will be none before Phase 7, there being no
userland to run one in. The kernel therefore asserts its own properties at boot,
in the order the subsystems are initialised, and `make verify` fails if any of
them reports a failure. Seventy-eight assertions presently report passed or sound.

Those tests are in [`../../kernel/test/`](../../kernel/test/), one file per
subsystem. Each subsystem's design document carries a table pairing every
property its self-test asserts with the silent failure that assertion exists to
catch; that table, and not this document, is where the reasoning lives. See
[`../../kernel/test/README.md`](../../kernel/test/README.md) and
[`TESTING.md`](TESTING.md).

**A self-test is not the same as a probe.** Two routines examine whatever volume
the machine actually carries and assert nothing, there being nothing to assert
about a disk this kernel did not write. Their value is the one thing a composed
fixture cannot supply: a fixture shares this kernel's understanding of the
format, so a misreading of the specification would be composed into it and then
asserted against itself.

## 5. What is known to be absent

These are the limitations that cross subsystem boundaries. Each subsystem's own
design document ends with its particular ones.

| Absent | Arrives at |
| ------ | ---------- |
| A user program upon anything but the bootstrap processor. Every user thread's affinity mask names processor 0 alone, because the allocators, the process tables and the filesystem layer its system calls reach are still unsynchronised. `SCHEDULER.md`, Section 4, and `CONCURRENCY.md`, Section 10, limitation 1. | Phase 7 |
| ~~More than one program at a time.~~ **Arrived at 8.6**: a child of `fork` joins the run queue at the fork, `wait` sleeps until a child ends, and a pipeline's commands run beside one another. Upon the bootstrap processor alone, and never pre-empted inside the kernel. `PROCESS.md`, Section 17; `SCHEDULER.md`, Section 9. | — |
| ~~A wait that yields the processor.~~ **Amended at 8.6**: `wait` and the pipe sleep upon the wait channel `SCHEDULER.md`, Section 9, added; a `read` of the terminal yields to whatever is runnable and halts only when nothing is, rather than sleeping, because its bytes arrive through an interrupt handler and nothing yet wakes a thread from one. `SHELL.md`, Section 22.3. | 8.7, for the wake from a handler |
| ~~A signal.~~ **Arrived at 8.7**: SIGPIPE, control-C and control-Z, `&`, `jobs`, `fg`, `bg` and `kill`. What remains is a control-C that does not flush the typed-ahead input, and an orphan collected by nobody. `SHELL.md`, Section 29; `PROCESS.md`, Section 19. | Phase 9, for the orphan |
| A canonical terminal. `stdin` is raw: `fgets` delivers keystrokes and control sequences, unechoed, and `cat` with no operand still reports the absence. The shell wants raw; nothing yet wants the other. `SHELL.md`, Section 2.1. | When something wants it |
| Synchronisation **applied**, beyond three structures. The diagnostic channel was locked at 6.14; the run queues and the process and thread tables at 6.15. Every other shared structure is still unsynchronised and still says so in its own file's header; `CONCURRENCY.md`, Section 10, limitation 1, enumerates them. | Phase 7 |
| ~~A program that owns a window.~~ **Arrived at 9.2**: six calls, and the demonstration is such a program. What remains is a face a program can draw text with, which 9.6 needs, and a shared mapping in place of the copy. `WINDOWS.md`, Section 12. | 9.6 for the face |
| A desktop: a root beneath every window, a panel above them, and a way to start a program from the screen. The default entry presents three demonstration windows upon a bare ground. | 9.5 |
| The shell upon the screen and the window manager at once. With the window manager the shell is upon the serial line; a terminal emulator window is what puts it back upon the screen. | 9.6 |
| ~~An orphan collected by nobody.~~ **Arrived at 9.3**: a process that ends gives its children to `init`, which collects them. What remains is that nothing but the desktop is told to stop at a shutdown, and that `init` may itself be killed. `INIT.md`, Section 7. | — |
| ~~A service configuration.~~ **Arrived at 9.4**: `/etc/system.conf` names the services and `/etc/desktop.conf` the desktop's appearance. What remains is that nothing in `/etc` survives a reboot, the ramdisk being memory, and that the kernel reads none of it. `CONFIG.md`, Section 7. | — |
| ~~A window a person can put away.~~ **Arrived on 2026-09-23**: minimise, full screen and a list of windows upon the panel; and at 9.7 a clock beside them. What remains is a resize by hand. `WINDOWS.md`, Section 13.7. | — |
| ~~The date.~~ **Arrived at 9.7**: the real-time clock, read at every `time` call. What remains is a time zone, a century, and any way to set the clock. `TIME.md`, Section 10.5. | — |
| A reaper. A kernel thread that finishes cannot free its own stack — it is standing on it — and nothing else does. Its slot and its four pages are held until the machine stops. | Phase 7 |
| Migration, work stealing, and more than one priority. A thread is placed once, at admission, upon the shortest queue its affinity permits, and stays there. | Later |
| `CR4.SMEP`, `CR4.SMAP` and `IA32_EFER.NXE`. The user mappings that would be protected now exist. | 13.3 |
| A UEFI boot path. | Phase 12 |
