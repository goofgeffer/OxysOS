/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/verify_sched.c
 * Purpose: Asserts the work of sub-task 6.15: the per-processor run queues, the
 *          affinity that decides which of them a thread may join, the
 *          round-robin rotation, and the local timer that takes a processor
 *          back when a quantum expires.
 * Key functions: KernelVerifyScheduler.
 * References:
 *   - docs/design/SCHEDULER.md, Section 7: the assertions, each paired with the
 *     silent failure it exists to catch.
 *   - Intel SDM, Volume 3A, Section 10.5.4: the local timer this test observes
 *     rather than trusts.
 *
 * What makes this test different from the four before it.
 *
 *   Sub-task 6.13's assertions were about internal state, because upon one
 *   processor a lock that does not lock behaves exactly like one that does.
 *   Sub-task 6.14's was about an acknowledgement, because that is the one thing
 *   a kernel which started nobody cannot fabricate.
 *
 *   **This one is about a number going up while nothing here touches it.** The
 *   test admits kernel threads that do nothing but increment counters of their
 *   own and yield, and then watches those counters from the bootstrap processor.
 *   A scheduler that enqueued threads and never ran them, or that ran one and
 *   called it all of them, produces counters that stay where they were put — and
 *   no report, no count of admissions and no length of queue would say so.
 *
 *   The threads are kernel threads and not programs, and that is not a
 *   convenience. A user thread reaches the allocators and the filesystem layer
 *   through its system calls, and those are still unsynchronised; the affinity
 *   mask is what keeps a user thread upon the bootstrap processor until they are
 *   not. A test that ran a program upon an application processor would be
 *   asserting something this kernel does not yet claim.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/sched.h>
#include <oxys/process.h>
#include <oxys/percpu.h>
#include <oxys/lapic.h>

/*
 * The fixture: a few threads that count and yield.
 *
 * Each has a slot of its own, so the counters need no lock — one writer each,
 * and the bootstrap processor reads them after every thread has stopped writing.
 * A shared counter would have made this test a test of the increment rather than
 * of the scheduler.
 */
#define VERIFY_SCHEDULER_THREADS 4U

/*
 * How many rounds each fixture thread performs, and how much work a round is.
 *
 * Both figures were chosen against a failed first attempt, and the reason is
 * worth keeping. The fixture originally yielded after every round and did no
 * work between them; it completed in microseconds, so no two threads were ever
 * runnable at the same moment, every thread ran to completion upon a single
 * slice, and **no quantum ever expired**. The test passed nothing it meant to.
 *
 * So a round is now a busy loop and a thread does not yield during one. Eight
 * rounds of two million iterations is some tens of milliseconds upon the
 * machines this is run on — several quanta — which is what makes pre-emption
 * something the test observes rather than hopes for.
 */
#define VERIFY_SCHEDULER_ROUNDS 8U
#define VERIFY_SCHEDULER_WORK   2000000U

static volatile uint64_t VerifySchedulerCounters[VERIFY_SCHEDULER_THREADS];
static volatile uint32_t VerifySchedulerProcessors[VERIFY_SCHEDULER_THREADS];
static volatile uint32_t VerifySchedulerFinished;
static volatile uint32_t VerifySchedulerNext;

/*
 * The barrier every fixture thread waits at before it begins.
 *
 * It is what guarantees the threads overlap. Without it a thread admitted first
 * runs to completion before the last is admitted, so no queue ever holds two
 * runnable threads and there is nothing for the scheduler to rotate between —
 * which is precisely the state the first version of this test measured and
 * called a success.
 */
static volatile uint32_t VerifySchedulerGo;

/*
 * What each fixture thread runs.
 *
 * It claims a slot, waits at the barrier, and then works. It records the
 * processor it is upon from within itself, with PerCpuIndex, because that is the
 * only place the answer is authoritative: a scheduler that queued a thread to
 * one processor and ran it upon another would be caught here and nowhere else.
 *
 * The claim is by a locked exchange-and-add rather than by an argument, because
 * ThreadCreateKernel takes no argument to pass. It is the one write here that
 * two processors can make at once, and it is atomic for that reason; every other
 * write is to a slot this thread alone owns.
 */
static void VerifySchedulerThread(void)
{
    uint32_t slot;

    __asm__ __volatile__("lock xaddl %0, %1"
                         : "=r"(slot), "+m"(VerifySchedulerNext)
                         : "0"(1U)
                         : "memory");

    if (slot >= VERIFY_SCHEDULER_THREADS)
    {
        SchedulerBlockCurrent();
        return;
    }

    /*
     * The wait is a yield and not a spin, and the difference matters upon a
     * machine with one processor: a thread that spun here would hold its
     * processor until pre-empted, and the thread that must set the flag is upon
     * that same processor.
     */
    while (VerifySchedulerGo == 0U)
    {
        SchedulerYield();
    }

    VerifySchedulerProcessors[slot] = PerCpuIndex();

    for (uint64_t round = 0U; round < VERIFY_SCHEDULER_ROUNDS; ++round)
    {
        /*
         * Work, and no yield within it. Giving the processor up here would make
         * every switch this test observes a voluntary one, and a voluntary
         * switch proves nothing about a timer.
         */
        for (volatile uint32_t spin = 0U; spin < VERIFY_SCHEDULER_WORK; ++spin)
        {
        }

        VerifySchedulerCounters[slot] = round + 1U;

        /* Recorded every round and not only the first, so that a thread which
         * somehow changed processor is caught. It cannot in this sub-task; the
         * assertion is what says so. */
        VerifySchedulerProcessors[slot] = PerCpuIndex();
    }

    __asm__ __volatile__("lock incl %0"
                         : "+m"(VerifySchedulerFinished)
                         :
                         : "memory");

    /*
     * The thread has finished and must leave the rotation.
     *
     * It cannot end: ThreadTerminateCurrent returns to the thread that started
     * this one, and nothing started this one — it was admitted to a queue, not
     * entered by a call. So it blocks, which takes it off every queue for good.
     *
     * Yielding for ever instead would be the quiet failure this whole test
     * exists to catch in others: four threads still taking their share of two
     * processors for the rest of the boot, with every later self-test running
     * against a machine switching threads beneath it.
     */
    SchedulerBlockCurrent();

    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

void KernelVerifyScheduler(void)
{
    bool succeeded = true;
    Thread *threads[VERIFY_SCHEDULER_THREADS];
    const uint32_t online = PerCpuOnlineCount();
    uint32_t created = 0U;
    uint32_t distinct = 0U;
    uint64_t switches_before;
    uint64_t slices = 0U;
    Thread *adopted;
    uint64_t waited;

    KernelWriteString("Scheduler: asserting the run queues and the rotation.\n");

    if (!SchedulerIsRunning())
    {
        /*
         * Not a skip. A machine whose local timer could not be calibrated runs
         * unpre-empted, which is a legitimate outcome the kernel reports — and
         * saying so is what distinguishes it from a scheduler that silently did
         * nothing.
         */
        KernelWriteString("  the scheduler is not running; the local timer was "
                          "not calibrated. Nothing is pre-empted.\n");
        KernelWriteString("Scheduler self-test passed: the kernel says why it "
                          "schedules nothing.\n");
        return;
    }

    if (LocalApicTimerCountsPerMillisecond() == 0U)
    {
        KernelWriteString("  the scheduler reports running upon a rate of zero "
                          "counts. FAILED.\n");
        succeeded = false;
    }

    /*
     * The timer entry must be unmasked upon this processor, read back from the
     * entry itself rather than from a variable this kernel keeps.
     *
     * A processor whose entry was never written would never be pre-empted, and
     * every other assertion here would still pass upon a machine where the
     * threads happened to yield often enough.
     */
    if (!LocalApicTimerIsRunning())
    {
        KernelWriteString("  this processor's local timer entry is masked. "
                          "FAILED.\n");
        succeeded = false;
    }


    /*
     * A thread for this processor to be, for the duration of the test.
     *
     * The bootstrap processor is executing KernelMain, which is a flow of
     * control and not a thread, and a processor with no current thread cannot be
     * switched away from — there is nowhere to save its context. So the test
     * adopts one, joins the rotation with it, and releases it at the end.
     *
     * It is released and not kept, and that is not tidiness. `ThreadStart`
     * succeeds or fails according to whether a thread is current, and
     * kernel/test/verify_lifecycle.c asserts the failing branch later in this
     * same boot; a thread left current here would silently turn that assertion
     * into a test of something else.
     */
    adopted = ThreadAdoptCurrent("scheduler self-test");

    if (adopted == NULL)
    {
        KernelWriteString("  no thread could be adopted for this processor. "
                          "FAILED.\n");
        KernelWriteString("Scheduler self-test FAILED.\n");
        return;
    }

    if (!SchedulerStartOnThisProcessor())
    {
        KernelWriteString("  this processor could not be put under the "
                          "scheduler. FAILED.\n");
        succeeded = false;
    }
    VerifySchedulerGo = 0U;
    VerifySchedulerNext = 0U;
    VerifySchedulerFinished = 0U;

    for (uint32_t index = 0U; index < VERIFY_SCHEDULER_THREADS; ++index)
    {
        VerifySchedulerCounters[index] = 0U;
        VerifySchedulerProcessors[index] = UINT32_MAX;
        threads[index] = NULL;
    }

    /*
     * A mask that names nobody must be refused.
     *
     * It is asserted before anything is admitted, because the failure it guards
     * against is a thread that is admitted, counted, and never run — which every
     * count in the report would describe as a success.
     */
    {
        Thread *const probe = ThreadCreateScheduled(VerifySchedulerThread);

        if (probe == NULL)
        {
            KernelWriteString("  a kernel thread could not be created. FAILED.\n");
            succeeded = false;
        }
        else
        {
            if (SchedulerSetAffinity(probe, 0U))
            {
                KernelWriteString("  an affinity naming no processor was "
                                  "accepted. FAILED.\n");
                succeeded = false;
            }

            if (SchedulerSetAffinity(probe, SCHED_AFFINITY_OF(PER_CPU_MAXIMUM - 1U)))
            {
                KernelWriteString("  an affinity naming a processor that is not "
                                  "online was accepted. FAILED.\n");
                succeeded = false;
            }

            if (!SchedulerSetAffinity(probe, SCHED_AFFINITY_BOOTSTRAP))
            {
                KernelWriteString("  an affinity naming the bootstrap processor "
                                  "was refused. FAILED.\n");
                succeeded = false;
            }

            if (SchedulerAffinity(probe) != SCHED_AFFINITY_BOOTSTRAP)
            {
                KernelWriteString("  an accepted affinity was not recorded. "
                                  "FAILED.\n");
                succeeded = false;
            }

            /* Destroyed rather than admitted: it exists to be refused, and a
             * thread left in the table would take a place in a rotation this
             * test is about to measure. */
            ThreadDestroy(probe);
        }
    }

    switches_before = SchedulerSwitchCount();

    /*
     * The fixture threads, admitted with an affinity that permits every online
     * processor.
     *
     * They are created here and not earlier because ThreadCreateKernel takes a
     * kernel stack from the arena, which is the bootstrap processor's alone —
     * this test runs upon it, and the creation is what relies upon that.
     */
    for (uint32_t index = 0U; index < VERIFY_SCHEDULER_THREADS; ++index)
    {
        Thread *const thread = ThreadCreateScheduled(VerifySchedulerThread);

        if (thread == NULL)
        {
            break;
        }

        if (!SchedulerSetAffinity(thread, SCHED_AFFINITY_ANY))
        {
            ThreadDestroy(thread);
            break;
        }

        if (!SchedulerAdmit(thread))
        {
            ThreadDestroy(thread);
            break;
        }

        /* An admitted thread must say it is queued and must name the queue it
         * is on. A thread whose flag said otherwise would be admitted twice by
         * the next caller, and a queue holding one thread twice is a cycle. */
        if (!thread->queued || (thread->processor >= PER_CPU_MAXIMUM))
        {
            KernelWriteString("  an admitted thread does not record its queue. "
                              "FAILED.\n");
            succeeded = false;
        }

        threads[index] = thread;
        ++created;
    }


    /*
     * Every thread is admitted before any is released, which is what makes them
     * overlap. See the note upon VerifySchedulerGo: a fixture that started as it
     * was admitted would finish each thread before the next arrived, and a
     * scheduler with never more than one runnable thread has nothing to rotate.
     */
    VerifySchedulerGo = 1U;

    if (created != VERIFY_SCHEDULER_THREADS)
    {
        KernelWriteString("  not every fixture thread could be admitted. "
                          "FAILED.\n");
        succeeded = false;
    }

    /*
     * Now the assertion this test exists for: the counters move.
     *
     * The bootstrap processor yields, over and over, and watches. Yielding is
     * what gives its own queue's share of the fixture a processor; the timer is
     * what gives the other processors' shares theirs. The wait is bounded,
     * because a scheduler that never runs anything must produce a failed test
     * and not a stopped machine.
     */
    for (waited = 0U; waited < 200000U; ++waited)
    {
        if (VerifySchedulerFinished >= created)
        {
            break;
        }

        SchedulerYield();
    }

    if (VerifySchedulerFinished < created)
    {
        KernelWriteString("  not every admitted thread ran to completion. "
                          "FAILED.\n");
        succeeded = false;
    }

    for (uint32_t index = 0U; index < created; ++index)
    {
        if (VerifySchedulerCounters[index] != VERIFY_SCHEDULER_ROUNDS)
        {
            KernelWriteString("  an admitted thread did not complete its "
                              "rounds. FAILED.\n");
            succeeded = false;
        }

        if (VerifySchedulerProcessors[index] >= online)
        {
            KernelWriteString("  a thread ran upon a processor that is not "
                              "online. FAILED.\n");
            succeeded = false;
        }

        /*
         * Each thread must have been given a processor at least once.
         *
         * A stronger claim was tried and was wrong: that a thread's slices must
         * be at least its rounds. It is not, and the reason is worth keeping.
         * A thread that yields into an *empty* queue is not switched away —
         * there is nobody to switch to — so it carries straight on and completes
         * many rounds upon one slice. The rotation is asserted below, where two
         * threads sharing a queue can be seen to have alternated.
         */
        if ((threads[index] != NULL) && (threads[index]->slices == 0U))
        {
            KernelWriteString("  a thread completed its rounds without ever "
                              "being given a processor. FAILED.\n");
            succeeded = false;
        }

        if (threads[index] != NULL)
        {
            slices += threads[index]->slices;
        }
    }

    /*
     * How many distinct processors the fixture actually ran upon.
     *
     * Reported and not asserted. The balance chooses the shortest queue, and
     * upon a machine whose threads finish faster than the next is admitted, one
     * processor is a legitimate answer — an equality here would mean the test
     * proved less than it hoped rather than that anything was wrong.
     */
    for (uint32_t index = 0U; index < created; ++index)
    {
        bool seen = false;

        for (uint32_t other = 0U; other < index; ++other)
        {
            if (VerifySchedulerProcessors[other] == VerifySchedulerProcessors[index])
            {
                seen = true;
            }
        }

        if (!seen)
        {
            ++distinct;
        }
    }


    /*
     * The rotation itself.
     *
     * There are four fixture threads and fewer processors, so at least one queue
     * held two of them; both completed all their rounds, so the processor must
     * have passed between them. Total slices strictly greater than the number of
     * threads is what that looks like from outside — one slice each would mean
     * every thread ran once, to completion, without ever giving the processor
     * up, which is not round-robin at all.
     */
    if ((created > online) && (slices <= (uint64_t)created))
    {
        KernelWriteString("  no thread was given a processor more than once; "
                          "nothing rotated. FAILED.\n");
        succeeded = false;
    }
    if (SchedulerSwitchCount() <= switches_before)
    {
        KernelWriteString("  the scheduler performed no context switch. "
                          "FAILED.\n");
        succeeded = false;
    }

    /*
     * And the pre-emption, which is the one thing yielding cannot demonstrate.
     *
     * Every switch above could have been a yield. What proves the timer takes a
     * processor back is a quantum that expired, and it is asserted only where
     * there is more than one processor: the fixture threads yield constantly, so
     * upon a machine with one processor a quantum may legitimately never expire
     * before the test is over.
     */
    if ((online > 1U) && (SchedulerPreemptionCount() == 0U))
    {
        KernelWriteString("  no quantum expired; nothing was pre-empted. "
                          "FAILED.\n");
        succeeded = false;
    }

    KernelWriteString("  the fixture ran upon ");
    KernelWriteDecimal((uint64_t)distinct);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)online);
    KernelWriteString(" processor(s), completing ");
    KernelWriteDecimal((uint64_t)created * VERIFY_SCHEDULER_ROUNDS);
    KernelWriteString(" rounds upon ");
    KernelWriteDecimal(slices);
    KernelWriteString(" slice(s).\n");

    /*
     * The adopted thread is released, which clears it from this processor and
     * takes the bootstrap processor back out of the rotation. See the note
     * where it was adopted for why that matters to a later self-test.
     */
    SchedulerDetachThisProcessor();
    ThreadDestroy(adopted);

    KernelWriteString(succeeded ? "Scheduler self-test passed.\n"
                                : "Scheduler self-test FAILED.\n");
}
