/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/name.c
 * Purpose: Implements the names by which a file is reached and the files
 *          themselves: the insertion and removal of a directory record, the
 *          judgement of whether a directory is empty, and the creation and
 *          destruction of files, directories and hard links.
 * Key functions: Ext2DirectoryInsert, Ext2DirectoryRemove, Ext2DirectoryIsEmpty,
 *          Ext2CreateFile, Ext2CreateDirectory, Ext2Link, Ext2Unlink,
 *          Ext2RemoveDirectory, Ext2NamesInserted, Ext2NamesRemoved,
 *          Ext2FilesCreated, Ext2FilesDestroyed.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Directory Structure chapter and the rec_len note: where a record is
 *     removed the record before it is lengthened to cover it, and where the
 *     first record of a block is removed a blank record is left in its place —
 *     a directory being a linked list with no back pointers and no free list.
 *   - The same, Table 4.1: a record length must be at least the length of its
 *     record, must be a multiple of four, and no entry may span two blocks.
 *   - The same, the Inode Table chapter: i_links_count counts the names that
 *     reach an inode, and an inode is freed only with its last one; a directory
 *     begins with two, being named by its parent and by its own ".".
 */

#include "internal.h"

#include <oxys/buffer.h>
#include <oxys/kernel.h>


/*
 * Writing.
 *
 * The disciplines this whole section is held to are stated at the head of the
 * writing declarations in oxys/ext2.h and are not repeated here. What follows is
 * their machinery.
 */

static void Ext2ComposeEntryHeader(uint8_t *raw, uint32_t number, uint16_t record_length,
                                   uint16_t name_length, uint8_t file_type, bool states_type)
{
    Ext2WriteWord(raw, EXT2_OFFSET_DE_INODE, number);
    Ext2WriteHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH, record_length);

    if (states_type)
    {
        raw[EXT2_OFFSET_DE_NAME_LENGTH] = (uint8_t)name_length;
        raw[EXT2_OFFSET_DE_FILE_TYPE] = file_type;
    }
    else
    {
        Ext2WriteHalf(raw, EXT2_OFFSET_DE_NAME_LENGTH, name_length);
    }
}

/* Writes a whole record — header and name — at an offset within a block. */
static bool Ext2PutEntry(BlockDevice *device, Ext2Superblock *superblock, uint32_t block,
                         uint32_t offset, uint32_t number, uint16_t record_length,
                         const char *name, size_t length, uint8_t file_type)
{
    uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

    Ext2ComposeEntryHeader(raw, number, record_length, (uint16_t)length, file_type,
                           Ext2VolumeStatesFileType(superblock));

    if (!Ext2WriteBytes(device, superblock, block, offset, EXT2_DIRECTORY_HEADER_SIZE, raw))
    {
        return false;
    }

    if (length == 0U)
    {
        return true;
    }

    return Ext2WriteBytes(device, superblock, block, offset + EXT2_DIRECTORY_HEADER_SIZE,
                          (uint32_t)length, (const uint8_t *)name);
}

/*
 * Finds room for a record of the given length within one block of a directory,
 * and lays the new record in it.
 *
 * Two kinds of room exist and both are used. A record naming no inode is unused
 * entirely and may be taken if it is long enough. A record in use is ordinarily
 * longer than the name it holds — because a removal lengthened it, or because it
 * is the last of its block and runs to the end — and the excess beyond what it
 * actually needs may be given away by shortening it.
 */
static bool Ext2InsertIntoBlock(BlockDevice *device, Ext2Superblock *superblock,
                                uint32_t block, uint16_t needed, uint32_t number,
                                const char *name, size_t length, uint8_t file_type,
                                bool *inserted)
{
    uint32_t offset = 0U;

    *inserted = false;

    while (offset <= (superblock->block_size - EXT2_DIRECTORY_HEADER_SIZE))
    {
        Ext2DirectoryEntry entry;
        uint16_t actual;

        if (!Ext2ReadEntryHeader(device, superblock, block, offset, &entry))
        {
            return false;
        }

        /* An unused record may be taken whole. */
        if (entry.inode == 0U)
        {
            if (entry.record_length >= needed)
            {
                *inserted = true;
                return Ext2PutEntry(device, superblock, block, offset, number,
                                    entry.record_length, name, length, file_type);
            }
        }
        else
        {
            actual = EXT2_RECORD_LENGTH(entry.name_length);

            if ((entry.record_length - actual) >= needed)
            {
                const uint16_t remainder = (uint16_t)(entry.record_length - actual);
                uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

                /*
                 * The record in use is shortened to what it needs and the new one
                 * takes what is left. The header of the existing record is rewritten
                 * rather than composed afresh, so that its name and its file type
                 * are untouched: only its length changes.
                 */
                if (!Ext2ReadBytes(device, superblock, block, offset,
                                   EXT2_DIRECTORY_HEADER_SIZE, raw))
                {
                    return false;
                }

                Ext2WriteHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH, actual);

                if (!Ext2WriteBytes(device, superblock, block, offset,
                                    EXT2_DIRECTORY_HEADER_SIZE, raw))
                {
                    return false;
                }

                *inserted = true;
                return Ext2PutEntry(device, superblock, block, offset + actual, number,
                                    remainder, name, length, file_type);
            }
        }

        offset += entry.record_length;

        if (offset == superblock->block_size)
        {
            break;
        }
    }

    return true;
}

bool Ext2DirectoryInsert(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                         const char *name, size_t length, uint32_t number, uint8_t file_type)
{
    Ext2DirectoryEntry existing;
    uint16_t needed;
    uint64_t blocks;
    uint32_t block;
    bool allocated;
    bool inserted = false;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or no name to insert");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(directory))
    {
        return Ext2WriteRefuse("a name may be inserted only into a directory");
    }

    if ((length == 0U) || (length > EXT2_NAME_MAXIMUM))
    {
        return Ext2WriteRefuse("a name of no length, or longer than the format permits");
    }

    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2WriteRefuse("a name may not be given to an inode the volume does not hold");
    }

    for (size_t index = 0U; index < length; ++index)
    {
        if ((name[index] == EXT2_PATH_SEPARATOR) || (name[index] == '\0'))
        {
            return Ext2WriteRefuse("a name holding a separator or a null byte");
        }
    }

    /*
     * A name already present is refused. A directory holding one name twice names
     * two files by the same path, and which is found depends upon which record
     * the traversal reaches first — so the duplicate is not merely untidy, it
     * makes the path ambiguous.
     */
    if (Ext2DirectoryFind(device, superblock, directory, name, length, &existing))
    {
        return Ext2WriteRefuse("the directory already holds that name");
    }

    needed = EXT2_RECORD_LENGTH(length);
    blocks = directory->size / (uint64_t)superblock->block_size;

    for (uint64_t index = 0U; index < blocks; ++index)
    {
        if (!Ext2InodeBlock(device, superblock, directory, index, &block))
        {
            return false;
        }

        if (block == 0U)
        {
            continue;
        }

        if (!Ext2InsertIntoBlock(device, superblock, block, needed, number, name, length,
                                 file_type, &inserted))
        {
            return false;
        }

        if (inserted)
        {
            ++Ext2NamesInsertedCount;
            return true;
        }
    }

    /*
     * No block had room, so the directory grows by one. The new block holds a
     * single record running its whole length, which is the shape of an empty
     * block, and the insertion then finds room in it as it would in any other.
     */
    if (!Ext2InodeBlockAllocate(device, superblock, directory, blocks, &block, &allocated))
    {
        return false;
    }

    if (!Ext2ZeroBlock(device, superblock, block) ||
        !Ext2PutEntry(device, superblock, block, 0U, 0U, (uint16_t)superblock->block_size,
                      NULL, 0U, (uint8_t)EXT2_FT_UNKNOWN))
    {
        return false;
    }

    directory->size += superblock->block_size;

    if (!Ext2WriteInode(device, superblock, directory))
    {
        return false;
    }

    if (!Ext2InsertIntoBlock(device, superblock, block, needed, number, name, length,
                             file_type, &inserted))
    {
        return false;
    }

    if (!inserted)
    {
        return Ext2WriteRefuse("a name did not fit within a block of its own");
    }

    ++Ext2NamesInsertedCount;
    return true;
}

bool Ext2DirectoryRemove(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                         const char *name, size_t length)
{
    uint64_t blocks;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or no name to remove");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(directory))
    {
        return Ext2WriteRefuse("a name may be removed only from a directory");
    }

    if ((length == 0U) || (length > EXT2_NAME_MAXIMUM))
    {
        return Ext2WriteRefuse("a name of no length, or longer than the format permits");
    }

    /*
     * A directory without "." no longer knows itself, and without ".." no longer
     * knows its parent; the resolver finds both by looking, so removing either
     * makes paths through this directory fail.
     */
    if (((length == 1U) && (name[0] == '.')) ||
        ((length == 2U) && (name[0] == '.') && (name[1] == '.')))
    {
        return Ext2WriteRefuse("the entries \".\" and \"..\" may not be removed");
    }

    blocks = directory->size / (uint64_t)superblock->block_size;

    for (uint64_t index = 0U; index < blocks; ++index)
    {
        uint32_t block;
        uint32_t offset = 0U;
        uint32_t previous = 0U;
        bool have_previous = false;

        if (!Ext2InodeBlock(device, superblock, directory, index, &block))
        {
            return false;
        }

        if (block == 0U)
        {
            continue;
        }

        while (offset <= (superblock->block_size - EXT2_DIRECTORY_HEADER_SIZE))
        {
            Ext2DirectoryEntry entry;
            bool same = false;

            if (!Ext2ReadEntryHeader(device, superblock, block, offset, &entry))
            {
                return false;
            }

            if ((entry.inode != 0U) && (entry.name_length == (uint16_t)length))
            {
                if (!Ext2ReadEntryName(device, superblock, &entry))
                {
                    return false;
                }

                same = true;

                for (size_t position = 0U; position < length; ++position)
                {
                    if (entry.name[position] != name[position])
                    {
                        same = false;
                        break;
                    }
                }
            }

            if (same)
            {
                uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

                if (have_previous)
                {
                    /*
                     * The record before it absorbs its space. Nothing is moved and
                     * nothing is zeroed: the bytes of the removed record remain
                     * where they are, unreachable, until an insertion takes them.
                     */
                    Ext2DirectoryEntry before;

                    if (!Ext2ReadEntryHeader(device, superblock, block, previous, &before) ||
                        !Ext2ReadBytes(device, superblock, block, previous,
                                       EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }

                    Ext2WriteHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH,
                                  (uint16_t)(before.record_length + entry.record_length));

                    if (!Ext2WriteBytes(device, superblock, block, previous,
                                        EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }
                }
                else
                {
                    /*
                     * The first record of a block has nothing before it to lengthen,
                     * so it is marked unused where it stands. This is why a traversal
                     * must pass over a record naming inode 0 rather than reading the
                     * name still lying in it.
                     */
                    if (!Ext2ReadBytes(device, superblock, block, offset,
                                       EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }

                    Ext2WriteWord(raw, EXT2_OFFSET_DE_INODE, 0U);

                    if (!Ext2WriteBytes(device, superblock, block, offset,
                                        EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }
                }

                ++Ext2NamesRemovedCount;
                return true;
            }

            previous = offset;
            have_previous = true;
            offset += entry.record_length;

            if (offset == superblock->block_size)
            {
                break;
            }
        }
    }

    return Ext2WriteRefuse("the directory holds no name of that name to remove");
}

bool Ext2DirectoryIsEmpty(BlockDevice *device, const Ext2Superblock *superblock,
                          const Ext2Inode *directory, bool *empty)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (empty == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or nowhere for the answer");
    }

    *empty = true;
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

        /* "." and ".." are what every directory holds; anything else occupies it. */
        if (((entry.name_length == 1U) && (entry.name[0] == '.')) ||
            ((entry.name_length == 2U) && (entry.name[0] == '.') && (entry.name[1] == '.')))
        {
            continue;
        }

        *empty = false;
        return true;
    }
}

/*
 * Creating and destroying files.
 *
 * A file is an inode and the names that lead to it, and the two are kept in
 * step by i_links_count. Creation allocates the inode and gives it one name;
 * a further name raises the count; removing a name lowers it; and the file
 * itself is destroyed only when the count reaches zero, since until then some
 * path still reaches it.
 */

/* Gives a newly allocated inode the state a file begins with: no blocks, no
 * size, and the mode the caller asked for. */
static void Ext2ComposeInode(Ext2Inode *inode, uint32_t number, uint16_t mode,
                             uint16_t link_count)
{
    inode->number = number;
    inode->mode = mode;
    inode->uid = 0U;
    inode->gid = 0U;
    inode->size = 0U;
    inode->access_time = 0U;
    inode->change_time = 0U;
    inode->modify_time = 0U;
    inode->delete_time = 0U;
    inode->link_count = link_count;
    inode->sector_count = 0U;
    inode->flags = 0U;
    inode->generation = 0U;
    inode->file_acl = 0U;

    for (uint32_t entry = 0U; entry < EXT2_BLOCK_POINTER_COUNT; ++entry)
    {
        inode->block[entry] = 0U;
    }
}

bool Ext2CreateFile(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                    const char *name, size_t length, uint16_t mode, Ext2Inode *inode)
{
    Ext2Inode created;
    uint32_t number;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL) ||
        (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, no name, or nowhere to "
                               "put the inode");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    /*
     * A directory is refused here. One is not merely an inode with a different
     * mode: it must hold "." and ".." before anything may resolve a path through
     * it, and its parent's link count must account for the "..". Creating one
     * with this would leave a directory that is not a directory.
     */
    if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
    {
        return Ext2WriteRefuse("a directory is created by Ext2CreateDirectory");
    }

    if ((mode & EXT2_S_IFMT) == 0U)
    {
        return Ext2WriteRefuse("a file must be created with a format");
    }

    if (!Ext2AllocateInode(device, superblock, false, &number))
    {
        return false;
    }

    Ext2ComposeInode(&created, number, mode, 1U);

    if (!Ext2WriteInode(device, superblock, &created))
    {
        return false;
    }

    /*
     * The inode is written before the name is inserted, so that a machine
     * stopping between the two leaves an inode nothing names — which a check
     * reclaims — rather than a name leading to an inode that was never written.
     */
    if (!Ext2DirectoryInsert(device, superblock, directory, name, length, number,
                             Ext2FileTypeOfMode(mode)))
    {
        return false;
    }

    *inode = created;
    ++Ext2FilesCreatedCount;
    return true;
}

bool Ext2CreateDirectory(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *parent,
                         const char *name, size_t length, uint16_t mode, Ext2Inode *inode)
{
    Ext2Inode created;
    uint32_t number;
    uint32_t block;
    uint16_t dot_length;
    bool allocated;

    if ((device == NULL) || (superblock == NULL) || (parent == NULL) || (name == NULL) ||
        (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no parent, no name, or nowhere to put "
                               "the inode");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(parent))
    {
        return Ext2WriteRefuse("a directory may be created only within a directory");
    }

    /*
     * The parent gains a link for the ".." this directory will hold, so a parent
     * already at the limit cannot take another child. Refusing before anything is
     * allocated is what keeps a half-made directory off the volume.
     */
    if (parent->link_count >= EXT2_LINK_MAXIMUM)
    {
        return Ext2WriteRefuse("the parent directory holds as many links as it may");
    }

    if (!Ext2AllocateInode(device, superblock, true, &number))
    {
        return false;
    }

    /* Two links: its own "." and the entry the parent is about to hold. */
    Ext2ComposeInode(&created, number, (uint16_t)((mode & EXT2_PERMISSION_MASK) | EXT2_S_IFDIR),
                     2U);

    if (!Ext2InodeBlockAllocate(device, superblock, &created, 0U, &block, &allocated) ||
        !Ext2ZeroBlock(device, superblock, block))
    {
        return false;
    }

    created.size = superblock->block_size;

    /*
     * The two entries every directory holds. The record for ".." runs to the end
     * of the block, which is what ends the list of that block; a directory whose
     * last record stopped after its name would have the padding beyond it read
     * as a further entry.
     */
    dot_length = EXT2_RECORD_LENGTH(1U);

    if (!Ext2PutEntry(device, superblock, block, 0U, number, dot_length, ".", 1U,
                      (uint8_t)EXT2_FT_DIR) ||
        !Ext2PutEntry(device, superblock, block, dot_length, parent->number,
                      (uint16_t)(superblock->block_size - dot_length), "..", 2U,
                      (uint8_t)EXT2_FT_DIR))
    {
        return false;
    }

    if (!Ext2WriteInode(device, superblock, &created) ||
        !Ext2DirectoryInsert(device, superblock, parent, name, length, number,
                             (uint8_t)EXT2_FT_DIR))
    {
        return false;
    }

    /* The parent's link count accounts for the ".." now standing in the child. */
    parent->link_count++;

    if (!Ext2WriteInode(device, superblock, parent))
    {
        return false;
    }

    *inode = created;
    ++Ext2FilesCreatedCount;
    return true;
}

bool Ext2Link(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
              const char *name, size_t length, Ext2Inode *target)
{
    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL) ||
        (target == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, no name, or no file");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    /*
     * A directory may bear only one name. Two paths to one directory make a cycle
     * in what the format requires to be a tree, and a resolver walking ".." from
     * within it would have no single answer to give.
     */
    if (Ext2InodeIsDirectory(target))
    {
        return Ext2WriteRefuse("a directory may not be given a second name");
    }

    if (target->link_count >= EXT2_LINK_MAXIMUM)
    {
        return Ext2WriteRefuse("the file bears as many names as it may");
    }

    if (!Ext2DirectoryInsert(device, superblock, directory, name, length, target->number,
                             Ext2FileTypeOfMode(target->mode)))
    {
        return false;
    }

    target->link_count++;
    return Ext2WriteInode(device, superblock, target);
}

bool Ext2Unlink(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                const char *name, size_t length)
{
    Ext2DirectoryEntry entry;
    Ext2Inode target;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or no name");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2DirectoryFind(device, superblock, directory, name, length, &entry) ||
        !Ext2ReadInode(device, superblock, entry.inode, &target))
    {
        return false;
    }

    if (Ext2InodeIsDirectory(&target))
    {
        return Ext2WriteRefuse("a directory is removed by Ext2RemoveDirectory");
    }

    if (!Ext2DirectoryRemove(device, superblock, directory, name, length))
    {
        return false;
    }

    if (target.link_count > 0U)
    {
        target.link_count--;
    }

    /*
     * A file with names remaining is not destroyed; only its count changes. A
     * file with none is destroyed, and the blocks go before the inode: an inode
     * freed while its blocks were still marked used would leak them with nothing
     * left to say which they were.
     */
    if (target.link_count > 0U)
    {
        return Ext2WriteInode(device, superblock, &target);
    }

    /*
     * A symbolic link holding its target within the inode has no blocks to free,
     * and the words of i_block are its target rather than pointers; truncating it
     * would read them as blocks of the volume.
     */
    if (!Ext2InodeIsFastSymbolicLink(superblock, &target) &&
        !Ext2TruncateBlocks(device, superblock, &target, 0U))
    {
        return false;
    }

    target.size = 0U;
    target.delete_time = EXT2_DELETION_TIME_UNKNOWN;

    if (!Ext2WriteInode(device, superblock, &target) ||
        !Ext2FreeInode(device, superblock, target.number, false))
    {
        return false;
    }

    ++Ext2FilesDestroyedCount;
    return true;
}

bool Ext2RemoveDirectory(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *parent,
                         const char *name, size_t length)
{
    Ext2DirectoryEntry entry;
    Ext2Inode target;
    bool empty = false;

    if ((device == NULL) || (superblock == NULL) || (parent == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no parent, or no name");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2DirectoryFind(device, superblock, parent, name, length, &entry) ||
        !Ext2ReadInode(device, superblock, entry.inode, &target))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(&target))
    {
        return Ext2WriteRefuse("what the name leads to is not a directory");
    }

    if (target.number == EXT2_ROOT_INODE)
    {
        return Ext2WriteRefuse("the root directory may not be removed");
    }

    if (!Ext2DirectoryIsEmpty(device, superblock, &target, &empty))
    {
        return false;
    }

    /*
     * A directory holding anything is not removed. Removing it would leave every
     * file within it reachable by no path — an inode with a link count above zero
     * that nothing names, which is exactly the state a check has to repair by
     * hand.
     */
    if (!empty)
    {
        return Ext2WriteRefuse("the directory is not empty");
    }

    if (!Ext2DirectoryRemove(device, superblock, parent, name, length) ||
        !Ext2TruncateBlocks(device, superblock, &target, 0U))
    {
        return false;
    }

    target.size = 0U;
    target.link_count = 0U;
    target.delete_time = EXT2_DELETION_TIME_UNKNOWN;

    if (!Ext2WriteInode(device, superblock, &target) ||
        !Ext2FreeInode(device, superblock, target.number, true))
    {
        return false;
    }

    /* The link the child's ".." held in the parent goes with it. */
    if (parent->link_count > 0U)
    {
        parent->link_count--;
    }

    if (!Ext2WriteInode(device, superblock, parent))
    {
        return false;
    }

    ++Ext2FilesDestroyedCount;
    return true;
}
