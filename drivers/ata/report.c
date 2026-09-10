/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/report.c
 * Purpose: Emits the driver's report upon the diagnostic path: what was found at
 *          each of the four addresses, where each channel was reached, the
 *          accounting, and — for a machine upon which no disk was found — every
 *          controller the bus carries, why this driver did not reach each, and
 *          the firmware setting that would make the disks visible where one
 *          exists.
 * Key functions: AtaReport, AtaReportControllers, AtaReportMassStorage,
 *          AtaReportForeignStorage, AtaReportAddressing, AtaKindName.
 * References:
 *   - PCI Local Bus Specification 3.0, the class codes: 0x01 subclass 0x01 is an
 *     IDE controller, subclass 0x06 an AHCI one, and class 0x08 subclass 0x05 an
 *     SD host controller — the last being a system peripheral and not mass
 *     storage at all, which is why a machine whose system is upon an embedded
 *     MultiMediaCard part carries no mass-storage controller whatever.
 *   - docs/storage/DISK.md, Sections 2.2, 2.3, 7.2 and 7.3: the reason this
 *     report is as long as it is.
 *
 * Why a driver has a report of this size.
 *
 *   The symptom of a controller in AHCI mode, of a controller in native mode
 *   whose addresses were never read, and of a machine with no disk at all are
 *   indistinguishable: this driver finds nothing. Reported as "no disk", all
 *   three are the same sentence, and two of them are false. So the report names
 *   what the bus carries and says of each why it was not reached — because that
 *   is the difference between a person changing one firmware setting and a
 *   person concluding their machine is unsupported.
 */

#include "internal.h"

#include <oxys/io.h>
#include <oxys/kernel.h>
#include <oxys/pit.h>
#include <oxys/block.h>

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

/*
 * States where each channel was addressed, and how that was decided.
 *
 * This is printed whether or not anything answered, because the address is the
 * first thing a person diagnosing a missing disk needs and the last thing they
 * can obtain otherwise.
 */
static void AtaReportAddressing(void)
{
    for (uint8_t channel = 0U; channel < ATA_CHANNEL_COUNT; ++channel)
    {
        KernelWriteString("ATA: ");
        KernelWriteString((channel == 0U) ? "primary" : "secondary");
        KernelWriteString(" channel at ");
        KernelWriteHexadecimal((uint64_t)AtaChannelIoBase[channel]);
        KernelWriteString(", control ");
        KernelWriteHexadecimal((uint64_t)AtaChannelControlBase[channel]);
        KernelWriteString(AtaChannelIsNative[channel]
                              ? ", from the controller's base address registers.\n"
                              : ", the compatibility address.\n");
    }
}

/*
 * Says what mass-storage controllers the machine has, and why this driver did
 * not reach them. Reports whether it found any.
 *
 * It is printed only where nothing answered, and it exists because that case had
 * exactly one symptom for two quite different causes. A machine with no disk and
 * a machine whose disks are behind a controller this driver cannot speak to both
 * reported "no device answered upon either channel", and the second is by far
 * the commoner upon anything made in the last fifteen years: a firmware that
 * presents its SATA controller in AHCI mode puts the disks somewhere this driver
 * has no way to look.
 *
 * The remedy is named as well as the cause. It is not this kernel's to apply —
 * an AHCI driver is a sub-task of its own — but it is within the reach of
 * whoever is standing at the machine, most firmware offering the choice.
 */
static bool AtaReportMassStorage(void)
{
    size_t position = 0U;
    size_t found_at = 0U;
    const PciFunction *function;
    bool any = false;

    while ((function = PciFindByClass(PCI_CLASS_MASS_STORAGE, PCI_CLASS_ANY_SUBCLASS,
                                      position, &found_at)) != NULL)
    {
        any = true;
        position = found_at + 1U;

        KernelWriteString("ATA:   ");
        KernelWriteString(PciClassName(function->class_code, function->subclass));
        KernelWriteString(", interface ");
        KernelWriteHexadecimal((uint64_t)function->programming_interface);
        KernelWriteString(": ");

        if (function->subclass == PCI_SUBCLASS_IDE)
        {
            /* This driver does speak to it, so nothing answering is the
             * ordinary case of a controller with no disk attached. */
            KernelWriteString("driven by this driver; no disk is attached to it.\n");
        }
        else if ((function->subclass == PCI_SUBCLASS_SATA) &&
                 (function->programming_interface == PCI_SATA_INTERFACE_AHCI))
        {
            KernelWriteString("an AHCI controller. Its registers are memory-mapped "
                              "and it answers at no I/O port, so it is driven by the "
                              "AHCI driver and not by this one.\n");
        }
        else if ((function->subclass == PCI_SUBCLASS_NVM) &&
                 (function->programming_interface == PCI_NVM_INTERFACE_NVME))
        {
            KernelWriteString("an NVM Express controller, which is not an ATA device "
                              "at all.\n");
        }
        else
        {
            KernelWriteString("not an ATA controller this driver addresses.\n");
        }
    }

    return any;
}

/*
 * Names the storage the machine carries outside the mass-storage class, and
 * reports whether it found any.
 *
 * The whole recorded table is walked rather than searched twice, because the
 * question asked of each function is one question — AtaClassifyForeignStorage —
 * and asking it of everything is both shorter and the same work.
 *
 * This exists because the report was wrong upon a real machine and wrong in the
 * most misleading direction. An inexpensive laptop carries its system upon an
 * embedded MultiMediaCard part and boots this kernel from a USB drive, and has
 * no mass-storage controller whatever; the report told its owner that the
 * machine had no disk. It has two kinds of storage. Neither is reachable
 * through the ATA command block registers, and no firmware setting will make
 * them so — the remedy is a driver, and saying that is the whole point.
 */
static bool AtaReportForeignStorage(void)
{
    const size_t count = PciFunctionCount();
    bool any = false;

    for (size_t index = 0U; index < count; ++index)
    {
        const PciFunction *const function = PciFunctionAt(index);
        const AtaForeignStorage kind = AtaClassifyForeignStorage(function);

        if (kind == ATA_FOREIGN_STORAGE_NONE)
        {
            continue;
        }

        any = true;

        KernelWriteString("ATA:   ");
        KernelWriteString(PciClassName(function->class_code, function->subclass));
        KernelWriteString(", interface ");
        KernelWriteHexadecimal((uint64_t)function->programming_interface);
        KernelWriteString(": ");
        KernelWriteString((kind == ATA_FOREIGN_STORAGE_SD)
                              ? "where an embedded MultiMediaCard or a card in a slot "
                                "is attached. It is not an ATA device, and is driven by "
                                "the SD host controller driver.\n"
                              : "where a USB drive is attached. It is not an ATA "
                                "device and has no command block registers.\n");
    }

    return any;
}

/*
 * Says what storage the machine has and why none of it answered, whether or not
 * any of it is of a class this driver could ever have driven.
 */
static void AtaReportControllers(void)
{
    const bool mass_storage = AtaReportMassStorage();
    const bool foreign = AtaReportForeignStorage();

    if (!mass_storage && !foreign)
    {
        KernelWriteString("ATA:   the bus carries no storage controller of any "
                          "class.\n");
        return;
    }

    if (mass_storage)
    {
        KernelWriteString("ATA: this driver reads and writes through the ATA command "
                          "block registers,\n");
        KernelWriteString("ATA: which is an IDE controller and nothing else. An AHCI "
                          "controller named\n");
        KernelWriteString("ATA: above is reached by the AHCI driver instead, whose "
                          "report follows this\n");
        KernelWriteString("ATA: one and says what it found upon each of its ports.\n");
        return;
    }

    KernelWriteString("ATA: this machine has no mass-storage controller at all, so "
                      "there is no\n");
    KernelWriteString("ATA: firmware setting that would present its storage as a disk. "
                      "What it\n");
    KernelWriteString("ATA: does have is named above, and the report of whichever driver "
                      "reaches\n");
    KernelWriteString("ATA: it follows this one. A USB drive is reached by nothing yet.\n");
}

void AtaReport(void)
{
    AtaReportAddressing();

    if (AtaPresent == 0U)
    {
        KernelWriteString("ATA: no device answered upon either channel.\n");
        AtaReportControllers();
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
