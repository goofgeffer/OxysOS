# The Process

**Phase**: 6, sub-task 6.9, of [`../project/PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../kernel/proc/process.c`](../../kernel/proc/process.c),
[`../../kernel/include/oxys/process.h`](../../kernel/include/oxys/process.h).

## 1. What this sub-task is, and what it is not

It defines the structures a running program is held in, the tables that hold
them, and the allocations each is given. It creates a process with an address
space of its own and a thread with a kernel stack of its own.

**Nothing here runs anything.** No context is ever restored, no address space is
made active by this file, and no thread has executed an instruction. Sub-task
6.10 is what transfers to one.

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

## 8. Limitations

1. **Nothing has run.** No context has a return address upon its stack, so no
   context could be switched to. Sub-task 6.10.
2. **The tables are fixed and are searched linearly.** Sixty-four processes and
   a hundred and twenty-eight threads, found by walking. Nothing here is on a
   path that runs often, and a hash of identifiers is worth writing when
   something is.
3. **A process's state is not derived from its threads'.** A process with one
   blocked thread and one running thread is running, and deciding that is the
   scheduler's business at sub-task 6.15.
4. **No priority, no scheduling class, no accounting of time.** All of them
   belong to the scheduler and none of them can be tested before there is one.
5. **No file descriptors.** A process has no open files. The virtual filesystem
   of Phase 5 has an open file table of its own and joining the two is the work
   of Phase 7, where `fork` must decide what a child inherits.
6. **Neither table is guarded.** From sub-task 6.13 both, and the current
   thread, become the business of that sub-task's lock.
7. **The user stack does not grow.** Sixteen pages, mapped at creation. Growing
   one on demand means faulting below it and deciding whether the fault is a
   stack that wants to grow or a program that has gone wrong, which needs the
   extent record this sub-task introduces and a policy it does not have.
