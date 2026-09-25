/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/storage.c
 * Purpose: Phase 5: the bus, the three disk controllers, the block layer and
 *          the ramdisk, the buffer cache, EXT2 and the filesystem layer; and
 *          the mounting of the root and of the machine's own volume, which the
 *          next phase calls.
 * Key functions: KernelInitialiseStorage, KernelMountRootVolume.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order that fixes
 *     where this phase stands in KernelMain, and the order within it.
 *
 * Moved out of kernel/kernel.c on 2026-09-25, unchanged in order, when
 * KernelMain was reduced to the driver that calls one function per phase;
 * kernel/init/internal.h says why.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/fs/persist.h>
#include <oxys/dev/pci.h>
#include <oxys/dev/storage/ata.h>
#include <oxys/dev/storage/ahci.h>
#include <oxys/dev/storage/sdhci.h>
#include <oxys/dev/storage/ramdisk.h>
#include <oxys/block/block.h>
#include <oxys/block/buffer.h>
#include <oxys/fs/vfs.h>
#include <oxys/fs/ext2_vfs.h>

/*
 * Where a volume the machine actually carries is mounted once the initial
 * ramdisk has taken the root, and the file the write probe acts upon within it.
 *
 * The directory exists upon the ramdisk because the Makefile puts it there; a
 * mount point that does not exist is refused, and inventing one at boot would
 * mean writing to a filesystem before anything had established that it works.
 */
#define KERNEL_MACHINE_MOUNT_POINT "/mnt"
#define KERNEL_WRITE_PROBE_FILE    "/oxys-write-test"

/*
 * Mounts the first volume the machine carries at KERNEL_MACHINE_MOUNT_POINT, so
 * that a disk is still reachable once the ramdisk holds the root.
 *
 * The ramdisk is skipped by name. It is a registered device like any other and
 * is already mounted at the root; mounting it a second time at `/mnt` would
 * succeed, would present the same volume twice, and would be the sort of thing
 * that is noticed only when something writes through one view and reads through
 * the other.
 *
 * A machine carrying no volume is not in error and nothing is said about it: the
 * report that follows lists what is mounted, and an absent line is the statement.
 * `make verify` runs upon such a machine.
 */
static void KernelMountMachineVolume(void)
{
    const size_t count = BlockDeviceCount();
    const BlockDevice *const ramdisk = RamdiskDevice();
    const bool writable = KernelCommandLineHasOption("ext2-write-test");

    for (size_t index = 0U; index < count; ++index)
    {
        const BlockDevice *const device = BlockDeviceAt(index);

        if ((device == NULL) || (device == ramdisk))
        {
            continue;
        }

        if (VfsMountVolume(device->name, KERNEL_MACHINE_MOUNT_POINT, "ext2", !writable))
        {
            KernelWriteString("VFS: a volume the machine carries is mounted at "
                              KERNEL_MACHINE_MOUNT_POINT ".\n");

            return;
        }
    }
}

/*
 * Mounts the root filesystem and reports what it holds.
 *
 * The initial ramdisk of sub-task 7.7 is preferred, and everything about how
 * that preference is expressed is deliberate.
 *
 * **It is chosen by name and not by being found first.** `VfsMountRoot` walks
 * the registered devices and mounts the first volume it can, which is exactly
 * right when the question is "is there anything to mount" and exactly wrong
 * when the answer must be a particular thing. A machine carrying a disk with an
 * EXT2 volume upon it would otherwise boot with a stranger's filesystem at the
 * root or with the ramdisk there, depending upon which driver had registered
 * first — a difference nobody chose, that changes every path in the system, and
 * that a boot log would not obviously show.
 *
 * **The ramdisk is mounted for writing and a disk is not.** The rule about
 * read-only mounts exists because a disk belongs to whoever owns the machine,
 * and a kernel that mounted theirs for writing would mark it as not cleanly
 * unmounted merely by having been booted — so their disk would demand a check
 * before they could mount it again, which is a real cost imposed for nothing.
 * None of that reasoning reaches a ramdisk. It was made by this build, it is
 * read by nothing else, and it ceases to exist when the machine is switched
 * off. Writing to it costs nobody anything, and a root nothing may write to is
 * a root the shell of Phase 8 cannot redirect into.
 *
 * A machine that was booted without a ramdisk falls back to the disks, under
 * the rule that governed this function before 7.7. Every ISO this project
 * builds carries one, so the fall-back is for a kernel loaded by some other
 * means; `make verify` exercises the ramdisk path and nothing presently
 * exercises this one.
 *
 * A machine carrying neither is not in error.
 */
void KernelMountRootVolume(void)
{
    const bool writable = KernelCommandLineHasOption("ext2-write-test");

    VfsInitialise();

    if (!Ext2VfsInitialise())
    {
        KernelWriteString("VFS: the EXT2 filesystem could not be registered.\n");
        return;
    }

    if (RamdiskDevice() != NULL)
    {
        if (VfsMountVolume(RAMDISK_DEVICE_NAME, "/", "ext2", false))
        {
            KernelWriteString("VFS: the initial ramdisk is mounted at the root.\n");

            /* The persistent `/etc` before the machine's volume, so that the
             * disk labelled for it is taken for `/etc` and not mounted at `/mnt`
             * as a stranger's: docs/storage/PERSIST.md. */
            (void)PersistEtc();
            KernelMountMachineVolume();
            VfsReport();
            VfsReportDirectory("/");
            KernelVfsProbeVolume(KERNEL_MACHINE_MOUNT_POINT,
                                 KERNEL_MACHINE_MOUNT_POINT KERNEL_WRITE_PROBE_FILE);

            return;
        }

        /*
         * A ramdisk that is present and will not mount is reported and then
         * fallen through from, rather than being treated as fatal. The
         * self-test of sub-task 7.7 is what turns this into a failure; saying
         * it here as well and stopping would deny a machine with a real volume
         * the root it could still have had.
         */
        KernelWriteString("VFS: the initial ramdisk would not mount: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
    }

    if (!VfsMountRoot("ext2", !writable))
    {
        KernelWriteString("VFS: no volume was mounted at the root: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    VfsReport();
    VfsReportDirectory("/");
    KernelVfsProbeVolume("/", KERNEL_WRITE_PROBE_FILE);
}

void KernelInitialiseStorage(void)
{
    /*
     * The bus is enumerated once every device driven so far is working, so that
     * a failure in the enumeration is reported through channels already proved.
     * Nothing is claimed or configured here; the enumeration only establishes
     * what the machine contains, which the disk driver then searches.
     */
    (void)PciInitialise();
    KernelVerifyPci();
    PciReport();

    /*
     * The disk is the last device of this phase and the first whose failure is
     * silent in the ordinary case: a driver that reads the wrong sector returns
     * data, and data that arrived is indistinguishable from data that is right
     * until something tries to interpret it.
     */
    (void)AtaInitialise();
    KernelVerifyAta();
    AtaReport();

    /*
     * The same command set, reached the other way. Sub-task 4.4 drives an IDE
     * controller through I/O ports; a firmware that presents its serial ATA
     * controller in AHCI mode puts the disks behind memory-mapped registers that
     * driver cannot reach, and upon most machines made in the last fifteen years
     * that is where the disks are. The two run one after the other because a
     * machine may carry both, and each finds only what belongs to it.
     */
    (void)AhciInitialise();
    KernelVerifyAhci();
    AhciReport();

    /*
     * And the storage that is of neither class. An inexpensive laptop keeps its
     * system upon an embedded MultiMediaCard behind a host controller the
     * assignment specification classes as a system peripheral, and carries no
     * mass-storage controller whatever: neither driver above will ever find
     * anything upon such a machine, and no firmware setting would give them one.
     */
    (void)SdhciInitialise();
    KernelVerifySdhci();
    SdhciReport();

    /*
     * Every disk found presents itself through the generic layer, which is what
     * everything above will address it by. The layer is asserted against a
     * device of memory rather than against a disk: the machine this is verified
     * upon has no disk, and one that has holds data a self-test must not write.
     */
    (void)AtaRegisterBlockDevices();
    (void)AhciRegisterBlockDevices();
    (void)SdhciRegisterBlockDevices();

    /*
     * Sub-task 7.7. The initial ramdisk is a device like any other, and is
     * registered here beside the three that are disks rather than anywhere
     * nearer the filesystem it carries.
     *
     * It needs nothing of the drivers above it: the boot loader has already put
     * the image in memory, `PhysicalMemoryInitialise` has already reserved the
     * frames it stands upon, and this call is the arithmetic that turns an
     * extent into a geometry. It is placed after them because the order devices
     * appear in a report is the order they are registered in, and a reader
     * should meet the machine's own storage before the one this build supplied.
     */
    (void)RamdiskInitialise(&KernelBootInformation);
    RamdiskReport();

    KernelVerifyBlock();
    BlockReport();

    /*
     * The cache stands between the block layer and everything that will read a
     * medium. Its storage comes from the kernel heap, so it cannot be prepared
     * until that exists, which it has since Phase 2.
     */
    (void)BufferInitialise();
    KernelVerifyBuffer();
    BufferReport();

    /*
     * Phase 5 begins here. Nothing is mounted: the superblock of any volume the
     * machine actually carries is read and reported, and the parser itself is
     * asserted against a volume composed in memory.
     */
    KernelVerifyExt2();
    KernelReportVolumes();

    /*
     * Sub-task 5.8. The layer is asserted against the two volumes of memory it
     * composes, which is where a mount, a descriptor and a mount point crossing
     * can be stated exactly; only then is a volume the machine actually carries
     * mounted at the root, read-only unless the operator permitted otherwise.
     */
    KernelVerifyVfs();
}
