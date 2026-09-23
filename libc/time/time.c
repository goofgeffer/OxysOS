/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/time/time.c
 * Purpose: Breaks a calendar time into a date and a time of day, which is
 *          arithmetic and touches nothing.
 * Key functions: gmtime, gmtime_r.
 * References:
 *   - libc/include/time.h: ISO/IEC 9899:2011, Section 7.27, and the ranges.
 *   - The Gregorian rule of leap years — every fourth year, save centuries not
 *     divisible by four hundred — as the proleptic calendar ISO C assumes.
 *
 * It is written apart from the kernel's own conversion, drivers/rtc/rtc.c,
 * rather than sharing it: the kernel's is under the kernel's licence and this
 * library is under another, LICENSING.md, Section 1. Both are asserted against
 * the same values computed by the build host's `date`, so they cannot drift
 * apart without a self-test saying so.
 */

#include <time.h>

static int TimeIsLeap(int64_t year)
{
    return (((year % 4) == 0) && ((year % 100) != 0)) || ((year % 400) == 0);
}

struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days;
    int64_t within;
    int64_t year = 1970;
    int month = 0;

    if ((timer == NULL) || (result == NULL) || (*timer < 0))
    {
        return NULL;
    }

    days = *timer / 86400;
    within = *timer % 86400;

    /* 1970-01-01 was a Thursday, the fourth day after Sunday. */
    result->tm_wday = (int)((days + 4) % 7);

    while (days >= (TimeIsLeap(year) ? 366 : 365))
    {
        days -= TimeIsLeap(year) ? 366 : 365;
        ++year;
    }

    result->tm_yday = (int)days;

    for (;;)
    {
        const int length = ((month == 1) && TimeIsLeap(year)) ? 29 : month_days[month];

        if (days < length)
        {
            break;
        }

        days -= length;
        ++month;
    }

    result->tm_year = (int)(year - 1900);
    result->tm_mon = month;
    result->tm_mday = (int)days + 1;
    result->tm_hour = (int)(within / 3600);
    result->tm_min = (int)((within % 3600) / 60);
    result->tm_sec = (int)(within % 60);
    result->tm_isdst = 0;

    return result;
}

struct tm *gmtime(const time_t *timer)
{
    static struct tm broken;

    return gmtime_r(timer, &broken);
}
