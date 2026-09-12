/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/storage/ext2/directory.c
 * Purpose: Asserts the directory record and the resolution of a path: that an
 *          entry contradicting the format is refused, that a traversal reads the
 *          entries the volume was composed with rather than merely the right
 *          number of them, and that a path resolves to the inode it names across
 *          symbolic links of both forms.
 * Key functions: KernelVerifyExt2PathIs, KernelVerifyExt2Directories.
 * References:
 *   - docs/storage/EXT2-VERIFICATION.md, which pairs every assertion here with
 *     the silent failure it catches.
 *   - The Second Extended File System, Dave Poirier, the Directory Structure
 *     chapter: the rules a record must satisfy, each of which is made false on
 *     purpose here.
 *
 * The entries are compared by name and by inode number and not counted, because
 * a traversal returning the right number of the wrong entries is exactly the
 * failure a count cannot see.
 */

#include "internal.h"

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include "../../volume.h"
#include <oxys/fs/ext2.h>
#include <oxys/block/block.h>
#include <oxys/block/buffer.h>

/*
 * Alters one field of the composed volume and reports whether a traversal of the
 * root directory refused the result, restoring the volume afterwards.
 *
 * The offset is a byte of the device's storage rather than of a block, because
 * the rules a directory is held to are stated partly by its entries and partly
 * by the inode that owns them, and both must be reachable from one helper.
 *
 * The cache is invalidated on both sides of the alteration for the reason
 * KernelVerifyExt2VolumeRefusedWith gives.
 */
static bool KernelDirectoryRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                       size_t offset, uint32_t value, uint32_t width)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    bool refused = true;

    if (width == 1U)
    {
        KernelMemoryDeviceStore[offset] = (uint8_t)value;
    }
    else if (width == 2U)
    {
        KernelStoreHalf(offset, (uint16_t)value);
    }
    else
    {
        KernelStoreWord(offset, value);
    }

    (void)BufferInvalidateDevice(device);

    if (Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        Ext2DirectoryOpen(&cursor, &root);
        refused = false;

        for (;;)
        {
            const Ext2DirectoryStep step =
                Ext2DirectoryNext(device, superblock, &cursor, &entry);

            if (step == EXT2_DIRECTORY_FAILED)
            {
                refused = true;
                break;
            }

            if (step == EXT2_DIRECTORY_END)
            {
                break;
            }
        }
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/*
 * The same for one byte and a path, for the rules that are not visible until an
 * entry and the inode it names are compared with one another.
 */
static bool KernelPathRefusedWith(BlockDevice *device, const Ext2Superblock *superblock,
                                  size_t offset, uint8_t value, const char *path)
{
    Ext2Inode inode;
    bool refused;

    KernelMemoryDeviceStore[offset] = value;
    (void)BufferInvalidateDevice(device);

    refused = !Ext2ResolvePath(device, superblock, path, &inode);

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return refused;
}

/* Whether a path resolves to the inode expected of it. */
bool KernelVerifyExt2PathIs(BlockDevice *device, const Ext2Superblock *superblock,
                         const char *path, uint32_t expected)
{
    Ext2Inode inode;

    return Ext2ResolvePath(device, superblock, path, &inode) && (inode.number == expected);
}

/* Whether a path is refused, which a path naming nothing must be. */
static bool KernelPathRefused(BlockDevice *device, const Ext2Superblock *superblock,
                              const char *path)
{
    Ext2Inode inode;

    return !Ext2ResolvePath(device, superblock, path, &inode);
}

/*
 * Asserts that the two readings of the two bytes at offset 6 of an entry are
 * distinguished by the incompatible feature flag and not by anything else.
 *
 * This is the one property of the format where the same bytes have two lawful
 * meanings, and where reading the wrong one produces no diagnostic of its own.
 * The entry "." bears a name length of 1 and a file type of EXT2_FT_DIR; read as
 * one sixteen-bit quantity those two bytes are 1 + 256 * 2 = 513, a name that
 * cannot fit within a record of twelve bytes. So the volume that states no file
 * type must refuse the entry the volume that states one accepts, and with the
 * file type byte cleared the same entry must read correctly with no type stated.
 */
static bool KernelVerifyEntryReadings(BlockDevice *device)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Superblock plain;
    Ext2Inode root;
    bool succeeded = true;

    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, 0U);
    (void)BufferInvalidateDevice(device);

    if (Ext2ReadSuperblock(device, &plain) &&
        Ext2ReadInode(device, &plain, EXT2_ROOT_INODE, &root))
    {
        Ext2DirectoryOpen(&cursor, &root);

        if (Ext2DirectoryNext(device, &plain, &cursor, &entry) != EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  A name length was read as eight bits upon a volume that "
                              "states no file type.\n");
            succeeded = false;
        }

        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA) +
                                EXT2_OFFSET_DE_FILE_TYPE] = 0U;
        (void)BufferInvalidateDevice(device);

        if (!Ext2DirectoryFind(device, &plain, &root, ".", 1U, &entry) ||
            (entry.inode != EXT2_ROOT_INODE) ||
            (entry.file_type != (uint8_t)EXT2_FT_UNKNOWN))
        {
            KernelWriteString("  An entry of a volume that states no file type was not "
                              "read.\n");
            succeeded = false;
        }
    }
    else
    {
        KernelWriteString("  A volume stating no file type was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    KernelComposeVolume();
    (void)BufferInvalidateDevice(device);
    return succeeded;
}

/*
 * Asserts that a directory is traversed as the format lays it out, and that a
 * path is resolved to the inode it names.
 *
 * A directory is the first structure of the volume whose contents are variable
 * rather than fixed: a superblock lies at a known offset, a descriptor is 32
 * bytes and an inode is 128, but an entry is as long as its record length says
 * and the next one begins wherever that lands. Every mistake in reading it is
 * therefore self-propagating — one record length taken from the wrong offset, or
 * one entry advanced by the length of its name rather than by its record length,
 * and every entry after it in the block is read from the middle of something
 * else. The names that come out of that are not obviously wrong; they are
 * fragments of real names, and a lookup that fails to find a file that is there
 * is indistinguishable from a file that is not.
 *
 * The traversal is therefore asserted entry by entry against the layout the
 * volume was composed with, and not merely counted.
 */
bool KernelVerifyExt2Directories(BlockDevice *device, const Ext2Superblock *superblock)
{
    static const char *const expected_names[] = {".",   "..",        "file",
                                                "sub", "link-fast", "link-slow"};
    static const uint32_t expected_inodes[] = {
        EXT2_ROOT_INODE,          EXT2_ROOT_INODE,               KERNEL_VOLUME_FILE_INODE,
        KERNEL_VOLUME_SUB_INODE,  KERNEL_VOLUME_FAST_LINK_INODE, KERNEL_VOLUME_SLOW_LINK_INODE};
    static const uint8_t expected_types[] = {
        (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_REG_FILE,
        (uint8_t)EXT2_FT_DIR,     (uint8_t)EXT2_FT_SYMLINK, (uint8_t)EXT2_FT_SYMLINK};
    const size_t expected_count = sizeof(expected_names) / sizeof(expected_names[0]);
    const size_t root_block = KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA);
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    Ext2Inode root;
    Ext2Inode subdirectory;
    size_t counted = 0U;
    bool succeeded = true;

    KernelWriteString("EXT2 directories: asserting traversal and path resolution.\n");

    if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &root))
    {
        KernelWriteString("  The root inode was refused: ");
        KernelWriteString(Ext2LastError());
        KernelWriteString("\n");
        return false;
    }

    /*
     * The root, entry by entry. The unused record standing between "file" and
     * "sub" must be passed over, and the final record, whose length runs to the
     * end of the block, must end the traversal rather than yield an entry.
     */
    Ext2DirectoryOpen(&cursor, &root);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  The root directory could not be traversed: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            break;
        }

        if (counted >= expected_count)
        {
            KernelWriteString("  The root directory yielded more entries than it holds.\n");
            succeeded = false;
            break;
        }

        if (!KernelSameString(entry.name, expected_names[counted]) ||
            (entry.inode != expected_inodes[counted]) ||
            (entry.file_type != expected_types[counted]))
        {
            KernelWriteString("  An entry of the root directory was read wrongly: ");
            KernelWriteString(entry.name);
            KernelWriteString("\n");
            succeeded = false;
        }

        ++counted;
    }

    if (counted != expected_count)
    {
        KernelWriteString("  The root directory did not yield the entries it holds.\n");
        succeeded = false;
    }

    /* A name the directory holds is found by looking for it. */
    if (!Ext2DirectoryFind(device, superblock, &root, "file", 4U, &entry) ||
        (entry.inode != KERNEL_VOLUME_FILE_INODE) || (entry.record_length != 16U) ||
        (entry.block != KERNEL_VOLUME_ROOT_DATA) || (entry.offset != 24U))
    {
        KernelWriteString("  A name the root directory holds was not found where it "
                          "stands.\n");
        succeeded = false;
    }

    /*
     * A name is matched by its whole length and not by a prefix of it. The
     * comparison is given a length rather than a terminator, and one that
     * stopped at the shorter of the two would match "fil" against "file".
     */
    if (Ext2DirectoryFind(device, superblock, &root, "fil", 3U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "files", 5U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "file", 3U, &entry))
    {
        KernelWriteString("  A name was matched against a prefix of another.\n");
        succeeded = false;
    }

    /* The name standing upon the unused record is not a name. */
    if (Ext2DirectoryFind(device, superblock, &root, "removed", 7U, &entry))
    {
        KernelWriteString("  The name upon an unused record was found.\n");
        succeeded = false;
    }

    /*
     * Path resolution. The root is named by the separator alone; repeated
     * separators are one; "." and ".." are resolved as the ordinary entries they
     * are, the ".." of the root naming the root itself; and a path of two
     * components reaches the file within the subdirectory.
     */
    if (!KernelVerifyExt2PathIs(device, superblock, "/", EXT2_ROOT_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "///", EXT2_ROOT_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/.", EXT2_ROOT_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/..", EXT2_ROOT_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/sub", KERNEL_VOLUME_SUB_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/sub/", KERNEL_VOLUME_SUB_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/sub/.", KERNEL_VOLUME_SUB_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/sub/..", EXT2_ROOT_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/sub/../file", KERNEL_VOLUME_FILE_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "/sub/inner", KERNEL_VOLUME_INNER_INODE) ||
        !KernelVerifyExt2PathIs(device, superblock, "//sub///inner", KERNEL_VOLUME_INNER_INODE))
    {
        KernelWriteString("  A path did not resolve to the inode it names.\n");
        succeeded = false;
    }

    /*
     * The refusals. A relative path has nothing to be resolved against; a
     * component that does not exist cannot be traversed; and a component that is
     * not a directory holds no names, whether it stands within the path or is
     * asserted to be a directory by a separator at the end of it.
     */
    if (!KernelPathRefused(device, superblock, "file") ||
        !KernelPathRefused(device, superblock, "") ||
        !KernelPathRefused(device, superblock, "/missing") ||
        !KernelPathRefused(device, superblock, "/sub/missing") ||
        !KernelPathRefused(device, superblock, "/removed") ||
        !KernelPathRefused(device, superblock, "/file/") ||
        !KernelPathRefused(device, superblock, "/file/inner") ||
        !KernelPathRefused(device, superblock, "/sub/inner/"))
    {
        KernelWriteString("  A path that names nothing was resolved.\n");
        succeeded = false;
    }

    /* The subdirectory is a directory, and holds what was composed within it. */
    if (!Ext2ResolvePath(device, superblock, "/sub", &subdirectory) ||
        !Ext2InodeIsDirectory(&subdirectory) ||
        !Ext2DirectoryFind(device, superblock, &subdirectory, "inner", 5U, &entry) ||
        (entry.inode != KERNEL_VOLUME_INNER_INODE) ||
        (entry.file_type != (uint8_t)EXT2_FT_REG_FILE))
    {
        KernelWriteString("  The subdirectory did not hold what was composed within it.\n");
        succeeded = false;
    }

    /* A file is not a directory, and holds no entries whatever its data is. */
    if (Ext2ReadInode(device, superblock, KERNEL_VOLUME_FILE_INODE, &subdirectory))
    {
        Ext2DirectoryOpen(&cursor, &subdirectory);

        if (Ext2DirectoryNext(device, superblock, &cursor, &entry) != EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("  A regular file was traversed as a directory.\n");
            succeeded = false;
        }
    }

    /*
     * A record contradicting the format is refused. Each of these is a rule the
     * traversal depends upon to terminate or to stay within its block: a record
     * length below the header cannot be advanced past; one that is not a multiple
     * of four leaves the next entry unaligned; one reaching beyond the block
     * contradicts the rule that no entry spans two; a name longer than its record
     * would be read out of the entry that follows; an inode number beyond the
     * volume names nothing; and a name holding the separator is reachable by no
     * path.
     */
    if (!KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH, 0U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH, 14U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_RECORD_LENGTH,
                                    KERNEL_VOLUME_BLOCK_SIZE + 4U, 2U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    root_block + EXT2_OFFSET_DE_NAME_LENGTH, 200U, 1U) ||
        !KernelDirectoryRefusedWith(device, superblock, root_block + EXT2_OFFSET_DE_INODE,
                                    superblock->inode_count + 1U, 4U) ||
        !KernelDirectoryRefusedWith(device, superblock, root_block + EXT2_OFFSET_DE_NAME,
                                    (uint32_t)EXT2_PATH_SEPARATOR, 1U))
    {
        KernelWriteString("  A directory entry contradicting the format was accepted.\n");
        succeeded = false;
    }

    /*
     * A directory occupies whole blocks, and holds at least its own entry. A
     * size that is not a multiple of the block size describes a final block
     * ending in the middle of a record.
     */
    if (!KernelDirectoryRefusedWith(device, superblock,
                                    KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE),
                                    KERNEL_VOLUME_BLOCK_SIZE - 24U, 4U) ||
        !KernelDirectoryRefusedWith(device, superblock,
                                    KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE), 0U,
                                    4U))
    {
        KernelWriteString("  A directory whose size cannot be traversed was accepted.\n");
        succeeded = false;
    }

    /*
     * The file type an entry declares must agree with the format of the inode it
     * names. The entry for "file" is made to declare a directory; the inode it
     * names is a regular file, and the path must be refused rather than resolved
     * to a file the caller will then treat as a directory.
     */
    if (!KernelPathRefusedWith(device, superblock, root_block + 24U + EXT2_OFFSET_DE_FILE_TYPE,
                               (uint8_t)EXT2_FT_DIR, "/file"))
    {
        KernelWriteString("  An entry contradicting the inode it names was accepted.\n");
        succeeded = false;
    }

    if (!KernelVerifyEntryReadings(device))
    {
        succeeded = false;
    }

    /* Nothing may be asked of a null argument. */
    if (Ext2ResolvePath(NULL, superblock, "/", &root) ||
        Ext2ResolvePath(device, superblock, NULL, &root) ||
        Ext2ResolvePath(device, superblock, "/", NULL) ||
        Ext2DirectoryFind(device, superblock, NULL, "file", 4U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, NULL, 4U, &entry) ||
        Ext2DirectoryFind(device, superblock, &root, "file", 0U, &entry) ||
        (Ext2DirectoryNext(device, superblock, NULL, &entry) != EXT2_DIRECTORY_FAILED))
    {
        KernelWriteString("  A directory operation accepted a null argument.\n");
        succeeded = false;
    }

    if (succeeded)
    {
        KernelWriteString("EXT2 directories: traversal and path resolution are sound.\n");
    }

    return succeeded;
}

