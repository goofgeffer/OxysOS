/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/dev/storage/sdhci.h
 * Purpose: Declares the interface of the SD host controller driver: the
 *          discovery of the controller upon the PCI bus, the bringing up of the
 *          card attached to it, the establishment of its capacity, and the
 *          reading and writing of blocks through the buffer data port.
 * Key definitions: SDHCI_BLOCK_SIZE, SdCardKind, SdCard, SdhciInitialise,
 *          SdhciCard, SdhciRead, SdhciWrite, SdhciCommandFlags,
 *          SdhciCapacityFromCsd, SdhciRegisterBlockDevices, SdhciReport.
 * References:
 *   - SD Host Controller Simplified Specification, version 4.20, Table 2-1, the
 *     register map: the argument at 008h, the block size at 004h and the block
 *     count at 006h, the transfer mode at 00Ch and the command at 00Eh, the four
 *     response registers from 010h, the buffer data port at 020h, the present
 *     state at 024h, host control 1 at 028h, power control at 029h, clock
 *     control at 02Ch, timeout control at 02Eh, software reset at 02Fh, the
 *     normal interrupt status at 030h and the error interrupt status at 032h,
 *     their enables at 034h and 036h, the capabilities at 040h, and the host
 *     controller version at 0FEh.
 *   - SD Host Controller Simplified Specification 4.20, Table 2-10, the command
 *     register: the command index in bits 13:8, data present select at bit 5,
 *     command index check enable at bit 4, command CRC check enable at bit 3,
 *     and the response type select in bits 1:0 — 00 no response, 01 a response
 *     of 136 bits, 10 one of 48 bits, and 11 one of 48 bits after which busy is
 *     checked.
 *   - SD Host Controller Simplified Specification 4.20, Tables 2-15 and 2-16,
 *     the present state register: command inhibit (CMD) at bit 0 and command
 *     inhibit (DAT) at bit 1, buffer write enable at bit 10 and buffer read
 *     enable at bit 11, card inserted at bit 16 and card state stable at 17.
 *   - SD Host Controller Simplified Specification 4.20, the clock control
 *     register: internal clock enable at bit 0, internal clock stable at bit 1,
 *     SD clock enable at bit 2, and the frequency divider in bits 15:8; the
 *     power control register: SD bus power at bit 0 and the bus voltage in bits
 *     3:1, the value 7 selecting 3.3 volts; the software reset register: reset
 *     for all at bit 0, for the command line at bit 1 and for the data line at 2.
 *   - SD Host Controller Simplified Specification 4.20, the normal interrupt
 *     status register: command complete at bit 0, transfer complete at bit 1,
 *     buffer write ready at bit 4, buffer read ready at bit 5, and the error
 *     interrupt at bit 15.
 *   - SD Physical Layer Simplified Specification, version 8.00: the commands
 *     CMD0 GO_IDLE_STATE, CMD2 ALL_SEND_CID, CMD3 SEND_RELATIVE_ADDR, CMD7
 *     SELECT/DESELECT_CARD, CMD8 SEND_IF_COND, CMD9 SEND_CSD, CMD16
 *     SET_BLOCKLEN, CMD17 READ_SINGLE_BLOCK, CMD24 WRITE_BLOCK, CMD55 APP_CMD
 *     and ACMD41 SD_SEND_OP_COND; the operating conditions register, whose bit
 *     31 is set when the card has finished its power-up sequence and whose bit
 *     30, the card capacity status, distinguishes a block-addressed card from a
 *     byte-addressed one.
 *   - SD Physical Layer Simplified Specification 8.00, the card specific data:
 *     CSD_STRUCTURE in bits 127:126; for version 1, READ_BL_LEN in bits 83:80,
 *     C_SIZE in 73:62 and C_SIZE_MULT in 49:47, the capacity in bytes being
 *     (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN; for version 2, C_SIZE
 *     in bits 69:48, the capacity being (C_SIZE + 1) * 512 kibibytes.
 *   - SD Host Controller Simplified Specification 4.20, Section 2.2.7: a
 *     response of 136 bits is presented in the four response registers with its
 *     low eight bits — the CRC and the end bit — removed, so that bit N of the
 *     card specific data appears at bit N - 8 of the response.
 *   - JEDEC Standard JESD84-B51, the embedded MultiMediaCard: CMD1 SEND_OP_COND
 *     in place of ACMD41, and CMD3 assigning a relative address the host chooses
 *     rather than reporting one the card chose.
 *   - PCI Local Bus Specification: an SD host controller is class 08h, subclass
 *     05h, and its registers are described by its first base address register.
 */

#ifndef OXYS_DEV_STORAGE_SDHCI_H
#define OXYS_DEV_STORAGE_SDHCI_H

#include <oxys/types.h>
#include <oxys/dev/pci.h>

/* The block size this driver uses, which every card supports. */
#define SDHCI_BLOCK_SIZE 512U

/* The greatest number of blocks one call may carry. Each is a command of its
 * own, so this bounds only how long a caller may occupy the processor. */
#define SDHCI_MAXIMUM_BLOCKS 128U

/* What answered, so far as the initialisation could establish. */
typedef enum SdCardKind
{
    SD_CARD_NONE = 0,   /* No card, or none that answered. */
    SD_CARD_SD,         /* An SD card of the first version: byte-addressed. */
    SD_CARD_SDHC,       /* An SD card of the second version: block-addressed. */
    SD_CARD_MMC         /* An embedded MultiMediaCard, which answers CMD1. */
} SdCardKind;

/* The card upon the controller's one slot. */
typedef struct SdCard
{
    SdCardKind kind;
    bool block_addressed; /* Whether an argument is a block number or a byte offset. */
    uint16_t relative_address;
    uint64_t block_count;
    uint32_t specific_data[4]; /* The card specific data, as the response gave it. */
} SdCard;

/*
 * The response a command expects, which decides three bits of the command
 * register and cannot be inferred from the command index.
 */
typedef enum SdResponseKind
{
    SD_RESPONSE_NONE = 0, /* CMD0: nothing comes back. */
    SD_RESPONSE_SHORT,    /* 48 bits, checked. */
    SD_RESPONSE_BUSY,     /* 48 bits, after which the card holds the data line low. */
    SD_RESPONSE_LONG,     /* 136 bits: the card identification, or the specific data. */
    SD_RESPONSE_UNCHECKED /* 48 bits with neither the CRC nor the index checked. */
} SdResponseKind;

/*
 * Finds the controller, brings up the card in its slot and establishes what the
 * card is and how large.
 *
 * Returns false where the machine has no SD host controller, where its registers
 * could not be mapped, where the slot is empty, or where the card did not
 * complete its power-up sequence. None of those is an error of the machine: most
 * boards have no such controller, and a kernel that treated their absence as a
 * failure would report one upon every machine with an ordinary disk.
 */
bool SdhciInitialise(void);

/* Whether a controller was found and prepared. */
bool SdhciIsPresent(void);

/* The card in the slot, or null where none answered. */
const SdCard *SdhciCard(void);

/*
 * Reads or writes count blocks of SDHCI_BLOCK_SIZE beginning at the stated
 * block. A count of zero succeeds and does nothing; an absent buffer, a range
 * outside the card and a count beyond SDHCI_MAXIMUM_BLOCKS are each refused
 * before the controller is touched.
 *
 * Each block is one command. The transfer is by programmed input/output through
 * the buffer data port, so the caller's buffer needs no particular alignment and
 * no residence beyond the call.
 */
bool SdhciRead(uint64_t block, uint32_t count, void *buffer);
bool SdhciWrite(uint64_t block, uint32_t count, const void *buffer);

/*
 * The value written to the command register for a command index, the response
 * it expects, and whether data accompanies it.
 *
 * Exposed because it is composed from four fields at once and every mistake in
 * it is quiet. A response type of 48 bits where 136 were due reads a quarter of
 * the card specific data and leaves the capacity arithmetic to work upon it; the
 * index check enabled upon a response that carries no index — CMD2 and CMD9
 * carry none — makes every such command report an index error.
 */
uint16_t SdhciCommandFlags(uint8_t index, SdResponseKind response, bool data);

/*
 * The number of blocks a card holds, from its card specific data.
 *
 * Exposed because it is the arithmetic in this driver most likely to be wrong
 * and least likely to say so. There are two encodings, chosen by a field of the
 * same register; they differ in where every other field sits, in the units of
 * the answer, and in whether a multiplier applies at all. A capacity computed by
 * the wrong one of them is not a small error but a wrong answer by a factor of
 * thousands — and a block layer told a card is larger than it is will read
 * beyond the end of it and be answered with nothing.
 *
 * The response is as the four response registers present it: the card specific
 * data with its low eight bits removed, so that bit N of the specific data lies
 * at bit N - 8 here.
 */
uint64_t SdhciCapacityFromCsd(const uint32_t response[4]);

/* Registers the card as a block device named "sd0". Returns how many were
 * registered, which is one or none. */
size_t SdhciRegisterBlockDevices(void);

/* Emits the controller, the card and the accounting upon the diagnostic path. */
void SdhciReport(void);

/* Accounting, for the report and for the self-test. */
uint64_t SdhciCommandCount(void);
uint64_t SdhciBlocksRead(void);
uint64_t SdhciBlocksWritten(void);
uint64_t SdhciErrorCount(void);
uint64_t SdhciRefusalCount(void);
uint64_t SdhciTimeoutCount(void);

#endif /* OXYS_DEV_STORAGE_SDHCI_H */
