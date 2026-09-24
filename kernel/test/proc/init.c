/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/proc/init.c
 * Purpose: Asserts sub-task 9.3: the adoption of orphans by `init`, upon
 *          fixture processes composed in the table, and the two calls `init`
 *          rests upon — `power`, refused every process but `init`, and
 *          `pause` — by running init-check at privilege level 3.
 * Key functions: KernelVerifyInit, VerifyInitOrphans, VerifyInitRun.
 * References:
 *   - docs/design/WINDOWS.md: every assertion here paired with
 *     what it would catch.
 *   - userland/init-check/main.c: the program, and what it asserts for itself.
 *
 * Two halves, as the signal test has. The adoption is pure kernel state — a
 * parent's children becoming `init`'s when the parent ends — and is asserted
 * upon processes composed in the table and never run, exactly as the pending
 * set is: the machinery is a field reassigned and a wake, and needs no
 * privilege transition. The two calls need one, `power` being reserved by the
 * dispatch and `pause` sleeping a real thread, and are asserted by the program.
 *
 * The fixture leaves `init` unset again, so that the real `init` the entry
 * point starts is the one the machine runs with — a self-test that named a
 * destroyed process as `init` would leave the adoption pointing at nothing.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>

extern const uint8_t KernelProgramInitCheckBegin[];
extern const uint8_t KernelProgramInitCheckEnd[];

static bool VerifyInitSucceeded;

static void VerifyInitRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyInitSucceeded = false;
    }
}

/* The adoption, upon fixture processes composed in the table. */
static void VerifyInitOrphans(void)
{
    Process *init;
    Process *parent;
    Process *first;
    Process *second;

    init = ProcessCreate("init-fixture", NULL);
    parent = ProcessCreate("parent-fixture", NULL);

    if ((init == NULL) || (parent == NULL))
    {
        VerifyInitRequire(false, "the adoption fixtures could not be created");

        return;
    }

    first = ProcessCreate("child-fixture", parent);
    second = ProcessCreate("child-fixture", parent);

    if ((first == NULL) || (second == NULL))
    {
        ProcessDestroy(init);
        ProcessDestroy(parent);
        VerifyInitRequire(false, "the orphan fixtures could not be created");

        return;
    }

    VerifyInitRequire((first->parent_id == parent->id) && (second->parent_id == parent->id),
                      "a child was not created with its parent's identifier");

    /* With no init set, nothing is adopted, and the children stay the parent's:
     * an adoption that reparented with no init would send orphans nowhere. */
    ProcessSetInit(0U);
    VerifyInitRequire(ProcessAdoptOrphansOf(parent->id) == 0U,
                      "orphans were adopted with no init to adopt them");
    VerifyInitRequire(first->parent_id == parent->id,
                      "a child was reparented with no init set");

    /* With init set, the parent's children become init's, and init's own is
     * not counted among them: a self-adoption would loop a parent to itself. */
    ProcessSetInit(init->id);
    VerifyInitRequire(ProcessAdoptOrphansOf(parent->id) == 2U,
                      "the two orphans were not both adopted");
    VerifyInitRequire((first->parent_id == init->id) && (second->parent_id == init->id),
                      "an orphan was not reparented to init");
    VerifyInitRequire(ProcessAdoptOrphansOf(init->id) == 0U,
                      "init adopted its own children, which would loop a parent to itself");

    /* Reset the identity before the fixtures are destroyed, so that no adoption
     * afterwards names a process that no longer exists. */
    ProcessSetInit(0U);

    ProcessDestroy(second);
    ProcessDestroy(first);
    ProcessDestroy(parent);
    ProcessDestroy(init);
}

static Thread *VerifyInitBoot;

static bool VerifyInitRun(int64_t *status)
{
    const uint64_t length = (uint64_t)(KernelProgramInitCheckEnd - KernelProgramInitCheckBegin);
    static const char name[] = "init-check";
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();
    const size_t processes_before = ProcessCount();

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
        VerifyInitRequire(false, "a process could not be created for init-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramInitCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyInitRequire(false, "init-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyInitRequire(false, "init-check was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyInitRequire(false, "init-check could not be started");

        return false;
    }

    VerifyInitRequire(process->state == PROCESS_EXITED,
                      "init-check's process was not marked as ended");
    VerifyInitRequire(SYSCALL_STATUS_KIND(process->wait_status) == SYSCALL_STATUS_KIND_EXITED,
                      "init-check itself was ended by a signal");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifyInitRequire(VfsOpenFileCount() == open_before, "init-check left an open file behind it");
    VerifyInitRequire(ProcessCount() == processes_before, "init-check left a child in the table");

    return true;
}

void KernelVerifyInit(void)
{
    int64_t status = 0;

    VerifyInitSucceeded = true;

    KernelWriteString("init: asserting the adoption of orphans, then running init-check.\n");

    VerifyInitOrphans();

    VerifyInitBoot = ThreadAdoptCurrent("boot");

    if (VerifyInitBoot == NULL)
    {
        VerifyInitRequire(false, "the kernel's own flow of control could not be adopted");
    }
    else
    {
        if (VerifyInitRun(&status) && (status != 0))
        {
            KernelWriteString("  init-check FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifyInitSucceeded = false;
        }

        ThreadDestroy(VerifyInitBoot);
        VerifyInitBoot = NULL;
    }

    KernelWriteString(VerifyInitSucceeded
                          ? "init self-test passed: an ended parent's children become init's, "
                            "power is refused every process but init, and pause reports a "
                            "signal.\n"
                          : "init self-test FAILED.\n");
}
