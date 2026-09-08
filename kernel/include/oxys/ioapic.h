/*
 * File: kernel/include/oxys/ioapic.h
 * Purpose: Declares the interface of the I/O Advanced Programmable Interrupt
 *          Controller driver: the mapping of each unit the ACPI tables declare,
 *          the indirect register pair its registers are reached through, and the
 *          redirection table entry by which an interrupt input is given a vector,
 *          a destination, a polarity, a trigger mode and a mask.
 * Key definitions: IOAPIC_WINDOW_OFFSET, IOAPIC_INDEX_*, IOAPIC_REDIRECTION_*,
 *          IoApicInitialise, IoApicRouteGlobalInterrupt, IoApicSetMask,
 *          IoApicGlobalInterruptIsMasked, IoApicEntryValue, IoApicCount,
 *          IoApicInputCount, IoApicReport.
 * References:
 *   - Intel 82093AA I/O Advanced Programmable Interrupt Controller datasheet
 *     (order number 290566-001), Section 3.1: the registers are reached
 *     indirectly through IOREGSEL at offset 0x00, which selects a register by
 *     its 8-bit address, and IOWIN at offset 0x10, through which the selected
 *     register is read and written.
 *   - 82093AA datasheet, Sections 3.2.1 to 3.2.3: IOAPICID at index 0x00, whose
 *     identification occupies bits 27:24; IOAPICVER at index 0x01, whose version
 *     occupies bits 7:0 and whose maximum redirection entry — the count of
 *     interrupt inputs less one — occupies bits 23:16; and IOAPICARB at 0x02.
 *   - 82093AA datasheet, Section 3.2.4: the twenty-four redirection table
 *     registers occupy indices 0x10 upward, two 32-bit registers to each 64-bit
 *     entry, whose fields are the vector in bits 7:0, the delivery mode in bits
 *     10:8, the destination mode in bit 11, the delivery status in bit 12, the
 *     input polarity in bit 13 where one denotes active low, the remote
 *     in-service flag in bit 14, the trigger mode in bit 15 where one denotes
 *     level sensitive, the mask in bit 16, and the destination in bits 63:56.
 *   - ACPI Specification 6.5, Section 5.2.12.3: each I/O APIC structure declares
 *     the unit's address and the global system interrupt its first input carries.
 *   - ACPI Specification 6.5, Section 5.2.12.4: upon a machine that supports both
 *     interrupt models, global system interrupts 0 to 15 carry the 8259A request
 *     lines 0 to 15 except where an Interrupt Source Override says otherwise.
 *   - docs/devices/APIC.md: how a request line reaches a vector once the 8259A
 *     has been retired.
 */

#ifndef OXYS_IOAPIC_H
#define OXYS_IOAPIC_H

#include <oxys/types.h>

/* The two memory-mapped registers of Section 3.1, and the extent that spans
 * them. The datasheet places IOWIN sixteen bytes beyond IOREGSEL. */
#define IOAPIC_SELECT_OFFSET UINT64_C(0x00)
#define IOAPIC_WINDOW_OFFSET UINT64_C(0x10)
#define IOAPIC_REGISTER_EXTENT UINT64_C(0x20)

/* The register indices of Section 3.2. */
#define IOAPIC_INDEX_IDENTIFIER        UINT8_C(0x00)
#define IOAPIC_INDEX_VERSION           UINT8_C(0x01)
#define IOAPIC_INDEX_ARBITRATION       UINT8_C(0x02)
#define IOAPIC_INDEX_REDIRECTION_BASE  UINT8_C(0x10)

/* Fields of IOAPICID and IOAPICVER. */
#define IOAPIC_IDENTIFIER_SHIFT   24U
#define IOAPIC_IDENTIFIER_MASK    UINT32_C(0x0F)
#define IOAPIC_VERSION_MASK       UINT32_C(0xFF)
#define IOAPIC_MAX_REDIRECTION_SHIFT 16U
#define IOAPIC_MAX_REDIRECTION_MASK  UINT32_C(0xFF)

/* Fields of a redirection table entry, per Section 3.2.4. */
#define IOAPIC_REDIRECTION_VECTOR_MASK     UINT64_C(0x00000000000000FF)
#define IOAPIC_REDIRECTION_DELIVERY_FIXED  UINT64_C(0x0000000000000000)
#define IOAPIC_REDIRECTION_DELIVERY_LOWEST UINT64_C(0x0000000000000100)
#define IOAPIC_REDIRECTION_DELIVERY_NMI    UINT64_C(0x0000000000000400)
#define IOAPIC_REDIRECTION_LOGICAL         UINT64_C(0x0000000000000800)
#define IOAPIC_REDIRECTION_DELIVERY_PENDING UINT64_C(0x0000000000001000)
#define IOAPIC_REDIRECTION_ACTIVE_LOW      UINT64_C(0x0000000000002000)
#define IOAPIC_REDIRECTION_REMOTE_IN_SERVICE UINT64_C(0x0000000000004000)
#define IOAPIC_REDIRECTION_LEVEL_TRIGGERED UINT64_C(0x0000000000008000)
#define IOAPIC_REDIRECTION_MASKED          UINT64_C(0x0000000000010000)
#define IOAPIC_REDIRECTION_DESTINATION_SHIFT 56U

/* The greatest number of interrupt inputs one unit may declare. The datasheet
 * gives the range of the maximum redirection entry field as 0 to 239. */
#define IOAPIC_INPUT_MAXIMUM 240U

/*
 * Maps every I/O APIC the Multiple APIC Description Table declares and masks
 * every one of their redirection table entries.
 *
 * Every entry is masked for the same reason every 8259A line is: an input whose
 * device has no driver would otherwise deliver a vector nothing had registered,
 * and a level-triggered input would continue to deliver it, the remote
 * in-service flag never being cleared by an end-of-interrupt that never comes.
 *
 * AcpiInitialise must have run. Returns false where the tables declare no I/O
 * APIC, or where none could be mapped.
 */
bool IoApicInitialise(void);

/* The number of units mapped, and the total number of interrupt inputs they
 * present between them. */
size_t IoApicCount(void);
uint32_t IoApicInputCount(void);

/* The identifier, address, version and input count of one mapped unit, for the
 * report and the self-test. Returns false where the index names no unit. */
bool IoApicDescribe(size_t index, uint8_t *identifier, PhysicalAddress *address,
                    uint32_t *interrupt_base, uint32_t *input_count);

/*
 * Programmes the redirection table entry carrying a global system interrupt: the
 * vector it is to present, the local APIC identifier it is to present it to, and
 * how its input is wired. The entry is left masked; IoApicSetMask permits it.
 *
 * The entry is written high half first. The datasheet requires two 32-bit
 * accesses for one 64-bit entry, and the low half holds the mask: writing the
 * low half first would, for the instant between the two writes, publish an
 * unmasked entry whose destination was still whatever it held before.
 *
 * Returns false where no mapped unit carries the interrupt, or where the vector
 * is one the controller refuses.
 */
bool IoApicRouteGlobalInterrupt(uint32_t global_interrupt, uint8_t vector,
                                uint8_t destination, bool active_low,
                                bool level_triggered);

/* Sets or clears the mask of the entry carrying a global system interrupt.
 * Returns false where no mapped unit carries it. */
bool IoApicSetMask(uint32_t global_interrupt, bool masked);

/* Whether the entry carrying a global system interrupt is masked. A global
 * interrupt no mapped unit carries is reported as masked, that being the
 * truthful answer: nothing will deliver it. */
bool IoApicGlobalInterruptIsMasked(uint32_t global_interrupt);

/* The whole of a redirection table entry, for the self-test and the report.
 * Returns false where no mapped unit carries the interrupt. */
bool IoApicEntryValue(uint32_t global_interrupt, uint64_t *value);

/* Emits a summary of every mapped unit upon the console and the serial port. */
void IoApicReport(void);

#endif /* OXYS_IOAPIC_H */
