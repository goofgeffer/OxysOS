<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Scheduler

**Phase**: 6, sub-task 6.15, of [`../project/PLAN.md`](../project/PLAN.md).
Section 2 is the run queue; Section 3 is the timer that measures a quantum;
Section 4 is affinity, which is a safety boundary before it is a policy;
Section 5 is placement; Section 6 is the critical section a switch is made
inside, which is where this sub-task's one real defect lived. Section 9 is
sub-task 8.6's: the wait channel, the first thing a thread could sleep upon and
be woken from, and what the first sleeping thread required of the rest.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../kernel/proc/sched.c`](../../kernel/proc/sched.c),
with [`../../kernel/include/oxys/proc/sched.h`](../../kernel/include/oxys/proc/sched.h).
The threads it rotates between are
[`../../kernel/proc/process.c`](../../kernel/proc/process.c), whose design is
[`PROCESS.md`](PROCESS.md); the timer is in
[`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c), whose design is
[`../devices/APIC.md`](../devices/APIC.md); the processors it schedules upon were
started by [`SMP.md`](SMP.md).

**Specifications**: Intel SDM, Volume 3A, Sections 10.5.4 and Figure 10-10 (the
local timer, its three modes, and the divide configuration register), 10.5.1
(the local vector table is per processor), 10.8.3.1 (the priority class of a
vector), 8.2.2 (the memory-ordering model); Intel SDM, Volume 2B, `STI` (the
one-instruction delay before the flag takes effect).

## 1. What this sub-task is, and what it is not

Sub-task 6.14 started every processor the firmware declared and gave none of them
anything to run. This gives them something.

**What it adds**: a run queue per processor, each with a lock of its own; a
round-robin rotation between the threads upon a queue; an affinity mask that
decides which queues a thread may join; a placement that chooses the shortest
eligible queue; and a local timer per processor that takes a thread away when its
quantum expires. An application processor no longer parks — it adopts an idle
thread and runs whatever its queue holds.

**What it does not add**: safety for a user program upon an application
processor. The allocators, the filesystem layer, and most of the structures
[`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, enumerates are
still unsynchronised, and a user thread reaches them through its system calls.
Section 4 explains how the affinity mask expresses that constraint rather than
leaving it as a rule somebody has to remember.

**What it applies**: one lock beyond its own — `ProcessTableLock`, in
`process.c`, which guards the claim of a slot in the process and thread tables.
That is now genuinely contended: each application processor claims one as it
comes online, and `SmpInitialise` waits only for a processor's *area* before
starting the next, so two can be inside `ThreadAdoptCurrent` at once.

## 2. The run queue

A singly linked list per processor, with a tail pointer, and a lock of its own:

```
    enqueue:  take queue.lock; link at tail; count += 1; release
    dequeue:  take queue.lock; unlink at head; count -= 1; release
```

Singly linked, because the two operations are "take from the front" and "put at
the back" and nothing else. Round-robin needs no removal from the middle, and a
second pointer per thread would be a second thing to keep consistent for a case
that does not arise.

The link is **intrusive** — a `queue_next` field in `Thread` — and the reason is
not economy. An enqueue happens with a lock held and interrupts masked, which is
the one place in this kernel an allocator must not be called from; a queue node
allocated per enqueue would call the arena there, and the arena is still
unsynchronised.

### 2.1 Why a lock each, and not one lock

A single scheduler lock would be correct and would throw away the entire reason
for per-processor queues: two processors rescheduling at the same moment would
wait for one another, on the path taken most often in the machine.

The cost of the choice is paid in `SchedulerChooseProcessor`, which reads several
queue lengths **without holding any of their locks**. That answer is stale before
it is used, and it is treated as what it is — a placement hint. The enqueue that
follows takes the chosen queue's lock and is correct whatever the length turned
out to be. A balance that was momentarily wrong costs one thread waiting behind
one other; a balance computed under a lock spanning every queue would cost every
admission on the machine.

### 2.2 The idle thread is never queued

Each processor has one, and it is the fallback rather than a competitor. A queued
idle thread would take a full quantum in the rotation, so a processor with one
runnable thread would spend half of its time halting.

An idle thread is **not created** — it is the execution already in progress,
described. The bootstrap processor's is whatever thread is current when
`SchedulerStartOnThisProcessor` runs; an application processor's is adopted in
`SchedulerEnterIdle`, from the stack the bootstrap processor prepared for it
before the startup interrupt was sent. That is what keeps the rule
[`SMP.md`](SMP.md), Section 4, is built around intact: a started processor takes
nothing from the arena, and `ThreadAdoptCurrent` takes a table slot and nothing
else.

## 3. The quantum, and the timer that measures it

**Ten milliseconds**, from the local APIC timer, periodic, on vector `0xFB`.

The vector stands beneath the two of sub-task 6.13 — `0xFD` for the shootdown and
`0xFC` for the halt — and beneath the controller's own `0xFF` and `0xFE`. The
priority class of a vector is its upper four bits (Section 10.8.3.1), so this
ordering is what stops a pre-emption outranking a shootdown: a processor that
deferred an invalidation in order to switch threads would run the incoming thread
against a translation whose sender has already been told it was discarded.

### 3.1 The rate is measured, not assumed

Intel SDM, Section 10.5.4, gives the timer's rate as the bus clock or core
crystal divided by the divide configuration register, and states no figure for
it: it is a property of the machine. `LocalApicCalibrateTimer` therefore measures
it, against the interval timer of [`../devices/TIME.md`](../devices/TIME.md):

```
    mask the entry; divide by 16
    initial count := 0xFFFFFFFF, one-shot
    PitBusyWaitMicroseconds(50 ms)
    elapsed := 0xFFFFFFFF - current count
    counts per millisecond := elapsed / 50
```

One-shot from the largest count there is, so the counter is falling throughout
and cannot reload beneath the measurement — a periodic timer would wrap, and a
wrap that went unobserved would report a rate that was a fraction of the truth.
The entry stays masked, because what is being read is the count and not the
interrupt.

The measurement is made **once, upon the bootstrap processor**, and used to
programme every processor's timer. The clock is the machine's, not the
processor's; measuring it upon each would be measuring one clock several times,
and the several answers would then have to be reconciled by something with no way
of knowing which was right.

Both refusals are real. A counter that reached zero, or a rate too low to divide
into a millisecond, is declined rather than divided by — and the kernel then runs
unpre-empted and says so, rather than upon a quantum computed from a number that
was merely the largest one available.

### 3.2 The divide register is not a divisor

Bit 2 is reserved; the divisor lives in bits 3, 1 and 0. Divide-by-one is `1011B`
and **`0000B` is divide-by-two**, so a register written with the value that
"looks like one" halves every interval the kernel believes it programmed. The
constants in [`../../kernel/include/oxys/dev/lapic.h`](../../kernel/include/oxys/dev/lapic.h)
are named rather than computed for that reason.

Sixteen is what this kernel uses: far enough from one that a calibration counting
down from `0xFFFFFFFF` spans a useful interval, and far enough from 128 that a
millisecond is still thousands of counts rather than tens.

### 3.3 Each processor starts its own

The local vector table is per processor (Section 10.5.1). A timer programmed by
the bootstrap processor upon its own controller says nothing whatever about any
other, so `SchedulerStartOnThisProcessor` is called by each — and the divide
register is written again there, because it is per processor too. A processor
given only the count would count at whatever divisor a reset left, which is
divide-by-two: its quantum would be an eighth of every other processor's, and
nothing would say so.

### 3.4 The end-of-interrupt comes before the switch

`SchedulerHandleTick` signals end-of-interrupt and *then* reschedules, and the
order is the whole of what makes pre-emption work. `ThreadSwitchTo` does not
return until this thread runs again, so an end-of-interrupt written after it
would be written when this thread is next scheduled — leaving the tick in service
upon the local controller in the meantime, which blocks every vector of its
priority class and above. **The processor would take one timer interrupt and then
no more**, and present as a machine on which pre-emption worked exactly once.

A tick that arrives while the processor holds a spinlock is counted and otherwise
ignored. Switching there would carry the lock to a thread that did not take it
and leave the taker unable to release it — and the taker may be inside the queue
lock of the scheduler itself.

## 4. Affinity is a safety boundary before it is a policy

A `uint64_t` mask over the **dense processor indices** of
[`../../kernel/include/oxys/arch/cpu/percpu.h`](../../kernel/include/oxys/arch/cpu/percpu.h), not
the identifiers the local controller answers to. The two differ on any machine
whose firmware numbers its processors sparsely, and a mask built from the wrong
one would name processors that do not exist while excluding ones that do.

| Thread | Default mask | Why |
| ------ | ------------ | --- |
| Kernel thread | Every processor | It runs kernel code the scheduler itself can account for. |
| **User thread** | **The bootstrap processor alone** | Its system calls reach the allocators, the process tables and the filesystem layer, and those are still unsynchronised. |

That second row is the important one. It is not a scheduling decision — it is
[`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, written as a value
in a field. When those locks land, the mask widens in one place and the
constraint is gone; until then the constraint is enforced by the same mechanism
that would enforce any other affinity, rather than living as a rule in somebody's
memory.

**A mask naming no online processor is refused**, both at `SchedulerSetAffinity`
and at `SchedulerAdmit`. Accepting one would produce a thread that is admitted,
counted, and permanently invisible — and every figure in the report would call
that a success.

## 5. Placement, and why a thread does not migrate

A thread is placed once, at admission, upon the shortest queue its affinity
permits. The bootstrap processor breaks a tie, because it is the one processor
certain to exist and a tie broken arbitrarily would scatter threads that could
have shared a cache.

**Afterwards it stays.** A pre-empted thread goes back onto the queue it came
from. There is no work stealing and no periodic rebalancing, and the reason is
the one [`../devices/APIC.md`](../devices/APIC.md), Section 7, gives for not
having built the inter-processor interrupt earlier: a mechanism with nothing to
use it is a mechanism nothing has ever shown to be right. Stealing needs a second
queue's lock taken while holding the first, which is a lock ordering this kernel
would then have to state and enforce everywhere; it is worth that when there is a
workload to measure it against.

## 6. The switch, and the critical section it happens inside

This is where this sub-task's one real defect was, and it is worth setting out
because nothing reported it.

The scheduler masks interrupts, chooses, and switches — so the switch happens
*inside* a critical section entered through `PerCpuPushInterruptState`. But
**the counted disable belongs to the processor, not to the thread.**

A thread that is *resumed* by the switch is fine: it continues inside its own
`PerCpuPushInterruptState` and executes the matching pop, so the count comes out
even. A thread that has **never run** has no such pop. It begins at a prepared
frame, with the depth and the interrupt flag of the thread that gave it the
processor — so it runs with interrupts masked for ever, takes no timer tick, and
is never pre-empted again.

The symptom is a machine that **hangs the first time the scheduler starts a
thread**, with no fault, no panic, and nothing in the log after the line before
it. It was met exactly that way.

`ThreadCreateScheduled` is the answer. It makes the same thread
`ThreadCreateKernel` does, except that the prepared frame enters
`ThreadScheduledEntry`, a trampoline whose first act is
`PerCpuResetInterruptState`: the depth goes to zero, the flag is restored, and
only then is the entry point called. `ThreadCreateKernel` itself is left alone,
because its other caller — the context-switch self-test of sub-task 6.10 —
switches by hand with interrupts enabled and has nothing to close.

The machine-wide count still comes out even: every push has a pop somewhere,
executed by the pushing thread when it is next resumed, against whatever depth
the resuming processor then holds.

### 6.1 A lock is never held across a switch

`ThreadSwitchTo` does not return until somebody switches back, so a lock held
across it would be released by a different thread, at a different time, upon a
processor that may not be the one that took it — which is precisely the misuse
sub-task 6.13's spinlock panics about. The outgoing thread is therefore enqueued
*before* the switch and not after: after does not exist.

## 7. Verification

`KernelVerifyScheduler`, in
[`../../kernel/test/proc/sched.c`](../../kernel/test/proc/sched.c).

**A count of admissions is not evidence that anything ran.** A scheduler that
enqueued four threads and never gave any of them a processor produces the same
admissions, the same queue lengths and the same report. What it cannot produce is
a counter that moves while nothing in the test writes it — so the fixture is four
kernel threads that do work and record it, and the assertion is made against what
they recorded.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| A mask naming no processor, and one naming a processor that is not online, are both refused | A thread admitted, counted, and never runnable. Every figure in the report would call it a success. |
| An accepted mask is recorded | An affinity that is checked and then forgotten. |
| This processor's timer entry is unmasked, read back from the entry | A processor that is never pre-empted. Every other assertion here passes upon a machine whose threads happen to yield. |
| The measured rate is not zero | A scheduler reporting itself running upon a quantum of no counts. |
| An admitted thread records that it is queued, and which queue | A thread whose flag said otherwise would be admitted twice, and a queue holding one thread twice is a cycle. |
| **Every fixture thread completed all of its rounds** | The assertion this test exists for. Nothing else distinguishes a scheduler that runs threads from one that only counts them. |
| Every thread ran upon a processor that is online, recorded by that thread with `PerCpuIndex` | A thread queued to one processor and run upon another. It is recorded from inside the thread because that is the only authoritative place. |
| Every thread was given a processor at least once | A thread that completed without ever being scheduled, which would mean the counters were moved by something else. |
| **Total slices exceed the number of threads** | The rotation. There are more threads than processors, so some queue held two; both finished, so the processor passed between them. One slice each would mean every thread ran once to completion without ever giving the processor up. |
| The machine-wide switch count rose | The switch path end to end. |
| **A quantum expired**, where there is more than one processor | The timer. Every other switch here could have been voluntary, and a voluntary switch proves nothing about a timer. |

Two of those were got wrong first, and the corrections are the useful part.

**The rotation assertion was originally "slices ≥ rounds", and that is false.** A
thread that yields into an *empty* queue is not switched away — there is nobody
to switch to — so it carries straight on and completes many rounds upon a single
slice. The assertion now counts slices across the fixture instead.

**The fixture originally yielded after every round and did no work between
them.** It completed in microseconds, so no two threads were ever runnable at the
same moment: every thread ran to completion upon one slice, no queue ever held
two, and **no quantum ever expired**. The test passed, and had demonstrated
nothing it was written to demonstrate. The fixture now waits at a barrier until
every thread is admitted, and then works without yielding — which is what makes
pre-emption something observed rather than hoped for.

### 7.1 Why the fixture is kernel threads

A user thread reaches the allocators and the filesystem layer through its system
calls, and Section 4 is why it is pinned to the bootstrap processor until those
are locked. A test that ran a program upon an application processor would be
asserting something this kernel does not claim.

### 7.2 Why the threads block rather than yield for ever

A fixture thread that had finished its work but stayed runnable would keep taking
a share of its processor for the whole of the remaining boot, and **every
self-test after it would be running against a machine quietly switching threads
underneath it**. `SchedulerBlockCurrent` takes the thread off every queue for
good.

The self-test's own adopted thread is released for the same class of reason:
`ThreadStart` succeeds or fails according to whether a thread is current, and
[`../../kernel/test/proc/lifecycle.c`](../../kernel/test/proc/lifecycle.c)
asserts the failing branch later in the same boot. A thread left current here
would silently turn that assertion into a test of something else — which is how
the regression was found.

### 7.3 The assertion that was a race, recorded at 7.3 and closed at 7.7

Immediately after admitting each fixture thread, the self-test read two of its
fields and required both: that `queued` is set, and that `processor` names a
queue. The stated reason was sound — a thread whose flag said otherwise would be
admitted twice by the next caller, and a queue holding one thread twice is a
cycle.

**The reading was not sound.** The threads are admitted with an affinity that
names every processor, with the scheduler already running upon both, so the other
processor is entitled to dequeue a thread and run it the instant it appears upon a
queue. When it does, `queued` becomes false because the scheduler did exactly what
it exists for — and an assertion made at that moment reports a defect that did not
occur.

It did occur, and it had been seen before it was understood.
[`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md) holds the row: on
2026-09-11, during sub-task 7.3, it was observed once in twelve runs, diagnosed
correctly as racy by construction, and **left for the sub-task that next revisited
this file** — which is the right disposition for a defect that belongs to neither
the sub-task in hand nor the code under test.

Sub-task 7.7 is that sub-task, and it arrived having made the race far easier to
see: it changed nothing in the scheduler, shifted the boot's timing, and the
failure appeared upon about one boot in three, always followed by a report stating
that the fixture had run correctly upon both processors and completed every round.
A test that fails one run in three is a test nobody reads, and its next genuine
failure would have been dismissed as the flake.

**What is asserted now is decided by who can reach the thread.**

| Field | Asserted | Why it is safe to |
| ----- | -------- | ----------------- |
| `processor` | Always. | It is written once, at admission, and not cleared when the thread is dequeued. Whatever reads it reads the queue the thread was put upon. |
| `queued` | Only where `processor` is *this* processor's queue. | The admission and the read are made with this processor's interrupts masked, and no other processor may take a thread from this one's queue. Nothing can have changed it. |

A thread queued elsewhere is left to the assertions further down, which are about
a rotation rather than about a flag and are not races: that every thread completed
its rounds, upon a processor it names itself, and that the slices exceed the
number of threads.

The masking is the whole of the fix, and it is worth being exact about what it
does. It does not make the other processor wait. It makes *this* processor's
sequence — admit, then read — indivisible with respect to anything that could
run here, and the affinity comparison is what excludes everything that could run
elsewhere.

## 8. Observed state

Under QEMU with `-machine q35 -cpu qemu64 -smp cores=2`, on 2026-09-10:

| Quantity | Value |
| -------- | ----- |
| Local timer rate | 62,607 counts/ms at divide-by-16 — a 1 GHz bus clock, which is what QEMU presents |
| Quantum | 10 ms; tick vector `0xFB` |
| Threads admitted | 4 |
| Context switches | 34 |
| **Quanta expired** | **19** |
| Times a processor found its queue empty | 49 |
| Processors the fixture ran upon | 2 of 2 |
| Rounds completed / slices taken | 32 / 32 |

The slices very nearly equal the rounds, and that is the rotation visible in the
accounting: four threads sharing two queues, each losing the processor about once
per round of work.

## 9. Sub-task 8.6: the wait channel, and the first thread to sleep

**Implementation**: `SchedulerSleep`, `SchedulerWake`, `SchedulerCanSleep`,
`SchedulerExitCurrent` and `SchedulerWithdraw` in
[`../../kernel/proc/sched.c`](../../kernel/proc/sched.c); the callers are
`ProcessWait` and `ThreadTerminateCurrent` in
[`../../kernel/proc/process.c`](../../kernel/proc/process.c) and the pipe of
[`../../kernel/fs/vfs/pipe.c`](../../kernel/fs/vfs/pipe.c). Asserted by the
pipe's transfer in `file-check` and the shell's fifth session,
[`SHELL.md`](SHELL.md), Section 23.

Limitation 8 of Section 10 said that nothing blocked on anything, and sub-task 8.6 is
where something had to: a pipeline is two programs alive at once, one asleep
while the other has the processor, and `SchedulerBlockCurrent` — which takes a
thread out of the rotation for good — was a way to sleep with no way to wake.

### 9.1 A channel is an address

`SchedulerSleep(channel)` sets the thread's `wait_channel`, marks it blocked and
reschedules; `SchedulerWake(channel)` walks the thread table and admits every
blocked thread whose channel matches. The channel is the address of the thing
waited upon — a parent's own process for `wait`, the pipe for a reader or a
writer — and carries no meaning beyond identity. That is the sleep-and-wakeup
of the first Unix kernels, and it is chosen over a queue per condition because
the two things it costs are both cheap today: the walk is a hundred and
twenty-eight slots, and a wake is broadcast to every sleeper upon the channel,
each of which re-tests its condition and sleeps again if it was not the one
meant. A list of sleepers per channel is worth its two pointers when the walk
is measured to matter, and nothing has measured it.

**The discipline is the caller's**: test the condition and sleep within one
masked section. `SchedulerSleep` enters its own masked section as well, so the
caller's nests it, and a wake that arrives after the test finds the thread
blocked and enqueues it — there is no moment between the test and the sleep at
which a wake is lost. Every wake in this kernel today comes from another thread
upon the same processor, which cannot run until the sleeper has switched away,
so the discipline is kept for the wake that will one day come from an
interrupt handler rather than for one that can happen now.

### 9.2 What sleeps, what is pre-empted, and what neither

`wait` sleeps, upon the parent's process, and is woken by `ThreadTerminateCurrent`
when a child ends. A pipe's reader and writer sleep upon the pipe, and are woken
by every write, read and close upon it. The terminal's reader does neither: it
yields to whatever is queued and halts only when nothing is, because its bytes
arrive through an interrupt handler and a wake performed there would be the
first enqueue from one — a cost 8.7 pays, when a signal must interrupt a read,
and not before. [`SHELL.md`](SHELL.md), Section 22.3.

**A user thread is pre-empted at privilege level 3 and nowhere else.** The tick
handler now returns without rescheduling where the interrupted frame is the
kernel's and the current thread is a user thread or the adopted boot flow. The
kernel beneath a system call is not written to be pre-empted —
[`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, is the list of
what a second thread entering it would race for — and a user thread therefore
holds the processor inside the kernel until it gives it up at one of the sleeps
above, which is where it holds nothing. A kernel thread the scheduler made is
pre-empted wherever it stands, as the fixture threads of Section 7 require.

### 9.3 A child joins the rotation at the fork

`ProcessFork` admits the child's thread to the bootstrap processor's queue — its
kernel stack prepared for the trampoline first, which `ThreadStart` had always
done and which admission at the fork did not, the first defect this sub-task met:
a switch into a child whose stack pointer stood at the very top of its stack
returned into the unmapped page above it. The child runs when the parent sleeps
in `wait` or is pre-empted at privilege level 3, whichever is first. A thread
the scheduler runs has no thread to return to, so `ThreadTerminateCurrent` wakes
the parent and calls `SchedulerExitCurrent` rather than switching back; and the
thread to return to is a field of the started thread rather than one pointer
per processor, [`PROCESS.md`](PROCESS.md), Section 17.

A child collected before it ran — which the fork self-test arranges, from a
caller with no thread to sleep upon — is withdrawn from its queue by
`SchedulerWithdraw` before it is destroyed. That is the removal from the middle
limitation 3 of Section 10 said nothing needed: a walk of a singly linked list under its
lock, on no path that runs often.

### 9.4 The bootstrap processor's idle thread

Until 8.6 the bootstrap processor had an idle thread only inside the scheduler's
own self-test, which adopted one and released it, and did not need one
otherwise: no thread upon it ever gave up the processor with nothing else to
run. The shell now sleeps in `wait` while a child that sleeps upon a pipe waits
for its reader, and a processor with two sleepers and no idle thread has
nothing to switch to — a reschedule with no idle thread returns to the sleeper,
which then runs while asleep. `SchedulerStartOnThisProcessor` therefore makes
one by `ThreadCreateScheduled` where nothing is current to adopt, and
`SchedulerDetachThisProcessor` forgets only an idle thread that is the caller's
own execution, so the self-test's adoption no longer discards it.

### 9.5 The interrupt state travels with the thread

A thread that sleeps does so from inside its own masked section and is resumed
by whichever thread pushed next, and the idle thread's push records that
interrupts were enabled. Left with the processor, the sleeper's pop would
execute `STI` inside a system call. `ThreadSwitchTo` saves the depth and the
recorded flag into the outgoing thread and loads the incoming thread's;
[`CONCURRENCY.md`](CONCURRENCY.md), Section 4.1, has the whole of it.

### 9.6 Verification, and a negative test

There is no new self-test: what asserts this section is that programs which
sleep run to completion. `file-check` sends three buffers' worth through a pipe
from a child and reads them back in the parent, which is the writer and the
reader taking turns at least twice; the shell's fifth session carries the whole
of `/bin/sh` — some thirty-three kibibytes, eight buffers — through `cat | wc
-c`, and the count comes back equal to the file's size; and every session that
forks now runs its children beside the shell. A negative test made the pipe's
writer drop what did not fit rather than sleep, and the size came back short,
[`SHELL.md`](SHELL.md), Section 23.

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| A parent's `wait` returns the child's status after the child has run beside it. | A parent woken before its child ended, or never woken. |
| Twelve kibibytes cross a four-kibibyte pipe intact and in order. | A writer that did not sleep when the pipe was full, or a reader that saw the end before the writer finished. |
| The fork self-test collects a child that never ran, from a caller with no thread. | A queued thread destroyed and later dequeued — a switch onto a released stack. |
| Every other self-test still passes, and the scheduler's fixture threads are still pre-empted. | A tick filter that stopped pre-empting kernel threads, or the boot flow taken from a self-test. |

## 10. Limitations

1. **The bootstrap processor is not itself a scheduled thread.** It executes
   `KernelMain`, which is a flow of control and not a thread, and the scheduler
   does not adopt one for it — doing so would change whether `ThreadStart`
   succeeds, which a later self-test asserts upon. It joins the rotation only
   while something has given it a thread to join as, which is the scheduler's
   own self-test and, since 8.1, the adoption every self-test that starts a
   program makes. **Since sub-task 8.6 it has an idle thread of its own**,
   Section 9.4, made rather than adopted; the boot flow itself is still not a
   thread the scheduler rotates, and the tick leaves it alone by the rule of
   Section 9.2. Phase 9's `init` is where the boot path ends and a scheduled
   thread takes over.
2. **A thread never migrates.** Placement is at admission and final; there is no
   work stealing and no rebalancing. Section 5 says why, and what it would cost
   to add.
3. ~~**A thread cannot be removed from a queue it is already upon.**~~ **Closed
   at sub-task 8.6**: `SchedulerWithdraw` walks the list, Section 9.3. The list
   is still singly linked, the walk being on no path that runs often.
4. **There is no reaper.** A kernel thread that finishes cannot free its own
   stack — it is standing on it — and nothing else does. Its table slot and its
   four pages are held until the machine stops. The self-test's four fixture
   threads are exactly that cost, paid once per boot. A user thread's stack is
   released by the `wait` that collects its process, which is a reaper of a
   kind since 8.6: the ended thread has switched away by then and stands on
   nothing.
5. **There is one priority.** Round-robin between equals, with no notion of a
   thread that should run sooner. Nothing in this kernel yet has a reason to be
   preferred.
6. **The quantum is a guess.** Ten milliseconds is a trade between switch cost
   and responsiveness, and neither has been measured, there being no workload to
   measure them against.
7. **`PagingActiveTable` is a global written by every processor.** It records
   which paging hierarchy was last activated, and every processor writes it on
   every switch. It is a diagnostic, and it is presently harmless for a reason
   rather than by luck: every kernel thread runs upon the kernel root, and a
   user thread is pinned to the bootstrap processor by Section 4 — so the writers
   all write the same value. It becomes wrong the moment a user thread may run
   elsewhere, and must become per-processor in the same change.
8. ~~**Nothing blocks on anything.**~~ **Closed at sub-task 8.6**: the wait
   channel of Section 9. `SchedulerBlockCurrent` still takes a thread out of the
   rotation for good, and the fixture threads still use it. What remains open
   is a lock that may be slept upon, which
   [`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 4, records, and
   a wake from an interrupt handler, which nothing performs yet — the terminal's
   reader polls and yields rather than sleeping, Section 9.2.
9. **A wake walks the thread table.** A hundred and twenty-eight slots, under a
   masked section, at every write, read and close upon a pipe and every ending
   of a child. It is the cost of a channel that is an address and nothing more,
   and it is paid knowingly until something measures it; a list of sleepers per
   channel is the remedy, and it is two pointers and a discipline.
10. **A user thread is never pre-empted inside the kernel.** Section 9.2. A
   system call that ran for a long time — none does — would hold the bootstrap
   processor for its duration, and every other user thread with it. The
   remedy is the locks of `CONCURRENCY.md`, Section 10, limitation 1, and not
   a change to the scheduler.
