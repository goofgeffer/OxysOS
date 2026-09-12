/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/channel.c
 * Purpose: Establishes where each channel actually answers: the walk of the bus
 *          for a controller, the reading of the base address registers of one in
 *          native mode, the fall back to the addresses the IBM Personal Computer
 *          AT fixed for one in compatibility mode, and the classification of
 *          storage this driver cannot reach at all.
 * Key functions: AtaLocateChannels, AtaChannelAddressesFor,
 *          AtaClassifyForeignStorage.
 * References:
 *   - PCI Local Bus Specification 3.0, the class code 0x01 subclass 0x01
 *     programming interface byte: bit 0 states whether the primary channel is in
 *     native mode and bit 2 the secondary, and a channel in native mode answers
 *     at the addresses its base address registers give rather than at 0x1F0 or
 *     0x170.
 *   - The same, the base address registers: the control block of a native
 *     channel lies at offset 2 within the second register of its pair, and not
 *     at its base.
 *   - docs/storage/DISK.md, Sections 2.1 to 2.3: why this file exists in the
 *     shape it does. A machine that boots this kernel found no disk upon it, and
 *     the cause was not a fault in the transfer path — it was a controller this
 *     driver never looked in the right place for, and then a controller of a
 *     class this driver cannot drive at all. The report must say which, because
 *     the two have identical symptoms and only one of them has a remedy within
 *     reach of whoever is standing at the machine.
 */

#include "internal.h"

#include <oxys/dev/io.h>
#include <oxys/kernel.h>
#include <oxys/dev/pit.h>
#include <oxys/block/block.h>

/*
 * Where each channel actually answers, and how that was established.
 *
 * These begin at the compatibility addresses and are replaced where a PCI IDE
 * controller declares a channel to be in native mode. They are held rather than
 * recomputed because the report must be able to say what was used: a driver that
 * found nothing at an address nobody can see is indistinguishable from a driver
 * with a fault.
 */
uint16_t AtaChannelIoBase[ATA_CHANNEL_COUNT];
uint16_t AtaChannelControlBase[ATA_CHANNEL_COUNT];
bool AtaChannelIsNative[ATA_CHANNEL_COUNT];

/* The IDE controller the addresses came from, if any. */
const PciFunction *AtaController;

/*
 * Establishes where the two channels answer, before anything is asked of them.
 *
 * The compatibility addresses are the default and are correct for every machine
 * whose IDE controller is in compatibility mode, which is every machine this
 * kernel had been tried upon before this routine existed. A controller in native
 * mode answers at its base address registers instead — the same registers in the
 * same order, at an address the firmware assigned — and probing the
 * compatibility addresses then finds nothing, which is the whole of the failure.
 *
 * A base address register that does not describe I/O ports, or that describes
 * port zero, is disregarded and the channel left at its compatibility address.
 * Such a register is a controller declaring native mode without having been
 * given an address, and following it would put commands to an arbitrary port.
 */
AtaForeignStorage AtaClassifyForeignStorage(const PciFunction *function)
{
    if (function == NULL)
    {
        return ATA_FOREIGN_STORAGE_NONE;
    }

    /*
     * Only the class and subclass are read. The programming interface of an SD
     * host controller distinguishes the standard register interface from a
     * vendor's own, and that of a USB controller distinguishes the four host
     * controller interfaces; neither distinction changes the one thing said
     * here, which is that this driver does not reach it.
     */
    if ((function->class_code == PCI_CLASS_SYSTEM_PERIPHERAL) &&
        (function->subclass == PCI_SUBCLASS_SD_HOST))
    {
        return ATA_FOREIGN_STORAGE_SD;
    }

    if ((function->class_code == PCI_CLASS_SERIAL_BUS) &&
        (function->subclass == PCI_SUBCLASS_USB))
    {
        return ATA_FOREIGN_STORAGE_USB;
    }

    return ATA_FOREIGN_STORAGE_NONE;
}

bool AtaChannelAddressesFor(const PciFunction *function, uint8_t channel, uint16_t *io_base,
                            uint16_t *control_base)
{
    const uint8_t native_bit =
        (channel == 0U) ? PCI_IDE_PRIMARY_NATIVE : PCI_IDE_SECONDARY_NATIVE;
    const size_t command_bar = (channel == 0U) ? 0U : 2U;
    const size_t control_bar = command_bar + 1U;
    uint64_t command_address;
    uint64_t control_address;

    if ((function == NULL) || (io_base == NULL) || (control_base == NULL) ||
        (channel >= ATA_CHANNEL_COUNT))
    {
        return false;
    }

    /* Only an IDE controller has these bits; the programming interface of any
     * other subclass means something else entirely, and reading it as this would
     * be reading a field that was never written. */
    if ((function->class_code != PCI_CLASS_MASS_STORAGE) ||
        (function->subclass != PCI_SUBCLASS_IDE))
    {
        return false;
    }

    if ((function->programming_interface & native_bit) == 0U)
    {
        return false;
    }

    if (!PciBarIsIoPort(function, command_bar) || !PciBarIsIoPort(function, control_bar))
    {
        return false;
    }

    command_address = PciBarBase(function, command_bar);
    control_address = PciBarBase(function, control_bar);

    /*
     * A register declaring native mode without having been given an address, or
     * an address beyond the 16-bit port space, is disregarded. Following either
     * would put commands to an arbitrary port, which is worse than not finding
     * the disk: an arbitrary port belongs to some other device.
     */
    if ((command_address == 0U) || (control_address == 0U) ||
        (command_address > UINT64_C(0xFFFF)) || (control_address > UINT64_C(0xFFFD)))
    {
        return false;
    }

    *io_base = (uint16_t)command_address;

    /*
     * The control block register lies at offset 2 within the four bytes the
     * control base address register describes, and not at its start. A driver
     * that took the base itself would write the device control register to a
     * reserved port: the software reset would do nothing and the device's
     * interrupt would never be disabled, so the channel would appear to work
     * until something raised IRQ14 that nothing had claimed.
     */
    *control_base = (uint16_t)(control_address + 2U);

    return true;
}

void AtaLocateChannels(void)
{
    size_t found_at = 0U;

    for (uint8_t channel = 0U; channel < ATA_CHANNEL_COUNT; ++channel)
    {
        AtaChannelIoBase[channel] =
            (channel == 0U) ? ATA_PRIMARY_IO_BASE : ATA_SECONDARY_IO_BASE;
        AtaChannelControlBase[channel] =
            (channel == 0U) ? ATA_PRIMARY_CONTROL_BASE : ATA_SECONDARY_CONTROL_BASE;
        AtaChannelIsNative[channel] = false;
    }

    AtaController = PciFindByClass(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_IDE, 0U, &found_at);

    if (AtaController == NULL)
    {
        return;
    }

    for (uint8_t channel = 0U; channel < ATA_CHANNEL_COUNT; ++channel)
    {
        uint16_t io_base;
        uint16_t control_base;

        if (AtaChannelAddressesFor(AtaController, channel, &io_base, &control_base))
        {
            AtaChannelIoBase[channel] = io_base;
            AtaChannelControlBase[channel] = control_base;
            AtaChannelIsNative[channel] = true;
        }
    }
}

