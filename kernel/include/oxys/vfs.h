/*
 * File: kernel/include/oxys/vfs.h
 * Purpose: Declares the virtual filesystem layer: the neutral description of a
 *          file, the operations a filesystem supplies in order to be mounted,
 *          the mount table that joins several volumes into one tree, the node
 *          cache that gives one file one identity however many callers hold it,
 *          and the open file with a position that advances.
 * Key definitions: VfsNodeType, VfsError, VfsAttributes, VfsDirectoryEntry,
 *          VfsNode, VfsMount, VfsFilesystemOperations, VfsRegisterFilesystem,
 *          VfsMountVolume, VfsUnmount, VfsMountRoot, VfsResolve,
 *          VfsResolveNoFollow, VfsNodeRelease, VfsOpen, VfsClose, VfsRead,
 *          VfsWrite, VfsSeek, VfsReadDirectory, VfsStat, VfsStatLink,
 *          VfsTruncate, VfsCreateDirectory, VfsRemoveDirectory, VfsUnlink,
 *          VfsLink, VfsReadLink, VfsSync, VfsSetError, VfsLastError,
 *          VfsLastErrorCode, VfsErrorName, VfsNodeTypeName, VfsReport,
 *          VfsReportDirectory.
 * References:
 *   - docs/storage/VFS.md: the design of this layer and the reasons for its
 *     shape.
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), Section 4.13 (Pathname Resolution):
 *     the rules this layer implements — a pathname beginning with a slash is
 *     resolved from the root, successive slashes are equivalent to one, a
 *     component that is not the last must resolve to a directory, and a
 *     symbolic link met in the path is replaced by its contents.
 *   - The same, `open()`, `read()`, `write()`, `lseek()`, `link()`, `unlink()`,
 *     `rmdir()`, `mkdir()`, `readlink()` and `stat()`: the shape of the
 *     operations declared here, so that the system calls of Phase 6 map upon
 *     them without an intervening translation.
 *   - The same, `lseek()`: a seek beyond the end of a file is permitted and
 *     writing there leaves a hole, which is the behaviour the EXT2 layer beneath
 *     already provides.
 *   - Ritchie, D. M. and Thompson, K., "The UNIX Time-Sharing System",
 *     Communications of the ACM 17(7), 1974, Section 3.4: the mount, which
 *     replaces a leaf of one file tree by the root of another. Consulted for the
 *     concept; the implementation here is original.
 */

#ifndef OXYS_VFS_H
#define OXYS_VFS_H

#include <oxys/types.h>
#include <oxys/block.h>

/*
 * The bounds of the layer.
 *
 * Every one of these is a bound upon this kernel and not upon any format. They
 * are stated together so that the storage the layer occupies is legible in one
 * place: there is no dynamic table here, every structure being drawn from a
 * fixed array, because a filesystem layer that could exhaust the heap would fail
 * at exactly the moment something needed to write an error to a file.
 */
#define VFS_FILESYSTEM_CAPACITY 4U
#define VFS_MOUNT_CAPACITY      4U
#define VFS_NODE_CAPACITY       64U
#define VFS_FILE_CAPACITY       32U

/* The greatest length of a filesystem type's name, excluding its terminator. */
#define VFS_TYPE_NAME_MAXIMUM 15U

/*
 * The greatest length of one component of a path, and of a whole path.
 *
 * The component bound is the EXT2 name bound, which every filesystem this kernel
 * is likely to carry shares. The path bound is this kernel's own: nothing here
 * copies a path, resolution walking the caller's string in place, so the bound
 * exists to refuse a string that is not terminated rather than to size a buffer.
 */
#define VFS_NAME_MAXIMUM 255U
#define VFS_PATH_MAXIMUM 1023U

/*
 * The longest symbolic link target this layer will read, and the number of links
 * it will follow in resolving one path.
 *
 * A link is followed by resolving its target, which re-enters the resolver, so
 * each link in flight is a stack frame carrying a target buffer of its own; the
 * depth bound is what keeps a link that names itself from consuming the stack.
 * Eight is the depth POSIX.1-2017 requires an implementation to allow. These
 * agree with the bounds the EXT2 resolver of sub-task 5.5 applies within one
 * volume, and for the same reasons.
 */
#define VFS_SYMLINK_MAXIMUM       255U
#define VFS_SYMLINK_DEPTH_MAXIMUM 8U

/* The separator between the components of a path. */
#define VFS_PATH_SEPARATOR '/'

/*
 * What a file is, without reference to how any filesystem records it.
 *
 * The eight formats are those POSIX distinguishes and those EXT2 stores in the
 * high four bits of i_mode. They are enumerated rather than carried as a mode
 * because the mode is a filesystem's own encoding: a caller asking whether a
 * path names a directory must not have to know that EXT2 writes 0x4000 there.
 */
typedef enum VfsNodeType
{
    VFS_NODE_UNKNOWN = 0,
    VFS_NODE_REGULAR,
    VFS_NODE_DIRECTORY,
    VFS_NODE_SYMBOLIC_LINK,
    VFS_NODE_CHARACTER_DEVICE,
    VFS_NODE_BLOCK_DEVICE,
    VFS_NODE_FIFO,
    VFS_NODE_SOCKET
} VfsNodeType;

/*
 * Why an operation was refused.
 *
 * A description in words is kept as well, and is what a diagnostic prints; the
 * code exists because Phase 6 must return a refusal to a user program, which
 * cannot be given a pointer into the kernel's read-only data. The two are
 * maintained together at every refusal, so that neither can say something the
 * other does not.
 */
typedef enum VfsError
{
    VFS_ERROR_NONE = 0,
    VFS_ERROR_NOT_FOUND,      /* No file of that name. */
    VFS_ERROR_EXISTS,         /* A file of that name already. */
    VFS_ERROR_NOT_DIRECTORY,  /* A component of the path is not a directory. */
    VFS_ERROR_IS_DIRECTORY,   /* A directory where a file was required. */
    VFS_ERROR_NOT_EMPTY,      /* A directory holding more than "." and "..". */
    VFS_ERROR_READ_ONLY,      /* The mount, or the volume, may not be written. */
    VFS_ERROR_INVALID,        /* The request itself is malformed. */
    VFS_ERROR_TOO_LONG,       /* A path or a component beyond the bounds above. */
    VFS_ERROR_TOO_MANY_LINKS, /* Symbolic links followed beyond the depth bound. */
    VFS_ERROR_NO_SPACE,       /* The volume has no room. */
    VFS_ERROR_NO_RESOURCE,    /* This layer has no room: a table is full. */
    VFS_ERROR_BUSY,           /* Something is held that the operation would destroy. */
    VFS_ERROR_CROSSES_MOUNT,  /* An operation confined to one volume was not. */
    VFS_ERROR_UNSUPPORTED,    /* The filesystem does not offer the operation. */
    VFS_ERROR_MEDIUM          /* The volume or the device beneath it failed. */
} VfsError;

/*
 * What is known about a file, in neutral terms. This is what `stat()` returns
 * and is deliberately a copy: a caller holding it does not hold the file.
 */
typedef struct VfsAttributes
{
    uint64_t number; /* The filesystem's identifier for the file. */
    VfsNodeType type;
    uint16_t permissions; /* The nine permission bits and the three set-id bits. */
    uint16_t uid;
    uint16_t gid;
    uint64_t size;        /* Bytes. */
    uint64_t link_count;  /* Names that lead to this file. */
    uint64_t block_count; /* 512-byte units the file occupies upon the medium. */
    uint32_t access_time;
    uint32_t modify_time;
    uint32_t change_time;
    uint32_t block_size; /* The unit of allocation upon the volume. */
} VfsAttributes;

/*
 * One entry of a directory, in neutral terms.
 *
 * The name is terminated here, which it is not upon an EXT2 volume. The type is
 * what the directory declares; a filesystem that declares none reports
 * VFS_NODE_UNKNOWN, and a caller that needs the type must stat the name.
 */
typedef struct VfsDirectoryEntry
{
    uint64_t number;
    VfsNodeType type;
    char name[VFS_NAME_MAXIMUM + 1U];
} VfsDirectoryEntry;

typedef struct VfsNode VfsNode;
typedef struct VfsMount VfsMount;

/*
 * The operations one filesystem supplies.
 *
 * Every one takes nodes and neutral types; none takes a block device or a
 * superblock, those being what the filesystem keeps to itself in the `context`
 * of its mount. The division is the whole point of the layer: the code above
 * this vector never learns what filesystem it is addressing, and the code below
 * it never learns about mounts, descriptors or the tree the mounts compose.
 *
 * A null entry means the filesystem does not offer the operation, and the layer
 * refuses with VFS_ERROR_UNSUPPORTED rather than calling through a null pointer.
 * `mount`, `unmount`, `read_node` and `lookup` are not optional: without them
 * nothing can be reached at all.
 *
 * Contracts common to all of them:
 *   - A name is given by its address and its length and is not terminated, so
 *     that a component may be used where it stands within a path.
 *   - A count reports what was transferred before a failure, not what was asked.
 *   - The mount's `read_only` has already been enforced by the layer; a
 *     filesystem need not test it a second time, though the EXT2 layer does
 *     because a volume may be read-only for reasons a mount knows nothing of.
 */
typedef struct VfsFilesystemOperations
{
    /*
     * Reads the volume upon the device and prepares the mount, filling
     * mount->context, mount->root_number and mount->block_size. It must not
     * touch any other field.
     */
    bool (*mount)(VfsMount *mount, BlockDevice *device, bool read_only);

    /*
     * Releases everything the mount holds and writes back whatever is
     * outstanding. Called after the last node of the mount has been released,
     * so nothing is in use by the time it runs.
     */
    bool (*unmount)(VfsMount *mount);

    /*
     * Fills the neutral fields of a node from the file the number names, and
     * attaches whatever private description the filesystem needs to
     * node->context.
     */
    bool (*read_node)(VfsMount *mount, uint64_t number, VfsNode *node);

    /* Releases whatever read_node attached. Called once for every node read. */
    void (*release_node)(VfsNode *node);

    /*
     * Finds a name within a directory and yields the number it names. A name
     * that is absent is a refusal of VFS_ERROR_NOT_FOUND and is an ordinary
     * outcome, not a fault of the volume.
     */
    bool (*lookup)(VfsNode *directory, const char *name, size_t length, uint64_t *number);

    /* Reads and writes the contents of a file at an offset within it. */
    bool (*read)(VfsNode *node, uint64_t offset, void *buffer, uint64_t length,
                 uint64_t *read);
    bool (*write)(VfsNode *node, uint64_t offset, const void *buffer, uint64_t length,
                  uint64_t *written);

    /* Sets the size of a file, freeing or holing what the change implies. */
    bool (*truncate)(VfsNode *node, uint64_t size);

    /* Reads the target of a symbolic link, terminating it. */
    bool (*read_link)(VfsNode *node, char *target, size_t capacity);

    /*
     * Produces the entry of a directory the cookie names and advances the cookie
     * past it. A cookie of zero is the first entry. The end of the directory is
     * reported by `end` and is not a failure: every reader arrives at it.
     *
     * The cookie is opaque to this layer and is whatever position the filesystem
     * finds convenient, so that a directory need not be traversed from its
     * beginning to continue reading it.
     */
    bool (*read_directory)(VfsNode *directory, uint64_t *cookie, VfsDirectoryEntry *entry,
                           bool *end);

    /* Creates a file, or a directory, within a directory, and yields its number. */
    bool (*create)(VfsNode *directory, const char *name, size_t length, VfsNodeType type,
                   uint16_t permissions, uint64_t *number);
    bool (*create_directory)(VfsNode *directory, const char *name, size_t length,
                             uint16_t permissions, uint64_t *number);

    /* Gives an existing file a further name within a directory. */
    bool (*link)(VfsNode *directory, const char *name, size_t length, VfsNode *target);

    /* Removes a name, and with it the file where it was the last name. */
    bool (*unlink)(VfsNode *directory, const char *name, size_t length);

    /* Removes an empty directory by name. */
    bool (*remove_directory)(VfsNode *directory, const char *name, size_t length);

    /* Writes back everything outstanding upon the mount. */
    bool (*sync)(VfsMount *mount);
} VfsFilesystemOperations;

/*
 * One filesystem type, as registered. The name is what a mount asks for.
 */
typedef struct VfsFilesystem
{
    char name[VFS_TYPE_NAME_MAXIMUM + 1U];
    const VfsFilesystemOperations *operations;
    bool registered;
} VfsFilesystem;

/*
 * One node: one file, as this layer holds it.
 *
 * A node is the identity of a file within the kernel. Two callers that reach the
 * same file by any route hold the same node, and that is not a convenience but a
 * correctness requirement: were each to hold a copy of the file's description,
 * a write through one that extended the file would leave the other's copy
 * stating the old size and the old block pointers, and a write through that one
 * would then truncate the file to what it had been. The node cache exists to
 * make that impossible.
 *
 * The neutral fields are maintained by the filesystem, which alters them as it
 * alters the file. `context` is the filesystem's own description — for EXT2, the
 * parsed inode — and is opaque here.
 */
struct VfsNode
{
    VfsMount *mount;
    uint64_t number; /* The filesystem's identifier: for EXT2, the inode number. */
    VfsNodeType type;
    uint16_t permissions;
    uint16_t uid;
    uint16_t gid;
    uint64_t size;
    uint64_t link_count;
    uint64_t block_count;
    uint32_t access_time;
    uint32_t modify_time;
    uint32_t change_time;

    void *context; /* The filesystem's own description of the file. */

    /*
     * How many holders the node has. A node reaching zero is released at once
     * rather than retained: see docs/storage/VFS.md, Section 5.
     */
    uint32_t references;

    /*
     * The mount whose root covers this node, where it is a mount point, and null
     * otherwise. Resolution substitutes the covering mount's root for the node,
     * which is what a mount means.
     */
    VfsMount *mounted;

    bool in_use;
};

/*
 * One mount: one volume, joined into the tree at one place.
 */
struct VfsMount
{
    char point[VFS_PATH_MAXIMUM + 1U]; /* Where it was mounted, for a report. */
    char type[VFS_TYPE_NAME_MAXIMUM + 1U];
    const VfsFilesystemOperations *operations;
    BlockDevice *device;
    void *context; /* The filesystem's own description of the volume. */

    uint64_t root_number; /* The identifier of the volume's root directory. */
    uint32_t block_size;  /* The volume's unit of allocation. */
    bool read_only;

    /*
     * The root node of this volume, held for the life of the mount, and the node
     * of the parent tree this mount covers — null for the root mount, which
     * covers nothing.
     *
     * Both are held rather than looked up, because both must not be released
     * while the mount stands: the root is what resolution substitutes, and the
     * covered node is what resolution must return to when ".." leaves the volume.
     */
    VfsNode *root;
    VfsNode *covered;

    bool mounted;
};

/* Where a seek measures its offset from. */
typedef enum VfsSeekOrigin
{
    VFS_SEEK_SET,    /* From the beginning of the file. */
    VFS_SEEK_CURRENT, /* From the present position. */
    VFS_SEEK_END     /* From the end of the file. */
} VfsSeekOrigin;

/*
 * How a file is opened.
 *
 * These are the flags of POSIX `open()` under this project's names. One of READ
 * and WRITE must be given: an open that asked for neither could do nothing, and
 * accepting it would mean every later operation had to discover that.
 */
#define VFS_OPEN_READ      UINT32_C(0x0001)
#define VFS_OPEN_WRITE     UINT32_C(0x0002)
#define VFS_OPEN_CREATE    UINT32_C(0x0004) /* Create the file if it is absent. */
#define VFS_OPEN_EXCLUSIVE UINT32_C(0x0008) /* With CREATE: refuse if it is present. */
#define VFS_OPEN_TRUNCATE  UINT32_C(0x0010) /* Discard the contents upon opening. */
#define VFS_OPEN_APPEND    UINT32_C(0x0020) /* Every write goes to the end. */
#define VFS_OPEN_DIRECTORY UINT32_C(0x0040) /* Refuse anything but a directory. */
#define VFS_OPEN_NO_FOLLOW UINT32_C(0x0080) /* Refuse a symbolic link standing last. */

/* The value VfsOpen returns when it could not open the file. */
#define VFS_NO_DESCRIPTOR (-1)

/*
 * Prepares the layer: empties the tables and clears the accounting. It must be
 * called before anything else here, and the kernel heap must already exist, the
 * filesystems' private descriptions being drawn from it.
 */
void VfsInitialise(void);

/*
 * Registers a filesystem type under a name. The operations are retained by
 * reference and must therefore outlive every mount made of the type, which
 * every static operations vector satisfies.
 */
bool VfsRegisterFilesystem(const char *name, const VfsFilesystemOperations *operations);

/*
 * Mounts the volume upon a device at a point in the tree.
 *
 * The first mount must be at "/" and becomes the root; every later one is at a
 * path that resolves to a directory, and that directory's contents are hidden
 * for as long as the mount stands — which is what a mount is, and why the
 * covered directory must be empty in no sense at all.
 *
 * A device already mounted is refused: two mounts of one device would keep two
 * superblocks of one volume and each would allocate without regard to the other.
 * A point already covered is refused likewise, this kernel not stacking mounts.
 *
 * `read_only` mounts a volume this kernel could write without writing it. A
 * volume the filesystem judges unwritable is mounted read-only whatever is asked
 * here; the two conditions are distinct and the mount records the disjunction.
 */
bool VfsMountVolume(const char *device_name, const char *point, const char *type,
                    bool read_only);

/*
 * Withdraws a mount, writing back everything outstanding upon it.
 *
 * It is refused where anything is still held: an open file upon the volume, or
 * another volume mounted within it. The alternative — withdrawing it regardless
 * — would leave descriptors addressing a volume that no longer exists, and the
 * failure would appear at the next read rather than here.
 */
bool VfsUnmount(const char *point);

/*
 * Mounts the first device carrying a volume of the named type at "/".
 *
 * The devices are tried in the order the block layer registered them. This is a
 * policy and not a mechanism, and it is the policy of a kernel that has no
 * command line naming a root device and no initial ramdisk to fall back upon;
 * both belong to Phase 7. Returns false, without a diagnosis being a failure of
 * the machine, where no device carries a volume this kernel can mount.
 */
bool VfsMountRoot(const char *type, bool read_only);

/* The mount at an index below VFS_MOUNT_CAPACITY, or null where none stands. */
VfsMount *VfsMountAt(size_t index);

/* The number of mounts standing, and whether a root mount exists. */
size_t VfsMountCount(void);
bool VfsRootIsMounted(void);

/*
 * Resolves an absolute path to the node it names, taking a reference upon it.
 *
 * The caller must release the node with VfsNodeRelease. A node held is a file
 * that cannot be destroyed beneath the holder, which is what makes holding one
 * meaningful; a caller that neglects to release exhausts the node table, which
 * is reported rather than silently tolerated.
 *
 * Resolution follows a symbolic link met anywhere in the path, including as the
 * last component, and crosses a mount point wherever one stands. A trailing
 * separator asserts that what the path names is a directory.
 */
bool VfsResolve(const char *path, VfsNode **node);

/*
 * The same, save that a symbolic link standing as the last component is returned
 * as it stands. Links within the path are followed as before: it is the file the
 * path names that is left unresolved, not the route taken to it.
 */
bool VfsResolveNoFollow(const char *path, VfsNode **node);

/* Releases a reference taken by VfsResolve or VfsResolveNoFollow. Null is
 * permitted and does nothing. */
void VfsNodeRelease(VfsNode *node);

/* Fills the neutral description of a node. */
void VfsNodeAttributes(const VfsNode *node, VfsAttributes *attributes);

/*
 * Opens a path and returns a descriptor, or VFS_NO_DESCRIPTOR.
 *
 * `permissions` is used only where the file is created, and only its low twelve
 * bits are taken. The position begins at zero whatever the flags, VFS_OPEN_APPEND
 * placing each write at the end rather than moving the position at the outset.
 *
 * A directory may be opened for reading and is then read by VfsReadDirectory
 * rather than by VfsRead: its bytes are entries, and a caller reading them as a
 * stream has mistaken what it holds.
 */
int VfsOpen(const char *path, uint32_t flags, uint16_t permissions);

/*
 * Closes a descriptor, releasing the node it held. A descriptor closed twice is
 * a defect in the caller and is refused rather than ignored.
 */
bool VfsClose(int descriptor);

/*
 * Reads from, and writes to, the position of an open file, advancing it by what
 * was transferred.
 *
 * A read at the end of the file transfers nothing and is not a failure — it is
 * how reading concludes — so the end is distinguished from an error by the count
 * and not by the return value.
 */
bool VfsRead(int descriptor, void *buffer, uint64_t length, uint64_t *read);
bool VfsWrite(int descriptor, const void *buffer, uint64_t length, uint64_t *written);

/*
 * Moves the position of an open file and reports where it now stands.
 *
 * A position beyond the end of the file is permitted, as POSIX requires: writing
 * there leaves a hole between the end and the write, which is how a sparse file
 * is made. A position before the beginning is refused, there being nothing there.
 */
bool VfsSeek(int descriptor, int64_t offset, VfsSeekOrigin origin, uint64_t *position);

/* Where the position of an open file stands. */
bool VfsTell(int descriptor, uint64_t *position);

/*
 * Produces the next entry of an open directory and advances its position.
 *
 * The end of the directory is reported by `end` and is not a failure, for the
 * reason the end of a file is not.
 */
bool VfsReadDirectory(int descriptor, VfsDirectoryEntry *entry, bool *end);

/* The description of an open file. */
bool VfsFileAttributes(int descriptor, VfsAttributes *attributes);

/* The number of descriptors presently open. */
size_t VfsOpenFileCount(void);

/*
 * The description of the file a path names. VfsStat follows a symbolic link
 * standing last and VfsStatLink reports the link itself, which is the
 * distinction between acting upon a file and acting upon its name.
 */
bool VfsStat(const char *path, VfsAttributes *attributes);
bool VfsStatLink(const char *path, VfsAttributes *attributes);

/* Sets the size of the file a path names. */
bool VfsTruncate(const char *path, uint64_t size);

/* Creates a directory, and removes an empty one. */
bool VfsCreateDirectory(const char *path, uint16_t permissions);
bool VfsRemoveDirectory(const char *path);

/*
 * Removes a name, and with it the file where it was the last name.
 *
 * A file that is open is refused. POSIX would keep such a file alive until its
 * last descriptor closed, which requires a list of files that have no name and
 * are not yet gone; this kernel has no such list, and the alternative to
 * refusing is to free the inode and the blocks beneath a descriptor still
 * reading them. The reason is recorded in docs/storage/VFS.md, Section 9.3.
 */
bool VfsUnlink(const char *path);

/*
 * Gives an existing file a further name. Both paths must lie within one mount:
 * a name is an entry of a directory naming an inode of that directory's own
 * volume, and there is no number one volume could write that would name a file
 * upon another.
 */
bool VfsLink(const char *existing, const char *name);

/* Reads the target of the symbolic link a path names, terminating it. */
bool VfsReadLink(const char *path, char *target, size_t capacity);

/* Writes back everything outstanding upon every mount, and then the buffer
 * cache beneath them. */
bool VfsSync(void);

/*
 * Records a refusal, and returns false so that a refusal may be one statement.
 *
 * It is exported for the filesystem implementations, which are the only callers
 * outside this layer: an operation of the vector above refuses by calling this
 * and returning what it returns, so that the code and the words a caller reads
 * are set at the point the refusal is decided rather than guessed at afterwards.
 *
 * The reason is retained by reference and must therefore be a string literal or
 * the stable diagnosis of a layer below, never a buffer that will be reused.
 */
bool VfsSetError(VfsError code, const char *reason);

/* A description in words of the most recent refusal, never null, and its code. */
const char *VfsLastError(void);
VfsError VfsLastErrorCode(void);

/* The name of an error code, for a diagnostic. */
const char *VfsErrorName(VfsError error);

/* The name of a node type, for a diagnostic. */
const char *VfsNodeTypeName(VfsNodeType type);

/* The accounting: what the layer has done, and what it has refused. */
uint64_t VfsPathsResolved(void);
uint64_t VfsPathsRefused(void);
uint64_t VfsNodesRead(void);
uint64_t VfsNodeCacheHits(void);
uint64_t VfsFilesOpened(void);
uint64_t VfsBytesRead(void);
uint64_t VfsBytesWritten(void);
uint64_t VfsRefusals(void);

/* The nodes and descriptors presently held, for the report and for a self-test
 * that must assert nothing was leaked. */
size_t VfsNodesHeld(void);

/* Writes the mounts, the nodes held, the open files and the accounting to the
 * console and the serial port. */
void VfsReport(void);

/* Writes the contents of the directory a path names to the console. */
void VfsReportDirectory(const char *path);

#endif /* OXYS_VFS_H */
