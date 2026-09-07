/*
 * File: kernel/test/ext2/file.c
 * Purpose: Asserts the reading of a file's contents: that a range of bytes is
 *          taken from the blocks the inode's pointers name, at every level of
 *          indirection; that a hole reads as zeroes rather than as an error; and
 *          that a symbolic link's target is found in whichever of its two places
 *          the volume put it.
 * Key functions: KernelVerifyExt2Files.
 * References:
 *   - docs/storage/EXT2.md, Section 14, which pairs every assertion here with
 *     the silent failure it catches.
 *   - The Second Extended File System, Dave Poirier, the description of i_block
 *     and the Symbolic Links chapter.
 *
 * Every assertion here is about *which* block was read and not merely that a
 * read succeeded. A reader returning the right number of bytes from the wrong
 * block is indistinguishable from a correct one under a length check or a
 * success check, and reading the wrong block is the failure this chapter is
 * arranged to catch.
 */

#include "internal.h"

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/ext2.h>
#include <oxys/block.h>
#include <oxys/buffer.h>

/*
 * Asserts that the contents of a file are read, that a hole reads as zeroes,
 * that the end of the file is reported by the count rather than as a failure,
 * and that both forms of symbolic link are read and followed.
 *
 * The composed file holds a byte derived from its own offset rather than a
 * constant or a pattern repeating every block. That is deliberate: a reader that
 * returned the right number of bytes from the wrong block would be
 * indistinguishable from a correct one under either of those, and resolving the
 * wrong block is the failure this whole chapter is arranged to catch.
 */
bool KernelVerifyExt2Files(BlockDevice *device, const Ext2Superblock *superblock)
{
    const uint64_t data_offset = (uint64_t)EXT2_DIRECT_BLOCK_COUNT * KERNEL_VOLUME_BLOCK_SIZE;
    const uint64_t hole_offset = data_offset + KERNEL_VOLUME_BLOCK_SIZE;
    char target[EXT2_SYMLINK_MAXIMUM + 1U];
    Ext2Inode inode;
    uint64_t read = 0U;
    bool succeeded = true;

    KernelWriteString("EXT2 files: asserting reading, holes and symbolic links.\n");

    if (!Ext2ResolvePath(device, superblock, "/sub/inner", &inode))
    {
        KernelWriteString("  The composed file was not found: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    /* The whole file, every byte of it, across the boundary between its two
     * blocks and ending part-way through the second. */
    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer,
                      sizeof KernelFileBuffer, &read) ||
        (read != KERNEL_VOLUME_INNER_SIZE) || !KernelFileBufferMatches(0U, read))
    {
        KernelWriteString("  A file was not read as it was composed.\n");
        succeeded = false;
    }

    /*
     * A run crossing the boundary between the two blocks. The first block ends
     * at 1024 and this run begins at 1000, so a reader that took the whole run
     * from one block would return 24 correct bytes and 76 wrong ones.
     */
    if (!Ext2ReadFile(device, superblock, &inode, 1000U, KernelFileBuffer, 100U, &read) ||
        (read != 100U) || !KernelFileBufferMatches(1000U, read))
    {
        KernelWriteString("  A read across a block boundary returned the wrong bytes.\n");
        succeeded = false;
    }

    /* A run wholly within the second block, which begins at an offset the first
     * block does not contain. */
    if (!Ext2ReadFile(device, superblock, &inode, 1100U, KernelFileBuffer, 64U, &read) ||
        (read != 64U) || !KernelFileBufferMatches(1100U, read))
    {
        KernelWriteString("  A read from the second block returned the wrong bytes.\n");
        succeeded = false;
    }

    /*
     * The end of the file. A read that would cross it is shortened to it, and a
     * read beginning at or beyond it yields nothing and succeeds — the end of a
     * file is where every reader arrives, and reporting it as a failure would
     * oblige each of them to treat the conclusion of its work as a fault.
     */
    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE - 100U,
                      KernelFileBuffer, 1000U, &read) ||
        (read != 100U) || !KernelFileBufferMatches(KERNEL_VOLUME_INNER_SIZE - 100U, read))
    {
        KernelWriteString("  A read crossing the end of the file was not shortened to it.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE, KernelFileBuffer,
                      64U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read at the end of the file did not report the end.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, KERNEL_VOLUME_INNER_SIZE + 4096U,
                      KernelFileBuffer, 64U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read beyond the end of the file did not report the end.\n");
        succeeded = false;
    }

    if (!Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 0U, &read) ||
        (read != 0U))
    {
        KernelWriteString("  A read of no length was not answered with no bytes.\n");
        succeeded = false;
    }

    /*
     * A hole reads as zeroes, and the block beside it reads as data. The sparse
     * file holds block 12 and not block 13, and the two are asserted together
     * because a reader that returned zeroes for both, or data for both, would
     * pass either assertion alone.
     */
    if (!Ext2ResolvePath(device, superblock, "/file", &inode))
    {
        KernelWriteString("  The composed sparse file was not found.\n");
        succeeded = false;
    }
    else
    {
        if (!Ext2ReadFile(device, superblock, &inode, data_offset, KernelFileBuffer, 64U,
                          &read) ||
            (read != 64U) || !KernelFileBufferMatches(data_offset, read))
        {
            KernelWriteString("  A block reached through the indirect block read wrongly.\n");
            succeeded = false;
        }

        if (!Ext2ReadFile(device, superblock, &inode, hole_offset, KernelFileBuffer, 64U,
                          &read) ||
            (read != 64U) || !KernelFileBufferIsZero(read))
        {
            KernelWriteString("  A hole did not read as zeroes.\n");
            succeeded = false;
        }
    }

    /* A directory is traversed and not read. */
    if (Ext2ResolvePath(device, superblock, "/sub", &inode) &&
        Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 64U, &read))
    {
        KernelWriteString("  A directory was read as a stream of bytes.\n");
        succeeded = false;
    }

    /* --- The symbolic links. --- */

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast", &inode) ||
        (inode.number != KERNEL_VOLUME_FAST_LINK_INODE) ||
        !Ext2InodeIsFastSymbolicLink(superblock, &inode) ||
        !Ext2ReadSymbolicLink(device, superblock, &inode, target, sizeof target) ||
        !KernelSameString(target, KERNEL_VOLUME_FAST_LINK_TARGET))
    {
        KernelWriteString("  A target held within its inode was not read.\n");
        succeeded = false;
    }

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-slow", &inode) ||
        (inode.number != KERNEL_VOLUME_SLOW_LINK_INODE) ||
        Ext2InodeIsFastSymbolicLink(superblock, &inode) ||
        !Ext2ReadSymbolicLink(device, superblock, &inode, target, sizeof target) ||
        !KernelSameString(target, KERNEL_VOLUME_SLOW_LINK_TARGET))
    {
        KernelWriteString("  A target held in a block was not read.\n");
        succeeded = false;
    }

    /* A target the caller has no room for is refused rather than truncated. */
    if (Ext2ResolvePathNoFollow(device, superblock, "/link-slow", &inode) &&
        Ext2ReadSymbolicLink(device, superblock, &inode, target,
                             sizeof(KERNEL_VOLUME_SLOW_LINK_TARGET) - 1U))
    {
        KernelWriteString("  A target longer than the buffer was accepted.\n");
        succeeded = false;
    }

    /*
     * Resolution through the links. The fast link names a directory by a
     * relative target, so it is resolved against the root, which holds the link;
     * the slow link names a file by an absolute target that walks through the
     * subdirectory eight times before descending into it.
     */
    if (!KernelVerifyExt2PathIs(device, superblock, "/link-fast", KERNEL_VOLUME_SUB_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/link-fast/", KERNEL_VOLUME_SUB_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/link-fast/inner", KERNEL_VOLUME_INNER_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/link-fast/../file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/link-slow", KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A path through a symbolic link did not resolve.\n");
        succeeded = false;
    }

    /*
     * The link itself, rather than what it names. A trailing separator overrides
     * the distinction: a path asserting a directory is asking for what the link
     * names, a link not being one.
     */
    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast", &inode) ||
        (inode.number != KERNEL_VOLUME_FAST_LINK_INODE))
    {
        KernelWriteString("  A last symbolic link was followed when it should not be.\n");
        succeeded = false;
    }

    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast/", &inode) ||
        (inode.number != KERNEL_VOLUME_SUB_INODE))
    {
        KernelWriteString("  A trailing separator did not override the link.\n");
        succeeded = false;
    }

    /* A link within the path is followed whether or not the last one is. */
    if (!Ext2ResolvePathNoFollow(device, superblock, "/link-fast/inner", &inode) ||
        (inode.number != KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A symbolic link within the path was not followed.\n");
        succeeded = false;
    }

    /*
     * A link that names itself. The format permits it — it is a valid file whose
     * contents happen to be its own name — and nothing but a depth bound stops
     * the resolver from following it until the stack is gone.
     */
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_SIZE), 9U);
    KernelStoreText(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCK),
                    "link-fast");
    (void)BufferInvalidateDevice(device);

    if (Ext2ResolvePath(device, superblock, "/link-fast", &inode))
    {
        KernelWriteString("  A symbolic link naming itself was followed to an end.\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);

    /* Nothing may be asked of a null argument. */
    if (Ext2ReadFile(NULL, superblock, &inode, 0U, KernelFileBuffer, 64U, &read) ||
        Ext2ReadFile(device, superblock, NULL, 0U, KernelFileBuffer, 64U, &read) ||
        Ext2ReadFile(device, superblock, &inode, 0U, NULL, 64U, &read) ||
        Ext2ReadFile(device, superblock, &inode, 0U, KernelFileBuffer, 64U, NULL) ||
        Ext2ReadSymbolicLink(device, superblock, &inode, NULL, sizeof target) ||
        Ext2ReadSymbolicLink(device, superblock, &inode, target, 0U) ||
        Ext2ResolvePathNoFollow(device, superblock, NULL, &inode))
    {
        KernelWriteString("  A read accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 files: reading, holes and symbolic links are sound.\n");
    }

    return succeeded;
}

