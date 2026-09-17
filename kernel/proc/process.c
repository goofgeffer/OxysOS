/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/proc/process.c
 * Purpose: Implements the process and thread tables: the creation and
 *          destruction of a process with an address space of its own, of a
 *          thread with a kernel stack of its own beneath a guard page, and the
 *          record of what a process has had loaded into it.
 * Key functions: ProcessInitialise, ProcessCreate, ProcessDestroy, ThreadCreate,
 *          ThreadDestroy, ThreadSetCurrent, ProcessCreateUserStack,
 *          ProcessRecordImage, ProcessReport, ProcessFork, ProcessExecute,
 *          ProcessExit, ProcessWait.
 * References: kernel/include/oxys/process.h states what this implements, and
 *          docs/design/PROCESS.md why.
 *
 * Nothing here runs anything.
 *
 *   This sub-task defines the structures and fills them in. No context is ever
 *   restored, no address space is made active, and no thread has executed a
 *   single instruction. Sub-task 6.10 is what transfers to one, and the division
 *   is deliberate: a structure that has never been switched to is one whose
 *   shape can still be argued about, and one that has is a structure with
 *   assembly written against its offsets.
 *
 *   Sub-task 6.11 adds the four calls by which a program governs another: fork,
 *   execve, exit and wait. They are placed here rather than beside the dispatch
 *   table because none of them is a system call in substance — each is an
 *   operation upon the two tables above, and kernel/arch/x86_64/syscall/syscall.c does no more
 *   than validate a caller's arguments and name one of them.
 *
 * A child runs when its parent waits for it.
 *
 *   There is one thread of control upon the bootstrap processor, so a
 *   forked child is created runnable and left standing until `wait` runs it upon
 *   the parent's own thread of control. Everything a program can observe of the
 *   ordering is preserved — a child runs after the fork that made it and before
 *   the wait that collects it — and concurrency is not. It is recorded here and
 *   in docs/design/PROCESS.md, Section 13.2, rather than left to be discovered.
 *
 * Identifiers are numbers and not indices.
 *
 *   A slot in a table is reused the moment its occupant is destroyed; an
 *   identifier never is. A parent therefore records its child by number, and a
 *   parent that outlives the table slot its child once held finds nobody rather
 *   than finding whoever was given that slot next — which is the whole class of
 *   fault that makes a process kill an unrelated one.
 *
 * Concurrency. The claim of a slot in either table is guarded by
 * ProcessTableLock, from sub-task 6.15: the search and the claim must be one
 * atomic act, because each application processor claims a thread slot as it
 * comes online and two that found the same free slot would produce two threads
 * sharing one identifier, one context and one kernel stack pointer. The current
 * thread and the thread to return to are per processor from the same sub-task,
 * and are the reason the tables were single-threaded before it — see the note
 * upon ProcessCurrentThreads.
 *
 * **The lock does not make the allocators safe.** ThreadCreate takes a kernel
 * stack from the arena while holding it, and the arena is unsynchronised;
 * docs/design/CONCURRENCY.md, Section 10, limitation 1, still names it. What
 * makes that sound is that the one path a second processor takes through this
 * file — ThreadAdoptCurrent — allocates nothing.
 *
 * **The break of sub-task 7.3 is guarded by nothing either**, and needs to be by
 * the same lock the rest of the process control block will take. Two threads of
 * one process moving the break at once would each read `break_current`, each map
 * the pages between it and what they asked for, and the second would map frames
 * over the first's — a leak of every frame the first obtained and a heap holding
 * pages it did not put there. There are no userland threads yet, and a user
 * thread's affinity names the bootstrap processor alone, so the case cannot
 * arise; docs/design/CONCURRENCY.md, Section 10, limitation 1, is where it is
 * counted with the rest.
 */

#include <oxys/proc/process.h>
#include <oxys/arch/mm/addrspace.h>
#include <oxys/kernel.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/mm/paging.h>
#include <oxys/mm/pmm.h>
#include <oxys/arch/cpu/gdt.h>
#include <oxys/arch/cpu/tss.h>
#include <oxys/mm/vmm.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/proc/sched.h>
#include <oxys/proc/signal.h>
#include <oxys/terminal/terminal.h>
#include <oxys/arch/cpu/spinlock.h>
#include <oxys/fs/vfs.h>

static Process ProcessTable[PROCESS_CAPACITY];
static Thread ThreadTable[THREAD_CAPACITY];

static uint64_t ProcessNextId = 1U;
static uint64_t ThreadNextId = 1U;
static uint64_t ProcessCreations;
static uint64_t ThreadCreations;
static uint64_t ProcessTerminations;

/*
 * The lock that guards the two tables' slots.
 *
 * **It exists from sub-task 6.15**, which is the sub-task that gives a second
 * processor a reason to claim one. Until then every claim was made by the
 * bootstrap processor: an application processor started by 6.14 was parked and
 * created nothing. Now each one adopts an idle thread as it comes online, and
 * SmpInitialise waits only for a processor's *area* before starting the next —
 * so two processors can be in ThreadAdoptCurrent at once.
 *
 * What must be atomic is the search together with the claim, and not either
 * alone. Two processors that each scanned for a free slot, each found the same
 * one, and each then wrote to it would produce two threads sharing a structure:
 * one identifier, one context, one kernel stack pointer — and the second write
 * would win, leaving the first processor running a thread that describes
 * somebody else's execution. No count would be wrong and nothing would fault
 * until the two switched.
 *
 * **It does not make the allocators safe.** ThreadCreate takes a kernel stack
 * from the arena while holding this lock, and the arena is still unsynchronised;
 * docs/design/CONCURRENCY.md, Section 10, limitation 1, still names it. What
 * makes that sound today is that the one path a second processor takes through
 * here — ThreadAdoptCurrent — allocates nothing, describing a stack that
 * already exists. A user thread created upon an application processor would
 * need the arena's own lock, and that is what a user thread's affinity mask
 * exists to prevent until it has one.
 */
static Spinlock ProcessTableLock = SPINLOCK_INITIALISER("process and thread tables");

/*
 * The thread each processor is running.
 *
 * **This was a single variable until sub-task 6.15, and that was the whole of
 * what made this kernel single-threaded.** A second processor executing kernel
 * code against one `ProcessCurrentThreads[ProcessProcessorIndex()]` would not race for it occasionally: it
 * would overwrite it on every switch, and the loser would find `rsp0` naming
 * another processor's stack at its next entry from privilege level 3. Two
 * programs would then write their trap frames over one another, which is a
 * corruption of the kernel's own state by two programs that never touched each
 * other.
 *
 * It is indexed by the dense processor index of kernel/include/oxys/percpu.h
 * rather than held inside the PerCpu area itself. The area's first three fields
 * are addressed by displacement from `GS` in kernel/arch/x86_64/syscall/syscall_entry.asm, and a
 * pointer added to it would be a fourth thing whose offset the assembly and the
 * C must agree about for no gain — the index is already available in one
 * instruction, and an array subscripted by it is per processor in exactly the
 * same sense.
 *
 * The thread each will *return to* stood beside it until sub-task 8.6, one per
 * processor, and is a field of the started thread since: `return_to`, of
 * <oxys/proc/process.h>, whose comment records why one per processor stopped
 * being enough the day a program could sleep while another ran.
 *
 * No lock guards the array. Each processor writes only its own element, and
 * reads another's only in the report, where a torn read costs a diagnostic.
 */
static Thread *ProcessCurrentThreads[PER_CPU_MAXIMUM];

/*
 * The index of the executing processor, or zero before there is an area to ask.
 *
 * The fallback is not a guess. Until PerCpuInitialise has run there is one
 * processor by construction — no other can have been started, the bring-up of
 * sub-task 6.14 being what starts them — and that processor is the bootstrap
 * processor, which is index 0. Asking through GS before the base is established
 * would read address sixteen; the guard is the same one KernelWriteString makes,
 * and for the same reason.
 */
static uint32_t ProcessProcessorIndex(void)
{
    const uint32_t index = PerCpuIsEstablished() ? PerCpuIndex() : 0U;

    return (index < PER_CPU_MAXIMUM) ? index : 0U;
}

/*
 * The scheduling fields every newly made thread starts with.
 *
 * It is one function and not four assignments repeated, because a creation path
 * added later that forgot one of them would produce a thread linked into a queue
 * it is not on, or one with an affinity of zero — which is a thread no processor
 * is permitted to run and which would therefore be admitted, counted, and never
 * scheduled, with nothing anywhere saying so.
 *
 * The default mask is the argument rather than a constant here, because the two
 * kinds of thread this kernel makes want different ones and the difference is
 * the subject of docs/design/SCHEDULER.md, Section 4.
 */
static void ThreadInitialiseScheduling(Thread *thread, uint64_t affinity)
{
    thread->queue_next = NULL;
    thread->affinity = affinity;
    thread->processor = 0U;
    thread->queued = false;
    thread->slices = 0U;
    thread->preemptions = 0U;

    /* The fields of sub-task 8.6, cleared here for the same reason: a thread
     * that began with a stale `return_to` would end by switching to a thread
     * that had long since been released. */
    thread->return_to = NULL;
    thread->wait_channel = NULL;
    thread->critical_depth = 0U;
    thread->interrupts_were_enabled = false;
    thread->adopted = false;
}

/* ------------------------------------------------------------------ helpers */

static void ProcessCopyName(char *destination, const char *source)
{
    size_t index = 0U;

    if (source != NULL)
    {
        while ((index < PROCESS_NAME_MAXIMUM) && (source[index] != '\0'))
        {
            destination[index] = source[index];
            ++index;
        }
    }

    destination[index] = '\0';
}

const char *ProcessStateName(ProcessState state)
{
    switch (state)
    {
    case PROCESS_CREATED:
        return "created";
    case PROCESS_READY:
        return "ready";
    case PROCESS_RUNNING:
        return "running";
    case PROCESS_BLOCKED:
        return "blocked";
    case PROCESS_STOPPED:
        return "stopped";
    case PROCESS_EXITED:
        return "exited";
    case PROCESS_UNUSED:
    default:
        return "unused";
    }
}

const char *ThreadStateName(ThreadState state)
{
    switch (state)
    {
    case THREAD_CREATED:
        return "created";
    case THREAD_READY:
        return "ready";
    case THREAD_RUNNING:
        return "running";
    case THREAD_BLOCKED:
        return "blocked";
    case THREAD_EXITED:
        return "exited";
    case THREAD_UNUSED:
    default:
        return "unused";
    }
}

void ProcessInitialise(void)
{
    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        ProcessTable[index].used = false;
        ProcessTable[index].state = PROCESS_UNUSED;
        ProcessTable[index].thread_count = 0U;
    }

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        ThreadTable[index].used = false;
        ThreadTable[index].state = THREAD_UNUSED;
    }

    ProcessCurrentThreads[ProcessProcessorIndex()] = NULL;
}

/* ---------------------------------------------------------------- processes */

/*
 * Takes a slot and gives it an address space, either an empty one or a clone of
 * another.
 *
 * The two differ in one call and in nothing else, which is why they are one
 * routine: a fork that built an empty space and then replaced it would have to
 * destroy a hierarchy it had just made, and a moment in which the process holds
 * a space that is neither the one it began with nor the one it is to have is a
 * moment in which a failure has nothing correct to fall back to.
 */
static Process *ProcessAllocate(const char *name, const Process *parent,
                                const AddressSpace *clone_of)
{
    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        Process *const process = &ProcessTable[index];

        if (process->used)
        {
            continue;
        }

        if ((clone_of != NULL) ? !AddressSpaceClone(&process->space, clone_of)
                               : !AddressSpaceCreate(&process->space))
        {
            /*
             * The slot is left unoccupied rather than half filled. A process
             * without an address space is a process that cannot be destroyed
             * correctly — the destruction would free a hierarchy that was never
             * made — and there is no state in which it would be useful.
             */
            return NULL;
        }

        process->id = ProcessNextId;
        ++ProcessNextId;

        /*
         * The parent by number and not by pointer. A parent may be destroyed
         * while its child lives, and a pointer to a slot since given to somebody
         * else names the wrong process convincingly.
         */
        process->parent_id = (parent != NULL) ? parent->id : 0U;
        process->state = PROCESS_CREATED;
        ProcessCopyName(process->name, name);

        process->image_lowest = 0U;
        process->image_highest = 0U;
        process->image_entry = 0U;
        process->mapped_pages = 0U;
        process->user_stack_top = 0U;
        process->user_stack_pages = 0U;
        process->thread_count = 0U;
        process->exit_status = 0;

        /* The job-control fields of sub-task 8.7. A process leads a group of
         * its own identifier unless it is a child, which begins in its
         * parent's — IEEE Std 1003.1-2017, `fork()`. */
        process->group = (parent != NULL) ? parent->group : process->id;
        process->pending = 0U;
        process->restorer = 0U;
        process->wait_status = 0U;
        process->stop_signal = 0U;
        process->termination_signal = 0U;
        process->stop_reported = false;
        process->stop_channel = 0U;

        for (size_t signal = 0U; signal <= SYSCALL_SIGNAL_MAXIMUM; ++signal)
        {
            process->handlers[signal] = SYSCALL_SIGNAL_DEFAULT;
        }

        process->used = true;

        for (size_t slot = 0U; slot < PROCESS_THREAD_MAXIMUM; ++slot)
        {
            process->threads[slot] = NULL;
        }

        /*
         * The descriptor table of sub-task 7.6, emptied explicitly.
         *
         * `ProcessCloseDescriptors` is not used here, because this slot may hold
         * whatever its last occupant left in it and closing that would close
         * descriptors belonging to a process that has already released them.
         * The distinction matters exactly once — at this line — and it is why
         * the free value is not zero: a table that had been cleared to zero
         * would name filesystem descriptor 0 in every slot.
         *
         * **A child of `fork` reaches this line too**, and therefore inherits
         * nothing. POSIX has a child inherit every descriptor its parent held;
         * this kernel does not, because inheriting one means two processes
         * sharing one open file and one file position, and the filesystem layer
         * has no reference count upon an open file to make that safe.
         * docs/design/PROCESS.md, Section 14.
         */
        for (size_t slot = 0U; slot < PROCESS_DESCRIPTOR_CAPACITY; ++slot)
        {
            process->descriptors[slot] = PROCESS_DESCRIPTOR_FREE;
        }

        /* Every process begins at the root, of sub-task 8.3. A child of `fork`
         * is given its parent's below, after this; a program started by the
         * kernel begins here. */
        process->working_directory[0] = '/';
        process->working_directory[1] = '\0';

        ++ProcessCreations;

        return process;
    }

    return NULL;
}

Process *ProcessCreate(const char *name, const Process *parent)
{
    return ProcessAllocate(name, parent, NULL);
}

void ProcessDestroy(Process *process)
{
    if ((process == NULL) || !process->used)
    {
        return;
    }

    /*
     * The threads go first, and downward through the array, because each
     * destruction removes itself from it. Walking upward while the array is
     * being compacted beneath is the classic way to leave the last entry
     * untouched.
     */
    while (process->thread_count > 0U)
    {
        ThreadDestroy(process->threads[process->thread_count - 1U]);
    }

    /*
     * The descriptors go with it, of sub-task 7.6. A process that ends while
     * holding one leaves an entry in the filesystem layer's table that nothing
     * will ever close, and that table is the machine's and holds
     * VFS_FILE_CAPACITY entries — so a program that ended with a file open would
     * cost the machine a descriptor permanently.
     */
    ProcessCloseDescriptors(process);

    AddressSpaceDestroy(&process->space);

    process->used = false;
    process->state = PROCESS_UNUSED;
    process->id = 0U;
}

Process *ProcessById(uint64_t id)
{
    if (id == 0U)
    {
        return NULL;
    }

    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        if (ProcessTable[index].used && (ProcessTable[index].id == id))
        {
            return &ProcessTable[index];
        }
    }

    return NULL;
}

Process *ProcessAt(size_t index)
{
    return (index < PROCESS_CAPACITY) ? &ProcessTable[index] : NULL;
}

size_t ProcessCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        if (ProcessTable[index].used)
        {
            ++count;
        }
    }

    return count;
}

/* ------------------------------------------------------------------ threads */

/*
 * Reserves a range of the arena and makes its lowest page a guard.
 *
 * The guard is left **mapped and read-only** rather than unmapped, and that is a
 * decision rather than an oversight. `KernelPagesFree` panics upon an unmapped
 * page within a range it is releasing — deliberately, an unmapped page there
 * meaning the caller has lost track of what it owns — so a guard that was
 * unmapped could not be given back without first putting a frame under it. A
 * read-only guard catches what an overflow actually does: a push is a write, and
 * a write to a read-only page faults.
 *
 * Returns the base of the whole reservation, guard included, and reports the top
 * of the usable stack through `top`.
 */
static void *ThreadAllocateStack(uint64_t *top)
{
    const size_t pages = THREAD_KERNEL_STACK_PAGES + THREAD_GUARD_PAGES;
    void *const base = KernelPagesAllocate(pages);
    VirtualAddress guard;
    PhysicalAddress frame;

    if (base == NULL)
    {
        return NULL;
    }

    guard = (VirtualAddress)(uintptr_t)base;
    frame = PagingTranslate(guard);

    if (frame != 0U)
    {
        /* The same frame, without the writable flag. The mapping stays, so the
         * range may be released as it was taken. */
        PagingMapKernelPage(guard, frame, 0U);
    }

    /*
     * The top is one past the last byte, which is where a stack pointer begins:
     * the first push decrements it and writes within the range. A top set to the
     * last byte would have the first push write one byte beyond it.
     */
    *top = guard + ((uint64_t)pages * PAGE_SIZE);

    return base;
}

/* Puts a guard page back the way it was found, so that the range may be freed. */
static void ThreadReleaseStack(void *base)
{
    const VirtualAddress guard = (VirtualAddress)(uintptr_t)base;
    const PhysicalAddress frame = PagingTranslate(guard);

    if (base == NULL)
    {
        return;
    }

    if (frame != 0U)
    {
        PagingMapKernelPage(guard, frame, PAGE_ENTRY_WRITABLE);
    }

    KernelPagesFree(base, THREAD_KERNEL_STACK_PAGES + THREAD_GUARD_PAGES);
}

Thread *ThreadCreate(Process *owner, uint64_t entry, uint64_t user_stack)
{
    if ((owner == NULL) || !owner->used ||
        (owner->thread_count >= PROCESS_THREAD_MAXIMUM))
    {
        return NULL;
    }

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        Thread *const thread = &ThreadTable[index];
        uint64_t top = 0U;
        void *stack;

        if (thread->used)
        {
            continue;
        }

        stack = ThreadAllocateStack(&top);

        if (stack == NULL)
        {
            return NULL;
        }

        thread->id = ThreadNextId;
        ++ThreadNextId;

        thread->state = THREAD_CREATED;
        thread->owner = owner;
        thread->kernel_stack_base = stack;
        thread->kernel_stack_top = top;
        thread->entry = entry;
        thread->user_stack = user_stack;

        /*
         * The context begins with the stack pointer at the top and every
         * preserved register zero. It is not yet a context that could be
         * switched to: there is no return address upon the stack for a switch to
         * return through, and putting one there is sub-task 6.10's, which is
         * what will know what the thread should return *into*.
         */
        thread->context.rbx = 0U;
        thread->context.rbp = 0U;
        thread->context.r12 = 0U;
        thread->context.r13 = 0U;
        thread->context.r14 = 0U;
        thread->context.r15 = 0U;
        thread->context.rsp = top;
        thread->owns_stack = true;
        ThreadInitialiseScheduling(thread, SCHED_AFFINITY_BOOTSTRAP);

        /* A thread created here begins at an entry point, not part way through a
         * program, so there is no saved user context to restore. Sub-task 6.11's
         * ProcessFork is what fills these in. */
        thread->resumes_from_fork = false;

        thread->used = true;

        owner->threads[owner->thread_count] = thread;
        ++owner->thread_count;
        ++ThreadCreations;

        return thread;
    }

    return NULL;
}

void ThreadDestroy(Thread *thread)
{
    Process *owner;

    if ((thread == NULL) || !thread->used)
    {
        return;
    }

    owner = thread->owner;

    if (owner != NULL)
    {
        /* Removed from its owner by compaction, the order of a process's threads
         * meaning nothing. */
        for (size_t index = 0U; index < owner->thread_count; ++index)
        {
            if (owner->threads[index] != thread)
            {
                continue;
            }

            owner->threads[index] = owner->threads[owner->thread_count - 1U];
            owner->threads[owner->thread_count - 1U] = NULL;
            --owner->thread_count;
            break;
        }
    }

    /*
     * A thread that was current is current no longer. Leaving the pointer
     * standing would leave `rsp0` naming a stack that has been given back to the
     * arena, and the next entry from privilege level 3 would arrive upon memory
     * belonging to somebody else.
     */
    if (ProcessCurrentThreads[ProcessProcessorIndex()] == thread)
    {
        ProcessCurrentThreads[ProcessProcessorIndex()] = NULL;
    }

    /*
     * And a thread that was the one to return to is that no longer, for the same
     * reason and with a worse consequence.
     *
     * `return_to` is what ThreadTerminateCurrent switches to when a program
     * ends. Left naming a destroyed thread, that switch would load a stack
     * pointer out of a context structure belonging to a released slot and
     * resume execution upon a kernel stack the arena has given to somebody else
     * — which is not a fault but a machine that continues, wrongly, with no
     * indication that anything happened. The pointer is per thread since
     * sub-task 8.6, so every thread that named this one is walked.
     */
    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        if (ThreadTable[index].used && (ThreadTable[index].return_to == thread))
        {
            ThreadTable[index].return_to = NULL;
        }
    }

    /*
     * A thread still upon a run queue is taken off it, of sub-task 8.6. A child
     * of `fork` is admitted at the fork and may be collected before it has run
     * — by a caller with no thread to sleep upon, which the fork self-test is —
     * and a queue link left naming a released slot would be dequeued at the
     * next reschedule and switched to.
     */
    if (thread->queued)
    {
        (void)SchedulerWithdraw(thread);
    }

    /*
     * Only a stack this thread took. The thread describing the kernel's own
     * execution runs upon the boot stack, which the linker established and which
     * the arena never gave out; handing it to KernelPagesFree would be releasing
     * an address the arena does not own.
     */
    if (thread->owns_stack)
    {
        ThreadReleaseStack(thread->kernel_stack_base);
    }

    thread->used = false;
    thread->state = THREAD_UNUSED;
    thread->owner = NULL;
    thread->kernel_stack_base = NULL;
    thread->kernel_stack_top = 0U;
    thread->owns_stack = false;
    ThreadInitialiseScheduling(thread, 0U);
    thread->id = 0U;
}

Thread *ThreadById(uint64_t id)
{
    if (id == 0U)
    {
        return NULL;
    }

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        if (ThreadTable[index].used && (ThreadTable[index].id == id))
        {
            return &ThreadTable[index];
        }
    }

    return NULL;
}

Thread *ThreadAt(size_t index)
{
    return (index < THREAD_CAPACITY) ? &ThreadTable[index] : NULL;
}

size_t ThreadCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        if (ThreadTable[index].used)
        {
            ++count;
        }
    }

    return count;
}

void ThreadSetCurrent(Thread *thread)
{
    ProcessCurrentThreads[ProcessProcessorIndex()] = thread;

    if (thread == NULL)
    {
        return;
    }

    /*
     * The task state segment is told where this thread's kernel stack is.
     *
     * `rsp0` is what the processor loads upon a transfer from privilege level 3,
     * so a thread entered while it still named another thread's stack would take
     * its first interrupt onto a stack somebody else is using — two threads
     * writing frames over one another, which is a corruption of the kernel's own
     * state by two programs that never touched each other.
     *
     * `TssSetKernelStack` has existed and been uncalled since sub-task 6.1 for
     * exactly this moment.
     */
    TssSetKernelStack(thread->kernel_stack_top);

    /*
     * And the *other* record of the same stack.
     *
     * SYSCALL performs no stack switch, so the entry path of sub-task 6.7 cannot
     * read `rsp0`: it reads a field of the block GS names instead. Two variables
     * therefore describe one stack, and until sub-task 6.11 only one of them
     * followed the current thread — the other still held the stack the task
     * state segment was initialised with, because SyscallInitialise wrote it once
     * and nothing wrote it again.
     *
     * With one program at a time that was invisible: the one stack nobody else
     * was using served. It stops being invisible the moment a program's child
     * makes a system call while the parent is inside one, which is exactly what
     * `wait` arranges — both entries would build their frames at the same
     * addresses, and the parent would return through the child's registers.
     */
    SyscallSetKernelStack(thread->kernel_stack_top);
}

Thread *ThreadCurrent(void)
{
    return ProcessCurrentThreads[ProcessProcessorIndex()];
}

/*
 * The thread a numbered processor is running.
 *
 * It exists for the scheduler's report and its self-test, which are the only
 * things that ask about a processor other than the one asking. The value is a
 * snapshot of something that processor is changing, and the caller is expected
 * to know that: it is a diagnostic, not a handle.
 */
Thread *ThreadCurrentOn(uint32_t processor)
{
    if (processor >= PER_CPU_MAXIMUM)
    {
        return NULL;
    }

    return ProcessCurrentThreads[processor];
}

/* --------------------------------------------------- what a process holds */

/* ------------------------------------------------------- sub-task 6.10 */

/*
 * The assembly of kernel/arch/x86_64/proc/switch.asm addresses ThreadContext by number and
 * cannot see this structure. A field reordered here without the assembly would
 * have a switch restore the stack pointer from a general register — which is a
 * jump to an address that was never an address.
 */
_Static_assert(offsetof(ThreadContext, rbx) == 0U, "The switch saves RBX at offset 0.");
_Static_assert(offsetof(ThreadContext, rsp) == 48U,
               "The switch saves the stack pointer at offset 48.");
_Static_assert(sizeof(ThreadContext) == 56U, "A context is seven quadwords.");

/* Defined in kernel/arch/x86_64/proc/switch.asm. */
extern void ThreadSwitchContext(ThreadContext *from, ThreadContext *to);
extern void ThreadEnterUser(uint64_t entry, uint64_t user_stack, uint64_t code_selector,
                            uint64_t stack_selector);
extern void ThreadResumeUser(const SyscallFrame *frame, uint64_t code_selector,
                             uint64_t stack_selector);
extern void ThreadTrampoline(void);

/*
 * ThreadResumeUser addresses SyscallFrame by number and cannot see this
 * structure either. The two fields asserted are the two it reads that are not
 * merely restored: RCX is the address the child resumes at and R11 the flags it
 * resumes with, so a field displaced here without the assembly would return a
 * program to whatever quadword had taken its place.
 */
_Static_assert(offsetof(SyscallFrame, rcx) == (12U * 8U),
               "The resume takes the instruction pointer from RCX at offset 96.");
_Static_assert(offsetof(SyscallFrame, r11) == (4U * 8U),
               "The resume takes RFLAGS from R11 at offset 32.");

/*
 * Prepares a thread's kernel stack so that switching to it lands in the
 * trampoline.
 *
 * ThreadSwitchContext restores six registers and then executes RET, so the
 * incoming stack must look exactly as it would have done had that thread once
 * called the switch: six saved registers, and above them the address to return
 * to. There is no such history for a thread that has never run, so the history
 * is fabricated — and the address returned to is the trampoline, which is where
 * a thread that has never run begins.
 *
 * The alignment is not incidental. The System V convention requires the stack
 * pointer to be sixteen-byte aligned at a call instruction, which means it is
 * eight modulo sixteen immediately *after* the call has pushed a return address.
 * The trampoline is entered by a return rather than by a call, so its stack is
 * aligned where a called function's would be misaligned, and it calls onward
 * from there.
 */
static uint64_t ThreadPrepareFrame(uint64_t stack_top, uint64_t resume_at)
{
    uint64_t *stack = (uint64_t *)(uintptr_t)stack_top;

    /*
     * Only a return address, and one quadword of padding above it.
     *
     * The switch keeps the six preserved registers **in the context structure**
     * and not upon the stack — it saves them with stores and restores them with
     * loads — so the only thing the incoming stack must hold is the address its
     * RET will take. Six zeroes were written here first, in the belief that the
     * switch popped them, and the RET then took the lowest of them: a return to
     * address zero, which is a page fault at an instruction pointer of nothing.
     *
     * The padding is the alignment. A function entered by an ordinary call finds
     * the stack pointer eight modulo sixteen, the call having pushed eight bytes
     * onto a sixteen-byte boundary. Placing the return address sixteen bytes
     * below the top reproduces that exactly; placing it eight below would enter
     * every thread with the stack aligned the other way, which the compiler is
     * entitled to assume it is not.
     */
    --stack;
    *stack = 0U;
    --stack;
    *stack = resume_at;

    return (uint64_t)(uintptr_t)stack;
}

static void ThreadPrepareStart(Thread *thread)
{
    thread->context.rsp = ThreadPrepareFrame(thread->kernel_stack_top,
                                             (uint64_t)(uintptr_t)&ThreadTrampoline);
}

/*
 * Creates a thread that runs kernel code at privilege level 0.
 *
 * It has no process and therefore no address space of its own: it runs in the
 * kernel's. What it has is a stack, which is the whole reason it is a thread —
 * something must be switched away from and back to, and both halves need a stack
 * to hold the history.
 *
 * Its prepared frame returns directly to the routine given rather than to the
 * trampoline: a kernel thread has no descent to privilege level 3 to make and
 * nothing for the trampoline to do for it.
 */
Thread *ThreadCreateKernel(void (*entry)(void))
{
    if (entry == NULL)
    {
        return NULL;
    }

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        Thread *const thread = &ThreadTable[index];
        uint64_t top = 0U;
        void *stack;

        if (thread->used)
        {
            continue;
        }

        stack = ThreadAllocateStack(&top);

        if (stack == NULL)
        {
            return NULL;
        }

        thread->id = ThreadNextId;
        ++ThreadNextId;
        thread->state = THREAD_CREATED;
        thread->owner = NULL;
        thread->kernel_stack_base = stack;
        thread->kernel_stack_top = top;
        thread->entry = (uint64_t)(uintptr_t)entry;
        thread->user_stack = 0U;
        thread->owns_stack = true;
        ThreadInitialiseScheduling(thread, SCHED_AFFINITY_ANY);
        thread->resumes_from_fork = false;
        thread->used = true;

        thread->context.rsp = ThreadPrepareFrame(top, (uint64_t)(uintptr_t)entry);
        ++ThreadCreations;

        return thread;
    }

    return NULL;
}

/*
 * Where a kernel thread started by the scheduler begins.
 *
 * It exists to close the critical section it inherited. The scheduler switches
 * threads from inside a masked region — it masks interrupts, chooses, and
 * switches — and the counted disable of <oxys/arch/cpu/percpu.h> belongs to the
 * processor rather than to the thread. A resumed thread carries on inside its
 * own push and executes the matching pop; a thread that has never run has no
 * such pop, and would run with interrupts masked for ever, taking no timer tick
 * and never being pre-empted. That is a machine that hangs the first time a
 * thread is scheduled, with no fault and nothing in the log, and it is why this
 * function is not merely a convenience.
 *
 * The entry is read back from the thread rather than passed, because a prepared
 * frame carries a return address and no arguments. The conversion is the same
 * one ThreadCreateKernel made in the other direction, and the two are the only
 * places a function address becomes an integer in this file.
 */
static void ThreadScheduledEntry(void)
{
    Thread *const self = ProcessCurrentThreads[ProcessProcessorIndex()];

    PerCpuResetInterruptState();

    if ((self != NULL) && (self->entry != 0U))
    {
        void (*const entry)(void) = (void (*)(void))(uintptr_t)self->entry;

        entry();
    }

    /*
     * A kernel thread that returns has nowhere to go: nothing called it, so
     * there is no caller to return to, and this kernel has no reaper. It stops
     * rather than falling off the prepared frame into whatever lies beneath it.
     * docs/design/SCHEDULER.md, Section 10, limitation 4, records what that
     * costs.
     */
    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

/*
 * A kernel thread made to be handed to the scheduler.
 *
 * It differs from ThreadCreateKernel in one respect: the prepared frame enters
 * the trampoline above rather than the entry point directly. ThreadCreateKernel
 * is left as it was because its other caller — the context-switch self-test of
 * sub-task 6.10 — switches to its thread by hand, with interrupts enabled and no
 * critical section open, and has no inherited state to close.
 */
Thread *ThreadCreateScheduled(void (*entry)(void))
{
    Thread *const thread = ThreadCreateKernel(entry);

    if (thread == NULL)
    {
        return NULL;
    }

    thread->context.rsp =
        ThreadPrepareFrame(thread->kernel_stack_top,
                           (uint64_t)(uintptr_t)&ThreadScheduledEntry);

    return thread;
}
Thread *ThreadAdoptCurrent(const char *name)
{
    Thread *thread = NULL;

    /*
     * The search and the claim, under one acquisition.
     *
     * This is the one path in this file that two processors take at once — each
     * application processor adopts its idle thread as it comes online — and
     * splitting the two would let both find the same free slot. See the note
     * upon ProcessTableLock.
     */
    SpinlockAcquire(&ProcessTableLock);

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        if (!ThreadTable[index].used)
        {
            thread = &ThreadTable[index];
            thread->used = true;
            thread->id = ThreadNextId;
            ++ThreadNextId;
            ++ThreadCreations;
            break;
        }
    }

    SpinlockRelease(&ProcessTableLock);

    if (thread == NULL)
    {
        return NULL;
    }

    (void)name;

    /*
     * The thread that is already running, described.
     *
     * It owns no stack: it runs upon the boot stack, which was established by
     * the linker and is not the arena's to give back. That is what `owns_stack`
     * records, and destroying this thread must not free what it did not take.
     *
     * Its context is left as it stands. Nothing reads it until something
     * switches *away* from this thread, and that is the moment the switch fills
     * it in.
     */
    thread->state = THREAD_RUNNING;
    thread->owner = NULL;
    thread->kernel_stack_base = NULL;
    thread->kernel_stack_top = TssKernelStack();
    thread->entry = 0U;
    thread->user_stack = 0U;
    thread->owns_stack = false;
    ThreadInitialiseScheduling(thread, SCHED_AFFINITY_ANY);
    thread->adopted = true;

    ProcessCurrentThreads[ProcessProcessorIndex()] = thread;

    return thread;
}

void ThreadSwitchTo(Thread *from, Thread *to)
{
    if ((from == NULL) || (to == NULL) || (from == to))
    {
        return;
    }

    /*
     * The address space is changed before the stack is.
     *
     * Both are safe to change in either order — the kernel's higher half is
     * mapped identically in every space, so the stack this function is running
     * upon remains addressable across a change of CR3 — but doing the space
     * first means that when the switch returns into the incoming thread, it is
     * already in the space that thread expects. A switch that changed CR3
     * afterwards would have the incoming thread execute its first instructions
     * in the outgoing thread's space.
     */
    if (to->owner != NULL)
    {
        AddressSpaceSwitch(&to->owner->space);
    }
    else
    {
        AddressSpaceSwitch(AddressSpaceKernel());
    }

    from->state = (from->state == THREAD_RUNNING) ? THREAD_READY : from->state;
    to->state = THREAD_RUNNING;

    /*
     * The one piece of state a context does not carry.
     *
     * GS.base holds the per-processor block within the kernel and the program's
     * own value outside it, and the two are exchanged by SWAPGS at each
     * boundary — so which of them GS.base holds depends upon *how the kernel was
     * entered*, and not upon which thread is running. A thread entered by a
     * system call is in the kernel with the block in GS.base; a thread entered
     * by an exception is in the kernel with the program's value there, the
     * interrupt path performing no exchange.
     *
     * A switch cannot tell those apart, and it must not have to: the thread it
     * resumes may return through the system-call path, whose closing SWAPGS
     * assumes the block is in GS.base and would otherwise hand the block to
     * privilege level 3 — where the *next* SYSCALL would exchange it away and
     * the entry path would look for its kernel stack through whatever the
     * program had left in the register.
     *
     * The register is therefore written rather than exchanged, at both
     * boundaries: here, where the kernel resumes, and in ThreadTrampolineEntry
     * below, where it departs. The state then follows from the transition being
     * made instead of from the history of the thread making it.
     */
    SyscallEstablishKernelGsBase();

    /*
     * The counted interrupt-disable goes with the thread, since sub-task 8.6.
     *
     * It belongs to the processor, and a thread that sleeps from inside its own
     * critical section — every sleeper does; the reschedule is entered under
     * one — is resumed by whichever thread pushes next. Left with the
     * processor, the sleeper's pop would restore what that other thread
     * recorded, and the idle thread records that interrupts were enabled: a
     * program woken from idle would leave its system call with interrupts
     * enabled inside the kernel, where the exit path's SWAPGS and SYSRET
     * assume they are not. Saved into the outgoing thread and loaded from the
     * incoming one, each pop answers its own push, whoever ran between.
     */
    PerCpuSaveInterruptState(&from->critical_depth, &from->interrupts_were_enabled);
    PerCpuLoadInterruptState(to->critical_depth, to->interrupts_were_enabled);

    /* rsp0 follows the incoming thread, so that its next entry from privilege
     * level 3 arrives upon its own stack. */
    ThreadSetCurrent(to);

    ThreadSwitchContext(&from->context, &to->context);
}

/*
 * Where a thread that has never run begins, called by the trampoline.
 *
 * It is `void` and does not return, because there is nothing to return to: the
 * stack beneath it is the prepared frame and holds no history. A thread leaves
 * this function by entering privilege level 3 and never comes back to it — what
 * comes back is the kernel, upon this thread's kernel stack, through the system
 * call path or an exception.
 */
void ThreadTrampolineEntry(void)
{
    Thread *const thread = ProcessCurrentThreads[ProcessProcessorIndex()];

    if ((thread == NULL) || (thread->entry == 0U))
    {
        KernelPanic("A thread was started with nowhere to begin.");
    }

    /* The kernel is about to be left, so the segment bases are put as a program
     * requires them; ThreadSwitchTo above says why they are written and not
     * exchanged. */
    SyscallEstablishUserGsBase();

    /*
     * A thread made by `fork` continues a program rather than beginning one.
     *
     * It resumes at the instruction after its parent's SYSCALL, upon the stack
     * its parent was using — which is its own, the address space having been
     * cloned — and with its parent's whole register set save RAX, which is zero
     * because that is how a child tells itself apart from its parent.
     *
     * The other path clears every register instead, for the reason
     * docs/design/PROCESS.md, Section 10, gives: whatever stands in a register at
     * that moment is a kernel address as often as not. That reasoning does not
     * reach a forked child, every value it inherits being one its parent already
     * had at privilege level 3.
     */
    if (thread->resumes_from_fork)
    {
        ThreadResumeUser(&thread->resume, (uint64_t)GDT_USER_CODE_SELECTOR | 3U,
                         (uint64_t)GDT_USER_DATA_SELECTOR | 3U);
    }

    ThreadEnterUser(thread->entry, thread->user_stack,
                    (uint64_t)GDT_USER_CODE_SELECTOR | 3U,
                    (uint64_t)GDT_USER_DATA_SELECTOR | 3U);
}

bool ThreadStart(Thread *thread)
{
    Thread *const caller = ProcessCurrentThreads[ProcessProcessorIndex()];

    if ((thread == NULL) || !thread->used || (caller == NULL) || (thread == caller))
    {
        return false;
    }

    if ((thread->entry == 0U) || (thread->user_stack == 0U))
    {
        return false;
    }

    ThreadPrepareStart(thread);

    /*
     * The thread that starts another is the one it will be returned to when the
     * program ends, and it is recorded upon the started thread.
     *
     * It was one pointer per processor from sub-task 6.10 to 8.5, saved and put
     * back around the switch so that a program could start another — a parent
     * that called `wait` started its child from within its own system call,
     * and the chain of them lived upon the kernel stacks of the calls. That
     * was the shape of a stack of callers, and it stopped being the shape of
     * anything at 8.6: a child is admitted to the scheduler at the fork, the
     * shell sleeps in `wait` while its children run, and a child that ended
     * while the processor's one pointer still named the shell's starter would
     * have returned to the boot flow with the shell asleep for ever. Upon the
     * thread, the pointer names the caller of this thread and no other.
     */
    thread->return_to = caller;

    ThreadSwitchTo(caller, thread);

    /* Reached when the started thread — or the kernel acting for it — switches
     * back. */
    ThreadSetCurrent(caller);

    return true;
}

/* Whether no live process but `except` belongs to a group, of sub-task 8.7. */
static bool ProcessGroupIsEmptyBut(uint64_t group, const Process *except)
{
    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        const Process *const candidate = &ProcessTable[index];

        if (candidate->used && (candidate != except) && (candidate->group == group) &&
            (candidate->state != PROCESS_EXITED))
        {
            return false;
        }
    }

    return true;
}

bool ThreadTerminateCurrent(int64_t status)
{
    Thread *const thread = ProcessCurrentThreads[ProcessProcessorIndex()];
    Thread *back;
    Process *parent = NULL;

    if (thread == NULL)
    {
        return false;
    }

    back = thread->return_to;

    /* A thread started by a call returns to its caller; one the scheduler
     * runs has no caller and gives the processor to the scheduler below. A
     * caller that is this very thread is a corruption and not a case. */
    if (thread == back)
    {
        return false;
    }

    if ((back == NULL) && !SchedulerIsRunning())
    {
        return false;
    }

    thread->state = THREAD_EXITED;

    if (thread->owner != NULL)
    {
        Process *const owner = thread->owner;

        owner->state = PROCESS_EXITED;
        owner->exit_status = status;

        /*
         * What `wait` reports, in the encoding of sub-task 8.7: the code a
         * program gave `exit`, or the signal that ended it — the one delivery
         * recorded in `termination_signal`, or for a fault the signal the
         * vector maps to. `exit_status` keeps the quadword, which the
         * self-tests read.
         */
        if (owner->termination_signal != 0U)
        {
            owner->wait_status = SYSCALL_STATUS_MAKE(SYSCALL_STATUS_KIND_SIGNALLED,
                                                     owner->termination_signal);
        }
        else if (status < 0)
        {
            owner->wait_status = SYSCALL_STATUS_MAKE(
                SYSCALL_STATUS_KIND_SIGNALLED, SignalFromVector((uint64_t)(-status)));
        }
        else
        {
            owner->wait_status = SYSCALL_STATUS_MAKE(SYSCALL_STATUS_KIND_EXITED, status);
        }

        parent = ProcessById(owner->parent_id);

        /*
         * The descriptors are released now, at the ending, and not when the
         * parent collects the process — which until sub-task 8.7 was the same
         * moment, the collecting `wait` being the only thing that ever ran
         * after a child. It stopped being the same moment the first time a
         * pipeline ran in the background: the shell collects a background job
         * at its next prompt, so `cat /bin/sh | wc -c &` left `cat` ended but
         * uncollected with the pipe's write end still open, `wc` waiting for
         * an end of file that only the close could give, and the shell waiting
         * for a keypress before it would collect anything. A process that has
         * ended holds nothing; what it held is given back here.
         */
        ProcessCloseDescriptors(owner);

        /*
         * The terminal's foreground group is cleared where this was its last
         * member, of sub-task 8.7: a foreground group naming nobody would
         * stop every later reader with SIGTTIN, the shell that starts next
         * included, for a job that no longer exists.
         */
        if ((TerminalForegroundGroup() == owner->group) && ProcessGroupIsEmptyBut(owner->group, owner))
        {
            TerminalSetForegroundGroup(0U);
        }
    }

    ++ProcessTerminations;

    /*
     * The switch does not return.
     *
     * This function is running upon the terminating thread's own kernel stack,
     * entered from privilege level 3 through a system call or an exception, and
     * that stack is about to belong to nobody. Nothing after the switch would
     * execute even if it were written, and the thread's context is saved into a
     * structure that will shortly be released — which is harmless precisely
     * because nothing will ever switch back to it.
     */
    if (back != NULL)
    {
        ThreadSwitchTo(thread, back);

        return true;
    }

    /*
     * A thread the scheduler ran, of sub-task 8.6: a child of `fork`. Its
     * parent may be asleep in `wait` upon its own process, and is woken before
     * the processor is given up so that the reschedule finds it upon the queue;
     * woken after would be woken by nobody, this thread being gone. A parent
     * that is not asleep finds the ended child at its next `wait`, and a
     * parent that has itself ended leaves the child in the table for the
     * `init` of Phase 9 to collect.
     */
    if (parent != NULL)
    {
        (void)SchedulerWake(parent);
        (void)SignalSend(parent, SYSCALL_SIGCHLD);
    }

    SchedulerExitCurrent();
}

uint64_t ProcessTerminationCount(void)
{
    return ProcessTerminations;
}

void ProcessRecordImage(Process *process, const ElfImage *image)
{
    if ((process == NULL) || !process->used || (image == NULL))
    {
        return;
    }

    process->image_lowest = image->lowest;
    process->image_highest = image->highest;
    process->image_entry = image->entry;
    process->mapped_pages += image->pages;

    ProcessEstablishBreak(process);
}

/*
 * The frames a user stack was just built from, in ascending order of address.
 *
 * The frames are kept rather than translated for, because the alternative is an
 * address-space walk the paging layer does not offer for a space that is not the
 * active one — and the space a stack is being filled for very often is not:
 * `ProcessCreateUserStack` is called from the boot sequence with the kernel's
 * space active. Keeping the sixteen physical addresses the mapping loop already
 * held is the same technique the ELF loader uses to write a segment it has not
 * mapped yet, and it needs nothing new.
 */
typedef struct ProcessStackFrames
{
    PhysicalAddress frame[PROCESS_USER_STACK_PAGES];
    uint64_t base;
} ProcessStackFrames;

/*
 * Writes bytes into a stack that has been built but not entered, through the
 * direct map of the frames above.
 *
 * The bounds check is not paranoia: every caller below computes an address from
 * a layout it is building downward, and a layout that ran past the bottom of the
 * stack would otherwise index the array out of range — which is the one way this
 * function could corrupt memory that has nothing to do with the process.
 */
static bool ProcessWriteUserStack(const ProcessStackFrames *frames, uint64_t address,
                                  const void *source, uint64_t length)
{
    const uint8_t *const bytes = (const uint8_t *)source;

    for (uint64_t offset = 0U; offset < length; ++offset)
    {
        const uint64_t at = address + offset;
        uint64_t page;
        uint8_t *destination;

        if ((at < frames->base) ||
            (at >= (frames->base + ((uint64_t)PROCESS_USER_STACK_PAGES * PAGE_SIZE))))
        {
            return false;
        }

        page = (at - frames->base) / PAGE_SIZE;
        destination = (uint8_t *)(uintptr_t)PhysicalToDirect(frames->frame[page]);
        destination[at % PAGE_SIZE] = bytes[offset];
    }

    return true;
}

/* An eightbyte, which is what every field of the frame below one is. It is
 * written byte by byte through the routine above rather than as a word, because
 * an eightbyte may straddle two pages of the stack and the two pages need not be
 * two consecutive frames. */
static bool ProcessWriteUserStackWord(const ProcessStackFrames *frames, uint64_t address,
                                      uint64_t value)
{
    return ProcessWriteUserStack(frames, address, &value, sizeof value);
}

/*
 * Lays out the initial process stack of the System V ABI, AMD64 supplement,
 * Section 3.4.1, and returns the stack pointer a program is to be entered upon.
 *
 * The order is the ABI's read from the top downward, which is the order it must
 * be built in: the strings stand highest, because the pointers below them have
 * to name addresses that are already fixed.
 *
 *   the information block   the argument and environment strings, terminated
 *   (alignment padding)
 *   the auxiliary vector    one null entry, being two eightbytes of zero
 *   null                    ending the environment vector
 *   envp[0..n)              pointers into the information block
 *   null                    ending the argument vector
 *   argv[0..argc)           pointers into the information block
 *   argc                    at the stack pointer
 *
 * Returns zero where the vectors do not fit, which the caller must treat as a
 * failure to make a stack at all. Nothing is written back: the pages were zeroed
 * when they were mapped, so a partly built frame is a frame of zeroes and the
 * caller is releasing the whole address space in any case.
 */
static uint64_t ProcessLayOutArguments(const ProcessStackFrames *frames,
                                       const ProcessArguments *arguments)
{
    uint64_t address =
        frames->base + ((uint64_t)PROCESS_USER_STACK_PAGES * PAGE_SIZE);
    uint64_t argument_address[PROCESS_ARGUMENT_COUNT_MAXIMUM];
    uint64_t environment_address[PROCESS_ARGUMENT_COUNT_MAXIMUM];
    uint64_t words;
    uint64_t pointer;

    if ((arguments->argument_count > PROCESS_ARGUMENT_COUNT_MAXIMUM) ||
        (arguments->environment_count > PROCESS_ARGUMENT_COUNT_MAXIMUM) ||
        (arguments->storage_used > PROCESS_ARGUMENT_BYTES_MAXIMUM))
    {
        return 0U;
    }

    /*
     * The information block, copied downward one string at a time so that each
     * string's address is known the moment it has been written.
     *
     * The strings are laid out in reverse order, which is a consequence of
     * building downward and not a requirement of anything: the ABI fixes where
     * the *pointers* stand and says nothing about the order of the bytes they
     * name.
     */
    for (uint32_t index = arguments->environment_count; index > 0U; --index)
    {
        const char *const string = &arguments->storage[arguments->environment[index - 1U]];
        uint64_t length = 0U;

        while (string[length] != '\0')
        {
            ++length;
        }

        address -= (length + 1U);

        if (!ProcessWriteUserStack(frames, address, string, length + 1U))
        {
            return 0U;
        }

        environment_address[index - 1U] = address;
    }

    for (uint32_t index = arguments->argument_count; index > 0U; --index)
    {
        const char *const string = &arguments->storage[arguments->argument[index - 1U]];
        uint64_t length = 0U;

        while (string[length] != '\0')
        {
            ++length;
        }

        address -= (length + 1U);

        if (!ProcessWriteUserStack(frames, address, string, length + 1U))
        {
            return 0U;
        }

        argument_address[index - 1U] = address;
    }

    /*
     * How many eightbytes stand below the information block: the count, the two
     * vectors, their two terminators, and the auxiliary vector's single null
     * entry, which is two eightbytes and not one — an entry is a pair.
     */
    words = 1U + arguments->argument_count + 1U + arguments->environment_count + 1U + 2U;

    /*
     * The stack pointer must be sixteen-byte aligned at the entry point, which
     * Section 3.4.1 guarantees a program. The information block's bottom is at
     * whatever address the last string ended at, so the padding is computed
     * here rather than assumed: the alignment is applied to the *stack pointer*,
     * so the bottom of the block is first rounded down to eight and then the
     * whole of the frame below it rounded down to sixteen.
     */
    address &= ~UINT64_C(7);
    address -= (words * sizeof(uint64_t));
    address &= ~UINT64_C(15);

    pointer = address;

    if (!ProcessWriteUserStackWord(frames, address, (uint64_t)arguments->argument_count))
    {
        return 0U;
    }

    address += sizeof(uint64_t);

    for (uint32_t index = 0U; index < arguments->argument_count; ++index)
    {
        if (!ProcessWriteUserStackWord(frames, address, argument_address[index]))
        {
            return 0U;
        }

        address += sizeof(uint64_t);
    }

    /* The terminators and the auxiliary vector are zero, and the pages are
     * already zeroed — but they are written all the same. The alignment above
     * may have moved the frame down past bytes a longer string once occupied in
     * some earlier layout, and "the page was zero when it was mapped" is an
     * invariant about mapping and not about this frame. */
    if (!ProcessWriteUserStackWord(frames, address, 0U))
    {
        return 0U;
    }

    address += sizeof(uint64_t);

    for (uint32_t index = 0U; index < arguments->environment_count; ++index)
    {
        if (!ProcessWriteUserStackWord(frames, address, environment_address[index]))
        {
            return 0U;
        }

        address += sizeof(uint64_t);
    }

    for (uint32_t remaining = 0U; remaining < 3U; ++remaining)
    {
        if (!ProcessWriteUserStackWord(frames, address, 0U))
        {
            return 0U;
        }

        address += sizeof(uint64_t);
    }

    return pointer;
}

uint64_t ProcessCreateUserStack(Process *process, const ProcessArguments *arguments)
{
    const uint64_t top = PROCESS_USER_STACK_TOP;
    const uint64_t base = top - ((uint64_t)PROCESS_USER_STACK_PAGES * PAGE_SIZE);
    ProcessStackFrames frames;

    if ((process == NULL) || !process->used || (process->user_stack_top != 0U))
    {
        return 0U;
    }

    frames.base = base;

    for (uint64_t page = base; page < top; page += PAGE_SIZE)
    {
        const PhysicalAddress frame = FrameAllocate();
        uint8_t *contents;

        if (frame == 0U)
        {
            return 0U;
        }

        frames.frame[(page - base) / PAGE_SIZE] = frame;

        /*
         * Zeroed, for the reason the loader zeroes a segment's pages: a frame
         * arrives holding whatever its last owner left in it, and a stack is the
         * first thing a program reads. Handing it the kernel's leavings is a
         * disclosure with nothing to report it.
         *
         * Since sub-task 7.5 this also **is** the initial process stack frame.
         * Every eightbyte the System V ABI names there is zero — the argument
         * count, the two vector terminators, the auxiliary vector's terminating
         * entry and the padding — so the zeroing that was already required for
         * the disclosure supplies the frame's contents as well, and nothing is
         * written a second time. See below.
         */
        contents = (uint8_t *)(uintptr_t)PhysicalToDirect(frame);

        for (uint64_t offset = 0U; offset < PAGE_SIZE; ++offset)
        {
            contents[offset] = 0U;
        }

        AddressSpaceMapPage(&process->space, page, frame,
                            PAGE_ENTRY_WRITABLE | PAGE_ENTRY_USER);
        ++process->mapped_pages;
    }

    /*
     * The page below the stack is left unmapped, which is the guard. It costs
     * nothing to leave a hole in an address space, so unlike the kernel stack's
     * guard this one may be a hole — and a hole catches a read as well as a
     * write.
     */
    process->user_stack_top = top;
    process->user_stack_pages = PROCESS_USER_STACK_PAGES;

    /*
     * The initial process stack of the System V ABI, AMD64 supplement, Section
     * 3.4.1 — which upon this system is a subtraction and nothing else.
     *
     * The ABI requires the argument count at the stack pointer, the argument
     * pointers above it, a null pointer ending them, the environment pointers, a
     * null pointer ending those, and the auxiliary vector ending with a null
     * entry. This kernel's `execve` accepts neither vector, so every one of those
     * eightbytes is zero — and the pages were zeroed above, so the frame is
     * already standing. What was missing was **room for it**: a stack pointer
     * left at `top` points one byte past the last mapped byte, and the first
     * instruction of every conforming `_start` reads the argument count through
     * it.
     *
     * So the whole of this is the address returned. An earlier version wrote six
     * zeroes into the topmost frame and kept a copy of that frame's physical
     * address in order to reach them; deleting the write changed nothing any
     * assertion could see, because the pages are zeroed unconditionally and for
     * a reason that has nothing to do with this. The write was a restatement of
     * an invariant established a few lines above, and the negative test that
     * removed it reported nothing — which is the same judgement sub-task 7.3
     * made about the second size check in `OxysHeapAdopt`.
     *
     * The address is sixteen-byte aligned, `top` being page-aligned and the
     * frame a multiple of sixteen, which is what Section 3.4.1 guarantees a
     * program at its entry point.
     *
     * **Since sub-task 7.6 that is the case where there is nothing to put upon
     * the frame**, and it is still a case: a process created by the kernel for
     * its own purposes has no arguments, and the composed programs of Phase 6
     * never read one. Where there are arguments, the frame is laid out rather
     * than left as zeroes, and `ProcessLayOutArguments` does it.
     */
    if ((arguments == NULL) ||
        ((arguments->argument_count == 0U) && (arguments->environment_count == 0U)))
    {
        return top - PROCESS_USER_STACK_FRAME_BYTES;
    }

    return ProcessLayOutArguments(&frames, arguments);
}

/* ------------------------------------------- the descriptors of sub-task 7.6 */

void ProcessCloseDescriptors(Process *process)
{
    if (process == NULL)
    {
        return;
    }

    for (size_t index = 0U; index < PROCESS_DESCRIPTOR_CAPACITY; ++index)
    {
        if (process->descriptors[index] != PROCESS_DESCRIPTOR_FREE)
        {
            (void)VfsClose(process->descriptors[index]);
            process->descriptors[index] = PROCESS_DESCRIPTOR_FREE;
        }
    }
}

int64_t ProcessAdoptDescriptor(Process *process, int file)
{
    if ((process == NULL) || !process->used || (file == VFS_NO_DESCRIPTOR))
    {
        return SYSCALL_EINVAL;
    }

    /*
     * The search begins at SYSCALL_DESCRIPTOR_FIRST and not at zero. The three
     * below it name the diagnostic path, and a program handed descriptor 1 for a
     * file it opened would then write to that file every time it called printf.
     */
    for (size_t index = SYSCALL_DESCRIPTOR_FIRST; index < PROCESS_DESCRIPTOR_CAPACITY;
         ++index)
    {
        if (process->descriptors[index] == PROCESS_DESCRIPTOR_FREE)
        {
            process->descriptors[index] = file;

            return (int64_t)index;
        }
    }

    return SYSCALL_EMFILE;
}

int ProcessDescriptorFile(const Process *process, int64_t descriptor)
{
    if ((process == NULL) || !process->used)
    {
        return VFS_NO_DESCRIPTOR;
    }

    if ((descriptor < 0) || (descriptor >= (int64_t)PROCESS_DESCRIPTOR_CAPACITY))
    {
        return VFS_NO_DESCRIPTOR;
    }

    return process->descriptors[descriptor];
}

bool ProcessReleaseDescriptor(Process *process, int64_t descriptor)
{
    const int file = ProcessDescriptorFile(process, descriptor);

    if (file == VFS_NO_DESCRIPTOR)
    {
        return false;
    }

    process->descriptors[descriptor] = PROCESS_DESCRIPTOR_FREE;

    return VfsClose(file);
}

int64_t ProcessPlaceDescriptor(Process *process, int64_t from, int64_t to)
{
    int file;

    if ((process == NULL) || !process->used || (from < 0) || (to < 0) ||
        (from >= (int64_t)PROCESS_DESCRIPTOR_CAPACITY) ||
        (to >= (int64_t)PROCESS_DESCRIPTOR_CAPACITY))
    {
        return SYSCALL_EBADF;
    }

    if (from == to)
    {
        return SYSCALL_OK;
    }

    file = process->descriptors[from];

    /*
     * A number below SYSCALL_DESCRIPTOR_FIRST that holds no file names the
     * kernel's own path — the terminal for 0, the diagnostic path for 1 and
     * 2 — and that path may be given to another of the three, which is
     * `2>&1` with nothing redirected, but not to a number above them: the
     * table holds files of the filesystem layer and the kernel's paths are
     * not files. An empty number above them is simply nothing to duplicate.
     */
    if (file == PROCESS_DESCRIPTOR_FREE)
    {
        if ((from >= (int64_t)SYSCALL_DESCRIPTOR_FIRST) || (to >= (int64_t)SYSCALL_DESCRIPTOR_FIRST))
        {
            return SYSCALL_EBADF;
        }
    }
    else if (!VfsHold(file))
    {
        return SYSCALL_EBADF;
    }

    if (process->descriptors[to] != PROCESS_DESCRIPTOR_FREE)
    {
        (void)VfsClose(process->descriptors[to]);
    }

    process->descriptors[to] = file;

    return SYSCALL_OK;
}

/* ------------------------------------------------------- sub-task 6.11 */

/* Accounting for the four calls. */
static uint64_t ProcessForks;
static uint64_t ProcessExecutions;
static uint64_t ProcessReaps;

Process *ProcessCurrent(void)
{
    return (ProcessCurrentThreads[ProcessProcessorIndex()] != NULL) ? ProcessCurrentThreads[ProcessProcessorIndex()]->owner : NULL;
}

Process *ProcessFork(Process *parent, const SyscallFrame *frame)
{
    Process *child;
    Thread *thread;

    if ((parent == NULL) || !parent->used || (frame == NULL))
    {
        return NULL;
    }

    /*
     * The address space is cloned, not built. That single call is the whole of
     * what sub-task 2.8 was written for: the pages are shared, the writable ones
     * are protected in both hierarchies, and a reference is recorded for the new
     * holder — so a fork costs the paging structures and nothing else until one
     * of the two writes.
     */
    child = ProcessAllocate(parent->name, parent, &parent->space);

    if (child == NULL)
    {
        return NULL;
    }

    /*
     * What the parent knows about its own memory is true of the child's, the
     * mappings being the same ones. It is copied rather than recomputed because
     * an address space still cannot answer what it maps — which is the reason
     * these fields exist at all; see docs/design/MEMORY-LAYOUT.md, limitation 2.
     */
    child->image_lowest = parent->image_lowest;
    child->image_highest = parent->image_highest;
    child->image_entry = parent->image_entry;
    child->mapped_pages = parent->mapped_pages;
    child->user_stack_top = parent->user_stack_top;
    child->user_stack_pages = parent->user_stack_pages;

    /*
     * The heap comes across with everything else, and both of its bounds must.
     *
     * The pages between them were cloned by the call above like any other, so the
     * child's heap holds exactly what its parent's held at the moment of the
     * fork. Copying `break_start` alone and letting the child begin with an empty
     * heap would leave those pages mapped and unaccounted: the child would grow
     * its break over memory it already had, and the second growth would map a
     * fresh frame over a page whose contents the program was still using.
     */
    child->break_start = parent->break_start;
    child->break_current = parent->break_current;

    /* The dispositions come across, of sub-task 8.7, and the pending set does
     * not: IEEE Std 1003.1-2017, `fork()`, has the child begin with no signal
     * pending and every disposition its parent had. The group came across in
     * ProcessAllocate. */
    for (size_t signal = 0U; signal <= SYSCALL_SIGNAL_MAXIMUM; ++signal)
    {
        child->handlers[signal] = parent->handlers[signal];
    }

    child->restorer = parent->restorer;

    /* The working directory of sub-task 8.3 comes across as IEEE Std 1003.1-2017
     * has it: a child begins where its parent stood. */
    for (size_t index = 0U; index <= PROCESS_PATH_MAXIMUM; ++index)
    {
        child->working_directory[index] = parent->working_directory[index];
    }

    /*
     * The descriptors come across too, of sub-task 8.5, each open file gaining
     * a holder: IEEE Std 1003.1-2017 has a child share its parent's open files
     * and their positions, and since VfsHold exists that is what happens. Until
     * this sub-task a child inherited nothing, the filesystem layer having no
     * count of holders and a shared file closed by either being closed for
     * both; docs/design/PROCESS.md, Section 14, records the interval.
     */
    for (size_t index = 0U; index < PROCESS_DESCRIPTOR_CAPACITY; ++index)
    {
        const int file = parent->descriptors[index];

        if ((file != PROCESS_DESCRIPTOR_FREE) && VfsHold(file))
        {
            child->descriptors[index] = file;
        }
    }

    /*
     * The child begins where its parent will resume: at the address SYSCALL put
     * in RCX, upon the stack the entry path saved. The stack is the parent's
     * address and is the child's stack all the same, the cloned space mapping
     * the same address to a frame of its own once either writes to it.
     */
    thread = ThreadCreate(child, frame->rcx, frame->user_stack);

    if (thread == NULL)
    {
        ProcessDestroy(child);

        return NULL;
    }

    thread->resume = *frame;
    thread->resume.rax = 0U;
    thread->resumes_from_fork = true;

    child->state = PROCESS_READY;
    ++ProcessForks;

    /*
     * And the child joins the rotation, since sub-task 8.6.
     *
     * Until then a child ran when its parent waited for it, upon the parent's
     * own flow of control, which docs/design/PROCESS.md, Section 13.2, recorded
     * as the one departure from the call it is named after. A pipeline cannot
     * be run that way: the writer must sleep when the pipe is full and the
     * reader must run meanwhile, which is two threads the scheduler switches
     * between and not one that starts the other. The child is therefore
     * admitted here and runs when the parent sleeps or is pre-empted at
     * privilege level 3, whichever is first — and a parent that never waits
     * no longer has a child that never runs.
     *
     * The kernel stack is prepared here, as ThreadStart prepares it for a
     * thread it starts by a call: the first switch to the child returns into
     * the trampoline, and a stack left as ThreadCreate made it — the pointer
     * at its very top — would have that return read the unmapped page above
     * it. That is the first defect this sub-task met, and it presented as a
     * page fault in the switch at a stack pointer with no stack beneath it.
     * The thread was created THREAD_CREATED, which SchedulerAdmit accepts,
     * and the resume is carried in the frame above. Where the scheduler was not
     * prepared — a local timer that could not be calibrated — the child is
     * left standing and `wait` runs it as it did before, so a machine that
     * cannot pre-empt still runs programs, one at a time.
     */
    if (SchedulerIsRunning())
    {
        ThreadPrepareStart(thread);
    }

    if (SchedulerIsRunning() && !SchedulerAdmit(thread))
    {
        ProcessDestroy(child);

        return NULL;
    }

    return child;
}

int64_t ProcessExecute(Process *process, const char *path,
                       const ProcessArguments *arguments)
{
    Thread *const thread = ProcessCurrentThreads[ProcessProcessorIndex()];
    AddressSpace fresh;
    AddressSpace previous;
    ElfImage image;
    uint64_t stack;

    if ((process == NULL) || !process->used || (path == NULL) || (thread == NULL) ||
        (thread->owner != process))
    {
        return SYSCALL_EINVAL;
    }

    /*
     * The new program is built entire before the old one is touched.
     *
     * A loader that filled the process's own address space would have nothing to
     * go back to when an image turned out to be malformed half way through, and
     * `execve` that fails must leave the caller running: a program told that its
     * file does not exist is a program that carries on and reports so. The cost
     * is that both address spaces exist at once, for as long as the load takes.
     */
    if (!AddressSpaceCreate(&fresh))
    {
        return SYSCALL_ENOMEM;
    }

    if (ElfLoadFile(&fresh, path, &image) != ELF_OK)
    {
        AddressSpaceDestroy(&fresh);

        return SYSCALL_ENOENT;
    }

    /*
     * The point of no return, and the order within it is not free.
     *
     * The new space is made active *before* the old one is released, because
     * AddressSpaceDestroy refuses to release the space the processor is
     * translating through — and rightly: releasing the frames beneath a running
     * program's own mappings is a fault that arrives at some unrelated later
     * instruction. This function continues to execute across the change because
     * the kernel's higher half is mapped identically in both, which is what
     * AddressSpaceCreate copies the kernel's entries for.
     */
    previous = process->space;
    AddressSpaceSwitch(&fresh);
    AddressSpaceDestroy(&previous);

    process->space = fresh;
    process->image_lowest = image.lowest;
    process->image_highest = image.highest;
    process->image_entry = image.entry;
    process->mapped_pages = image.pages;

    /* The old stack went with the old address space, and the extent record must
     * say so before a new one may be given: ProcessCreateUserStack refuses a
     * process that already has one, which is what stops a second stack being
     * mapped over the first. */
    process->user_stack_top = 0U;
    process->user_stack_pages = 0U;

    /*
     * The old heap went with the old address space, so the break is placed anew
     * from the image that replaced it. A break carried across would name an
     * address derived from a program that no longer exists — which, the new
     * image being smaller, may lie within the new program's own `.bss`.
     */
    ProcessEstablishBreak(process);

    /*
     * The descriptors the old program held are kept, since sub-task 8.5, as
     * IEEE Std 1003.1-2017 has them unless marked close-on-exec — which this
     * kernel has no mark for. They were closed here from 7.6 to 8.4, the safe
     * half of the rule, because a descriptor kept was a descriptor nothing
     * could have meant to keep; the shell's redirection is what means to: it
     * opens the file in the child, places it at 0 or 1, and the program it
     * then becomes must find it there. The table is the process's own and
     * the process is the same one, so nothing leaks: what it holds it holds
     * until it closes or ends. docs/design/PROCESS.md, Section 14.
     */

    stack = ProcessCreateUserStack(process, arguments);

    if (stack == 0U)
    {
        /*
         * Beyond the point of no return there is no program to fail back into:
         * the one that called is gone. The process is ended instead, with a
         * status that says which failure it was, and its parent collects that
         * exactly as it would collect any other ending.
         */
        ProcessExit(SYSCALL_ENOMEM);
    }

    thread->entry = image.entry;
    thread->user_stack = stack;

    /* The process takes the program's name — the last component of the path
     * — since 2026-09-16, so that `ps` names what runs rather than the shell
     * every child was forked from. */
    {
        const char *last = path;

        for (const char *at = path; *at != '\0'; ++at)
        {
            if ((*at == '/') && (at[1] != '\0'))
            {
                last = at + 1;
            }
        }

        ProcessCopyName(process->name, last);
    }

    /* A thread that reached here by `fork` has a saved user context, and it
     * describes a program that no longer exists. Leaving it set would resume the
     * old program's registers in the new program's address space. */
    thread->resumes_from_fork = false;

    /* A handler belongs to the program that installed it, and that program is
     * gone: IEEE Std 1003.1-2017 resets a caught signal to its default across
     * an exec and keeps an ignored one ignored. Of sub-task 8.7. */
    SignalResetForExecute(process);

    ++ProcessExecutions;

    SyscallEstablishUserGsBase();
    ThreadEnterUser(image.entry, stack, (uint64_t)GDT_USER_CODE_SELECTOR | 3U,
                    (uint64_t)GDT_USER_DATA_SELECTOR | 3U);

    /* Not reached. */
    return SYSCALL_OK;
}

void ProcessExit(int64_t status)
{
    if (!ThreadTerminateCurrent(status))
    {
        /*
         * The same condition the exception path panics upon, reached by the
         * other route: a program asked to end and there was nobody recorded to
         * return to, which means privilege level 3 was reached by something that
         * did not go through ThreadStart. Returning would return through SYSRET
         * to a program that believes it has ended.
         */
        KernelPanic("A program ended that nothing this kernel started had begun.");
    }
}

uint64_t ProcessWaitFor(Process *parent, int64_t pid, uint64_t options, int64_t *status)
{
    Process *child = NULL;
    uint64_t collected;

    if ((parent == NULL) || !parent->used || (status == NULL))
    {
        return PROCESS_WAIT_NO_CHILD;
    }

    for (;;)
    {
        bool any = false;

        child = NULL;

        /*
         * A child that has ended is preferred to one that has not, and one
         * that has stopped and not yet been reported comes next where the
         * caller asked for stops; of the rest, whichever is found first is
         * the one waited for.
         *
         * Collecting one that has already ended costs nothing, where
         * collecting one that has not means waiting for it. Taking the
         * finished one first is therefore what makes a parent with several
         * children collect them as they finish rather than in the order the
         * table happens to hold them.
         *
         * The scan and the sleep below are one masked section, which is the
         * discipline <oxys/proc/sched.h> sets out: a child that ends between
         * the scan and the sleep would otherwise wake a parent not yet asleep,
         * and the parent would then sleep for a wake that had already
         * happened.
         */
        PerCpuPushInterruptState();

        for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
        {
            Process *const candidate = &ProcessTable[index];

            if (!candidate->used || (candidate->parent_id != parent->id))
            {
                continue;
            }

            /* The selector of `waitpid`, of sub-task 8.7: one child by
             * identifier, every child, or every child of a group. */
            if (pid > 0)
            {
                if (candidate->id != (uint64_t)pid)
                {
                    continue;
                }
            }
            else if (pid < -1)
            {
                if (candidate->group != (uint64_t)(-pid))
                {
                    continue;
                }
            }

            any = true;

            if (candidate->state == PROCESS_EXITED)
            {
                child = candidate;
                break;
            }

            if (((options & SYSCALL_WAIT_UNTRACED) != 0U) &&
                (candidate->state == PROCESS_STOPPED) && !candidate->stop_reported)
            {
                child = candidate;
                continue;
            }

            if (child == NULL)
            {
                child = candidate;
            }
        }

        if (!any)
        {
            PerCpuPopInterruptState();

            return PROCESS_WAIT_NO_CHILD;
        }

        if ((child != NULL) && (child->state == PROCESS_EXITED))
        {
            PerCpuPopInterruptState();
            break;
        }

        if ((child != NULL) && (child->state == PROCESS_STOPPED) && !child->stop_reported &&
            ((options & SYSCALL_WAIT_UNTRACED) != 0U))
        {
            /* A stop is reported once, and the child is not collected: it is
             * still there, and `waitpid` will report its ending later. */
            child->stop_reported = true;
            *status = (int64_t)child->wait_status;
            PerCpuPopInterruptState();

            return child->id;
        }

        if ((options & SYSCALL_WAIT_NO_HANG) != 0U)
        {
            PerCpuPopInterruptState();
            *status = 0;

            return 0U;
        }

        if (SchedulerCanSleep())
        {
            /*
             * A child that has not ended is waited for, since sub-task 8.6: the
             * parent sleeps upon its own process, which is the channel every
             * child of it wakes when it ends or stops, and looks again when
             * woken. The child was admitted to the rotation at the fork and
             * runs while the parent sleeps — or ran already, pre-empting the
             * parent at privilege level 3, in which case the scan above found
             * it ended and this branch was not taken.
             *
             * A signal that arrives during the sleep ends the wait with
             * EINTR, since 8.7, so that the signal is acted upon before the
             * parent is told anything else; the shell's `wait` is interrupted
             * that way by nothing today, because it ignores what the terminal
             * sends, and a program that catches SIGINT sees its wait fail.
             */
            SchedulerSleep(parent);
            PerCpuPopInterruptState();

            if (SignalIsPending(parent))
            {
                return PROCESS_WAIT_INTERRUPTED;
            }

            continue;
        }

        PerCpuPopInterruptState();

        break;
    }

    if (child == NULL)
    {
        return PROCESS_WAIT_NO_CHILD;
    }

    /*
     * A child that has not run and cannot be waited for is run now, here, by
     * the caller's own thread of control — which is what `wait` meant until
     * sub-task 8.6, and still means where the scheduler was not prepared or
     * where the caller has no thread to sleep upon, which is the kernel's own
     * flow of control inside a self-test. docs/design/PROCESS.md, Section
     * 13.2, records the interval in which this was the only path.
     *
     * The child is withdrawn from the queue first where the fork admitted it:
     * a thread started by a call while still linked into a queue would be
     * dequeued and switched to a second time.
     */
    if (child->state != PROCESS_EXITED)
    {
        Thread *const thread = (child->thread_count > 0U) ? child->threads[0] : NULL;

        if ((thread != NULL) && thread->queued)
        {
            (void)SchedulerWithdraw(thread);
        }

        if ((thread == NULL) || !ThreadStart(thread))
        {
            /*
             * A child that cannot be started is ended rather than left standing.
             * Returning zero here would tell the parent it has no children while
             * one sits in the table for ever, and a parent that waited again
             * would be told the same thing again.
             */
            child->state = PROCESS_EXITED;
            child->exit_status = SYSCALL_EINVAL;
            child->wait_status = (uint64_t)SYSCALL_EINVAL;
        }
    }

    collected = child->id;
    *status = (int64_t)child->wait_status;

    /* And the slot, the threads and the address space go back. Nothing else
     * holds the child: its parent named it by number, which is why a parent may
     * be told an identifier that is already nobody's. */
    ProcessDestroy(child);
    ++ProcessReaps;

    return collected;
}

uint64_t ProcessWait(Process *parent, int64_t *status)
{
    const uint64_t collected = ProcessWaitFor(parent, -1, 0U, status);

    /* The callers of the form that predates 8.7 — the self-tests — ask for
     * any child and cannot be interrupted, and are told 0 for none as they
     * always were. */
    if ((collected == PROCESS_WAIT_NO_CHILD) || (collected == PROCESS_WAIT_INTERRUPTED))
    {
        return 0U;
    }

    return collected;
}

uint64_t ProcessForkCount(void)
{
    return ProcessForks;
}

uint64_t ProcessExecuteCount(void)
{
    return ProcessExecutions;
}

uint64_t ProcessReapCount(void)
{
    return ProcessReaps;
}

/* ------------------------------------------------------- sub-task 7.3 */

/* Accounting for the break. */
static uint64_t ProcessBreakGrowths;
static uint64_t ProcessBreakShrinks;
static uint64_t ProcessBreakPages;

void ProcessEstablishBreak(Process *process)
{
    if ((process == NULL) || !process->used)
    {
        return;
    }

    /*
     * A process with no image gets no heap. Deriving one from an image_highest
     * of zero would place the break in the lowest page of the address space,
     * which is the page deliberately left unmapped so that a null pointer
     * dereference faults; a heap there would take that away from every program.
     */
    if (process->image_highest == 0U)
    {
        process->break_start = 0U;
        process->break_current = 0U;

        return;
    }

    process->break_start = AlignUp(process->image_highest, PAGE_SIZE) +
                           ((uint64_t)PROCESS_BREAK_GAP_PAGES * PAGE_SIZE);
    process->break_current = process->break_start;
}

uint64_t ProcessBreak(const Process *process)
{
    if ((process == NULL) || !process->used)
    {
        return 0U;
    }

    return process->break_current;
}

/*
 * Maps one zeroed, writable, user-accessible page of heap.
 *
 * Zeroed for the reason ProcessCreateUserStack zeroes a stack: a frame arrives
 * holding whatever its last owner left in it, and handing that to a program is a
 * disclosure with nothing to report it. An allocator is the one caller most
 * likely to hand the bytes straight on without writing them first.
 *
 * Returns false where no frame could be had, having mapped nothing.
 */
static bool ProcessMapBreakPage(Process *process, uint64_t page)
{
    const PhysicalAddress frame = FrameAllocate();
    uint8_t *contents;

    if (frame == FRAME_ALLOCATION_FAILED)
    {
        return false;
    }

    contents = (uint8_t *)(uintptr_t)PhysicalToDirect(frame);

    for (uint64_t offset = 0U; offset < PAGE_SIZE; ++offset)
    {
        contents[offset] = 0U;
    }

    AddressSpaceMapPage(&process->space, page, frame,
                        PAGE_ENTRY_WRITABLE | PAGE_ENTRY_USER);

    ++process->mapped_pages;
    ++ProcessBreakPages;

    return true;
}

/* Withdraws one page of heap and releases the frame beneath it. FrameFree
 * returns the frame to the allocator only upon the last reference, so a page
 * still shared with a forked relation survives this. */
static void ProcessUnmapBreakPage(Process *process, uint64_t page)
{
    const PhysicalAddress frame = AddressSpaceUnmapPage(&process->space, page);

    if (frame == FRAME_ALLOCATION_FAILED)
    {
        return;
    }

    FrameFree(frame);

    if (process->mapped_pages > 0U)
    {
        --process->mapped_pages;
    }

    if (ProcessBreakPages > 0U)
    {
        --ProcessBreakPages;
    }
}

int64_t ProcessSetBreak(Process *process, uint64_t requested)
{
    uint64_t established;
    uint64_t wanted;

    if ((process == NULL) || !process->used || (process->break_start == 0U))
    {
        return SYSCALL_EINVAL;
    }

    /*
     * Below where the heap begins is refused rather than clamped. A program that
     * asked for a break beneath its own image has computed an address wrongly,
     * and a kernel that silently moved the request to the nearest legal value
     * would leave the program believing the arithmetic that produced it was
     * sound.
     */
    if (requested < process->break_start)
    {
        return SYSCALL_EINVAL;
    }

    if ((requested - process->break_start) > PROCESS_BREAK_MAXIMUM)
    {
        return SYSCALL_ENOMEM;
    }

    /* The pages presently covering the heap, and those the request asks for. The
     * break itself is byte-granular and the mapping is not, so both bounds are
     * rounded up: a break part way through a page needs the whole of that page. */
    established = AlignUp(process->break_current, PAGE_SIZE);
    wanted = AlignUp(requested, PAGE_SIZE);

    if (wanted > established)
    {
        for (uint64_t page = established; page < wanted; page += PAGE_SIZE)
        {
            if (!ProcessMapBreakPage(process, page))
            {
                /*
                 * The attempt is undone entire.
                 *
                 * A partial growth is worse than no growth at all: the call must
                 * report either the break it was asked for or a failure, and a
                 * break left part way between the two would be a heap whose
                 * first pages are mapped and whose last are not — which the
                 * program discovers at whichever byte it happens to touch first,
                 * far from the request that failed.
                 */
                for (uint64_t undo = established; undo < page; undo += PAGE_SIZE)
                {
                    ProcessUnmapBreakPage(process, undo);
                }

                return SYSCALL_ENOMEM;
            }
        }

        ++ProcessBreakGrowths;
    }
    else if (wanted < established)
    {
        for (uint64_t page = wanted; page < established; page += PAGE_SIZE)
        {
            ProcessUnmapBreakPage(process, page);
        }

        ++ProcessBreakShrinks;
    }
    else
    {
        /*
         * The request moves the break within a page that is already mapped.
         * Nothing is mapped and nothing withdrawn, and it is neither a growth
         * nor a shrink for the purpose of the accounting — but the break moves,
         * because it is the break and not the mapping that says how much of the
         * heap the program owns.
         */
    }

    process->break_current = requested;

    return (int64_t)requested;
}

uint64_t ProcessBreakGrowthCount(void)
{
    return ProcessBreakGrowths;
}

uint64_t ProcessBreakShrinkCount(void)
{
    return ProcessBreakShrinks;
}

uint64_t ProcessBreakPageCount(void)
{
    return ProcessBreakPages;
}

uint64_t ProcessesCreated(void)
{
    return ProcessCreations;
}

uint64_t ThreadsCreated(void)
{
    return ThreadCreations;
}

void ProcessReport(void)
{
    const size_t processes = ProcessCount();

    KernelWriteString("Processes: ");
    KernelWriteDecimal((uint64_t)processes);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)PROCESS_CAPACITY);
    KernelWriteString(", threads ");
    KernelWriteDecimal((uint64_t)ThreadCount());
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)THREAD_CAPACITY);
    KernelWriteString("; created ");
    KernelWriteDecimal(ProcessCreations);
    KernelWriteString(" and ");
    KernelWriteDecimal(ThreadCreations);
    KernelWriteString(" since the start.\n");

    /*
     * The four calls, counted. The forks and the collections are printed
     * together because they must balance: a process forked and never collected
     * is a slot that stays occupied, and the difference between these two
     * numbers is the number of children nobody has waited for.
     */
    KernelWriteString("Processes: ");
    KernelWriteDecimal(ProcessForks);
    KernelWriteString(" fork(s), ");
    KernelWriteDecimal(ProcessExecutions);
    KernelWriteString(" execution(s), ");
    KernelWriteDecimal(ProcessReaps);
    KernelWriteString(" child(ren) collected.\n");

    /* The break, of sub-task 7.3. The two directions are printed apart because a
     * heap that only ever grows and one that grows and gives memory back are
     * different behaviours, and a single count of requests would not tell them
     * apart. */
    KernelWriteString("Processes: ");
    KernelWriteDecimal(ProcessBreakGrowths);
    KernelWriteString(" break growth(s), ");
    KernelWriteDecimal(ProcessBreakShrinks);
    KernelWriteString(" shrink(s), ");
    KernelWriteDecimal(ProcessBreakPages);
    KernelWriteString(" heap page(s) presently mapped by them.\n");

    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        const Process *const process = &ProcessTable[index];

        if (!process->used)
        {
            continue;
        }

        KernelWriteString("  ");
        KernelWriteDecimal(process->id);
        KernelWriteString(" ");
        KernelWriteString(process->name);
        KernelWriteString(", ");
        KernelWriteString(ProcessStateName(process->state));
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)process->thread_count);
        KernelWriteString(" thread(s), ");
        KernelWriteDecimal(process->mapped_pages);
        KernelWriteString(" page(s) mapped, entry ");
        KernelWriteHexadecimal(process->image_entry);
        KernelWriteString(", break ");
        KernelWriteHexadecimal(process->break_current);
        KernelWriteString(" of ");
        KernelWriteHexadecimal(process->break_start);
        KernelWriteString("\n");
    }

    if (ProcessCurrentThreads[ProcessProcessorIndex()] != NULL)
    {
        KernelWriteString("Processes: the current thread is ");
        KernelWriteDecimal(ProcessCurrentThreads[ProcessProcessorIndex()]->id);
        KernelWriteString(", kernel stack top ");
        KernelWriteHexadecimal(ProcessCurrentThreads[ProcessProcessorIndex()]->kernel_stack_top);
        KernelWriteString(".\n");
    }
    else if (ProcessTerminations > 0U)
    {
        /*
         * No thread is current, but things have run.
         *
         * This is the ordinary state at the end of a boot: the self-tests adopt
         * a thread, switch away from it, switch back, and give everything up
         * before they return, so nothing is current by the time this report is
         * reached. Saying "nothing has run" here would be a false statement
         * printed immediately below the log of a program running, which is the
         * one place a reader can see that it is false.
         */
        KernelWriteString("Processes: no thread is current; ");
        KernelWriteDecimal(ProcessTerminations);
        KernelWriteString(" program(s) have run and ended.\n");
    }
    else
    {
        KernelWriteString("Processes: no thread is current, and nothing has run.\n");
    }
}
