/*
 * File: drivers/serial/serial.c
 * Purpose: Implements the interrupt-driven driver for the 16550 compatible
 *          universal asynchronous receiver/transmitter, which carries the
 *          diagnostic channel that is independent of the display hardware and
 *          that a virtual machine monitor or a null-modem cable may capture. It
 *          configures the line parameters, buffers both directions, services the
 *          adapter's four interrupt sources, and retains a polled path for the
 *          circumstances in which no interrupt can be delivered.
 * Key functions: SerialInitialise, SerialConfigure, SerialActivateInterrupts,
 *          SerialPutCharacter, SerialWriteString, SerialFlush,
 *          SerialReadCharacter, SerialLoopbackTest, SerialHandleInterrupt,
 *          SerialReport.
 * References:
 *   - National Semiconductor PC16550D datasheet, Table 2 ("Register
 *     Addresses"): the receiver and transmitter buffer registers lie at offset
 *     0, the interrupt enable register at offset 1, the interrupt
 *     identification register (read) and first-in-first-out control register
 *     (write) at offset 2, the line control register at offset 3, the modem
 *     control register at offset 4, the line status register at offset 5, the
 *     modem status register at offset 6 and the scratch register at offset 7;
 *     while the divisor latch access bit is set, offsets 0 and 1 address the
 *     divisor latches instead.
 *   - PC16550D datasheet, Table 1 ("Summary of Registers"): the bit assignments
 *     of every register named above.
 *   - PC16550D datasheet, Table 5 ("Interrupt Control Functions"): bit 0 of the
 *     interrupt identification register is clear while an interrupt is pending;
 *     bits 3 to 1 identify the highest-priority pending source, being 011 the
 *     receiver line status, 010 received data available, 110 the character
 *     timeout, 001 the transmitter holding register empty and 000 the modem
 *     status; and each source is reset by a stated action, the transmitter
 *     holding register empty interrupt by reading that register or by writing
 *     the transmitter holding register.
 *   - PC16550D datasheet, Section "Line Control Register": bits 1 and 0 select
 *     a word length of five to eight bits, bit 2 the number of stop bits, bits 5
 *     to 3 the parity, and bit 7 the divisor latch access bit.
 *   - PC16550D datasheet, Section "Line Status Register": bit 0 indicates a
 *     received character, bits 1 to 4 the overrun, parity, framing and break
 *     conditions, bit 5 that the transmitter holding register is empty and bit 6
 *     that the transmitter is wholly idle.
 *   - PC16550D datasheet, Section "Programmable Baud Generator": the divisor is
 *     the reference oscillator frequency divided by sixteen times the desired
 *     signalling rate.
 *   - PC16550D datasheet, Section "FIFO Interrupt Mode Operation": the
 *     transmitter first-in-first-out buffer holds sixteen characters, so that
 *     number may be written for each transmitter interrupt.
 *   - PC16550D datasheet, Section "MODEM Control Register": bit 3 controls the
 *     auxiliary output OUT2 and bit 4 places the adapter in local loopback.
 *   - IBM Personal Computer AT technical reference: the first serial adapter is
 *     decoded at I/O base address 0x03F8 and its interrupt line is gated onto
 *     IRQ4 by OUT2, which must therefore be asserted before any interrupt of the
 *     adapter reaches the interrupt controller; the reference oscillator is
 *     1.8432 MHz.
 *   - Intel SDM, Volume 1, Section 3.4.3: bit 9 of RFLAGS is the interrupt
 *     enable flag, read here through ReadRflags in order to decide whether a
 *     wait for an interrupt could ever end.
 */

#include <oxys/serial.h>
#include <oxys/kernel.h>
#include <oxys/io.h>
#include <oxys/cpu.h>
#include <oxys/pic.h>
#include <oxys/interrupts.h>

/* Register offsets from the adapter's I/O base address. */
#define SERIAL_REGISTER_DATA                  0U /* Receiver and transmitter buffers. */
#define SERIAL_REGISTER_INTERRUPT_ENABLE      1U
#define SERIAL_REGISTER_INTERRUPT_IDENTIFY    2U /* Read. */
#define SERIAL_REGISTER_FIFO_CONTROL          2U /* Write. */
#define SERIAL_REGISTER_LINE_CONTROL          3U
#define SERIAL_REGISTER_MODEM_CONTROL         4U
#define SERIAL_REGISTER_LINE_STATUS           5U
#define SERIAL_REGISTER_MODEM_STATUS          6U
#define SERIAL_REGISTER_SCRATCH               7U

/* The divisor latches, accessible only while the divisor latch access bit is set. */
#define SERIAL_REGISTER_DIVISOR_LOW           0U
#define SERIAL_REGISTER_DIVISOR_HIGH          1U

/* Line control register bits. */
#define SERIAL_LINE_CONTROL_WORD_LENGTH_MASK  UINT8_C(0x03)
#define SERIAL_LINE_CONTROL_TWO_STOP_BITS     UINT8_C(0x04)
#define SERIAL_LINE_CONTROL_PARITY_ENABLE     UINT8_C(0x08)
#define SERIAL_LINE_CONTROL_EVEN_PARITY       UINT8_C(0x10)
#define SERIAL_LINE_CONTROL_STICK_PARITY      UINT8_C(0x20)
#define SERIAL_LINE_CONTROL_DIVISOR_LATCH     UINT8_C(0x80)

/* Line status register bits. */
#define SERIAL_LINE_STATUS_DATA_READY         UINT8_C(0x01)
#define SERIAL_LINE_STATUS_OVERRUN_ERROR      UINT8_C(0x02)
#define SERIAL_LINE_STATUS_PARITY_ERROR       UINT8_C(0x04)
#define SERIAL_LINE_STATUS_FRAMING_ERROR      UINT8_C(0x08)
#define SERIAL_LINE_STATUS_BREAK_INTERRUPT    UINT8_C(0x10)
#define SERIAL_LINE_STATUS_TRANSMITTER_EMPTY  UINT8_C(0x20)
#define SERIAL_LINE_STATUS_TRANSMITTER_IDLE   UINT8_C(0x40)

/* The four conditions the receiver line status interrupt reports. */
#define SERIAL_LINE_STATUS_ERROR_MASK                                          \
    (SERIAL_LINE_STATUS_OVERRUN_ERROR | SERIAL_LINE_STATUS_PARITY_ERROR |      \
     SERIAL_LINE_STATUS_FRAMING_ERROR | SERIAL_LINE_STATUS_BREAK_INTERRUPT)

/* Interrupt enable register bits. */
#define SERIAL_INTERRUPT_ENABLE_RECEIVED_DATA UINT8_C(0x01)
#define SERIAL_INTERRUPT_ENABLE_TRANSMITTER   UINT8_C(0x02)
#define SERIAL_INTERRUPT_ENABLE_LINE_STATUS   UINT8_C(0x04)
#define SERIAL_INTERRUPT_ENABLE_MODEM_STATUS  UINT8_C(0x08)

/* Interrupt identification register fields. */
#define SERIAL_INTERRUPT_NOT_PENDING          UINT8_C(0x01)
#define SERIAL_INTERRUPT_IDENTITY_MASK        UINT8_C(0x0E)
#define SERIAL_INTERRUPT_MODEM_STATUS         UINT8_C(0x00)
#define SERIAL_INTERRUPT_TRANSMITTER_EMPTY    UINT8_C(0x02)
#define SERIAL_INTERRUPT_RECEIVED_DATA        UINT8_C(0x04)
#define SERIAL_INTERRUPT_LINE_STATUS          UINT8_C(0x06)
#define SERIAL_INTERRUPT_CHARACTER_TIMEOUT    UINT8_C(0x0C)

/* Modem control register bits. */
#define SERIAL_MODEM_CONTROL_DATA_TERMINAL_READY  UINT8_C(0x01)
#define SERIAL_MODEM_CONTROL_REQUEST_TO_SEND      UINT8_C(0x02)
#define SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO UINT8_C(0x08)
#define SERIAL_MODEM_CONTROL_LOOPBACK             UINT8_C(0x10)

/* First-in-first-out control register bits. */
#define SERIAL_FIFO_CONTROL_ENABLE            UINT8_C(0x01)
#define SERIAL_FIFO_CONTROL_CLEAR_RECEIVE     UINT8_C(0x02)
#define SERIAL_FIFO_CONTROL_CLEAR_TRANSMIT    UINT8_C(0x04)
#define SERIAL_FIFO_CONTROL_TRIGGER_FOURTEEN  UINT8_C(0xC0)

/*
 * The depth of the transmitter first-in-first-out buffer, and therefore the
 * number of characters that may be written for each transmitter interrupt. A
 * larger number would be discarded by the adapter without notice.
 */
#define SERIAL_TRANSMIT_FIFO_DEPTH            16U

/* An arbitrary value transmitted during the loopback presence test. */
#define SERIAL_LOOPBACK_TEST_BYTE             UINT8_C(0xAE)

/* The default line parameters, being those of the Phase 1 polled routine. */
#define SERIAL_DEFAULT_BAUD_RATE              UINT32_C(115200)
#define SERIAL_DEFAULT_DATA_BITS              UINT8_C(8)

/*
 * The number of iterations a polled wait upon the transmitter will perform
 * before abandoning the character. An adapter that answered the loopback test
 * and has since ceased to empty its transmitter would otherwise hang the machine
 * in a routine whose entire purpose is to report that something has gone wrong.
 * The bound is generous: at the lowest rate the driver will accept, one
 * character occupies some tens of thousands of port reads.
 */
#define SERIAL_POLL_LIMIT                     UINT32_C(10000000)

/*
 * The capacities of the two buffers. The transmit buffer is the larger because
 * the boot-time reporting produces several kilobytes in bursts far faster than
 * the line can carry them, and a caller that fills it must wait; the receive
 * buffer need only cover the interval between one reader and the next.
 */
#define SERIAL_TRANSMIT_BUFFER_CAPACITY       4096U
#define SERIAL_RECEIVE_BUFFER_CAPACITY        256U

/*
 * Both capacities must be powers of two, an index being reduced to a subscript
 * by a bitwise mask. The requirement is load-bearing rather than an
 * optimisation: any other capacity would leave the mask addressing only the
 * first power of two below it, silently corrupting the buffer. The assertions
 * convert that into a build failure.
 */
_Static_assert((SERIAL_TRANSMIT_BUFFER_CAPACITY &
                (SERIAL_TRANSMIT_BUFFER_CAPACITY - 1U)) == 0U,
               "The serial transmit buffer capacity must be a power of two.");
_Static_assert((SERIAL_RECEIVE_BUFFER_CAPACITY &
                (SERIAL_RECEIVE_BUFFER_CAPACITY - 1U)) == 0U,
               "The serial receive buffer capacity must be a power of two.");

/*
 * The base address of the adapter in use. It is zero until SerialInitialise has
 * succeeded, and every output function treats zero as an instruction to discard
 * its argument, so that a system without a serial adapter proceeds unimpeded.
 */
static uint16_t SerialActivePort;

/* The line parameters in force, and the divisor by which they were realised. */
static SerialConfiguration SerialActiveConfiguration;
static uint16_t SerialActiveDivisor;

/* True once the request line has been claimed and unmasked. */
static bool SerialInterruptsEnabled;

/*
 * The interrupt enable register is write-only in effect, the adapter offering no
 * means of reading back what was written that is worth relying upon, so its
 * value is retained here. The transmitter bit is set only while characters are
 * waiting, for the reason given in SerialStartTransmission.
 */
static uint8_t SerialInterruptEnableShadow;

/*
 * The two buffers. Each is a single-producer, single-consumer queue: for the
 * transmit buffer the producer is the writing code and the consumer the
 * interrupt handler, and for the receive buffer the reverse. Neither therefore
 * requires a lock upon a machine of one processor.
 *
 * The indices are not wrapped to the capacity; they increase without bound and
 * are masked when used, so that their difference is the occupancy directly and
 * the arithmetic remains correct across the wrap of the index itself. This is
 * the discipline of drivers/keyboard/keyboard.c, and is described there.
 */
static char SerialTransmitBuffer[SERIAL_TRANSMIT_BUFFER_CAPACITY];
static volatile uint32_t SerialTransmitWriteIndex;
static volatile uint32_t SerialTransmitReadIndex;

static char SerialReceiveBuffer[SERIAL_RECEIVE_BUFFER_CAPACITY];
static volatile uint32_t SerialReceiveWriteIndex;
static volatile uint32_t SerialReceiveReadIndex;

/* Accounting. */
static uint64_t SerialTransmitted;
static uint64_t SerialReceived;
static uint64_t SerialInterrupts;
static uint64_t SerialLineErrors;
static uint64_t SerialReceiveOverruns;
static uint64_t SerialTransmitWaits;

/* The last line status in which an error was reported, retained for diagnosis. */
static uint8_t SerialLastErrorStatus;

/*
 * Reads a register of the adapter in use.
 */
static uint8_t SerialRead(uint16_t offset)
{
    return PortReadByte((uint16_t)(SerialActivePort + offset));
}

/*
 * Writes a register of the adapter in use.
 */
static void SerialWrite(uint16_t offset, uint8_t value)
{
    PortWriteByte((uint16_t)(SerialActivePort + offset), value);
}

/*
 * Composes the line control register value denoting a set of line parameters,
 * or reports that the parameters cannot be expressed.
 *
 * The parity encoding is not a plain enumeration in the register: bit 3 enables
 * parity at all, bit 4 selects even rather than odd, and bit 5, the stick
 * parity bit, replaces the computed parity with the complement of bit 4, which
 * is how the mark and space schemes are obtained.
 */
static bool SerialComposeLineControl(const SerialConfiguration *configuration,
                                     uint8_t *line_control)
{
    uint8_t value;

    if ((configuration->data_bits < 5U) || (configuration->data_bits > 8U))
    {
        return false;
    }

    value = (uint8_t)((configuration->data_bits - 5U) &
                      SERIAL_LINE_CONTROL_WORD_LENGTH_MASK);

    if (configuration->stop_bits == SERIAL_STOP_BITS_TWO)
    {
        value = (uint8_t)(value | SERIAL_LINE_CONTROL_TWO_STOP_BITS);
    }

    switch (configuration->parity)
    {
    case SERIAL_PARITY_NONE:
        break;

    case SERIAL_PARITY_ODD:
        value = (uint8_t)(value | SERIAL_LINE_CONTROL_PARITY_ENABLE);
        break;

    case SERIAL_PARITY_EVEN:
        value = (uint8_t)(value | SERIAL_LINE_CONTROL_PARITY_ENABLE |
                          SERIAL_LINE_CONTROL_EVEN_PARITY);
        break;

    case SERIAL_PARITY_MARK:
        value = (uint8_t)(value | SERIAL_LINE_CONTROL_PARITY_ENABLE |
                          SERIAL_LINE_CONTROL_STICK_PARITY);
        break;

    case SERIAL_PARITY_SPACE:
        value = (uint8_t)(value | SERIAL_LINE_CONTROL_PARITY_ENABLE |
                          SERIAL_LINE_CONTROL_EVEN_PARITY |
                          SERIAL_LINE_CONTROL_STICK_PARITY);
        break;

    default:
        return false;
    }

    *line_control = value;

    return true;
}

/*
 * Determines the divisor realising a signalling rate, or reports that no divisor
 * does. The division is truncating, so the rate obtained is the oscillator
 * divided by the divisor rather than the rate requested; the two are reported
 * separately by SerialReport, an error of a known size that does not announce
 * itself being worse than a coarse one that does.
 */
static bool SerialComposeDivisor(uint32_t baud_rate, uint16_t *divisor)
{
    uint32_t value;

    if ((baud_rate == 0U) || (baud_rate > SERIAL_MAXIMUM_BAUD_RATE))
    {
        return false;
    }

    value = SERIAL_MAXIMUM_BAUD_RATE / baud_rate;

    if ((value == 0U) || (value > UINT32_C(0xFFFF)))
    {
        return false;
    }

    *divisor = (uint16_t)value;

    return true;
}

/*
 * Writes the line control register and the divisor latches, which is the whole
 * of the line configuration. The divisor latch access bit must be set to reach
 * the latches and cleared afterwards, because it overlays the two registers the
 * driver uses most.
 */
static void SerialApplyLineParameters(uint8_t line_control, uint16_t divisor)
{
    SerialWrite(SERIAL_REGISTER_LINE_CONTROL,
                (uint8_t)(line_control | SERIAL_LINE_CONTROL_DIVISOR_LATCH));
    SerialWrite(SERIAL_REGISTER_DIVISOR_LOW, (uint8_t)(divisor & 0x00FFU));
    SerialWrite(SERIAL_REGISTER_DIVISOR_HIGH, (uint8_t)((divisor >> 8) & 0x00FFU));
    SerialWrite(SERIAL_REGISTER_LINE_CONTROL, line_control);
}

/* The number of characters presently queued for transmission. */
static uint32_t SerialTransmitOccupancy(void)
{
    return (uint32_t)(SerialTransmitWriteIndex - SerialTransmitReadIndex);
}

/* The number of characters presently waiting to be read. */
static uint32_t SerialReceiveOccupancy(void)
{
    return (uint32_t)(SerialReceiveWriteIndex - SerialReceiveReadIndex);
}

/*
 * Requests that the adapter interrupt when its transmitter holding register
 * falls empty.
 *
 * The bit is set only while characters are waiting, and the handler clears it
 * upon draining the last of them. The condition the interrupt reports is a level
 * rather than an event: an adapter with nothing to send has an empty transmitter
 * holding register permanently, so leaving the bit set would present an
 * interrupt that no service could dismiss, and the machine would make no further
 * progress. This is the principal hazard of driving a 16550 by interrupt, and
 * the reason the enable register is shadowed rather than written blindly.
 */
static void SerialStartTransmission(void)
{
    if ((SerialInterruptEnableShadow & SERIAL_INTERRUPT_ENABLE_TRANSMITTER) != 0U)
    {
        return;
    }

    SerialInterruptEnableShadow =
        (uint8_t)(SerialInterruptEnableShadow | SERIAL_INTERRUPT_ENABLE_TRANSMITTER);
    SerialWrite(SERIAL_REGISTER_INTERRUPT_ENABLE, SerialInterruptEnableShadow);
}

/* Withdraws that request, there being nothing further to send. */
static void SerialStopTransmission(void)
{
    if ((SerialInterruptEnableShadow & SERIAL_INTERRUPT_ENABLE_TRANSMITTER) == 0U)
    {
        return;
    }

    SerialInterruptEnableShadow =
        (uint8_t)(SerialInterruptEnableShadow & (uint8_t)~SERIAL_INTERRUPT_ENABLE_TRANSMITTER);
    SerialWrite(SERIAL_REGISTER_INTERRUPT_ENABLE, SerialInterruptEnableShadow);
}

/*
 * Waits, by polling, until the transmitter holding register is empty. Returns
 * false if the bound is exhausted, which denotes an adapter that has stopped
 * transmitting and from which nothing further should be expected.
 */
static bool SerialWaitForTransmitterEmpty(void)
{
    for (uint32_t iteration = 0U; iteration < SERIAL_POLL_LIMIT; ++iteration)
    {
        if ((SerialRead(SERIAL_REGISTER_LINE_STATUS) &
             SERIAL_LINE_STATUS_TRANSMITTER_EMPTY) != 0U)
        {
            return true;
        }
    }

    return false;
}

/*
 * Presents one character to the line, waiting for the transmitter. This is the
 * path of the polled mode and of every circumstance in which an interrupt could
 * not arrive.
 */
static void SerialTransmitPolled(char character)
{
    if (!SerialWaitForTransmitterEmpty())
    {
        return;
    }

    SerialWrite(SERIAL_REGISTER_DATA, (uint8_t)(unsigned char)character);
    ++SerialTransmitted;
}

/*
 * Empties the transmit buffer by polling. It is used when the buffer must be
 * drained but no interrupt can be relied upon to drain it: before the request
 * line is claimed, within a panic, and wherever the interrupt flag is clear.
 *
 * The transmitter interrupt is withdrawn first. Were it left enabled, the
 * handler and this loop would both consume the buffer, and the two would
 * interleave their characters.
 */
static void SerialDrainPolled(void)
{
    SerialStopTransmission();

    while (SerialTransmitOccupancy() != 0U)
    {
        const char character =
            SerialTransmitBuffer[SerialTransmitReadIndex &
                                 (SERIAL_TRANSMIT_BUFFER_CAPACITY - 1U)];

        SerialTransmitPolled(character);
        ++SerialTransmitReadIndex;
    }
}

/*
 * True if the buffered path may be used: the request line is claimed and the
 * processor is accepting interrupts. Where it is not, the character must go to
 * the line directly, because nothing else would ever carry it.
 */
static bool SerialBufferedPathIsUsable(void)
{
    return SerialInterruptsEnabled && InterruptsAreEnabled();
}

/*
 * Places one character in the transmit buffer, waiting for room if the buffer is
 * full. A diagnostic channel that discarded its output would be worse than a
 * slow one, so the wait is unbounded in characters, though each individual
 * character is bounded by SERIAL_POLL_LIMIT within the polled path.
 */
static void SerialEnqueue(char character)
{
    if (SerialTransmitOccupancy() >= SERIAL_TRANSMIT_BUFFER_CAPACITY)
    {
        /* Counted once for each character that had to wait, not once for each
         * iteration of the loop below, which would say only how fast the
         * processor spins. */
        ++SerialTransmitWaits;
    }

    while (SerialTransmitOccupancy() >= SERIAL_TRANSMIT_BUFFER_CAPACITY)
    {
        /*
         * The handler empties the buffer as the line permits. Should the
         * interrupt flag have been cleared since the occupancy was read, no
         * handler will run and the wait would not end, so the remaining
         * characters are carried by polling instead.
         */
        if (!SerialBufferedPathIsUsable())
        {
            SerialDrainPolled();
            break;
        }
    }

    SerialTransmitBuffer[SerialTransmitWriteIndex &
                         (SERIAL_TRANSMIT_BUFFER_CAPACITY - 1U)] = character;
    ++SerialTransmitWriteIndex;
}

/*
 * Transmits one character by whichever path is available, preserving the order
 * in which characters were written. A character carried by polling while others
 * remain queued would overtake them, so the queue is drained first.
 */
static void SerialEmit(char character)
{
    if (SerialBufferedPathIsUsable())
    {
        SerialEnqueue(character);
        SerialStartTransmission();
        return;
    }

    SerialDrainPolled();
    SerialTransmitPolled(character);
}

/*
 * Reads every character the adapter has received into the receive buffer. An
 * overrun of that buffer discards the newest character and is counted; the
 * adapter is drained regardless, because a character left in it would present
 * the same interrupt again and the handler would not terminate.
 */
static void SerialReceiveAvailable(void)
{
    while ((SerialRead(SERIAL_REGISTER_LINE_STATUS) & SERIAL_LINE_STATUS_DATA_READY) != 0U)
    {
        const char character = (char)SerialRead(SERIAL_REGISTER_DATA);

        ++SerialReceived;

        if (SerialReceiveOccupancy() >= SERIAL_RECEIVE_BUFFER_CAPACITY)
        {
            ++SerialReceiveOverruns;
            continue;
        }

        SerialReceiveBuffer[SerialReceiveWriteIndex &
                            (SERIAL_RECEIVE_BUFFER_CAPACITY - 1U)] = character;
        ++SerialReceiveWriteIndex;
    }
}

/*
 * Writes as much of the transmit buffer as the transmitter first-in-first-out
 * buffer will hold. The count is bounded by the depth of that buffer because the
 * adapter reports it empty when it has room for a full complement, not for one
 * character, and anything written beyond the sixteenth would be discarded
 * without notice.
 */
static void SerialTransmitAvailable(void)
{
    uint32_t written = 0U;

    while ((written < SERIAL_TRANSMIT_FIFO_DEPTH) && (SerialTransmitOccupancy() != 0U))
    {
        SerialWrite(SERIAL_REGISTER_DATA,
                    (uint8_t)(unsigned char)
                        SerialTransmitBuffer[SerialTransmitReadIndex &
                                             (SERIAL_TRANSMIT_BUFFER_CAPACITY - 1U)]);
        ++SerialTransmitReadIndex;
        ++SerialTransmitted;
        ++written;
    }

    if (SerialTransmitOccupancy() == 0U)
    {
        SerialStopTransmission();
    }
}

/*
 * Services the adapter. The frame is unused: nothing here alters the state to
 * which control returns.
 *
 * The identification register is read repeatedly until it reports no interrupt
 * pending, because the adapter presents its sources one at a time in order of
 * priority and a single pass would leave the others asserted. The request line
 * is level-sensitive as the interrupt controller sees it, so an unserviced
 * source would raise the request again the moment the end-of-interrupt was
 * signalled.
 */
static void SerialHandleInterrupt(TrapFrame *frame)
{
    (void)frame;

    ++SerialInterrupts;

    for (;;)
    {
        const uint8_t identification = SerialRead(SERIAL_REGISTER_INTERRUPT_IDENTIFY);

        if ((identification & SERIAL_INTERRUPT_NOT_PENDING) != 0U)
        {
            break;
        }

        switch (identification & SERIAL_INTERRUPT_IDENTITY_MASK)
        {
        case SERIAL_INTERRUPT_LINE_STATUS:
        {
            /*
             * Reading the line status register is what resets this interrupt,
             * so the read is required and not merely diagnostic.
             */
            const uint8_t status = SerialRead(SERIAL_REGISTER_LINE_STATUS);

            if ((status & SERIAL_LINE_STATUS_ERROR_MASK) != 0U)
            {
                ++SerialLineErrors;
                SerialLastErrorStatus = status;
            }

            break;
        }

        case SERIAL_INTERRUPT_RECEIVED_DATA:
        case SERIAL_INTERRUPT_CHARACTER_TIMEOUT:
            /*
             * The timeout reports characters that have been waiting without the
             * trigger level being reached. It is serviced identically: in both
             * cases the remedy is to empty the receiver.
             */
            SerialReceiveAvailable();
            break;

        case SERIAL_INTERRUPT_TRANSMITTER_EMPTY:
            SerialTransmitAvailable();
            break;

        case SERIAL_INTERRUPT_MODEM_STATUS:
            /*
             * The modem status interrupt is not enabled, and this arm exists
             * only so that a spurious one is dismissed rather than repeated
             * without end. Reading the register is what resets it.
             */
            (void)SerialRead(SERIAL_REGISTER_MODEM_STATUS);
            break;

        default:
            /*
             * No other value is defined. The loop must not be left to spin upon
             * one, so the identification is treated as dismissed.
             */
            return;
        }
    }
}

bool SerialConfigure(const SerialConfiguration *configuration)
{
    uint8_t line_control;
    uint16_t divisor;

    if ((SerialActivePort == 0U) || (configuration == NULL))
    {
        return false;
    }

    if (!SerialComposeLineControl(configuration, &line_control) ||
        !SerialComposeDivisor(configuration->baud_rate, &divisor))
    {
        return false;
    }

    /*
     * Anything already queued was composed for the parameters presently in
     * force, and would be received as noise were the line altered beneath it.
     */
    SerialFlush();

    SerialApplyLineParameters(line_control, divisor);

    SerialActiveConfiguration = *configuration;
    SerialActiveDivisor = divisor;

    return true;
}

bool SerialInitialise(uint16_t port)
{
    static const SerialConfiguration default_configuration = {
        SERIAL_DEFAULT_BAUD_RATE,
        SERIAL_DEFAULT_DATA_BITS,
        SERIAL_PARITY_NONE,
        SERIAL_STOP_BITS_ONE
    };

    uint8_t line_control;
    uint16_t divisor;
    uint8_t received_byte;

    SerialActivePort = 0U;
    SerialInterruptsEnabled = false;
    SerialInterruptEnableShadow = 0U;
    SerialTransmitWriteIndex = 0U;
    SerialTransmitReadIndex = 0U;
    SerialReceiveWriteIndex = 0U;
    SerialReceiveReadIndex = 0U;

    /*
     * The port is adopted before the registers are touched, the accessors being
     * expressed in terms of it. It is withdrawn again should the adapter fail to
     * answer.
     */
    SerialActivePort = port;

    /* Mask every adapter interrupt: the driver begins in the polled mode. */
    SerialWrite(SERIAL_REGISTER_INTERRUPT_ENABLE, 0x00U);

    if (!SerialComposeLineControl(&default_configuration, &line_control) ||
        !SerialComposeDivisor(default_configuration.baud_rate, &divisor))
    {
        SerialActivePort = 0U;
        return false;
    }

    SerialApplyLineParameters(line_control, divisor);

    /*
     * Enable and clear both first-in-first-out buffers, with the receiver
     * trigger level at fourteen characters. The character timeout interrupt
     * covers the characters that arrive without completing a trigger, so the
     * high level costs no latency that matters while sparing the machine an
     * interrupt for each character of a burst.
     */
    SerialWrite(SERIAL_REGISTER_FIFO_CONTROL,
                (uint8_t)(SERIAL_FIFO_CONTROL_ENABLE |
                          SERIAL_FIFO_CONTROL_CLEAR_RECEIVE |
                          SERIAL_FIFO_CONTROL_CLEAR_TRANSMIT |
                          SERIAL_FIFO_CONTROL_TRIGGER_FOURTEEN));

    /*
     * Determine whether an adapter is present by placing the device in local
     * loopback mode and confirming that a transmitted byte is received again.
     */
    SerialWrite(SERIAL_REGISTER_MODEM_CONTROL,
                (uint8_t)(SERIAL_MODEM_CONTROL_LOOPBACK |
                          SERIAL_MODEM_CONTROL_REQUEST_TO_SEND |
                          SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO));
    SerialWrite(SERIAL_REGISTER_DATA, SERIAL_LOOPBACK_TEST_BYTE);
    received_byte = SerialRead(SERIAL_REGISTER_DATA);

    if (received_byte != SERIAL_LOOPBACK_TEST_BYTE)
    {
        SerialActivePort = 0U;
        return false;
    }

    /*
     * Leave loopback mode and assert data terminal ready, request to send and
     * the second auxiliary output. The last is not a courtesy: upon the PC/AT
     * the adapter's interrupt line reaches IRQ4 through a buffer that OUT2
     * enables, so an adapter whose OUT2 is clear can be configured to interrupt
     * and will never be heard.
     */
    SerialWrite(SERIAL_REGISTER_MODEM_CONTROL,
                (uint8_t)(SERIAL_MODEM_CONTROL_DATA_TERMINAL_READY |
                          SERIAL_MODEM_CONTROL_REQUEST_TO_SEND |
                          SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO));

    /* Discard anything the firmware left in the receiver. */
    while ((SerialRead(SERIAL_REGISTER_LINE_STATUS) & SERIAL_LINE_STATUS_DATA_READY) != 0U)
    {
        (void)SerialRead(SERIAL_REGISTER_DATA);
    }

    SerialActiveConfiguration = default_configuration;
    SerialActiveDivisor = divisor;

    return true;
}

void SerialActivateInterrupts(void)
{
    if ((SerialActivePort == 0U) || SerialInterruptsEnabled)
    {
        return;
    }

    /*
     * The receiver and the line status are enabled permanently; the transmitter
     * is enabled only while characters are waiting, for the reason given in
     * SerialStartTransmission. The modem status is left disabled, no use being
     * made of the modem control signals.
     */
    SerialInterruptEnableShadow = (uint8_t)(SERIAL_INTERRUPT_ENABLE_RECEIVED_DATA |
                                            SERIAL_INTERRUPT_ENABLE_LINE_STATUS);

    PicInstallHandler(SERIAL_COM1_IRQ, SerialHandleInterrupt, "16550 serial adapter");
    SerialWrite(SERIAL_REGISTER_INTERRUPT_ENABLE, SerialInterruptEnableShadow);
    PicUnmaskLine(SERIAL_COM1_IRQ);

    SerialInterruptsEnabled = true;
}

bool SerialIsPresent(void)
{
    return SerialActivePort != 0U;
}

bool SerialInterruptsActive(void)
{
    return SerialInterruptsEnabled;
}

bool SerialTransmitInterruptEnabled(void)
{
    return (SerialInterruptEnableShadow & SERIAL_INTERRUPT_ENABLE_TRANSMITTER) != 0U;
}

/*
 * Waits, by polling, until a received character is available. Returns false if
 * the bound is exhausted, which in loopback denotes a character that was
 * transmitted and never returned.
 */
static bool SerialWaitForReceivedData(void)
{
    for (uint32_t iteration = 0U; iteration < SERIAL_POLL_LIMIT; ++iteration)
    {
        if ((SerialRead(SERIAL_REGISTER_LINE_STATUS) & SERIAL_LINE_STATUS_DATA_READY) != 0U)
        {
            return true;
        }
    }

    return false;
}

bool SerialLoopbackTest(void)
{
    /*
     * The sequence is asymmetric and contains no repeated character, so that a
     * reversal or a duplication is distinguishable from a correct result.
     */
    static const char pattern[] = { 'O', 'x', 'y', 's', '-', 'O', 'S', '\x01' };

    uint8_t saved_interrupt_enable;
    bool succeeded = true;

    if (SerialActivePort == 0U)
    {
        return false;
    }

    /* Anything queued belongs upon the line, not in the loopback. */
    SerialFlush();

    saved_interrupt_enable = SerialInterruptEnableShadow;
    SerialWrite(SERIAL_REGISTER_INTERRUPT_ENABLE, 0x00U);

    SerialWrite(SERIAL_REGISTER_MODEM_CONTROL,
                (uint8_t)(SERIAL_MODEM_CONTROL_LOOPBACK |
                          SERIAL_MODEM_CONTROL_DATA_TERMINAL_READY |
                          SERIAL_MODEM_CONTROL_REQUEST_TO_SEND |
                          SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO));
    SerialWrite(SERIAL_REGISTER_FIFO_CONTROL,
                (uint8_t)(SERIAL_FIFO_CONTROL_ENABLE |
                          SERIAL_FIFO_CONTROL_CLEAR_RECEIVE |
                          SERIAL_FIFO_CONTROL_CLEAR_TRANSMIT |
                          SERIAL_FIFO_CONTROL_TRIGGER_FOURTEEN));

    for (size_t index = 0U; index < sizeof pattern; ++index)
    {
        if (!SerialWaitForTransmitterEmpty())
        {
            succeeded = false;
            break;
        }

        SerialWrite(SERIAL_REGISTER_DATA, (uint8_t)(unsigned char)pattern[index]);

        if (!SerialWaitForReceivedData() ||
            (SerialRead(SERIAL_REGISTER_DATA) != (uint8_t)(unsigned char)pattern[index]))
        {
            succeeded = false;
            break;
        }
    }

    /* Nothing beyond the sequence may have been manufactured by the adapter. */
    if ((SerialRead(SERIAL_REGISTER_LINE_STATUS) & SERIAL_LINE_STATUS_DATA_READY) != 0U)
    {
        succeeded = false;
    }

    SerialWrite(SERIAL_REGISTER_MODEM_CONTROL,
                (uint8_t)(SERIAL_MODEM_CONTROL_DATA_TERMINAL_READY |
                          SERIAL_MODEM_CONTROL_REQUEST_TO_SEND |
                          SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO));
    SerialWrite(SERIAL_REGISTER_FIFO_CONTROL,
                (uint8_t)(SERIAL_FIFO_CONTROL_ENABLE |
                          SERIAL_FIFO_CONTROL_CLEAR_RECEIVE |
                          SERIAL_FIFO_CONTROL_CLEAR_TRANSMIT |
                          SERIAL_FIFO_CONTROL_TRIGGER_FOURTEEN));

    SerialInterruptEnableShadow = saved_interrupt_enable;
    SerialWrite(SERIAL_REGISTER_INTERRUPT_ENABLE, SerialInterruptEnableShadow);

    return succeeded;
}

void SerialPutCharacter(char character)
{
    if (SerialActivePort == 0U)
    {
        return;
    }

    /*
     * A line feed is preceded by a carriage return so that the output is legible
     * upon a receiving terminal that performs no such translation of its own.
     */
    if (character == '\n')
    {
        SerialEmit('\r');
    }

    SerialEmit(character);
}

void SerialWriteString(const char *string)
{
    if (string == NULL)
    {
        return;
    }

    for (size_t index = 0U; string[index] != '\0'; ++index)
    {
        SerialPutCharacter(string[index]);
    }
}

void SerialFlush(void)
{
    if (SerialActivePort == 0U)
    {
        return;
    }

    /*
     * Where interrupts are available the handler is left to empty the buffer,
     * since it does so sixteen characters at a time; where they are not, the
     * buffer is carried by polling, which is the only way it would ever leave.
     */
    while (SerialTransmitOccupancy() != 0U)
    {
        if (!SerialBufferedPathIsUsable())
        {
            SerialDrainPolled();
            break;
        }
    }

    /* The adapter itself still holds up to a full transmitter buffer. */
    (void)SerialWaitForTransmitterEmpty();
}

bool SerialHasInput(void)
{
    return (SerialActivePort != 0U) && (SerialReceiveOccupancy() != 0U);
}

bool SerialReadCharacter(char *character)
{
    if ((character == NULL) || !SerialHasInput())
    {
        return false;
    }

    *character =
        SerialReceiveBuffer[SerialReceiveReadIndex & (SERIAL_RECEIVE_BUFFER_CAPACITY - 1U)];
    ++SerialReceiveReadIndex;

    return true;
}

void SerialFlushBuffers(void)
{
    if (SerialActivePort == 0U)
    {
        return;
    }

    SerialStopTransmission();
    SerialTransmitReadIndex = SerialTransmitWriteIndex;
    SerialReceiveReadIndex = SerialReceiveWriteIndex;

    while ((SerialRead(SERIAL_REGISTER_LINE_STATUS) & SERIAL_LINE_STATUS_DATA_READY) != 0U)
    {
        (void)SerialRead(SERIAL_REGISTER_DATA);
    }
}

const SerialConfiguration *SerialCurrentConfiguration(void)
{
    return &SerialActiveConfiguration;
}

uint32_t SerialRealisedBaudRate(void)
{
    if (SerialActiveDivisor == 0U)
    {
        return 0U;
    }

    return SERIAL_MAXIMUM_BAUD_RATE / (uint32_t)SerialActiveDivisor;
}

uint16_t SerialDivisor(void)
{
    return SerialActiveDivisor;
}

uint64_t SerialCharactersTransmitted(void)
{
    return SerialTransmitted;
}

uint64_t SerialCharactersReceived(void)
{
    return SerialReceived;
}

uint64_t SerialInterruptCount(void)
{
    return SerialInterrupts;
}

uint64_t SerialLineErrorCount(void)
{
    return SerialLineErrors;
}

uint64_t SerialReceiveOverrunCount(void)
{
    return SerialReceiveOverruns;
}

uint64_t SerialTransmitWaitCount(void)
{
    return SerialTransmitWaits;
}

/* The name of a parity scheme, for the report. */
static const char *SerialParityName(SerialParity parity)
{
    switch (parity)
    {
    case SERIAL_PARITY_NONE:
        return "none";
    case SERIAL_PARITY_ODD:
        return "odd";
    case SERIAL_PARITY_EVEN:
        return "even";
    case SERIAL_PARITY_MARK:
        return "mark";
    case SERIAL_PARITY_SPACE:
        return "space";
    default:
        return "unknown";
    }
}

void SerialReport(void)
{
    KernelWriteString("Serial adapter: ");

    if (SerialActivePort == 0U)
    {
        KernelWriteString("absent; no diagnostic channel.\n");
        return;
    }

    KernelWriteString("base ");
    KernelWriteHexadecimal((uint64_t)SerialActivePort);
    KernelWriteString(", divisor ");
    KernelWriteDecimal((uint64_t)SerialActiveDivisor);
    KernelWriteString(", requested ");
    KernelWriteDecimal((uint64_t)SerialActiveConfiguration.baud_rate);
    KernelWriteString(" baud, realised ");
    KernelWriteDecimal((uint64_t)SerialRealisedBaudRate());
    KernelWriteString(" baud.\n");

    KernelWriteString("Serial adapter: ");
    KernelWriteDecimal((uint64_t)SerialActiveConfiguration.data_bits);
    KernelWriteString(" data bits, parity ");
    KernelWriteString(SerialParityName(SerialActiveConfiguration.parity));
    KernelWriteString(", ");
    KernelWriteDecimal(SerialActiveConfiguration.stop_bits == SERIAL_STOP_BITS_TWO ? 2U : 1U);
    KernelWriteString(" stop bit(s), ");

    if (!SerialInterruptsEnabled)
    {
        KernelWriteString("polled.\n");
    }
    else
    {
        KernelWriteString("interrupt-driven upon line ");
        KernelWriteDecimal((uint64_t)SERIAL_COM1_IRQ);
        KernelWriteString(PicLineIsMasked(SERIAL_COM1_IRQ) ? ", masked.\n" : ", unmasked.\n");
    }

    KernelWriteString("Serial adapter: transmitted ");
    KernelWriteDecimal(SerialTransmitted);
    KernelWriteString(", received ");
    KernelWriteDecimal(SerialReceived);
    KernelWriteString(", interrupts ");
    KernelWriteDecimal(SerialInterrupts);
    KernelWriteString(", queued ");
    KernelWriteDecimal((uint64_t)SerialTransmitOccupancy());
    KernelWriteString(", waits ");
    KernelWriteDecimal(SerialTransmitWaits);
    KernelWriteString(", line errors ");
    KernelWriteDecimal(SerialLineErrors);
    KernelWriteString(" (last status ");
    KernelWriteHexadecimal((uint64_t)SerialLastErrorStatus);
    KernelWriteString("), receive overruns ");
    KernelWriteDecimal(SerialReceiveOverruns);
    KernelWriteString(".\n");
}
