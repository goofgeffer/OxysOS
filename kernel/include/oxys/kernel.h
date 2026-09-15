/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/kernel.h
 * Purpose: Declares the kernel entry point and the constants describing the
 *          kernel's position within the virtual address space.
 * Key definitions: KERNEL_VIRTUAL_BASE, PhysicalToVirtual, VirtualToPhysical,
 *          KernelTextStart, KernelTextEnd, KernelMain, KernelPanic,
 *          KernelWriteString, KernelDiagnosticChannelReset,
 *          KernelDisplaySetQuiet, KernelDisplayIsQuiet, OXYS_VERSION_BANNER.
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
 * The extents of the kernel text section, established by the link script.
 *
 * They are arrays of char because a linker symbol has an address and no value;
 * taking the address of the array yields the address the linker assigned, and
 * reading it would read whatever happens to be the first byte of the section.
 *
 * They are declared here rather than in each file that needs them because three
 * now do: the paging hierarchy, which maps the section without write permission,
 * and the interrupt self-test, which asserts that a trap frame's return address
 * lies within it. A declaration repeated in three files is three places for the
 * link script to be contradicted.
 */
extern char KernelTextStart[];
extern char KernelTextEnd[];

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
/*
 * The name and version of the system.
 *
 * They live here rather than in kernel.c because two things now present them:
 * the banner at every start, and the system call by which a program asks what it
 * is running upon. A second copy would be a second thing to forget to change.
 *
 * OXYS_VERSION_STRING holds the *ordinal form* of the release this image belongs
 * to, as docs/project/VERSIONING.md, Section 3, defines it — "1", "1.1",
 * "4-workspace" — and "unreleased" where it belongs to none, which is the case
 * today. It held "0.1.0" until the versioning scheme was written, which was a
 * release that had been published and then withdrawn; see
 * docs/project/HISTORY.md, 2026-09-09.
 *
 * The release's *name* is deliberately absent. Nothing inside the kernel has any
 * use for one: the banner is read by whoever is already looking at the machine,
 * and a program asking what it runs upon wants something it can compare. The
 * name lives in the release notes and the tag annotation.
 */
#define OXYS_SYSTEM_NAME    "Oxys-OS"
#define OXYS_VERSION_STRING "unreleased"

/* The same, as the boot banner shows it. The two change together; a release
 * is named in one place and shown in two. */
#define OXYS_VERSION_BANNER "UNRELEASED"

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
 * Returns the diagnostic channel's lock to its unheld state, whoever was holding
 * it.
 *
 * It is for the panic path and for nothing else. A panic may be raised from
 * inside the channel's own critical section, or by a processor that has just
 * halted every other processor — one of which may have been holding the lock and
 * will now never release it. In both cases there is nothing left for the lock to
 * protect, every other writer having been stopped, and a report that waited for
 * it would be a machine that failed silently instead of one that said why.
 */
void KernelDiagnosticChannelReset(void);

/*
 * Silences, or restores, the two paths a person at the machine sees — the
 * text-mode display and the framebuffer console. The serial line is never
 * silenced: it is the record. The default boot is quiet from the parse of the
 * command line to the start of the shell, and the `diagnostics` menu entry is
 * what asks for the boot log upon the screen. KernelPanic restores the display
 * unconditionally.
 */
void KernelDisplaySetQuiet(bool quiet);
bool KernelDisplayIsQuiet(void);

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
