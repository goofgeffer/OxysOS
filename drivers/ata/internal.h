/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ata/internal.h
 * Purpose: Declares what the translation units of the ATA driver share with one
 *          another and with nothing else: the table of devices found, the
 *          addresses each channel actually answers at, the accounting and the
 *          record of the last failure, and the register-level discipline every
 *          one of them reaches the task file through.
 * Key definitions: AtaDevices, AtaPresent, the channel address tables, the
 *          accounting, AtaFail, AtaReject, AtaReadRegister, AtaWriteRegister,
 *          AtaAlternateStatus, AtaDelay, AtaSelect, AtaWaitNotBusy,
 *          AtaWaitForData, AtaResetChannel, AtaIdentify, AtaLocateChannels,
 *          AtaRequest.
 * References:
 *   - The specifications are cited where the behaviour they govern is
 *     implemented. This header declares an interface internal to the driver and
 *     states no hardware behaviour of its own; `kernel/include/oxys/ata.h` is
 *     the public one.
 *
 * Why this header exists, and why it is not in kernel/include/oxys/.
 *
 *   `kernel/include/oxys/ata.h` is what the block layer and the self-tests
 *   depend upon, and nothing outside `drivers/ata/` may depend upon anything
 *   declared here. These are the seams left by dividing one translation unit
 *   into six: before the division they were file-scope statics, and they would
 *   be statics still if C offered any way of sharing them among a group of files
 *   without also offering them to everybody.
 *
 *   `docs/design/ARCHITECTURE.md`, Section 2.2, records the rule.
 */

#ifndef OXYS_DRIVERS_ATA_INTERNAL_H
#define OXYS_DRIVERS_ATA_INTERNAL_H

#include <oxys/ata.h>
#include <oxys/pci.h>
#include <oxys/types.h>

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

/*
 * The four addresses, and what was found at each; the device presently selected
 * upon each channel; the accounting; and the description of the most recent
 * failure. Defined in `ata.c`.
 *
 * None of it is guarded against concurrent access. The spinlock of sub-task 6.13
 * exists and has not been applied here; the device table, the selection cache and
 * the counters all require it before a second processor runs — the selection
 * cache most urgently, since two processors transferring upon one channel would
 * each believe the other's device selected.
 */
extern AtaDevice AtaDevices[ATA_DEVICE_COUNT];
extern size_t AtaPresent;
extern uint8_t AtaSelected[ATA_CHANNEL_COUNT];
extern bool AtaSelectionKnown[ATA_CHANNEL_COUNT];
extern uint64_t AtaRead64;
extern uint64_t AtaWritten64;
extern uint64_t AtaCommands;
extern uint64_t AtaErrors;
extern uint64_t AtaRejections;
extern uint64_t AtaTimeouts;
extern const char *AtaError;

/*
 * Where each channel actually answers, and whether it was found in native PCI
 * mode. Defined in `channel.c`, which is what establishes them.
 *
 * These are not the addresses the IBM Personal Computer AT fixed. A controller
 * in native mode answers at the addresses its base address registers give, and a
 * driver that assumed the legacy ones would find no disk upon any machine whose
 * channels are elsewhere — which is a real fault and not a theoretical one; see
 * `docs/storage/DISK.md`, Section 2.1.
 */
extern uint16_t AtaChannelIoBase[ATA_CHANNEL_COUNT];
extern uint16_t AtaChannelControlBase[ATA_CHANNEL_COUNT];
extern bool AtaChannelIsNative[ATA_CHANNEL_COUNT];
extern const PciFunction *AtaController;

/*
 * Records a failure or a rejection, so that a report may say what went wrong and
 * not merely that something did. Each returns false, so that a caller may write
 * `return AtaFail("...")` and neither forget the accounting nor state the reason
 * twice. They are counted apart because a device that failed a command and a
 * request this driver refused to issue are different conditions.
 */
bool AtaFail(const char *reason);
bool AtaReject(const char *reason);

/*
 * The register-level discipline, implemented in `port.c`.
 *
 * Every part of the driver reaches the task file through these, and they are
 * shared rather than duplicated because the timing rules they encode are the
 * ones a driver gets silently wrong: the 400-nanosecond settling delay after a
 * selection, read from the alternate status register so that reading it does not
 * itself clear the interrupt; and the wait for BSY to fall before any other bit
 * of the status register means anything at all.
 */
uint8_t AtaReadRegister(const AtaDevice *device, unsigned int offset);
void AtaWriteRegister(const AtaDevice *device, unsigned int offset, uint8_t value);
uint8_t AtaAlternateStatus(const AtaDevice *device);
void AtaDelay(const AtaDevice *device);
void AtaSelect(const AtaDevice *device, uint8_t address_bits);
bool AtaWaitNotBusy(const AtaDevice *device, uint8_t *final_status);
bool AtaWaitForData(const AtaDevice *device);
void AtaResetChannel(uint8_t channel);

/* Identifies whatever stands at one of the four addresses, in `identify.c`. */
void AtaIdentify(AtaDevice *device);

/* Establishes where each channel answers, in `channel.c`. */
void AtaLocateChannels(void);

/*
 * The validated path to a transfer, implemented in `transfer.c`.
 *
 * `AtaRead` and `AtaWrite` are the public entry points and differ only in a
 * qualifier the shared path cannot express in both directions; this is what both
 * reach, and it is where every judgement about a request is made.
 */
bool AtaRequest(const AtaDevice *device, uint64_t lba, uint32_t count, void *buffer,
                bool writing);

#endif /* OXYS_DRIVERS_ATA_INTERNAL_H */
