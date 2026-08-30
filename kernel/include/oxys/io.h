/*
 * File: kernel/include/oxys/io.h
 * Purpose: Provides inline accessors for the x86 programmed input/output address
 *          space, which is separate from the memory address space.
 * Key definitions: PortReadByte, PortWriteByte, IoWait.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1,
 *     Section 18.3 (Input/Output): the I/O address space comprises 65536
 *     individually addressable 8-bit ports.
 *   - Intel SDM, Volume 2A, "IN" and "OUT" instruction descriptions: a port
 *     number greater than 255 must be supplied in the DX register.
 */

#ifndef OXYS_IO_H
#define OXYS_IO_H

#include <oxys/types.h>

/*
 * Reads a single byte from the specified I/O port.
 *
 * The GNU C extended assembly syntax is employed because ISO C provides no means
 * of expressing the IN instruction. The rationale for this deviation from the
 * prohibition upon compiler extensions, recorded in PROJECT_GUIDELINES.md Section 8, is that
 * no conforming alternative exists; the same rationale applies to every other
 * inline assembly construct within the kernel.
 */
static inline uint8_t PortReadByte(uint16_t port)
{
    uint8_t value;

    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");

    return value;
}

/*
 * Writes a single byte to the specified I/O port.
 */
static inline void PortWriteByte(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/*
 * Introduces a short delay sufficient for slow legacy peripherals to settle, by
 * writing to port 0x80. That port is used by the firmware for power-on self-test
 * codes and is guaranteed not to be decoded by any device that the kernel drives.
 */
static inline void IoWait(void)
{
    PortWriteByte(0x80, 0x00);
}

#endif /* OXYS_IO_H */
