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

**Sub-task 9.5 is complete**, on 2026-09-20: the session. Three stacking layers
— a root beneath every window, the programs' windows, a panel above them all —
because an order alone cannot express either: under the old rule a panel is a
window the next press buries and a root is one the next raise hides, and a
session cannot prevent it, not seeing the presses that raise other programs'
windows. A **claim upon the display**, exclusive and released when its holder
ends, so that exactly one program may make a root or a panel and every other is
refused `EPERM` — for being the wrong program, as `power` refuses one that is
not `init`. A call by which a program draws text with the system's one face,
which stays in the kernel because a second face in an `MIT` library would be a
relicensing this project may not perform. And `/bin/session` itself: the root
carrying the boot screen's mark, the panel with its launcher, and the entries
the launcher offers read from `/etc/session.conf`.
[`../design/SESSION.md`](../design/SESSION.md).

**Sub-task 9.6 is complete**, on 2026-09-22: the terminal emulator. A window
with an **unmodified** `/bin/sh` beneath it upon a pair of pipes, which is the
property that keeps the emulator small — everything about being a shell stays in
the shell, and the shell does not know it is in a window. The character grid is
in the C library, so the whole of what stands where is asserted by the kernel's
self-test before there is a window to look at, as the line editor's editing and
the configuration's parsing are. Two things had to be added to the system for it:
**`poll`**, because a program that must wait upon its window and upon a pipe at
once had no way to, and sleeping in either alone is deafness to the other while
sleeping in neither is a machine that never halts; and a **refusal**, `tcgroup`
from a process whose standard input is not the terminal being `ENOTTY`, which is
what stops a shell in a window taking the terminal from the shell at the
keyboard — the trap `/etc/session.conf` had been carrying a paragraph about. The
shell in a window therefore runs without job control, which is stated rather than
worked around: a pseudo-terminal is what would give it back.
[`../design/TERMINAL.md`](../design/TERMINAL.md).

**The launcher draws icons from the same sub-task**, and they are **files**: an
entry of `/etc/session.conf` names one, the session reads it at start, and the
format is a header and one pixel per position with a value that means "nothing
here". A picture compiled in would need the system rebuilt to change, which a
launcher whose entries come from a file cannot afford.
[`../design/SESSION.md`](../design/SESSION.md), Section 8.

**Both pictures are drawn at the resolution they are shown at, since
2026-09-23.** The mark is a table of 192 pixels of coverage drawn one to one
at the scale of two and averaged at one, and an icon is version 2 of its
format, carrying a transparency of 256 levels, the terminal's at forty-eight
pixels — the slot's own extent. Both edges are mixed with whatever they are
drawn upon rather than being all or nothing, which was a staircase.
[`../design/SESSION.md`](../design/SESSION.md), Sections 3.2 and 8.

**The desktop has a background, and windows may be minimised and made full,
since 2026-09-23.** The background is a file on the ramdisk in a run-length
format, scaled to cover the screen; the frame carries two controls beside the
close, and the panel lists the windows so that a minimised one can be brought
back. [`../design/SESSION.md`](../design/SESSION.md), Sections 9 and 10;
[`../design/WINDOWS.md`](../design/WINDOWS.md), Section 13.

**Sub-task 9.7 is complete**, on 2026-09-23: the utilities the desktop is not
usable without. `/bin/files` lists a directory, directories first, a press
selecting and a second opening — a directory in its own window, a file in
`/bin/view`, which wraps the text to its window and wraps it again when the
window is made full. The panel carries a clock, and `/bin/date` prints the
same at the shell. Beneath them the kernel reads the date from the real-time
clock — a driver for the MC146818A's registers in both data modes and both hour
modes — at every `time` call, having been watched falling behind when the date
was the timer's count; and `alarm` sends SIGALRM at a time, served from the tick,
which is what wakes the clock at each minute. Two calls, 42 and 43, and
`<time.h>`. [`../design/UTILITIES.md`](../design/UTILITIES.md);
[`../devices/TIME.md`](../devices/TIME.md), Section 10.

**`Oxys 1 Beta` is fixed at this sub-task**, [`VERSIONING.md`](VERSIONING.md),
Section 11.1, and is cut at the project owner's direction, as the alpha was.

**Next: sub-task 9.8** — the settings application, by which the configuration
of 9.4 is edited rather than written by hand. It closes Phase 9.
