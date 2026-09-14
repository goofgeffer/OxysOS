/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/file-check/main.c
 * Purpose: Asserts the six filesystem system calls of sub-task 7.6 from the
 *          only side that can reach them — a program at privilege level 3 —
 *          comparing what each call returns against what it is required to
 *          return, and ending with the number of comparisons that failed.
 * Key functions: main, FileRequire, FileContents, FileDirectory, FileRefusals.
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
 *   - docs/design/LIBC.md, Section 12.5: what this asserts and why it exists.
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

    descriptor = OxysOpen(FileFirstPath, SYSCALL_OPEN_READ);
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

    descriptor = OxysOpen(FileDirectoryPath, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY);
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

    FileRefused(OxysOpen(FileAbsentPath, SYSCALL_OPEN_READ), ENOENT,
                "a path that leads nowhere was not refused as absent");

    FileRefused(OxysOpen(FileFirstPath, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY),
                ENOTDIR, "a regular file was opened as a directory");

    FileRefused(OxysOpen("/scratch/one/below", SYSCALL_OPEN_READ), ENOTDIR,
                "a path leading through a regular file was not refused");

    /*
     * A flag outside the pair the interface offers. This is the assertion that
     * the kernel refuses what it does not implement rather than masking it away
     * — and it is worth making from a program, because no utility here would
     * ever pass such a flag and nothing else would notice.
     */
    FileRefused(OxysOpen(FileFirstPath, SYSCALL_OPEN_READ | UINT64_C(0x0002)), EINVAL,
                "an open asking to write was not refused");
    FileRefused(OxysOpen(FileFirstPath, 0U), EINVAL,
                "an open asking for neither reading nor writing was not refused");

    /* A path beyond what the kernel will copy. The bound is the kernel's and is
     * published, so this is a program asking to be refused at exactly the
     * boundary it was told about. */
    memset(path, 'a', sizeof path - 1U);
    path[0] = '/';
    path[sizeof path - 1U] = '\0';
    FileRefused(OxysOpen(path, SYSCALL_OPEN_READ), ENAMETOOLONG,
                "a path beyond the published bound was not refused as too long");

    FileRefused(OxysOpen((const char *)(uintptr_t)0x10, SYSCALL_OPEN_READ), EFAULT,
                "a path this program may not read was not refused");

    /* A directory opens, and refuses to be read as a file. This is the one case
     * where `cat` fails at the read rather than at the open, and the refusal
     * must say which. */
    descriptor = OxysOpen(FileDirectoryPath, SYSCALL_OPEN_READ);
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
        const int64_t descriptor = OxysOpen(FileFirstPath, SYSCALL_OPEN_READ);

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
        const int64_t descriptor = OxysOpen(FileFirstPath, SYSCALL_OPEN_READ);

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

int main(void)
{
    (void)printf("file-check: the six filesystem calls, from a program.\n");

    FileContents();
    FileDirectoryContents();
    FileRefusals();
    FileExhaustion();

    (void)printf("file-check: %d assertion(s) failed.\n", FileFailures);

    return FileFailures;
}
