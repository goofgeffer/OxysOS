/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/port.c
 * Purpose: Implements the register-level discipline of the ATA task file: the
 *          reading and writing of a register, the settling delay a selection
 *          must be followed by, the selection of a device upon a channel, the
 *          two waits every command is bracketed by, and the reset of a channel.
 * Key functions: AtaReadRegister, AtaWriteRegister, AtaAlternateStatus,
 *          AtaDelay, AtaSelect, AtaWaitNotBusy, AtaWaitForData, AtaResetChannel.
 * References:
 *   - ATA/ATAPI Command Set (ACS-3), the Device/Head register and the device
 *     selection protocol: a selection must be followed by a settling interval of
 *     400 nanoseconds before the status register means anything, and the
 *     interval is obtained by reading the *alternate* status register fifteen
 *     times, that register having no side effect upon the interrupt.
 *   - The same, the Status register: BSY must be clear before any other bit of
 *     the register may be read as meaningful, so every wait tests BSY first and
 *     the remaining bits only afterwards.
 *   - The same, the Device Control register: SRST is asserted and then released,
 *     and the channel is given time to settle before either device upon it is
 *     addressed again.
 *
 * These are shared with every other part of the driver rather than duplicated,
 * because the timing rules above are the ones a driver gets silently wrong: a
 * missing settling delay yields a status register read too early, which is a
 * plausible value describing the previous command.
 */

#include "internal.h"

#include <oxys/io.h>
#include <oxys/kernel.h>
#include <oxys/pit.h>
#include <oxys/block.h>

uint8_t AtaReadRegister(const AtaDevice *device, unsigned int offset)
{
    return PortReadByte((uint16_t)(device->io_base + offset));
}

void AtaWriteRegister(const AtaDevice *device, unsigned int offset, uint8_t value)
{
    PortWriteByte((uint16_t)(device->io_base + offset), value);
}

/* The alternate status register reports the status without side effects. */
uint8_t AtaAlternateStatus(const AtaDevice *device)
{
    return PortReadByte(device->control_base);
}

/*
 * Waits the 400 nanoseconds a device is allowed after a command or a selection
 * before its status is meaningful. The alternate status register is used because
 * reading the status register itself clears a pending interrupt, which is a side
 * effect this driver has no business causing.
 */
void AtaDelay(const AtaDevice *device)
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
void AtaSelect(const AtaDevice *device, uint8_t address_bits)
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
bool AtaWaitNotBusy(const AtaDevice *device, uint8_t *final_status)
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
bool AtaWaitForData(const AtaDevice *device)
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
void AtaResetChannel(uint8_t channel)
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

