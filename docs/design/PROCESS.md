<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Process

**Phase**: sub-tasks 6.9 to 6.11, 7.5, 7.6, 8.5 to 8.7 and 9.3 of
[`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/proc/process.c`](../../kernel/proc/process.c),
[`../../kernel/proc/signal.c`](../../kernel/proc/signal.c),
[`../../kernel/arch/x86_64/proc/switch.asm`](../../kernel/arch/x86_64/proc/switch.asm),
[`../../kernel/arch/x86_64/syscall/sigframe.c`](../../kernel/arch/x86_64/syscall/sigframe.c),
[`../../kernel/include/oxys/proc/process.h`](../../kernel/include/oxys/proc/process.h),
[`../../kernel/include/oxys/proc/signal.h`](../../kernel/include/oxys/proc/signal.h).
**Specifications**: Intel SDM, Volume 3A, "IRETQ" and Section 6.12; System V
ABI, AMD64 supplement, Section 3.4.1 (the initial process stack); IEEE Std
1003.1-2017, `fork`, `execve`, `_exit`, `waitpid`, `kill`, `sigaction`, and
Sections 2.4.1 and 2.4.3 (signal generation and default actions).

The structures a running program is held in and the calls by which programs
make, replace, end and signal one another: processes and threads, their stacks,
the context switch, the descent to privilege level 3 and the ways back, `fork`,
`execve`, `exit`, `wait` and `waitpid`, and signals and process groups. The
system-call dispatch and argument validation are [`PRIVILEGE.md`](PRIVILEGE.md);
the scheduler is [`SCHEDULER.md`](SCHEDULER.md).

## 1. Processes and threads

A process holds an address space, an identifier, its parent's identifier, a
group, a descriptor table, a pending signal set and its dispositions, and the
extents of its image, stack and heap. A thread holds a saved context and a
kernel stack. Both are entries of growing tables
([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md), Section 14).

- **A thread is a structure of its own because its kernel stack must be.** Two
  threads of one process share every page, but if they shared the stack the
  kernel is entered on, a system call by one would build its frame on the stack
  the other was using and return into it: kernel state corrupted by threads that
  never touched each other's memory.
- **Identifiers are numbers, never reused, and slots are.** A parent records its
  child by number, so a parent that outlives its child finds nobody rather than
  whoever took the slot next; a pointer or an index would name the wrong process
  convincingly, and signalling the wrong process is how a program kills an
  unrelated one.
- **The extents live in the process control block**, because an address space
  is only a paging hierarchy and cannot say what it maps or why.

## 2. Stacks and the heap

**The kernel stack** is five arena pages: four of stack and a guard beneath it,
left **mapped read-only**. An overflow is a write, and a write to a read-only
page faults; an unmapped guard could not be released, `KernelPagesFree` treating
an unmapped page in a range as a lost mapping. The stack pointer begins one past
the last byte, since a push decrements before it writes.

**The user stack** is sixteen zeroed pages below `PROCESS_USER_STACK_TOP`, with
an **unmapped** guard below, which costs nothing in an address space and catches
reads as well as writes. It is at the far end of the lower half so that it
cannot grow into the program's data. The pages are zeroed because a frame holds
whatever its last owner left, and the stack is the first thing a program reads.

**The break** is the heap's extent, `break_start` fixed when the image is loaded
and `break_current` moved only by `brk`:

- **It is re-established whenever the image changes**, at load and at `execve`,
  since a break carried across an `execve` could lie inside the new program's
  `.bss`.
- **A process with no image has no heap**, since one derived from an image end
  of zero would sit in the unmapped first page.
- **A forked child inherits both bounds**, since its heap pages were cloned; a
  child with its parent's data but an empty break would map fresh frames over
  pages it still used.

## 3. The context switch

`ThreadSwitchContext` is an ordinary function call, so the System V convention
has already saved everything the compiler wanted: **a context is six registers
and a stack pointer** (`RBX`, `RBP`, `R12` to `R15`, `RSP`). The instruction
pointer is the return address on the stack. One context format serves every
switch; an interrupt-driven switch would need all registers and a second format.

**A thread that has never run is given a return address and nothing else**,
into `ThreadTrampoline`, since the switch keeps the six registers in the context
structure rather than popping them. A quadword of padding above it leaves the
stack as an ordinary call would, eight modulo sixteen at entry, which the
compiler is entitled to assume.

`ThreadSwitchTo` also carries the per-processor interrupt state (the critical
depth and the saved interrupt flag) out of the outgoing thread and into the
incoming one: a thread sleeps inside its own masked section and is resumed by
whichever thread switched next, whose state would otherwise enable interrupts
inside a system call ([`CONCURRENCY.md`](CONCURRENCY.md)).

## 4. The descent to privilege level 3

`ThreadEnterUser` clears every register, because what is left in one is a
kernel address as often as not, and a program that printed it would disclose it
with no fault to report. Then `IRETQ`, which unlike a far return also loads
`RFLAGS`, pops:

| Value | Why |
| ----- | --- |
| `SS`: user data selector, RPL 3 | RPL 3 makes it a return to an outer level; without it the program runs in the kernel. |
| `RSP`: 48 bytes below the stack's top | Where the ABI's initial frame stands (below). |
| `RFLAGS`: `0x202` | The interrupt flag, so the program can be pre-empted; bit 1 is architecturally one. |
| `CS`: user code selector, RPL 3 | As `SS`. |
| `RIP`: the image's entry point | |

**The initial stack is the ABI's** (Section 3.4.1): the argument count at the
stack pointer, the argument pointers, a null, the environment pointers, a null,
and an auxiliary vector ending in a null entry, with the stack pointer 16-byte
aligned. `ProcessLayOutArguments` builds it downward: the strings at the top,
then the vectors naming them, then the count, the pointer aligned after the frame
is sized. It writes through the direct map of the stack's own frames, since the
space being filled is usually not the active one. A process with no arguments
gets the same frame of zeroes, 48 bytes rather than 40 so that it is aligned: a
misaligned entry faults in the first aligned move of code the program did not
write ([`LIBC.md`](LIBC.md)).

## 5. The ways back into the kernel

| Way | What happens |
| --- | ------------ |
| A system call | Returned by `SYSRET` ([`PRIVILEGE.md`](PRIVILEGE.md)). |
| An interrupt, including the timer | Returned by `IRETQ`; the timer may switch to another thread ([`SCHEDULER.md`](SCHEDULER.md)). |
| A fault | The program is ended, reported as the signal its vector maps to (Section 9), and its parent collects it. |
| `exit` | The program is ended with the status it chose. |

## 6. What a transition owes

Two pieces of machine state are **written at every transition** rather than left
to history, because a thread resumed can have entered the kernel either way.

- **The `GS` bases are written, not exchanged.** `SWAPGS` exchanges the kernel
  and user values, so which one `GS.base` holds depends on how the kernel was
  entered. `SyscallEstablishKernelGsBase` writes it where a thread resumes and
  `SyscallEstablishUserGsBase` where one departs. Exchanging would, after a child
  that ended by faulting, hand the per-processor block to privilege level 3 and
  leave the next `SYSCALL` looking for its stack at address 8.
- **The kernel stack is recorded twice** and both copies follow the current
  thread. `SYSCALL` switches no stack, so its entry path reads the stack from
  the per-processor area rather than `rsp0`; `ThreadSetCurrent` writes both.
  With one copy stale, a child's system call builds its frame over a parent
  suspended in `wait`, and the parent returns through the child's registers.

## 7. `fork` and `execve`

**`fork`** clones the address space by copy-on-write
([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md)), copies the parent's saved system-call
frame into the child's thread with `RAX` zero, and admits the child to the run
queue with its stack prepared for the trampoline. The child runs when the parent
sleeps or is pre-empted, and runs even if the parent never waits.

- **The child keeps its parent's registers**, not cleared as a new program's
  are: the convention entitles code after a call to find `RBX`, `RBP` and `R12`
  to `R15` unchanged, and every value is one the parent already held at
  privilege level 3.
- **It leaves the kernel by `ThreadResumeUser` and `IRETQ`**, restoring the
  whole frame, because `SYSRET` takes its return address and flags from `RCX`
  and `R11`, which nothing loaded for a thread reached by a switch.
- **The child inherits its parent's descriptors, group and dispositions.**

**`execve`** builds the new program entire before touching the old: a fresh
address space with the image loaded from its path, and only then the exchange.

- **Arguments and environment are copied out of the caller before anything is
  destroyed**, into a `ProcessArguments` on the kernel stack, since the strings
  stand in the address space about to go. Both bounds of the ABI are enforced
  before the point of no return, so a program that exceeds one keeps running.
- **The new space is activated before the old is released**, because the space
  the processor translates through cannot be dismantled; the kernel half is the
  same in both.
- **Refusals are distinguished**: a path that leads nowhere and an image the
  loader refuses are `ENOENT`, a frame that could not be had is `ENOMEM`, so a
  program is not sent looking for a missing file when memory ran out. Past the
  point of no return a failure ends the process with a status saying which.
- **Descriptors are kept**, as IEEE Std 1003.1-2017 has it without
  close-on-exec, which is how the shell's redirections reach a program; handlers
  are reset to the default and ignores kept; the process takes the program's
  name, the last component of its path.

## 8. Ending and collecting

**What a process held is given back at its ending**, in `ThreadTerminateCurrent`,
not when it is collected: its descriptors, so a pipeline that finished in the
background delivers its end of file at once; its windows; and its children,
given to `init` by `ProcessAdoptOrphansOf`, which is woken if one of them has
already ended ([`INIT.md`](INIT.md)). The parent is woken on its own process as
the channel and sent SIGCHLD.

**`wait` and `waitpid`** (`ProcessWaitFor`) choose one child by identifier, any
child, or any of a group:

- **An ended child is preferred** over a running one, so children are collected
  as they finish.
- **The caller's buffer is validated before anything is collected**, since a
  status produced with nowhere to go would lose the child's outcome for good.
- **The validation resolves a copy-on-write fault** on the page it will write,
  since `wait`'s status often lands in a page the fork just made read-only;
  refusing would fail for a reason the caller cannot see or correct.
- **`SYSCALL_WAIT_NO_HANG`** returns 0 where nothing has ended;
  **`SYSCALL_WAIT_UNTRACED`** reports a stopped child once per stop, without
  collecting it.
- **The status is encoded**: a kind in bits 8 to 15 (exited, signalled,
  stopped) and a number in bits 0 to 7, the code or the signal. `exit_status`
  keeps the full quadword for the self-tests.
- **It sleeps on the caller's own process.** The kernel's own flow of control,
  which cannot sleep, instead starts the child by `ThreadStart` and returns when
  it ends, which is also what a machine without a calibrated timer uses.

The identifier returned names nobody by the time the caller sees it; the slot,
threads and address space are already released.

## 9. Signals and process groups

**A signal is a pending bit, set by the sender and acted on by the target**, on
its next way out of the kernel, the one moment all its registers stand in a
frame the kernel can edit. A sender never touches the target's frame; where the
target sleeps in a call, it is woken, the call re-tests its condition, finds the
signal, and reports `EINTR`.

- **Delivery takes the lowest pending signal** and acts by its disposition:
  ignore, the standard's default (terminate; ignore for SIGCHLD; stop for
  SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU; continue for SIGCONT), or a handler.
- **A signal that would do nothing is discarded at generation**: an ignored one,
  an unrequested SIGCHLD, a SIGCONT to a running process. A sleeping call is
  never woken for nothing.
- **SIGKILL and SIGSTOP cannot be caught or ignored**; a stop discards a pending
  SIGCONT and SIGCONT a pending stop (Section 2.4.1).
- **The numbers are the x86 System V and Linux ones**, so `kill -9` means what a
  person expects, and a fault is reported as SIGSEGV, SIGILL, SIGFPE, SIGBUS or
  SIGTRAP by its vector.

**Stopping** marks the process stopped, wakes and signals the parent, and sleeps
on its own `stop_channel` until SIGCONT; only SIGKILL and SIGCONT reach it
meanwhile, the rest waiting until it continues.

**Groups.** Every process is in a group, the identifier of its leader; a child
begins in its parent's, `setpgid` moves oneself or a child, and `kill` with a
negative identifier reaches every member. The terminal's foreground group
receives control-C and control-Z, and a member of another group that reads the
terminal is stopped by SIGTTIN inside the `read`, which retries when continued.
The foreground group is cleared when its last member ends.

**A handler runs on the program's own stack**, below the ABI's 128-byte red
zone, aligned as a call, with the signal number in `RDI` and the C library's
restorer as its return address. The restorer calls `sigreturn`, which puts back
the whole interrupted context from a `SignalContext` above the return address.

- **One delivery serves both frames** a program's registers can stand in, the
  `SyscallFrame` of a system call and the `TrapFrame` of an interrupt, through
  two small adapters to one register set.
- **The flags a program hands back are masked**: only the status flags, `DF`
  and `TF` are taken, `IF` is forced set, and an instruction pointer above the
  user limit is refused, since `SYSRET` loads what it is given.
- **A call a signal interrupted is restarted** where no handler ran (ignored, or
  stopped and continued): the instruction pointer moves back to the `SYSCALL` and
  `RAX` back to the number, so `cat` stopped by control-Z reads on after `fg`. A
  handler that ran leaves `EINTR` to the program.

## 10. Threads started by the kernel

`ThreadStart` runs a thread by a call and returns when it ends, recording the
caller in the started thread's own `return_to`: right for a program the kernel
waits for. `ThreadLaunch` admits a thread to the scheduler with nothing to
return to, so the kernel can start the desktop and the shell side by side. A
launched process has no parent and is `init`'s when it ends. `ThreadDestroy`
clears every `return_to` naming the thread destroyed, since returning through a
released slot resumes on a stack the arena has given to someone else, silently.

## Verification

The self-tests are [`../../kernel/test/proc/process.c`](../../kernel/test/proc/process.c),
[`../../kernel/test/arch/usermode.c`](../../kernel/test/arch/usermode.c),
[`../../kernel/test/proc/lifecycle.c`](../../kernel/test/proc/lifecycle.c) and
[`../../kernel/test/proc/signal.c`](../../kernel/test/proc/signal.c), with the
programs `signal-check`, `env-check` and `dir-check` and the shell's sessions.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Two processes get different address spaces; a child records its parent by number; a new process does not reuse an old identifier. | Programs overwriting each other; a signal to the wrong process. |
| Two threads get different kernel stacks, each starting at its top; the page beneath is not writable and the first page of the stack is. | A system call returning into another thread; an overflow into a neighbour; a thread that faults on its first push. |
| Making a thread current points `rsp0` at its stack; a destroyed thread is not current; destroying a process destroys its threads. | The first interrupt on someone else's stack; a pointer to a released slot. |
| The arena and both tables return to what they held. | A leak of four pages per thread, or of a slot per program. |
| A second kernel thread runs once and the first resumes; a program composed in memory runs at privilege level 3, makes a system call and ends with −6 by the invalid opcode it executes. | A switch that loses a thread; a program that never ran its own instructions. |
| A fork gives a distinct hierarchy, one more clone, a child that would resume with zero and its parent's `RBX` and `R12`, its stack where its parent's is. | An empty copy; a child that cannot tell itself apart, or loses its preserved registers. |
| A program forks, has the child `execve` a program read from a volume that exits 7, collects it, forks a child that faults, collects that, and exits 7; three programs ended, a copy-on-write fault resolved, the tables balanced. | Any one of the four calls wrong: 99 is a refused `execve`, 0 a `wait` that collected nothing, a fault at address 8 a mishandled `GS` base. |
| A child that cannot be started is ended and collected; one is not collected twice. | A child left in the table for ever. |
| Signals: lowest first; discarded when they would do nothing; SIGKILL and SIGSTOP not catchable; stop and continue cancel; exec resets handlers and keeps ignores; the vectors map to their signals. | A call woken for nothing; an unkillable process; a program inheriting the shell's indifference to control-C. |
| From privilege level 3: a computing child ended by SIGTERM, a sleeping one by SIGINT, a handled one ending with its handler's code; SIGPIPE ends a writer with no reader; a group ended by one `kill` and collected by `waitpid`; a sleeping child stopped, reported once, continued, reads its byte and ends 7; NO_HANG reports 0 and ECHILD. | Delivery missing on one path; a byte lost across a stop; a call not restarted; a `jobs` that blocks. |
| The shell composes 130, 148 and 130 from control-C, control-Z and control-C, and the foreground group clears when the shell ends. | Control-C reaching the shell instead of its job. |

## Limitations

1. The tables are searched linearly; nothing on them runs often enough yet to
   want a hash.
2. A process's state is not derived from its threads'; `procinfo` reports the
   thread's.
3. No priorities and no accounting of time ([`SCHEDULER.md`](SCHEDULER.md)).
4. The process and thread tables are not yet used under their lock: user threads
   run on the bootstrap processor only ([`CONCURRENCY.md`](CONCURRENCY.md)).
5. The user stack does not grow; the heap does, because a program asks for it
   by a call naming how far, and a stack fault would need guessing.
6. A fault cannot be caught: a handler for SIGSEGV is not entered by a fault,
   which ends the program directly.
7. A signal is delivered to the process's one thread; a second thread would need
   the pending set divided.
8. `init` can itself be killed, and only the desktop is asked to stop at a
   shutdown ([`INIT.md`](INIT.md)).
