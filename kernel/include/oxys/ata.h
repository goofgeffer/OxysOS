/*
 * File: kernel/include/oxys/ata.h
 * Purpose: Declares the interface of the ATA driver in programmed input/output
 *          mode: the reset of a channel, the identification of the devices upon
 *          it, and the reading and writing of sectors by 28-bit and 48-bit
 *          logical block addressing.
 * Key definitions: ATA_SECTOR_SIZE, AtaDeviceKind, AtaDevice, AtaInitialise,
 *          AtaDeviceCount, AtaDeviceAt, AtaFirstDisk, AtaRead, AtaWrite,
 *          AtaReport.
 * References:
 *   - AT Attachment with Packet Interface (ATA/ATAPI-6 and later), the command
 *     block registers: at offsets 0 to 7 from the base address lie the data
 *     register, the error and features register, the sector count, the three
 *     logical block address registers, the device register and the status and
 *     command register; the alternate status and device control register lies in
 *     the separate control block.
 *   - ATA/ATAPI, the Status register: bit 7 BSY, bit 6 DRDY, bit 5 DF, bit 3
 *     DRQ, bit 0 ERR.
 *   - ATA/ATAPI, the Device Control register: bit 1 nIEN, which stops the device
 *     interrupting; bit 2 SRST, which resets both devices upon the channel.
 *   - ATA/ATAPI command set: IDENTIFY DEVICE (ECh), READ SECTOR(S) (20h), WRITE
 *     SECTOR(S) (30h), READ SECTOR(S) EXT (24h), WRITE SECTOR(S) EXT (34h),
 *     FLUSH CACHE (E7h) and FLUSH CACHE EXT (EAh).
 *   - IBM Personal Computer AT technical reference: the fixed disk adapter is
 *     decoded at 0x01F0 with its control register at 0x03F6 and raises IRQ14;
 *     the second channel answers at 0x0170 and 0x0376 upon IRQ15.
 */

#ifndef OXYS_ATA_H
#define OXYS_ATA_H

#include <oxys/types.h>

/* The size of a sector, invariant across every device this driver addresses. */
#define ATA_SECTOR_SIZE 512U

/* The compatibility-mode addresses of the two channels, and their request lines. */
#define ATA_PRIMARY_IO_BASE        UINT16_C(0x01F0)
#define ATA_PRIMARY_CONTROL_BASE   UINT16_C(0x03F6)
#define ATA_PRIMARY_IRQ            UINT8_C(14)
#define ATA_SECONDARY_IO_BASE      UINT16_C(0x0170)
#define ATA_SECONDARY_CONTROL_BASE UINT16_C(0x0376)
#define ATA_SECONDARY_IRQ          UINT8_C(15)

/* Two channels of two devices apiece. */
#define ATA_CHANNEL_COUNT 2U
#define ATA_DRIVE_COUNT   2U
#define ATA_DEVICE_COUNT  (ATA_CHANNEL_COUNT * ATA_DRIVE_COUNT)

/* The greatest count a single command may carry, by addressing mode. */
#define ATA_MAXIMUM_SECTORS_LBA28 256U
#define ATA_MAXIMUM_SECTORS_LBA48 65536U

/* The greatest block a 28-bit address can name, plus one. */
#define ATA_LBA28_LIMIT UINT64_C(0x10000000)

/* What answered at an address, so far as the identification could establish. */
typedef enum AtaDeviceKind
{
    ATA_DEVICE_NONE = 0,  /* Nothing answered. */
    ATA_DEVICE_ATA,       /* A device that identified itself as ATA. */
    ATA_DEVICE_ATAPI,     /* A packet device: an optical drive, typically. */
    ATA_DEVICE_SATA,      /* A serial ATA device behind a compatibility bridge. */
    ATA_DEVICE_UNKNOWN    /* Something answered and was not recognised. */
} AtaDeviceKind;

/* A device, present or otherwise, at one of the four addresses. */
typedef struct AtaDevice
{
    AtaDeviceKind kind;
    uint8_t channel; /* 0 primary, 1 secondary. */
    uint8_t drive;   /* 0 master, 1 slave. */
    uint16_t io_base;
    uint16_t control_base;
    bool supports_lba48;
    uint64_t sector_count; /* Addressable sectors; zero if not a disk. */
    char model[41];        /* Words 27 to 46 of the identification, trimmed. */
    char serial[21];       /* Words 10 to 19, trimmed. */
} AtaDevice;

/*
 * Resets both channels and identifies the four devices they may carry.
 *
 * The channels are driven by polling and their interrupts are disabled at the
 * device, no handler being registered for IRQ14 or IRQ15; a device that
 * interrupted would raise a request that nothing claims.
 *
 * Returns false if no device answered anywhere, in which case the machine has no
 * disk this driver can reach and every operation below reports failure.
 */
bool AtaInitialise(void);

/* The number of addresses at which something answered. */
size_t AtaDeviceCount(void);

/* The device at an index below AtaDeviceCount, or null beyond it. */
const AtaDevice *AtaDeviceAt(size_t index);

/*
 * The first device that identified itself as ATA and reported a capacity, which
 * is the disk the kernel will read a filesystem from. Returns null if there is
 * none: a machine that booted from an optical medium has an ATAPI device and no
 * disk, and that is not an error.
 */
const AtaDevice *AtaFirstDisk(void);

/*
 * Reads count sectors beginning at the stated logical block into the buffer,
 * which must have room for count times ATA_SECTOR_SIZE bytes.
 *
 * The addressing mode is chosen from the request and the device: 48-bit where
 * the device supports it and the request needs it, 28-bit otherwise. A request
 * larger than one command may carry is divided into several.
 *
 * Returns false if the device is absent or is not a disk, if the range lies
 * beyond its capacity, if the device reported an error, or if it did not respond
 * within the driver's patience. Nothing is written to the buffer beyond the
 * sectors that were successfully read.
 */
bool AtaRead(const AtaDevice *device, uint64_t lba, uint32_t count, void *buffer);

/*
 * Writes count sectors beginning at the stated logical block from the buffer.
 * The cache of the device is flushed before the call returns, since a device
 * that has accepted the data but not committed it will report success and lose
 * it, and the failure appears only upon a later read.
 *
 * The conditions of failure are those of AtaRead.
 */
bool AtaWrite(const AtaDevice *device, uint64_t lba, uint32_t count, const void *buffer);

/* Accounting, read by the boot-time self-test and by AtaReport. */
uint64_t AtaSectorsRead(void);
uint64_t AtaSectorsWritten(void);
uint64_t AtaCommandCount(void);
uint64_t AtaErrorCount(void);

/*
 * The number of requests the driver declined to issue: a range beyond the
 * capacity, a buffer that is not there, a device that is not a disk. It is
 * counted apart from an error of the hardware because a refusal is the driver
 * working, and a figure that added the two together would show a healthy machine
 * accumulating errors.
 */
uint64_t AtaRejectionCount(void);

uint64_t AtaTimeoutCount(void);

/* A description of the most recent failure, never null. */
const char *AtaLastError(void);

/* Writes every device identified, and the accounting, to the console. */
void AtaReport(void);

#endif /* OXYS_ATA_H */
