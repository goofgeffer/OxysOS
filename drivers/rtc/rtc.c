/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/rtc/rtc.c
 * Purpose: Implements the real-time clock driver of sub-task 9.7: the clock
 *          read through its index and data ports without an update in the way,
 *          its bytes decoded from either of its two data modes and either of its
 *          two hour modes, and the date turned into seconds since 1970 and back.
 * Key functions: RtcDecodeField, RtcDecode, RtcDaysFromDate,
 *          RtcSecondsFromTime, RtcTimeFromSeconds, RtcRead, RtcInitialise,
 *          RtcIsPresent, RtcNow, RtcReport.
 * References:
 *   - kernel/include/oxys/dev/rtc.h: the MC146818A data sheet's address map,
 *     Table 3, registers A and B and the update cycle; and the Intel register
 *     that places the index at port 0x70.
 *   - docs/devices/TIME.md, Section 10: the design, and every assertion made
 *     upon it.
 *
 * Concurrency. RtcInitialise runs once, upon the bootstrap processor, before
 * any other processor or any program is started. RtcNow is reached by the
 * `time` call, which runs with interrupts masked upon the bootstrap processor
 * alone, as every call here does; the ports and the anchor are touched by
 * nothing else.
 */

#include <oxys/dev/rtc.h>
#include <oxys/dev/io.h>
#include <oxys/dev/pit.h>
#include <oxys/kernel.h>

/* How many times a read tries for two agreeing readings, and how many times it
 * looks at the update bit, before it says the clock never settled. An update
 * lasts at most 1984 microseconds by the data sheet; a port read is about a
 * microsecond; so the bound is several updates and never a hang. */
#define RTC_ATTEMPTS        8U
#define RTC_UPDATE_POLLS    100000U

static bool RtcPresent;
static uint64_t RtcBootSeconds;
static uint64_t RtcBootMilliseconds;
static RtcTime RtcBootTime;

/*
 * One register. Bit 7 of the index port is the non-maskable interrupt's enable
 * upon Intel's chipsets, a one masking it; it is written as zero, which is the
 * state the machine starts in and leaves NMIs as they were.
 */
static uint8_t RtcReadRegister(uint8_t index)
{
    PortWriteByte(RTC_PORT_INDEX, (uint8_t)(index & 0x7FU));
    IoWait();

    return PortReadByte(RTC_PORT_DATA);
}

uint32_t RtcDecodeField(uint8_t value, uint8_t register_b)
{
    if ((register_b & RTC_B_BINARY) != 0U)
    {
        return value;
    }

    return ((uint32_t)(value >> 4) * 10U) + (uint32_t)(value & 0x0FU);
}

static bool RtcIsLeap(uint32_t year)
{
    return (((year % 4U) == 0U) && ((year % 100U) != 0U)) || ((year % 400U) == 0U);
}

static uint32_t RtcDaysInMonth(uint32_t year, uint32_t month)
{
    static const uint8_t days[12] = { 31U, 28U, 31U, 30U, 31U, 30U,
                                      31U, 31U, 30U, 31U, 30U, 31U };

    if ((month == 2U) && RtcIsLeap(year))
    {
        return 29U;
    }

    return days[month - 1U];
}

bool RtcDecode(const uint8_t raw[6], uint8_t register_b, RtcTime *time)
{
    RtcTime decoded;
    uint8_t hours;
    bool pm = false;

    if ((raw == NULL) || (time == NULL))
    {
        return false;
    }

    hours = raw[2];

    /*
     * In the 12-hour mode the post-meridiem flag is the high-order bit of the
     * hours byte in either data mode, Table 3, and is taken off before the
     * byte is decoded: left on, a BCD decode reads $81 as eighty-one.
     */
    if ((register_b & RTC_B_24_HOUR) == 0U)
    {
        pm = (hours & RTC_HOURS_PM) != 0U;
        hours = (uint8_t)(hours & (uint8_t)~RTC_HOURS_PM);
    }

    decoded.second = RtcDecodeField(raw[0], register_b);
    decoded.minute = RtcDecodeField(raw[1], register_b);
    decoded.hour = RtcDecodeField(hours, register_b);
    decoded.day = RtcDecodeField(raw[3], register_b);
    decoded.month = RtcDecodeField(raw[4], register_b);
    decoded.year = 2000U + RtcDecodeField(raw[5], register_b);

    /*
     * Twelve o'clock is the first hour of its half of the day: 12 AM is 0 and
     * 12 PM is 12. A conversion that only added twelve for PM would make
     * half past midnight half past noon, and noon twenty-four o'clock.
     */
    if ((register_b & RTC_B_24_HOUR) == 0U)
    {
        if ((decoded.hour < 1U) || (decoded.hour > 12U))
        {
            return false;
        }

        decoded.hour %= 12U;

        if (pm)
        {
            decoded.hour += 12U;
        }
    }

    if ((decoded.second > 59U) || (decoded.minute > 59U) || (decoded.hour > 23U) ||
        (decoded.month < 1U) || (decoded.month > 12U) || (decoded.year > 2099U) ||
        (decoded.day < 1U) || (decoded.day > RtcDaysInMonth(decoded.year, decoded.month)))
    {
        return false;
    }

    *time = decoded;

    return true;
}

uint64_t RtcDaysFromDate(uint32_t year, uint32_t month, uint32_t day)
{
    uint64_t days = 0U;

    /* A year and a month at a time. Written as the count a person would make,
     * because it is asserted against dates a person can check, and it is run
     * once at start and once a minute by the panel — a closed form would be
     * faster at something nobody measures. */
    for (uint32_t each = 1970U; each < year; ++each)
    {
        days += RtcIsLeap(each) ? 366U : 365U;
    }

    for (uint32_t each = 1U; each < month; ++each)
    {
        days += RtcDaysInMonth(year, each);
    }

    return days + (uint64_t)(day - 1U);
}

uint64_t RtcSecondsFromTime(const RtcTime *time)
{
    if (time == NULL)
    {
        return 0U;
    }

    return (RtcDaysFromDate(time->year, time->month, time->day) * 86400U) +
           ((uint64_t)time->hour * 3600U) + ((uint64_t)time->minute * 60U) + time->second;
}

void RtcTimeFromSeconds(uint64_t seconds, RtcTime *time)
{
    uint64_t days = seconds / 86400U;
    uint64_t within = seconds % 86400U;
    uint32_t year = 1970U;
    uint32_t month = 1U;

    if (time == NULL)
    {
        return;
    }

    while (days >= (RtcIsLeap(year) ? 366U : 365U))
    {
        days -= RtcIsLeap(year) ? 366U : 365U;
        ++year;
    }

    while (days >= RtcDaysInMonth(year, month))
    {
        days -= RtcDaysInMonth(year, month);
        ++month;
    }

    time->year = year;
    time->month = month;
    time->day = (uint32_t)days + 1U;
    time->hour = (uint32_t)(within / 3600U);
    time->minute = (uint32_t)((within % 3600U) / 60U);
    time->second = (uint32_t)(within % 60U);
}

/* Waits until no update is in progress. False where one never ends. */
static bool RtcAwaitNoUpdate(void)
{
    for (uint32_t poll = 0U; poll < RTC_UPDATE_POLLS; ++poll)
    {
        if ((RtcReadRegister(RTC_REGISTER_A) & RTC_A_UPDATE_IN_PROGRESS) == 0U)
        {
            return true;
        }
    }

    return false;
}

static void RtcReadRaw(uint8_t raw[6], uint8_t *register_b)
{
    raw[0] = RtcReadRegister(RTC_REGISTER_SECONDS);
    raw[1] = RtcReadRegister(RTC_REGISTER_MINUTES);
    raw[2] = RtcReadRegister(RTC_REGISTER_HOURS);
    raw[3] = RtcReadRegister(RTC_REGISTER_DATE);
    raw[4] = RtcReadRegister(RTC_REGISTER_MONTH);
    raw[5] = RtcReadRegister(RTC_REGISTER_YEAR);
    *register_b = RtcReadRegister(RTC_REGISTER_B);
}

bool RtcRead(RtcTime *time)
{
    uint8_t first[6];
    uint8_t second[6];
    uint8_t first_b;
    uint8_t second_b;

    if (time == NULL)
    {
        return false;
    }

    /*
     * The update bit says only that an update will not begin for 244
     * microseconds, and six reads may take longer than that under an emulator
     * that is slow at port I/O. So the bytes are read twice, and the reading
     * kept only when both agree: an update that ran between them changed at
     * least the seconds, and the pair is read again.
     */
    for (uint32_t attempt = 0U; attempt < RTC_ATTEMPTS; ++attempt)
    {
        bool same = true;

        if (!RtcAwaitNoUpdate())
        {
            return false;
        }

        RtcReadRaw(first, &first_b);

        if (!RtcAwaitNoUpdate())
        {
            return false;
        }

        RtcReadRaw(second, &second_b);

        for (uint32_t index = 0U; index < 6U; ++index)
        {
            same = same && (first[index] == second[index]);
        }

        if (same && (first_b == second_b))
        {
            return RtcDecode(first, first_b, time);
        }
    }

    return false;
}

bool RtcInitialise(void)
{
    RtcTime time;

    RtcPresent = false;
    RtcBootSeconds = 0U;

    if (!RtcRead(&time))
    {
        return false;
    }

    RtcBootTime = time;
    RtcBootSeconds = RtcSecondsFromTime(&time);
    RtcBootMilliseconds = PitMillisecondsElapsed();
    RtcPresent = true;

    return true;
}

bool RtcIsPresent(void)
{
    return RtcPresent;
}

uint64_t RtcNow(void)
{
    RtcTime time;

    if (!RtcPresent)
    {
        return 0U;
    }

    /* The clock, where it reads; the anchor moved to this reading, so that a
     * later failure advances from here and not from the start. */
    if (RtcRead(&time))
    {
        RtcBootSeconds = RtcSecondsFromTime(&time);
        RtcBootMilliseconds = PitMillisecondsElapsed();

        return RtcBootSeconds;
    }

    return RtcBootSeconds + ((PitMillisecondsElapsed() - RtcBootMilliseconds) / 1000U);
}

/* Two digits, with the leading zero. */
static void RtcWriteTwo(uint32_t value)
{
    if (value < 10U)
    {
        KernelWriteString("0");
    }

    KernelWriteDecimal(value);
}

void RtcReport(void)
{
    if (!RtcPresent)
    {
        KernelWriteString("Real-time clock: no date could be read; the time is unknown.\n");

        return;
    }

    KernelWriteString("Real-time clock: ");
    KernelWriteDecimal(RtcBootTime.year);
    KernelWriteString("-");
    RtcWriteTwo(RtcBootTime.month);
    KernelWriteString("-");
    RtcWriteTwo(RtcBootTime.day);
    KernelWriteString(" ");
    RtcWriteTwo(RtcBootTime.hour);
    KernelWriteString(":");
    RtcWriteTwo(RtcBootTime.minute);
    KernelWriteString(":");
    RtcWriteTwo(RtcBootTime.second);
    KernelWriteString(" as the clock holds it, ");
    KernelWriteDecimal(RtcBootSeconds);
    KernelWriteString(" seconds after 1970.\n");
}
