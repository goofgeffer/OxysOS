/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/proc/directory.c
 * Purpose: Asserts the working directory of sub-task 8.3 — that a process
 *          begins at the root, that `chdir` and `getcwd` move and report it,
 *          that every call resolves a relative path against it, that a child
 *          inherits it, and that each refusal is the named one — by running
 *          `dir-check` at privilege level 3 upon the root the machine booted
 *          with and checking the number it ends with.
 * Key functions: KernelVerifyDirectory.
 * References:
 *   - docs/design/SHELL.md: the table pairing every property the
 *     program asserts with the silent failure the assertion exists to catch.
 *   - userland/dir-check/main.c: the assertions themselves.
 *   - kernel/test/proc/directory_image.asm: the program, embedded.
 *   - kernel/test/libc/utilities.c: the run procedure this repeats, and why a
 *     test repeats it rather than sharing it with the thing it asserts.
 *
 * Why this runs after the root is mounted.
 *
 *   The program changes into `/bin` and opens `echo` by a relative path, so
 *   it needs the initial ramdisk at the root — which is the only volume the
 *   working directory will ever be asked about, and the reason a composed
 *   volume was not used instead. It is therefore among the tests that run
 *   after KernelMountRootVolume, as the ramdisk's own does.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>

extern const uint8_t KernelProgramDirCheckBegin[];
extern const uint8_t KernelProgramDirCheckEnd[];

static bool VerifyDirectorySucceeded;

static void VerifyDirectoryRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyDirectorySucceeded = false;
    }
}

static Thread *VerifyDirectoryBoot;

static bool VerifyDirectoryRun(int64_t *status)
{
    const uint64_t length = (uint64_t)(KernelProgramDirCheckEnd - KernelProgramDirCheckBegin);
    static const char name[] = "dir-check";
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)sizeof name;

    for (size_t index = 0U; index < sizeof name; ++index)
    {
        arguments.storage[index] = name[index];
    }

    process = ProcessCreate(name, NULL);

    if (process == NULL)
    {
        VerifyDirectoryRequire(false, "a process could not be created for dir-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramDirCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyDirectoryRequire(false, "dir-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyDirectoryRequire(false, "dir-check was given no stack");

        return false;
    }

    /* The program asserts that it begins at the root, which is the kernel's
     * promise and is checked here too, before it runs. */
    VerifyDirectoryRequire((process->working_directory[0] == '/') &&
                               (process->working_directory[1] == '\0'),
                           "a new process does not begin at the root");

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyDirectoryRequire(false, "dir-check could not be started");

        return false;
    }

    VerifyDirectoryRequire(ThreadCurrent() == VerifyDirectoryBoot,
                           "the kernel did not resume the thread that started dir-check");
    VerifyDirectoryRequire(process->state == PROCESS_EXITED,
                           "dir-check's process was not marked as ended");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifyDirectoryRequire(VfsOpenFileCount() == open_before,
                           "dir-check left an open file behind it");

    return true;
}

void KernelVerifyDirectory(void)
{
    int64_t status = 0;

    VerifyDirectorySucceeded = true;

    KernelWriteString("Working directory: asserting chdir, getcwd, relative resolution and "
                      "inheritance by a program.\n");

    if (!VfsRootIsMounted())
    {
        KernelWriteString("Working directory self-test FAILED: nothing is mounted at the "
                          "root.\n");

        return;
    }

    VerifyDirectoryBoot = ThreadAdoptCurrent("boot");

    if (VerifyDirectoryBoot == NULL)
    {
        VerifyDirectoryRequire(false, "the kernel's own flow of control could not be adopted");
    }
    else
    {
        if (VerifyDirectoryRun(&status) && (status != 0))
        {
            KernelWriteString("  dir-check FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifyDirectorySucceeded = false;
        }

        ThreadDestroy(VerifyDirectoryBoot);
        VerifyDirectoryBoot = NULL;
    }

    if (VerifyDirectorySucceeded)
    {
        KernelWriteString("Working directory self-test passed: dir-check began at the root, "
                          "moved by chdir, read a relative path, inherited across fork, and "
                          "was refused by name.\n");
    }
    else
    {
        KernelWriteString("Working directory self-test FAILED.\n");
    }
}
