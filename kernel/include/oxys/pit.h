/*
 * File: kernel/include/oxys/pit.h
 * Purpose: Declares the interface of the 8253/8254 programmable interval timer
 *          driver, which provides the kernel's first periodic time source: the
 *          monotonic tick counter, the conversion of ticks to elapsed time, and
 *          the bounded wait upon which the calibration of later subsystems will
 *          depend.
 * Key definitions: PIT_BASE_FREQUENCY, PIT_DEFAULT_FREQUENCY, PIT_IRQ,
 *          PIT_DIVISOR_MINIMUM, PIT_DIVISOR_MAXIMUM, PitInitialise,
 *          PitTickCount, PitMillisecondsElapsed, PitWaitTicks, PitReadCounter,
 *          PitDivisor, PitRealisedFrequencyMilliHertz, PitIsRunning, PitReport.
 * References:
 *   - Intel 8254 Programmable Interval Timer datasheet (order number
 *     231164-005), section "Programming the 8254": the control word selects the
 *     counter in bits 7 and 6, the read/write format in bits 5 and 4, the
 *     operating mode in bits 3 to 1, and binary or binary-coded-decimal counting
 *     in bit 0. A read/write format of 11 transfers the count as two bytes, the
 *     least significant first.
 *   - 8254 datasheet, section "Mode 2: Rate Generator": the counter reloads
 *     automatically upon reaching one and the output is periodic, which is what
 *     a system tick requires; a count of one is illegal in this mode.
 *   - IBM Personal Computer AT technical reference: counter 0 is decoded at
 *     I/O port 0x40 and the control register at 0x43; counter 0's output is
 *     attached to the interrupt controller's IR0 input; the counters are driven
 *     by a 1.193182 MHz clock derived from the 14.31818 MHz colour subcarrier
 *     reference oscillator divided by twelve.
 *   - Intel SDM, Volume 3A, Section 6.2: the vector upon which the request is
 *     delivered, by way of the remapping of drivers/pic/pic.c.
 */

#ifndef OXYS_PIT_H
#define OXYS_PIT_H

#include <oxys/types.h>

/*
 * The frequency of the clock driving the counters, in hertz.
 *
 * The value is not arbitrary and is not a round number for a reason. The
 * original IBM Personal Computer derived every timing signal from a single
 * 14.31818 MHz crystal, that being four times the 3.579545 MHz colour subcarrier
 * frequency of the NTSC television standard, so that a television receiver could
 * serve as a display. Dividing it by twelve yields 1193181.6 Hz, which is
 * conventionally rounded to the value below.
 */
#define PIT_BASE_FREQUENCY UINT32_C(1193182)

/*
 * The tick rate the kernel requests. One thousand hertz gives a resolution of
 * one millisecond, which is the natural unit for the delays a device driver
 * requires and for the scheduling quantum of sub-task 6.10, and is low enough
 * that the interrupt is of no consequence to throughput.
 */
#define PIT_DEFAULT_FREQUENCY UINT32_C(1000)

/* The request line upon which counter 0 delivers its output. */
#define PIT_IRQ UINT8_C(0)

/*
 * The greatest and least divisors the counter accepts. A divisor of 65536 is
 * expressed as zero, the counter being sixteen bits; a divisor of one is
 * rejected because the rate generator mode does not admit a count of one.
 */
#define PIT_DIVISOR_MINIMUM UINT32_C(2)
#define PIT_DIVISOR_MAXIMUM UINT32_C(65536)

/*
 * Programmes counter 0 as a rate generator at the requested frequency, registers
 * the tick handler upon the timer's request line and unmasks it.
 *
 * The requested frequency is clamped to the range the divisor admits, which is
 * approximately 18.2 Hz to 596591 Hz. The frequency actually realised is
 * generally not the frequency requested, the divisor being an integer;
 * PitRealisedFrequencyMilliHertz reports what was obtained.
 *
 * The interrupt controller must have been initialised by PicInitialise before
 * this is called. Interrupts need not be enabled; no tick is counted until they
 * are.
 */
void PitInitialise(uint32_t frequency);

/*
 * The number of ticks counted since initialisation.
 *
 * The counter is incremented by an interrupt handler and read by ordinary kernel
 * code. A 64-bit aligned read is not torn upon this architecture, so no lock is
 * required to observe a consistent value; from sub-task 6.8 an ordering
 * guarantee will nevertheless be required of readers upon other processors.
 */
uint64_t PitTickCount(void);

/* The elapsed time since initialisation, in milliseconds, derived from the tick
 * count and the frequency actually realised. */
uint64_t PitMillisecondsElapsed(void);

/*
 * Waits until the given number of ticks has elapsed.
 *
 * Returns false if the wait was abandoned because no tick was observed within a
 * bounded number of iterations, which denotes a timer that is not running or
 * interrupts that are not enabled. The bound exists so that a defective timer
 * reports itself rather than hanging the machine: a self-test that never returns
 * destroys the very evidence it was written to produce.
 */
bool PitWaitTicks(uint64_t ticks);

/*
 * Latches and reads the present value of counter 0. The counter decrements
 * continuously, so two reads separated in time differ; that is the only means of
 * establishing that the device is running before interrupts are enabled.
 */
uint16_t PitReadCounter(void);

/* The divisor programmed into counter 0. */
uint32_t PitDivisor(void);

/*
 * The frequency actually realised, in millihertz. It is expressed in thousandths
 * of a hertz because the interesting quantity is the departure from the
 * frequency requested, which is a fraction of one hertz and would vanish if
 * reported as an integer number of hertz.
 */
uint64_t PitRealisedFrequencyMilliHertz(void);

/* Reports whether PitInitialise has run. */
bool PitIsRunning(void);

/* Emits a summary of the timer's configuration and state upon both output devices. */
void PitReport(void);

#endif /* OXYS_PIT_H */
