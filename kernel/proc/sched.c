/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/proc/sched.c
 * Purpose: Implements the multiprocessor round-robin scheduler of sub-task
 *          6.15: the per-processor run queues and the lock each carries, the
 *          affinity that decides which queue a thread may join, the choice of
 *          the shortest eligible queue at admission, the local timer that takes
 *          a processor back when a quantum expires, and the idle thread each
 *          processor falls back to.
 * Key functions: SchedulerInitialise, SchedulerStartOnThisProcessor,
 *          SchedulerEnterIdle, SchedulerAdmit, SchedulerYield,
 *          SchedulerSetAffinity, SchedulerAffinity, SchedulerQueueLength,
 *          SchedulerRunningOn, SchedulerIsRunning, SchedulerReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 10.5.4 and Figure 10-10: the local timer's three modes, and the
 *     divide configuration register in which bit 2 is reserved and the divisor
 *     is carried in bits 3, 1 and 0.
 *   - Intel SDM, Volume 3A, Section 10.5.1: the local vector table is per
 *     processor, which is why each processor starts its own timer.
 *   - Intel SDM, Volume 3A, Section 10.8.3.1: the priority class of a vector is
 *     its upper four bits, which is why the tick stands beneath the shootdown.
 *   - Intel SDM, Volume 3A, Section 8.2.2: stores are not reordered with other
 *     stores, which is what makes a thread enqueued under one processor's lock
 *     complete to the processor that dequeues it.
 *   - docs/design/SCHEDULER.md: the design and the reasoning.
 *
 * The rule this file is built around.
 *
 *   **A run queue is locked, and nothing else here is.** Each queue has a lock
 *   of its own rather than one lock covering all of them, because the whole
 *   purpose of per-processor queues is that two processors scheduling at once do
 *   not wait for each other; a single scheduler lock would give the correctness
 *   and none of the reason for the structure.
 *
 *   That decision has a cost, and it is paid in SchedulerAdmit: choosing the
 *   shortest queue means reading several lengths, and the answer is stale before
 *   it is acted upon. It is a placement hint and is treated as one — the
 *   enqueue that follows takes the chosen queue's lock and is correct whatever
 *   the length turned out to be. A balance that was momentarily wrong costs a
 *   thread waiting behind one other thread; a balance computed under a lock
 *   spanning every queue would cost every admission on the machine.
 *
 * Concurrency. `SchedulerQueues[i].lock` guards that queue's head, tail and
 * count, and is the only lock this file takes. The idle threads and the
 * calibration are written once by the bootstrap processor before any other
 * processor exists and are read-only thereafter. The four machine-wide counters
 * are incremented without a lock and are diagnostics: a lost increment costs a
 * figure in a report, and a lock upon the switch path would cost every switch.
 */

#include <oxys/proc/sched.h>
#include <oxys/proc/process.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/arch/cpu/spinlock.h>
#include <oxys/dev/lapic.h>
#include <oxys/arch/interrupt/interrupts.h>
#include <oxys/kernel.h>

/*
 * A processor's run queue.
 *
 * Singly linked, with a tail pointer, because the two operations are "take from
 * the front" and "put at the back" and nothing else: round-robin needs no
 * removal from the middle, and a doubly linked list would be two pointers to
 * keep consistent where one suffices. A thread that must leave a queue it is
 * already upon does not exist in this sub-task — see docs/design/SCHEDULER.md,
 * Section 5, limitation 3.
 */
typedef struct SchedulerQueue
{
    Thread *head;
    Thread *tail;
    uint32_t count;
    Spinlock lock;
} SchedulerQueue;

static SchedulerQueue SchedulerQueues[PER_CPU_MAXIMUM];

/*
 * The thread a processor runs when its queue is empty.
 *
 * One per processor, and it must be one per processor: an idle thread is a
 * kernel stack, and two processors idling upon one stack would be two
 * interrupt frames written over each other the moment both took a timer tick.
 *
 * Each is the execution that processor was already performing, described by a
 * thread structure rather than created as a new one — the bootstrap processor's
 * by ThreadAdoptCurrent at sub-task 6.9, and every other processor's by
 * SchedulerEnterIdle below. Neither takes a stack from the arena, which is what
 * lets a started processor have one at all: the arena admits one processor, and
 * a processor that had to allocate its own idle stack could not be started
 * safely. Its element is written by the processor it belongs to.
 */
static Thread *SchedulerIdleThreads[PER_CPU_MAXIMUM];

/* Whether the tick handler is installed and the rate was measured. Written by
 * the bootstrap processor before any other processor runs. */
static bool SchedulerPrepared;

/* Whether this processor's own timer has been started. Each element is written
 * by the processor it belongs to and read by the report. */
static bool SchedulerTimerStarted[PER_CPU_MAXIMUM];

/* Machine-wide accounting; see the concurrency note in the file header. */
static uint64_t SchedulerAdmissions;
static uint64_t SchedulerSwitches;
static uint64_t SchedulerPreemptions;
static uint64_t SchedulerIdles;

/* ------------------------------------------------------------------ helpers */

static uint32_t SchedulerIndex(void)
{
    const uint32_t index = PerCpuIsEstablished() ? PerCpuIndex() : 0U;

    return (index < PER_CPU_MAXIMUM) ? index : 0U;
}

/*
 * Puts a thread at the back of a numbered queue.
 *
 * The caller has already decided the queue and has checked the affinity. This
 * takes the lock, links the thread, and records upon the thread which queue it
 * is on — all three under the one acquisition, because a thread linked into a
 * queue whose `processor` field said otherwise would be dequeued by one
 * processor and accounted to another.
 */
static void SchedulerEnqueue(uint32_t processor, Thread *thread)
{
    SchedulerQueue *const queue = &SchedulerQueues[processor];

    SpinlockAcquire(&queue->lock);

    thread->queue_next = NULL;
    thread->processor = processor;
    thread->queued = true;
    thread->state = THREAD_READY;

    if (queue->tail == NULL)
    {
        queue->head = thread;
    }
    else
    {
        queue->tail->queue_next = thread;
    }

    queue->tail = thread;
    ++queue->count;

    SpinlockRelease(&queue->lock);
}

/* Takes the thread at the front, or NULL where there is none. */
static Thread *SchedulerDequeue(uint32_t processor)
{
    SchedulerQueue *const queue = &SchedulerQueues[processor];
    Thread *thread;

    SpinlockAcquire(&queue->lock);

    thread = queue->head;

    if (thread != NULL)
    {
        queue->head = thread->queue_next;

        if (queue->head == NULL)
        {
            queue->tail = NULL;
        }

        thread->queue_next = NULL;
        thread->queued = false;
        --queue->count;
    }

    SpinlockRelease(&queue->lock);

    return thread;
}

/*
 * Which processor a thread should be admitted to.
 *
 * The shortest queue among those its affinity permits and that are online. The
 * bootstrap processor breaks a tie, because it is the one processor certain to
 * exist and a tie broken arbitrarily would scatter threads that could have
 * shared a cache.
 *
 * Returns PER_CPU_MAXIMUM where the mask names nobody online, which the caller
 * turns into a refusal rather than a default: a thread admitted to processor 0
 * because its mask excluded processor 0 is a thread running where it was
 * forbidden to, and nothing downstream would ever notice.
 */
static uint32_t SchedulerChooseProcessor(uint64_t affinity)
{
    const uint32_t online = PerCpuOnlineCount();
    uint32_t chosen = PER_CPU_MAXIMUM;
    uint32_t shortest = UINT32_MAX;

    for (uint32_t index = 0U; (index < online) && (index < PER_CPU_MAXIMUM); ++index)
    {
        uint32_t length;

        if ((affinity & SCHED_AFFINITY_OF(index)) == 0U)
        {
            continue;
        }

        /*
         * Read without the lock, deliberately. See the file header: this is a
         * placement hint, the enqueue that follows is what is correct, and a
         * lock taken across every queue to compute it would serialise every
         * admission on the machine against every other.
         */
        length = SchedulerQueues[index].count;

        if (length < shortest)
        {
            shortest = length;
            chosen = index;
        }
    }

    return chosen;
}

/*
 * The switch itself, performed with interrupts already masked by the caller.
 *
 * Everything difficult about this function is in what it does *not* do. It does
 * not take a lock across the switch: ThreadSwitchTo does not return until
 * somebody switches back, so a lock held across it would be released by a
 * different thread at a different time, upon a processor that may not be this
 * one — which is the misuse sub-task 6.13's spinlock panics about, and rightly.
 *
 * The outgoing thread is enqueued *before* the switch and not after, because
 * after does not exist: control does not come back here until this thread is
 * chosen again, and by then the processor is running something else that would
 * have found nothing to run.
 */
static void SchedulerSwitchTo(Thread *current, Thread *next)
{
    if ((next == NULL) || (next == current))
    {
        return;
    }

    ++SchedulerSwitches;
    ++next->slices;

    ThreadSwitchTo(current, next);
}

/*
 * Chooses what to run next upon this processor and goes there.
 *
 * `preempted` distinguishes a quantum that expired from a thread that gave the
 * processor up, which is the difference the accounting exists to show: a thread
 * that is always pre-empted is one the quantum is too short for, and a thread
 * that always yields is one the quantum never reaches.
 *
 * It is called with interrupts masked and returns with them masked; the caller
 * owns the critical section, because the caller is sometimes an interrupt
 * handler that must signal end-of-interrupt at a particular moment.
 */
static void SchedulerRescheduleLocked(bool preempted)
{
    const uint32_t index = SchedulerIndex();
    Thread *const current = ThreadCurrent();
    Thread *const idle = SchedulerIdleThreads[index];
    Thread *next;

    if (current == NULL)
    {
        return;
    }

    next = SchedulerDequeue(index);

    if (next == NULL)
    {
        /*
         * Nothing waiting. A thread that is still runnable keeps the processor
         * — there is no one to give it to — and the idle thread is entered only
         * where the running thread has stopped being runnable.
         */
        if (current == idle)
        {
            ++SchedulerIdles;
            return;
        }

        if (current->state == THREAD_RUNNING)
        {
            return;
        }

        next = idle;
    }

    if (preempted)
    {
        ++SchedulerPreemptions;
        ++current->preemptions;
    }

    /*
     * The outgoing thread goes back on its own queue, and only if it is still
     * runnable and is not the idle thread.
     *
     * The idle thread is never queued. It is the fallback, not a competitor: a
     * queued idle thread would take a full quantum in the rotation, so a
     * processor with one runnable thread would spend half its time halting.
     */
    if ((current != idle) && (current->state == THREAD_RUNNING))
    {
        SchedulerEnqueue(index, current);
    }

    SchedulerSwitchTo(current, next);
}


/*
 * What a processor does when it has nothing to run.
 *
 * The two instructions are adjacent and must be, for the reason sub-task 6.14's
 * park loop gave: Intel SDM, Volume 2B, "STI", provides that the interrupt flag
 * takes effect only after the instruction *following* STI, so an interrupt
 * arriving in between is held until the HLT has been entered rather than being
 * delivered before it and leaving the processor asleep with the reason it was
 * woken already past.
 *
 * What wakes it is the local timer, or a shootdown, or a halt request. Whichever
 * it was, the queue is asked again on the way round — a thread admitted to this
 * processor while it slept is picked up at the first tick after the admission,
 * and not later.
 */
static _Noreturn void SchedulerIdleLoop(void)
{
    for (;;)
    {
        __asm__ __volatile__("sti; hlt");

        SchedulerYield();
    }
}
/* ------------------------------------------------------------- the tick */

/*
 * The local timer's handler.
 *
 * The end-of-interrupt is signalled **before** the switch and not after, and
 * that ordering is the whole of what makes pre-emption work. ThreadSwitchTo does
 * not return until this thread runs again, so an end-of-interrupt written after
 * it would be written when this thread is next scheduled — leaving the tick in
 * service upon the local controller in the meantime, which blocks every vector
 * of its priority class and above. The processor would take one timer interrupt
 * and then no more, and present as a machine on which pre-emption worked once.
 */
static void SchedulerHandleTick(TrapFrame *frame)
{
    (void)frame;

    LocalApicSignalEndOfInterrupt();

    if (!SchedulerPrepared)
    {
        return;
    }

    /*
     * A tick that arrives while this processor holds a lock is counted and
     * otherwise ignored.
     *
     * Switching there would carry the lock to a thread that did not take it and
     * leave the taker unable to release it — and the taker may be inside the
     * queue lock of this very function. It is not deferred to a later moment
     * either: the next tick is ten milliseconds away, and a section that is
     * still held then is a defect this kernel would rather report than schedule
     * around.
     */
    if (PerCpuLocksHeld() != 0U)
    {
        return;
    }

    PerCpuPushInterruptState();
    SchedulerRescheduleLocked(true);
    PerCpuPopInterruptState();
}

/* --------------------------------------------------------------- interface */

bool SchedulerInitialise(void)
{
    SchedulerPrepared = false;

    for (uint32_t index = 0U; index < PER_CPU_MAXIMUM; ++index)
    {
        SpinlockInitialise(&SchedulerQueues[index].lock, "run queue");
        SchedulerQueues[index].head = NULL;
        SchedulerQueues[index].tail = NULL;
        SchedulerQueues[index].count = 0U;
        SchedulerIdleThreads[index] = NULL;
        SchedulerTimerStarted[index] = false;
    }

    InterruptRegisterHandler(SCHED_TICK_VECTOR, SchedulerHandleTick,
                             "scheduler tick");

    if (!LocalApicCalibrateTimer())
    {
        return false;
    }

    SchedulerPrepared = true;

    return true;
}

bool SchedulerStartOnThisProcessor(void)
{
    const uint32_t index = SchedulerIndex();

    if (!SchedulerPrepared)
    {
        return false;
    }

    /*
     * This processor's idle thread, if it is running something that can serve as
     * one and has not been given one yet.
     *
     * An idle thread is the execution already in progress, described — not a new
     * one. Every processor arrives here already running upon a stack that is its
     * own: the bootstrap processor upon the boot stack the linker established,
     * and every other upon the stack the bootstrap processor prepared for it
     * before the startup interrupt was sent.
     *
     * **The bootstrap processor may have no current thread at all, and this does
     * not make one for it.** At this point in the boot it is executing
     * KernelMain, which is a flow of control and not a thread: nothing has
     * adopted one since the self-tests of sub-tasks 6.10 and 6.11 released
     * theirs. Adopting one here would be visible far outside the scheduler —
     * ThreadStart succeeds or fails according to whether a thread is current,
     * and kernel/test/verify_lifecycle.c asserts the failing branch — so the
     * bootstrap processor joins the rotation only when something has given it a
     * thread to join as. docs/design/SCHEDULER.md, Section 8, limitation 1,
     * records what that costs.
     */
    if (SchedulerIdleThreads[index] == NULL)
    {
        Thread *const idle = ThreadCurrent();

        if (idle != NULL)
        {
            idle->affinity = SCHED_AFFINITY_OF(index);
            idle->processor = index;
            SchedulerIdleThreads[index] = idle;
        }
    }

    if (!LocalApicStartTimer(SCHED_TICK_VECTOR, SCHED_QUANTUM_MILLISECONDS))
    {
        return false;
    }

    SchedulerTimerStarted[index] = true;

    return true;
}


_Noreturn void SchedulerEnterIdle(void)
{
    /*
     * The idle thread this processor becomes, adopted here and not in
     * SchedulerStartOnThisProcessor.
     *
     * The distinction is which processor is asking. An application processor has
     * no current thread and nothing else will ever give it one, so it must adopt
     * — and adopting is safe for it: it is already running upon a stack the
     * bootstrap processor prepared, and its task state segment names that stack,
     * so ThreadAdoptCurrent takes a slot in the thread table and nothing from
     * the arena. The bootstrap processor is a different case entirely, and
     * SchedulerStartOnThisProcessor says why.
     *
     * The slot is the one thing two processors could contend for here, and
     * ProcessTableLock is what makes the claim of it atomic.
     */
    if (ThreadAdoptCurrent("idle") == NULL)
    {
        KernelWriteString("  A processor could not claim an idle thread; it "
                          "answers interrupts and idles.\n");
    }
    else if (!SchedulerStartOnThisProcessor())
    {
        /*
         * No rate was measured, so no timer can be started. The processor is
         * exactly what sub-task 6.14 left it: one that answers inter-processor
         * interrupts and nothing else. That is a smaller machine than intended
         * and a reported one.
         */
        KernelWriteString("  A processor could not be put under the scheduler; "
                          "it answers interrupts and idles.\n");
    }


    SchedulerIdleLoop();
}

void SchedulerDetachThisProcessor(void)
{
    const uint32_t index = SchedulerIndex();

    PerCpuPushInterruptState();
    SchedulerIdleThreads[index] = NULL;
    PerCpuPopInterruptState();
}
bool SchedulerAdmit(Thread *thread)
{
    uint32_t processor;

    if ((thread == NULL) || !thread->used || thread->queued)
    {
        return false;
    }

    if ((thread->state == THREAD_EXITED) || (thread->state == THREAD_UNUSED))
    {
        return false;
    }

    processor = SchedulerChooseProcessor(thread->affinity);

    if (processor >= PER_CPU_MAXIMUM)
    {
        return false;
    }

    SchedulerEnqueue(processor, thread);
    ++SchedulerAdmissions;

    return true;
}

void SchedulerYield(void)
{
    if (!SchedulerPrepared)
    {
        return;
    }

    PerCpuPushInterruptState();
    SchedulerRescheduleLocked(false);
    PerCpuPopInterruptState();
}


void SchedulerBlockCurrent(void)
{
    Thread *current;

    if (!SchedulerPrepared)
    {
        return;
    }

    PerCpuPushInterruptState();

    current = ThreadCurrent();

    if ((current != NULL) && (current != SchedulerIdleThreads[SchedulerIndex()]))
    {
        /*
         * The state is changed *before* the reschedule and not after, because
         * after does not exist: the switch does not return until something
         * makes this thread runnable again, and the reschedule is what reads
         * the state to decide whether to put it back on a queue.
         */
        current->state = THREAD_BLOCKED;
        SchedulerRescheduleLocked(false);
    }

    PerCpuPopInterruptState();
}
bool SchedulerSetAffinity(Thread *thread, uint64_t mask)
{
    if ((thread == NULL) || !thread->used)
    {
        return false;
    }

    if (SchedulerChooseProcessor(mask) >= PER_CPU_MAXIMUM)
    {
        return false;
    }

    thread->affinity = mask;

    return true;
}

uint64_t SchedulerAffinity(const Thread *thread)
{
    return (thread != NULL) ? thread->affinity : 0U;
}

uint32_t SchedulerQueueLength(uint32_t processor)
{
    if (processor >= PER_CPU_MAXIMUM)
    {
        return 0U;
    }

    return SchedulerQueues[processor].count;
}

const Thread *SchedulerRunningOn(uint32_t processor)
{
    if (processor >= PER_CPU_MAXIMUM)
    {
        return NULL;
    }

    return ThreadCurrentOn(processor);
}

bool SchedulerIsRunning(void)
{
    return SchedulerPrepared && SchedulerTimerStarted[0];
}

uint64_t SchedulerAdmissionCount(void)
{
    return SchedulerAdmissions;
}

uint64_t SchedulerSwitchCount(void)
{
    return SchedulerSwitches;
}

uint64_t SchedulerPreemptionCount(void)
{
    return SchedulerPreemptions;
}

uint64_t SchedulerIdleCount(void)
{
    return SchedulerIdles;
}

void SchedulerReport(void)
{
    const uint32_t online = PerCpuOnlineCount();

    KernelWriteString("Scheduler: ");

    if (!SchedulerPrepared)
    {
        KernelWriteString("not running; the local timer could not be "
                          "calibrated, so nothing is pre-empted.\n");
        return;
    }

    KernelWriteString("round-robin, quantum ");
    KernelWriteDecimal((uint64_t)SCHED_QUANTUM_MILLISECONDS);
    KernelWriteString(" ms, tick vector ");
    KernelWriteHexadecimal((uint64_t)SCHED_TICK_VECTOR);
    KernelWriteString(", local timer ");
    KernelWriteDecimal((uint64_t)LocalApicTimerCountsPerMillisecond());
    KernelWriteString(" counts/ms.\n");

    KernelWriteString("Scheduler: admitted ");
    KernelWriteDecimal(SchedulerAdmissions);
    KernelWriteString(", switches ");
    KernelWriteDecimal(SchedulerSwitches);
    KernelWriteString(", pre-empted ");
    KernelWriteDecimal(SchedulerPreemptions);
    KernelWriteString(", found idle ");
    KernelWriteDecimal(SchedulerIdles);
    KernelWriteString(".\n");

    for (uint32_t index = 0U; (index < online) && (index < PER_CPU_MAXIMUM); ++index)
    {
        const Thread *const running = ThreadCurrentOn(index);

        KernelWriteString("  Processor ");
        KernelWriteDecimal((uint64_t)index);
        KernelWriteString(": queue ");
        KernelWriteDecimal((uint64_t)SchedulerQueues[index].count);
        KernelWriteString(", timer ");
        KernelWriteString(SchedulerTimerStarted[index] ? "running" : "STOPPED");
        KernelWriteString(", running thread ");
        KernelWriteDecimal((running != NULL) ? running->id : 0U);

        if ((running != NULL) && (running == SchedulerIdleThreads[index]))
        {
            KernelWriteString(" (idle)");
        }

        KernelWriteString(".\n");
    }
}
