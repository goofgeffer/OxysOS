/*
 * File: kernel/include/oxys/ahci.h
 * Purpose: Declares the interface of the AHCI driver: the discovery of the host
 *          bus adaptor upon the PCI bus, the ports it implements, the
 *          identification of the devices attached to them, and the reading and
 *          writing of sectors by first-party direct memory access.
 * Key definitions: AHCI_SECTOR_SIZE, AhciDeviceKind, AhciDevice, AhciInitialise,
 *          AhciDeviceCount, AhciDeviceAt, AhciFirstDisk, AhciRead, AhciWrite,
 *          AhciPortIsUsable, AhciKindFromSignature, AhciDescribeCommand,
 *          AhciRegisterBlockDevices, AhciReport.
 * References:
 *   - Serial ATA Advanced Host Controller Interface Specification, revision
 *     1.3.1, Section 3.1, the generic host control registers: CAP at 00h, GHC at
 *     04h, IS at 08h, PI at 0Ch, VS at 10h, CAP2 at 24h and BOHC at 28h.
 *   - AHCI 1.3.1, Section 3.3, the port registers: the block for port x begins
 *     at 100h + x * 80h, and within it PxCLB lies at 00h, PxCLBU at 04h, PxFB at
 *     08h, PxFBU at 0Ch, PxIS at 10h, PxIE at 14h, PxCMD at 18h, PxTFD at 20h,
 *     PxSIG at 24h, PxSSTS at 28h, PxSCTL at 2Ch, PxSERR at 30h, PxSACT at 34h
 *     and PxCI at 38h.
 *   - AHCI 1.3.1: GHC.AE is bit 31 and GHC.HR bit 0; PxCMD.ST is bit 0, PxCMD.FRE
 *     bit 4, PxCMD.FR bit 14 and PxCMD.CR bit 15; PxTFD carries the device's
 *     status byte in its low eight bits, BSY at bit 7 and DRQ at bit 3.
 *   - AHCI 1.3.1, Section 3.3.8, PxSSTS: the DET field, bits 3:0, is 3 when a
 *     device is present and communication is established; the IPM field, bits
 *     11:8, is 1 when the interface is active.
 *   - AHCI 1.3.1, Section 4.2.1, the command header: thirty-two bytes, of which
 *     the first double word holds the command FIS length in double words in bits
 *     4:0, the ATAPI bit at 5, the write bit at 6 and the table length in bits
 *     31:16; the second holds the byte count transferred and the third and
 *     fourth the physical address of the command table.
 *   - AHCI 1.3.1, Section 4.2.3, the command table: the command FIS at offset 0,
 *     the ATAPI command at 40h and the region descriptors from 80h, each of
 *     sixteen bytes holding a physical address, a reserved double word, and a
 *     byte count **less one** in bits 21:0.
 *   - AHCI 1.3.1, Section 4.2.2: the command list is 1024 bytes and must lie
 *     upon a 1024-byte boundary; the received FIS structure is 256 bytes upon a
 *     256-byte boundary; a command table lies upon a 128-byte boundary.
 *   - Serial ATA revision 3.0, the Register Host to Device FIS: type 27h, the C
 *     bit at bit 7 of byte 1, the command at byte 2, the low three address bytes
 *     at 4 to 6, the device at 7, the high three at 8 to 10, and the count at 12
 *     and 13.
 *   - AHCI 1.3.1: the signature a port presents in PxSIG — 00000101h a serial
 *     ATA disk, EB140101h a packet device, C33C0101h an enclosure services
 *     device and 96690101h a port multiplier.
 *   - ATA/ATAPI command set: 25h READ DMA EXT, 35h WRITE DMA EXT, EAh FLUSH
 *     CACHE EXT, ECh IDENTIFY DEVICE.
 *   - PCI Local Bus Specification: an AHCI controller is class 01h, subclass 06h
 *     and programming interface 01h, and its registers are described by the
 *     sixth base address register, which describes memory and not I/O ports.
 */

#ifndef OXYS_AHCI_H
#define OXYS_AHCI_H

#include <oxys/types.h>
#include <oxys/pci.h>

/* The size of a sector, invariant across every device this driver addresses. */
#define AHCI_SECTOR_SIZE 512U

/* The greatest number of ports an adaptor may implement, PI being 32 bits. */
#define AHCI_PORT_COUNT 32U

/*
 * The greatest number of sectors one command may carry.
 *
 * It is not the device's limit, which is 65536 for the extended commands, but
 * this driver's. Each command is described by one region descriptor per page of
 * the caller's buffer, and the table those descriptors sit in is bounded; 128
 * sectors is 64 kibibytes, which is sixteen pages and sixteen descriptors.
 */
#define AHCI_MAXIMUM_SECTORS 128U

/* What answered upon a port, so far as its signature could establish. */
typedef enum AhciDeviceKind
{
    AHCI_DEVICE_NONE = 0, /* Nothing is attached, or the link is not established. */
    AHCI_DEVICE_SATA,     /* A serial ATA disk. */
    AHCI_DEVICE_SATAPI,   /* A packet device: an optical drive, typically. */
    AHCI_DEVICE_ENCLOSURE,/* An enclosure services device. */
    AHCI_DEVICE_MULTIPLIER, /* A port multiplier, which this driver does not follow. */
    AHCI_DEVICE_UNKNOWN   /* Something answered with a signature not named here. */
} AhciDeviceKind;

/* A device found upon one of the adaptor's ports. */
typedef struct AhciDevice
{
    AhciDeviceKind kind;
    uint8_t port;          /* The port number, which is not the index in the table. */
    bool supports_lba48;
    uint64_t sector_count; /* Addressable sectors; zero if not a disk. */
    char model[41];        /* Words 27 to 46 of the identification, trimmed. */
    char serial[21];       /* Words 10 to 19, trimmed. */
} AhciDevice;

/*
 * Finds the host bus adaptor, takes it from the firmware, prepares every port it
 * implements and identifies whatever is attached to them.
 *
 * Returns false where the machine has no AHCI controller, where its registers
 * could not be mapped, or where the adaptor did not accept the handoff. None of
 * those is an error of the machine: most boards have no such controller, and a
 * kernel that treated their absence as a failure would report one upon every
 * machine whose firmware presents its disks as IDE.
 *
 * The ports are driven by polling and their interrupts are left disabled, no
 * handler being registered; a port that interrupted would raise a request that
 * nothing claims. This is the same discipline the ATA driver of sub-task 4.4
 * keeps, and for the same reason: the interrupt flag is clear throughout
 * initialisation, so an interrupt-driven driver could not complete its own
 * discovery.
 */
bool AhciInitialise(void);

/* Whether an adaptor was found and prepared. */
bool AhciIsPresent(void);

/* The number of devices found upon its ports. */
size_t AhciDeviceCount(void);

/* The device at an index below AhciDeviceCount, or null beyond it. */
const AhciDevice *AhciDeviceAt(size_t index);

/* The first device that is a disk, or null where none is. */
const AhciDevice *AhciFirstDisk(void);

/*
 * Reads or writes count sectors beginning at the stated logical block.
 *
 * The buffer holds count times AHCI_SECTOR_SIZE bytes and is addressed by the
 * device itself, not by the processor: its physical pages are described to the
 * adaptor. It must therefore be resident and word-aligned, and a request whose
 * buffer is neither is refused rather than attempted.
 *
 * A count of zero succeeds and does nothing. A range outside the device, a count
 * beyond AHCI_MAXIMUM_SECTORS and an absent buffer are each refused before the
 * adaptor is touched.
 *
 * A write is followed by FLUSH CACHE EXT within the same sequence. A device that
 * has accepted data without committing it reports success and loses it.
 */
bool AhciRead(const AhciDevice *device, uint64_t lba, uint32_t count, void *buffer);
bool AhciWrite(const AhciDevice *device, uint64_t lba, uint32_t count, const void *buffer);

/*
 * Whether a port's PxSSTS describes a device this driver may address: DET of 3,
 * which is a device present with communication established, and IPM of 1, which
 * is an active interface.
 *
 * This is exposed because it cannot be tested otherwise upon a machine whose
 * ports are all empty, and because the two fields must be read together. A DET
 * of 3 with the interface in a sleep state is a port that will not answer, and a
 * driver that read DET alone would issue a command to it and wait out its whole
 * patience for a reply that cannot come.
 */
bool AhciPortIsUsable(uint32_t status);

/*
 * What the signature in PxSIG says is attached.
 *
 * Exposed for the same reason: no board available to this project presents a
 * packet device, an enclosure or a port multiplier upon an AHCI port, and the
 * consequence of confusing the first of those with a disk is a driver that
 * issues READ DMA EXT to something that cannot answer it.
 */
AhciDeviceKind AhciKindFromSignature(uint32_t signature);

/*
 * The first double word of a command header: the length of the command FIS in
 * double words, whether the command writes, and the number of region
 * descriptors that follow it.
 *
 * Exposed because it is the one field of the whole driver that is composed from
 * three numbers at once, and because getting it wrong is silent. A length in
 * bytes rather than double words describes a FIS four times too long; a write
 * bit left clear upon a write makes the adaptor transfer in the wrong direction,
 * which reads the disk into a buffer the caller meant to write from and reports
 * success.
 */
uint32_t AhciDescribeCommand(uint32_t fis_double_words, bool write, uint32_t regions);

/*
 * Registers every disk found as a block device, and returns how many were
 * registered. The names are "ahci0" onward.
 */
size_t AhciRegisterBlockDevices(void);

/* Emits the adaptor, its ports and its accounting upon the diagnostic path. */
void AhciReport(void);

/* Accounting, for the report and for the self-test. */
uint64_t AhciCommandCount(void);
uint64_t AhciSectorsRead(void);
uint64_t AhciSectorsWritten(void);
uint64_t AhciErrorCount(void);
uint64_t AhciRefusalCount(void);
uint64_t AhciTimeoutCount(void);

#endif /* OXYS_AHCI_H */
