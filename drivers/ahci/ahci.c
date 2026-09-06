/*
 * File: drivers/ahci/ahci.c
 * Purpose: Implements the AHCI driver: the discovery of the host bus adaptor
 *          upon the PCI bus, the handoff from the firmware, the preparation of
 *          the ports it implements, the identification of the devices attached
 *          to them, and the reading and writing of sectors by first-party direct
 *          memory access.
 * Key functions: AhciInitialise, AhciRead, AhciWrite, AhciPortIsUsable,
 *          AhciKindFromSignature, AhciDescribeCommand, AhciRegisterBlockDevices,
 *          AhciReport.
 * References: kernel/include/oxys/ahci.h states every specification citation
 *          this file relies upon, and docs/storage/AHCI.md the reasoning.
 *
 * What this driver is, beside the ATA driver of sub-task 4.4.
 *
 *   The two speak to the same command set and reach it by entirely different
 *   means. The ATA driver writes the task file a byte at a time through I/O
 *   ports and moves every word of a sector through one register with the
 *   processor. This driver writes a command into memory, hands the adaptor the
 *   physical address of it, and the adaptor fetches the command, transfers the
 *   sectors into the caller's own pages, and says when it has finished.
 *
 *   That difference is why the earlier driver could not simply be extended. It
 *   is not a different register layout for the same conversation; the disk is
 *   addressed by memory the device reads for itself, so every structure below
 *   is described to the adaptor by its **physical** address, and every buffer a
 *   caller supplies must be resident while the transfer runs.
 *
 * Concurrency, and the single command slot.
 *
 *   An adaptor accepts up to thirty-two commands at once and completes them in
 *   whatever order it likes. This driver issues one, waits for it, and issues
 *   the next: slot zero, always. That is the same discipline the ATA driver
 *   keeps and it is kept for the same reason — the interrupt flag is clear
 *   throughout initialisation, so there is nothing to be woken by, and a driver
 *   that queued commands it could not be told about would poll thirty-two slots
 *   instead of one and be no faster for it. Queuing belongs with the scheduler
 *   of Phase 7, which can wait upon a command without occupying the processor.
 *
 * Cache coherence, and why nothing is flushed.
 *
 *   The command list, the received FIS area and the command table are ordinary
 *   write-back memory that the adaptor reads by bus mastering. Intel SDM,
 *   Volume 3A, Section 11.3.4 provides that the processor's caches are kept
 *   coherent with such accesses by the hardware, so there is nothing to flush.
 *   What must be got right is the *order* of the writes: every structure below
 *   is addressed through a volatile pointer, and the write to PxCI that issues
 *   the command is volatile too, so the compiler may not move the command
 *   ahead of the description of it.
 */

#include <oxys/ahci.h>
#include <oxys/block.h>
#include <oxys/kernel.h>
#include <oxys/memory.h>
#include <oxys/paging.h>
#include <oxys/pci.h>
#include <oxys/pmm.h>
#include <oxys/vmm.h>

/* The generic host control registers, as byte offsets from the mapped base. */
#define AHCI_CAPABILITIES       0x00U
#define AHCI_GLOBAL_CONTROL     0x04U
#define AHCI_INTERRUPT_STATUS   0x08U
#define AHCI_PORTS_IMPLEMENTED  0x0CU
#define AHCI_VERSION            0x10U
#define AHCI_CAPABILITIES_TWO   0x24U
#define AHCI_HANDOFF            0x28U

/* Bits of CAP this driver reads. */
#define AHCI_CAPABILITY_SLOTS_SHIFT 8U
#define AHCI_CAPABILITY_SLOTS_MASK  UINT32_C(0x1F)
#define AHCI_CAPABILITY_PORTS_MASK  UINT32_C(0x1F)
#define AHCI_CAPABILITY_ADDRESS_64  UINT32_C(0x80000000)

/* Bits of GHC. */
#define AHCI_GLOBAL_ENABLE UINT32_C(0x80000000)
#define AHCI_GLOBAL_RESET  UINT32_C(0x00000001)

/* CAP2.BOH, and the bits of BOHC that carry the handoff out. */
#define AHCI_CAPABILITY_HANDOFF UINT32_C(0x00000001)
#define AHCI_HANDOFF_BIOS_OWNED UINT32_C(0x00000001)
#define AHCI_HANDOFF_OS_OWNED   UINT32_C(0x00000002)
#define AHCI_HANDOFF_BUSY       UINT32_C(0x00000010)

/* The port register block, and the registers within it. */
#define AHCI_PORT_REGISTERS_BASE   0x100U
#define AHCI_PORT_REGISTERS_STRIDE 0x80U
#define AHCI_PORT_COMMAND_LIST     0x00U
#define AHCI_PORT_COMMAND_LIST_HIGH 0x04U
#define AHCI_PORT_FIS_BASE         0x08U
#define AHCI_PORT_FIS_BASE_HIGH    0x0CU
#define AHCI_PORT_INTERRUPT_STATUS 0x10U
#define AHCI_PORT_INTERRUPT_ENABLE 0x14U
#define AHCI_PORT_COMMAND          0x18U
#define AHCI_PORT_TASK_FILE        0x20U
#define AHCI_PORT_SIGNATURE        0x24U
#define AHCI_PORT_SATA_STATUS      0x28U
#define AHCI_PORT_SATA_ERROR       0x30U
#define AHCI_PORT_COMMAND_ISSUE    0x38U

/* Bits of PxCMD. */
#define AHCI_PORT_START             UINT32_C(0x0001)
#define AHCI_PORT_FIS_RECEIVE       UINT32_C(0x0010)
#define AHCI_PORT_FIS_RUNNING       UINT32_C(0x4000)
#define AHCI_PORT_COMMAND_RUNNING   UINT32_C(0x8000)

/* PxIS.TFES, the task file error status, which is how a failed command is told
 * from one that is merely slow. */
#define AHCI_PORT_TASK_FILE_ERROR UINT32_C(0x40000000)

/* The device's status byte, in the low eight bits of PxTFD. */
#define AHCI_STATUS_ERROR UINT32_C(0x01)
#define AHCI_STATUS_DATA  UINT32_C(0x08)
#define AHCI_STATUS_BUSY  UINT32_C(0x80)

/* PxSSTS: DET in bits 3:0 and IPM in bits 11:8. */
#define AHCI_DETECTION_MASK    UINT32_C(0x0000000F)
#define AHCI_DETECTION_PRESENT UINT32_C(0x00000003)
#define AHCI_POWER_MASK        UINT32_C(0x00000F00)
#define AHCI_POWER_ACTIVE      UINT32_C(0x00000100)

/* The signatures a port may present. */
#define AHCI_SIGNATURE_SATA       UINT32_C(0x00000101)
#define AHCI_SIGNATURE_SATAPI     UINT32_C(0xEB140101)
#define AHCI_SIGNATURE_ENCLOSURE  UINT32_C(0xC33C0101)
#define AHCI_SIGNATURE_MULTIPLIER UINT32_C(0x96690101)

/* Fields of the first double word of a command header. */
#define AHCI_HEADER_FIS_LENGTH_MASK UINT32_C(0x1F)
#define AHCI_HEADER_WRITE           UINT32_C(0x40)
#define AHCI_HEADER_REGIONS_SHIFT   16U

/* The Register Host to Device FIS: its type, its length in double words, and
 * the bit that distinguishes a command from a mere register update. */
#define AHCI_FIS_HOST_TO_DEVICE  UINT8_C(0x27)
#define AHCI_FIS_DOUBLE_WORDS    5U
#define AHCI_FIS_IS_COMMAND      UINT8_C(0x80)

/* The commands issued. */
#define AHCI_COMMAND_IDENTIFY    UINT8_C(0xEC)
#define AHCI_COMMAND_READ_DMA    UINT8_C(0x25)
#define AHCI_COMMAND_WRITE_DMA   UINT8_C(0x35)
#define AHCI_COMMAND_FLUSH_CACHE UINT8_C(0xEA)

/* The device register: bit 6 selects logical block addressing. */
#define AHCI_DEVICE_LBA UINT8_C(0x40)

/*
 * The layout of the one page each active port is given.
 *
 * A single frame holds every structure a port needs, which is what allows the
 * whole of a port's memory to be one allocation with one physical address. The
 * offsets are chosen to satisfy the alignments the specification requires: the
 * command list upon 1024 bytes, the received FIS structure upon 256, and the
 * command table upon 128. A frame is page-aligned, so every offset below that is
 * a multiple of its own requirement is aligned in the absolute sense too.
 */
#define AHCI_AREA_LIST      0x0000U /* 32 headers of 32 bytes. */
#define AHCI_AREA_RECEIVED  0x0400U /* 256 bytes. */
#define AHCI_AREA_TABLE     0x0500U /* The command table of slot 0. */
#define AHCI_AREA_REGIONS   0x0580U /* Its region descriptors, 128 bytes in. */
#define AHCI_AREA_IDENTITY  0x0800U /* Where IDENTIFY DEVICE is read into. */
#define AHCI_HEADER_BYTES   32U
#define AHCI_REGION_BYTES   16U

/*
 * The greatest number of region descriptors one command may carry.
 *
 * Sixteen pages is 64 kibibytes, which is AHCI_MAXIMUM_SECTORS of 512 bytes, and
 * a caller's buffer may begin part way through a page — so a transfer of that
 * length may straddle seventeen. The capacity is that, and the space for it is
 * reserved in the layout above.
 */
#define AHCI_REGION_CAPACITY 17U

/* Words of the identification data this driver reads. */
#define AHCI_IDENTIFY_WORDS        256U
#define AHCI_IDENTIFY_SERIAL       10U
#define AHCI_IDENTIFY_SERIAL_WORDS 10U
#define AHCI_IDENTIFY_MODEL        27U
#define AHCI_IDENTIFY_MODEL_WORDS  20U
#define AHCI_IDENTIFY_LBA28        60U
#define AHCI_IDENTIFY_COMMAND_SETS 83U
#define AHCI_IDENTIFY_LBA48        100U
#define AHCI_COMMAND_SET_LBA48     UINT16_C(0x0400)

/*
 * How many times a register is read while waiting for a bit to change.
 *
 * The interval timer counts by interrupt and the interrupt flag is clear
 * throughout initialisation, so this driver's only clock is the read itself. A
 * read of a memory-mapped register crosses the bus and is not fast; a million of
 * them is comfortably longer than the 500 milliseconds the specification allows
 * a port to take to stop, and short enough that a machine whose adaptor never
 * answers still finishes booting.
 */
#define AHCI_WAIT_LIMIT 1000000U

/* The mapped registers, and how much of them was mapped. */
static volatile uint8_t *AhciRegisters;
static uint64_t AhciMappedBytes;

/* The adaptor's PCI address, and whether one was found at all. */
static PciAddress AhciAddress;
static bool AhciFound;
static bool AhciSupports64Bit;
static uint32_t AhciImplementedPorts;
static uint32_t AhciSlotCount;
static uint32_t AhciVersion;

/* The devices found, and the memory each active port was given. */
static AhciDevice AhciDevices[AHCI_PORT_COUNT];
static size_t AhciPresent;
static PhysicalAddress AhciPortArea[AHCI_PORT_COUNT];

/* Accounting. */
static uint64_t AhciCommands;
static uint64_t AhciReadSectors;
static uint64_t AhciWrittenSectors;
static uint64_t AhciErrors;
static uint64_t AhciRefusals;
static uint64_t AhciTimeouts;


/* ---------------------------------------------------------------- registers */

static uint32_t AhciReadRegister(uint32_t offset)
{
    return *(volatile uint32_t *)(const void *)(AhciRegisters + offset);
}

static void AhciWriteRegister(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(void *)(AhciRegisters + offset) = value;
}

static uint32_t AhciPortOffset(uint8_t port, uint32_t offset)
{
    return AHCI_PORT_REGISTERS_BASE + ((uint32_t)port * AHCI_PORT_REGISTERS_STRIDE) + offset;
}

static uint32_t AhciPortRead(uint8_t port, uint32_t offset)
{
    return AhciReadRegister(AhciPortOffset(port, offset));
}

static void AhciPortWrite(uint8_t port, uint32_t offset, uint32_t value)
{
    AhciWriteRegister(AhciPortOffset(port, offset), value);
}

/* ------------------------------------------------------------------- memory */

static void AhciFillZero(volatile uint8_t *destination, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        destination[index] = 0U;
    }
}

/* The processor's view of a port's page, through the direct physical map. */
static volatile uint8_t *AhciAreaOf(uint8_t port)
{
    return (volatile uint8_t *)PhysicalToVirtual(AhciPortArea[port]);
}

static void AhciWriteArea32(volatile uint8_t *area, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(void *)(area + offset) = value;
}

/* ------------------------------------------------------- the pure decisions */

bool AhciPortIsUsable(uint32_t status)
{
    return ((status & AHCI_DETECTION_MASK) == AHCI_DETECTION_PRESENT) &&
           ((status & AHCI_POWER_MASK) == AHCI_POWER_ACTIVE);
}

AhciDeviceKind AhciKindFromSignature(uint32_t signature)
{
    switch (signature)
    {
    case AHCI_SIGNATURE_SATA:
        return AHCI_DEVICE_SATA;
    case AHCI_SIGNATURE_SATAPI:
        return AHCI_DEVICE_SATAPI;
    case AHCI_SIGNATURE_ENCLOSURE:
        return AHCI_DEVICE_ENCLOSURE;
    case AHCI_SIGNATURE_MULTIPLIER:
        return AHCI_DEVICE_MULTIPLIER;
    default:
        return AHCI_DEVICE_UNKNOWN;
    }
}

uint32_t AhciDescribeCommand(uint32_t fis_double_words, bool write, uint32_t regions)
{
    uint32_t value = fis_double_words & AHCI_HEADER_FIS_LENGTH_MASK;

    if (write)
    {
        value |= AHCI_HEADER_WRITE;
    }

    return value | (regions << AHCI_HEADER_REGIONS_SHIFT);
}

/* ------------------------------------------------------------------ waiting */

/*
 * Waits for every bit of a mask to become clear in a port register.
 *
 * Returns false where the patience of AHCI_WAIT_LIMIT was exhausted, which is
 * counted apart from an error: a port that never stops is a different fault from
 * a device that refused a command, and a figure that added them together would
 * hide both.
 */
static bool AhciWaitClear(uint8_t port, uint32_t offset, uint32_t mask)
{
    for (uint32_t attempt = 0U; attempt < AHCI_WAIT_LIMIT; ++attempt)
    {
        if ((AhciPortRead(port, offset) & mask) == 0U)
        {
            return true;
        }
    }

    ++AhciTimeouts;
    return false;
}

/* ------------------------------------------------------------------- ports */

/*
 * Stops a port's command engine and its FIS receiver, in that order.
 *
 * The order is the specification's and is not interchangeable. The engine must
 * be told to stop and be observed to have stopped before its memory may be moved
 * — an adaptor still running would fetch a command from a page this driver has
 * since given to something else.
 */
static bool AhciStopPort(uint8_t port)
{
    uint32_t command = AhciPortRead(port, AHCI_PORT_COMMAND);

    AhciPortWrite(port, AHCI_PORT_COMMAND, command & ~AHCI_PORT_START);

    if (!AhciWaitClear(port, AHCI_PORT_COMMAND, AHCI_PORT_COMMAND_RUNNING))
    {
        return false;
    }

    command = AhciPortRead(port, AHCI_PORT_COMMAND);
    AhciPortWrite(port, AHCI_PORT_COMMAND, command & ~AHCI_PORT_FIS_RECEIVE);

    return AhciWaitClear(port, AHCI_PORT_COMMAND, AHCI_PORT_FIS_RUNNING);
}

/* Starts the FIS receiver and then the command engine, which is the reverse. */
static void AhciStartPort(uint8_t port)
{
    uint32_t command = AhciPortRead(port, AHCI_PORT_COMMAND);

    AhciPortWrite(port, AHCI_PORT_COMMAND, command | AHCI_PORT_FIS_RECEIVE);

    command = AhciPortRead(port, AHCI_PORT_COMMAND);
    AhciPortWrite(port, AHCI_PORT_COMMAND, command | AHCI_PORT_START);
}

/*
 * Gives a port its page and points the adaptor at the structures within it.
 *
 * The frame is taken below four gibibytes where the adaptor cannot address more
 * than that. An adaptor told to fetch its command list from an address it can
 * only express in 32 bits would fetch from the low half of it — which is some
 * other page of this kernel, read as a command list, and executed.
 */
static bool AhciPreparePort(uint8_t port)
{
    const PhysicalAddress frame =
        AhciSupports64Bit ? FrameAllocate() : FrameAllocateBelow(UINT64_C(0x100000000));
    volatile uint8_t *area;

    if (frame == 0U)
    {
        return false;
    }

    AhciPortArea[port] = frame;
    area = AhciAreaOf(port);
    AhciFillZero(area, PAGE_SIZE);

    AhciPortWrite(port, AHCI_PORT_COMMAND_LIST, (uint32_t)(frame + AHCI_AREA_LIST));
    AhciPortWrite(port, AHCI_PORT_COMMAND_LIST_HIGH,
                  (uint32_t)((frame + AHCI_AREA_LIST) >> 32));
    AhciPortWrite(port, AHCI_PORT_FIS_BASE, (uint32_t)(frame + AHCI_AREA_RECEIVED));
    AhciPortWrite(port, AHCI_PORT_FIS_BASE_HIGH,
                  (uint32_t)((frame + AHCI_AREA_RECEIVED) >> 32));

    /*
     * The error register is written with what it holds, every bit of it being
     * cleared by writing one. A port whose error bits stand from the firmware's
     * own use of it would report a failure this driver did not cause.
     */
    AhciPortWrite(port, AHCI_PORT_SATA_ERROR, AhciPortRead(port, AHCI_PORT_SATA_ERROR));
    AhciPortWrite(port, AHCI_PORT_INTERRUPT_STATUS,
                  AhciPortRead(port, AHCI_PORT_INTERRUPT_STATUS));

    /* No handler is registered, so no port is permitted to raise a request. */
    AhciPortWrite(port, AHCI_PORT_INTERRUPT_ENABLE, 0U);

    return true;
}

/* ---------------------------------------------------------------- commands */

/*
 * Describes the caller's buffer to the adaptor, one region descriptor per page.
 *
 * A buffer is contiguous to the processor and need not be to the device: the
 * pages behind it are wherever the allocator had them. Each page therefore
 * becomes a descriptor of its own, and the byte count in each is one **less**
 * than the bytes it covers, which is the specification's encoding and the
 * likeliest single mistake in this file.
 *
 * Returns the number of descriptors written, or zero where the buffer could not
 * be described — a page that is not mapped, or one the adaptor cannot address.
 */
static uint32_t AhciDescribeBuffer(volatile uint8_t *area, const void *buffer, uint32_t length)
{
    const uint64_t base = (uint64_t)(uintptr_t)buffer;
    uint32_t written = 0U;
    uint32_t offset = 0U;

    while (offset < length)
    {
        const uint64_t address = base + offset;
        const uint64_t within = address & (PAGE_SIZE - 1U);
        const uint64_t physical = PagingTranslate(address);
        uint32_t chunk = (uint32_t)(PAGE_SIZE - within);
        uint32_t entry;

        if ((physical == 0U) || (written >= AHCI_REGION_CAPACITY))
        {
            return 0U;
        }

        if (!AhciSupports64Bit && ((physical + chunk) > UINT64_C(0x100000000)))
        {
            return 0U;
        }

        if (chunk > (length - offset))
        {
            chunk = length - offset;
        }

        entry = AHCI_AREA_REGIONS + (written * AHCI_REGION_BYTES);

        AhciWriteArea32(area, entry, (uint32_t)physical);
        AhciWriteArea32(area, entry + 4U, (uint32_t)(physical >> 32));
        AhciWriteArea32(area, entry + 8U, 0U);
        AhciWriteArea32(area, entry + 12U, chunk - 1U);

        offset += chunk;
        ++written;
    }

    return written;
}

/*
 * Composes the Register Host to Device FIS of a command in slot zero, issues it
 * and waits for the adaptor to finish.
 *
 * Every command this driver sends is of this shape: a command byte, a 48-bit
 * address, a count, and a buffer that may be absent. The alternative was five
 * routines that each rebuilt the same twenty bytes, and a mistake in the address
 * arithmetic of one of them that the other four would not reveal.
 */
static bool AhciIssue(uint8_t port, uint8_t command, uint64_t lba, uint32_t count,
                      const void *buffer, uint32_t length, bool write)
{
    volatile uint8_t *const area = AhciAreaOf(port);
    const PhysicalAddress table = AhciPortArea[port] + AHCI_AREA_TABLE;
    uint32_t regions = 0U;

    if (!AhciWaitClear(port, AHCI_PORT_TASK_FILE, AHCI_STATUS_BUSY | AHCI_STATUS_DATA))
    {
        return false;
    }

    if (buffer != NULL)
    {
        regions = AhciDescribeBuffer(area, buffer, length);

        if (regions == 0U)
        {
            ++AhciRefusals;
            return false;
        }
    }

    /* The command header of slot zero: the description, the byte count so far,
     * and the physical address of the table the FIS is composed in. */
    AhciWriteArea32(area, AHCI_AREA_LIST,
                    AhciDescribeCommand(AHCI_FIS_DOUBLE_WORDS, write, regions));
    AhciWriteArea32(area, AHCI_AREA_LIST + 4U, 0U);
    AhciWriteArea32(area, AHCI_AREA_LIST + 8U, (uint32_t)table);
    AhciWriteArea32(area, AHCI_AREA_LIST + 12U, (uint32_t)(table >> 32));

    /* The FIS itself, byte by byte, as the specification lays it out. */
    area[AHCI_AREA_TABLE + 0U] = AHCI_FIS_HOST_TO_DEVICE;
    area[AHCI_AREA_TABLE + 1U] = AHCI_FIS_IS_COMMAND;
    area[AHCI_AREA_TABLE + 2U] = command;
    area[AHCI_AREA_TABLE + 3U] = 0U;
    area[AHCI_AREA_TABLE + 4U] = (uint8_t)(lba & 0xFFU);
    area[AHCI_AREA_TABLE + 5U] = (uint8_t)((lba >> 8) & 0xFFU);
    area[AHCI_AREA_TABLE + 6U] = (uint8_t)((lba >> 16) & 0xFFU);
    area[AHCI_AREA_TABLE + 7U] = AHCI_DEVICE_LBA;
    area[AHCI_AREA_TABLE + 8U] = (uint8_t)((lba >> 24) & 0xFFU);
    area[AHCI_AREA_TABLE + 9U] = (uint8_t)((lba >> 32) & 0xFFU);
    area[AHCI_AREA_TABLE + 10U] = (uint8_t)((lba >> 40) & 0xFFU);
    area[AHCI_AREA_TABLE + 11U] = 0U;
    area[AHCI_AREA_TABLE + 12U] = (uint8_t)(count & 0xFFU);
    area[AHCI_AREA_TABLE + 13U] = (uint8_t)((count >> 8) & 0xFFU);
    area[AHCI_AREA_TABLE + 14U] = 0U;
    area[AHCI_AREA_TABLE + 15U] = 0U;
    area[AHCI_AREA_TABLE + 16U] = 0U;
    area[AHCI_AREA_TABLE + 17U] = 0U;
    area[AHCI_AREA_TABLE + 18U] = 0U;
    area[AHCI_AREA_TABLE + 19U] = 0U;

    AhciPortWrite(port, AHCI_PORT_INTERRUPT_STATUS,
                  AhciPortRead(port, AHCI_PORT_INTERRUPT_STATUS));

    ++AhciCommands;
    AhciPortWrite(port, AHCI_PORT_COMMAND_ISSUE, 1U);

    for (uint32_t attempt = 0U; attempt < AHCI_WAIT_LIMIT; ++attempt)
    {
        const uint32_t issued = AhciPortRead(port, AHCI_PORT_COMMAND_ISSUE);
        const uint32_t interrupts = AhciPortRead(port, AHCI_PORT_INTERRUPT_STATUS);

        if ((interrupts & AHCI_PORT_TASK_FILE_ERROR) != 0U)
        {
            ++AhciErrors;
            return false;
        }

        if ((issued & 1U) == 0U)
        {
            if ((AhciPortRead(port, AHCI_PORT_TASK_FILE) & AHCI_STATUS_ERROR) != 0U)
            {
                ++AhciErrors;
                return false;
            }

            return true;
        }
    }

    ++AhciTimeouts;
    return false;
}

/* -------------------------------------------------------- identification */

static void AhciExtractString(const volatile uint16_t *identity, size_t first_word, size_t words,
                              char *destination, size_t capacity)
{
    size_t length = 0U;

    for (size_t index = 0U; (index < words) && ((length + 2U) < capacity); ++index)
    {
        const uint16_t word = identity[first_word + index];

        /* Each word holds two characters with the first in its high half, which
         * is the opposite of the order the processor would place them in. */
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

/* Issues IDENTIFY DEVICE upon a port and records what answered. */
static void AhciIdentify(uint8_t port, AhciDevice *device)
{
    volatile uint8_t *const area = AhciAreaOf(port);
    const volatile uint16_t *const identity =
        (const volatile uint16_t *)(const void *)(area + AHCI_AREA_IDENTITY);

    AhciFillZero(area + AHCI_AREA_IDENTITY, AHCI_SECTOR_SIZE);

    if (!AhciIssue(port, AHCI_COMMAND_IDENTIFY, 0U, 0U,
                   (const void *)(const volatile void *)(area + AHCI_AREA_IDENTITY),
                   AHCI_SECTOR_SIZE, false))
    {
        return;
    }

    device->supports_lba48 =
        (identity[AHCI_IDENTIFY_COMMAND_SETS] & AHCI_COMMAND_SET_LBA48) != 0U;

    device->sector_count = (uint64_t)identity[AHCI_IDENTIFY_LBA28] |
                           ((uint64_t)identity[AHCI_IDENTIFY_LBA28 + 1U] << 16);

    if (device->supports_lba48)
    {
        uint64_t extended = 0U;

        for (size_t index = 0U; index < 4U; ++index)
        {
            extended |= (uint64_t)identity[AHCI_IDENTIFY_LBA48 + index] << (16U * index);
        }

        /* A device may support the extended commands and hold fewer sectors than
         * 28 bits can name, so the larger of the two is the authority. */
        if (extended > device->sector_count)
        {
            device->sector_count = extended;
        }
    }

    AhciExtractString(identity, AHCI_IDENTIFY_MODEL, AHCI_IDENTIFY_MODEL_WORDS, device->model,
                      sizeof(device->model));
    AhciExtractString(identity, AHCI_IDENTIFY_SERIAL, AHCI_IDENTIFY_SERIAL_WORDS,
                      device->serial, sizeof(device->serial));
}

/* ------------------------------------------------------------ the handoff */

/*
 * Asks the firmware for the adaptor, where the adaptor says the firmware may
 * still hold it.
 *
 * This is not ceremony. A system management interrupt may be servicing the
 * controller on the firmware's behalf — presenting a disk as a floppy drive to
 * an operating system that does not know AHCI — and an adaptor reset issued
 * underneath that leaves the firmware writing to registers this driver has
 * since taken. The bit that asks is set, and the bit that says the firmware has
 * let go is waited for.
 */
static bool AhciTakeFromFirmware(void)
{
    uint32_t handoff;

    if ((AhciReadRegister(AHCI_CAPABILITIES_TWO) & AHCI_CAPABILITY_HANDOFF) == 0U)
    {
        /* The adaptor does not implement the handoff, so there is nothing to
         * ask: it was never the firmware's to hold. */
        return true;
    }

    handoff = AhciReadRegister(AHCI_HANDOFF);

    if ((handoff & AHCI_HANDOFF_BIOS_OWNED) == 0U)
    {
        return true;
    }

    AhciWriteRegister(AHCI_HANDOFF, handoff | AHCI_HANDOFF_OS_OWNED);

    for (uint32_t attempt = 0U; attempt < AHCI_WAIT_LIMIT; ++attempt)
    {
        handoff = AhciReadRegister(AHCI_HANDOFF);

        if (((handoff & AHCI_HANDOFF_BIOS_OWNED) == 0U) &&
            ((handoff & AHCI_HANDOFF_BUSY) == 0U))
        {
            return true;
        }
    }

    ++AhciTimeouts;
    return false;
}

/* ------------------------------------------------------------------- setup */

/* The adaptor upon the bus, or null where the machine carries none. */
static const PciFunction *AhciFindAdaptor(void)
{
    size_t position = 0U;
    size_t found_at = 0U;
    const PciFunction *function;

    while ((function = PciFindByClass(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, position,
                                      &found_at)) != NULL)
    {
        position = found_at + 1U;

        if (function->programming_interface == PCI_SATA_INTERFACE_AHCI)
        {
            return function;
        }
    }

    return NULL;
}

bool AhciInitialise(void)
{
    const PciFunction *const function = AhciFindAdaptor();
    uint64_t abar;
    uint32_t capabilities;

    AhciFound = false;
    AhciPresent = 0U;

    if (function == NULL)
    {
        return false;
    }

    /*
     * The adaptor's registers are memory, and it fetches its commands by bus
     * mastering. Both must be permitted before anything below will work: the
     * first because the mapping would otherwise decode to nothing, the second
     * because the adaptor would accept a command and never fetch it.
     */
    (void)PciEnableCommandBits(function->address,
                               PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);

    if (PciBarIsIoPort(function, 5U))
    {
        return false;
    }

    abar = PciBarBase(function, 5U);

    if (abar == 0U)
    {
        return false;
    }

    AhciMappedBytes = AHCI_PORT_REGISTERS_BASE +
                      ((uint64_t)AHCI_PORT_COUNT * AHCI_PORT_REGISTERS_STRIDE);
    AhciRegisters = (volatile uint8_t *)KernelDeviceMap(
        abar, AhciMappedBytes, PAGE_ENTRY_WRITABLE | PAGE_ENTRY_CACHE_DISABLE);

    if (AhciRegisters == NULL)
    {
        return false;
    }

    AhciAddress = function->address;

    if (!AhciTakeFromFirmware())
    {
        return false;
    }

    /*
     * The enable bit is set before anything else is read. An adaptor that has
     * not been enabled presents its registers in a legacy arrangement, and the
     * ports implemented register read from that arrangement is not the ports
     * implemented register.
     */
    AhciWriteRegister(AHCI_GLOBAL_CONTROL,
                      AhciReadRegister(AHCI_GLOBAL_CONTROL) | AHCI_GLOBAL_ENABLE);

    capabilities = AhciReadRegister(AHCI_CAPABILITIES);
    AhciSupports64Bit = (capabilities & AHCI_CAPABILITY_ADDRESS_64) != 0U;
    AhciSlotCount = ((capabilities >> AHCI_CAPABILITY_SLOTS_SHIFT) &
                     AHCI_CAPABILITY_SLOTS_MASK) + 1U;
    AhciVersion = AhciReadRegister(AHCI_VERSION);
    AhciImplementedPorts = AhciReadRegister(AHCI_PORTS_IMPLEMENTED);
    AhciFound = true;

    for (uint8_t port = 0U; port < AHCI_PORT_COUNT; ++port)
    {
        AhciDevice *device;
        uint32_t status;

        if ((AhciImplementedPorts & (UINT32_C(1) << port)) == 0U)
        {
            continue;
        }

        /*
         * A port that will not stop is left alone rather than reprogrammed. Its
         * command list is still being fetched from wherever the firmware put it,
         * and pointing it at a page of this kernel would not stop that.
         */
        if (!AhciStopPort(port))
        {
            continue;
        }

        status = AhciPortRead(port, AHCI_PORT_SATA_STATUS);

        if (!AhciPortIsUsable(status))
        {
            continue;
        }

        if (!AhciPreparePort(port))
        {
            continue;
        }

        AhciStartPort(port);

        device = &AhciDevices[AhciPresent];
        device->port = port;
        device->kind = AhciKindFromSignature(AhciPortRead(port, AHCI_PORT_SIGNATURE));
        device->supports_lba48 = false;
        device->sector_count = 0U;
        device->model[0] = '\0';
        device->serial[0] = '\0';

        if (device->kind == AHCI_DEVICE_SATA)
        {
            AhciIdentify(port, device);
        }

        ++AhciPresent;
    }

    return true;
}

bool AhciIsPresent(void)
{
    return AhciFound;
}

size_t AhciDeviceCount(void)
{
    return AhciPresent;
}

const AhciDevice *AhciDeviceAt(size_t index)
{
    return (index < AhciPresent) ? &AhciDevices[index] : NULL;
}

const AhciDevice *AhciFirstDisk(void)
{
    for (size_t index = 0U; index < AhciPresent; ++index)
    {
        if ((AhciDevices[index].kind == AHCI_DEVICE_SATA) &&
            (AhciDevices[index].sector_count != 0U))
        {
            return &AhciDevices[index];
        }
    }

    return NULL;
}

/* ------------------------------------------------------ reading and writing */

/*
 * The tests every transfer is refused by, before the adaptor is touched.
 *
 * The alignment test is this driver's own and has no counterpart in the ATA
 * driver: a region descriptor's address must be even, the specification
 * reserving its low bit, so an odd buffer would be transferred to the address
 * one below it. That is a silent corruption of whatever lies there, so it is
 * refused rather than rounded.
 */
static bool AhciRequestIsSound(const AhciDevice *device, uint64_t lba, uint32_t count,
                               const void *buffer)
{
    if ((device == NULL) || (buffer == NULL) || !AhciFound)
    {
        return false;
    }

    if ((device->kind != AHCI_DEVICE_SATA) || (device->sector_count == 0U))
    {
        return false;
    }

    if ((count == 0U) || (count > AHCI_MAXIMUM_SECTORS))
    {
        return false;
    }

    if ((lba >= device->sector_count) ||
        ((device->sector_count - lba) < (uint64_t)count))
    {
        return false;
    }

    return ((uint64_t)(uintptr_t)buffer & 1U) == 0U;
}

bool AhciRead(const AhciDevice *device, uint64_t lba, uint32_t count, void *buffer)
{
    if (!AhciRequestIsSound(device, lba, count, buffer))
    {
        ++AhciRefusals;
        return false;
    }

    if (!AhciIssue(device->port, AHCI_COMMAND_READ_DMA, lba, count, buffer,
                   count * AHCI_SECTOR_SIZE, false))
    {
        return false;
    }

    AhciReadSectors += count;
    return true;
}

bool AhciWrite(const AhciDevice *device, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!AhciRequestIsSound(device, lba, count, buffer))
    {
        ++AhciRefusals;
        return false;
    }

    if (!AhciIssue(device->port, AHCI_COMMAND_WRITE_DMA, lba, count, buffer,
                   count * AHCI_SECTOR_SIZE, true))
    {
        return false;
    }

    /*
     * The flush is part of the write and not a courtesy after it. A device that
     * has accepted the data into its own cache reports success and may lose it,
     * and a caller told the write succeeded has no way to discover that.
     */
    if (!AhciIssue(device->port, AHCI_COMMAND_FLUSH_CACHE, 0U, 0U, NULL, 0U, false))
    {
        return false;
    }

    AhciWrittenSectors += count;
    return true;
}

/* --------------------------------------------------- the block-device face */

static bool AhciBlockRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    return AhciRead((const AhciDevice *)context, block, count, buffer);
}

static bool AhciBlockWrite(void *context, uint64_t block, uint32_t count, const void *buffer)
{
    return AhciWrite((const AhciDevice *)context, block, count, buffer);
}

static const BlockOperations AhciBlockOperations = { AhciBlockRead, AhciBlockWrite };

size_t AhciRegisterBlockDevices(void)
{
    static char names[AHCI_PORT_COUNT][6];
    size_t registered = 0U;

    for (size_t index = 0U; index < AhciPresent; ++index)
    {
        AhciDevice *const device = &AhciDevices[index];

        /* Only a disk is registered. A packet device answered the enumeration
         * and is reported, but this driver cannot read one, and a block device
         * whose every transfer fails is worse than an absent one. */
        if ((device->kind != AHCI_DEVICE_SATA) || (device->sector_count == 0U))
        {
            continue;
        }

        names[registered][0] = 'a';
        names[registered][1] = 'h';
        names[registered][2] = 'c';
        names[registered][3] = 'i';
        names[registered][4] = (char)('0' + (char)registered);
        names[registered][5] = '\0';

        if (BlockRegister(names[registered], &AhciBlockOperations, device, AHCI_SECTOR_SIZE,
                          device->sector_count, false) != NULL)
        {
            ++registered;
        }
    }

    return registered;
}

/* ------------------------------------------------------------- accounting */

uint64_t AhciCommandCount(void)
{
    return AhciCommands;
}

uint64_t AhciSectorsRead(void)
{
    return AhciReadSectors;
}

uint64_t AhciSectorsWritten(void)
{
    return AhciWrittenSectors;
}

uint64_t AhciErrorCount(void)
{
    return AhciErrors;
}

uint64_t AhciRefusalCount(void)
{
    return AhciRefusals;
}

uint64_t AhciTimeoutCount(void)
{
    return AhciTimeouts;
}

/* ----------------------------------------------------------------- report */

static const char *AhciKindName(AhciDeviceKind kind)
{
    switch (kind)
    {
    case AHCI_DEVICE_SATA:
        return "serial ATA disk";
    case AHCI_DEVICE_SATAPI:
        return "packet device";
    case AHCI_DEVICE_ENCLOSURE:
        return "enclosure services device";
    case AHCI_DEVICE_MULTIPLIER:
        return "port multiplier, which this driver does not follow";
    case AHCI_DEVICE_NONE:
        return "nothing";
    default:
        return "a device with an unrecognised signature";
    }
}

void AhciReport(void)
{
    uint32_t implemented = 0U;

    if (!AhciFound)
    {
        KernelWriteString("AHCI: no host bus adaptor upon this machine.\n");
        return;
    }

    for (uint8_t port = 0U; port < AHCI_PORT_COUNT; ++port)
    {
        if ((AhciImplementedPorts & (UINT32_C(1) << port)) != 0U)
        {
            ++implemented;
        }
    }

    KernelWriteString("AHCI: adaptor at ");
    KernelWriteDecimal((uint64_t)AhciAddress.bus);
    KernelWriteString(":");
    KernelWriteDecimal((uint64_t)AhciAddress.device);
    KernelWriteString(".");
    KernelWriteDecimal((uint64_t)AhciAddress.function);
    KernelWriteString(", version ");
    KernelWriteHexadecimal((uint64_t)AhciVersion);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)implemented);
    KernelWriteString(" ports implemented, ");
    KernelWriteDecimal((uint64_t)AhciSlotCount);
    KernelWriteString(" command slots, ");
    KernelWriteString(AhciSupports64Bit ? "64-bit addressing.\n" : "32-bit addressing.\n");

    if (AhciPresent == 0U)
    {
        KernelWriteString("AHCI: no device answered upon any implemented port.\n");
        return;
    }

    for (size_t index = 0U; index < AhciPresent; ++index)
    {
        const AhciDevice *const device = &AhciDevices[index];

        KernelWriteString("  port ");
        KernelWriteDecimal((uint64_t)device->port);
        KernelWriteString(": ");
        KernelWriteString(AhciKindName(device->kind));

        if (device->kind == AHCI_DEVICE_SATA)
        {
            KernelWriteString(", ");
            KernelWriteDecimal(device->sector_count);
            KernelWriteString(" sectors (");
            KernelWriteDecimal(device->sector_count / 2U);
            KernelWriteString(" KiB), ");
            KernelWriteString(device->supports_lba48 ? "48-bit addressing, "
                                                     : "28-bit addressing, ");
            KernelWriteString(device->model);
        }

        KernelWriteString("\n");
    }

    KernelWriteString("AHCI: commands ");
    KernelWriteDecimal(AhciCommands);
    KernelWriteString(", sectors read ");
    KernelWriteDecimal(AhciReadSectors);
    KernelWriteString(", written ");
    KernelWriteDecimal(AhciWrittenSectors);
    KernelWriteString(", device errors ");
    KernelWriteDecimal(AhciErrors);
    KernelWriteString(", requests refused ");
    KernelWriteDecimal(AhciRefusals);
    KernelWriteString(", timeouts ");
    KernelWriteDecimal(AhciTimeouts);
    KernelWriteString(".\n");
}
