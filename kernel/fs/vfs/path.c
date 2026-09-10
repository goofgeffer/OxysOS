/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/vfs/path.c
 * Purpose: Implements the resolution of a path to the node it names: the walk
 *          from the root or from a directory through each component, the
 *          crossing of mount points in both directions, the following of
 *          symbolic links, and the resolution of a path's parent for the
 *          operations that alter a directory.
 * Key functions: VfsWalk, VfsResolveCounted, VfsResolve, VfsResolveNoFollow,
 *          VfsResolveParent, VfsWritable.
 * References:
 *   - IEEE Std 1003.1-2017, Section 4.13, pathname resolution: a path beginning
 *     with a separator resolves from the root; successive separators are
 *     equivalent to one; a component that is not the last must be a directory;
 *     and a trailing separator asserts that what the path names is a directory.
 *   - docs/storage/VFS.md, Sections 4 and 5: the rules above and where each is
 *     applied, and why `.` and `..` are not interpreted here.
 *
 * VfsWalk takes a length as well as a path, and that is what makes the rest of
 * this layer free of buffers: the parent of a path is resolved by walking the
 * prefix of that path where it stands, so nothing here copies a path or a
 * component out of the caller's string.
 */

#include "internal.h"

#include <oxys/vfs.h>
#include <oxys/kernel.h>
#include <oxys/block.h>
#include <oxys/heap.h>

/* The length of a path, refusing one that is absent, empty, or not terminated
 * within the bound this kernel resolves. */
bool VfsPathLength(const char *path, size_t *length)
{
    size_t measured;

    if (path == NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "no path was given");
    }

    measured = VfsStringLength(path, VFS_PATH_MAXIMUM + 1U);

    if (measured == 0U)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the path is empty and names nothing");
    }

    if (measured > VFS_PATH_MAXIMUM)
    {
        return VfsRefuse(VFS_ERROR_TOO_LONG,
                         "the path is longer than this kernel will resolve");
    }

    *length = measured;
    return true;
}

/* Whether a component is "." or "..", which name a directory that already
 * exists and may therefore neither be created nor removed. */
static bool VfsComponentIsDot(const char *name, size_t length)
{
    if ((length == 1U) && (name[0] == '.'))
    {
        return true;
    }

    return (length == 2U) && (name[0] == '.') && (name[1] == '.');
}

/*
 * Walks a path of a stated length from a starting directory and produces the
 * node it names, holding a reference upon it.
 *
 * The length is explicit so that a prefix of a path may be walked where it
 * stands. That is what allows the parent of a path to be resolved without
 * copying the path into a buffer to terminate it, and it is why nothing in this
 * file holds a path buffer at all.
 *
 * `depth` counts the symbolic links followed so far in this resolution. A link
 * is followed by re-entering this function upon its target, so the depth is the
 * recursion depth and bounding it is what keeps a link that names itself from
 * consuming the stack. Each frame carries a target buffer of
 * VFS_SYMLINK_MAXIMUM + 1 bytes; eight of them is the whole cost.
 */
bool VfsWalk(VfsNode *start, const char *path, size_t length, bool follow_last,
                    unsigned int depth, VfsNode **result)
{
    VfsNode *current;
    size_t position = 0U;
    bool asserts_directory = false;

    if (depth > VFS_SYMLINK_DEPTH_MAXIMUM)
    {
        return VfsRefuse(VFS_ERROR_TOO_MANY_LINKS,
                         "more symbolic links were met in one path than this kernel follows");
    }

    if (length > VFS_PATH_MAXIMUM)
    {
        return VfsRefuse(VFS_ERROR_TOO_LONG,
                         "the path is longer than this kernel will resolve");
    }

    if ((length > 0U) && (path[0] == VFS_PATH_SEPARATOR))
    {
        if (VfsRootMount == NULL)
        {
            return VfsRefuse(VFS_ERROR_NOT_FOUND,
                             "no volume is mounted at the root, so no absolute path resolves");
        }

        current = VfsRootMount->root;
    }
    else
    {
        if (start == NULL)
        {
            return VfsRefuse(VFS_ERROR_INVALID,
                             "the path is relative and there is no directory to resolve it "
                             "against");
        }

        current = start;
    }

    VfsNodeHold(current);

    while (position < length)
    {
        const char *name;
        size_t name_length = 0U;
        size_t look;
        bool last;
        VfsNode *child;
        uint64_t number = 0U;

        while ((position < length) && (path[position] == VFS_PATH_SEPARATOR))
        {
            ++position;
        }

        if (position >= length)
        {
            break;
        }

        name = &path[position];

        while ((position < length) && (path[position] != VFS_PATH_SEPARATOR))
        {
            ++position;
            ++name_length;
        }

        if (name_length > VFS_NAME_MAXIMUM)
        {
            VfsNodeRelease(current);
            return VfsRefuse(VFS_ERROR_TOO_LONG,
                             "a component of the path is longer than a name may be");
        }

        /*
         * Whether this is the last component, and whether the path asserts that
         * what it names is a directory. Repeated separators between components
         * are equivalent to one and assert nothing; separators after the last
         * component are the assertion.
         */
        look = position;

        while ((look < length) && (path[look] == VFS_PATH_SEPARATOR))
        {
            ++look;
        }

        last = (look >= length);
        asserts_directory = last && (look > position);

        if (current->type != VFS_NODE_DIRECTORY)
        {
            VfsNodeRelease(current);
            return VfsRefuse(VFS_ERROR_NOT_DIRECTORY,
                             "a component of the path that must be a directory is not one");
        }

        /*
         * Leaving a mounted volume by "..".
         *
         * The ".." of a volume's root names that root, which is what the volume
         * says and is right when the volume stands alone. Where the volume is
         * mounted within another, the parent of its root is the directory the
         * mount covers, and that is a fact of the tree rather than of the volume:
         * only this layer knows it. The step is taken before the lookup, so the
         * ".." that is then looked up is the one in the covering directory.
         *
         * It is a loop and not a single step because a mount point may itself be
         * the root of a mount in a kernel that stacks them. This one does not,
         * but a bound written as a loop does not become wrong when it does.
         */
        if ((name_length == 2U) && (name[0] == '.') && (name[1] == '.'))
        {
            while ((current->mount->root == current) && (current->mount->covered != NULL))
            {
                VfsNode *const covered = current->mount->covered;

                VfsNodeHold(covered);
                VfsNodeRelease(current);
                current = covered;
            }
        }

        if (!current->mount->operations->lookup(current, name, name_length, &number))
        {
            VfsNodeRelease(current);
            return false;
        }

        child = VfsNodeAcquire(current->mount, number);

        if (child == NULL)
        {
            VfsNodeRelease(current);
            return false;
        }

        /*
         * Crossing into a mounted volume. The node the name reached is a mount
         * point, and what the path names is the root of the volume mounted upon
         * it; the directory beneath is hidden for as long as the mount stands.
         */
        while (child->mounted != NULL)
        {
            VfsNode *const root = child->mounted->root;

            VfsNodeHold(root);
            VfsNodeRelease(child);
            child = root;
        }

        /*
         * Following a symbolic link.
         *
         * A link within the path is always followed, there being no way to
         * continue through one otherwise. A link standing last is followed where
         * the caller asked for the file rather than for its name, and always
         * where the path ends in a separator: such a path asserts a directory,
         * and a link is not one, so it is asking for what the link names.
         */
        if ((child->type == VFS_NODE_SYMBOLIC_LINK) &&
            ((!last) || follow_last || asserts_directory))
        {
            char target[VFS_SYMLINK_MAXIMUM + 1U];
            VfsNode *resolved = NULL;
            size_t target_length;

            if (child->mount->operations->read_link == NULL)
            {
                VfsNodeRelease(child);
                VfsNodeRelease(current);
                return VfsRefuse(VFS_ERROR_UNSUPPORTED,
                                 "the filesystem does not read symbolic links");
            }

            if (!child->mount->operations->read_link(child, target, sizeof target))
            {
                VfsNodeRelease(child);
                VfsNodeRelease(current);
                return false;
            }

            target_length = VfsStringLength(target, sizeof target);

            if (target_length == 0U)
            {
                VfsNodeRelease(child);
                VfsNodeRelease(current);
                return VfsRefuse(VFS_ERROR_INVALID,
                                 "a symbolic link has an empty target and names nothing");
            }

            VfsNodeRelease(child);

            /*
             * A relative target is resolved against the directory holding the
             * link and an absolute one from the root, which is the whole of the
             * difference between the two. `current` is that directory, the link
             * having been found within it.
             */
            if (!VfsWalk(current, target, target_length, true, depth + 1U, &resolved))
            {
                VfsNodeRelease(current);
                return false;
            }

            child = resolved;
        }

        VfsNodeRelease(current);
        current = child;
    }

    if (asserts_directory && (current->type != VFS_NODE_DIRECTORY))
    {
        VfsNodeRelease(current);
        return VfsRefuse(VFS_ERROR_NOT_DIRECTORY,
                         "the path ends in a separator and asserts a directory, which it "
                         "does not name");
    }

    *result = current;
    return true;
}

/* The public entry points, which count what they resolve. */
bool VfsResolveCounted(const char *path, bool follow_last, VfsNode **node)
{
    size_t length;

    if (node == NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "there is nowhere to put the node");
    }

    if (!VfsPathLength(path, &length))
    {
        ++VfsResolveRefusedCount;
        return false;
    }

    if (path[0] != VFS_PATH_SEPARATOR)
    {
        ++VfsResolveRefusedCount;
        return VfsRefuse(VFS_ERROR_INVALID,
                         "the path is not absolute, and there is no working directory to "
                         "resolve it against before Phase 6");
    }

    if (!VfsWalk(NULL, path, length, follow_last, 0U, node))
    {
        ++VfsResolveRefusedCount;
        return false;
    }

    ++VfsResolvedCount;
    VfsSucceed();
    return true;
}

bool VfsResolve(const char *path, VfsNode **node)
{
    return VfsResolveCounted(path, true, node);
}

bool VfsResolveNoFollow(const char *path, VfsNode **node)
{
    return VfsResolveCounted(path, false, node);
}

/*
 * Resolves the directory that holds the last component of a path, and reports
 * that component where it stands within the path.
 *
 * This is what every operation that alters a directory needs: a name is created
 * or destroyed within its parent, and the parent must be a node so that the link
 * count and the size the operation alters are the ones every other holder sees.
 *
 * The prefix walked is the path up to and including the separator before the
 * final component, so it is never empty — for "/file" it is "/" — and it always
 * ends in a separator, which asserts that it names a directory.
 */
bool VfsResolveParent(const char *path, VfsNode **parent, const char **name,
                             size_t *length)
{
    size_t path_length;
    size_t first;

    if (!VfsPathLength(path, &path_length))
    {
        return false;
    }

    if (path[0] != VFS_PATH_SEPARATOR)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the path is not absolute");
    }

    if (path[path_length - 1U] == VFS_PATH_SEPARATOR)
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "the path ends in a separator and so names no final component");
    }

    first = path_length;

    while ((first > 0U) && (path[first - 1U] != VFS_PATH_SEPARATOR))
    {
        --first;
    }

    *name = &path[first];
    *length = path_length - first;

    if (*length > VFS_NAME_MAXIMUM)
    {
        return VfsRefuse(VFS_ERROR_TOO_LONG,
                         "the final component is longer than a name may be");
    }

    if (VfsComponentIsDot(*name, *length))
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "\".\" and \"..\" name directories that already exist and may "
                         "neither be created nor removed");
    }

    if (!VfsWalk(NULL, path, first, true, 0U, parent))
    {
        return false;
    }

    if ((*parent)->type != VFS_NODE_DIRECTORY)
    {
        VfsNodeRelease(*parent);
        return VfsRefuse(VFS_ERROR_NOT_DIRECTORY,
                         "what holds the final component of the path is not a directory");
    }

    return true;
}

/* Refuses where the mount a node belongs to may not be written. The test is made
 * once, here, rather than by each operation remembering to make it. */
bool VfsWritable(const VfsNode *node)
{
    if (node->mount->read_only)
    {
        return VfsRefuse(VFS_ERROR_READ_ONLY,
                         "the volume is mounted read-only and may not be altered");
    }

    return true;
}

