/*
 * File: drivers/serial/serial.c
 * Purpose: Implements the early polled serial output driver for the 16550
 *          compatible universal asynchronous receiver/transmitter, providing
 *          diagnostic output that is independent of the display hardware and
 *          that may be captured by a virtual machine monitor or by a null-modem
 *          cable attached to physical hardware.
 * Key functions: SerialInitialise, SerialPutCharacter, SerialWriteString.
 * References:
 *   - National Semiconductor PC16550D datasheet, Table 2 (register map): the
 *     receiver and transmitter buffer registers lie at offset 0, the interrupt
 *     enable register at offset 1, the first-in-first-out control register at
 *     offset 2, the line control register at offset 3, the modem control
 *     register at offset 4, the line status register at offset 5, and, when the
 *     divisor latch access bit is set, the divisor latches at offsets 0 and 1.
 *   - PC16550D datasheet, Section "Line Status Register": bit 5 indicates that
 *     the transmitter holding register is empty and may accept a further
 *     character.
 *   - PC16550D datasheet, Section "Modem Control Register": bit 4 places the
 *     device in local loopback mode, which permits presence detection without
 *     transmitting upon the external line.
 *   - IBM Personal Computer AT technical reference: the first serial adapter is
 *     decoded at I/O base address 0x03F8, and its reference oscillator yields a
 *     maximum signalling rate of 115200 baud with a divisor of one.
 *
 * Note: this is the minimal polled subset required for Phase 1 diagnostics. The
 * formal interrupt-driven driver is scheduled for Phase 4, sub-task 4.1.
 */

#include <oxys/serial.h>
#include <oxys/io.h>

/* Register offsets from the adapter's I/O base address. */
#define SERIAL_REGISTER_DATA                  0U /* Receiver and transmitter buffers. */
#define SERIAL_REGISTER_INTERRUPT_ENABLE      1U
#define SERIAL_REGISTER_FIFO_CONTROL          2U
#define SERIAL_REGISTER_LINE_CONTROL          3U
#define SERIAL_REGISTER_MODEM_CONTROL         4U
#define SERIAL_REGISTER_LINE_STATUS           5U

/* The divisor latches, accessible only while the divisor latch access bit is set. */
#define SERIAL_REGISTER_DIVISOR_LOW           0U
#define SERIAL_REGISTER_DIVISOR_HIGH          1U

/* Line control register bits. */
#define SERIAL_LINE_CONTROL_EIGHT_DATA_BITS   UINT8_C(0x03)
#define SERIAL_LINE_CONTROL_DIVISOR_LATCH     UINT8_C(0x80)

/* Line status register bits. */
#define SERIAL_LINE_STATUS_TRANSMITTER_EMPTY  UINT8_C(0x20)

/* Modem control register bits. */
#define SERIAL_MODEM_CONTROL_DATA_TERMINAL_READY UINT8_C(0x01)
#define SERIAL_MODEM_CONTROL_REQUEST_TO_SEND     UINT8_C(0x02)
#define SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO UINT8_C(0x08)
#define SERIAL_MODEM_CONTROL_LOOPBACK            UINT8_C(0x10)

/* First-in-first-out control register bits. */
#define SERIAL_FIFO_CONTROL_ENABLE            UINT8_C(0x01)
#define SERIAL_FIFO_CONTROL_CLEAR_RECEIVE     UINT8_C(0x02)
#define SERIAL_FIFO_CONTROL_CLEAR_TRANSMIT    UINT8_C(0x04)
#define SERIAL_FIFO_CONTROL_TRIGGER_FOURTEEN  UINT8_C(0xC0)

/*
 * The divisor corresponding to 115200 baud, the reference oscillator being
 * divided by unity at that rate.
 */
#define SERIAL_DIVISOR_115200                 UINT16_C(1)

/* An arbitrary value transmitted during the loopback presence test. */
#define SERIAL_LOOPBACK_TEST_BYTE             UINT8_C(0xAE)

/*
 * The base address of the adapter in use. It is zero until SerialInitialise has
 * succeeded, and every output function treats zero as an instruction to discard
 * its argument, so that a system without a serial adapter proceeds unimpeded.
 */
static uint16_t SerialActivePort;

bool SerialInitialise(uint16_t port)
{
    uint8_t received_byte;

    SerialActivePort = 0U;

    /* Mask all adapter interrupts; this driver operates by polling alone. */
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_INTERRUPT_ENABLE), 0x00U);

    /* Set the divisor latch access bit in order to reach the divisor latches. */
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_LINE_CONTROL),
                  SERIAL_LINE_CONTROL_DIVISOR_LATCH);
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_DIVISOR_LOW),
                  (uint8_t)(SERIAL_DIVISOR_115200 & 0x00FFU));
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_DIVISOR_HIGH),
                  (uint8_t)((SERIAL_DIVISOR_115200 >> 8) & 0x00FFU));

    /*
     * Clear the divisor latch access bit and select eight data bits, no parity
     * and one stop bit, the configuration conventionally abbreviated 8N1.
     */
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_LINE_CONTROL),
                  SERIAL_LINE_CONTROL_EIGHT_DATA_BITS);

    /* Enable and clear the first-in-first-out buffers, with a trigger level of fourteen. */
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_FIFO_CONTROL),
                  (uint8_t)(SERIAL_FIFO_CONTROL_ENABLE |
                            SERIAL_FIFO_CONTROL_CLEAR_RECEIVE |
                            SERIAL_FIFO_CONTROL_CLEAR_TRANSMIT |
                            SERIAL_FIFO_CONTROL_TRIGGER_FOURTEEN));

    /*
     * Determine whether an adapter is present by placing the device in local
     * loopback mode and confirming that a transmitted byte is received again.
     */
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_MODEM_CONTROL),
                  (uint8_t)(SERIAL_MODEM_CONTROL_LOOPBACK |
                            SERIAL_MODEM_CONTROL_REQUEST_TO_SEND |
                            SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO));
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_DATA), SERIAL_LOOPBACK_TEST_BYTE);
    received_byte = PortReadByte((uint16_t)(port + SERIAL_REGISTER_DATA));

    if (received_byte != SERIAL_LOOPBACK_TEST_BYTE)
    {
        return false;
    }

    /*
     * Leave loopback mode and assert data terminal ready, request to send and
     * the second auxiliary output, the last of which gates the interrupt line
     * upon the original adapter and is conventionally asserted.
     */
    PortWriteByte((uint16_t)(port + SERIAL_REGISTER_MODEM_CONTROL),
                  (uint8_t)(SERIAL_MODEM_CONTROL_DATA_TERMINAL_READY |
                            SERIAL_MODEM_CONTROL_REQUEST_TO_SEND |
                            SERIAL_MODEM_CONTROL_AUXILIARY_OUTPUT_TWO));

    SerialActivePort = port;

    return true;
}

/*
 * Blocks until the transmitter holding register is reported empty.
 */
static void SerialWaitForTransmitterEmpty(void)
{
    while ((PortReadByte((uint16_t)(SerialActivePort + SERIAL_REGISTER_LINE_STATUS)) &
            SERIAL_LINE_STATUS_TRANSMITTER_EMPTY) == 0U)
    {
        /* Deliberately empty: the register is polled until the flag is set. */
    }
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
        SerialWaitForTransmitterEmpty();
        PortWriteByte((uint16_t)(SerialActivePort + SERIAL_REGISTER_DATA), (uint8_t)'\r');
    }

    SerialWaitForTransmitterEmpty();
    PortWriteByte((uint16_t)(SerialActivePort + SERIAL_REGISTER_DATA),
                  (uint8_t)(unsigned char)character);
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
