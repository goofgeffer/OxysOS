/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: drivers/ramdisk/ramdisk.c
 * Purpose: Implements the ramdisk driver, which presents the extent of physical
 *          memory a boot module occupies to the block layer as a device of
 *          fixed-size blocks, so that the initial ramdisk of sub-task 7.7 is
 *          read by exactly the code that reads a disk.
 * Key functions: RamdiskInitialise, RamdiskRead, RamdiskWrite, RamdiskDevice,
 *          RamdiskReport.
 * References:
 *   - docs/storage/INITRD.md: the initial ramdisk in full — why it is an EXT2
 *     image rather than an archive of its own, why it is not copied, and what
 *     this driver is therefore permitted to assume.
 *   - docs/storage/BLOCK.md: the four refusals the layer makes before
 *     a driver is reached, which is why nothing below re-tests them.
 *   - Multiboot2 Specification 2.0, Section 3.6.6: the module tag that states
 *     where the boot loader put the image.
 *   - docs/design/MEMORY-LAYOUT.md: the direct map, through which
 *     every physical address below is addressed.
 *
 * What this driver is, and the one thing that makes it different from the other
 * three.
 *
 *   Every other storage driver in this project converses with hardware. This one
 *   converses with nothing: a transfer is a copy, it cannot fail for any reason
 *   outside this file, and there is no state to reset, no command to time out,
 *   and no status register to read. Almost the whole of what the ATA and AHCI
 *   drivers are is absent, and what remains is the arithmetic that turns a block
 *   number into an address.
 *
 *   That is precisely why it belongs behind the block layer rather than beside
 *   it. The value of the arrangement is not that a ramdisk needs a driver; it is
 *   that the EXT2 implementation of Phase 5, the buffer cache of sub-task 4.6
 *   and the filesystem layer of 5.8 read the initial ramdisk through the same
 *   path they read a disk through, and are therefore exercised by every boot
 *   rather than only upon a machine that happens to carry a volume.
 *
 * Why the module is not copied.
 *
 *   The obvious implementation copies the module into memory the kernel owns and
 *   frees the module's frames. It is not done, for two reasons. The copy would
 *   have to come from the kernel heap, which would make the greatest ramdisk
 *   this kernel can mount a function of how much heap is left at the moment the
 *   device is registered — a limit nobody can predict and nothing states. And
 *   the copy buys nothing: the module's frames are reserved by
 *   PhysicalMemoryInitialise whether or not this driver uses them, so freeing
 *   them would mean releasing them deliberately, which is a thing to get wrong
 *   rather than a saving.
 *
 * Concurrency. This driver holds one device description and no lock. Two
 *   processors transferring simultaneously would each read or write correctly —
 *   a transfer is a copy between disjoint ranges — but the block layer's
 *   accounting above it is not safe, and neither is the buffer cache. See
 *   docs/design/CONCURRENCY.md.
 */

#include <oxys/dev/storage/ramdisk.h>

#include <oxys/kernel.h>
#include <oxys/block/block.h>
#include <oxys/boot/bootinfo.h>

/*
 * The one device this driver presents, and the extent it stands upon.
 *
 * `base` is a direct-map address and not a physical one. The conversion is done
 * once, at registration, rather than upon every transfer: a transfer that
 * converted would be doing the same arithmetic some thousands of times to reach
 * the same answer, and the conversion is the one step in this file that could be
 * got wrong in a way a transfer could not report.
 */
static uint8_t *RamdiskBase;
static uint64_t RamdiskLength;
static BlockDevice *RamdiskRegistered;

/* What prevented a registration, for RamdiskReport; null where none did. */
static const char *RamdiskRefusal = "the boot loader supplied no module named "
                                    RAMDISK_MODULE_NAME;

/* Copies bytes. The kernel is not compiled against the C library, so the copy a
 * transfer performs is written here. */
static void RamdiskCopy(uint8_t *destination, const uint8_t *source, uint64_t count)
{
    for (uint64_t index = 0U; index < count; ++index)
    {
        destination[index] = source[index];
    }
}

/*
 * The two operations the block layer calls.
 *
 * Neither re-tests the range. The layer has already refused a null buffer, a
 * count of zero and a range outside the device, and repeating those tests here
 * would be the second of two places a bound is written — which is the
 * arrangement in which the two disagree and the weaker one wins.
 *
 * The context is unused: this driver presents one device, and a second would
 * need a description to point at rather than a file-scope extent. It is named
 * and voided rather than omitted, because the layer's operation type fixes the
 * signature.
 */
static bool RamdiskRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    (void)context;

    RamdiskCopy((uint8_t *)buffer, RamdiskBase + (block * BLOCK_SIZE_DEFAULT),
                (uint64_t)count * BLOCK_SIZE_DEFAULT);

    return true;
}

static bool RamdiskWrite(void *context, uint64_t block, uint32_t count,
                         const void *buffer)
{
    (void)context;

    RamdiskCopy(RamdiskBase + (block * BLOCK_SIZE_DEFAULT), (const uint8_t *)buffer,
                (uint64_t)count * BLOCK_SIZE_DEFAULT);

    return true;
}

static const BlockOperations RamdiskOperations = {
    .read = RamdiskRead,
    .write = RamdiskWrite,
};

bool RamdiskInitialise(const BootInformation *information)
{
    const BootModule *module;
    uint64_t length;

    RamdiskRegistered = NULL;
    RamdiskBase = NULL;
    RamdiskLength = 0U;

    module = BootInformationFindModule(information, RAMDISK_MODULE_NAME);

    if (module == NULL)
    {
        RamdiskRefusal = "the boot loader supplied no module named "
                         RAMDISK_MODULE_NAME;
        return false;
    }

    length = module->end - module->start;

    /*
     * A module that is not a whole number of blocks is refused rather than
     * rounded down.
     *
     * Rounding would produce a device whose last block is the image's last block
     * with something else after it, and the something else is whatever the boot
     * loader happened to place next. The filesystem above would read it as data
     * and would be right to: a block device that is a truncation of an image is
     * indistinguishable from an image that was truncated. An image this project
     * builds is always a whole number of blocks, so this refusal fires only when
     * something else went wrong, and it is worth saying so plainly.
     */
    if ((length % BLOCK_SIZE_DEFAULT) != 0U)
    {
        RamdiskRefusal = "the module is not a whole number of blocks";
        return false;
    }

    RamdiskBase = (uint8_t *)(uintptr_t)PhysicalToDirect(module->start);
    RamdiskLength = length;

    /*
     * The device is registered writable, and this is the one decision here that
     * differs from how a disk is treated.
     *
     * A volume upon a disk is mounted read-only unless the operator asks
     * otherwise, because the disk belongs to somebody and a kernel that mounted
     * it for writing would mark it unclean merely by having been booted. None of
     * that reasoning reaches a ramdisk: it belongs to this kernel, it was made
     * by this build, and it ceases to exist when the machine is switched off.
     * There is nothing to protect and nobody to inconvenience, so the device
     * says what is true of it — that it can be written.
     */
    RamdiskRegistered = BlockRegister(RAMDISK_DEVICE_NAME, &RamdiskOperations, NULL,
                                      BLOCK_SIZE_DEFAULT,
                                      length / BLOCK_SIZE_DEFAULT, false);

    if (RamdiskRegistered == NULL)
    {
        RamdiskRefusal = "the block layer refused the registration";
        RamdiskBase = NULL;
        RamdiskLength = 0U;
        return false;
    }

    RamdiskRefusal = NULL;

    return true;
}

BlockDevice *RamdiskDevice(void)
{
    return RamdiskRegistered;
}

void RamdiskReport(void)
{
    if (RamdiskRegistered == NULL)
    {
        KernelWriteString("Ramdisk: none was registered: ");
        KernelWriteString(RamdiskRefusal != NULL ? RamdiskRefusal
                                                 : "no reason was recorded");
        KernelWriteString(".\n");
        return;
    }

    KernelWriteString("Ramdisk: ");
    KernelWriteString(RAMDISK_DEVICE_NAME);
    KernelWriteString(" is the module ");
    KernelWriteString(RAMDISK_MODULE_NAME);
    KernelWriteString(" at ");
    KernelWriteHexadecimal(DirectToPhysical((VirtualAddress)(uintptr_t)RamdiskBase));
    KernelWriteString(", ");
    KernelWriteDecimal(RamdiskLength / 1024U);
    KernelWriteString(" KiB in ");
    KernelWriteDecimal(RamdiskRegistered->block_count);
    KernelWriteString(" blocks of ");
    KernelWriteDecimal((uint64_t)RamdiskRegistered->block_size);
    KernelWriteString(" bytes, writable.\n");
}
