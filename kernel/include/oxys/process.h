/*
 * File: kernel/include/oxys/process.h
 * Purpose: Declares the process control block, the thread structure and the
 *          saved context a switch will exchange: what a program is while it is
 *          running, what runs within it, and what must be put back to resume it.
 * Key definitions: ProcessState, ThreadState, ThreadContext, Thread, Process,
 *          ProcessInitialise, ProcessCreate, ProcessDestroy, ThreadCreate,
 *          ThreadDestroy, ThreadSetCurrent, ThreadCurrent, ProcessRecordImage,
 *          ProcessCreateUserStack, ProcessReport.
 * References:
 *   - docs/design/PROCESS.md: the design of these structures and the reasons for
 *     their shape.
 *   - docs/design/PRIVILEGE.md, Section 3.3 and limitations 4 and 6: each thread
 *     has a kernel stack of its own, taken from the arena with a guard beneath
 *     it, and `rsp0` is written whenever the thread that owns it becomes the
 *     current one.
 *   - docs/design/MEMORY-LAYOUT.md, limitation 2: an address space has no record
 *     of its own extent, and the process control block is where that record
 *     belongs.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.2.1:
 *     RBX, RBP, R12, R13, R14 and R15 are preserved across a call, so a switch
 *     performed by an ordinary function call need save no others.
 *
 * What this sub-task defines and what it deliberately does not do.
 *
 *   Sub-task 6.9 defined the structures and the tables that hold them, allocated
 *   and released them, and gave each thread the stack it would be entered upon,
 *   without switching to anything: the division was deliberate, a structure that
 *   has never been switched to being one whose shape can still be argued about.
 *
 *   Sub-task 6.10 switches. ThreadSwitchTo exchanges one thread's execution for
 *   another's, ThreadStart descends to privilege level 3, and
 *   ThreadTerminateCurrent is how a program that has ended gives the processor
 *   back — which is what made a fault outside the kernel survivable.
 */

#ifndef OXYS_PROCESS_H
#define OXYS_PROCESS_H

#include <oxys/types.h>
#include <oxys/addrspace.h>
#include <oxys/elf.h>

/* How many processes and threads may exist at once. */
#define PROCESS_CAPACITY 64U
#define THREAD_CAPACITY  128U

/* How many threads one process may hold. */
#define PROCESS_THREAD_MAXIMUM 8U

/* The greatest length of a process's name, excluding its terminator. */
#define PROCESS_NAME_MAXIMUM 31U

/*
 * The kernel stack each thread is given, in pages, and the guard beneath it.
 *
 * Sixteen kibibytes is four pages and is what the boot stack has been all along.
 * The guard is one page below it, and it exists because
 * `docs/design/PRIVILEGE.md` promised it here: with one stack an overflow ran
 * into the `.bss` and happened to be caught by the double-fault stack, which was
 * an accident of placement; with a stack per thread the stacks become numerous
 * and the accident stops holding.
 */
#define THREAD_KERNEL_STACK_PAGES 4U
#define THREAD_GUARD_PAGES        1U

/*
 * The user stack, and where its top is placed.
 *
 * Immediately below the boundary of what a user program may occupy, growing
 * downward, with a guard page beneath it. Placed there rather than after the
 * program's own segments because the two must not meet: a stack that grew into
 * the program's data would corrupt it silently, and the whole point of putting
 * the stack at the far end of the address space is that the gap between them is
 * the size of the address space.
 */
#define PROCESS_USER_STACK_PAGES 16U
#define PROCESS_USER_STACK_TOP   UINT64_C(0x0000700000000000)

/*
 * What a process is doing. The states are what a scheduler will need to tell
 * apart, and they are defined here rather than there so that the structure is
 * complete before anything acts upon it.
 */
typedef enum ProcessState
{
    PROCESS_UNUSED = 0, /* The slot holds no process. */
    PROCESS_CREATED,    /* Built, and never yet runnable. */
    PROCESS_READY,      /* Runnable, and not running. */
    PROCESS_RUNNING,    /* Upon a processor now. */
    PROCESS_BLOCKED,    /* Waiting for something that has not happened. */
    PROCESS_EXITED      /* Finished; its status has not yet been collected. */
} ProcessState;

/* The same, for a thread. A process's state is not the conjunction of its
 * threads': a process with one blocked thread and one running thread is
 * running, and deciding that is the scheduler's business and not this file's. */
typedef enum ThreadState
{
    THREAD_UNUSED = 0,
    THREAD_CREATED,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_EXITED
} ThreadState;

/*
 * What a switch must put back.
 *
 * The six callee-saved registers and the stack pointer, and nothing else. The
 * System V convention provides that RBX, RBP and R12 to R15 are preserved across
 * a call, so a switch performed *as an ordinary function call* — which is what
 * sub-task 6.10 will do — need save no others: the compiler has already spilled
 * anything else it cared about at the call site.
 *
 * The instruction pointer is not among them and is not an omission. A switch
 * that returns to its caller resumes at the return address upon the stack, so
 * the stack pointer carries the instruction pointer with it. A thread that has
 * never run has no such address, which is what `entry` is for.
 */
typedef struct ThreadContext
{
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
} ThreadContext;

typedef struct Process Process;

/*
 * A thread: something that runs, and the stack it runs upon.
 *
 * The kernel stack belongs to the thread and not to the processor. That is the
 * whole reason this structure exists separately from the process: two threads of
 * one process share every page of memory and must not share the stack the kernel
 * is entered upon, or a system call made by one would return into the other.
 */
typedef struct Thread
{
    uint64_t id;
    ThreadState state;
    Process *owner;

    /* The kernel stack: the lowest address of the whole reservation, the guard
     * page included, and the address the stack pointer starts at, which is its
     * top. The two differ by the guard. */
    void *kernel_stack_base;
    uint64_t kernel_stack_top;

    /* Where the thread begins, and upon what, when it is first entered. Both are
     * user addresses and neither is touched by this file. */
    uint64_t entry;
    uint64_t user_stack;

    ThreadContext context;

    /* Whether the stack above was taken from the arena and must be given back.
     * The thread describing the kernel's own execution runs upon the boot stack,
     * which the linker established and which is not the arena's to release. */
    bool owns_stack;

    bool used;
} Thread;

/*
 * A process: an address space, the threads within it, and what is known about
 * the program it is running.
 *
 * The extent fields are the record `docs/design/MEMORY-LAYOUT.md` asked for. An
 * address space is a paging hierarchy and nothing besides, so nothing could
 * answer what it maps without walking it; a process knows, because a process is
 * what put things there.
 */
struct Process
{
    uint64_t id;
    uint64_t parent_id;
    ProcessState state;
    char name[PROCESS_NAME_MAXIMUM + 1U];

    AddressSpace space;

    /* What was loaded into it, and where. */
    uint64_t image_lowest;
    uint64_t image_highest;
    uint64_t image_entry;
    uint64_t mapped_pages;

    /* The user stack, and its extent. */
    uint64_t user_stack_top;
    uint64_t user_stack_pages;

    Thread *threads[PROCESS_THREAD_MAXIMUM];
    size_t thread_count;

    int64_t exit_status;
    bool used;
};

/* Empties both tables. Called once, before anything is created. */
void ProcessInitialise(void);

/*
 * Creates a process with an address space of its own and no threads.
 *
 * The parent is recorded by identifier and not by pointer: a parent may be
 * destroyed while a child lives, and a pointer to a slot that has since been
 * given to somebody else is worse than a number that names nobody.
 *
 * Returns null where the table is full or an address space could not be made.
 */
Process *ProcessCreate(const char *name, const Process *parent);

/*
 * Destroys a process: its threads, its address space, and its slot.
 *
 * Its threads are destroyed with it. A thread outliving its process would have a
 * pointer to a slot that no longer describes anything, and there is no
 * circumstance in which one should.
 */
void ProcessDestroy(Process *process);

/*
 * Creates a thread within a process, with a kernel stack of its own.
 *
 * The stack comes from the kernel arena with a guard page beneath it, and the
 * context is prepared so that the thread's stack pointer names the top of it.
 *
 * Returns null where either table is full, where the process already holds
 * PROCESS_THREAD_MAXIMUM threads, or where the arena could not supply a stack.
 */
Thread *ThreadCreate(Process *owner, uint64_t entry, uint64_t user_stack);

/* Destroys a thread and releases its stack. */
void ThreadDestroy(Thread *thread);

/*
 * Records that a thread is the one now running, and writes the top of its kernel
 * stack into the task state segment.
 *
 * The second half is the point. `rsp0` is what the processor loads upon a
 * transfer from privilege level 3, so a thread entered while `rsp0` still names
 * another thread's stack takes its first interrupt onto a stack somebody else is
 * using. `TssSetKernelStack` has existed and been uncalled since sub-task 6.1
 * for exactly this moment.
 */
void ThreadSetCurrent(Thread *thread);

/* The thread most recently made current, or null. */
Thread *ThreadCurrent(void);

/* Records what an image occupied, once it has been loaded into the process. */
void ProcessRecordImage(Process *process, const ElfImage *image);

/*
 * Gives a process a stack for its user code, below PROCESS_USER_STACK_TOP and
 * growing downward, with a guard page beneath it.
 *
 * Returns the address the stack pointer should begin at, or zero. The address is
 * the top and not the base: a stack grows downward, and the first push writes
 * below it.
 */
uint64_t ProcessCreateUserStack(Process *process);

/* The tables, for a report and a self-test. */
Process *ProcessById(uint64_t id);
Thread *ThreadById(uint64_t id);
Process *ProcessAt(size_t index);
Thread *ThreadAt(size_t index);
size_t ProcessCount(void);
size_t ThreadCount(void);

/* The names of the states, for a report. */
const char *ProcessStateName(ProcessState state);
const char *ThreadStateName(ThreadState state);

/* Accounting. */
uint64_t ProcessesCreated(void);
uint64_t ThreadsCreated(void);

/* ------------------------------------------------------------------------------
 * Sub-task 6.10: switching, and the descent to privilege level 3.
 * ------------------------------------------------------------------------------ */

/*
 * Describes the execution already in progress as a thread, so that something may
 * be switched away from it and back to it.
 *
 * It owns no stack: it runs upon the boot stack, which the linker established.
 * Its context is left as it stands, nothing reading it until the first switch
 * away fills it in.
 */
Thread *ThreadAdoptCurrent(const char *name);

/*
 * Creates a thread that runs kernel code at privilege level 0, with a stack of
 * its own and no process.
 *
 * It exists so that the switch may be asserted without a program, an address
 * space or a privilege transition being involved at all: a failure there is a
 * failure of the switch, where a failure in the descent could be a failure of
 * anything.
 */
Thread *ThreadCreateKernel(void (*entry)(void));

/*
 * Exchanges the running thread for another: the address space, `rsp0`, and then
 * the registers and the stack.
 *
 * Returns when somebody switches back to `from`. A thread that is never switched
 * back to never returns from this, which is what happens to a thread that ends.
 */
void ThreadSwitchTo(Thread *from, Thread *to);

/*
 * Starts a thread at privilege level 3 and waits for it to end.
 *
 * The caller becomes the thread the program will be returned to when it ends,
 * whether it ends by asking or by faulting. Returns false where the thread has
 * no entry point or no stack, and true once the program has ended.
 *
 * There is one such caller at a time because there is one thread of control
 * until the scheduler of sub-task 6.15.
 */
bool ThreadStart(Thread *thread);

/*
 * Ends the running thread and returns to whoever started it.
 *
 * Does not return. Called from the system-call path when a program asks to end,
 * and from the exception path when a program is ended for it — which is what
 * `docs/design/INTERRUPTS.md` has called terminating the program since the
 * dispositions were written, and what could not be done until there was
 * somewhere to return to.
 *
 * Returns false, having done nothing, where there is nobody to return to.
 */
bool ThreadTerminateCurrent(int64_t status);

/* Where the trampoline enters. Declared for the assembly that calls it. */
void ThreadTrampolineEntry(void);

/* How many programs have ended. */
uint64_t ProcessTerminationCount(void);

/* Emits the tables upon the diagnostic path. */
void ProcessReport(void);

#endif /* OXYS_PROCESS_H */
