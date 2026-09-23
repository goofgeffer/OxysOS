/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/date/main.c
 * Purpose: Writes the date and the time of day, as the machine's clock holds
 *          them, to the standard output — the clock of sub-task 9.7 for a
 *          person at the shell rather than at the panel.
 * Key functions: main.
 * References:
 *   - IEEE Std 1003.1-2017, `date`: with no operand, the utility writes the
 *     current date and time. Its default format is that of the POSIX locale's
 *     `%a %b %e %H:%M:%S %Z %Y`; this one writes the ISO 8601 form instead,
 *     for the reason below, and takes no operand and no option.
 *   - ISO 8601, the extended format of a calendar date and a time of day:
 *     `YYYY-MM-DD` and `hh:mm:ss`.
 *   - libc/include/time.h: `time` and `gmtime_r`.
 *
 * Why ISO 8601 and not the POSIX default. The default names a time zone, and
 * this system has none: the clock holds whatever its owner set, commonly the
 * local time, and writing `UTC` after it would be a statement the program
 * cannot make. The ISO form names none, sorts as text, and is what the kernel's
 * own report of the clock at start prints, so the two can be compared by eye.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    const time_t now = time(NULL);
    struct tm broken;

    if ((now < 0) || (gmtime_r(&now, &broken) == NULL))
    {
        (void)fprintf(stderr, "date: this machine's clock gave no time.\n");

        return EXIT_FAILURE;
    }

    (void)printf("%04d-%02d-%02d %02d:%02d:%02d\n", broken.tm_year + 1900, broken.tm_mon + 1,
                 broken.tm_mday, broken.tm_hour, broken.tm_min, broken.tm_sec);

    return EXIT_SUCCESS;
}
