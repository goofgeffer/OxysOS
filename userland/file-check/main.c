/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/file-check/main.c
 * Purpose: Asserts the six filesystem system calls of sub-task 7.6 from the
 *          only side that can reach them — a program at privilege level 3 —
 *          comparing what each call returns against what it is required to
 *          return, and ending with the number of comparisons that failed;
 *          the writable file and the shared open file of 8.5, and the pipe of
 *          8.6, are asserted here too.
 * Key functions: main, FileRequire, FileContents, FileDirectory, FileRefusals,
 *          FileWriting, FilePipes.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the six calls, the two open flags, the
 *     failure results and the directory entry, all of which this compares
 *     against.
 *   - libc/include/syscall.h: the wrappers, and what each is documented to
 *     return.
 *   - IEEE Std 1003.1-2017: `open()`, `read()`, `close()`, `mkdir()` and
 *     `unlink()`, from which the refusals below take their meaning.
 *   - kernel/test/libc/utilities.c: the fixture this reads, which that file
 *     builds through the filesystem layer before running this.
 *   - docs/design/LIBC.md: what this asserts and why it exists.
 *
 * **Why this program exists, and what it closes.**
 *
 *   The five utilities assert the calls only through what they print, and
 *   nothing in this kernel captures what a program prints. A negative test made
 *   that concrete: the `read` system call was altered to report a count and copy
 *   no bytes at all, and `make verify` reported nothing wrong — `cat` opened its
 *   files, was told how many bytes it had, wrote a buffer it had never been
 *   given, and exited with a status of zero.
 *
 *   This program compares instead. It reads a file whose contents it knows and
 *   checks every byte; it lists a directory whose entries it knows and checks
 *   that each is there; and it makes each call in a way that must be refused and
 *   checks that the refusal is the *right* one, so that a kernel which reported
 *   every failure as EINVAL is caught by something other than a person's
 *   judgement about a diagnostic.
 *
 * Why the refusals are asserted by name and not merely as failures.
 *
 *   Because `errno` is the only thing a program has to decide what to do next.
 *   `ls` treats ENOTDIR as an operand to print and every other refusal as a
 *   failure; `rm -f` treats ENOENT as success; `mkdir -p` treats EEXIST as
 *   success. A kernel that returned the right sign and the wrong name would make
 *   each of those three do the wrong thing, and each would still exit with a
 *   plausible status.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <signal.h>

/* The fixture, which kernel/test/libc/utilities.c composes before running this.
 * The contents are written out here rather than passed in, for the reason
 * userland/arg-check/main.c records: a program told what to expect by the thing
 * under test agrees with itself however that thing fails. */
static const char FileFirstPath[] = "/scratch/one";
static const char FileFirstContents[] = "the first file\n";
static const char FileDirectoryPath[] = "/scratch";
static const char FileAbsentPath[] = "/absent";

/* How many assertions have failed. It is this program's exit status. */
static int FileFailures;

static void FileRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        ++FileFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

/* Asserts that a call was refused, and refused for the stated reason. `errno` is
 * read only where the result was a failure: a wrapper that succeeded leaves it
 * alone, so reading it after a success would report whatever the last failure
 * set. */
static void FileRefused(int64_t result, int expected, const char *statement)
{
    if (result >= 0)
    {
        ++FileFailures;
        (void)printf("  %s FAILED: the call succeeded.\n", statement);

        return;
    }

    if (errno != expected)
    {
        ++FileFailures;
        (void)printf("  %s FAILED: errno is %d (%s) and not %d.\n", statement, errno,
                     strerror(errno), expected);
    }
}

/* Reads a file of known contents and compares every byte of it. */
static void FileContents(void)
{
    char buffer[64];
    const size_t length = sizeof FileFirstContents - 1U;
    int64_t descriptor;
    int64_t read;

    descriptor = OxysOpen(FileFirstPath, SYSCALL_OPEN_READ, 0U);
    FileRequire(descriptor >= (int64_t)SYSCALL_DESCRIPTOR_FIRST,
                "a file could not be opened, or was opened upon a descriptor reserved "
                "to the diagnostic path");

    if (descriptor < 0)
    {
        return;
    }

    read = OxysRead((int)descriptor, buffer, sizeof buffer);
    FileRequire(read == (int64_t)length,
                "a read did not report the number of bytes the file holds");

    if (read == (int64_t)length)
    {
        FileRequire(memcmp(buffer, FileFirstContents, length) == 0,
                    "a read reported bytes it did not deliver, or delivered the wrong "
                    "ones");
    }

    /*
     * The end of the file is zero, and it is the assertion that says the
     * position advanced. A `read` that returned the same bytes for ever would
     * satisfy the comparison above at every call.
     */
    FileRequire(OxysRead((int)descriptor, buffer, sizeof buffer) == 0,
                "a second read did not report the end of the file, so the position did "
                "not advance");

    /* A length of zero is refused and not answered with zero, because zero is
     * what the end of a file returns. */
    FileRefused(OxysRead((int)descriptor, buffer, 0U), EINVAL,
                "a read of no bytes was not refused");

    /* An address this program may not use. The lowest page of every address
     * space is deliberately unmapped, so this one is certainly not its own. */
    FileRefused(OxysRead((int)descriptor, (void *)(uintptr_t)0x10, 1U), EFAULT,
                "a read into memory this program may not write was not refused");

    /* A descriptor opened upon a regular file is not a directory. */
    {
        SyscallDirectoryEntry entry;

        FileRefused(OxysReadDirectory((int)descriptor, &entry), ENOTDIR,
                    "a regular file was read as a directory");
    }

    FileRequire(OxysClose((int)descriptor) >= 0, "a descriptor could not be closed");

    /*
     * And it is gone. Both of these would succeed upon a kernel that forgot the
     * number rather than releasing it, and the second is the one that matters:
     * a `read` upon a closed descriptor must not reach the file that descriptor
     * used to name.
     */
    FileRefused(OxysClose((int)descriptor), EBADF, "a descriptor was closed twice");
    FileRefused(OxysRead((int)descriptor, buffer, sizeof buffer), EBADF,
                "a closed descriptor could still be read");
}

/* Lists a directory whose entries are known and checks that each is there. */
static void FileDirectoryContents(void)
{
    SyscallDirectoryEntry entry;
    int64_t descriptor;
    bool saw_dot = false;
    bool saw_parent = false;
    bool saw_first = false;
    bool saw_second = false;
    bool saw_hidden = false;
    int entries = 0;

    descriptor = OxysOpen(FileDirectoryPath, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY, 0U);
    FileRequire(descriptor >= 0, "a directory could not be opened");

    if (descriptor < 0)
    {
        return;
    }

    for (;;)
    {
        const int64_t result = OxysReadDirectory((int)descriptor, &entry);

        if (result < 0)
        {
            FileRequire(false, "a directory could not be read");
            break;
        }

        if (result == 0)
        {
            break;
        }

        ++entries;

        /*
         * A bound upon the loop, so that a kernel which never reports the end
         * fails an assertion rather than running for ever. A self-test that
         * hangs reports nothing at all, which is the one failure worse than a
         * wrong answer.
         */
        if (entries > 64)
        {
            FileRequire(false, "a directory never reported its end");
            break;
        }

        if (strcmp(entry.name, ".") == 0)
        {
            saw_dot = true;
            FileRequire(entry.type == SYSCALL_TYPE_DIRECTORY,
                        "the entry for the directory itself is not declared a directory");
        }
        else if (strcmp(entry.name, "..") == 0)
        {
            saw_parent = true;
        }
        else if (strcmp(entry.name, "one") == 0)
        {
            saw_first = true;
            FileRequire(entry.type == SYSCALL_TYPE_REGULAR,
                        "a regular file is not declared a regular file");
        }
        else if (strcmp(entry.name, "two") == 0)
        {
            saw_second = true;
        }
        else if (strcmp(entry.name, ".hidden") == 0)
        {
            saw_hidden = true;
        }
        else
        {
            /* Anything else is a name this program does not know about, which is
             * not a failure: the test that composes the fixture may add one. */
        }
    }

    FileRequire(saw_dot, "the directory does not hold an entry for itself");
    FileRequire(saw_parent, "the directory does not hold an entry for its parent");
    FileRequire(saw_first, "an entry the fixture created is not in the directory");
    FileRequire(saw_second, "an entry the fixture created is not in the directory");
    FileRequire(saw_hidden,
                "a name beginning with a period was not returned, so the kernel is "
                "filtering what only a program may filter");

    /* The end is stable: a directory that has been read to its end reports the
     * end again rather than beginning afresh. */
    FileRequire(OxysReadDirectory((int)descriptor, &entry) == 0,
                "a directory read past its end did not report the end again");

    FileRequire(OxysClose((int)descriptor) >= 0, "a directory could not be closed");
}

/* Every call made in a way that must be refused, with the name of the refusal
 * asserted rather than merely its sign. */
static void FileRefusals(void)
{
    char path[SYSCALL_PATH_MAXIMUM + 8U];
    char buffer[8];
    int64_t descriptor;

    FileRefused(OxysOpen(FileAbsentPath, SYSCALL_OPEN_READ, 0U), ENOENT,
                "a path that leads nowhere was not refused as absent");

    FileRefused(OxysOpen(FileFirstPath, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY, 0U),
                ENOTDIR, "a regular file was opened as a directory");

    FileRefused(OxysOpen("/scratch/one/below", SYSCALL_OPEN_READ, 0U), ENOTDIR,
                "a path leading through a regular file was not refused");

    /*
     * A flag outside the set the interface offers. This is the assertion that
     * the kernel refuses what it does not implement rather than masking it away
     * — and it is worth making from a program, because no utility here would
     * ever pass such a flag and nothing else would notice. It was WRITE until
     * sub-task 8.5 gave the interface writing; it is now a bit the interface
     * has never named, and CREATE without WRITE, which asks to change a file
     * without opening it for change.
     */
    FileRefused(OxysOpen(FileFirstPath, SYSCALL_OPEN_READ | UINT64_C(0x8000), 0U), EINVAL,
                "an open asking for a flag the interface does not name was not refused");
    FileRefused(OxysOpen(FileFirstPath, SYSCALL_OPEN_READ | SYSCALL_OPEN_CREATE, 0U), EINVAL,
                "an open asking to create without writing was not refused");
    FileRefused(OxysOpen(FileFirstPath, 0U, 0U), EINVAL,
                "an open asking for neither reading nor writing was not refused");

    /* A path beyond what the kernel will copy. The bound is the kernel's and is
     * published, so this is a program asking to be refused at exactly the
     * boundary it was told about. */
    memset(path, 'a', sizeof path - 1U);
    path[0] = '/';
    path[sizeof path - 1U] = '\0';
    FileRefused(OxysOpen(path, SYSCALL_OPEN_READ, 0U), ENAMETOOLONG,
                "a path beyond the published bound was not refused as too long");

    FileRefused(OxysOpen((const char *)(uintptr_t)0x10, SYSCALL_OPEN_READ, 0U), EFAULT,
                "a path this program may not read was not refused");

    /* A directory opens, and refuses to be read as a file. This is the one case
     * where `cat` fails at the read rather than at the open, and the refusal
     * must say which. */
    descriptor = OxysOpen(FileDirectoryPath, SYSCALL_OPEN_READ, 0U);
    FileRequire(descriptor >= 0, "a directory could not be opened for reading");

    if (descriptor >= 0)
    {
        FileRefused(OxysRead((int)descriptor, buffer, sizeof buffer), EISDIR,
                    "a directory was read as a file");
        FileRequire(OxysClose((int)descriptor) >= 0, "a directory could not be closed");
    }

    FileRefused(OxysClose(SYSCALL_DESCRIPTOR_OUTPUT), EBADF,
                "the standard output was closed as though it named a file");
    FileRefused(OxysClose(4096), EBADF,
                "a descriptor beyond this program's table was not refused");

    FileRefused(OxysMakeDirectory(FileDirectoryPath, 0755U), EEXIST,
                "a directory that is already there was created again");
    FileRefused(OxysMakeDirectory("/absent/below", 0755U), ENOENT,
                "a directory was created beneath a parent that is not there");

    FileRefused(OxysUnlink(FileDirectoryPath), EISDIR,
                "a directory was removed by unlink");
    FileRefused(OxysUnlink(FileAbsentPath), ENOENT,
                "a name that is not there was removed");
}

/*
 * The table is filled, and the refusal when it is full is EMFILE.
 *
 * Every descriptor is closed again afterwards. A program that left them open
 * would cost the machine descriptors it holds for the whole system, and the
 * self-test running this asserts the count is unchanged when it ends — so a
 * failure here would be reported twice, once by name and once by the count.
 */
static void FileExhaustion(void)
{
    int64_t held[64];
    int count = 0;

    for (;;)
    {
        const int64_t descriptor = OxysOpen(FileFirstPath, SYSCALL_OPEN_READ, 0U);

        if (descriptor < 0)
        {
            FileRequire(errno == EMFILE,
                        "a full descriptor table was refused for some reason other than "
                        "being full");
            break;
        }

        held[count] = descriptor;
        ++count;

        if (count >= (int)(sizeof held / sizeof held[0]))
        {
            FileRequire(false, "the descriptor table accepted more than it can hold");
            break;
        }
    }

    FileRequire(count > 0, "no descriptor could be opened at all");

    for (int index = 0; index < count; ++index)
    {
        FileRequire(OxysClose((int)held[index]) >= 0,
                    "a descriptor could not be released after the table was filled");
    }

    /* And the table takes one again, which is what says the releases above did
     * something. */
    {
        const int64_t descriptor = OxysOpen(FileFirstPath, SYSCALL_OPEN_READ, 0U);

        FileRequire(descriptor >= 0,
                    "the descriptor table is still full after every descriptor was "
                    "released");

        if (descriptor >= 0)
        {
            FileRequire(OxysClose((int)descriptor) >= 0,
                        "the last descriptor could not be closed");
        }
    }
}

/* ---------------------------------------------------------------------------
 * Sub-task 8.5: writing, and the sharing of an open file.
 * ------------------------------------------------------------------------- */

static const char FileWrittenPath[] = "/scratch/written";

/* Reads the whole of a file into `buffer` and compares it with `expected`,
 * length first. */
static bool FileHolds(const char *path, const char *expected)
{
    char buffer[128];
    const int64_t descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);
    int64_t total = 0;

    if (descriptor < 0)
    {
        return false;
    }

    for (;;)
    {
        const int64_t got = OxysRead((int)descriptor, &buffer[total],
                                     sizeof buffer - 1U - (size_t)total);

        if (got <= 0)
        {
            break;
        }

        total += got;
    }

    (void)OxysClose((int)descriptor);
    buffer[total] = '\0';

    return strcmp(buffer, expected) == 0;
}

static void FileWriting(void)
{
    int64_t descriptor;
    int64_t child;

    /* Create and write; the position advances, so two writes are one file. */
    descriptor = OxysOpen(FileWrittenPath,
                          SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE, 0644U);
    FileRequire(descriptor >= 0, "a file could not be created for writing");

    if (descriptor >= 0)
    {
        FileRequire(OxysWrite((int)descriptor, "abc", 3U) == 3, "a write of three bytes was short");
        FileRequire(OxysWrite((int)descriptor, "def", 3U) == 3, "a second write was short");

        /* A read of a descriptor opened for writing alone is refused. */
        {
            char one;

            errno = 0;
            FileRequire((OxysRead((int)descriptor, &one, 1U) < 0) && (errno == EINVAL),
                        "a read of a write-only descriptor was not refused");
        }

        FileRequire(OxysClose((int)descriptor) >= 0, "the written file could not be closed");
    }

    FileRequire(FileHolds(FileWrittenPath, "abcdef"), "the file does not hold what was written");

    /* Append goes to the end; truncate empties. */
    descriptor = OxysOpen(FileWrittenPath, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_APPEND, 0U);
    FileRequire(descriptor >= 0, "the file could not be opened for appending");

    if (descriptor >= 0)
    {
        (void)OxysWrite((int)descriptor, "ghi", 3U);
        (void)OxysClose((int)descriptor);
    }

    FileRequire(FileHolds(FileWrittenPath, "abcdefghi"), "an append did not go to the end");

    descriptor = OxysOpen(FileWrittenPath, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_TRUNCATE, 0U);

    if (descriptor >= 0)
    {
        (void)OxysClose((int)descriptor);
    }

    FileRequire(FileHolds(FileWrittenPath, ""), "a truncating open did not empty the file");

    /* Duplication: two numbers, one file, one position; closing one leaves
     * the other; a number below SYSCALL_DESCRIPTOR_FIRST may be given a file
     * and reverts to the kernel's path when that file is closed. */
    descriptor = OxysOpen(FileWrittenPath, SYSCALL_OPEN_WRITE, 0U);
    FileRequire(descriptor >= 0, "the file could not be reopened for writing");

    if (descriptor >= 0)
    {
        FileRequire(OxysDuplicate((int)descriptor, 7) == 0, "dup2 onto 7 failed");
        FileRequire(OxysWrite((int)descriptor, "one", 3U) == 3, "a write through the original was short");
        FileRequire(OxysWrite(7, "two", 3U) == 3, "a write through the duplicate was short");
        FileRequire(OxysClose((int)descriptor) >= 0, "the original could not be closed");
        FileRequire(OxysWrite(7, "three", 5U) == 5,
                    "the duplicate did not survive the original's close");
        FileRequire(OxysClose(7) >= 0, "the duplicate could not be closed");
        errno = 0;
        FileRequire((OxysWrite(7, "x", 1U) < 0) && (errno == EBADF),
                    "a closed duplicate still accepted a write");
    }

    FileRequire(FileHolds(FileWrittenPath, "onetwothree"),
                "the two numbers did not share one position");

    /* A duplicate below SYSCALL_DESCRIPTOR_FIRST: what a redirection does. */
    descriptor = OxysOpen(FileWrittenPath, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_TRUNCATE, 0U);

    if (descriptor >= 0)
    {
        FileRequire(OxysDuplicate((int)descriptor, 2) == 0, "dup2 onto 2 failed");
        (void)OxysClose((int)descriptor);
        FileRequire(OxysWrite(2, "diag", 4U) == 4, "a write to the redirected 2 was short");
        FileRequire(OxysClose(2) >= 0, "the redirected 2 could not be closed");
        FileRequire(OxysWrite(2, "\n", 1U) == 1, "2 did not revert to the diagnostic path when closed");
    }

    FileRequire(FileHolds(FileWrittenPath, "diag"), "the redirected 2 did not reach the file");

    /* The kernel's own path cannot be moved above the three: EBADF. */
    errno = 0;
    FileRequire((OxysDuplicate(1, 9) < 0) && (errno == EBADF),
                "the diagnostic path was duplicated onto a number above the three");
    errno = 0;
    FileRequire((OxysDuplicate(12, 13) < 0) && (errno == EBADF),
                "an empty number was duplicated");

    /* Inheritance across fork: the child writes through the parent's open
     * file and moves its position, and the parent's write follows it. */
    descriptor = OxysOpen(FileWrittenPath, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_TRUNCATE, 0U);
    FileRequire(descriptor >= 0, "the file could not be reopened for the fork");
    child = OxysFork();

    if (child == 0)
    {
        OxysExit((OxysWrite((int)descriptor, "child", 5U) == 5) ? 0 : 1);
    }

    if (child > 0)
    {
        int64_t status = -1;

        FileRequire(OxysWait(&status) == child, "the child was not collected");
        FileRequire(status == 0, "the child could not write through the inherited descriptor");
        FileRequire(OxysWrite((int)descriptor, "parent", 6U) == 6, "the parent's write was short");
        (void)OxysClose((int)descriptor);
        FileRequire(FileHolds(FileWrittenPath, "childparent"),
                    "the child's write and the parent's did not share one position");
    }
    else
    {
        FileRequire(false, "fork failed");
    }

    FileRequire(OxysUnlink(FileWrittenPath) >= 0, "the written file could not be removed");
}

/* ---------------------------------------------------------------------------
 * Sub-task 8.6: the pipe.
 * ------------------------------------------------------------------------- */

/* How much the child of the fork below sends: three times the pipe's buffer,
 * so that the writer must sleep for the reader at least twice, and the
 * transfer is the two threads taking turns rather than one filling a buffer
 * the other empties afterwards. */
#define FILE_PIPE_CHUNK    4096U
#define FILE_PIPE_CHUNKS   3U

static char FilePipeChunk[FILE_PIPE_CHUNK];

static void FilePipes(void)
{
    int ends[2] = { -1, -1 };
    char buffer[16];
    int64_t child;

    FileRequire(OxysPipe(ends) == 0, "a pipe could not be made");

    if ((ends[0] < 0) || (ends[1] < 0))
    {
        return;
    }

    FileRequire((ends[0] >= (int)SYSCALL_DESCRIPTOR_FIRST) &&
                    (ends[1] >= (int)SYSCALL_DESCRIPTOR_FIRST) && (ends[0] != ends[1]),
                "the pipe's ends are not two new numbers above the three");

    /* Bytes go in one end and come out the other, in order. */
    FileRequire(OxysWrite(ends[1], "abc", 3U) == 3, "a write to the pipe was short");
    FileRequire(OxysRead(ends[0], buffer, sizeof buffer) == 3, "a read of the pipe was short");
    FileRequire(memcmp(buffer, "abc", 3U) == 0, "the pipe did not deliver what was written");

    /* Each end does one thing. */
    FileRefused(OxysRead(ends[1], buffer, sizeof buffer), EINVAL,
                "a read of the write end was accepted");
    FileRefused(OxysWrite(ends[0], "x", 1U), EINVAL, "a write to the read end was accepted");

    /* The end of the file is the last writer's close, and only then. */
    FileRequire(OxysWrite(ends[1], "de", 2U) == 2, "a second write to the pipe was short");
    FileRequire(OxysClose(ends[1]) >= 0, "the write end could not be closed");
    FileRequire(OxysRead(ends[0], buffer, sizeof buffer) == 2,
                "the bytes written before the close were not delivered after it");
    FileRequire(OxysRead(ends[0], buffer, sizeof buffer) == 0,
                "a pipe with no writer did not read as the end of the file");
    FileRequire(OxysClose(ends[0]) >= 0, "the read end could not be closed");

    /* A writer with no reader is refused by name — and, since 8.7, sent
     * SIGPIPE, which would end this program; it is ignored here so that the
     * refusal can be seen. signal-check asserts the signal. */
    FileRequire(signal(SIGPIPE, SIG_IGN) == SIG_DFL, "SIGPIPE was not at its default");
    FileRequire(OxysPipe(ends) == 0, "a second pipe could not be made");
    FileRequire(OxysClose(ends[0]) >= 0, "the second pipe's read end could not be closed");
    FileRefused(OxysWrite(ends[1], "x", 1U), EPIPE, "a write with no reader was accepted");
    FileRequire(OxysClose(ends[1]) >= 0, "the second pipe's write end could not be closed");

    /*
     * Two processes, taking turns. The child writes three buffers' worth and
     * ends; the parent reads to the end of the file and must find every byte,
     * in order, with the pattern each chunk carries. A writer that did not
     * sleep when the pipe was full would lose bytes or overwrite them; a
     * reader that did not sleep when it was empty would see the end before
     * the writer had finished; and a close in the child that did not wake the
     * parent would leave the parent asleep for ever, which this program would
     * report by never ending.
     */
    FileRequire(OxysPipe(ends) == 0, "a third pipe could not be made");
    child = OxysFork();

    if (child == 0)
    {
        int status = 0;

        (void)OxysClose(ends[0]);

        for (unsigned chunk = 0U; chunk < FILE_PIPE_CHUNKS; ++chunk)
        {
            for (size_t index = 0U; index < FILE_PIPE_CHUNK; ++index)
            {
                FilePipeChunk[index] = (char)('a' + (int)chunk);
            }

            if (OxysWrite(ends[1], FilePipeChunk, FILE_PIPE_CHUNK) != (int64_t)FILE_PIPE_CHUNK)
            {
                status = 1;
            }
        }

        (void)OxysClose(ends[1]);
        OxysExit(status);
    }

    if (child > 0)
    {
        int64_t status = -1;
        uint64_t total = 0U;
        bool ordered = true;

        (void)OxysClose(ends[1]);

        for (;;)
        {
            const int64_t got = OxysRead(ends[0], FilePipeChunk, sizeof FilePipeChunk);

            if (got <= 0)
            {
                FileRequire(got == 0, "a read of the pipe failed before its end");
                break;
            }

            for (int64_t index = 0; index < got; ++index)
            {
                const char expected = (char)('a' + (int)((total + (uint64_t)index) / FILE_PIPE_CHUNK));

                if (FilePipeChunk[index] != expected)
                {
                    ordered = false;
                }
            }

            total += (uint64_t)got;
        }

        FileRequire(total == (uint64_t)FILE_PIPE_CHUNK * FILE_PIPE_CHUNKS,
                    "the pipe did not deliver every byte the child wrote");
        FileRequire(ordered, "the pipe delivered the child's bytes out of order");
        (void)OxysClose(ends[0]);
        FileRequire(OxysWait(&status) == child, "the writing child was not collected");
        FileRequire(status == 0, "the child's writes to the pipe were not all delivered");
    }
    else
    {
        FileRequire(false, "fork failed for the pipe");
    }
}

int main(void)
{
    (void)printf("file-check: the filesystem calls, from a program.\n");

    FileContents();
    FileDirectoryContents();
    FileRefusals();
    FileExhaustion();
    FileWriting();
    FilePipes();

    (void)printf("file-check: %d assertion(s) failed.\n", FileFailures);

    return FileFailures;
}
