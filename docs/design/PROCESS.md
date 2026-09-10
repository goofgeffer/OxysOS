<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Process

**Phase**: 6, sub-tasks 6.9, 6.10 and 6.11, of
[`../project/PLAN.md`](../project/PLAN.md). Sections 1 to 8 are 6.9, which
defines the structures; Sections 9 and 10 are 6.10, which switches to them and
descends to privilege level 3; Sections 11 to 16 are 6.11, which gives a program
the four calls by which it governs another.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../kernel/proc/process.c`](../../kernel/proc/process.c),
[`../../kernel/proc/switch.asm`](../../kernel/proc/switch.asm),
[`../../kernel/include/oxys/process.h`](../../kernel/include/oxys/process.h).
The dispatch and the validation of a caller's arguments are
[`../../kernel/cpu/syscall.c`](../../kernel/cpu/syscall.c), whose design is
[`PRIVILEGE.md`](PRIVILEGE.md).

## 1. What this sub-task is, and what it is not

It defines the structures a running program is held in, the tables that hold
them, and the allocations each is given. It creates a process with an address
space of its own and a thread with a kernel stack of its own.

**Sub-task 6.9 ran nothing.** No context was restored, no address space made
active, and no thread had executed an instruction; that is what Sections 1 to 8
describe. Sub-task 6.10 is Sections 9 and 10, and a program has now run.

The division is deliberate rather than tidy. A structure that has never been
switched to is one whose shape can still be argued about; one that has is a
structure with assembly written against its offsets. Getting the shape settled
first is cheaper than getting it settled afterwards.

## 2. Three promises this sub-task was made

| Promised in | Promise |
| ----------- | ------- |
| [`PRIVILEGE.md`](PRIVILEGE.md), limitation 4 | Each thread takes a kernel stack from the arena **with a guard page beneath it** |
| [`PRIVILEGE.md`](PRIVILEGE.md), limitation 6 | `rsp0` is written when a thread becomes current; `TssSetKernelStack` has existed and been uncalled since 6.1 for that moment |
| [`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md), limitation 2 | An address space has no record of its own extent, and the process control block is where that record belongs |

All three are kept, and a fourth from
[`EXECUTABLE.md`](EXECUTABLE.md) — that the process is what knows how large a
user stack should be — is kept with `ProcessCreateUserStack`.

## 3. Why a thread is a structure of its own

Two threads of one process share every page of memory. They must not share the
stack the kernel is entered upon.

If they did, a system call made by one would build its frame upon the stack the
other was using, and would return into it. That is a corruption of the kernel's
own state by two threads that never touched each other's memory — and it is
invisible, every byte involved being one something meant to write.

The kernel stack therefore belongs to the **thread** and not to the processor,
and that single fact is the whole reason this structure exists separately from
the process.

## 4. What a context holds, and what it does not

Six registers and a stack pointer: `RBX`, `RBP`, `R12` to `R15`, and `RSP`.

The System V convention provides that those six are preserved across a call, so a
switch performed **as an ordinary function call** — which is what sub-task 6.10
will do — need save no others: the compiler has already spilled anything else it
cared about at the call site.

**The instruction pointer is not among them**, and that is not an omission. A
switch that returns to its caller resumes at the return address upon the stack,
so the stack pointer carries the instruction pointer with it. A thread that has
never run has no such address, which is what `entry` is for — and putting one
there is 6.10's, because 6.10 is what will know what a thread should return
*into*.

## 5. The allocations

### 5.1 The kernel stack, and its guard

Five pages from the arena: four of stack and one of guard beneath it.

The guard is left **mapped and read-only** rather than unmapped, and that is a
decision. `KernelPagesFree` panics upon an unmapped page within a range it is
releasing — deliberately, an unmapped page there meaning the caller has lost
track of what it owns — so a guard that was unmapped could not be given back
without first putting a frame under it. A read-only guard catches what an
overflow actually does: a push is a write, and a write to a read-only page
faults.

The one thing it does not catch is a *read* below the stack, which is not what an
overflow is.

Before this sub-task there was one kernel stack, and its overflow ran into the
`.bss` below — which happened to be the double-fault stack, so the overflow was
caught by the double fault. `PRIVILEGE.md` recorded that as "an accident of
placement and not a design", and with a stack per thread the accident stops
holding: the stacks are numerous and what lies below one is another one.

**The top is one past the last byte.** A stack pointer begins there because the
first push decrements it and then writes; a top set to the last byte would have
the first push write one byte beyond the reservation.

### 5.2 The user stack

Sixteen pages immediately below `PROCESS_USER_STACK_TOP`, growing downward, with
the page beneath left **unmapped**.

Unlike the kernel stack's guard this one may be a hole, because a hole in an
address space costs nothing — and a hole catches a read as well as a write.

It is placed at the far end of the address space rather than after the program's
own segments because the two must not meet. A stack growing into a program's data
corrupts it silently; putting the stack at the far end makes the gap between them
the size of the address space.

Every page is zeroed, for the reason [`EXECUTABLE.md`](EXECUTABLE.md), Section
5.3 gives: a frame arrives holding whatever its last owner left in it, and a
stack is the first thing a program reads.

## 6. Identifiers are numbers, not indices

A slot in a table is reused the moment its occupant is destroyed. An identifier
never is.

A parent therefore records its child **by number**, so that a parent outliving
its child finds nobody rather than finding whoever was given that slot next. A
pointer, or an index, would name the wrong process convincingly — and acting upon
the wrong process is how a program kills an unrelated one.

## 7. Verification

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A process is given an address space, and two processes are not given the same one | Two programs writing over one another with every appearance of isolation |
| A child records its parent, and by number | Section 6 |
| A thread knows its process and the process records the thread | A thread destroyed without its owner knowing, leaving a pointer to a released slot |
| A thread's stack pointer begins at the top of its stack, and the top is the end of the reservation | Section 5.1: a first push one byte beyond the stack |
| The page beneath a thread's stack is **not writable** | Section 5.1: an overflow that runs into whatever is below rather than faulting |
| The first page of the stack **is** writable | The other half. A guard covering the stack itself is a thread that faults upon its first push |
| Two threads of one process have different stacks | Section 3: a system call made by one returning into the other |
| Making a thread current points `rsp0` at its stack, and following it | The promise of `PRIVILEGE.md` limitation 6. A thread entered while `rsp0` names another thread's stack takes its first interrupt onto a stack somebody else is using |
| A user stack is placed where it belongs and given once | A second stack mapped over the first, losing whatever the process had pushed |
| Destroying a process destroys its threads | A thread with a pointer to a slot that no longer describes anything |
| A destroyed thread is no longer the current one | `rsp0` naming a stack that has gone back to the arena |
| **The arena returns to what it held** | A leak of four pages a thread, which is a kernel that runs out of address space after a few thousand programs |
| A new process does not take a destroyed one's identifier | Section 6 |

The arena measurement is the one that catches the widest class of fault. Every
other assertion here is about a field; that one is about whether the whole
sequence of allocations and releases balanced.

**One guard here is not asserted, and is recorded as unasserted.** `ThreadDestroy`
clears `ProcessCurrentThread` where it named the thread being destroyed, and the
table above asserts that. It did not clear `ProcessReturnThread`, which
sub-task 6.10 introduced beside it and which is worse to leave dangling:
`ThreadTerminateCurrent` switches to whatever it names, so a destroyed thread
there means loading a stack pointer out of a released slot's context and resuming
upon a kernel stack the arena has since given to somebody else — which does not
fault, but continues, wrongly, with nothing to indicate that anything happened.
The review after 6.10 added the clearing. Asserting it would need an accessor for
a variable that has no other reader, and the mechanism cannot be reached at all
while there is one thread of control (limitation 3); it becomes assertable, and
must be asserted, when something makes more than one
program's death possible.

### 7.1 The negative tests

Each was applied to `kernel/proc/process.c`, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The guard page left writable. | `the page beneath a thread's stack is writable, so it is not a guard` |
| A process destroyed without destroying its threads. | `a thread outlived the process that owned it` **and** `the arena did not return to what it held before` — the second being the one that would have caught it even had the first been thought unnecessary |
| `TssSetKernelStack` not called when a thread becomes current. | `making a thread current did not point rsp0 at its stack` and `rsp0 did not follow the thread that became current` |

## 8. Observed state

`ProcessReport` is emitted at the end of the self-tests, and upon QEMU with the
`q35` machine and 512 MiB it reads:

```
Processes: 0 of 64, threads 0 of 128; created 4 and 6 since the start.
Processes: no thread is current; 1 program(s) have run and ended.
```

**Both numbers matter and the second pair of the first line is the one worth
reading.** Zero occupied slots against four processes and six threads created is
the assertion of Section 7 restated by the tables themselves: everything the
self-tests of this sub-task and of 6.10 built was given back, so a leak would
show here as a non-zero occupancy long before it showed as an exhausted arena.
The counts are cumulative and never decrease, which is what makes them useful for
that comparison.

**The second line said `nothing has run` until the review that followed sub-task
6.10.** It was printed immediately below the log of a program running — the
program's own line of output, and the trace of the fault it raised on purpose,
stand a few lines above it. The statement was defensible on a narrow reading, no
thread being current at the moment the report is reached: the tests adopt a
thread, switch away from it, switch back, and release everything before they
return. But a report is read by whoever is looking at the log, not by whoever
wrote the condition, and a line that contradicts the evidence directly above it
teaches its reader that the report is not to be trusted.

The two facts are now separated. Whether a thread is current is one question;
whether anything has run is another, and the termination count already answered
it. The kernel says `nothing has run` only when nothing has.

## 9. The switch, of sub-task 6.10

### 9.1 Why it is a function call

A switch performed by an ordinary call inherits the calling convention's promise:
the compiler has already saved whatever it wanted to keep across the call, so
**six registers and a stack pointer are the whole of a context**.

A switch performed from an interrupt would have to save every register, the
interrupted code having made no such promise — and would then need a second,
different context format for threads switched voluntarily. One format is better
than two, and the voluntary switch is the one that happens most.

The instruction pointer is not saved. The call put a return address upon the
stack, so saving the stack pointer saves the return address with it, and
switching back returns through it.

### 9.2 The prepared frame, and the fault it caused

A thread that has never run has no history upon its stack, so one is fabricated:
the stack is given **a return address and nothing else**, and the switch's `RET`
takes it.

Nothing else, and that is the whole of what went wrong first. The frame was
prepared with six saved registers beneath the return address, in the belief that
the switch popped them — and it does not: it keeps them in the context structure
and moves them with loads and stores. The `RET` therefore took the lowest of the
six zeroes, and the machine faulted at an instruction pointer of zero with a
stack pointer that was perfectly valid. The trace said `IP=0000000000000000` and
nothing else, which is as little as a fault can say.

The frame also carries a quadword of padding above the return address, so that
the address sits sixteen bytes below the top rather than eight. A function
entered by an ordinary call finds the stack pointer eight modulo sixteen, the
call having pushed eight bytes onto a boundary; a thread entered with the other
alignment is one the compiler is entitled to assume it is not.

## 10. The descent to privilege level 3

`IRETQ` is the instruction that can do it. A far return could too, but `IRETQ` is
the one that also loads `RFLAGS`, and the flags a program starts with are part of
the state it is entitled to.

Five quadwords, pushed in the reverse of the order the instruction pops them:

| Pushed | Value | Why |
| ------ | ----- | --- |
| `SS` | The user data selector, **RPL 3** | The requested privilege level is what makes this a return to an outer level; without it the return is same-privilege and the program runs in the kernel |
| `RSP` | The top of the process's user stack | |
| `RFLAGS` | `0x202` | The interrupt flag, because a program that could not be interrupted could not be pre-empted and would own the machine. Bit 1 is written because the architecture reserves it as one — though the processor forces it whether or not it is written, which was established by clearing it and observing that nothing changed. It is there for the reader |
| `CS` | The user code selector, **RPL 3** | As `SS` |
| `RIP` | The image's entry point | |

**Every register is cleared first.** What is left in a register at that moment is
a kernel address as often as not, and handing one to a program that then prints
it is a disclosure no fault would report.

### 10.1 How the kernel gets back

Three ways, and this sub-task implements two of them.

A program that **faults** is ended. The dispositions of sub-task 6.4 already
classified a fault at privilege level 3 as belonging to the program rather than
to the machine, and `ExceptionTerminateProgram` has said so since — and then
panicked, because there was nothing to terminate and nowhere to return to. There
is now: the program's thread is abandoned and whoever started it resumes.

A program that makes a **system call** is returned to it by `SYSRET`, which is
sub-task 6.7's path and needed nothing new here.

A program that is **pre-empted** is not yet, there being no scheduler. That is
sub-task 6.15.

### 10.2 What running one proves

The self-test composes a program of twenty-nine bytes, wraps it in an ELF image,
loads it with the loader of sub-task 6.8 into an address space made by 6.9, and
enters it. The program writes a string through a system call and then executes an
undefined instruction, which is a fault that belongs to it.

Both halves are needed. A program that only wrote might have been simulated by
the kernel; one that only faulted might never have executed an instruction of its
own. Together they say it ran, called, was returned to, and ran again.

The trace the fault produces is the evidence, and every field of it is checked by
eye at least once:

```
  vector 6: #UD Invalid Opcode
  RIP 0x40101B  CS 0x2B  RFLAGS 0x202
  RSP 0x700000000000  SS 0x23
  RAX 0x48  RCX 0x40101B  RDX 0x48  RSI 0x402000  RDI 0x1  R11 0x202
```

`CS` of `2B` is the user code selector with a requested privilege level of 3, and
`SS` of `23` the user data selector likewise: the program was at privilege level
3 and not merely at an address a program would use. `RIP` is the twenty-eighth
byte of the text, so every instruction before the fault was executed. `RSP` is
the top of the stack the process was given. `RAX` of `48` is seventy-two, the
length the write returned — **the system call's result, in the program's own
register**. And `RCX` holding the return address with `R11` holding the flags is
`SYSRET` having done exactly what sub-task 6.7 said it would.

### 10.3 Verification

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A second kernel thread runs, once, and the first resumes after the switch | A switch that restored the incoming thread and lost the outgoing one never reaches the line after itself |
| The thread that resumes is the one switched away from | — |
| A system call arrived while the program ran | Section 10.2: the program executed instructions of its own rather than merely having been loaded |
| The program ended, and its process is marked ended | The termination path of Section 10.1 |
| **The exit status is −6** | The vector the program faulted upon, negated. Six is the undefined instruction it executed on purpose, so this says the program reached its *last* instruction and not merely its first — and it is what caught the address space not being switched, which faulted at the entry with vector 14 instead |

### 10.4 The negative tests

| The damage | What the run reported |
| ---------- | --------------------- |
| The code selector pushed without its requested privilege level of 3. | `KERNEL PANIC: An unrecoverable processor exception was raised within the kernel.` The descent did not descend: the program ran at privilege level 0, where its undefined instruction is a fault belonging to the machine |
| The address space not switched before the thread is entered. | `A fault was raised outside the kernel, at privilege level 3, vector 0xE` and `User mode self-test FAILED` — a page fault at the entry point, the program's pages not being mapped in the space it was entered in. Caught by the exit status being −14 rather than −6 |
| The termination not performed upon a fault outside the kernel. | `KERNEL PANIC: A fault outside the kernel was raised by nothing this kernel started.` Which is what this path did before this sub-task, so the panic is the previous behaviour restored |
| The six preserved registers written onto the prepared frame. | **Found during development, not as a deliberate test.** A return to address zero; see Section 9.2 |
| `RFLAGS` pushed without bit 1. | Nothing. The processor forces the bit whether or not it is written, so the assertion this was meant to justify does not exist and the comment says so instead |

## 11. The four calls, of sub-task 6.11

Until this sub-task a program could be started and could stop. It could not make
another program, could not become another program, and could not say that it had
finished — the only ways out of privilege level 3 being a fault and a system call
that returned. `fork`, `execve`, `exit` and `wait` are what close that, and they
are numbered 3 to 6 after the three calls that already existed.

**They are numbered after and not among.** A number handed to a program is a
number that must not change: `write` is call zero and is assembled as such by
hand in the self-test of sub-task 6.10, so a renumbering to put the four in a
tidier order would silently make every program written before this sub-task call
something else.

The four are implemented in `process.c` and not beside the dispatch table,
because none of them is a system call in substance. Each is an operation upon the
process table and the thread table; `syscall.c` copies a string, validates an
address, and names one of them.

## 12. The state a transition owes

Two things describe the machine rather than the thread, and both had to be made
explicit before a program could start another one. Neither was wrong before, and
both were wrong the moment two programs existed at once — which is the shape of
this whole sub-task's difficulty.

### 12.1 The segment bases, written rather than exchanged

`GS.base` holds the per-processor block inside the kernel and the program's own
value outside it, and `SWAPGS` moves between the two by **exchanging** them.
Which of the two `GS.base` holds therefore depends upon *how the kernel was
entered*:

| Entered by | `GS.base` within the kernel |
| ---------- | --------------------------- |
| `SYSCALL` | the per-processor block; the entry path exchanged |
| an interrupt or an exception | the program's own value; the stub exchanged nothing |

**The second row was corrected at sub-task 6.13.** The stub now exchanges too,
conditionally, upon the privilege level the saved `CS` names — because from that
sub-task the kernel itself reads `GS` on every lock, and a handler entered from
privilege level 3 would otherwise reach for the area through a base of zero. The
argument below is unaffected: a switch still cannot tell how the kernel was
entered, and still must not have to. See
[`CONCURRENCY.md`](CONCURRENCY.md), Section 3.3, and
[`INTERRUPTS.md`](INTERRUPTS.md), Section 3.3.

A context switch cannot tell those apart and must not have to. The thread it
resumes may be one suspended inside a system call, and the closing `SWAPGS` of
that path assumes the block is in `GS.base`. Resume such a thread after a child
that ended by *faulting*, and the exchange runs the wrong way: the block is
handed to privilege level 3, and the program's own value — zero — is left in
`IA32_KERNEL_GS_BASE`, where the **next** `SYSCALL` exchanges it back in and the
entry path looks for its kernel stack at address 8.

That is the failure exactly, and it was produced deliberately: the negative test
of Section 16.1 reports a supervisor read of linear address `0x8`, three
instructions after the parent was resumed.

The registers are therefore **written and not exchanged**, at both boundaries:
`SyscallEstablishKernelGsBase` where the kernel resumes a thread, and
`SyscallEstablishUserGsBase` where it departs for privilege level 3. Two writes
to model-specific registers per switch and per descent is the price, and what is
bought is that the state follows from the transition being made rather than from
the history of the thread making it.

### 12.2 The kernel stack, recorded twice

`SYSCALL` performs no stack switch. The entry path therefore cannot read `rsp0`
— it has no stack from which to reach the task state segment — and reads a field
of the block `GS` names instead. From sub-task 6.13 that block is the
per-processor area of [`CONCURRENCY.md`](CONCURRENCY.md), Section 3; the field is
the same field at the same offset, and nothing below changes. **Two variables
describe one stack**, and until this sub-task only one of them followed the
current thread.

`SyscallInitialise` wrote the block's copy once, at boot, with the stack the task
state segment was initialised with. `ThreadSetCurrent` has updated `rsp0` since
sub-task 6.9 and did not update the other. With one program running at a time
that was invisible: the stale stack belonged to nobody and served.

It stops being invisible the moment a program's child makes a system call while
the parent is suspended inside one, which is precisely what `wait` arranges. Both
entries build their frames at the same addresses; the child's work overwrites the
registers the parent's entry saved; and the parent returns by `SYSRET` through
whatever the child left. The negative test reports a jump to `0x652000` in
supervisor mode, with `RCX` holding `0xC0000102` — the number of the
model-specific register the child's kernel work had most recently named.

`ThreadSetCurrent` now writes both. **This is a defect of sub-task 6.7's work
found by sub-task 6.11**, and it is recorded here rather than quietly corrected,
because a fault that only appears when two programs exist is exactly the kind
this project's method exists to name.

## 13. `fork`

A child is a second process holding the same memory by the copy-on-write
discipline of sub-task 2.8, and one thread prepared to resume where its parent
will. `AddressSpaceClone` is the whole of the memory half: the pages are shared,
the writable ones protected in both hierarchies, and a reference recorded for the
new holder, so a fork costs the paging structures and nothing else until one of
the two writes.

### 13.1 What the child inherits, and why it is not cleared

The frame the entry path saved for the parent is copied into the child's thread
with `RAX` set to zero. Three things come from it and from nowhere else: the
address the parent will return to, which is where the child begins; the parent's
stack pointer; and the parent's registers.

The descent of Section 10 clears every register instead, and that would be wrong
here in a way nothing would report. The System V convention entitles the code
after a call to find `RBX`, `RBP` and `R12` to `R15` as it left them, so a child
whose preserved registers had been zeroed would return from `fork` into a frame
pointer of nothing and carry on. The reasoning that justifies clearing — that
whatever stands in a register at that moment is a kernel address as often as not
— does not reach a forked child: every value it inherits is one its parent
already held at privilege level 3.

A child therefore leaves the kernel by `ThreadResumeUser`, which restores the
whole frame and returns by `IRETQ`. `SYSRET` cannot be used: it takes the address
to return to from `RCX` and the flags from `R11`, which is where `SYSCALL` put
them — and a thread reaching this routine was placed there by a context switch
and not by a `SYSCALL`, so nothing has loaded either. `IRETQ` takes both from the
stack, so every register of the frame is restored in the same way as every other.

### 13.2 When a child runs

**A child runs when its parent waits for it**, and this is the sub-task's one
substantial departure from the call it is named after.

The bootstrap processor has one thread of control, so a forked
child is created `READY` and left standing; `wait` starts it upon the parent's
own thread of control and returns when it ends. Everything a program can observe
of the *ordering* is preserved — a child runs after the fork that made it and
before the wait that collects it — and concurrency is not.

A parent that never waits is therefore a child that never runs, which is the one
observable difference and is recorded as limitation 9.

## 14. `execve`

The new program is built entire before the old one is touched: a fresh address
space, the image loaded into it from a path through the filesystem of Phase 5,
and only then the exchange. A loader that filled the process's own address space
would have nothing to go back to when an image turned out to be malformed half
way through — and an `execve` that fails must leave its caller running, a program
told that its file does not exist being a program that carries on and says so.
The cost is that both address spaces exist at once for as long as the load takes.

**The point of no return has an order, and it is not free.** The new space is
made active *before* the old one is released, because `AddressSpaceDestroy`
refuses to release the space the processor is translating through — and rightly:
releasing the frames beneath a running program's own mappings is a fault that
arrives at some unrelated later instruction. The kernel continues to execute
across the change because its higher half is mapped identically in both.

Past that point a failure is fatal to the process rather than to the call. A user
stack that could not be given means a process whose program is gone and whose
caller no longer exists, so it is ended with a status saying which failure it was
and its parent collects that as it would collect any other ending.

**The refusals before that point are distinguished and not collapsed.** A path
that leads nowhere and an image this loader will not load are both
`SYSCALL_ENOENT`, which is a judgement — the caller can do nothing different
about either — but a frame that could not be had is `SYSCALL_ENOMEM`. A program
told that its file does not exist when the machine had in fact run out of memory
would look for the fault in the one place it is not, and would be told the same
thing however many times it looked.

Upon success the call does not return. The kernel stack it arrived upon is
abandoned where it stands, which costs nothing: the next entry from privilege
level 3 begins at the top of that stack again.

**Arguments and environment are refused, not ignored.** There is no C library and
no convention yet fixed for where a program finds them upon its stack, so
accepting them would mean discarding them silently — and a program that passed
arguments and found none would have no way to tell that the kernel had thrown
them away. A non-null vector is `SYSCALL_EINVAL`. See limitation 10.

## 15. `exit` and `wait`

`exit` is `ThreadTerminateCurrent` with a status the program chose, and it is a
**fourth** way back from privilege level 3 — not one of the three Section 10.1
named, which are the fault, the system-call return and the pre-emption that does
not yet exist. A program could already fault its way out and be returned to by
`SYSRET`; what it could not do was say it had finished, and the difference
between those is a status somebody chose and a vector the processor raised.

`wait` prefers a child that has already ended to one that has not. Both are
children and either may be collected, but collecting one that has ended costs
nothing where collecting one that has not means running it first — so taking the
finished one first is what makes a parent with several children collect them as
they finish rather than in the order the table happens to hold them.

**The caller's buffer is validated before the child is run**, not afterwards.
Running the child is what produces the status, and a status produced and then
found to have nowhere to go would be a child collected and its outcome discarded,
which is the one loss in this call that nothing could recover from.

The identifier returned names nobody by the time the caller sees it: the slot,
the threads and the address space are released before `wait` returns. It is
returned rather than left to be looked up for the reason Section 6 gives.

### 15.1 The copy-on-write page the kernel must write

`wait` writes its status into a page that the fork has just made read-only in
**both** hierarchies. Three things could happen there and only one of them is
correct.

The kernel could write through it. `CR0.WP` has been set since sub-task 3.4, so
that write raises a page fault at privilege level 0, and a fault at privilege
level 0 is a panic.

The validation could refuse the address. That is what it did before this
sub-task, and it is worse than the panic in the way that matters: the call fails
for a reason the caller cannot see and cannot correct — correcting it would mean
touching the page itself, which is the very thing it asked the kernel to do.

The validation therefore **resolves** the fault instead, calling
`PagingResolveCopyOnWriteFault` for a page that is marked and not writable before
concluding that a caller may not write to it. The negative test of Section 16.1
shows what its absence costs: the parent's `wait` returns `SYSCALL_EFAULT`
without starting the child at all, and two children are left in the table for
ever.

### 15.2 Nesting, and the thread to return to

`ThreadStart` records the thread to return to in a single variable, and until
this sub-task cleared it upon return. A program may now start another — a parent
that calls `wait` starts its child from within its own system call — so the
variable is **saved and put back** rather than cleared. The chain of them lives
upon the kernel stacks of the calls that made it, one to a stack, which is the
shape a stack of callers takes when there is one thread of control and no
scheduler to hold a queue.

Clearing it was correct while nothing nested. The negative test shows what it
became: the parent ends with nobody recorded to return to, `ThreadTerminateCurrent`
refuses, and the exception path panics about a program the kernel had itself
started.

## 16. Verification

Two tests, and the division is the one sub-task 6.10 used. `KernelVerifyFork`
forks a process and examines what was made without running anything, so a failure
there is a failure of the fork. `KernelVerifyLifecycle` runs a program that calls
all four, where a failure could be a failure of the fork, the loader, the
filesystem, the system-call path, the switch or the descent — every one of which
it puts together at once.

The program is twenty-nine bytes' worth of ancestor grown to about a hundred, and
it does this:

```
    write(1, greeting, length)
    if (fork() == 0) { execve("/prog", 0, 0); exit(99); }
    wait(&status)
    if (fork() == 0) { ud2 }
    wait(&second)
    exit(status)
```

`/prog` is a second program, composed here and written to a volume of memory
through the filesystem before the first is run — because `execve` loads from a
path and a path must lead to something. It writes a line of its own and exits
with 7.

**The number 7 is the whole assertion.** It is decided by a program read from a
file, carried out of that program by `exit`, into its parent by `wait`, and out of
its parent by `exit` — so a parent observed to end with 7 has exercised all four
calls, and no one of them could have produced it alone. A parent ending with 99
is an `execve` that was refused; ending with 0 is a `wait` that collected nothing.

**The second child exists for one reason.** The two ways out of privilege level 3
leave the kernel holding different segment bases, and a program whose children all
ended by asking would exercise only one of them. This one ends by faulting, and
the parent's next `SYSCALL` — its own `exit`, three instructions later — is what
a mishandled `GS.base` would fail at.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A fork produces a process with a **distinct paging hierarchy** | Two programs writing over one another with every appearance of isolation |
| The child records its parent, and is left runnable | A child nothing can find and nothing will run |
| The child's stack stands where its parent's does | Section 13.1: a child resuming upon an address its own space does not map |
| **The clone count rose by one** | A fork that built an empty address space and called it a copy |
| The child's thread would **resume** rather than begin | Section 13.1: every register cleared, and a frame pointer of nothing |
| The child would see **zero** returned from `fork` | The one value by which a child tells itself apart from its parent |
| The child inherited its parent's `RBX` and `R12` | Section 13.1, the other half: a child that sees zero and nothing else |
| A process with no children is told so | A `wait` that returned somebody else's child |
| A child that cannot be started is **ended and collected** | A parent told it has no children while one stands in the table for ever, told the same thing again at every attempt |
| A collected child no longer occupies the table, and cannot be collected twice | A slot released twice, which is an address space destroyed twice |
| **Three programs ended, not one** | A `wait` that collected without starting: the children never ran |
| Two forks and one execution were recorded | A child that resumed its parent's program rather than replacing it |
| **The parent's exit status is 7** | The paragraph above. The one number that requires all four calls to be right |
| A copy-on-write fault was resolved | Section 15.1: the pages were never shared, or the kernel refused to write to one it had itself protected |
| Both tables return to what they held | A leak of a process, a thread or an address space per program run |

### 16.1 The negative tests

Each was applied, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| `SyscallSetKernelStack` not called when a thread becomes current (Section 12.2). | A page fault in **supervisor mode** at `0x652000`, the parent having returned by `SYSRET` through registers its child's system call had overwritten. `RCX` held `0xC0000102`, which is the number of `IA32_KERNEL_GS_BASE` and not an address of anything |
| `SyscallEstablishKernelGsBase` not called upon a switch (Section 12.1). | `page not present, read, supervisor mode, faulting linear address 0x8` — the entry path reading its kernel stack out of the block `GS` no longer names, at the parent's first system call after its faulting child was collected |
| The thread to return to cleared rather than restored (Section 15.2). | `KERNEL PANIC: A program ended that nothing this kernel started had begun.` |
| The copy-on-write page not resolved for the kernel's write (Section 15.1). | `three programs did not end`, `the child did not replace itself with the program upon the volume`, `no copy-on-write fault was resolved` — and the report showing **two processes still occupying the table**, both `ready`, both never run |
| The child's `RAX` left as its parent's. | `the child would not see zero returned from fork`, and then every assertion of the second test, the child having taken the parent's branch and waited for a child of its own that does not exist |

### 16.2 Observed state

Upon QEMU with the `q35` machine and 512 MiB:

```
Processes: 0 of 64, threads 0 of 128; created 9 and 11 since the start.
Processes: 3 fork(s), 1 execution(s), 3 child(ren) collected.
Processes: no thread is current; 4 program(s) have run and ended.
Copy-on-write: faults resolved 6, frames duplicated 4, resolved without duplication 2.
Address spaces: clones 4, pages shared 56, of which protected 36.
```

The four clones are one apiece from the address-space self-test of sub-task 2.8
and the fork self-test above, and two from the program's two forks. Three forks
are counted against them because the address-space test clones a hierarchy
without making a process of it, which is the distinction between the two numbers.

**The forks and the collections are printed together because they must balance.**
A process forked and never collected is a slot that stays occupied, so the
difference between those two numbers is the number of children nobody has waited
for — which is the leak this sub-task can produce and the tables above cannot
show by themselves.

The trace the second child's fault produces is worth reading beside Section 13.1:

```
  vector 6: #UD Invalid Opcode
  RIP 0x401074  CS 0x2B  RFLAGS 0x246
  RSP 0x700000000000  SS 0x23
  RAX 0x0  RCX 0x401035  RDX 0x4A  RSI 0x402000  RDI 0x402200
```

`RAX` of zero is the child seeing what a child sees. `RCX` of `0x401035` is the
address after its parent's `SYSCALL`, which is where it resumed. And `RDX`,
`RSI` and `RDI` hold the length, the string and the status address its parent had
left in them before forking — the inheritance of Section 13.1, in registers no
part of the kernel wrote.

## 17. Present limitations

1. ~~**Nothing has run.**~~ A program has: see Section 10.2. What has not
   happened is pre-emption — nothing takes a processor away from a thread that
   has not given it up — until sub-task 6.15, whose local timer does exactly that
   for any thread the scheduler has placed upon a run queue. A program still
   runs to completion, no program having yet been admitted to one.
2. **The tables are fixed and are searched linearly.** Sixty-four processes and
   a hundred and twenty-eight threads, found by walking. Nothing here is on a
   path that runs often, and a hash of identifiers is worth writing when
   something is.
3. **One thread of control.** `ThreadStart` records the thread to return to in a
   single variable, so one program runs at a time and it runs to its end. A
   scheduler is what makes that a queue. Sub-task 6.11 made the variable
   *nestable* — it is saved and put back, so a program may start another — which
   is a stack of callers and still not a queue; see Section 15.2.
4. **A process's state is not derived from its threads'.** A process with one
   blocked thread and one running thread is running, and deciding that is the
   scheduler's business at sub-task 6.15.
5. **No priority, no scheduling class, no accounting of time.** All of them
   belong to the scheduler and none of them can be tested before there is one.
6. **No file descriptors.** A process has no open files. The virtual filesystem
   of Phase 5 has an open file table of its own and joining the two is the work
   of Phase 7. `fork` now exists and must therefore decide what a child inherits
   the moment there is anything to inherit; presently there is nothing, and the
   whole of what a child gets is its parent's memory and its parent's registers.
7. **Neither table is guarded.** Both, and the current thread, become the
   business of the lock sub-task 6.13 built. That lock exists and has not been
   applied here: nothing runs but the boot sequence until the scheduler of
   sub-task 6.15, which is when a table entry may be claimed upon one processor
   while it is being read upon another.
8. **The user stack does not grow.** Sixteen pages, mapped at creation. Growing
   one on demand means faulting below it and deciding whether the fault is a
   stack that wants to grow or a program that has gone wrong, which needs the
   extent record this sub-task introduces and a policy it does not have.
9. **A child runs only when its parent waits for it.** Section 13.2. A parent
   that forks and never waits is a child that never runs, and a parent that ends
   before waiting leaves its child in the table with a parent identifier naming
   nobody. There is no `init` to reparent an orphan to and no scheduler to run
   one; both arrive at sub-task 6.15 and Phase 9 respectively.
10. **`execve` takes no arguments and no environment.** Both vectors are refused
    rather than ignored, for the reason Section 14 gives. Passing them needs a
    convention for where a program finds them upon its stack, which is Phase 7's
    to fix and the C library's to read.
11. **A status is a quadword and nothing more.** `wait` reports what the program
    passed to `exit`, or the negated vector where a fault ended it, and there is
    no encoding distinguishing the two beyond the sign. A program cannot ask
    *how* its child died. The distinction costs nothing to record and is
    deliberately not invented here: the encoding belongs with the C library that
    will have to agree with it.
12. **`Thread` embeds a `SyscallFrame`, so `process.h` now includes `syscall.h`.**
    That is the honest dependency — a forked child resumes a system call — and it
    enlarges a division already owed. `LICENSING.md` records that `syscall.h`
    mixes the user-visible interface with the kernel's implementation and must be
    divided before sub-task 7.2, so that an `MIT` C library may include the
    former without the latter; `Thread` will then depend upon whichever half
    `SyscallFrame` lands in, and it is the kernel's half.
13. **`wait` cannot name which child to wait for, and cannot decline to block.**
    It collects whichever child is ready and otherwise runs one. Both refinements
    require a caller that could do something else meanwhile, which is the
    scheduler's.
