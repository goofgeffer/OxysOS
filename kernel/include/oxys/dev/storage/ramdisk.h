/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/dev/storage/ramdisk.h
 * Purpose: Declares the ramdisk driver, which presents an extent of physical
 *          memory to the block layer as a device of fixed-size blocks, and the
 *          initialisation that registers the initial ramdisk the boot loader
 *          supplied as a module.
 * Key definitions: RAMDISK_DEVICE_NAME, RAMDISK_MODULE_NAME, RamdiskInitialise,
 *          RamdiskDevice, RamdiskReport.
 * References:
 *   - docs/storage/INITRD.md: the initial ramdisk, the module it arrives as,
 *     this device beneath it, and the root it is mounted as.
 *   - docs/storage/BLOCK.md: the layer this driver registers with, and the four
 *     refusals it makes before any driver is reached.
 *   - Multiboot2 Specification 2.0, Section 3.6.6: the module tag by which the
 *     boot loader states where it put the image.
 */

#ifndef OXYS_DEV_STORAGE_RAMDISK_H
#define OXYS_DEV_STORAGE_RAMDISK_H

#include <oxys/types.h>
#include <oxys/block/block.h>
#include <oxys/boot/bootinfo.h>

/*
 * The name the initial ramdisk is registered under, and the name of the module
 * it is carried in.
 *
 * The two are deliberately different words. `ram0` names a *device*, in the
 * series `ata0`, `sd0` and the rest, and says nothing about what is upon it;
 * `initrd` names the *module*, and is the string boot/grub/grub.cfg gives upon
 * the `module2` line. A kernel that used one word for both would be unable to
 * express a second ramdisk that is not a root filesystem.
 */
#define RAMDISK_DEVICE_NAME "ram0"
#define RAMDISK_MODULE_NAME "initrd"

/*
 * Registers the module named RAMDISK_MODULE_NAME, if the boot loader supplied
 * one, as the block device named RAMDISK_DEVICE_NAME.
 *
 * Returns false, having said why, where no such module was supplied, where its
 * extent is not a whole number of blocks, or where the block layer refused the
 * registration. A machine booted without a ramdisk is not in error: every ISO
 * this project builds carries one, and a kernel loaded by some other means must
 * still boot.
 */
bool RamdiskInitialise(const BootInformation *information);

/* The registered device, or null where none was registered. */
BlockDevice *RamdiskDevice(void);

/* Emits what was registered, or what prevented it, upon the console and the
 * serial port. */
void RamdiskReport(void);

#endif /* OXYS_DEV_STORAGE_RAMDISK_H */
