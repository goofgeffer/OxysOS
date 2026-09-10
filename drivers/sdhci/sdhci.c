/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/sdhci/sdhci.c
 * Purpose: Implements the SD host controller driver: the discovery of the
 *          controller upon the PCI bus, the reset and clocking of it, the
 *          bringing up of the card in its slot by the identification sequence,
 *          the establishment of the card's capacity from its specific data, and
 *          the reading and writing of blocks through the buffer data port.
 * Key functions: SdhciInitialise, SdhciRead, SdhciWrite, SdhciCommandFlags,
 *          SdhciCapacityFromCsd, SdhciRegisterBlockDevices, SdhciReport.
 * References: kernel/include/oxys/sdhci.h states every specification citation
 *          this file relies upon, and docs/storage/SDCARD.md the reasoning.
 *
 * Why this driver exists.
 *
 *   An inexpensive laptop has no disk in any sense the other two storage
 *   drivers understand. Its system sits upon an embedded MultiMediaCard part,
 *   which is attached to a host controller the assignment specification classes
 *   as a *system peripheral* and not as mass storage. Such a machine carries no
 *   mass-storage controller at all: neither the ATA driver of sub-task 4.4 nor
 *   the AHCI driver of 4.7 will ever find anything upon it, and no setting in
 *   its firmware will produce a controller that they would.
 *
 *   This was reported from exactly such a machine. The kernel said the machine
 *   had no disk, of a laptop that had just booted from its own storage.
 *
 * What this driver is not.
 *
 *   It is a driver for the *host controller*, which is the thing upon the bus.
 *   The card behind it is a second device with a command set of its own, and
 *   most of this file is that conversation rather than register programming:
 *   the card must be woken, asked what it is, given an address, asked how large
 *   it is, and selected, before a single block may be read.
 *
 * Programmed input/output, and why.
 *
 *   Every block moves through the buffer data port a word at a time, as the ATA
 *   driver of sub-task 4.4 moves a sector. The controller can master the bus and
 *   this driver does not ask it to. Direct memory access here means composing a
 *   descriptor table in a second format, for a second engine, with a second set
 *   of alignment rules — and the AHCI driver of 4.7 already carries the cost of
 *   that arrangement where it buys the most. Here it would buy a faster path to
 *   a medium that is itself the slow part.
 */

#include <oxys/sdhci.h>
#include <oxys/block.h>
#include <oxys/kernel.h>
#include <oxys/pci.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>

/* The registers, as byte offsets from the mapped base. */
#define SDHCI_ARGUMENT            0x08U
#define SDHCI_BLOCK_SIZE_REGISTER 0x04U
#define SDHCI_BLOCK_COUNT         0x06U
#define SDHCI_TRANSFER_MODE       0x0CU
#define SDHCI_COMMAND             0x0EU
#define SDHCI_RESPONSE            0x10U
#define SDHCI_BUFFER_DATA_PORT    0x20U
#define SDHCI_PRESENT_STATE       0x24U
#define SDHCI_HOST_CONTROL        0x28U
#define SDHCI_POWER_CONTROL       0x29U
#define SDHCI_CLOCK_CONTROL       0x2CU
#define SDHCI_TIMEOUT_CONTROL     0x2EU
#define SDHCI_SOFTWARE_RESET      0x2FU
#define SDHCI_NORMAL_STATUS       0x30U
#define SDHCI_ERROR_STATUS        0x32U
#define SDHCI_NORMAL_ENABLE       0x34U
#define SDHCI_ERROR_ENABLE        0x36U
#define SDHCI_NORMAL_SIGNAL       0x38U
#define SDHCI_ERROR_SIGNAL        0x3AU
#define SDHCI_CAPABILITIES        0x40U
#define SDHCI_VERSION             0xFEU

/* The mapped extent: the standard register set of one slot. */
#define SDHCI_REGISTER_BYTES 0x100U

/* Bits of the present state register. */
#define SDHCI_INHIBIT_COMMAND UINT32_C(0x00000001)
#define SDHCI_INHIBIT_DATA    UINT32_C(0x00000002)
#define SDHCI_BUFFER_WRITABLE UINT32_C(0x00000400)
#define SDHCI_BUFFER_READABLE UINT32_C(0x00000800)
#define SDHCI_CARD_INSERTED   UINT32_C(0x00010000)

/* Bits of the clock control register. */
#define SDHCI_INTERNAL_CLOCK        UINT16_C(0x0001)
#define SDHCI_INTERNAL_CLOCK_STABLE UINT16_C(0x0002)
#define SDHCI_CARD_CLOCK            UINT16_C(0x0004)

/* Bits of the power control register: the bus power, and 3.3 volts. */
#define SDHCI_BUS_POWER   UINT8_C(0x01)
#define SDHCI_VOLTAGE_3V3 UINT8_C(0x0E)

/* Bits of the software reset register. */
#define SDHCI_RESET_ALL     UINT8_C(0x01)
#define SDHCI_RESET_COMMAND UINT8_C(0x02)
#define SDHCI_RESET_DATA    UINT8_C(0x04)

/* Bits of the normal interrupt status register. */
#define SDHCI_COMMAND_COMPLETE UINT16_C(0x0001)
#define SDHCI_TRANSFER_COMPLETE UINT16_C(0x0002)
#define SDHCI_WRITE_READY      UINT16_C(0x0010)
#define SDHCI_READ_READY       UINT16_C(0x0020)
#define SDHCI_ERROR_INTERRUPT  UINT16_C(0x8000)

/* Bits of the transfer mode register. */
#define SDHCI_TRANSFER_BLOCK_COUNT UINT16_C(0x0002)
#define SDHCI_TRANSFER_READ        UINT16_C(0x0010)

/* Fields of the command register. */
#define SDHCI_COMMAND_INDEX_SHIFT 8U
#define SDHCI_COMMAND_DATA        UINT16_C(0x0020)
#define SDHCI_COMMAND_CHECK_INDEX UINT16_C(0x0010)
#define SDHCI_COMMAND_CHECK_CRC   UINT16_C(0x0008)
#define SDHCI_RESPONSE_136        UINT16_C(0x0001)
#define SDHCI_RESPONSE_48         UINT16_C(0x0002)
#define SDHCI_RESPONSE_48_BUSY    UINT16_C(0x0003)

/* The capabilities register: the base clock in bits 15:8 for version 3 and
 * above, and in bits 13:8 below it. */
#define SDHCI_CAPABILITY_CLOCK_SHIFT 8U
#define SDHCI_CAPABILITY_CLOCK_MASK  UINT32_C(0xFF)

/* The card commands this driver issues. */
#define SD_GO_IDLE          UINT8_C(0)
#define MMC_SEND_OP_COND    UINT8_C(1)
#define SD_ALL_SEND_CID     UINT8_C(2)
#define SD_RELATIVE_ADDRESS UINT8_C(3)
#define SD_SELECT_CARD      UINT8_C(7)
#define SD_SEND_IF_COND     UINT8_C(8)
#define SD_SEND_CSD         UINT8_C(9)
#define SD_SET_BLOCK_LENGTH UINT8_C(16)
#define SD_READ_BLOCK       UINT8_C(17)
#define SD_WRITE_BLOCK      UINT8_C(24)
#define SD_APPLICATION      UINT8_C(55)
#define SD_SEND_OP_COND     UINT8_C(41)

/*
 * The argument of CMD8: the supply voltage in bits 11:8 and a check pattern in
 * bits 7:0. A card of the second version answers with both unchanged, which is
 * the whole of the test — a card of the first version does not answer at all.
 */
#define SD_IF_COND_ARGUMENT UINT32_C(0x000001AA)

/* Bits of the operating conditions register. */
#define SD_OCR_BUSY          UINT32_C(0x80000000) /* Clear while powering up. */
#define SD_OCR_CAPACITY      UINT32_C(0x40000000) /* Set when block-addressed. */
#define SD_OCR_VOLTAGE_RANGE UINT32_C(0x00FF8000)
#define SD_OCR_HIGH_CAPACITY UINT32_C(0x40000000)

/* The relative address this driver gives an embedded card, which chooses none
 * of its own. Any value but zero will serve; zero deselects. */
#define MMC_RELATIVE_ADDRESS UINT16_C(1)

/*
 * How many times a register is read while waiting.
 *
 * The interval timer counts by interrupt and the interrupt flag is clear
 * throughout initialisation, so this driver's only clock is the read itself.
 * The power-up of a card is the longest wait here and the specification allows
 * it one second.
 */
#define SDHCI_WAIT_LIMIT 2000000U

/* How many times ACMD41 or CMD1 is repeated while the card powers up. */
#define SDHCI_POWER_UP_LIMIT 20000U

/* The clock this driver runs at, in hertz: the identification rate the
 * specification fixes, and the rate used once a card has been selected. */
#define SDHCI_IDENTIFICATION_CLOCK 400000U
#define SDHCI_TRANSFER_CLOCK       25000000U

static volatile uint8_t *SdhciRegisters;
static PciAddress SdhciAddress;
static bool SdhciFound;
static bool SdhciCardPresent;
static uint32_t SdhciBaseClock;
static uint8_t SdhciVersionByte;
static SdCard SdhciTheCard;

static uint64_t SdhciCommands;
static uint64_t SdhciReadBlocks;
static uint64_t SdhciWrittenBlocks;
static uint64_t SdhciErrors;
static uint64_t SdhciRefusals;
static uint64_t SdhciTimeouts;

/* ---------------------------------------------------------------- registers */

static uint32_t SdhciRead32(uint32_t offset)
{
    return *(volatile uint32_t *)(const void *)(SdhciRegisters + offset);
}

static uint16_t SdhciRead16(uint32_t offset)
{
    return *(volatile uint16_t *)(const void *)(SdhciRegisters + offset);
}

static uint8_t SdhciRead8(uint32_t offset)
{
    return SdhciRegisters[offset];
}

static void SdhciWrite32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(void *)(SdhciRegisters + offset) = value;
}

static void SdhciWrite16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(void *)(SdhciRegisters + offset) = value;
}

static void SdhciWrite8(uint32_t offset, uint8_t value)
{
    SdhciRegisters[offset] = value;
}

/* ------------------------------------------------------- the pure decisions */

uint16_t SdhciCommandFlags(uint8_t index, SdResponseKind response, bool data)
{
    uint16_t value = (uint16_t)((uint16_t)index << SDHCI_COMMAND_INDEX_SHIFT);

    switch (response)
    {
    case SD_RESPONSE_SHORT:
        value |= SDHCI_RESPONSE_48 | SDHCI_COMMAND_CHECK_CRC | SDHCI_COMMAND_CHECK_INDEX;
        break;

    case SD_RESPONSE_BUSY:
        value |= SDHCI_RESPONSE_48_BUSY | SDHCI_COMMAND_CHECK_CRC | SDHCI_COMMAND_CHECK_INDEX;
        break;

    case SD_RESPONSE_LONG:
        /*
         * A response of 136 bits carries no command index to check against, so
         * the index check must be off. Left on, every CMD2 and CMD9 reports an
         * index error and the card is never identified.
         */
        value |= SDHCI_RESPONSE_136 | SDHCI_COMMAND_CHECK_CRC;
        break;

    case SD_RESPONSE_UNCHECKED:
        /*
         * The operating conditions register is returned with neither a CRC nor
         * an index, both fields carrying part of the register instead. Checking
         * either rejects a card that answered correctly.
         */
        value |= SDHCI_RESPONSE_48;
        break;

    case SD_RESPONSE_NONE:
    default:
        break;
    }

    if (data)
    {
        value |= SDHCI_COMMAND_DATA;
    }

    return value;
}

uint64_t SdhciCapacityFromCsd(const uint32_t response[4])
{
    uint32_t structure;

    if (response == NULL)
    {
        return 0U;
    }

    /*
     * The response registers hold the card specific data with its low eight bits
     * — the CRC and the end bit — removed, so bit N of the specific data lies at
     * bit N - 8 here. Every field below is named by its position in the specific
     * data and then shifted by that eight, because reading the specification
     * against code that has already subtracted is how a field ends up one nibble
     * from where it belongs.
     */
    structure = (response[3] >> 22) & UINT32_C(0x3); /* CSD_STRUCTURE, bits 127:126. */

    if (structure == 1U)
    {
        /*
         * Version 2. One field, in fixed units: the capacity is
         * (C_SIZE + 1) * 512 kibibytes, which is (C_SIZE + 1) * 1024 blocks.
         * C_SIZE occupies bits 69:48, which is 61:40 here, and lies wholly
         * within the second response register.
         */
        const uint64_t size = (response[1] >> 8) & UINT32_C(0x003FFFFF);

        return (size + 1U) * 1024U;
    }

    if (structure == 0U)
    {
        /*
         * Version 1. Three fields, in units the card chooses: the capacity in
         * bytes is (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN.
         *
         * C_SIZE is bits 73:62, which is 65:54 here, and straddles two response
         * registers; C_SIZE_MULT is 49:47, which is 41:39; READ_BL_LEN is 83:80,
         * which is 75:72.
         */
        const uint64_t size = ((response[1] >> 22) & UINT32_C(0x3FF)) |
                              ((uint64_t)(response[2] & UINT32_C(0x3)) << 10);
        const uint32_t multiplier = (response[1] >> 7) & UINT32_C(0x7);
        const uint32_t length = (response[2] >> 8) & UINT32_C(0xF);
        const uint64_t bytes = (size + 1U) * ((uint64_t)1U << (multiplier + 2U)) *
                               ((uint64_t)1U << length);

        return bytes / SDHCI_BLOCK_SIZE;
    }

    /*
     * A structure this driver does not know is not guessed at. A capacity
     * computed by the wrong encoding is wrong by a factor of thousands, and a
     * block layer told a card is larger than it is reads beyond the end of it.
     */
    return 0U;
}

/* ------------------------------------------------------------------ waiting */

static bool SdhciWaitPresentClear(uint32_t mask)
{
    for (uint32_t attempt = 0U; attempt < SDHCI_WAIT_LIMIT; ++attempt)
    {
        if ((SdhciRead32(SDHCI_PRESENT_STATE) & mask) == 0U)
        {
            return true;
        }
    }

    ++SdhciTimeouts;
    return false;
}

/*
 * Waits for a bit of the normal interrupt status, and clears it.
 *
 * The error interrupt is watched in the same loop. A command that failed sets it
 * and never sets the bit being waited for, so a loop that watched only the one
 * would wait out its whole patience upon a failure the controller had already
 * reported.
 */
static bool SdhciWaitStatus(uint16_t mask)
{
    for (uint32_t attempt = 0U; attempt < SDHCI_WAIT_LIMIT; ++attempt)
    {
        const uint16_t status = SdhciRead16(SDHCI_NORMAL_STATUS);

        if ((status & SDHCI_ERROR_INTERRUPT) != 0U)
        {
            ++SdhciErrors;
            return false;
        }

        if ((status & mask) != 0U)
        {
            SdhciWrite16(SDHCI_NORMAL_STATUS, mask);
            return true;
        }
    }

    ++SdhciTimeouts;
    return false;
}

/* Clears whatever the controller is reporting, and resets the command and data
 * lines where an error left them holding. */
static void SdhciClearStatus(void)
{
    const uint16_t errors = SdhciRead16(SDHCI_ERROR_STATUS);

    SdhciWrite16(SDHCI_NORMAL_STATUS, 0xFFFFU);

    if (errors != 0U)
    {
        SdhciWrite16(SDHCI_ERROR_STATUS, errors);
        SdhciWrite8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_COMMAND | SDHCI_RESET_DATA);

        for (uint32_t attempt = 0U; attempt < SDHCI_WAIT_LIMIT; ++attempt)
        {
            if ((SdhciRead8(SDHCI_SOFTWARE_RESET) &
                 (SDHCI_RESET_COMMAND | SDHCI_RESET_DATA)) == 0U)
            {
                break;
            }
        }
    }
}

/* ----------------------------------------------------------------- commands */

/*
 * Issues one command and waits for the card to answer it.
 *
 * The command register is written last and is what starts the command; every
 * other register it reads must therefore be in place before it. The inhibit bits
 * of the present state say when the controller is ready to be given one — the
 * data inhibit only where a response is awaited with the card holding the data
 * line, which is why it is waited for separately.
 */
static bool SdhciIssue(uint8_t index, uint32_t argument, SdResponseKind response, bool data,
                       uint32_t answer[4])
{
    if (!SdhciWaitPresentClear(SDHCI_INHIBIT_COMMAND))
    {
        return false;
    }

    if ((response == SD_RESPONSE_BUSY) || data)
    {
        if (!SdhciWaitPresentClear(SDHCI_INHIBIT_DATA))
        {
            return false;
        }
    }

    SdhciClearStatus();
    SdhciWrite32(SDHCI_ARGUMENT, argument);
    ++SdhciCommands;
    SdhciWrite16(SDHCI_COMMAND, SdhciCommandFlags(index, response, data));

    if (!SdhciWaitStatus(SDHCI_COMMAND_COMPLETE))
    {
        return false;
    }

    if (answer != NULL)
    {
        for (uint32_t word = 0U; word < 4U; ++word)
        {
            answer[word] = SdhciRead32(SDHCI_RESPONSE + (word * 4U));
        }
    }

    return true;
}

/* ------------------------------------------------------------------- clock */

/*
 * Sets the card clock to the greatest rate not exceeding the one asked for.
 *
 * The divider is a power of two and the register holds *half* of it, in the
 * eight-bit field of the second version of the specification. A divider written
 * whole runs the card at twice the rate intended, which is a bus that works
 * until it does not.
 */
static bool SdhciSetClock(uint32_t hertz)
{
    uint32_t divisor = 1U;
    uint16_t control;

    SdhciWrite16(SDHCI_CLOCK_CONTROL, 0U);

    if (SdhciBaseClock == 0U)
    {
        return false;
    }

    while (((SdhciBaseClock / divisor) > hertz) && (divisor < 256U))
    {
        divisor *= 2U;
    }

    control = (uint16_t)(((divisor / 2U) & 0xFFU) << 8);
    SdhciWrite16(SDHCI_CLOCK_CONTROL, control | SDHCI_INTERNAL_CLOCK);

    for (uint32_t attempt = 0U; attempt < SDHCI_WAIT_LIMIT; ++attempt)
    {
        if ((SdhciRead16(SDHCI_CLOCK_CONTROL) & SDHCI_INTERNAL_CLOCK_STABLE) != 0U)
        {
            SdhciWrite16(SDHCI_CLOCK_CONTROL,
                         SdhciRead16(SDHCI_CLOCK_CONTROL) | SDHCI_CARD_CLOCK);
            return true;
        }
    }

    ++SdhciTimeouts;
    return false;
}

/* ------------------------------------------------------ bringing up the card */

/*
 * Asks a card to finish powering up, and reports what it is.
 *
 * There are two conversations and which one a card answers is how its kind is
 * established. An SD card answers ACMD41 — CMD55 followed by CMD41 — and an
 * embedded MultiMediaCard answers CMD1 and does not implement ACMD41 at all.
 * Neither answers instantly: the command is repeated until bit 31 of the
 * operating conditions register says the card has finished.
 */
static bool SdhciPowerUpCard(bool second_version)
{
    uint32_t answer[4] = { 0U, 0U, 0U, 0U };
    const uint32_t argument =
        SD_OCR_VOLTAGE_RANGE | (second_version ? SD_OCR_HIGH_CAPACITY : 0U);

    for (uint32_t attempt = 0U; attempt < SDHCI_POWER_UP_LIMIT; ++attempt)
    {
        if (!SdhciIssue(SD_APPLICATION, 0U, SD_RESPONSE_SHORT, false, NULL))
        {
            break;
        }

        if (!SdhciIssue(SD_SEND_OP_COND, argument, SD_RESPONSE_UNCHECKED, false, answer))
        {
            break;
        }

        if ((answer[0] & SD_OCR_BUSY) != 0U)
        {
            SdhciTheCard.kind = ((answer[0] & SD_OCR_CAPACITY) != 0U) ? SD_CARD_SDHC
                                                                      : SD_CARD_SD;
            SdhciTheCard.block_addressed = (answer[0] & SD_OCR_CAPACITY) != 0U;
            return true;
        }
    }

    /*
     * Not an SD card, or not one that answered. An embedded part is asked in its
     * own language before the slot is given up as empty — which is the whole
     * reason this driver exists, the machine that reported the fault having no
     * removable card at all.
     */
    for (uint32_t attempt = 0U; attempt < SDHCI_POWER_UP_LIMIT; ++attempt)
    {
        if (!SdhciIssue(MMC_SEND_OP_COND, SD_OCR_VOLTAGE_RANGE | SD_OCR_HIGH_CAPACITY,
                        SD_RESPONSE_UNCHECKED, false, answer))
        {
            return false;
        }

        if ((answer[0] & SD_OCR_BUSY) != 0U)
        {
            SdhciTheCard.kind = SD_CARD_MMC;
            SdhciTheCard.block_addressed = (answer[0] & SD_OCR_CAPACITY) != 0U;
            return true;
        }
    }

    return false;
}

/* Wakes the card, identifies it, learns its capacity and selects it. */
static bool SdhciBringUpCard(void)
{
    uint32_t answer[4] = { 0U, 0U, 0U, 0U };
    bool second_version;

    if (!SdhciSetClock(SDHCI_IDENTIFICATION_CLOCK))
    {
        return false;
    }

    /* Every card begins in the idle state, whatever the firmware left it in. */
    if (!SdhciIssue(SD_GO_IDLE, 0U, SD_RESPONSE_NONE, false, NULL))
    {
        return false;
    }

    /*
     * CMD8 is the version test and is not merely informative: a card of the
     * second version will not complete its power-up sequence unless it has been
     * asked, so the answer decides both the kind and the argument of what
     * follows. A card of the first version does not answer at all, which is not
     * an error.
     */
    second_version = SdhciIssue(SD_SEND_IF_COND, SD_IF_COND_ARGUMENT, SD_RESPONSE_SHORT,
                                false, answer) &&
                     ((answer[0] & 0xFFFU) == (SD_IF_COND_ARGUMENT & 0xFFFU));

    SdhciClearStatus();

    if (!SdhciPowerUpCard(second_version))
    {
        return false;
    }

    /* The card identification, which is read and not used: the command is what
     * moves the card out of the ready state. */
    if (!SdhciIssue(SD_ALL_SEND_CID, 0U, SD_RESPONSE_LONG, false, answer))
    {
        return false;
    }

    if (SdhciTheCard.kind == SD_CARD_MMC)
    {
        /*
         * An embedded card is given an address rather than reporting one. It has
         * no other card to contend with, so any value but zero will serve, and
         * zero is what deselects.
         */
        SdhciTheCard.relative_address = MMC_RELATIVE_ADDRESS;

        if (!SdhciIssue(SD_RELATIVE_ADDRESS,
                        (uint32_t)SdhciTheCard.relative_address << 16, SD_RESPONSE_SHORT,
                        false, answer))
        {
            return false;
        }
    }
    else
    {
        if (!SdhciIssue(SD_RELATIVE_ADDRESS, 0U, SD_RESPONSE_SHORT, false, answer))
        {
            return false;
        }

        SdhciTheCard.relative_address = (uint16_t)(answer[0] >> 16);
    }

    if (!SdhciIssue(SD_SEND_CSD, (uint32_t)SdhciTheCard.relative_address << 16,
                    SD_RESPONSE_LONG, false, answer))
    {
        return false;
    }

    for (uint32_t word = 0U; word < 4U; ++word)
    {
        SdhciTheCard.specific_data[word] = answer[word];
    }

    SdhciTheCard.block_count = SdhciCapacityFromCsd(SdhciTheCard.specific_data);

    if (!SdhciIssue(SD_SELECT_CARD, (uint32_t)SdhciTheCard.relative_address << 16,
                    SD_RESPONSE_BUSY, false, NULL))
    {
        return false;
    }

    /*
     * The block length is set even for a block-addressed card, which fixes it at
     * 512 and would ignore this. A card of the first version does not, and this
     * driver transfers 512 bytes from either.
     */
    if (!SdhciIssue(SD_SET_BLOCK_LENGTH, SDHCI_BLOCK_SIZE, SD_RESPONSE_SHORT, false, NULL))
    {
        return false;
    }

    /* Identification is over; the bus may run at its ordinary rate. */
    (void)SdhciSetClock(SDHCI_TRANSFER_CLOCK);

    return SdhciTheCard.block_count != 0U;
}

/* ------------------------------------------------------------------- setup */

static const PciFunction *SdhciFindController(void)
{
    size_t found_at = 0U;

    return PciFindByClass(PCI_CLASS_SYSTEM_PERIPHERAL, PCI_SUBCLASS_SD_HOST, 0U, &found_at);
}

bool SdhciInitialise(void)
{
    const PciFunction *const function = SdhciFindController();
    uint64_t base;
    uint32_t capabilities;

    SdhciFound = false;
    SdhciCardPresent = false;
    SdhciTheCard.kind = SD_CARD_NONE;
    SdhciTheCard.block_count = 0U;

    if (function == NULL)
    {
        return false;
    }

    if (PciBarIsIoPort(function, 0U))
    {
        return false;
    }

    base = PciBarBase(function, 0U);

    if (base == 0U)
    {
        return false;
    }

    (void)PciEnableCommandBits(function->address,
                               PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);

    SdhciRegisters = (volatile uint8_t *)KernelDeviceMap(
        base, SDHCI_REGISTER_BYTES, PAGE_ENTRY_WRITABLE | PAGE_ENTRY_CACHE_DISABLE);

    if (SdhciRegisters == NULL)
    {
        return false;
    }

    SdhciAddress = function->address;
    SdhciFound = true;

    /* Whatever the firmware left the controller doing, it is not this driver's
     * arrangement, and every register below is read after the reset. */
    SdhciWrite8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);

    for (uint32_t attempt = 0U; attempt < SDHCI_WAIT_LIMIT; ++attempt)
    {
        if ((SdhciRead8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_ALL) == 0U)
        {
            break;
        }
    }

    SdhciVersionByte = SdhciRead8(SDHCI_VERSION);
    capabilities = SdhciRead32(SDHCI_CAPABILITIES);
    SdhciBaseClock = ((capabilities >> SDHCI_CAPABILITY_CLOCK_SHIFT) &
                      SDHCI_CAPABILITY_CLOCK_MASK) * 1000000U;

    /*
     * No handler is registered for this controller, so nothing is permitted to
     * raise a request. The *status* bits are enabled all the same: they are what
     * this driver polls, and a controller whose status enable is clear reports
     * nothing at all — the command would appear to hang.
     */
    SdhciWrite16(SDHCI_NORMAL_SIGNAL, 0U);
    SdhciWrite16(SDHCI_ERROR_SIGNAL, 0U);
    SdhciWrite16(SDHCI_NORMAL_ENABLE, 0xFFFFU);
    SdhciWrite16(SDHCI_ERROR_ENABLE, 0xFFFFU);
    SdhciWrite16(SDHCI_NORMAL_STATUS, 0xFFFFU);
    SdhciWrite16(SDHCI_ERROR_STATUS, 0xFFFFU);

    /* The timeout counter at its greatest value, this driver having no interest
     * in a shorter one and every interest in not being told a slow card failed. */
    SdhciWrite8(SDHCI_TIMEOUT_CONTROL, 0x0EU);

    if ((SdhciRead32(SDHCI_PRESENT_STATE) & SDHCI_CARD_INSERTED) == 0U)
    {
        return true; /* A controller with an empty slot is a working controller. */
    }

    SdhciWrite8(SDHCI_POWER_CONTROL, SDHCI_VOLTAGE_3V3);
    SdhciWrite8(SDHCI_POWER_CONTROL, SDHCI_VOLTAGE_3V3 | SDHCI_BUS_POWER);

    SdhciCardPresent = SdhciBringUpCard();

    return true;
}

bool SdhciIsPresent(void)
{
    return SdhciFound;
}

const SdCard *SdhciCard(void)
{
    return SdhciCardPresent ? &SdhciTheCard : NULL;
}

/* ------------------------------------------------------ reading and writing */

/* The argument of a transfer command: a block number where the card is
 * block-addressed, and a byte offset where it is not. */
static uint32_t SdhciTransferArgument(uint64_t block)
{
    return SdhciTheCard.block_addressed ? (uint32_t)block
                                        : (uint32_t)(block * SDHCI_BLOCK_SIZE);
}

/* Moves one block through the buffer data port, a word at a time. */
static bool SdhciTransferBlock(uint64_t block, uint8_t *buffer, bool write)
{
    const uint16_t mode = write ? SDHCI_TRANSFER_BLOCK_COUNT
                                : (SDHCI_TRANSFER_BLOCK_COUNT | SDHCI_TRANSFER_READ);

    SdhciWrite16(SDHCI_BLOCK_SIZE_REGISTER, (uint16_t)SDHCI_BLOCK_SIZE);
    SdhciWrite16(SDHCI_BLOCK_COUNT, 1U);
    SdhciWrite16(SDHCI_TRANSFER_MODE, mode);

    if (!SdhciIssue(write ? SD_WRITE_BLOCK : SD_READ_BLOCK, SdhciTransferArgument(block),
                    SD_RESPONSE_SHORT, true, NULL))
    {
        return false;
    }

    if (!SdhciWaitStatus(write ? SDHCI_WRITE_READY : SDHCI_READ_READY))
    {
        return false;
    }

    for (uint32_t offset = 0U; offset < SDHCI_BLOCK_SIZE; offset += 4U)
    {
        if (write)
        {
            const uint32_t word = (uint32_t)buffer[offset] |
                                  ((uint32_t)buffer[offset + 1U] << 8) |
                                  ((uint32_t)buffer[offset + 2U] << 16) |
                                  ((uint32_t)buffer[offset + 3U] << 24);

            SdhciWrite32(SDHCI_BUFFER_DATA_PORT, word);
        }
        else
        {
            const uint32_t word = SdhciRead32(SDHCI_BUFFER_DATA_PORT);

            buffer[offset] = (uint8_t)(word & 0xFFU);
            buffer[offset + 1U] = (uint8_t)((word >> 8) & 0xFFU);
            buffer[offset + 2U] = (uint8_t)((word >> 16) & 0xFFU);
            buffer[offset + 3U] = (uint8_t)((word >> 24) & 0xFFU);
        }
    }

    /*
     * The transfer is not finished when the last word has moved. A write is
     * still in the card, and the completion is what says it has been committed;
     * a caller told a write succeeded before that has been told something this
     * driver does not know.
     */
    return SdhciWaitStatus(SDHCI_TRANSFER_COMPLETE);
}

static bool SdhciRequestIsSound(uint64_t block, uint32_t count, const void *buffer)
{
    if (!SdhciCardPresent || (buffer == NULL))
    {
        return false;
    }

    if ((count == 0U) || (count > SDHCI_MAXIMUM_BLOCKS))
    {
        return false;
    }

    return (block < SdhciTheCard.block_count) &&
           ((SdhciTheCard.block_count - block) >= (uint64_t)count);
}

bool SdhciRead(uint64_t block, uint32_t count, void *buffer)
{
    uint8_t *const bytes = (uint8_t *)buffer;

    if (!SdhciRequestIsSound(block, count, buffer))
    {
        ++SdhciRefusals;
        return false;
    }

    for (uint32_t index = 0U; index < count; ++index)
    {
        if (!SdhciTransferBlock(block + index, &bytes[index * SDHCI_BLOCK_SIZE], false))
        {
            return false;
        }

        ++SdhciReadBlocks;
    }

    return true;
}

bool SdhciWrite(uint64_t block, uint32_t count, const void *buffer)
{
    uint8_t *const bytes = (uint8_t *)(void *)(uintptr_t)buffer;

    if (!SdhciRequestIsSound(block, count, buffer))
    {
        ++SdhciRefusals;
        return false;
    }

    for (uint32_t index = 0U; index < count; ++index)
    {
        if (!SdhciTransferBlock(block + index, &bytes[index * SDHCI_BLOCK_SIZE], true))
        {
            return false;
        }

        ++SdhciWrittenBlocks;
    }

    return true;
}

/* --------------------------------------------------- the block-device face */

static bool SdhciBlockRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    (void)context;
    return SdhciRead(block, count, buffer);
}

static bool SdhciBlockWrite(void *context, uint64_t block, uint32_t count, const void *buffer)
{
    (void)context;
    return SdhciWrite(block, count, buffer);
}

static const BlockOperations SdhciBlockOperations = { SdhciBlockRead, SdhciBlockWrite };

size_t SdhciRegisterBlockDevices(void)
{
    if (!SdhciCardPresent || (SdhciTheCard.block_count == 0U))
    {
        return 0U;
    }

    return (BlockRegister("sd0", &SdhciBlockOperations, &SdhciTheCard, SDHCI_BLOCK_SIZE,
                          SdhciTheCard.block_count, false) != NULL)
               ? 1U
               : 0U;
}

/* ------------------------------------------------------------- accounting */

uint64_t SdhciCommandCount(void)
{
    return SdhciCommands;
}

uint64_t SdhciBlocksRead(void)
{
    return SdhciReadBlocks;
}

uint64_t SdhciBlocksWritten(void)
{
    return SdhciWrittenBlocks;
}

uint64_t SdhciErrorCount(void)
{
    return SdhciErrors;
}

uint64_t SdhciRefusalCount(void)
{
    return SdhciRefusals;
}

uint64_t SdhciTimeoutCount(void)
{
    return SdhciTimeouts;
}

/* ----------------------------------------------------------------- report */

static const char *SdhciCardName(SdCardKind kind)
{
    switch (kind)
    {
    case SD_CARD_SD:
        return "SD card, byte-addressed";
    case SD_CARD_SDHC:
        return "SDHC or SDXC card, block-addressed";
    case SD_CARD_MMC:
        return "embedded MultiMediaCard";
    case SD_CARD_NONE:
    default:
        return "nothing this driver could identify";
    }
}

void SdhciReport(void)
{
    if (!SdhciFound)
    {
        KernelWriteString("SD: no host controller upon this machine.\n");
        return;
    }

    KernelWriteString("SD: host controller at ");
    KernelWriteDecimal((uint64_t)SdhciAddress.bus);
    KernelWriteString(":");
    KernelWriteDecimal((uint64_t)SdhciAddress.device);
    KernelWriteString(".");
    KernelWriteDecimal((uint64_t)SdhciAddress.function);
    KernelWriteString(", specification version ");
    KernelWriteDecimal((uint64_t)SdhciVersionByte + 1U);
    KernelWriteString(".00, base clock ");
    KernelWriteDecimal((uint64_t)(SdhciBaseClock / 1000000U));
    KernelWriteString(" MHz.\n");

    if (!SdhciCardPresent)
    {
        KernelWriteString("SD: the slot is empty, or the card did not answer.\n");
        return;
    }

    KernelWriteString("  ");
    KernelWriteString(SdhciCardName(SdhciTheCard.kind));
    KernelWriteString(", address ");
    KernelWriteHexadecimal((uint64_t)SdhciTheCard.relative_address);
    KernelWriteString(", ");
    KernelWriteDecimal(SdhciTheCard.block_count);
    KernelWriteString(" blocks (");
    KernelWriteDecimal(SdhciTheCard.block_count / 2U);
    KernelWriteString(" KiB)\n");

    KernelWriteString("SD: commands ");
    KernelWriteDecimal(SdhciCommands);
    KernelWriteString(", blocks read ");
    KernelWriteDecimal(SdhciReadBlocks);
    KernelWriteString(", written ");
    KernelWriteDecimal(SdhciWrittenBlocks);
    KernelWriteString(", card errors ");
    KernelWriteDecimal(SdhciErrors);
    KernelWriteString(", requests refused ");
    KernelWriteDecimal(SdhciRefusals);
    KernelWriteString(", timeouts ");
    KernelWriteDecimal(SdhciTimeouts);
    KernelWriteString(".\n");
}
