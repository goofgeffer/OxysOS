/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: graphics/client.c
 * Purpose: Implements the client side of the window manager, sub-task 9.2: the
 *          five window calls, the ownership of a window by the process that
 *          made it, the copy of a client's pixels into a window, the conversion
 *          of an event into the structure the ABI declares, and the sleep a
 *          program waits in until an event arrives.
 * Key functions: WindowClientCreate, WindowClientDestroy, WindowClientMove,
 *          WindowClientBlit, WindowClientEvent, WindowClientScreen,
 *          WindowClientSession, WindowClientText,
 *          WindowClientReleaseProcess,
 *          WindowClientWakeAll, WindowClientReport.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the calls and the two structures that
 *     cross, SyscallWindowRectangle and SyscallWindowEvent.
 *   - docs/design/WINDOWS.md, Sections 10 and 11: the protocol and every
 *     assertion made upon it.
 *   - docs/design/SCHEDULER.md, Section 9: the wait channel, and the
 *     discipline of testing the condition and sleeping within one masked
 *     section, which WindowClientEvent follows as the pipe does.
 *
 * Ownership is a tag and nothing more.
 *
 *   The manager keeps a number upon each window and attaches no meaning to it;
 *   this file sets it to the creating process's identifier and refuses every
 *   call whose window carries another — EBADF, as for a descriptor the caller
 *   does not hold, because that is what it is: a handle that is not the
 *   caller's. The kernel's own windows carry zero, which no process is.
 *
 * The pixels are validated whole before one is copied.
 *
 *   The area is checked against the content and the client's buffer against
 *   its address space, both before the first pixel moves. A copy that checked
 *   as it went would leave a window half painted by a call that then reported
 *   EFAULT, and a program that trusted the report would redraw what was never
 *   wrong and leave what was.
 *
 * Concurrency. Every call runs upon the bootstrap processor with interrupts
 * masked, every user thread being pinned there; the tick that fills the queues
 * runs upon the same processor, likewise masked. The two cannot interleave.
 * The sleep of WindowClientEvent tests the queue and sleeps within one masked
 * section, so that a wake from the next tick cannot fall between the two.
 */

#include <oxys/gfx/client.h>
#include <oxys/gfx/window.h>
#include <oxys/gfx/graphics.h>
#include <oxys/syscall_abi.h>
#include <oxys/arch/syscall/syscall.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/proc/process.h>
#include <oxys/proc/sched.h>
#include <oxys/proc/signal.h>
#include <oxys/mm/heap.h>
#include <oxys/kernel.h>

static uint64_t WindowClientCalls;
static uint64_t WindowClientRefusals;
static uint64_t WindowClientSleeps;

/* The one channel every sleeper waits upon; an address and nothing more. */
static uint8_t WindowClientChannel;

/* The caller's process identifier, or zero where there is no process — the
 * kernel's own flow of control calling from a self-test, which owns nothing. */
static uint64_t WindowClientCaller(void)
{
    const Process *const process = ProcessCurrent();

    return (process == NULL) ? 0U : process->id;
}

/*
 * The window named by a call, as the caller's: WINDOW_NONE where the number
 * names no window or one the caller does not own, in which case the refusal
 * is counted.
 */
static size_t WindowClientOwned(uint64_t window)
{
    const uint64_t caller = WindowClientCaller();

    if ((window >= WINDOW_CAPACITY) || !WindowExists((size_t)window) || (caller == 0U) ||
        (WindowOwner((size_t)window) != caller))
    {
        ++WindowClientRefusals;

        return WINDOW_NONE;
    }

    return (size_t)window;
}

/* Reads a rectangle from the caller's memory, field by field. */
static bool WindowClientReadRectangle(uint64_t address, GraphicsRectangle *rectangle)
{
    const SyscallWindowRectangle *source;

    if (!SyscallUserRangeIsReadable(address, (uint64_t)sizeof *source))
    {
        return false;
    }

    source = (const SyscallWindowRectangle *)(uintptr_t)address;
    rectangle->x = source->x;
    rectangle->y = source->y;
    rectangle->width = source->width;
    rectangle->height = source->height;

    return true;
}

int64_t WindowClientCreate(uint64_t geometry_address, uint64_t title_address, uint64_t layer)
{
    GraphicsRectangle geometry;
    char title[WINDOW_TITLE_CAPACITY + 1U];
    const uint64_t caller = WindowClientCaller();
    size_t window;

    ++WindowClientCalls;

    if (!WindowManagerIsActive() || (caller == 0U))
    {
        ++WindowClientRefusals;

        return SYSCALL_ENOTSUP;
    }

    if (layer > SYSCALL_WINDOW_LAYER_PANEL)
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    /*
     * The root and the panel are the session's alone, of sub-task 9.5. Two
     * programs each painting a root would each paint the whole screen and each
     * be right to; what a person would see is whichever composed last, and
     * nothing would say why. The refusal is EPERM rather than EINVAL because
     * the argument is not wrong — the caller is.
     */
    if ((layer != SYSCALL_WINDOW_LAYER_NORMAL) && (caller != WindowSession()))
    {
        ++WindowClientRefusals;

        return SYSCALL_EPERM;
    }

    if (!WindowClientReadRectangle(geometry_address, &geometry) ||
        !SyscallCopyUserString(title_address, title, sizeof title))
    {
        ++WindowClientRefusals;

        return SYSCALL_EFAULT;
    }

    if ((geometry.width < WINDOW_MINIMUM_EXTENT) || (geometry.height < WINDOW_MINIMUM_EXTENT) ||
        (geometry.width > WINDOW_MAXIMUM_EXTENT) || (geometry.height > WINDOW_MAXIMUM_EXTENT))
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    window = WindowCreate(geometry.x, geometry.y, geometry.width, geometry.height, title,
                          (WindowLayer)layer);

    if (window == WINDOW_NONE)
    {
        ++WindowClientRefusals;

        return SYSCALL_ENOMEM;
    }

    WindowSetOwner(window, caller);

    return (int64_t)window;
}

int64_t WindowClientDestroy(uint64_t window)
{
    const size_t owned = WindowClientOwned(window);

    ++WindowClientCalls;

    if (owned == WINDOW_NONE)
    {
        return SYSCALL_EBADF;
    }

    WindowDestroy(owned);

    return SYSCALL_OK;
}

int64_t WindowClientMove(uint64_t window, int64_t x, int64_t y)
{
    const size_t owned = WindowClientOwned(window);

    ++WindowClientCalls;

    if (owned == WINDOW_NONE)
    {
        return SYSCALL_EBADF;
    }

    if ((x < -GRAPHICS_COORDINATE_LIMIT) || (x > GRAPHICS_COORDINATE_LIMIT) ||
        (y < -GRAPHICS_COORDINATE_LIMIT) || (y > GRAPHICS_COORDINATE_LIMIT))
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    WindowMove(owned, (int32_t)x, (int32_t)y);

    return SYSCALL_OK;
}

int64_t WindowClientBlit(uint64_t window, uint64_t area_address, uint64_t pixels_address)
{
    const size_t owned = WindowClientOwned(window);
    GraphicsRectangle area;
    GraphicsRectangle content;
    uint64_t byte_count;

    ++WindowClientCalls;

    if (owned == WINDOW_NONE)
    {
        return SYSCALL_EBADF;
    }

    if (!WindowClientReadRectangle(area_address, &area))
    {
        ++WindowClientRefusals;

        return SYSCALL_EFAULT;
    }

    content = WindowContentBounds(owned);

    if ((area.width <= 0) || (area.height <= 0) || (area.x < 0) || (area.y < 0) ||
        (area.x > content.width - area.width) || (area.y > content.height - area.height))
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    /* The extents are each within WINDOW_MAXIMUM_EXTENT, so the product is
     * within 2^24 pixels and the byte count within 2^26: no overflow. */
    byte_count = (uint64_t)area.width * (uint64_t)area.height * 4U;

    if (!SyscallUserRangeIsReadable(pixels_address, byte_count))
    {
        ++WindowClientRefusals;

        return SYSCALL_EFAULT;
    }

    if (!WindowWritePixels(owned, area, (const uint32_t *)(uintptr_t)pixels_address))
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    return SYSCALL_OK;
}

/* Converts the manager's event into the ABI's, field by field. */
static void WindowClientConvert(const WindowEvent *from, SyscallWindowEvent *to)
{
    switch (from->kind)
    {
    case WINDOW_EVENT_KEY:
        to->kind = SYSCALL_WINDOW_EVENT_KEY;
        break;
    case WINDOW_EVENT_POINTER_MOVE:
        to->kind = SYSCALL_WINDOW_EVENT_POINTER_MOVE;
        break;
    case WINDOW_EVENT_BUTTON_PRESS:
        to->kind = SYSCALL_WINDOW_EVENT_BUTTON_PRESS;
        break;
    case WINDOW_EVENT_BUTTON_RELEASE:
        to->kind = SYSCALL_WINDOW_EVENT_BUTTON_RELEASE;
        break;
    case WINDOW_EVENT_FOCUS_IN:
        to->kind = SYSCALL_WINDOW_EVENT_FOCUS_IN;
        break;
    case WINDOW_EVENT_FOCUS_OUT:
        to->kind = SYSCALL_WINDOW_EVENT_FOCUS_OUT;
        break;
    case WINDOW_EVENT_CLOSE:
    default:
        to->kind = SYSCALL_WINDOW_EVENT_CLOSE;
        break;
    }

    to->x = from->x;
    to->y = from->y;
    to->button = from->button;
    to->buttons = from->buttons;
    to->key_scancode = from->key.scancode;
    to->key_modifiers = from->key.modifiers;
    to->key_character = from->key.character;
    to->key_pressed = from->key.pressed ? 1U : 0U;
    to->key_extended = from->key.extended ? 1U : 0U;
    to->reserved = 0U;
}

/*
 * Where the last event of an "any" request came from, so that the next scan
 * begins after it: a program with a busy window and a quiet one would
 * otherwise never hear from the quiet one while the busy one had events.
 */
static size_t WindowClientLastServed;

/*
 * Reads the oldest event of any window the caller owns, scanning from after
 * the last served. Returns the window it came from, or WINDOW_NONE where none
 * had one; `owns_any` says whether the caller owns any window at all.
 */
static size_t WindowClientReadAny(uint64_t caller, WindowEvent *event, bool *owns_any)
{
    *owns_any = false;

    for (size_t step = 1U; step <= WINDOW_CAPACITY; ++step)
    {
        const size_t index = (WindowClientLastServed + step) % WINDOW_CAPACITY;

        if (!WindowExists(index) || (WindowOwner(index) != caller))
        {
            continue;
        }

        *owns_any = true;

        if (WindowReadEvent(index, event))
        {
            WindowClientLastServed = index;

            return index;
        }
    }

    return WINDOW_NONE;
}

int64_t WindowClientEvent(uint64_t window, uint64_t event_address, uint64_t flags)
{
    const bool any = (window == SYSCALL_WINDOW_ANY);
    const uint64_t caller = WindowClientCaller();
    size_t owned = WINDOW_NONE;
    WindowEvent event;
    SyscallWindowEvent *destination;

    ++WindowClientCalls;

    if (!any)
    {
        owned = WindowClientOwned(window);

        if (owned == WINDOW_NONE)
        {
            return SYSCALL_EBADF;
        }
    }
    else if (caller == 0U)
    {
        ++WindowClientRefusals;

        return SYSCALL_EBADF;
    }

    if ((flags & ~SYSCALL_WINDOW_WAIT) != 0U)
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    if (!SyscallUserRangeIsWritable(event_address, (uint64_t)sizeof *destination))
    {
        ++WindowClientRefusals;

        return SYSCALL_EFAULT;
    }

    for (;;)
    {
        bool taken;
        bool owns_any = true;

        PerCpuPushInterruptState();

        if (any)
        {
            owned = WindowClientReadAny(caller, &event, &owns_any);
            taken = (owned != WINDOW_NONE);
        }
        else
        {
            taken = WindowReadEvent(owned, &event);
        }

        if (taken)
        {
            PerCpuPopInterruptState();
            break;
        }

        if (!owns_any)
        {
            PerCpuPopInterruptState();
            ++WindowClientRefusals;

            return SYSCALL_EBADF;
        }

        if ((flags & SYSCALL_WINDOW_WAIT) == 0U)
        {
            PerCpuPopInterruptState();

            return 0;
        }

        if (!SchedulerCanSleep())
        {
            /* The kernel's own flow of control, which nothing can wake. */
            PerCpuPopInterruptState();
            ++WindowClientRefusals;

            return SYSCALL_ENOTSUP;
        }

        ++WindowClientSleeps;
        SchedulerSleep(&WindowClientChannel);
        PerCpuPopInterruptState();

        if (SignalIsPending(ProcessCurrent()))
        {
            return SYSCALL_EINTR;
        }

        /*
         * Woken, and the window may be gone: a process's windows are destroyed
         * at its ending, but a window can also be destroyed by another thread
         * of the same process, and a sleeper that read a destroyed window's
         * queue would read a table entry that may now be somebody else's.
         */
        if (!any && (!WindowExists(owned) || (WindowOwner(owned) != caller)))
        {
            ++WindowClientRefusals;

            return SYSCALL_EBADF;
        }
    }

    destination = (SyscallWindowEvent *)(uintptr_t)event_address;
    WindowClientConvert(&event, destination);
    destination->window = (uint32_t)owned;

    return 1;
}

int64_t WindowClientScreen(uint64_t geometry_address)
{
    SyscallWindowRectangle *destination;
    GraphicsRectangle bounds;

    ++WindowClientCalls;

    if (!WindowManagerIsActive())
    {
        ++WindowClientRefusals;

        return SYSCALL_ENOTSUP;
    }

    if (!SyscallUserRangeIsWritable(geometry_address, (uint64_t)sizeof *destination))
    {
        ++WindowClientRefusals;

        return SYSCALL_EFAULT;
    }

    bounds = WindowManagerScreenBounds();
    destination = (SyscallWindowRectangle *)(uintptr_t)geometry_address;
    destination->x = bounds.x;
    destination->y = bounds.y;
    destination->width = bounds.width;
    destination->height = bounds.height;

    return SYSCALL_OK;
}

int64_t WindowClientSession(void)
{
    const uint64_t caller = WindowClientCaller();
    const uint64_t holder = WindowSession();

    ++WindowClientCalls;

    if (!WindowManagerIsActive() || (caller == 0U))
    {
        ++WindowClientRefusals;

        return SYSCALL_ENOTSUP;
    }

    /*
     * First come, and held until the claimant ends. Claiming it twice is not an
     * error: a session that restarted a part of itself should not have to know
     * whether it had claimed already.
     */
    if (holder == caller)
    {
        return SYSCALL_OK;
    }

    if (holder != 0U)
    {
        ++WindowClientRefusals;

        return SYSCALL_EPERM;
    }

    WindowSetSession(caller);

    return SYSCALL_OK;
}

int64_t WindowClientText(uint64_t window, uint64_t placement_address, uint64_t text_address)
{
    const size_t owned = WindowClientOwned(window);
    const SyscallWindowText *placement;
    char text[SYSCALL_WINDOW_TEXT_MAXIMUM + 1U];

    ++WindowClientCalls;

    if (owned == WINDOW_NONE)
    {
        return SYSCALL_EBADF;
    }

    if (!SyscallUserRangeIsReadable(placement_address, (uint64_t)sizeof *placement) ||
        !SyscallCopyUserString(text_address, text, sizeof text))
    {
        ++WindowClientRefusals;

        return SYSCALL_EFAULT;
    }

    placement = (const SyscallWindowText *)(uintptr_t)placement_address;

    if (!WindowDrawText(owned, placement->x, placement->y, text, placement->ink, placement->paper,
                        placement->scale))
    {
        ++WindowClientRefusals;

        return SYSCALL_EINVAL;
    }

    return SYSCALL_OK;
}

void WindowClientReleaseProcess(uint64_t process_id)
{
    if (process_id == 0U)
    {
        return;
    }

    /* The session goes with the process that claimed it, since sub-task 9.5:
     * a claim held by a process that has ended is a screen nobody may take
     * again, and a desktop that could never be started a second time. */
    if (WindowSession() == process_id)
    {
        WindowSetSession(0U);
    }

    if (WindowDestroyOwnedBy(process_id) != 0U)
    {
        WindowClientWakeAll();
    }
}

void WindowClientWakeAll(void)
{
    (void)SchedulerWake(&WindowClientChannel);
}

uint64_t WindowClientCallCount(void)
{
    return WindowClientCalls;
}

uint64_t WindowClientRefusalCount(void)
{
    return WindowClientRefusals;
}

uint64_t WindowClientSleepCount(void)
{
    return WindowClientSleeps;
}

void WindowClientReport(void)
{
    KernelWriteString("Window clients: ");
    KernelWriteDecimal(WindowClientCalls);
    KernelWriteString(" call(s), ");
    KernelWriteDecimal(WindowClientRefusals);
    KernelWriteString(" refused, ");
    KernelWriteDecimal(WindowClientSleeps);
    KernelWriteString(" sleep(s) for an event.\n");
}
