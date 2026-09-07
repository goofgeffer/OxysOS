/*
 * File: kernel/fs/ext2/directory.c
 * Purpose: Implements the record a directory is made of: the correspondence
 *          between the format's file types and the modes of an inode, the
 *          decoding and validation of one entry, the traversal of a directory's
 *          records, and the search for a name among them.
 * Key functions: Ext2FileTypeOfMode, Ext2FileTypeName, Ext2VolumeStatesFileType,
 *          Ext2ReadEntryHeader, Ext2ReadEntryName, Ext2DirectoryOpen,
 *          Ext2DirectoryNext, Ext2DirectoryFind, Ext2EntriesRead,
 *          Ext2EntriesRefused, Ext2ReportDirectoryEntry, Ext2ReportDirectory.
 *
 *          Turning a *path* into an inode is `path.c`; creating and destroying
 *          the names themselves is `name.c`. This file is the format they both
 *          read and write through, and the three entry-decoding routines above
 *          are declared in `internal.h` for that reason.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Directory Structure chapter and Table 4.1: a directory is a file whose
 *     data is a linked list of entries; the inode number lies at offset 0 and is
 *     zero where the entry is not in use, the record length at 4, the name
 *     length at 6 and the file type at 7, the name following at 8. A record
 *     length must be at least the length of its record, must be a multiple of
 *     four, and no entry may span two blocks; the name length may never exceed
 *     the record length less eight.
 *   - The same, Table 4.2: the eight values a file type may take, which are not
 *     numbered as the formats of i_mode are and must agree with them.
 *   - The same, the rec_len note: where a record is removed the record before it
 *     is lengthened to cover it, and where the first record of a block is
 *     removed a blank record is left in its place.
 *   - Linux kernel documentation, filesystems/ext2.rst: the file type is an
 *     incompatible feature because a kernel unaware of it would read the name
 *     length as sixteen bits; which of the two readings applies is therefore a
 *     property of the feature flag and not of the revision.
 */

#include "internal.h"

#include <oxys/buffer.h>
#include <oxys/kernel.h>

uint8_t Ext2FileTypeOfMode(uint16_t mode)
{
    switch (mode & EXT2_S_IFMT)
    {
    case EXT2_S_IFREG:
        return (uint8_t)EXT2_FT_REG_FILE;
    case EXT2_S_IFDIR:
        return (uint8_t)EXT2_FT_DIR;
    case EXT2_S_IFCHR:
        return (uint8_t)EXT2_FT_CHRDEV;
    case EXT2_S_IFBLK:
        return (uint8_t)EXT2_FT_BLKDEV;
    case EXT2_S_IFIFO:
        return (uint8_t)EXT2_FT_FIFO;
    case EXT2_S_IFSOCK:
        return (uint8_t)EXT2_FT_SOCK;
    case EXT2_S_IFLNK:
        return (uint8_t)EXT2_FT_SYMLINK;
    default:
        return (uint8_t)EXT2_FT_UNKNOWN;
    }
}

const char *Ext2FileTypeName(uint8_t type)
{
    switch (type)
    {
    case EXT2_FT_REG_FILE:
        return "regular file";
    case EXT2_FT_DIR:
        return "directory";
    case EXT2_FT_CHRDEV:
        return "character device";
    case EXT2_FT_BLKDEV:
        return "block device";
    case EXT2_FT_FIFO:
        return "fifo";
    case EXT2_FT_SOCK:
        return "socket";
    case EXT2_FT_SYMLINK:
        return "symbolic link";
    default:
        return "of no stated type";
    }
}

/*
 * Whether the volume states a file type in its directory entries.
 *
 * The two bytes at offset 6 are either a name length of eight bits followed by a
 * file type, or a name length of sixteen bits. Which of the two a volume holds
 * is stated by the incompatible feature flag and by nothing else — not by the
 * revision, a revision 1 volume being free to omit the feature. Reading the
 * wrong one of the two is not a subtle error: a volume without the feature,
 * read as though it had it, gives every entry a file type equal to the high byte
 * of its name length, which is zero, and so declares every file to be of no
 * stated type. Read the other way about, a name of three bytes becomes a name of
 * 3 + 256 * EXT2_FT_DIR bytes and the entry is refused.
 */
bool Ext2VolumeStatesFileType(const Ext2Superblock *superblock)
{
    return (superblock->feature_incompatible & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0U;
}

/*
 * Whether a directory's data may be traversed at all.
 *
 * A directory occupies whole blocks: the record length of the last entry of a
 * block runs to the end of that block, so a size that is not a multiple of the
 * block size describes a directory whose final block ends in the middle of an
 * entry. A directory of no size is refused likewise — every directory holds at
 * least its own entry and its parent's — and both refusals catch an inode that
 * is not really a directory long before its bytes are interpreted as entries.
 */
static bool Ext2DirectoryTraversable(const Ext2Superblock *superblock,
                                     const Ext2Inode *directory)
{
    if (!Ext2InodeIsDirectory(directory))
    {
        return Ext2EntryRefuse("the inode is not a directory");
    }

    if (directory->size == 0U)
    {
        return Ext2EntryRefuse("a directory of no size holds not even its own entry");
    }

    if ((directory->size % (uint64_t)superblock->block_size) != 0U)
    {
        return Ext2EntryRefuse("a directory's size is not a whole number of blocks");
    }

    return true;
}

/*
 * Reads and validates the eight-byte header of the entry standing at an offset
 * within a block, leaving the name unread.
 *
 * Every rule the specification states about a record is applied here, because
 * every one of them is what keeps the traversal from walking off the block or
 * looping upon itself: a record length below the header cannot be advanced past,
 * one that is not a multiple of four leaves the next entry unaligned, and one
 * that reaches beyond the block contradicts the rule that no entry spans two.
 */
bool Ext2ReadEntryHeader(BlockDevice *device, const Ext2Superblock *superblock,
                                uint32_t block, uint32_t offset, Ext2DirectoryEntry *entry)
{
    uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

    if (!Ext2ReadBytes(device, superblock, block, offset, EXT2_DIRECTORY_HEADER_SIZE, raw))
    {
        return false;
    }

    entry->inode = Ext2ReadWord(raw, EXT2_OFFSET_DE_INODE);
    entry->record_length = Ext2ReadHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH);

    if (Ext2VolumeStatesFileType(superblock))
    {
        entry->name_length = (uint16_t)raw[EXT2_OFFSET_DE_NAME_LENGTH];
        entry->file_type = raw[EXT2_OFFSET_DE_FILE_TYPE];
    }
    else
    {
        entry->name_length = Ext2ReadHalf(raw, EXT2_OFFSET_DE_NAME_LENGTH);
        entry->file_type = (uint8_t)EXT2_FT_UNKNOWN;
    }

    entry->block = block;
    entry->offset = offset;

    if (entry->record_length < EXT2_DIRECTORY_HEADER_SIZE)
    {
        return Ext2EntryRefuse("a directory entry is shorter than its own header");
    }

    if ((entry->record_length % EXT2_DIRECTORY_ALIGNMENT) != 0U)
    {
        return Ext2EntryRefuse("a directory entry is not a multiple of four bytes long");
    }

    if (entry->record_length > (superblock->block_size - offset))
    {
        return Ext2EntryRefuse("a directory entry reaches beyond the block that holds it");
    }

    if (entry->name_length > (entry->record_length - EXT2_DIRECTORY_HEADER_SIZE))
    {
        return Ext2EntryRefuse("a directory entry's name does not fit within it");
    }

    if (entry->name_length > EXT2_NAME_MAXIMUM)
    {
        return Ext2EntryRefuse("a directory entry's name is longer than the format permits");
    }

    /*
     * An entry in use names an inode of this volume and bears a name. An entry
     * naming inode zero is not in use and is not held to either rule: it is the
     * record left where a name was removed, and its name length is ordinarily
     * zero but need not be.
     */
    if (entry->inode != 0U)
    {
        if (entry->inode > superblock->inode_count)
        {
            return Ext2EntryRefuse("a directory entry names an inode the volume does not hold");
        }

        if (entry->name_length == 0U)
        {
            return Ext2EntryRefuse("a directory entry in use bears no name");
        }
    }

    return true;
}

/*
 * Reads the name of an entry whose header has been read and validated.
 *
 * The name is read directly into the entry rather than through a buffer of its
 * own: a name may be 255 bytes, the entry has room for it already, and the
 * kernel stack is not large enough to hold a second copy without reason. A
 * character type may alias any object, so reading bytes into the storage of a
 * char array is defined and not a pun.
 *
 * The name is then held to the two rules the resolver depends upon. Neither is
 * stated by the specification, which describes a name as bytes and attributes no
 * meaning to any of them; both are enforced because a name containing the
 * separator would be reachable by no path, and a name containing a null byte
 * would compare equal to its own prefix once terminated. A volume bearing such a
 * name is not one this kernel can address correctly, and saying so is better
 * than resolving a path to the wrong file.
 */
bool Ext2ReadEntryName(BlockDevice *device, const Ext2Superblock *superblock,
                              Ext2DirectoryEntry *entry)
{
    if (!Ext2ReadBytes(device, superblock, entry->block,
                       entry->offset + EXT2_DIRECTORY_HEADER_SIZE, entry->name_length,
                       (uint8_t *)entry->name))
    {
        return false;
    }

    entry->name[entry->name_length] = '\0';

    for (uint16_t index = 0U; index < entry->name_length; ++index)
    {
        if ((entry->name[index] == EXT2_PATH_SEPARATOR) || (entry->name[index] == '\0'))
        {
            return Ext2EntryRefuse("a directory entry's name holds a separator or a null byte");
        }
    }

    return true;
}

void Ext2DirectoryOpen(Ext2DirectoryCursor *cursor, const Ext2Inode *directory)
{
    if (cursor == NULL)
    {
        return;
    }

    cursor->directory = directory;
    cursor->index = 0U;
    cursor->offset = 0U;
}

Ext2DirectoryStep Ext2DirectoryNext(BlockDevice *device, const Ext2Superblock *superblock,
                                    Ext2DirectoryCursor *cursor, Ext2DirectoryEntry *entry)
{
    uint64_t blocks;

    if ((device == NULL) || (superblock == NULL) || (cursor == NULL) || (entry == NULL) ||
        (cursor->directory == NULL))
    {
        (void)Ext2EntryRefuse("no device, no volume, no cursor, or nowhere to put the entry");
        return EXT2_DIRECTORY_FAILED;
    }

    if (!Ext2DirectoryTraversable(superblock, cursor->directory))
    {
        return EXT2_DIRECTORY_FAILED;
    }

    blocks = cursor->directory->size / (uint64_t)superblock->block_size;

    while (cursor->index < blocks)
    {
        uint32_t block;

        if (!Ext2InodeBlock(device, superblock, cursor->directory, cursor->index, &block))
        {
            return EXT2_DIRECTORY_FAILED;
        }

        /*
         * A block the directory never had allocated holds no entries. Reading it
         * would yield zeroes, and a record length of zero cannot be advanced
         * past; passing over the block is both the correct reading of a hole and
         * the only one that terminates.
         */
        if (block == 0U)
        {
            ++cursor->index;
            cursor->offset = 0U;
            continue;
        }

        /*
         * The records of a block run to its end, so a remainder too small to
         * hold a header is a block that does not account for itself. It is
         * refused rather than passed over: something wrote a record length that
         * stops short, and the entries beyond it are unreachable.
         */
        if (cursor->offset > (superblock->block_size - EXT2_DIRECTORY_HEADER_SIZE))
        {
            (void)Ext2EntryRefuse("a directory block ends in too little space for an entry");
            return EXT2_DIRECTORY_FAILED;
        }

        if (!Ext2ReadEntryHeader(device, superblock, block, cursor->offset, entry))
        {
            return EXT2_DIRECTORY_FAILED;
        }

        cursor->offset += entry->record_length;

        if (cursor->offset == superblock->block_size)
        {
            ++cursor->index;
            cursor->offset = 0U;
        }

        /* An entry naming no inode holds space and not a name. */
        if (entry->inode == 0U)
        {
            continue;
        }

        if (!Ext2ReadEntryName(device, superblock, entry))
        {
            return EXT2_DIRECTORY_FAILED;
        }

        ++Ext2EntriesReadCount;
        return EXT2_DIRECTORY_ENTRY_READ;
    }

    return EXT2_DIRECTORY_END;
}

bool Ext2DirectoryFind(BlockDevice *device, const Ext2Superblock *superblock,
                       const Ext2Inode *directory, const char *name, size_t length,
                       Ext2DirectoryEntry *entry)
{
    Ext2DirectoryCursor cursor;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL) ||
        (entry == NULL))
    {
        return Ext2PathRefuse("no device, no volume, no directory, or no name to look for");
    }

    if (length == 0U)
    {
        return Ext2PathRefuse("a name of no length names nothing");
    }

    if (length > EXT2_NAME_MAXIMUM)
    {
        return Ext2PathRefuse("a name longer than the format permits can be upon no volume");
    }

    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, entry);
        bool same;

        if (step == EXT2_DIRECTORY_FAILED)
        {
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            return Ext2PathRefuse("the directory holds no entry of that name");
        }

        if (entry->name_length != (uint16_t)length)
        {
            continue;
        }

        same = true;

        for (size_t index = 0U; index < length; ++index)
        {
            if (entry->name[index] != name[index])
            {
                same = false;
                break;
            }
        }

        if (same)
        {
            return true;
        }
    }
}

uint64_t Ext2EntriesRead(void)
{
    return Ext2EntriesReadCount;
}

uint64_t Ext2EntriesRefused(void)
{
    return Ext2EntriesRefusedCount;
}

void Ext2ReportDirectoryEntry(const Ext2DirectoryEntry *entry)
{
    if (entry == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 entry: inode ");
    KernelWriteDecimal((uint64_t)entry->inode);
    KernelWriteString(", ");
    KernelWriteString(Ext2FileTypeName(entry->file_type));
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)entry->record_length);
    KernelWriteString(" bytes at block ");
    KernelWriteDecimal((uint64_t)entry->block);
    KernelWriteString(" offset ");
    KernelWriteDecimal((uint64_t)entry->offset);
    KernelWriteString(": ");
    KernelWriteString(entry->name);
    KernelWriteString("\n");
}

void Ext2ReportDirectory(BlockDevice *device, const Ext2Superblock *superblock,
                         const Ext2Inode *directory)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    uint64_t counted = 0U;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL))
    {
        return;
    }

    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("EXT2 directory ");
            KernelWriteDecimal((uint64_t)directory->number);
            KernelWriteString(" could not be read: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            break;
        }

        if (counted < EXT2_REPORTED_ENTRIES)
        {
            Ext2ReportDirectoryEntry(&entry);
        }

        ++counted;
    }

    KernelWriteString("EXT2 directory ");
    KernelWriteDecimal((uint64_t)directory->number);
    KernelWriteString(" holds ");
    KernelWriteDecimal(counted);
    KernelWriteString(" entries.\n");
}
