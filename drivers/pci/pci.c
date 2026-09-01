/*
 * File: drivers/pci/pci.c
 * Purpose: Implements the enumeration of PCI configuration space by access
 *          mechanism one, recording every function that answered together with
 *          its identifiers, its class, its base address registers and its
 *          interrupt routing, and providing the searches by which a driver finds
 *          the hardware it was written for.
 * Key functions: PciReadConfiguration32, PciWriteConfiguration32,
 *          PciMechanismOnePresent, PciInitialise, PciFindByClass,
 *          PciFindByIdentifier, PciBarBase, PciEnableCommandBits, PciReport.
 * References:
 *   - PCI Local Bus Specification, Configuration Space Access Mechanism #1:
 *     CONFIG_ADDRESS at 0x0CF8 and CONFIG_DATA at 0x0CFC. Bit 31 of the former
 *     is the enable flag, bits 30 to 24 are reserved, bits 23 to 16 the bus,
 *     bits 15 to 11 the device, bits 10 to 8 the function and bits 7 to 2 the
 *     register number, the two least significant bits being always zero.
 *   - PCI Local Bus Specification, Configuration Space Header: the layout of the
 *     first sixteen bytes, common to every header type; bit 7 of the header type
 *     register marking a multifunction device.
 *   - PCI Local Bus Specification: a configuration access to a function that
 *     does not exist completes without error and returns all ones.
 *   - PCI Local Bus Specification, PCI-to-PCI bridge header: the secondary bus
 *     number at offset 0x19 is the bus immediately behind the bridge.
 *   - PCI Local Bus Specification, Base Address Registers: bit 0 selects between
 *     memory and I/O space; for memory, bits 2 and 1 give the width and bit 3
 *     marks the region prefetchable.
 *   - PCI Code and ID Assignment Specification: the base class, subclass and
 *     programming interface codes named by PciClassName.
 */

#include <oxys/pci.h>
#include <oxys/kernel.h>
#include <oxys/io.h>

/*
 * The number of functions recorded. Sixty-four is far beyond what the machines
 * this kernel runs upon present — the QEMU q35 board offers fewer than a dozen —
 * and the array is static because the enumeration is performed once, at a point
 * where a fixed bound is a plainer statement of the limit than an allocation
 * that could fail.
 */
#define PCI_FUNCTION_CAPACITY 64U

/* Bit 31 of CONFIG_ADDRESS, which enables the translation of accesses to data. */
#define PCI_CONFIG_ENABLE UINT32_C(0x80000000)

/* The mask applied to the register number: aligned to a double word. */
#define PCI_CONFIG_REGISTER_MASK UINT32_C(0x000000FC)

/* Bit 0 of a base address register: set for I/O space. */
#define PCI_BAR_IO_SPACE UINT32_C(0x00000001)

/* Bits 2 and 1 of a memory base address register: the width of the address. */
#define PCI_BAR_TYPE_MASK  UINT32_C(0x00000006)
#define PCI_BAR_TYPE_64BIT UINT32_C(0x00000004)

/* The masks that remove the type and attribute bits from a base address. */
#define PCI_BAR_MEMORY_MASK UINT32_C(0xFFFFFFF0)
#define PCI_BAR_IO_MASK     UINT32_C(0xFFFFFFFC)

/* The functions recorded by the enumeration, and how many were recorded. */
static PciFunction PciFunctions[PCI_FUNCTION_CAPACITY];
static size_t PciFunctionsRecorded;

/* Accounting. */
static uint64_t PciBuses;
static uint64_t PciDiscarded;

/* True once PciInitialise has completed an enumeration. */
static bool PciEnumerated;

/*
 * The buses already scanned, one bit apiece. A bridge that reported a secondary
 * bus already visited would otherwise send the enumeration around a cycle
 * forever; the specification does not permit such a topology, but a driver that
 * hangs upon malformed hardware is a worse thing than one bit per bus.
 */
static uint32_t PciBusVisited[PCI_BUS_COUNT / 32U];

/* The buses awaiting a scan, held explicitly rather than upon the stack. */
static uint8_t PciBusQueue[PCI_BUS_COUNT];
static size_t PciBusQueueLength;

/*
 * Composes the value written to CONFIG_ADDRESS. The register number is masked to
 * a double word boundary, the specification providing that the two least
 * significant bits are always zero, so that a caller may pass the offset of a
 * field of any width without first aligning it.
 */
static uint32_t PciComposeAddress(PciAddress address, uint8_t offset)
{
    return PCI_CONFIG_ENABLE | ((uint32_t)address.bus << 16) |
           (((uint32_t)address.device & 0x1FU) << 11) |
           (((uint32_t)address.function & 0x07U) << 8) |
           ((uint32_t)offset & PCI_CONFIG_REGISTER_MASK);
}

uint32_t PciReadConfiguration32(PciAddress address, uint8_t offset)
{
    PortWriteDoubleWord(PCI_CONFIG_ADDRESS_PORT, PciComposeAddress(address, offset));
    return PortReadDoubleWord(PCI_CONFIG_DATA_PORT);
}

void PciWriteConfiguration32(PciAddress address, uint8_t offset, uint32_t value)
{
    PortWriteDoubleWord(PCI_CONFIG_ADDRESS_PORT, PciComposeAddress(address, offset));
    PortWriteDoubleWord(PCI_CONFIG_DATA_PORT, value);
}

/*
 * The narrower accessors extract their field from the double word containing it.
 * The data register is defined for accesses of thirty-two bits, and a kernel
 * that read sixteen of them directly would be relying upon a behaviour of the
 * host bridge rather than upon the specification.
 */
uint16_t PciReadConfiguration16(PciAddress address, uint8_t offset)
{
    const uint32_t word = PciReadConfiguration32(address, offset);
    const unsigned int shift = ((unsigned int)offset & 2U) * 8U;

    return (uint16_t)((word >> shift) & 0xFFFFU);
}

uint8_t PciReadConfiguration8(PciAddress address, uint8_t offset)
{
    const uint32_t word = PciReadConfiguration32(address, offset);
    const unsigned int shift = ((unsigned int)offset & 3U) * 8U;

    return (uint8_t)((word >> shift) & 0xFFU);
}

void PciWriteConfiguration16(PciAddress address, uint8_t offset, uint16_t value)
{
    const uint32_t word = PciReadConfiguration32(address, offset);
    const unsigned int shift = ((unsigned int)offset & 2U) * 8U;
    const uint32_t cleared = word & ~(UINT32_C(0xFFFF) << shift);

    PciWriteConfiguration32(address, offset, cleared | ((uint32_t)value << shift));
}

bool PciMechanismOnePresent(void)
{
    /*
     * CONFIG_ADDRESS is a readable register, so the mechanism announces itself:
     * an enabled address written to it is returned unaltered, and a machine that
     * does not implement the mechanism returns something else. The value chosen
     * addresses no function in particular and the data register is not touched,
     * so nothing is disturbed by asking.
     */
    static const uint32_t probe = PCI_CONFIG_ENABLE;
    uint32_t returned;

    PortWriteDoubleWord(PCI_CONFIG_ADDRESS_PORT, probe);
    returned = PortReadDoubleWord(PCI_CONFIG_ADDRESS_PORT);

    return returned == probe;
}

/* True if the bus has already been scanned. */
static bool PciBusWasVisited(uint8_t bus)
{
    return (PciBusVisited[bus / 32U] & (UINT32_C(1) << (bus % 32U))) != 0U;
}

static void PciMarkBusVisited(uint8_t bus)
{
    PciBusVisited[bus / 32U] |= (UINT32_C(1) << (bus % 32U));
}

/* Adds a bus to the queue of those awaiting a scan, unless already scanned. */
static void PciQueueBus(uint8_t bus)
{
    if (PciBusWasVisited(bus) || (PciBusQueueLength >= PCI_BUS_COUNT))
    {
        return;
    }

    for (size_t index = 0U; index < PciBusQueueLength; ++index)
    {
        if (PciBusQueue[index] == bus)
        {
            return;
        }
    }

    PciBusQueue[PciBusQueueLength] = bus;
    ++PciBusQueueLength;
}

/*
 * Records a function that answered, reading the whole of the description that
 * the common header and the standard header provide.
 *
 * The base address registers are read only for the standard header type. A
 * bridge's header holds its bus numbers and its windows where a standard header
 * holds base addresses four to six, and reading them as base addresses would
 * describe regions that do not exist.
 */
static void PciRecordFunction(PciAddress address, uint16_t vendor_id, uint8_t header_type)
{
    PciFunction *entry;

    if (PciFunctionsRecorded >= PCI_FUNCTION_CAPACITY)
    {
        ++PciDiscarded;
        return;
    }

    entry = &PciFunctions[PciFunctionsRecorded];
    ++PciFunctionsRecorded;

    entry->address = address;
    entry->vendor_id = vendor_id;
    entry->device_id = PciReadConfiguration16(address, PCI_OFFSET_DEVICE_ID);
    entry->revision = PciReadConfiguration8(address, PCI_OFFSET_REVISION);
    entry->programming_interface = PciReadConfiguration8(address, PCI_OFFSET_PROGRAMMING_IF);
    entry->subclass = PciReadConfiguration8(address, PCI_OFFSET_SUBCLASS);
    entry->class_code = PciReadConfiguration8(address, PCI_OFFSET_CLASS);
    entry->header_type = (uint8_t)(header_type & (uint8_t)~PCI_HEADER_TYPE_MULTIFUNCTION);
    entry->multifunction = (header_type & PCI_HEADER_TYPE_MULTIFUNCTION) != 0U;
    entry->interrupt_line = PciReadConfiguration8(address, PCI_OFFSET_INTERRUPT_LINE);
    entry->interrupt_pin = PciReadConfiguration8(address, PCI_OFFSET_INTERRUPT_PIN);

    for (size_t index = 0U; index < PCI_BAR_COUNT; ++index)
    {
        entry->base_address[index] =
            (entry->header_type == PCI_HEADER_TYPE_STANDARD)
                ? PciReadConfiguration32(
                      address, (uint8_t)(PCI_OFFSET_BASE_ADDRESS_0 + (index * 4U)))
                : 0U;
    }

    /*
     * A bridge leads to a bus that would otherwise never be probed. Its
     * secondary bus number is the bus immediately behind it, and is queued
     * rather than descended into, so that the depth of the tree is not the depth
     * of the call stack.
     */
    if ((entry->class_code == PCI_CLASS_BRIDGE) && (entry->subclass == PCI_SUBCLASS_PCI_BRIDGE))
    {
        PciQueueBus(PciReadConfiguration8(address, PCI_OFFSET_SECONDARY_BUS));
    }
}

/*
 * Scans one bus. Function zero of a device is examined first, and the remaining
 * seven only if it declared itself multifunction: the specification does not
 * require a device to decode a function number it does not implement, and a
 * single-function device may answer every function with a copy of itself.
 */
static void PciScanBus(uint8_t bus)
{
    if (PciBusWasVisited(bus))
    {
        return;
    }

    PciMarkBusVisited(bus);
    ++PciBuses;

    for (uint8_t device = 0U; device < PCI_DEVICE_COUNT; ++device)
    {
        PciAddress address = { bus, device, 0U };
        uint16_t vendor_id = PciReadConfiguration16(address, PCI_OFFSET_VENDOR_ID);
        uint8_t header_type;

        if (vendor_id == PCI_VENDOR_INVALID)
        {
            continue;
        }

        header_type = PciReadConfiguration8(address, PCI_OFFSET_HEADER_TYPE);
        PciRecordFunction(address, vendor_id, header_type);

        if ((header_type & PCI_HEADER_TYPE_MULTIFUNCTION) == 0U)
        {
            continue;
        }

        for (uint8_t function = 1U; function < PCI_FUNCTION_COUNT; ++function)
        {
            address.function = function;
            vendor_id = PciReadConfiguration16(address, PCI_OFFSET_VENDOR_ID);

            if (vendor_id == PCI_VENDOR_INVALID)
            {
                continue;
            }

            PciRecordFunction(address, vendor_id,
                              PciReadConfiguration8(address, PCI_OFFSET_HEADER_TYPE));
        }
    }
}

bool PciInitialise(void)
{
    PciAddress host = { 0U, 0U, 0U };
    uint8_t host_header;

    PciFunctionsRecorded = 0U;
    PciBuses = 0U;
    PciDiscarded = 0U;
    PciBusQueueLength = 0U;
    PciEnumerated = false;

    for (size_t index = 0U; index < (PCI_BUS_COUNT / 32U); ++index)
    {
        PciBusVisited[index] = 0U;
    }

    if (!PciMechanismOnePresent())
    {
        return false;
    }

    /*
     * The host bridge is the root of the enumeration and is itself a function
     * upon bus zero. Where it is multifunction, each of its functions is a
     * separate host bridge and the bus each of them is numbered by is the
     * function number; that is the arrangement the specification describes, and
     * bus zero alone would miss the buses behind the others.
     */
    host_header = PciReadConfiguration8(host, PCI_OFFSET_HEADER_TYPE);

    if ((host_header & PCI_HEADER_TYPE_MULTIFUNCTION) == 0U)
    {
        PciQueueBus(0U);
    }
    else
    {
        for (uint8_t function = 0U; function < PCI_FUNCTION_COUNT; ++function)
        {
            host.function = function;

            if (PciReadConfiguration16(host, PCI_OFFSET_VENDOR_ID) != PCI_VENDOR_INVALID)
            {
                PciQueueBus(function);
            }
        }
    }

    /*
     * The queue grows as bridges are found, so the loop reads its length upon
     * each iteration rather than taking a copy of it.
     */
    for (size_t index = 0U; index < PciBusQueueLength; ++index)
    {
        PciScanBus(PciBusQueue[index]);
    }

    PciEnumerated = true;
    return true;
}

size_t PciFunctionCount(void)
{
    return PciFunctionsRecorded;
}

const PciFunction *PciFunctionAt(size_t index)
{
    return (index < PciFunctionsRecorded) ? &PciFunctions[index] : NULL;
}

const PciFunction *PciFindByClass(uint8_t class_code, uint8_t subclass, size_t from,
                                  size_t *found_at)
{
    for (size_t index = from; index < PciFunctionsRecorded; ++index)
    {
        const PciFunction *const entry = &PciFunctions[index];

        if ((class_code != 0xFFU) && (entry->class_code != class_code))
        {
            continue;
        }

        if ((subclass != 0xFFU) && (entry->subclass != subclass))
        {
            continue;
        }

        if (found_at != NULL)
        {
            *found_at = index;
        }

        return entry;
    }

    return NULL;
}

const PciFunction *PciFindByIdentifier(uint16_t vendor_id, uint16_t device_id)
{
    for (size_t index = 0U; index < PciFunctionsRecorded; ++index)
    {
        if ((PciFunctions[index].vendor_id == vendor_id) &&
            (PciFunctions[index].device_id == device_id))
        {
            return &PciFunctions[index];
        }
    }

    return NULL;
}

bool PciBarIsIoPort(const PciFunction *function, size_t index)
{
    if ((function == NULL) || (index >= PCI_BAR_COUNT))
    {
        return false;
    }

    return (function->base_address[index] & PCI_BAR_IO_SPACE) != 0U;
}

uint64_t PciBarBase(const PciFunction *function, size_t index)
{
    uint32_t value;

    if ((function == NULL) || (index >= PCI_BAR_COUNT))
    {
        return 0U;
    }

    value = function->base_address[index];

    if ((value & PCI_BAR_IO_SPACE) != 0U)
    {
        return (uint64_t)(value & PCI_BAR_IO_MASK);
    }

    /*
     * A 64-bit memory register is one address held in two, the second being its
     * upper half rather than a region of its own. Where the pair would run off
     * the end of the header the upper half is taken as zero, a header that
     * declared such a register in its last slot being malformed.
     */
    if (((value & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT) && ((index + 1U) < PCI_BAR_COUNT))
    {
        return (uint64_t)(value & PCI_BAR_MEMORY_MASK) |
               ((uint64_t)function->base_address[index + 1U] << 32);
    }

    return (uint64_t)(value & PCI_BAR_MEMORY_MASK);
}

bool PciEnableCommandBits(PciAddress address, uint16_t bits)
{
    const uint16_t command = PciReadConfiguration16(address, PCI_OFFSET_COMMAND);

    PciWriteConfiguration16(address, PCI_OFFSET_COMMAND, (uint16_t)(command | bits));

    /*
     * The register is read back. A bit the device does not implement reads as
     * zero however it was written, and a driver that assumed its write had taken
     * would go on to address hardware that is not decoding anything.
     */
    return (PciReadConfiguration16(address, PCI_OFFSET_COMMAND) & bits) == bits;
}

uint64_t PciBusesScanned(void)
{
    return PciBuses;
}

uint64_t PciFunctionsDiscarded(void)
{
    return PciDiscarded;
}

const char *PciClassName(uint8_t class_code, uint8_t subclass)
{
    /*
     * Only the classes this kernel expects to meet are named, and the rest are
     * reported by their numbers. A complete table of the assignment
     * specification would be several hundred lines of data serving no purpose
     * but to be printed once at boot.
     */
    switch (class_code)
    {
    case 0x00U:
        return "device predating the class codes";

    case PCI_CLASS_MASS_STORAGE:
        switch (subclass)
        {
        case 0x00U:
            return "SCSI storage controller";
        case PCI_SUBCLASS_IDE:
            return "IDE storage controller";
        case 0x05U:
            return "ATA storage controller";
        case 0x06U:
            return "serial ATA controller";
        case 0x08U:
            return "non-volatile memory controller";
        default:
            return "mass storage controller";
        }

    case 0x02U:
        return "network controller";

    case 0x03U:
        return "display controller";

    case 0x04U:
        return "multimedia controller";

    case 0x05U:
        return "memory controller";

    case PCI_CLASS_BRIDGE:
        switch (subclass)
        {
        case PCI_SUBCLASS_HOST_BRIDGE:
            return "host bridge";
        case 0x01U:
            return "ISA bridge";
        case PCI_SUBCLASS_PCI_BRIDGE:
            return "PCI-to-PCI bridge";
        default:
            return "bridge";
        }

    case 0x07U:
        return "communication controller";

    case 0x08U:
        return "system peripheral";

    case 0x09U:
        return "input device controller";

    case 0x0CU:
        switch (subclass)
        {
        case 0x03U:
            return "USB controller";
        case 0x05U:
            return "SMBus controller";
        default:
            return "serial bus controller";
        }

    default:
        return "device";
    }
}

void PciReport(void)
{
    if (!PciEnumerated)
    {
        KernelWriteString("PCI: configuration mechanism one is unavailable.\n");
        return;
    }

    KernelWriteString("PCI: ");
    KernelWriteDecimal((uint64_t)PciFunctionsRecorded);
    KernelWriteString(" functions upon ");
    KernelWriteDecimal(PciBuses);
    KernelWriteString(" buses, ");
    KernelWriteDecimal(PciDiscarded);
    KernelWriteString(" beyond the table.\n");

    for (size_t index = 0U; index < PciFunctionsRecorded; ++index)
    {
        const PciFunction *const entry = &PciFunctions[index];

        KernelWriteString("  ");
        KernelWriteDecimal((uint64_t)entry->address.bus);
        KernelWriteString(":");
        KernelWriteDecimal((uint64_t)entry->address.device);
        KernelWriteString(".");
        KernelWriteDecimal((uint64_t)entry->address.function);
        KernelWriteString("  ");
        KernelWriteHexadecimal((uint64_t)entry->vendor_id);
        KernelWriteString(":");
        KernelWriteHexadecimal((uint64_t)entry->device_id);
        KernelWriteString("  ");
        KernelWriteString(PciClassName(entry->class_code, entry->subclass));
        KernelWriteString(" (class ");
        KernelWriteHexadecimal((uint64_t)entry->class_code);
        KernelWriteString(", subclass ");
        KernelWriteHexadecimal((uint64_t)entry->subclass);
        KernelWriteString(", interface ");
        KernelWriteHexadecimal((uint64_t)entry->programming_interface);
        KernelWriteString(")");

        if (entry->interrupt_pin != 0U)
        {
            KernelWriteString(", IRQ ");
            KernelWriteDecimal((uint64_t)entry->interrupt_line);
        }

        KernelWriteString("\n");
    }
}
