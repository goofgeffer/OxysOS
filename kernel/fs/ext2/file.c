/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/file.c
 * Purpose: Implements the contents of a file: the reading of a range of its
 *          bytes through the block pointers that name them, the two forms a
 *          symbolic link's target may be stored in, the writing that extends a
 *          file, and the truncation that releases what a file no longer covers.
 * Key functions: Ext2ReadFile, Ext2ReadSymbolicLink,
 *          Ext2InodeIsFastSymbolicLink, Ext2WriteFile, Ext2TruncateFile,
 *          Ext2FilesRead, Ext2BytesRead, Ext2ReadsRefused.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Symbolic Links chapter: a symbolic link holds a text string interpreted as
 *     a path to another file; for a target shorter than 60 bytes the string is
 *     stored within the inode itself, in the fields that would otherwise hold
 *     the pointers to its data blocks, which avoids allocating a whole block for
 *     a string most links are shorter than.
 *   - The same, the description of i_block: a zero entry denotes a block that is
 *     not allocated rather than the end of the file, which is what makes a hole
 *     in a file readable as zeroes rather than an error.
 *   - The same, i_size and i_blocks: the size is what the file covers and the
 *     block count what it occupies, and a file with a hole has the first without
 *     the second.
 */

#include "internal.h"

#include <oxys/block/buffer.h>
#include <oxys/kernel.h>

bool Ext2ReadFile(BlockDevice *device, const Ext2Superblock *superblock,
                  const Ext2Inode *inode, uint64_t offset, void *buffer, uint64_t length,
                  uint64_t *read)
{
    uint8_t *destination = (uint8_t *)buffer;
    uint64_t remaining;
    uint64_t taken = 0U;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (read == NULL) ||
        ((buffer == NULL) && (length != 0U)))
    {
        return Ext2ReadRefuse("no device, no volume, no inode, nowhere to read into, or "
                              "nowhere to put the count");
    }

    *read = 0U;

    /*
     * A directory's bytes are entries, and are read by traversing it. A caller
     * reading them as a stream has mistaken what it holds, and would receive
     * record lengths and inode numbers as though they were text.
     */
    if (Ext2InodeIsDirectory(inode))
    {
        return Ext2ReadRefuse("a directory is traversed and not read as a stream of bytes");
    }

    /*
     * The end of the file is not a failure. A reader arrives at it by reading,
     * and a kernel that reported it as an error would oblige every caller to
     * treat the ordinary conclusion of its work as a fault; the count reports it
     * instead. An offset beyond the end is the same answer for the same reason.
     */
    if (offset >= inode->size)
    {
        return true;
    }

    remaining = inode->size - offset;

    if (remaining > length)
    {
        remaining = length;
    }

    while (remaining > 0U)
    {
        const uint64_t index = offset / (uint64_t)superblock->block_size;
        const uint32_t within = (uint32_t)(offset % (uint64_t)superblock->block_size);
        uint32_t take = superblock->block_size - within;
        uint32_t block;

        if ((uint64_t)take > remaining)
        {
            take = (uint32_t)remaining;
        }

        if (!Ext2InodeBlock(device, superblock, inode, index, &block))
        {
            *read = taken;
            return false;
        }

        /*
         * A hole reads as zeroes. The block was never allocated, so there is
         * nothing upon the volume to read and nothing is read: the file's
         * contents at that offset are zeroes by definition, not by accident, and
         * a reader cannot tell a hole from a block that was written with zeroes,
         * which is exactly the point of one.
         */
        if (block == 0U)
        {
            Ext2FillZero(destination, take);
        }
        else if (!Ext2ReadBytes(device, superblock, block, within, take, destination))
        {
            *read = taken;
            return false;
        }

        destination += take;
        offset += take;
        remaining -= take;
        taken += take;
    }

    *read = taken;
    Ext2BytesReadCount += taken;
    ++Ext2FilesReadCount;
    return true;
}

bool Ext2InodeIsFastSymbolicLink(const Ext2Superblock *superblock, const Ext2Inode *inode)
{
    uint32_t attribute_sectors;

    if ((superblock == NULL) || !Ext2InodeIsSymbolicLink(inode))
    {
        return false;
    }

    /*
     * i_blocks counts 512-byte sectors, and an extended attribute block is among
     * them although it is not data. Subtracting it leaves the sectors the file's
     * own contents occupy, and a symbolic link with none of those holds its
     * target within the inode.
     */
    attribute_sectors = (inode->file_acl != 0U) ? (superblock->block_size / 512U) : 0U;

    return inode->sector_count == attribute_sectors;
}

bool Ext2ReadSymbolicLink(BlockDevice *device, const Ext2Superblock *superblock,
                          const Ext2Inode *inode, char *target, size_t capacity)
{
    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (target == NULL) ||
        (capacity == 0U))
    {
        return Ext2ReadRefuse("no device, no volume, no inode, or nowhere to put the target");
    }

    if (!Ext2InodeIsSymbolicLink(inode))
    {
        return Ext2ReadRefuse("the inode is not a symbolic link");
    }

    /*
     * A link with no target names nothing. The format does not forbid it; it
     * cannot be resolved, and saying so here is better than resolving the empty
     * path to whatever it happens to reach.
     */
    if (inode->size == 0U)
    {
        return Ext2ReadRefuse("a symbolic link bearing no target");
    }

    if (inode->size >= (uint64_t)capacity)
    {
        return Ext2ReadRefuse("a symbolic link's target is longer than this kernel will read");
    }

    if (Ext2InodeIsFastSymbolicLink(superblock, inode))
    {
        if (inode->size > EXT2_FAST_SYMLINK_CAPACITY)
        {
            return Ext2ReadRefuse("a symbolic link holds no blocks and no room for its target");
        }

        /*
         * The target occupies the sixty bytes of i_block, and i_block was decoded
         * into fifteen words when the inode was read. The bytes are recovered
         * from those words in the order the volume stores them, least significant
         * first — the same order the decoding assumed, applied in reverse.
         */
        for (uint32_t index = 0U; index < (uint32_t)inode->size; ++index)
        {
            const uint32_t word = inode->block[index / EXT2_BLOCK_POINTER_SIZE];
            const uint32_t shift = (index % EXT2_BLOCK_POINTER_SIZE) * 8U;

            target[index] = (char)((word >> shift) & 0xFFU);
        }
    }
    else
    {
        uint64_t read = 0U;

        if (!Ext2ReadFile(device, superblock, inode, 0U, target, inode->size, &read))
        {
            return false;
        }

        if (read != inode->size)
        {
            return Ext2ReadRefuse("a symbolic link's target was read short");
        }
    }

    target[inode->size] = '\0';

    /*
     * A target holding a null byte would be a path shorter than the file says it
     * is, and everything below this point treats it as a terminated string. The
     * format permits the byte; this kernel cannot resolve what it produces.
     */
    for (uint32_t index = 0U; index < (uint32_t)inode->size; ++index)
    {
        if (target[index] == '\0')
        {
            return Ext2ReadRefuse("a symbolic link's target holds a null byte");
        }
    }

    return true;
}

uint64_t Ext2FilesRead(void)
{
    return Ext2FilesReadCount;
}

uint64_t Ext2BytesRead(void)
{
    return Ext2BytesReadCount;
}

uint64_t Ext2ReadsRefused(void)
{
    return Ext2ReadsRefusedCount;
}

/*
 * The directory.
 *
 * Everything above reads a file by its inode number; nothing above knows an
 * inode number, because a user names a file. A directory is what stands between
 * the two, and it is an ordinary file whose data happens to be a sequence of
 * entries rather than anything the format treats specially — which is why the
 * traversal below rests entirely upon Ext2InodeBlock and adds nothing to it but
 * an interpretation of the bytes.
 */

/*
 * The entry file type corresponding to a format held in i_mode.
 *
 * The two numberings are unrelated: i_mode holds the historical Unix values in
 * its high four bits, and the entry's file type is a small integer assigned in
 * an order of its own. A directory is 0x4000 in the one and 2 in the other, a
 * regular file 0x8000 and 1, and a socket 0xC000 and 6. Nothing about either
 * numbering derives from the other, so the correspondence must be written out.
 */
bool Ext2WriteFile(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *inode,
                   uint64_t offset, const void *buffer, uint64_t length, uint64_t *written)
{
    const uint8_t *source = (const uint8_t *)buffer;
    uint64_t remaining = length;
    uint64_t put = 0U;
    bool allocated = false;
    bool grew = false;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (written == NULL) ||
        ((buffer == NULL) && (length != 0U)))
    {
        return Ext2WriteRefuse("no device, no volume, no inode, nothing to write, or "
                               "nowhere to put the count");
    }

    *written = 0U;

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (Ext2InodeIsDirectory(inode))
    {
        return Ext2WriteRefuse("a directory's entries are not written as a stream of bytes");
    }

    while (remaining > 0U)
    {
        const uint64_t index = offset / (uint64_t)superblock->block_size;
        const uint32_t within = (uint32_t)(offset % (uint64_t)superblock->block_size);
        uint32_t take = superblock->block_size - within;
        uint32_t block;

        if ((uint64_t)take > remaining)
        {
            take = (uint32_t)remaining;
        }

        if (!Ext2InodeBlockAllocate(device, superblock, inode, index, &block, &allocated))
        {
            break;
        }

        /*
         * A block newly allocated holds whatever its previous owner left in it.
         * Where this write covers the whole of it that does not matter, every
         * byte being about to be replaced; where it does not, the remainder would
         * become another file's data appearing as this one's contents. It is
         * zeroed in that case and only that case, which is why the allocation
         * reports whether it allocated rather than the caller inferring it from
         * the offsets.
         */
        if (allocated && (take != superblock->block_size) &&
            !Ext2ZeroBlock(device, superblock, block))
        {
            break;
        }

        if (!Ext2WriteBytes(device, superblock, block, within, take, source))
        {
            break;
        }

        source += take;
        offset += take;
        remaining -= take;
        put += take;

        if (offset > inode->size)
        {
            inode->size = offset;
            grew = true;
        }
    }

    *written = put;
    Ext2BytesWrittenCount += put;

    /*
     * The inode is written back whether or not every byte was written. The blocks
     * that were allocated are allocated and the bytes that were written are upon
     * the volume; an inode left unwritten would describe a file shorter than its
     * contents and would leak every block beyond it.
     */
    if ((put > 0U) || grew)
    {
        if (!Ext2WriteInode(device, superblock, inode))
        {
            return false;
        }
    }

    return remaining == 0U;
}

/*
 * Frees the blocks of one pointer subtree at or beyond a file block index.
 *
 * `base` is the file block index the subtree's first entry stands for, and
 * `span` the indices one entry covers. The table itself is freed, and the
 * caller's pointer to it cleared, only when nothing is left in it — which is
 * what makes a truncation to zero return every block of the file and a
 * truncation to the middle of an indirect range keep the table that still holds
 * the earlier half.
 */
static bool Ext2TruncateSubtree(BlockDevice *device, Ext2Superblock *superblock,
                                Ext2Inode *inode, uint32_t *table, uint32_t level,
                                uint64_t base, uint64_t span, uint64_t first)
{
    const uint64_t per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    const uint64_t entry_span = span / per_block;
    bool occupied = false;

    if (*table == 0U)
    {
        return true;
    }

    /*
     * A subtree lying wholly below the new size is retained entire, and is not
     * walked. Without this a truncation of one block from a large file would read
     * every pointer block the file has, which is the whole of its indirection for
     * the sake of one block.
     */
    if ((base + span) <= first)
    {
        return true;
    }

    for (uint64_t entry = 0U; entry < per_block; ++entry)
    {
        const uint64_t entry_base = base + (entry * entry_span);
        uint8_t raw[EXT2_BLOCK_POINTER_SIZE];
        uint32_t child;

        if (!Ext2ReadBytes(device, superblock, *table,
                           (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE),
                           EXT2_BLOCK_POINTER_SIZE, raw))
        {
            return false;
        }

        child = Ext2ReadWord(raw, 0U);

        if (child == 0U)
        {
            continue;
        }

        if (level > 1U)
        {
            if (!Ext2TruncateSubtree(device, superblock, inode, &child, level - 1U, entry_base,
                                     entry_span, first))
            {
                return false;
            }
        }
        else if (entry_base >= first)
        {
            if (!Ext2FreeBlock(device, superblock, child))
            {
                return false;
            }

            inode->sector_count -= Ext2SectorsPerBlock(superblock);
            child = 0U;
        }

        if (child == 0U)
        {
            Ext2WriteWord(raw, 0U, 0U);

            if (!Ext2WriteBytes(device, superblock, *table,
                                (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE),
                                EXT2_BLOCK_POINTER_SIZE, raw))
            {
                return false;
            }
        }
        else
        {
            occupied = true;
        }
    }

    if (!occupied)
    {
        if (!Ext2FreeBlock(device, superblock, *table))
        {
            return false;
        }

        inode->sector_count -= Ext2SectorsPerBlock(superblock);
        *table = 0U;
    }

    return true;
}

/*
 * The mechanism of truncation, without the judgement that a directory is not
 * truncated as a file is.
 *
 * A directory's blocks must be freed when it is removed, and that is the same
 * work; separating the two lets the public entry point refuse a directory — a
 * caller truncating one has almost certainly mistaken what it holds — while
 * Ext2RemoveDirectory, which knows exactly what it holds, uses the mechanism.
 */
bool Ext2TruncateBlocks(BlockDevice *device, Ext2Superblock *superblock,
                        Ext2Inode *inode, uint64_t size)
{
    const uint64_t per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    uint64_t first;

    /*
     * A size above the present one extends the file with a hole rather than with
     * allocated blocks. That is what every Unix does, and it is why truncation is
     * the cheap way to make a large sparse file: nothing is allocated and nothing
     * is written but the size.
     */
    if (size >= inode->size)
    {
        inode->size = size;
        return Ext2WriteInode(device, superblock, inode);
    }

    /* The first block index the file no longer needs. A size that ends within a
     * block keeps that block, the bytes before the new end still being in it. */
    first = (size + (uint64_t)superblock->block_size - 1U) / (uint64_t)superblock->block_size;

    for (uint32_t entry = 0U; entry < EXT2_DIRECT_BLOCK_COUNT; ++entry)
    {
        if ((entry >= first) && (inode->block[entry] != 0U))
        {
            if (!Ext2FreeBlock(device, superblock, inode->block[entry]))
            {
                return false;
            }

            inode->sector_count -= Ext2SectorsPerBlock(superblock);
            inode->block[entry] = 0U;
        }
    }

    if (!Ext2TruncateSubtree(device, superblock, inode, &inode->block[EXT2_INDIRECT_INDEX], 1U,
                             EXT2_DIRECT_BLOCK_COUNT, per_block, first) ||
        !Ext2TruncateSubtree(device, superblock, inode,
                             &inode->block[EXT2_DOUBLE_INDIRECT_INDEX], 2U,
                             EXT2_DIRECT_BLOCK_COUNT + per_block, per_block * per_block,
                             first) ||
        !Ext2TruncateSubtree(device, superblock, inode,
                             &inode->block[EXT2_TRIPLE_INDIRECT_INDEX], 3U,
                             EXT2_DIRECT_BLOCK_COUNT + per_block + (per_block * per_block),
                             per_block * per_block * per_block, first))
    {
        return false;
    }

    inode->size = size;
    return Ext2WriteInode(device, superblock, inode);
}

bool Ext2TruncateFile(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *inode,
                      uint64_t size)
{
    if ((device == NULL) || (superblock == NULL) || (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or no inode to truncate");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (Ext2InodeIsDirectory(inode))
    {
        return Ext2WriteRefuse("a directory is not truncated as a file is");
    }

    return Ext2TruncateBlocks(device, superblock, inode, size);
}


/*
 * Altering a directory.
 *
 * A directory is a file, so the blocks it occupies are allocated and freed by
 * the machinery above. What is particular to a directory is the linked list of
 * records within each of its blocks, and every operation here is an alteration
 * of that list: an insertion splits a record, and a removal joins two.
 *
 * The list is what makes both cheap. Nothing is ever moved: a name is removed by
 * lengthening the record before it, and a name is inserted into the slack that
 * such a lengthening left behind. A directory therefore does not shrink, and
 * repeated creation and removal reuses the same space rather than growing.
 */

/* Writes the eight-byte header of a record, in the volume's own order and in
 * whichever of the two readings of offset 6 the volume uses. */
