/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/verify_storage.c
 * Purpose: Asserts the storage stack of Phase 4: the ATA and AHCI drivers, the
 *          generic block-device layer above them, and the buffer cache above that.
 * Key functions: KernelVerifyAta, KernelVerifyAhci, KernelVerifyBlock,
 *          KernelVerifyBuffer.
 * References:
 *   - docs/storage/DISK.md, docs/storage/AHCI.md, docs/storage/BLOCK.md and
 *     docs/storage/BUFFER.md: each has a verification section pairing the
 *     assertions below with the silent failure each would catch.
 *
 * The block and buffer assertions are made against the memory-backed devices of
 * `kernel/test/volume.h`, so they hold upon a machine with no disk. The ATA and
 * AHCI assertions cannot be, a driver for a device being untestable
 * without one; where no device answers, that is reported and nothing is
 * asserted. The write path is exercised only when the boot loader's command
 * line asks for it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include "volume.h"
#include <oxys/ata.h>
#include <oxys/ahci.h>
#include <oxys/sdhci.h>
#include <oxys/block.h>
#include <oxys/buffer.h>
#include <oxys/pci.h>

/*
 * The buffers the disk self-test reads into. They are of static storage duration
 * because the boot stack is 64 KiB and three sectors of it would be a
 * disproportionate share of what remains after the self-tests above.
 */
static uint8_t KernelDiskBufferA[ATA_SECTOR_SIZE * 2U];
static uint8_t KernelDiskBufferB[ATA_SECTOR_SIZE * 2U];

/* True if two regions hold the same bytes. */
static bool KernelRegionsMatch(const uint8_t *left, const uint8_t *right, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the disk driver addresses the sector it was asked for and
 * transfers exactly its contents.
 *
 * The failure this guards against is the worst kind the kernel has yet had to
 * consider: a driver that reads the wrong sector returns data, and data that
 * arrived is indistinguishable from data that is correct until something tries
 * to interpret it. An address composed with a byte in the wrong register, a
 * transfer of 255 words instead of 256, a second sector written over the first —
 * each of these produces a disk that appears to work and a filesystem that
 * decays. Every assertion below is chosen to make one of those visible.
 *
 * The test reads. It writes only when the operator has asked for it upon the
 * command line, and then only to a sector whose previous contents it has read
 * and restores afterwards: a self-test that wrote to a disk unbidden would
 * destroy the data of anybody who booted this kernel upon their own machine.
 */
/*
 * Composes a PCI configuration header for an IDE controller, so that the
 * addressing decision may be asserted upon headers no board here presents.
 *
 * Every field the decision reads is set and none other. The base address
 * registers carry their low bit set, which is what marks a register as
 * describing I/O ports rather than memory; the address occupies the bits above
 * the two the specification reserves.
 */
static PciFunction KernelComposeIdeController(uint8_t programming_interface, uint32_t bar0,
                                              uint32_t bar1, uint32_t bar2, uint32_t bar3)
{
    PciFunction function;

    function.address.bus = 0U;
    function.address.device = 31U;
    function.address.function = 1U;
    function.vendor_id = 0x8086U;
    function.device_id = 0x7010U;
    function.class_code = PCI_CLASS_MASS_STORAGE;
    function.subclass = PCI_SUBCLASS_IDE;
    function.programming_interface = programming_interface;
    function.revision = 0U;
    function.header_type = 0U;
    function.multifunction = false;
    function.interrupt_line = 0U;
    function.interrupt_pin = 0U;

    for (size_t index = 0U; index < PCI_BAR_COUNT; ++index)
    {
        function.base_address[index] = 0U;
    }

    function.base_address[0] = bar0;
    function.base_address[1] = bar1;
    function.base_address[2] = bar2;
    function.base_address[3] = bar3;

    return function;
}

/* An I/O base address register naming the given port. Bit 0 marks the space. */
static uint32_t KernelIoBar(uint16_t port)
{
    return (uint32_t)port | 1U;
}

/*
 * Asserts how the driver decides where a channel answers.
 *
 * This exists because the failure it guards against was reported from a real
 * machine and could not be reproduced upon any board available here. A channel
 * in native PCI mode answers at the addresses its base address registers give
 * and at no others, and a driver that probed the compatibility addresses
 * regardless finds nothing — which is indistinguishable, in every symptom, from
 * a machine that has no disk.
 *
 * Every board this project can be run upon uses the compatibility addresses, so
 * there is nothing here to probe. The decision is therefore a pure function of a
 * configuration header, and headers no machine here has are composed and asked
 * about. That is the only alternative to writing the arithmetic and hoping, and
 * hoping is what produced the fault being corrected.
 */
static bool KernelVerifyAtaAddressing(void)
{
    bool succeeded = true;
    uint16_t io_base = 0U;
    uint16_t control_base = 0U;
    PciFunction function;

    /* --- A controller in compatibility mode yields nothing, whatever its BARs. --- */

    /*
     * The BARs are set to plausible addresses deliberately. A driver that read
     * them without consulting the programming interface would take these and
     * would then probe ports the controller does not decode — upon a machine
     * whose disks were at 0x1F0 all along.
     */
    function = KernelComposeIdeController(0x80U, KernelIoBar(0xC000U), KernelIoBar(0xC008U),
                                          KernelIoBar(0xC010U), KernelIoBar(0xC018U));

    if (AtaChannelAddressesFor(&function, 0U, &io_base, &control_base) ||
        AtaChannelAddressesFor(&function, 1U, &io_base, &control_base))
    {
        KernelWriteString("  A channel in compatibility mode was moved to its base "
                          "address registers.\n");
        succeeded = false;
    }

    /* --- A channel in native mode yields its own addresses. --- */

    function = KernelComposeIdeController(
        (uint8_t)(PCI_IDE_PRIMARY_NATIVE | PCI_IDE_SECONDARY_NATIVE), KernelIoBar(0xC000U),
        KernelIoBar(0xC008U), KernelIoBar(0xC010U), KernelIoBar(0xC018U));

    if (!AtaChannelAddressesFor(&function, 0U, &io_base, &control_base))
    {
        KernelWriteString("  A channel in native mode was left at the compatibility "
                          "address.\n");
        succeeded = false;
    }
    else if ((io_base != 0xC000U) || (control_base != 0xC00AU))
    {
        /*
         * The control block register is at offset 2 within the four bytes the
         * control register describes, not at its start. A driver that took the
         * base itself would write the device control register to a reserved
         * port: the software reset would do nothing and the device's interrupt
         * would never be disabled, so the channel would appear to work until
         * something raised IRQ14 that nothing had claimed.
         */
        KernelWriteString("  A native channel's command or control address is wrong.\n");
        succeeded = false;
    }

    if (!AtaChannelAddressesFor(&function, 1U, &io_base, &control_base))
    {
        KernelWriteString("  The secondary channel in native mode was left at the "
                          "compatibility address.\n");
        succeeded = false;
    }
    else if ((io_base != 0xC010U) || (control_base != 0xC01AU))
    {
        /* The secondary channel reads the third and fourth registers. Taking the
         * first pair for both channels is the obvious slip, and would put both
         * channels' commands to the primary's ports. */
        KernelWriteString("  The secondary channel read the primary's base address "
                          "registers.\n");
        succeeded = false;
    }

    /* --- One channel native and the other not. --- */

    function = KernelComposeIdeController(PCI_IDE_SECONDARY_NATIVE, KernelIoBar(0xC000U),
                                          KernelIoBar(0xC008U), KernelIoBar(0xC010U),
                                          KernelIoBar(0xC018U));

    if (AtaChannelAddressesFor(&function, 0U, &io_base, &control_base))
    {
        KernelWriteString("  A compatibility channel was moved because the other was "
                          "native.\n");
        succeeded = false;
    }

    if (!AtaChannelAddressesFor(&function, 1U, &io_base, &control_base))
    {
        KernelWriteString("  A native channel was left behind because the other was "
                          "not.\n");
        succeeded = false;
    }

    /* --- A malformed declaration is refused rather than followed. --- */

    /*
     * A controller declaring native mode with no address assigned, or with a
     * register describing memory rather than ports, is refused. Following either
     * would put ATA commands to an arbitrary port — and an arbitrary port belongs
     * to some other device, so the failure would not be a missing disk but
     * whatever that device does when written to.
     */
    function = KernelComposeIdeController(PCI_IDE_PRIMARY_NATIVE, 1U, 1U, 0U, 0U);

    if (AtaChannelAddressesFor(&function, 0U, &io_base, &control_base))
    {
        KernelWriteString("  A native channel with no address assigned was followed.\n");
        succeeded = false;
    }

    function = KernelComposeIdeController(PCI_IDE_PRIMARY_NATIVE, 0xF0000000U, 0xF0001000U,
                                          0U, 0U);

    if (AtaChannelAddressesFor(&function, 0U, &io_base, &control_base))
    {
        KernelWriteString("  A memory base address register was read as I/O ports.\n");
        succeeded = false;
    }

    /* --- A controller of another subclass has no such bits to read. --- */

    /*
     * The programming interface of an AHCI controller is 0x01, which happens to
     * be the same bit that marks an IDE primary channel as native. A driver that
     * read it without checking the subclass would take an AHCI controller's first
     * two base address registers — which describe memory — as I/O ports. This is
     * not hypothetical: 0x01 is exactly what the controller in this project's own
     * QEMU board reports.
     */
    function = KernelComposeIdeController(PCI_SATA_INTERFACE_AHCI, KernelIoBar(0xC000U),
                                          KernelIoBar(0xC008U), 0U, 0U);
    function.subclass = PCI_SUBCLASS_SATA;

    if (AtaChannelAddressesFor(&function, 0U, &io_base, &control_base))
    {
        KernelWriteString("  An AHCI controller's interface byte was read as an IDE "
                          "controller's.\n");
        succeeded = false;
    }

    /* --- Arguments that name nothing are refused. --- */

    if (AtaChannelAddressesFor(NULL, 0U, &io_base, &control_base) ||
        AtaChannelAddressesFor(&function, 2U, &io_base, &control_base))
    {
        KernelWriteString("  An impossible argument was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

/*
 * Composes a PCI configuration header of an arbitrary class, for the storage
 * that is not of the mass-storage class at all.
 */
static PciFunction KernelComposeFunction(uint8_t class_code, uint8_t subclass,
                                         uint8_t programming_interface)
{
    PciFunction function = KernelComposeIdeController(0U, 0U, 0U, 0U, 0U);

    function.class_code = class_code;
    function.subclass = subclass;
    function.programming_interface = programming_interface;

    return function;
}

/*
 * Asserts that the storage a machine carries outside the mass-storage class is
 * recognised as storage.
 *
 * The failure this guards against was reported from a real machine and is worse
 * than silence. An inexpensive laptop keeps its system upon an embedded
 * MultiMediaCard part behind an SD host controller and boots from a USB drive,
 * and carries no mass-storage controller at all. The report searched the
 * mass-storage class, found nothing, and announced that the machine had no
 * disk — to somebody holding a laptop that had just booted from its own
 * storage. That sends a person to look for a fault in hardware that has none.
 *
 * There is no board available here that presents an SD host controller, so as
 * with the channel addressing the decision is a pure function of a configuration
 * header and headers this project cannot obtain are composed and asked about.
 */
static bool KernelVerifyAtaForeignStorage(void)
{
    bool succeeded = true;
    PciFunction function;

    /* --- An SD host controller is where an eMMC part is. --- */

    function = KernelComposeFunction(PCI_CLASS_SYSTEM_PERIPHERAL, PCI_SUBCLASS_SD_HOST, 0x01U);

    if (AtaClassifyForeignStorage(&function) != ATA_FOREIGN_STORAGE_SD)
    {
        KernelWriteString("  An SD host controller was not recognised as storage.\n");
        succeeded = false;
    }

    /* --- A USB controller is where a removable drive is. --- */

    function = KernelComposeFunction(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, 0x30U);

    if (AtaClassifyForeignStorage(&function) != ATA_FOREIGN_STORAGE_USB)
    {
        KernelWriteString("  A USB controller was not recognised as storage.\n");
        succeeded = false;
    }

    /*
     * --- The subclass is read against its own class and no other. ---
     *
     * Subclass 0x05 is an SD host controller under the system-peripheral class,
     * an ATA controller under the mass-storage class, and an SMBus controller
     * under the serial-bus class. A classifier that read the subclass alone
     * would report the machine's SMBus as a place its disks might be.
     */
    function = KernelComposeFunction(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_SD_HOST, 0U);

    if (AtaClassifyForeignStorage(&function) != ATA_FOREIGN_STORAGE_NONE)
    {
        KernelWriteString("  An SMBus controller was taken for storage.\n");
        succeeded = false;
    }

    function = KernelComposeFunction(PCI_CLASS_SYSTEM_PERIPHERAL, PCI_SUBCLASS_USB, 0U);

    if (AtaClassifyForeignStorage(&function) != ATA_FOREIGN_STORAGE_NONE)
    {
        KernelWriteString("  A system peripheral was taken for a USB controller.\n");
        succeeded = false;
    }

    /*
     * --- What this driver's own class holds is not foreign to it. ---
     *
     * An IDE controller counted here would be counted twice in the report, and
     * the paragraph naming the firmware remedy is chosen by whether anything of
     * the mass-storage class was found. Counting one there would print the
     * remedy for a machine that has none.
     */
    function = KernelComposeFunction(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_IDE, 0U);

    if (AtaClassifyForeignStorage(&function) != ATA_FOREIGN_STORAGE_NONE)
    {
        KernelWriteString("  An IDE controller was reported as beyond this driver's "
                          "class.\n");
        succeeded = false;
    }

    function = KernelComposeFunction(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                                     PCI_SATA_INTERFACE_AHCI);

    if (AtaClassifyForeignStorage(&function) != ATA_FOREIGN_STORAGE_NONE)
    {
        KernelWriteString("  An AHCI controller was counted outside its own class.\n");
        succeeded = false;
    }

    /* --- A function that names nothing is not storage. --- */

    if (AtaClassifyForeignStorage(NULL) != ATA_FOREIGN_STORAGE_NONE)
    {
        KernelWriteString("  A function that names nothing was taken for storage.\n");
        succeeded = false;
    }

    return succeeded;
}

void KernelVerifyAta(void)
{
    const AtaDevice *const disk = AtaFirstDisk();
    bool succeeded = KernelVerifyAtaAddressing();

    /*
     * The addressing verdict is announced on its own, before anything that
     * depends upon a device answering. It is the one part of this test that runs
     * upon every machine, and a reader who saw only "nothing to assert" below
     * would have no way to know it had run at all.
     */
    KernelWriteString(succeeded ? "Disk self-test: channel addressing is sound.\n"
                                : "Disk self-test FAILED: channel addressing.\n");

    succeeded = KernelVerifyAtaForeignStorage();
    KernelWriteString(succeeded
                          ? "Disk self-test: storage outside this class is recognised.\n"
                          : "Disk self-test FAILED: storage outside this class.\n");

    if (AtaDeviceCount() == 0U)
    {
        KernelWriteString("Disk self-test: no device answered; nothing to assert.\n");
        return;
    }

    if (disk == NULL)
    {
        KernelWriteString("Disk self-test: devices answered but none is a disk.\n");
        return;
    }

    /* An identification that yielded no capacity was not understood. */
    if ((disk->sector_count == 0U) || (disk->model[0] == '\0'))
    {
        KernelWriteString("  The identification data yielded no capacity or model.\n");
        succeeded = false;
    }

    /* The first sector must be readable, and must read the same way twice. */
    if (!AtaRead(disk, 0U, 1U, KernelDiskBufferA))
    {
        KernelWriteString("  The first sector could not be read: ");
        KernelWriteString(AtaLastError());
        KernelWriteString("\n");
        succeeded = false;
    }
    else if (!AtaRead(disk, 0U, 1U, KernelDiskBufferB) ||
             !KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
    {
        KernelWriteString("  The same sector read differently upon a second attempt.\n");
        succeeded = false;
    }

    /*
     * A two-sector read must place the second sector after the first, and the
     * first must be what a one-sector read of the same address returned. A
     * driver that lost synchronisation between sectors, or that overwrote the
     * first with the second, passes every other assertion here.
     */
    if (disk->sector_count >= 2U)
    {
        if (!AtaRead(disk, 0U, 2U, KernelDiskBufferB))
        {
            KernelWriteString("  A two-sector read failed.\n");
            succeeded = false;
        }
        else
        {
            if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
            {
                KernelWriteString("  A two-sector read did not begin where a one-sector "
                                  "read did.\n");
                succeeded = false;
            }

            if (!AtaRead(disk, 1U, 1U, KernelDiskBufferA) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[ATA_SECTOR_SIZE],
                                    ATA_SECTOR_SIZE))
            {
                KernelWriteString("  The second sector of a two-sector read is not the "
                                  "sector that follows.\n");
                succeeded = false;
            }
        }
    }

    /* A range beyond the capacity is refused rather than attempted. */
    if (AtaRead(disk, disk->sector_count, 1U, KernelDiskBufferA) ||
        AtaRead(disk, disk->sector_count - 1U, 2U, KernelDiskBufferA))
    {
        KernelWriteString("  A read beyond the capacity of the device was accepted.\n");
        succeeded = false;
    }

    /* A request without a buffer, and one for no sectors, are both harmless. */
    if (AtaRead(disk, 0U, 1U, NULL) || !AtaRead(disk, 0U, 0U, KernelDiskBufferA))
    {
        KernelWriteString("  A degenerate request was mishandled.\n");
        succeeded = false;
    }

    /*
     * A device larger than 28 bits can name exercises the 48-bit commands, which
     * are otherwise never reached. The register writing they require is
     * different in kind and not merely in width — each register is written
     * twice, high-order byte first — so a driver that has never issued one has
     * not been tested at all in that mode.
     */
    if (disk->supports_lba48 && (disk->sector_count > ATA_LBA28_LIMIT))
    {
        if (!AtaRead(disk, ATA_LBA28_LIMIT + 1U, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  A sector beyond the 28-bit limit could not be read: ");
            KernelWriteString(AtaLastError());
            KernelWriteString("\n");
            succeeded = false;
        }
    }

    /*
     * The write path, only upon request. The sector is read, overwritten with a
     * pattern, read back, compared, and then restored from what was read; the
     * restoration is verified in its turn, since a test that damaged the disk
     * and reported success would be worse than no test.
     */
    if (KernelCommandLineHasOption("disk-write-test"))
    {
        const uint64_t target = disk->sector_count - 1U;

        KernelWriteString("  Writing to the final sector, as the command line permits.\n");

        if (!AtaRead(disk, target, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  The sector to be written could not first be read.\n");
            succeeded = false;
        }
        else
        {
            for (size_t index = 0U; index < ATA_SECTOR_SIZE; ++index)
            {
                KernelDiskBufferB[index] = (uint8_t)(index ^ 0xA5U);
            }

            if (!AtaWrite(disk, target, 1U, KernelDiskBufferB))
            {
                KernelWriteString("  The pattern could not be written: ");
                KernelWriteString(AtaLastError());
                KernelWriteString("\n");
                succeeded = false;
            }
            else if (!AtaRead(disk, target, 1U, &KernelDiskBufferB[ATA_SECTOR_SIZE]))
            {
                KernelWriteString("  The pattern could not be read back.\n");
                succeeded = false;
            }
            else
            {
                for (size_t index = 0U; index < ATA_SECTOR_SIZE; ++index)
                {
                    if (KernelDiskBufferB[ATA_SECTOR_SIZE + index] != (uint8_t)(index ^ 0xA5U))
                    {
                        KernelWriteString("  The pattern read back altered.\n");
                        succeeded = false;
                        break;
                    }
                }
            }

            /* Whatever happened above, the sector is put back as it was found. */
            if (!AtaWrite(disk, target, 1U, KernelDiskBufferA) ||
                !AtaRead(disk, target, 1U, KernelDiskBufferB) ||
                !KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
            {
                KernelWriteString("  The sector was not restored to its previous contents.\n");
                succeeded = false;
            }
        }
    }

    if (AtaTimeoutCount() != 0U)
    {
        KernelWriteString("  A device failed to respond within the driver's patience.\n");
        succeeded = false;
    }

    /*
     * Every refusal above was provoked deliberately; an error is a failure of the
     * hardware and none was expected.
     */
    if (AtaErrorCount() != 0U)
    {
        KernelWriteString("  A device reported an error: ");
        KernelWriteString(AtaLastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Disk self-test passed.\n" : "Disk self-test FAILED.\n");
}

/*
 * Asserts the three decisions of the AHCI driver that are pure, and then the
 * transfers themselves where a disk answered.
 *
 * The pure three are exposed for the same reason the ATA driver's addressing is:
 * no board available to this project presents a packet device, an enclosure or a
 * port multiplier upon an AHCI port, and none presents a port whose interface
 * has gone to sleep. Every one of those is a value the driver must read
 * correctly and none can be produced here, so each decision is asked directly of
 * the values the hardware would have given.
 */
static bool KernelVerifyAhciDecisions(void)
{
    bool succeeded = true;

    /* --- A port is usable when a device is present and the link is active. --- */

    if (!AhciPortIsUsable(0x00000123U))
    {
        KernelWriteString("  A port with a device present and an active interface was "
                          "called unusable.\n");
        succeeded = false;
    }

    /*
     * The speed field lies between the two that are read, in bits 7:4, and a
     * mask that swept it in would reject every port upon every machine that
     * negotiated anything but the slowest link. 0x123 above is DET 3, speed 2,
     * IPM 1.
     */
    if (!AhciPortIsUsable(0x00000103U) || !AhciPortIsUsable(0x00000163U))
    {
        KernelWriteString("  The negotiated speed was read as part of the detection.\n");
        succeeded = false;
    }

    /*
     * DET of 1 is presence detected without communication established, which is
     * the state of a port whose device has not finished negotiating. A driver
     * that accepted it would issue a command and wait out its whole patience.
     */
    if (AhciPortIsUsable(0x00000101U) || AhciPortIsUsable(0x00000100U) ||
        AhciPortIsUsable(0x00000104U))
    {
        KernelWriteString("  A port with no communication established was called "
                          "usable.\n");
        succeeded = false;
    }

    /*
     * IPM of 2 is a partial power state and of 6 a slumbering one. The device is
     * present in both, so a driver reading the detection alone would find them
     * indistinguishable from a port ready to answer.
     */
    if (AhciPortIsUsable(0x00000203U) || AhciPortIsUsable(0x00000603U) ||
        AhciPortIsUsable(0x00000003U))
    {
        KernelWriteString("  A port whose interface is not active was called usable.\n");
        succeeded = false;
    }

    /* --- The signature says what is attached, and the four differ. --- */

    if ((AhciKindFromSignature(0x00000101U) != AHCI_DEVICE_SATA) ||
        (AhciKindFromSignature(0xEB140101U) != AHCI_DEVICE_SATAPI) ||
        (AhciKindFromSignature(0xC33C0101U) != AHCI_DEVICE_ENCLOSURE) ||
        (AhciKindFromSignature(0x96690101U) != AHCI_DEVICE_MULTIPLIER))
    {
        KernelWriteString("  A signature was not recognised as what it names.\n");
        succeeded = false;
    }

    /*
     * Every signature ends in 0101h, so a comparison of the low half alone would
     * call a packet device a disk — and this driver would then issue READ DMA
     * EXT to something that answers only the packet interface.
     */
    if ((AhciKindFromSignature(0xFFFFFFFFU) != AHCI_DEVICE_UNKNOWN) ||
        (AhciKindFromSignature(0x00000000U) != AHCI_DEVICE_UNKNOWN))
    {
        KernelWriteString("  An unrecognised signature was taken for a device.\n");
        succeeded = false;
    }

    /* --- The command header describes the FIS, the direction and the table. --- */

    /*
     * Five double words is the Register Host to Device FIS, eight regions is
     * eight pages, and the two occupy opposite ends of the same word. A length
     * given in bytes rather than double words is 20, which does not fit the five
     * bits the field has and would silently become 4.
     */
    if (AhciDescribeCommand(5U, false, 8U) != 0x00080005U)
    {
        KernelWriteString("  A read command header was composed wrongly.\n");
        succeeded = false;
    }

    /*
     * The write bit is bit 6. A command whose direction is wrong transfers the
     * disk into the buffer the caller meant to write from, and reports success.
     */
    if (AhciDescribeCommand(5U, true, 8U) != 0x00080045U)
    {
        KernelWriteString("  The write bit is not at bit 6 of the command header.\n");
        succeeded = false;
    }

    if ((AhciDescribeCommand(5U, false, 0U) != 0x00000005U) ||
        (AhciDescribeCommand(5U, true, 1U) != 0x00010045U))
    {
        KernelWriteString("  The region count is not in the high half of the command "
                          "header.\n");
        succeeded = false;
    }

    return succeeded;
}

void KernelVerifyAhci(void)
{
    const AhciDevice *const disk = AhciFirstDisk();
    bool succeeded = KernelVerifyAhciDecisions();

    KernelWriteString(succeeded ? "AHCI self-test: the port and command decisions are "
                                  "sound.\n"
                                : "AHCI self-test FAILED: the port and command "
                                  "decisions.\n");

    if (!AhciIsPresent())
    {
        KernelWriteString("AHCI self-test: no adaptor upon this machine; nothing "
                          "transferred.\n");
        return;
    }

    if (disk == NULL)
    {
        KernelWriteString("AHCI self-test: no disk upon any port; nothing "
                          "transferred.\n");
        return;
    }

    succeeded = true;

    /*
     * The first sector, twice, into buffers seeded differently.
     *
     * Two reads compared against each other establish less than they appear to:
     * a transfer that is consistently the wrong length leaves both buffers
     * holding the same wrong thing, and the comparison passes. **Seeding them
     * with different bytes is what gives the comparison its force.** Wherever
     * the device did not write, the two buffers still differ, so a region
     * descriptor that describes half a sector — a byte count mistaken for a word
     * count — is caught at the first byte the device did not reach.
     *
     * This was not hypothetical. The first form of this test compared two reads
     * into buffers that both held the previous read, and it passed with the
     * descriptor's byte count halved and again with it one too large.
     */
    for (size_t index = 0U; index < (AHCI_SECTOR_SIZE * 2U); ++index)
    {
        KernelDiskBufferA[index] = 0xA5U;
        KernelDiskBufferB[index] = 0x5AU;
    }

    if (!AhciRead(disk, 0U, 1U, KernelDiskBufferA) ||
        !AhciRead(disk, 0U, 1U, KernelDiskBufferB))
    {
        KernelWriteString("  The first sector did not read.\n");
        succeeded = false;
    }
    else
    {
        if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, AHCI_SECTOR_SIZE))
        {
            KernelWriteString("  A sector read twice differs, so less than a whole "
                              "sector was transferred.\n");
            succeeded = false;
        }

        /*
         * And nothing beyond it. A byte count one too large — the descriptor
         * holding the length rather than the length less one — writes past the
         * sector the caller asked for, into whatever the buffer is part of.
         */
        for (size_t index = AHCI_SECTOR_SIZE; index < (AHCI_SECTOR_SIZE * 2U); ++index)
        {
            if (KernelDiskBufferA[index] != 0xA5U)
            {
                KernelWriteString("  The transfer wrote past the end of the sector.\n");
                succeeded = false;
                break;
            }
        }
    }

    /*
     * A two-sector transfer is where the region descriptors are exercised
     * against a length that is not one sector. The first sector of it must be
     * what a one-sector read returned, and the second what a read of the
     * following address returns.
     */
    if (disk->sector_count >= 2U)
    {
        if (!AhciRead(disk, 0U, 2U, KernelDiskBufferB))
        {
            KernelWriteString("  A two-sector read failed.\n");
            succeeded = false;
        }
        else
        {
            if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, AHCI_SECTOR_SIZE))
            {
                KernelWriteString("  A two-sector read did not begin where a one-sector "
                                  "read did.\n");
                succeeded = false;
            }

            if (!AhciRead(disk, 1U, 1U, KernelDiskBufferA) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[AHCI_SECTOR_SIZE],
                                    AHCI_SECTOR_SIZE))
            {
                KernelWriteString("  The second sector of a two-sector read is not the "
                                  "sector that follows.\n");
                succeeded = false;
            }
        }
    }

    /*
     * A sector beyond what 28 bits can name, where the device is large enough to
     * have one.
     *
     * Every command this driver issues is an extended one, so unlike the ATA
     * driver there is no second path to reach — but the address is composed from
     * six bytes across two halves of the command FIS, and a byte written into
     * the wrong one of them addresses a sector some multiple of 16 megabytes
     * away. That is a read which succeeds and returns the wrong data, which is
     * the failure this whole file exists to catch.
     */
    if (disk->sector_count > ATA_LBA28_LIMIT)
    {
        if (!AhciRead(disk, ATA_LBA28_LIMIT + 1U, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  A sector beyond the 28-bit limit did not read.\n");
            succeeded = false;
        }
    }

    /*
     * The refusals. Each is a request the adaptor must not be asked to attempt,
     * and each is refused before it is touched: a range outside the device, a
     * count beyond what one command may carry, a count of nothing, an absent
     * buffer, and — this driver's own — a buffer at an odd address, whose low
     * bit the region descriptor has no room for and would transfer one byte
     * below where the caller asked.
     */
    if (AhciRead(disk, disk->sector_count, 1U, KernelDiskBufferA) ||
        AhciRead(disk, 0U, AHCI_MAXIMUM_SECTORS + 1U, KernelDiskBufferA) ||
        AhciRead(disk, 0U, 0U, KernelDiskBufferA) || AhciRead(disk, 0U, 1U, NULL) ||
        AhciRead(disk, 0U, 1U, &KernelDiskBufferA[1]))
    {
        KernelWriteString("  A request that should have been refused was attempted.\n");
        succeeded = false;
    }

    /*
     * The write path, only upon request, and by the same rule the ATA driver's
     * self-test keeps: the sector is read first, overwritten with a pattern,
     * read back, compared, and then restored from what was read, the restoration
     * being verified in its turn. Anybody may boot this kernel upon their own
     * machine, and a self-test that wrote to their disk unbidden would destroy
     * their data.
     */
    if (KernelCommandLineHasOption("disk-write-test"))
    {
        const uint64_t target = disk->sector_count - 1U;

        KernelWriteString("  Writing to the final sector, as the command line permits.\n");

        if (!AhciRead(disk, target, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  The sector to be written could not first be read.\n");
            succeeded = false;
        }
        else
        {
            for (size_t index = 0U; index < AHCI_SECTOR_SIZE; ++index)
            {
                KernelDiskBufferB[index] = (uint8_t)(index ^ 0x5AU);
            }

            if (!AhciWrite(disk, target, 1U, KernelDiskBufferB))
            {
                KernelWriteString("  The pattern could not be written.\n");
                succeeded = false;
            }
            else if (!AhciRead(disk, target, 1U, &KernelDiskBufferB[AHCI_SECTOR_SIZE]))
            {
                KernelWriteString("  The pattern could not be read back.\n");
                succeeded = false;
            }
            else if (!KernelRegionsMatch(KernelDiskBufferB,
                                         &KernelDiskBufferB[AHCI_SECTOR_SIZE],
                                         AHCI_SECTOR_SIZE))
            {
                KernelWriteString("  The sector read back is not the pattern written.\n");
                succeeded = false;
            }

            /* Restored whatever happened above, and the restoration checked. */
            if (!AhciWrite(disk, target, 1U, KernelDiskBufferA) ||
                !AhciRead(disk, target, 1U, &KernelDiskBufferB[AHCI_SECTOR_SIZE]) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[AHCI_SECTOR_SIZE],
                                    AHCI_SECTOR_SIZE))
            {
                KernelWriteString("  The sector was not restored to what it held.\n");
                succeeded = false;
            }
        }
    }

    /*
     * A device that exceeded the driver's patience is a different fault from one
     * that refused a command, and neither is expected here.
     */
    if (AhciTimeoutCount() != 0U)
    {
        KernelWriteString("  A port exceeded the driver's patience.\n");
        succeeded = false;
    }

    if (AhciErrorCount() != 0U)
    {
        KernelWriteString("  A device reported an error.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "AHCI self-test passed.\n" : "AHCI self-test FAILED.\n");
}


/*
 * Composes the four response registers a card specific data would appear in.
 *
 * The registers hold the specific data with its low eight bits removed, so a
 * field at bit N of the specific data lies at bit N - 8 here. The helper takes
 * the position in the **specific data**, because that is what the specification
 * states and what a reader will check this against; subtracting the eight in
 * only one place is the point of it.
 */
static void KernelPlaceCsdField(uint32_t response[4], uint32_t first_bit, uint32_t width,
                                uint64_t value)
{
    for (uint32_t index = 0U; index < width; ++index)
    {
        const uint32_t position = (first_bit + index) - 8U;
        const uint32_t word = position / 32U;
        const uint32_t bit = position % 32U;

        if (((value >> index) & 1U) != 0U)
        {
            response[word] |= (uint32_t)1U << bit;
        }
    }
}

/*
 * Asserts the two decisions of the SD driver that are pure.
 *
 * The capacity arithmetic is the reason this exists. There are two encodings of
 * a card's size, chosen by a field of the same register, and they differ in
 * where every other field sits, in the units of the answer, and in whether a
 * multiplier applies at all. A capacity computed by the wrong one is not a small
 * error: it is wrong by a factor of thousands, and a block layer told a card is
 * larger than it is reads beyond the end of it and is answered with nothing.
 *
 * No card available to this project uses the first encoding — it is the one used
 * by cards of two gibibytes and below, and QEMU's is not one — so the values are
 * composed rather than obtained, exactly as the ATA driver's channel addressing
 * is.
 */
static bool KernelVerifySdhciDecisions(void)
{
    bool succeeded = true;
    uint32_t response[4];

    /* --- The second encoding: one field, in fixed units. --- */

    /*
     * C_SIZE of 3823 is the value a 2 GiB SDHC card reports: the capacity is
     * (C_SIZE + 1) * 512 KiB, which is 3824 * 1024 blocks.
     */
    for (uint32_t word = 0U; word < 4U; ++word)
    {
        response[word] = 0U;
    }

    KernelPlaceCsdField(response, 126U, 2U, 1U);    /* CSD_STRUCTURE = 1. */
    KernelPlaceCsdField(response, 48U, 22U, 3823U); /* C_SIZE. */

    if (SdhciCapacityFromCsd(response) != (3824ULL * 1024ULL))
    {
        KernelWriteString("  A version 2 capacity was computed wrongly.\n");
        succeeded = false;
    }

    /* The largest a version 2 card may report, which is where a 32-bit
     * intermediate would wrap: 4194304 blocks per unit of C_SIZE. */
    for (uint32_t word = 0U; word < 4U; ++word)
    {
        response[word] = 0U;
    }

    KernelPlaceCsdField(response, 126U, 2U, 1U);
    KernelPlaceCsdField(response, 48U, 22U, 0x3FFFFFU);

    if (SdhciCapacityFromCsd(response) != (0x400000ULL * 1024ULL))
    {
        KernelWriteString("  The greatest version 2 capacity overflowed or was "
                          "truncated.\n");
        succeeded = false;
    }

    /* --- The first encoding: three fields, in units the card chooses. --- */

    /*
     * A 1 GiB card: C_SIZE 3815, C_SIZE_MULT 7, READ_BL_LEN 10. The capacity in
     * bytes is (3815 + 1) * 2^9 * 2^10, which is 2000683008, and in blocks
     * 3907584.
     */
    for (uint32_t word = 0U; word < 4U; ++word)
    {
        response[word] = 0U;
    }

    KernelPlaceCsdField(response, 126U, 2U, 0U);  /* CSD_STRUCTURE = 0. */
    KernelPlaceCsdField(response, 80U, 4U, 10U);  /* READ_BL_LEN. */
    KernelPlaceCsdField(response, 62U, 12U, 3815U); /* C_SIZE. */
    KernelPlaceCsdField(response, 47U, 3U, 7U);   /* C_SIZE_MULT. */

    if (SdhciCapacityFromCsd(response) != 3907584ULL)
    {
        KernelWriteString("  A version 1 capacity was computed wrongly.\n");
        succeeded = false;
    }

    /*
     * The same card read by the other encoding would give a wholly different
     * answer, which is what makes the structure field load-bearing rather than
     * decorative. The assertion is that the two disagree: a driver that ignored
     * the structure would pass every test above by accident if they did not.
     */
    for (uint32_t word = 0U; word < 4U; ++word)
    {
        response[word] = 0U;
    }

    KernelPlaceCsdField(response, 126U, 2U, 0U);
    KernelPlaceCsdField(response, 80U, 4U, 9U);
    KernelPlaceCsdField(response, 62U, 12U, 1000U);
    KernelPlaceCsdField(response, 47U, 3U, 3U);

    if (SdhciCapacityFromCsd(response) != ((1001ULL * 32ULL * 512ULL) / 512ULL))
    {
        KernelWriteString("  The version 1 multiplier or block length was misread.\n");
        succeeded = false;
    }

    /* --- A structure this driver does not know yields nothing. --- */

    for (uint32_t word = 0U; word < 4U; ++word)
    {
        response[word] = 0U;
    }

    KernelPlaceCsdField(response, 126U, 2U, 2U);
    KernelPlaceCsdField(response, 48U, 22U, 3823U);

    if (SdhciCapacityFromCsd(response) != 0U)
    {
        KernelWriteString("  An unknown card specific data structure was guessed at.\n");
        succeeded = false;
    }

    if (SdhciCapacityFromCsd(NULL) != 0U)
    {
        KernelWriteString("  A card specific data that names nothing yielded a "
                          "capacity.\n");
        succeeded = false;
    }

    /* --- The command register. --- */

    /*
     * CMD17 with a short response and data: the index in bits 13:8, the data
     * bit at 5, the index and CRC checks at 4 and 3, and a response type of 2.
     */
    if (SdhciCommandFlags(17U, SD_RESPONSE_SHORT, true) != 0x113AU)
    {
        KernelWriteString("  A data command was composed wrongly.\n");
        succeeded = false;
    }

    /*
     * A response of 136 bits carries no command index, so the index check must
     * be off. Left on, every CMD2 and CMD9 reports an index error and the card
     * is never identified — which presents as a machine with no storage.
     */
    if (SdhciCommandFlags(9U, SD_RESPONSE_LONG, false) != 0x0909U)
    {
        KernelWriteString("  A long response was composed with the index checked.\n");
        succeeded = false;
    }

    /*
     * The operating conditions register comes back with neither a CRC nor an
     * index, both fields carrying part of the register instead. Checking either
     * rejects a card that answered correctly, and the card is then never
     * brought up.
     */
    if (SdhciCommandFlags(41U, SD_RESPONSE_UNCHECKED, false) != 0x2902U)
    {
        KernelWriteString("  An unchecked response was composed with a check.\n");
        succeeded = false;
    }

    if ((SdhciCommandFlags(0U, SD_RESPONSE_NONE, false) != 0x0000U) ||
        (SdhciCommandFlags(7U, SD_RESPONSE_BUSY, false) != 0x071BU))
    {
        KernelWriteString("  A command with no response, or one with busy, was "
                          "composed wrongly.\n");
        succeeded = false;
    }

    return succeeded;
}

void KernelVerifySdhci(void)
{
    const SdCard *const card = SdhciCard();
    bool succeeded = KernelVerifySdhciDecisions();

    KernelWriteString(succeeded
                          ? "SD self-test: the capacity and command arithmetic is sound.\n"
                          : "SD self-test FAILED: the capacity and command arithmetic.\n");

    if (!SdhciIsPresent())
    {
        KernelWriteString("SD self-test: no host controller upon this machine; nothing "
                          "transferred.\n");
        return;
    }

    if (card == NULL)
    {
        KernelWriteString("SD self-test: no card in the slot; nothing transferred.\n");
        return;
    }

    succeeded = true;

    /*
     * The first block, twice, into buffers seeded differently — the assertion
     * the AHCI driver's self-test was corrected to make. Two reads compared
     * against each other establish less than they appear to: a transfer that is
     * consistently the wrong length leaves both holding the same wrong thing.
     */
    for (size_t index = 0U; index < (SDHCI_BLOCK_SIZE * 2U); ++index)
    {
        KernelDiskBufferA[index] = 0xA5U;
        KernelDiskBufferB[index] = 0x5AU;
    }

    if (!SdhciRead(0U, 1U, KernelDiskBufferA) || !SdhciRead(0U, 1U, KernelDiskBufferB))
    {
        KernelWriteString("  The first block did not read.\n");
        succeeded = false;
    }
    else
    {
        if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, SDHCI_BLOCK_SIZE))
        {
            KernelWriteString("  A block read twice differs, so less than a whole block "
                              "was transferred.\n");
            succeeded = false;
        }

        for (size_t index = SDHCI_BLOCK_SIZE; index < (SDHCI_BLOCK_SIZE * 2U); ++index)
        {
            if (KernelDiskBufferA[index] != 0xA5U)
            {
                KernelWriteString("  The transfer wrote past the end of the block.\n");
                succeeded = false;
                break;
            }
        }
    }

    /*
     * Two blocks are two commands here, one per block, so this asserts that the
     * second command addressed the block after the first and placed it after it
     * — which is where the byte-addressed and block-addressed forms of the
     * argument differ, and where confusing them is invisible upon block zero.
     */
    if (card->block_count >= 2U)
    {
        if (!SdhciRead(0U, 2U, KernelDiskBufferB))
        {
            KernelWriteString("  A two-block read failed.\n");
            succeeded = false;
        }
        else
        {
            if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, SDHCI_BLOCK_SIZE))
            {
                KernelWriteString("  A two-block read did not begin where a one-block "
                                  "read did.\n");
                succeeded = false;
            }

            if (!SdhciRead(1U, 1U, KernelDiskBufferA) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[SDHCI_BLOCK_SIZE],
                                    SDHCI_BLOCK_SIZE))
            {
                KernelWriteString("  The second block of a two-block read is not the "
                                  "block that follows.\n");
                succeeded = false;
            }
        }
    }

    /* The refusals, each made before the controller is touched. */
    if (SdhciRead(card->block_count, 1U, KernelDiskBufferA) ||
        SdhciRead(0U, SDHCI_MAXIMUM_BLOCKS + 1U, KernelDiskBufferA) ||
        SdhciRead(0U, 0U, KernelDiskBufferA) || SdhciRead(0U, 1U, NULL))
    {
        KernelWriteString("  A request that should have been refused was attempted.\n");
        succeeded = false;
    }

    /*
     * The write path, only upon request, and restored afterwards — the rule the
     * other two drivers keep, and for the same reason: this may be the only
     * storage the machine has.
     */
    if (KernelCommandLineHasOption("disk-write-test"))
    {
        const uint64_t target = card->block_count - 1U;

        KernelWriteString("  Writing to the final block, as the command line permits.\n");

        if (!SdhciRead(target, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  The block to be written could not first be read.\n");
            succeeded = false;
        }
        else
        {
            for (size_t index = 0U; index < SDHCI_BLOCK_SIZE; ++index)
            {
                KernelDiskBufferB[index] = (uint8_t)(index ^ 0x3CU);
            }

            if (!SdhciWrite(target, 1U, KernelDiskBufferB))
            {
                KernelWriteString("  The pattern could not be written.\n");
                succeeded = false;
            }
            else if (!SdhciRead(target, 1U, &KernelDiskBufferB[SDHCI_BLOCK_SIZE]))
            {
                KernelWriteString("  The pattern could not be read back.\n");
                succeeded = false;
            }
            else if (!KernelRegionsMatch(KernelDiskBufferB,
                                         &KernelDiskBufferB[SDHCI_BLOCK_SIZE],
                                         SDHCI_BLOCK_SIZE))
            {
                KernelWriteString("  The block read back is not the pattern written.\n");
                succeeded = false;
            }

            if (!SdhciWrite(target, 1U, KernelDiskBufferA) ||
                !SdhciRead(target, 1U, &KernelDiskBufferB[SDHCI_BLOCK_SIZE]) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[SDHCI_BLOCK_SIZE],
                                    SDHCI_BLOCK_SIZE))
            {
                KernelWriteString("  The block was not restored to what it held.\n");
                succeeded = false;
            }
        }
    }

    if (SdhciTimeoutCount() != 0U)
    {
        KernelWriteString("  A command exceeded the driver's patience.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "SD self-test passed.\n" : "SD self-test FAILED.\n");
}
/* Two blocks of working space for the transfers the self-tests perform. */
static uint8_t KernelBlockBufferA[BLOCK_SIZE_DEFAULT * 2U];
static uint8_t KernelBlockBufferB[BLOCK_SIZE_DEFAULT * 2U];

/* Fills a region with a pattern that depends upon the seed, so that two regions
 * filled from different seeds cannot be confused for one another. */
static void KernelFillPattern(uint8_t *region, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; ++index)
    {
        region[index] = (uint8_t)((index * 31U) + seed);
    }
}

/* True if a region holds the pattern that seed would have produced. */
static bool KernelPatternMatches(const uint8_t *region, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (region[index] != (uint8_t)((index * 31U) + seed))
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the block layer validates what it is asked before it reaches a
 * driver, and transfers what it was given when it does.
 *
 * The layer exists precisely so that the four tests every driver would otherwise
 * repeat are written once, and the consequence of that is that a defect here is
 * a defect in every device at once. The assertions are made against a device of
 * known contents rather than against a disk, for the reason given where that
 * device is defined.
 */
void KernelVerifyBlock(void)
{
    BlockDevice *device;
    BlockDevice *read_only;
    const size_t already_registered = BlockDeviceCount();
    bool succeeded = true;

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Block self-test FAILED.\n");
        return;
    }

    read_only = BlockRegister("mem1", &KernelMemoryDeviceReadOnlyOperations, NULL,
                              BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, true);

    if (read_only == NULL)
    {
        KernelWriteString("  A read-only device could not be registered.\n");
        succeeded = false;
    }

    /* A name identifies a device, so a second device may not take one in use. */
    if (BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 1U,
                      false) != NULL)
    {
        KernelWriteString("  A name already registered was accepted a second time.\n");
        succeeded = false;
    }

    /*
     * A writable device without a writer, and a read-only device with one, are
     * both refused: either would be a device whose declared nature and whose
     * behaviour disagree.
     */
    if ((BlockRegister("mem2", &KernelMemoryDeviceReadOnlyOperations, NULL, BLOCK_SIZE_DEFAULT,
                       1U, false) != NULL) ||
        (BlockRegister("mem3", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 1U,
                       true) != NULL))
    {
        KernelWriteString("  A device was registered whose nature and operations disagree.\n");
        succeeded = false;
    }

    /* A degenerate geometry, and a name that cannot be held, are refused. */
    if ((BlockRegister("mem4", &KernelMemoryDeviceOperations, NULL, 0U, 1U, false) != NULL) ||
        (BlockRegister("mem5", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 0U,
                       false) != NULL) ||
        (BlockRegister("a-name-far-too-long-to-hold", &KernelMemoryDeviceOperations, NULL,
                       BLOCK_SIZE_DEFAULT, 1U, false) != NULL))
    {
        KernelWriteString("  A degenerate registration was accepted.\n");
        succeeded = false;
    }

    if ((BlockFindByName("mem0") != device) || (BlockFindByName("mem") != NULL) ||
        (BlockFindByName("mem00") != NULL))
    {
        KernelWriteString("  A device was found by a name that is not its own.\n");
        succeeded = false;
    }

    if (BlockDeviceCount() != (already_registered + 2U))
    {
        KernelWriteString("  The registry holds a different number of devices than "
                          "were registered.\n");
        succeeded = false;
    }

    if (BlockDeviceAt(BlockDeviceCount()) != NULL)
    {
        KernelWriteString("  A device was reported beyond the end of the registry.\n");
        succeeded = false;
    }

    /* What is written to a block must be what is read back from it. */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x11U);

    if (!BlockWrite(device, 3U, 1U, KernelBlockBufferA) ||
        !BlockRead(device, 3U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x11U))
    {
        KernelWriteString("  A block did not read back as it was written.\n");
        succeeded = false;
    }

    /*
     * A two-block transfer must carry both blocks and must not carry a third.
     * The two halves are given different patterns so that a layer which passed
     * the same block twice, or which lost the count, cannot pass this.
     */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x22U);
    KernelFillPattern(&KernelBlockBufferA[BLOCK_SIZE_DEFAULT], BLOCK_SIZE_DEFAULT, 0x33U);

    if (!BlockWrite(device, 8U, 2U, KernelBlockBufferA) ||
        !BlockRead(device, 8U, 2U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x22U) ||
        !KernelPatternMatches(&KernelBlockBufferB[BLOCK_SIZE_DEFAULT], BLOCK_SIZE_DEFAULT,
                              0x33U))
    {
        KernelWriteString("  A two-block transfer did not carry both blocks in order.\n");
        succeeded = false;
    }

    if (!BlockRead(device, 9U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x33U))
    {
        KernelWriteString("  The second block of a two-block write is not the block "
                          "that follows.\n");
        succeeded = false;
    }

    /* A range outside the device is refused, and so is one that would wrap. */
    if (BlockRead(device, KERNEL_MEMORY_DEVICE_BLOCKS, 1U, KernelBlockBufferB) ||
        BlockRead(device, KERNEL_MEMORY_DEVICE_BLOCKS - 1U, 2U, KernelBlockBufferB) ||
        BlockRead(device, UINT64_MAX, 2U, KernelBlockBufferB))
    {
        KernelWriteString("  A range outside the device was accepted.\n");
        succeeded = false;
    }

    /* A request without a buffer is refused; one for no blocks is harmless. */
    if (BlockRead(device, 0U, 1U, NULL) || BlockWrite(device, 0U, 1U, NULL) ||
        !BlockRead(device, 0U, 0U, NULL))
    {
        KernelWriteString("  A degenerate request was mishandled.\n");
        succeeded = false;
    }

    /* A read-only device refuses a write before the driver is reached. */
    if (BlockWrite(read_only, 0U, 1U, KernelBlockBufferA))
    {
        KernelWriteString("  A read-only device accepted a write.\n");
        succeeded = false;
    }

    if (!BlockRead(read_only, 3U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x11U))
    {
        KernelWriteString("  A read-only device did not read.\n");
        succeeded = false;
    }

    /* The accounting must reflect the blocks that actually moved. */
    if ((device->blocks_written != 3U) || (device->blocks_read != 4U))
    {
        KernelWriteString("  The accounting does not match the transfers performed.\n");
        succeeded = false;
    }

    /* A device may be withdrawn, and is then neither found nor addressable. */
    if (!BlockUnregister(read_only) || !BlockUnregister(device))
    {
        KernelWriteString("  A registered device could not be withdrawn.\n");
        succeeded = false;
    }

    if (BlockUnregister(device) || (BlockFindByName("mem0") != NULL) ||
        BlockRead(device, 0U, 1U, KernelBlockBufferB) ||
        (BlockDeviceCount() != already_registered))
    {
        KernelWriteString("  A withdrawn device was still reachable.\n");
        succeeded = false;
    }

    if (BlockTotalErrors() != 0U)
    {
        KernelWriteString("  A device reported an error where none was expected.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Block self-test passed.\n" : "Block self-test FAILED.\n");
}

/* The buffers held at once by the assertion that every buffer may be held. */
static Buffer *KernelHeldBuffers[BUFFER_CAPACITY];

/*
 * Asserts that the cache returns the block that was asked for, that a block held
 * is not read again, that a modified block reaches its device, and that a buffer
 * somebody is using is never taken from them.
 *
 * A cache is a thing that lies about where data came from, and every one of its
 * failures is silent by construction. A lookup that matched the wrong device
 * returns a block; an eviction that discarded a dirty buffer reports success and
 * loses a write; a buffer handed to two callers at once corrupts whichever of
 * them writes second, at a place unrelated to the defect. The assertions below
 * are chosen so that each of those produces a failure here instead.
 *
 * They are made against the device of memory, for the reasons given where it is
 * defined: this must be assertable upon a machine with no disk, and must not
 * write to a machine that has one.
 */
void KernelVerifyBuffer(void)
{
    BlockDevice *device;
    Buffer *first;
    Buffer *second;
    uint64_t reads;
    uint64_t writes;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    bool succeeded = true;

    if (BufferCount() == 0U)
    {
        KernelWriteString("  The cache has no buffers; its storage was not allocated.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        return;
    }

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        return;
    }

    /* Blocks 5, 6 and 7 are given contents the assertions below can name. */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x55U);
    (void)BlockWrite(device, 5U, 1U, KernelBlockBufferA);
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x66U);
    (void)BlockWrite(device, 6U, 1U, KernelBlockBufferA);
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x77U);
    (void)BlockWrite(device, 7U, 1U, KernelBlockBufferA);

    /* A block not held is read from the device, and read correctly. */
    reads = device->blocks_read;
    first = BufferGet(device, 5U);

    if ((first == NULL) || (device->blocks_read != (reads + 1U)) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x55U) ||
        (first->block != 5U) || (first->device != device))
    {
        KernelWriteString("  A block was not fetched from the device correctly.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
        return;
    }

    BufferRelease(first);

    /* The same block is then found in the cache, and the device is not touched. */
    hits = BufferHits();
    reads = device->blocks_read;
    second = BufferGet(device, 5U);

    if ((second != first) || (BufferHits() != (hits + 1U)) || (device->blocks_read != reads))
    {
        KernelWriteString("  A block already held was fetched from the device again.\n");
        succeeded = false;
    }

    BufferRelease(second);

    /* Two different blocks occupy two different buffers. */
    first = BufferGet(device, 6U);
    second = BufferGet(device, 7U);

    if ((first == NULL) || (second == NULL) || (first == second) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x66U) ||
        !KernelPatternMatches(second->data, BLOCK_SIZE_DEFAULT, 0x77U))
    {
        KernelWriteString("  Two blocks were confused for one another.\n");
        succeeded = false;
    }

    BufferRelease(second);

    /*
     * A modified block does not reach the device until it is written back. That
     * deferral is the whole difference between this cache and none at all, so it
     * is asserted directly: the device must still hold the old contents.
     */
    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0x88U);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    writes = device->blocks_written;

    if ((device->blocks_written != writes) ||
        !BlockRead(device, 6U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x66U))
    {
        KernelWriteString("  A modified block reached the device before it was written "
                          "back.\n");
        succeeded = false;
    }

    if (BufferDirtyCount() == 0U)
    {
        KernelWriteString("  A modified block was not recorded as dirty.\n");
        succeeded = false;
    }

    /* Synchronising writes it back, and the device then holds the new contents. */
    if (!BufferSync() || (device->blocks_written != (writes + 1U)) ||
        !BlockRead(device, 6U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x88U) ||
        (BufferDirtyCount() != 0U))
    {
        KernelWriteString("  A modified block did not reach the device upon "
                          "synchronisation.\n");
        succeeded = false;
    }

    /*
     * A dirty block evicted under pressure must be written back as it goes. The
     * failure this catches is the one that loses data: an eviction that dropped
     * the contents would report nothing and be discovered only by a later read.
     */
    first = BufferGet(device, 7U);

    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0x99U);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    evictions = BufferEvictions();

    for (uint64_t block = 16U; block < (16U + (uint64_t)BUFFER_CAPACITY); ++block)
    {
        Buffer *const transient = BufferGet(device, block);

        BufferRelease(transient);
    }

    if ((BufferEvictions() <= evictions) || !BlockRead(device, 7U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x99U))
    {
        KernelWriteString("  A dirty block was evicted without being written back.\n");
        succeeded = false;
    }

    /* Having been evicted, the block is fetched from the device once more. */
    misses = BufferMisses();
    reads = device->blocks_read;
    first = BufferGet(device, 5U);

    if ((first == NULL) || (BufferMisses() != (misses + 1U)) ||
        (device->blocks_read != (reads + 1U)))
    {
        KernelWriteString("  An evicted block was reported as still held.\n");
        succeeded = false;
    }

    /*
     * A buffer a caller is holding is passed over by the eviction, however long
     * it has been there. The reference above is deliberately not released.
     */
    for (uint64_t block = 128U; block < (128U + (uint64_t)BUFFER_CAPACITY); ++block)
    {
        Buffer *const transient = BufferGet(device, block);

        BufferRelease(transient);
    }

    hits = BufferHits();
    second = BufferGet(device, 5U);

    if ((second != first) || (BufferHits() != (hits + 1U)) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x55U))
    {
        KernelWriteString("  A buffer being held by a caller was evicted beneath them.\n");
        succeeded = false;
    }

    BufferRelease(second);
    BufferRelease(first);

    /*
     * With every buffer held, a further request is refused rather than served by
     * evicting one of them. Handing out storage twice is the failure this
     * prevents, and it would appear as corruption somewhere else entirely.
     */
    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        KernelHeldBuffers[index] = BufferGet(device, (uint64_t)index);

        if (KernelHeldBuffers[index] == NULL)
        {
            KernelWriteString("  A buffer could not be held while others were.\n");
            succeeded = false;
            break;
        }
    }

    if (BufferHeldCount() != BUFFER_CAPACITY)
    {
        KernelWriteString("  The cache does not agree upon how many buffers are held.\n");
        succeeded = false;
    }

    if (BufferGet(device, (uint64_t)BUFFER_CAPACITY + 1U) != NULL)
    {
        KernelWriteString("  A buffer was issued when every one of them was held.\n");
        succeeded = false;
    }

    /* Nor may a device be discarded while its buffers are held. */
    if (BufferInvalidateDevice(device))
    {
        KernelWriteString("  A device was invalidated while its buffers were held.\n");
        succeeded = false;
    }

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        BufferRelease(KernelHeldBuffers[index]);
        KernelHeldBuffers[index] = NULL;
    }

    if (BufferHeldCount() != 0U)
    {
        KernelWriteString("  A buffer remained held after being released.\n");
        succeeded = false;
    }

    /*
     * Invalidation writes back what is dirty and then discards everything of the
     * device, which is what makes it safe to withdraw the device afterwards.
     */
    first = BufferGet(device, 4U);

    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0xAAU);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    if (!BufferInvalidateDevice(device) || (BufferValidCount() != 0U) ||
        !BlockRead(device, 4U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0xAAU))
    {
        KernelWriteString("  Invalidation did not write back and discard the device.\n");
        succeeded = false;
    }

    misses = BufferMisses();
    first = BufferGet(device, 5U);

    if ((first == NULL) || (BufferMisses() != (misses + 1U)))
    {
        KernelWriteString("  A block survived the invalidation of its device.\n");
        succeeded = false;
    }

    BufferRelease(first);

    /*
     * Nothing beneath the cache failed. Every transfer the test performed was of
     * a block the device holds, so a failure here means the cache asked for one
     * it should not have.
     */
    if (BufferFailures() != 0U)
    {
        KernelWriteString("  A transfer beneath the cache failed.\n");
        succeeded = false;
    }

    /* The device is discarded and withdrawn in that order, as it must be. */
    (void)BufferInvalidateDevice(device);
    (void)BlockUnregister(device);

    KernelWriteString(succeeded ? "Buffer self-test passed.\n" : "Buffer self-test FAILED.\n");
}
