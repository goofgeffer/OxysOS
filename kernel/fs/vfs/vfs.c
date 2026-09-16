/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/vfs/vfs.c
 * Purpose: Holds the state the virtual filesystem layer is made of — the four
 *          fixed tables, the record of the most recent refusal and the
 *          accounting beside it — together with the refusal every operation
 *          reports through, the names of the error codes and node types, the
 *          bounded string primitives, and the report.
 * Key functions: VfsSetError, VfsRefuse, VfsSucceed, VfsLastError,
 *          VfsLastErrorCode, VfsErrorName, VfsNodeTypeName, VfsStringLength,
 *          VfsSameString, VfsCopyString, the accounting accessors, VfsReport,
 *          VfsReportDirectory.
 * References:
 *   - docs/storage/VFS.md: the design of this layer, the reasons for its shape,
 *     and what it does not promise.
 *   - Ritchie, D. M. and Thompson, K., "The UNIX Time-Sharing System",
 *     Communications of the ACM 17(7), 1974, Section 3.4: the mount as the
 *     replacement of a leaf of one tree by the root of another.
 *
 * This file is the layer's ground floor, and the parts above it are the five
 * files beside this one; `internal.h` is what joins them. The layer was one
 * translation unit of 2,355 lines until the review that followed sub-task 6.10,
 * and docs/design/ARCHITECTURE.md, Section 2.2, records why it is no longer.
 *
 * Design note. There is no string prefix matching anywhere in this layer. A
 * mount is found through the *node* it covers and not through the path it was
 * mounted at, which is the difference between a layer that composes a tree and
 * one that rewrites paths. The path a mount was made at is retained for a report
 * and for nothing else. Matching by prefix appears to work and then fails in
 * three ways that have no remedy within it: a symbolic link whose target crosses
 * a mount point is not a path any prefix describes; ".." leaving a mounted
 * volume must arrive at the parent of the mount point rather than at the
 * volume's own root, and the prefix has no way to know it has left; and a path
 * reaching one directory by two routes would be matched against one prefix and
 * not the other.
 *
 * Concurrency. This implementation is not yet safe against concurrent access.
 * The spinlock of sub-task 6.13 exists and has not been applied here; the mount
 * table, the node table and the open file table each require one, and a node's
 * reference count must be adjusted atomically.
 * The open file table additionally becomes per-process in Phase 7 — not in
 * sub-task 6.9, which introduced the process control block and gave it no file
 * descriptors at all; see docs/design/PROCESS.md, limitation 6. Nothing here
 * assumes otherwise; the table is simply global while there is one thread of
 * control.
 */

#include "internal.h"

#include <oxys/fs/vfs.h>
#include <oxys/block/buffer.h>
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

VfsFilesystem VfsFilesystems[VFS_FILESYSTEM_CAPACITY];
VfsMount VfsMounts[VFS_MOUNT_CAPACITY];
VfsNode VfsNodes[VFS_NODE_CAPACITY];


VfsFile VfsFiles[VFS_FILE_CAPACITY];

/* The mount at the root of the tree, through which every absolute path begins. */
VfsMount *VfsRootMount;

/* ---------------------------------------------------------------------------
 * Refusals and accounting.
 * ------------------------------------------------------------------------- */

VfsError VfsRefusalCode;
const char *VfsRefusalReason = "no refusal has been recorded";

uint64_t VfsResolvedCount;
uint64_t VfsResolveRefusedCount;
uint64_t VfsNodesReadCount;
uint64_t VfsNodeHitCount;
uint64_t VfsFilesOpenedCount;
uint64_t VfsBytesReadCount;
uint64_t VfsBytesWrittenCount;
uint64_t VfsRefusalCount;

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
bool VfsRefuse(VfsError code, const char *reason)
{
    return VfsSetError(code, reason);
}

/* Records that nothing has gone wrong, so that a stale diagnosis is not read as
 * a fresh one. */
void VfsSucceed(void)
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
    case VFS_ERROR_BROKEN_PIPE:
        return "broken pipe";
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

size_t VfsStringLength(const char *string, size_t capacity)
{
    size_t length = 0U;

    while ((length < capacity) && (string[length] != '\0'))
    {
        ++length;
    }

    return length;
}

bool VfsSameString(const char *left, const char *right, size_t capacity)
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

void VfsCopyString(char *destination, const char *source, size_t capacity)
{
    size_t index = 0U;

    while (((index + 1U) < capacity) && (source[index] != '\0'))
    {
        destination[index] = source[index];
        ++index;
    }

    destination[index] = '\0';
}

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
