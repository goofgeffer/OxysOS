/*
 * File: drivers/pit/pit.c
 * Purpose: Implements the driver for counter 0 of the 8253/8254 programmable
 *          interval timer, which is the kernel's first periodic time source: it
 *          programmes the counter as a rate generator at a requested frequency,
 *          counts the resulting interrupts, and converts that count into elapsed
 *          time.
 * Key functions: PitInitialise, PitTickCount, PitMillisecondsElapsed,
 *          PitWaitTicks, PitReadCounter, PitDivisor, PitIsRunning,
 *          PitRealisedFrequencyMilliHertz, PitReport.
 * References:
 *   - Intel 8254 Programmable Interval Timer datasheet (order number
 *     231164-005), section "Programming the 8254" and the Control Word Format:
 *     bits 7 and 6 (SC1, SC0) select the counter; bits 5 and 4 (RW1, RW0) select
 *     the read/write format, the value 11 transferring the count as two bytes
 *     with the least significant first; bits 3 to 1 (M2, M1, M0) select the
 *     operating mode; bit 0 selects binary counting when clear.
 *   - 8254 datasheet, "Mode 2: Rate Generator": the counter is reloaded
 *     automatically upon reaching one, so the output is periodic without further
 *     intervention by software. A count of one is illegal in this mode.
 *   - 8254 datasheet, "Counter Latch Command": a control word whose read/write
 *     field is 00 latches the present count, which may then be read without
 *     disturbing the counting in progress.
 *   - IBM Personal Computer AT technical reference: counter 0 is decoded at I/O
 *     port 0x40 and the control register at port 0x43; the output of counter 0
 *     is attached to the interrupt controller's IR0 input; the counters are
 *     driven by a clock of 1.193182 MHz, being the 14.31818 MHz reference
 *     oscillator divided by twelve.
 *
 * Why mode 2 and not mode 3.
 *
 *   Both modes produce a periodic output and either would raise a periodic
 *   interrupt. Mode 3, the square wave generator, decrements the count by two
 *   upon each clock in order to divide the period equally between the high and
 *   low phases of its output, and consequently behaves correctly only for an
 *   even count. Half of the available divisors are therefore unusable, and an
 *   odd divisor produces a period differing from the one requested.
 *
 *   Nothing here has any interest in the shape of the output waveform; only the
 *   interval between its edges matters. Mode 2 imposes no constraint upon the
 *   divisor beyond excluding one, and therefore realises a frequency closer to
 *   the one requested. Mode 3 is the correct choice for counter 2, which drives
 *   the loudspeaker and where the waveform is the entire point.
 *
 * Concurrency. The tick counter is written by the interrupt handler and read by
 * ordinary kernel code. A 64-bit aligned access is not torn upon x86_64, so a
 * reader observes either the old value or the new and never a mixture, and no
 * lock is required. A reader upon another processor will additionally require
 * the compiler and the processor to be prevented from reordering the read, which
 * the volatile qualifier alone does not guarantee. Since sub-task 6.14 there is
 * another processor, but it is parked and reads nothing here; sub-task 6.15 is
 * when that begins to matter.
 */

#include <oxys/pit.h>
#include <oxys/irq.h>
#include <oxys/interrupts.h>
#include <oxys/io.h>
#include <oxys/kernel.h>

/* The data port of counter 0, and the write-only control register. The data
 * ports of counters 1 and 2 are not defined, this driver having no use for
 * either; docs/devices/TIME.md, Section 2, records what they are attached to. */
#define PIT_CHANNEL0_DATA UINT16_C(0x0040)
#define PIT_COMMAND       UINT16_C(0x0043)

/*
 * Control word fields. The counter is selected in bits 7 and 6, counter 0 being
 * zero; the read/write format in bits 5 and 4; the operating mode in bits 3 to
 * 1; and the radix in bit 0, which is left clear for binary counting.
 */
#define PIT_SELECT_COUNTER0      UINT8_C(0x00)
#define PIT_ACCESS_LATCH         UINT8_C(0x00)
#define PIT_ACCESS_LOW_THEN_HIGH UINT8_C(0x30)
#define PIT_MODE_RATE_GENERATOR  UINT8_C(0x04)
#define PIT_COUNT_BINARY         UINT8_C(0x00)

/*
 * The bound upon PitWaitTicks, expressed in iterations of its inner loop. It is
 * generous: at any plausible processor speed it corresponds to far longer than
 * the period of the slowest tick the divisor admits, so a wait that exhausts it
 * denotes a timer that is not running rather than one that is merely slow.
 */
#define PIT_WAIT_ITERATION_LIMIT UINT64_C(4000000000)

/*
 * The number of ticks counted. Written by the interrupt handler alone and read
 * by ordinary kernel code; declared volatile so that the compiler does not cache
 * it in a register across a loop that waits upon it, which would spin for ever.
 */
static volatile uint64_t PitTicks;

/* The divisor programmed, and whether the counter has been programmed at all. */
static uint32_t PitProgrammedDivisor;
static bool PitRunning;

/*
 * Receives the timer's request line.
 *
 * The handler does nothing but count, and from sub-task 6.15 that is a division
 * of labour rather than a deferral. Pre-emption is measured by the local APIC
 * timer, one per processor — this counter interrupts the bootstrap processor
 * alone, so a scheduler built upon it could never take a thread away from any
 * other. What this device contributes to the scheduler is the calibration:
 * LocalApicCalibrateTimer measures the local timers against this one, the
 * architecture stating no rate for them.
 *
 * The end-of-interrupt is not signalled here; the routing layer of
 * drivers/pic/pic.c does so upon this handler's return, for the reason given in
 * docs/design/INTERRUPTS.md, Section 9.4.
 */
static void PitHandleTick(TrapFrame *frame)
{
    (void)frame;

    ++PitTicks;
}

uint16_t PitReadCounter(void)
{
    uint8_t low;
    uint8_t high;

    /*
     * The counter latch command captures the present count into a holding
     * register, so that the two bytes read below belong to the same instant. A
     * plain read without it would return a value whose halves were sampled at
     * different times, and which could therefore be a count the counter never
     * held.
     */
    PortWriteByte(PIT_COMMAND, (uint8_t)(PIT_SELECT_COUNTER0 | PIT_ACCESS_LATCH));

    low = PortReadByte(PIT_CHANNEL0_DATA);
    high = PortReadByte(PIT_CHANNEL0_DATA);

    return (uint16_t)(((uint16_t)high << 8) | (uint16_t)low);
}

bool PitBusyWaitMicroseconds(uint32_t microseconds)
{
    /*
     * A wait measured by watching the counter rather than by counting ticks.
     *
     * PitWaitTicks reads PitTicks, which the interrupt handler increments, so it
     * cannot be used where interrupts are masked — and the startup sequence of
     * sub-task 6.14 is masked throughout, Intel SDM, Volume 3A, Section 8.4.4.1,
     * requiring every device capable of delivering an interrupt to be inhibited
     * between an INIT and the last startup interrupt of a sequence. It also
     * cannot express two hundred microseconds, a tick being a millisecond.
     *
     * The counter runs at PIT_BASE_FREQUENCY whatever divisor is programmed —
     * Intel 8254 datasheet, "Functional Description": the divisor is the value
     * the counter reloads from, not the rate at which it decrements — so one
     * count is 1/1193182 of a second, or about 838 nanoseconds, which is the
     * resolution of this wait and is ample for both delays the protocol asks
     * for.
     */
    const uint32_t divisor = PitProgrammedDivisor;
    uint64_t remaining;
    uint16_t previous;

    if ((divisor == 0U) || !PitRunning)
    {
        /*
         * There is no counter to watch. The caller is told rather than left to
         * believe it waited: a startup sequence whose delays did not happen is
         * a processor that may not have answered because it was not given time
         * to, and that is a different report from one that would not start.
         */
        return false;
    }

    /* Counts, from microseconds, rounded upward so that a wait is never short. */
    remaining = (((uint64_t)microseconds * (uint64_t)PIT_BASE_FREQUENCY) +
                 UINT64_C(999999)) /
                UINT64_C(1000000);

    previous = PitReadCounter();

    while (remaining > 0U)
    {
        const uint16_t current = PitReadCounter();
        uint64_t elapsed;

        /*
         * The counter decrements and reloads from the divisor, so a reading
         * greater than the previous one is a reload rather than an increase.
         * The two cases are distinguished by comparison and not by watching for
         * the reload, because the reload can be missed: this loop is not
         * guaranteed to observe every count, and it does not have to — it has
         * only to observe the counter at least once per reload, which at a
         * millisecond per reload it does by an enormous margin.
         */
        elapsed = (current <= previous)
                      ? (uint64_t)(previous - current)
                      : ((uint64_t)previous + (uint64_t)divisor - (uint64_t)current);

        remaining = (elapsed >= remaining) ? 0U : (remaining - elapsed);
        previous = current;
    }

    return true;
}

/*
 * Converts a requested frequency into the divisor that most nearly realises it.
 *
 * The division rounds to nearest rather than truncating, the truncation of
 * 1193182/1000 = 1193.182 to 1193 being correct here but the truncation of a
 * value such as 1193.9 to 1193 costing nearly a whole part in a thousand for no
 * reason.
 */
static uint32_t PitDivisorForFrequency(uint32_t frequency)
{
    uint32_t divisor;

    if (frequency == 0U)
    {
        return PIT_DIVISOR_MAXIMUM;
    }

    divisor = (PIT_BASE_FREQUENCY + (frequency / 2U)) / frequency;

    if (divisor < PIT_DIVISOR_MINIMUM)
    {
        /* The rate generator mode does not admit a count of one. */
        divisor = PIT_DIVISOR_MINIMUM;
    }

    if (divisor > PIT_DIVISOR_MAXIMUM)
    {
        divisor = PIT_DIVISOR_MAXIMUM;
    }

    return divisor;
}

void PitInitialise(uint32_t frequency)
{
    const uint32_t divisor = PitDivisorForFrequency(frequency);

    PitProgrammedDivisor = divisor;
    PitTicks = 0U;

    /*
     * Counter 0, both bytes of the count, the rate generator mode, binary
     * counting. The control word must precede the count, the counter using it to
     * determine how many bytes to expect.
     */
    PortWriteByte(PIT_COMMAND,
                  (uint8_t)(PIT_SELECT_COUNTER0 | PIT_ACCESS_LOW_THEN_HIGH |
                            PIT_MODE_RATE_GENERATOR | PIT_COUNT_BINARY));

    /*
     * The count is written least significant byte first, as the read/write
     * format demands. A divisor of 65536 is written as zero, the counter being
     * sixteen bits wide and treating a written zero as its full range; the
     * truncation below performs that conversion without a special case.
     */
    PortWriteByte(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFFU));
    PortWriteByte(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFFU));

    IrqInstallHandler(PIT_IRQ, PitHandleTick, "interval timer");

    PitRunning = true;

    /*
     * The line is unmasked last. Were it unmasked before the handler were
     * registered, a request arriving in between would be counted as unclaimed,
     * and the first tick would be lost.
     */
    IrqUnmaskLine(PIT_IRQ);
}

uint64_t PitTickCount(void)
{
    return PitTicks;
}

uint32_t PitDivisor(void)
{
    return PitProgrammedDivisor;
}

bool PitIsRunning(void)
{
    return PitRunning;
}

uint64_t PitRealisedFrequencyMilliHertz(void)
{
    if (PitProgrammedDivisor == 0U)
    {
        return 0U;
    }

    return ((uint64_t)PIT_BASE_FREQUENCY * UINT64_C(1000)) /
           (uint64_t)PitProgrammedDivisor;
}

uint64_t PitMillisecondsElapsed(void)
{
    const uint64_t frequency_millihertz = PitRealisedFrequencyMilliHertz();

    if (frequency_millihertz == 0U)
    {
        return 0U;
    }

    /*
     * Ticks are converted by the frequency actually realised rather than the one
     * requested. The two differ by a fraction of a per cent, which is
     * immaterial over a single interval and accumulates without bound over a
     * long one; a clock that is wrong by a known amount and does not say so is
     * worse than one that is merely coarse.
     */
    return (PitTicks * UINT64_C(1000000)) / frequency_millihertz;
}

bool PitWaitTicks(uint64_t ticks)
{
    const uint64_t target = PitTicks + ticks;
    uint64_t last_observed = PitTicks;
    uint64_t iterations = 0U;

    while (PitTicks < target)
    {
        /*
         * The bound governs the interval since the last tick observed, not the
         * whole of the wait. Applied to the whole, it would be exhausted by any
         * sufficiently long wait upon a perfectly healthy timer, and the
         * function would report a failure of the device where the caller had
         * merely asked to wait a while.
         */
        if (PitTicks != last_observed)
        {
            last_observed = PitTicks;
            iterations = 0U;
            continue;
        }

        if (++iterations > PIT_WAIT_ITERATION_LIMIT)
        {
            return false;
        }
    }

    return true;
}

void PitReport(void)
{
    uint64_t frequency_millihertz;

    KernelWriteString("Interval timer: ");

    if (!PitRunning)
    {
        KernelWriteString("not initialised.\n");
        return;
    }

    frequency_millihertz = PitRealisedFrequencyMilliHertz();

    KernelWriteString("counter 0 in rate generator mode, divisor ");
    KernelWriteDecimal((uint64_t)PitProgrammedDivisor);
    KernelWriteString(", realised ");
    KernelWriteDecimal(frequency_millihertz / UINT64_C(1000));
    KernelWriteString(".");
    KernelWriteDecimal(frequency_millihertz % UINT64_C(1000));
    KernelWriteString(" Hz.\n");

    KernelWriteString("Interval timer: ticks ");
    KernelWriteDecimal(PitTicks);
    KernelWriteString(", elapsed ");
    KernelWriteDecimal(PitMillisecondsElapsed());
    KernelWriteString(" ms, line ");
    KernelWriteString(IrqLineIsMasked(PIT_IRQ) ? "masked" : "unmasked");
    KernelWriteString(".\n");
}
