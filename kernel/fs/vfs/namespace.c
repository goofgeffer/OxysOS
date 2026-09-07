/*
 * File: kernel/fs/vfs/namespace.c
 * Purpose: Implements the operations that name a file rather than hold one open:
 *          the reading of its attributes, its truncation, the creation and
 *          removal of a directory, the insertion and removal of a name, the
 *          reading of a symbolic link, and the flushing of every mounted volume.
 * Key functions: VfsStat, VfsStatLink, VfsTruncate, VfsCreateDirectory,
 *          VfsUnlink, VfsRemoveDirectory, VfsLink, VfsReadLink, VfsSync.
 * References:
 *   - docs/storage/VFS.md, Sections 9 and 10: what each of these refuses and
 *     why — a directory removed while it still holds entries, a file destroyed
 *     while something still has it open, a link across a mount point.
 *   - IEEE Std 1003.1-2017, the definitions of `unlink`, `rmdir` and `link`: the
 *     conditions each must refuse, and the distinction between `stat` and
 *     `lstat` upon a symbolic link.
 *
 * Every one of these resolves the *parent* of the path and then acts upon a name
 * within it, because that is what the operations are: an alteration of a
 * directory. Resolving the path itself would give the node and lose the
 * directory holding it, which is the thing being altered.
 */

#include "internal.h"

#include <oxys/vfs.h>
#include <oxys/kernel.h>
#include <oxys/block.h>
#include <oxys/heap.h>
#include <oxys/buffer.h>

static bool VfsStatCommon(const char *path, VfsAttributes *attributes, bool follow)
{
    VfsNode *node = NULL;

    if (attributes == NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "there is nowhere to put the description");
    }

    if (!VfsResolveCounted(path, follow, &node))
    {
        return false;
    }

    VfsNodeAttributes(node, attributes);
    VfsNodeRelease(node);

    VfsSucceed();
    return true;
}

bool VfsStat(const char *path, VfsAttributes *attributes)
{
    return VfsStatCommon(path, attributes, true);
}

bool VfsStatLink(const char *path, VfsAttributes *attributes)
{
    return VfsStatCommon(path, attributes, false);
}

bool VfsTruncate(const char *path, uint64_t size)
{
    VfsNode *node = NULL;
    bool outcome;

    if (!VfsResolveCounted(path, true, &node))
    {
        return false;
    }

    if (node->type == VFS_NODE_DIRECTORY)
    {
        VfsNodeRelease(node);
        return VfsRefuse(VFS_ERROR_IS_DIRECTORY, "a directory has no size to set");
    }

    if (!VfsWritable(node))
    {
        VfsNodeRelease(node);
        return false;
    }

    if (node->mount->operations->truncate == NULL)
    {
        VfsNodeRelease(node);
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not truncate files");
    }

    outcome = node->mount->operations->truncate(node, size);
    VfsNodeRelease(node);

    if (outcome)
    {
        VfsSucceed();
    }

    return outcome;
}

bool VfsCreateDirectory(const char *path, uint16_t permissions)
{
    VfsNode *parent = NULL;
    const char *name = NULL;
    size_t length = 0U;
    uint64_t number = 0U;
    bool outcome;

    if (!VfsResolveParent(path, &parent, &name, &length))
    {
        return false;
    }

    if (!VfsWritable(parent))
    {
        VfsNodeRelease(parent);
        return false;
    }

    if (parent->mount->operations->create_directory == NULL)
    {
        VfsNodeRelease(parent);
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not create directories");
    }

    outcome = parent->mount->operations->create_directory(parent, name, length, permissions,
                                                          &number);
    VfsNodeRelease(parent);

    if (outcome)
    {
        VfsSucceed();
    }

    return outcome;
}

/*
 * Finds the final component of a path within its parent and produces both, so
 * that the operations that destroy a name may examine what they would destroy
 * before they destroy it.
 *
 * The child is produced as a node, and holding it is what makes the test for
 * being in use meaningful: a node held by anything beyond this resolution is a
 * file somebody has open.
 */
static bool VfsResolveForRemoval(const char *path, VfsNode **parent, VfsNode **child,
                                 const char **name, size_t *length)
{
    uint64_t number = 0U;

    if (!VfsResolveParent(path, parent, name, length))
    {
        return false;
    }

    if (!VfsWritable(*parent))
    {
        VfsNodeRelease(*parent);
        return false;
    }

    if (!(*parent)->mount->operations->lookup(*parent, *name, *length, &number))
    {
        VfsNodeRelease(*parent);
        return false;
    }

    *child = VfsNodeAcquire((*parent)->mount, number);

    if (*child == NULL)
    {
        VfsNodeRelease(*parent);
        return false;
    }

    /*
     * A file anything else holds is not destroyed.
     *
     * POSIX would keep such a file alive until its last descriptor closed, which
     * requires a list of files that have no name and are not yet gone, together
     * with the discipline that empties it after a machine has stopped. This
     * kernel has neither. The alternative to refusing is to free the inode and
     * the blocks beneath a descriptor still reading them, and to reissue that
     * inode to another file while the first is still being read from — which is
     * silent, and is corruption rather than a surprise.
     *
     * The reference this resolution took is the one that is expected; any beyond
     * it is somebody else's. A mount point is caught by the same test, its mount
     * holding the node as the directory it covers.
     */
    if ((*child)->references > 1U)
    {
        VfsNodeRelease(*child);
        VfsNodeRelease(*parent);
        return VfsRefuse(VFS_ERROR_BUSY,
                         "the file is open, or is a mount point, and this kernel does not "
                         "destroy a file that something still holds");
    }

    return true;
}

bool VfsUnlink(const char *path)
{
    VfsNode *parent = NULL;
    VfsNode *child = NULL;
    const char *name = NULL;
    size_t length = 0U;
    bool outcome;

    if (!VfsResolveForRemoval(path, &parent, &child, &name, &length))
    {
        return false;
    }

    if (child->type == VFS_NODE_DIRECTORY)
    {
        VfsNodeRelease(child);
        VfsNodeRelease(parent);
        return VfsRefuse(VFS_ERROR_IS_DIRECTORY,
                         "a directory is removed by VfsRemoveDirectory, which requires it to "
                         "be empty");
    }

    if (parent->mount->operations->unlink == NULL)
    {
        VfsNodeRelease(child);
        VfsNodeRelease(parent);
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not remove names");
    }

    /*
     * The child is released before the name is removed. The filesystem is about
     * to free the inode the node describes, and a node describing a freed inode
     * is exactly what must not survive the operation.
     */
    VfsNodeRelease(child);

    outcome = parent->mount->operations->unlink(parent, name, length);
    VfsNodeRelease(parent);

    if (outcome)
    {
        VfsSucceed();
    }

    return outcome;
}

bool VfsRemoveDirectory(const char *path)
{
    VfsNode *parent = NULL;
    VfsNode *child = NULL;
    const char *name = NULL;
    size_t length = 0U;
    bool outcome;

    if (!VfsResolveForRemoval(path, &parent, &child, &name, &length))
    {
        return false;
    }

    if (child->type != VFS_NODE_DIRECTORY)
    {
        VfsNodeRelease(child);
        VfsNodeRelease(parent);
        return VfsRefuse(VFS_ERROR_NOT_DIRECTORY, "the path does not name a directory");
    }

    if (parent->mount->operations->remove_directory == NULL)
    {
        VfsNodeRelease(child);
        VfsNodeRelease(parent);
        return VfsRefuse(VFS_ERROR_UNSUPPORTED,
                         "the filesystem does not remove directories");
    }

    VfsNodeRelease(child);

    outcome = parent->mount->operations->remove_directory(parent, name, length);
    VfsNodeRelease(parent);

    if (outcome)
    {
        VfsSucceed();
    }

    return outcome;
}

bool VfsLink(const char *existing, const char *name)
{
    VfsNode *target = NULL;
    VfsNode *parent = NULL;
    const char *component = NULL;
    size_t length = 0U;
    bool outcome;

    /*
     * The existing path is resolved without following a symbolic link standing
     * last: a link to a link is a thing a system may hold, and `link()` names
     * the file the path names rather than the file that path leads to.
     */
    if (!VfsResolveCounted(existing, false, &target))
    {
        return false;
    }

    if (target->type == VFS_NODE_DIRECTORY)
    {
        VfsNodeRelease(target);
        return VfsRefuse(VFS_ERROR_IS_DIRECTORY,
                         "a directory may not be given a second name, two paths to one "
                         "directory making a cycle in what must be a tree");
    }

    if (!VfsResolveParent(name, &parent, &component, &length))
    {
        VfsNodeRelease(target);
        return false;
    }

    /*
     * Both names must lie upon one volume. A directory entry names an inode of
     * the volume the directory belongs to, and there is no number one volume
     * could write that would name a file upon another.
     */
    if (parent->mount != target->mount)
    {
        VfsNodeRelease(parent);
        VfsNodeRelease(target);
        return VfsRefuse(VFS_ERROR_CROSSES_MOUNT,
                         "a name and the file it names must lie upon one volume");
    }

    if (!VfsWritable(parent))
    {
        VfsNodeRelease(parent);
        VfsNodeRelease(target);
        return false;
    }

    if (parent->mount->operations->link == NULL)
    {
        VfsNodeRelease(parent);
        VfsNodeRelease(target);
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not give further names");
    }

    outcome = parent->mount->operations->link(parent, component, length, target);
    VfsNodeRelease(parent);
    VfsNodeRelease(target);

    if (outcome)
    {
        VfsSucceed();
    }

    return outcome;
}

bool VfsReadLink(const char *path, char *target, size_t capacity)
{
    VfsNode *node = NULL;
    bool outcome;

    if ((target == NULL) || (capacity == 0U))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "there is nowhere to put the target");
    }

    if (!VfsResolveCounted(path, false, &node))
    {
        return false;
    }

    if (node->type != VFS_NODE_SYMBOLIC_LINK)
    {
        VfsNodeRelease(node);
        return VfsRefuse(VFS_ERROR_INVALID, "the path does not name a symbolic link");
    }

    if (node->mount->operations->read_link == NULL)
    {
        VfsNodeRelease(node);
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not read symbolic links");
    }

    outcome = node->mount->operations->read_link(node, target, capacity);
    VfsNodeRelease(node);

    if (outcome)
    {
        VfsSucceed();
    }

    return outcome;
}

bool VfsSync(void)
{
    bool outcome = true;

    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        VfsMount *const mount = &VfsMounts[index];

        if (mount->mounted && (mount->operations->sync != NULL))
        {
            if (!mount->operations->sync(mount))
            {
                outcome = false;
            }
        }
    }

    /*
     * The buffer cache is written back after the filesystems, and not before: a
     * filesystem's sync is what puts its outstanding structures into the cache,
     * and flushing the cache first would leave exactly those behind.
     */
    if (!BufferSync())
    {
        outcome = VfsRefuse(VFS_ERROR_MEDIUM, "the buffer cache could not be written back");
    }

    return outcome;
}

