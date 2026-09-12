/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/process.h
 * Purpose: Declares the process control block, the thread structure and the
 *          saved context a switch will exchange: what a program is while it is
 *          running, what runs within it, and what must be put back to resume it.
 * Key definitions: ProcessState, ThreadState, ThreadContext, Thread, Process,
 *          ProcessInitialise, ProcessCreate, ProcessDestroy, ThreadCreate,
 *          ThreadDestroy, ThreadSetCurrent, ThreadCurrent, ProcessRecordImage,
 *          ProcessCreateUserStack, ProcessReport, ProcessFork, ProcessExecute,
 *          ProcessExit, ProcessWait, ProcessCurrent, ProcessEstablishBreak,
 *          ProcessSetBreak, ProcessBreak.
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
 *
 *   Sub-task 6.11 gives a program the four calls by which it may make another
 *   one: ProcessFork clones a process upon the copy-on-write substrate of Phase
 *   2, ProcessExecute replaces the program a process is running, ProcessExit
 *   ends one on its own request, and ProcessWait collects what a child ended
 *   with. Nothing runs concurrently: a child runs when its parent waits for it,
 *   upon the bootstrap processor, which is where every user thread runs.
 */

#ifndef OXYS_PROCESS_H
#define OXYS_PROCESS_H

#include <oxys/types.h>
#include <oxys/addrspace.h>
#include <oxys/elf.h>
#include <oxys/syscall.h>

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
 * Sixteen kibibytes is four pages, and is the size TSS_KERNEL_STACK_SIZE has
 * given the stack named by `rsp0` since sub-task 6.1. (The boot stack is larger
 * — 64 KiB, established in `boot/boot.asm` — and is not one of these.)
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
 * The heap, and where it begins, of sub-task 7.3.
 *
 * A process's *break* is the address one past the last byte of the region it may
 * use for a heap. It begins immediately above the program's image, rounded up to
 * a page and then advanced by a guard, and it grows upward by the `brk` system
 * call. The stack grows downward from PROCESS_USER_STACK_TOP, so the two
 * approach one another across the whole of the address space between them and
 * the maximum below is what stops either reaching the other.
 *
 * **The guard is not decoration.** The image's highest address is the end of a
 * program's `.bss`, and a program that walks off the end of its last static
 * array would otherwise walk into the first byte of its own heap — where it
 * would find memory that is mapped, writable and holding an allocator's
 * bookkeeping. One unmapped page turns that into a fault at the instruction that
 * caused it.
 *
 * The maximum is a bound upon what one process may ask for, not upon what the
 * machine has. It exists because `brk` maps a frame for every page it grows by:
 * a program asking for its whole address space would otherwise consume every
 * frame in the machine before the request was refused, and the refusal would
 * arrive with nothing left to report it with. Sixteen mebibytes is four thousand
 * and ninety-six pages, which is more than anything this system runs will ask
 * for and far less than the memory a machine running it has.
 */
#define PROCESS_BREAK_GAP_PAGES  1U
#define PROCESS_BREAK_MAXIMUM    UINT64_C(0x0000000001000000)

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
 * sub-task 6.10 does — need save no others: the compiler has already spilled
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

    /*
     * The user registers a forked child resumes upon, of sub-task 6.11.
     *
     * A thread created by `fork` is not entered at an entry point: it continues
     * a program that is already running, at the instruction after the SYSCALL
     * its parent executed, with the whole of its parent's register set. The
     * frame the entry path saved for the parent is therefore copied here with
     * RAX set to zero, which is how a child tells itself apart from its parent.
     *
     * A thread entered at an entry point has every register cleared instead —
     * see docs/design/PROCESS.md, Section 10 — and that would be wrong here in a
     * way nothing would report: the System V convention entitles the code after
     * a call to find RBX, RBP and R12 to R15 as it left them, so a child whose
     * preserved registers had been zeroed would return from `fork` into a frame
     * pointer of nothing and carry on.
     */
    SyscallFrame resume;
    bool resumes_from_fork;

    /* Whether the stack above was taken from the arena and must be given back.
     * The thread describing the kernel's own execution runs upon the boot stack,
     * which the linker established and which is not the arena's to release. */
    bool owns_stack;


    /*
     * Scheduling, of sub-task 6.15.
     *
     * The run-queue link is intrusive because the alternative is an allocation
     * upon every enqueue, and an enqueue happens with a lock held and interrupts
     * masked — which is the one place in this kernel an allocator must not be
     * called from. A thread is upon at most one queue, so one link suffices.
     *
     * The affinity is a bitmask over the processor indices of
     * kernel/include/oxys/percpu.h — this kernel's dense numbering, not the
     * firmware's. It is a hard constraint and not a hint: a thread is never
     * placed upon a processor whose bit is clear. See docs/design/SCHEDULER.md,
     * Section 4, for why a user thread's mask names the bootstrap processor
     * alone, which is a statement about the locks that do not yet exist rather
     * than about scheduling.
     *
     * `processor` is the index of the queue the thread is upon, or that it last
     * ran upon; it is meaningful only while `queued` or while the state is
     * THREAD_RUNNING.
     */
    struct Thread *queue_next;
    uint64_t affinity;
    uint32_t processor;
    bool queued;

    /* How many times this thread has been given a processor, and how many of
     * those ended because its quantum expired rather than because it gave the
     * processor up. The difference is what distinguishes a thread that yields
     * from one that must be taken away. */
    uint64_t slices;
    uint64_t preemptions;
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

    /*
     * The heap, of sub-task 7.3: where it may begin and where it presently ends.
     *
     * Both are recorded here for the reason the image extent is: an address
     * space is a paging hierarchy and cannot say what it maps or why, so the
     * distinction between a heap page and any other mapped page exists in this
     * structure alone. `break_start` is fixed when a program is loaded and never
     * moves; `break_current` is what the program has asked for and is the only
     * one of the two a system call may change.
     *
     * `break_start` is page-aligned and `break_current` need not be. A program
     * may ask for a break part way through a page, and the page it falls within
     * is mapped entire — there being no finer granularity to map with — so the
     * bytes between the break and the end of that page are addressable and are
     * not the program's to use. That is a property of every kernel of this shape
     * and is recorded rather than corrected.
     */
    uint64_t break_start;
    uint64_t break_current;

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

/* The thread a numbered processor is running, of sub-task 6.15. A snapshot of
 * another processor's state, for the report and the self-test; NULL for an index
 * beyond the reservation. */
Thread *ThreadCurrentOn(uint32_t processor);

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
 * A kernel thread made to be handed to the scheduler of sub-task 6.15.
 *
 * The same thing ThreadCreateKernel makes, save that its prepared frame enters a
 * trampoline which closes the critical section the scheduler switched out of.
 * A thread created by ThreadCreateKernel and then admitted would begin with
 * interrupts masked and never be pre-empted; see the note upon
 * PerCpuResetInterruptState.
 */
Thread *ThreadCreateScheduled(void (*entry)(void));

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
 * There is one such caller at a time upon each processor, which is why the
 * thread to return to is one pointer per processor rather than one for the
 * machine.
 */
bool ThreadStart(Thread *thread);

/*
 * Ends the running thread and returns to whoever started it.
 *
 * Does not return. Its one caller is the exception path, where a program is
 * ended for it — which is what `docs/design/INTERRUPTS.md` has called
 * terminating the program since the dispositions were written, and what could
 * not be done until there was somewhere to return to. There is no `exit` system
 * call by which a program may ask to end; the table holds `write`, `ticks` and
 * `version` alone until sub-task 6.11.
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

/* ------------------------------------------------------------------------------
 * Sub-task 6.11: fork, execve, exit and wait.
 * ------------------------------------------------------------------------------ */

/*
 * Makes a child of a process: a second process holding the same memory by the
 * copy-on-write discipline of Phase 2, and one thread prepared to resume where
 * its parent will.
 *
 * The frame is the parent's, as the system-call entry path saved it. It supplies
 * three things that exist nowhere else: the address the parent will return to,
 * which is where the child begins; the parent's stack pointer, which the child
 * inherits because the stack itself is cloned; and the parent's registers, which
 * the child is entitled to find unchanged.
 *
 * The child is created READY and does not run. There is one thread of control
 * upon the bootstrap processor, so what starts a child is its parent
 * asking for it by `wait`; see docs/design/PROCESS.md, Section 13.2.
 *
 * Returns null where a slot, a frame or a paging structure could not be had. The
 * parent is unchanged in that case save for the pages the attempt protected,
 * which is a loss of speed and not of correctness.
 */
Process *ProcessFork(Process *parent, const SyscallFrame *frame);

/*
 * Replaces the program a process is running with one loaded from a file.
 *
 * Upon success **this does not return**: the process's old address space has
 * been released, a new one built from the image, a fresh user stack given, and
 * the calling thread has descended to privilege level 3 at the new entry point.
 * The kernel stack the call arrived upon is abandoned where it stands, which
 * costs nothing — the next entry from privilege level 3 begins at its top again.
 *
 * Returns, having changed nothing, one of the SYSCALL_ result values of
 * <oxys/syscall.h>: SYSCALL_ENOENT where the path names no file or names one
 * this loader will not load, SYSCALL_ENOMEM where a frame or a paging structure
 * could not be had, and SYSCALL_EINVAL where the arguments are not this
 * process's to act upon.
 *
 * The three are distinguished rather than collapsed into one refusal. A program
 * told that its file does not exist, when what happened was that the machine ran
 * out of memory, would look for the fault in the one place it is not — and would
 * be told the same thing however many times it looked.
 *
 * Beyond the point of no return a failure is fatal to the process rather than to
 * the call, because a process whose address space has been released has no
 * program left to return to.
 */
int64_t ProcessExecute(Process *process, const char *path);

/*
 * Ends the process the running thread belongs to, with a status, and returns to
 * whoever started it. Does not return.
 *
 * This is a fourth way back from privilege level 3, and not one of the three
 * docs/design/PROCESS.md, Section 10.1, named — those being the fault, the
 * system-call return, and the pre-emption that does not yet exist. A program
 * could fault its way out and could be returned to by SYSRET; what it could not
 * do was say that it had finished.
 */
void ProcessExit(int64_t status);

/*
 * Collects a child that has ended, running it first if it has not yet run.
 *
 * Returns the identifier of the child collected and places its status through
 * `status`, or zero where the caller has no children. The child's slot, its
 * threads and its address space are released before this returns, so the
 * identifier it names is already nobody's by the time the caller sees it — which
 * is why it is returned rather than left to be looked up.
 */
uint64_t ProcessWait(Process *parent, int64_t *status);

/* The process the running thread belongs to, or null where the running thread
 * has none — which is every thread of the kernel's own. */
Process *ProcessCurrent(void);

/* Accounting. */
uint64_t ProcessForkCount(void);
uint64_t ProcessExecuteCount(void);
uint64_t ProcessReapCount(void);

/* ------------------------------------------------------------------------------
 * Sub-task 7.3: the break, which is where a program's heap ends.
 * ------------------------------------------------------------------------------ */

/*
 * Fixes where a process's heap may begin, from the image it is running.
 *
 * Called whenever the image changes and at no other time: once when a program is
 * first loaded and again when `execve` replaces it. The break is placed a guard
 * page above the end of the image and the process begins with a heap of no
 * bytes, which is the state in which the first request for memory maps the first
 * page.
 *
 * A process whose image was never recorded — one that has been created and not
 * loaded — keeps a break of zero, and every request against it is refused. That
 * is deliberate: a heap placed at an address derived from an image that does not
 * exist would be placed at zero, which is the one page in the address space that
 * must stay unmapped.
 */
void ProcessEstablishBreak(Process *process);

/*
 * Moves a process's break to the requested address and returns where it stands
 * afterwards, or one of the SYSCALL_ results of <oxys/syscall_abi.h>.
 *
 * Growing maps a zeroed, writable, user-accessible frame for every page the
 * region gains; shrinking withdraws the pages the region has given up and
 * releases their frames. **A growth that cannot be completed is undone**: a
 * caller told that it has memory it has not got would discover otherwise at some
 * later instruction, so the pages mapped by a failed attempt are withdrawn again
 * and the break is left where it was.
 *
 * Returns SYSCALL_EINVAL for an address below where the heap begins, and
 * SYSCALL_ENOMEM for one beyond PROCESS_BREAK_MAXIMUM or where a frame could not
 * be had.
 */
int64_t ProcessSetBreak(Process *process, uint64_t requested);

/* Where a process's break stands, or zero where it has no heap. */
uint64_t ProcessBreak(const Process *process);

/*
 * Accounting: how many requests moved the break each way, and how many pages the
 * call has mapped and not itself withdrawn.
 *
 * The third is a count of what this call did and not a census of the machine. A
 * process destroyed while it holds a heap releases its pages through
 * AddressSpaceDestroy without passing through here, and a forked child's heap
 * pages are shared rather than mapped, so neither event is visible to it.
 */
uint64_t ProcessBreakGrowthCount(void);
uint64_t ProcessBreakShrinkCount(void);
uint64_t ProcessBreakPageCount(void);

#endif /* OXYS_PROCESS_H */
