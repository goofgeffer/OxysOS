/*
 * File: kernel/fs/vfs.c
 * Purpose: Implements the virtual filesystem layer: the registry of filesystem
 *          types, the mount table that joins several volumes into one tree, the
 *          node cache that gives one file one identity however many callers
 *          reach it, the resolution of a path across mount points and through
 *          symbolic links, and the open file with a position that advances.
 * Key functions: VfsInitialise, VfsRegisterFilesystem, VfsMountVolume,
 *          VfsUnmount, VfsMountRoot, VfsResolve, VfsResolveNoFollow,
 *          VfsNodeRelease, VfsOpen, VfsClose, VfsRead, VfsWrite, VfsSeek,
 *          VfsReadDirectory, VfsStat, VfsStatLink, VfsTruncate,
 *          VfsCreateDirectory, VfsRemoveDirectory, VfsUnlink, VfsLink,
 *          VfsReadLink, VfsSync, VfsReport.
 * References:
 *   - docs/storage/VFS.md: the design of this layer, the reasons for its shape,
 *     and what it does not promise.
 *   - IEEE Std 1003.1-2017, Section 4.13 (Pathname Resolution): the rules the
 *     walker below implements.
 *   - The same, `open()`, `lseek()`, `unlink()`, `rmdir()`, `link()`: the
 *     behaviour of the operations, including that a seek beyond the end of a
 *     file is permitted and that writing there leaves a hole.
 *   - Ritchie, D. M. and Thompson, K., "The UNIX Time-Sharing System",
 *     Communications of the ACM 17(7), 1974, Section 3.4: the mount as the
 *     replacement of a leaf of one tree by the root of another.
 *
 * Design note. There is no string prefix matching anywhere in this file. A mount
 * is found through the *node* it covers and not through the path it was mounted
 * at, which is the difference between a layer that composes a tree and one that
 * rewrites paths. The path a mount was made at is retained for a report and for
 * nothing else. Matching by prefix appears to work and then fails in three ways
 * that have no remedy within it: a symbolic link whose target crosses a mount
 * point is not a path any prefix describes; ".." leaving a mounted volume must
 * arrive at the parent of the mount point rather than at the volume's own root,
 * and the prefix has no way to know it has left; and a path reaching one
 * directory by two routes would be matched against one prefix and not the other.
 *
 * Concurrency. This implementation is not yet safe against concurrent access.
 * From sub-task 6.13 the mount table, the node table and the open file table each
 * require a lock, and a node's reference count must be adjusted atomically. The
 * open file table additionally becomes per-process in sub-task 6.9: a descriptor
 * is an index into a process's own table, and the description it names is shared
 * between the processes a fork produced. Nothing here assumes otherwise; the
 * table is simply global while there is one thread of control.
 */

#include <oxys/vfs.h>
#include <oxys/buffer.h>
#include <oxys/kernel.h>

/* ---------------------------------------------------------------------------
 * The tables.
 *
 * Every one is a fixed array. A filesystem layer that drew its own structures
 * from the heap could exhaust it, and it would do so at exactly the moment
 * something needed to write a diagnostic to a file. Only the filesystems'
 * private descriptions — a superblock, an inode — are allocated, and those are
 * bounded by these arrays.
 * ------------------------------------------------------------------------- */

static VfsFilesystem VfsFilesystems[VFS_FILESYSTEM_CAPACITY];
static VfsMount VfsMounts[VFS_MOUNT_CAPACITY];
static VfsNode VfsNodes[VFS_NODE_CAPACITY];

/*
 * One open file: the node it reached, the position that advances, and the flags
 * it was opened with.
 *
 * The position belongs to the open file and not to the node, which is why two
 * descriptors upon one file read independently of one another while writing to
 * the same bytes. Upon a directory the position is the filesystem's own cookie
 * rather than a byte offset; see VfsReadDirectory.
 */
typedef struct VfsFile
{
    VfsNode *node;
    uint64_t position;
    uint32_t flags;
    bool open;
} VfsFile;

static VfsFile VfsFiles[VFS_FILE_CAPACITY];

/* The mount at the root of the tree, through which every absolute path begins. */
static VfsMount *VfsRootMount;

/* ---------------------------------------------------------------------------
 * Refusals and accounting.
 * ------------------------------------------------------------------------- */

static VfsError VfsRefusalCode;
static const char *VfsRefusalReason = "no refusal has been recorded";

static uint64_t VfsResolvedCount;
static uint64_t VfsResolveRefusedCount;
static uint64_t VfsNodesReadCount;
static uint64_t VfsNodeHitCount;
static uint64_t VfsFilesOpenedCount;
static uint64_t VfsBytesReadCount;
static uint64_t VfsBytesWrittenCount;
static uint64_t VfsRefusalCount;

/*
 * Records a refusal and returns false, so that every refusal is one statement.
 *
 * The reason is retained by reference and is therefore always a string literal
 * or the stable diagnosis of a layer below — never a buffer, which would have
 * been reused by the time anything read it.
 */
bool VfsSetError(VfsError code, const char *reason)
{
    VfsRefusalCode = code;
    VfsRefusalReason = reason;
    ++VfsRefusalCount;
    return false;
}

/* The same, under the name every refusal within this file is written as. */
static bool VfsRefuse(VfsError code, const char *reason)
{
    return VfsSetError(code, reason);
}

/* Records that nothing has gone wrong, so that a stale diagnosis is not read as
 * a fresh one. */
static void VfsSucceed(void)
{
    VfsRefusalCode = VFS_ERROR_NONE;
    VfsRefusalReason = "no refusal has been recorded";
}

const char *VfsLastError(void)
{
    return VfsRefusalReason;
}

VfsError VfsLastErrorCode(void)
{
    return VfsRefusalCode;
}

const char *VfsErrorName(VfsError error)
{
    switch (error)
    {
    case VFS_ERROR_NONE:
        return "none";
    case VFS_ERROR_NOT_FOUND:
        return "not found";
    case VFS_ERROR_EXISTS:
        return "exists";
    case VFS_ERROR_NOT_DIRECTORY:
        return "not a directory";
    case VFS_ERROR_IS_DIRECTORY:
        return "is a directory";
    case VFS_ERROR_NOT_EMPTY:
        return "not empty";
    case VFS_ERROR_READ_ONLY:
        return "read-only";
    case VFS_ERROR_INVALID:
        return "invalid";
    case VFS_ERROR_TOO_LONG:
        return "too long";
    case VFS_ERROR_TOO_MANY_LINKS:
        return "too many symbolic links";
    case VFS_ERROR_NO_SPACE:
        return "no space upon the volume";
    case VFS_ERROR_NO_RESOURCE:
        return "no resource within the kernel";
    case VFS_ERROR_BUSY:
        return "busy";
    case VFS_ERROR_CROSSES_MOUNT:
        return "crosses a mount";
    case VFS_ERROR_UNSUPPORTED:
        return "unsupported";
    case VFS_ERROR_MEDIUM:
        return "the medium failed";
    default:
        break;
    }

    return "unknown";
}

const char *VfsNodeTypeName(VfsNodeType type)
{
    switch (type)
    {
    case VFS_NODE_REGULAR:
        return "regular file";
    case VFS_NODE_DIRECTORY:
        return "directory";
    case VFS_NODE_SYMBOLIC_LINK:
        return "symbolic link";
    case VFS_NODE_CHARACTER_DEVICE:
        return "character device";
    case VFS_NODE_BLOCK_DEVICE:
        return "block device";
    case VFS_NODE_FIFO:
        return "FIFO";
    case VFS_NODE_SOCKET:
        return "socket";
    case VFS_NODE_UNKNOWN:
    default:
        break;
    }

    return "unknown";
}

/* ---------------------------------------------------------------------------
 * Strings.
 *
 * There is no C library before Phase 7, so the three operations this file needs
 * are written here. Each is bounded: an unterminated string is a bug somewhere
 * else, and running off the end of one would turn that bug into a fault in this
 * file, where it would be diagnosed against the wrong subsystem.
 * ------------------------------------------------------------------------- */

static size_t VfsStringLength(const char *string, size_t capacity)
{
    size_t length = 0U;

    while ((length < capacity) && (string[length] != '\0'))
    {
        ++length;
    }

    return length;
}

static bool VfsSameString(const char *left, const char *right, size_t capacity)
{
    for (size_t index = 0U; index < capacity; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }

        if (left[index] == '\0')
        {
            return true;
        }
    }

    return false;
}

static void VfsCopyString(char *destination, const char *source, size_t capacity)
{
    size_t index = 0U;

    while (((index + 1U) < capacity) && (source[index] != '\0'))
    {
        destination[index] = source[index];
        ++index;
    }

    destination[index] = '\0';
}

/* ---------------------------------------------------------------------------
 * The node cache.
 *
 * A node is the identity of a file within the kernel: two callers reaching one
 * file by any route hold one node. That is a correctness requirement and not a
 * convenience. Were each caller to hold a copy of the file's description, a
 * write through one that extended the file would leave the other's copy holding
 * the old size and the old block pointers, and the next write through that copy
 * would restore them — truncating the file and orphaning every block the first
 * write allocated, silently.
 *
 * A node whose last reference goes is released at once rather than retained.
 * This is a table of nodes in use and not a cache of nodes recently used, and
 * the distinction is deliberate: a retained node is a description of a file that
 * may since have been destroyed and its inode reissued to another file, and
 * nothing here would know. The cost is that opening the same file twice in
 * succession reads its inode twice; those reads are served by the buffer cache
 * of sub-task 4.6, so the cost is the decoding and not the medium.
 * ------------------------------------------------------------------------- */

static void VfsNodeHold(VfsNode *node)
{
    ++node->references;
}

/*
 * Produces the node for a file, reading it where it is not already held, and
 * takes a reference upon it.
 */
static VfsNode *VfsNodeAcquire(VfsMount *mount, uint64_t number)
{
    VfsNode *available = NULL;

    for (size_t index = 0U; index < VFS_NODE_CAPACITY; ++index)
    {
        VfsNode *const node = &VfsNodes[index];

        if (node->in_use)
        {
            if ((node->mount == mount) && (node->number == number))
            {
                VfsNodeHold(node);
                ++VfsNodeHitCount;
                return node;
            }
        }
        else if (available == NULL)
        {
            available = node;
        }
    }

    if (available == NULL)
    {
        (void)VfsRefuse(VFS_ERROR_NO_RESOURCE,
                        "every node of the filesystem layer is in use; a caller has not "
                        "released one");
        return NULL;
    }

    /*
     * The slot is cleared before the filesystem fills it, so that a filesystem
     * which sets only some of the neutral fields cannot leave the previous
     * occupant's values in the rest.
     */
    *available = (VfsNode){ 0 };
    available->mount = mount;
    available->number = number;
    available->references = 1U;
    available->in_use = true;

    if (!mount->operations->read_node(mount, number, available))
    {
        available->in_use = false;
        available->references = 0U;
        return NULL;
    }

    ++VfsNodesReadCount;
    return available;
}

void VfsNodeRelease(VfsNode *node)
{
    if (node == NULL)
    {
        return;
    }

    if ((!node->in_use) || (node->references == 0U))
    {
        /*
         * Releasing a node nobody holds is a defect in the caller, and the next
         * release of the node somebody does hold would free it beneath them. It
         * is reported rather than ignored, for the reason KernelFree reports a
         * double release.
         */
        KernelWriteString("VFS: a node that was not held has been released.\n");
        return;
    }

    --node->references;

    if (node->references > 0U)
    {
        return;
    }

    if (node->mount->operations->release_node != NULL)
    {
        node->mount->operations->release_node(node);
    }

    node->in_use = false;
    node->context = NULL;
    node->mounted = NULL;
}

size_t VfsNodesHeld(void)
{
    size_t held = 0U;

    for (size_t index = 0U; index < VFS_NODE_CAPACITY; ++index)
    {
        if (VfsNodes[index].in_use)
        {
            ++held;
        }
    }

    return held;
}

void VfsNodeAttributes(const VfsNode *node, VfsAttributes *attributes)
{
    if ((node == NULL) || (attributes == NULL))
    {
        return;
    }

    attributes->number = node->number;
    attributes->type = node->type;
    attributes->permissions = node->permissions;
    attributes->uid = node->uid;
    attributes->gid = node->gid;
    attributes->size = node->size;
    attributes->link_count = node->link_count;
    attributes->block_count = node->block_count;
    attributes->access_time = node->access_time;
    attributes->modify_time = node->modify_time;
    attributes->change_time = node->change_time;
    attributes->block_size = node->mount->block_size;
}

/* ---------------------------------------------------------------------------
 * Path resolution.
 * ------------------------------------------------------------------------- */

/* The length of a path, refusing one that is absent, empty, or not terminated
 * within the bound this kernel resolves. */
static bool VfsPathLength(const char *path, size_t *length)
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
static bool VfsWalk(VfsNode *start, const char *path, size_t length, bool follow_last,
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
static bool VfsResolveCounted(const char *path, bool follow_last, VfsNode **node)
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
static bool VfsResolveParent(const char *path, VfsNode **parent, const char **name,
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
static bool VfsWritable(const VfsNode *node)
{
    if (node->mount->read_only)
    {
        return VfsRefuse(VFS_ERROR_READ_ONLY,
                         "the volume is mounted read-only and may not be altered");
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * Filesystem types.
 * ------------------------------------------------------------------------- */

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

static VfsFilesystem *VfsFindFilesystem(const char *name)
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
static bool VfsMountIsBusy(const VfsMount *mount)
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

/* The open file a descriptor names, or null where the descriptor names none. */
static VfsFile *VfsFileOf(int descriptor)
{
    if ((descriptor < 0) || ((size_t)descriptor >= VFS_FILE_CAPACITY))
    {
        (void)VfsRefuse(VFS_ERROR_INVALID, "the descriptor is outside the table");
        return NULL;
    }

    if (!VfsFiles[descriptor].open)
    {
        (void)VfsRefuse(VFS_ERROR_INVALID, "the descriptor names no open file");
        return NULL;
    }

    return &VfsFiles[descriptor];
}

size_t VfsOpenFileCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < VFS_FILE_CAPACITY; ++index)
    {
        if (VfsFiles[index].open)
        {
            ++count;
        }
    }

    return count;
}

/*
 * Creates the file a path names, where it is absent, and produces its node.
 *
 * The parent is resolved and the creation performed through it, so that the link
 * count and size the creation alters are those of the node every other holder of
 * that directory sees.
 */
static VfsNode *VfsCreateAndAcquire(const char *path, uint16_t permissions)
{
    VfsNode *parent = NULL;
    VfsNode *node;
    const char *name = NULL;
    size_t length = 0U;
    uint64_t number = 0U;

    if (!VfsResolveParent(path, &parent, &name, &length))
    {
        return NULL;
    }

    if (!VfsWritable(parent))
    {
        VfsNodeRelease(parent);
        return NULL;
    }

    if (parent->mount->operations->create == NULL)
    {
        VfsNodeRelease(parent);
        (void)VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not create files");
        return NULL;
    }

    if (!parent->mount->operations->create(parent, name, length, VFS_NODE_REGULAR, permissions,
                                           &number))
    {
        VfsNodeRelease(parent);
        return NULL;
    }

    node = VfsNodeAcquire(parent->mount, number);
    VfsNodeRelease(parent);
    return node;
}

int VfsOpen(const char *path, uint32_t flags, uint16_t permissions)
{
    VfsFile *file = NULL;
    VfsNode *node = NULL;
    int descriptor = VFS_NO_DESCRIPTOR;

    if ((flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U)
    {
        (void)VfsRefuse(VFS_ERROR_INVALID,
                        "an open must ask to read or to write; one that asked for neither "
                        "could do nothing");
        return VFS_NO_DESCRIPTOR;
    }

    if (((flags & VFS_OPEN_EXCLUSIVE) != 0U) && ((flags & VFS_OPEN_CREATE) == 0U))
    {
        (void)VfsRefuse(VFS_ERROR_INVALID,
                        "an exclusive open is a condition upon a creation and means nothing "
                        "without one");
        return VFS_NO_DESCRIPTOR;
    }

    if (((flags & (VFS_OPEN_TRUNCATE | VFS_OPEN_APPEND)) != 0U) &&
        ((flags & VFS_OPEN_WRITE) == 0U))
    {
        (void)VfsRefuse(VFS_ERROR_INVALID,
                        "truncating and appending alter a file and require an open for "
                        "writing");
        return VFS_NO_DESCRIPTOR;
    }

    /*
     * A descriptor is reserved before the file is created. Creating it first and
     * then discovering that the table is full would leave a file upon the volume
     * that the caller was told it had failed to make.
     */
    for (size_t index = 0U; index < VFS_FILE_CAPACITY; ++index)
    {
        if (!VfsFiles[index].open)
        {
            file = &VfsFiles[index];
            descriptor = (int)index;
            break;
        }
    }

    if (file == NULL)
    {
        (void)VfsRefuse(VFS_ERROR_NO_RESOURCE, "every descriptor is in use");
        return VFS_NO_DESCRIPTOR;
    }

    if (VfsResolveCounted(path, (flags & VFS_OPEN_NO_FOLLOW) == 0U, &node))
    {
        if ((flags & (VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE)) ==
            (VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE))
        {
            VfsNodeRelease(node);
            (void)VfsRefuse(VFS_ERROR_EXISTS,
                            "the file exists and the open required that it should not");
            return VFS_NO_DESCRIPTOR;
        }
    }
    else if (((flags & VFS_OPEN_CREATE) != 0U) && (VfsRefusalCode == VFS_ERROR_NOT_FOUND))
    {
        node = VfsCreateAndAcquire(path, permissions);

        if (node == NULL)
        {
            return VFS_NO_DESCRIPTOR;
        }
    }
    else
    {
        return VFS_NO_DESCRIPTOR;
    }

    if (node->type == VFS_NODE_SYMBOLIC_LINK)
    {
        VfsNodeRelease(node);
        (void)VfsRefuse(VFS_ERROR_INVALID,
                        "the path names a symbolic link and the open refused to follow it; "
                        "a link is read, not opened");
        return VFS_NO_DESCRIPTOR;
    }

    if (((flags & VFS_OPEN_DIRECTORY) != 0U) && (node->type != VFS_NODE_DIRECTORY))
    {
        VfsNodeRelease(node);
        (void)VfsRefuse(VFS_ERROR_NOT_DIRECTORY,
                        "the open required a directory and the path names something else");
        return VFS_NO_DESCRIPTOR;
    }

    if ((node->type == VFS_NODE_DIRECTORY) && ((flags & VFS_OPEN_WRITE) != 0U))
    {
        VfsNodeRelease(node);
        (void)VfsRefuse(VFS_ERROR_IS_DIRECTORY,
                        "a directory is altered by creating and removing names within it, "
                        "not by writing to it");
        return VFS_NO_DESCRIPTOR;
    }

    if (((flags & VFS_OPEN_WRITE) != 0U) && (!VfsWritable(node)))
    {
        VfsNodeRelease(node);
        return VFS_NO_DESCRIPTOR;
    }

    if (((flags & VFS_OPEN_TRUNCATE) != 0U) && (node->size != 0U))
    {
        if (node->mount->operations->truncate == NULL)
        {
            VfsNodeRelease(node);
            (void)VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not truncate files");
            return VFS_NO_DESCRIPTOR;
        }

        if (!node->mount->operations->truncate(node, 0U))
        {
            VfsNodeRelease(node);
            return VFS_NO_DESCRIPTOR;
        }
    }

    file->node = node;
    file->position = 0U;
    file->flags = flags;
    file->open = true;

    ++VfsFilesOpenedCount;
    VfsSucceed();
    return descriptor;
}

bool VfsClose(int descriptor)
{
    VfsFile *const file = VfsFileOf(descriptor);

    if (file == NULL)
    {
        return false;
    }

    VfsNodeRelease(file->node);
    *file = (VfsFile){ 0 };

    VfsSucceed();
    return true;
}

bool VfsRead(int descriptor, void *buffer, uint64_t length, uint64_t *read)
{
    VfsFile *const file = VfsFileOf(descriptor);
    uint64_t transferred = 0U;

    if (read != NULL)
    {
        *read = 0U;
    }

    if (file == NULL)
    {
        return false;
    }

    if ((buffer == NULL) || (read == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "no buffer, or nowhere to report the count");
    }

    if ((file->flags & VFS_OPEN_READ) == 0U)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the file was not opened for reading");
    }

    if (file->node->type == VFS_NODE_DIRECTORY)
    {
        return VfsRefuse(VFS_ERROR_IS_DIRECTORY,
                         "a directory holds entries and is read by VfsReadDirectory");
    }

    if (file->node->mount->operations->read == NULL)
    {
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not read files");
    }

    if (!file->node->mount->operations->read(file->node, file->position, buffer, length,
                                             &transferred))
    {
        *read = transferred;
        file->position += transferred;
        VfsBytesReadCount += transferred;
        return false;
    }

    file->position += transferred;
    VfsBytesReadCount += transferred;
    *read = transferred;

    VfsSucceed();
    return true;
}

bool VfsWrite(int descriptor, const void *buffer, uint64_t length, uint64_t *written)
{
    VfsFile *const file = VfsFileOf(descriptor);
    uint64_t offset;
    uint64_t transferred = 0U;

    if (written != NULL)
    {
        *written = 0U;
    }

    if (file == NULL)
    {
        return false;
    }

    if ((buffer == NULL) || (written == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "no buffer, or nowhere to report the count");
    }

    if ((file->flags & VFS_OPEN_WRITE) == 0U)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the file was not opened for writing");
    }

    if (!VfsWritable(file->node))
    {
        return false;
    }

    if (file->node->mount->operations->write == NULL)
    {
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not write files");
    }

    /*
     * An appending write goes to the end of the file as it stands at this
     * moment, and not to where the position happens to be. That is the whole
     * purpose of the flag: two writers appending to one file must not overwrite
     * one another, which they would were the offset taken from a position each
     * had advanced independently.
     */
    offset = ((file->flags & VFS_OPEN_APPEND) != 0U) ? file->node->size : file->position;

    if (!file->node->mount->operations->write(file->node, offset, buffer, length, &transferred))
    {
        *written = transferred;
        file->position = offset + transferred;
        VfsBytesWrittenCount += transferred;
        return false;
    }

    file->position = offset + transferred;
    VfsBytesWrittenCount += transferred;
    *written = transferred;

    VfsSucceed();
    return true;
}

bool VfsSeek(int descriptor, int64_t offset, VfsSeekOrigin origin, uint64_t *position)
{
    VfsFile *const file = VfsFileOf(descriptor);
    uint64_t base;
    uint64_t result;

    if (file == NULL)
    {
        return false;
    }

    switch (origin)
    {
    case VFS_SEEK_SET:
        base = 0U;
        break;
    case VFS_SEEK_CURRENT:
        base = file->position;
        break;
    case VFS_SEEK_END:
        base = file->node->size;
        break;
    default:
        return VfsRefuse(VFS_ERROR_INVALID, "the seek names no origin this layer knows");
    }

    /*
     * The two directions are separated so that neither the sum nor the
     * difference can wrap. A negative offset is negated in the unsigned domain
     * before it is subtracted, INT64_MIN having no positive counterpart in the
     * signed one; the conversion is defined because the value converted is
     * within the range of uint64_t.
     */
    if (offset >= 0)
    {
        const uint64_t magnitude = (uint64_t)offset;

        if (magnitude > (UINT64_MAX - base))
        {
            return VfsRefuse(VFS_ERROR_INVALID,
                             "the seek would place the position beyond what an offset can "
                             "express");
        }

        result = base + magnitude;
    }
    else
    {
        const uint64_t magnitude = (~(uint64_t)offset) + 1U;

        if (magnitude > base)
        {
            return VfsRefuse(VFS_ERROR_INVALID,
                             "the seek would place the position before the beginning of the "
                             "file");
        }

        result = base - magnitude;
    }

    file->position = result;

    if (position != NULL)
    {
        *position = result;
    }

    VfsSucceed();
    return true;
}

bool VfsTell(int descriptor, uint64_t *position)
{
    const VfsFile *const file = VfsFileOf(descriptor);

    if (file == NULL)
    {
        return false;
    }

    if (position == NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "there is nowhere to report the position");
    }

    *position = file->position;

    VfsSucceed();
    return true;
}

bool VfsReadDirectory(int descriptor, VfsDirectoryEntry *entry, bool *end)
{
    VfsFile *const file = VfsFileOf(descriptor);

    if (end != NULL)
    {
        *end = false;
    }

    if (file == NULL)
    {
        return false;
    }

    if ((entry == NULL) || (end == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "there is nowhere to put the entry or to report the end");
    }

    if (file->node->type != VFS_NODE_DIRECTORY)
    {
        return VfsRefuse(VFS_ERROR_NOT_DIRECTORY, "the descriptor does not name a directory");
    }

    if ((file->flags & VFS_OPEN_READ) == 0U)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the directory was not opened for reading");
    }

    if (file->node->mount->operations->read_directory == NULL)
    {
        return VfsRefuse(VFS_ERROR_UNSUPPORTED, "the filesystem does not read directories");
    }

    /*
     * The position of a directory descriptor is the filesystem's own cookie and
     * not a byte offset. It is kept in the same field because it is the same
     * thing — where the next read begins — and because a caller that seeks a
     * directory to a cookie it was given earlier is doing what `seekdir` means.
     */
    if (!file->node->mount->operations->read_directory(file->node, &file->position, entry, end))
    {
        return false;
    }

    VfsSucceed();
    return true;
}

bool VfsFileAttributes(int descriptor, VfsAttributes *attributes)
{
    const VfsFile *const file = VfsFileOf(descriptor);

    if (file == NULL)
    {
        return false;
    }

    if (attributes == NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "there is nowhere to put the description");
    }

    VfsNodeAttributes(file->node, attributes);

    VfsSucceed();
    return true;
}

/* ---------------------------------------------------------------------------
 * Operations upon a path.
 * ------------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------------
 * Accounting and reports.
 * ------------------------------------------------------------------------- */

uint64_t VfsPathsResolved(void)
{
    return VfsResolvedCount;
}

uint64_t VfsPathsRefused(void)
{
    return VfsResolveRefusedCount;
}

uint64_t VfsNodesRead(void)
{
    return VfsNodesReadCount;
}

uint64_t VfsNodeCacheHits(void)
{
    return VfsNodeHitCount;
}

uint64_t VfsFilesOpened(void)
{
    return VfsFilesOpenedCount;
}

uint64_t VfsBytesRead(void)
{
    return VfsBytesReadCount;
}

uint64_t VfsBytesWritten(void)
{
    return VfsBytesWrittenCount;
}

uint64_t VfsRefusals(void)
{
    return VfsRefusalCount;
}

void VfsReport(void)
{
    KernelWriteString("VFS: ");

    if (VfsRootMount == NULL)
    {
        KernelWriteString("nothing is mounted.\n");
    }
    else
    {
        KernelWriteDecimal(VfsMountCount());
        KernelWriteString(" mount(s).\n");

        for (size_t index = 0U; index < VFS_MOUNT_CAPACITY; ++index)
        {
            const VfsMount *const mount = &VfsMounts[index];

            if (!mount->mounted)
            {
                continue;
            }

            KernelWriteString("  ");
            KernelWriteString(mount->point);
            KernelWriteString(" <- ");
            KernelWriteString(mount->device->name);
            KernelWriteString(" (");
            KernelWriteString(mount->type);
            KernelWriteString(", block ");
            KernelWriteDecimal(mount->block_size);
            KernelWriteString(mount->read_only ? ", read-only)\n" : ", writable)\n");
        }
    }

    KernelWriteString("VFS: nodes held ");
    KernelWriteDecimal(VfsNodesHeld());
    KernelWriteString(" of ");
    KernelWriteDecimal(VFS_NODE_CAPACITY);
    KernelWriteString(", files open ");
    KernelWriteDecimal(VfsOpenFileCount());
    KernelWriteString(" of ");
    KernelWriteDecimal(VFS_FILE_CAPACITY);
    KernelWriteString(".\n");

    KernelWriteString("VFS: paths resolved ");
    KernelWriteDecimal(VfsResolvedCount);
    KernelWriteString(", refused ");
    KernelWriteDecimal(VfsResolveRefusedCount);
    KernelWriteString("; nodes read ");
    KernelWriteDecimal(VfsNodesReadCount);
    KernelWriteString(", found held ");
    KernelWriteDecimal(VfsNodeHitCount);
    KernelWriteString(".\n");

    KernelWriteString("VFS: files opened ");
    KernelWriteDecimal(VfsFilesOpenedCount);
    KernelWriteString(", bytes read ");
    KernelWriteDecimal(VfsBytesReadCount);
    KernelWriteString(", written ");
    KernelWriteDecimal(VfsBytesWrittenCount);
    KernelWriteString(", refusals ");
    KernelWriteDecimal(VfsRefusalCount);
    KernelWriteString(".\n");
}

void VfsReportDirectory(const char *path)
{
    VfsDirectoryEntry entry;
    bool end = false;
    uint64_t count = 0U;
    const int descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U);

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        KernelWriteString("VFS: ");
        KernelWriteString(path);
        KernelWriteString(" could not be opened: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("VFS: contents of ");
    KernelWriteString(path);
    KernelWriteString(":\n");

    while (VfsReadDirectory(descriptor, &entry, &end) && (!end))
    {
        KernelWriteString("  ");
        KernelWriteDecimal(entry.number);
        KernelWriteString(" ");
        KernelWriteString(VfsNodeTypeName(entry.type));
        KernelWriteString(" ");
        KernelWriteString(entry.name);
        KernelWriteString("\n");
        ++count;
    }

    KernelWriteString("VFS: ");
    KernelWriteDecimal(count);
    KernelWriteString(" entries.\n");

    (void)VfsClose(descriptor);
}
