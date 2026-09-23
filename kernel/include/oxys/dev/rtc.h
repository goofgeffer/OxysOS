/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/dev/rtc.h
 * Purpose: Declares the driver of sub-task 9.7 for the real-time clock the PC
 *          carries from the MC146818A — the one source of the date and the time
 *          of day this machine has — and the arithmetic that turns what it
 *          reads into seconds since 1970.
 * Key definitions: RtcTime, RTC_PORT_INDEX, RTC_PORT_DATA, RTC_REGISTER_*,
 *          RtcDecodeField, RtcDecode, RtcDaysFromDate, RtcSecondsFromTime,
 *          RtcTimeFromSeconds, RtcRead, RtcInitialise, RtcNow, RtcIsPresent,
 *          RtcReport.
 * References:
 *   - Motorola MC146818A data sheet, "Real-Time Clock Plus RAM (RTC)":
 *     Figure 14, "Address Map", and Table 3, "Time, Calendar, and Alarm Data
 *     Modes" — the seconds at 0, the minutes at 2, the hours at 4, the date of
 *     the month at 7, the month at 8 and the year, zero to ninety-nine, at 9;
 *     in the 12-hour mode the hours are 1 to 12 and the post-meridiem hours
 *     carry the high-order bit, $81 to $92 in BCD.
 *   - The same, "Register A" and "Update Cycle": the update-in-progress bit is
 *     bit 7 of register A ($0A), the update begins 244 microseconds after it
 *     goes high, and while the update runs the time and calendar bytes are not
 *     accessible.
 *   - The same, "Register B" ($0B): from bit 7 down, SET, PIE, AIE, UIE, SQWE,
 *     DM, 24/12, DSE. DM is 1 for binary and 0 for BCD; 24/12 is 1 for the
 *     24-hour mode.
 *   - Intel Platform Controller Hub register database, "NMI Enable (and Real
 *     Time Clock Index) (NMI_EN) – Offset 70": the index of the clock's
 *     registers is written to I/O port 0x70, whose bit 7 is also the
 *     non-maskable interrupt's enable, and the register is then read at 0x71.
 *
 * Why there is no century.
 *
 *   The MC146818A counts the year from zero to ninety-nine and holds no
 *   century. Chipsets put one in the RAM beyond, at an offset the firmware
 *   declares in the ACPI FADT's CENTURY field, and not always. This driver
 *   reads the two digits and places them in the twenty-first century — the
 *   years 2000 to 2099 — which every machine this system runs upon is in, and
 *   says so rather than reading a byte whose place it does not know.
 *
 * Why it is read at every call, and not once.
 *
 *   It was read once, at start, and the time afterwards was that reading
 *   advanced by the interval timer — until the panel's clock was watched
 *   against the build host's under QEMU, and turned its minute several seconds
 *   after the host's: an emulated timer at a thousand interrupts a second loses
 *   some of them, and a time made of the timer falls behind by what it lost,
 *   for as long as the machine runs. The clock does not lose seconds. So RtcNow
 *   reads it, and takes the timer's count only where a read fails — an update
 *   that never ends, or bytes that are not a date — advanced from the last read
 *   that succeeded. A read waits upon the update cycle, two milliseconds at
 *   worst, which the panel pays once a minute.
 *
 *   What the timer is still for is intervals: an alarm is so many milliseconds
 *   of the timer from now, and a timer that runs slow makes it late by the same
 *   fraction. The panel's clock asks the time again when the alarm arrives, so
 *   lateness never accumulates past one minute's worth.
 */

#ifndef OXYS_DEV_RTC_H
#define OXYS_DEV_RTC_H

#include <oxys/types.h>

#define RTC_PORT_INDEX UINT16_C(0x70)
#define RTC_PORT_DATA  UINT16_C(0x71)

#define RTC_REGISTER_SECONDS UINT8_C(0x00)
#define RTC_REGISTER_MINUTES UINT8_C(0x02)
#define RTC_REGISTER_HOURS   UINT8_C(0x04)
#define RTC_REGISTER_DATE    UINT8_C(0x07)
#define RTC_REGISTER_MONTH   UINT8_C(0x08)
#define RTC_REGISTER_YEAR    UINT8_C(0x09)
#define RTC_REGISTER_A       UINT8_C(0x0A)
#define RTC_REGISTER_B       UINT8_C(0x0B)

#define RTC_A_UPDATE_IN_PROGRESS UINT8_C(0x80)
#define RTC_B_BINARY             UINT8_C(0x04)
#define RTC_B_24_HOUR            UINT8_C(0x02)

/* The post-meridiem flag of the hours byte in the 12-hour mode. */
#define RTC_HOURS_PM UINT8_C(0x80)

/* A date and a time of day, as the clock holds them, the year whole. */
typedef struct RtcTime
{
    uint32_t year;
    uint32_t month;  /* 1 to 12. */
    uint32_t day;    /* 1 to 31. */
    uint32_t hour;   /* 0 to 23. */
    uint32_t minute; /* 0 to 59. */
    uint32_t second; /* 0 to 59. */
} RtcTime;

/* One byte of the clock as a number: BCD unless register B says binary. */
uint32_t RtcDecodeField(uint8_t value, uint8_t register_b);

/*
 * The six time and calendar bytes as the clock gave them, with register B,
 * into a time — the hours brought to 0 to 23 from the 12-hour mode where that
 * is the mode, and the year into the twenty-first century. False, with the time
 * untouched, where a field is outside its range: a clock that was never set,
 * or a battery gone flat, reads bytes that are not a date.
 */
bool RtcDecode(const uint8_t raw[6], uint8_t register_b, RtcTime *time);

/* Days from 1970-01-01 to a date of the proleptic Gregorian calendar, the year
 * 1970 or later. */
uint64_t RtcDaysFromDate(uint32_t year, uint32_t month, uint32_t day);

/* Seconds from 1970-01-01T00:00:00 to a time, and back again. */
uint64_t RtcSecondsFromTime(const RtcTime *time);
void RtcTimeFromSeconds(uint64_t seconds, RtcTime *time);

/*
 * Reads the clock: waits until no update is in progress, reads the six bytes
 * and register B, and reads them again, until two readings agree — the
 * data sheet's first method, which is what catches an update that began between
 * the test of the bit and the last byte. False where the clock never settles or
 * reads no date.
 */
bool RtcRead(RtcTime *time);

/*
 * Reads the clock once and records the reading beside the interval timer's
 * count at that moment. Returns whether a date was read. Before it, and after
 * a failed read, RtcNow is zero and RtcIsPresent false.
 */
bool RtcInitialise(void);
bool RtcIsPresent(void);

/* Seconds since 1970-01-01T00:00:00 as the clock reads now; where it cannot be
 * read, the last reading advanced by the interval timer; zero where no date
 * was ever read. */
uint64_t RtcNow(void);

void RtcReport(void);

#endif /* OXYS_DEV_RTC_H */
