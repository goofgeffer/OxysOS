# The Process

**Phase**: 6, sub-tasks 6.9 and 6.10, of
[`../project/PLAN.md`](../project/PLAN.md). Sections 1 to 8 are 6.9, which
defines the structures; Sections 9 and 10 are 6.10, which switches to them and
descends to privilege level 3.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../kernel/proc/process.c`](../../kernel/proc/process.c),
[`../../kernel/proc/switch.asm`](../../kernel/proc/switch.asm),
[`../../kernel/include/oxys/process.h`](../../kernel/include/oxys/process.h).

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

### 7.1 The negative tests

Each was applied to `kernel/proc/process.c`, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| The guard page left writable. | `the page beneath a thread's stack is writable, so it is not a guard` |
| A process destroyed without destroying its threads. | `a thread outlived the process that owned it` **and** `the arena did not return to what it held before` — the second being the one that would have caught it even had the first been thought unnecessary |
| `TssSetKernelStack` not called when a thread becomes current. | `making a thread current did not point rsp0 at its stack` and `rsp0 did not follow the thread that became current` |


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
## 11. Limitations

1. ~~**Nothing has run.**~~ A program has: see Section 10.2. What has not
   happened is pre-emption — nothing takes a processor away from a thread that
   has not given it up, which is the scheduler of sub-task 6.15.
2. **The tables are fixed and are searched linearly.** Sixty-four processes and
   a hundred and twenty-eight threads, found by walking. Nothing here is on a
   path that runs often, and a hash of identifiers is worth writing when
   something is.
3. **One thread of control.** `ThreadStart` records the thread to return to in a
   single variable, so one program runs at a time and it runs to its end. A
   scheduler is what makes that a queue.
4. **A process's state is not derived from its threads'.** A process with one
   blocked thread and one running thread is running, and deciding that is the
   scheduler's business at sub-task 6.15.
5. **No priority, no scheduling class, no accounting of time.** All of them
   belong to the scheduler and none of them can be tested before there is one.
6. **No file descriptors.** A process has no open files. The virtual filesystem
   of Phase 5 has an open file table of its own and joining the two is the work
   of Phase 7, where `fork` must decide what a child inherits.
7. **Neither table is guarded.** From sub-task 6.13 both, and the current
   thread, become the business of that sub-task's lock.
8. **The user stack does not grow.** Sixteen pages, mapped at creation. Growing
   one on demand means faulting below it and deciding whether the fault is a
   stack that wants to grow or a program that has gone wrong, which needs the
   extent record this sub-task introduces and a policy it does not have.
