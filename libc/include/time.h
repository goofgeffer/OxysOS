/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/time.h
 * Purpose: Declares the part of ISO/IEC 9899:2011, Section 7.27, "Date and
 *          time", that sub-task 9.7's clock needs: the calendar time, and its
 *          breaking into a date and a time of day.
 * Key definitions: time_t, struct tm, time, gmtime, gmtime_r.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.27.1, "Components of time" — the members
 *     of struct tm and their ranges; Section 7.27.2.4, `time`; Section
 *     7.27.3.3, `gmtime`.
 *   - IEEE Std 1003.1-2017, `gmtime_r()`: the same, into the caller's storage.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_TIME, which says what the
 *     seconds are counted from and why they are the clock's time.
 *
 * What is absent, and why. `clock`, `difftime`, `mktime`, `localtime`,
 * `strftime` and `asctime` are not here: nothing in this system has asked for
 * them, and `localtime` in particular would need a time zone this system does
 * not have — the machine's clock is taken as it reads, SYSCALL_TIME.
 */

#ifndef OXYS_TIME_H
#define OXYS_TIME_H

#include <stddef.h>
#include <stdint.h>

/* Seconds since 1970-01-01T00:00:00, as a signed count so that a failure may
 * be (time_t)-1, which ISO C's `time` requires. */
typedef int64_t time_t;

struct tm
{
    int tm_sec;   /* 0 to 60. */
    int tm_min;   /* 0 to 59. */
    int tm_hour;  /* 0 to 23. */
    int tm_mday;  /* 1 to 31. */
    int tm_mon;   /* 0 to 11: months since January. */
    int tm_year;  /* Years since 1900. */
    int tm_wday;  /* 0 to 6: days since Sunday. */
    int tm_yday;  /* 0 to 365: days since the first of January. */
    int tm_isdst; /* Zero: there is no daylight saving here. */
};

time_t time(time_t *timer);

/* Null for a time before 1970, which the arithmetic here does not reach. */
struct tm *gmtime(const time_t *timer);
struct tm *gmtime_r(const time_t *timer, struct tm *result);

#endif /* OXYS_TIME_H */
