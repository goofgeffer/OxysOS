/*
 * File: drivers/ps2/ps2.c
 * Purpose: Implements the driver for the 8042 keyboard controller itself: its
 *          self-test, the discovery and testing of its two device ports, the
 *          single configuration byte governing both, and the bounded exchange of
 *          bytes with the device upon either port.
 * Key functions: Ps2Initialise, Ps2IsPresent, Ps2PortIsUsable, Ps2EnablePort,
 *          Ps2SetPortInterrupt, Ps2ReadData, Ps2ReadDataFrom,
 *          Ps2ReadPending, Ps2ByteIsFromSecondPort, Ps2SendDeviceCommand, Ps2SendDeviceByte,
 *          Ps2DrainOutputBuffer, Ps2Configuration, Ps2Report.
 * References:
 *   - IBM Personal Computer AT technical reference, the 8042 keyboard
 *     controller: the data port at 0x60, and the status register read at 0x64
 *     with the command register written at the same address. Status bit 0 is set
 *     while the output buffer holds a byte for the processor and bit 1 while the
 *     input buffer still holds one for the controller.
 *   - The same, the auxiliary device: status bit 5 is set when the byte standing
 *     in the output buffer arrived from the second port rather than the first.
 *   - The 8042 controller command set: 0x20 reads the configuration byte and
 *     0x60 writes it; 0xAD and 0xAE disable and enable the first port, 0xA7 and
 *     0xA8 the second; 0xAA is the controller self-test, answered by 0x55; 0xAB
 *     tests the first port and 0xA9 the second, each answered by 0x00; 0xD4
 *     directs the byte written next to the second port.
 *   - The same, the configuration byte: bit 0 enables the first port's
 *     interrupt, bit 1 the second port's, bit 4 disables the first port's clock
 *     when set, bit 5 the second port's clock, and bit 6 enables the translation
 *     of scan code set 2 into set 1.
 *   - The PS/2 device command set: a device answers 0xFA to acknowledge and 0xFE
 *     to ask that the command be sent again.
 *   - docs/devices/KEYBOARD.md, Section 2: the controller, as distinct from the
 *     keyboard reached through it.
 *
 * Why every wait is bounded.
 *
 * The convention recorded in drivers/README.md is that a missing device must
 * never cause the kernel to block, and an unbounded wait upon a status flag is
 * exactly how it would. A machine with no PS/2 controller decodes port 0x64 as a
 * constant — commonly all ones, which has both the input-full and output-full
 * bits set — and a loop awaiting either to change would run for as long as the
 * machine did. Every wait here therefore counts its attempts and gives up, and
 * giving up is reported rather than retried.
 *
 * Concurrency. Everything save the reading of the data port and the status bit
 * beside it is called during initialisation, before the interrupt flag is set.
 * The spinlock of sub-task 6.13 exists and has not been applied here. When a
 * second processor runs, the read-modify-write of the configuration byte
 * requires it; the handlers' reads do not, the controller holding one byte at a
 * time and the two handlers being woken by different request lines.
 */

#include <oxys/ps2.h>
#include <oxys/io.h>
#include <oxys/kernel.h>

/* The data port, and the status and command port. */
#define PS2_DATA_PORT    UINT16_C(0x0060)
#define PS2_STATUS_PORT  UINT16_C(0x0064)
#define PS2_COMMAND_PORT UINT16_C(0x0064)

/* Status register bits. */
#define PS2_STATUS_OUTPUT_FULL UINT8_C(0x01)
#define PS2_STATUS_INPUT_FULL  UINT8_C(0x02)
#define PS2_STATUS_AUXILIARY   UINT8_C(0x20)

/* Controller commands. */
#define PS2_COMMAND_READ_CONFIGURATION  UINT8_C(0x20)
#define PS2_COMMAND_WRITE_CONFIGURATION UINT8_C(0x60)
#define PS2_COMMAND_DISABLE_SECOND_PORT UINT8_C(0xA7)
#define PS2_COMMAND_ENABLE_SECOND_PORT  UINT8_C(0xA8)
#define PS2_COMMAND_TEST_SECOND_PORT    UINT8_C(0xA9)
#define PS2_COMMAND_SELF_TEST           UINT8_C(0xAA)
#define PS2_COMMAND_TEST_FIRST_PORT     UINT8_C(0xAB)
#define PS2_COMMAND_DISABLE_FIRST_PORT  UINT8_C(0xAD)
#define PS2_COMMAND_ENABLE_FIRST_PORT   UINT8_C(0xAE)
#define PS2_COMMAND_WRITE_SECOND_PORT   UINT8_C(0xD4)

/* The answers those commands give upon success. */
#define PS2_CONTROLLER_SELF_TEST_PASSED UINT8_C(0x55)
#define PS2_PORT_TEST_PASSED            UINT8_C(0x00)

/* Configuration byte bits. */
#define PS2_CONFIGURATION_FIRST_PORT_INTERRUPT  UINT8_C(0x01)
#define PS2_CONFIGURATION_SECOND_PORT_INTERRUPT UINT8_C(0x02)
#define PS2_CONFIGURATION_FIRST_PORT_CLOCK_OFF  UINT8_C(0x10)
#define PS2_CONFIGURATION_SECOND_PORT_CLOCK_OFF UINT8_C(0x20)
#define PS2_CONFIGURATION_TRANSLATION           UINT8_C(0x40)

/*
 * The bound upon every wait for the controller, in iterations. See the header
 * comment: this is a requirement and not a refinement.
 */
#define PS2_WAIT_LIMIT 100000U

/*
 * The bound upon a drain of the output buffer. It counts bytes, not iterations,
 * and does not share the constant above: the two count different things, and a
 * change to one must not silently alter the other. A controller holding more
 * than this many bytes is malfunctioning.
 */
#define PS2_DRAIN_LIMIT 32U

/* The number of times a command is repeated while the device asks for it
 * again. Three is enough for a device that stuttered and few enough that a
 * device answering 0xFE for ever is discovered rather than obeyed. */
#define PS2_RESEND_LIMIT 3U

/* Whether a working controller was found, and which of its ports are usable. */
static bool Ps2Present;
static bool Ps2PortUsable[PS2_PORT_COUNT];

/*
 * The configuration byte, as this module last wrote it.
 *
 * It is cached rather than re-read before each modification because a
 * read-modify-write against the device would reintroduce exactly the fault this
 * module exists to remove: the byte read back reflects whatever the other port's
 * driver has since done, and merging two drivers' intentions by reading the
 * result of one of them is how the translation bit came to be lost.
 */
static uint8_t Ps2ConfigurationByte;

/* Accounting, for the report and for the self-test. */
static uint64_t Ps2CommandsIssued;
static uint64_t Ps2TimeoutsObserved;
static uint64_t Ps2BytesDrained;

/*
 * Waits until the controller will accept a byte, which is to say until the input
 * buffer is empty. Returns false if the bound was reached.
 */
static bool Ps2WaitToWrite(void)
{
    for (uint32_t attempt = 0U; attempt < PS2_WAIT_LIMIT; ++attempt)
    {
        if ((PortReadByte(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0U)
        {
            return true;
        }
    }

    ++Ps2TimeoutsObserved;

    return false;
}

/*
 * Waits until the controller has a byte for the processor. Returns false if the
 * bound was reached, which is how the absence of a device is discovered.
 */
static bool Ps2WaitToRead(void)
{
    for (uint32_t attempt = 0U; attempt < PS2_WAIT_LIMIT; ++attempt)
    {
        if ((PortReadByte(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0U)
        {
            return true;
        }
    }

    ++Ps2TimeoutsObserved;

    return false;
}

/* Sends a command to the controller itself. */
static bool Ps2SendControllerCommand(uint8_t command)
{
    if (!Ps2WaitToWrite())
    {
        return false;
    }

    PortWriteByte(PS2_COMMAND_PORT, command);
    ++Ps2CommandsIssued;

    return true;
}

/* Writes a byte to the data port, whether as the argument of a controller
 * command or as a command to the device upon the first port. */
static bool Ps2WriteDataPort(uint8_t value)
{
    if (!Ps2WaitToWrite())
    {
        return false;
    }

    PortWriteByte(PS2_DATA_PORT, value);

    return true;
}

/* Reads and writes the configuration byte held by the controller. */
static bool Ps2ReadConfigurationFromDevice(uint8_t *configuration)
{
    if (!Ps2SendControllerCommand(PS2_COMMAND_READ_CONFIGURATION))
    {
        return false;
    }

    return Ps2ReadData(configuration);
}

static bool Ps2WriteConfigurationToDevice(uint8_t configuration)
{
    if (!Ps2SendControllerCommand(PS2_COMMAND_WRITE_CONFIGURATION))
    {
        return false;
    }

    if (!Ps2WriteDataPort(configuration))
    {
        return false;
    }

    Ps2ConfigurationByte = configuration;

    return true;
}

/*
 * Directs the next byte written to the second port, where that is the port
 * addressed. The first port needs no prefix, being where a byte written to the
 * data port goes by default.
 */
static bool Ps2AddressPort(uint8_t port)
{
    if (port == PS2_PORT_SECOND)
    {
        return Ps2SendControllerCommand(PS2_COMMAND_WRITE_SECOND_PORT);
    }

    return true;
}

/*
 * Sends one byte to a device and awaits its acknowledgement, retrying while the
 * device asks for the byte to be sent again.
 *
 * Both a command and a command's argument are sent this way, a device
 * acknowledging the two identically; the two public entry points differ only in
 * what they are called, for the reason given in the header.
 */
static bool Ps2SendToDevice(uint8_t port, uint8_t value)
{
    if (port >= PS2_PORT_COUNT || !Ps2PortUsable[port])
    {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < PS2_RESEND_LIMIT; ++attempt)
    {
        uint8_t answer;

        if (!Ps2AddressPort(port) || !Ps2WriteDataPort(value))
        {
            return false;
        }

        if (!Ps2ReadData(&answer))
        {
            return false;
        }

        if (answer == PS2_ACKNOWLEDGE)
        {
            return true;
        }

        if (answer != PS2_RESEND)
        {
            return false;
        }
    }

    return false;
}

bool Ps2ReadData(uint8_t *value)
{
    if (value == NULL)
    {
        return false;
    }

    if (!Ps2WaitToRead())
    {
        return false;
    }

    *value = PortReadByte(PS2_DATA_PORT);

    return true;
}

bool Ps2ByteIsFromSecondPort(void)
{
    return (PortReadByte(PS2_STATUS_PORT) & PS2_STATUS_AUXILIARY) != 0U;
}

bool Ps2ReadDataFrom(uint8_t *value, uint8_t *port)
{
    uint8_t status;

    if (value == NULL || port == NULL)
    {
        return false;
    }

    if (!Ps2WaitToRead())
    {
        return false;
    }

    /*
     * The status is read before the data and not after. Reading the data port
     * empties the output buffer and clears the auxiliary bit along with it, so a
     * caller that read the byte first would have nothing left to ask about it.
     */
    status = PortReadByte(PS2_STATUS_PORT);
    *port = ((status & PS2_STATUS_AUXILIARY) != 0U) ? PS2_PORT_SECOND : PS2_PORT_FIRST;
    *value = PortReadByte(PS2_DATA_PORT);

    return true;
}

bool Ps2ReadPending(uint8_t *value, uint8_t *port)
{
    uint8_t status;

    if (value == NULL || port == NULL)
    {
        return false;
    }

    status = PortReadByte(PS2_STATUS_PORT);

    if ((status & PS2_STATUS_OUTPUT_FULL) == 0U)
    {
        return false;
    }

    /* The status just read is the one that describes this byte, for the reason
     * given in Ps2ReadDataFrom: the read below clears the auxiliary bit with the
     * buffer. */
    *port = ((status & PS2_STATUS_AUXILIARY) != 0U) ? PS2_PORT_SECOND : PS2_PORT_FIRST;
    *value = PortReadByte(PS2_DATA_PORT);

    return true;
}

void Ps2DrainOutputBuffer(void)
{
    for (uint32_t attempt = 0U; attempt < PS2_DRAIN_LIMIT; ++attempt)
    {
        if ((PortReadByte(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) == 0U)
        {
            return;
        }

        (void)PortReadByte(PS2_DATA_PORT);
        ++Ps2BytesDrained;
    }
}

bool Ps2SendDeviceCommand(uint8_t port, uint8_t command)
{
    return Ps2SendToDevice(port, command);
}

bool Ps2SendDeviceByte(uint8_t port, uint8_t value)
{
    return Ps2SendToDevice(port, value);
}

bool Ps2EnablePort(uint8_t port)
{
    if (port >= PS2_PORT_COUNT || !Ps2PortUsable[port])
    {
        return false;
    }

    return Ps2SendControllerCommand((port == PS2_PORT_FIRST)
                                        ? PS2_COMMAND_ENABLE_FIRST_PORT
                                        : PS2_COMMAND_ENABLE_SECOND_PORT);
}

bool Ps2SetPortInterrupt(uint8_t port, bool enabled)
{
    uint8_t bit;
    uint8_t configuration;

    if (port >= PS2_PORT_COUNT || !Ps2PortUsable[port])
    {
        return false;
    }

    bit = (port == PS2_PORT_FIRST) ? PS2_CONFIGURATION_FIRST_PORT_INTERRUPT
                                   : PS2_CONFIGURATION_SECOND_PORT_INTERRUPT;

    configuration = Ps2ConfigurationByte;

    if (enabled)
    {
        configuration |= bit;
    }
    else
    {
        configuration &= (uint8_t)~bit;
    }

    return Ps2WriteConfigurationToDevice(configuration);
}

/*
 * Determines whether the controller has a second port.
 *
 * There is no register reporting the number of ports, so the question is put to
 * the controller indirectly. The second port is enabled and the configuration
 * byte read back: a controller that has a second port has started its clock, and
 * the clock-disable bit is therefore found clear. A controller with one port
 * ignores the command, and the bit stands as it was. The port is disabled again
 * immediately, nothing being ready to receive from it.
 *
 * The test is destructive of nothing and is the arrangement described in the
 * controller command set; it is performed before either port's own test, since
 * testing a port that does not exist is how a controller with one port comes to
 * report a failure it did not have.
 */
static bool Ps2SecondPortExists(void)
{
    uint8_t configuration;

    if (!Ps2SendControllerCommand(PS2_COMMAND_ENABLE_SECOND_PORT))
    {
        return false;
    }

    if (!Ps2ReadConfigurationFromDevice(&configuration))
    {
        return false;
    }

    (void)Ps2SendControllerCommand(PS2_COMMAND_DISABLE_SECOND_PORT);

    return (configuration & PS2_CONFIGURATION_SECOND_PORT_CLOCK_OFF) == 0U;
}

bool Ps2Initialise(void)
{
    uint8_t configuration;
    uint8_t answer;

    Ps2Present = false;
    Ps2PortUsable[PS2_PORT_FIRST] = false;
    Ps2PortUsable[PS2_PORT_SECOND] = false;
    Ps2ConfigurationByte = 0U;
    Ps2CommandsIssued = 0U;
    Ps2TimeoutsObserved = 0U;
    Ps2BytesDrained = 0U;

    /*
     * Both ports are disabled first, so that nothing arrives while the
     * controller is being configured and no byte read below belongs to a
     * keystroke or a movement rather than to the exchange in progress.
     */
    (void)Ps2SendControllerCommand(PS2_COMMAND_DISABLE_FIRST_PORT);
    (void)Ps2SendControllerCommand(PS2_COMMAND_DISABLE_SECOND_PORT);

    Ps2DrainOutputBuffer();

    if (!Ps2ReadConfigurationFromDevice(&configuration))
    {
        return false;
    }

    /*
     * Silence both ports' interrupts for the duration, ensure the first port's
     * clock is running, and ensure the translation of set 2 into set 1 is in
     * force.
     *
     * The translation bit is set here, in the controller's own module, because
     * it is the controller that translates: a keyboard sends set 2 whatever this
     * kernel does, and set 1 is what appears at the data port only while this
     * bit stands. The firmware ordinarily sets it, so a driver that merely
     * assumed set 1 would work upon most machines and fail upon the rest — and
     * would fail by delivering plausible characters that were simply the wrong
     * ones, the two sets overlapping without agreeing.
     */
    configuration &= (uint8_t)~(PS2_CONFIGURATION_FIRST_PORT_INTERRUPT |
                                PS2_CONFIGURATION_SECOND_PORT_INTERRUPT |
                                PS2_CONFIGURATION_FIRST_PORT_CLOCK_OFF);
    configuration |= PS2_CONFIGURATION_TRANSLATION;

    if (!Ps2WriteConfigurationToDevice(configuration))
    {
        return false;
    }

    /* The controller's own self-test. */
    if (!Ps2SendControllerCommand(PS2_COMMAND_SELF_TEST) || !Ps2ReadData(&answer) ||
        answer != PS2_CONTROLLER_SELF_TEST_PASSED)
    {
        return false;
    }

    /*
     * The self-test resets the controller upon some implementations, discarding
     * the configuration written above. It is therefore written again. Upon an
     * implementation that does not reset, this is merely redundant.
     */
    if (!Ps2WriteConfigurationToDevice(configuration))
    {
        return false;
    }

    /*
     * Whether there is a second port at all, before either port is tested. The
     * enabling this performs is undone within it.
     */
    if (Ps2SecondPortExists())
    {
        /*
         * The clock bit was cleared by the controller when it started the port,
         * and the cached byte must record that or the next write of it would
         * stop the clock again — which is the read-modify-write fault this
         * module exists to prevent, appearing within the module itself.
         */
        Ps2ConfigurationByte &= (uint8_t)~PS2_CONFIGURATION_SECOND_PORT_CLOCK_OFF;

        if (Ps2SendControllerCommand(PS2_COMMAND_TEST_SECOND_PORT) && Ps2ReadData(&answer) &&
            answer == PS2_PORT_TEST_PASSED)
        {
            Ps2PortUsable[PS2_PORT_SECOND] = true;
        }
    }

    /* The first port's own test. A failure here is not fatal to the module: a
     * machine may have a mouse and no keyboard, and the second port has already
     * been judged upon its own evidence. */
    if (Ps2SendControllerCommand(PS2_COMMAND_TEST_FIRST_PORT) && Ps2ReadData(&answer) &&
        answer == PS2_PORT_TEST_PASSED)
    {
        Ps2PortUsable[PS2_PORT_FIRST] = true;
    }

    Ps2Present = Ps2PortUsable[PS2_PORT_FIRST] || Ps2PortUsable[PS2_PORT_SECOND];

    return Ps2Present;
}

bool Ps2IsPresent(void)
{
    return Ps2Present;
}

bool Ps2PortIsUsable(uint8_t port)
{
    if (port >= PS2_PORT_COUNT)
    {
        return false;
    }

    return Ps2PortUsable[port];
}

uint8_t Ps2Configuration(void)
{
    return Ps2ConfigurationByte;
}

void Ps2Report(void)
{
    KernelWriteString("8042 controller: ");

    if (!Ps2Present)
    {
        KernelWriteString("absent or unusable; neither port may be used.\n");
        return;
    }

    KernelWriteString("present, first port ");
    KernelWriteString(Ps2PortUsable[PS2_PORT_FIRST] ? "usable" : "unusable");
    KernelWriteString(", second port ");
    KernelWriteString(Ps2PortUsable[PS2_PORT_SECOND] ? "usable" : "absent or unusable");
    KernelWriteString(".\n");

    KernelWriteString("8042 controller: configuration ");
    KernelWriteHexadecimal((uint64_t)Ps2ConfigurationByte);
    KernelWriteString(", translation ");
    KernelWriteString(((Ps2ConfigurationByte & PS2_CONFIGURATION_TRANSLATION) != 0U) ? "on"
                                                                                    : "off");
    KernelWriteString(", commands ");
    KernelWriteDecimal(Ps2CommandsIssued);
    KernelWriteString(", timeouts ");
    KernelWriteDecimal(Ps2TimeoutsObserved);
    KernelWriteString(", bytes drained ");
    KernelWriteDecimal(Ps2BytesDrained);
    KernelWriteString(".\n");
}
