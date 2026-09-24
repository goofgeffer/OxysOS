/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/utilities.c
 * Purpose: Asserts the work of sub-task 7.6 — the six filesystem system calls,
 *          the argument vector `execve` now carries, and the five utilities
 *          built above them — by running each program at privilege level 3 upon
 *          a volume this kernel composed and then inspecting what it did to it.
 * Key functions: KernelVerifyUtilities.
 * References:
 *   - docs/design/LIBC.md: what this asserts, what each program
 *     asserts of itself, and what nothing here can assert.
 *   - userland/arg-check/main.c: the program that compares the vector it was
 *     given against the vector it expects, and ends with the number of
 *     comparisons that failed.
 *   - userland/exec-check/main.c: the program that reaches `arg-check` through
 *     `execve`, so that the vector crosses an address space that is destroyed.
 *   - kernel/test/libc/utilities_image.asm: the seven programs, embedded here.
 *   - kernel/test/volume.h: the composed EXT2 volume every path below resolves
 *     against.
 *   - IEEE Std 1003.1-2017: the exit statuses each utility is required to
 *     produce, which are what the assertions below are written against.
 *
 * **What this asserts, and the one thing it cannot.**
 *
 *   It asserts what a program *did*: the status it ended with, and the state of
 *   the volume afterwards — that `mkdir` left a directory where there was none,
 *   that `rm` removed a name, that neither touched anything else, and that no
 *   program ended holding a descriptor.
 *
 *   It cannot assert what a program *printed*. `cat` writes its bytes to the
 *   diagnostic path through `write`, and nothing in this kernel captures that
 *   path — so `cat` copying the wrong file would satisfy every assertion here
 *   and be visible only to a person reading the serial log. That is why
 *   `arg-check` exists and why the utilities are given operands whose *absence*
 *   is detectable: a `cat` that could not open its file exits with a failure,
 *   and a `cat` that opened a directory exits with a failure, and those two are
 *   machine-readable statements about the calls beneath it.
 *   docs/design/LIBC.md.
 *
 * Why the negative cases outnumber the positive ones.
 *
 *   Because a status of zero is the weakest evidence a program can offer: a
 *   utility that did nothing at all would produce one. Every positive case here
 *   is therefore paired with the same program given something it must refuse,
 *   and the pair together says that the program can tell the two apart.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include "../volume.h"
#include <oxys/proc/process.h>
#include <oxys/exec/elf.h>
#include <oxys/mm/memory.h>
#include <oxys/fs/vfs.h>
#include <oxys/block/block.h>
#include <oxys/arch/syscall/syscall.h>
#include <oxys/terminal/terminal.h>

/*
 * The device the composed volume is presented through, and it is a name of its
 * own for the reason kernel/test/proc/lifecycle.c records: the buffer cache is
 * keyed by device, so a volume recomposed beneath a device the cache still holds
 * blocks for would be read as it was before the recomposition.
 */
#define KERNEL_UTILITIES_DEVICE "mem4"

/* Where `arg-check` is written upon the volume. userland/exec-check/main.c names
 * this same path in its `execve`, and the two must agree. */
#define KERNEL_UTILITIES_PROGRAM_PATH "/bin/arg-check"

/* The tree the utilities are pointed at. It is made by this test through the
 * filesystem layer directly, so that a utility which fails is not also the thing
 * that was supposed to have prepared the ground. */
#define KERNEL_UTILITIES_SCRATCH  "/scratch"
#define KERNEL_UTILITIES_FIRST    "/scratch/one"
#define KERNEL_UTILITIES_SECOND   "/scratch/two"
#define KERNEL_UTILITIES_HIDDEN   "/scratch/.hidden"
#define KERNEL_UTILITIES_VICTIM   "/scratch/victim"

/* The paths `mkdir` is asked to make, and which must not exist beforehand. */
#define KERNEL_UTILITIES_MADE     "/made"
#define KERNEL_UTILITIES_DEEP     "/made/a/b/c"

/* What a utility exits with when it failed. Every one of the five returns
 * EXIT_FAILURE, which <stdlib.h> defines as 1, and `arg-check` returns a count
 * — so a status of exactly 1 from a utility is "it reported a failure" and any
 * other non-zero status is something this test did not predict. */
#define KERNEL_UTILITIES_FAILURE 1

/* The programs, placed here by kernel/test/libc/utilities_image.asm. They are
 * arrays of unknown size and not pointers, for the reason
 * kernel/test/libc/startup.c records: a linker symbol has an address and no
 * storage. */
extern const uint8_t KernelProgramArgCheckBegin[];
extern const uint8_t KernelProgramArgCheckEnd[];
extern const uint8_t KernelProgramExecCheckBegin[];
extern const uint8_t KernelProgramExecCheckEnd[];
extern const uint8_t KernelProgramFileCheckBegin[];
extern const uint8_t KernelProgramFileCheckEnd[];
extern const uint8_t KernelProgramEchoBegin[];
extern const uint8_t KernelProgramEchoEnd[];
extern const uint8_t KernelProgramCatBegin[];
extern const uint8_t KernelProgramCatEnd[];
extern const uint8_t KernelProgramListBegin[];
extern const uint8_t KernelProgramListEnd[];
extern const uint8_t KernelProgramMakeDirBegin[];
extern const uint8_t KernelProgramMakeDirEnd[];
extern const uint8_t KernelProgramRemoveBegin[];
extern const uint8_t KernelProgramRemoveEnd[];

static bool KernelUtilitiesSucceeded;

static void KernelUtilitiesRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        KernelUtilitiesSucceeded = false;
    }
}

/* The thread the kernel is executing upon, adopted once so that every program
 * below has something to return to. */
static Thread *KernelUtilitiesBoot;

/*
 * Composes the argument block a program is to be entered with, out of a
 * null-terminated list of strings.
 *
 * It is built here rather than by the system call's own copier, because these
 * strings stand in the kernel's read-only data and not in a user address space
 * — which is the whole difference between the two paths this sub-task adds, and
 * the reason `exec-check` exists to exercise the other one.
 */
static bool KernelUtilitiesArguments(ProcessArguments *arguments,
                                     const char *const *strings)
{
    arguments->argument_count = 0U;
    arguments->environment_count = 0U;
    arguments->storage_used = 0U;

    for (size_t index = 0U; strings[index] != NULL; ++index)
    {
        const char *const string = strings[index];
        size_t length = 0U;

        while (string[length] != '\0')
        {
            ++length;
        }

        if ((arguments->argument_count >= PROCESS_ARGUMENT_COUNT_MAXIMUM) ||
            ((arguments->storage_used + length + 1U) > PROCESS_ARGUMENT_BYTES_MAXIMUM))
        {
            return false;
        }

        arguments->argument[arguments->argument_count] = arguments->storage_used;
        ++arguments->argument_count;

        for (size_t offset = 0U; offset <= length; ++offset)
        {
            arguments->storage[arguments->storage_used + offset] = string[offset];
        }

        arguments->storage_used += (uint32_t)(length + 1U);
    }

    return true;
}

/*
 * Loads a program, runs it with the given arguments, and reports the status it
 * ended with.
 *
 * Returns false where the program could not be prepared or started at all, which
 * is a different failure from a program that ran and ended badly: the first says
 * this test is broken and the second says the program is.
 *
 * The count of open files is taken before and after, and the process is
 * destroyed here. Together those assert what nothing else can: that a program
 * which ended holding a descriptor did not cost the machine one permanently.
 */
static bool KernelUtilitiesRun(const char *name, const uint8_t *image, uint64_t length,
                               const char *const *strings, int64_t *status)
{
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();

    if (!KernelUtilitiesArguments(&arguments, strings))
    {
        KernelUtilitiesRequire(false, "the argument block could not be composed");

        return false;
    }

    process = ProcessCreate(name, NULL);

    if (process == NULL)
    {
        KernelUtilitiesRequire(false, "a process could not be created for a program");

        return false;
    }

    if (ElfLoad(&process->space, image, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        KernelUtilitiesRequire(false, "a program did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        KernelUtilitiesRequire(false, "a program was given no stack, so its arguments "
                                      "did not fit upon one");

        return false;
    }

    /*
     * The stack pointer is sixteen-byte aligned, which the System V ABI, AMD64
     * supplement, Section 3.4.1, guarantees a program at its entry point. It is
     * asserted here rather than left to the program, because a misaligned stack
     * faults inside a library function the program did not write and the fault
     * says nothing about the stack.
     */
    KernelUtilitiesRequire((stack % 16U) == 0U,
                           "the stack a program is entered upon is not sixteen-byte "
                           "aligned");

    thread = ThreadCreate(process, loaded.entry, stack);

    if (thread == NULL)
    {
        ProcessDestroy(process);
        KernelUtilitiesRequire(false, "a thread could not be created for a program");

        return false;
    }

    if (!ThreadStart(thread))
    {
        ProcessDestroy(process);
        KernelUtilitiesRequire(false, "a program could not be started");

        return false;
    }

    KernelUtilitiesRequire(ThreadCurrent() == KernelUtilitiesBoot,
                           "the kernel did not resume the thread that started a program");
    KernelUtilitiesRequire(process->state == PROCESS_EXITED,
                           "a program's process was not marked as ended");

    *status = process->exit_status;

    ProcessDestroy(process);

    KernelUtilitiesRequire(VfsOpenFileCount() == open_before,
                           "a program that ended left an open file behind it");

    return true;
}

/* Runs a program and asserts the status it ended with. The two are separated so
 * that a program which could not be started at all is reported as that rather
 * than as an unexpected status. */
static void KernelUtilitiesExpect(const char *name, const uint8_t *image,
                                  uint64_t length, const char *const *strings,
                                  int64_t expected, const char *statement)
{
    int64_t status = 0;

    if (!KernelUtilitiesRun(name, image, length, strings, &status))
    {
        return;
    }

    if (status != expected)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        /*
         * The status is written as hexadecimal and not as a decimal number,
         * because it may be negative — a program ended by a fault carries the
         * negated vector — and this kernel's only decimal routine takes an
         * unsigned value. A negative status printed through that would appear as
         * eighteen quintillion and say nothing.
         */
        KernelWriteString(" FAILED: the status was ");
        KernelWriteHexadecimal((uint64_t)status);
        KernelWriteString(" and not ");
        KernelWriteHexadecimal((uint64_t)expected);
        KernelWriteString(".\n");
        KernelUtilitiesSucceeded = false;
    }
}

/* Writes a file of known contents through the filesystem layer. Returns false
 * having said what went wrong. */
static bool KernelUtilitiesCreate(const char *path, const char *contents)
{
    uint64_t length = 0U;
    uint64_t written = 0U;
    int descriptor;

    while (contents[length] != '\0')
    {
        ++length;
    }

    descriptor = VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                         0644U);

    if (descriptor < 0)
    {
        KernelWriteString("  A file of the fixture could not be created: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");

        return false;
    }

    if (!VfsWrite(descriptor, contents, length, &written) || (written != length))
    {
        KernelWriteString("  A file of the fixture could not be written.\n");
        (void)VfsClose(descriptor);

        return false;
    }

    return VfsClose(descriptor);
}

/* Writes `arg-check` onto the volume, so that `exec-check` has a path to name. */
static bool KernelUtilitiesPublish(void)
{
    const uint64_t length =
        (uint64_t)(KernelProgramArgCheckEnd - KernelProgramArgCheckBegin);
    uint64_t written = 0U;
    int descriptor;

    if (!VfsCreateDirectory("/bin", 0755U))
    {
        KernelWriteString("  /bin could not be created: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");

        return false;
    }

    descriptor = VfsOpen(KERNEL_UTILITIES_PROGRAM_PATH,
                         VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, 0755U);

    if (descriptor < 0)
    {
        KernelWriteString("  The program could not be created upon the volume: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");

        return false;
    }

    /*
     * The write is made in pieces, because VfsWrite reports what it transferred
     * and is entitled to transfer less than it was asked for. A single call
     * whose result was compared against the length would fail upon a filesystem
     * that answered in block-sized pieces, and would fail as "the program could
     * not be written" rather than as what it is.
     */
    while (written < length)
    {
        uint64_t piece = 0U;

        if (!VfsWrite(descriptor, &KernelProgramArgCheckBegin[written], length - written,
                      &piece) ||
            (piece == 0U))
        {
            KernelWriteString("  The program could not be written to the volume: ");
            KernelWriteString(VfsLastError());
            KernelWriteString("\n");
            (void)VfsClose(descriptor);

            return false;
        }

        written += piece;
    }

    return VfsClose(descriptor);
}

/* Builds the tree the utilities operate upon. */
static bool KernelUtilitiesCompose(void)
{
    if (!VfsCreateDirectory(KERNEL_UTILITIES_SCRATCH, 0755U))
    {
        KernelWriteString("  The scratch directory could not be created: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");

        return false;
    }

    return KernelUtilitiesCreate(KERNEL_UTILITIES_FIRST, "the first file\n") &&
           KernelUtilitiesCreate(KERNEL_UTILITIES_SECOND, "the second file\n") &&
           KernelUtilitiesCreate(KERNEL_UTILITIES_HIDDEN, "hidden\n") &&
           KernelUtilitiesCreate(KERNEL_UTILITIES_VICTIM, "to be removed\n");
}

/* Whether a path names something, and whether it names a directory. Both are
 * asked through the filesystem layer rather than through a utility, so that the
 * thing under test is not also the witness. */
static bool KernelUtilitiesExists(const char *path)
{
    VfsAttributes attributes;

    return VfsStat(path, &attributes);
}

static bool KernelUtilitiesIsDirectory(const char *path)
{
    VfsAttributes attributes;

    return VfsStat(path, &attributes) && (attributes.type == VFS_NODE_DIRECTORY);
}

/*
 * The descriptor table itself, asserted from within the kernel rather than
 * through a program.
 *
 * This is the division sub-tasks 7.3 and 7.4 made and it is made here for the
 * third time: the *policy* — which numbers are given out, which are refused,
 * what a full table does, what a destruction releases — is ordinary code that
 * can be called directly, and the system calls above it are asserted by
 * programs. Two of its properties cannot be reached by a program at all:
 *
 *   **The table is emptied and not zeroed.** A table cleared to zero would name
 *   filesystem descriptor 0 in every slot, and the first `close` of a fresh
 *   process would close a file belonging to somebody else. Nothing a program can
 *   do distinguishes that from a correct table until the machine has two
 *   processes with files open, at which point it is a corruption and not a
 *   diagnostic.
 *
 *   **A process that ends holding a descriptor gives it back.** No program of
 *   this sub-task ends holding one — each closes what it opened — so the
 *   assertion in `KernelUtilitiesRun` that the count is unchanged would pass
 *   whether or not `ProcessDestroy` released anything. Here the descriptor is
 *   deliberately left open and the count is what reports it.
 */
static void KernelUtilitiesDescriptors(void)
{
    Process *const process = ProcessCreate("descriptors", NULL);
    const size_t open_before = VfsOpenFileCount();
    int64_t descriptors[PROCESS_DESCRIPTOR_CAPACITY];
    size_t adopted = 0U;
    int file;

    if (process == NULL)
    {
        KernelUtilitiesRequire(false, "a process could not be created for the descriptor "
                                      "assertions");

        return;
    }

    /*
     * Every slot is free and none of them names filesystem descriptor 0, which
     * is the property a table cleared with memset would not have.
     */
    for (size_t index = 0U; index < PROCESS_DESCRIPTOR_CAPACITY; ++index)
    {
        if (process->descriptors[index] != PROCESS_DESCRIPTOR_FREE)
        {
            KernelUtilitiesRequire(false, "a fresh process holds a descriptor, so the "
                                          "table was cleared rather than emptied");
            break;
        }
    }

    /* The numbers below SYSCALL_DESCRIPTOR_FIRST name the diagnostic path and
     * are not this table's to translate. */
    KernelUtilitiesRequire(ProcessDescriptorFile(process, 0) == VFS_NO_DESCRIPTOR,
                           "the standard input was translated as though it named a file");
    KernelUtilitiesRequire(ProcessDescriptorFile(process, 1) == VFS_NO_DESCRIPTOR,
                           "the standard output was translated as though it named a file");
    KernelUtilitiesRequire(ProcessDescriptorFile(process, -1) == VFS_NO_DESCRIPTOR,
                           "a negative descriptor was translated rather than refused");
    KernelUtilitiesRequire(
        ProcessDescriptorFile(process, (int64_t)PROCESS_DESCRIPTOR_CAPACITY) ==
            VFS_NO_DESCRIPTOR,
        "a descriptor beyond the table was translated rather than refused");

    /*
     * The table is filled by opening the same file repeatedly. The filesystem
     * layer permits it — two descriptors upon one file have positions of their
     * own — and it is the only way to exhaust a table of sixteen upon a volume
     * this small.
     */
    for (;;)
    {
        int64_t number;

        file = VfsOpen(KERNEL_UTILITIES_FIRST, VFS_OPEN_READ, 0U);

        if (file == VFS_NO_DESCRIPTOR)
        {
            KernelUtilitiesRequire(false, "the fixture file could not be opened");
            break;
        }

        number = ProcessAdoptDescriptor(process, file);

        if (number < 0)
        {
            KernelUtilitiesRequire(number == SYSCALL_EMFILE,
                                   "a full descriptor table was refused for some reason "
                                   "other than being full");

            /*
             * The refusal takes no ownership of the open file, which is the one
             * thing about this function a caller must get right — so the caller
             * closes it, and the count below asserts that doing so was enough.
             */
            (void)VfsClose(file);
            break;
        }

        KernelUtilitiesRequire(number >= (int64_t)SYSCALL_DESCRIPTOR_FIRST,
                               "a descriptor below the standard three was given out");
        KernelUtilitiesRequire(ProcessDescriptorFile(process, number) == file,
                               "a descriptor does not translate to the file it was "
                               "adopted for");

        descriptors[adopted] = number;
        ++adopted;

        if (adopted > PROCESS_DESCRIPTOR_CAPACITY)
        {
            KernelUtilitiesRequire(false, "the descriptor table accepted more than it "
                                          "holds");
            break;
        }
    }

    KernelUtilitiesRequire(
        adopted == (PROCESS_DESCRIPTOR_CAPACITY - SYSCALL_DESCRIPTOR_FIRST),
        "the table did not hold every slot above the standard three");

    /* One is released explicitly, and releasing it twice is a refusal and not a
     * second close of a number that may since have been reused. */
    if (adopted > 0U)
    {
        KernelUtilitiesRequire(ProcessReleaseDescriptor(process, descriptors[0]),
                               "a descriptor this process holds could not be released");
        KernelUtilitiesRequire(!ProcessReleaseDescriptor(process, descriptors[0]),
                               "a descriptor was released twice");
        KernelUtilitiesRequire(ProcessDescriptorFile(process, descriptors[0]) ==
                                   VFS_NO_DESCRIPTOR,
                               "a released descriptor still translates to a file");
    }

    /*
     * And the rest go with the process. The count returning to what it was is
     * the whole assertion: every descriptor this process still held was closed
     * by its destruction, which is what stops a program that ended badly costing
     * the machine a descriptor for ever.
     */
    KernelUtilitiesRequire(VfsOpenFileCount() > open_before,
                           "the descriptors were never opened, so the assertion below "
                           "would pass without them");

    ProcessDestroy(process);

    KernelUtilitiesRequire(VfsOpenFileCount() == open_before,
                           "a destroyed process left its open files behind it");
}

void KernelVerifyUtilities(void)
{
    static const char *const arg_check_vector[] = { "arg-check", "alpha", "beta gamma",
                                                    "", NULL };
    static const char *const exec_check_vector[] = { "exec-check", NULL };
    static const char *const file_check_vector[] = { "file-check", NULL };
    static const char *const echo_vector[] = { "echo", "the", "first", "words", NULL };
    static const char *const cat_vector[] = { "cat", KERNEL_UTILITIES_FIRST,
                                              KERNEL_UTILITIES_SECOND, NULL };
    static const char *const cat_absent_vector[] = { "cat", "/absent", NULL };
    static const char *const cat_directory_vector[] = { "cat", KERNEL_UTILITIES_SCRATCH,
                                                        NULL };
    static const char *const cat_bare_vector[] = { "cat", NULL };
    static const char *const ls_vector[] = { "ls", KERNEL_UTILITIES_SCRATCH, NULL };
    static const char *const ls_all_vector[] = { "ls", "-a", KERNEL_UTILITIES_SCRATCH,
                                                 NULL };
    static const char *const ls_root_vector[] = { "ls", NULL };
    static const char *const ls_file_vector[] = { "ls", KERNEL_UTILITIES_FIRST, NULL };
    static const char *const ls_absent_vector[] = { "ls", "/absent", NULL };
    static const char *const ls_option_vector[] = { "ls", "-Z", NULL };
    static const char *const mkdir_vector[] = { "mkdir", KERNEL_UTILITIES_MADE, NULL };
    static const char *const mkdir_deep_vector[] = { "mkdir", "-p", KERNEL_UTILITIES_DEEP,
                                                     NULL };
    static const char *const mkdir_bare_vector[] = { "mkdir", NULL };
    static const char *const rm_vector[] = { "rm", KERNEL_UTILITIES_VICTIM, NULL };
    static const char *const rm_force_vector[] = { "rm", "-f", KERNEL_UTILITIES_VICTIM,
                                                   NULL };
    static const char *const rm_directory_vector[] = { "rm", KERNEL_UTILITIES_SCRATCH,
                                                       NULL };

    const uint64_t dispatched_before = SyscallDispatched();
    const uint64_t executions_before = ProcessExecuteCount();
    const size_t processes_before = ProcessCount();
    const size_t open_before = VfsOpenFileCount();
    BlockDevice *device;

    KernelUtilitiesSucceeded = true;

    KernelWriteString("Utilities: the five programs of sub-task 7.6, the six calls "
                      "beneath them, and the vector they are given.\n");

    KernelComposeVolume();

    device = BlockRegister(KERNEL_UTILITIES_DEVICE, &KernelMemoryDeviceOperations, NULL,
                           BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelUtilitiesRequire(false, "the device the programs run upon could not be "
                                      "registered");
        KernelWriteString("Utilities self-test FAILED.\n");

        return;
    }

    if (!VfsMountVolume(KERNEL_UTILITIES_DEVICE, "/", "ext2", false))
    {
        (void)BlockUnregister(device);
        KernelUtilitiesRequire(false, "the volume the programs run upon could not be "
                                      "mounted");
        KernelWriteString("Utilities self-test FAILED.\n");

        return;
    }

    KernelUtilitiesBoot = ThreadAdoptCurrent("boot");

    if ((KernelUtilitiesBoot == NULL) || !KernelUtilitiesPublish() ||
        !KernelUtilitiesCompose())
    {
        if (KernelUtilitiesBoot != NULL)
        {
            ThreadDestroy(KernelUtilitiesBoot);
        }

        (void)VfsUnmount("/");
        (void)BlockUnregister(device);
        KernelWriteString("Utilities self-test FAILED.\n");

        return;
    }

    /* --------------------------------- the descriptor table, from within */

    KernelUtilitiesDescriptors();

    /* ------------------------------------------- the vector, both ways to it */

    KernelUtilitiesExpect("arg-check", KernelProgramArgCheckBegin,
                          (uint64_t)(KernelProgramArgCheckEnd - KernelProgramArgCheckBegin),
                          arg_check_vector, 0,
                          "the vector the kernel laid upon a stack is not the vector "
                          "the program found");

    /*
     * And the same program, reached through `execve` from another.
     *
     * This is the assertion the whole of the argument-copying path rests upon:
     * `exec-check` passes a vector standing in *its* address space, and that
     * space is destroyed before the new program's stack is built. A kernel that
     * read the strings after the destruction rather than before it would produce
     * a program started with whatever happened to lie at those addresses, which
     * faults sometimes and succeeds with the wrong arguments otherwise.
     */
    KernelUtilitiesExpect("exec-check", KernelProgramExecCheckBegin,
                          (uint64_t)(KernelProgramExecCheckEnd - KernelProgramExecCheckBegin),
                          exec_check_vector, 0,
                          "a vector did not survive execve, or the program it names "
                          "was not reached");

    KernelUtilitiesRequire(ProcessExecuteCount() == (executions_before + 1U),
                           "execve was not performed, so the program that ended was "
                           "the one that was started");

    /* ----------------------------------- the six calls, from a program */

    /*
     * **This is the assertion the utilities cannot make.**
     *
     * A negative test removed the copy at the end of the `read` system call —
     * so that it reported a count and delivered no bytes — and `make verify`
     * reported nothing: `cat` wrote a buffer it had never been given and exited
     * with zero. This program reads a file whose contents it knows, compares
     * every byte, and asserts each refusal by the *name* of the failure rather
     * than by its sign.
     *
     * It runs before `rm` removes anything and before `mkdir` adds anything, so
     * that the tree it lists is the tree the fixture composed.
     */
    KernelUtilitiesExpect("file-check", KernelProgramFileCheckBegin,
                          (uint64_t)(KernelProgramFileCheckEnd - KernelProgramFileCheckBegin),
                          file_check_vector, 0,
                          "a filesystem call did not do or report what it is required "
                          "to");

    /* --------------------------------------------------------------- echo */

    KernelUtilitiesExpect("echo", KernelProgramEchoBegin,
                          (uint64_t)(KernelProgramEchoEnd - KernelProgramEchoBegin),
                          echo_vector, 0, "echo did not write its operands");

    /* ---------------------------------------------------------------- cat */

    KernelUtilitiesExpect("cat", KernelProgramCatBegin,
                          (uint64_t)(KernelProgramCatEnd - KernelProgramCatBegin),
                          cat_vector, 0, "cat could not copy two files it was given");

    KernelUtilitiesExpect("cat", KernelProgramCatBegin,
                          (uint64_t)(KernelProgramCatEnd - KernelProgramCatBegin),
                          cat_absent_vector, KERNEL_UTILITIES_FAILURE,
                          "cat did not report a file that is not there");

    /* A directory is opened successfully and refuses to be read, which is the
     * one case where the failure arrives from `read` rather than from `open`. */
    KernelUtilitiesExpect("cat", KernelProgramCatBegin,
                          (uint64_t)(KernelProgramCatEnd - KernelProgramCatBegin),
                          cat_directory_vector, KERNEL_UTILITIES_FAILURE,
                          "cat did not report a directory given as an operand");

    /*
     * `cat` with no operand copies the standard input since sub-task 8.5, so
     * it is given one: the terminal, with a line and the control-D at which
     * a copy of the terminal ends. It ends with zero, having copied the line
     * to the log, and consumed exactly what was placed — a `cat` that stopped
     * short would leave the control-D for the shell to read as its end.
     */
    {
        static const char line[] = "a line for cat to copy\n\x04";
        const uint64_t delivered = TerminalBytesDelivered();

        TerminalFlush();
        TerminalInject(line, sizeof line - 1U);
        KernelUtilitiesExpect("cat", KernelProgramCatBegin,
                              (uint64_t)(KernelProgramCatEnd - KernelProgramCatBegin),
                              cat_bare_vector, 0,
                              "cat with no operand did not copy the standard input to its end");
        KernelUtilitiesRequire(TerminalBytesDelivered() - delivered == sizeof line - 1U,
                               "cat did not consume exactly the line and the control-D");
        TerminalFlush();
    }

    /* ----------------------------------------------------------------- ls */

    KernelUtilitiesExpect("ls", KernelProgramListBegin,
                          (uint64_t)(KernelProgramListEnd - KernelProgramListBegin),
                          ls_vector, 0, "ls could not list a directory");

    KernelUtilitiesExpect("ls", KernelProgramListBegin,
                          (uint64_t)(KernelProgramListEnd - KernelProgramListBegin),
                          ls_all_vector, 0, "ls -a could not list a directory");

    KernelUtilitiesExpect("ls", KernelProgramListBegin,
                          (uint64_t)(KernelProgramListEnd - KernelProgramListBegin),
                          ls_root_vector, 0, "ls with no operand could not list the root");

    /* An operand that is not a directory is written out as itself and is not a
     * failure, which IEEE Std 1003.1-2017 requires. */
    KernelUtilitiesExpect("ls", KernelProgramListBegin,
                          (uint64_t)(KernelProgramListEnd - KernelProgramListBegin),
                          ls_file_vector, 0,
                          "ls treated a regular-file operand as a failure");

    KernelUtilitiesExpect("ls", KernelProgramListBegin,
                          (uint64_t)(KernelProgramListEnd - KernelProgramListBegin),
                          ls_absent_vector, KERNEL_UTILITIES_FAILURE,
                          "ls did not report a directory that is not there");

    KernelUtilitiesExpect("ls", KernelProgramListBegin,
                          (uint64_t)(KernelProgramListEnd - KernelProgramListBegin),
                          ls_option_vector, KERNEL_UTILITIES_FAILURE,
                          "ls accepted an option it does not implement");

    /* -------------------------------------------------------------- mkdir */

    KernelUtilitiesRequire(!KernelUtilitiesExists(KERNEL_UTILITIES_MADE),
                           "the directory mkdir is to create is already there, so the "
                           "assertion below would pass without it");

    KernelUtilitiesExpect("mkdir", KernelProgramMakeDirBegin,
                          (uint64_t)(KernelProgramMakeDirEnd - KernelProgramMakeDirBegin),
                          mkdir_vector, 0, "mkdir could not create a directory");

    KernelUtilitiesRequire(KernelUtilitiesIsDirectory(KERNEL_UTILITIES_MADE),
                           "mkdir reported success and the directory is not there");

    /* The same run a second time must fail, which is what says the first one did
     * something rather than that mkdir always reports success. */
    KernelUtilitiesExpect("mkdir", KernelProgramMakeDirBegin,
                          (uint64_t)(KernelProgramMakeDirEnd - KernelProgramMakeDirBegin),
                          mkdir_vector, KERNEL_UTILITIES_FAILURE,
                          "mkdir created a directory that was already there");

    KernelUtilitiesExpect("mkdir", KernelProgramMakeDirBegin,
                          (uint64_t)(KernelProgramMakeDirEnd - KernelProgramMakeDirBegin),
                          mkdir_deep_vector, 0,
                          "mkdir -p could not create a path of missing components");

    KernelUtilitiesRequire(KernelUtilitiesIsDirectory(KERNEL_UTILITIES_DEEP),
                           "mkdir -p reported success and the deepest component is not "
                           "there");
    KernelUtilitiesRequire(KernelUtilitiesIsDirectory("/made/a"),
                           "mkdir -p did not create the intermediate components");

    KernelUtilitiesExpect("mkdir", KernelProgramMakeDirBegin,
                          (uint64_t)(KernelProgramMakeDirEnd - KernelProgramMakeDirBegin),
                          mkdir_bare_vector, KERNEL_UTILITIES_FAILURE,
                          "mkdir with no operand did not report it");

    /* ----------------------------------------------------------------- rm */

    KernelUtilitiesRequire(KernelUtilitiesExists(KERNEL_UTILITIES_VICTIM),
                           "the file rm is to remove is not there, so the assertion "
                           "below would pass without it");

    KernelUtilitiesExpect("rm", KernelProgramRemoveBegin,
                          (uint64_t)(KernelProgramRemoveEnd - KernelProgramRemoveBegin),
                          rm_vector, 0, "rm could not remove a file");

    KernelUtilitiesRequire(!KernelUtilitiesExists(KERNEL_UTILITIES_VICTIM),
                           "rm reported success and the file is still there");
    KernelUtilitiesRequire(KernelUtilitiesExists(KERNEL_UTILITIES_FIRST),
                           "rm removed a name it was not given");

    KernelUtilitiesExpect("rm", KernelProgramRemoveBegin,
                          (uint64_t)(KernelProgramRemoveEnd - KernelProgramRemoveBegin),
                          rm_vector, KERNEL_UTILITIES_FAILURE,
                          "rm removed a file that was not there");

    /* And -f makes exactly that case succeed, which is the whole of what the
     * option means upon a system with nothing to prompt. */
    KernelUtilitiesExpect("rm", KernelProgramRemoveBegin,
                          (uint64_t)(KernelProgramRemoveEnd - KernelProgramRemoveBegin),
                          rm_force_vector, 0,
                          "rm -f reported a file that was not there");

    KernelUtilitiesExpect("rm", KernelProgramRemoveBegin,
                          (uint64_t)(KernelProgramRemoveEnd - KernelProgramRemoveBegin),
                          rm_directory_vector, KERNEL_UTILITIES_FAILURE,
                          "rm did not refuse a directory");

    KernelUtilitiesRequire(KernelUtilitiesIsDirectory(KERNEL_UTILITIES_SCRATCH),
                           "rm removed a directory it was required to refuse");

    /* ------------------------------------------------------------ the sum */

    /*
     * Every program made system calls, and a floor rather than an exact number
     * for the reason kernel/test/libc/startup.c records: how many `write` calls
     * a buffered stream becomes depends upon how the buffer filled, which
     * depends upon the length of a diagnostic somebody may reword. Twenty
     * programs have run, each of which at least opened or wrote and then ended.
     */
    KernelUtilitiesRequire((SyscallDispatched() - dispatched_before) >= 40U,
                           "the programs did not make the system calls their work "
                           "required");

    KernelUtilitiesRequire(VfsOpenFileCount() == open_before,
                           "the filesystem layer holds descriptors nothing closed");
    KernelUtilitiesRequire(ProcessCount() == processes_before,
                           "the process table did not return to what it held");

    ThreadDestroy(KernelUtilitiesBoot);
    KernelUtilitiesBoot = NULL;

    (void)VfsUnmount("/");
    (void)BlockUnregister(device);

    KernelWriteString(KernelUtilitiesSucceeded ? "Utilities self-test passed.\n"
                                               : "Utilities self-test FAILED.\n");
}
