/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/gfx/client.c
 * Purpose: Asserts the client protocol of sub-task 9.2: window-check is run at
 *          privilege level 3 upon a window manager holding a screen composed in
 *          memory, a kernel thread stands in for the world outside the program
 *          — it composes the screen, reads the pixels the program blitted, and
 *          injects the key the program sleeps for — and the kernel asserts
 *          afterwards that the program's ending took its window with it and
 *          left the kernel's own standing.
 * Key functions: KernelVerifyClients, VerifyClientsHand, VerifyClientsRun.
 * References:
 *   - docs/design/WINDOWS.md, Section 11: every assertion here paired with the
 *     silent failure it would catch.
 *   - userland/window-check/main.c: the program, and what it asserts for
 *     itself.
 *   - docs/design/SCHEDULER.md, Section 7: the fixture thread that blocks
 *     itself when done, which this file's hand is another of.
 *
 * Why there is a kernel thread.
 *
 *   The program blits pixels and then waits for an event, and the two halves of
 *   the protocol that matter most — that the pixels arrived, and that a sleeper
 *   is woken by an event — can be asserted only while the program is asleep:
 *   after it has blitted and before it has ended. The kernel's own flow of
 *   control cannot look then, having handed the processor to the program by
 *   ThreadStart and getting it back only when the program ends. A thread the
 *   scheduler runs when the program sleeps can, and that is what the hand is:
 *   it waits for the program to sleep, composes the screen and reads the
 *   pattern where the window stands, injects a key into the window's queue,
 *   wakes the sleepers, and blocks itself for good. It is pinned to the
 *   bootstrap processor, so that it touches the manager's tables upon the one
 *   processor the program's calls touch them from, and never beside them.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>
#include <oxys/proc/sched.h>
#include <oxys/gfx/window.h>
#include <oxys/gfx/client.h>
#include <oxys/gfx/graphics.h>
#include <oxys/dev/keyboard.h>

extern const uint8_t KernelProgramWindowCheckBegin[];
extern const uint8_t KernelProgramWindowCheckEnd[];

#define KERNEL_CLIENT_WIDTH  160U
#define KERNEL_CLIENT_HEIGHT 120U
#define KERNEL_CLIENT_PITCH  (KERNEL_CLIENT_WIDTH + 5U)

static uint32_t KernelClientStore[KERNEL_CLIENT_HEIGHT * KERNEL_CLIENT_PITCH];

static const WindowPalette KernelClientPalette = {
    UINT32_C(0x00111111), UINT32_C(0x00222222), UINT32_C(0x00333333),
    UINT32_C(0x00444444), UINT32_C(0x00555555), UINT32_C(0x00666666),
    UINT32_C(0x00777777)
};

static bool VerifyClientsSucceeded;

static void VerifyClientsRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyClientsSucceeded = false;
    }
}

static uint32_t KernelClientPixel(int32_t x, int32_t y)
{
    return KernelClientStore[((uint32_t)y * KERNEL_CLIENT_PITCH) + (uint32_t)x];
}

/* ------------------------------------------------------------- the hand */

/* What the hand found, read by the test after the program has ended. */
static bool VerifyHandRan;
static bool VerifyHandSawSleeper;
static bool VerifyHandPixelsRight;
static bool VerifyHandFocusRight;
static size_t VerifyHandWindow;
static size_t VerifyHandKernelWindow;
static uint64_t VerifyHandSleepsBefore;

static void VerifyClientsHand(void)
{
    VerifyHandRan = true;

    /*
     * Until the program sleeps in window_event, the hand has nothing to do and
     * gives the processor back: the program is running its assertions, and
     * the hand looking at the screen before the blit would find the paper.
     */
    while (WindowClientSleepCount() == VerifyHandSleepsBefore)
    {
        SchedulerYield();
    }

    VerifyHandSawSleeper = true;

    /*
     * The program's first window is the one after the kernel's, and holds the
     * focus, its second having been destroyed; the program moved it off the
     * top left, so its content stands where the confinement put the frame.
     */
    VerifyHandWindow = WindowManagerFocused();
    VerifyHandFocusRight = (VerifyHandWindow != WINDOW_NONE) &&
                           (VerifyHandWindow != VerifyHandKernelWindow) &&
                           (WindowOwner(VerifyHandWindow) != 0U);

    (void)WindowManagerCompose();

    if (VerifyHandFocusRight)
    {
        const GraphicsRectangle content = WindowContentBounds(VerifyHandWindow);
        bool right = true;

        /* The pattern window-check blits: red the column, green the row, blue
         * 0x5A. Read where the content stands upon the screen and is within
         * it, which after the move off the top left is its right-hand part. */
        for (int32_t row = 0; row < content.height; ++row)
        {
            for (int32_t column = 0; column < content.width; ++column)
            {
                const int32_t x = content.x + column;
                const int32_t y = content.y + row;

                if ((x < 0) || (y < 0) || (x >= (int32_t)KERNEL_CLIENT_WIDTH) ||
                    (y >= (int32_t)KERNEL_CLIENT_HEIGHT))
                {
                    continue;
                }

                if (KernelClientPixel(x, y) !=
                    (((uint32_t)column << 16) | ((uint32_t)row << 8) | 0x5AU))
                {
                    right = false;
                }
            }
        }

        VerifyHandPixelsRight = right;
    }

    /* The key the program waits for, and the wake. */
    {
        KeyEvent key;

        key.scancode = 0x11U;
        key.character = 'w';
        key.modifiers = 0U;
        key.pressed = true;
        key.extended = false;

        WindowManagerHandleKey(&key);
        WindowClientWakeAll();
    }

    SchedulerBlockCurrent();
}

/* ---------------------------------------------------------- the program */

static Thread *VerifyClientsBoot;

static bool VerifyClientsRun(int64_t *status)
{
    const uint64_t length =
        (uint64_t)(KernelProgramWindowCheckEnd - KernelProgramWindowCheckBegin);
    static const char name[] = "window-check";
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
        VerifyClientsRequire(false, "a process could not be created for window-check");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramWindowCheckBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyClientsRequire(false, "window-check did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyClientsRequire(false, "window-check was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyClientsRequire(false, "window-check could not be started");

        return false;
    }

    VerifyClientsRequire(ThreadCurrent() == VerifyClientsBoot,
                         "the kernel did not resume the thread that started window-check");
    VerifyClientsRequire(process->state == PROCESS_EXITED,
                         "window-check's process was not marked as ended");
    VerifyClientsRequire(SYSCALL_STATUS_KIND(process->wait_status) == SYSCALL_STATUS_KIND_EXITED,
                         "window-check itself was ended by a signal");

    *status = process->exit_status;

    /* The ending, not the destruction, is what takes the windows: asserted
     * before ProcessDestroy so that the two are not confused. */
    VerifyClientsRequire(WindowManagerCount() == 1U,
                         "the program's ending did not destroy the window it left standing");

    ProcessDestroy(process);

    VerifyClientsRequire(VfsOpenFileCount() == open_before,
                         "window-check left an open file behind it");
    VerifyClientsRequire(ProcessCount() == processes_before,
                         "window-check left a child in the table");

    return true;
}

void KernelVerifyClients(void)
{
    GraphicsSurface screen;
    int64_t status = 0;
    Thread *hand;

    VerifyClientsSucceeded = true;
    VerifyHandRan = false;
    VerifyHandSawSleeper = false;
    VerifyHandPixelsRight = false;
    VerifyHandFocusRight = false;

    KernelWriteString("Window clients: running window-check at privilege level 3, with a "
                      "hand to read its pixels and wake it.\n");

    for (size_t index = 0U; index < (KERNEL_CLIENT_HEIGHT * KERNEL_CLIENT_PITCH); ++index)
    {
        KernelClientStore[index] = 0U;
    }

    (void)GraphicsSurfaceInitialise(&screen, KernelClientStore, KERNEL_CLIENT_WIDTH,
                                    KERNEL_CLIENT_HEIGHT, KERNEL_CLIENT_PITCH * 4U, 4U);

    if (!WindowManagerInitialise(&screen, &KernelClientPalette, NULL))
    {
        VerifyClientsRequire(false, "the manager refused the surface in memory");
        KernelWriteString("Window client self-test FAILED.\n");

        return;
    }

    /* The kernel's own window, owner zero, which the program must not be able
     * to touch and the program's ending must not take. Number 0, being first. */
    VerifyHandKernelWindow = WindowCreate(100, 60, 40, 30, "kernel");
    VerifyClientsRequire(VerifyHandKernelWindow == 0U, "the kernel's window is not number 0");

    /* The calls refuse the kernel's own flow of control, which owns nothing. */
    {
        int64_t result = WindowClientDestroy(0U);

        VerifyClientsRequire(result == SYSCALL_EBADF,
                             "a call from no process was not refused as EBADF");
    }

    VerifyHandSleepsBefore = WindowClientSleepCount();
    VerifyClientsBoot = ThreadAdoptCurrent("boot");

    if (VerifyClientsBoot == NULL)
    {
        VerifyClientsRequire(false, "the kernel's own flow of control could not be adopted");
    }
    else
    {
        hand = ThreadCreateScheduled(VerifyClientsHand);

        if ((hand == NULL) || !SchedulerSetAffinity(hand, 1U) || !SchedulerAdmit(hand))
        {
            VerifyClientsRequire(false, "the hand could not be started");
        }
        else
        {
            if (VerifyClientsRun(&status) && (status != 0))
            {
                KernelWriteString("  window-check FAILED: the status was ");
                KernelWriteHexadecimal((uint64_t)status);
                KernelWriteString(" and not zero.\n");
                VerifyClientsSucceeded = false;
            }

            VerifyClientsRequire(VerifyHandRan && VerifyHandSawSleeper,
                                 "the hand never ran, or never saw the program sleep");
            VerifyClientsRequire(VerifyHandFocusRight,
                                 "the program's window did not hold the focus when it slept");
            VerifyClientsRequire(VerifyHandPixelsRight,
                                 "the pixels the program blitted are not upon the screen");
            VerifyClientsRequire(WindowClientSleepCount() == VerifyHandSleepsBefore + 1U,
                                 "the program did not sleep exactly once for its event");

            ThreadDestroy(hand);
        }

        ThreadDestroy(VerifyClientsBoot);
        VerifyClientsBoot = NULL;
    }

    VerifyClientsRequire(WindowExists(VerifyHandKernelWindow) &&
                             (WindowOwner(VerifyHandKernelWindow) == 0U),
                         "the kernel's own window did not survive the program's ending");

    WindowManagerShutdown();

    KernelWriteString(VerifyClientsSucceeded ? "Window client self-test passed.\n"
                                             : "Window client self-test FAILED.\n");
}
