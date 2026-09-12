/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/group.c
 * Purpose: Implements the block group descriptor table: the geometry of the
 *          groups a volume is divided into, the reading and writing of one
 *          descriptor, and the validation of the whole table against the volume
 *          it describes.
 * Key functions: Ext2GroupCount, Ext2GroupDescriptorBlock,
 *          Ext2GroupDescriptorBlocks, Ext2InodeTableBlocks, Ext2GroupFirstBlock,
 *          Ext2GroupBlockCount, Ext2ReadGroupDescriptor,
 *          Ext2WriteGroupDescriptor, Ext2VerifyGroupDescriptors, Ext2GroupsRead,
 *          Ext2GroupsRefused, Ext2ReportGroup.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the Block
 *     Group Descriptor Table chapter: the table starts upon the first block
 *     following the superblock, which is the third block of a 1 KiB volume and
 *     the second of any larger one; a descriptor is 32 bytes; bg_block_bitmap,
 *     bg_inode_bitmap and bg_inode_table at offsets 0, 4 and 8 are absolute
 *     block identifiers, and bg_free_blocks_count, bg_free_inodes_count and
 *     bg_used_dirs_count at 12, 14 and 16 are halves.
 *   - The same, Inode Table: there is one inode table per group and it holds
 *     s_inodes_per_group inodes, so its length follows from the inode size.
 *   - Linux kernel documentation, the ext4 block group descriptor, consulted as
 *     an independent statement of the same offsets.
 */

#include "internal.h"

#include <oxys/block/buffer.h>
#include <oxys/kernel.h>


uint32_t Ext2GroupCount(const Ext2Superblock *superblock)
{
    return (superblock != NULL) ? superblock->group_count : 0U;
}

uint32_t Ext2GroupDescriptorBlock(const Ext2Superblock *superblock)
{
    /*
     * The table begins upon the block after the one the superblock lies within.
     * The superblock always occupies the second kibibyte of the volume, so that
     * block is block 1 of a volume of 1024-byte blocks and block 0 of any other,
     * which is exactly what s_first_data_block holds.
     */
    return superblock->first_data_block + 1U;
}

uint32_t Ext2GroupDescriptorBlocks(const Ext2Superblock *superblock)
{
    const uint32_t per_block = superblock->block_size / EXT2_GROUP_DESCRIPTOR_SIZE;

    return Ext2DivideRoundingUp(superblock->group_count, per_block);
}

uint32_t Ext2InodeTableBlocks(const Ext2Superblock *superblock)
{
    const uint32_t per_block = superblock->block_size / superblock->inode_size;

    return Ext2DivideRoundingUp(superblock->inodes_per_group, per_block);
}

uint32_t Ext2GroupFirstBlock(const Ext2Superblock *superblock, uint32_t group)
{
    return superblock->first_data_block + (group * superblock->blocks_per_group);
}

uint32_t Ext2GroupBlockCount(const Ext2Superblock *superblock, uint32_t group)
{
    const uint32_t first = Ext2GroupFirstBlock(superblock, group);
    const uint32_t remaining = superblock->block_count - first;

    /*
     * Every group holds s_blocks_per_group blocks except the last, which holds
     * whatever remains. The division that fixes the group count rounds upward,
     * so the last group is short whenever the volume is not an exact multiple of
     * the group size, which is the usual case rather than the exception.
     */
    return (remaining < superblock->blocks_per_group) ? remaining
                                                      : superblock->blocks_per_group;
}

bool Ext2ReadGroupDescriptor(BlockDevice *device, const Ext2Superblock *superblock,
                             uint32_t group, Ext2GroupDescriptor *descriptor)
{
    uint8_t raw[EXT2_GROUP_DESCRIPTOR_SIZE];
    Ext2GroupDescriptor parsed;
    uint32_t table;
    uint32_t position;
    uint32_t inode_table_blocks;
    uint32_t group_blocks;

    if ((device == NULL) || (superblock == NULL) || (descriptor == NULL))
    {
        return Ext2GroupRefuse("no device, no volume, or nowhere to put the descriptor");
    }

    if (group >= superblock->group_count)
    {
        return Ext2GroupRefuse("the volume holds no such group");
    }

    /*
     * The table may occupy several blocks, so the descriptor is located by its
     * position within the table and not within one block of it.
     */
    table = Ext2GroupDescriptorBlock(superblock);
    position = group * EXT2_GROUP_DESCRIPTOR_SIZE;

    if ((table + Ext2GroupDescriptorBlocks(superblock)) > superblock->block_count)
    {
        return Ext2GroupRefuse("the descriptor table does not fit within the volume");
    }

    if (!Ext2ReadBytes(device, superblock, table + (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GROUP_DESCRIPTOR_SIZE, raw))
    {
        /* Ext2ReadBytes has already recorded the reason and counted the
         * refusal; counting it twice would say two things went wrong. */
        return false;
    }

    parsed.group = group;
    parsed.block_bitmap = Ext2ReadWord(raw, EXT2_OFFSET_BG_BLOCK_BITMAP);
    parsed.inode_bitmap = Ext2ReadWord(raw, EXT2_OFFSET_BG_INODE_BITMAP);
    parsed.inode_table = Ext2ReadWord(raw, EXT2_OFFSET_BG_INODE_TABLE);
    parsed.free_block_count = Ext2ReadHalf(raw, EXT2_OFFSET_BG_FREE_BLOCKS);
    parsed.free_inode_count = Ext2ReadHalf(raw, EXT2_OFFSET_BG_FREE_INODES);
    parsed.used_directory_count = Ext2ReadHalf(raw, EXT2_OFFSET_BG_USED_DIRECTORIES);

    if (!Ext2BlockExists(superblock, parsed.block_bitmap) ||
        !Ext2BlockExists(superblock, parsed.inode_bitmap) ||
        !Ext2BlockExists(superblock, parsed.inode_table))
    {
        return Ext2GroupRefuse("a structure of the group lies outside the volume");
    }

    /*
     * The three structures are each at least one block long, so no two of them
     * can begin upon the same block. A descriptor read four bytes adrift yields
     * two identical pointers far more often than it yields three plausible ones,
     * which is why this is asserted rather than assumed.
     */
    if ((parsed.block_bitmap == parsed.inode_bitmap) ||
        (parsed.block_bitmap == parsed.inode_table) ||
        (parsed.inode_bitmap == parsed.inode_table))
    {
        return Ext2GroupRefuse("two structures of the group begin upon the same block");
    }

    inode_table_blocks = Ext2InodeTableBlocks(superblock);

    if ((superblock->block_count - parsed.inode_table) < inode_table_blocks)
    {
        return Ext2GroupRefuse("the inode table runs past the end of the volume");
    }

    group_blocks = Ext2GroupBlockCount(superblock, group);

    if (((uint32_t)parsed.free_block_count > group_blocks) ||
        ((uint32_t)parsed.free_inode_count > superblock->inodes_per_group))
    {
        return Ext2GroupRefuse("the group reports more free than it holds");
    }

    /*
     * A directory occupies an inode that is in use, so the directories cannot
     * outnumber the inodes of the group that are not free.
     */
    if ((uint32_t)parsed.used_directory_count >
        (superblock->inodes_per_group - (uint32_t)parsed.free_inode_count))
    {
        return Ext2GroupRefuse("the group holds more directories than used inodes");
    }

    *descriptor = parsed;
    ++Ext2GroupsReadCount;
    return true;
}

bool Ext2VerifyGroupDescriptors(BlockDevice *device, const Ext2Superblock *superblock)
{
    uint64_t free_blocks = 0U;
    uint64_t free_inodes = 0U;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2GroupRefuse("no device or no volume");
    }

    for (uint32_t group = 0U; group < superblock->group_count; ++group)
    {
        Ext2GroupDescriptor descriptor;

        if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
        {
            return false;
        }

        free_blocks += (uint64_t)descriptor.free_block_count;
        free_inodes += (uint64_t)descriptor.free_inode_count;
    }

    /*
     * The groups account for every free block and every free inode of the
     * volume, so their sums must equal the totals the superblock states. This is
     * the strongest statement that can be made about the table as a whole
     * without reading the bitmaps: a table read at the wrong offset, or one
     * descriptor short, yields descriptors that are individually plausible and a
     * sum that is not.
     *
     * It holds only of a volume that was cleanly unmounted. A volume that was
     * not is permitted to disagree with itself — that disagreement is what the
     * state means — and Ext2ReadSuperblock has already made it read-only.
     */
    if (superblock->state == EXT2_VALID_FS)
    {
        if ((free_blocks != (uint64_t)superblock->free_block_count) ||
            (free_inodes != (uint64_t)superblock->free_inode_count))
        {
            return Ext2GroupRefuse("the groups do not account for the volume's free space");
        }
    }

    return true;
}

uint64_t Ext2GroupsRead(void)
{
    return Ext2GroupsReadCount;
}

uint64_t Ext2GroupsRefused(void)
{
    return Ext2GroupsRefusedCount;
}

void Ext2ReportGroup(const Ext2GroupDescriptor *descriptor)
{
    if (descriptor == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 group ");
    KernelWriteDecimal((uint64_t)descriptor->group);
    KernelWriteString(": block bitmap at ");
    KernelWriteDecimal((uint64_t)descriptor->block_bitmap);
    KernelWriteString(", inode bitmap at ");
    KernelWriteDecimal((uint64_t)descriptor->inode_bitmap);
    KernelWriteString(", inode table at ");
    KernelWriteDecimal((uint64_t)descriptor->inode_table);
    KernelWriteString("; ");
    KernelWriteDecimal((uint64_t)descriptor->free_block_count);
    KernelWriteString(" free blocks, ");
    KernelWriteDecimal((uint64_t)descriptor->free_inode_count);
    KernelWriteString(" free inodes, ");
    KernelWriteDecimal((uint64_t)descriptor->used_directory_count);
    KernelWriteString(" directories.\n");
}

bool Ext2WriteGroupDescriptor(BlockDevice *device, const Ext2Superblock *superblock,
                              const Ext2GroupDescriptor *descriptor)
{
    uint8_t raw[EXT2_GROUP_DESCRIPTOR_SIZE];
    uint32_t position;

    if ((device == NULL) || (superblock == NULL) || (descriptor == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or no descriptor to write");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (descriptor->group >= superblock->group_count)
    {
        return Ext2WriteRefuse("the volume holds no group of that number");
    }

    position = descriptor->group * EXT2_GROUP_DESCRIPTOR_SIZE;

    /* Read first, for the reason the superblock is: bg_pad and bg_reserved are
     * not this kernel's to overwrite. */
    if (!Ext2ReadBytes(device, superblock,
                       Ext2GroupDescriptorBlock(superblock) +
                           (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GROUP_DESCRIPTOR_SIZE, raw))
    {
        return false;
    }

    Ext2WriteWord(raw, EXT2_OFFSET_BG_BLOCK_BITMAP, descriptor->block_bitmap);
    Ext2WriteWord(raw, EXT2_OFFSET_BG_INODE_BITMAP, descriptor->inode_bitmap);
    Ext2WriteWord(raw, EXT2_OFFSET_BG_INODE_TABLE, descriptor->inode_table);
    Ext2WriteHalf(raw, EXT2_OFFSET_BG_FREE_BLOCKS, descriptor->free_block_count);
    Ext2WriteHalf(raw, EXT2_OFFSET_BG_FREE_INODES, descriptor->free_inode_count);
    Ext2WriteHalf(raw, EXT2_OFFSET_BG_USED_DIRECTORIES, descriptor->used_directory_count);

    return Ext2WriteBytes(device, superblock,
                          Ext2GroupDescriptorBlock(superblock) +
                              (position / superblock->block_size),
                          position % superblock->block_size, EXT2_GROUP_DESCRIPTOR_SIZE, raw);
}

