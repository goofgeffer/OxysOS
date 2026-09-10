/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/alloc.c
 * Purpose: Implements allocation from the two bitmaps a volume keeps: the
 *          testing and setting of a bit, the search for a free one, the
 *          allocation and release of a block and of an inode, and the group and
 *          superblock summaries that must be kept in step with every one of
 *          them.
 * Key functions: Ext2BlockInUse, Ext2InodeInUse, Ext2AllocateBlock,
 *          Ext2FreeBlock, Ext2AllocateInode, Ext2FreeInode, Ext2BlocksAllocated,
 *          Ext2BlocksFreed, Ext2InodesAllocated, Ext2InodesFreed.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the Block
 *     Bitmap and Inode Bitmap chapters: one bit per block or inode of the group,
 *     1 meaning used, the first of the group being bit 0 of byte 0 and the ninth
 *     bit 0 of byte 1; the inode bitmap begins at inode 1, so an inode number is
 *     converted to a bit index by subtracting one before the group arithmetic.
 *   - The same, the Superblock and Block Group Descriptor chapters:
 *     s_free_blocks_count and s_free_inodes_count, and the per-group
 *     bg_free_blocks_count, bg_free_inodes_count and bg_used_dirs_count, are
 *     summaries of the bitmaps and are not derived from them at read time — so
 *     an allocator that altered a bitmap without altering both summaries would
 *     leave a volume that every tool outside this kernel judges corrupt.
 */

#include "internal.h"

#include <oxys/buffer.h>
#include <oxys/kernel.h>

/*
 * The bitmaps.
 *
 * One bit stands for each block of a group and each inode of it, 1 meaning used
 * and 0 free. The first of the group is bit 0 of byte 0 and the ninth is bit 0
 * of byte 1: least significant bit first within a byte, which is not what a
 * diagram of a byte suggests and is what the format states.
 *
 * The bitmaps are the first structure of the volume this kernel reads that it
 * did not need in order to read a file. Nothing before now had to know which
 * blocks were in use, because nothing allocated one; the free counts were read
 * and believed. That ends here.
 */
#define EXT2_BITS_PER_BYTE 8U

static bool Ext2BitmapTest(BlockDevice *device, const Ext2Superblock *superblock,
                           uint32_t bitmap, uint32_t index, bool *used)
{
    /* Initialised for the reason given upon Ext2BlockPosition below: Ext2ReadBytes
     * fills this only when it succeeds, and it is no longer in this translation
     * unit for a compiler to establish that from. */
    uint8_t byte = 0U;

    if (!Ext2ReadBytes(device, superblock, bitmap, index / EXT2_BITS_PER_BYTE, 1U, &byte))
    {
        return false;
    }

    *used = (byte & (uint8_t)(1U << (index % EXT2_BITS_PER_BYTE))) != 0U;
    return true;
}

/*
 * Sets or clears one bit, refusing to do what has already been done.
 *
 * Setting a bit already set means two owners believe they hold the same block;
 * clearing one already clear means a block is about to be freed twice, and the
 * second free is what lets it be allocated to two files at once. Both are
 * refused rather than performed, because both are corruption that spreads and
 * neither announces itself at the moment it occurs.
 */
static bool Ext2BitmapSet(BlockDevice *device, const Ext2Superblock *superblock,
                          uint32_t bitmap, uint32_t index, bool used, const char *what)
{
    const uint8_t mask = (uint8_t)(1U << (index % EXT2_BITS_PER_BYTE));
    const uint32_t offset = index / EXT2_BITS_PER_BYTE;
    uint8_t byte;

    if (!Ext2ReadBytes(device, superblock, bitmap, offset, 1U, &byte))
    {
        return false;
    }

    if (((byte & mask) != 0U) == used)
    {
        return Ext2WriteRefuse(what);
    }

    byte = used ? (uint8_t)(byte | mask) : (uint8_t)(byte & (uint8_t)~mask);

    return Ext2WriteBytes(device, superblock, bitmap, offset, 1U, &byte);
}

/*
 * The group a block belongs to, and its index within that group's bitmap.
 *
 * Both outputs are written before anything can fail, so that they are defined
 * whatever this returns. That is not defensive habit: `Ext2WriteRefuse` returns
 * false and has always returned false, but it lives in another translation unit
 * since this file was divided out of `kernel/fs/ext2.c`, and a compiler that
 * cannot see its body cannot know that. It must therefore assume this function
 * may return true without having written either output — which is exactly the
 * inference `-Werror=maybe-uninitialized` reported when the division was made.
 *
 * Writing them first makes the postcondition unconditional rather than dependent
 * upon an inference that only held while the two functions shared a file.
 */
static bool Ext2BlockPosition(const Ext2Superblock *superblock, uint32_t block,
                              uint32_t *group, uint32_t *index)
{
    *group = 0U;
    *index = 0U;

    if (!Ext2BlockExists(superblock, block))
    {
        return Ext2WriteRefuse("the block lies outside the volume");
    }

    *group = (block - superblock->first_data_block) / superblock->blocks_per_group;
    *index = (block - superblock->first_data_block) % superblock->blocks_per_group;
    return true;
}

bool Ext2BlockInUse(BlockDevice *device, const Ext2Superblock *superblock, uint32_t block,
                    bool *used)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL) || (used == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the answer");
    }

    if (!Ext2BlockPosition(superblock, block, &group, &index) ||
        !Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    return Ext2BitmapTest(device, superblock, descriptor.block_bitmap, index, used);
}

bool Ext2InodeInUse(BlockDevice *device, const Ext2Superblock *superblock, uint32_t number,
                    bool *used)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL) || (used == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the answer");
    }

    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2WriteRefuse("the volume holds no inode of that number");
    }

    /* Inode numbers begin at one and the bits at zero, exactly as when an inode
     * is located within its table. */
    group = (number - 1U) / superblock->inodes_per_group;
    index = (number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    return Ext2BitmapTest(device, superblock, descriptor.inode_bitmap, index, used);
}

/*
 * Finds the first free bit of a group's bitmap, searching only the bits that
 * stand for something.
 *
 * The last group is short whenever the volume is not an exact multiple of the
 * group size, and the bits beyond its blocks are set by whatever made the
 * volume. Bounding the search by the group's true extent rather than trusting
 * those bits is what keeps this from issuing a block the volume does not hold
 * upon a volume that left them clear.
 */
static bool Ext2BitmapFindFree(BlockDevice *device, const Ext2Superblock *superblock,
                               uint32_t bitmap, uint32_t count, uint32_t *index, bool *found)
{
    *found = false;

    for (uint32_t position = 0U; position < count; ++position)
    {
        bool used;

        if (!Ext2BitmapTest(device, superblock, bitmap, position, &used))
        {
            return false;
        }

        if (!used)
        {
            *index = position;
            *found = true;
            return true;
        }
    }

    return true;
}

bool Ext2AllocateBlock(BlockDevice *device, Ext2Superblock *superblock, uint32_t near,
                       uint32_t *block)
{
    uint32_t first_group = 0U;

    if ((device == NULL) || (superblock == NULL) || (block == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the block");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (superblock->free_block_count == 0U)
    {
        return Ext2WriteRefuse("the volume holds no free block");
    }

    /*
     * The hint names a block the caller would like to be near — ordinarily the
     * previous block of the same file. Beginning the search in that block's group
     * is the whole of this kernel's allocation policy: it keeps a file's blocks
     * together, which is what makes reading it sequential, and it costs one
     * division.
     */
    if ((near != 0U) && Ext2BlockExists(superblock, near))
    {
        uint32_t index;

        if (!Ext2BlockPosition(superblock, near, &first_group, &index))
        {
            return false;
        }
    }

    for (uint32_t attempt = 0U; attempt < superblock->group_count; ++attempt)
    {
        const uint32_t group = (first_group + attempt) % superblock->group_count;
        Ext2GroupDescriptor descriptor;
        uint32_t index;
        bool found;

        if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
        {
            return false;
        }

        if (descriptor.free_block_count == 0U)
        {
            continue;
        }

        if (!Ext2BitmapFindFree(device, superblock, descriptor.block_bitmap,
                                Ext2GroupBlockCount(superblock, group), &index, &found))
        {
            return false;
        }

        if (!found)
        {
            /*
             * The descriptor claimed free blocks and the bitmap holds none. The
             * volume contradicts itself, and allocating from another group would
             * leave the contradiction in place for the next caller to meet.
             */
            return Ext2WriteRefuse("a group's free count disagrees with its block bitmap");
        }

        /* The bitmap first, then the accounting; see the discipline in ext2.h. */
        if (!Ext2BitmapSet(device, superblock, descriptor.block_bitmap, index, true,
                           "a block already in use was allocated"))
        {
            return false;
        }

        descriptor.free_block_count--;
        superblock->free_block_count--;

        if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
            !Ext2WriteSuperblock(device, superblock))
        {
            return false;
        }

        *block = Ext2GroupFirstBlock(superblock, group) + index;
        ++Ext2BlocksAllocatedCount;
        return true;
    }

    return Ext2WriteRefuse("no group of the volume holds a free block");
}

bool Ext2FreeBlock(BlockDevice *device, Ext2Superblock *superblock, uint32_t block)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2WriteRefuse("no device or no volume");
    }

    if (!Ext2Writable(superblock) || !Ext2BlockPosition(superblock, block, &group, &index) ||
        !Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    if (!Ext2BitmapSet(device, superblock, descriptor.block_bitmap, index, false,
                       "a block that was already free was freed"))
    {
        return false;
    }

    descriptor.free_block_count++;
    superblock->free_block_count++;

    if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
        !Ext2WriteSuperblock(device, superblock))
    {
        return false;
    }

    ++Ext2BlocksFreedCount;
    return true;
}

bool Ext2AllocateInode(BlockDevice *device, Ext2Superblock *superblock, bool directory,
                       uint32_t *number)
{
    if ((device == NULL) || (superblock == NULL) || (number == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the inode");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (superblock->free_inode_count == 0U)
    {
        return Ext2WriteRefuse("the volume holds no free inode");
    }

    for (uint32_t group = 0U; group < superblock->group_count; ++group)
    {
        Ext2GroupDescriptor descriptor;
        uint32_t index;
        uint32_t candidate;
        bool found;

        if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
        {
            return false;
        }

        if (descriptor.free_inode_count == 0U)
        {
            continue;
        }

        if (!Ext2BitmapFindFree(device, superblock, descriptor.inode_bitmap,
                                superblock->inodes_per_group, &index, &found))
        {
            return false;
        }

        if (!found)
        {
            return Ext2WriteRefuse("a group's free count disagrees with its inode bitmap");
        }

        candidate = (group * superblock->inodes_per_group) + index + 1U;

        /*
         * An inode below s_first_ino belongs to the filesystem. Such an inode is
         * ordinarily marked used in the bitmap already, so this is a second line
         * and not the first; a volume that left one clear would otherwise have
         * its root directory issued to a file.
         */
        if (candidate < superblock->first_inode)
        {
            continue;
        }

        if (!Ext2BitmapSet(device, superblock, descriptor.inode_bitmap, index, true,
                           "an inode already in use was allocated"))
        {
            return false;
        }

        descriptor.free_inode_count--;
        superblock->free_inode_count--;

        if (directory)
        {
            descriptor.used_directory_count++;
        }

        if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
            !Ext2WriteSuperblock(device, superblock))
        {
            return false;
        }

        *number = candidate;
        ++Ext2InodesAllocatedCount;
        return true;
    }

    return Ext2WriteRefuse("no group of the volume holds a free inode");
}

bool Ext2FreeInode(BlockDevice *device, Ext2Superblock *superblock, uint32_t number,
                   bool directory)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2WriteRefuse("no device or no volume");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2WriteRefuse("the volume holds no inode of that number");
    }

    if (number < superblock->first_inode)
    {
        return Ext2WriteRefuse("an inode belonging to the filesystem was freed");
    }

    group = (number - 1U) / superblock->inodes_per_group;
    index = (number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    if (!Ext2BitmapSet(device, superblock, descriptor.inode_bitmap, index, false,
                       "an inode that was already free was freed"))
    {
        return false;
    }

    descriptor.free_inode_count++;
    superblock->free_inode_count++;

    if (directory && (descriptor.used_directory_count > 0U))
    {
        descriptor.used_directory_count--;
    }

    if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
        !Ext2WriteSuperblock(device, superblock))
    {
        return false;
    }

    ++Ext2InodesFreedCount;
    return true;
}

/*
 * Growing a file.
 *
 * The decomposition of a block index into levels of indirection is the one
 * Ext2InodeBlock performs, and it is performed a second time here rather than
 * shared, because the two walks differ at every step: one reads a pointer and
 * accepts zero as a hole, and the other must allocate a block where it finds
 * zero, zero that block if it is a block of pointers, and write the pointer back
 * into whatever holds it.
 */

