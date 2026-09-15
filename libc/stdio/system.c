/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdio/system.c
 * Purpose: The one place the streams of libc/stdio/stream.c touch the system:
 *          the transfer that carries a buffer to a descriptor, and the transfer
 *          that carries one back — the second real since sub-task 8.1, when a
 *          call that reads the terminal arrived.
 * Key functions: OxysStreamWrite, OxysStreamFill.
 * References:
 *   - libc/include/stream.h: the seam this implements, and why a stream's
 *     transfers are named functions rather than calls inside the buffering.
 *   - libc/include/syscall.h: OxysWrite, and what it returns.
 *   - kernel/abi/oxys/syscall_abi.h: the fourteen calls this kernel has, and
 *     descriptor 0, which since sub-task 8.1 a `read` reaches the terminal by.
 *   - docs/design/LIBC.md, Section 10.2: the division of the sub-task into a
 *     policy that runs anywhere and a pair of transfers that run only at
 *     privilege level 3.
 *
 * This file is two functions and a translation unit of its own, which is
 * deliberate, and it is the same arrangement libc/stdlib/system.c made for the
 * heap one sub-task earlier.
 *
 * It is the whole of what a stream knows about the machine beneath it. The
 * policy in libc/stdio/stream.c calls nothing else that could fail for a reason
 * outside the C language, so the two can be — and are — asserted separately: the
 * policy by the kernel's own boot-time self-test, which gives it a memory stream
 * to work against, and these two by a program at privilege level 3.
 *
 * **Neither of these may be called by the kernel**, and neither may anything
 * that reaches a system call: SYSRET returns to privilege level 3
 * unconditionally, so a kernel that flushed a stream would leave its own entry
 * path as a user program upon a stack that is not a user program's. The
 * self-test therefore asserts that the streams it exercises made no transfer at
 * all, so a change that made one would be reported rather than discovered as a
 * reset.
 */

#include <stddef.h>
#include <stdint.h>

#include <stream.h>
#include <syscall.h>

int64_t OxysStreamWrite(int descriptor, const void *buffer, size_t length)
{
    if ((buffer == NULL) && (length != 0U))
    {
        return -1;
    }

    if (length == 0U)
    {
        return 0;
    }

    /*
     * The result is passed through unaltered, including a result short of the
     * length asked for.
     *
     * The kernel bounds a single transfer, so a short result is its normal
     * behaviour and not a failure, and the loop that calls again from where this
     * one stopped belongs to the policy above rather than here. A seam that
     * looped would be a seam whose behaviour could not be stated in one
     * sentence, and the whole value of this file is that it can.
     */
    return OxysWrite(descriptor, buffer, length);
}

int64_t OxysStreamFill(int descriptor, void *buffer, size_t capacity)
{
    if ((buffer == NULL) || (capacity == 0U))
    {
        return -1;
    }

    /*
     * Since sub-task 8.1 there is a call that reads, and this is the one
     * function that changed — as the note it replaced said it would be.
     *
     * The result is passed through as OxysWrite's is. For descriptor 0 the
     * kernel waits until something has been typed and then delivers at least
     * one byte and at most `capacity`, so a fill of the standard input blocks
     * a program until a person acts and returns short; the policy above treats
     * a short fill as ordinary, which it is. Zero is the end of a file, and a
     * negative result an error, and the policy sets the indicator each of them
     * calls for.
     */
    return OxysRead(descriptor, buffer, capacity);
}
