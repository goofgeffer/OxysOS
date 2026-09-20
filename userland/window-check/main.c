/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/window-check/main.c
 * Purpose: Asserts the client protocol of sub-task 9.2 from the only side that
 *          can reach it — a program at privilege level 3 — and ends with the
 *          number of assertions that failed: that a window is made and is told
 *          it holds the focus, that pixels are taken and a rectangle outside
 *          the content is refused, that the focus passes between two windows
 *          of one process and back when one is destroyed, that a window the
 *          caller does not hold is refused, that the screen's bounds are the
 *          self-test's surface, that the session of sub-task 9.5 is claimed
 *          and the two layers it guards refused until it is, and that a wait
 *          for an event sleeps until the kernel's self-test injects one.
 * Key functions: main, WindowRequire.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the five calls and the two structures.
 *   - kernel/test/gfx/client.c: the self-test that runs this, reads the pixels
 *     this program blitted, and injects the key it waits for.
 *   - docs/design/WINDOWS.md, Section 11.
 *
 * The window is left standing on purpose. The last thing this program does is
 * end without destroying its first window, so that the kernel's self-test can
 * assert that a process's ending destroys what it owned — the one property of
 * the protocol a program cannot assert of itself, being no longer there.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

static int WindowFailures;

static void WindowRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        ++WindowFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

#define CHECK_WIDTH  60
#define CHECK_HEIGHT 40

static uint32_t CheckPixels[CHECK_WIDTH * CHECK_HEIGHT];

/* A rectangle, built in place so that a call reads what it was given. */
static SyscallWindowRectangle CheckRectangle(int32_t x, int32_t y, int32_t width, int32_t height)
{
    SyscallWindowRectangle rectangle;

    rectangle.x = x;
    rectangle.y = y;
    rectangle.width = width;
    rectangle.height = height;

    return rectangle;
}

int main(void)
{
    SyscallWindowRectangle geometry = CheckRectangle(10, 10, CHECK_WIDTH, CHECK_HEIGHT);
    SyscallWindowRectangle area;
    SyscallWindowEvent event;
    int64_t first;
    int64_t second;

    (void)printf("window-check: the client protocol, from privilege level 3.\n");

    /* --- The screen is the one the self-test composed. --- */

    WindowRequire((OxysWindowScreen(&area) == 0) && (area.x == 0) && (area.y == 0) &&
                      (area.width == 160) && (area.height == 120),
                  "the screen's bounds are not the self-test's surface");
    errno = 0;
    WindowRequire((OxysWindowScreen(NULL) == -1) && (errno == EFAULT),
                  "the screen written to an address the program may not use was not EFAULT");

    /* --- A window is made, and is told it holds the focus. --- */

    first = OxysWindowCreate(&geometry, "check", SYSCALL_WINDOW_LAYER_NORMAL);
    WindowRequire(first >= 0, "the first window could not be made");
    WindowRequire((OxysWindowEvent(first, &event, 0U) == 1) &&
                      (event.kind == SYSCALL_WINDOW_EVENT_FOCUS_IN),
                  "the first event was not the focus arriving");
    WindowRequire(OxysWindowEvent(first, &event, 0U) == 0,
                  "an empty queue did not report no event");

    /* --- Pixels are taken; a rectangle outside the content is not. --- */

    for (int32_t row = 0; row < CHECK_HEIGHT; ++row)
    {
        for (int32_t column = 0; column < CHECK_WIDTH; ++column)
        {
            CheckPixels[(row * CHECK_WIDTH) + column] =
                ((uint32_t)column << 16) | ((uint32_t)row << 8) | 0x5AU;
        }
    }

    area = CheckRectangle(0, 0, CHECK_WIDTH, CHECK_HEIGHT);
    WindowRequire(OxysWindowBlit(first, &area, CheckPixels) == 0,
                  "a blit of the whole content was refused");

    area = CheckRectangle(50, 30, 20, 20);
    errno = 0;
    WindowRequire((OxysWindowBlit(first, &area, CheckPixels) == -1) && (errno == EINVAL),
                  "a blit reaching outside the content was not EINVAL");

    area = CheckRectangle(0, 0, 8, 8);
    errno = 0;
    WindowRequire((OxysWindowBlit(first, &area, NULL) == -1) && (errno == EFAULT),
                  "a blit from an address the program may not use was not EFAULT");

    errno = 0;
    WindowRequire((OxysWindowBlit(first, NULL, CheckPixels) == -1) && (errno == EFAULT),
                  "a blit with a rectangle at an address the program may not use was not EFAULT");

    /* --- The focus passes between two windows of one process, and back. --- */

    geometry = CheckRectangle(30, 30, CHECK_WIDTH, CHECK_HEIGHT);
    second = OxysWindowCreate(&geometry, "two", SYSCALL_WINDOW_LAYER_NORMAL);
    WindowRequire((second >= 0) && (second != first), "the second window could not be made");
    /* Read as "any", which a program with two windows must be able to do:
     * two events, one from each window, in either order. */
    {
        SyscallWindowEvent other;
        bool first_lost = false;
        bool second_gained = false;

        WindowRequire((OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, 0U) == 1) &&
                          (OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &other, 0U) == 1),
                      "two events of any window could not be read");

        first_lost = ((event.window == (uint32_t)first) &&
                      (event.kind == SYSCALL_WINDOW_EVENT_FOCUS_OUT)) ||
                     ((other.window == (uint32_t)first) &&
                      (other.kind == SYSCALL_WINDOW_EVENT_FOCUS_OUT));
        second_gained = ((event.window == (uint32_t)second) &&
                         (event.kind == SYSCALL_WINDOW_EVENT_FOCUS_IN)) ||
                        ((other.window == (uint32_t)second) &&
                         (other.kind == SYSCALL_WINDOW_EVENT_FOCUS_IN));

        WindowRequire(first_lost, "the first window was not told it lost the focus");
        WindowRequire(second_gained, "the second window was not told it gained the focus");
        WindowRequire(OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, 0U) == 0,
                      "any window reported an event when none had one");
    }

    WindowRequire(OxysWindowDestroy(second) == 0, "the second window could not be destroyed");
    WindowRequire((OxysWindowEvent(first, &event, 0U) == 1) &&
                      (event.kind == SYSCALL_WINDOW_EVENT_FOCUS_IN),
                  "the focus did not pass back when the second window was destroyed");

    /* --- A window the caller does not hold is EBADF. --- */

    errno = 0;
    WindowRequire((OxysWindowDestroy(second) == -1) && (errno == EBADF),
                  "a destroyed window could be destroyed again");
    errno = 0;
    WindowRequire((OxysWindowEvent(999, &event, 0U) == -1) && (errno == EBADF),
                  "a window number naming nothing was not EBADF");
    errno = 0;
    WindowRequire((OxysWindowEvent(first, &event, 0x2U) == -1) && (errno == EINVAL),
                  "a flag that does not exist was not EINVAL");

    /* A window the kernel's self-test made, which this program does not own,
     * is number 0 by the order of creation; see kernel/test/gfx/client.c. */
    errno = 0;
    WindowRequire((OxysWindowDestroy(0) == -1) && (errno == EBADF),
                  "the kernel's own window could be destroyed by a program");

    /* --- A move is accepted; where the frame went the program cannot see. --- */

    WindowRequire(OxysWindowMove(first, -1000, -1000) == 0, "a move was refused");
    errno = 0;
    WindowRequire((OxysWindowMove(first, 100000, 0) == -1) && (errno == EINVAL),
                  "a move beyond the coordinate limit was not EINVAL");

    /* --- The session of sub-task 9.5, and the layers it alone may use. --- */

    {
        SyscallWindowRectangle full = CheckRectangle(0, 0, 32, 32);

        /* This program has not claimed the session, and the two layers are
         * refused it — for being the wrong program, which is EPERM, and not
         * for asking wrongly, which would be EINVAL. */
        errno = 0;
        WindowRequire((OxysWindowCreate(&full, "root", SYSCALL_WINDOW_LAYER_ROOT) == -1) &&
                          (errno == EPERM),
                      "a root was made by a program that does not hold the session");
        errno = 0;
        WindowRequire((OxysWindowCreate(&full, "panel", SYSCALL_WINDOW_LAYER_PANEL) == -1) &&
                          (errno == EPERM),
                      "a panel was made by a program that does not hold the session");
        errno = 0;
        WindowRequire((OxysWindowCreate(&full, "odd", 9U) == -1) && (errno == EINVAL),
                      "a layer that does not exist was not EINVAL");

        /* Claiming it is permitted — nothing holds it during this test — and
         * claiming it twice is not an error. */
        WindowRequire(OxysWindowSession() == 0, "the session could not be claimed");
        WindowRequire(OxysWindowSession() == 0, "claiming the session twice was refused");

        /* And then the two layers are permitted. */
        {
            const int64_t root = OxysWindowCreate(&full, "root", SYSCALL_WINDOW_LAYER_ROOT);

            WindowRequire(root >= 0, "the session could not make a root");
            WindowRequire(OxysWindowDestroy(root) == 0, "the session's root could not be destroyed");
        }
    }

    /*
     * --- The text of sub-task 9.5, into a window of its own. ---
     *
     * Not into the first window: the kernel's self-test reads that window's
     * pixels while this program sleeps, and text drawn over them would be a
     * failure of this program's making reported as a failure of the blit.
     */
    {
        SyscallWindowRectangle small = CheckRectangle(0, 0, 32, 32);
        const int64_t scratch = OxysWindowCreate(&small, "text", SYSCALL_WINDOW_LAYER_NORMAL);
        SyscallWindowText placement;

        placement.x = 1;
        placement.y = 1;
        placement.ink = 0x00FFFFFFU;
        placement.paper = 0U;
        placement.scale = 1;

        WindowRequire(scratch >= 0, "a window for the text could not be made");
        WindowRequire(OxysWindowText(scratch, &placement, "text") == 0,
                      "text was refused a window this program holds");

        errno = 0;
        WindowRequire((OxysWindowText(999, &placement, "text") == -1) && (errno == EBADF),
                      "text into a window naming nothing was not EBADF");
        errno = 0;
        WindowRequire((OxysWindowText(scratch, &placement, NULL) == -1) && (errno == EFAULT),
                      "text from an address the program may not use was not EFAULT");

        placement.scale = 0;
        errno = 0;
        WindowRequire((OxysWindowText(scratch, &placement, "text") == -1) && (errno == EINVAL),
                      "a scale of zero was not EINVAL");

        WindowRequire(OxysWindowDestroy(scratch) == 0, "the text's window could not be destroyed");
    }

    /*
     * --- The wait: asleep until the kernel's self-test injects a key. ---
     *
     * The queue is drained first. Making and destroying the windows above
     * passed the focus away from this window and back, and each passing is an
     * event; a wait that met one of those would return at once with something
     * this program had caused itself, and would assert nothing about being
     * woken.
     */
    while (OxysWindowEvent(first, &event, 0U) == 1)
    {
    }


    WindowRequire((OxysWindowEvent(first, &event, SYSCALL_WINDOW_WAIT) == 1) &&
                      (event.kind == SYSCALL_WINDOW_EVENT_KEY) &&
                      (event.key_character == 'w') && (event.key_pressed == 1U),
                  "the wait did not end with the key the self-test injected");

    (void)printf("window-check: %d assertion(s) failed.\n", WindowFailures);

    /* The first window is left for the ending to destroy. */
    return WindowFailures;
}
