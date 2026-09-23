<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Timekeeping in Oxys-OS

**Corresponding phase**: Phase 3, sub-task 3.6. This document is revised
whenever a time source is added or the kernel's notion of time is altered.

**Specifications**: Intel 8254 Programmable Interval Timer datasheet (order
number 231164-005), sections "Programming the 8254", "Mode 2: Rate Generator",
"Mode 3: Square Wave Mode" and "Counter Latch Command"; IBM Personal Computer AT
technical reference. **Since sub-task 9.7**, Section 10: the Motorola MC146818A
data sheet, Figure 14 "Address Map", Table 3, registers A and B and "Update
Cycle"; and Intel's Platform Controller Hub register "NMI Enable (and Real
Time Clock Index) (NMI_EN) – Offset 70".

## 1. What the kernel presently knows about time

One thing: how many times counter 0 of the interval timer has raised its request
line since the timer was programmed. Everything else — elapsed milliseconds, the
bounded wait — is derived from that count and the frequency at which the counter
was programmed to produce it.

~~There is no wall-clock time.~~ **Since sub-task 9.7 the real-time clock is
read**, Section 10, and the kernel knows the date. Before it, the clock was not
read, so the kernel did
not know the date. There is no
high-resolution time source; the time-stamp counter is not read, and its
frequency could not be established without a second clock to calibrate it
against. Both are additions of later phases, and Section 6 records where they
belong.

## 2. The device

The 8253, and the 8254 that superseded it, provides three independent 16-bit
counters driven by a common clock. The IBM Personal Computer AT technical
reference records their use:

| Counter | Port | Attached to | Used by Oxys-OS |
| ------- | ---- | ----------- | --------------- |
| 0 | `0x40` | The interrupt controller's IR0 input | Yes: the system tick. |
| 1 | `0x41` | The dynamic memory refresh request | No. Obsolete upon any machine this kernel will run on. |
| 2 | `0x42` | The loudspeaker gate | No. |

The control register is at port `0x43` and is write-only.

### 2.1 The clock frequency, and why it is not a round number

The counters are driven at 1193182 Hz. The value looks arbitrary and is not.

The original IBM Personal Computer derived every timing signal in the machine
from a single 14.31818 MHz crystal, that frequency being four times the
3.579545 MHz colour subcarrier of the NTSC television standard. The choice was
deliberate: a machine whose timing derived from the television reference could
drive a domestic television receiver as its display without a second oscillator.
Dividing that reference by twelve yields 1193181.6 Hz, conventionally rounded to
1193182.

Every interval this kernel measures is therefore ultimately a count of cycles of
a crystal chosen for the convenience of American analogue television.

## 3. Programming the counter

The control word is written to port `0x43` and the count to port `0x40`. The
8254 datasheet, "Programming the 8254", defines the fields:

| Bits | Field | Value used | Meaning |
| ---- | ----- | ---------- | ------- |
| 7:6 | SC1, SC0 | `00` | Counter 0. |
| 5:4 | RW1, RW0 | `11` | The count is transferred as two bytes, least significant first. |
| 3:1 | M2, M1, M0 | `010` | Mode 2, the rate generator. |
| 0 | BCD | `0` | Binary counting. |

The assembled control word is `0x34`.

The control word must precede the count, the counter using it to determine how
many bytes to expect. The count is then written least significant byte first, as
the read/write field demands; the two writes go to the same port, and their order
is the only thing that distinguishes them.

### 3.1 Why mode 2 and not mode 3

Both modes produce a periodic output, and either would raise a periodic
interrupt. The choice is nevertheless not arbitrary.

Mode 3, the square wave generator, exists to produce an output whose high and
low phases are of equal duration. It achieves this by decrementing the count by
**two** upon each clock, and consequently behaves as intended only for an even
count. Half of the available divisors are therefore unusable, and an odd divisor
yields a period that is not the one asked for.

Nothing in this kernel has any interest in the shape of the waveform; only the
interval between its edges matters, because only the edge raises the interrupt.
Mode 2 constrains the divisor not at all beyond excluding one, and so realises a
frequency closer to the one requested.

Mode 3 is the correct choice for counter 2, which drives the loudspeaker, and
where the duty cycle is the entire point.

### 3.2 The illegal count

The 8254 datasheet records that a count of one is illegal in mode 2. The
counter's output is preconditioned to fall as the count passes from two to one,
and a reload value of one leaves no interval in which that transition can occur;
the output remains high and no interrupt is ever raised.

`PitDivisorForFrequency` therefore clamps the divisor to a minimum of two. The
frequency at which this binds is 596591 Hz, which is far beyond any rate the
kernel would request, but the clamp costs nothing and its absence would produce
a timer that was silently dead.

## 4. The divisor and the frequency actually realised

The divisor is an integer, so the frequency requested is generally not the
frequency obtained:

```
divisor  = round(1193182 / requested)
realised = 1193182 / divisor
```

The rounding is to nearest rather than by truncation. Truncating 1193.182 to
1193 happens to be correct, but truncating a value such as 1193.9 would discard
nearly a whole part in a thousand for no reason.

For the kernel's requested 1000 Hz:

| Quantity | Value |
| -------- | ----- |
| Requested frequency | 1000 Hz |
| Divisor | 1193 |
| Realised frequency | 1000.152 Hz |
| Departure | +0.0152 per cent |
| Accumulated error over one day | approximately +13 seconds |

`PitMillisecondsElapsed` converts by the frequency **realised**, not by the
frequency requested. The difference is immaterial across a single interval and
unbounded across a long one. A clock that is wrong by a known amount and does not
say so is worse than one that is merely coarse, because the error is invisible
in every individual measurement and present in every one.

One thousand hertz was chosen because a millisecond is the natural unit for the
delays a device driver requires and for the scheduling quantum of sub-task 6.15,
and because the resulting interrupt load — one interrupt per millisecond — is of
no consequence to throughput.

## 5. Reading the counter

A count may be read while the counter is running, by the counter latch command:
a control word whose read/write field is `00`, which captures the present count
into a holding register that is then read as two bytes.

The latch is not a convenience. The count is sixteen bits and the port is eight,
so an unlatched read samples the two halves at different instants, and the
counter decrements between them. The value assembled from such a pair may be one
the counter never held at any moment — and the error appears only when the low
byte wraps between the two reads, which is to say rarely, and unpredictably.

Reading the counter is the only means the kernel has of establishing that the
divisor took effect, and Section 7 describes the use the self-test makes of it.

### 5.1 The microsecond wait of sub-task 6.14

`PitBusyWaitMicroseconds` is built upon that latched read, and it exists because
`PitWaitTicks` cannot serve the application-processor bring-up. Two reasons, and
either alone would be sufficient:

- **The tick cannot advance.** `PitWaitTicks` reads a variable the interrupt
  handler increments, and Intel SDM, Volume 3A, Section 8.4.4.1, requires every
  device capable of delivering an interrupt to be inhibited between an INIT and
  the last startup interrupt of a sequence. The bring-up therefore runs with the
  interrupt flag clear throughout, and a wait upon a variable nothing can write
  would never end.
- **A tick is a millisecond.** One of the protocol's two delays is two hundred
  microseconds, which the tick cannot express at all.

So the wait watches the counter itself. Each latched read is compared against the
last and the difference accumulated, the counter being a decrementing one that
reloads — the wrap is handled by adding the divisor where the new reading exceeds
the old, which is the same free-running arithmetic the circular buffers of
[`KEYBOARD.md`](KEYBOARD.md), Section 5.1, use for the same reason.

The resolution is one count of the 1.193182 MHz input, about 838 nanoseconds, and
the requested interval is rounded upward so that a wait is never short: a startup
sequence whose ten-millisecond delay was nine is a sequence the manual does not
describe. It returns false where the counter is not running, so the caller is
told that nothing was waited for rather than left to assume it was —
`SmpInitialise` declines the whole bring-up on that answer, and says so.

## 6. Time sources yet to come

| Source | Phase, sub-task | What it adds |
| ------ | --------------- | ------------ |
| The Local APIC timer | 6.7 | A per-processor timer, which a multiprocessor scheduler requires; the single 8254 cannot serve several processors. |
| The time-stamp counter | 6.7 onward | High-resolution intervals, once the interval timer can calibrate it. |
| ~~The real-time clock~~ | **Arrived at 9.7**, Section 10 | Wall-clock date and time. |
| UEFI runtime services | 12.6 | Time and date by firmware call, upon the UEFI boot path. |

The interval timer is retired as an interrupt source when the I/O APIC
supersedes the 8259A in sub-task 6.12. It is likely to be retained until then as
the calibration reference for the sources that replace it, which is the usual
arrangement and the reason the counter-reading interface of Section 5 is exposed
rather than kept private.

## 7. Verification

`KernelVerifyPit` faces a difficulty peculiar to this subsystem: there is no
second clock against which to check the first. An assertion that a tick took one
millisecond would require a source of known accuracy, and the timer under test is
the only source the kernel has. Every assertion is therefore either internal to
the timer, or concerns the path between the timer and the interrupt controller
beneath it — which is where a defect is in any case most likely.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The divisor is that which the requested frequency demands, to within one | An error in the rounding, which would make every interval the kernel ever measured wrong by the same proportion. |
| Two latched readings, separated by a delay, differ | A counter that was never programmed, or that was programmed in a mode that does not count. |
| No reading exceeds the divisor | **The divisor did not take effect.** Were it not in force the counter would range over the whole of its sixteen bits, and a reading above the divisor would appear almost at once. This is the only confirmation of the divisor available from within the machine. |
| The timer claimed its request line, and the line is unmasked | A device programmed but never connected to anything. |
| No tick is counted while the interrupt flag is clear | A tick counter being advanced by something other than the interrupt. |
| Ticks are counted once the flag is set, within a bounded wait | The timer does not fire at all. |
| The interrupt controller recorded the request, and recorded no unclaimed request beyond the one the controller's own self-test provoked | The handler is entered by some path other than the controller's, in which case the end-of-interrupt is not being sent and the machine would fall silent shortly afterwards. |
| The elapsed time agrees with the tick count | An error in the conversion rather than in the timer. |
| Masking the line stops the ticks; unmasking resumes them | A mask that is not honoured, and with it the whole of the controller's ability to silence a device. |

### 7.1 Why the wait is bounded

`PitWaitTicks` abandons its wait after a bounded number of iterations and returns
false. A timer that never fires is precisely the defect this test exists to find,
and an unbounded wait would meet that defect by hanging — destroying the
diagnosis it was written to produce, and leaving an operator with a machine that
has stopped for no stated reason. The bound is generous enough that it cannot be
reached by a timer that is merely slow.

## 8. Observed state

Under QEMU, at the completion of the self-test:

| Quantity | Value |
| -------- | ----- |
| Divisor | 1193 |
| Realised frequency | 1000.152 Hz |
| Ticks counted by the self-test | 14 upon the run recorded here. The figure depends upon the speed of the host and upon the iteration counts of the delay loops, and is expected to vary between runs; only its being non-zero is asserted. |
| Controller mask | `0xFFFE` |
| Lines claimed | 1, being IR0 |

## 9. Limitations

1. The tick counter is unsynchronised. A 64-bit aligned access is not torn upon
   x86_64, so a reader observes either the old value or the new; from sub-task
   6.14 there is another processor, though a parked one reads nothing here; a
   reader upon another processor will additionally require the read to be
   ordered, which the `volatile` qualifier does not by itself guarantee. No lock
   is required for it — the counter has one writer — so the ordering, and not
   the spinlock sub-task 6.13 built, is what this limitation waits upon.
2. `PitWaitTicks` is a busy wait. It occupies the processor entirely and cannot
   be used once there is anything else for the processor to do. The sleeping wait
   belongs to the scheduler of sub-task 6.15, which measures its quantum by the
   local APIC timer instead — one per processor, this counter reaching only the
   bootstrap processor.
3. There is no accounting of a tick that was missed. Were interrupts masked
   across a period longer than the tick interval, the ticks falling within it
   would simply not be counted and the kernel's notion of elapsed time would lag
   with no record of the fact. The counter's own value could in principle be used
   to detect this, and is not. **Observed on 2026-09-23**, Section 10.1: under
   QEMU a time made of this count fell seconds behind the host within minutes,
   which is why the date is read from the real-time clock at every call.
4. The timer is a single device and cannot serve several processors. Sub-task 6.12
   introduces the per-processor Local APIC timer for that purpose.

## 10. The real-time clock, of sub-task 9.7

The panel's clock of sub-task 9.7 needed the date, which nothing in Sections 1
to 9 provides. It is the clock the PC has carried since the AT, compatible with
the Motorola MC146818A: fourteen registers behind an index written to port
`0x70` and data read from `0x71`. The driver is
[`../../drivers/rtc/rtc.c`](../../drivers/rtc/rtc.c), and the two calls above it
are `time` (42) and `alarm` (43).

**Two modes of each thing, and both read.** The bytes are BCD or binary as
register B's DM bit says, and the hours are 0 to 23 or 1 to 12 with the
afternoon in the high-order bit as its 24/12 bit says. Emulators and firmware
choose differently, and a driver that assumed one reads the other as a
different time: BCD's `$23` read as binary is thirty-five o'clock and refused;
binary's `23` read as BCD is seventeen and accepted. **Twelve is the first hour
of its half of the day** — `$12` is midnight and `$92` noon — which a
conversion that only added twelve for the afternoon gets wrong twice.

**No century.** The chip counts the year to ninety-nine. A century byte exists
upon many chipsets at an offset the ACPI FADT declares, and not upon all; this
driver reads the two digits into 2000 to 2099 and says so, rather than reading
a byte whose place it does not know.

**The update cycle.** Once a second the chip spends up to 1984 microseconds
advancing its registers, during which they are not to be read; the
update-in-progress bit of register A goes high 244 microseconds before it
begins. The driver waits for the bit to be low, reads the six bytes and register
B, waits and reads them all again, and keeps the reading only when the two
agree — the data sheet's first method, and the one that also catches an update
that began between the test of the bit and the last byte, which six port reads
under an emulator can outlast.

**Bit 7 of the index port is the non-maskable interrupt's mask** upon Intel's
chipsets. The driver writes the index with it clear, which is the state the
machine starts in; a driver that set it would mask NMIs for the rest of the run
and nothing would say so.

### 10.1 Why the clock is read at every call

The first form of this driver read the clock once, at start, and made the time
afterwards of that reading and the interval timer's count — one read, and two
readings a second apart could never go backwards. **It was watched and it was
wrong.** Under QEMU the panel's minute turned several seconds after the build
host's within minutes of starting: an emulated timer at a thousand interrupts
a second loses some, limitation 3 of Section 9 said so, and a time made of the
count falls behind by every one it lost, for as long as the machine runs.

So `RtcNow` reads the clock at every call and keeps the interval timer only as
the fallback where a read fails, advanced from the last read that succeeded.
Afterwards the panel turned within two or three seconds of the host's minute,
at each of two minutes watched. The timer remains the measure of **intervals**:
an alarm is so many of its milliseconds from now, so a slow timer makes it late
by the same fraction, and the panel asks the time again when it arrives — the
lateness never accumulates past a minute's worth.

### 10.2 The alarm

`alarm(milliseconds)` sends the caller SIGALRM once, then; zero cancels, and the
call returns what remained of the alarm it replaced. It is IEEE Std
1003.1-2017's `alarm()` counted in milliseconds, because a clock that wants the
start of the next minute wants it to better than a second. **It is served from
the tick**, beside the terminal's control-C and for its reason: a program asleep
in a call is reached by nothing else. Each tick upon the bootstrap processor
compares sixty-four deadlines; a passed one is cleared before SIGALRM is sent,
so that an alarm is one signal and not one per tick until the program runs. **A
child of `fork` has none**, as the standard requires — a copied alarm would send
SIGALRM, whose default ends a process, to a program that never asked for it.

### 10.3 Verification

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A BCD reading in the 24-hour mode is 17:15:30 on 2026-09-23; the same moment in binary is too | A driver that decodes one mode for both, reading one environment's clock as another time |
| In the 12-hour mode `$81` is 13, `$12` is 0 and `$92` is 12 | Half past midnight shown as half past noon; noon as twenty-four o'clock, refused. **Observed** as a damage, Section 10.4 |
| A thirteenth month and the 29th of February 2025 are refused; the 29th of February 2024 is accepted | A flat battery's bytes drawn as a date; or the leap day of a real year refused |
| Days since 1970 of 1970-01-01, 2000-03-01, 2026-09-23 and 2100-01-01 are those the host's `date -u` gives; the last second of 2024-02-29 is too, and turns back into that date, and the second after it is the first of March | A leap rule of "every fourth year" (2100), or of none (2000); a conversion wrong consistently, which a test of it against itself would pass |
| The clock this machine carries reads a time after 2000 | A driver that read nothing and reported 1970 |
| `gmtime` of 0, of the last second of 2024-02-29, of 2026-09-23 and of 2100-01-01 is the host's date, weekday and day of the year; a time before 1970 is refused | A weekday counted from the wrong day, or a month from one — a date exactly that far out. **Observed** as a damage |
| `alarm(50)` ends a `pause` with SIGALRM, once; a replaced alarm reports what remained of it; a child of `fork` has none; `time` is after 2000 and is stored through the pointer | The panel's clock stopped at the minute the session started; SIGALRM ending a child that never asked for it. **Observed** as a damage |

The first five are [`../../kernel/test/dev/rtc.c`](../../kernel/test/dev/rtc.c),
the sixth [`../../kernel/test/libc/time.c`](../../kernel/test/libc/time.c), and
the last is `signal-check`, at privilege level 3.

### 10.4 The damage applied, and what the tests said

In one build, reverted: the 12-hour conversion's reduction of twelve to zero
removed, `gmtime`'s weekday counted from Wednesday, and `fork` made to copy the
alarm. The run said `twelve midnight in the 12-hour mode was not hour 0` and
`twelve noon in the 12-hour mode was not hour 12` — the second because noon
became twenty-four and was refused as out of range; `the first second of 1970
was not a Thursday in January`, with the three dates after it; and from
`signal-check`, `a child of fork inherited its parent's alarm`. Build 16 of the
register is that image.

### 10.5 Limitations

1. **No time zone.** The clock holds whatever its owner set — commonly local
   time under VirtualBox and Bochs as this project runs them, universal under
   QEMU by default — and everything here
   shows it as it reads. `/bin/date` prints no zone for that reason.
2. **No century byte**, Section 10; a clock set to 1999 reads as 2099.
3. **Nothing sets the clock.** There is no call to write it and no network to
   ask.
4. **The alarm is one per process**, as the standard's, and is counted by a
   timer that runs slow under emulation, Section 10.1.
5. **Each call reads the hardware**, which costs a wait of up to two
   milliseconds with interrupts masked. The panel asks once a minute and
   `/bin/date` once; a program that asked in a loop would pay it each time.
