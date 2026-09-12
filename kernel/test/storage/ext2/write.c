/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/storage/ext2/write.c
 * Purpose: Asserts everything that alters a volume: the allocation of blocks and
 *          inodes from the two bitmaps, the writing and extension of a file, its
 *          truncation, and the insertion and removal of the names by which a
 *          file is reached — together with the summaries the superblock and the
 *          group descriptors keep, which must agree with the bitmaps after each.
 * Key functions: KernelRestoreVolume, KernelVerifyExt2Writes,
 *          KernelVerifyExt2DirectoryWrites.
 * References:
 *   - docs/storage/EXT2-VERIFICATION.md, which pairs every assertion here with
 *     the silent failure it catches.
 *   - The Second Extended File System, Dave Poirier, the Block Bitmap, Inode
 *     Bitmap and Block Group Descriptor chapters: the summaries are not derived
 *     from the bitmaps at read time, so an allocator that altered one without
 *     the other leaves a volume every tool outside this kernel judges corrupt.
 *
 * These are the assertions that must begin from a known volume, because each of
 * them leaves a different one behind. KernelRestoreVolume composes the fixture
 * afresh between them, and is shared with the entry point for that reason.
 */

#include "internal.h"

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include "../../volume.h"
#include <oxys/fs/ext2.h>
#include <oxys/block/block.h>
#include <oxys/block/buffer.h>

/*
 * Restores the composed volume after something has written to it.
 *
 * The order matters and is not the order used everywhere else in this file.
 * BufferInvalidateDevice writes dirty buffers back before it discards them, so
 * composing first and invalidating afterwards would flush the writes of the test
 * just finished onto the volume just composed — restoring nothing and leaving a
 * volume that is neither what was written nor what was composed. The cache is
 * therefore emptied first, and the composition follows it.
 *
 * Every self-test before sub-task 5.6 could use the other order safely, none of
 * them having left a dirty buffer behind.
 */
void KernelRestoreVolume(BlockDevice *device)
{
    (void)BufferInvalidateDevice(device);
    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
}

/*
 * Asserts that a volume may be altered, and that it still describes itself
 * afterwards.
 *
 * This is the first self-test in the project that writes to a filesystem, and
 * the standard it is held to differs from every one before it. A read that goes
 * wrong returns the wrong bytes to one caller; a write that goes wrong destroys
 * data and cannot be undone, and the destruction is ordinarily silent — a block
 * allocated to two files reads correctly for both of them until one of them
 * writes.
 *
 * Two things follow. The assertions are made about the volume as a whole and not
 * only about the operation performed: after every sequence below, the free counts
 * of the group and of the superblock must agree with one another and with what
 * was actually taken, which is the statement a corrupted allocator cannot
 * satisfy. And every write here is made to the device of memory, never to a disk
 * the machine carries: the volumes upon those belong to whoever booted this
 * kernel.
 */
bool KernelVerifyExt2Writes(BlockDevice *device, Ext2Superblock *superblock)
{
    const uint64_t indirect_base = EXT2_DIRECT_BLOCK_COUNT + KERNEL_VOLUME_POINTERS;
    const uint64_t double_base = indirect_base + (KERNEL_VOLUME_POINTERS *
                                                  KERNEL_VOLUME_POINTERS);
    Ext2Superblock reread;
    Ext2GroupDescriptor descriptor;
    Ext2Inode inode;
    uint32_t free_blocks;
    uint32_t block = 0U;
    uint32_t number = 0U;
    uint64_t moved = 0U;
    bool used = false;
    bool succeeded = true;

    KernelWriteString("EXT2 writes: asserting allocation, writing and truncation.\n");

    /* --- The bitmaps, read against the composition. --- */

    if (!Ext2BlockInUse(device, superblock, KERNEL_VOLUME_INODE_TABLE, &used) || !used ||
        !Ext2BlockInUse(device, superblock, KERNEL_VOLUME_LAST_BLOCK, &used) || used ||
        !Ext2InodeInUse(device, superblock, EXT2_ROOT_INODE, &used) || !used ||
        !Ext2InodeInUse(device, superblock, KERNEL_VOLUME_UNUSED_INODE, &used) || used ||
        !Ext2InodeInUse(device, superblock, KERNEL_VOLUME_SLOW_LINK_INODE, &used) || !used)
    {
        KernelWriteString("  A bitmap did not report the volume as it was composed.\n");
        succeeded = false;
    }

    /* --- One block, allocated and returned. --- */

    free_blocks = superblock->free_block_count;

    if (!Ext2AllocateBlock(device, superblock, 0U, &block) ||
        !Ext2BlockInUse(device, superblock, block, &used) || !used ||
        (superblock->free_block_count != (free_blocks - 1U)))
    {
        KernelWriteString("  A block was not allocated, or was not then in use.\n");
        succeeded = false;
    }
    else
    {
        /*
         * The superblock upon the volume, and not the copy in memory. An
         * allocator that decremented its own structure and did not write it back
         * would satisfy every assertion made against memory and would leave the
         * volume claiming a block it had given away.
         */
        if (!Ext2ReadSuperblock(device, &reread) ||
            (reread.free_block_count != superblock->free_block_count) ||
            !Ext2ReadGroupDescriptor(device, superblock, 0U, &descriptor) ||
            (descriptor.free_block_count != superblock->free_block_count))
        {
            KernelWriteString("  An allocation was not written back to the volume.\n");
            succeeded = false;
        }

        if (!Ext2FreeBlock(device, superblock, block) ||
            !Ext2BlockInUse(device, superblock, block, &used) || used ||
            (superblock->free_block_count != free_blocks))
        {
            KernelWriteString("  A block was not returned to the volume.\n");
            succeeded = false;
        }
    }

    /* Freeing what is already free is refused: the second free is what allows a
     * block to be given to two files at once. */
    if (Ext2FreeBlock(device, superblock, block) ||
        Ext2FreeInode(device, superblock, KERNEL_VOLUME_UNUSED_INODE, false))
    {
        KernelWriteString("  Something already free was freed a second time.\n");
        succeeded = false;
    }

    /* --- The one free inode, and the exhaustion after it. --- */

    {
        /*
         * Every free inode of the volume, taken until there are none, and then
         * returned. Exhausting the volume rather than allocating one is what
         * asserts that the free count and the bitmap describe the same set: an
         * allocator that miscounted would either stop early, leaving inodes the
         * bitmap says are free, or run past the count and issue one twice.
         */
        uint32_t taken[KERNEL_VOLUME_INODES];
        uint32_t count = 0U;
        const uint32_t available = superblock->free_inode_count;

        while ((count < KERNEL_VOLUME_INODES) &&
               Ext2AllocateInode(device, superblock, false, &number))
        {
            taken[count] = number;
            ++count;
        }

        if ((count != available) || (superblock->free_inode_count != 0U))
        {
            KernelWriteString("  The free inodes of the volume were not all issued.\n");
            succeeded = false;
        }

        /* The lowest free inode is issued first, which is inode 14: the one the
         * self-test of sub-task 5.3 requires to be empty. */
        if ((count == 0U) || (taken[0] != KERNEL_VOLUME_UNUSED_INODE))
        {
            KernelWriteString("  The lowest free inode was not the first issued.\n");
            succeeded = false;
        }

        if (Ext2AllocateInode(device, superblock, false, &number))
        {
            KernelWriteString("  An inode was allocated from a volume holding none.\n");
            succeeded = false;
        }

        for (uint32_t index = 0U; index < count; ++index)
        {
            if (!Ext2InodeInUse(device, superblock, taken[index], &used) || !used ||
                !Ext2FreeInode(device, superblock, taken[index], false))
            {
                KernelWriteString("  An inode was not returned to the volume.\n");
                succeeded = false;
                break;
            }
        }

        if (superblock->free_inode_count != available)
        {
            KernelWriteString("  The inodes returned did not restore the free count.\n");
            succeeded = false;
        }
    }

    /* An inode belonging to the filesystem is never issued and never freed. */
    if (Ext2FreeInode(device, superblock, EXT2_ROOT_INODE, true))
    {
        KernelWriteString("  A reserved inode was freed.\n");
        succeeded = false;
    }

    /* --- Writing within a file that already has the blocks. --- */

    if (!Ext2ResolvePath(device, superblock, "/sub/inner", &inode))
    {
        KernelWriteString("  The composed file was not found.\n");
        return false;
    }

    for (uint32_t index = 0U; index < 128U; ++index)
    {
        KernelFileBuffer[index] = (uint8_t)(0xA0U + (index & 0x0FU));
    }

    if (!Ext2WriteFile(device, superblock, &inode, 100U, KernelFileBuffer, 128U, &moved) ||
        (moved != 128U) || (inode.size != KERNEL_VOLUME_INNER_SIZE))
    {
        KernelWriteString("  A write within a file did not write what it was given.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 100U, KernelFileBuffer, 128U, &moved) ||
        (moved != 128U))
    {
        KernelWriteString("  A file could not be read after being written.\n");
        succeeded = false;
    }
    else
    {
        for (uint32_t index = 0U; index < 128U; ++index)
        {
            if (KernelFileBuffer[index] != (uint8_t)(0xA0U + (index & 0x0FU)))
            {
                KernelWriteString("  A write did not reach the volume.\n");
                succeeded = false;
                break;
            }
        }
    }

    /* The bytes on either side of the write are untouched. */
    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 100U, &moved) ||
        !KernelFileBufferMatches(0U, moved) ||
        !Ext2ReadFile(device, superblock, &inode, 228U, KernelFileBuffer, 100U, &moved) ||
        !KernelFileBufferMatches(228U, moved))
    {
        KernelWriteString("  A write altered bytes beyond the range it was given.\n");
        succeeded = false;
    }

    /* --- Conservation: what a file gives up, it takes back. --- */

    free_blocks = superblock->free_block_count;

    if (!Ext2TruncateFile(device, superblock, &inode, 0U) || (inode.size != 0U) ||
        (inode.sector_count != 0U) || (superblock->free_block_count != (free_blocks + 2U)))
    {
        KernelWriteString("  Truncation to nothing did not return the file's blocks.\n");
        succeeded = false;
    }

    for (uint64_t index = 0U; index < KERNEL_VOLUME_INNER_SIZE; ++index)
    {
        KernelFileBuffer[index] = KernelFileByteAt(index);
    }

    if (!Ext2WriteFile(device, superblock, &inode, 0U, KernelFileBuffer,
                       KERNEL_VOLUME_INNER_SIZE, &moved) ||
        (moved != KERNEL_VOLUME_INNER_SIZE) || (inode.size != KERNEL_VOLUME_INNER_SIZE) ||
        (superblock->free_block_count != free_blocks))
    {
        KernelWriteString("  Rewriting a truncated file did not restore the volume.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer,
                      KERNEL_VOLUME_INNER_SIZE, &moved) ||
        (moved != KERNEL_VOLUME_INNER_SIZE) || !KernelFileBufferMatches(0U, moved))
    {
        KernelWriteString("  A file rewritten after truncation did not read back.\n");
        succeeded = false;
    }

    /* --- Extension, and the hole a write beyond the end leaves. --- */

    if (!Ext2WriteFile(device, superblock, &inode, 4096U, KernelFileBuffer, 16U, &moved) ||
        (moved != 16U) || (inode.size != (4096U + 16U)))
    {
        KernelWriteString("  A write beyond the end did not extend the file.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 2048U, KernelFileBuffer, 512U, &moved) ||
        (moved != 512U) || !KernelFileBufferIsZero(moved))
    {
        KernelWriteString("  The hole left by an extending write did not read as zeroes.\n");
        succeeded = false;
    }

    /* Truncation upward allocates nothing: the file grows by a hole. */
    free_blocks = superblock->free_block_count;

    if (!Ext2TruncateFile(device, superblock, &inode, 1U << 20) ||
        (inode.size != (1U << 20)) || (superblock->free_block_count != free_blocks))
    {
        KernelWriteString("  Truncation upward allocated blocks it need not have.\n");
        succeeded = false;
    }

    /* --- Allocation through the indirection. --- */

    if (!Ext2ResolvePath(device, superblock, "/file", &inode))
    {
        KernelWriteString("  The composed sparse file was not found.\n");
        succeeded = false;
    }
    else
    {
        /*
         * An entry of the doubly indirect block that holds nothing, so that both
         * an indirect block and a data block must be allocated to reach it. Two
         * blocks, and the difference between one and two is the whole of whether
         * a level of the walk was allocated or silently skipped.
         */
        const uint64_t offset = (double_base + KERNEL_VOLUME_POINTERS) *
                                (uint64_t)KERNEL_VOLUME_BLOCK_SIZE;

        free_blocks = superblock->free_block_count;
        KernelFileBuffer[0] = 0x5AU;

        if (!Ext2WriteFile(device, superblock, &inode, offset, KernelFileBuffer, 1U, &moved) ||
            (moved != 1U) || (superblock->free_block_count != (free_blocks - 2U)))
        {
            KernelWriteString("  A write through the indirection did not allocate a chain.\n");
            succeeded = false;
        }

        KernelFileBuffer[0] = 0U;

        if (!Ext2ReadFile(device, superblock, &inode, offset, KernelFileBuffer, 1U, &moved) ||
            (moved != 1U) || (KernelFileBuffer[0] != 0x5AU))
        {
            KernelWriteString("  A byte written through the indirection did not read back.\n");
            succeeded = false;
        }
    }

    /* --- The volume still describes itself. --- */

    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  The volume no longer accounts for itself: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* --- A volume that may not be written is not written. --- */

    KernelRestoreVolume(device);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadSuperblock(device, &reread) && reread.read_only)
    {
        Ext2Inode victim;

        if (Ext2AllocateBlock(device, &reread, 0U, &block) ||
            Ext2AllocateInode(device, &reread, false, &number) ||
            Ext2FreeBlock(device, &reread, KERNEL_VOLUME_LAST_BLOCK) ||
            Ext2WriteSuperblock(device, &reread) ||
            (Ext2ReadInode(device, &reread, KERNEL_VOLUME_INNER_INODE, &victim) &&
             (Ext2WriteInode(device, &reread, &victim) ||
              Ext2WriteFile(device, &reread, &victim, 0U, KernelFileBuffer, 16U, &moved) ||
              Ext2TruncateFile(device, &reread, &victim, 0U))))
        {
            KernelWriteString("  A read-only volume was altered.\n");
            succeeded = false;
        }
    }
    else
    {
        KernelWriteString("  A volume not cleanly unmounted was not made read-only.\n");
        succeeded = false;
    }

    KernelRestoreVolume(device);

    /* Nothing may be asked of a null argument. */
    if (Ext2AllocateBlock(NULL, superblock, 0U, &block) ||
        Ext2AllocateBlock(device, superblock, 0U, NULL) ||
        Ext2AllocateInode(device, NULL, false, &number) ||
        Ext2WriteInode(device, superblock, NULL) ||
        Ext2WriteFile(device, superblock, NULL, 0U, KernelFileBuffer, 16U, &moved) ||
        Ext2TruncateFile(device, superblock, NULL, 0U) ||
        Ext2BlockInUse(device, superblock, KERNEL_VOLUME_LAST_BLOCK, NULL))
    {
        KernelWriteString("  A write accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 writes: allocation, writing and truncation are sound.\n");
    }

    return succeeded;
}

/* How many entries a directory holds, for an assertion about a whole directory
 * rather than about one name within it. */
static bool KernelCountEntries(BlockDevice *device, const Ext2Superblock *superblock,
                               const Ext2Inode *directory, uint64_t *count)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;

    *count = 0U;
    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            return true;
        }

        ++*count;
    }
}

/*
 * Asserts that names may be inserted into a directory and removed from it, and
 * that files and directories may be created and destroyed.
 *
 * A directory is a linked list of records within each of its blocks, and every
 * operation here is an alteration of that list. The failures are accordingly the
 * failures of a list: a record whose length no longer reaches the next one, two
 * records overlapping, a record left in use that nothing points past. None of
 * them is visible in the operation that caused it — the directory reads
 * correctly until the traversal reaches the record that was damaged — so the
 * assertions are made by traversing the whole directory afterwards and counting
 * what comes out, and by requiring the volume to account for itself at the end.
 */
bool KernelVerifyExt2DirectoryWrites(BlockDevice *device, Ext2Superblock *superblock)
{
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    Ext2Inode made;
    Ext2Inode found;
    uint64_t count = 0U;
    uint64_t before = 0U;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint16_t root_links;
    bool empty = false;
    bool succeeded = true;

    KernelWriteString("EXT2 names: asserting insertion, removal and creation.\n");

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root) ||
        !KernelCountEntries(device, superblock, &root, &before))
    {
        KernelWriteString("  The root directory could not be read.\n");
        return false;
    }

    free_blocks = superblock->free_block_count;
    free_inodes = superblock->free_inode_count;
    root_links = root.link_count;

    /* --- A name inserted, found, and removed. --- */

    if (!Ext2DirectoryInsert(device, superblock, &root, "inserted", 8U,
                             KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  A name could not be inserted: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    if (!Ext2DirectoryFind(device, superblock, &root, "inserted", 8U, &entry) ||
        (entry.inode != KERNEL_VOLUME_FILE_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/inserted", KERNEL_VOLUME_FILE_INODE))
    {
        KernelWriteString("  An inserted name was not found by looking for it.\n");
        succeeded = false;
    }

    /*
     * The whole directory, traversed. An insertion that split a record wrongly
     * leaves the records after it unreachable or overlapping, and neither shows
     * in the name just inserted — only in the count of everything.
     */
    if (!KernelCountEntries(device, superblock, &root, &count) || (count != (before + 1U)))
    {
        KernelWriteString("  The directory no longer yields the entries it holds.\n");
        succeeded = false;
    }

    /* A name already present is refused, a directory holding one name twice
     * making the path to it ambiguous. */
    if (Ext2DirectoryInsert(device, superblock, &root, "inserted", 8U, KERNEL_VOLUME_SUB_INODE,
                            (uint8_t)EXT2_FT_DIR) ||
        Ext2DirectoryInsert(device, superblock, &root, "file", 4U, KERNEL_VOLUME_FILE_INODE,
                            (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  A name already present was inserted a second time.\n");
        succeeded = false;
    }

    if (!Ext2DirectoryRemove(device, superblock, &root, "inserted", 8U) ||
        Ext2DirectoryFind(device, superblock, &root, "inserted", 8U, &entry) ||
        !KernelCountEntries(device, superblock, &root, &count) || (count != before))
    {
        KernelWriteString("  A name was not removed, or the directory did not recover.\n");
        succeeded = false;
    }

    /* Removing what is not there, and removing what may not be removed. */
    if (Ext2DirectoryRemove(device, superblock, &root, "inserted", 8U) ||
        Ext2DirectoryRemove(device, superblock, &root, ".", 1U) ||
        Ext2DirectoryRemove(device, superblock, &root, "..", 2U))
    {
        KernelWriteString("  A name that may not be removed was removed.\n");
        succeeded = false;
    }

    /*
     * The space a removal leaves is reused rather than the directory growing.
     * Inserting and removing the same name many times over must not consume a
     * block: the record before the removed one absorbs its space, and the next
     * insertion splits it again.
     */
    for (uint32_t attempt = 0U; attempt < 64U; ++attempt)
    {
        if (!Ext2DirectoryInsert(device, superblock, &root, "recycled", 8U,
                                 KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE) ||
            !Ext2DirectoryRemove(device, superblock, &root, "recycled", 8U))
        {
            KernelWriteString("  A name could not be inserted and removed repeatedly.\n");
            succeeded = false;
            break;
        }
    }

    if (superblock->free_block_count != free_blocks)
    {
        KernelWriteString("  Repeated insertion and removal consumed blocks.\n");
        succeeded = false;
    }

    /* --- A file created, written, linked and destroyed. --- */

    if (!Ext2CreateFile(device, superblock, &root, "created", 7U,
                        (uint16_t)(EXT2_S_IFREG | 0x01A4U), &made) ||
        !Ext2InodeIsRegular(&made) || (made.link_count != 1U) || (made.size != 0U) ||
        (superblock->free_inode_count != (free_inodes - 1U)))
    {
        KernelWriteString("  A file was not created.\n");
        succeeded = false;
    }
    else
    {
        uint64_t moved = 0U;

        KernelFileBuffer[0] = 0x11U;
        KernelFileBuffer[1] = 0x22U;

        if (!Ext2WriteFile(device, superblock, &made, 0U, KernelFileBuffer, 2U, &moved) ||
            (moved != 2U) ||
            !KernelVerifyExt2PathIs(device, superblock, "/created", made.number))
        {
            KernelWriteString("  A created file could not be written or reached.\n");
            succeeded = false;
        }

        /* A second name for the same file, and the count that records it. */
        if (!Ext2Link(device, superblock, &root, "linked", 6U, &made) ||
            (made.link_count != 2U) ||
            !KernelVerifyExt2PathIs(device, superblock, "/linked", made.number))
        {
            KernelWriteString("  A file was not given a second name.\n");
            succeeded = false;
        }

        /*
         * Removing one of two names removes the name and not the file. An unlink
         * that destroyed the file here would leave the other name leading to an
         * inode that had been freed, and quite possibly reissued.
         */
        if (!Ext2Unlink(device, superblock, &root, "created", 7U) ||
            !Ext2ReadInode(device, superblock, made.number, &found) ||
            (found.link_count != 1U) ||
            !KernelVerifyExt2PathIs(device, superblock, "/linked", made.number))
        {
            KernelWriteString("  Removing one of two names destroyed the file.\n");
            succeeded = false;
        }

        if (!Ext2Unlink(device, superblock, &root, "linked", 6U) ||
            (superblock->free_inode_count != free_inodes) ||
            (superblock->free_block_count != free_blocks))
        {
            KernelWriteString("  Removing the last name did not destroy the file.\n");
            succeeded = false;
        }

        /*
         * The inode is free in the bitmap and is refused as a deleted file. Both
         * are asserted: the bitmap is what lets it be issued again, and the
         * refusal is what stops anything reaching it in the meantime.
         */
        if (!Ext2InodeInUse(device, superblock, made.number, &empty) || empty ||
            Ext2ReadInode(device, superblock, made.number, &found))
        {
            KernelWriteString("  An inode freed with its last name was still in use.\n");
            succeeded = false;
        }
    }

    /* --- A directory created and removed. --- */

    if (!Ext2CreateDirectory(device, superblock, &root, "made", 4U, 0x01EDU, &made) ||
        !Ext2InodeIsDirectory(&made) || (made.link_count != 2U) ||
        (made.size != KERNEL_VOLUME_BLOCK_SIZE) || (root.link_count != (root_links + 1U)))
    {
        KernelWriteString("  A directory was not created with its two links.\n");
        succeeded = false;
    }
    else
    {
        /*
         * The two entries every directory holds, resolved by the ordinary lookup
         * rather than assumed. A directory whose ".." named the wrong inode would
         * be reachable and would lead out of itself to somewhere else.
         */
        if (!KernelVerifyExt2PathIs(device, superblock, "/made", made.number) ||
            !KernelVerifyExt2PathIs(device, superblock, "/made/.", made.number) ||
            !KernelVerifyExt2PathIs(device, superblock, "/made/..", EXT2_ROOT_INODE) ||
            !KernelVerifyExt2PathIs(device, superblock, "/made/../made", made.number))
        {
            KernelWriteString("  A created directory did not hold \".\" and \"..\".\n");
            succeeded = false;
        }

        if (!Ext2DirectoryIsEmpty(device, superblock, &made, &empty) || !empty)
        {
            KernelWriteString("  A newly created directory was not empty.\n");
            succeeded = false;
        }

        /* A directory holding something is not removed. */
        if (!Ext2CreateFile(device, superblock, &made, "within", 6U,
                            (uint16_t)(EXT2_S_IFREG | 0x01A4U), &found) ||
            !Ext2DirectoryIsEmpty(device, superblock, &made, &empty) || empty)
        {
            KernelWriteString("  A directory holding a file reported itself empty.\n");
            succeeded = false;
        }

        if (Ext2RemoveDirectory(device, superblock, &root, "made", 4U))
        {
            KernelWriteString("  A directory holding a file was removed.\n");
            succeeded = false;
        }

        /* A directory may not be unlinked as a file, nor given a second name. */
        if (Ext2Unlink(device, superblock, &root, "made", 4U) ||
            Ext2Link(device, superblock, &root, "second", 6U, &made))
        {
            KernelWriteString("  A directory was treated as a file.\n");
            succeeded = false;
        }

        if (!Ext2Unlink(device, superblock, &made, "within", 6U) ||
            !Ext2RemoveDirectory(device, superblock, &root, "made", 4U) ||
            (root.link_count != root_links))
        {
            KernelWriteString("  An emptied directory was not removed.\n");
            succeeded = false;
        }
    }

    /* The root may never be removed, whatever it is named by. */
    if (Ext2RemoveDirectory(device, superblock, &root, ".", 1U))
    {
        KernelWriteString("  The root directory was removed.\n");
        succeeded = false;
    }

    /* --- Everything taken has been given back. --- */

    if ((superblock->free_block_count != free_blocks) ||
        (superblock->free_inode_count != free_inodes) ||
        !KernelCountEntries(device, superblock, &root, &count) || (count != before))
    {
        KernelWriteString("  The volume did not return to what it was.\n");
        succeeded = false;
    }

    if (!Ext2VerifyGroupDescriptors(device, superblock))
    {
        KernelWriteString("  The volume no longer accounts for itself: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    /* --- A read-only volume holds its names. --- */

    KernelRestoreVolume(device);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_ERROR_FS);
    (void)BufferInvalidateDevice(device);

    {
        Ext2Superblock frozen;

        if (Ext2ReadSuperblock(device, &frozen) && frozen.read_only &&
            Ext2ReadInode(device, &frozen, EXT2_ROOT_INODE, &found))
        {
            if (Ext2DirectoryInsert(device, &frozen, &found, "no", 2U,
                                    KERNEL_VOLUME_FILE_INODE, (uint8_t)EXT2_FT_REG_FILE) ||
                Ext2DirectoryRemove(device, &frozen, &found, "file", 4U) ||
                Ext2CreateFile(device, &frozen, &found, "no", 2U,
                               (uint16_t)(EXT2_S_IFREG | 0x01A4U), &made) ||
                Ext2CreateDirectory(device, &frozen, &found, "no", 2U, 0x01EDU, &made) ||
                Ext2Unlink(device, &frozen, &found, "file", 4U) ||
                Ext2RemoveDirectory(device, &frozen, &found, "sub", 3U))
            {
                KernelWriteString("  A read-only volume had its names altered.\n");
                succeeded = false;
            }
        }
        else
        {
            KernelWriteString("  A volume not cleanly unmounted was not made read-only.\n");
            succeeded = false;
        }
    }

    KernelRestoreVolume(device);

    /* Nothing may be asked of a null argument, or of a name that is not one. */
    if (Ext2DirectoryInsert(device, superblock, NULL, "x", 1U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, NULL, 1U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 0U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "a/b", 3U, KERNEL_VOLUME_FILE_INODE, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 1U, 0U, 0U) ||
        Ext2DirectoryInsert(device, superblock, &root, "x", 1U, superblock->inode_count + 1U,
                            0U) ||
        Ext2DirectoryRemove(device, superblock, NULL, "x", 1U) ||
        Ext2CreateFile(device, superblock, &root, "d", 1U, (uint16_t)EXT2_S_IFDIR, &made) ||
        Ext2CreateDirectory(device, superblock, NULL, "x", 1U, 0x01EDU, &made) ||
        Ext2DirectoryIsEmpty(device, superblock, &root, NULL))
    {
        KernelWriteString("  A directory operation accepted what it should refuse.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 names: insertion, removal and creation are sound.\n");
    }

    return succeeded;
}

