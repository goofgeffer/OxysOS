/*
 * File: kernel/fs/ext2_vfs.c
 * Purpose: Binds the EXT2 implementation of kernel/fs/ext2.c to the virtual
 *          filesystem layer: the operations vector, the translation between the
 *          format's mode and the layer's neutral node type, the mount that
 *          records upon a volume that this kernel has it open, and the
 *          translation of the format's refusals into the layer's codes.
 * Key functions: Ext2VfsInitialise, Ext2VfsMount, Ext2VfsUnmount,
 *          Ext2VfsReadNode, Ext2VfsReleaseNode, Ext2VfsLookup, Ext2VfsRead,
 *          Ext2VfsWrite, Ext2VfsTruncate, Ext2VfsReadLink,
 *          Ext2VfsReadDirectory, Ext2VfsCreate, Ext2VfsCreateDirectory,
 *          Ext2VfsLink, Ext2VfsUnlink, Ext2VfsRemoveDirectory, Ext2VfsSync.
 * References:
 *   - docs/storage/VFS.md, Section 3: the operations vector and the contracts
 *     common to every entry of it.
 *   - docs/storage/VFS.md, Section 8: the mark a mount leaves upon a volume it
 *     has opened for writing, and why it is made at the mount and not at the
 *     unmount.
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Superblock chapter: `s_state` is EXT2_VALID_FS when the volume was
 *     cleanly unmounted and EXT2_ERROR_FS otherwise, and `s_mnt_count` counts
 *     the mounts since the last check.
 *   - Linux kernel documentation, filesystems/ext2.rst: a volume is marked as
 *     not cleanly unmounted for as long as it is mounted for writing, so that a
 *     machine which stopped while it was open is discovered at the next mount.
 *   - The same, Table 4.2 and Defined i_mode Values: the file types of a
 *     directory entry and the formats of i_mode are numbered differently and
 *     both must be translated to the layer's neutral type.
 *
 * Design note. This file stands apart from `ext2.c` because the two answer
 * different questions. `ext2.c` answers what the format is: where a structure
 * lies, how its bytes are ordered, what makes a volume self-contradictory. This
 * file answers how that format is presented as one filesystem among several: it
 * holds the only code in the project that knows both that a node has a reference
 * count and that an inode has fifteen block pointers. Every operation here is
 * accordingly short, and where one is not, the length is translation and not
 * mechanism.
 *
 * Concurrency. Nothing here holds state of its own beyond the volume attached to
 * a mount and the inode attached to a node, both of which the layer above
 * governs the lifetime of. The locking that sub-task 6.8 introduces therefore
 * belongs in that layer and in `ext2.c`, not here.
 */

#include <oxys/ext2_vfs.h>
#include <oxys/ext2.h>
#include <oxys/vfs.h>
#include <oxys/buffer.h>
#include <oxys/heap.h>

/* ---------------------------------------------------------------------------
 * Reaching the format's structures from the layer's.
 *
 * A mount's context is the parsed superblock of its volume and a node's context
 * is the parsed inode of its file. Both are drawn from the kernel heap when they
 * are read and returned when they are released, and both are reached only
 * through these two accessors, so that the casts appear once each.
 * ------------------------------------------------------------------------- */

static Ext2Superblock *Ext2VfsVolumeOf(const VfsMount *mount)
{
    return (Ext2Superblock *)mount->context;
}

static Ext2Inode *Ext2VfsInodeOf(const VfsNode *node)
{
    return (Ext2Inode *)node->context;
}

/*
 * Records a refusal in the layer's terms, keeping the format's own words.
 *
 * The EXT2 implementation distinguishes its refusals in words and not by code —
 * a failed lookup and a directory whose records are malformed are both `false`
 * with a different sentence behind them — so the code assigned here is the
 * likeliest of the outcomes the operation admits and the sentence is the
 * authority. That is stated as a limitation in docs/storage/VFS.md, Section 12.6,
 * rather than concealed: a caller acting upon the code alone will occasionally
 * act upon the wrong one, and a caller printing the sentence never will.
 */
static bool Ext2VfsFail(VfsError code)
{
    return VfsSetError(code, Ext2LastError());
}

/*
 * The refusal of something that alters a volume.
 *
 * A volume with nothing left to give and a volume that could not be written are
 * distinguished here because they are the two a caller acts upon differently:
 * the first is a condition of the medium the caller may report to a person, and
 * the second is a fault. The test is a heuristic — the free counts are consulted
 * after the failure rather than the allocator being asked why it refused — and
 * it is right whenever the volume is genuinely full, which is the case it exists
 * for.
 */
static bool Ext2VfsFailWrite(const Ext2Superblock *superblock)
{
    if ((superblock->free_block_count == 0U) || (superblock->free_inode_count == 0U))
    {
        return Ext2VfsFail(VFS_ERROR_NO_SPACE);
    }

    return Ext2VfsFail(VFS_ERROR_MEDIUM);
}

/* ---------------------------------------------------------------------------
 * The translation of types.
 * ------------------------------------------------------------------------- */

/* The neutral type of the format held in the high four bits of i_mode. */
static VfsNodeType Ext2VfsTypeOfMode(uint16_t mode)
{
    switch (mode & EXT2_S_IFMT)
    {
    case EXT2_S_IFREG:
        return VFS_NODE_REGULAR;
    case EXT2_S_IFDIR:
        return VFS_NODE_DIRECTORY;
    case EXT2_S_IFLNK:
        return VFS_NODE_SYMBOLIC_LINK;
    case EXT2_S_IFCHR:
        return VFS_NODE_CHARACTER_DEVICE;
    case EXT2_S_IFBLK:
        return VFS_NODE_BLOCK_DEVICE;
    case EXT2_S_IFIFO:
        return VFS_NODE_FIFO;
    case EXT2_S_IFSOCK:
        return VFS_NODE_SOCKET;
    default:
        break;
    }

    return VFS_NODE_UNKNOWN;
}

/*
 * The format bits of i_mode for a neutral type, and zero where the type has
 * none. A zero return is what refuses a creation of something the format cannot
 * express, and is why the caller tests it rather than passing it on.
 */
static uint16_t Ext2VfsModeOfType(VfsNodeType type)
{
    switch (type)
    {
    case VFS_NODE_REGULAR:
        return EXT2_S_IFREG;
    case VFS_NODE_DIRECTORY:
        return EXT2_S_IFDIR;
    case VFS_NODE_SYMBOLIC_LINK:
        return EXT2_S_IFLNK;
    case VFS_NODE_CHARACTER_DEVICE:
        return EXT2_S_IFCHR;
    case VFS_NODE_BLOCK_DEVICE:
        return EXT2_S_IFBLK;
    case VFS_NODE_FIFO:
        return EXT2_S_IFIFO;
    case VFS_NODE_SOCKET:
        return EXT2_S_IFSOCK;
    case VFS_NODE_UNKNOWN:
    default:
        break;
    }

    return 0U;
}

/*
 * The neutral type of the file type a directory entry declares.
 *
 * These are numbered differently from the formats of i_mode — a directory is 2
 * here and 0x4000 there — which is the format's own arrangement and the reason
 * two translations exist rather than one. A volume declaring no file types
 * reports EXT2_FT_UNKNOWN for every entry, which becomes VFS_NODE_UNKNOWN, and
 * a caller that needs the type of such an entry must stat the name.
 */
static VfsNodeType Ext2VfsTypeOfFileType(uint8_t file_type)
{
    switch (file_type)
    {
    case EXT2_FT_REG_FILE:
        return VFS_NODE_REGULAR;
    case EXT2_FT_DIR:
        return VFS_NODE_DIRECTORY;
    case EXT2_FT_SYMLINK:
        return VFS_NODE_SYMBOLIC_LINK;
    case EXT2_FT_CHRDEV:
        return VFS_NODE_CHARACTER_DEVICE;
    case EXT2_FT_BLKDEV:
        return VFS_NODE_BLOCK_DEVICE;
    case EXT2_FT_FIFO:
        return VFS_NODE_FIFO;
    case EXT2_FT_SOCK:
        return VFS_NODE_SOCKET;
    case EXT2_FT_UNKNOWN:
    default:
        break;
    }

    return VFS_NODE_UNKNOWN;
}

/*
 * Brings a node's neutral fields into agreement with the inode beneath it.
 *
 * Every operation that alters a file alters the parsed inode in place — a write
 * changes the size and the block pointers, an insertion changes a directory's
 * size, a link changes a link count — and the node's own fields are a rendering
 * of those. Refreshing them is therefore not an optimisation but the second half
 * of every such operation: a node whose size disagreed with its inode would
 * bound the next read at the wrong place.
 */
static void Ext2VfsFillNode(VfsNode *node, const Ext2Inode *inode)
{
    node->type = Ext2VfsTypeOfMode(inode->mode);
    node->permissions = inode->mode & EXT2_PERMISSION_MASK;
    node->uid = inode->uid;
    node->gid = inode->gid;
    node->size = inode->size;
    node->link_count = inode->link_count;
    node->block_count = inode->sector_count;
    node->access_time = inode->access_time;
    node->modify_time = inode->modify_time;
    node->change_time = inode->change_time;
}

/* ---------------------------------------------------------------------------
 * The mount.
 * ------------------------------------------------------------------------- */

/*
 * Reads the volume upon a device and prepares the mount.
 *
 * Beyond reading the superblock, the whole descriptor table is read and
 * validated here. It is the structure every inode and every block of the volume
 * is found through, and a table that is one descriptor short, or read from the
 * wrong block, yields descriptors that are individually plausible and a
 * filesystem that addresses the wrong blocks with confidence. Mounting is the
 * one moment at which that check costs nothing anybody will notice.
 *
 * A writable mount then records upon the volume that this kernel has it open, by
 * setting the state to EXT2_ERROR_FS and raising the mount count. The volume is
 * marked before anything is written to it, which is the whole point: a machine
 * that stops while the volume is open leaves that mark behind, so the next mount
 * reads a volume that was not cleanly unmounted and makes it read-only until a
 * check has been run over it. A kernel that marked the volume upon unmounting
 * would record only the mounts that ended well, which are exactly the ones that
 * need no record.
 */
static bool Ext2VfsMount(VfsMount *mount, BlockDevice *device, bool read_only)
{
    Ext2Superblock *volume = KernelAllocateZeroed(sizeof *volume);

    if (volume == NULL)
    {
        return VfsSetError(VFS_ERROR_NO_RESOURCE,
                           "there is no room in the kernel heap for the volume");
    }

    if (!Ext2ReadSuperblock(device, volume))
    {
        KernelFree(volume);
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    if (!Ext2VerifyGroupDescriptors(device, volume))
    {
        KernelFree(volume);
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    if (read_only)
    {
        volume->read_only = true;
    }

    mount->context = volume;
    mount->root_number = EXT2_ROOT_INODE;
    mount->block_size = volume->block_size;
    mount->read_only = volume->read_only;

    if (!volume->read_only)
    {
        /*
         * The volume is marked open by *clearing* the bit that says it was
         * cleanly unmounted, and not by setting the bit that says errors were
         * found in it. The two are distinct bits of one field and they say
         * different things: a volume that is merely open is intact, and a kernel
         * that recorded it as faulty would have `e2fsck` report errors upon a
         * disk that has none, and would erase the record of a volume that
         * genuinely had some by overwriting the field rather than masking it.
         */
        volume->state &= (uint16_t)~(uint16_t)EXT2_VALID_FS;

        /*
         * The count saturates rather than wrapping. A count that returned to
         * zero would tell the next check that the volume had never been
         * mounted, which is the one thing the field exists to contradict.
         */
        if (volume->mount_count < UINT16_MAX)
        {
            ++volume->mount_count;
        }

        if (!Ext2WriteSuperblock(device, volume) || !BufferSync())
        {
            KernelFree(volume);
            mount->context = NULL;
            return Ext2VfsFail(VFS_ERROR_MEDIUM);
        }
    }

    return true;
}

/*
 * Releases the volume, having first recorded upon it that it was cleanly
 * unmounted.
 *
 * The order is the reverse of the mount's and for the same reason: the state is
 * written and then forced to the medium, so that the mark of a clean unmount
 * cannot be sitting in the buffer cache of a machine that then stops. A failure
 * to write is reported but does not prevent the unmount from completing, there
 * being nothing useful to do with a mount whose volume has become unwritable but
 * to let go of it.
 */
static bool Ext2VfsUnmount(VfsMount *mount)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(mount);
    bool outcome = true;

    if (volume == NULL)
    {
        return VfsSetError(VFS_ERROR_INVALID, "the mount holds no volume");
    }

    if (!volume->read_only)
    {
        /* The mirror of the mount: the bit is set, and whatever else the field
         * held is left as it stands. */
        volume->state |= (uint16_t)EXT2_VALID_FS;

        if (!Ext2WriteSuperblock(mount->device, volume))
        {
            outcome = Ext2VfsFail(VFS_ERROR_MEDIUM);
        }
    }

    if (!BufferSync())
    {
        outcome = VfsSetError(VFS_ERROR_MEDIUM,
                              "the buffer cache could not be written back before the "
                              "volume was released");
    }

    KernelFree(volume);
    mount->context = NULL;

    return outcome;
}

static bool Ext2VfsSync(VfsMount *mount)
{
    (void)mount;

    /*
     * Every structure this implementation alters is written into the buffer
     * cache as it is altered — the superblock, the descriptors, the bitmaps, the
     * inodes and the data alike — so there is nothing outstanding here that is
     * not outstanding there. Flushing the cache is therefore the whole of the
     * work, and the layer above flushes it once for every mount rather than each
     * mount flushing it again.
     */
    if (!BufferSync())
    {
        return VfsSetError(VFS_ERROR_MEDIUM, "the buffer cache could not be written back");
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * Nodes.
 * ------------------------------------------------------------------------- */

static bool Ext2VfsReadNode(VfsMount *mount, uint64_t number, VfsNode *node)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(mount);
    Ext2Inode *inode;

    /*
     * An inode number is 32 bits upon this format. The layer's identifier is 64
     * so that a filesystem whose identifiers are wider may be added without the
     * layer changing; a number beyond what this format can express did not come
     * from this volume and is refused rather than truncated.
     */
    if (number > UINT32_MAX)
    {
        return VfsSetError(VFS_ERROR_INVALID,
                           "the identifier is beyond what an EXT2 inode number can express");
    }

    inode = KernelAllocateZeroed(sizeof *inode);

    if (inode == NULL)
    {
        return VfsSetError(VFS_ERROR_NO_RESOURCE,
                           "there is no room in the kernel heap for the inode");
    }

    if (!Ext2ReadInode(mount->device, volume, (uint32_t)number, inode))
    {
        KernelFree(inode);
        return Ext2VfsFail(VFS_ERROR_NOT_FOUND);
    }

    node->context = inode;
    Ext2VfsFillNode(node, inode);
    return true;
}

static void Ext2VfsReleaseNode(VfsNode *node)
{
    KernelFree(node->context);
    node->context = NULL;
}

static bool Ext2VfsLookup(VfsNode *directory, const char *name, size_t length,
                          uint64_t *number)
{
    Ext2DirectoryEntry entry;

    if (!Ext2DirectoryFind(directory->mount->device, Ext2VfsVolumeOf(directory->mount),
                           Ext2VfsInodeOf(directory), name, length, &entry))
    {
        return Ext2VfsFail(VFS_ERROR_NOT_FOUND);
    }

    *number = entry.inode;
    return true;
}

/* ---------------------------------------------------------------------------
 * Contents.
 * ------------------------------------------------------------------------- */

static bool Ext2VfsRead(VfsNode *node, uint64_t offset, void *buffer, uint64_t length,
                        uint64_t *read)
{
    if (!Ext2ReadFile(node->mount->device, Ext2VfsVolumeOf(node->mount), Ext2VfsInodeOf(node),
                      offset, buffer, length, read))
    {
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    return true;
}

static bool Ext2VfsWrite(VfsNode *node, uint64_t offset, const void *buffer, uint64_t length,
                         uint64_t *written)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(node->mount);
    Ext2Inode *const inode = Ext2VfsInodeOf(node);
    const bool outcome =
        Ext2WriteFile(node->mount->device, volume, inode, offset, buffer, length, written);

    /*
     * The node is refreshed whether the write succeeded or not. A partial write
     * is a partial write: the bytes before the failure are upon the volume and
     * the inode accounts for them, so a node left holding the old size would
     * report a file shorter than it is.
     */
    Ext2VfsFillNode(node, inode);

    if (!outcome)
    {
        return Ext2VfsFailWrite(volume);
    }

    return true;
}

static bool Ext2VfsTruncate(VfsNode *node, uint64_t size)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(node->mount);
    Ext2Inode *const inode = Ext2VfsInodeOf(node);
    const bool outcome = Ext2TruncateFile(node->mount->device, volume, inode, size);

    Ext2VfsFillNode(node, inode);

    if (!outcome)
    {
        return Ext2VfsFailWrite(volume);
    }

    return true;
}

static bool Ext2VfsReadLink(VfsNode *node, char *target, size_t capacity)
{
    if (!Ext2ReadSymbolicLink(node->mount->device, Ext2VfsVolumeOf(node->mount),
                              Ext2VfsInodeOf(node), target, capacity))
    {
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    return true;
}

/*
 * Produces one entry of a directory and advances the cookie past it.
 *
 * The cookie is the traversal's position — which block of the directory, and the
 * byte within it the next record begins at — packed into one quantity, the high
 * half being the block index and the low half the offset within the block. It is
 * opaque to the layer above, which is what allows it to be exactly this: a
 * caller that reads a directory in pieces continues from where it stopped
 * without the directory being traversed from its beginning again.
 *
 * The packing is sound because both halves are bounded well below what they are
 * given. The offset is a byte within a block, so it is under 4096; and the block
 * index of a file is bounded by what fifteen pointers reach, which upon a
 * 1 KiB volume is some sixteen million and upon a 4 KiB volume some sixteen
 * thousand million — both far below 2^32. An index that nevertheless exceeded it
 * is refused rather than truncated, a truncated index naming a different block
 * of the same directory and reporting entries twice.
 */
static bool Ext2VfsReadDirectory(VfsNode *directory, uint64_t *cookie,
                                 VfsDirectoryEntry *entry, bool *end)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry found;
    Ext2DirectoryStep step;

    cursor.directory = Ext2VfsInodeOf(directory);
    cursor.index = *cookie >> 32;
    cursor.offset = (uint32_t)(*cookie & UINT32_C(0xFFFFFFFF));

    step = Ext2DirectoryNext(directory->mount->device, Ext2VfsVolumeOf(directory->mount),
                             &cursor, &found);

    if (step == EXT2_DIRECTORY_FAILED)
    {
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    if (step == EXT2_DIRECTORY_END)
    {
        *end = true;
        return true;
    }

    if (cursor.index > UINT32_MAX)
    {
        return VfsSetError(VFS_ERROR_MEDIUM,
                           "the directory is longer than a position of this layer can name");
    }

    *cookie = (cursor.index << 32) | (uint64_t)cursor.offset;

    entry->number = found.inode;
    entry->type = Ext2VfsTypeOfFileType(found.file_type);

    for (size_t index = 0U; index < (sizeof entry->name); ++index)
    {
        entry->name[index] = found.name[index];

        if (found.name[index] == '\0')
        {
            break;
        }
    }

    *end = false;
    return true;
}

/* ---------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

/*
 * Whether a directory already holds a name.
 *
 * It is consulted before a creation so that an existing name is refused with the
 * code that says so. The insertion beneath refuses it too — a directory holding
 * one name twice makes the path to it ambiguous — but it refuses in words alone,
 * and a caller acting upon the code would be told only that the medium had
 * failed.
 */
static bool Ext2VfsNameExists(VfsNode *directory, const char *name, size_t length)
{
    Ext2DirectoryEntry entry;

    return Ext2DirectoryFind(directory->mount->device, Ext2VfsVolumeOf(directory->mount),
                             Ext2VfsInodeOf(directory), name, length, &entry);
}

static bool Ext2VfsCreate(VfsNode *directory, const char *name, size_t length,
                          VfsNodeType type, uint16_t permissions, uint64_t *number)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(directory->mount);
    Ext2Inode *const parent = Ext2VfsInodeOf(directory);
    const uint16_t format = Ext2VfsModeOfType(type);
    Ext2Inode created;
    bool outcome;

    if (format == 0U)
    {
        return VfsSetError(VFS_ERROR_INVALID, "the format asked for is not one EXT2 records");
    }

    if (format == EXT2_S_IFDIR)
    {
        return VfsSetError(VFS_ERROR_IS_DIRECTORY,
                           "a directory is created by create_directory, which writes the "
                           "\".\" and \"..\" that make it one");
    }

    if (Ext2VfsNameExists(directory, name, length))
    {
        return VfsSetError(VFS_ERROR_EXISTS, "the directory holds that name already");
    }

    outcome = Ext2CreateFile(directory->mount->device, volume, parent, name, length,
                             (uint16_t)(format | (permissions & EXT2_PERMISSION_MASK)),
                             &created);

    Ext2VfsFillNode(directory, parent);

    if (!outcome)
    {
        return Ext2VfsFailWrite(volume);
    }

    *number = created.number;
    return true;
}

static bool Ext2VfsCreateDirectory(VfsNode *directory, const char *name, size_t length,
                                   uint16_t permissions, uint64_t *number)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(directory->mount);
    Ext2Inode *const parent = Ext2VfsInodeOf(directory);
    Ext2Inode created;
    bool outcome;

    if (Ext2VfsNameExists(directory, name, length))
    {
        return VfsSetError(VFS_ERROR_EXISTS, "the directory holds that name already");
    }

    outcome = Ext2CreateDirectory(
        directory->mount->device, volume, parent, name, length,
        (uint16_t)(EXT2_S_IFDIR | (permissions & EXT2_PERMISSION_MASK)), &created);

    /*
     * The parent's link count rises by one for the ".." written into the child,
     * and its size may have risen by a block; both are in the inode and neither
     * is in the node until this call.
     */
    Ext2VfsFillNode(directory, parent);

    if (!outcome)
    {
        return Ext2VfsFailWrite(volume);
    }

    *number = created.number;
    return true;
}

static bool Ext2VfsLink(VfsNode *directory, const char *name, size_t length, VfsNode *target)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(directory->mount);
    Ext2Inode *const parent = Ext2VfsInodeOf(directory);
    Ext2Inode *const inode = Ext2VfsInodeOf(target);
    bool outcome;

    if (Ext2VfsNameExists(directory, name, length))
    {
        return VfsSetError(VFS_ERROR_EXISTS, "the directory holds that name already");
    }

    outcome = Ext2Link(directory->mount->device, volume, parent, name, length, inode);

    /* Both change: the directory gains a record, and the file gains a link. */
    Ext2VfsFillNode(directory, parent);
    Ext2VfsFillNode(target, inode);

    if (!outcome)
    {
        return Ext2VfsFailWrite(volume);
    }

    return true;
}

static bool Ext2VfsUnlink(VfsNode *directory, const char *name, size_t length)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(directory->mount);
    Ext2Inode *const parent = Ext2VfsInodeOf(directory);
    const bool outcome = Ext2Unlink(directory->mount->device, volume, parent, name, length);

    Ext2VfsFillNode(directory, parent);

    if (!outcome)
    {
        return Ext2VfsFail(VFS_ERROR_NOT_FOUND);
    }

    return true;
}

static bool Ext2VfsRemoveDirectory(VfsNode *directory, const char *name, size_t length)
{
    Ext2Superblock *const volume = Ext2VfsVolumeOf(directory->mount);
    Ext2Inode *const parent = Ext2VfsInodeOf(directory);
    Ext2DirectoryEntry entry;
    Ext2Inode target;
    bool empty = false;
    bool outcome;

    /*
     * Emptiness is established here, before the removal is attempted, so that a
     * directory holding names is refused with the code that says so. The removal
     * beneath refuses it as well; it refuses in words, and everything within the
     * directory becoming reachable by no path is a condition a caller must be
     * able to distinguish from a volume that failed.
     */
    if (!Ext2DirectoryFind(directory->mount->device, volume, parent, name, length, &entry))
    {
        return Ext2VfsFail(VFS_ERROR_NOT_FOUND);
    }

    if (!Ext2ReadInode(directory->mount->device, volume, entry.inode, &target))
    {
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    if (!Ext2DirectoryIsEmpty(directory->mount->device, volume, &target, &empty))
    {
        return Ext2VfsFail(VFS_ERROR_MEDIUM);
    }

    if (!empty)
    {
        return VfsSetError(VFS_ERROR_NOT_EMPTY,
                           "the directory holds names, and removing it would leave every one "
                           "of them reachable by no path");
    }

    outcome = Ext2RemoveDirectory(directory->mount->device, volume, parent, name, length);

    /* The parent loses the link the child's ".." held. */
    Ext2VfsFillNode(directory, parent);

    if (!outcome)
    {
        return Ext2VfsFailWrite(volume);
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * The vector, and its registration.
 * ------------------------------------------------------------------------- */

static const VfsFilesystemOperations Ext2VfsOperations = {
    Ext2VfsMount,      Ext2VfsUnmount,         Ext2VfsReadNode,  Ext2VfsReleaseNode,
    Ext2VfsLookup,     Ext2VfsRead,            Ext2VfsWrite,     Ext2VfsTruncate,
    Ext2VfsReadLink,   Ext2VfsReadDirectory,   Ext2VfsCreate,    Ext2VfsCreateDirectory,
    Ext2VfsLink,       Ext2VfsUnlink,          Ext2VfsRemoveDirectory,
    Ext2VfsSync
};

bool Ext2VfsInitialise(void)
{
    return VfsRegisterFilesystem("ext2", &Ext2VfsOperations);
}
