/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/proc/process.h
 * Purpose: Declares the process control block, the thread structure and the
 *          saved context a switch will exchange: what a program is while it is
 *          running, what runs within it, and what must be put back to resume it.
 * Key definitions: ProcessState, ThreadState, ThreadContext, Thread, Process,
 *          ProcessInitialise, ProcessCreate, ProcessDestroy, ThreadCreate,
 *          ThreadDestroy, ThreadSetCurrent, ThreadCurrent, ProcessRecordImage,
 *          ProcessCreateUserStack, ProcessReport, ProcessFork, ProcessExecute,
 *          ThreadLaunch, ProcessSetInit, ProcessInitId, ProcessAdoptOrphansOf,
 *          ProcessExit, ProcessWait, ProcessWaitFor, ProcessCurrent, ProcessEstablishBreak,
 *          ProcessSetBreak, ProcessBreak, ProcessArguments,
 *          ProcessCloseDescriptors, ProcessAdoptDescriptor,
 *          ProcessDescriptorFile, ProcessReleaseDescriptor.
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
 *   with. Until sub-task 8.6 nothing ran concurrently: a child ran when its
 *   parent waited for it, upon the parent's own flow of control.
 *
 *   Sub-task 8.6 admits a child of `fork` to the scheduler's run queue at the
 *   fork, so that it runs beside its parent — upon the bootstrap processor,
 *   which is where every user thread runs — and `wait` sleeps until a child
 *   ends rather than running one. That is what a pipeline needs: two programs
 *   alive at once, one filling a pipe and the other draining it, each asleep
 *   while the other has the processor.
 */

#ifndef OXYS_PROC_PROCESS_H
#define OXYS_PROC_PROCESS_H

#include <oxys/types.h>
#include <oxys/arch/mm/addrspace.h>
#include <oxys/exec/elf.h>
#include <oxys/arch/syscall/syscall.h>

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
 * What stands upon a user stack before the program's first instruction, of
 * sub-task 7.5.
 *
 * The System V Application Binary Interface, AMD64 supplement, Section 3.4.1,
 * "Stack State", fixes what `_start` finds: the argument count at `%rsp`, the
 * argument pointers at `8+%rsp`, a null pointer terminating them, the
 * environment pointers, a null pointer terminating those, and the auxiliary
 * vector ending with a null entry. `%rsp` "is guaranteed to be 16-byte aligned
 * at process entry".
 *
 * This kernel's `execve` accepts neither vector — there being no convention yet
 * fixed for where the strings go — so every one of those is empty, and the frame
 * is six eightbytes:
 *
 *   +40  padding, so that the frame is a multiple of sixteen
 *   +32  the auxiliary vector's terminating entry, value
 *   +24  the auxiliary vector's terminating entry, AT_NULL
 *   +16  the null pointer ending the environment vector
 *   +8   the null pointer ending the argument vector, argc being zero
 *   +0   the argument count
 *
 * **It is built for every program and not only for a compiled one.** A stack
 * whose first eightbyte is unmapped is a stack upon which the ABI's own first
 * instruction faults, and the programs this project composes by hand do not read
 * it — so a kernel that built the frame only when it thought a program wanted it
 * would be a kernel whose contract depended upon what it guessed. The five
 * eightbytes cost nothing and the contract is then one sentence.
 *
 * The padding is what keeps the alignment. Five eightbytes is forty bytes, and a
 * stack top that is page-aligned less forty is not sixteen-byte aligned; the
 * sixth makes the frame forty-eight, which is. A program entered upon a
 * misaligned stack faults at the first instruction that uses an aligned move,
 * which upon this architecture is somewhere inside a function the program did
 * not write.
 */
#define PROCESS_USER_STACK_FRAME_BYTES 48U
#define PROCESS_USER_STACK_FRAME_WORDS 6U

/*
 * The vectors upon that frame, of sub-task 7.6.
 *
 * Sub-task 7.5 left room for the frame and every eightbyte within it was zero,
 * because this kernel's `execve` refused both vectors. It refused them for want
 * of a convention, and this is the convention: the strings are copied to the top
 * of the new stack, the pointers to them stand below in the order the ABI fixes,
 * and the argument count stands at the stack pointer.
 *
 * `ProcessArguments` is the copy, and it exists because the strings must be
 * taken out of the caller's address space *before* that space is destroyed.
 * `execve` replaces an address space; a kernel that read `argv[0]` after the
 * replacement would read whatever the new program has at that address, which is
 * a fault if it is lucky and the new program's own data if it is not.
 *
 * The bounds are <oxys/syscall_abi.h>'s, restated as sizes here, and they are
 * what makes this structure something a kernel stack can hold: sixteen strings
 * and two kibibytes of them is about two and a half kibibytes in total, against
 * a kernel stack of THREAD_KERNEL_STACK_PAGES pages.
 */
#define PROCESS_ARGUMENT_COUNT_MAXIMUM SYSCALL_ARGUMENT_COUNT_MAXIMUM
#define PROCESS_ARGUMENT_BYTES_MAXIMUM SYSCALL_ARGUMENT_BYTES_MAXIMUM

/* The longest working directory a process may hold, which is the longest path a
 * call accepts: a directory that could be entered but not named would be one a
 * program could not report. Sub-task 8.3. */
#define PROCESS_PATH_MAXIMUM SYSCALL_PATH_MAXIMUM

typedef struct ProcessArguments
{
    /*
     * Where each string begins within `storage`, as a displacement and not as a
     * pointer. A pointer would name an address in this structure, and this
     * structure is a kernel stack frame that has been copied nowhere by the time
     * the strings are written to a user stack: a displacement survives the copy
     * and an address does not.
     */
    uint32_t argument[PROCESS_ARGUMENT_COUNT_MAXIMUM];
    uint32_t environment[PROCESS_ARGUMENT_COUNT_MAXIMUM];
    uint32_t argument_count;
    uint32_t environment_count;

    /* The strings themselves, each terminated, laid end to end. */
    char storage[PROCESS_ARGUMENT_BYTES_MAXIMUM];
    uint32_t storage_used;
} ProcessArguments;

/*
 * How many descriptors a process may hold open at once, of sub-task 7.6.
 *
 * The first three are the standard ones of <oxys/syscall_abi.h> and are never
 * given out by `open`; they name the diagnostic path and are not entries in the
 * filesystem layer's own table. So a process may hold this many less three open
 * files, and the bound is small on purpose: the filesystem layer has
 * VFS_FILE_CAPACITY descriptors for the whole machine, and a process permitted
 * to take more than a share of them could starve every other process of the
 * ability to open anything at all.
 */
#define PROCESS_DESCRIPTOR_CAPACITY 16U

/* What a descriptor slot holds when nothing is open upon it. It is not zero:
 * zero is a valid descriptor of the filesystem layer, and a table cleared to
 * zero would appear to hold that one open in every slot. */
#define PROCESS_DESCRIPTOR_FREE (-1)

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
    PROCESS_STOPPED,    /* Stopped by a signal, until SIGCONT; of sub-task 8.7. */
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

    /*
     * Where the thread returns to when it ends, and what it sleeps upon, of
     * sub-task 8.6.
     *
     * `return_to` is the thread that called ThreadStart upon this one — the
     * kernel's own flow of control, adopted for the purpose — and is null for
     * a thread the scheduler runs: a child of `fork`, which is admitted to a
     * run queue at the fork and is nobody's to return to. Until 8.6 it was one
     * pointer per processor, which was sufficient while a program ran to its
     * end upon its starter's flow of control; it is per thread now because the
     * shell sleeps in `wait` while its children run, and a child that ended
     * while the processor's pointer still named the shell's starter would have
     * returned to the boot flow, with the shell left asleep for ever.
     *
     * `wait_channel` is the object a sleeping thread waits upon — its own
     * process for `wait`, the pipe for a read or a write that cannot proceed
     * — and is what SchedulerWake matches against. A blocked thread whose
     * channel is null is blocked for good, which is what the scheduler's own
     * fixture threads are.
     *
     * The two interrupt-state fields carry the processor's counted disable
     * across a switch, so that a thread which slept from inside its own
     * critical section is resumed inside it and not inside whichever one the
     * thread that ran between had entered; <oxys/arch/cpu/percpu.h> records
     * the failure that occurs otherwise.
     */
    struct Thread *return_to;
    const void *wait_channel;
    uint32_t critical_depth;
    bool interrupts_were_enabled;

    /* Whether this thread was adopted by ThreadAdoptCurrent rather than made
     * — the kernel's own flow of control, which the timer must not take the
     * processor from while it stands inside a self-test. */
    bool adopted;

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

    /*
     * The working directory, of sub-task 8.3: an absolute path, terminated,
     * with no `.` or `..` component and no trailing separator but for the root
     * itself. Every relative path a system call is given is resolved against
     * it — by the call's path copier, in one place, so that no call can forget
     * — and `chdir` is the only thing that changes it, after establishing that
     * what it names is a directory. It is inherited across `fork` and kept
     * across `execve`, as IEEE Std 1003.1-2017 has both.
     *
     * It is a *path* and not a held node, which is the cheaper of the two
     * shapes and the one with a consequence worth recording: a directory that
     * is removed or renamed beneath a process leaves that process with a
     * working directory that names nothing, and its next relative path fails
     * with ENOENT rather than resolving from where it was. Holding the node
     * would need the reference count the filesystem layer does not have.
     */
    char working_directory[PROCESS_PATH_MAXIMUM + 1U];

    /*
     * The descriptors the program holds open, of sub-task 7.6: each entry is a
     * descriptor of the filesystem layer, or PROCESS_DESCRIPTOR_FREE.
     *
     * The table is indexed by the number the program was given, so that the
     * numbers a program sees are its own and small — and, more to the point, so
     * that a program cannot name a descriptor belonging to another process by
     * guessing a number. The filesystem layer's table is one table for the whole
     * machine; without this indirection, descriptor 4 would mean the same open
     * file to every program in the system.
     *
     * The first SYSCALL_DESCRIPTOR_FIRST entries are never used. They are the
     * standard three, which reach the diagnostic path and not a file, and they
     * are left free rather than filled with a sentinel so that exactly one rule
     * governs the table: an entry is a filesystem descriptor or it is nothing.
     */
    int descriptors[PROCESS_DESCRIPTOR_CAPACITY];

    Thread *threads[PROCESS_THREAD_MAXIMUM];
    size_t thread_count;

    int64_t exit_status;

    /*
     * Job control and signals, of sub-task 8.7.
     *
     * `group` is the process group, the identifier of the process that leads
     * it: a child inherits its parent's, `setpgid` moves one, and the terminal
     * delivers control-C and control-Z to every member of its foreground
     * group. `pending` holds one bit per signal, set by SignalSend and cleared
     * by delivery; `handlers` holds a disposition per signal — default,
     * ignore, or the address of a handler — and `restorer` the address a
     * handler returns through. `wait_status` is what `wait` reports, in the
     * encoding of <oxys/syscall_abi.h>, composed when the process ends or
     * stops; `exit_status` above stays the quadword `exit` was given, or the
     * negated vector of a fault, which the self-tests read. `stop_signal` is
     * why a stopped process stopped and `stop_reported` whether its parent
     * has been told; `stop_channel` is the wait channel a stopped thread
     * sleeps upon, chosen for its address alone.
     */
    uint64_t group;
    uint32_t pending;
    uint64_t handlers[SYSCALL_SIGNAL_MAXIMUM + 1U];
    uint64_t restorer;
    uint64_t wait_status;
    uint32_t stop_signal;
    uint32_t termination_signal;
    bool stop_reported;
    uint8_t stop_channel;
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
 *
 * `arguments` is the two vectors the new program is to find upon that stack, and
 * may be null — which is what every caller before sub-task 7.6 passed in effect,
 * and produces the frame of six zeroes that sub-task described. Where it is
 * given, the strings are copied to the top of the stack and the pointers to them
 * laid out below in the order the System V ABI, AMD64 supplement, Section 3.4.1,
 * fixes. The returned address is sixteen-byte aligned either way.
 */
uint64_t ProcessCreateUserStack(Process *process, const ProcessArguments *arguments);

/* -------------------------------------------- the descriptors of sub-task 7.6 */

/*
 * Empties a process's descriptor table.
 *
 * It is called when a process is created and again when `execve` replaces the
 * program within it, and in the second case it *closes* what was open rather
 * than forgetting it: the filesystem layer's table is the machine's, and an
 * entry forgotten here is a descriptor nothing will ever close.
 */
void ProcessCloseDescriptors(Process *process);

/*
 * Gives a process's descriptor table an entry naming an open file of the
 * filesystem layer, and returns the number the program is to use.
 *
 * Returns SYSCALL_EMFILE where the table is full, which is a refusal the caller
 * must act upon by closing the filesystem descriptor: this function takes no
 * ownership of one it did not record.
 */
int64_t ProcessAdoptDescriptor(Process *process, int file);

/*
 * Translates a number a program named into a descriptor of the filesystem layer,
 * or VFS_NO_DESCRIPTOR where the number names nothing this process holds.
 *
 * Every bound is checked here and nowhere else, which is the point of the
 * function: a negative number, one beyond the table and one naming a free slot
 * are three ways of saying the same thing to a caller, and three places for one
 * of them to be forgotten if each call site did its own checking.
 */
int ProcessDescriptorFile(const Process *process, int64_t descriptor);

/*
 * Releases a number a program named, closing the file beneath it.
 *
 * Returns false where the number names nothing this process holds.
 */
bool ProcessReleaseDescriptor(Process *process, int64_t descriptor);

/*
 * Makes the number `to` name what the number `from` names, of sub-task 8.5 —
 * IEEE Std 1003.1-2017's `dup2`. Whatever `to` named is closed first; the
 * open file gains a holder, so the two numbers share one file and one
 * position until both are closed. A number below SYSCALL_DESCRIPTOR_FIRST
 * holding no file is the kernel's own path, which may be given to another of
 * the three and to nothing above them. Returns SYSCALL_OK or SYSCALL_EBADF.
 */
int64_t ProcessPlaceDescriptor(Process *process, int64_t from, int64_t to);

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
 * The thread to return to is recorded upon the started thread, since sub-task
 * 8.6; it was one pointer per processor until then, which sufficed while a
 * program ran to its end upon its starter's flow of control and stopped
 * sufficing when a program could sleep while another ran.
 */
bool ThreadStart(Thread *thread);

/*
 * Hands a thread to the scheduler without waiting for it, of sub-task 9.2: what
 * ThreadStart does by a call, this does by admission, so that the caller and
 * the thread run beside each other rather than one after the other. It is how
 * the entry point starts the window demonstration and then the shell, with
 * neither waiting for the other. Requires the scheduler to be running; returns
 * false, changing nothing, where it is not or where the thread cannot be
 * admitted.
 */
bool ThreadLaunch(Thread *thread);

/*
 * Ends the running thread: returns to whoever started it, or — for a thread
 * the scheduler runs, since sub-task 8.6 — wakes the parent that may be
 * waiting for it and gives the processor to the scheduler.
 *
 * Does not return. Its callers are the exception path, where a program is
 * ended for it — which is what `docs/design/INTERRUPTS.md` has called
 * terminating the program since the dispositions were written — and the `exit`
 * call of sub-task 6.11.
 *
 * Returns false, having done nothing, where the thread was started by a call
 * and there is nobody to return to.
 */
bool ThreadTerminateCurrent(int64_t status);

/* Where the trampoline enters. Declared for the assembly that calls it. */
void ThreadTrampolineEntry(void);

/* How many programs have ended. */
uint64_t ProcessTerminationCount(void);

/*
 * `init`, of sub-task 9.3: the first user process, which the kernel names by
 * its identifier once it has started it. A process that ends with children
 * has them given to `init` — ProcessAdoptOrphansOf, called at the ending —
 * and `init` is woken where one of them had already ended, so that its `wait`
 * collects the orphan at once. Returns how many were given. Nothing is given
 * where there is no `init` yet, or where the ending process is `init` itself.
 */
void ProcessSetInit(uint64_t id);
uint64_t ProcessInitId(void);
size_t ProcessAdoptOrphansOf(uint64_t parent_id);
uint64_t ProcessOrphanCount(void);

/*
 * Suspends the caller until a signal, of sub-task 9.3: POSIX's `pause`, for an
 * `init` that has no child to `wait` upon and must not spin. Returns EINTR.
 */
int64_t ProcessPause(void);

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
 * <oxys/arch/syscall/syscall.h>: SYSCALL_ENOENT where the path names no file or names one
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
 *
 * `arguments` is the two vectors the new program is to find upon its stack,
 * already copied out of the caller's memory by whoever validated them — which
 * must happen before this is called, for the reason `ProcessArguments` records.
 * A null pointer is a program entered with an empty argument vector.
 */
int64_t ProcessExecute(Process *process, const char *path,
                       const ProcessArguments *arguments);

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
 * Collects a child that has ended — sleeping until one does, since sub-task
 * 8.6 — or reports one that has stopped, since 8.7.
 *
 * `pid` selects the children considered, as `waitpid` of IEEE Std 1003.1-2017
 * has it: one child by identifier, -1 for any, and a number below -1 for any
 * child of the group whose identifier is its negation. `options` may hold
 * SYSCALL_WAIT_NO_HANG, upon which nothing to report is 0 and no sleep is
 * made, and SYSCALL_WAIT_UNTRACED, upon which a stopped child is reported —
 * once per stop, without being collected — through `status` in the encoding
 * of <oxys/syscall_abi.h>.
 *
 * Returns the identifier of the child collected or reported; 0 where there is
 * nothing yet and the caller declined to sleep; PROCESS_WAIT_NO_CHILD where
 * no child matches; and PROCESS_WAIT_INTERRUPTED where a signal arrived while
 * the caller slept, which the system call reports as EINTR. A collected
 * child's slot, threads and address space are released before this returns,
 * so the identifier it names is already nobody's by the time the caller sees
 * it — which is why it is returned rather than left to be looked up.
 *
 * ProcessWait is the form that predates 8.7: any child, no options, and 0 for
 * none, which is what its callers — the self-tests — expect.
 */
#define PROCESS_WAIT_NO_CHILD    UINT64_MAX
#define PROCESS_WAIT_INTERRUPTED (UINT64_MAX - 1U)

uint64_t ProcessWaitFor(Process *parent, int64_t pid, uint64_t options, int64_t *status);
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

#endif /* OXYS_PROC_PROCESS_H */
