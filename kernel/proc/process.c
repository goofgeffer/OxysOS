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
 *   operation upon the two tables above, and kernel/cpu/syscall.c does no more
 *   than validate a caller's arguments and name one of them.
 *
 * A child runs when its parent waits for it.
 *
 *   There is one thread of control until the scheduler of sub-task 6.15, so a
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
 * Concurrency. Neither table is guarded. Nothing runs but the boot sequence
 * until sub-task 6.15, and the spinlock of sub-task 6.13 exists and has not been
 * applied here; both tables and the current thread require it then.
 */

#include <oxys/process.h>
#include <oxys/addrspace.h>
#include <oxys/kernel.h>
#include <oxys/memory.h>
#include <oxys/paging.h>
#include <oxys/pmm.h>
#include <oxys/gdt.h>
#include <oxys/tss.h>
#include <oxys/vmm.h>

static Process ProcessTable[PROCESS_CAPACITY];
static Thread ThreadTable[THREAD_CAPACITY];

static uint64_t ProcessNextId = 1U;
static uint64_t ThreadNextId = 1U;
static uint64_t ProcessCreations;
static uint64_t ThreadCreations;
static uint64_t ProcessTerminations;

static Thread *ProcessCurrentThread;

/*
 * The thread that started a program and is waiting to be returned to when it
 * ends, of sub-task 6.10.
 *
 * Declared here beside the current thread rather than beside the switching code
 * that sets it, because ThreadDestroy must clear both and for the same reason:
 * either pointer left naming a destroyed thread names a released slot, and a
 * kernel stack that has gone back to the arena.
 */
static Thread *ProcessReturnThread;

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
    if (ProcessCurrentThread == thread)
    {
        ProcessCurrentThread = NULL;
    }

    /*
     * And a thread that was the one to return to is that no longer, for the same
     * reason and with a worse consequence.
     *
     * ProcessReturnThread is what ThreadTerminateCurrent switches to when a
     * program ends. Left naming a destroyed thread, that switch would load a
     * stack pointer out of a context structure belonging to a released slot and
     * resume execution upon a kernel stack the arena has given to somebody else
     * — which is not a fault but a machine that continues, wrongly, with no
     * indication that anything happened.
     */
    if (ProcessReturnThread == thread)
    {
        ProcessReturnThread = NULL;
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
    return ProcessCurrentThread;
}

/* --------------------------------------------------- what a process holds */


/* ------------------------------------------------------- sub-task 6.10 */

/*
 * The assembly of kernel/proc/switch.asm addresses ThreadContext by number and
 * cannot see this structure. A field reordered here without the assembly would
 * have a switch restore the stack pointer from a general register — which is a
 * jump to an address that was never an address.
 */
_Static_assert(offsetof(ThreadContext, rbx) == 0U, "The switch saves RBX at offset 0.");
_Static_assert(offsetof(ThreadContext, rsp) == 48U,
               "The switch saves the stack pointer at offset 48.");
_Static_assert(sizeof(ThreadContext) == 56U, "A context is seven quadwords.");

/* Defined in kernel/proc/switch.asm. */
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
        thread->resumes_from_fork = false;
        thread->used = true;

        thread->context.rsp = ThreadPrepareFrame(top, (uint64_t)(uintptr_t)entry);
        ++ThreadCreations;

        return thread;
    }

    return NULL;
}

Thread *ThreadAdoptCurrent(const char *name)
{
    Thread *thread = NULL;

    for (size_t index = 0U; index < THREAD_CAPACITY; ++index)
    {
        if (!ThreadTable[index].used)
        {
            thread = &ThreadTable[index];
            break;
        }
    }

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
    thread->id = ThreadNextId;
    ++ThreadNextId;
    thread->state = THREAD_RUNNING;
    thread->owner = NULL;
    thread->kernel_stack_base = NULL;
    thread->kernel_stack_top = TssKernelStack();
    thread->entry = 0U;
    thread->user_stack = 0U;
    thread->owns_stack = false;
    thread->used = true;
    ++ThreadCreations;

    ProcessCurrentThread = thread;

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
    Thread *const thread = ProcessCurrentThread;

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
    Thread *const caller = ProcessCurrentThread;

    /*
     * Whoever the caller was itself started by, of sub-task 6.11.
     *
     * A program may now start another — a parent that calls `wait` starts its
     * child from within its own system call — so the single variable naming the
     * thread to return to must be saved and put back rather than cleared. The
     * chain of them lives upon the kernel stacks of the calls that made it, one
     * to a stack, which is the shape a stack of callers takes when there is one
     * thread of control and no scheduler to hold a queue.
     *
     * Clearing it instead was correct while nothing nested and would be a
     * particular kind of silent failure now: the parent would end with nobody
     * recorded to return to, and ThreadTerminateCurrent would refuse — leaving
     * the exception path to panic about a program the kernel had itself started.
     */
    Thread *const previous = ProcessReturnThread;

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
     * program ends. There is one such at a time because there is one thread of
     * control until the scheduler of sub-task 6.15; recording it here is what
     * makes a program's death a return rather than a halt.
     */
    ProcessReturnThread = caller;

    ThreadSwitchTo(caller, thread);

    /* Reached when the started thread — or the kernel acting for it — switches
     * back. */
    ProcessReturnThread = previous;
    ThreadSetCurrent(caller);

    return true;
}

bool ThreadTerminateCurrent(int64_t status)
{
    Thread *const thread = ProcessCurrentThread;
    Thread *const back = ProcessReturnThread;

    if ((thread == NULL) || (back == NULL) || (thread == back))
    {
        return false;
    }

    thread->state = THREAD_EXITED;

    if (thread->owner != NULL)
    {
        thread->owner->state = PROCESS_EXITED;
        thread->owner->exit_status = status;
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
    ThreadSwitchTo(thread, back);

    return true;
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

/* ------------------------------------------------------- sub-task 6.11 */

/* Accounting for the four calls. */
static uint64_t ProcessForks;
static uint64_t ProcessExecutions;
static uint64_t ProcessReaps;

Process *ProcessCurrent(void)
{
    return (ProcessCurrentThread != NULL) ? ProcessCurrentThread->owner : NULL;
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

    return child;
}

int64_t ProcessExecute(Process *process, const char *path)
{
    Thread *const thread = ProcessCurrentThread;
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

    stack = ProcessCreateUserStack(process);

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

    /* A thread that reached here by `fork` has a saved user context, and it
     * describes a program that no longer exists. Leaving it set would resume the
     * old program's registers in the new program's address space. */
    thread->resumes_from_fork = false;

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

uint64_t ProcessWait(Process *parent, int64_t *status)
{
    Process *child = NULL;
    uint64_t collected;

    if ((parent == NULL) || !parent->used || (status == NULL))
    {
        return 0U;
    }

    /*
     * A child that has ended is preferred to one that has not.
     *
     * Both are children and either may be collected, but collecting one that has
     * already ended costs nothing, where collecting one that has not means
     * running it to its end first. Taking the finished one first is therefore
     * what makes a parent with several children collect them as they finish
     * rather than in the order the table happens to hold them.
     */
    for (size_t index = 0U; index < PROCESS_CAPACITY; ++index)
    {
        Process *const candidate = &ProcessTable[index];

        if (!candidate->used || (candidate->parent_id != parent->id))
        {
            continue;
        }

        if (candidate->state == PROCESS_EXITED)
        {
            child = candidate;
            break;
        }

        if (child == NULL)
        {
            child = candidate;
        }
    }

    if (child == NULL)
    {
        return 0U;
    }

    /*
     * A child that has not run is run now, here, by its parent's own thread of
     * control — which is what `wait` means while there is no scheduler.
     *
     * This is the sub-task's one substantial departure from the call it is named
     * after, and it is recorded as such rather than disguised: elsewhere a child
     * runs concurrently and `wait` blocks until it finishes, and here the two are
     * the same act. What is preserved is everything a program can observe of the
     * ordering — a child runs after the fork that made it and before the wait
     * that collects it — and what is not is concurrency, which sub-task 6.15
     * supplies. See docs/design/PROCESS.md, Section 13.2.
     */
    if (child->state != PROCESS_EXITED)
    {
        Thread *const thread = (child->thread_count > 0U) ? child->threads[0] : NULL;

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
        }
    }

    collected = child->id;
    *status = child->exit_status;

    /* And the slot, the threads and the address space go back. Nothing else
     * holds the child: its parent named it by number, which is why a parent may
     * be told an identifier that is already nobody's. */
    ProcessDestroy(child);
    ++ProcessReaps;

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
