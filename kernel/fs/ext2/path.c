/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/path.c
 * Purpose: Implements the resolution of an absolute path to the inode it names:
 *          the walk from the root through each component, the following of
 *          symbolic links encountered on the way, and the bound upon how many of
 *          them one resolution may follow.
 * Key functions: Ext2ResolvePath, Ext2ResolvePathNoFollow, Ext2PathsResolved,
 *          Ext2PathsRefused.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Symbolic Links chapter: a link's target is a path, which is resolved
 *     against the directory holding the link where it is relative.
 *   - IEEE Std 1003.1-2017, the definition of pathname resolution: a pathname
 *     ending in a separator names a directory, and a symbolic link encountered
 *     as a non-final component is always followed whatever the caller asked of
 *     the final one.
 *
 * Why this is a file of its own.
 *
 *   Turning a name into an inode and managing the records a directory is made of
 *   are different problems that happen to meet at `Ext2DirectoryFind`. This file
 *   holds the first; `directory.c` holds the record format and the traversal;
 *   `name.c` holds the creation and destruction of the names themselves. The
 *   three shared one 4,325-line translation unit until they were divided, and
 *   the division is along the line their bugs fall upon: a fault here is a path
 *   resolved to the wrong file, a fault there is a volume left malformed.
 */

#include "internal.h"

#include <oxys/block/buffer.h>
#include <oxys/kernel.h>


/*
 * Whether a path ends in a separator, which is an assertion by the caller that
 * what it names is a directory.
 *
 * It is determined before the walk because it governs the treatment of the last
 * component: a link is not a directory, so a path ending in a separator is
 * asking for what the link names whether or not the caller asked links to be
 * followed.
 */
static bool Ext2PathAssertsDirectory(const char *path, size_t length)
{
    return (length > 0U) && (path[length - 1U] == EXT2_PATH_SEPARATOR);
}

/* The length of a path, refusing one that is not terminated within the bound. */
static bool Ext2PathLength(const char *path, size_t *length)
{
    size_t position = 0U;

    while (path[position] != '\0')
    {
        ++position;

        if (position > EXT2_PATH_MAXIMUM)
        {
            return Ext2PathRefuse("the path is longer than this kernel will resolve");
        }
    }

    *length = position;
    return true;
}

/*
 * Resolves a path against a starting directory, following symbolic links.
 *
 * The depth is the number of links already followed. A link is followed by
 * resolving its target, which re-enters this function, so the depth is what
 * bounds a link that names itself — directly, or around a cycle of several. The
 * format offers no protection against such a link and cannot: it is a valid
 * file whose contents happen to be its own name.
 *
 * A link is followed by resolving its target to an inode and continuing the
 * original path from that point, rather than by splicing the target into the
 * path and starting again. The two are equivalent, and this one needs no buffer
 * to hold the spliced path — which matters, every level of the recursion already
 * carrying a target of its own.
 */
static bool Ext2ResolveFrom(BlockDevice *device, const Ext2Superblock *superblock,
                            const Ext2Inode *start, const char *path, bool follow_last,
                            uint32_t depth, Ext2Inode *inode)
{
    Ext2DirectoryEntry entry;
    Ext2Inode current;
    Ext2Inode next;
    /* Initialised because Ext2PathLength writes it only when it succeeds, and
     * its failure path returns through Ext2PathRefuse, which is in another
     * translation unit since this file was divided out of `kernel/fs/ext2.c`.
     * A compiler that cannot see that function's body cannot know it always
     * returns false. The reasoning is set out upon Ext2BlockPosition in
     * `alloc.c`. */
    size_t length = 0U;
    size_t position = 0U;
    bool asserts_directory;

    if (depth > EXT2_SYMLINK_DEPTH_MAXIMUM)
    {
        return Ext2PathRefuse("too many symbolic links were followed to resolve one path");
    }

    if (!Ext2PathLength(path, &length))
    {
        return false;
    }

    if (length == 0U)
    {
        return Ext2PathRefuse("an empty path names nothing");
    }

    asserts_directory = Ext2PathAssertsDirectory(path, length);

    /*
     * An absolute target begins again at the root; a relative one continues from
     * the directory it was found in. This is the whole of the difference between
     * the two, and it is why a link must be resolved against the directory
     * holding it rather than against the root or the working directory.
     */
    if (path[0] == EXT2_PATH_SEPARATOR)
    {
        if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &current))
        {
            return false;
        }
    }
    else if (start != NULL)
    {
        current = *start;
    }
    else
    {
        return Ext2PathRefuse("the path is not absolute");
    }

    if (!Ext2InodeIsDirectory(&current))
    {
        return Ext2PathRefuse("a path is resolved against something that is not a directory");
    }

    for (;;)
    {
        size_t start_of_component;
        size_t component_length;
        bool last;

        /* Consecutive separators are one separator, and a path may end in them. */
        while (path[position] == EXT2_PATH_SEPARATOR)
        {
            ++position;
        }

        if (path[position] == '\0')
        {
            break;
        }

        start_of_component = position;

        while ((path[position] != '\0') && (path[position] != EXT2_PATH_SEPARATOR))
        {
            ++position;
        }

        component_length = position - start_of_component;

        /*
         * Whether this is the last component, which governs whether a link
         * standing here is followed. A separator after it does not make it any
         * less the last: "/link/" names the last component still, and asserts
         * that it is a directory.
         */
        last = true;

        for (size_t look = position; path[look] != '\0'; ++look)
        {
            if (path[look] != EXT2_PATH_SEPARATOR)
            {
                last = false;
                break;
            }
        }

        /*
         * Only a directory holds names. Refusing here rather than within the
         * lookup distinguishes the two failures a caller cares about: a path
         * whose components do not exist, and a path that treats a file as though
         * it were a directory.
         */
        if (!Ext2InodeIsDirectory(&current))
        {
            return Ext2PathRefuse("a component of the path is not a directory");
        }

        if (!Ext2DirectoryFind(device, superblock, &current, &path[start_of_component],
                               component_length, &entry))
        {
            return false;
        }

        if (!Ext2ReadInode(device, superblock, entry.inode, &next))
        {
            return false;
        }

        /*
         * The specification requires the file type of an entry to match the
         * format of the inode it names. The two are written at different times
         * by different code, and a volume upon which they disagree is one whose
         * directories and inodes no longer describe the same filesystem; the
         * check costs nothing here, the inode having just been read.
         *
         * A volume that states no file type declares EXT2_FT_UNKNOWN for every
         * entry, and there is nothing to check.
         */
        if ((entry.file_type != (uint8_t)EXT2_FT_UNKNOWN) &&
            (entry.file_type != Ext2FileTypeOfMode(next.mode)))
        {
            return Ext2PathRefuse("a directory entry's file type contradicts its inode");
        }

        /*
         * A link within the path must be followed for the rest of the path to
         * mean anything. A link as the last component is followed only if the
         * caller asked for the file rather than for its name — or if the path
         * asserts a directory, a link not being one.
         */
        if (Ext2InodeIsSymbolicLink(&next) && (!last || follow_last || asserts_directory))
        {
            char target[EXT2_SYMLINK_MAXIMUM + 1U];
            Ext2Inode resolved;

            if (!Ext2ReadSymbolicLink(device, superblock, &next, target, sizeof target))
            {
                return false;
            }

            if (!Ext2ResolveFrom(device, superblock, &current, target, true, depth + 1U,
                                 &resolved))
            {
                return false;
            }

            next = resolved;
        }

        current = next;
    }

    /*
     * A path written with a trailing separator asserts that what it names is a
     * directory. The assertion is the caller's and is honoured: "/etc/" names a
     * directory or it names nothing.
     */
    if (asserts_directory && !Ext2InodeIsDirectory(&current))
    {
        return Ext2PathRefuse("the path ends in a separator but does not name a directory");
    }

    *inode = current;
    return true;
}

bool Ext2ResolvePath(BlockDevice *device, const Ext2Superblock *superblock, const char *path,
                     Ext2Inode *inode)
{
    if ((device == NULL) || (superblock == NULL) || (path == NULL) || (inode == NULL))
    {
        return Ext2PathRefuse("no device, no volume, no path, or nowhere to put the inode");
    }

    /*
     * Only an absolute path is resolved here. A relative one is resolved against
     * a working directory, which is a property of a process and not of a volume,
     * and there are no processes until Phase 6. Ext2ResolveFrom accepts a
     * relative path because a symbolic link's target may be one, and is resolved
     * against the directory holding the link.
     */
    if (path[0] != EXT2_PATH_SEPARATOR)
    {
        return Ext2PathRefuse("the path is not absolute");
    }

    if (!Ext2ResolveFrom(device, superblock, NULL, path, true, 0U, inode))
    {
        return false;
    }

    ++Ext2PathsResolvedCount;
    return true;
}

bool Ext2ResolvePathNoFollow(BlockDevice *device, const Ext2Superblock *superblock,
                             const char *path, Ext2Inode *inode)
{
    if ((device == NULL) || (superblock == NULL) || (path == NULL) || (inode == NULL))
    {
        return Ext2PathRefuse("no device, no volume, no path, or nowhere to put the inode");
    }

    if (path[0] != EXT2_PATH_SEPARATOR)
    {
        return Ext2PathRefuse("the path is not absolute");
    }

    if (!Ext2ResolveFrom(device, superblock, NULL, path, false, 0U, inode))
    {
        return false;
    }

    ++Ext2PathsResolvedCount;
    return true;
}

uint64_t Ext2PathsResolved(void)
{
    return Ext2PathsResolvedCount;
}

uint64_t Ext2PathsRefused(void)
{
    return Ext2PathsRefusedCount;
}

