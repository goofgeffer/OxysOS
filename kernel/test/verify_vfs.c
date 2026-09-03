/*
 * File: kernel/test/verify_vfs.c
 * Purpose: Asserts the virtual filesystem layer of sub-task 5.8: the
 *          resolution of a path across mount points and through symbolic links,
 *          the identity of a file reached by two routes, the open file and its
 *          position, the mount and the withdrawal, and the refusals that keep a
 *          volume from being withdrawn while something still holds it. Probes
 *          whatever volume the machine actually carries.
 * Key functions: KernelVerifyVfs, KernelVfsProbeVolume.
 * References:
   - docs/storage/VFS.md, Section 10: every assertion below, paired with the
 *     silent failure it catches.
 *   - IEEE Std 1003.1-2017: the depth bound upon following symbolic links, and
 *     the semantics of . and .. that the layer does and does not interpret.
 *
 * The mount assertions are made against two volumes rather than one,
 * the second being a copy of the first with the owner of one file altered, so
 * that an assertion can state which volume a path reached. Two identical volumes
 * would make every crossing assertion vacuous. The tests end by requiring that
 * no node was left held and no descriptor open, which is the assertion a fixed
 * table must be held to: a resolution that failed to release what it held would
 * exhaust the table long before a machine had done any real work.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/vfs.h>
#include <oxys/ext2.h>
#include <oxys/ext2_vfs.h>
#include <oxys/block.h>
#include <oxys/buffer.h>

/*
 * ---------------------------------------------------------------------------
 * The self-test of the virtual filesystem layer, sub-task 5.8.
 *
 * Everything above this point asserts one volume in isolation: a superblock, a
 * table, an inode, a directory, the contents of a file. This asserts the tree
 * those volumes are joined into, and there are three things in it that no test
 * of one volume can reach.
 *
 * The first is identity. A file reached twice must be one file, and not two
 * descriptions of one file that may disagree; the assertion is that a write
 * through one descriptor is seen by the other, and that neither of them, upon
 * closing, leaves a node behind.
 *
 * The second is the mount. A second volume is mounted upon a directory of the
 * first, and the assertions state which volume each path reached — which
 * requires the two to be distinguishable, and is why the second is a copy of the
 * first with one field altered rather than a second composition. Crossing the
 * mount point and returning across it by ".." are the two directions, and the
 * second is the one a layer that matched paths by prefix would get wrong.
 *
 * The third is the record a mount leaves upon a volume. A volume opened for
 * writing is marked as not cleanly unmounted for as long as it is open, and the
 * assertion reads that mark back out of the medium rather than out of the
 * superblock in memory: a mark that never reached the disk would protect
 * nothing, that being the one circumstance — a machine that stopped — the mark
 * exists for.
 * ------------------------------------------------------------------------- */

/* The device names the self-test registers, and the point the second is mounted
 * upon. */
#define KERNEL_VFS_ROOT_DEVICE   "mem0"
#define KERNEL_VFS_SECOND_DEVICE "mem2"
#define KERNEL_VFS_MOUNT_POINT   "/sub"

/*
 * The owner given to the file "/file" upon the second volume alone.
 *
 * The two volumes are otherwise identical, so this is the whole of what
 * distinguishes them, and every assertion below that states which volume a path
 * reached states it by reading this field. A value no composition writes is
 * chosen so that reading it by accident is not possible.
 */
#define KERNEL_VFS_SECOND_UID UINT16_C(0x5A5A)

/* Whether the sequence below has held so far. It is a file-scope variable
 * because the assertion helper sets it, there being some sixty assertions and
 * threading a result through each of them obscuring what is being asserted. */
static bool KernelVfsSucceeded;

/*
 * Asserts one property, naming it and the layer's own diagnosis where it fails.
 *
 * The self-tests of the earlier sub-tasks state each failure with a block of
 * their own because there was no diagnosis to print beside it. There is one
 * here — VfsLastError describes every refusal — so a failure names both what was
 * expected and what the layer said instead, which is the difference between
 * knowing that a test failed and knowing why.
 */
static void KernelVfsRequire(bool condition, const char *statement)
{
    if (condition)
    {
        return;
    }

    KernelWriteString("  ");
    KernelWriteString(statement);
    KernelWriteString(" [");
    KernelWriteString(VfsErrorName(VfsLastErrorCode()));
    KernelWriteString(": ");
    KernelWriteString(VfsLastError());
    KernelWriteString("]\n");
    KernelVfsSucceeded = false;
}

/* A halfword read back out of a store, so that what reached the medium may be
 * asserted rather than what was intended to. */
static uint16_t KernelLoadHalf(const uint8_t *store, size_t offset)
{
    return (uint16_t)((uint16_t)store[offset] | ((uint16_t)store[offset + 1U] << 8));
}

/* A halfword written into an arbitrary store, the composer writing only into the
 * first. */
static void KernelStoreHalfIn(uint8_t *store, size_t offset, uint16_t value)
{
    store[offset] = (uint8_t)(value & 0xFFU);
    store[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

/* Whether a path names the file it should, of the format it should. */
static bool KernelVfsIs(const char *path, uint64_t number, VfsNodeType type)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return false;
    }

    return (attributes.number == number) && (attributes.type == type);
}

/* Whether a path is refused, and refused for the stated reason. Both halves
 * matter: a path refused for the wrong reason is a resolver that reached the
 * wrong conclusion by luck. */
static bool KernelVfsRefusedWith(const char *path, VfsError expected)
{
    VfsAttributes attributes;

    if (VfsStat(path, &attributes))
    {
        return false;
    }

    return VfsLastErrorCode() == expected;
}

/* The size of the file a path names, or UINT64_MAX where it names none. */
static uint64_t KernelVfsSizeOf(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return UINT64_MAX;
    }

    return attributes.size;
}

/* The owner of the file a path names, which is what says which volume it lies
 * upon. */
static uint16_t KernelVfsOwnerOf(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return 0U;
    }

    return attributes.uid;
}

/* The file a path names, or zero where it names none. */
static uint64_t KernelVfsInodeOfPath(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return 0U;
    }

    return attributes.number;
}

/* The number of names a file bears, or zero where the path names none. */
static uint64_t KernelVfsLinksOf(const char *path)
{
    VfsAttributes attributes;

    if (!VfsStat(path, &attributes))
    {
        return 0U;
    }

    return attributes.link_count;
}

/* Whether a directory holds a name, found by reading the directory rather than
 * by resolving the name: the two are different operations and a directory that
 * lists what it cannot resolve is as wrong as one that resolves what it does not
 * list. */
static bool KernelVfsDirectoryHolds(const char *path, const char *name, uint64_t *count)
{
    VfsDirectoryEntry entry;
    bool end = false;
    bool found = false;
    const int descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U);

    if (count != NULL)
    {
        *count = 0U;
    }

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        return false;
    }

    while (VfsReadDirectory(descriptor, &entry, &end) && (!end))
    {
        if (KernelSameString(entry.name, name))
        {
            found = true;
        }

        if (count != NULL)
        {
            ++*count;
        }
    }

    (void)VfsClose(descriptor);
    return found;
}

/*
 * Asserts the resolution of a path: the components, the separators, the entries
 * "." and "..", and the symbolic links.
 */
static void KernelVerifyVfsResolution(void)
{
    char target[VFS_SYMLINK_MAXIMUM + 1U];
    VfsAttributes attributes;

    KernelVfsRequire(KernelVfsIs("/", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "the root did not resolve to the root directory");
    KernelVfsRequire(KernelVfsIs("/file", KERNEL_VOLUME_FILE_INODE, VFS_NODE_REGULAR),
                     "a file in the root did not resolve");
    KernelVfsRequire(KernelVfsIs("/sub", KERNEL_VOLUME_SUB_INODE, VFS_NODE_DIRECTORY),
                     "a subdirectory did not resolve");
    KernelVfsRequire(KernelVfsIs("/sub/inner", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "a file within a subdirectory did not resolve");

    /* The size joins the two halves the format holds it in, and is what bounds
     * every read of the file. */
    KernelVfsRequire(KernelVfsSizeOf("/file") == KERNEL_VOLUME_FILE_SIZE,
                     "the size of a file was not reported as the volume states it");
    KernelVfsRequire(KernelVfsSizeOf("/sub/inner") == KERNEL_VOLUME_INNER_SIZE,
                     "the size of a file within a subdirectory was wrong");

    /*
     * The separators. Repeated ones are equivalent to one, a trailing one
     * asserts a directory, and neither is a curiosity: both arise in every path
     * a person types.
     */
    KernelVfsRequire(KernelVfsIs("//sub//inner", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "repeated separators were not equivalent to one");
    KernelVfsRequire(KernelVfsIs("/sub/", KERNEL_VOLUME_SUB_INODE, VFS_NODE_DIRECTORY),
                     "a trailing separator upon a directory was refused");
    KernelVfsRequire(KernelVfsRefusedWith("/file/", VFS_ERROR_NOT_DIRECTORY),
                     "a trailing separator upon a regular file was accepted");

    /*
     * "." and ".." are ordinary entries of the volume and are resolved by
     * looking rather than by being interpreted. The ".." of the root names the
     * root, which is what the volume says and what a resolver must not
     * second-guess.
     */
    KernelVfsRequire(KernelVfsIs("/.", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "\".\" did not resolve to the directory holding it");
    KernelVfsRequire(KernelVfsIs("/..", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "the \"..\" of the root did not name the root");
    KernelVfsRequire(KernelVfsIs("/sub/..", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "\"..\" did not leave a subdirectory");
    KernelVfsRequire(KernelVfsIs("/sub/../file", KERNEL_VOLUME_FILE_INODE, VFS_NODE_REGULAR),
                     "a path continuing through \"..\" did not resolve");

    /*
     * The symbolic links. A link is followed where the file is wanted and
     * reported as itself where the name is, which is the distinction between
     * acting upon a file and acting upon its name.
     */
    KernelVfsRequire(KernelVfsIs("/link-fast", KERNEL_VOLUME_SUB_INODE, VFS_NODE_DIRECTORY),
                     "a fast symbolic link was not followed");
    KernelVfsRequire(KernelVfsIs("/link-fast/inner", KERNEL_VOLUME_INNER_INODE,
                                 VFS_NODE_REGULAR),
                     "a symbolic link standing within a path was not followed");
    KernelVfsRequire(KernelVfsIs("/link-slow", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "a symbolic link held in a block was not followed");

    KernelVfsRequire(VfsStatLink("/link-fast", &attributes) &&
                         (attributes.number == KERNEL_VOLUME_FAST_LINK_INODE) &&
                         (attributes.type == VFS_NODE_SYMBOLIC_LINK),
                     "a symbolic link was followed where the name was asked for");

    KernelVfsRequire(VfsReadLink("/link-fast", target, sizeof target) &&
                         KernelSameString(target, KERNEL_VOLUME_FAST_LINK_TARGET),
                     "the target of a fast symbolic link was not read");
    KernelVfsRequire(VfsReadLink("/link-slow", target, sizeof target) &&
                         KernelSameString(target, KERNEL_VOLUME_SLOW_LINK_TARGET),
                     "the target of a symbolic link held in a block was not read");
    KernelVfsRequire(!VfsReadLink("/file", target, sizeof target),
                     "the target of something that is not a link was read");

    /* The refusals, each with the reason that distinguishes it from the others. */
    KernelVfsRequire(KernelVfsRefusedWith("/absent", VFS_ERROR_NOT_FOUND),
                     "a name the volume does not hold was resolved");
    KernelVfsRequire(KernelVfsRefusedWith("/file/beyond", VFS_ERROR_NOT_DIRECTORY),
                     "a path continuing through a regular file was resolved");
    KernelVfsRequire(KernelVfsRefusedWith("relative", VFS_ERROR_INVALID),
                     "a relative path was resolved, there being nothing to resolve it "
                     "against");
    KernelVfsRequire(KernelVfsRefusedWith("", VFS_ERROR_INVALID),
                     "an empty path was resolved");

    /*
     * The record whose inode number is zero holds space where a name was
     * removed. The bytes of the name are still lying in it, and a resolver that
     * read them rather than the inode number would report a file that was
     * deleted.
     */
    KernelVfsRequire(KernelVfsRefusedWith("/removed", VFS_ERROR_NOT_FOUND),
                     "a record marked unused was resolved as a name");
}

/*
 * Asserts the open file: that its position advances by what it transferred, that
 * the end of a file is reported by the count and not as a failure, and that two
 * descriptors upon one file have one identity and two positions.
 */
static void KernelVerifyVfsFiles(void)
{
    uint64_t transferred = 0U;
    uint64_t position = 0U;
    VfsAttributes attributes;
    int first;
    int second;

    first = VfsOpen("/sub/inner", VFS_OPEN_READ, 0U);
    KernelVfsRequire(first != VFS_NO_DESCRIPTOR, "a file could not be opened for reading");

    if (first == VFS_NO_DESCRIPTOR)
    {
        return;
    }

    KernelVfsRequire(VfsFileAttributes(first, &attributes) &&
                         (attributes.size == KERNEL_VOLUME_INNER_SIZE) &&
                         (attributes.type == VFS_NODE_REGULAR),
                     "an open file did not describe itself as the volume does");

    /*
     * The whole file, read in one call. The composed contents depend upon the
     * offset, so a read that returned the right count from the wrong block fails
     * here rather than passing.
     */
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, KERNEL_VOLUME_INNER_SIZE,
                             &transferred) &&
                         (transferred == KERNEL_VOLUME_INNER_SIZE) &&
                         KernelFileBufferMatches(0U, KERNEL_VOLUME_INNER_SIZE),
                     "the contents of a file were not read through a descriptor");

    KernelVfsRequire(VfsTell(first, &position) && (position == KERNEL_VOLUME_INNER_SIZE),
                     "the position did not advance by what was read");

    /*
     * The end of the file. Every reader arrives at it, so it is reported by the
     * count rather than by the return value; a layer that reported it as a
     * failure would oblige each caller to treat the conclusion of its work as a
     * fault.
     */
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, 64U, &transferred) &&
                         (transferred == 0U),
                     "reading at the end of a file was reported as a failure");

    /* Seeking, in all three of its origins. */
    KernelVfsRequire(VfsSeek(first, 100, VFS_SEEK_SET, &position) && (position == 100U),
                     "a seek from the beginning did not arrive where it was sent");
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, 16U, &transferred) &&
                         (transferred == 16U) && KernelFileBufferMatches(100U, 16U),
                     "a read after a seek did not begin where the seek left the position");

    KernelVfsRequire(VfsSeek(first, 0, VFS_SEEK_END, &position) &&
                         (position == KERNEL_VOLUME_INNER_SIZE),
                     "a seek to the end did not arrive at the size");
    KernelVfsRequire(VfsSeek(first, -16, VFS_SEEK_CURRENT, &position) &&
                         (position == (KERNEL_VOLUME_INNER_SIZE - 16U)),
                     "a seek backwards from the position was wrong");

    /*
     * A position beyond the end is permitted, writing there being how a sparse
     * file is made; a position before the beginning is refused, there being
     * nothing there to name.
     */
    KernelVfsRequire(VfsSeek(first, 1000000, VFS_SEEK_END, &position),
                     "a seek beyond the end of a file was refused");
    KernelVfsRequire(!VfsSeek(first, -1, VFS_SEEK_SET, &position) &&
                         (VfsLastErrorCode() == VFS_ERROR_INVALID),
                     "a seek before the beginning of a file was accepted");
    KernelVfsRequire(VfsRead(first, KernelFileBuffer, 16U, &transferred) &&
                         (transferred == 0U),
                     "a read beyond the end of a file returned bytes");

    /*
     * Two descriptors upon one file. They must share the file — one node, so
     * that what one alters the other sees — and not share the position, which
     * belongs to the open file and not to the file.
     */
    second = VfsOpen("/sub/inner", VFS_OPEN_READ, 0U);
    KernelVfsRequire(second != VFS_NO_DESCRIPTOR, "a file could not be opened a second time");

    if (second != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsSeek(second, 0, VFS_SEEK_SET, &position) && (position == 0U),
                         "the second descriptor could not be positioned");
        KernelVfsRequire(VfsTell(first, &position) && (position != 0U),
                         "positioning one descriptor moved the position of another upon the "
                         "same file");
        KernelVfsRequire(VfsRead(second, KernelFileBuffer, 32U, &transferred) &&
                             (transferred == 32U) && KernelFileBufferMatches(0U, 32U),
                         "the second descriptor did not read from its own position");
        KernelVfsRequire(VfsClose(second), "a descriptor could not be closed");
    }

    /* A directory is not read as a stream, and a file is not opened as a
     * directory. */
    KernelVfsRequire(VfsOpen("/", VFS_OPEN_READ | VFS_OPEN_WRITE, 0U) == VFS_NO_DESCRIPTOR,
                     "a directory was opened for writing");
    KernelVfsRequire(VfsOpen("/file", VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U) ==
                         VFS_NO_DESCRIPTOR,
                     "a regular file was opened as a directory");
    KernelVfsRequire(VfsOpen("/file", 0U, 0U) == VFS_NO_DESCRIPTOR,
                     "an open asking neither to read nor to write was accepted");
    KernelVfsRequire(VfsOpen("/link-fast", VFS_OPEN_READ | VFS_OPEN_NO_FOLLOW, 0U) ==
                         VFS_NO_DESCRIPTOR,
                     "a symbolic link was opened where the open refused to follow one");

    KernelVfsRequire(VfsClose(first), "a descriptor could not be closed");
    KernelVfsRequire(!VfsClose(first), "a descriptor was closed twice");
}

/* Asserts that a directory is listed, and that what it lists is what resolves. */
static void KernelVerifyVfsDirectories(void)
{
    uint64_t count = 0U;
    uint64_t transferred = 0U;
    int descriptor;

    KernelVfsRequire(KernelVfsDirectoryHolds("/", "file", &count),
                     "a directory listing did not hold a name the directory holds");
    KernelVfsRequire(count == 6U,
                     "the root was not listed as holding six entries: \".\", \"..\", "
                     "\"file\", \"sub\" and the two links");
    KernelVfsRequire(!KernelVfsDirectoryHolds("/", "removed", NULL),
                     "a record marked unused was listed as a name");
    KernelVfsRequire(KernelVfsDirectoryHolds("/", "..", NULL),
                     "\"..\" was not listed, though the volume holds it as an entry");
    KernelVfsRequire(KernelVfsDirectoryHolds("/sub", "inner", &count) && (count == 3U),
                     "a subdirectory was not listed as holding \".\", \"..\" and one file");

    descriptor = VfsOpen("/", VFS_OPEN_READ | VFS_OPEN_DIRECTORY, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a directory could not be opened");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(!VfsRead(descriptor, KernelFileBuffer, 16U, &transferred) &&
                             (VfsLastErrorCode() == VFS_ERROR_IS_DIRECTORY),
                         "a directory was read as a stream of bytes");
        KernelVfsRequire(VfsClose(descriptor), "a directory could not be closed");
    }
}

/*
 * Asserts that a volume may be altered through the layer: that a file is
 * created, written, read back, given a second name, truncated and destroyed, and
 * that a file something holds is not destroyed beneath it.
 */
static void KernelVerifyVfsWrites(void)
{
    static const char *const path = "/made";
    uint64_t transferred = 0U;
    uint64_t position = 0U;
    uint64_t root_links;
    int descriptor;

    /* Creating a file, and writing to it. */
    descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                                   VFS_OPEN_EXCLUSIVE,
                         0644U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be created");

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        return;
    }

    for (size_t index = 0U; index < 512U; ++index)
    {
        KernelFileBuffer[index] = KernelFileByteAt((uint64_t)index);
    }

    KernelVfsRequire(VfsWrite(descriptor, KernelFileBuffer, 512U, &transferred) &&
                         (transferred == 512U),
                     "a newly created file could not be written to");
    KernelVfsRequire(VfsTell(descriptor, &position) && (position == 512U),
                     "the position did not advance by what was written");
    KernelVfsRequire(VfsClose(descriptor), "a written file could not be closed");

    KernelVfsRequire(KernelVfsSizeOf(path) == 512U,
                     "the size of a written file was not what was written to it");
    KernelVfsRequire(KernelVfsLinksOf(path) == 1U, "a created file did not bear one name");

    /* Reading it back. The contents were composed from the offset, so a write
     * that reached the wrong block is caught here. */
    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a written file could not be reopened");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        for (size_t index = 0U; index < 512U; ++index)
        {
            KernelFileBuffer[index] = 0U;
        }

        KernelVfsRequire(VfsRead(descriptor, KernelFileBuffer, 512U, &transferred) &&
                             (transferred == 512U) && KernelFileBufferMatches(0U, 512U),
                         "what was read back from a file was not what was written to it");
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    /* An exclusive creation of a file that exists. */
    KernelVfsRequire(VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE,
                             0644U) == VFS_NO_DESCRIPTOR,
                     "an exclusive creation of a file that exists was accepted");
    KernelVfsRequire(VfsLastErrorCode() == VFS_ERROR_EXISTS,
                     "an exclusive creation was refused for the wrong reason");

    /*
     * An appending write goes to the end of the file as it stands and not to
     * where the position is. The position is deliberately left at zero to make
     * the two distinguishable.
     */
    descriptor = VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_APPEND, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be opened to append");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsWrite(descriptor, KernelFileBuffer, 128U, &transferred) &&
                             (transferred == 128U),
                         "an appending write failed");
        KernelVfsRequire(VfsTell(descriptor, &position) && (position == 640U),
                         "an appending write did not go to the end of the file");
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(KernelVfsSizeOf(path) == 640U,
                     "the file did not grow by what was appended to it");

    /* Truncation, downward to nothing and upward into a hole. */
    KernelVfsRequire(VfsTruncate(path, 0U) && (KernelVfsSizeOf(path) == 0U),
                     "a file could not be truncated to nothing");
    KernelVfsRequire(VfsTruncate(path, 4096U) && (KernelVfsSizeOf(path) == 4096U),
                     "a file could not be extended by truncation");

    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsRead(descriptor, KernelFileBuffer, 512U, &transferred) &&
                             (transferred == 512U) && KernelFileBufferIsZero(512U),
                         "the hole a truncation left did not read as zeroes");
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    /* Opening with a truncation discards the contents. */
    descriptor = VfsOpen(path, VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR,
                     "a file could not be opened for truncation");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(KernelVfsSizeOf(path) == 0U,
                     "an open that truncates did not discard the contents");

    /* A second name for one file, and the link count that says how many names
     * lead to it. */
    KernelVfsRequire(VfsLink(path, "/made-again"), "a file could not be given a second name");
    KernelVfsRequire(KernelVfsLinksOf(path) == 2U,
                     "the link count did not rise with the second name");
    KernelVfsRequire(KernelVfsInodeOfPath(path) == KernelVfsInodeOfPath("/made-again"),
                     "the two names did not lead to one file");
    KernelVfsRequire(!VfsLink("/sub", "/sub-again") &&
                         (VfsLastErrorCode() == VFS_ERROR_IS_DIRECTORY),
                     "a directory was given a second name");
    KernelVfsRequire(VfsUnlink("/made-again") && (KernelVfsLinksOf(path) == 1U),
                     "removing one of two names did not leave the file with one");

    /*
     * A file something holds is not destroyed. This kernel keeps no list of
     * files that have no name and are not yet gone, so the alternative to
     * refusing is to free the inode and the blocks beneath a descriptor still
     * reading them.
     */
    descriptor = VfsOpen(path, VFS_OPEN_READ, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be opened");
    KernelVfsRequire(!VfsUnlink(path) && (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "a file that was open was destroyed");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(VfsUnlink(path), "a file could not be destroyed once nothing held it");
    KernelVfsRequire(KernelVfsRefusedWith(path, VFS_ERROR_NOT_FOUND),
                     "a destroyed file still resolved");

    /*
     * Directories. A new one bears two links — its own "." and the parent's
     * entry — and the parent gains one for the ".." within it. The parent's
     * count is the half that is easy to omit and impossible to see.
     */
    root_links = KernelVfsLinksOf("/");
    KernelVfsRequire(VfsCreateDirectory("/dir", 0755U), "a directory could not be created");
    KernelVfsRequire(KernelVfsInodeOfPath("/dir") != 0U, "a created directory did not resolve");
    KernelVfsRequire(KernelVfsLinksOf("/dir") == 2U,
                     "a new directory did not bear the two links \".\" and its own entry "
                     "give it");
    KernelVfsRequire(KernelVfsLinksOf("/") == (root_links + 1U),
                     "the parent did not gain the link the child's \"..\" holds");
    KernelVfsRequire(KernelVfsIs("/dir/..", EXT2_ROOT_INODE, VFS_NODE_DIRECTORY),
                     "the \"..\" of a new directory did not name its parent");

    KernelVfsRequire(!VfsCreateDirectory("/dir", 0755U) &&
                         (VfsLastErrorCode() == VFS_ERROR_EXISTS),
                     "a directory that exists was created again");
    KernelVfsRequire(!VfsRemoveDirectory("/sub") &&
                         (VfsLastErrorCode() == VFS_ERROR_NOT_EMPTY),
                     "a directory holding names was removed");
    KernelVfsRequire(!VfsUnlink("/dir") && (VfsLastErrorCode() == VFS_ERROR_IS_DIRECTORY),
                     "a directory was removed as though it were a file");

    KernelVfsRequire(VfsRemoveDirectory("/dir"), "an empty directory could not be removed");
    KernelVfsRequire(KernelVfsLinksOf("/") == root_links,
                     "the parent did not give back the link the removed child held");
    KernelVfsRequire(KernelVfsRefusedWith("/dir", VFS_ERROR_NOT_FOUND),
                     "a removed directory still resolved");
}

/*
 * Asserts the mount: that a second volume covers a directory of the first, that
 * a path crossing the mount point reaches the second volume, that ".." from the
 * root of the second arrives at the parent of the mount point in the first, and
 * that the directory the mount covers reappears when the mount is withdrawn.
 *
 * The two volumes are identical but for the owner of "/file", and every
 * assertion about which volume a path reached is made by reading that field.
 */
static void KernelVerifyVfsMounts(void)
{
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_SECOND_DEVICE, "/file", "ext2", true) &&
                         (VfsLastErrorCode() == VFS_ERROR_NOT_DIRECTORY),
                     "a volume was mounted upon something that is not a directory");
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_ROOT_DEVICE, KERNEL_VFS_MOUNT_POINT, "ext2",
                                     true) &&
                         (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "a device already mounted was mounted a second time");
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_SECOND_DEVICE, "/sub", "minix", true) &&
                         (VfsLastErrorCode() == VFS_ERROR_UNSUPPORTED),
                     "a volume was mounted as a filesystem that is not registered");

    /* Before the mount, the point is the first volume's own directory. */
    KernelVfsRequire(KernelVfsIs(KERNEL_VFS_MOUNT_POINT, KERNEL_VOLUME_SUB_INODE,
                                 VFS_NODE_DIRECTORY),
                     "the mount point was not the first volume's directory before the mount");

    KernelVfsRequire(VfsMountVolume(KERNEL_VFS_SECOND_DEVICE, KERNEL_VFS_MOUNT_POINT, "ext2",
                                    true),
                     "a second volume could not be mounted");
    KernelVfsRequire(VfsMountCount() == 2U, "two mounts were not recorded");

    /*
     * The mount point now names the root of the second volume, and not the
     * directory beneath it. Both are directories and the discriminator is what
     * lies within them.
     */
    KernelVfsRequire(KernelVfsIs(KERNEL_VFS_MOUNT_POINT, EXT2_ROOT_INODE,
                                 VFS_NODE_DIRECTORY),
                     "the mount point did not become the root of the mounted volume");
    KernelVfsRequire(KernelVfsOwnerOf("/sub/file") == KERNEL_VFS_SECOND_UID,
                     "a path crossing the mount point did not reach the second volume");
    KernelVfsRequire(KernelVfsOwnerOf("/file") != KERNEL_VFS_SECOND_UID,
                     "a path not crossing the mount point reached the second volume");

    /* What the mount covers is hidden, entirely, for as long as it stands. */
    KernelVfsRequire(KernelVfsRefusedWith("/sub/inner", VFS_ERROR_NOT_FOUND),
                     "the directory the mount covers was still reachable through it");

    /*
     * Leaving the mounted volume by "..".
     *
     * The ".." of the second volume's root names that root, which is what the
     * volume says; the parent of a mounted root is the directory the mount
     * covers, which only this layer knows. A layer matching paths by prefix
     * would answer this with the second volume's own root and would be wrong in
     * a way nothing else here would catch.
     */
    KernelVfsRequire(KernelVfsOwnerOf("/sub/../file") != KERNEL_VFS_SECOND_UID,
                     "\"..\" from the root of a mounted volume did not leave it");
    KernelVfsRequire(KernelVfsOwnerOf("/sub/../sub/file") == KERNEL_VFS_SECOND_UID,
                     "a path returning across a mount point and crossing it again did not "
                     "reach the second volume");

    /* A read-only mount refuses everything that would alter its volume. */
    KernelVfsRequire(VfsOpen("/sub/file", VFS_OPEN_WRITE, 0U) == VFS_NO_DESCRIPTOR,
                     "a file upon a read-only mount was opened for writing");
    KernelVfsRequire(VfsLastErrorCode() == VFS_ERROR_READ_ONLY,
                     "a read-only mount refused a write for the wrong reason");
    KernelVfsRequire(!VfsCreateDirectory("/sub/new", 0755U) &&
                         (VfsLastErrorCode() == VFS_ERROR_READ_ONLY),
                     "a directory was created upon a read-only mount");

    /* The root may not be withdrawn while a volume stands within it. */
    KernelVfsRequire(!VfsUnmount("/") && (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "the root was withdrawn while a volume was mounted within it");

    KernelVfsRequire(VfsUnmount(KERNEL_VFS_MOUNT_POINT),
                     "a mounted volume could not be withdrawn");
    KernelVfsRequire(VfsMountCount() == 1U, "the withdrawn mount was still recorded");

    /* What the mount covered reappears exactly as it was. */
    KernelVfsRequire(KernelVfsIs(KERNEL_VFS_MOUNT_POINT, KERNEL_VOLUME_SUB_INODE,
                                 VFS_NODE_DIRECTORY),
                     "the covered directory did not reappear when the mount was withdrawn");
    KernelVfsRequire(KernelVfsIs("/sub/inner", KERNEL_VOLUME_INNER_INODE, VFS_NODE_REGULAR),
                     "what the covered directory held did not reappear");
    KernelVfsRequire(!VfsUnmount("/sub") && (VfsLastErrorCode() == VFS_ERROR_NOT_FOUND),
                     "a mount that does not stand was withdrawn");
}

/*
 * The self-test of the virtual filesystem layer.
 *
 * Every write here is made to the two devices of memory and never to a disk the
 * machine carries, for the reason every earlier write self-test is: the volumes
 * upon those belong to whoever booted this kernel.
 */
void KernelVerifyVfs(void)
{
    BlockDevice *first;
    BlockDevice *second;
    int descriptor;

    KernelVfsSucceeded = true;

    VfsInitialise();

    if (!Ext2VfsInitialise())
    {
        KernelWriteString("  The EXT2 filesystem could not be registered.\n");
        KernelWriteString("Filesystem self-test FAILED.\n");
        return;
    }

    KernelVfsRequire(!Ext2VfsInitialise(),
                     "a filesystem type was registered twice under one name");

    first = BlockRegister(KERNEL_VFS_ROOT_DEVICE, &KernelMemoryDeviceOperations, NULL,
                          BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, false);
    second = BlockRegister(KERNEL_VFS_SECOND_DEVICE, &KernelMemoryDeviceOperations,
                           KernelMemoryDeviceSecondStore, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if ((first == NULL) || (second == NULL))
    {
        KernelWriteString("  The devices of memory could not be registered.\n");
        KernelWriteString("Filesystem self-test FAILED.\n");
        return;
    }

    /*
     * Both volumes are composed before either is mounted, and the second is a
     * copy of the first taken before the first was marked as open. The copy is
     * then given a different owner for "/file", which is the only respect in
     * which the two differ and therefore the only thing an assertion can use to
     * say which of them a path reached.
     */
    (void)BufferInvalidateDevice(first);
    (void)BufferInvalidateDevice(second);
    KernelComposeVolume();

    for (size_t index = 0U; index < sizeof KernelMemoryDeviceSecondStore; ++index)
    {
        KernelMemoryDeviceSecondStore[index] = KernelMemoryDeviceStore[index];
    }

    KernelStoreHalfIn(KernelMemoryDeviceSecondStore,
                      KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_UID),
                      KERNEL_VFS_SECOND_UID);

    (void)BufferInvalidateDevice(first);
    (void)BufferInvalidateDevice(second);

    /* Nothing is mounted, so no absolute path resolves and no mount may be made
     * anywhere but at the root. */
    KernelVfsRequire(!VfsRootIsMounted(), "a root was mounted before anything mounted one");
    KernelVfsRequire(KernelVfsRefusedWith("/", VFS_ERROR_NOT_FOUND),
                     "a path resolved with nothing mounted");
    KernelVfsRequire(!VfsMountVolume(KERNEL_VFS_ROOT_DEVICE, "/anywhere", "ext2", false) &&
                         (VfsLastErrorCode() == VFS_ERROR_INVALID),
                     "the first mount was made somewhere other than the root");
    KernelVfsRequire(!VfsMountVolume("no-such-device", "/", "ext2", false) &&
                         (VfsLastErrorCode() == VFS_ERROR_NOT_FOUND),
                     "a volume was mounted from a device that does not exist");

    if (!VfsMountVolume(KERNEL_VFS_ROOT_DEVICE, "/", "ext2", false))
    {
        KernelWriteString("  The root volume could not be mounted: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        KernelWriteString("Filesystem self-test FAILED.\n");
        (void)BufferInvalidateDevice(first);
        (void)BlockUnregister(first);
        (void)BufferInvalidateDevice(second);
        (void)BlockUnregister(second);
        return;
    }

    KernelVfsRequire(VfsRootIsMounted(), "the root mount was not recorded");
    KernelVfsRequire(VfsMountCount() == 1U, "one mount was not recorded");

    /*
     * The volume records that this kernel has it open, and the record is read
     * back out of the medium rather than out of the superblock in memory: a mark
     * that had not reached the device would protect nothing, a machine that
     * stopped being the one circumstance it exists for.
     */
    KernelVfsRequire((KernelLoadHalf(KernelMemoryDeviceStore,
                                     EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE) &
                      (uint16_t)EXT2_VALID_FS) == 0U,
                     "a volume mounted for writing was not marked as not cleanly unmounted");
    KernelVfsRequire((KernelLoadHalf(KernelMemoryDeviceStore,
                                     EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE) &
                      (uint16_t)EXT2_ERROR_FS) == 0U,
                     "a volume merely opened for writing was marked as holding errors");
    KernelVfsRequire(KernelLoadHalf(KernelMemoryDeviceStore,
                                    EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_MOUNT_COUNT) == 1U,
                     "the mount count upon the volume was not raised");

    KernelVerifyVfsResolution();
    KernelVerifyVfsFiles();
    KernelVerifyVfsDirectories();
    KernelVerifyVfsWrites();
    KernelVerifyVfsMounts();

    /* The root is not withdrawn while a file upon it is open. */
    descriptor = VfsOpen("/file", VFS_OPEN_READ, 0U);
    KernelVfsRequire(descriptor != VFS_NO_DESCRIPTOR, "a file could not be opened");
    KernelVfsRequire(!VfsUnmount("/") && (VfsLastErrorCode() == VFS_ERROR_BUSY),
                     "a volume was withdrawn while a file upon it was open");

    if (descriptor != VFS_NO_DESCRIPTOR)
    {
        KernelVfsRequire(VfsClose(descriptor), "a file could not be closed");
    }

    KernelVfsRequire(VfsSync(), "the mounts could not be written back");
    KernelVfsRequire(VfsUnmount("/"), "the root volume could not be withdrawn");
    KernelVfsRequire(!VfsRootIsMounted(), "the root was still mounted after being withdrawn");

    /*
     * Nothing was leaked. Every node this sequence took was released, and every
     * descriptor closed. This is the assertion the node cache exists to be held
     * to: a resolution that failed to release what it held would exhaust the
     * table long before a machine had done any real work, and would do so
     * silently until it did.
     */
    KernelVfsRequire(VfsNodesHeld() == 0U,
                     "nodes were left held after everything was closed and withdrawn");
    KernelVfsRequire(VfsOpenFileCount() == 0U, "descriptors were left open");

    /*
     * The volume records that it was cleanly unmounted, and describes itself
     * consistently afterwards. The descriptor table is verified here rather than
     * while the volume was mounted because a mounted volume is marked unclean and
     * is therefore permitted to disagree with itself.
     */
    KernelVfsRequire((KernelLoadHalf(KernelMemoryDeviceStore,
                                     EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_STATE) &
                      (uint16_t)EXT2_VALID_FS) != 0U,
                     "a volume withdrawn cleanly was not marked as cleanly unmounted");

    {
        Ext2Superblock superblock;

        KernelVfsRequire(Ext2ReadSuperblock(first, &superblock) &&
                             Ext2VerifyGroupDescriptors(first, &superblock),
                         "the volume did not describe itself consistently after everything "
                         "the layer did to it");
    }

    (void)BufferInvalidateDevice(first);
    (void)BlockUnregister(first);
    (void)BufferInvalidateDevice(second);
    (void)BlockUnregister(second);

    KernelWriteString(KernelVfsSucceeded ? "Filesystem self-test passed.\n"
                                         : "Filesystem self-test FAILED.\n");
}

/*
 * How many bytes the probe below writes through the filesystem layer.
 *
 * It is deliberately larger than one block of any volume this kernel accepts, so
 * that the write crosses a block boundary and the position of the descriptor is
 * carried across it, which is the whole of what the layer adds to the write of
 * sub-task 5.6.
 */
#define KERNEL_VFS_PROBE_SIZE 5000U

/*
 * Exercises a volume the machine actually carries, through the layer rather than
 * through the format.
 *
 * The discipline is the one every write self-test in this project observes, and
 * it is the discipline of a kernel that may be booted upon somebody else's
 * machine. Nothing is written unless the operator asked for it at the GRUB menu;
 * nothing is created, so a volume that does not already hold the probe file is
 * left exactly as it was; and the file acted upon bears a name nothing else
 * would choose.
 *
 * The volume is then withdrawn and mounted afresh read-only. A mount that is
 * never withdrawn has not been shown to withdraw, and withdrawing it is what
 * writes back the mark that says the volume was cleanly unmounted — so the
 * operator's disk is left clean rather than left claiming to be open, which is
 * both the better outcome and the one that demonstrates both directions of the
 * mark.
 */
void KernelVfsProbeVolume(void)
{
    static const char *const path = "/oxys-write-test";
    VfsAttributes attributes;
    uint64_t transferred = 0U;
    uint64_t index;
    int descriptor;

    if (!KernelCommandLineHasOption("ext2-write-test"))
    {
        return;
    }

    KernelWriteString("VFS write test: the command line permits writing to the mounted "
                      "volume.\n");

    if (!VfsStat(path, &attributes))
    {
        KernelWriteString("VFS write test: " "/oxys-write-test" " is not present; nothing "
                          "written.\n");
        return;
    }

    if (attributes.type != VFS_NODE_REGULAR)
    {
        KernelWriteString("VFS write test: " "/oxys-write-test" " is not a regular file; "
                          "nothing written.\n");
        return;
    }

    descriptor = VfsOpen(path, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE, 0U);

    if (descriptor == VFS_NO_DESCRIPTOR)
    {
        KernelWriteString("VFS write test: the file could not be opened: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    /*
     * The contents are derived from the offset, so that a file written from the
     * wrong place is distinguishable from one written correctly when it is
     * examined from outside with `debugfs`.
     */
    for (index = 0U; index < KERNEL_VFS_PROBE_SIZE; index += sizeof KernelFileBuffer)
    {
        uint64_t run = KERNEL_VFS_PROBE_SIZE - index;

        if (run > sizeof KernelFileBuffer)
        {
            run = sizeof KernelFileBuffer;
        }

        for (uint64_t offset = 0U; offset < run; ++offset)
        {
            KernelFileBuffer[offset] = KernelFileByteAt(index + offset);
        }

        if (!VfsWrite(descriptor, KernelFileBuffer, run, &transferred) || (transferred != run))
        {
            KernelWriteString("VFS write test: the file could not be written: ");
            KernelWriteString(VfsLastError());
            KernelWriteString("\n");
            (void)VfsClose(descriptor);
            return;
        }
    }

    /*
     * The file is read back through the same descriptor, which requires the
     * position to be moved: the whole of what a descriptor adds to a write is
     * that it remembers where it is, and reading back without seeking would read
     * from the end of what was just written.
     */
    if (!VfsSeek(descriptor, 0, VFS_SEEK_SET, NULL))
    {
        KernelWriteString("VFS write test: the position could not be moved.\n");
        (void)VfsClose(descriptor);
        return;
    }

    for (index = 0U; index < KERNEL_VFS_PROBE_SIZE; index += sizeof KernelFileBuffer)
    {
        uint64_t run = KERNEL_VFS_PROBE_SIZE - index;

        if (run > sizeof KernelFileBuffer)
        {
            run = sizeof KernelFileBuffer;
        }

        if (!VfsRead(descriptor, KernelFileBuffer, run, &transferred) || (transferred != run) ||
            (!KernelFileBufferMatches(index, run)))
        {
            KernelWriteString("VFS write test: what was read back was not what was "
                              "written.\n");
            (void)VfsClose(descriptor);
            return;
        }
    }

    (void)VfsClose(descriptor);

    if (!VfsSync() || !VfsStat(path, &attributes) ||
        (attributes.size != KERNEL_VFS_PROBE_SIZE))
    {
        KernelWriteString("VFS write test: the volume could not be written back.\n");
        return;
    }

    KernelWriteString("VFS write test: ");
    KernelWriteDecimal(KERNEL_VFS_PROBE_SIZE);
    KernelWriteString(" bytes written to " "/oxys-write-test" " and read back "
                      "identically.\n");

    /* The withdrawal, and the fresh mount that leaves the volume clean. */
    if (!VfsUnmount("/"))
    {
        KernelWriteString("VFS write test: the volume could not be withdrawn: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    KernelWriteString("VFS write test: the volume was withdrawn and marked cleanly "
                      "unmounted.\n");

    if (!VfsMountRoot("ext2", true))
    {
        KernelWriteString("VFS write test: the volume could not be mounted afresh: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    VfsReport();
}
