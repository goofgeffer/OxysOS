/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/dev/rtc.c
 * Purpose: Asserts the real-time clock driver of sub-task 9.7 — the decoding
 *          of both data modes and both hour modes upon bytes composed here, the
 *          refusal of bytes that are not a date, and the arithmetic between a
 *          date and seconds since 1970 against values computed independently —
 *          and then that the clock the machine carries was read.
 * Key functions: KernelVerifyRtc.
 * References:
 *   - kernel/include/oxys/dev/rtc.h: the data sheet's Table 3 and register B.
 *   - docs/devices/TIME.md, Section 10: every assertion here paired with the
 *     silent failure it would catch.
 *
 * Why the expected seconds are written as numbers.
 *
 *   They were computed by GNU `date -u -d <date> +%s` upon the build host, and
 *   not by this driver's own arithmetic run backwards: a test that checked the
 *   conversion against itself would pass a conversion that was consistently
 *   wrong, which is the one kind of wrong a clock can be for years unnoticed.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/dev/rtc.h>

static bool VerifyRtcSucceeded;

static void VerifyRtcRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyRtcSucceeded = false;
    }
}

static bool VerifyRtcIs(const RtcTime *time, uint32_t year, uint32_t month, uint32_t day,
                        uint32_t hour, uint32_t minute, uint32_t second)
{
    return (time->year == year) && (time->month == month) && (time->day == day) &&
           (time->hour == hour) && (time->minute == minute) && (time->second == second);
}

static void VerifyRtcDecoding(void)
{
    RtcTime time;

    /* BCD, the 24-hour mode: 17:15:30 on 2026-09-23. */
    {
        const uint8_t raw[6] = { 0x30U, 0x15U, 0x17U, 0x23U, 0x09U, 0x26U };

        VerifyRtcRequire(RtcDecode(raw, RTC_B_24_HOUR, &time) &&
                             VerifyRtcIs(&time, 2026U, 9U, 23U, 17U, 15U, 30U),
                         "a BCD reading in the 24-hour mode was misread");
    }

    /* Binary, the same moment. A driver that decoded BCD regardless would read
     * the seconds, 30 in binary, as 1E and refuse it — or worse, accept 23 as 17. */
    {
        const uint8_t raw[6] = { 30U, 15U, 17U, 23U, 9U, 26U };

        VerifyRtcRequire(RtcDecode(raw, RTC_B_24_HOUR | RTC_B_BINARY, &time) &&
                             VerifyRtcIs(&time, 2026U, 9U, 23U, 17U, 15U, 30U),
                         "a binary reading was misread");
    }

    /*
     * The 12-hour mode, BCD. $81 is one in the afternoon — the flag is taken
     * off before the digits are read, or it is eighty-one. $12 is midnight's
     * hour and $92 noon's: twelve is the first hour of its half of the day.
     */
    {
        uint8_t raw[6] = { 0x00U, 0x00U, 0x81U, 0x23U, 0x09U, 0x26U };

        VerifyRtcRequire(RtcDecode(raw, 0U, &time) && (time.hour == 13U),
                         "one in the afternoon in the 12-hour mode was not 13");
        raw[2] = 0x12U;
        VerifyRtcRequire(RtcDecode(raw, 0U, &time) && (time.hour == 0U),
                         "twelve midnight in the 12-hour mode was not hour 0");
        raw[2] = 0x92U;
        VerifyRtcRequire(RtcDecode(raw, 0U, &time) && (time.hour == 12U),
                         "twelve noon in the 12-hour mode was not hour 12");
    }

    /* Bytes that are not a date: a clock never set, or with its battery gone,
     * reads bytes like these, and a time made of them is a panel showing a
     * time that never was. */
    {
        uint8_t raw[6] = { 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x26U };

        VerifyRtcRequire(!RtcDecode(raw, RTC_B_24_HOUR, &time),
                         "a thirteenth month was accepted");
        raw[3] = 0x29U;
        raw[4] = 0x02U;
        raw[5] = 0x25U;
        VerifyRtcRequire(!RtcDecode(raw, RTC_B_24_HOUR, &time),
                         "the twenty-ninth of February of 2025 was accepted");
        raw[5] = 0x24U;
        VerifyRtcRequire(RtcDecode(raw, RTC_B_24_HOUR, &time) && (time.day == 29U),
                         "the twenty-ninth of February of 2024 was refused");
    }
}

static void VerifyRtcArithmetic(void)
{
    RtcTime time;

    VerifyRtcRequire((RtcDaysFromDate(1970U, 1U, 1U) == 0U) &&
                         (RtcDaysFromDate(2000U, 3U, 1U) == 11017U) &&
                         (RtcDaysFromDate(2026U, 9U, 23U) == 20719U) &&
                         (RtcDaysFromDate(2100U, 1U, 1U) == 47482U),
                     "a count of days since 1970 disagreed with the host's `date`");

    /* The last second of a leap day, and back again. 2000 was a leap year and
     * 2100 is not; 2000-03-01 and 2100-01-01 above are what catch a rule of
     * "every fourth year" alone. */
    time.year = 2024U;
    time.month = 2U;
    time.day = 29U;
    time.hour = 23U;
    time.minute = 59U;
    time.second = 59U;
    VerifyRtcRequire(RtcSecondsFromTime(&time) == UINT64_C(1709251199),
                     "the last second of a leap day was not the host's seconds since 1970");

    RtcTimeFromSeconds(UINT64_C(1709251199), &time);
    VerifyRtcRequire(VerifyRtcIs(&time, 2024U, 2U, 29U, 23U, 59U, 59U),
                     "seconds since 1970 were not turned back into the date they came from");
    RtcTimeFromSeconds(UINT64_C(1709251200), &time);
    VerifyRtcRequire(VerifyRtcIs(&time, 2024U, 3U, 1U, 0U, 0U, 0U),
                     "the second after a leap day was not the first of March");
}

void KernelVerifyRtc(void)
{
    VerifyRtcSucceeded = true;

    KernelWriteString("Real-time clock: asserting the decoding and the arithmetic, then the "
                      "clock this machine carries.\n");

    VerifyRtcDecoding();
    VerifyRtcArithmetic();

    /*
     * The clock itself, where there is one: a reading of this century that the
     * timer then advances. Every environment this system is tested in carries
     * one; a machine without one is reported and not failed, as a machine
     * without a mouse is.
     */
    if (RtcIsPresent())
    {
        VerifyRtcRequire(RtcNow() >= UINT64_C(946684800),
                         "the clock was read as a time before 2000");
    }
    else
    {
        KernelWriteString("  No date could be read from this machine's clock.\n");
    }

    KernelWriteString(VerifyRtcSucceeded
                          ? "Real-time clock self-test passed: both data modes and both hour "
                            "modes, twelve o'clock in each half, bytes that are not a date refused, "
                            "and seconds since 1970 as the host's `date` gives them.\n"
                          : "Real-time clock self-test FAILED.\n");
}
