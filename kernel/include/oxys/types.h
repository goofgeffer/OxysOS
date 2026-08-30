/*
 * File: kernel/include/oxys/types.h
 * Purpose: Defines the fixed-width integer types, the boolean type and the
 *          fundamental address types employed throughout the Oxys-OS kernel,
 *          without recourse to a hosted C library.
 * Key definitions: uint8_t, uint16_t, uint32_t, uint64_t and their signed
 *          counterparts; size_t; bool; PhysicalAddress; VirtualAddress.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.20 (<stdint.h>) and Section 7.18 (<stdbool.h>).
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement, Section 3.1.2 (Data Representation): the LP64 model, in which
 *     int is 32 bits, and long and pointers are 64 bits.
 */

#ifndef OXYS_TYPES_H
#define OXYS_TYPES_H

/*
 * The cross-compiler supplies freestanding implementations of <stdint.h>,
 * <stddef.h> and <stdbool.h>. ISO/IEC 9899:2011, Section 4, paragraph 6,
 * requires a freestanding implementation to provide these headers, so their use
 * introduces no dependency upon a hosted C library.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * A physical address, as understood by the memory management unit and by device
 * bus masters. It is deliberately distinct from VirtualAddress so that the
 * confusion of the two is visible upon inspection.
 */
typedef uint64_t PhysicalAddress;

/*
 * A linear (virtual) address, as understood by the executing processor after
 * paging has been enabled.
 */
typedef uint64_t VirtualAddress;

#endif /* OXYS_TYPES_H */
