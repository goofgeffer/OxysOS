/*
 * File: kernel/include/oxys/pci.h
 * Purpose: Declares the interface of the PCI configuration-space enumerator: the
 *          configuration accessors of mechanism one, the description of a
 *          function that answered, and the search by identifier and by class
 *          that every driver of a PCI device will use to find its hardware.
 * Key definitions: PciAddress, PciFunction, PciInitialise, PciReadConfiguration8,
 *          PciReadConfiguration16, PciReadConfiguration32,
 *          PciWriteConfiguration16, PciWriteConfiguration32, PciFunctionCount,
 *          PciFunctionAt, PciFindByClass, PciFindByIdentifier, PciBarIsIoPort,
 *          PciBarBase, PciEnableBusMastering, PciReport.
 * References:
 *   - PCI Local Bus Specification, Configuration Space Access Mechanism #1: two
 *     32-bit I/O locations are used, CONFIG_ADDRESS at 0x0CF8 and CONFIG_DATA at
 *     0x0CFC. Bit 31 of CONFIG_ADDRESS is the enable flag, bits 30 to 24 are
 *     reserved, bits 23 to 16 the bus number, bits 15 to 11 the device number,
 *     bits 10 to 8 the function number and bits 7 to 2 the register number; the
 *     two least significant bits are always zero, every configuration access
 *     being aligned to a double word.
 *   - PCI Local Bus Specification, Configuration Space Header: the first sixteen
 *     bytes are common to every header type and hold the vendor and device
 *     identifiers, the command and status registers, the revision, the
 *     programming interface, the subclass and the class code, the cache line
 *     size, the latency timer, the header type and the built-in self-test
 *     register.
 *   - PCI Local Bus Specification, Configuration Space Header, header type: bit
 *     7 set indicates a multifunction device; the remaining bits give the layout
 *     of the header beyond its first sixteen bytes.
 *   - PCI Local Bus Specification: "When a configuration access attempts to
 *     select a device that does not exist, the host bridge will complete the
 *     access without error, dropping all data on writes and returning all ones
 *     on reads." A vendor identifier of 0xFFFF therefore means no function.
 *   - PCI Local Bus Specification, PCI-to-PCI bridge header (header type 1): the
 *     primary, secondary and subordinate bus numbers lie at offsets 0x18, 0x19
 *     and 0x1A.
 *   - PCI Local Bus Specification, Base Address Registers: bit 0 clear denotes
 *     memory space and set denotes I/O space; for memory, bits 2 and 1 give the
 *     width (0 for 32-bit, 2 for 64-bit, the latter consuming the following
 *     register also) and bit 3 marks the region prefetchable. The base address
 *     is the register with its low four bits cleared for memory and its low two
 *     bits cleared for I/O.
 */

#ifndef OXYS_PCI_H
#define OXYS_PCI_H

#include <oxys/types.h>

/* The two I/O locations of configuration space access mechanism one. */
#define PCI_CONFIG_ADDRESS_PORT UINT16_C(0x0CF8)
#define PCI_CONFIG_DATA_PORT    UINT16_C(0x0CFC)

/* The extent of the address space: 256 buses of 32 devices of 8 functions. */
#define PCI_BUS_COUNT      256U
#define PCI_DEVICE_COUNT   32U
#define PCI_FUNCTION_COUNT 8U

/* The vendor identifier returned for a function that is not there. */
#define PCI_VENDOR_INVALID UINT16_C(0xFFFF)

/* The offsets within the header that this kernel reads by name. */
#define PCI_OFFSET_VENDOR_ID        UINT8_C(0x00)
#define PCI_OFFSET_DEVICE_ID        UINT8_C(0x02)
#define PCI_OFFSET_COMMAND          UINT8_C(0x04)
#define PCI_OFFSET_STATUS           UINT8_C(0x06)
#define PCI_OFFSET_REVISION         UINT8_C(0x08)
#define PCI_OFFSET_PROGRAMMING_IF   UINT8_C(0x09)
#define PCI_OFFSET_SUBCLASS         UINT8_C(0x0A)
#define PCI_OFFSET_CLASS            UINT8_C(0x0B)
#define PCI_OFFSET_HEADER_TYPE      UINT8_C(0x0E)
#define PCI_OFFSET_BASE_ADDRESS_0   UINT8_C(0x10)
#define PCI_OFFSET_SECONDARY_BUS    UINT8_C(0x19)
#define PCI_OFFSET_INTERRUPT_LINE   UINT8_C(0x3C)
#define PCI_OFFSET_INTERRUPT_PIN    UINT8_C(0x3D)

/* Bit 7 of the header type register marks a device as multifunction. */
#define PCI_HEADER_TYPE_MULTIFUNCTION UINT8_C(0x80)

/* The header layouts this kernel distinguishes. */
#define PCI_HEADER_TYPE_STANDARD UINT8_C(0x00)
#define PCI_HEADER_TYPE_BRIDGE   UINT8_C(0x01)

/* Bits of the command register that a driver may need to assert. */
#define PCI_COMMAND_IO_SPACE      UINT16_C(0x0001)
#define PCI_COMMAND_MEMORY_SPACE  UINT16_C(0x0002)
#define PCI_COMMAND_BUS_MASTER    UINT16_C(0x0004)

/* The class codes this kernel names or searches for. */
#define PCI_CLASS_MASS_STORAGE UINT8_C(0x01)
#define PCI_CLASS_BRIDGE       UINT8_C(0x06)

/* Subclasses of those classes. */
#define PCI_SUBCLASS_IDE        UINT8_C(0x01)
#define PCI_SUBCLASS_HOST_BRIDGE UINT8_C(0x00)
#define PCI_SUBCLASS_PCI_BRIDGE UINT8_C(0x04)

/* The number of base address registers in a standard header. */
#define PCI_BAR_COUNT 6U

/* The geographical address of a function upon the bus. */
typedef struct PciAddress
{
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} PciAddress;

/*
 * A function that answered the enumeration. It is a function and not a device:
 * a multifunction device presents as many of these as it implements, and each is
 * separately identified and separately driven.
 */
typedef struct PciFunction
{
    PciAddress address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t programming_interface;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type; /* The layout alone; the multifunction bit is removed. */
    bool multifunction;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint32_t base_address[PCI_BAR_COUNT];
} PciFunction;

/*
 * Reads a register of a function's configuration space. Every access is a double
 * word at the hardware, the specification defining no other width for the data
 * register; the narrower accessors extract the field from the word containing
 * it, so the offset they take need not be aligned.
 *
 * A read of a function that is not present returns all ones rather than failing,
 * which is the behaviour the bus specifies and the basis upon which the
 * enumeration detects absence.
 */
uint32_t PciReadConfiguration32(PciAddress address, uint8_t offset);
uint16_t PciReadConfiguration16(PciAddress address, uint8_t offset);
uint8_t PciReadConfiguration8(PciAddress address, uint8_t offset);

/*
 * Writes a register of a function's configuration space. The 16-bit form reads
 * the containing double word, replaces the half of it addressed and writes it
 * back, the data register admitting no narrower access.
 */
void PciWriteConfiguration32(PciAddress address, uint8_t offset, uint32_t value);
void PciWriteConfiguration16(PciAddress address, uint8_t offset, uint16_t value);

/*
 * True if configuration space access mechanism one is available, established by
 * writing an enabled address to CONFIG_ADDRESS and reading it back. The register
 * is readable, and a machine that does not implement the mechanism does not
 * return what was written.
 */
bool PciMechanismOnePresent(void);

/*
 * Enumerates the bus and records every function that answered. Buses are reached
 * from the host bridge and through each PCI-to-PCI bridge found, so that a bus
 * behind a bridge is scanned but the 256 buses that are not populated are not
 * probed one by one.
 *
 * Returns false if the mechanism is unavailable, in which case no function is
 * recorded and every search below reports nothing.
 */
bool PciInitialise(void);

/* The number of functions recorded by the enumeration. */
size_t PciFunctionCount(void);

/* The function at an index below PciFunctionCount, or null beyond it. */
const PciFunction *PciFunctionAt(size_t index);

/*
 * Finds the first recorded function of a class and subclass at or after the
 * stated index, and reports the index at which it was found so that the search
 * may be continued past it. Either the subclass or the class may be given as
 * 0xFF to match any. Returns null if no such function was recorded.
 */
const PciFunction *PciFindByClass(uint8_t class_code, uint8_t subclass, size_t from,
                                  size_t *found_at);

/* Finds the first recorded function bearing an identifier pair. */
const PciFunction *PciFindByIdentifier(uint16_t vendor_id, uint16_t device_id);

/* True if the stated base address register describes I/O ports rather than memory. */
bool PciBarIsIoPort(const PciFunction *function, size_t index);

/*
 * The base address a base address register describes, with the type and
 * attribute bits removed. A 64-bit memory register is combined with the register
 * following it, which is the upper half of the same address and not a region of
 * its own. Returns zero if the register is unimplemented or the index is beyond
 * the header.
 */
uint64_t PciBarBase(const PciFunction *function, size_t index);

/*
 * Asserts bits of the command register, leaving the others as they stand, and
 * reports whether the register read back with them set. A device whose I/O space
 * decoding the firmware left disabled will answer nothing until this is done.
 */
bool PciEnableCommandBits(PciAddress address, uint16_t bits);

/* Accounting, read by the boot-time self-test and by PciReport. */
uint64_t PciBusesScanned(void);
uint64_t PciFunctionsDiscarded(void);

/* The printable name of a class and subclass pair, never null. */
const char *PciClassName(uint8_t class_code, uint8_t subclass);

/* Writes every function discovered, and the accounting, to the console. */
void PciReport(void);

#endif /* OXYS_PCI_H */
