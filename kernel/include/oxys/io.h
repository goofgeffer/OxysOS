/*
 * File: kernel/include/oxys/io.h
 * Purpose: Provides inline accessors for the x86 programmed input/output address
 *          space, which is separate from the memory address space.
 * Key definitions: PortReadByte, PortWriteByte, PortReadWord, PortWriteWord,
 *          PortReadDoubleWord, PortWriteDoubleWord, PortReadWordString, IoWait.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1,
 *     Section 18.3 (Input/Output): the I/O address space comprises 65536
 *     individually addressable 8-bit ports.
 *   - Intel SDM, Volume 2A, "IN" and "OUT" instruction descriptions: a port
 *     number greater than 255 must be supplied in the DX register.
 *   - Intel SDM, Volume 2A, "INS/INSB/INSW/INSD": the string form transfers from
 *     a port to the memory operand addressed by RDI, which the REP prefix
 *     repeats RCX times, the index register advancing by the operand size upon
 *     each iteration while the direction flag is clear. The System V AMD64 ABI
 *     requires that flag to be clear at every function boundary.
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
 * Reads a 16-bit word from the specified I/O port. It is the width in which an
 * ATA device transfers data, its interface being sixteen bits wide.
 */
static inline uint16_t PortReadWord(uint16_t port)
{
    uint16_t value;

    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");

    return value;
}

/*
 * Writes a 16-bit word to the specified I/O port.
 */
static inline void PortWriteWord(uint16_t port, uint16_t value)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/*
 * Reads a number of 16-bit words from a port into memory.
 *
 * A sector transferred by programmed input/output is 256 words, and the string
 * form moves them without the loop overhead of 256 separate transfers upon a
 * path that is already the slowest way to reach a disk. There is no
 * corresponding writer: a device is entitled to a short recovery between the
 * words it is given, and the transmitting side is therefore written as a loop.
 */
static inline void PortReadWordString(uint16_t port, void *buffer, size_t count)
{
    __asm__ __volatile__("rep insw" : "+D"(buffer), "+c"(count) : "d"(port) : "memory");
}

/*
 * Reads a 32-bit double word from the specified I/O port. The PCI configuration
 * data register is defined only for accesses of this width.
 */
static inline uint32_t PortReadDoubleWord(uint16_t port)
{
    uint32_t value;

    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");

    return value;
}

/*
 * Writes a 32-bit double word to the specified I/O port.
 */
static inline void PortWriteDoubleWord(uint16_t port, uint32_t value)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
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
