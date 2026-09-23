/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/time/system.c
 * Purpose: The one place <time.h> touches the system: `time`, by SYSCALL_TIME.
 * Key functions: time.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.27.2.4: `time` returns (time_t)(-1) where
 *     the calendar time is not available, and stores the result through a
 *     pointer that is not null.
 *
 * **Nothing here may be called by the kernel**, for the reason of every other
 * system.c of this library: SYSRET returns to privilege level 3.
 */

#include <time.h>
#include <syscall.h>

time_t time(time_t *timer)
{
    const int64_t now = OxysTime();
    const time_t result = (now < 0) ? (time_t)-1 : (time_t)now;

    if (timer != NULL)
    {
        *timer = result;
    }

    return result;
}
