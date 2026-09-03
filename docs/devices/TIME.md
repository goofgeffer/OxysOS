# Timekeeping in Oxys-OS

**Corresponding phase**: Phase 3, sub-task 3.6. This document is revised
whenever a time source is added or the kernel's notion of time is altered.

**Specifications**: Intel 8254 Programmable Interval Timer datasheet (order
number 231164-005), sections "Programming the 8254", "Mode 2: Rate Generator",
"Mode 3: Square Wave Mode" and "Counter Latch Command"; IBM Personal Computer AT
technical reference.

## 1. What the kernel presently knows about time

One thing: how many times counter 0 of the interval timer has raised its request
line since the timer was programmed. Everything else — elapsed milliseconds, the
bounded wait — is derived from that count and the frequency at which the counter
was programmed to produce it.

There is no wall-clock time. The real-time clock is not read, so the kernel does
not know the date, and cannot until a driver for it exists. There is no
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

## 6. Time sources yet to come

| Source | Phase, sub-task | What it adds |
| ------ | --------------- | ------------ |
| The Local APIC timer | 6.7 | A per-processor timer, which a multiprocessor scheduler requires; the single 8254 cannot serve several processors. |
| The time-stamp counter | 6.7 onward | High-resolution intervals, once the interval timer can calibrate it. |
| The real-time clock | Phase 4 onward | Wall-clock date and time, which no source described here provides. |
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
   6.13 a reader upon another processor will additionally require the read to be
   ordered, which the `volatile` qualifier does not by itself guarantee.
2. `PitWaitTicks` is a busy wait. It occupies the processor entirely and cannot
   be used once there is anything else for the processor to do. The sleeping wait
   belongs to the scheduler of sub-task 6.15.
3. There is no accounting of a tick that was missed. Were interrupts masked
   across a period longer than the tick interval, the ticks falling within it
   would simply not be counted and the kernel's notion of elapsed time would lag
   with no record of the fact. The counter's own value could in principle be used
   to detect this, and is not.
4. The timer is a single device and cannot serve several processors. Sub-task 6.12
   introduces the per-processor Local APIC timer for that purpose.
