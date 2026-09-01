/*
 * File: drivers/ata/ata.c
 * Purpose: Implements the ATA driver in programmed input/output mode: the
 *          software reset of a channel, the identification of the devices upon
 *          it, and the reading and writing of sectors by 28-bit and 48-bit
 *          logical block addressing.
 * Key functions: AtaInitialise, AtaIdentify, AtaRead, AtaWrite, AtaTransfer,
 *          AtaSelect, AtaWaitNotBusy, AtaWaitForData, AtaFail, AtaReject, AtaReport.
 * References:
 *   - AT Attachment with Packet Interface, the command block registers: the data
 *     register at offset 0, the error register (features when written) at 1, the
 *     sector count at 2, the logical block address low, mid and high at 3, 4 and
 *     5, the device register at 6 and the status register (the command register
 *     when written) at 7; the alternate status register (the device control
 *     register when written) lies in the control block, at 0x03F6 for the first
 *     channel in its compatibility addressing.
 *   - ATA/ATAPI, the Status register: BSY (bit 7) while the device owns the
 *     command block; DRDY (bit 6) when it is ready to accept a command; DF (bit
 *     5), a device fault, which does not set ERR; DRQ (bit 3) when a block of
 *     data is ready to be transferred; ERR (bit 0), the error register then
 *     describing the failure.
 *   - ATA/ATAPI, the Device Control register: nIEN (bit 1) stops the device
 *     asserting its interrupt; SRST (bit 2), set and then cleared, resets both
 *     devices upon the channel.
 *   - ATA/ATAPI, the reading of the status register: a device requires 400
 *     nanoseconds after a command or a device selection before the status it
 *     presents is valid. The delay is obtained by reading the alternate status
 *     register, which has no side effect, several times; an input from an I/O
 *     port may be assumed to take at least 30 nanoseconds, so fourteen reads
 *     preceding the one that is believed give better than 400.
 *   - ATA command set, cross-verified against an independent table of opcodes:
 *     20h READ SECTOR(S), 24h READ SECTOR(S) EXT, 30h WRITE SECTOR(S), 34h WRITE
 *     SECTOR(S) EXT, E7h FLUSH CACHE, EAh FLUSH CACHE EXT, ECh IDENTIFY DEVICE.
 *   - ATA/ATAPI, the identification data: words 10 to 19 the serial number and
 *     27 to 46 the model number, each word holding two characters with the first
 *     in its high half; words 60 and 61 the number of sectors addressable by 28
 *     bits; word 83 bit 10, the support of 48-bit addressing; words 100 to 103
 *     the number of sectors addressable by 48 bits.
 *   - ATA/ATAPI, the signature of a device that rejects IDENTIFY DEVICE: a
 *     packet device leaves 14h in the logical block address mid register and EBh
 *     in the high register, and a serial ATA device leaves 3Ch and C3h; an ATA
 *     device that aborted the command leaves both at zero.
 *   - IBM Personal Computer AT technical reference: the fixed disk adapter is
 *     decoded at 0x01F0 with its control register at 0x03F6, and the second
 *     channel at 0x0170 and 0x0376.
 */

#include <oxys/ata.h>
#include <oxys/kernel.h>
#include <oxys/io.h>

/* The command block registers, as offsets from the base address. */
#define ATA_REGISTER_DATA          0U
#define ATA_REGISTER_ERROR         1U
#define ATA_REGISTER_FEATURES      1U
#define ATA_REGISTER_SECTOR_COUNT  2U
#define ATA_REGISTER_LBA_LOW       3U
#define ATA_REGISTER_LBA_MID       4U
#define ATA_REGISTER_LBA_HIGH      5U
#define ATA_REGISTER_DEVICE        6U
#define ATA_REGISTER_STATUS        7U
#define ATA_REGISTER_COMMAND       7U

/* The bits of the status register. */
#define ATA_STATUS_ERR  UINT8_C(0x01)
#define ATA_STATUS_DRQ  UINT8_C(0x08)
#define ATA_STATUS_DF   UINT8_C(0x20)
#define ATA_STATUS_DRDY UINT8_C(0x40)
#define ATA_STATUS_BSY  UINT8_C(0x80)

/* The bits of the device control register. */
#define ATA_CONTROL_NIEN UINT8_C(0x02)
#define ATA_CONTROL_SRST UINT8_C(0x04)

/* The bits of the device register that are not part of an address. */
#define ATA_DEVICE_LBA       UINT8_C(0x40)
#define ATA_DEVICE_SLAVE     UINT8_C(0x10)
#define ATA_DEVICE_OBSOLETE  UINT8_C(0xA0)

/* The commands issued by this driver. */
#define ATA_COMMAND_READ_SECTORS      UINT8_C(0x20)
#define ATA_COMMAND_READ_SECTORS_EXT  UINT8_C(0x24)
#define ATA_COMMAND_WRITE_SECTORS     UINT8_C(0x30)
#define ATA_COMMAND_WRITE_SECTORS_EXT UINT8_C(0x34)
#define ATA_COMMAND_FLUSH_CACHE       UINT8_C(0xE7)
#define ATA_COMMAND_FLUSH_CACHE_EXT   UINT8_C(0xEA)
#define ATA_COMMAND_IDENTIFY          UINT8_C(0xEC)

/* The words of the identification data that this driver reads. */
#define ATA_IDENTIFY_WORDS          256U
#define ATA_IDENTIFY_SERIAL         10U
#define ATA_IDENTIFY_SERIAL_WORDS   10U
#define ATA_IDENTIFY_MODEL          27U
#define ATA_IDENTIFY_MODEL_WORDS    20U
#define ATA_IDENTIFY_LBA28_SECTORS  60U
#define ATA_IDENTIFY_COMMAND_SETS   83U
#define ATA_IDENTIFY_LBA48_SECTORS  100U

/* Bit 10 of word 83: the device supports the 48-bit address commands. */
#define ATA_COMMAND_SET_LBA48 UINT16_C(0x0400)

/* The signatures a device leaves when it declines IDENTIFY DEVICE. */
#define ATA_SIGNATURE_ATAPI_MID  UINT8_C(0x14)
#define ATA_SIGNATURE_ATAPI_HIGH UINT8_C(0xEB)
#define ATA_SIGNATURE_SATA_MID   UINT8_C(0x3C)
#define ATA_SIGNATURE_SATA_HIGH  UINT8_C(0xC3)

/*
 * The number of times the status register is examined before a device is
 * declared unresponsive.
 *
 * A disk may legitimately take seconds to spin up, and the driver has no clock
 * it can consult: the interval timer counts by interrupt, and this driver runs
 * with the interrupt flag clear during initialisation. The limit is therefore a
 * count of examinations rather than a time, and is set high enough that no
 * healthy device reaches it and low enough that an absent one does not stop the
 * machine.
 */
#define ATA_POLL_LIMIT 10000000U

/* The number of reads of the alternate status register that yield 400 ns. */
#define ATA_DELAY_READS 15U

/* The four addresses, and what was found at each. */
static AtaDevice AtaDevices[ATA_DEVICE_COUNT];
static size_t AtaPresent;

/*
 * The device presently selected upon each channel, so that a selection already
 * in force is not repeated: a redundant selection costs the 400 nanoseconds that
 * must follow it, upon every sector of every transfer.
 */
static uint8_t AtaSelected[ATA_CHANNEL_COUNT];
static bool AtaSelectionKnown[ATA_CHANNEL_COUNT];

/* Accounting. */
static uint64_t AtaRead64;
static uint64_t AtaWritten64;
static uint64_t AtaCommands;
static uint64_t AtaErrors;
static uint64_t AtaRejections;
static uint64_t AtaTimeouts;

/* The description of the most recent failure. */
static const char *AtaError = "none";

/* Records a failure, so that a report may say what went wrong and not only that. */
static bool AtaFail(const char *reason)
{
    AtaError = reason;
    ++AtaErrors;
    return false;
}

/*
 * Records a request the driver declined to issue: a range beyond the capacity of
 * the device, a buffer that is not there, a device that is not a disk.
 *
 * It is counted apart from a failure of the hardware because the two mean
 * opposite things. A refusal is the driver working: the caller asked for
 * something impossible and was told so before the disk was touched. A report
 * that added the two together would show a healthy machine accumulating errors,
 * and an operator would learn to ignore the number.
 */
static bool AtaReject(const char *reason)
{
    AtaError = reason;
    ++AtaRejections;
    return false;
}

static uint8_t AtaReadRegister(const AtaDevice *device, unsigned int offset)
{
    return PortReadByte((uint16_t)(device->io_base + offset));
}

static void AtaWriteRegister(const AtaDevice *device, unsigned int offset, uint8_t value)
{
    PortWriteByte((uint16_t)(device->io_base + offset), value);
}

/* The alternate status register reports the status without side effects. */
static uint8_t AtaAlternateStatus(const AtaDevice *device)
{
    return PortReadByte(device->control_base);
}

/*
 * Waits the 400 nanoseconds a device is allowed after a command or a selection
 * before its status is meaningful. The alternate status register is used because
 * reading the status register itself clears a pending interrupt, which is a side
 * effect this driver has no business causing.
 */
static void AtaDelay(const AtaDevice *device)
{
    for (unsigned int index = 0U; index < ATA_DELAY_READS; ++index)
    {
        (void)AtaAlternateStatus(device);
    }
}

/*
 * Selects a device upon its channel, and waits afterwards. The selection is
 * skipped where the same device is already selected, that being the one
 * optimisation the specification positively recommends.
 */
static void AtaSelect(const AtaDevice *device, uint8_t address_bits)
{
    const uint8_t value = (uint8_t)(address_bits | ((device->drive != 0U) ? ATA_DEVICE_SLAVE : 0U));

    if (AtaSelectionKnown[device->channel] && (AtaSelected[device->channel] == value))
    {
        return;
    }

    AtaWriteRegister(device, ATA_REGISTER_DEVICE, value);
    AtaSelected[device->channel] = value;
    AtaSelectionKnown[device->channel] = true;
    AtaDelay(device);
}

/* Waits for the device to release the command block. */
static bool AtaWaitNotBusy(const AtaDevice *device, uint8_t *final_status)
{
    for (uint32_t attempt = 0U; attempt < ATA_POLL_LIMIT; ++attempt)
    {
        const uint8_t status = AtaAlternateStatus(device);

        if ((status & ATA_STATUS_BSY) == 0U)
        {
            if (final_status != NULL)
            {
                *final_status = status;
            }

            return true;
        }
    }

    ++AtaTimeouts;
    return false;
}

/*
 * Waits for a block of data to become transferable, which is the conjunction of
 * two conditions and not one: the device must have released the command block
 * and must then have asserted DRQ. A device that reports an error, or a fault,
 * asserts neither and would otherwise be waited for until the limit.
 */
static bool AtaWaitForData(const AtaDevice *device)
{
    uint8_t status = 0U;

    if (!AtaWaitNotBusy(device, &status))
    {
        return AtaFail("the device did not release the command block");
    }

    if ((status & ATA_STATUS_ERR) != 0U)
    {
        return AtaFail("the device reported an error");
    }

    if ((status & ATA_STATUS_DF) != 0U)
    {
        return AtaFail("the device reported a fault");
    }

    if ((status & ATA_STATUS_DRQ) == 0U)
    {
        return AtaFail("the device offered no data");
    }

    return true;
}

/*
 * Resets both devices upon a channel and leaves their interrupts disabled.
 *
 * The reset is how a channel is brought to a known state without assuming what
 * the firmware left behind it. Interrupts are disabled at the device rather than
 * masked at the controller because nothing claims IRQ14 or IRQ15: a device that
 * asserted one would raise a request the routing layer counts as unclaimed, upon
 * every command.
 */
static void AtaResetChannel(uint8_t channel)
{
    const AtaDevice *const device = &AtaDevices[channel * ATA_DRIVE_COUNT];

    AtaSelectionKnown[channel] = false;

    PortWriteByte(device->control_base, (uint8_t)(ATA_CONTROL_SRST | ATA_CONTROL_NIEN));

    /*
     * The reset must be asserted for at least five microseconds. The delay is
     * expressed in reads of the alternate status register for the same reason as
     * everywhere else here: there is no clock available to this driver, and a
     * read of an I/O port is the one unit of time it can count.
     */
    for (unsigned int index = 0U; index < (ATA_DELAY_READS * 20U); ++index)
    {
        (void)AtaAlternateStatus(device);
    }

    PortWriteByte(device->control_base, ATA_CONTROL_NIEN);
    AtaDelay(device);

    /*
     * A channel with nothing upon it never clears BSY, the floating bus reading
     * as all ones. The wait is bounded and its expiry is not an error.
     */
    (void)AtaWaitNotBusy(device, NULL);
}

/* Extracts a string field of the identification data, trimmed of its padding. */
static void AtaExtractString(const uint16_t *identity, size_t first_word, size_t words,
                             char *destination, size_t capacity)
{
    size_t length = 0U;

    for (size_t index = 0U; (index < words) && ((length + 2U) < capacity); ++index)
    {
        const uint16_t word = identity[first_word + index];

        /*
         * Each word holds two characters with the first in its high half, which
         * is the opposite of the order the processor would place them in.
         */
        destination[length] = (char)((word >> 8) & 0xFFU);
        ++length;
        destination[length] = (char)(word & 0xFFU);
        ++length;
    }

    while ((length > 0U) && (destination[length - 1U] == ' '))
    {
        --length;
    }

    destination[length] = '\0';
}

/*
 * Issues IDENTIFY DEVICE and records what answered.
 *
 * A device that is not there leaves the bus floating, which reads as all ones;
 * a status of zero likewise means nothing answered. A packet device or a serial
 * ATA device rejects the command and leaves its signature in the address
 * registers, which is the only way to tell those apart from a device that
 * aborted the command for some other reason.
 */
static void AtaIdentify(AtaDevice *device)
{
    uint16_t identity[ATA_IDENTIFY_WORDS];
    uint8_t status;

    device->kind = ATA_DEVICE_NONE;

    AtaSelect(device, ATA_DEVICE_OBSOLETE);

    /* The address registers are cleared, the command taking no address. */
    AtaWriteRegister(device, ATA_REGISTER_SECTOR_COUNT, 0U);
    AtaWriteRegister(device, ATA_REGISTER_LBA_LOW, 0U);
    AtaWriteRegister(device, ATA_REGISTER_LBA_MID, 0U);
    AtaWriteRegister(device, ATA_REGISTER_LBA_HIGH, 0U);

    AtaWriteRegister(device, ATA_REGISTER_COMMAND, ATA_COMMAND_IDENTIFY);
    ++AtaCommands;
    AtaDelay(device);

    status = AtaAlternateStatus(device);

    if ((status == 0U) || (status == 0xFFU))
    {
        return;
    }

    if (!AtaWaitNotBusy(device, &status))
    {
        return;
    }

    if ((status & ATA_STATUS_ERR) != 0U)
    {
        const uint8_t mid = AtaReadRegister(device, ATA_REGISTER_LBA_MID);
        const uint8_t high = AtaReadRegister(device, ATA_REGISTER_LBA_HIGH);

        if ((mid == ATA_SIGNATURE_ATAPI_MID) && (high == ATA_SIGNATURE_ATAPI_HIGH))
        {
            device->kind = ATA_DEVICE_ATAPI;
        }
        else if ((mid == ATA_SIGNATURE_SATA_MID) && (high == ATA_SIGNATURE_SATA_HIGH))
        {
            device->kind = ATA_DEVICE_SATA;
        }
        else
        {
            device->kind = ATA_DEVICE_UNKNOWN;
        }

        return;
    }

    if ((status & ATA_STATUS_DRQ) == 0U)
    {
        return;
    }

    PortReadWordString((uint16_t)(device->io_base + ATA_REGISTER_DATA), identity,
                       ATA_IDENTIFY_WORDS);

    device->kind = ATA_DEVICE_ATA;
    device->supports_lba48 =
        (identity[ATA_IDENTIFY_COMMAND_SETS] & ATA_COMMAND_SET_LBA48) != 0U;

    device->sector_count = (uint64_t)identity[ATA_IDENTIFY_LBA28_SECTORS] |
                           ((uint64_t)identity[ATA_IDENTIFY_LBA28_SECTORS + 1U] << 16);

    if (device->supports_lba48)
    {
        uint64_t extended = 0U;

        for (size_t index = 0U; index < 4U; ++index)
        {
            extended |= (uint64_t)identity[ATA_IDENTIFY_LBA48_SECTORS + index] << (16U * index);
        }

        /*
         * A device may support the 48-bit commands and hold fewer sectors than
         * 28 bits can name, in which case the 48-bit count is the authority only
         * where it is the larger of the two.
         */
        if (extended > device->sector_count)
        {
            device->sector_count = extended;
        }
    }

    AtaExtractString(identity, ATA_IDENTIFY_MODEL, ATA_IDENTIFY_MODEL_WORDS, device->model,
                     sizeof(device->model));
    AtaExtractString(identity, ATA_IDENTIFY_SERIAL, ATA_IDENTIFY_SERIAL_WORDS, device->serial,
                     sizeof(device->serial));
}

bool AtaInitialise(void)
{
    AtaPresent = 0U;
    AtaRead64 = 0U;
    AtaWritten64 = 0U;
    AtaCommands = 0U;
    AtaErrors = 0U;
    AtaRejections = 0U;
    AtaTimeouts = 0U;
    AtaError = "none";

    for (uint8_t channel = 0U; channel < ATA_CHANNEL_COUNT; ++channel)
    {
        for (uint8_t drive = 0U; drive < ATA_DRIVE_COUNT; ++drive)
        {
            AtaDevice *const device = &AtaDevices[(channel * ATA_DRIVE_COUNT) + drive];

            device->kind = ATA_DEVICE_NONE;
            device->channel = channel;
            device->drive = drive;
            device->io_base = (channel == 0U) ? ATA_PRIMARY_IO_BASE : ATA_SECONDARY_IO_BASE;
            device->control_base =
                (channel == 0U) ? ATA_PRIMARY_CONTROL_BASE : ATA_SECONDARY_CONTROL_BASE;
            device->supports_lba48 = false;
            device->sector_count = 0U;
            device->model[0] = '\0';
            device->serial[0] = '\0';
        }

        AtaResetChannel(channel);

        for (uint8_t drive = 0U; drive < ATA_DRIVE_COUNT; ++drive)
        {
            AtaDevice *const device = &AtaDevices[(channel * ATA_DRIVE_COUNT) + drive];

            AtaIdentify(device);

            if (device->kind != ATA_DEVICE_NONE)
            {
                ++AtaPresent;
            }
        }
    }

    return AtaPresent != 0U;
}

size_t AtaDeviceCount(void)
{
    return AtaPresent;
}

const AtaDevice *AtaDeviceAt(size_t index)
{
    size_t seen = 0U;

    for (size_t position = 0U; position < ATA_DEVICE_COUNT; ++position)
    {
        if (AtaDevices[position].kind == ATA_DEVICE_NONE)
        {
            continue;
        }

        if (seen == index)
        {
            return &AtaDevices[position];
        }

        ++seen;
    }

    return NULL;
}

const AtaDevice *AtaFirstDisk(void)
{
    for (size_t position = 0U; position < ATA_DEVICE_COUNT; ++position)
    {
        if ((AtaDevices[position].kind == ATA_DEVICE_ATA) &&
            (AtaDevices[position].sector_count != 0U))
        {
            return &AtaDevices[position];
        }
    }

    return NULL;
}

/*
 * Issues one read or write command and transfers its data.
 *
 * The count is the number of sectors, which the register holds in one byte for
 * the 28-bit commands and two for the 48-bit ones; a register value of zero
 * means the greatest count the mode allows, which is why the caller's limits are
 * 256 and 65536 rather than 255 and 65535.
 *
 * The 48-bit form writes each register twice, the high-order byte first and the
 * low-order byte second, the device keeping the previous content of each
 * register in a hidden half. That ordering is the whole of the mechanism and is
 * not an artefact of this implementation.
 */
static bool AtaTransfer(const AtaDevice *device, uint64_t lba, uint32_t count, void *buffer,
                        bool writing, bool extended)
{
    uint8_t *const bytes = (uint8_t *)buffer;
    uint8_t command;

    if (extended)
    {
        AtaSelect(device, (uint8_t)(ATA_DEVICE_LBA));

        AtaWriteRegister(device, ATA_REGISTER_SECTOR_COUNT, (uint8_t)((count >> 8) & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_LOW, (uint8_t)((lba >> 24) & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_MID, (uint8_t)((lba >> 32) & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_HIGH, (uint8_t)((lba >> 40) & 0xFFU));

        AtaWriteRegister(device, ATA_REGISTER_SECTOR_COUNT, (uint8_t)(count & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_LOW, (uint8_t)(lba & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_MID, (uint8_t)((lba >> 8) & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFFU));

        command = writing ? ATA_COMMAND_WRITE_SECTORS_EXT : ATA_COMMAND_READ_SECTORS_EXT;
    }
    else
    {
        /* The four most significant bits of a 28-bit address live in the device
         * register, which is therefore part of the address and not only a
         * selection. */
        AtaSelect(device,
                  (uint8_t)(ATA_DEVICE_OBSOLETE | ATA_DEVICE_LBA | ((lba >> 24) & 0x0FU)));

        AtaWriteRegister(device, ATA_REGISTER_SECTOR_COUNT, (uint8_t)(count & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_LOW, (uint8_t)(lba & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_MID, (uint8_t)((lba >> 8) & 0xFFU));
        AtaWriteRegister(device, ATA_REGISTER_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFFU));

        command = writing ? ATA_COMMAND_WRITE_SECTORS : ATA_COMMAND_READ_SECTORS;
    }

    AtaWriteRegister(device, ATA_REGISTER_COMMAND, command);
    ++AtaCommands;
    AtaDelay(device);

    for (uint32_t sector = 0U; sector < count; ++sector)
    {
        uint8_t *const position = &bytes[(size_t)sector * ATA_SECTOR_SIZE];

        if (!AtaWaitForData(device))
        {
            return false;
        }

        if (writing)
        {
            /*
             * The transmitting side is a loop rather than a string instruction.
             * A device is entitled to a short recovery between the words it is
             * given, which the string form does not allow for and which some
             * devices are documented to require.
             */
            for (size_t word = 0U; word < (ATA_SECTOR_SIZE / 2U); ++word)
            {
                const uint16_t value =
                    (uint16_t)((uint16_t)position[word * 2U] |
                               ((uint16_t)position[(word * 2U) + 1U] << 8));

                PortWriteWord((uint16_t)(device->io_base + ATA_REGISTER_DATA), value);
            }
        }
        else
        {
            PortReadWordString((uint16_t)(device->io_base + ATA_REGISTER_DATA), position,
                               ATA_SECTOR_SIZE / 2U);
        }

        /*
         * The device is given its 400 nanoseconds to withdraw DRQ before the
         * next sector is waited for; without the pause the status of the sector
         * just transferred would be read as though it described the next.
         */
        AtaDelay(device);
    }

    if (writing)
    {
        uint8_t status = 0U;

        /*
         * The cache is flushed within the same command sequence. A device that
         * has accepted the data but not committed it reports success, and the
         * loss appears only upon a later read — which is to say, as corruption
         * with no failure attached to it.
         */
        AtaWriteRegister(device, ATA_REGISTER_COMMAND,
                         extended ? ATA_COMMAND_FLUSH_CACHE_EXT : ATA_COMMAND_FLUSH_CACHE);
        ++AtaCommands;
        AtaDelay(device);

        if (!AtaWaitNotBusy(device, &status))
        {
            return AtaFail("the device did not complete the cache flush");
        }

        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0U)
        {
            return AtaFail("the device reported an error flushing its cache");
        }
    }

    return true;
}

/*
 * Divides a request into commands and issues them.
 *
 * The addressing mode is chosen for the whole request: 48-bit where the device
 * supports it and the request reaches beyond what 28 bits can name, 28-bit
 * otherwise. The 28-bit commands are preferred where they suffice because every
 * device understands them, including one whose declaration of 48-bit support is
 * mistaken.
 */
static bool AtaRequest(const AtaDevice *device, uint64_t lba, uint32_t count, void *buffer,
                       bool writing)
{
    bool extended;
    uint32_t maximum;
    uint8_t *position = (uint8_t *)buffer;

    if ((device == NULL) || (buffer == NULL))
    {
        return AtaReject("no device or no buffer");
    }

    if (device->kind != ATA_DEVICE_ATA)
    {
        return AtaReject("the device is not one this driver can address");
    }

    if (count == 0U)
    {
        return true;
    }

    if ((lba + (uint64_t)count) > device->sector_count)
    {
        return AtaReject("the range lies beyond the capacity of the device");
    }

    extended = device->supports_lba48 &&
               ((lba + (uint64_t)count) > ATA_LBA28_LIMIT ||
                (count > ATA_MAXIMUM_SECTORS_LBA28));
    maximum = extended ? ATA_MAXIMUM_SECTORS_LBA48 : ATA_MAXIMUM_SECTORS_LBA28;

    if (!extended && ((lba + (uint64_t)count) > ATA_LBA28_LIMIT))
    {
        return AtaReject("the address requires 48-bit addressing, which the device lacks");
    }

    while (count > 0U)
    {
        const uint32_t chunk = (count > maximum) ? maximum : count;

        if (!AtaTransfer(device, lba, chunk, position, writing, extended))
        {
            return false;
        }

        if (writing)
        {
            AtaWritten64 += chunk;
        }
        else
        {
            AtaRead64 += chunk;
        }

        lba += chunk;
        count -= chunk;
        position += (size_t)chunk * ATA_SECTOR_SIZE;
    }

    return true;
}

bool AtaRead(const AtaDevice *device, uint64_t lba, uint32_t count, void *buffer)
{
    return AtaRequest(device, lba, count, buffer, false);
}

bool AtaWrite(const AtaDevice *device, uint64_t lba, uint32_t count, const void *buffer)
{
    /*
     * The buffer is not modified by a write, and the cast discards a const
     * qualifier that the shared path cannot express in both directions. The
     * transfer reads from it and never writes to it when writing is asked for.
     */
    return AtaRequest(device, lba, count, (void *)(uintptr_t)buffer, true);
}

uint64_t AtaSectorsRead(void)
{
    return AtaRead64;
}

uint64_t AtaSectorsWritten(void)
{
    return AtaWritten64;
}

uint64_t AtaCommandCount(void)
{
    return AtaCommands;
}

uint64_t AtaErrorCount(void)
{
    return AtaErrors;
}

uint64_t AtaRejectionCount(void)
{
    return AtaRejections;
}

uint64_t AtaTimeoutCount(void)
{
    return AtaTimeouts;
}

const char *AtaLastError(void)
{
    return AtaError;
}

/* The printable name of what was found at an address. */
static const char *AtaKindName(AtaDeviceKind kind)
{
    switch (kind)
    {
    case ATA_DEVICE_ATA:
        return "ATA disk";
    case ATA_DEVICE_ATAPI:
        return "ATAPI packet device";
    case ATA_DEVICE_SATA:
        return "serial ATA device";
    case ATA_DEVICE_UNKNOWN:
        return "unrecognised device";
    default:
        return "nothing";
    }
}

void AtaReport(void)
{
    if (AtaPresent == 0U)
    {
        KernelWriteString("ATA: no device answered upon either channel.\n");
        return;
    }

    KernelWriteString("ATA: ");
    KernelWriteDecimal((uint64_t)AtaPresent);
    KernelWriteString(" devices, polled, device interrupts disabled.\n");

    for (size_t index = 0U; index < AtaPresent; ++index)
    {
        const AtaDevice *const device = AtaDeviceAt(index);

        if (device == NULL)
        {
            break;
        }

        KernelWriteString("  ");
        KernelWriteString((device->channel == 0U) ? "primary " : "secondary ");
        KernelWriteString((device->drive == 0U) ? "master: " : "slave:  ");
        KernelWriteString(AtaKindName(device->kind));

        if (device->kind == ATA_DEVICE_ATA)
        {
            KernelWriteString(", ");
            KernelWriteDecimal(device->sector_count);
            KernelWriteString(" sectors (");
            KernelWriteDecimal((device->sector_count * ATA_SECTOR_SIZE) / 1024U);
            KernelWriteString(" KiB), ");
            KernelWriteString(device->supports_lba48 ? "48-bit" : "28-bit");
            KernelWriteString(" addressing");

            if (device->model[0] != '\0')
            {
                KernelWriteString(", ");
                KernelWriteString(device->model);
            }
        }

        KernelWriteString("\n");
    }

    KernelWriteString("ATA: commands ");
    KernelWriteDecimal(AtaCommands);
    KernelWriteString(", sectors read ");
    KernelWriteDecimal(AtaRead64);
    KernelWriteString(", written ");
    KernelWriteDecimal(AtaWritten64);
    KernelWriteString(", device errors ");
    KernelWriteDecimal(AtaErrors);
    KernelWriteString(", requests refused ");
    KernelWriteDecimal(AtaRejections);
    KernelWriteString(", timeouts ");
    KernelWriteDecimal(AtaTimeouts);
    KernelWriteString(", last error: ");
    KernelWriteString(AtaError);
    KernelWriteString(".\n");
}
