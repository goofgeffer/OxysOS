/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/proc/sched.h
 * Purpose: Declares the multiprocessor round-robin scheduler of sub-task 6.15,
 *          and the wait channel of sub-task 8.6 that lets a thread sleep upon it:
 *          the per-processor run queues, the affinity mask that decides which
 *          queue a thread may join, the quantum the local timer measures, and
 *          the idle thread each processor falls back to when its queue is empty.
 * Key definitions: SCHED_AFFINITY_ANY, SCHED_AFFINITY_BOOTSTRAP,
 *          SCHED_AFFINITY_OF, SCHED_QUANTUM_MILLISECONDS, SCHED_TICK_VECTOR,
 *          SchedulerInitialise, SchedulerPrepareProcessor, SchedulerEnterIdle,
 *          SchedulerAdmit, SchedulerYield, SchedulerBlockCurrent,
 *          SchedulerCanSleep, SchedulerSleep, SchedulerWake, SchedulerWakeThread,
 *          SchedulerExitCurrent, SchedulerWithdraw,
 *          SchedulerDetachThisProcessor,
 *          SchedulerSetAffinity,
 *          SchedulerAffinity, SchedulerQueueLength, SchedulerIsRunning,
 *          SchedulerReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 10.5.4 and Figure 10-10: the local timer, its three modes, and the
 *     divide configuration register whose encoding is not a plain divisor.
 *   - Intel SDM, Volume 3A, Section 10.5.1: the local vector table is per
 *     processor, which is why each processor starts its own timer rather than
 *     being given one.
 *   - Intel SDM, Volume 3A, Section 8.2.2: the memory-ordering model, which is
 *     what makes a thread enqueued under one processor's lock visible to the
 *     processor that dequeues it.
 *   - docs/design/SCHEDULER.md: the design, the ordering, and what each
 *     assertion exists to catch.
 *
 * What this sub-task adds, and what it deliberately does not.
 *
 *   Sub-task 6.14 started the application processors and gave them nothing to
 *   run. This gives them something: a run queue of their own, a timer of their
 *   own that takes a thread away when its quantum expires, and an idle thread to
 *   fall back to. A thread is placed upon a queue once, by affinity and by
 *   length, and stays there.
 *
 *   **It does not make the kernel safe for a user program upon an application
 *   processor.** The allocators, the filesystem layer and the process tables
 *   that a system call reaches are still unsynchronised, and
 *   docs/design/CONCURRENCY.md, Section 10, limitation 1, enumerates them. The
 *   affinity mask is what expresses that: a user thread's mask names the
 *   bootstrap processor alone, so the constraint is a value in a field that can
 *   be widened in one place when those locks land, rather than a rule written
 *   nowhere and remembered by whoever reads the code next.
 */

#ifndef OXYS_PROC_SCHED_H
#define OXYS_PROC_SCHED_H

#include <oxys/types.h>
#include <oxys/proc/process.h>

/*
 * The affinity masks.
 *
 * A bit is a processor index of kernel/include/oxys/percpu.h — this kernel's
 * dense numbering from zero, not the identifier the local controller answers to.
 * The two differ on any machine whose firmware numbers its processors sparsely,
 * and a mask built from the wrong one would name processors that do not exist
 * while excluding ones that do.
 */
#define SCHED_AFFINITY_ANY       UINT64_MAX
#define SCHED_AFFINITY_BOOTSTRAP UINT64_C(1)
#define SCHED_AFFINITY_OF(index) (UINT64_C(1) << (index))

/*
 * How long a thread holds a processor before the timer takes it away.
 *
 * Ten milliseconds is the figure this kernel starts from, and the reason it is
 * stated here rather than tuned is that there is nothing yet to tune against: a
 * quantum is a trade between switch cost and responsiveness, and neither has
 * been measured because no workload exists to measure them with. It is long
 * enough that the switch — six registers and a stack pointer — is lost in it,
 * and short enough that a thread which never yields does not hold a processor
 * for a visible time.
 */
#define SCHED_QUANTUM_MILLISECONDS 10U

/*
 * The vector the local timer delivers pre-emption upon.
 *
 * It stands beneath the two of sub-task 6.13 — 0xFD for the shootdown and 0xFC
 * for the halt — and beneath the controller's own 0xFF and 0xFE, because the
 * priority class of a vector is its top four bits and a pre-emption must not
 * outrank a shootdown. A processor that deferred an invalidation in order to
 * switch threads would run the incoming thread against a translation the sender
 * has already been told was discarded.
 */
#define SCHED_TICK_VECTOR UINT8_C(0xFB)

/*
 * Prepares the scheduler, upon the bootstrap processor and before any
 * application processor is started.
 *
 * It creates one idle thread per processor slot, registers the tick handler, and
 * calibrates the local timer against the interval timer. All three are done here
 * rather than by each processor for itself, and the reason is the rule sub-task
 * 6.14 was built around: a starting processor allocates nothing. An idle thread
 * is a kernel stack out of the arena, and the arena admits one processor.
 *
 * It must be called after the process tables exist, after the local controller
 * is enabled and after the interval timer is running — the calibration being a
 * busy wait upon that timer's counter — and before SmpInitialise, so that a
 * processor which comes online finds an idle thread already made for it.
 *
 * Returns false where the calibration failed, in which case no timer is started
 * anywhere and the machine runs unpre-empted rather than upon an invented rate.
 */
bool SchedulerInitialise(void);

/*
 * Starts the executing processor's own timer and puts it under the scheduler.
 *
 * Each processor calls it for itself because the local vector table is per
 * processor: a timer programmed by the bootstrap processor upon its own
 * controller says nothing whatever about any other, and a processor that never
 * called this would be one that is never pre-empted — which presents as a
 * machine that works until one thread upon that processor stops yielding.
 */
bool SchedulerStartOnThisProcessor(void);

/*
 * Forgets this processor's idle thread, leaving its timer running.
 *
 * It exists for a caller that adopted a thread in order to join the rotation and
 * is now giving that thread up — the scheduler's own self-test is the only one
 * today. Without it the pointer would outlive the thread it names, and the next
 * reschedule upon this processor would switch to a released table slot.
 *
 * The timer is deliberately left running. The scheduler has not stopped; this
 * processor has merely stopped having a thread to be, and a reschedule with no
 * current thread does nothing.
 */
void SchedulerDetachThisProcessor(void);

/*
 * What an application processor runs instead of parking.
 *
 * It replaces the halt loop of sub-task 6.14. The processor becomes its own idle
 * thread, takes whatever its queue holds, and returns to idling when the queue
 * is empty. It does not return.
 */
_Noreturn void SchedulerEnterIdle(void);

/*
 * Places a runnable thread upon a run queue.
 *
 * The queue is chosen among the processors the thread's affinity permits and
 * that are online, by the shortest queue — which is the whole of the balancing
 * this sub-task performs, and it is performed here because here is the only
 * moment a thread changes processor. See docs/design/SCHEDULER.md, Section 5,
 * for why a thread does not migrate afterwards.
 *
 * Returns false where the thread is unusable, already queued, or where its
 * affinity names no online processor — the last being a thread that would
 * otherwise be counted as admitted and never run.
 */
bool SchedulerAdmit(Thread *thread);

/*
 * Gives up the rest of this thread's quantum.
 *
 * The thread goes to the back of its own queue and the next is taken from the
 * front, which is what makes the discipline round-robin rather than a rotation
 * of whatever happened to be at the head.
 */
void SchedulerYield(void);

/*
 * Takes the calling thread out of the rotation for good.
 *
 * It sets the state to blocked and gives the processor up. A blocked thread is
 * never put back on a queue — SchedulerRescheduleLocked enqueues only a thread
 * that is still running — so nothing will schedule it again until something
 * makes it runnable, and in this sub-task nothing does.
 *
 * That asymmetry is deliberate and is what the self-test needs: a fixture thread
 * that had finished its work but stayed runnable would keep taking a share of
 * its processor for the whole of the remaining boot, and every self-test after
 * it would be running against a machine that was quietly switching threads
 * underneath it. There is no reaper, so the thread's table slot is not
 * reclaimed; docs/design/SCHEDULER.md, Section 10, limitation 4, records that.
 *
 * It returns only if the caller is an idle thread, which has nowhere to go.
 */
void SchedulerBlockCurrent(void);

/*
 * The wait channel, of sub-task 8.6: the first thing in this kernel a thread
 * can sleep upon and be woken from.
 *
 * SchedulerSleep takes the calling thread out of the rotation until something
 * calls SchedulerWake with the same channel. The channel is an address and
 * nothing more — the process a parent waits upon, the pipe a reader or writer
 * waits upon — and carries no meaning of its own; what it identifies is the
 * condition the caller tested and found false. The caller tests again upon
 * waking, in a loop, because a wake is broadcast to every sleeper upon the
 * channel and says only that the condition may have changed.
 *
 * The discipline the caller must follow is the one every sleep-and-wakeup
 * kernel has had since the first: test the condition and call SchedulerSleep
 * within one masked section, so that no wake can fall between the test and the
 * sleep. SchedulerSleep enters its own masked section as well, so a caller
 * that has already entered one nests it and nothing is lost.
 *
 * SchedulerSleep returns at once, doing nothing, where the caller cannot
 * sleep: where no thread is current, where the caller is the idle thread, or
 * where the scheduler was never prepared. SchedulerCanSleep is that test,
 * exposed so that a caller with no way to wait — the kernel's own flow of
 * control inside a self-test — can refuse an operation that would block
 * rather than spin upon it.
 *
 * SchedulerWake returns how many threads it made runnable.
 */
bool SchedulerCanSleep(void);
void SchedulerSleep(const void *channel);
size_t SchedulerWake(const void *channel);

/*
 * Wakes one thread from whatever channel it sleeps upon, of sub-task 8.7: what
 * a signal does to a target asleep in a call, so that the call re-tests its
 * condition, finds the signal pending, and reports EINTR. A thread that is not
 * asleep upon a channel is left alone. Returns whether it was woken.
 */
bool SchedulerWakeThread(Thread *thread);

/*
 * Gives the processor up for good, of sub-task 8.6, on behalf of a thread that
 * has ended.
 *
 * The caller has marked its own state THREAD_EXITED; this function merely
 * reschedules, and a thread whose state is not running is not put back upon a
 * queue. It does not return: a thread that has ended has nothing to run on to,
 * and it panics rather than returning where the processor had neither another
 * thread nor an idle one, which upon a prepared scheduler cannot happen.
 */
_Noreturn void SchedulerExitCurrent(void);

/*
 * Takes a thread off the queue it is upon, of sub-task 8.6.
 *
 * It exists for a thread that was admitted and is being destroyed before it
 * ran — a child of `fork` collected by a caller with no thread to sleep upon,
 * which the fork self-test arranges — because a destroyed thread left linked
 * into a queue would be dequeued later and switched to, and its stack is by
 * then somebody else's. Returns false where the thread was not queued.
 */
bool SchedulerWithdraw(Thread *thread);

/*
 * Sets a thread's affinity, and refuses a mask that names no online processor.
 *
 * The refusal is the point: a mask of zero, or one naming only processors this
 * machine does not have, describes a thread nothing may ever run. Accepting it
 * would produce a thread that is admitted, counted and permanently invisible.
 *
 * A thread already queued is not moved. The mask governs the next admission, and
 * Section 5 of the design says why that is the whole of the policy.
 */
bool SchedulerSetAffinity(Thread *thread, uint64_t mask);
uint64_t SchedulerAffinity(const Thread *thread);

/* The number of threads waiting upon a processor's queue, not counting the one
 * it is running. */
uint32_t SchedulerQueueLength(uint32_t processor);

/* The thread a processor is running, or its idle thread; NULL for a processor
 * that is not online. It is for the report and the self-test, and it is a
 * snapshot of something another processor is changing. */
const Thread *SchedulerRunningOn(uint32_t processor);

/* Whether the scheduler is initialised and its timers are running. */
bool SchedulerIsRunning(void);

/* Totals across the machine: threads admitted, context switches performed,
 * quanta expired, and times a processor found its queue empty. */
uint64_t SchedulerAdmissionCount(void);
uint64_t SchedulerSwitchCount(void);
uint64_t SchedulerPreemptionCount(void);
uint64_t SchedulerIdleCount(void);

/* Emits a summary upon the console and the serial port. */
void SchedulerReport(void);

#endif /* OXYS_PROC_SCHED_H */
