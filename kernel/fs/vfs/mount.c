/*
 * File: kernel/fs/vfs/mount.c
 * Purpose: Implements the registry of filesystem types and the mount table that
 *          joins several volumes into one tree: the registration of a type, the
 *          mounting of a volume upon a node, the refusals that keep a volume
 *          from being withdrawn while something still holds it, and the mounting
 *          of a root.
 * Key functions: VfsInitialise, VfsFindFilesystem, VfsRegisterFilesystem,
 *          VfsMountAt, VfsMountCount, VfsRootIsMounted, VfsMountVolume,
 *          VfsMountIsBusy, VfsUnmount, VfsMountRoot.
 * References:
 *   - docs/storage/VFS.md, Section 5: a mount is found through the node it
 *     covers and never through a path prefix — which is the decision the obvious
 *     alternative gets silently wrong, a prefix match calling `/usrlocal` a path
 *     within a volume mounted at `/usr`.
 *   - The same, Section 11: a volume opened for writing is marked unclean before
 *     anything else is written to it, a kernel that marked it upon unmounting
 *     recording only the mounts that ended well.
 *
 * Four of a filesystem's sixteen operations are not optional — mount, unmount,
 * read_node and lookup — and their absence is refused at registration rather
 * than at the first call, nothing being reachable without them.
 */

#include "internal.h"

#include <oxys/vfs.h>
#include <oxys/kernel.h>
#include <oxys/block.h>
#include <oxys/heap.h>

void VfsInitialise(void)
{
    for (size_t index = 0U; index < VFS_FILESYSTEM_CAPACITY; ++index)
    {
        VfsFilesystems[index] = (VfsFilesystem){ 0 };
    }

    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        VfsMounts[index] = (VfsMount){ 0 };
    }

    for (size_t index = 0U; index < VFS_NODE_CAPACITY; ++index)
    {
        VfsNodes[index] = (VfsNode){ 0 };
    }

    for (size_t index = 0U; index < VFS_FILE_CAPACITY; ++index)
    {
        VfsFiles[index] = (VfsFile){ 0 };
    }

    VfsRootMount = NULL;
    VfsSucceed();
}

VfsFilesystem *VfsFindFilesystem(const char *name)
{
    for (size_t index = 0U; index < VFS_FILESYSTEM_CAPACITY; ++index)
    {
        VfsFilesystem *const filesystem = &VfsFilesystems[index];

        if (filesystem->registered &&
            VfsSameString(filesystem->name, name, VFS_TYPE_NAME_MAXIMUM + 1U))
        {
            return filesystem;
        }
    }

    return NULL;
}

bool VfsRegisterFilesystem(const char *name, const VfsFilesystemOperations *operations)
{
    VfsFilesystem *available = NULL;
    size_t length;

    if ((name == NULL) || (operations == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "no name or no operations were given");
    }

    length = VfsStringLength(name, VFS_TYPE_NAME_MAXIMUM + 1U);

    if ((length == 0U) || (length > VFS_TYPE_NAME_MAXIMUM))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the name of the filesystem is unusable");
    }

    /*
     * The four operations without which nothing can be reached at all. The rest
     * are optional and their absence is refused where they are called; these
     * cannot be, because a mount that could not read its root would be a mount
     * of nothing.
     */
    if ((operations->mount == NULL) || (operations->unmount == NULL) ||
        (operations->read_node == NULL) || (operations->lookup == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "a filesystem must supply mount, unmount, read_node and lookup");
    }

    if (VfsFindFilesystem(name) != NULL)
    {
        return VfsRefuse(VFS_ERROR_EXISTS, "a filesystem of that name is registered already");
    }

    for (size_t index = 0U; index < VFS_FILESYSTEM_CAPACITY; ++index)
    {
        if (!VfsFilesystems[index].registered)
        {
            available = &VfsFilesystems[index];
            break;
        }
    }

    if (available == NULL)
    {
        return VfsRefuse(VFS_ERROR_NO_RESOURCE, "the filesystem table is full");
    }

    VfsCopyString(available->name, name, sizeof available->name);
    available->operations = operations;
    available->registered = true;

    VfsSucceed();
    return true;
}

/* ---------------------------------------------------------------------------
 * Mounting.
 * ------------------------------------------------------------------------- */

VfsMount *VfsMountAt(size_t index)
{
    if ((index >= VFS_MOUNT_CAPACITY) || (!VfsMounts[index].mounted))
    {
        return NULL;
    }

    return &VfsMounts[index];
}

size_t VfsMountCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        if (VfsMounts[index].mounted)
        {
            ++count;
        }
    }

    return count;
}

bool VfsRootIsMounted(void)
{
    return VfsRootMount != NULL;
}

/* Whether a device already carries a mount. Two mounts of one device would hold
 * two superblocks of one volume, and each would allocate blocks without regard
 * to what the other had taken. */
static bool VfsDeviceIsMounted(const BlockDevice *device)
{
    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        if (VfsMounts[index].mounted && (VfsMounts[index].device == device))
        {
            return true;
        }
    }

    return false;
}

bool VfsMountVolume(const char *device_name, const char *point, const char *type,
                    bool read_only)
{
    VfsFilesystem *filesystem;
    BlockDevice *device;
    VfsMount *mount = NULL;
    VfsNode *covered = NULL;
    VfsNode *root;
    size_t point_length;

    if ((device_name == NULL) || (point == NULL) || (type == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "no device, no mount point, or no type was given");
    }

    if (!VfsPathLength(point, &point_length))
    {
        return false;
    }

    filesystem = VfsFindFilesystem(type);

    if (filesystem == NULL)
    {
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "no filesystem of that type is registered");
    }

    device = BlockFindByName(device_name);

    if (device == NULL)
    {
        return VfsRefuse(VFS_ERROR_NOT_FOUND, "no block device bears that name");
    }

    if (VfsDeviceIsMounted(device))
    {
        return VfsRefuse(VFS_ERROR_BUSY, "the device is mounted already");
    }

    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        if (!VfsMounts[index].mounted)
        {
            mount = &VfsMounts[index];
            break;
        }
    }

    if (mount == NULL)
    {
        return VfsRefuse(VFS_ERROR_NO_RESOURCE, "the mount table is full");
    }

    /*
     * The first mount is the root and must be made at "/": until it stands there
     * is no tree for a path to be resolved within, so a mount anywhere else
     * would have nowhere to attach.
     */
    if (VfsRootMount == NULL)
    {
        if ((point_length != 1U) || (point[0] != VFS_PATH_SEPARATOR))
        {
            return VfsRefuse(VFS_ERROR_INVALID,
                             "the first mount must be made at the root, nothing else being "
                             "reachable before it");
        }
    }
    else
    {
        if (!VfsWalk(NULL, point, point_length, true, 0U, &covered))
        {
            return false;
        }

        if (covered->type != VFS_NODE_DIRECTORY)
        {
            VfsNodeRelease(covered);
            return VfsRefuse(VFS_ERROR_NOT_DIRECTORY, "a mount point must be a directory");
        }

        if (covered->mounted != NULL)
        {
            VfsNodeRelease(covered);
            return VfsRefuse(VFS_ERROR_BUSY,
                             "a volume is mounted there already, and this kernel does not "
                             "stack mounts");
        }
    }

    *mount = (VfsMount){ 0 };
    VfsCopyString(mount->point, point, sizeof mount->point);
    VfsCopyString(mount->type, type, sizeof mount->type);
    mount->operations = filesystem->operations;
    mount->device = device;
    mount->read_only = read_only;

    if (!mount->operations->mount(mount, device, read_only))
    {
        VfsNodeRelease(covered);
        *mount = (VfsMount){ 0 };
        return false;
    }

    /*
     * A volume the filesystem judges unwritable is read-only whatever was asked
     * for here, and a read-only mount of a writable volume is read-only because
     * it was asked for. The two are distinct conditions and the mount is the
     * disjunction of them.
     */
    if (read_only)
    {
        mount->read_only = true;
    }

    mount->mounted = true;

    root = VfsNodeAcquire(mount, mount->root_number);

    if ((root == NULL) || (root->type != VFS_NODE_DIRECTORY))
    {
        if (root != NULL)
        {
            (void)VfsRefuse(VFS_ERROR_NOT_DIRECTORY,
                            "the root of the volume is not a directory");
            VfsNodeRelease(root);
        }

        (void)mount->operations->unmount(mount);
        mount->mounted = false;
        VfsNodeRelease(covered);
        *mount = (VfsMount){ 0 };
        return false;
    }

    mount->root = root;
    mount->covered = covered;

    if (covered != NULL)
    {
        covered->mounted = mount;
    }
    else
    {
        VfsRootMount = mount;
    }

    VfsSucceed();
    return true;
}

/* Whether any node of a mount is held by anything but the mount itself. */
bool VfsMountIsBusy(const VfsMount *mount)
{
    for (size_t index = 0U; index < VFS_FILE_CAPACITY; ++index)
    {
        if (VfsFiles[index].open && (VfsFiles[index].node->mount == mount))
        {
            return true;
        }
    }

    for (size_t index = 0U; index < VFS_NODE_CAPACITY; ++index)
    {
        const VfsNode *const node = &VfsNodes[index];

        if ((!node->in_use) || (node->mount != mount))
        {
            continue;
        }

        /*
         * The root is held once by the mount itself, which is not a reason to
         * refuse. Any other node in use, or a root held more than once, is
         * somebody's reference and withdrawing the mount would leave it
         * addressing a volume that no longer exists.
         */
        if (node == mount->root)
        {
            if (node->references > 1U)
            {
                return true;
            }
        }
        else
        {
            return true;
        }
    }

    /* A volume mounted within this one is held by its own mount, whose covered
     * node belongs to this mount. */
    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        const VfsMount *const other = &VfsMounts[index];

        if (other->mounted && (other != mount) && (other->covered != NULL) &&
            (other->covered->mount == mount))
        {
            return true;
        }
    }

    return false;
}

bool VfsUnmount(const char *point)
{
    VfsMount *mount = NULL;
    size_t point_length;

    if (!VfsPathLength(point, &point_length))
    {
        return false;
    }

    for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
    {
        if (VfsMounts[index].mounted &&
            VfsSameString(VfsMounts[index].point, point, VFS_PATH_MAXIMUM + 1U))
        {
            mount = &VfsMounts[index];
            break;
        }
    }

    if (mount == NULL)
    {
        return VfsRefuse(VFS_ERROR_NOT_FOUND, "nothing is mounted at that point");
    }

    /*
     * The covered node of a mount within this one is a node of this mount, so
     * VfsMountIsBusy already refuses that case; it is stated there rather than
     * here because it is one instance of the same rule.
     */
    if (VfsMountIsBusy(mount))
    {
        return VfsRefuse(VFS_ERROR_BUSY,
                         "something upon the volume is still held: an open file, or another "
                         "volume mounted within it");
    }

    if (mount->operations->sync != NULL)
    {
        (void)mount->operations->sync(mount);
    }

    /*
     * The mount's own references go before the filesystem is told to release the
     * volume, and not after. Releasing a node calls the filesystem's
     * release_node, which is entitled to look at whatever the volume's own
     * description holds; doing it after unmount would be reading a description
     * the filesystem had just given back.
     */
    VfsNodeRelease(mount->root);

    if (mount->covered != NULL)
    {
        mount->covered->mounted = NULL;
    }

    if (!mount->operations->unmount(mount))
    {
        return false;
    }

    if (mount->covered != NULL)
    {
        VfsNodeRelease(mount->covered);
    }

    if (VfsRootMount == mount)
    {
        VfsRootMount = NULL;
    }

    *mount = (VfsMount){ 0 };

    VfsSucceed();
    return true;
}

bool VfsMountRoot(const char *type, bool read_only)
{
    const size_t count = BlockDeviceCount();

    if (VfsRootMount != NULL)
    {
        return VfsRefuse(VFS_ERROR_BUSY, "a volume is mounted at the root already");
    }

    for (size_t index = 0U; index < count; ++index)
    {
        const BlockDevice *const device = BlockDeviceAt(index);

        if (device == NULL)
        {
            break;
        }

        if (VfsMountVolume(device->name, "/", type, read_only))
        {
            return true;
        }
    }

    return VfsRefuse(VFS_ERROR_NOT_FOUND,
                     "no block device carries a volume this kernel can mount");
}

/* ---------------------------------------------------------------------------
 * Open files.
 * ------------------------------------------------------------------------- */

