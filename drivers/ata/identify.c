/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/identify.c
 * Purpose: Implements the identification of whatever stands at one of the four
 *          addresses a pair of channels presents: the issue of IDENTIFY DEVICE,
 *          the distinction between a device that is absent and one that answers
 *          a different command set, the extraction of the model and serial
 *          strings from the words that hold them, and the two addressing forms
 *          the returned block declares.
 * Key functions: AtaIdentify, AtaExtractString.
 * References:
 *   - ATA/ATAPI Command Set (ACS-3), IDENTIFY DEVICE: the 256-word block, of
 *     which word 83 bit 10 states that 48-bit addressing is supported, words 60
 *     and 61 hold the 28-bit sector count and words 100 to 103 the 48-bit one.
 *   - The same, the ATAPI signature: a device answering IDENTIFY DEVICE with an
 *     abort, and presenting 0x14 and 0xEB in the cylinder registers, is a packet
 *     device and not a disk. A driver that read the abort alone would report no
 *     device where there is one it cannot drive, which is a different fact.
 *   - The same, the identify strings: each pair of bytes is stored with the two
 *     characters exchanged, so a reader that copied them in order produces a
 *     model name with every pair of letters transposed — which looks like
 *     corruption and is a byte-order error.
 */

#include "internal.h"

#include <oxys/io.h>
#include <oxys/kernel.h>
#include <oxys/pit.h>
#include <oxys/block.h>

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
void AtaIdentify(AtaDevice *device)
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

