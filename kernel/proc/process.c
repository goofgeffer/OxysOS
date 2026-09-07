/*
 * File: kernel/proc/process.c
 * Purpose: Implements the process and thread tables: the creation and
 *          destruction of a process with an address space of its own, of a
 *          thread with a kernel stack of its own beneath a guard page, and the
 *          record of what a process has had loaded into it.
 * Key functions: ProcessInitialise, ProcessCreate, ProcessDestroy, ThreadCreate,
 *          ThreadDestroy, ThreadSetCurrent, ProcessCreateUserStack,
 *          ProcessRecordImage, ProcessReport.
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
 * Identifiers are numbers and not indices.
 *
 *   A slot in a table is reused the moment its occupant is destroyed; an
 *   identifier never is. A parent therefore records its child by number, and a
 *   parent that outlives the table slot its child once held finds nobody rather
 *   than finding whoever was given that slot next — which is the whole class of
 *   fault that makes a process kill an unrelated one.
 *
 * Concurrency. Neither table is guarded. Nothing runs but the boot sequence
 * until sub-task 6.15, and from sub-task 6.13 both tables and the current thread
 * become the business of that sub-task's lock.
 */

#include <oxys/process.h>
#include <oxys/addrspace.h>
#include <oxys/kernel.h>
#include <oxys/memory.h>
#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/tss.h>
#include <oxys/vmm.h>

static Process ProcessTable[PROCESS_CAPACITY];
static Thread ThreadTable[THREAD_CAPACITY];

static uint64_t ProcessNextId = 1U;
static uint64_t ThreadNextId = 1U;
static uint64_t ProcessCreations;
static uint64_t ThreadCreations;

static Thread *ProcessCurrentThread;

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

    ProcessCurrentThread = NULL;
}

/* ---------------------------------------------------------------- processes */

Process *ProcessCreate(const char *name, const Process *parent)
{
    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        Process *const process = &ProcessTable[index];

        if (process->used)
        {
            continue;
        }

        if (!AddressSpaceCreate(&process->space))
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
        process->used = true;

        for (size_t slot = 0U; slot < PROCESS_THREAD_MAXIMUM; ++slot)
        {
            process->threads[slot] = NULL;
        }

        ++ProcessCreations;

        return process;
    }

    return NULL;
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
    if (ProcessCurrentThread == thread)
    {
        ProcessCurrentThread = NULL;
    }

    ThreadReleaseStack(thread->kernel_stack_base);

    thread->used = false;
    thread->state = THREAD_UNUSED;
    thread->owner = NULL;
    thread->kernel_stack_base = NULL;
    thread->kernel_stack_top = 0U;
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
    ProcessCurrentThread = thread;

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
}

Thread *ThreadCurrent(void)
{
    return ProcessCurrentThread;
}

/* --------------------------------------------------- what a process holds */

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
}

uint64_t ProcessCreateUserStack(Process *process)
{
    const uint64_t top = PROCESS_USER_STACK_TOP;
    const uint64_t base = top - ((uint64_t)PROCESS_USER_STACK_PAGES * PAGE_SIZE);

    if ((process == NULL) || !process->used || (process->user_stack_top != 0U))
    {
        return 0U;
    }

    for (uint64_t page = base; page < top; page += PAGE_SIZE)
    {
        const PhysicalAddress frame = FrameAllocate();
        uint8_t *contents;

        if (frame == 0U)
        {
            return 0U;
        }

        /*
         * Zeroed, for the reason the loader zeroes a segment's pages: a frame
         * arrives holding whatever its last owner left in it, and a stack is the
         * first thing a program reads. Handing it the kernel's leavings is a
         * disclosure with nothing to report it.
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

    return top;
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
        KernelWriteString("\n");
    }

    if (ProcessCurrentThread != NULL)
    {
        KernelWriteString("Processes: the current thread is ");
        KernelWriteDecimal(ProcessCurrentThread->id);
        KernelWriteString(", kernel stack top ");
        KernelWriteHexadecimal(ProcessCurrentThread->kernel_stack_top);
        KernelWriteString(".\n");
    }
    else
    {
        KernelWriteString("Processes: no thread is current; nothing has run.\n");
    }
}
