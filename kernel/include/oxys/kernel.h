/*
 * File: kernel/include/oxys/kernel.h
 * Purpose: Declares the kernel entry point and the constants describing the
 *          kernel's position within the virtual address space.
 * Key definitions: KERNEL_VIRTUAL_BASE, PhysicalToVirtual, VirtualToPhysical,
 *          KernelMain, KernelPanic.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5 (Four-Level Paging) and Section 3.3.7.1 (Canonical Addressing).
 *   - Multiboot2 Specification 2.0, Section 3.3 (I386 machine state).
 */

#ifndef OXYS_KERNEL_H
#define OXYS_KERNEL_H

#include <oxys/types.h>

/*
 * The virtual address at which physical address zero is mapped for the kernel.
 * This value must agree with the symbol KernelVirtualBase defined in linker.ld
 * and with KERNEL_VIRTUAL_BASE defined in boot/boot.asm.
 */
#define KERNEL_VIRTUAL_BASE UINT64_C(0xFFFFFFFF80000000)

/*
 * The magic value that a Multiboot2-compliant boot loader places in EAX prior to
 * transferring control to the operating system image. Refer to the Multiboot2
 * Specification, Section 3.3.
 */
#define MULTIBOOT2_BOOTLOADER_MAGIC UINT32_C(0x36D76289)

/*
 * Translates a physical address within the first gibibyte into the corresponding
 * kernel virtual address. The boot-time paging hierarchy maps only the first
 * gibibyte of physical memory into the higher half; addresses beyond that range
 * are not translatable until Phase 2 establishes the direct physical map.
 */
static inline VirtualAddress PhysicalToVirtual(PhysicalAddress physical_address)
{
    return physical_address + KERNEL_VIRTUAL_BASE;
}

/*
 * Translates a kernel virtual address within the higher-half mapping into the
 * corresponding physical address.
 */
static inline PhysicalAddress VirtualToPhysical(VirtualAddress virtual_address)
{
    return virtual_address - KERNEL_VIRTUAL_BASE;
}

/*
 * The C entry point of the kernel, invoked from KernelEntryHigh in
 * boot/boot.asm once long mode is active and the higher-half mapping is in
 * effect. The arguments are those preserved from the machine state described by
 * Multiboot2 Specification, Section 3.3.
 *
 * multiboot_information_address: the physical address of the Multiboot2
 *     information structure, as supplied in EBX.
 * multiboot_magic: the value supplied in EAX, expected to equal
 *     MULTIBOOT2_BOOTLOADER_MAGIC.
 *
 * This function does not return.
 */
void KernelMain(uint32_t multiboot_information_address, uint32_t multiboot_magic);

/*
 * Reports an unrecoverable condition upon both the text console and the serial
 * port, and then halts the processor permanently. This function does not return.
 */
void KernelPanic(const char *message);

/*
 * Writes a null-terminated string to both the text console and the serial port,
 * so that the diagnostic record is complete irrespective of which device the
 * operator is observing.
 */
void KernelWriteString(const char *string);

/*
 * Writes an unsigned value in hexadecimal, prefixed by "0x", to both output
 * devices. Provided because the formatted output facilities of the C library do
 * not exist until Phase 7.
 */
void KernelWriteHexadecimal(uint64_t value);

/*
 * Writes an unsigned value in decimal to both output devices. Provided for the
 * same reason as KernelWriteHexadecimal.
 */
void KernelWriteDecimal(uint64_t value);

#endif /* OXYS_KERNEL_H */
