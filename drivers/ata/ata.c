/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/ata.c
 * Purpose: Implements the ATA driver in programmed input/output mode: the
 *          software reset of a channel, the identification of the devices upon
 *          it, and the reading and writing of sectors by 28-bit and 48-bit
 *          logical block addressing.
 * Key functions: AtaInitialise, AtaIdentify, AtaRead, AtaWrite, AtaTransfer,
 *          AtaSelect, AtaWaitNotBusy, AtaWaitForData, AtaFail, AtaReject,
 *          AtaRegisterBlockDevices, AtaReport.
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
 *     channel at 0x0170 and 0x0376. Those are the *compatibility* addresses and
 *     are where a channel answers only while it is in compatibility mode.
 *   - PCI Local Bus Specification, the class code register, and the IDE
 *     controller programming interface: bit 0 states that the primary channel is
 *     in native PCI mode and bit 2 that the secondary is. A native channel
 *     answers at the addresses its base address registers give — BAR0 and BAR1
 *     for the primary, BAR2 and BAR3 for the secondary — and at no others. The
 *     control block register is at offset 2 within the four bytes the control
 *     BAR describes.
 *
 * What this driver reaches, and what it does not.
 *
 *   This is a driver for the ATA command block registers, reached through I/O
 *   ports. It therefore drives an IDE controller, in either mode, and nothing
 *   else. In particular it does not drive an AHCI controller — a serial ATA
 *   controller whose programming interface is 0x01 — whose registers are
 *   memory-mapped and which answers at no I/O port whatever.
 *
 *   That is not a small omission upon a modern machine: a machine whose firmware
 *   presents its SATA controller in AHCI mode carries disks this driver cannot
 *   see, and the symptom is exactly the symptom of having no disk at all. The
 *   report therefore says which it is, rather than leaving a person to guess;
 *   see AtaReportControllers below.
 *
 *   Nor is AHCI the end of it. An inexpensive laptop has no disk in any sense
 *   this driver understands: its system sits upon an embedded MultiMediaCard
 *   part behind an SD host controller, which the specification classes as a
 *   system peripheral, and its removable storage sits behind a USB controller.
 *   Such a machine carries no mass-storage controller at all, and no setting in
 *   its firmware will produce one. The report names those paths too, because
 *   the alternative was to tell somebody holding a working laptop that their
 *   machine has no disk. Since sub-tasks 4.7 and 4.8 both of the storage paths
 *   it names are driven — AHCI by drivers/ahci, the SD host controller by
 *   drivers/sdhci — so this report's business with them is to say which driver
 *   has them and stand aside. A USB drive is still reached by nothing.
 */

#include "internal.h"

#include <oxys/ata.h>
#include <oxys/kernel.h>
#include <oxys/io.h>
#include <oxys/block.h>
#include <oxys/pci.h>

/* The command block registers, the status and control bits, the commands, the
 * words of the identification data, the poll limit and the settling delay are
 * all declared in internal.h, every part of the driver needing them. */

/* The four addresses, and what was found at each. */
AtaDevice AtaDevices[ATA_DEVICE_COUNT];
size_t AtaPresent;

/*
 * The device presently selected upon each channel, so that a selection already
 * in force is not repeated: a redundant selection costs the 400 nanoseconds that
 * must follow it, upon every sector of every transfer.
 */
uint8_t AtaSelected[ATA_CHANNEL_COUNT];
bool AtaSelectionKnown[ATA_CHANNEL_COUNT];

/* Accounting. */
uint64_t AtaRead64;
uint64_t AtaWritten64;
uint64_t AtaCommands;
uint64_t AtaErrors;
uint64_t AtaRejections;
uint64_t AtaTimeouts;

/* The description of the most recent failure. */
const char *AtaError = "none";


/* Records a failure, so that a report may say what went wrong and not only that. */
bool AtaFail(const char *reason)
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
bool AtaReject(const char *reason)
{
    AtaError = reason;
    ++AtaRejections;
    return false;
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

    /*
     * Where the channels answer is established before any of them is addressed.
     * The bus was enumerated before this driver ran, which is the ordering
     * kernel.c fixes and the reason this may consult it.
     */
    AtaLocateChannels();

    for (uint8_t channel = 0U; channel < ATA_CHANNEL_COUNT; ++channel)
    {
        for (uint8_t drive = 0U; drive < ATA_DRIVE_COUNT; ++drive)
        {
            AtaDevice *const device = &AtaDevices[(channel * ATA_DRIVE_COUNT) + drive];

            device->kind = ATA_DEVICE_NONE;
            device->channel = channel;
            device->drive = drive;
            device->io_base = AtaChannelIoBase[channel];
            device->control_base = AtaChannelControlBase[channel];
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

/*
 * The operations by which the block layer reaches this driver. The context is
 * the AtaDevice the layer was given at registration, so the adaptor is a cast
 * and a call: the validation of the request has already been performed above,
 * and repeating it here would be two statements of one rule.
 */
static bool AtaBlockRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    return AtaRead((const AtaDevice *)context, block, count, buffer);
}

static bool AtaBlockWrite(void *context, uint64_t block, uint32_t count, const void *buffer)
{
    return AtaWrite((const AtaDevice *)context, block, count, buffer);
}

static const BlockOperations AtaBlockOperations = { AtaBlockRead, AtaBlockWrite };

size_t AtaRegisterBlockDevices(void)
{
    static char names[ATA_DEVICE_COUNT][5] = { "ata0", "ata1", "ata2", "ata3" };
    size_t registered = 0U;

    for (size_t position = 0U; position < ATA_DEVICE_COUNT; ++position)
    {
        AtaDevice *const device = &AtaDevices[position];

        /*
         * Only a disk is registered. A packet device answered the enumeration
         * and is reported, but this driver cannot read one, and a block device
         * whose every transfer fails is worse than an absent one.
         */
        if ((device->kind != ATA_DEVICE_ATA) || (device->sector_count == 0U))
        {
            continue;
        }

        if (BlockRegister(names[position], &AtaBlockOperations, device, ATA_SECTOR_SIZE,
                          device->sector_count, false) != NULL)
        {
            ++registered;
        }
    }

    return registered;
}

