<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oxys-OS Development Plan

**Document status**: Living document. This file is the single source of truth for
task tracking and shall be updated in every session in which a functional change
is made, in accordance with `PROJECT_GUIDELINES.md`, Section 7.

**Target architecture**: x86_64.
**Boot protocol**: Multiboot2 (legacy BIOS, GRUB) initially; native UEFI added in Phase 12.
**Kernel model**: Monolithic.

## What is being built

A monolithic, Unix-like operating system for x86_64, whose kernel and userland
are written from scratch in ISO C11 and NASM assembly, in thirteen phases ordered
by dependency. Each phase is divided into atomic sub-tasks, and every milestone
must be bootable and testable.

**The long-term objective is that Oxys-OS should build Oxys-OS.** That lies
beyond the thirteen phases and is recorded here so that the work leading to it is
not quietly foreclosed; the section [Beyond the thirteen
phases](#beyond-the-thirteen-phases--self-hosting) sets out what it means, what
it depends upon, and the decision already taken about how the compiler is got.

## Where we are
**Phases 1 to 5 are complete.** Sub-task 1.12 closed on 2026-09-07: the kernel
has booted from a USB medium upon real hardware, and the boot log was read there.
**Phase 6 is complete**: a statically linked ELF64 program is loaded into an
address space of its own, entered at privilege level 3, returned to by `SYSRET`
when it makes a system call, and ended when it faults or when it asks — and it
may make a child of itself upon the copy-on-write substrate of Phase 2, replace
that child's program with one read from a volume, and collect what it ended with.
Since sub-task 6.12 the machine's own interrupt controllers are the Local APIC
and the I/O APIC, programmed from what the firmware's ACPI tables declare; the
8259A pair is masked and retired. Since sub-task 6.13 each processor holds a
per-processor area of its own, reached through `GS`; there is a ticket spinlock
that masks interrupts for as long as it is held; one processor can interrupt
another; and a paging-structure change is announced by a
translation-lookaside-buffer shootdown that waits to be acknowledged. Since
sub-task 6.14 every processor the firmware declares usable is started by an
INIT-startup-startup sequence into a real-mode trampoline and carried into
64-bit mode upon the kernel's own paging hierarchy. **Since sub-task 6.15 those
processors have work**: each holds a run queue of its own with a lock of its own,
rotates round-robin between the threads upon it, and is taken back by a local
timer calibrated against the interval timer when a ten-millisecond quantum
expires.

**Phases 7 and 8 are complete, and `Oxys 1 Alpha` is cut.** Sub-task 7.1 is complete: the nineteen string and memory
functions of ISO/IEC 9899:2011, Section 7.24, that do not require a locale or an
`errno`, in [`../../libc/`](../../libc/) under the userland's permissive licence
— and, because 7.2 could not be written until it was done,
`kernel/include/oxys/arch/syscall/syscall.h` divided into the interface a program is entitled
to and the implementation it is not.

**Sub-task 7.2 is complete**: a wrapper for each of the seven calls the kernel
implements, the `SYSCALL` instruction beneath them in a translation unit of NASM,
the `errno` of ISO/IEC 9899:2011, Section 7.5, and the `strerror` that 7.1 left
for it. **The instruction cannot be executed by the kernel** — `SYSRET` returns
to privilege level 3 unconditionally — so the invocation is asserted by copying
the bytes the library ships into a program composed for the purpose and running
them there. [`../design/LIBC.md`](../design/LIBC.md), Section 8.

**Sub-task 7.3 is complete**: `malloc`, `calloc`, `realloc` and `free` of ISO/IEC
9899:2011, Section 7.22.3, above a first-fit allocator over an address-ordered
free list — and beneath it the **eighth system call**, `brk`, the first added
since Phase 6, by which a program asks this kernel for memory and gives it back.
The sub-task divides in two because it has to: the policy is ordinary C and is
asserted by calling it against a region the self-test supplies, while the call
beneath it cannot be executed by this kernel at all and is asserted by a program
at privilege level 3 which grows its heap, has the kernel write into the page it
gained, reads it back, and gives the page up.
[`../design/LIBC.md`](../design/LIBC.md), Section 9.

**Sub-task 7.4 is complete**: the buffered stream of ISO/IEC 9899:2011, Section
7.21, the three standard streams, and one conversion engine beneath the eight
formatted-output names — so that a conversion cannot be right in `printf` and
wrong in `snprintf`. The division is the one 7.3 made, applied a third time, and
this time the kernel cannot make the second half at all: the buffering and the
conversion are asserted against streams whose device is a region of memory, and
the transfer beneath them executes `SYSCALL`, so it is asserted by the program of
7.5 and by nothing before it. **It does not allocate**, and the forward reference
that said it would was wrong: a stream must be usable before a heap has been
grown, so its buffers are static. [`../design/LIBC.md`](../design/LIBC.md),
Section 10.

**Sub-task 7.5 is complete**: `_start`, the termination functions it ends
through, the linker script and archive a program is built against, and — beneath
all of it — the six eightbytes the System V ABI requires upon a stack before a
program's first instruction, which this kernel had never left room for. **It is
the first thing here asserted by a program that was built rather than composed
byte by byte**, and it closes four limitations at once: the C library has now run
at privilege level 3, the heap has obtained memory from the break through
`malloc`, a stream has reached a descriptor through `printf`, and every
translation unit has been compiled a second time with a program's flags rather
than the kernel's. [`../design/LIBC.md`](../design/LIBC.md), Section 11.

**Sub-task 7.6 is complete**: the utilities `ls`, `cat`, `echo`, `mkdir` and
`rm`, the **six system calls** by which a program reaches the filesystem, and the
**argument vector** this kernel's `execve` had refused since Phase 6. It took
more than its line names and had to: `echo` without a vector prints a blank line
for ever. So the count of calls was fourteen (sixteen since 8.3, nineteen since 8.6), the count of failure results was
twenty (twenty-one since 8.6) — three of the five utilities act upon `errno` and not upon the sign of a
result — and each process now holds a descriptor table of its own, so that a
program cannot reach another's open file by guessing a number.

**Three of the eight programs exist because nothing here can read what a program
printed**, and one of them was written because a negative test proved it had to:
the `read` system call was altered to report a count and deliver no bytes, and
`make verify` passed — `cat` wrote a buffer it had never been given and exited
with zero. That program then found a defect that had stood since sub-task 6.11,
in which a path too long to copy was reported as an address the program may not
use. [`../design/LIBC.md`](../design/LIBC.md), Section 12.

**Sub-task 7.7 is complete, and Phase 7 with it.** There is a filesystem: an
EXT2 image built beside the kernel by `mke2fs`, carried in the ISO, placed in
memory by GRUB as a Multiboot2 module, presented to the block layer as the device
`ram0`, and mounted at the root before anything else. `/bin/echo` is a file upon
it, and the line it prints is the first in this project's boot log written by a
program that was read off a filesystem.

**It adds almost no kernel**, which is the measure of Phase 5 rather than of this
sub-task: the image is EXT2, so the mount is the mount Phase 5 already built and
the code that reads a program off the ramdisk is the code that reads a file off a
disk. What is new is the module tag, the reservation of the frames it occupies, a
block driver that converses with nothing, and the decision about which volume is
the root — which is made **by name**, because a kernel that took whichever device
registered first would boot a stranger's disk at the root upon a machine that had
one. A volume the machine carries is mounted at `/mnt` instead.
[`../storage/INITRD.md`](../storage/INITRD.md).

**The image is made by an implementation this project did not write**, and that
is deliberate: the volume the kernel mounts as its root at every boot, in every
environment, was composed by e2fsprogs, so the superblock, the group descriptor,
the root inode, the directory entries and the file blocks of Phase 5 are read
against something that shares none of their assumptions before the banner is
printed. Each utility is then compared, byte for byte, against the copy of the
same program embedded in the kernel image — the two having been one file at build
time — so a block read from the wrong offset or a length rounded to a boundary is
caught rather than returned as data.

**Phase 8 is complete in its seven sub-tasks; sub-task 8.1 is where it began**: a person can type at this
system and be answered by it. The plan's line for it is "line editing with
history", and that is what is seen — a prompt, a line edited with the cursor
keys, Home, End, Delete, Backspace and the control characters every shell of
this lineage accepts, and a history of thirty-two lines the arrow keys walk —
but the thing that had to exist first is the one the line does not name: **a way
for a program to read what a person types.** Until this sub-task there was none.

So it is three things. **The terminal**, in the kernel: one byte stream, drawn
from the keyboard and the serial line, that `read` of descriptor 0 delivers,
waiting until there is something to deliver. The keyboard's cursor keys become
the control sequences ECMA-48 and every terminal emulator send, so a program
parses one dialect whether the person is at the machine or at the far end of a
serial line. It is raw — nothing echoes and nothing assembles a line — because
that is the only arrangement a line editor can work upon. **The line editor**,
in the C library, above an output function and never a descriptor: the division
of the heap and the streams made a third time, and this time it buys something
new, because the kernel's self-test captures what the editor writes and asserts
the *display* byte for byte — the first output in this project asserted rather
than read by a person. A negative test removed the backspaces after an
insertion and every assertion upon the *line* still passed. **The shell**,
`/bin/sh` upon the ramdisk, which the kernel starts when the boot finishes and
which, there being no tokeniser until 8.2, answers every line with a statement
that nothing runs it. [`../design/SHELL.md`](../design/SHELL.md).

**`stdin` reads.** `OxysStreamFill` was written as a function that reported
end-of-file so that, on the day a call that reads existed, one function would
change and nothing above it; that day was this one, and that is what happened.
Two self-tests that read `stdin` to assert it was at its end had to stop,
because it now waits for a person.

**Sub-task 8.2 is complete**: the tokeniser and the command parser. A line is
now the operators and words of IEEE Std 1003.1-2017, Section 2.3, quoted as
Section 2.2 quotes them, and parsed into lists of pipelines of simple commands
with their redirections — the subset of Section 2.10's grammar the rest of the
phase acts upon, with the remainder refused by name so that `if true` is not
a search for a program called `if`. The tokens keep their quotes for the
expansions that have not arrived; a command that ends inside a quote or after
an operator is continued upon a second prompt; and the shell, having nothing
yet to run, describes what it understood. The grammar is compiled into the
kernel image and asserted there, as the C library is.
[`../design/SHELL.md`](../design/SHELL.md), Sections 8 to 10.

**Sub-task 8.3 is complete**: `cd`, `pwd`, `export` and `exit`. The first of
them reached the kernel, which until now had no working directory: every
process holds one, two calls move and report it, and every path-taking call
resolves a relative path against it in the one place they all copy their
argument through. The shell sets variables, expands `$NAME` and `$?`, exports,
and answers every command that is not a built-in with the 127 of a command not
found — which is the truth of it until 8.4. `ls` with no operand lists `.` at
last. [`../design/SHELL.md`](../design/SHELL.md), Sections 11 to 15.

**Sub-task 8.4 is complete, and it is what the phase was for**: `ls`, `cat`,
`echo`, `mkdir` and `rm` run from the prompt. A command that is not a built-in
is sought upon `PATH`, forked, executed with the exported variables as its
environment — `getenv` arriving with the first program that could use it —
and waited for, with the statuses Section 2.8.2 assigns. The first program the
shell ran found a defect that had stood in the system-call entry path since
6.7: the caller's stack pointer was restored from the per-processor block,
which a child's `SYSCALL` inside the parent's `wait` overwrote. It is restored
from the per-thread frame now. [`../design/SHELL.md`](../design/SHELL.md),
Sections 16 to 18.

**Sub-task 8.5 is complete**: redirection. `ls >out`, `cat <in`, `>>`,
`2>&1` and the rest are performed in the child between `fork` and `execve`,
in the order written; beneath them the kernel gained a writable `open` and
`write`, a count of holders upon an open file — so that a child inherits its
parent's descriptors, `execve` keeps them, and `dup2` shares a position — and
`rmdir`. With the writable file came `touch`, `cp` and `rmdir`, `cat` reading
its standard input, and the built-ins `help`, `true`, `false` and `unset`.
The shell greets nobody now; `help` is the built-in for that.
[`../design/SHELL.md`](../design/SHELL.md), Sections 19 to 21.

**Sub-task 8.6 is complete**: pipelines, and beneath them the one thing this
kernel had never had — two programs running at once. `a | b | c` is one child
per command with a pipe between each pair, the ends placed by `dup2` before the
command's own redirections and closed wherever they are not needed, every child
collected and the status the last one's. The pipe is an open file of the
filesystem layer with no node beneath it, so that inheritance, `dup2` and the
close at exit needed nothing written twice; a reader sleeps while it is empty
and a writer while it is full. What that required of the kernel is the larger
half: a child of `fork` is admitted to the scheduler at the fork, `wait` sleeps
upon a wait channel until a child ends, the thread to return to became a field
of the thread, and the counted interrupt-disable travels with a thread across a
switch. `wc` joins `/bin`, and — beside the sub-task, at the project owner's
request — `micro`, a line editor, the first thing that changes a file rather
than writes one; `help` is a list of every command. The shell's first
session, which the shell had answered by refusing its pipelines, now runs them.
[`../design/SHELL.md`](../design/SHELL.md), Sections 22 to 24;
[`../design/PROCESS.md`](../design/PROCESS.md), Section 17;
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 9.

**Sub-task 8.7 is complete, and Phase 8 with it — the release it cuts was cut the
same day, below.** Signals: a bit in the
target, set by the sender and cleared by the target on its own way out of the
kernel, where its registers stand in a frame the kernel can edit; a handler
entered upon the program's own stack below the red zone and returned through
a two-instruction restorer; SIGKILL and SIGSTOP that cannot be given a
disposition; a stop that sleeps the process until SIGCONT and tells its
parent; SIGPIPE from the pipe; a fault reported as the signal its vector maps
to. Process groups, and a terminal that holds a foreground group: control-C
and control-Z are removed from the input at the head of the queue — by a
reader or by the bootstrap processor's tick — and sent to that group as
SIGINT and SIGTSTP, and a background reader is stopped by SIGTTIN inside its
`read` and reads on when brought to the foreground. `waitpid`, with the
status at last an encoding of kind and number, and a child's descriptors
released at its ending rather than its collecting — the defect a background
pipeline found. The shell runs every pipeline as a job in a group of its own,
`&` leaves one running, and `jobs`, `fg`, `bg` and `kill` govern them; the
shell ignores what the terminal sends and every child puts the default back.
Eight calls, twenty-seven in all; `<signal.h>` in the C library.
[`../design/PROCESS.md`](../design/PROCESS.md), Section 18;
[`../design/SHELL.md`](../design/SHELL.md), Sections 28 and 29;
[`../design/LIBC.md`](../design/LIBC.md), Section 13.

**`Oxys 1 Alpha` was cut on 2026-09-16**, at the close of Phase 8 and at the
project owner's direction: tag `v1-alpha`, build 1 of the register, and the
release notes [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md), which say what it does,
what it does not have, and that it has not been booted upon physical hardware.
The build register resumes with it, numbering from 1.

**Sub-task 9.1 is complete**: the window manager, the first of Phase 9. A
window is a content surface the owner draws into and a queue the owner drains,
with a frame the manager draws — a flat band, a one-pixel border, a title in the
face at twice its size, and a disc for a close control, the one curve the
primitives now have. The stack is an array walked from the top; a key goes to
the window holding the focus; a movement goes to the window beneath the pointer
in that window's own coordinates; a press raises, focuses, and binds the pointer
to its window until every button is up; a press in the band drags the window or,
upon the disc, asks it to close — the manager destroys nothing. It runs from the
bootstrap processor's tick beside the terminal, and the default menu entry now
gives it the screen, the keyboard and the mouse, the shell running upon the
serial line; two entries, **Shell-only** and **Shell Diagnostics**, give the
shell the screen as before. Three windows a person can operate stand in for the
desktop until 9.5. Asserted upon a screen composed in memory, with the stacking
order read from the pixel where two windows overlap; judged by eye under QEMU,
VirtualBox and Bochs. [`../design/WINDOWS.md`](../design/WINDOWS.md);
[`../design/DRAWING.md`](../design/DRAWING.md), Section 4.1.

**Sub-task 9.2 is complete**: the client protocol. Six calls — `window_create`,
`window_destroy`, `window_move`, `window_blit`, `window_event` and
`window_screen`, thirty-five in all — by which a process owns a window: a
rectangle of `0x00RRGGBB` pixels carried across by a copy the kernel encodes
for the screen it has, and an event read one at a time from one window or from
any of the caller's, sleeping until one arrives and woken by the tick that
routed it. A window belongs to the process that made it and is `EBADF` to every
other; a process's windows go at its ending, beside its descriptors. **The
first real client's verdict upon the surface interface of 6.6**: a surface does
not cross — it describes kernel memory — and what crosses is the promise that a
rectangle of pixels will arrive; the abstraction stays on this side unchanged,
and two things the first client found wanting were added before it was done —
a wait upon any of a program's windows, and a call that says how big the
screen is. The demonstration is a program now, `/bin/windows`, launched beside
the shell by `ThreadLaunch` and waited for by nobody. Asserted by
`window-check` at privilege level 3 with a kernel thread to read its pixels and
wake it. [`../design/WINDOWS.md`](../design/WINDOWS.md), Sections 10 to 12.

**Sub-task 9.3 is complete**: `init`, the first user process. The kernel now
starts one program and names it to itself; `init` starts the desktop and starts
it again whenever it ends, and collects every orphan — a process that ends gives
its children to `init`, which is what [`../design/PROCESS.md`](../design/PROCESS.md),
Section 19, had recorded as owed since 8.7. The machine can be stopped in order:
`power` halts it or restarts it through the keyboard controller's reset line and
is **reserved to `init` alone**, every other caller being `EPERM`, so `shutdown`
finds `init` by name in the process table and asks it by a signal, and `init`
stops the desktop before it stops the machine; `pause` was added for an `init`
with nothing to collect, which must not spin. Thirty-seven calls. At the project
owner's request the default entry now shows a **boot screen** — a mark, a
wordmark and a line — from the moment there is a back buffer until the desktop
composes over it, in place of the banner and the black screen it showed before;
the power screen is its counterpart, and neither is drawn upon the entries that
give the shell the screen. [`../design/INIT.md`](../design/INIT.md).

**Sub-task 9.4 is complete**, on 2026-09-18: the system configuration. A format
read a line at a time — a comment, a bracketed section, `key = value`, and a
section repeated is a list — chosen so that **a line which cannot be read costs
that line and not the file**: the faults are kept with their line numbers and
reported, and the parse carries on, because a configuration read at boot by the
first user process cannot be allowed to fail whole. The parser is in the C
library with its parsing held apart from the file it reads, as the line editor's
editing is held apart from the terminal, so that the whole of the format is
asserted before there is a program to read a file. `/etc` holds two files, kept
in the repository and staged onto the ramdisk: `system.conf`, from which `init`
now takes its services — `run`, `restart`, and `needs = display`, which is what
keeps the desktop off the entries that give the shell the screen — and
`desktop.conf`, from which the desktop takes its scale and its accent. A service
that ends five times in a row is given up on, which is the bound
[`../design/INIT.md`](../design/INIT.md) owed and which is a count and not a
rate, there being no clock. [`../design/CONFIG.md`](../design/CONFIG.md).

**Next: sub-task 9.5** — the session: the desktop root, the panel, the launcher,
and the ownership of the display that decides who may draw upon it.





**Three releases are planned, and each is fixed to a sub-task below rather than
to a date**: `Oxys 1 Alpha` at **8.7**, `Oxys 1 Beta` at **9.7**, and `Oxys 1` at
or about **11.10**. A sub-task rather than a date, because a date is a guess about
how long work takes and a sub-task is a statement about what the system can do.
Each is marked in the table of its phase, and
[`VERSIONING.md`](VERSIONING.md), Section 11.1, holds the plan, what each release
can do that the one before it could not, and — for `Oxys 1` — what it will not
have, Phases 12 and 13 falling after it.

**The build register was suspended until the alpha, and resumed with it.** At
the project owner's direction on 2026-09-13 every row of [`BUILDS.md`](BUILDS.md)
was removed and every archived image deleted, and no build was recorded again
until `Oxys 1 Alpha` was cut at sub-task 8.7, on 2026-09-16. The machinery was
untouched — `make build-record` works, and `tools/builds.sh` is byte for byte the
script it was — and **numbering restarted at 1**, the alpha's image being build
1, so a build number is unique within a register and not across this project's
life. That document holds the reasoning and what the restart costs.

**The alpha and the beta are the first release's and are not expected to recur.**
Every release after `Oxys 1` is preceded by internal debug builds, which are
images rather than releases: they are numbered in [`BUILDS.md`](BUILDS.md), carry
no tag and no version string, and become releases subject to every condition the
moment one is handed to anybody. `VERSIONING.md`, Section 5.5.

**Two things Phase 6 leaves for it to inherit.** The scheduler's affinity mask
names the bootstrap processor alone for every user thread, because the
allocators, the process tables and the filesystem layer a system call reaches are
still unsynchronised; [`../design/SCHEDULER.md`](../design/SCHEDULER.md),
Section 4, records that this is a limitation written as a value in a field rather
than a scheduling decision, and
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10, limitation 1,
enumerates what is outstanding. And the bootstrap processor is not itself a
scheduled thread: it executes `KernelMain` as a flow of control, joining the
rotation only while something has adopted a thread for it.

For what the system does today, and where it has been observed to do it, see
[`STATUS.md`](STATUS.md). For how it came to be that way, see
[`HISTORY.md`](HISTORY.md).

## How to read this document

**A sub-task carries three independent facts, and none of them implies another.**
One checkbox used to carry all three, which meant it could not distinguish a
sub-task whose code exists from one that has been run, or either from one that
some assertion actually covers.

| Column | What it says | What it does **not** say |
| ------ | ------------ | ------------------------ |
| **State** | Whether the code exists. `Planned`, `In progress`, or `Implemented`. | Nothing about whether it works, or where it has been run. |
| **Asserted by** | Which boot-time self-test covers the sub-task, if any. `—` means no assertion of its own — a statement of fact, not a defect: some sub-tasks are not the kind of thing an assertion can reach. `(indirect)` means no assertion names the sub-task, but an assertion elsewhere would fail if it were wrong. | Nothing about whether that assertion is sufficient. |
| **Verified** | Recorded per phase in [`STATUS.md`](STATUS.md), Section 3, because the evidence is per environment — QEMU, VirtualBox, OVMF, physical hardware — and not per sub-task. | — |

So `Implemented` means the code exists and builds under the full diagnostic
regime. It does not mean tested, and it does not mean verified upon any
particular machine.

Sub-task 6.1 is the case that shows why the columns are separate. It is
`Implemented`, and it has been since the phase opened; but its self-test stopped
executing `SYSCALL` when sub-task 6.7 replaced the entry point, so what it
asserts today is narrower than what it asserted then. One marker could not have
shown that change at all. Sub-task 1.12 was the other such case until it was
closed: everything around it was implemented and nothing about it was, the
sub-task being the verification itself.

## The thirteen phases

| Phase | Subject | State |
| ----- | ------- | ----- |
| [1](#phase-1--bootstrapping-and-early-output) | Bootstrapping and early output | Implemented |
| [2](#phase-2--memory-management-including-copy-on-write) | Memory management, including copy-on-write | Implemented |
| [3](#phase-3--interrupts-exceptions-and-keyboard-input) | Interrupts, exceptions and keyboard input | Implemented |
| [4](#phase-4--basic-device-drivers) | Basic device drivers | Implemented |
| [5](#phase-5--ext2-filesystem) | EXT2 filesystem | Implemented |
| [6](#phase-6--graphics-system-calls-process-management-and-symmetric-multi-processing) | Graphics, system calls, processes, SMP | Implemented |
| [7](#phase-7--userland-and-minimal-c-library) | Userland and minimal C library | Implemented |
| [8](#phase-8--shell) | Shell | In progress |
| [9](#phase-9--the-desktop-its-system-services-and-its-configuration) | The desktop, its services and its configuration | Planned |
| [10](#phase-10--cryptography) | Cryptography | Planned |
| [11](#phase-11--networking) | Networking | Planned |
| [12](#phase-12--uefi-transition) | UEFI transition | Planned |
| [13](#phase-13--polish-optimisation-and-final-hardening) | Polish, optimisation and final hardening | Planned |

The ordering is dictated by dependency, which
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4, sets out —
including the two places where the order was deliberately chosen against the
obvious one, and why.

---

## Phase 1 — Bootstrapping and Early Output

**Objective**: Produce a bootable ISO image containing a Multiboot2-compliant
ELF64 kernel that enters long mode, relocates to the higher half of the virtual
address space, and emits identifying output.

**Specifications**: Multiboot2 Specification 2.0; Intel SDM Volume 3A,
Chapters 2, 4 and 9; System V ABI for AMD64.
**Design**: [`../design/BOOT.md`](../design/BOOT.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 1.1 | Verify the cross-compilation toolchain (`x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`, `xorriso`) is present and functional. | Implemented | `make toolcheck` |
| 1.2 | Author the linker script `linker.ld` defining a higher-half kernel at virtual base `0xFFFFFFFF80000000` with a physical load address of `0x00100000`. | Implemented | — |
| 1.3 | Author `boot/boot.asm`: Multiboot2 header, 32-bit protected-mode entry point, Multiboot2 magic validation, CPUID and long-mode feature detection. | Implemented | `grub-file`, at each link |
| 1.4 | Construct the boot-time paging hierarchy (PML4, two PDPTs, one PD) providing a 1 GiB identity map and a coincident 1 GiB higher-half map using 2 MiB pages. | Implemented | — |
| 1.5 | Perform the long-mode transition (CR4.PAE, IA32_EFER.LME, CR0.PG) and load a 64-bit GDT. | Implemented | — |
| 1.6 | Transfer control to the higher-half 64-bit kernel entry point and establish the kernel stack. | Implemented | — |
| 1.7 | Implement a minimal VGA text-mode output routine and a minimal COM1 serial output routine for early diagnostics. | Implemented | `dev/devices.c` |
| 1.8 | Implement `KernelMain`, which clears the screen and prints the string "Oxys-OS". | Implemented | `make verify` banner |
| 1.9 | Author the `Makefile` with the targets `all`, `clean`, `iso`, `run-qemu`, `run-vbox` and `run-uefi`. | Implemented | — |
| 1.10 | Generate the ISO image and verify boot under QEMU. | Implemented | `make verify` |
| 1.11 | Verify boot under VirtualBox. | Implemented | [`TESTING.md`](TESTING.md) §4 |
| 1.12 | Verify boot on physical hardware from a USB medium. | Implemented | [`TESTING.md`](TESTING.md) §5.1–5.2 |

---

## Phase 2 — Memory Management (including Copy-on-Write)

**Objective**: Establish complete control of physical and virtual memory, and
provide the copy-on-write primitives upon which `fork()` will later depend.

**Specifications**: Intel SDM Volume 3A, Chapter 4 (Paging) and Section 6.15
(Page-Fault Exception); Multiboot2 Specification, Sections 3.6.7 (ELF-Symbols
tag) and 3.6.8 (memory map tag).
**Design**: [`../design/MEMORY-LAYOUT.md`](../design/MEMORY-LAYOUT.md).

Sub-tasks 2.7 and 2.8 were deferred until Phase 3 supplied a page-fault handler;
that is the one mutual dependency in the whole ordering, and
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4, records how
it was discharged.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 2.1 | Parse the Multiboot2 information structure and extract the memory map (tag type 6) and the ELF section headers (tag type 9). | Implemented | `mm/memory.c` (indirect) |
| 2.2 | Implement a physical frame allocator (bitmap) covering all usable regions, reserving the kernel image, the Multiboot2 structures and the low 1 MiB. | Implemented | `mm/memory.c` |
| 2.3 | Construct a permanent kernel page-table hierarchy, replacing the boot-time tables and removing the low identity map. | Implemented | `mm/memory.c` |
| 2.4 | Implement a direct physical map region for kernel access to arbitrary frames. | Implemented | `mm/memory.c` |
| 2.5 | Implement a kernel virtual-address-space allocator and a general-purpose kernel heap (slab allocator over a buddy-style page allocator). | Implemented | `mm/memory.c` |
| 2.6 | Implement per-frame reference counting as the substrate for shared pages. | Implemented | `mm/memory.c` |
| 2.7 | Implement the page-fault handler dispatch path (dependent upon Phase 3) and the copy-on-write fault resolution routine. | Implemented | `mm/memory.c` |
| 2.8 | Implement address-space cloning that marks writable user pages read-only and increments frame reference counts. | Implemented | `mm/memory.c` |

---

## Phase 3 — Interrupts, Exceptions and Keyboard Input

**Objective**: Install a complete interrupt descriptor table, service processor
exceptions, and accept keyboard input.

**Specifications**: Intel SDM Volume 3A, Chapter 6; Intel 8259A datasheet;
IBM PS/2 controller documentation.
**Design**: [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md),
[`../devices/TIME.md`](../devices/TIME.md),
[`../devices/KEYBOARD.md`](../devices/KEYBOARD.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 3.1 | Define the IDT and the 64-bit interrupt-gate descriptor format; load it with `lidt`. | Implemented | `arch/interrupts.c` |
| 3.2 | Author assembly stubs for vectors 0–255, normalising the presence or absence of a processor-pushed error code. | Implemented | `arch/interrupts.c` |
| 3.3 | Implement a C interrupt dispatcher operating on a formal trap frame structure. | Implemented | `arch/interrupts.c` |
| 3.4 | Implement exception handlers with register and stack diagnostics emitted over the serial port. | Implemented | `arch/interrupts.c`, `gfx/faultscreen.c` |
| 3.5 | Remap the 8259A PIC to vectors 32–47 and implement end-of-interrupt signalling. | Implemented | `dev/devices.c` |
| 3.6 | Implement the Programmable Interval Timer as the initial timer source. | Implemented | `dev/devices.c` |
| 3.7 | Implement the PS/2 keyboard driver: controller initialisation, scancode set 1 translation, modifier state and a circular input buffer. | Implemented | `dev/devices.c` |

---

## Phase 4 — Basic Device Drivers

**Objective**: Provide the device support required by the filesystem and by
subsequent user-facing subsystems.

**Specifications**: PC16550D UART datasheet; ATA/ATAPI Command Set (ACS-3);
PCI Local Bus Specification 3.0; Serial ATA Advanced Host Controller Interface
1.3.1; SD Host Controller Simplified Specification 4.20.
**Design**: [`../devices/`](../devices/) and [`../storage/`](../storage/).

Sub-tasks 4.7 and 4.8 were added after the phase was otherwise complete, both at
the project owner's report from a real machine that this kernel found no disk
upon. The diagnosis and the reasoning are in
[`../storage/DISK.md`](../storage/DISK.md), Sections 2.1 to 2.3; the short of it
is that "no disk" had three indistinguishable causes and only one of them was
the absence of a disk.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 4.1 | Promote the early serial routine to a formal, interrupt-driven COM1 driver with configurable line parameters. | Implemented | `dev/devices.c` |
| 4.2 | Promote the early VGA routine to a formal text-mode driver with scrolling, cursor control and colour attributes. | Implemented | `dev/devices.c` |
| 4.3 | Implement PCI configuration-space enumeration by the legacy I/O port mechanism, with device and class identification. | Implemented | `dev/devices.c` |
| 4.4 | Implement an ATA PIO driver: bus reset, `IDENTIFY DEVICE`, 28-bit and 48-bit LBA sector read and write. | Implemented | `storage/stack.c` |
| 4.5 | Define a generic block-device abstraction layer above the ATA driver. | Implemented | `storage/stack.c` |
| 4.6 | Implement a buffer cache for block devices. | Implemented | `storage/stack.c` |
| 4.7 | Implement an AHCI driver, so that a machine whose firmware presents its SATA controller in AHCI mode has a disk at all. *(Added 2026-09-04.)* | Implemented | `storage/stack.c` |
| 4.8 | Implement an SD host controller driver, so that a machine whose system is upon an embedded MultiMediaCard part has storage at all. *(Added 2026-09-06.)* | Implemented | `storage/stack.c` |

---

## Phase 5 — EXT2 Filesystem

**Objective**: Mount, read and write an EXT2 volume.

**Specifications**: The Second Extended File System (Poirier); Linux kernel
documentation, `Documentation/filesystems/ext2.rst`.
**Design**: [`../storage/EXT2.md`](../storage/EXT2.md) and the two it heads,
[`../storage/VFS.md`](../storage/VFS.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 5.1 | Parse the superblock and validate the EXT2 magic number and revision level. | Implemented | `ext2/format.c` |
| 5.2 | Parse the block-group descriptor table. | Implemented | `ext2/format.c` |
| 5.3 | Implement inode retrieval and the resolution of direct, singly, doubly and triply indirect block pointers. | Implemented | `ext2/format.c`, `ext2/file.c` |
| 5.4 | Implement directory-entry traversal and absolute path resolution. | Implemented | `ext2/directory.c` |
| 5.5 | Implement file reading. | Implemented | `ext2/file.c` |
| 5.6 | Implement block and inode allocation, file writing, extension and truncation. | Implemented | `ext2/write.c` |
| 5.7 | Implement directory creation and entry insertion and removal. | Implemented | `ext2/write.c` |
| 5.8 | Define a virtual filesystem layer and mount an EXT2 root volume. | Implemented | `storage/vfs.c` |

---

## Phase 6 — Graphics, System Calls, Process Management and Symmetric Multi-Processing

**Objective**: Present a graphical display, execute user-mode programs, schedule
them across multiple processors, and expose kernel services by system call.

**Specifications**: Intel SDM Volume 3A, Chapters 5, 8 and 10, and Volume 2B
(`SYSCALL`/`SYSRET`); System V ABI for AMD64; Intel 82093AA I/O APIC datasheet;
Intel MultiProcessor Specification 1.4; ACPI Specification 6.5 (the RSDP, the
table directory and the MADT); Multiboot2 Specification, Sections 3.6.12
(framebuffer information tag), 3.6.16 and 3.6.17 (the ACPI pointer tags); VESA
BIOS Extensions 3.0.
**Design**: [`../design/PRIVILEGE.md`](../design/PRIVILEGE.md),
[`../design/GRAPHICS.md`](../design/GRAPHICS.md) and the five it indexes,
[`../design/EXECUTABLE.md`](../design/EXECUTABLE.md),
[`../design/PROCESS.md`](../design/PROCESS.md),
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md),
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md), Section 10,
[`../devices/MOUSE.md`](../devices/MOUSE.md),
[`../devices/ACPI.md`](../devices/ACPI.md),
[`../devices/APIC.md`](../devices/APIC.md).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 6.1 | Install the GDT and TSS required for privilege transition; configure IA32_STAR, IA32_LSTAR and IA32_FMASK. | Implemented | `arch/privilege.c` — **partial**, see note (a) |
| 6.2 | Request a linear framebuffer by the Multiboot2 framebuffer tag and map it into kernel space. | Implemented | `gfx/framebuffer.c` |
| 6.3 | Implement 2D primitives: pixel, line, rectangle, blit and clipping. | Implemented | `gfx/graphics.c` |
| 6.4 | Implement a bitmap font renderer, and a graphical console above it that the diagnostic path may write to. | Implemented | `gfx/console.c`, `gfx/faultscreen.c` |
| 6.5 | Implement a PS/2 mouse driver upon the second device port of the 8042, and a cursor. | Implemented | `dev/mouse.c` |
| 6.6 | Implement a compositing surface abstraction and double buffering. | Implemented | `gfx/compositor.c` |
| 6.7 | Implement the `SYSCALL` entry path, the system-call dispatch table and argument validation. | Implemented | `arch/syscall.c` |
| 6.8 | Implement the ELF64 loader for statically linked executables. | Implemented | `exec/elf.c` |
| 6.9 | Define the process control block, the address-space descriptor and the thread structure. | Implemented | `proc/process.c` |
| 6.10 | Implement context switching and the initial transition to user mode via `IRETQ`. | Implemented | `arch/usermode.c` |
| 6.11 | Implement `fork()` upon the Phase 2 copy-on-write substrate, together with `execve()`, `exit()` and `wait()`. | Implemented | `proc/lifecycle.c` |
| 6.12 | Parse the ACPI MADT; initialise the Local APIC and the I/O APIC; retire the 8259A PIC. | Implemented | `arch/apic.c`, `dev/devices.c` — see note (b) |
| 6.13 | Implement spinlocks, per-CPU data areas and inter-processor interrupts, including TLB shootdown. | Implemented | `arch/smp.c` — see note (c) |
| 6.14 | Implement application-processor bring-up by INIT-SIPI-SIPI and a real-mode trampoline. | Implemented | `arch/smp.c` — see note (d) |
| 6.15 | Implement a multiprocessor-aware round-robin scheduler with per-CPU run queues and processor affinity. | Implemented | `proc/sched.c` — see note (e) |

**(a)** Sub-task 6.1's self-test executed `SYSCALL` until sub-task 6.7 replaced
the entry point with one returning by `SYSRET`, which returns to privilege level
3 unconditionally. That assertion is recorded as lost rather than disguised, and
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), Section 9.4, says why no test
hook was added to recover it. The configuration is asserted still; the
instruction is now executed only by a user program.

**(b)** Sub-task 6.12 is asserted by four routines in `arch/apic.c` — the ACPI
parse, the Local APIC, the I/O APIC and the routing after the adoption — and by
`KernelVerifyIrq` in `dev/devices.c`, which asserts the routing layer while
the 8259A pair still answers. The division is deliberate: the same path is
asserted under each controller, so a failure says which of them broke it.

**(c)** Sub-task 6.13 is asserted by four routines in `arch/smp.c`. The first
two — the per-processor area and the spinlock — assert internal state and not
behaviour, because upon a machine with one processor a lock that does not lock
behaves exactly like one that does. The last two are behavioural: an interrupt a
processor sends to itself is delivered like any other, so the whole shootdown
path is exercised, against a mapping the test makes stale on purpose. **Only one
of the locks has been applied**; see note (d).

**(d)** Sub-task 6.14 is asserted by `KernelVerifyApplicationProcessors`, the
fifth routine in `arch/smp.c`. **A count is not the assertion**: a kernel that
incremented a variable and started nobody would produce the same count, the same
report and the same banner. What only a running processor can produce is an
acknowledgement to an interrupt it was sent, so the substance of the test is a
shootdown broadcast with each target's own service count read out afterwards —
the mechanism of 6.13 doing, at last, the thing it was built for. It also checks
what each started processor read out of its own task register, descriptor table
registers and control registers, `CR0.WP` among them, whose absence upon one
processor nothing else in this kernel would ever report; and upon a machine with
one processor it asserts the other side, that nobody was started and that the
kernel says which condition declined it.

**The one lock 6.14 applies is the diagnostic channel**, in `KernelWriteString`,
because that is the whole of what a started processor touches — a started
processor has nothing to run and is parked in a halt loop until 6.15. Every other
structure in
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10, limitation 1,
is still unsynchronised; see note (e) for what 6.15 did and did not change.

**(e)** Sub-task 6.15 is asserted by `KernelVerifyScheduler` in
`proc/sched.c`. **A count of admissions is not evidence that anything ran**: a
scheduler that enqueued four threads and gave none of them a processor produces
the same admissions, the same queue lengths and the same report. So the fixture
is four kernel threads that do work and record it, and the assertions are made
against what they recorded — that every thread completed its rounds, upon a
processor it names itself, having been given the processor at least once; that
the slices across the fixture exceed the number of threads, which is the rotation
visible from outside; and that a quantum expired, which is the one thing a
voluntary yield cannot demonstrate.

Two of those assertions were got wrong first and the corrections are recorded in
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7. **A third was
recorded at sub-task 7.3 and closed at 7.7**: the test read two fields of a
thread whose affinity names every processor, with the scheduler already running,
so the other processor was entitled to take the thread between the admission and
the read, and it reported a defect that had not occurred. It was seen once in
twelve runs on 2026-09-11 and left for the sub-task that next revisited the file;
7.7 shifted the timing enough to make it one boot in three. Section 7.3 of that
document records what may now be asserted of which field, and why.

**The locks are still not applied beyond two of them** — the run queues, which this sub-task
creates, and the process and thread tables, which it makes contended. A user
thread's affinity mask names the bootstrap processor alone for exactly that
reason, and Section 4 of that document says so; widening it is the work of the
sub-task that locks the allocators and the filesystem layer.

**Why 6.2 to 6.6 sit here rather than in Phase 9**, and **why 6.13 precedes
6.14**: both orderings were chosen against the obvious one, and both arguments
are in [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4.1.

---

## Phase 7 — Userland and Minimal C Library

**Objective**: Provide the runtime environment in which user programs execute.

**Specifications**: ISO/IEC 9899:2011; System V ABI for AMD64.
**Design**: [`../design/LIBC.md`](../design/LIBC.md).

Phase 7 is also where the filesystem layer's open file table becomes per-process
and `fork` must decide what a child inherits; a process has no file descriptors
before it. See [`../storage/VFS.md`](../storage/VFS.md), limitation 2.

**The obligation sub-task 7.2 carried from the licensing is discharged.** The
userland is `MIT` and the kernel `LGPL-3.0-or-later`, so a C library could not
include a header that mixed the user-visible interface with the kernel's
implementation of it — and `kernel/include/oxys/arch/syscall/syscall.h` did. Sub-task 7.1
divided it: the interface is now `kernel/abi/oxys/syscall_abi.h`, under `MIT` and
reachable by a second include root of its own, and the implementation stays with
the kernel. See [`../../LICENSING.md`](../../LICENSING.md), Section 2.1, and
[`../design/LIBC.md`](../design/LIBC.md), Section 2. **The wrappers of 7.2 are
declared by the C library**, in `libc/include/syscall.h`, and not by the
interface header: that directory holds constants and a convention and never a
symbol.

**Sub-task 7.1 is the first thing in this project whose test subject is not the
kernel**, and it is asserted by the kernel anyway — `make verify` being the only
thing here that can execute anything until this phase produces a userland to host
a harness in. [`../design/LIBC.md`](../design/LIBC.md), Section 7, records the
arrangement, why it is honest rather than expedient, and what sub-task 7.5
changes about it.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 7.1 | Implement the freestanding string and memory functions (`<string.h>`). | Implemented | `libc/string.c` — see note (a) |
| 7.2 | Implement system-call wrappers for the complete kernel interface. | Implemented | `libc/wrappers.c` — see note (b) |
| 7.3 | Implement a user-space heap allocator (`malloc`, `free`, `realloc`) above `brk`/`mmap`. | Implemented | `libc/heap.c` — see note (c) |
| 7.4 | Implement buffered input and output (`<stdio.h>`) and formatted conversion. | Implemented | `libc/stdio/` — see note (d) |
| 7.5 | Author the C runtime startup object (`crt0`) and the static-linking procedure for user programs. | Implemented | `userland/startup-check/` — see note (e) |
| 7.6 | Implement the utilities `ls`, `cat`, `echo`, `mkdir` and `rm`. | Implemented | `userland/` — see note (f) |
| 7.7 | Construct an initial ramdisk containing the utilities and mount it as the early root. | Implemented | `kernel/test/storage/initrd.c` — see note (g) |

**(a)** Sub-task 7.1 implements nineteen of the twenty-two functions of ISO/IEC
9899:2011, Section 7.24, and sub-task 7.2 adds `strerror`, making twenty. The two
absent are `strcoll` and `strxfrm`, which compare and transform according to a
locale this system has not got. [`../design/LIBC.md`](../design/LIBC.md), Section
4, records each.

**(b)** Sub-task 7.2's assertion is in two halves because its subject is.
`SYSCALL` cannot be executed by this kernel at all — the `SYSRET` that ends the
kernel's handling of it returns to privilege level 3 unconditionally — so the
translation of a result into an `errno`, which is on this side of the
instruction, is asserted by calling it; and the invocation, which is not, is
asserted by copying the bytes `libc/syscall/invoke.asm` ships into a program
composed for the purpose and running them at privilege level 3.
[`../design/LIBC.md`](../design/LIBC.md), Section 8.4.

**The negative test found the first version of that assertion worthless.** A
three-argument invocation was altered to lose its third argument — the defect the
whole arrangement exists to catch — and every assertion passed, the lost argument
being a length the kernel bounds rather than refuses. The status a program ends
with is now the sum of what every one of its calls returned, and Section 8.7 of
that document records the reasoning at length.

**The negative test found a defect in the test rather than in the code**, which
is the class of defect a passing run cannot report. The guard at the head of
`strstr` was deleted on purpose and every assertion still passed: the search loop
already handles an empty needle against a haystack that is not empty, and the
assertion had never covered the one case the guard exists for — an empty needle
in an *empty* haystack. The assertion now covers it.
[`../design/LIBC.md`](../design/LIBC.md), Section 5.1.

**(c)** Sub-task 7.3's assertion is in two halves for the same reason 7.2's is,
and the division is the sub-task's own rather than the test's. The allocator's
policy calls nothing that can fail outside the C language, so it is given a
region by `OxysHeapAdopt` and exercised directly; `brk` is a system call and is
asserted by a program at privilege level 3. `mmap` is **not** implemented and the
heap does not need it: the sub-task's title names both because either would
serve, and a second way of obtaining memory is a second thing to get right for no
present gain. [`../design/LIBC.md`](../design/LIBC.md), Section 9.1.

**Sub-task 7.3 added the eighth system call, and the numbering rule held.** `brk`
is call 7 and `SYSCALL_COUNT` is now 8. It is numbered after the seven for the
reason the four of sub-task 6.11 were numbered after the three before them: a
number already handed to a program is a number that must not change, and there
are now programs — the self-tests of 7.2 and 7.3 — compiled against the old ones.

**Two negative tests of 7.3 found something the passing run could not.** One
removed the undo of a failed heap growth and every assertion passed, no test here
being able to exhaust the frame allocator; that is recorded as a limitation
rather than papered over. The other removed a size check from `OxysHeapAdopt` and
every assertion passed, because a second check made later rejects strictly more —
so the first was deleted rather than kept for appearances.
[`../design/LIBC.md`](../design/LIBC.md), Section 9.7.


**(d)** Sub-task 7.4's assertion is in two halves for the third time, and this
time **the kernel cannot make the second half at all**. The buffering policy and
the whole of the formatted conversion are ordinary C and are asserted by giving
the library streams whose device is a region of memory — `OxysStreamOpenMemoryWrite`
and `OxysStreamOpenMemoryRead`, which are a documented interface and not a test
hook, being what a hosted implementation spells `fmemopen`. The transfer beneath
them executes `SYSCALL`, so it is asserted by the program of sub-task 7.5 and by
nothing before it. [`../design/LIBC.md`](../design/LIBC.md), Section 10.2.

**The first run of 7.4's self-test failed, and the assertion was at fault rather
than the code.** The census had one counter for both seams, and the test asserted
it was zero — that being what keeps a test inside this kernel from executing a
system call. But only one of the two seams executes one: there is no call that
reads, so the source of a stream is ordinary C that reports end-of-file, and the
test reads `stdin` deliberately in order to assert that it reports an *end* and
not an *error*. The counter is now two counters. Section 10.2.1.

**The conversions were checked against an implementation this project did not
write.** Sixty-six conversion specifications were formatted by this library and
by the host's `snprintf` and compared byte for byte; sixty-five agreed exactly,
and the one that differed is `%p` of a null pointer, which this library prints as
`0x0` deliberately. It is the same kind of corroboration the `mke2fs` comparison
of [`TESTING.md`](TESTING.md) is, and it is the only judge in this phase that
does not share this project's reading of the standard.

**Four of the twenty negative tests of 7.4 found gaps in the assertions**, each
of which is now closed: a buffer emptied one byte late, which overruns a caller's
array while delivering exactly the right bytes; an end-of-file indicator
consulted after the source rather than before, which costs a system call per call
and changes nothing observable; the sign flags let through an unsigned
conversion, which no assertion covered; and a partial delivery counted as a whole
one. **Two found things no assertion here can defend** and are recorded as
limitations rather than papered over. Section 10.8.

**Nothing in 7.4 required a system call, and the count of them is still eight.**
The only thing a stream asks of the kernel is `write`, which has existed since
sub-task 6.7.

**(e)** Sub-task 7.5 is the first thing in this project asserted by a program
that was **built** rather than composed byte by byte, and it closes four
limitations that earlier sub-tasks had recorded and could not close: nothing in
the C library had ever run at privilege level 3 (Section 6, limitation 4); the
heap's policy and its `brk` were asserted apart and never joined (Section 9.1);
the transfer beneath a stream could not be reached from inside the kernel
(Section 10.5.2); and the whole library had only ever been compiled one way.
[`../design/LIBC.md`](../design/LIBC.md), Section 11.

**The kernel gained a half of the process-entry contract it had never had.** The
System V ABI, Section 3.4.1, puts the argument count at the stack pointer, and
this kernel left that pointer one byte past the last mapped byte of the stack —
which every program composed by hand ignored and the first conforming `_start`
faults upon. The frame is six eightbytes of zero, and since the stack pages are
already zeroed the whole of the change is a subtraction.

**No `make` target was added**, deliberately: the program is embedded in the
kernel image and is therefore a dependency of it, exactly as the real-mode
trampoline is. A new target would have obliged an amendment to
`PROJECT_GUIDELINES.md`, Section 3, which Section 7 of that document reserves to
the project owner.

**Three of the fifteen negative tests of 7.5 found a false claim rather than a
defect**, and in each case the documentation was corrected against a measurement
rather than the code being changed: `-z max-page-size=0x1000` was passed on the
belief that it kept the image small and was measured to change the output by not
one byte (it was removed; `-n` does that work); `-mcmodel=kernel` was said to
produce relocations the linker would refuse, and in fact links and runs at four
mebibytes; and the `ar` index was said to matter, which GNU `ar` writes either
way. Two more found things no assertion here can defend and are limitations.
Section 11.7.

**The sub-task also deleted code it had just written.** The kernel's frame was at
first written out explicitly, with the topmost frame's physical address kept in
order to reach it; the negative test that removed the write reported nothing,
because the pages are zeroed unconditionally a few lines above. It was deleted
rather than kept, which is what 7.3 did with the second size check in
`OxysHeapAdopt`.

**(f)** Sub-task 7.6 is the first thing in this project whose subject is a
**program a person would recognise**, and it is asserted in four groups rather
than in two, because there are four different things in it that can be wrong.
[`../design/LIBC.md`](../design/LIBC.md), Section 12.

**It took more than the five utilities its line names, and had to.** `echo`
without an argument vector prints a blank line for ever, and this kernel's
`execve` had refused both vectors since Phase 6 — for want of a convention about
where a program finds them, which [`../design/PROCESS.md`](../design/PROCESS.md),
limitation 10, recorded as Phase 7's to fix. So the sub-task is three things at
once: **six system calls** by which a program reaches the filesystem, **the
vectors** and the descriptor table that come with them, and **the five
utilities** above both. Each of the three is asserted separately, because a
utility that works proves nothing about a call it happens not to make.

**The count of system calls is fourteen**, the six being numbered ninth to
fourteenth by the rule that has held since 6.11: a number already handed to a
program is a number that must not change. **The count of failure results is
twenty**, having been seven: `VfsError` distinguishes fifteen causes, and three
of the five utilities act upon `errno` rather than upon the sign of a result —
`ls` prints an operand that reports `ENOTDIR`, `rm -f` treats `ENOENT` as
success, `mkdir -p` treats `EEXIST` as success — so a kernel that collapsed the
causes would make all three do the wrong thing and each would still exit with a
plausible status.

**Three of the eight programs exist because nothing here can read what a program
printed.** `ls` given a directory prints names, and a kernel watching it cannot
tell the names it printed from the names it should have printed. `arg-check`
compares the vector it was given against the vector it expects; `exec-check`
becomes `arg-check` through `execve`, so that a vector crosses an address space
that is destroyed; and `file-check` reads a file of known contents byte for byte
and asserts twenty refusals **by the name of the failure** rather than by its
sign. Each ends with the number of comparisons that failed, which the kernel
reads exactly as it reads sub-task 7.5's program.

**`file-check` exists because a negative test proved it had to.** The copy at the
end of the `read` system call was removed — so that the call reported a count and
delivered no bytes — and `make verify` reported nothing at all: `cat` opened its
files, was told how many bytes it had, wrote a buffer it had never been given,
and exited with a status of zero. Two of the thirteen negative tests were silent
and both are now caught by that program. Section 12.6.

**One of the sub-task's own programs found a defect that had stood since 6.11.**
`SyscallCopyUserString` returns one refusal for two causes — memory the caller may
not read, and a string too long for the room given — and every caller reported
`EFAULT` for both. Nothing had noticed because nothing had ever asked to be
refused for being too long. `file-check` asked, with a path of three hundred
characters standing entirely within its own memory, and was told the address was
one it may not use. The cause was corrected rather than the assertion. Section
12.2.2.

**Ten limitations were recorded and three of them were Phase 8's.** No program
could create or write a file, and no child inherited a descriptor — both what
the shell's redirection at sub-task 8.5 needed, and neither invented before
something called it; both closed there. There was also **no working directory**,
which was 8.3's `cd`, and the reason `ls` with no operand listed the root
rather than `.`; closed there.
Section 12.7.

**(g)** Sub-task 7.7 is the first thing in this project that a person could
find. Everything Phase 7 built before it ran from one of two places — embedded in
the kernel image by `incbin`, or written onto a volume the kernel composed in an
array — and both are a self-test's apparatus. Since 7.7 there is a filesystem:
`/bin/echo` is a file, upon a volume, upon a device, and it is where it is when
the machine finishes booting. [`../storage/INITRD.md`](../storage/INITRD.md).

**It adds no filesystem and almost no kernel.** The image is an EXT2 volume, so
everything above the block layer is what Phase 5 already built: the mount is
`VfsMountVolume(…, "ext2", …)` and the code that reads a program off the ramdisk
is the code that reads a file off a disk. What 7.7 actually adds is the module
tag of Multiboot2, Section 3.6.6; the reservation of the frames it occupies; a
block driver that converses with nothing; and the decision about which volume is
the root. Section 3.1 of that document records why a format of this project's own
was refused, and the first of the two reasons is that it would have had no
specification to cite.

**The image is made by `mke2fs` and this is the point of it.** A volume composed
here and read here proves the reader consistent with the composer, which is what
[`../../kernel/test/volume.h`](../../kernel/test/volume.h) has warned about its
own fixture since Phase 5. A volume e2fsprogs composed proves it consistent with
an implementation that has never seen this one — and where
[`../storage/EXT2-VERIFICATION.md`](../storage/EXT2-VERIFICATION.md), Section 6,
made that comparison upon images somebody had to remember to build, **this makes
it happen at every boot in every environment**. `mke2fs` is accordingly a build
dependency, the first here that is not a compiler, an assembler, a linker or an
image builder; [`TOOLCHAIN.md`](TOOLCHAIN.md) records it and
`PROJECT_GUIDELINES.md`, Section 3, is unamended, the five tools it names all
still being required.

**The assertion that matters is a byte-for-byte comparison.** Each utility is
upon the ramdisk *and* inside the kernel image, both copied from one file at
build time, so any difference observed at boot was introduced by the path between
them — the module's extent, the direct map, the device's arithmetic, the buffer
cache, an indirect block, a length. Every one of those failures returns *data*,
and a test that opened the files and found them present would pass upon all of
them. One utility is then read from the root, loaded, and entered at privilege
level 3.

**Two things were nearly lost quietly, and both were caught by asking what the
change displaced.** The root was the machine's own volume until this sub-task, so
the write probe of 5.8 resolved its path from the root and would have found
nothing ever again — a diagnostic that stops printing, which is indistinguishable
from a volume that does not hold the file. The machine's volume is now mounted at
`/mnt` and the probe is told where to look. And the self-test of 6.15 read two
fields of a thread the *other* processor was entitled to be running — a race
recorded on 2026-09-11 and left for whichever sub-task next revisited that file.
This one shifted the timing enough to make it fail upon about one boot in three,
always with the scheduler working correctly, so it is the sub-task that closes
it. [`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 7.3.

**Six limitations are recorded and the first is Phase 8's.** Nothing pivots: the
ramdisk is the root and stays the root, because exchanging it needs a working
directory, a way to move a mount, and something that decides which volume is the
system's — and the first of those is sub-task 8.3's.
[`../storage/INITRD.md`](../storage/INITRD.md), Section 8.

---

## Phase 8 — Shell

**Objective**: Provide an interactive command interpreter.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 8.1 | Implement line editing with history. | Implemented | `KernelVerifyTerminal`, `KernelVerifyLine` |
| 8.2 | Implement the tokeniser and the command parser. | Implemented | `KernelVerifyShell` |
| 8.3 | Implement built-in commands (`cd`, `exit`, `export`, `pwd`). | Implemented | `KernelVerifyDirectory`, `KernelVerifyShell` |
| 8.4 | Implement external program execution by `fork()` and `execve()`. | Implemented | `KernelVerifyShell`, `KernelVerifyDirectory` |
| 8.5 | Implement input and output redirection. | Implemented | `KernelVerifyUtilities`, `KernelVerifyShell` |
| 8.6 | Implement pipelines. | Implemented | `KernelVerifyVfs`, `KernelVerifyUtilities`, `KernelVerifyShell` |
| 8.7 | Implement job control, process groups and terminal signal delivery. **`Oxys 1 Alpha` is cut here** — cut on 2026-09-16, [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md). | Implemented | `KernelVerifySignals`, `KernelVerifyShell` |

**Specifications**: IEEE Std 1003.1-2017, Section 11 (the terminal) and `sh`;
ECMA-48, Section 5.4 and Sections 8.3.18 to 8.3.22; XTerm Control Sequences.
**Design**: [`../design/SHELL.md`](../design/SHELL.md), one section per
sub-task.

**(a)** Sub-task 8.1 is more than its line names, and had to be. "Line editing
with history" presumes a program can read what a person types, and until this
sub-task none could: `stdin` was a stream permanently at its end. So the
sub-task is the **terminal** — one byte stream in the kernel, drawn from the
keyboard and the serial line, that `read` of descriptor 0 delivers, the
keyboard's cursor keys translated to the sequences ECMA-48 and every terminal
emulator send — and the **line editor** in the C library above it, and the
**shell** that prompts with it. The terminal is raw, echoing nothing and
assembling nothing, because that is the only arrangement a line editor can work
upon; a canonical mode is recorded as absent and wanted by nothing yet.
[`../design/SHELL.md`](../design/SHELL.md), Section 2.

**The editor asks one thing of a display — that a backspace moves the cursor
left — and is asserted by what it writes.** It draws every edit with printable
characters, spaces and backspaces, so it draws correctly upon a serial terminal,
the text-mode display and the framebuffer console alike; and it writes through a
function it is given, so the kernel's self-test captures the bytes and compares
them. That is the first output in this project asserted rather than read by a
person, and the negative test that removed the backspaces after an insertion
showed why it matters: `line-check`, reading the same session at privilege level
3, passed — the line was right and only the screen was wrong. Section 5.

**Eight limitations are recorded and two are structural.** A `read` of the
terminal halts the processor, which is right while one program runs upon the
bootstrap processor's own flow of control and wrong the moment there are two;
it is the first thing here that would use the wait queue
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 10, records as
absent. And a line longer than the display is wide is drawn wrongly once it
wraps upon a serial terminal, the editor being unable to move the cursor up.
Section 6.

**(b)** Sub-task 8.2 is the tokeniser of IEEE Std 1003.1-2017, Section 2.3,
the quoting of Section 2.2, and the parser of the subset of Section 2.10's
grammar that the rest of the phase acts upon: lists of pipelines of simple
commands, with `;`, `&`, `&&`, `||`, `!`, `|`, and every redirection operator
of Section 2.7 with its `io_number`, the here-document excepted. **What is
outside the subset is refused by name** — `if true` is answered "not
implemented by this shell near `if'" rather than parsed as a program called
`if` — and the reserved words are recognised only in command position, as rule
1 of 2.10.2 requires. **The tokens keep their quotes**, because Section 2.6
orders the expansions before quote removal and a tokeniser that removed them
now would be rewritten when the expansions arrive; quote removal is the last
step, applied by whoever wants the string. A line that ends inside a quote or
after an operator is not an error but a command that continues, and the shell
prompts for the rest with `> `. Nothing allocates and every bound is a number
a person can be told. [`../design/SHELL.md`](../design/SHELL.md), Section 8.

**The shell's grammar is compiled into the kernel image**, as the C library
is, so that the self-test asserts the code the shell ships against fifty lines
with known answers; the shell is then run at privilege level 3 upon a session
that continues commands across lines and is refused twice. Of two negative
tests one made an empty command legal, and the shell run upon the same session
still ended with zero — it printed an empty pipeline and nothing asserts what
it prints — while two of the kernel's assertions caught it. Section 9.

**Five limitations are recorded.** No expansion of any kind — a `$` is a
character — and no assignment word, both arriving with the environment 8.3's
`export` sets; no compound command, function or here-document; bounds of
sixteen words, eight redirections, eight commands and sixteen pipelines; and
nothing runs. Section 10.

**(c)** Sub-task 8.3 is the four built-ins, and `cd` is the one that reaches
the kernel: until this sub-task no process had a working directory and every
relative path resolved against the root. Each process now holds one, `/` at
creation, inherited across `fork` and kept across `execve`; two calls, `chdir`
and `getcwd`, move and report it; and **every call that takes a path resolves
a relative one against it in the one function they all copy their argument
through**, so that none can differ or forget. `chdir` stores the canonical
form and establishes it names a directory first, because a working directory
that named a file would fail at the wrong call. The count of calls is sixteen.
[`../design/SHELL.md`](../design/SHELL.md), Section 11.

**`export` needed something to read a variable back, so the sub-task carries
the smallest expansion that makes it mean something**: `$NAME`, `${NAME}` and
`$?`, and the assignment word `NAME=value` of Section 2.10.2, rule 7. The
tokens kept their quotes at 8.2 for exactly this, and the tokeniser did not
change; expansion and quote removal are one pass, because the characters a
value supplies are never quoting characters. `exit` and `export` are special
built-ins and `cd` and `pwd` are not, as Section 2.14 divides them. A command
that is not a built-in is answered with the 127 of a command not found, which
is the truth of it until 8.4. Sections 12 and 13.

**The evidence is a status.** `dir-check` asserts the working directory from
the only place it can be asserted — a program — and ends with the number that
failed; and the shell is run upon a session whose `exit $F$G$H$Y` is 137 only
if assignment, export, expansion, `cd` into a directory, `cd` into nothing,
`cd` into a file, `&&` and `||` all did what they should. Three negative tests
were caught. Section 14. **Six limitations are recorded**, of which the first
is that the working directory is a path reduced lexically and not a held
node, and the third that no environment reaches a program until 8.4's
`execve` carries it. Section 15.

**(d)** Sub-task 8.4 is what the phase was for: a command that is not a
built-in is sought upon `PATH` (`/bin` when unset), forked, executed with the
exported variables as its environment, and waited for, with the statuses
Section 2.8.2 assigns — 127 not found, 126 found and not runnable, the
program's own otherwise. The search happens in the child by executing each
candidate, this kernel having no call that asks whether a file exists short of
opening it. `getenv` arrives in `<stdlib.h>` with the first program that could
have used it. [`../design/SHELL.md`](../design/SHELL.md), Sections 16 and 17.

**The first program the shell ran found a defect in the system-call entry
path that had stood since 6.7.** The caller's stack pointer was saved in the
per-processor block and restored from there at `SYSRET`; `wait` runs a child
within the parent's call, and a child that executed `SYSCALL` overwrote it.
Five sub-tasks of `fork` and `wait` never saw it because a child that exited
from the cloned stack had the same pointer; the first child to become another
program did, and the shell resumed upon the child's stack. The pointer is now
restored from the per-thread frame. Section 16.3, and
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), Section 9.1.

**The evidence is a status again.** `env-check` is written onto the root by
the test — a program written there at run time being found and run is itself
an assertion — and the shell's session ends with 168 only if the vector, the
environment, a program's failure, a not-found and a `PATH` search all behaved;
`dir-check` asserts that a parent's stack survives a child's `execve`. Two
negative tests were caught. Section 18.

**(e)** Sub-task 8.5 is redirection, and it closes the two limitations
[`../design/LIBC.md`](../design/LIBC.md), Section 12.7, held for it since 7.6.
**A call that creates or writes a file**: `open` takes WRITE, CREATE, TRUNCATE
and APPEND and a mode, and `write` reaches a file. **A descriptor a child
inherits**, and its cause: an open file of the filesystem layer counts its
holders, so `dup2` and inheritance are both two numbers of one file and one
position, a child of `fork` inherits everything, and `execve` keeps it — it
closed everything from 7.6 to 8.4, the safe half of the rule while nothing
could mean to keep a descriptor, and the redirection is what means to. The
redirections are performed in the child between `fork` and `execve`, in the
order written, so that `>out 2>&1` and `2>&1 >out` differ as they should.
`rmdir` arrives beside them, and with the writable file the utilities
`touch`, `cp` and `rmdir`, the built-ins `help`, `true`, `false` and `unset`,
and `cat` copying its standard input at last.
[`../design/SHELL.md`](../design/SHELL.md), Section 19.

**The evidence is what the files hold.** `file-check` asserts the four
flags, the shared position of two numbers and of a parent and its child, and
the rule that the kernel's own paths may not be duplicated above the three;
the shell's fourth session uses every operator and the kernel reads each file
back. Two negative tests were caught, one of them — a close releasing a file
from under its other holder — six times over. Section 20. **Five limitations
are recorded**, the first that a redirection upon a built-in is named and not
performed. Section 21.

**(f)** Sub-task 8.6 is pipelines, and it is the sub-task in which this kernel
first ran two programs at once. IEEE Std 1003.1-2017, Section 2.9.2: each
command in a subshell, the output of each connected to the input of the next,
the status the last command's or its inverse after `!`. The shell makes one
child per command with a pipe between each pair, closes every end where it is
not needed — a write end left open in the shell keeps every reader waiting for a
writer that has gone, which the negative test showed — and a built-in in a
pipeline runs in the child, which is what makes `help | wc -l` count something.
The pipe is an open file of the filesystem layer with no node beneath it, a
page of buffer, a reader that sleeps while it is empty and a writer that sleeps
while it is full, and `EPIPE` for a writer whose reader has gone.
[`../design/SHELL.md`](../design/SHELL.md), Sections 22.1 and 22.2.

**The larger half is beneath the shell.** A child of `fork` had run upon its
parent's flow of control inside the parent's `wait` since 6.11, and a pipeline
cannot be run that way. So a child is admitted to the scheduler at the fork;
`wait` sleeps upon a wait channel — the first thing in this kernel a thread
could sleep upon and be woken from — and is woken when a child ends; a user
thread is pre-empted at privilege level 3 and nowhere else, so that the kernel
beneath a system call stays unentered by a second thread; the thread to return
to became a field of the started thread, one pointer per processor having
become wrong the day a program could sleep while another ran; and the counted
interrupt-disable travels with a thread across a switch, the first sleeper
resumed from the idle thread having otherwise left its system call with
interrupts enabled. The bootstrap processor gained an idle thread of its own.
[`../design/PROCESS.md`](../design/PROCESS.md), Section 17;
[`../design/SCHEDULER.md`](../design/SCHEDULER.md), Section 9;
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 4.1.

**The evidence is what crossed the pipe.** `file-check` sends twelve kibibytes
from a child through a four-kibibyte pipe and finds every byte in order; the
kernel asserts the pipe from a caller that cannot sleep, including the refusal
of a read that would block; and the shell's fifth session carries the whole of
`/bin/sh` through `cat | wc -c` and composes `exit $T$F$N` from the statuses of
`false | true`, `true | false` and `! true | false`. Two negative tests were
caught, and one defect the change made — a child admitted with its stack
unprepared — was found by the first program to fork after it. Section 23.
**Six limitations are recorded**, the first that there is no `SIGPIPE` until
8.7. Section 24.

**(g)** Sub-task 8.7 closes the phase: job control, process groups and the
signals the terminal delivers. A signal is a bit in the target process, set by
whoever sends it and acted upon by the target itself on its next way out of
the kernel — a system call returning or an interrupt taken at privilege level 3
— which is the one moment every register stands in a frame the kernel can
edit; a sender that reached into a target asleep upon a pipe would rewrite a
frame the read was still going to return through. A sender wakes a sleeping
target instead, the call reports `EINTR`, and the way out delivers: ignore,
terminate, stop until SIGCONT, or a handler entered upon the program's own
stack and returned through a two-instruction restorer, the interrupted
context restored whole and the flags a program may not alter masked. A call a
signal interrupted is made again where the signal entered no handler, so that
`cat` stopped by control-Z and continued by `fg` goes on reading.
[`../design/PROCESS.md`](../design/PROCESS.md), Section 18.

**The terminal holds a foreground process group**, and control-C and
control-Z are signals to it rather than input: removed at the head of the
queue by whoever polls next — a reader, or the bootstrap processor's tick, so
that a program which never reads is reached — and delivered in order with the
bytes before them. A background reader is stopped by SIGTTIN inside its
`read`. The shell makes every pipeline a job in a group of its own, gives a
foreground job the terminal and takes it back, ignores SIGINT and SIGTSTP
itself while every child puts the default back, and governs the rest with
`jobs`, `fg`, `bg` and `kill`. `waitpid` names a child, declines to sleep, and
reports a stop; the status is an encoding of kind and number at last.
[`../design/SHELL.md`](../design/SHELL.md), Section 28.

**The evidence is a program ended, stopped and continued from outside itself.**
`signal-check` asserts a handler entered and returned through, the two
withheld signals, a computing child reached by the timer's interrupt and a
sleeping one by the wake, SIGPIPE, a fault as SIGILL, a group ended by one
`kill`, and a child stopped, reported once, continued and collected; the
kernel asserts the rules a program can only see the consequence of; and the
shell's sixth session composes its exit from control-C, control-Z, `bg`,
`fg` and `kill`. **Three defects were found by the tests**: a child's
descriptors released at its collecting rather than its ending, which left a
background pipeline waiting for a keypress; a stopped job `kill` could not end;
and a `%n` in `help`'s own format string. **Six limitations are recorded**,
the first that a control-C does not flush what was typed after it, and the
last that the alpha is not yet cut. Section 29.

**Sub-task 8.7 closes Phase 8 and is where the first release is cut** — the
sub-task and the cut both of 2026-09-16, [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md).
`Oxys 1
Alpha` is the first image worth handing to somebody, because it is the first one
that does anything when they type at it.
[`VERSIONING.md`](VERSIONING.md), Section 11.1, is the plan and Section 5.4 the
rules a pre-release is published under. **The alpha and the beta belong to the
first release alone**; every release after `Oxys 1` is expected to be preceded by
internal debug builds and nothing else, which Section 5.5 holds apart from
releases entirely.

---

## Phase 9 — The Desktop, its System Services and its Configuration

**Objective**: Build a usable desktop upon the graphics of Phase 6: the window
system that arranges it, the long-lived processes that maintain it, the
configuration those processes read, and the applications a person operates it
with.

**Specifications**: IEEE Std 1003.1-2017 (process lifetime, signals and the
descriptors a service inherits); System V ABI for AMD64; UEFI Specification 2.10,
Section 12.9 (Graphics Output Protocol), where Phase 12 supplies the framebuffer
in place of Phase 6.

Phase 6 supplied the framebuffer, the primitives, the font, the pointer and the
compositing surface — everything that can be built without a process to own it.
What remains is everything that cannot, which is why this phase follows the shell
and not the framebuffer. The division of the graphical work between Phase 6 and
this phase is argued in [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md),
Section 4.1, which records what it cost.

**Sub-tasks 9.1, 9.2 and 9.3 are complete**, all on 2026-09-17, and 9.1 is the
first thing built against the preference below — with the boot screen and the
power screen of 9.3 drawn to the same rule, a ring of discs and one line of
text, [`../design/INIT.md`](../design/INIT.md), Section 5: [`../design/WINDOWS.md`](../design/WINDOWS.md),
Section 4, is where the frame and the palette are judged against it, and
records that a flat frame with a disc for its one control was reached and that
rounded corners and any asymmetry were not. It also changed what the default
menu entry boots — the window manager, with the shell upon the serial line —
and added the **Shell-only** entry beside the renamed **Shell Diagnostics**, at
the project owner's direction; Section 7.2 of that document.

**The appearance of what 9.5 to 9.8 present** is not decided here, but the
preference it is to be designed against is written down:
[`INSPIRATIONS.md`](INSPIRATIONS.md), Section 3 — a contemporary playful
minimalism, modernist and playfully geometric. A retro-styled desktop is
expressly not wanted, that section stating why the prohibition is written down
rather than left implied.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 9.1 | Implement a stacking window manager with focus and event routing. | Implemented | `KernelVerifyWindows`, `KernelVerifyCircle` |
| 9.2 | Implement the client protocol by which user processes create, draw and receive events upon windows. *(The surface interface of 6.6 is revisited here against its first real client.)* | Implemented | `KernelVerifyClients`, `window-check` |
| 9.3 | Implement `init`: the first user process, the supervision of the services below it, and the orderly shutdown of both. | Implemented | `KernelVerifyInit`, `init-check` |
| 9.4 | Define the system configuration format, its parser, and the `/etc` hierarchy the services and the desktop read at start. | Implemented | `KernelVerifyConfig`, `config-check` |
| 9.5 | Implement the session: the desktop root, the panel, the launcher, and the ownership of the display that decides who may draw upon it. | Planned | — |
| 9.6 | Implement a terminal emulator window hosting the Phase 8 shell. | Planned | — |
| 9.7 | Implement the utilities the desktop is not usable without: a file manager, a text viewer and a clock. **`Oxys 1 Beta` is cut here.** | Planned | — |
| 9.8 | Implement the settings application, by which the configuration of 9.4 is edited rather than hand-written. | Planned | — |

**`Oxys 1 Beta` is cut at 9.7 and not at the end of this phase**, which is
deliberate: a desktop whose settings are edited in a text file is a desktop
somebody can use and complain about, and that is what a beta is for. Waiting for
9.8 would leave the beta and the release two phases apart.
[`VERSIONING.md`](VERSIONING.md), Section 11.1.

---

## Phase 10 — Cryptography

**Objective**: Provide primitives for random-number generation, hashing and
symmetric encryption.

**Specifications**: FIPS 180-4 (SHA-256); FIPS 197 (AES); NIST SP 800-38A
(modes of operation); NIST SP 800-90A (deterministic random bit generators);
Intel SDM Volume 2B (`RDRAND`, `RDSEED`).

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 10.1 | Implement an entropy pool seeded from `RDSEED`/`RDRAND` where available and from timer jitter otherwise. | Planned | — |
| 10.2 | Implement a cryptographically secure deterministic random bit generator. | Planned | — |
| 10.3 | Implement SHA-256 with the FIPS 180-4 test vectors. | Planned | — |
| 10.4 | Implement AES-128 and AES-256 with the FIPS 197 test vectors. | Planned | — |
| 10.5 | Implement CBC and CTR modes of operation. | Planned | — |
| 10.6 | Expose the primitives to user space by system call and by a `/dev/random` device node. | Planned | — |

---

## Phase 11 — Networking

**Objective**: Provide a functional TCP/IP stack.

**Specifications**: IEEE 802.3; RFC 826 (ARP); RFC 791 (IP); RFC 792 (ICMP);
RFC 768 (UDP); RFC 9293 (TCP); RFC 2131 (DHCP); Realtek RTL8139 or Intel 8254x
datasheet.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 11.1 | Implement an Ethernet controller driver (RTL8139 or Intel E1000) with descriptor rings and interrupt handling. | Planned | — |
| 11.2 | Define the network buffer structure and the protocol layering framework. | Planned | — |
| 11.3 | Implement Ethernet frame transmission and reception. | Planned | — |
| 11.4 | Implement ARP with a resolution cache. | Planned | — |
| 11.5 | Implement IPv4, including fragmentation and reassembly, and a routing table. | Planned | — |
| 11.6 | Implement ICMP echo request and reply. | Planned | — |
| 11.7 | Implement UDP. | Planned | — |
| 11.8 | Implement TCP: the state machine, sequence-number handling, retransmission and flow control. | Planned | — |
| 11.9 | Implement the BSD-style socket system-call interface. | Planned | — |
| 11.10 | Implement DHCP client configuration and the `ping` utility. **`Oxys 1` is cut at or about here.** | Planned | — |

**Sub-task 11.10 closes Phase 11, and `Oxys 1` is cut at or about it** — "about",
because it is the one of the three planned releases whose trigger the project
owner left approximate. **It is cut before Phases 12 and 13**, so `Oxys 1` boots
by BIOS alone, carries none of Phase 13's hardening, and will not have been tested
upon the three machines sub-task 13.7 requires. Those are what `Oxys 2` is for.
[`VERSIONING.md`](VERSIONING.md), Section 11.1, states each of them plainly rather
than leaving them to be discovered by whoever installs it.

---

## Phase 12 — UEFI Transition

**Objective**: Boot the identical kernel by native UEFI as well as by legacy
BIOS.

**Specifications**: UEFI Specification 2.10; Microsoft PE/COFF Specification;
ACPI Specification 6.5.

`make run-uefi` exists already and is expected to fail; it is provided in advance
so that this phase has an established point of entry. Sub-task 12.7 renders it
functional. See [`TESTING.md`](TESTING.md), Section 3.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 12.1 | Establish a PE32+ build path for a UEFI application image. | Planned | — |
| 12.2 | Implement the UEFI entry point and parse the System Table. | Planned | — |
| 12.3 | Retrieve the memory map, the ACPI RSDP and the Graphics Output Protocol framebuffer by Boot Services. | Planned | — |
| 12.4 | Define a boot-protocol-neutral handoff structure consumed by the kernel, populated identically from Multiboot2 or from UEFI. | Planned | — |
| 12.5 | Invoke `ExitBootServices` and transfer control to the kernel. | Planned | — |
| 12.6 | Integrate UEFI Runtime Services: time and variable access. | Planned | — |
| 12.7 | Produce a hybrid ISO image bootable by both BIOS and UEFI, and verify under OVMF. | Planned | — |

---

## Phase 13 — Polish, Optimisation and Final Hardening

**Objective**: Prepare the system for release.

**Specifications**: Intel SDM Volume 3A, Chapters 4 and 5 (SMEP, SMAP, NX).

Sub-task 13.3 is depended upon by name from two earlier documents:
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), limitation 8, and
[`../design/EXECUTABLE.md`](../design/EXECUTABLE.md), limitation 4. The user
mappings those protections exist for came into being at sub-task 6.10.

| # | Sub-task | State | Asserted by |
| - | -------- | ----- | ----------- |
| 13.1 | Profile interrupt latency, context-switch cost and filesystem throughput. | Planned | — |
| 13.2 | Optimise the scheduler for fairness and the block layer for read-ahead. | Planned | — |
| 13.3 | Enable NX, SMEP and SMAP; enforce write-exclusive-or-execute in kernel mappings. | Planned | — |
| 13.4 | Implement kernel stack guard pages and stack-canary protection. | Planned | — |
| 13.5 | Implement kernel address-space layout randomisation, if feasible. | Planned | — |
| 13.6 | Complete and review the whole of the `docs/` corpus. | Planned | — |
| 13.7 | Test on a minimum of three distinct physical machines, including UEFI systems. | Planned | — |
| 13.8 | Extend the userland utility set and produce the final release image. | Planned | — |

---

## Beyond the thirteen phases — self-hosting

**The objective**: a machine running Oxys-OS can check out this repository, build
it, and produce a bootable image of Oxys-OS — with no other operating system
involved at any step. The project is self-hosting when the cross-compiler of
`PROJECT_GUIDELINES.md`, Section 3, is no longer the only way to make it.

This is a statement of direction and not a fourteenth phase. It has no sub-tasks,
no ordering and no assertions, because the work it names has not been broken down
yet — only its shape is settled. It is written down so that the work leading
towards it is not foreclosed by accident — a system call omitted, a filesystem
left read-only, a toolchain assumption baked into the build — which is the way
this goal is usually lost.

### A. What it actually requires

Self-hosting is not one piece of work. It is the point at which several
independent lines of work happen to meet, and most of them are already on the
roadmap for their own reasons.

| Requirement | Where it stands |
| ----------- | --------------- |
| A filesystem that can be written to, with directories and a mountable root | **Phase 5.** Present. |
| Processes that can fork, execute a program from a volume, and be collected | **Phase 6**, sub-task 6.11. Present. |
| A scheduler, so that a build runs while other work does | **Phase 6**, sub-task 6.15. Present, though a user thread is confined to the bootstrap processor until the locks of [`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10, limitation 1, are applied. A parallel build is what will first want that. |
| A C library a compiler can be built against | **Phase 7**, begun at sub-task 7.1, whose scope Section B enlarges: a libc sized for `ls` and `cat` is nowhere near enough for a ported compiler to link against. The string functions are the floor and are deliberately unoptimised; [`../design/LIBC.md`](../design/LIBC.md), Section 6, limitation 1, records that a ported compiler is the workload that will justify measuring them. |
| A shell, a job-control model, and pipes | **Phase 8.** Planned. A build system is a program that runs programs. |
| An assembler and a linker | **Porting work, not yet scheduled.** Section B settles that they are ports; which ones, and when, is not decided. |
| A C compiler that runs upon Oxys-OS | **Porting work, not yet scheduled.** Section B settles it; which compiler is a judgement better taken once Phase 7 has shown what each would demand. |
| A text editor, and enough of a utility set to work in | **Phase 13**, sub-task 13.8, in outline only. |
| Storage, memory and time enough to compile a kernel on the machine itself | An open question. It bears on Phase 13's optimisation work and on what hardware the final image targets. |

Three of those rows are porting work that has not been scheduled, and the honest
summary is that self-hosting is presently **further away than the thirteen phases
are long**. Recording it is worth doing anyway: several of the rows above are
cheaper to get right the first time than to retrofit, and the ones not yet
scheduled are easier to plan for if it is known they are coming.

### B. The compiler is ported, not written

**Decided by the project owner: Oxys-OS ports a C compiler rather than writing
one.** `PROJECT_GUIDELINES.md`, Section 2, records the rule that follows from it
— external tools, toolchains and their supporting libraries may be ported and
depended upon — and Section 8's prohibition on third-party code now says
explicitly that it stops at the kernel.

The alternative was to write a compiler here. It was declined on the plainest
possible ground: a compiler good enough to compile this kernel is a larger
undertaking than the thirteen phases combined, and one that is *not* good enough
does not reach the objective at all. Porting reaches it, and the cost is that the
system depends upon code this project did not write.

**What that changes about the work**:

- **Phase 7's C library is sized for a compiler, not for utilities.** This is the
  decision's largest consequence and it lands earliest. A libc that supports `ls`
  and `cat` is a fraction of what a ported compiler links against, and the
  difference is far cheaper to plan for than to discover.
- **The three unplanned rows in Section A become porting work**, not original
  work: a C compiler, an assembler and a linker. Which compiler — a small one,
  or GCC, or LLVM — is not decided here; it is a judgement about how much libc
  and how many system calls each demands, and it is better taken when Phase 7
  has shown what those cost. **The project owner has named `tinycc` as an
  acceptable candidate**, on 2026-09-10, without narrowing the choice to it —
  the words were "tinycc or just any C compiler". It is recorded because a
  candidate somebody has already assented to is worth knowing when the judgement
  is finally taken, and because `tinycc` is the small end of the range and
  therefore the cheapest thing to size Phase 7's library against first.
- **A port is held apart from original source**, in a directory of its own,
  under its own licence, recorded in [`../../LICENSING.md`](../../LICENSING.md)
  before it is committed, and modified no further than the port requires. The
  rule is in the guidelines rather than here because it binds every port and not
  only the compiler.

The identity in `PROJECT_GUIDELINES.md`, Section 1, states the position this
implies: the kernel and userland are written from scratch, and the system may run
and depend upon ported third-party tools.

### C. What to avoid foreclosing in the meantime

These cost nothing now and are expensive to retrofit:

1. **The build must not assume the host is Linux.** `Makefile` and the scripts
   beside it should keep to what a POSIX shell provides, so that the day they are
   run under an Oxys-OS shell is a day of finding bugs rather than of rewriting.
2. **System calls should be added in the shapes a real program expects**, and
   not in whatever shape the self-test that first needed them found convenient.
   A compiler wants files, directories, pipes and processes; each is far cheaper
   to get right at the moment it is introduced.
3. **The filesystem must stay writable and must stay correct under a load it has
   never seen.** A build writes many files, of many sizes, quickly.
   [`../storage/BUFFER.md`](../storage/BUFFER.md) records that the buffer cache
   is the first structure that will want a lock it may sleep upon; a build is the
   workload that will demonstrate it.
4. **The `docs/` corpus should keep saying which parts are self-hosting
   obstacles**, in the file that owns each, rather than accumulating the list
   here. A limitation named in one place is one somebody can find.

---

## Where the rest of the detail lives

| Question | Document |
| -------- | -------- |
| What does the system do today, and where has it been observed to work? | [`STATUS.md`](STATUS.md) |
| How did it come to be that way? | [`HISTORY.md`](HISTORY.md) |
| Why is a subsystem built the way it is? | [`../design/`](../design/), [`../devices/`](../devices/), [`../storage/`](../storage/) |
| Why are the phases in this order? | [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 4 |
| How is any of it tested, and what was the result? | [`TESTING.md`](TESTING.md) |
| What are the conventions the work is bound by? | [`../../PROJECT_GUIDELINES.md`](../../PROJECT_GUIDELINES.md) |
