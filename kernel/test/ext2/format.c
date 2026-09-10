/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/ext2/format.c
 * Purpose: Asserts the superblock, the block group descriptor table and the
 *          inode against the volume composed by kernel/test/volume.c: that each
 *          field is read from the place the format puts it, that the geometry
 *          derived from them is right, and that a volume contradicting itself in
 *          any one of a dozen ways is refused rather than addressed.
 * Key functions: KernelVerifyExt2VolumeRefusedWith, KernelVerifyExt2Groups,
 *          KernelVerifyExt2Inodes.
 * References:
 *   - docs/storage/EXT2-VERIFICATION.md, which pairs every assertion here with
 *     the silent failure it catches.
 *   - The Second Extended File System, Dave Poirier: the format itself. The
 *     field offsets are named in <oxys/ext2.h> and are not restated here.
 *
 * The refusals are the substance of this file. A parser reading the right number
 * from the wrong offset produces a plausible volume, and a kernel that addressed
 * it would do so for the rest of the machine's life; so each field that must be
 * refused is made wrong on purpose, the volume re-read, and the field restored.
 */

#include "internal.h"

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/ext2.h>
#include <oxys/block.h>
#include <oxys/buffer.h>

/*
 * Alters one field of the composed volume and reports whether the parser refused
 * the result, restoring the volume afterwards.
 *
 * The cache is invalidated around the alteration. The superblock is written into
 * the device's storage directly, beneath both the block layer and the cache, so
 * a cache holding the previous contents would answer the next read with them and
 * the assertion would be made against the volume that no longer exists.
 */
bool KernelVerifyExt2VolumeRefusedWith(BlockDevice *device, size_t offset, uint32_t value,
                                    bool half)
{
    Ext2Superblock superblock;
    bool refused;

    if (half)
    {
        KernelSetVolumeHalf(offset, (uint16_t)value);
    }
    else
    {
        KernelSetVolumeWord(offset, value);
    }

    (void)BufferInvalidateDevice(device);
    refused = !Ext2ReadSuperblock(device, &superblock);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Alters one field of the composed group descriptor and reports whether the
 * given judgement refused the result, restoring the descriptor afterwards.
 *
 * The cache is invalidated on both sides of the alteration for the reason
 * KernelVerifyExt2VolumeRefusedWith gives: the descriptor is written beneath the cache,
 * and a cache still holding the previous descriptor would answer with it.
 */
static bool KernelDescriptorRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                        size_t offset, uint32_t value, bool half,
                                        bool whole_table)
{
    Ext2GroupDescriptor descriptor;
    bool refused;

    if (half)
    {
        KernelStoreHalf(KernelDescriptorField(offset), (uint16_t)value);
    }
    else
    {
        KernelStoreWord(KernelDescriptorField(offset), value);
    }

    (void)BufferInvalidateDevice(device);

    refused = whole_table ? !Ext2VerifyGroupDescriptors(device, superblock)
                          : !Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor);

    KernelComposeGroupDescriptor();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Asserts that the block group descriptor table is read as it stands, and that a
 * table this kernel must not trust is refused.
 *
 * A descriptor is three block numbers and three counts, and every one of them is
 * a plausible number wherever it is read from. A table read one block early, or
 * a descriptor taken to be 24 or 40 bytes rather than 32, yields block numbers
 * that address real blocks of the volume — the wrong ones — and a kernel that
 * then wrote an inode would write it over a file. Naming the values, and
 * asserting the one statement the table makes as a whole, is what catches that.
 */
bool KernelVerifyExt2Groups(BlockDevice *device, const Ext2Superblock *superblock)
{
    Ext2GroupDescriptor descriptor;
    bool succeeded = true;

    /* The derived geometry of the table itself, before any of it is read. */
    if ((Ext2GroupDescriptorBlock(superblock) != KERNEL_VOLUME_DESCRIPTOR_BLOCK) ||
        (Ext2GroupDescriptorBlocks(superblock) != 1U) ||
        (Ext2InodeTableBlocks(superblock) != 4U) ||
        (Ext2GroupFirstBlock(superblock, 0U) != 1U) ||
        (Ext2GroupBlockCount(superblock, 0U) != 127U))
    {
        KernelWriteString("  The geometry of the descriptor table is wrong.\n");
        succeeded = false;
    }

    if (!Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor))
    {
        KernelWriteString("  A well-formed group descriptor was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((descriptor.group != 0U) || (descriptor.block_bitmap != KERNEL_VOLUME_BLOCK_BITMAP) ||
        (descriptor.inode_bitmap != KERNEL_VOLUME_INODE_BITMAP) ||
        (descriptor.inode_table != KERNEL_VOLUME_INODE_TABLE) ||
        (descriptor.free_block_count != KERNEL_VOLUME_FREE_BLOCKS) ||
        (descriptor.free_inode_count != KERNEL_VOLUME_FREE_INODES) ||
        (descriptor.used_directory_count != KERNEL_VOLUME_DIRECTORIES))
    {
        KernelWriteString("  A field of the group descriptor was read from the wrong "
                          "place.\n");
        succeeded = false;
    }

    /* The whole table, and the one statement it makes about the volume. */
    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  A well-formed descriptor table was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* A group the volume does not hold. */
    if (Ext2ReadGroupDescriptor(device, superblock, superblock->group_count, &descriptor))
    {
        KernelWriteString("  A descriptor beyond the end of the table was read.\n");
        succeeded = false;
    }

    /* A structure of the group outside the volume, in both directions. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_TABLE, 200U,
                                     false, false) ||
        !KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_BLOCK_BITMAP, 0U,
                                     false, false))
    {
        KernelWriteString("  A group whose structures lie outside the volume was "
                          "accepted.\n");
        succeeded = false;
    }

    /*
     * An inode table that begins within the volume and ends beyond it. The
     * length is not stored anywhere and follows from the inode size, so a kernel
     * that checked only the first block would read the last inodes of the group
     * from nowhere.
     */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_TABLE, 127U,
                                     false, false))
    {
        KernelWriteString("  An inode table running past the end of the volume was "
                          "accepted.\n");
        succeeded = false;
    }

    /* Two structures beginning upon the same block. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_INODE_BITMAP,
                                     KERNEL_VOLUME_BLOCK_BITMAP, false, false))
    {
        KernelWriteString("  A group with two structures upon one block was accepted.\n");
        succeeded = false;
    }

    /* Counts beyond what the group holds. The count of directories is derived
     * from the volume rather than stated, the bound it must exceed being the
     * inodes in use, which changes whenever the composition does. */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_BLOCKS, 200U,
                                     true, false) ||
        !KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_INODES,
                                     superblock->inodes_per_group + 1U, true, false) ||
        !KernelDescriptorRefusedWith(
            device, superblock, EXT2_OFFSET_BG_USED_DIRECTORIES,
            (superblock->inodes_per_group - superblock->free_inode_count) + 1U, true, false))
    {
        KernelWriteString("  A group reporting more than it holds was accepted.\n");
        succeeded = false;
    }

    /*
     * A descriptor every rule above accepts, whose free count nevertheless
     * disagrees with the superblock's. This is the assertion the table makes as
     * a whole and it is the one a misread table fails.
     */
    if (!KernelDescriptorRefusedWith(device, superblock, EXT2_OFFSET_BG_FREE_BLOCKS, 50U, true,
                                     true))
    {
        KernelWriteString("  A table not accounting for the volume's free space was "
                          "accepted.\n");
        succeeded = false;
    }

    /* Requests with nothing to work upon. */
    if (Ext2ReadGroupDescriptor(NULL, superblock, 0U, &descriptor) ||
        Ext2ReadGroupDescriptor(device, superblock, 0U, NULL) ||
        Ext2VerifyGroupDescriptors(NULL, superblock))
    {
        KernelWriteString("  A degenerate descriptor request was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

/*
 * Alters one word of the composed filesystem, beneath the superblock, and
 * reports whether the inode reader refused the result. The volume is recomposed
 * and the cache invalidated afterwards, for the reason KernelVerifyExt2VolumeRefusedWith
 * gives.
 */
static bool KernelInodeRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                   size_t offset, uint32_t value)
{
    Ext2Inode inode;
    bool refused;

    KernelStoreWord(offset, value);
    (void)BufferInvalidateDevice(device);

    refused = !Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &inode);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * Asserts that an inode is found where the format says it is, that its fields
 * are read from the right offsets, and that a file block index is resolved
 * through however many levels of indirection it requires.
 *
 * Locating an inode is three pieces of arithmetic upon numbers that begin at one
 * and indices that begin at zero, and every plausible mistake in it — omitting
 * the subtraction, using the block size where the inode size belongs, taking the
 * group's first block for its inode table — yields an offset that lands upon
 * some other inode of the same volume. That inode is a valid inode. It simply
 * belongs to a different file, and nothing in the machine can tell.
 *
 * The resolution of the block pointers fails the same way. An index that lands
 * one entry adrift within an indirect block, or a level of the walk that divides
 * by the wrong span, produces a block number that is a real block of the volume
 * holding somebody else's data.
 */
bool KernelVerifyExt2Inodes(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t indirect_base = EXT2_DIRECT_BLOCK_COUNT;
    const uint64_t double_base = indirect_base + KERNEL_VOLUME_POINTERS;
    const uint64_t triple_base =
        double_base + ((uint64_t)KERNEL_VOLUME_POINTERS * KERNEL_VOLUME_POINTERS);
    const uint64_t beyond =
        triple_base + ((uint64_t)KERNEL_VOLUME_POINTERS * KERNEL_VOLUME_POINTERS *
                       KERNEL_VOLUME_POINTERS);
    Ext2Inode root;
    Ext2Inode file;
    uint32_t block;
    bool succeeded = true;

    /* The root directory, which the format reserves as inode 2. */
    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("  The root inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((root.number != EXT2_ROOT_INODE) || !Ext2InodeIsDirectory(&root) ||
        Ext2InodeIsRegular(&root) || (root.link_count != 3U) ||
        (root.size != KERNEL_VOLUME_BLOCK_SIZE) ||
        (root.block[0] != KERNEL_VOLUME_ROOT_DATA) ||
        ((root.mode & EXT2_PERMISSION_MASK) != 0x01EDU))
    {
        KernelWriteString("  The root inode was not read correctly.\n");
        succeeded = false;
    }

    /*
     * The file, which lies in the second block of the inode table: inode 11 is
     * index 10, and eight inodes of 128 bytes occupy a block of 1024. An inode
     * reader that never crossed out of the first block of the table would pass
     * every assertion above and fail here.
     */
    if (!Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &file))
    {
        KernelWriteString("  The composed file inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    if ((file.number != KERNEL_VOLUME_FILE_INODE) || !Ext2InodeIsRegular(&file) ||
        Ext2InodeIsDirectory(&file) || (file.size != KERNEL_VOLUME_FILE_SIZE) ||
        (file.link_count != 1U) || (file.sector_count != 32U) || (file.uid != 1000U) ||
        (file.gid != 1001U) || ((file.mode & EXT2_PERMISSION_MASK) != 0x01A4U))
    {
        KernelWriteString("  A field of the file inode was read from the wrong place.\n");
        succeeded = false;
    }

    if (Ext2InodeBlockCount(superblock, &file) != (KERNEL_VOLUME_FILE_SIZE / 1024U))
    {
        KernelWriteString("  The blocks the file's size spans were counted wrongly.\n");
        succeeded = false;
    }

    /* The twelve direct blocks, at both ends of the range. */
    if (!Ext2InodeBlock(device, superblock, &file, 0U, &block) ||
        (block != KERNEL_VOLUME_DIRECT_FIRST))
    {
        KernelWriteString("  The first direct block was resolved wrongly.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, EXT2_DIRECT_BLOCK_COUNT - 1U, &block) ||
        (block != (KERNEL_VOLUME_DIRECT_FIRST + EXT2_DIRECT_BLOCK_COUNT - 1U)))
    {
        KernelWriteString("  The last direct block was resolved wrongly.\n");
        succeeded = false;
    }

    /*
     * The indirect block, at its first and last entries. The last is the
     * boundary the whole decomposition turns upon: an index one beyond it must
     * enter the doubly indirect block instead.
     */
    if (!Ext2InodeBlock(device, superblock, &file, indirect_base, &block) ||
        (block != KERNEL_VOLUME_INDIRECT_DATA))
    {
        KernelWriteString("  The first indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, double_base - 1U, &block) ||
        (block != KERNEL_VOLUME_INDIRECT_LAST))
    {
        KernelWriteString("  The last indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /* A hole within an indirect block, which is a block of zeroes and not an
     * error and not the end of the file. */
    if (!Ext2InodeBlock(device, superblock, &file, indirect_base + 1U, &block) ||
        (block != 0U))
    {
        KernelWriteString("  A hole within an indirect block was not reported as one.\n");
        succeeded = false;
    }

    /* The doubly indirect block: a hole at its first entry, data at its sixth. */
    if (!Ext2InodeBlock(device, superblock, &file, double_base, &block) || (block != 0U))
    {
        KernelWriteString("  A hole beneath the doubly indirect block was not reported "
                          "as one.\n");
        succeeded = false;
    }

    if (!Ext2InodeBlock(device, superblock, &file, double_base + 5U, &block) ||
        (block != KERNEL_VOLUME_DOUBLE_DATA))
    {
        KernelWriteString("  A doubly indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /* The triply indirect block, three levels down. */
    if (!Ext2InodeBlock(device, superblock, &file, triple_base + 3U, &block) ||
        (block != KERNEL_VOLUME_TRIPLE_DATA))
    {
        KernelWriteString("  A triply indirect block was resolved wrongly.\n");
        succeeded = false;
    }

    /*
     * A hole at the top of a subtree. The triply indirect entry of this inode is
     * present, but the doubly indirect block beneath it holds one entry only, so
     * everything past that entry's range is a hole reached without any block
     * being read at all.
     */
    if (!Ext2InodeBlock(device, superblock, &file,
                        triple_base + ((uint64_t)KERNEL_VOLUME_POINTERS *
                                       KERNEL_VOLUME_POINTERS),
                        &block) ||
        (block != 0U))
    {
        KernelWriteString("  A hole occupying a whole subtree was not reported as one.\n");
        succeeded = false;
    }

    /* An index beyond what fifteen pointers can address is refused, not held. */
    if (Ext2InodeBlock(device, superblock, &file, beyond, &block))
    {
        KernelWriteString("  An index beyond the triply indirect range was resolved.\n");
        succeeded = false;
    }

    /* Inode numbers the volume does not hold, at both ends. */
    if (Ext2ReadInode(device, superblock, 0U, &file) ||
        Ext2ReadInode(device, superblock, superblock->inode_count + 1U, &file))
    {
        KernelWriteString("  An inode outside the volume was read.\n");
        succeeded = false;
    }

    /*
     * An inode of the table that was never filled. The bytes are zeroes, which
     * are a valid encoding of nothing, so accepting them would let arithmetic
     * that had strayed beyond the table report a file rather than a mistake.
     */
    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_UNUSED_INODE, &file))
    {
        KernelWriteString("  An inode not in use was read as a file.\n");
        succeeded = false;
    }

    /* A direct pointer outside the volume, refused when the inode is read. */
    if (!KernelInodeRefusedWith(device, superblock,
                                KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, 0U), 9999U))
    {
        KernelWriteString("  An inode naming a block outside the volume was accepted.\n");
        succeeded = false;
    }

    /*
     * A pointer within an indirect block that lies outside the volume. It cannot
     * be caught when the inode is read, the block holding it not having been
     * read then, so it is checked where it is fetched.
     */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, 0U), 9999U);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &file) &&
        Ext2InodeBlock(device, superblock, &file, indirect_base, &block))
    {
        KernelWriteString("  An indirect pointer outside the volume was resolved.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /* Requests with nothing to work upon. */
    if (Ext2ReadInode(NULL, superblock, EXT2_ROOT_INODE, &file) ||
        Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, NULL) ||
        Ext2InodeBlock(device, superblock, NULL, 0U, &block) ||
        Ext2InodeBlock(device, superblock, &root, 0U, NULL))
    {
        KernelWriteString("  A degenerate inode request was accepted.\n");
        succeeded = false;
    }

    return succeeded;
}

