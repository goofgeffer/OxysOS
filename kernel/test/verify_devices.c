/*
 * File: kernel/test/verify_devices.c
 * Purpose: Asserts the device drivers of Phases 3 and 4: the pair of 8259A
 *          interrupt controllers, the interval timer, the PS/2 keyboard, the
 *          16550 serial adapter, the VGA text-mode display, and the enumeration
 *          of the PCI configuration space.
 * Key functions: KernelVerifyPic, KernelVerifyPit, KernelVerifyKeyboard,
 *          KernelVerifySerial, KernelVerifyVga, KernelVerifyPci.
 * References:
   - docs/design/INTERRUPTS.md, Section 9.6: the controller assertions.
 *   - docs/devices/TIME.md, docs/devices/KEYBOARD.md, docs/devices/SERIAL.md,
 *     docs/devices/DISPLAY.md and docs/devices/PCI.md: each has a verification
 *     section pairing the assertions below with what their failure would mean.
 *
 * A driver is asserted through its own interface wherever it can be, and
 * against the device only where it cannot. What is looked for throughout is the
 * failure that does not announce itself: a transmitter interrupt that cannot be
 * dismissed, a request line claimed twice, a cursor moved by the wrong rule.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/pic.h>
#include <oxys/pit.h>
#include <oxys/keyboard.h>
#include <oxys/vga.h>
#include <oxys/serial.h>
#include <oxys/pci.h>
#include <oxys/interrupts.h>
#include <oxys/cpu.h>
#include <oxys/io.h>

/* State recorded by the probe handler of the interrupt controller self-test. */
static uint64_t KernelPicProbeCount;

/* A probe handler standing in for a device driver upon a request line. */
static void KernelPicProbeHandler(TrapFrame *frame)
{
    (void)frame;
    ++KernelPicProbeCount;
}

/*
 * Exercises the 8259A driver, being sub-task 3.5.
 *
 * Three properties are asserted, and the failure of each would present quite
 * differently. A mask register that did not respond would leave a device unable
 * to interrupt, or unable to stop; a routing layer that mistook the vector for
 * the request line would deliver every interrupt to the wrong driver; and an
 * end-of-interrupt sent upon a spurious request would reset the bit of whatever
 * line was genuinely in service, losing a real interrupt at a rate governed by
 * electrical noise and therefore reproducible nowhere.
 */
void KernelVerifyPic(void)
{
    const uint64_t spurious_before = PicSpuriousCount();
    const uint64_t requests_before = PicRequestCount();
    const uint64_t unclaimed_before = PicUnclaimedCount();
    bool succeeded = true;

    /* --- Every line is withheld until a driver claims it. --- */

    if (PicMaskValue() != UINT16_C(0xFFFF))
    {
        KernelWriteString("  Not every request line is masked after initialisation.\n");
        succeeded = false;
    }

    /* Nothing can be in service, no line having been permitted. */
    if (PicInServiceRegister() != 0U)
    {
        KernelWriteString("  A line is in service before any was unmasked.\n");
        succeeded = false;
    }

    /* --- A mask register responds, and the correct controller is addressed. --- */

    PicUnmaskLine(1U);

    if (PicLineIsMasked(1U) || PicMaskValue() != UINT16_C(0xFFFD))
    {
        KernelWriteString("  A master line was not unmasked correctly.\n");
        succeeded = false;
    }

    PicMaskLine(1U);

    if (!PicLineIsMasked(1U) || PicMaskValue() != UINT16_C(0xFFFF))
    {
        KernelWriteString("  A master line was not masked again.\n");
        succeeded = false;
    }

    /*
     * Unmasking a slave line must unmask the cascade with it. Without that, the
     * line would be permitted at the slave and the request would still never
     * reach the processor, the slave's output being attached to the master's IR2.
     */
    PicUnmaskLine(12U);

    if (PicLineIsMasked(12U))
    {
        KernelWriteString("  A slave line was not unmasked.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(PIC_CASCADE_IRQ))
    {
        KernelWriteString("  Unmasking a slave line did not unmask the cascade.\n");
        succeeded = false;
    }

    PicMaskLine(12U);
    PicMaskLine(PIC_CASCADE_IRQ);

    /* --- A request is routed to the driver that claims its line. --- */

    PicInstallHandler(3U, KernelPicProbeHandler, "self-test probe");

    if (PicRegisteredHandler(3U) != KernelPicProbeHandler)
    {
        KernelWriteString("  The claimed line did not record its handler.\n");
        succeeded = false;
    }

    /*
     * Vector 35 is the third request line of the master. Raising it by software
     * exercises the routing arithmetic without requiring a device to be present.
     * The end-of-interrupt this provokes is harmless: per the 8259A datasheet,
     * section "OPERATION COMMAND WORDS (OCWS)", a non-specific command resets the
     * highest priority bit set in the in-service register, and no bit is set.
     */
    __asm__ __volatile__("int $35" : : : "memory");

    if (KernelPicProbeCount != 1U)
    {
        KernelWriteString("  The claimed line's handler was not entered.\n");
        succeeded = false;
    }

    if (PicRequestCount() != (requests_before + 1U))
    {
        KernelWriteString("  The request was not counted.\n");
        succeeded = false;
    }

    PicRemoveHandler(3U);

    /* An unclaimed line must still be acknowledged, and counted as unclaimed. */
    __asm__ __volatile__("int $35" : : : "memory");

    if (KernelPicProbeCount != 1U)
    {
        KernelWriteString("  A removed handler was entered.\n");
        succeeded = false;
    }

    if (PicUnclaimedCount() != (unclaimed_before + 1U))
    {
        KernelWriteString("  An unclaimed request was not counted.\n");
        succeeded = false;
    }

    /* --- A spurious request is recognised and not acknowledged. --- */

    /*
     * Vector 39 is the master's lowest priority line, upon which a spurious
     * request is delivered. No line is in service, so the in-service register
     * bit is clear and the request must be recognised as spurious: counted, not
     * routed, and above all not acknowledged.
     */
    __asm__ __volatile__("int $39" : : : "memory");

    if (PicSpuriousCount() != (spurious_before + 1U))
    {
        KernelWriteString("  A spurious request was not recognised.\n");
        succeeded = false;
    }

    if (PicRequestCount() != (requests_before + 2U))
    {
        KernelWriteString("  A spurious request was counted as a genuine one.\n");
        succeeded = false;
    }

    /* --- The remapping holds with the interrupt flag set. --- */

    /*
     * This is the assertion that the remapping itself is correct, and it cannot
     * be made by inspection: the 8259A does not present ICW2 for reading, so the
     * vector base cannot be read back from the device.
     *
     * Were the controllers still presenting the vectors the firmware programmed,
     * the interval timer, which the IBM Personal Computer AT technical reference
     * records as left running by the firmware upon IR0, would deliver its request
     * as vector 8 the instant the interrupt flag was set. Vector 8 is the double
     * fault, per Intel SDM, Volume 3A, Table 6-1, and the machine would not reach
     * the following line.
     *
     * Every line is masked, so nothing should be delivered at all; the pause is
     * long enough for many timer periods at the firmware's default rate.
     */
    __asm__ __volatile__("sti" : : : "memory");

    for (volatile uint32_t spin = 0U; spin < 2000000U; ++spin)
    {
        /* Deliberately empty: time is allowed to pass with interrupts enabled. */
    }

    __asm__ __volatile__("cli" : : : "memory");

    if (PicRequestCount() != (requests_before + 2U))
    {
        KernelWriteString("  A masked line was nevertheless delivered.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded
                          ? "Interrupt controller self-test passed.\n"
                          : "Interrupt controller self-test FAILED.\n");
}

/*
 * Exercises the programmable interval timer, being sub-task 3.6.
 *
 * This is the first self-test whose subject is a device that acts of its own
 * accord, and the difficulty that introduces is that there is no second clock
 * against which to check the first. Every assertion below is therefore either
 * internal to the timer, or concerns the relationship between the timer and the
 * interrupt controller beneath it, which is the part most likely to be wrong.
 */
void KernelVerifyPit(void)
{
    const uint32_t expected_divisor = PIT_BASE_FREQUENCY / PIT_DEFAULT_FREQUENCY;
    uint16_t first_reading;
    uint16_t second_reading;
    uint64_t ticks_before;
    uint64_t ticks_after;
    uint64_t ticks_while_masked;
    bool observed_change = false;
    bool succeeded = true;

    if (!PitIsRunning())
    {
        KernelWriteString("  The timer reports that it was not initialised.\n");
        KernelWriteString("Interval timer self-test FAILED.\n");
        return;
    }

    /* --- The divisor took effect. --- */

    /*
     * The requested frequency is 1000 Hz and the clock 1193182 Hz, so the
     * divisor rounds to 1193. A divisor that differed would mean the arithmetic
     * of PitDivisorForFrequency was wrong, and every interval the kernel ever
     * measured would be wrong with it.
     */
    if (PitDivisor() != expected_divisor && PitDivisor() != (expected_divisor + 1U))
    {
        KernelWriteString("  The divisor is not that required by the frequency.\n");
        succeeded = false;
    }

    /* --- The counter is running, and running within the divisor. --- */

    /*
     * Two latched readings separated by a delay must differ. This is the only
     * assertion available before interrupts are enabled, and it distinguishes a
     * counter that was programmed from one that was not.
     */
    first_reading = PitReadCounter();

    for (volatile uint32_t spin = 0U; spin < 100000U; ++spin)
    {
        /* Deliberately empty: time is allowed to pass. */
    }

    second_reading = PitReadCounter();

    if (first_reading == second_reading)
    {
        KernelWriteString("  The counter is not counting.\n");
        succeeded = false;
    }

    /*
     * Every reading must lie within the divisor, the counter counting down from
     * it and reloading. Were the divisor not in force the counter would range
     * over the whole of its sixteen bits, and readings above the divisor would
     * appear almost at once. This is the only means of confirming the divisor
     * from within the machine, the 8254 offering no way to read a count back
     * other than the one in progress.
     */
    for (size_t sample = 0U; sample < 64U; ++sample)
    {
        const uint16_t reading = PitReadCounter();

        if ((uint32_t)reading > PitDivisor())
        {
            KernelWriteString("  A count exceeded the divisor.\n");
            succeeded = false;
            break;
        }

        if (reading != first_reading)
        {
            observed_change = true;
        }
    }

    if (!observed_change)
    {
        KernelWriteString("  Repeated readings of the counter never changed.\n");
        succeeded = false;
    }

    /* --- The line is claimed and permitted. --- */

    if (PicRegisteredHandler(PIT_IRQ) == NULL)
    {
        KernelWriteString("  The timer did not claim its request line.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(PIT_IRQ))
    {
        KernelWriteString("  The timer's request line is masked.\n");
        succeeded = false;
    }

    /* --- No tick is counted while interrupts are disabled. --- */

    ticks_before = PitTickCount();

    for (volatile uint32_t spin = 0U; spin < 500000U; ++spin)
    {
        /* Deliberately empty. */
    }

    if (PitTickCount() != ticks_before)
    {
        KernelWriteString("  A tick was counted with the interrupt flag clear.\n");
        succeeded = false;
    }

    /* --- Ticks are counted once interrupts are enabled. --- */

    __asm__ __volatile__("sti" : : : "memory");

    /*
     * The wait is bounded and reports its own failure rather than spinning for
     * ever. A timer that never fires is precisely the defect this test exists to
     * find, and a test that hung upon finding it would destroy the diagnosis it
     * was written to produce.
     */
    if (!PitWaitTicks(10U))
    {
        KernelWriteString("  No tick arrived within the permitted interval.\n");
        succeeded = false;
    }

    ticks_after = PitTickCount();

    if (ticks_after < (ticks_before + 10U))
    {
        KernelWriteString("  The tick count did not advance as far as awaited.\n");
        succeeded = false;
    }

    /*
     * The request must have reached the routing layer of the interrupt
     * controller and been counted there. A tick counted here but not there would
     * mean the handler was being entered by some path other than the one the
     * controller uses, and the end-of-interrupt would not be being sent.
     */
    if (PicRequestCount() == 0U)
    {
        KernelWriteString("  The controller recorded no request for the timer.\n");
        succeeded = false;
    }

    if (PicUnclaimedCount() > 1U)
    {
        KernelWriteString("  A timer request was recorded as unclaimed.\n");
        succeeded = false;
    }

    /*
     * The elapsed time must agree with the tick count and the realised
     * frequency. At 1000 Hz the two are numerically equal to within a
     * millisecond, and a gross disagreement would denote an error in the
     * conversion rather than in the timer.
     */
    if (PitMillisecondsElapsed() < (ticks_after - 1U) ||
        PitMillisecondsElapsed() > (ticks_after + 1U))
    {
        KernelWriteString("  The elapsed time does not agree with the tick count.\n");
        succeeded = false;
    }

    /* --- Masking the line stops the ticks, and unmasking resumes them. --- */

    PicMaskLine(PIT_IRQ);

    ticks_while_masked = PitTickCount();

    for (volatile uint32_t spin = 0U; spin < 2000000U; ++spin)
    {
        /* Deliberately empty: far longer than a tick period. */
    }

    if (PitTickCount() != ticks_while_masked)
    {
        KernelWriteString("  A tick was counted while the line was masked.\n");
        succeeded = false;
    }

    PicUnmaskLine(PIT_IRQ);

    if (!PitWaitTicks(2U))
    {
        KernelWriteString("  Ticks did not resume when the line was unmasked.\n");
        succeeded = false;
    }

    __asm__ __volatile__("cli" : : : "memory");

    KernelWriteString(succeeded
                          ? "Interval timer self-test passed.\n"
                          : "Interval timer self-test FAILED.\n");
}

/*
 * Drives the keyboard decoder with one scancode and yields the character of the
 * event it produced, or zero where it produced none.
 *
 * The decoder is driven directly rather than by way of the controller, which is
 * what permits the whole of scan code set 1 to be exercised upon a machine at
 * which nobody is typing. The path from the controller to the decoder is covered
 * separately, by the assertions upon the request line.
 */
static char KernelKeyboardDecode(uint8_t scancode)
{
    KeyEvent event;

    KeyboardProcessScancode(scancode);

    if (!KeyboardReadEvent(&event))
    {
        return '\0';
    }

    return event.pressed ? event.character : '\0';
}

/*
 * Exercises the PS/2 keyboard driver, being sub-task 3.7.
 *
 * A keyboard cannot be made to produce a keystroke by the kernel that drives it,
 * so the test is divided. The controller and the request line are asserted as
 * configured state; the decoding of scan code set 1, the modifier discipline and
 * the circular buffer are asserted by driving the decoder with codes of the
 * kernel's own choosing, which is exactly what the hardware would deliver.
 */
void KernelVerifyKeyboard(void)
{
    KeyEvent event;
    char character;
    uint64_t discarded_before;
    bool succeeded = true;

    if (!KeyboardIsPresent())
    {
        /*
         * Not a failure of the test. A machine may genuinely have no PS/2
         * keyboard, and the driver is required to discover that without
         * blocking; reaching this line at all is evidence that it did.
         */
        KernelWriteString("  No keyboard was found; the decoder is not exercised.\n");
        KernelWriteString("Keyboard self-test skipped.\n");
        return;
    }

    /* --- The controller was configured and the line claimed. --- */

    if (PicRegisteredHandler(KEYBOARD_IRQ) == NULL)
    {
        KernelWriteString("  The keyboard did not claim its request line.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(KEYBOARD_IRQ))
    {
        KernelWriteString("  The keyboard's request line is masked.\n");
        succeeded = false;
    }

    /* Begin from a known state, the firmware having used the keyboard before us. */
    KeyboardFlush();

    if (KeyboardHasEvent() || KeyboardReadEvent(&event))
    {
        KernelWriteString("  The buffer is not empty after a flush.\n");
        succeeded = false;
    }

    if (KeyboardModifiers() != 0U)
    {
        KernelWriteString("  A modifier is in force after a flush.\n");
        succeeded = false;
    }

    /* --- An unshifted key yields its lower-case character. --- */

    if (KernelKeyboardDecode(0x1EU) != 'a')
    {
        KernelWriteString("  An unshifted key did not yield its character.\n");
        succeeded = false;
    }

    /* --- A release is recorded, and is distinguished from a depression. --- */

    KeyboardProcessScancode(0x9EU);

    if (!KeyboardReadEvent(&event))
    {
        KernelWriteString("  A release produced no event.\n");
        succeeded = false;
    }
    else if (event.pressed || event.scancode != 0x1EU)
    {
        KernelWriteString("  A release was decoded as a depression.\n");
        succeeded = false;
    }

    /* --- A modifier produces no event of its own, and alters the next key. --- */

    KeyboardProcessScancode(0x2AU);

    if (KeyboardHasEvent())
    {
        KernelWriteString("  A modifier key produced an event of its own.\n");
        succeeded = false;
    }

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_SHIFT) == 0U)
    {
        KernelWriteString("  A depressed shift key did not set its flag.\n");
        succeeded = false;
    }

    if (KernelKeyboardDecode(0x1EU) != 'A')
    {
        KernelWriteString("  A shifted letter did not yield its capital.\n");
        succeeded = false;
    }

    /* A shifted digit yields its punctuation, which capitals lock must not. */
    if (KernelKeyboardDecode(0x02U) != '!')
    {
        KernelWriteString("  A shifted digit did not yield its punctuation.\n");
        succeeded = false;
    }

    KeyboardProcessScancode(0xAAU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_SHIFT) != 0U)
    {
        KernelWriteString("  A released shift key did not clear its flag.\n");
        succeeded = false;
    }

    if (KernelKeyboardDecode(0x1EU) != 'a')
    {
        KernelWriteString("  The letter did not revert when shift was released.\n");
        succeeded = false;
    }

    /* --- Capitals lock is a latch, and applies to letters alone. --- */

    KeyboardProcessScancode(0x3AU);
    KeyboardProcessScancode(0xBAU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CAPS_LOCK) == 0U)
    {
        KernelWriteString("  Capitals lock did not latch upon a full keystroke.\n");
        succeeded = false;
    }

    if (KernelKeyboardDecode(0x1EU) != 'A')
    {
        KernelWriteString("  Capitals lock did not capitalise a letter.\n");
        succeeded = false;
    }

    /*
     * The lock must not act upon a digit. Were it implemented as a second shift
     * this would yield an exclamation mark, which is the commonest way for this
     * to be got wrong.
     */
    if (KernelKeyboardDecode(0x02U) != '1')
    {
        KernelWriteString("  Capitals lock altered a digit.\n");
        succeeded = false;
    }

    /*
     * Shift with the lock engaged yields the lower-case letter. The two combine
     * as an exclusive disjunction, not as a disjunction.
     */
    KeyboardProcessScancode(0x2AU);

    if (KernelKeyboardDecode(0x1EU) != 'a')
    {
        KernelWriteString("  Shift with capitals lock did not yield lower case.\n");
        succeeded = false;
    }

    KeyboardProcessScancode(0xAAU);

    /* Release the latch, so that the state left behind is the state found. */
    KeyboardProcessScancode(0x3AU);
    KeyboardProcessScancode(0xBAU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CAPS_LOCK) != 0U)
    {
        KernelWriteString("  Capitals lock did not unlatch.\n");
        succeeded = false;
    }

    /* --- The extended prefix is consumed and marks the event it precedes. --- */

    KeyboardProcessScancode(0xE0U);
    KeyboardProcessScancode(0x1DU);

    if (KeyboardHasEvent())
    {
        KernelWriteString("  The right control key produced an event.\n");
        succeeded = false;
    }

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CONTROL) == 0U)
    {
        KernelWriteString("  The right control key did not set the control flag.\n");
        succeeded = false;
    }

    KeyboardProcessScancode(0xE0U);
    KeyboardProcessScancode(0x9DU);

    if ((KeyboardModifiers() & KEYBOARD_MODIFIER_CONTROL) != 0U)
    {
        KernelWriteString("  The right control key did not clear the control flag.\n");
        succeeded = false;
    }

    /*
     * An extended key that is not a modifier produces an event marked extended
     * and bearing no character, its number being shared with an ordinary key.
     */
    KeyboardProcessScancode(0xE0U);
    KeyboardProcessScancode(0x48U);

    if (!KeyboardReadEvent(&event))
    {
        KernelWriteString("  An extended key produced no event.\n");
        succeeded = false;
    }
    else if (!event.extended || event.character != '\0' || event.scancode != 0x48U)
    {
        KernelWriteString("  An extended key was decoded as its ordinary twin.\n");
        succeeded = false;
    }

    /* --- Reading a character skips releases. --- */

    KeyboardProcessScancode(0x30U); /* 'b' depressed. */
    KeyboardProcessScancode(0xB0U); /* 'b' released. */
    KeyboardProcessScancode(0x2EU); /* 'c' depressed. */

    if (!KeyboardReadCharacter(&character) || character != 'b')
    {
        KernelWriteString("  A character was not read from the buffer.\n");
        succeeded = false;
    }

    if (!KeyboardReadCharacter(&character) || character != 'c')
    {
        KernelWriteString("  A release was not skipped when reading a character.\n");
        succeeded = false;
    }

    if (KeyboardReadCharacter(&character))
    {
        KernelWriteString("  A character was read from an exhausted buffer.\n");
        succeeded = false;
    }

    /* --- The buffer is circular, and an overrun is counted rather than silent. --- */

    discarded_before = KeyboardOverflowCount();

    for (size_t index = 0U; index < (KEYBOARD_BUFFER_CAPACITY + 8U); ++index)
    {
        KeyboardProcessScancode(0x1EU);
    }

    if (KeyboardOverflowCount() != (discarded_before + 8U))
    {
        KernelWriteString("  An overrun was not counted exactly.\n");
        succeeded = false;
    }

    /*
     * The events that were accepted must still be readable and intact. An
     * overrun that corrupted the buffer rather than refusing the surplus would
     * be far worse than one that simply lost keystrokes.
     */
    {
        size_t recovered = 0U;

        while (KeyboardReadEvent(&event))
        {
            if (event.scancode != 0x1EU || !event.pressed)
            {
                KernelWriteString("  An event survived the overrun corrupted.\n");
                succeeded = false;
                break;
            }

            ++recovered;
        }

        if (recovered != KEYBOARD_BUFFER_CAPACITY)
        {
            KernelWriteString("  The buffer did not hold its stated capacity.\n");
            succeeded = false;
        }
    }

    KeyboardFlush();

    KernelWriteString(succeeded
                          ? "Keyboard self-test passed.\n"
                          : "Keyboard self-test FAILED.\n");
}

/*
 * Exercises the serial driver, being sub-task 4.1.
 *
 * Three things here can fail silently, and they are what the test is for.
 *
 * The first is the transmitter interrupt. The condition it reports is a level
 * and not an event: an adapter with nothing to send holds its transmitter
 * holding register empty permanently, so a driver that left the interrupt
 * enabled would be asked to service a condition it could not dismiss, and the
 * machine would make no further progress while reporting nothing at all.
 *
 * The second is the interrupt path itself. The driver retains a polled path for
 * the circumstances in which no interrupt can arrive, and that path works
 * whether or not the request line was ever claimed; a driver that had claimed
 * nothing would therefore appear to function perfectly. The count of the
 * adapter's interrupts is what distinguishes the two.
 *
 * The third is the line parameters. A divisor computed wrongly yields output at
 * a rate nothing is listening at, which is indistinguishable from an absent
 * adapter, and the rate realised is not in general the rate requested.
 */
void KernelVerifySerial(void)
{
    static const SerialConfiguration alternative = {
        9600U, 7U, SERIAL_PARITY_EVEN, SERIAL_STOP_BITS_TWO
    };
    static const SerialConfiguration standard = {
        115200U, 8U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE
    };

    /*
     * A rate of zero, a rate above the greatest the oscillator can produce, and
     * word lengths on either side of the five to eight the register can express.
     * Each must be refused with the parameters in force left untouched.
     */
    static const SerialConfiguration impossible[] = {
        { 0U, 8U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE },
        { 230400U, 8U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE },
        { 9600U, 4U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE },
        { 9600U, 9U, SERIAL_PARITY_NONE, SERIAL_STOP_BITS_ONE }
    };

    uint64_t interrupts_before;
    uint64_t transmitted_before;
    bool loopback_passed;
    bool rejected_impossible = true;
    bool accepted_alternative;
    bool alternative_divisor_correct;
    bool restored;
    bool succeeded = true;

    if (!SerialIsPresent())
    {
        /*
         * Not a failure of the test. A machine may genuinely have no serial
         * adapter, and the loopback test at initialisation is what discovers it.
         */
        KernelWriteString("Serial self-test skipped; no adapter is present.\n");
        return;
    }

    /* --- The line was claimed and the request line permitted. --- */

    if (!SerialInterruptsActive())
    {
        KernelWriteString("  The serial driver is still polling.\n");
        succeeded = false;
    }

    if (PicRegisteredHandler(SERIAL_COM1_IRQ) == NULL)
    {
        KernelWriteString("  The serial adapter did not claim its request line.\n");
        succeeded = false;
    }

    if (PicLineIsMasked(SERIAL_COM1_IRQ))
    {
        KernelWriteString("  The serial adapter's request line is masked.\n");
        succeeded = false;
    }

    /* --- The adapter carries a character out and back unaltered. --- */

    loopback_passed = SerialLoopbackTest();

    if (!loopback_passed)
    {
        KernelWriteString("  A sequence did not return unaltered through the loopback.\n");
        succeeded = false;
    }

    /*
     * --- The line parameters are computed, and the impossible refused. ---
     *
     * Nothing is written to the console between the two configurations below.
     * The alternative rate is applied to the adapter, and anything transmitted
     * while it stood would reach a listening terminal as noise.
     */

    for (size_t index = 0U;
         index < (sizeof impossible / sizeof impossible[0]);
         ++index)
    {
        if (SerialConfigure(&impossible[index]))
        {
            rejected_impossible = false;
        }
    }

    if (SerialConfigure(NULL))
    {
        rejected_impossible = false;
    }

    accepted_alternative = SerialConfigure(&alternative);
    alternative_divisor_correct =
        (SerialDivisor() == 12U) && (SerialRealisedBaudRate() == 9600U);
    restored = SerialConfigure(&standard) && (SerialDivisor() == 1U) &&
               (SerialRealisedBaudRate() == SERIAL_MAXIMUM_BAUD_RATE);

    if (!rejected_impossible)
    {
        KernelWriteString("  An impossible line configuration was accepted.\n");
        succeeded = false;
    }

    if (!accepted_alternative || !alternative_divisor_correct)
    {
        KernelWriteString("  9600 baud did not yield a divisor of twelve.\n");
        succeeded = false;
    }

    if (!restored)
    {
        KernelWriteString("  The default line parameters were not restored.\n");
        succeeded = false;
    }

    /* --- Characters leave by way of an interrupt, and the request is withdrawn. --- */

    interrupts_before = SerialInterruptCount();
    transmitted_before = SerialCharactersTransmitted();

    /*
     * The interrupt flag is set for the duration, this being the only way an
     * interrupt can be taken; the flag is otherwise clear throughout
     * initialisation. The string is written to the adapter alone, the display
     * having no part in what is being asserted.
     */
    __asm__ __volatile__("sti" : : : "memory");

    SerialWriteString("Serial self-test: this line was carried by interrupt.\n");
    SerialFlush();

    __asm__ __volatile__("cli" : : : "memory");

    if (SerialInterruptCount() == interrupts_before)
    {
        KernelWriteString("  The adapter transmitted without raising an interrupt.\n");
        succeeded = false;
    }

    if (SerialCharactersTransmitted() == transmitted_before)
    {
        KernelWriteString("  No character was transmitted.\n");
        succeeded = false;
    }

    if (SerialTransmitInterruptEnabled())
    {
        KernelWriteString("  The transmitter interrupt was not withdrawn when idle.\n");
        succeeded = false;
    }

    if (SerialLineErrorCount() != 0U)
    {
        KernelWriteString("  The line reported an error during the test.\n");
        succeeded = false;
    }

    /* The loopback and the firmware may both have left characters behind. */
    SerialFlushBuffers();

    KernelWriteString(succeeded
                          ? "Serial self-test passed.\n"
                          : "Serial self-test FAILED.\n");
}

/*
 * Asserts that the display driver moves the cursor as the control characters
 * require, that it reaches the CRT controller, and that the backspace stops
 * where it is told to. The failure this guards against is a silent one: a
 * control character for which the driver has no case is written into the frame
 * buffer as whatever glyph the adapter's font holds at that code point, and the
 * cursor then advances to the right. The display is not corrupted in any way the
 * machine can notice, and the defect is visible only to somebody reading the
 * screen. That is exactly how the backspace came to be broken.
 *
 * The properties asserted here are chosen upon the same principle throughout: a
 * cursor written to the wrong CRT controller register, an attribute written
 * while the controller's flip-flop stood at the data register, a scroll that
 * moved the display by the wrong number of rows — each leaves a machine that
 * runs perfectly and a display that is wrong to look at.
 *
 * The test writes upon the display, so it begins at the start of a row and
 * leaves its own result to overwrite the characters used.
 */
void KernelVerifyVga(void)
{
    size_t row;
    size_t column;
    size_t original_row;
    size_t limit_row;
    size_t limit_column;
    uint64_t scroll_marker;
    bool succeeded = true;

    /* The adapter's configuration governs every register access that follows. */
    if (!VgaIsColourAdapter() || (VgaCrtcIndexPort() != 0x03D4U))
    {
        KernelWriteString("  The display adapter is not in its colour configuration.\n");
        succeeded = false;
    }

    if (VgaBlinkEnabled())
    {
        KernelWriteString("  Blinking was not disabled; bright backgrounds will blink.\n");
        succeeded = false;
    }

    VgaPutCharacter('\n');
    VgaCursorPosition(&original_row, &column);

    if (column != 0U)
    {
        KernelWriteString("  A line feed did not return the cursor to the first column.\n");
        succeeded = false;
    }

    /*
     * The erase limit is placed here. Everything the test writes below stands
     * after it, and the boot log above it is therefore beyond the reach of the
     * backspaces the test performs, which is the property the limit exists for.
     */
    VgaSetEraseLimit();
    VgaEraseLimit(&limit_row, &limit_column);

    if ((limit_row != original_row) || (limit_column != 0U))
    {
        KernelWriteString("  The erase limit was not recorded at the cursor.\n");
        succeeded = false;
    }

    /* A backspace at the limit must not move at all. */
    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U))
    {
        KernelWriteString("  A backspace at the erase limit moved the cursor.\n");
        succeeded = false;
    }

    /* A backspace elsewhere retreats by exactly one column. */
    VgaPutCharacter('X');
    VgaPutCharacter('Y');
    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 1U))
    {
        KernelWriteString("  A backspace did not retreat by one column.\n");
        succeeded = false;
    }

    /*
     * The erasing sequence the callers use must leave the cursor where the
     * erased character stood, so that the next character written replaces it,
     * and must have blanked the cell it passed over.
     */
    VgaWriteString("\b \b");
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U) || (VgaCharacterAt(row, 0U) != ' '))
    {
        KernelWriteString("  The erasing sequence did not erase and restore the cursor.\n");
        succeeded = false;
    }

    /* A tabulation advances to a multiple of eight columns, not by eight. */
    VgaPutCharacter('A');
    VgaPutCharacter('\t');
    VgaCursorPosition(&row, &column);

    if (column != 8U)
    {
        KernelWriteString("  A tabulation did not advance to a multiple of eight.\n");
        succeeded = false;
    }

    /* A carriage return returns to the first column without changing the row. */
    VgaPutCharacter('\r');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U))
    {
        KernelWriteString("  A carriage return did not return to the first column.\n");
        succeeded = false;
    }

    /*
     * A backspace in the first column crosses into the row above and stops
     * immediately after the text standing there, having consumed the separator
     * between the two rows and nothing else. This is what allows a line of input
     * to be corrected after a line feed; the character at the end of the row
     * above is erased by the next backspace, not by this one.
     */
    scroll_marker = VgaScrollCount();
    VgaWriteString("ab\n");

    /*
     * That line feed stood upon the final row if the boot log had filled the
     * display, in which case everything above has moved up by one row and the
     * row the test is reasoning about has moved with it.
     */
    if (VgaScrollCount() != scroll_marker)
    {
        --original_row;
    }

    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 2U) || (VgaCharacterAt(row, 1U) != 'b'))
    {
        KernelWriteString("  A backspace across the row boundary consumed a character.\n");
        succeeded = false;
    }

    /* Erasing both characters returns the cursor to the limit, and no further. */
    VgaWriteString("\b \b");
    VgaWriteString("\b \b");
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U) || (VgaCharacterAt(row, 1U) != ' '))
    {
        KernelWriteString("  Erasing across the row boundary left the cursor astray.\n");
        succeeded = false;
    }

    VgaPutCharacter('\b');
    VgaCursorPosition(&row, &column);

    if ((row != original_row) || (column != 0U))
    {
        KernelWriteString("  A backspace passed the erase limit into the boot log.\n");
        succeeded = false;
    }

    /* The controller must hold the position the driver believes it holds. */
    VgaCursorPosition(&row, &column);

    {
        size_t hardware_row;
        size_t hardware_column;
        const bool visible = VgaHardwareCursorPosition(&hardware_row, &hardware_column);

        if ((hardware_row != row) || (hardware_column != column))
        {
            KernelWriteString("  The hardware cursor is not where the driver believes.\n");
            succeeded = false;
        }

        if (!visible)
        {
            KernelWriteString("  The hardware cursor was not displayed.\n");
            succeeded = false;
        }
    }

    /* A position outside the display is refused rather than wrapped. */
    if (VgaSetCursorPosition(VGA_HEIGHT, 0U) || VgaSetCursorPosition(0U, VGA_WIDTH))
    {
        KernelWriteString("  A cursor position outside the display was accepted.\n");
        succeeded = false;
    }

    /* Hiding the cursor must be observable in the controller, and reversible. */
    VgaSetCursorVisible(false);

    if (VgaCursorVisible())
    {
        KernelWriteString("  The cursor was not hidden when it was hidden.\n");
        succeeded = false;
    }

    VgaSetCursorVisible(true);

    if (!VgaCursorVisible())
    {
        KernelWriteString("  The cursor was not restored after being hidden.\n");
        succeeded = false;
    }

    /* A shape whose first scan line is below its last would present no cursor. */
    if (VgaSetCursorShape(4U, 2U) || VgaSetCursorShape(0U, 32U))
    {
        KernelWriteString("  An impossible cursor shape was accepted.\n");
        succeeded = false;
    }

    /*
     * The scroll is asserted upon the contents of the display itself, a scroll
     * by the wrong number of rows being invisible to everything else. One row of
     * the boot log leaves the display for the purpose; the record upon the
     * serial line is unaffected.
     */
    if (original_row > 0U)
    {
        const uint64_t scrolls = VgaScrollCount();

        VgaSetCursorPosition(original_row, 0U);
        VgaPutCharacter('Z');
        VgaScroll();

        if ((VgaCharacterAt(original_row - 1U, 0U) != 'Z') ||
            (VgaCharacterAt(VGA_HEIGHT - 1U, 0U) != ' ') || (VgaScrollCount() != scrolls + 1U))
        {
            KernelWriteString("  The display did not scroll by exactly one row.\n");
            succeeded = false;
        }

        VgaSetCursorPosition(original_row, 0U);
    }

    /*
     * The characters written above stand upon this row still. The result is
     * written over them, and padded so that none survives to its right.
     */
    KernelWriteString(succeeded
                          ? "Display self-test passed.            \n"
                          : "Display self-test FAILED.            \n");
}

/*
 * Asserts that the bus enumeration reached the hardware and understood what it
 * read.
 *
 * The failure this guards against is that the enumeration is unfalsifiable by
 * inspection. A configuration read of a function that is not there returns all
 * ones rather than failing, and so does a read composed with the bus, device and
 * function fields shifted into the wrong positions: an enumerator with its
 * address arithmetic wrong finds nothing at all and reports an empty bus, which
 * is indistinguishable from a machine that has no devices. The test therefore
 * asserts that specific things were found and that the accessors agree with one
 * another, rather than that the enumeration completed.
 */
void KernelVerifyPci(void)
{
    static const PciAddress host = { 0U, 0U, 0U };
    static const PciAddress absent = { 255U, 31U, 7U };
    const PciFunction *entry;
    uint32_t identifiers;
    size_t found_at = 0U;
    bool succeeded = true;

    if (!PciMechanismOnePresent())
    {
        KernelWriteString("  Configuration mechanism one did not answer.\n");
        KernelWriteString("Bus self-test FAILED.\n");
        return;
    }

    /* An address nothing decodes must read as all ones, not as a device. */
    if (PciReadConfiguration32(absent, PCI_OFFSET_VENDOR_ID) != UINT32_C(0xFFFFFFFF))
    {
        KernelWriteString("  An absent function did not read as all ones.\n");
        succeeded = false;
    }

    /*
     * The narrow accessors extract their field from the double word containing
     * it. A shift taken from the wrong bits of the offset would yield a
     * plausible number rather than an obviously wrong one, so the halves are
     * compared against the whole.
     */
    identifiers = PciReadConfiguration32(host, PCI_OFFSET_VENDOR_ID);

    if ((PciReadConfiguration16(host, PCI_OFFSET_VENDOR_ID) !=
         (uint16_t)(identifiers & 0xFFFFU)) ||
        (PciReadConfiguration16(host, PCI_OFFSET_DEVICE_ID) !=
         (uint16_t)(identifiers >> 16)) ||
        (PciReadConfiguration8(host, PCI_OFFSET_VENDOR_ID) != (uint8_t)(identifiers & 0xFFU)))
    {
        KernelWriteString("  The narrow accessors disagree with the wide one.\n");
        succeeded = false;
    }

    /* Something must have been found, and the first bus must have been scanned. */
    if ((PciFunctionCount() == 0U) || (PciBusesScanned() == 0U))
    {
        KernelWriteString("  The enumeration found nothing at all.\n");
        succeeded = false;
    }

    if (PciFunctionsDiscarded() != 0U)
    {
        KernelWriteString("  More functions answered than the table holds.\n");
        succeeded = false;
    }

    /*
     * Every machine this kernel runs upon presents a host bridge at the first
     * address of the first bus. Its absence means the enumeration is reading
     * somewhere other than where it believes.
     */
    entry = PciFunctionAt(0U);

    if ((entry == NULL) || (entry->address.bus != 0U) || (entry->address.device != 0U) ||
        (entry->address.function != 0U) || (entry->class_code != PCI_CLASS_BRIDGE) ||
        (entry->subclass != PCI_SUBCLASS_HOST_BRIDGE))
    {
        KernelWriteString("  No host bridge stands at the root of the bus.\n");
        succeeded = false;
    }

    /* The index is bounded, and the search finds what the table holds. */
    if (PciFunctionAt(PciFunctionCount()) != NULL)
    {
        KernelWriteString("  A function was reported beyond the end of the table.\n");
        succeeded = false;
    }

    if ((entry != NULL) &&
        (PciFindByIdentifier(entry->vendor_id, entry->device_id) == NULL))
    {
        KernelWriteString("  A recorded function was not found by its identifiers.\n");
        succeeded = false;
    }

    if (PciFindByClass(PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST_BRIDGE, 0U, &found_at) == NULL)
    {
        KernelWriteString("  The host bridge was not found by its class.\n");
        succeeded = false;
    }

    if (PciFindByClass(0xFFU, 0xFFU, PciFunctionCount(), NULL) != NULL)
    {
        KernelWriteString("  A search beginning past the table returned a function.\n");
        succeeded = false;
    }

    /*
     * Every function recorded must be a function that answered, and every base
     * address must have had its type and attribute bits removed. A base address
     * still carrying them would be a port number or an address off by up to
     * fifteen, which addresses hardware that is nearly right.
     */
    for (size_t index = 0U; index < PciFunctionCount(); ++index)
    {
        const PciFunction *const current = PciFunctionAt(index);

        if ((current == NULL) || (current->vendor_id == PCI_VENDOR_INVALID))
        {
            KernelWriteString("  A function was recorded that did not answer.\n");
            succeeded = false;
            break;
        }

        for (size_t bar = 0U; bar < PCI_BAR_COUNT; ++bar)
        {
            const uint64_t base = PciBarBase(current, bar);
            const uint64_t alignment = PciBarIsIoPort(current, bar) ? 3U : 15U;

            if ((base & alignment) != 0U)
            {
                KernelWriteString("  A base address retains its type bits.\n");
                succeeded = false;
                break;
            }
        }
    }

    KernelWriteString(succeeded ? "Bus self-test passed.\n" : "Bus self-test FAILED.\n");
}
