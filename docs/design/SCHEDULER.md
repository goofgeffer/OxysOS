<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Scheduler

**Phase**: sub-task 6.15 of [`../project/PLAN.md`](../project/PLAN.md); sleeping
and waking from 8.6 and 8.7; the poll channel from 9.6.
**Source**: [`../../kernel/proc/sched.c`](../../kernel/proc/sched.c),
[`../../kernel/include/oxys/proc/sched.h`](../../kernel/include/oxys/proc/sched.h);
the timer in [`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c).
**Specifications**: Intel SDM, Volume 3A, Sections 10.5.1 (the LVT is per
processor), 10.5.4 and Figure 10-10 (the timer and divide register), 10.8.3.1
(vector priority classes), 8.2.2 (memory ordering); Volume 2B, `STI`.

Per-processor run queues, a round-robin rotation on a 10 ms quantum from each
processor's Local APIC timer, affinity masks, and the wait channels threads sleep
on. The threads themselves are [`PROCESS.md`](PROCESS.md); the processors were
started by [`SMP.md`](SMP.md).

## 1. Run queues

One singly linked list per processor, with a tail pointer and its own lock:
enqueue at the tail, dequeue at the head. Round-robin needs nothing else.

- **The link is intrusive** (`queue_next` in `Thread`): an enqueue happens with a
  lock held and interrupts masked, where no allocator may be called.
- **A lock per queue, not one for all**: one lock would make every processor wait
  for every other on the machine's most frequent path.
  `SchedulerChooseProcessor` reads other queues' lengths without their locks; the
  answer is only a placement hint, and the enqueue that follows takes the chosen
  queue's lock.
- **The idle thread is never queued**: queued, it would take a full quantum in the
  rotation. An application processor's idle thread is its own execution, adopted
  in `SchedulerEnterIdle` from a stack the bootstrap processor prepared, so a
  starting processor allocates nothing ([`SMP.md`](SMP.md)). The bootstrap
  processor's is made by `ThreadCreateScheduled` in
  `SchedulerStartOnThisProcessor`.
- `SchedulerWithdraw` removes a thread from the middle of a queue (for a child
  collected before it ever ran), by a walk under the lock on a rare path.

## 2. The quantum

**Ten milliseconds**, the Local APIC timer in periodic mode on vector `0xFB`. That
vector is below the shootdown (`0xFD`) and halt (`0xFC`) vectors, and priority
class is a vector's upper four bits (SDM 10.8.3.1), so a pre-emption never outranks
a shootdown: a processor that deferred an invalidation to switch threads would run
on a translation its sender believes discarded.

**The rate is measured** (SDM 10.5.4 states none), once, on the bootstrap
processor, against the 8254 ([`../devices/TIME.md`](../devices/TIME.md)):

```
mask the entry; divide by 16
initial count := 0xFFFFFFFF, one-shot
PitBusyWaitMicroseconds(50 000)
counts per ms := (0xFFFFFFFF - current count) / 50
```

One-shot from the largest count, so the counter cannot reload mid-measurement. The
clock is the machine's, so one measurement serves every processor. A counter that
reached zero, or a rate below one count per millisecond, is refused, and the
kernel then runs unpre-empted and says so.

- **The divide register is not a divisor.** Its value lives in bits 3, 1 and 0
  (bit 2 reserved): divide-by-one is `1011B`, and `0000B` is divide-by-two. The
  constants in [`../../kernel/include/oxys/dev/lapic.h`](../../kernel/include/oxys/dev/lapic.h)
  are named, not computed. Sixteen gives thousands of counts per millisecond.
- **Each processor starts its own timer and writes its own divide register**, both
  being per processor; one left at the reset divisor would have a quantum an eighth
  of the others', silently.
- **End-of-interrupt before the switch.** `ThreadSwitchTo` returns only when this
  thread next runs; an end-of-interrupt after it would leave the tick in service,
  blocking its priority class: one pre-emption and then none.
- **A tick while a spinlock is held is counted and ignored**: switching would hand
  the lock to a thread that did not take it, possibly the scheduler's own.
- **User threads are pre-empted only at privilege level 3.** The kernel beneath a
  system call is not written to be pre-empted ([`CONCURRENCY.md`](CONCURRENCY.md));
  a user thread in the kernel holds the processor until it sleeps, where it holds
  nothing. Kernel threads the scheduler made are pre-empted anywhere.

## 3. Affinity

A `uint64_t` mask over the dense processor indices of
[`../../kernel/include/oxys/arch/cpu/percpu.h`](../../kernel/include/oxys/arch/cpu/percpu.h),
not APIC identifiers, which can be sparse.

| Thread | Default mask | Why |
| ------ | ------------ | --- |
| Kernel thread | Every processor | It runs code the scheduler can account for. |
| **User thread** | **The bootstrap processor** | Its system calls reach unsynchronised allocators, process tables and filesystem ([`CONCURRENCY.md`](CONCURRENCY.md)). |

The second row is that constraint written as a value: when those structures are
locked, the mask widens in one place. **A mask naming no online processor is
refused**, at `SchedulerSetAffinity` and at `SchedulerAdmit`, or a thread would be
admitted, counted, and never run.

**Placement** is at admission, on the shortest permitted queue, ties to the
bootstrap processor. **A thread never migrates**: no work stealing, no rebalancing.
Stealing needs one queue's lock taken while holding another's, a lock order worth
stating only when a workload can measure the benefit.

## 4. The switch

The scheduler masks interrupts, chooses, and switches, inside a critical section
entered by `PerCpuPushInterruptState`.

- **A new thread starts through `ThreadScheduledEntry`**, whose first act is
  `PerCpuResetInterruptState`. A resumed thread executes its own matching pop, but
  a thread that has never run has none, and would inherit the switching thread's
  masked state for ever: never ticked, never pre-empted, and the machine hangs at
  the first scheduled thread with nothing in the log. `ThreadCreateScheduled`
  makes such threads; `ThreadCreateKernel` is for the hand-switched context test.
- **The interrupt state travels with the thread.** `ThreadSwitchTo` saves the
  outgoing thread's depth and recorded flag and loads the incoming thread's;
  otherwise a sleeper resumed by the idle thread's pop would execute `STI` inside a
  system call ([`CONCURRENCY.md`](CONCURRENCY.md)).
- **No lock is held across a switch**; the outgoing thread is enqueued before it.
  A lock held across would be released by another thread, perhaps on another
  processor.

## 5. Sleeping and waking

**A channel is an address**: that of the thing waited on (a parent's process for
`wait`, a pipe for its reader and writer). `SchedulerSleep(channel)` records it,
marks the thread blocked, and reschedules; `SchedulerWake(channel)` walks the
thread table (128 slots) and admits every thread sleeping on it. Every sleeper
re-tests its condition and sleeps again if the wake was not for it. This is the
first Unix kernels' sleep and wakeup; a sleeper list per channel is worth its
pointers only when the walk is measured to matter.

- **The caller tests its condition and sleeps within one masked section.**
  `SchedulerSleep` nests its own, so no wake can fall between the test and the
  sleep.
- **Who sleeps**: `wait`, woken when a child ends; pipe readers and writers, woken
  by every read, write and close on the pipe; `pause`; and `poll`, on one channel
  that every readiness source also wakes ([`TERMINAL.md`](TERMINAL.md)).
- **The terminal's reader yields and halts rather than sleeping**
  ([`SHELL.md`](SHELL.md)).
- **`SchedulerWakeThread` wakes one thread from whatever it sleeps on**: what a
  signal does to a target asleep in a call, so the call sees the signal and returns
  `EINTR`. The bootstrap processor's tick performs it for Control-C and for alarms
  ([`../devices/TIME.md`](../devices/TIME.md)); the enqueue is safe there because
  the tick never reschedules while any lock is held.
- **A forked child is admitted** to the bootstrap processor's queue at the fork,
  with its kernel stack prepared for the trampoline. A thread the scheduler runs
  has nothing to return to, so a finishing thread wakes its parent and calls
  `SchedulerExitCurrent`.
- `SchedulerBlockCurrent` removes a thread from the rotation for good (the
  self-test's finished fixture threads, which would otherwise keep taking slices
  from every later self-test).

## Verification

`KernelVerifyScheduler` in [`../../kernel/test/proc/sched.c`](../../kernel/test/proc/sched.c)
runs four kernel threads that wait at a barrier until all are admitted, then work
without yielding and record what they did. A count of admissions is no evidence
that anything ran; a counter moved by the threads themselves is. The fixture is
kernel threads because user threads are confined to the bootstrap processor.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A mask naming no processor, or an offline one, is refused; an accepted mask is recorded. | A thread counted and never runnable; an affinity forgotten. |
| This processor's timer entry is unmasked; the measured rate is not zero. | A processor never pre-empted; a quantum of no counts. |
| An admitted thread records its queue always, and that it is queued when the queue is this processor's. | A thread admitted twice (a cyclic queue). The queued flag is checked only here because another processor may dequeue it at once. |
| **Every fixture thread completed all its rounds.** | A scheduler that counts threads without running them. |
| Each thread ran on an online processor, recorded from inside the thread. | A thread run on a processor other than its queue's. |
| Total slices exceed the number of threads. | No rotation: each thread ran once to completion. |
| The switch count rose, and **a quantum expired** (with more than one processor). | A switch path never taken; a timer never firing. |

Sleeping is asserted by the programs that depend on it: `file-check` passes 12 KiB
through a 4 KiB pipe between a parent and child; the shell's pipeline session sends
`/bin/sh` through `cat | wc -c` and gets its size back ([`SHELL.md`](SHELL.md));
the fork self-test collects a child that never ran; `pause` and `alarm` are
asserted by `init-check` and `signal-check`.

On QEMU (`-smp cores=2`) the log reports a timer rate of about 62,600 counts per
millisecond at divide-by-16 (a 1 GHz bus clock), four threads over two processors,
and quanta expired.

## Limitations

1. The bootstrap processor's boot flow is not a scheduled thread; `KernelMain` runs
   until `init` and the shell take over, and the tick leaves it alone.
2. No migration, stealing or rebalancing.
3. A finished kernel thread's stack and slot are never freed (no reaper); the four
   fixture threads cost that once per boot. A user thread's stack is freed when its
   process is collected.
4. One priority.
5. The 10 ms quantum is unmeasured.
6. `PagingActiveTable` is one global written on every switch. Harmless while user
   threads are confined to the bootstrap processor (all writers write the same
   value); it must become per processor when they are not.
7. No lock a thread can sleep on ([`CONCURRENCY.md`](CONCURRENCY.md)).
8. A wake walks all 128 thread slots.
9. A user thread is never pre-empted inside the kernel; a long system call would
   hold the bootstrap processor.
