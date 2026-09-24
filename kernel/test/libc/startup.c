/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/startup.c
 * Purpose: Asserts the work of sub-task 7.5 — the C runtime startup object and
 *          the static-linking procedure — by loading the program that procedure
 *          produced and running it at privilege level 3.
 * Key functions: KernelVerifyStartup.
 * References:
 *   - docs/design/LIBC.md: what this asserts, what the program
 *     asserts of itself, and why the two halves are both needed.
 *   - userland/startup-check/main.c: the program, which makes its own
 *     assertions and ends with the number that failed.
 *   - kernel/test/libc/startup_image.asm: the bytes, embedded in this image.
 *   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1:
 *     the initial process stack the program reads, whose kernel half is
 *     ProcessCreateUserStack.
 *
 * **This is the first test here whose subject was built rather than composed.**
 *
 *   Every program this project has run before this one was assembled byte by
 *   byte by kernel/test/program.c, because there was no way to build one. There
 *   now is, and the difference is not a convenience: a composed program asserts
 *   the instructions somebody wrote into an array, and a built one asserts the
 *   toolchain, the linker script, the startup object, the archive, and every
 *   translation unit of the C library compiled with the flags a program uses
 *   rather than the kernel's. Those flags differ — the kernel's `-mcmodel=kernel`
 *   is wrong for a program — so this is a genuine second compilation of code
 *   that had only ever been compiled one way.
 *
 * Why the assertions are in two places.
 *
 *   The program asserts what only a program can reach: what stands upon its
 *   stack, that printf transmits, that malloc obtains memory from the break,
 *   that atexit runs its registrations in reverse and that exit flushes after
 *   them. It reports each failure by printing, and ends with the number that
 *   failed.
 *
 *   This file asserts what only the kernel can see: that the program loaded at
 *   all, that it was entered, that it ended, and **what status it ended with**.
 *   The status is the one that matters, because the program's own reporting
 *   depends upon the very machinery it is testing — a program whose printf did
 *   not work would print nothing, and a test whose only evidence was output
 *   would read silence as success.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/proc/process.h>
#include <oxys/exec/elf.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/syscall/syscall.h>

static bool VerifyStartupSucceeded;

static void VerifyStartupRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyStartupSucceeded = false;
    }
}

/*
 * The program, placed in this image by kernel/test/libc/startup_image.asm.
 *
 * They are declared as arrays of unknown size and not as pointers. A linker
 * symbol has an address and no storage, so `extern const uint8_t *begin` would
 * declare a pointer *at* that address and read the program's first eight bytes
 * as one — which is the classic way to misuse a linker symbol, and it produces a
 * plausible address that faults somewhere else.
 */
extern const uint8_t KernelStartupProgramBegin[];
extern const uint8_t KernelStartupProgramEnd[];

void KernelVerifyStartup(void)
{
    const uint64_t length =
        (uint64_t)(KernelStartupProgramEnd - KernelStartupProgramBegin);

    Process *process;
    Thread *boot;
    Thread *thread;
    ElfImage image;
    uint64_t stack;
    const uint64_t terminations_before = ProcessTerminationCount();
    const uint64_t dispatched_before = SyscallDispatched();

    VerifyStartupSucceeded = true;

    KernelWriteString("Startup: asserting the runtime startup object and the link, "
                      "by running what they produced.\n");

    VerifyStartupRequire(length > 0U,
                         "the image carries no program, so the link produced nothing");

    if (length == 0U)
    {
        KernelWriteString("Startup self-test FAILED.\n");

        return;
    }

    /*
     * The loader is asked to judge the image before it is run.
     *
     * This is worth doing separately rather than relying upon ElfLoad's own
     * refusal, because the whole of the linker script is a set of claims about
     * what the loader will accept — segments in ascending order, none below the
     * first page, none at or above the boundary a program may not name — and a
     * script that got one of them wrong would produce a program that simply did
     * not load, reported as "the program did not load" rather than as what was
     * actually wrong with it.
     */
    VerifyStartupRequire(ElfValidate(KernelStartupProgramBegin, length) == ELF_OK,
                         "the linked program is not an image this loader accepts");

    /*
     * And the program is not absurdly larger than the code in it.
     *
     * This is a bound upon the *link* and not upon the program. Two things make
     * an image enormously larger than what it does, and neither produces a
     * program that misbehaves: debugging information the loader never reads,
     * and the linker's default two-mebibyte segment alignment, which pads the
     * file between every pair of segments. Both were present in the first
     * version of this sub-task's build; both are corrected — the embedded copy
     * is stripped, and `-z max-page-size=0x1000` is passed — and neither
     * correction was visible to any assertion until this one.
     *
     * Sixty-four kibibytes is about four times what the program presently
     * occupies, which is room for it to grow and far below the six mebibytes
     * either defect produces. It is a bound and not an exact size, because an
     * exact size is a number that a change to a diagnostic would break.
     */
    VerifyStartupRequire(length <= (64U * 1024U),
                         "the linked program is far larger than the code within "
                         "it, so the link is padding or carrying it");

    process = ProcessCreate("startup-check", NULL);
    VerifyStartupRequire(process != NULL, "a process could not be created");

    if (process == NULL)
    {
        KernelWriteString("Startup self-test FAILED.\n");

        return;
    }

    if (ElfLoad(&process->space, KernelStartupProgramBegin, length, &image) != ELF_OK)
    {
        ProcessDestroy(process);
        KernelWriteString("  The linked program did not load. FAILED.\n");
        KernelWriteString("Startup self-test FAILED.\n");

        return;
    }

    ProcessRecordImage(process, &image);

    /*
     * The entry point is the startup object's and not the program's.
     *
     * `_start` is placed first by the linker script, so the entry the loader
     * read from the file header is the lowest address of the text. A link that
     * put `main` first would produce an image entered at `main` with no argument
     * count upon the stack and no `exit` after it — which runs, prints, and then
     * executes whatever follows `main` in the text.
     */
    VerifyStartupRequire(image.entry == image.lowest,
                         "the program is not entered at its first instruction, so the "
                         "startup object is not what runs first");

    stack = ProcessCreateUserStack(process, NULL);
    VerifyStartupRequire(stack != 0U, "the program was given no stack");

    boot = ThreadAdoptCurrent("boot");
    thread = ThreadCreate(process, image.entry, stack);
    VerifyStartupRequire(thread != NULL, "a thread could not be created");

    if ((boot == NULL) || (thread == NULL) || (stack == 0U))
    {
        ProcessDestroy(process);

        if (boot != NULL)
        {
            ThreadDestroy(boot);
        }

        KernelWriteString("Startup self-test FAILED.\n");

        return;
    }

    /*
     * And it runs. What appears between this line and the next was written by a
     * program, through this library's own printf, at privilege level 3 — which
     * is the half of sub-task 7.4 the kernel could not assert.
     */
    VerifyStartupRequire(ThreadStart(thread), "the program could not be started");

    VerifyStartupRequire(ThreadCurrent() == boot,
                         "the kernel did not resume the thread that started the "
                         "program");

    VerifyStartupRequire(ProcessTerminationCount() == (terminations_before + 1U),
                         "the program was not ended");
    VerifyStartupRequire(process->state == PROCESS_EXITED,
                         "the program's process was not marked as ended");

    /*
     * **The status is zero, and this is the assertion the test rests upon.**
     *
     * It is the count of the program's own failed assertions, returned from
     * `main` and carried to the kernel by the startup object and `exit`. A
     * non-zero status is a program that found something wrong and said so; a
     * program that printed nothing at all still reports through this number,
     * which is why the number exists rather than the log being the only
     * evidence.
     *
     * It also asserts the path the number travelled: `main` returned it in the
     * lower half of one register, `_start` moved it into the first argument
     * register, `exit` called every registration and flushed every stream, and
     * `_Exit` passed it to the kernel. A startup object that dropped the value,
     * or that moved the whole of a register that `main` had left something else
     * in, produces a status that is not zero and is not the count either.
     */
    VerifyStartupRequire(process->exit_status == 0,
                         "the program reported failed assertions of its own, or did "
                         "not return its status through the startup object");

    /*
     * The program made system calls, and a count with a floor rather than an
     * exact number.
     *
     * Every other program-running test here asserts an exact count, because
     * every other program was composed instruction by instruction and the number
     * of calls it makes is a property of what somebody wrote. This one is
     * compiled: how many `write` calls its buffered output becomes depends upon
     * how the buffer filled, which depends upon the length of a diagnostic
     * somebody may reword. A floor asserts what is worth asserting — that the
     * program reached the kernel repeatedly rather than faulting on its first
     * call — without being a number that a change to a message would break.
     */
    VerifyStartupRequire((SyscallDispatched() - dispatched_before) >= 5U,
                         "the program did not make the system calls its output "
                         "required");

    ProcessDestroy(process);
    ThreadDestroy(boot);

    KernelWriteString(VerifyStartupSucceeded ? "Startup self-test passed.\n"
                                             : "Startup self-test FAILED.\n");
}
