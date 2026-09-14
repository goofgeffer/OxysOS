<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Concurrency

**Phase**: 6, sub-task 6.13, of [`../project/PLAN.md`](../project/PLAN.md).
Sections 2 to 4 are the lock and the per-processor area beneath it; Section 5 is
the inter-processor interrupt; Section 6 is the translation-lookaside-buffer
shootdown built upon that.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**:
[`../../kernel/arch/x86_64/cpu/spinlock.c`](../../kernel/arch/x86_64/cpu/spinlock.c),
[`../../kernel/arch/x86_64/cpu/percpu.c`](../../kernel/arch/x86_64/cpu/percpu.c),
[`../../kernel/arch/x86_64/smp/ipi.c`](../../kernel/arch/x86_64/smp/ipi.c),
[`../../kernel/arch/x86_64/mm/shootdown.c`](../../kernel/arch/x86_64/mm/shootdown.c), with their headers
in [`../../kernel/include/oxys/`](../../kernel/include/oxys/). The command
register the interrupt is sent through belongs to the local controller and is in
[`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c), whose design is
[`../devices/APIC.md`](../devices/APIC.md).

**Specifications**: Intel SDM, Volume 3A, Sections 3.4.4 (the FS and GS bases in
64-bit mode), 4.10.4 and 4.10.5 (invalidation and its propagation between
processors), 8.1.2.2 (bus locking) and 8.2.2 (the memory-ordering model), 10.6
and 10.6.1 (issuing interprocessor interrupts and the command register); Intel
SDM, Volume 2A and 2B, `XADD`, `CMPXCHG`, `PAUSE`, `SWAPGS`, `INVLPG`.

## 1. What this sub-task is, and what it is not

It builds the four mechanisms a second processor cannot safely exist without: a
lock, a place for a processor to keep what is its own, a way for one processor to
interrupt another, and the one use of that which the memory manager already owes.

**It starts no processor.** There was one thread of control after this sub-task
exactly as there was before it, and every mechanism here was exercised upon that
one. [`ARCHITECTURE.md`](ARCHITECTURE.md), Section 4.1, records why the ordering
is this way round: sub-task 6.14 starts the processors, and starting them against
a kernel whose every shared structure is unsynchronised produces a milestone that
the testing mandate requires to be demonstrable and that cannot be demonstrated.
**6.14 has since arrived** — [`SMP.md`](SMP.md) is its design — so the mechanisms
below now have real targets, and where a section here says "there is one
processor" it is describing what 6.13 could rely upon and is marked as such.

**It does not put a lock around every shared structure.** The structures that
need one say so in their own file headers and in their own documents, and they
acquire theirs as the sub-task that makes them contended reaches them.
Section 10, limitation 1, lists what is outstanding. What is locked here is what
this sub-task itself creates.

## 2. The lock

A **ticket spinlock**: two sixteen-bit counters, of which one is the number the
next arrival takes and the other the number presently admitted. A lock is free
when they are equal.

```
    acquire:  mask interrupts
              ticket := LOCK XADD(lock.ticket, 1)
              while lock.serving /= ticket: PAUSE
    release:  lock.serving := lock.serving + 1
              unmask interrupts, if this was the outermost section
```

### 2.1 Why a ticket and not a test-and-set

A test-and-set lock hands the lock to whichever waiter's access happens to win,
which upon a machine with an unfair interconnect can be the same waiter
repeatedly while another waits without bound. That is a defect that does not
appear at all until there are several processors and a contended structure, and
then appears as a machine that is inexplicably slow rather than as a machine that
is wrong. A ticket lock admits its waiters in arrival order by construction.

The second reason is diagnostic, and it matters more at this stage than the
first. The difference between the two counters is the number of processors
waiting — a quantity a report can print and a self-test can assert upon. A
test-and-set lock holds one bit and can say nothing about itself, and a lock that
can say nothing about itself is a lock whose correctness cannot be established
upon a machine with one processor.

### 2.2 Only one operation is atomic

`LOCK XADD` upon the ticket counter, and nothing else. Intel SDM, Volume 3A,
Section 8.1.2.2, gives that prefix the guarantee that the read-modify-write is
indivisible with respect to every other processor. Two processors arriving
together must receive different tickets; an ordinary increment would give them
the same one, after which both would be served and the structure the lock governs
would be modified by two processors each believing itself alone.

The release is an ordinary store. Intel SDM, Volume 3A, Section 8.2.2, provides
that stores are not reordered with other stores, so the clearing of the owner
field is visible everywhere before the serving number advances, and a processor
admitted by the new serving number never sees the lock still owned. No fence
instruction is needed anywhere in the lock; what is needed is that the compiler
does not move memory accesses across it, which is what the memory clobber upon
the inline assembly expresses.

`PAUSE` in the wait loop. Intel SDM, Volume 2B, gives it as the instruction that
tells the processor a loop is a spin-wait: it de-pipelines the loop, so the reads
speculated ahead of the one that finally succeeds do not cost a memory-order
violation when the loop is left, and it lowers the power the wait draws. Upon a
processor that does not implement it the instruction is a no-operation, so no
test guards it.

### 2.3 The lock masks interrupts, always

Every acquire clears the interrupt flag and every release restores it, whether or
not any handler touches the structure the lock governs.

The alternative — masking only where a handler is known to be a party to the lock
— requires every caller to know what every handler touches, and it is wrong the
first time somebody is mistaken: a handler that takes a lock the interrupted code
already holds spins for it upon the one processor able to release it, and the
machine stops with no record of why. The cost of masking always is interrupt
latency for the duration of a critical section, and a critical section in this
kernel is a few dozen instructions.

The mask is applied **before the ticket is issued**, not after. Between issuing a
ticket and being served, a processor holds a claim upon the lock that only it can
relinquish; an interrupt delivered in that window has the same consequence as one
delivered inside the section. Masking first closes the window rather than
narrowing it.

### 2.4 The two misuses that are otherwise silent

| Misuse | What it does without the check | What the check does |
| ------ | ------------------------------ | ------------------- |
| Acquiring a lock this processor already holds | Issues a second ticket and waits for a serving number only this processor could advance. The machine stops inside a lock whose name nothing records. | Panics at the moment it is committed, naming the lock, its counters and its owner. The check is exact rather than heuristic: the owner field is written under the lock and cleared under it. |
| Releasing a lock this processor does not hold | Admits a second processor into a section the first is still inside. Nothing observes it; the structure the lock governs has no way to know how many processors are modifying it. | Panics, naming the lock and its recorded owner. |

Both were exercised deliberately upon 2026-09-09 by temporarily inserting the
misuse into the self-test, and both produced the report and the panic intended.
The procedure and what each run reported are in
[`../project/TESTING-SYSTEM.md`](../project/TESTING-SYSTEM.md), Section 9.3.

### 2.5 The bound upon the wait

A spinlock that waits without bound turns every deadlock into a machine that
stops with a blank screen. The bound is some hundreds of millions of iterations
of a two-instruction loop — enormous, because it must not fire for a lock that is
merely contended, however heavily. What it fires for is a lock whose holder is
never going to release it, and when it fires the machine panics and names the
lock, the holder, the waiter and the queue length.

It is a diagnostic and not a recovery. There is no correct way to proceed past a
lock that will not be released; what is recovered is the explanation.

## 3. The per-processor area

One structure per processor, holding what that processor owns outright: its
kernel stack, its index, the identifier its local controller answers to, the
nesting depth of its interrupt-disable, the number of locks it holds, and its
counters.

Nothing in it is guarded and nothing in it needs to be, because every field is
written by the processor that owns the area and by no other. That is the whole
reason it exists.

### 3.1 It is reached through `GS`

```c
static inline PerCpu *PerCpuCurrent(void)
{
    PerCpu *area;
    __asm__ __volatile__("movq %%gs:16, %0" : "=r"(area));
    return area;
}
```

One load, of the third quadword of the area, through a segment base that only
privilege level 0 can have written.

Every alternative requires the processor to establish *which* processor it is
before it can reach its own state, and the cheapest honest answer to that
question — a read of the local controller's identifier register — is an uncached
load from a memory-mapped device. A spinlock acquire performs this lookup on
every acquire and every release. It must not cost a device access.

The area holds a pointer to itself at offset 16 because a `GS`-relative access
can read a field of the area but cannot produce its address: there is no
instruction that loads a segment base. So the base is stored inside the area and
read like any other field.

### 3.2 The invariant, and the four places that maintain it

> **While kernel code executes, `GS.base` holds the executing processor's area
> and `IA32_KERNEL_GS_BASE` holds zero. While a user program executes, the two
> are exchanged.**

| Where | What it does |
| ----- | ------------ |
| `PerCpuInitialise` | Establishes the area and writes `GS.base` directly. Runs before anything that could take a lock. |
| `GdtInitialise` | **Repairs** `GS.base`, which the segment reload it performs destroys. Section 3.4. |
| `kernel/arch/x86_64/syscall/syscall_entry.asm` | `SWAPGS` on entry and again on the return. `SYSCALL` is only ever executed at privilege level 3, so the exchange is unconditional. |
| `kernel/arch/x86_64/interrupt/interrupt_stubs.asm` | `SWAPGS` on entry and on the return, **conditionally** — only where the saved `CS` says the interrupt came from privilege level 3. Section 3.3. |

`ThreadSwitchTo` and `ThreadTrampolineEntry` in
[`../../kernel/proc/process.c`](../../kernel/proc/process.c) *write* the two
registers rather than exchanging them, for the reason [`PROCESS.md`](PROCESS.md),
Section 12, gives: which of the two holds the area depends upon how the kernel
was entered and not upon which thread is running, and a switch cannot tell those
apart.

### 3.3 The conditional exchange in the interrupt path

Added at this sub-task, and it closes a hole that had been harmless only because
nothing in the kernel read `GS`.

An interrupt that arrives while a user program is running enters kernel code with
the program's segment base in the register, the interrupt path performing no
exchange of its own. Every spinlock the handler takes reaches for the area
through that base — so a page fault taken at privilege level 3, or a keyboard
request delivered to a running program, would run kernel code with `GS.base` at
zero and the first per-processor access would read address sixteen.

The stub therefore tests the saved `CS` in the frame and exchanges where its low
two bits are not zero. The test is made against the frame rather than against a
register because at that moment no register can be trusted: the interrupted code
owned all of them. The frame is read again on the way out rather than a decision
being remembered, because a handler is permitted to alter it
([`INTERRUPTS.md`](INTERRUPTS.md), Section 7.2) and the exchange must match the
privilege level being *returned to*.

### 3.4 The defect that a segment reload is

`GdtInitialise` reloads `DS`, `ES`, `FS`, `GS` and `SS` so that no selector
naming the retired table remains in a register. Intel SDM, Volume 3A, Section
3.4.4, provides that in 64-bit mode the `FS` and `GS` bases are **not** ignored
as the other segment bases are, and that loading the register from a descriptor
replaces the hidden base with the descriptor's — which for a flat data descriptor
is zero.

The area is established before the frame allocator and `GdtInitialise` runs some
hundreds of lines later, so the first per-processor access after it faulted upon
address `0x10`. The repair is made inside `GdtInitialise` itself, and not by its
caller: a caller that forgot would produce a fault far away with nothing
connecting the two, which is exactly how this was found.

Repairing it requires finding one's own area *without* the register being
repaired, so `PerCpuEstablishSegmentBase` searches the areas for the one whose
`apic_identifier` matches what `CPUID` leaf 1 reports. That identifier is the
right key because it is assigned by the hardware at reset and is unique among the
processors of a machine; the dense index is this kernel's own numbering and could
not be recovered from anything a processor knows about itself.

### 3.5 The area subsumes `SyscallProcessorBlock`

Sub-task 6.7 gave the system-call entry path a two-field structure — a kernel
stack and a scratch for the caller's stack pointer — reached through `GS` for
precisely the reason above, and recorded that sub-task 6.13 would replace it with
the area proper. It has. The two fields are still the first two, at the same two
offsets, and `kernel/arch/x86_64/syscall/syscall_entry.asm` is unchanged apart from its comment.
The offsets are asserted by `_Static_assert` in `kernel/arch/x86_64/cpu/percpu.c`, beside the
structure they belong to.

One thing did change. `SyscallInitialise` used to write the area into
`IA32_KERNEL_GS_BASE` and zero into `GS.base` — correct for a kernel in which
nothing read `GS`, and wrong the moment something does, since the kernel is
executing at that point. It now settles the registers in the direction the
invariant requires.

## 4. The interrupt-disable is counted, not saved

`PerCpuPushInterruptState` clears the flag and records the state it had **if the
depth was zero**; `PerCpuPopInterruptState` restores it **when the depth returns
to zero**.

The obvious alternative is for each section to save the flags and hand them back
to the caller to restore. It is wrong in a way that takes months to find. Two
nested locks released in the wrong order restore the flags in the wrong order and
re-enable interrupts inside a section that had disabled them; the fault shows
itself as a deadlock in an interrupt handler minutes later and nowhere near the
code that caused it. Counting the depth in the area makes the pairing structural:
the flag is restored when the last section is left, whichever of them that turns
out to be.

Two conditions panic rather than proceed:

- A pop with no matching push, which is a release without an acquire.
- A pop performed with interrupts already enabled, which means something inside
  the section executed `STI` and the section was therefore not critical at all.

The flag is read **before** `CLI` and the area reached **after** it. Reading first
is the point — what is being recorded is the state the caller was in. Reaching
the area afterwards costs nothing now and will matter when there is a scheduler
that could move the flow of control between the load and its use.

## 5. The inter-processor interrupt

One processor interrupts another by writing the interrupt command register of its
own local controller. The target receives the vector exactly as it would receive
a device's request: the same gate, the same dispatcher, the same
end-of-interrupt.

### 5.1 The command register

Intel SDM, Volume 3A, Section 10.6.1 and Figure 10-12. Two halves: the low half
at offset `0x300` holds the vector, the delivery mode, the destination mode, the
level, the trigger mode and the destination shorthand; the high half at `0x310`
holds the destination in its bits 31:24 in xAPIC mode (Section 10.6.2.1).

**The high half is written first.** The manual states that "the act of writing to
the low doubleword of the ICR causes the IPI to be sent", so a destination
written afterwards is the destination of the *next* interrupt and not of this
one.

The register is waited for on both sides of the write. Before, because it may
still be carrying the previous interrupt — bit 12 is the delivery status,
read-only, set from the write until the controller has accepted the message — and
a write during that interval discards a message this kernel believes it sent.
After, because a caller that goes on to wait for an acknowledgement must first
know the message left; a send that was never accepted and a target that never
answered are indistinguishable from the waiting end.

Both waits are bounded, and a send abandoned for want of an idle register is
counted and reported. An unbounded wait here would be a machine stopping inside
the routine that sends interrupts, with interrupts masked.

### 5.2 The two vectors

| Vector | Purpose |
| ------ | ------- |
| `0xFD` | Translation-lookaside-buffer shootdown. Section 6. |
| `0xFC` | Halt. Sent by `KernelPanic` before it prints a word. |

They sit immediately below the local controller's own two — `0xFF` spurious and
`0xFE` error, chosen there for the reason [`../devices/APIC.md`](../devices/APIC.md),
Section 3.5, gives — and far above the vectors a device can be routed to, which
top out at 47. Intel SDM, Volume 3A, Section 10.8.3, makes a vector's number its
priority, so an interrupt one processor sends to another is served ahead of any
device request the target is also holding. That is what a shootdown requires: the
sender is stopped until the target answers.

The halt is not built ahead of a use. `KernelPanic` calls it, and the call site is
real whether or not there is presently anybody to send it to. A machine that has
failed keeps running upon its other processors, and each of them goes on
modifying the structures the report is about — so a report written while they run
describes a machine that no longer exists by the time it is read. It is not
waited for, because a panic that waited could be stopped by the very processors it
is trying to stop.

### 5.3 The three audiences

`IpiSendToOthers` uses the "all excluding self" shorthand, `IpiSendToSelf` the
"self" shorthand, and `IpiSendToProcessor` no shorthand and a destination.

A shorthand is used in preference to a list of identifiers because the set is
exactly what the shorthand names, and because a list would have to be walked with
the command register held between entries — during which another processor's send
would find it busy.

`IpiSendToSelf` exists because this kernel has started one processor and must
nevertheless demonstrate that the mechanism works. Section 10.6.1 gives the self
shorthand as a delivery like any other, which is what makes the demonstration a
demonstration and not an approximation.

## 6. The translation-lookaside-buffer shootdown

Intel SDM, Volume 3A, Section 4.10.4.1, requires software to invalidate a
translation whenever it changes a paging-structure entry the processor may have
cached, and `INVLPG` does that for one linear address. Section 4.10.5 records
that the instruction reaches **the executing processor alone**: where several
processors may have cached a translation, each must be made to invalidate it for
itself, and the manual names the procedure "TLB shootdown".

### 6.1 The protocol

```
    sender:   invalidate here
              if fewer than two processors are online: done
              take the shootdown lock
              publish the address; pending := online - 1
              send vector 0xFD to all processors but this one
              wait until pending = 0
              release the lock

    receiver: read the published address
              invalidate it
              pending := pending - 1        (LOCK SUB)
              signal the end of the interrupt
```

The receiver's three actions are in that order and the order is the whole of the
protocol. The address is read **first**, because the acknowledgement is what
permits the sender to publish another and a read afterwards could read that one.
The acknowledgement is made **last**, because it is the sender's evidence that the
translation is gone; made before the invalidation it would be a promise rather
than a report, and the sender would proceed to reuse a frame this processor could
still reach.

### 6.2 The wait is what makes it correct

Section 4.10.4.4 permits an invalidation to be deferred only while no processor
can use the stale translation. A sender that returned before its targets had
invalidated would leave exactly that window, and would go on to give the frame
away — after which another processor writes through a translation to a page that
now belongs to somebody else.

A shootdown that is not acknowledged is therefore **fatal**. `PagingInvalidate`
panics upon it. There is no report that could be made later about the corruption
it would otherwise cause, because the state that would explain it is what gets
overwritten.

### 6.3 What the ordinary path costs

`ShootdownBroadcast` returns before it takes the lock when fewer than two
processors are online. That was every machine this kernel had run upon until
sub-task 6.14, and the early return is now the exception rather than the rule:
upon a machine with processors started, an unmap costs an interrupt to each of
them and a wait for every acknowledgement.

The test is nevertheless still made before the lock, because `PagingInvalidate`
is on the path of every map, unmap and copy-on-write fault, and a lock taken
there for nobody's benefit would be paid for by all of them upon the machines
that still have nobody — one declaring a single processor, or one whose bring-up
was declined for a reason [`SMP.md`](SMP.md), Section 7.1, enumerates.

### 6.4 One request at a time

The published address is a single global and the lock is what makes it one. The
alternative — a request block per sending processor, which the handler would scan
— removes the serialisation and is what this becomes if shootdowns are ever
measured to be the thing a workload waits for. It is not built now, for the
reason [`../devices/APIC.md`](../devices/APIC.md), Section 7, gives for not having
built any of this earlier: a mechanism with nothing to use it is a mechanism
nothing has ever shown to be right. Since sub-task 6.14 there are processors, but
only one of them ever sends — a parked processor announces no paging change — so
the simple form remains the one whose correctness can be argued in a paragraph,
and 6.15 is the sub-task that could make the measurement worth taking.

### 6.5 `ShootdownToSelf`, and the condition that makes it safe

The self-directed form cannot hold the shootdown lock while it waits: the
acknowledgement is made by a handler upon the processor that is waiting for it,
and the lock masks that processor's interrupts. So the published address is
unprotected for the duration of the wait.

That is safe upon a machine with one processor online and upon no other, and the
function therefore **refuses** where more than one is, and refuses again where
the interrupt flag is clear.

**Since sub-task 6.14 that refusal is reachable, and the ordering is what keeps
it from firing.** `KernelVerifyShootdown`, which is the only caller, runs before
`SmpInitialise` — so one processor is online when it runs, and the condition
holds. Moving the self-test after the bring-up would turn a passing assertion
into a refusal upon every multiprocessor machine, which is why
[`../../kernel/test/arch/smp.c`](../../kernel/test/arch/smp.c) states the
dependency rather than leaving it to be rediscovered.

## 7. `PagingInvalidateLocalPage`

The instruction alone, announcing nothing. It exists for the shootdown handler,
which is what the other processors run when they are told; an announcement made
from within it would be an announcement of an announcement, and the processors
would tell each other about the same address without end.

No other caller has any business with it. A mapping changed without the
announcement leaves every other processor holding a translation of a page this
one believes it has taken away.

## 8. Verification

`KernelVerifyPerCpu`, `KernelVerifySpinlock`, `KernelVerifyIpi` and
`KernelVerifyShootdown`, in
[`../../kernel/test/arch/smp.c`](../../kernel/test/arch/smp.c).

**A lock that does not lock behaves exactly like a lock that does, upon a machine
with one processor.** Nothing observable distinguishes them until a second
processor exists, and by then the failure presents as corruption somewhere else
entirely. So the first two assertions are not "the machine still works": they are
that each mechanism's internal state moves as it must, because that state is the
only thing a single processor can be made to show.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The area's self pointer names the area | An area whose pointer named another would give every processor another's state, and every other assertion here would still pass. |
| `GS.base` holds the area, read back from the model-specific register | The invariant of Section 3.2 broken. The register is the only place the value lives. |
| `IA32_KERNEL_GS_BASE` holds zero | The area in both registers: works until the first `SWAPGS`, after which the kernel has a user's value and the user has a pointer to the kernel's stack. |
| The area's APIC identifier is the one the controller reports | An interrupt addressed to a processor by the number its area carries delivered to a different one. |
| The area names the kernel stack the task state segment names | The entry path loads `RSP` from that field without checking. A zero is a kernel executing upon address zero the moment a program makes a call. |
| The depth and the lock count are zero on entry | A section entered and never left, masking every interrupt since, with nothing recording an owner. |
| An acquire masks interrupts | The half of an acquire that is easy to omit and impossible to notice. |
| A held lock stands exactly one ticket ahead of its serving number | The ordering guarantee. A test-and-set lock could not make this assertion of itself. |
| A nested section deepens and restores the count, and the flag stays clear | **The design in Section 4 being wrong.** Under the save-and-restore alternative the inner pop restores "masked" and the code happens to work; with the pushes in the other order it re-enables interrupts inside a section that had disabled them, and nothing says so. |
| A release restores the flag to what the acquire *found*, not to a fixed value | A release that always set the flag works at every call site reached with interrupts on and enables interrupts mid-boot at every other one. |
| A conditional acquire of a held lock fails rather than waits | The path a report takes when it declines to wait for a structure another processor is changing. Upon one processor this is the only way that path can be reached. |
| An uncontended acquire is not counted as a contention | A counter that said every lock was contended would be useless for deciding which structure needs a finer lock. |
| An interrupt sent with the flag clear is not delivered until it is set | The condition every shootdown sent to a processor inside a critical section will be delivered under. |
| An interrupt this processor sends to itself is delivered | The send path end to end: command register, gate, dispatcher, handler. |
| **A second interrupt of the same vector is delivered** | The end-of-interrupt. It cannot be read back without disturbing the in-service register, so what is asserted is the consequence: one left in service would block every vector of its priority class and above, which at `0xFD` is everything but the controller's own two. |
| The controller reported no error and abandoned no send | An illegal vector or a register that never went idle, which nothing else reports. |
| **After a shootdown, a deliberately staled window reads through the new mapping** | The whole mechanism. See below. |
| The handler ran exactly once, and the address it was given is the address requested | A handler that acknowledged without invalidating, or invalidated the wrong page. |

### 8.1 How the shootdown is shown end to end

The self-test maps an ordinary page of the kernel arena, writes a pattern through
it and reads it back, which is what puts the translation in the buffer. It then
**rewrites the page-table entry by hand** to name a second frame holding a
different pattern, and invalidates nothing — which is precisely the state a
mapping changed upon another processor leaves this one in. It then asks for a
shootdown addressed to itself, and requires the window to read the second
pattern.

The final read is sound whether or not the processor had in fact cached anything:
a shootdown that works produces the new value, and there is no arrangement of a
broken one that also does. Whether staleness was *observable* beforehand is
reported and not asserted, the architecture nowhere requiring a processor to cache
a translation it has used. Under QEMU it is observable, and the run says so.

This is the one place in the kernel outside `kernel/mm/` that walks the paging
hierarchy, and it is a self-test doing deliberately what a defect would do
accidentally.

## 9. Observed state

Under QEMU with `-machine q35 -cpu qemu64 -smp cores=2`, on 2026-09-10, read at
the point in the boot where the reports are emitted — which since sub-task 6.14
is **after** `SmpInitialise` and before
`KernelVerifyApplicationProcessors`:

| Quantity | Value |
| -------- | ----- |
| Processors online | 2 — index 0 (APIC 0, bootstrap), index 1 (APIC 1, application) |
| Shootdown vector | 253; halt vector 252 |
| Interrupts sent through the command register | 4, none refused, none abandoned |
| Interrupts received | 4 |
| Shootdown requests | 2, serviced 4, abandoned 0; last address `0x8000` |
| Serviced by processor 1, from its own handler | 1 |
| Stale translation before the shootdown | **Observable** |
| Locks acquired, processor 0 / processor 1 | 1127 / 1, none contended |
| Local controller errors | 0 |

**The shootdowns serviced exceed the requests** because `KernelVerifyIpi` sends
the shootdown vector twice on its own account, to establish the delivery and the
end-of-interrupt, without publishing a request. Of the two requests, one is
`KernelVerifyShootdown`'s self-directed test and one is the removal of the
trampoline's identity mapping — which is why the last address is `0x8000` and
why processor 1 has serviced exactly one.

**Processor 1 has acquired exactly one lock**, and that figure is the design of
[`SMP.md`](SMP.md), Section 6, visible in the accounting: the one acquisition is
its arrival announcement, which is the only thing a parked processor does.

The figures recorded here on 2026-09-09, when sub-task 6.13 closed, were one
processor online, three interrupts sent and received, one shootdown request
serviced three times, and two locks acquired. They are superseded rather than
kept, this section describing what the kernel does now.

## 10. Limitations

1. **Three locks are applied; the rest are not.** Sub-task 6.14 put the diagnostic
   channel under a lock — `KernelWriteString`, in
   [`../../kernel/kernel.c`](../../kernel/kernel.c) — because that is the whole
   of what a started processor touches, and one function reaches all four of the
   structures beneath it: the text-mode display's cursor
   (`drivers/vga/vga.c`), the console (`graphics/console.c`), the serial
   adapter's transmit buffer (`drivers/serial/serial.c`), and the compositor's
   back buffer and damage rectangle (`graphics/compositor.c`). Those four are
   struck from the list below, and each of their headers now names
   `KernelWriteString` as where its lock is taken. See
   [`SMP.md`](SMP.md), Section 6.

   **Sub-task 6.15 added two more.** Each run queue carries a lock of its own —
   one per queue and not one for the scheduler, so that two processors
   rescheduling at once do not wait for each other — and `ProcessTableLock`, in
   `kernel/proc/process.c`, makes the search for a free slot in the process and
   thread tables atomic with the claim of it. That became necessary the moment
   each application processor began adopting an idle thread as it came online:
   `SmpInitialise` waits only for a processor's *area* before starting the next,
   so two can be inside `ThreadAdoptCurrent` at once, and two that found the same
   free slot would produce two threads sharing one identifier, one context and
   one kernel stack pointer — with no count wrong and nothing faulting until they
   switched. The process and thread tables are struck from the list below. See
   [`SCHEDULER.md`](SCHEDULER.md), Sections 2.1 and 6.

   **Everything else is still unsynchronised**, and each says so in the header of
   the file that owns it: the frame allocator's bitmap and search hint
   (`kernel/mm/pmm.c`), the kernel arena (`kernel/mm/vmm.c`), the heap
   (`kernel/mm/heap.c`), the block layer's device table
   (`kernel/block/block.c`), the buffer cache (`kernel/block/buffer.c`), the
   mount, node and open file tables (`kernel/fs/vfs/vfs.c`, which holds all
   four), the interrupt dispatch table (`kernel/arch/x86_64/interrupt/interrupts.c`), the request
   layer's mask state (`kernel/arch/x86_64/interrupt/irq.c`), the 8259A's mask registers
   (`drivers/pic/pic.c`), the I/O APIC's select-then-window sequence
   (`drivers/apic/ioapic.c`), the 8042's configuration byte
   (`drivers/ps2/ps2.c`), the drawing surfaces (`graphics/draw.c`), the fault
   screen (`graphics/faultscreen.c`), the keyboard and mouse buffers
   (`drivers/keyboard/keyboard.c`, `drivers/mouse/mouse.c`), and the ramdisk's
   extent and registration (`drivers/ramdisk/ramdisk.c`).

   The last of those is the mildest entry in the list and is named anyway. A
   transfer to or from a ramdisk is a copy between disjoint ranges, so two
   processors performing one simultaneously would each be correct; what is
   unsynchronised is the registration, and the block layer's accounting and the
   buffer cache above it — both of which are already in this list. A driver that
   is safe by accident and not by construction still belongs here, because the
   accident is a property of what it does today.

   **Sub-task 7.6 added one more to that list**: each process's descriptor table,
   a field of the process control block in `kernel/proc/process.c`, guarded by
   nothing as the break of sub-task 7.3 is. Two threads of one process opening at
   once would each find the same free slot and each write its own file into it,
   and the loser's open file would be an entry of the filesystem layer's that
   nothing holds a number for and nothing will ever close. The case cannot arise
   — there are no userland threads, and a user thread's affinity names the
   bootstrap processor alone — and it is counted here with the rest rather than
   left to be discovered. It needs the same lock the rest of the process control
   block will take. [`LIBC.md`](LIBC.md), Section 12.7, limitation 10.

   **None of that is unsafe today, and the reason has changed twice.** It used to
   be that there was one flow of control. Then sub-task 6.14 started the other
   processors, and the reason became that a started processor had nothing to run.
   Sub-task 6.15 gave them something to run, and the reason is now narrower and
   more precise: **what runs upon an application processor is a kernel thread,
   and no user thread may be placed upon one.** Every file above is reached
   through a system call or through a driver a user program drives, and a user
   thread's affinity mask names the bootstrap processor alone.

   That is deliberately a value in a field rather than a rule written nowhere.
   [`SCHEDULER.md`](SCHEDULER.md), Section 4, records it, and the mask widens in
   one place when these locks land — at which point the list above must shrink in
   the same change, or the widening is the defect.

   **The fault screen is in that list and will never leave it.** It takes no lock
   by decision rather than by omission: a fault handler that waited upon a lock
   held by the processor that faulted would replace a reported fault with a
   stopped machine. Two interleaved screens is the lesser failure, and it is
   accepted. It is named here so that the omission is visible as a choice. The
   same reasoning is why `KernelPanic` resets the diagnostic lock rather than
   waiting for it.

   The list is written out file by file rather than by subsystem so that it can
   be checked mechanically: every file named holds a `Concurrency.` paragraph in
   its header, and a file that acquires its lock is struck from this list in the
   same change that makes it do so. A structure brought under a lock while this
   list still named it would be the one failure this limitation cannot otherwise
   report.
2. **A shootdown carries one address.** A change affecting a range is announced
   one page at a time, at one interrupt and one round trip each. A range form is
   worth its complexity when there are processors to send it to.
3. **An address space is never shot down as a whole.** A process that has ended
   does not announce its address space, because the paging structures it used are
   not freed until nothing refers to them. Sub-task 6.15 must revisit this when a
   process may be running upon one processor and reaped upon another.
4. **There is no reader-writer lock and no lock that may be slept upon.** Every
   critical section in this kernel is short enough that spinning is cheaper than
   the alternative, and there is no scheduler to sleep against until sub-task
   6.15. The buffer cache is the first structure that will genuinely want one.
5. **`SWAPGS` has a window against the non-maskable interrupt.** Between the
   `SWAPGS` and the `SYSRET` of the system-call return path, and between the
   conditional `SWAPGS` and the `IRETQ` of the interrupt path, the code segment
   is the kernel's and the segment base is the user's. A non-maskable interrupt
   delivered in that window — this kernel programmes `LINT1` for it — would run a
   handler with the wrong base. Closing it requires an entry path that reads
   `IA32_GS_BASE` with `RDMSR` and decides from its value rather than from `CS`,
   upon a stack from the interrupt stack table. It is recorded rather than closed
   because the handler in question presently does nothing that reaches the area.
6. **The counters are not atomic.** The per-processor counters are each written by
   one processor and need nothing; the two global counters written by handlers
   upon arbitrary processors use the `LOCK` prefix; but a lock's `contentions`
   field is written by a processor that does not hold it yet, and a torn
   increment there costs a diagnostic. That is a cost paid knowingly.
7. **`PER_CPU_MAXIMUM` is 64**, matching `ACPI_PROCESSOR_MAXIMUM`. A machine
   declaring more is refused at `PerCpuInitialise` with a panic that says so,
   rather than silently running upon the first sixty-four.
