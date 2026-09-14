/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/proc/process.c
 * Purpose: Asserts the process and thread tables: that a process is given an
 *          address space of its own, that a thread is given a kernel stack of
 *          its own with a guard beneath it, that identifiers are not reused
 *          while a slot is, and that destruction gives everything back.
 * Key functions: KernelVerifyProcess.
 * References:
 *   - docs/design/PROCESS.md, Section 6: these assertions paired with the silent
 *     failure each would catch.
 *
 * Nothing here runs a thread. There is nothing yet that could: no context has a
 * return address upon its stack and no address space is ever made active by the
 * file under test. What is asserted is what the structures hold and what the
 * allocations did, which is the whole of what this sub-task claims.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/proc/process.h>
#include <oxys/arch/mm/addrspace.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/mm/paging.h>
#include <oxys/arch/cpu/tss.h>
#include <oxys/mm/vmm.h>

static bool KernelProcessSucceeded;

static void KernelProcessRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelProcessSucceeded = false;
    }
}

void KernelVerifyProcess(void)
{
    Process *first;
    Process *second;
    Thread *thread;
    Thread *sibling;
    uint64_t first_id;
    uint64_t thread_id;
    const uint64_t arena_before = KernelVirtualPagesInUse();
    const uint64_t rsp0_before = TssKernelStack();

    KernelProcessSucceeded = true;

    KernelWriteString("Process: asserting the control block and the thread structure.\n");

    /* --- A process, and an address space of its own. --- */

    first = ProcessCreate("first", NULL);
    KernelProcessRequire(first != NULL, "a process could not be created");

    if (first == NULL)
    {
        KernelWriteString("Process self-test FAILED.\n");
        return;
    }

    KernelProcessRequire(first->id != 0U, "a process was given the identifier nobody has");
    KernelProcessRequire(first->parent_id == 0U,
                         "a process with no parent recorded one");
    KernelProcessRequire(first->state == PROCESS_CREATED,
                         "a new process is not in the created state");
    KernelProcessRequire(first->thread_count == 0U, "a new process already has threads");
    KernelProcessRequire(first->space.root != 0U, "a process has no address space");

    second = ProcessCreate("second", first);
    KernelProcessRequire(second != NULL, "a second process could not be created");

    if (second == NULL)
    {
        ProcessDestroy(first);
        KernelWriteString("Process self-test FAILED.\n");
        return;
    }

    /*
     * Two processes must not share a hierarchy. A second space that came back
     * with the first's root would be two programs writing over one another with
     * every appearance of isolation.
     */
    KernelProcessRequire(second->space.root != first->space.root,
                         "two processes share one address space");
    KernelProcessRequire(second->id != first->id,
                         "two processes were given the same identifier");
    KernelProcessRequire(second->parent_id == first->id,
                         "a child did not record its parent");

    /*
     * The parent is recorded by number. A pointer would name a slot, and a slot
     * is given to somebody else the moment its occupant is destroyed — so a
     * child holding one would name the wrong process convincingly.
     */
    first_id = first->id;
    KernelProcessRequire(ProcessById(first_id) == first,
                         "a process could not be found by its identifier");
    KernelProcessRequire(ProcessById(0U) == NULL,
                         "the identifier nobody has named a process");

    /* --- A thread, and a stack of its own. --- */

    thread = ThreadCreate(first, 0x400000U, 0U);
    KernelProcessRequire(thread != NULL, "a thread could not be created");

    if (thread == NULL)
    {
        ProcessDestroy(second);
        ProcessDestroy(first);
        KernelWriteString("Process self-test FAILED.\n");
        return;
    }

    thread_id = thread->id;

    KernelProcessRequire(thread->owner == first, "a thread does not know its process");
    KernelProcessRequire(first->thread_count == 1U,
                         "a process did not record the thread it was given");
    KernelProcessRequire(first->threads[0] == thread,
                         "a process recorded a thread other than the one created");
    KernelProcessRequire(ThreadById(thread_id) == thread,
                         "a thread could not be found by its identifier");

    /*
     * The stack pointer begins at the top, which is one past the last byte: the
     * first push decrements it and writes within the range. A top set to the
     * last byte would have the first push write one byte beyond the stack.
     */
    KernelProcessRequire(thread->context.rsp == thread->kernel_stack_top,
                         "a thread's stack pointer does not begin at the top of its "
                         "stack");
    KernelProcessRequire(
        thread->kernel_stack_top ==
            ((uint64_t)(uintptr_t)thread->kernel_stack_base +
             ((THREAD_KERNEL_STACK_PAGES + THREAD_GUARD_PAGES) * PAGE_SIZE)),
        "a thread's stack top is not the end of its reservation");

    /*
     * The guard. It is the lowest page of the reservation and must not be
     * writable: an overflow is a push, a push is a write, and a write to a page
     * that is not writable faults instead of quietly running into whatever lies
     * below. Until this sub-task there was one kernel stack whose overflow ran
     * into the double-fault stack and was caught by accident of placement.
     */
    KernelProcessRequire(
        !PagingAddressIsWritable((VirtualAddress)(uintptr_t)thread->kernel_stack_base),
        "the page beneath a thread's stack is writable, so it is not a guard");

    /*
     * And the first page of the stack proper *is* writable, which is the other
     * half: a guard that covered the stack as well would be a thread that
     * faulted upon its first push.
     */
    KernelProcessRequire(
        PagingAddressIsWritable((VirtualAddress)(uintptr_t)thread->kernel_stack_base +
                                PAGE_SIZE),
        "the first page of a thread's stack is not writable");

    /* Two threads of one process must not share a stack: a system call made by
     * one would return into the other. */
    sibling = ThreadCreate(first, 0x400000U, 0U);
    KernelProcessRequire(sibling != NULL, "a second thread could not be created");

    if (sibling != NULL)
    {
        KernelProcessRequire(sibling->kernel_stack_top != thread->kernel_stack_top,
                             "two threads of one process share a kernel stack");
        KernelProcessRequire(sibling->id != thread->id,
                             "two threads were given the same identifier");
        KernelProcessRequire(first->thread_count == 2U,
                             "a process did not record its second thread");
    }

    /* --- Making a thread current writes rsp0. --- */

    /*
     * This is the promise sub-task 6.1 left standing: `TssSetKernelStack` has
     * existed and been uncalled since then. A thread entered while `rsp0` still
     * named another thread's stack would take its first interrupt onto a stack
     * somebody else is using.
     */
    ThreadSetCurrent(thread);
    KernelProcessRequire(ThreadCurrent() == thread,
                         "the current thread was not recorded");
    KernelProcessRequire(TssKernelStack() == thread->kernel_stack_top,
                         "making a thread current did not point rsp0 at its stack");

    if (sibling != NULL)
    {
        ThreadSetCurrent(sibling);
        KernelProcessRequire(TssKernelStack() == sibling->kernel_stack_top,
                             "rsp0 did not follow the thread that became current");
    }

    /* --- A user stack, at the far end of the address space, with the frame the
     * System V ABI requires standing upon it. --- */

    {
        const uint64_t entry = ProcessCreateUserStack(first, NULL);

        KernelProcessRequire(entry == (PROCESS_USER_STACK_TOP -
                                       PROCESS_USER_STACK_FRAME_BYTES),
                             "a user stack was not placed where it belongs");
        KernelProcessRequire(first->user_stack_top == PROCESS_USER_STACK_TOP,
                             "a user stack does not end where it claims");
        KernelProcessRequire(first->user_stack_pages == PROCESS_USER_STACK_PAGES,
                             "a user stack is not the size it claims");

        /*
         * The stack pointer a program begins with is sixteen-byte aligned, which
         * the System V ABI, AMD64 supplement, Section 3.4.1, guarantees it. A
         * program entered upon a misaligned stack faults at the first
         * instruction that uses an aligned move — which is inside a function the
         * program did not write, and nothing about the fault names the stack.
         */
        KernelProcessRequire((entry % 16U) == 0U,
                             "the stack pointer a program begins with is not "
                             "sixteen-byte aligned");

        /*
         * And it is below the top by exactly the frame, so the eightbytes the
         * ABI names are within the mapped region rather than one past it. A
         * stack pointer left at the top is the defect this frame exists to
         * prevent, and it is invisible until a program reads its argument count.
         */
        KernelProcessRequire((PROCESS_USER_STACK_TOP - entry) ==
                                 PROCESS_USER_STACK_FRAME_BYTES,
                             "the initial frame is not between the stack pointer "
                             "and the top of the stack");

        /*
         * Asked for twice, given once. A second stack would map pages over the
         * first and the process would lose whatever it had pushed.
         */
        KernelProcessRequire(ProcessCreateUserStack(first, NULL) == 0U,
                             "a second user stack was given to one process");

        /* The extent record MEMORY-LAYOUT.md asked for: an address space cannot
         * say what it maps, and a process can, because a process is what put
         * things there. */
        KernelProcessRequire(first->mapped_pages >= PROCESS_USER_STACK_PAGES,
                             "the pages given to a process were not counted");
    }

    /* --- A thread outliving its process, which must not happen. --- */

    KernelProcessRequire(ThreadById(thread_id) != NULL,
                         "a thread vanished before its process did");

    ProcessDestroy(first);

    KernelProcessRequire(ProcessById(first_id) == NULL,
                         "a destroyed process is still found by its identifier");
    KernelProcessRequire(ThreadById(thread_id) == NULL,
                         "a thread outlived the process that owned it");

    /*
     * And the current thread is no longer one of them. A pointer left standing
     * would leave `rsp0` naming a stack that has gone back to the arena, and the
     * next entry from privilege level 3 would arrive upon memory belonging to
     * somebody else.
     */
    KernelProcessRequire(ThreadCurrent() == NULL,
                         "a destroyed thread is still the current one");

    ProcessDestroy(second);

    /*
     * Everything is given back. The arena is the measure: a thread's stack comes
     * from it, and a leak of four pages a thread is a kernel that runs out of
     * address space after a few thousand programs.
     */
    KernelProcessRequire(KernelVirtualPagesInUse() == arena_before,
                         "the arena did not return to what it held before");
    KernelProcessRequire(ProcessCount() == 0U, "a process slot was not released");
    KernelProcessRequire(ThreadCount() == 0U, "a thread slot was not released");

    /*
     * An identifier is never reused although a slot always is. The next process
     * takes the slot the first one had and must not take its number: a parent
     * that outlived its child would otherwise find a stranger where its child
     * had been, and act upon it.
     */
    {
        Process *const reused = ProcessCreate("reused", NULL);

        KernelProcessRequire(reused != NULL, "a process could not be created after the "
                                             "table was emptied");

        if (reused != NULL)
        {
            KernelProcessRequire(reused->id != first_id,
                                 "a new process was given a destroyed one's identifier");
            KernelProcessRequire(ProcessById(first_id) == NULL,
                                 "a destroyed identifier names the process that took its "
                                 "slot");
            ProcessDestroy(reused);
        }
    }

    /* The task state segment is left as it was found, the boot stack being what
     * everything after this test runs upon. */
    TssSetKernelStack(rsp0_before);

    KernelWriteString(KernelProcessSucceeded ? "Process self-test passed.\n"
                                             : "Process self-test FAILED.\n");
}
