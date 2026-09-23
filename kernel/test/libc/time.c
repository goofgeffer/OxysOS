/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/time.c
 * Purpose: Asserts `gmtime_r` of the C library's <time.h>, of sub-task 9.7,
 *          against dates the build host's `date -u` gave for the same seconds.
 * Key functions: KernelVerifyTime.
 * References:
 *   - libc/include/time.h: ISO/IEC 9899:2011, Section 7.27.1, the ranges —
 *     `tm_mon` from zero, `tm_year` from 1900, `tm_yday` from zero, which
 *     `date`'s `%j` counts from one.
 *   - docs/devices/TIME.md, Section 10: the assertions paired with the failures.
 *
 * `time` itself reaches SYSCALL and cannot be run here; the panel's clock and
 * `/bin/date` are what show it working, and `signal-check` asserts it at
 * privilege level 3.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <time.h>

static bool VerifyTimeSucceeded;

static void VerifyTimeRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyTimeSucceeded = false;
    }
}

/* Whether the seconds break into the date given, as `date -u -d @seconds`
 * prints it — the month and the day of the year as `date` counts them. */
static bool VerifyTimeBreaks(time_t seconds, int year, int month, int day, int hour, int minute,
                             int second, int wday, int yday)
{
    struct tm broken;

    return (gmtime_r(&seconds, &broken) == &broken) && (broken.tm_year == year - 1900) &&
           (broken.tm_mon == month - 1) && (broken.tm_mday == day) && (broken.tm_hour == hour) &&
           (broken.tm_min == minute) && (broken.tm_sec == second) && (broken.tm_wday == wday) &&
           (broken.tm_yday == yday - 1) && (broken.tm_isdst == 0);
}

void KernelVerifyTime(void)
{
    const time_t before = -1;
    struct tm broken;

    VerifyTimeSucceeded = true;

    KernelWriteString("Time: asserting gmtime against the build host's date.\n");

    /*
     * The first second, a Thursday; the last second of a leap day, also a
     * Thursday and the sixtieth day; the day this was written, a Wednesday;
     * and 2100, which is not a leap year, a Friday. A weekday counted from
     * the wrong day, or a month counted from one, gives a panel reading a date
     * off by exactly that — which looks right to anyone not checking.
     */
    VerifyTimeRequire(VerifyTimeBreaks(0, 1970, 1, 1, 0, 0, 0, 4, 1),
                      "the first second of 1970 was not a Thursday in January");
    VerifyTimeRequire(VerifyTimeBreaks(1709251199, 2024, 2, 29, 23, 59, 59, 4, 60),
                      "the last second of a leap day was misread");
    VerifyTimeRequire(VerifyTimeBreaks(1790121600, 2026, 9, 23, 0, 0, 0, 3, 266),
                      "2026-09-23 was misread");
    VerifyTimeRequire(VerifyTimeBreaks(INT64_C(4102444800), 2100, 1, 1, 0, 0, 0, 5, 1),
                      "2100 was taken for a leap year, or its first day misread");
    VerifyTimeRequire(gmtime_r(&before, &broken) == NULL,
                      "a time before 1970 was broken into a date");

    KernelWriteString(VerifyTimeSucceeded
                          ? "Time self-test passed: gmtime agrees with the host's date upon "
                            "1970, a leap day, 2026 and 2100, and refuses a time before 1970.\n"
                          : "Time self-test FAILED.\n");
}
