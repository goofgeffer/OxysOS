/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/inode.c
 * Purpose: Implements the inode: its retrieval from the table of its group, the
 *          resolution of a file's block index through the direct, indirect,
 *          doubly and triply indirect pointers, the allocation of the blocks and
 *          the pointer blocks a file grows into, and the writing of an inode
 *          back to the volume.
 * Key functions: Ext2ReadInode, Ext2WriteInode, Ext2InodeBlock,
 *          Ext2InodeBlockAllocate, Ext2InodeBlockCount, Ext2InodeIsDirectory,
 *          Ext2InodeIsRegular, Ext2InodeIsSymbolicLink, Ext2InodesRead,
 *          Ext2InodesRefused, Ext2ReportInode.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", Locating
 *     an Inode: the group holding an inode is (inode - 1) / s_inodes_per_group
 *     and its index within that group's table is (inode - 1) % s_inodes_per_group,
 *     inode numbers beginning at one and indices at zero. The worked values of
 *     Table 3.20 were used to check the arithmetic.
 *   - The same, Table 3.13 and the description of i_block: an inode is 128 bytes
 *     of defined structure; i_block at offset 40 holds fifteen block numbers, the
 *     first twelve direct, the thirteenth indirect, the fourteenth doubly
 *     indirect and the fifteenth triply indirect; a zero entry denotes a block
 *     that is not allocated rather than the end of the file.
 *   - The same, i_size: upon a revision 1 volume the high 32 bits of a regular
 *     file's size are held in the field otherwise called i_dir_acl, at offset
 *     108.
 *   - The same, i_blocks: counted in 512-byte sectors and not in blocks of the
 *     volume, which is why a pointer block costs the count as much as a data one.
 */

#include "internal.h"

#include <oxys/block/buffer.h>
#include <oxys/kernel.h>

/*
 * Reads one entry of a block of pointers.
 *
 * A table block of zero is a hole occupying the whole subtree beneath it: the
 * pointer block was never allocated, so none of the blocks it would have named
 * exist, and every entry of it reads as zero. Returning zero rather than
 * refusing is what makes a sparse file readable, and it is the reason this is
 * one function rather than a check repeated at each of the three levels.
 */
static bool Ext2ReadPointer(BlockDevice *device, const Ext2Superblock *superblock,
                            uint32_t table, uint64_t entry, uint32_t *block)
{
    uint8_t raw[EXT2_BLOCK_POINTER_SIZE];

    if (table == 0U)
    {
        *block = 0U;
        return true;
    }

    if (!Ext2BlockExists(superblock, table))
    {
        return Ext2InodeRefuse("a block of pointers lies outside the volume");
    }

    if (!Ext2ReadBytes(device, superblock, table,
                       (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE), EXT2_BLOCK_POINTER_SIZE,
                       raw))
    {
        return false;
    }

    *block = Ext2ReadWord(raw, 0U);

    if ((*block != 0U) && !Ext2BlockExists(superblock, *block))
    {
        return Ext2InodeRefuse("a block pointer addresses a block the volume does not hold");
    }

    return true;
}

bool Ext2InodeBlock(BlockDevice *device, const Ext2Superblock *superblock,
                    const Ext2Inode *inode, uint64_t index, uint32_t *block)
{
    uint64_t per_block;
    uint64_t remaining;
    uint32_t level;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (block == NULL))
    {
        return Ext2InodeRefuse("no device, no volume, no inode, or nowhere to put the block");
    }

    if (index < EXT2_DIRECT_BLOCK_COUNT)
    {
        *block = inode->block[index];
        return true;
    }

    per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    remaining = index - EXT2_DIRECT_BLOCK_COUNT;

    /*
     * The three indirect entries address per_block, per_block squared and
     * per_block cubed blocks in turn. The index is reduced by each range it lies
     * beyond, so that what remains is the offset within the range it lies in;
     * the level is then the number of pointer blocks that must be walked.
     */
    if (remaining < per_block)
    {
        level = 1U;
    }
    else
    {
        remaining -= per_block;

        if (remaining < (per_block * per_block))
        {
            level = 2U;
        }
        else
        {
            remaining -= per_block * per_block;

            if (remaining < (per_block * per_block * per_block))
            {
                level = 3U;
            }
            else
            {
                return Ext2InodeRefuse("the block index is beyond what an inode can address");
            }
        }
    }

    /*
     * The walk begins at the entry of i_block for the level and descends,
     * dividing the offset by the span of one entry at each step. The span of an
     * entry at the deepest level is one block, so the last step indexes directly.
     */
    *block = inode->block[EXT2_INDIRECT_INDEX + (level - 1U)];

    while (level > 0U)
    {
        uint64_t span = 1U;

        for (uint32_t power = 1U; power < level; ++power)
        {
            span *= per_block;
        }

        if (!Ext2ReadPointer(device, superblock, *block, remaining / span, block))
        {
            return false;
        }

        remaining %= span;
        --level;
    }

    return true;
}

uint64_t Ext2InodeBlockCount(const Ext2Superblock *superblock, const Ext2Inode *inode)
{
    if ((superblock == NULL) || (inode == NULL))
    {
        return 0U;
    }

    return (inode->size + (uint64_t)superblock->block_size - 1U) /
           (uint64_t)superblock->block_size;
}

bool Ext2InodeIsDirectory(const Ext2Inode *inode)
{
    return (inode != NULL) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
}

bool Ext2InodeIsRegular(const Ext2Inode *inode)
{
    return (inode != NULL) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG);
}

bool Ext2InodeIsSymbolicLink(const Ext2Inode *inode)
{
    return (inode != NULL) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK);
}

bool Ext2ReadInode(BlockDevice *device, const Ext2Superblock *superblock, uint32_t number,
                   Ext2Inode *inode)
{
    uint8_t raw[EXT2_GOOD_OLD_INODE_SIZE];
    Ext2GroupDescriptor descriptor;
    Ext2Inode parsed;
    uint32_t group;
    uint32_t index;
    uint32_t position;
    bool holds_target;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL))
    {
        return Ext2InodeRefuse("no device, no volume, or nowhere to put the inode");
    }

    /*
     * Inode numbers begin at one, and the volume holds s_inodes_count of them.
     * Zero is not an inode at all: a directory entry bearing it names nothing,
     * which is how a deleted entry is recorded.
     */
    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2InodeRefuse("the volume holds no inode of that number");
    }

    group = (number - 1U) / superblock->inodes_per_group;
    index = (number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    /*
     * The inode lies at index * s_inode_size within the group's table. The
     * superblock has already been made to state an inode size that is a power of
     * two no larger than a block, so a whole number of inodes occupies a block
     * and the 128 bytes read below never straddle two.
     */
    position = index * superblock->inode_size;

    if (!Ext2ReadBytes(device, superblock,
                       descriptor.inode_table + (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GOOD_OLD_INODE_SIZE, raw))
    {
        return false;
    }

    parsed.number = number;
    parsed.mode = Ext2ReadHalf(raw, EXT2_OFFSET_I_MODE);
    parsed.uid = Ext2ReadHalf(raw, EXT2_OFFSET_I_UID);
    parsed.gid = Ext2ReadHalf(raw, EXT2_OFFSET_I_GID);
    parsed.access_time = Ext2ReadWord(raw, EXT2_OFFSET_I_ATIME);
    parsed.change_time = Ext2ReadWord(raw, EXT2_OFFSET_I_CTIME);
    parsed.modify_time = Ext2ReadWord(raw, EXT2_OFFSET_I_MTIME);
    parsed.delete_time = Ext2ReadWord(raw, EXT2_OFFSET_I_DTIME);
    parsed.link_count = Ext2ReadHalf(raw, EXT2_OFFSET_I_LINKS_COUNT);
    parsed.sector_count = Ext2ReadWord(raw, EXT2_OFFSET_I_BLOCKS);
    parsed.flags = Ext2ReadWord(raw, EXT2_OFFSET_I_FLAGS);
    parsed.generation = Ext2ReadWord(raw, EXT2_OFFSET_I_GENERATION);
    parsed.file_acl = Ext2ReadWord(raw, EXT2_OFFSET_I_FILE_ACL);
    parsed.size = (uint64_t)Ext2ReadWord(raw, EXT2_OFFSET_I_SIZE);

    /*
     * A revision 1 volume keeps the high half of a regular file's size in the
     * field a revision 0 volume calls i_dir_acl. It is a size only for a regular
     * file: upon a directory the same bytes mean something else entirely, and a
     * kernel that joined them regardless would give a directory a size of some
     * gigabytes and read it until it fell off the volume.
     */
    if ((superblock->revision >= EXT2_DYNAMIC_REV) &&
        ((parsed.mode & EXT2_S_IFMT) == EXT2_S_IFREG))
    {
        parsed.size |= (uint64_t)Ext2ReadWord(raw, EXT2_OFFSET_I_DIR_ACL) << 32;
    }

    /*
     * The fifteen words of i_block are block pointers for every file but one: a
     * symbolic link whose target is shorter than sixty bytes holds that target
     * in them instead, which is why it needs no block at all.
     *
     * Whether this inode is such a link is decided before the words are
     * examined, because the two readings are incompatible. Read as pointers, the
     * text "sub" is the word 0x00627573 — a block number some millions beyond the
     * end of any volume this kernel composes — and validating it as one refuses
     * the inode outright. Every fast symbolic link upon every real volume would
     * be unreadable, and the diagnosis would name a block pointer that is not a
     * block pointer.
     *
     * The words are decoded either way. Ext2ReadSymbolicLink recovers the bytes
     * of the target from them, in the order the volume stores them. The decision
     * rests upon the mode, the sector count and the extended attribute block,
     * every one of which has been parsed above.
     */
    holds_target = Ext2InodeIsFastSymbolicLink(superblock, &parsed);

    for (uint32_t entry = 0U; entry < EXT2_BLOCK_POINTER_COUNT; ++entry)
    {
        parsed.block[entry] =
            Ext2ReadWord(raw, EXT2_OFFSET_I_BLOCK + (entry * EXT2_BLOCK_POINTER_SIZE));

        if (!holds_target && (parsed.block[entry] != 0U) &&
            !Ext2BlockExists(superblock, parsed.block[entry]))
        {
            return Ext2InodeRefuse("a block pointer of the inode lies outside the volume");
        }
    }

    /*
     * An inode with no format and no links is a table entry that was never
     * filled. Refusing it is how arithmetic that has strayed beyond the inode
     * table announces itself: the bytes past the table are zeroes upon a fresh
     * volume, and a kernel that accepted them would report a file of no type and
     * no blocks rather than the mistake that produced it.
     */
    if ((parsed.mode == 0U) && (parsed.link_count == 0U))
    {
        return Ext2InodeRefuse("the inode is not in use");
    }

    /*
     * An inode bearing no names and a deletion time is a file that was
     * destroyed. Its mode and its block pointers are still what they were —
     * nothing overwrites them, and a recovery tool reads them for exactly that
     * reason — so nothing here distinguishes it from a live file but this.
     *
     * It is refused because no name leads to it, so nothing above has any lawful
     * way to reach it: an attempt to read one means a directory entry survives
     * that should not, and the blocks it names have been given to somebody else.
     * Reading it would serve another file's data under the dead file's name.
     */
    if ((parsed.link_count == 0U) && (parsed.delete_time != 0U))
    {
        return Ext2InodeRefuse("the inode names a file that was deleted");
    }

    *inode = parsed;
    ++Ext2InodesReadCount;
    return true;
}

uint64_t Ext2InodesRead(void)
{
    return Ext2InodesReadCount;
}

uint64_t Ext2InodesRefused(void)
{
    return Ext2InodesRefusedCount;
}

void Ext2ReportInode(const Ext2Inode *inode)
{
    if (inode == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 inode ");
    KernelWriteDecimal((uint64_t)inode->number);
    KernelWriteString(": mode ");
    KernelWriteHexadecimal((uint64_t)inode->mode);
    KernelWriteString(" (");

    if (Ext2InodeIsDirectory(inode))
    {
        KernelWriteString("directory");
    }
    else if (Ext2InodeIsRegular(inode))
    {
        KernelWriteString("regular file");
    }
    else if (Ext2InodeIsSymbolicLink(inode))
    {
        KernelWriteString("symbolic link");
    }
    else
    {
        KernelWriteString("other");
    }

    KernelWriteString("), ");
    KernelWriteDecimal(inode->size);
    KernelWriteString(" bytes, ");
    KernelWriteDecimal((uint64_t)inode->link_count);
    KernelWriteString(" links, ");
    KernelWriteDecimal((uint64_t)inode->sector_count);
    KernelWriteString(" sectors, first block ");
    KernelWriteDecimal((uint64_t)inode->block[0]);
    KernelWriteString(".\n");
}

/*
 * File reading.
 *
 * Everything to this point locates things: a superblock, a descriptor, an inode,
 * a block of a file, a name within a directory. This is the first that produces
 * the contents of a file, and it is the shortest piece of work in the chapter
 * precisely because the locating was done properly — the whole of it is the
 * arithmetic of a byte range against a block size, and one call per block to
 * machinery that already exists.
 */

bool Ext2WriteInode(BlockDevice *device, const Ext2Superblock *superblock,
                    const Ext2Inode *inode)
{
    uint8_t raw[EXT2_GOOD_OLD_INODE_SIZE];
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;
    uint32_t position;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or no inode to write");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if ((inode->number == 0U) || (inode->number > superblock->inode_count))
    {
        return Ext2WriteRefuse("the volume holds no inode of that number");
    }

    group = (inode->number - 1U) / superblock->inodes_per_group;
    index = (inode->number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    position = index * superblock->inode_size;

    /* Read first: an inode of 256 bytes carries extensions beyond the 128 this
     * kernel parses, and they are not this kernel's to discard. */
    if (!Ext2ReadBytes(device, superblock,
                       descriptor.inode_table + (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GOOD_OLD_INODE_SIZE, raw))
    {
        return false;
    }

    Ext2WriteHalf(raw, EXT2_OFFSET_I_MODE, inode->mode);
    Ext2WriteHalf(raw, EXT2_OFFSET_I_UID, inode->uid);
    Ext2WriteHalf(raw, EXT2_OFFSET_I_GID, inode->gid);
    Ext2WriteWord(raw, EXT2_OFFSET_I_SIZE, (uint32_t)(inode->size & 0xFFFFFFFFU));
    Ext2WriteWord(raw, EXT2_OFFSET_I_ATIME, inode->access_time);
    Ext2WriteWord(raw, EXT2_OFFSET_I_CTIME, inode->change_time);
    Ext2WriteWord(raw, EXT2_OFFSET_I_MTIME, inode->modify_time);
    Ext2WriteWord(raw, EXT2_OFFSET_I_DTIME, inode->delete_time);
    Ext2WriteHalf(raw, EXT2_OFFSET_I_LINKS_COUNT, inode->link_count);
    Ext2WriteWord(raw, EXT2_OFFSET_I_BLOCKS, inode->sector_count);
    Ext2WriteWord(raw, EXT2_OFFSET_I_FLAGS, inode->flags);
    Ext2WriteWord(raw, EXT2_OFFSET_I_GENERATION, inode->generation);
    Ext2WriteWord(raw, EXT2_OFFSET_I_FILE_ACL, inode->file_acl);

    /*
     * The high half of the size is written only for a regular file upon a
     * revision 1 volume. Upon a directory those same bytes are i_dir_acl and
     * mean something else entirely, exactly as when they are read.
     */
    if ((superblock->revision >= EXT2_DYNAMIC_REV) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG))
    {
        Ext2WriteWord(raw, EXT2_OFFSET_I_DIR_ACL, (uint32_t)(inode->size >> 32));
    }

    for (uint32_t entry = 0U; entry < EXT2_BLOCK_POINTER_COUNT; ++entry)
    {
        Ext2WriteWord(raw, EXT2_OFFSET_I_BLOCK + (entry * EXT2_BLOCK_POINTER_SIZE),
                      inode->block[entry]);
    }

    return Ext2WriteBytes(device, superblock,
                          descriptor.inode_table + (position / superblock->block_size),
                          position % superblock->block_size, EXT2_GOOD_OLD_INODE_SIZE, raw);
}

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

/* Reads one bit of a bitmap block, the index counting from the first of the group. */
/*
 * Reads one entry of a block of pointers, allocating the entry's block where it
 * is zero and writing the pointer back.
 *
 * `zero` says whether the newly allocated block is itself a block of pointers,
 * which must be zeroed: an unzeroed one is read as pointers to whatever the
 * block last held, and those are real blocks belonging to other files.
 */
static bool Ext2PointerAllocate(BlockDevice *device, Ext2Superblock *superblock,
                                Ext2Inode *inode, uint32_t table, uint64_t entry, bool zero,
                                uint32_t *block, bool *allocated)
{
    uint8_t raw[EXT2_BLOCK_POINTER_SIZE];
    const uint32_t offset = (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE);

    if (!Ext2ReadBytes(device, superblock, table, offset, EXT2_BLOCK_POINTER_SIZE, raw))
    {
        return false;
    }

    *block = Ext2ReadWord(raw, 0U);

    if (*block != 0U)
    {
        if (!Ext2BlockExists(superblock, *block))
        {
            return Ext2WriteRefuse("a block pointer addresses a block the volume does not hold");
        }

        return true;
    }

    if (!Ext2AllocateBlock(device, superblock, table, block))
    {
        return false;
    }

    if (zero && !Ext2ZeroBlock(device, superblock, *block))
    {
        return false;
    }

    inode->sector_count += Ext2SectorsPerBlock(superblock);
    *allocated = true;

    Ext2WriteWord(raw, 0U, *block);

    return Ext2WriteBytes(device, superblock, table, offset, EXT2_BLOCK_POINTER_SIZE, raw);
}

/* The same for one of the fifteen entries of i_block, which is held in memory
 * rather than upon the volume and so is assigned rather than written. */
static bool Ext2InodePointerAllocate(BlockDevice *device, Ext2Superblock *superblock,
                                     Ext2Inode *inode, uint32_t entry, bool zero,
                                     uint32_t *block, bool *allocated)
{
    if (inode->block[entry] != 0U)
    {
        *block = inode->block[entry];
        return true;
    }

    if (!Ext2AllocateBlock(device, superblock, inode->block[0], block))
    {
        return false;
    }

    if (zero && !Ext2ZeroBlock(device, superblock, *block))
    {
        return false;
    }

    inode->sector_count += Ext2SectorsPerBlock(superblock);
    inode->block[entry] = *block;
    *allocated = true;
    return true;
}

bool Ext2InodeBlockAllocate(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *inode,
                            uint64_t index, uint32_t *block, bool *allocated)
{
    uint64_t per_block;
    uint64_t remaining;
    uint32_t level;
    uint32_t table;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (block == NULL) ||
        (allocated == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no inode, or nowhere to put the block");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    *allocated = false;

    if (index < EXT2_DIRECT_BLOCK_COUNT)
    {
        return Ext2InodePointerAllocate(device, superblock, inode, (uint32_t)index, false,
                                        block, allocated);
    }

    per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    remaining = index - EXT2_DIRECT_BLOCK_COUNT;

    if (remaining < per_block)
    {
        level = 1U;
    }
    else
    {
        remaining -= per_block;

        if (remaining < (per_block * per_block))
        {
            level = 2U;
        }
        else
        {
            remaining -= per_block * per_block;

            if (remaining < (per_block * per_block * per_block))
            {
                level = 3U;
            }
            else
            {
                return Ext2WriteRefuse("the index is beyond what fifteen pointers can address");
            }
        }
    }

    /*
     * The block of pointers named by i_block for this level, allocated and zeroed
     * where the file has not reached this far before.
     */
    if (!Ext2InodePointerAllocate(device, superblock, inode,
                                  EXT2_INDIRECT_INDEX + (level - 1U), true, &table, allocated))
    {
        return false;
    }

    while (level > 0U)
    {
        uint64_t span = 1U;

        for (uint32_t power = 1U; power < level; ++power)
        {
            span *= per_block;
        }

        /* Every block of this walk is a block of pointers save the last, which is
         * the file's own data and is not zeroed: the caller is about to write it,
         * and a caller writing only part of it zeroes the rest itself. */
        if (!Ext2PointerAllocate(device, superblock, inode, table, remaining / span,
                                 level > 1U, &table, allocated))
        {
            return false;
        }

        remaining %= span;
        --level;
    }

    *block = table;
    return true;
}

