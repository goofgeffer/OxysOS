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
 * The base of the direct physical map, at which the whole of physical memory is
 * mapped once Phase 2, sub-task 2.4, has run. The region occupies the 64 TiB
 * beginning at this address; refer to docs/design/MEMORY-LAYOUT.md, Section 2.
 */
#define DIRECT_MAP_BASE UINT64_C(0xFFFF800000000000)

/*
 * Translates a physical address within the first gibibyte into the corresponding
 * address in the kernel image window.
 *
 * This translation is valid only below one gibibyte, that being the extent of
 * the window, and is used during the construction of the paging hierarchy, at
 * which point the direct map does not yet exist. Code running after
 * PagingInitialise should prefer PhysicalToDirect, which is valid for the whole
 * of physical memory.
 */
static inline VirtualAddress PhysicalToVirtual(PhysicalAddress physical_address)
{
    return physical_address + KERNEL_VIRTUAL_BASE;
}

/*
 * Translates a kernel virtual address within the kernel image window into the
 * corresponding physical address.
 */
static inline PhysicalAddress VirtualToPhysical(VirtualAddress virtual_address)
{
    return virtual_address - KERNEL_VIRTUAL_BASE;
}

/*
 * Translates any physical address into its address within the direct physical
 * map. Valid for the whole of physical memory, but only after PagingInitialise
 * has established and activated the map.
 */
static inline VirtualAddress PhysicalToDirect(PhysicalAddress physical_address)
{
    return physical_address + DIRECT_MAP_BASE;
}

/* Translates an address within the direct physical map back to its physical
 * address. */
static inline PhysicalAddress DirectToPhysical(VirtualAddress virtual_address)
{
    return virtual_address - DIRECT_MAP_BASE;
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
