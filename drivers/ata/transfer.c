/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/transfer.c
 * Purpose: Implements the transfer of sectors in programmed input/output mode:
 *          the judgement of a request before any register is written, the
 *          composition of a command in either addressing form, the movement of
 *          the words of each sector, and the cache flush that makes a write
 *          durable.
 * Key functions: AtaRequest, AtaTransfer, AtaRead, AtaWrite.
 * References:
 *   - ATA/ATAPI Command Set (ACS-3), READ SECTORS and WRITE SECTORS, and their
 *     EXT counterparts: the 28-bit form takes a count of 256 in a byte where
 *     zero means 256, and the 48-bit form a count of 65536 in a half where zero
 *     means 65536 — so the two forms disagree about what a count of zero means
 *     and neither means nothing.
 *   - The same, the high-order-byte-first rule of the 48-bit form: each of the
 *     three registers that carries two bytes is written with the high byte and
 *     then the low, and a driver that wrote them in the other order addresses a
 *     sector whose number is a plausible one somewhere else on the disk.
 *   - The same, FLUSH CACHE: a write is not durable until the device has been
 *     told to commit it, so the flush is issued within the same sequence as the
 *     write and not deferred.
 *   - docs/storage/DISK.md, Section 5: why there is deliberately no `REP OUTSW`
 *     to match the `REP INSW` of the read path.
 *
 * Every judgement about a request is made here and once. The block layer above
 * validates what it is responsible for, and repeating those rules in the block
 * adaptor would be two statements of one rule with two opportunities to differ.
 */

#include "internal.h"

#include <oxys/dev/io.h>
#include <oxys/kernel.h>
#include <oxys/dev/pit.h>
#include <oxys/block/block.h>

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
bool AtaRequest(const AtaDevice *device, uint64_t lba, uint32_t count, void *buffer,
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
