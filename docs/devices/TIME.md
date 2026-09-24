<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Timekeeping: the Interval Timer and the Real-Time Clock

**Phase**: sub-task 3.6 (the interval timer), 6.14 (the microsecond wait) and 9.7
(the real-time clock, `time`, `alarm`) of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/pit/`](../../drivers/pit/),
[`../../drivers/rtc/rtc.c`](../../drivers/rtc/rtc.c); the calls in
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c).
**Specifications**: Intel 8254 data sheet (231164-005: programming, mode 2, mode
3, counter latch); IBM PC/AT Technical Reference; Motorola MC146818A data sheet
(Figure 14 address map, Table 3, registers A and B, the update cycle); Intel PCH
register NMI_EN (offset 70h); Intel SDM, Volume 3A, Section 8.4.4.1; IEEE Std
1003.1-2017, `time()`, `alarm()`.

The kernel's two clocks. The **interval timer** (8254 counter 0) ticks a thousand
times a second and measures intervals. The **real-time clock** (MC146818A) gives
the date. The per-processor Local APIC timer that drives scheduling is
[`APIC.md`](APIC.md) and [`../design/SCHEDULER.md`](../design/SCHEDULER.md).

## 1. The interval timer

Three 16-bit counters on one clock; control register at `0x43` (write-only).

| Counter | Port | Attached to | Used |
| ------- | ---- | ----------- | ---- |
| 0 | `0x40` | ISA interrupt line 0 | The tick. |
| 1 | `0x41` | DRAM refresh | No; obsolete. |
| 2 | `0x42` | Loudspeaker gate | No. |

The input clock is 1,193,182 Hz: the original PC's 14.31818 MHz crystal (four
times the NTSC colour subcarrier) divided by twelve. ISA line 0 reaches the
processor through the I/O APIC, on global interrupt 2 under QEMU
([`ACPI.md`](ACPI.md)).

**The control word is `0x34`**: counter 0 (bits 7:6 `00`), low byte then high
byte (5:4 `11`), mode 2 (3:1 `010`), binary (0). It is written before the count,
and the count low byte first, both to port `0x40`.

- **Mode 2 (rate generator), not mode 3 (square wave).** Mode 3 decrements by two
  and is exact only for even divisors; only the interval between edges matters
  here, and mode 2 accepts any divisor from 2.
- **A count of 1 is illegal in mode 2**: the output never falls and no interrupt
  is raised. `PitDivisorForFrequency` clamps to 2.

**Divisor and realised frequency.** `divisor = round(1193182 / requested)`;
`realised = 1193182 / divisor`. For 1,000 Hz the divisor is 1,193 and the realised
rate 1,000.152 Hz (+0.0152 %, about 13 seconds a day). `PitMillisecondsElapsed`
converts by the **realised** rate: an error of known size that is not accounted
for is present in every measurement and visible in none.

**Reading the counter** uses the latch command (read/write field `00`), which
captures the count for reading as two bytes. Unlatched, the two halves are sampled
at different moments and can form a value the counter never held, rarely and
unpredictably.

### 1.1 The microsecond wait

`PitBusyWaitMicroseconds` serves bring-up of the application processors
([`../design/SMP.md`](../design/SMP.md)), where the tick cannot help: interrupts
must be inhibited between INIT and the last startup interrupt (SDM 8.4.4.1), so
the tick does not advance, and one delay is 200 µs, below a tick. The wait
accumulates the differences between successive latched readings, adding the
divisor when a reading exceeds the last (the counter reloads). Resolution is one
input count, about 838 ns; the interval is rounded **up**, so a wait is never
short. It returns false if the counter is not running, and `SmpInitialise` then
declines bring-up and says so.

## 2. The real-time clock

Fourteen registers behind an index written to `0x70`, data at `0x71`.

- **Both data modes and both hour modes.** Bytes are BCD or binary per register B's
  DM bit; hours are 0–23, or 1–12 with the high bit for afternoon, per its 24/12
  bit. Firmware and emulators differ. Assuming BCD reads binary `23` as 17
  o'clock and accepts it. **Twelve is the first hour of its half**: `$12` is
  midnight and `$92` noon, which "add twelve in the afternoon" gets wrong twice.
- **No century.** The year is two digits, read as 2000–2099. A century register
  exists on many chipsets at an offset only the ACPI FADT gives, which is not
  read.
- **The update cycle.** Once a second the chip spends up to 1,984 µs updating,
  and register A's update-in-progress bit rises 244 µs before. The driver waits
  for the bit to fall, reads the six date bytes and register B, waits and reads
  them again, and accepts only two identical readings. This also catches an update
  that starts mid-read, which six port reads under an emulator can outlast.
- **Index bit 7 is the NMI mask** on Intel chipsets. The index is written with it
  clear, as the machine starts; setting it would mask NMIs for the rest of the run,
  silently.
- **The clock is read at every `time` call** (`RtcNow`). A time kept from one
  reading plus the tick count falls behind by every tick an emulator drops: under
  QEMU the panel's minute lagged the host's by several seconds within minutes. Read
  each time, it stays within two or three seconds. The tick count is only the
  fallback, advanced from the last good read, if a read fails.

## 3. The calls

| Call | Number | Behaviour |
| ---- | ------ | --------- |
| `time` | 42 | Seconds since 1970-01-01 00:00:00, from the clock as it reads (no zone). Stored through the pointer if one is given. |
| `alarm` | 43 | Sends the caller `SIGALRM` once after the given **milliseconds**; zero cancels; returns what remained of the alarm replaced. |

- **Milliseconds, not seconds**, because a clock that wants the start of the next
  minute needs better than a second.
- **Served from the tick**, like the terminal's Control-C: a program asleep in a
  call is reached by nothing else. Each tick on the bootstrap processor checks
  64 deadlines; a passed one is cleared **before** the signal is sent, so an alarm
  is one signal, not one per tick.
- **A child of `fork` has no alarm**, as POSIX requires: a copied one would send
  `SIGALRM`, which ends a process by default, to a program that never asked.
- The alarm counts ticks, so a slow emulated timer makes it late by the same
  fraction; the panel asks the time again when it fires, so lateness never
  accumulates past one minute ([`../design/UTILITIES.md`](../design/UTILITIES.md)).

`<time.h>` above the calls is [`../design/LIBC.md`](../design/LIBC.md).

## Verification

`KernelVerifyPit` in [`../../kernel/test/dev/devices.c`](../../kernel/test/dev/devices.c).
There is no second clock to check the timer against, so the assertions are
internal to the timer or concern its path to the interrupt controller.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The divisor is the one the requested rate demands, within one. | Every interval wrong by the same proportion. |
| Two latched readings, a delay apart, differ. | A counter not programmed, or not counting. |
| No reading exceeds the divisor. | The divisor not in force (the counter would range over 16 bits); the only in-machine proof that it took effect. |
| The line is claimed and unmasked. | A timer connected to nothing. |
| No tick counts while interrupts are off; ticks count once on, within a bounded wait. | Ticks from elsewhere; a timer that never fires. The wait is bounded so this failure reports rather than hangs. |
| The controller recorded the request, with no unexpected unclaimed ones. | A handler entered some other way, with no end-of-interrupt sent. |
| Elapsed time agrees with the tick count. | A conversion error. |
| Masking stops ticks; unmasking resumes them. | A mask not honoured. |

`KernelVerifyRtc` in [`../../kernel/test/dev/rtc.c`](../../kernel/test/dev/rtc.c),
`KernelVerifyTime` in [`../../kernel/test/libc/time.c`](../../kernel/test/libc/time.c),
and `signal-check` at privilege level 3. Expected dates come from the host's
`date -u`, not from the code under test.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A BCD 24-hour reading is 2026-09-23 17:15:30, and so is the same moment in binary. | One mode decoded for both. |
| In 12-hour mode `$81` is 13, `$12` is 0, `$92` is 12. | Midnight as noon; noon refused as 24. |
| Month 13 and 2025-02-29 are refused; 2024-02-29 is accepted. | A flat battery's bytes shown as a date; a real leap day refused. |
| Days since 1970 of 1970-01-01, 2000-03-01, 2026-09-23 and 2100-01-01, and the last second of 2024-02-29 in both directions, match the host. | "Every fourth year" (2100) or "never" (2000) leap rules. |
| The machine's clock reads after 2000. | A driver reading nothing and reporting 1970. |
| `gmtime` of 0, 2024-02-29 23:59:59, 2026-09-23 and 2100-01-01 gives the host's date, weekday and day of year; a time before 1970 is refused. | A weekday or month counted from the wrong origin. |
| `alarm(50)` ends a `pause` with one `SIGALRM`; a replaced alarm reports its remainder; a forked child has none; `time` is after 2000 and is stored through its pointer. | The panel's clock stopped; `SIGALRM` ending a child. |

## Limitations

1. The tick counter has one writer and no lock; a reader on another processor
   would also need the read ordered, which `volatile` does not guarantee.
2. `PitWaitTicks` busy-waits; sleeping waits are the scheduler's.
3. A tick missed while interrupts are masked is lost, unrecorded.
4. No time zone: the clock is shown as its owner set it (commonly local time under
   VirtualBox and Bochs, universal time under QEMU). `/bin/date` prints no zone.
5. No century: a clock set to 1999 reads as 2099.
6. Nothing sets the clock.
7. One alarm per process, late by the timer's error under emulation.
8. Each `time` call waits for a stable reading, up to about two milliseconds with
   interrupts masked; callers ask once a minute or once per command.
9. The time-stamp counter is not used; there is no high-resolution time.
