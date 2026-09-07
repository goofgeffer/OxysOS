/*
 * File: kernel/test/verify_ext2.c
 * Purpose: Runs the EXT2 self-test: it composes the fixture volume, asserts the
 *          superblock and every refusal a malformed one must meet, and then
 *          calls in turn the five chapters of assertions that live beneath
 *          kernel/test/ext2/, restoring the volume between those that alter it.
 * Key functions: KernelVerifyExt2.
 * References:
 *   - docs/storage/EXT2.md, Section 14: every assertion made here and beneath,
 *     paired with the silent failure it catches.
 *   - The Second Extended File System, Dave Poirier: the format itself. The
 *     field offsets are named in <oxys/ext2.h> and are not restated here.
 *
 * Why this file is small and the chapters are elsewhere.
 *
 *   It was 2,618 lines, of which this entry point was the last 291 — the same
 *   shape, and for the same reason, as the implementation it asserts: the file
 *   had become the whole of a subsystem's testing rather than one test. The
 *   chapters now stand in kernel/test/ext2/, one to a concern, and are declared
 *   by the private header there.
 *
 *   What `KernelMain` knows is unchanged: <oxys/verify.h> declares
 *   `KernelVerifyExt2` and `KernelReportVolumes`, and nothing else here is
 *   visible outside this directory.
 *
 *   The order below is not free. The superblock is asserted first because every
 *   other structure is found by arithmetic upon it, and the chapters that alter
 *   a volume run last, each beginning from a volume composed afresh.
 */

#include "ext2/internal.h"

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/ext2.h>
#include <oxys/block.h>
#include <oxys/buffer.h>

void KernelVerifyExt2(void)
{
    BlockDevice *device;
    Ext2Superblock superblock;
    bool succeeded = true;

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Volume self-test FAILED.\n");
        return;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock))
    {
        KernelWriteString("  A well-formed volume was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        KernelWriteString("Volume self-test FAILED.\n");
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
        return;
    }

    /*
     * Every field is compared against the value composed above. A parser reading
     * the right number from the wrong offset is the failure this catches, and
     * only naming the values catches it.
     */
    if ((superblock.magic != EXT2_SUPER_MAGIC) || (superblock.revision != EXT2_DYNAMIC_REV) ||
        (superblock.inode_count != KERNEL_VOLUME_INODES) || (superblock.block_count != 128U) ||
        (superblock.reserved_block_count != 6U) ||
        (superblock.free_block_count != KERNEL_VOLUME_FREE_BLOCKS) ||
        (superblock.free_inode_count != KERNEL_VOLUME_FREE_INODES) ||
        (superblock.first_data_block != 1U) ||
        (superblock.blocks_per_group != 8192U) ||
        (superblock.inodes_per_group != KERNEL_VOLUME_INODES) ||
        (superblock.state != EXT2_VALID_FS))
    {
        KernelWriteString("  A field of the superblock was read from the wrong place.\n");
        succeeded = false;
    }

    /* The block size is derived, not stored, and the geometry follows from it. */
    if ((superblock.block_size != 1024U) || (superblock.sectors_per_block != 2U) ||
        (superblock.group_count != 1U) || (Ext2GroupCount(&superblock) != 1U))
    {
        KernelWriteString("  The geometry derived from the superblock is wrong.\n");
        succeeded = false;
    }

    /* The revision 1 fields, including the label, which is padded and not
     * terminated upon the volume. */
    if ((superblock.first_inode != EXT2_GOOD_OLD_FIRST_INODE) ||
        (superblock.inode_size != EXT2_GOOD_OLD_INODE_SIZE) ||
        (superblock.feature_incompatible != EXT2_FEATURE_INCOMPAT_FILETYPE) ||
        (superblock.feature_read_only != EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ||
        (superblock.volume_name[0] != 'o') || (superblock.volume_name[8] != 't') ||
        (superblock.volume_name[9] != '\0'))
    {
        KernelWriteString("  The revision 1 fields were not read correctly.\n");
        succeeded = false;
    }

    /* A volume declaring only features this kernel implements may be written. */
    if (superblock.read_only)
    {
        KernelWriteString("  A volume this kernel fully implements was marked read-only.\n");
        succeeded = false;
    }

    /* The refusals. Each is a volume this kernel must not address as it stands. */
    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_MAGIC, 0x1234U, true))
    {
        KernelWriteString("  A volume bearing no magic number was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_REVISION, 2U, false))
    {
        KernelWriteString("  A volume of an unknown revision was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_LOG_BLOCK_SIZE, 4U, false))
    {
        KernelWriteString("  A block size beyond this kernel was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_FIRST_DATA_BLOCK, 0U, false))
    {
        KernelWriteString("  A first data block contradicting the block size was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_BLOCKS_PER_GROUP, 0U, false) ||
        !KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_BLOCK_COUNT, 0U, false))
    {
        KernelWriteString("  A degenerate geometry was accepted.\n");
        succeeded = false;
    }

    /*
     * The group count is derivable from the blocks and from the inodes, and the
     * two must agree. Halving the inodes per group leaves a volume every other
     * rule accepts.
     */
    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_INODES_PER_GROUP, 8U, false))
    {
        KernelWriteString("  A volume whose two group counts disagree was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_FREE_BLOCKS, 1000U, false))
    {
        KernelWriteString("  A volume reporting more free blocks than it holds was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_INODE_SIZE, 100U, true) ||
        !KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_INODE_SIZE, 2048U, true))
    {
        KernelWriteString("  An inode size that is not a power of two within a block was "
                          "accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_FIRST_INODE, 2U, false))
    {
        KernelWriteString("  A first usable inode among the reserved ones was accepted.\n");
        succeeded = false;
    }

    /*
     * An incompatible feature this kernel lacks makes the volume unreadable; a
     * read-only compatible one makes it unwritable. The distinction is the whole
     * purpose of the two fields, so both directions are asserted.
     */
    if (!KernelVerifyExt2VolumeRefusedWith(device, EXT2_OFFSET_FEATURE_INCOMPAT,
                                 EXT2_FEATURE_INCOMPAT_RECOVER, false))
    {
        KernelWriteString("  A volume requiring an unimplemented feature was accepted.\n");
        succeeded = false;
    }

    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_RO_COMPAT, EXT2_FEATURE_RO_COMPAT_BTREE_DIR);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !superblock.read_only)
    {
        KernelWriteString("  A volume with an unimplemented read-only feature was not "
                          "made read-only.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !superblock.read_only)
    {
        KernelWriteString("  A volume that was not cleanly unmounted was not made "
                          "read-only.\n");
        succeeded = false;
    }

    /* A volume of revision 0 states neither inode size nor first inode. */
    KernelComposeVolume();
    KernelSetVolumeWord(EXT2_OFFSET_REVISION, EXT2_GOOD_OLD_REV);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_RECOVER);
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        (superblock.inode_size != EXT2_GOOD_OLD_INODE_SIZE) ||
        (superblock.first_inode != EXT2_GOOD_OLD_FIRST_INODE) ||
        (superblock.feature_incompatible != 0U) || (superblock.volume_name[0] != '\0'))
    {
        KernelWriteString("  A volume of revision 0 was not given its fixed values.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /*
     * The block group descriptor table, read from the volume just restored. It
     * is asserted here rather than in a self-test of its own because it can only
     * be read through a superblock, and this is where a valid one exists.
     */
    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyExt2Groups(device, &superblock))
    {
        succeeded = false;
    }

    /* The inodes, read through the descriptor table just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyExt2Inodes(device, &superblock))
    {
        succeeded = false;
    }

    /* The directories, traversed through the inodes just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        !KernelVerifyExt2Directories(device, &superblock))
    {
        succeeded = false;
    }

    /* The contents of the files, reached through the paths just asserted. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyExt2Files(device, &superblock))
    {
        succeeded = false;
    }

    /* The alteration of a volume, upon the device of memory alone. */
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    if (!Ext2ReadSuperblock(device, &superblock) || !KernelVerifyExt2Writes(device, &superblock))
    {
        succeeded = false;
    }

    /* The alteration of a directory, which is what turns an inode into a file
     * somebody can name. */
    KernelRestoreVolume(device);

    if (!Ext2ReadSuperblock(device, &superblock) ||
        !KernelVerifyExt2DirectoryWrites(device, &superblock))
    {
        succeeded = false;
    }

    KernelRestoreVolume(device);

    /* A device with nowhere to put a superblock, and requests without one. */
    if (Ext2ReadSuperblock(NULL, &superblock) || Ext2ReadSuperblock(device, NULL))
    {
        KernelWriteString("  A degenerate request was accepted.\n");
        succeeded = false;
    }

    (void)BufferInvalidateDevice(device);
    (void)BlockUnregister(device);

    device = BlockRegister("mem1", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 2U,
                           false);

    if ((device == NULL) || Ext2ReadSuperblock(device, &superblock))
    {
        KernelWriteString("  A device too short to hold a superblock was accepted.\n");
        succeeded = false;
    }

    if (device != NULL)
    {
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
    }

    KernelWriteString(succeeded ? "Volume self-test passed.\n" : "Volume self-test FAILED.\n");
}
