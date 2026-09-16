/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/vfs/file.c
 * Purpose: Implements the open file: the descriptor table, the opening of a
 *          path, the position that advances, the reading and writing of a
 *          file's bytes, the seek, and the traversal of a directory through a
 *          descriptor — and, since sub-task 8.6, the branch each of those
 *          takes where the open file is an end of a pipe.
 * Key functions: VfsFileOf, VfsOpenFileCount, VfsOpen, VfsHold, VfsClose, VfsRead,
 *          VfsWrite, VfsSeek, VfsTell, VfsReadDirectory, VfsFileAttributes.
 * References:
 *   - docs/storage/VFS.md, Sections 7 and 8: the open file, and why the position
 *     belongs to it and not to the node — which is why two descriptors upon one
 *     file read independently of one another while writing to the same bytes.
 *   - IEEE Std 1003.1-2017, the definitions of `read`, `write` and `lseek`: a
 *     count reports what was transferred and not what was asked, so a partial
 *     write is a partial write and those bytes are upon the volume.
 *
 * Upon a directory the position is the filesystem's own cookie rather than a
 * byte offset, which is why a seek upon one is refused rather than translated:
 * a cookie is not an address and arithmetic upon it names nothing.
 */

#include "internal.h"

#include <oxys/fs/vfs.h>
#include <oxys/kernel.h>
#include <oxys/block/block.h>
#include <oxys/mm/heap.h>

/* The open file a descriptor names, or null where the descriptor names none. */
VfsFile *VfsFileOf(int descriptor)
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
    file->holders = 1U;

    ++VfsFilesOpenedCount;
    VfsSucceed();
    return descriptor;
}

bool VfsHold(int descriptor)
{
    VfsFile *const file = VfsFileOf(descriptor);

    if (file == NULL)
    {
        return false;
    }

    ++file->holders;
    VfsSucceed();

    return true;
}

bool VfsClose(int descriptor)
{
    VfsFile *const file = VfsFileOf(descriptor);

    if (file == NULL)
    {
        return false;
    }

    /* One holder fewer, and the file stays open for the rest: a close by the
     * program's 4 must not pull the file from under its 1. */
    if (file->holders > 1U)
    {
        --file->holders;
        VfsSucceed();

        return true;
    }

    /* An end of a pipe has no node to release; the pipe is told the end has
     * gone, which is what wakes whoever was waiting for it. */
    if (file->pipe != NULL)
    {
        VfsPipeReleaseEnd(file);
    }
    else
    {
        VfsNodeRelease(file->node);
    }

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

    /* An end of a pipe, since sub-task 8.6: the bytes come from the pipe and
     * the read may sleep; nothing below concerns it. */
    if (file->pipe != NULL)
    {
        return VfsPipeRead(file, buffer, length, read);
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

    /* An end of a pipe, since sub-task 8.6: the bytes go to the pipe and the
     * write may sleep; no volume is written and no position advances. */
    if (file->pipe != NULL)
    {
        const bool delivered = VfsPipeWrite(file, buffer, length, written);

        VfsBytesWrittenCount += *written;

        return delivered;
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

    if (file->pipe != NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "an end of a pipe has no position, no entries and no attributes");
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

    if (file->pipe != NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "an end of a pipe has no position, no entries and no attributes");
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

    if (file->pipe != NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "an end of a pipe has no position, no entries and no attributes");
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

    if (file->pipe != NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID,
                         "an end of a pipe has no position, no entries and no attributes");
    }

    if (attributes == NULL)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "there is nowhere to put the description");
    }

    VfsNodeAttributes(file->node, attributes);

    VfsSucceed();
    return true;
}

