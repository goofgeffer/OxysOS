/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/vfs/internal.h
 * Purpose: Declares what the translation units of the virtual filesystem layer
 *          share with one another and with nothing else: the four fixed tables
 *          the layer's whole state lives in, the record of the most recent
 *          refusal and the accounting beside it, the open file, and the
 *          resolution and node-cache primitives every operation reaches a file
 *          through.
 * Key definitions: VfsFile, the four tables, VfsRootMount, the refusal record
 *          and the accounting, VfsRefuse, VfsSucceed, VfsStringLength,
 *          VfsSameString, VfsCopyString, VfsNodeHold, VfsNodeAcquire, VfsWalk,
 *          VfsResolveCounted, VfsResolveParent, VfsWritable, VfsFileOf,
 *          VfsMountIsBusy, VfsFindFilesystem.
 * References:
 *   - The behaviour these declare is specified where it is implemented.
 *     `kernel/include/oxys/vfs.h` is the public interface and
 *     `docs/storage/VFS.md` the design; neither is restated here.
 *
 * Why this header exists, and why it is not in kernel/include/oxys/.
 *
 *   `kernel/include/oxys/vfs.h` is what a consumer depends upon — the node, the
 *   mount, the operations a filesystem supplies, and the calls above them.
 *   Nothing outside `kernel/fs/vfs/` may depend upon anything declared here.
 *   These are the seams left by dividing one translation unit of 2,355 lines
 *   into six; before the division they were file-scope statics, and they would
 *   be statics still if C offered any way of sharing them among a chosen few.
 *
 *   `docs/design/ARCHITECTURE.md` records the rule.
 *
 * Nothing here is guarded against concurrent access.
 *
 *   The tables, the refusal record and the counters all become the business of
 *   the lock of sub-task 6.13. The node cache is the most urgent of them: two
 *   processors acquiring the same file at once would each find no node holding
 *   it and each make one, and two nodes for one file is exactly the condition
 *   this layer exists to prevent — see `docs/storage/VFS.md`.
 */

#ifndef OXYS_FS_VFS_INTERNAL_H
#define OXYS_FS_VFS_INTERNAL_H

#include <oxys/fs/vfs.h>
#include <oxys/block/block.h>
#include <oxys/types.h>

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

    /*
     * How many descriptors name this open file, of sub-task 8.5: one at
     * VfsOpen, one more per VfsHold, one fewer per VfsClose, and the file is
     * released at zero. It is what lets two numbers — a parent's and its
     * child's, or a program's 1 and the 4 it was duplicated from — share one
     * open file and one position, which is what `fork` inheriting a descriptor
     * and `dup2` both mean.
     */
    uint32_t holders;
    bool open;

    /*
     * The pipe this open file is an end of, of sub-task 8.6, or null for a
     * file with a node beneath it. Exactly one of `node` and `pipe` is set:
     * an end of a pipe has no node, and `VfsRead`, `VfsWrite`, `VfsClose` and
     * the rest ask which before they touch the node. `flags` says which end
     * it is — VFS_OPEN_READ for the end that reads, VFS_OPEN_WRITE for the
     * end that writes — and nothing else is set upon it.
     */
    struct VfsPipe *pipe;
} VfsFile;

/*
 * The whole of the layer's state, defined in `vfs.c`.
 *
 * Four fixed tables and nothing allocated. The layer must be able to open a file
 * when the heap is exhausted, because the commonest reason to want one at that
 * moment is that something needed to write a diagnostic to a file. Only the
 * filesystems' private descriptions — a superblock, an inode — are allocated,
 * and those are bounded by these arrays.
 */
extern VfsFilesystem VfsFilesystems[VFS_FILESYSTEM_CAPACITY];
extern VfsMount VfsMounts[VFS_MOUNT_CAPACITY];
extern VfsNode VfsNodes[VFS_NODE_CAPACITY];
extern VfsFile VfsFiles[VFS_FILE_CAPACITY];

/* The mount at the root of the tree, through which every absolute path begins. */
extern VfsMount *VfsRootMount;

/*
 * Whether a mount's volume outlives the machine and may be written: any writable
 * mount but the root, the root being the ramdisk. A change to such a volume is
 * written back when it completes, since 2026-09-23 for a file closed and since
 * 2026-09-24 for a name made or removed — an emulator stopped by closing its
 * window would otherwise lose it. docs/storage/PERSIST.md.
 */
static inline bool VfsMountIsDurable(const VfsMount *mount)
{
    return (mount != NULL) && (mount != VfsRootMount) && !mount->read_only;
}

/* The most recent refusal, and the accounting. */
extern VfsError VfsRefusalCode;
extern const char *VfsRefusalReason;
extern uint64_t VfsResolvedCount;
extern uint64_t VfsResolveRefusedCount;
extern uint64_t VfsNodesReadCount;
extern uint64_t VfsNodeHitCount;
extern uint64_t VfsFilesOpenedCount;
extern uint64_t VfsBytesReadCount;
extern uint64_t VfsBytesWrittenCount;
extern uint64_t VfsRefusalCount;

/*
 * Records a refusal and returns false, or clears the record upon success.
 *
 * Every refusal in this layer goes through `VfsRefuse`, so that a caller may
 * write `return VfsRefuse(code, "...")` and neither forget the accounting nor
 * state the reason twice. `VfsSucceed` clears the record, so that a reason left
 * standing from an earlier call is never reported for a later one that worked.
 */
bool VfsRefuse(VfsError code, const char *reason);
void VfsSucceed(void);

/*
 * The string primitives, in `vfs.c`.
 *
 * Bounded by construction: there is no C library until Phase 7, and a name in
 * this layer is given by its address and its length rather than by a terminator,
 * so every one of these takes the capacity it may not read beyond.
 */
size_t VfsStringLength(const char *string, size_t capacity);
bool VfsSameString(const char *left, const char *right, size_t capacity);
void VfsCopyString(char *destination, const char *source, size_t capacity);

/*
 * The node cache, in `node.c`.
 *
 * `VfsNodeAcquire` is the only route by which a node comes into existence, which
 * is what makes one file one node however many callers reach it.
 */
void VfsNodeHold(VfsNode *node);
VfsNode *VfsNodeAcquire(VfsMount *mount, uint64_t number);

/*
 * Resolution, in `path.c`.
 *
 * `VfsWalk` takes a length as well as a path, and that is what makes the rest of
 * this layer free of buffers: the parent of a path is resolved by walking the
 * prefix of that path where it stands, so nothing copies a path or a component
 * out of the caller's string.
 */
bool VfsWalk(VfsNode *start, const char *path, size_t length, bool follow_last,
             unsigned int depth, VfsNode **node);
/* The length of a path, refused where it is absent, empty, or longer than this
 * layer resolves. Shared with `mount.c`, a mount point being a path. */
bool VfsPathLength(const char *path, size_t *length);

bool VfsResolveCounted(const char *path, bool follow_last, VfsNode **node);
bool VfsResolveParent(const char *path, VfsNode **parent, const char **name,
                      size_t *name_length);
bool VfsWritable(const VfsNode *node);

/* The open file a descriptor names, or null where it names none. In `file.c`. */
VfsFile *VfsFileOf(int descriptor);

/* The pipe's own half of the open-file operations, of sub-task 8.6, in
 * pipe.c; each is reached from the function of the same name in file.c when
 * the open file is an end of a pipe. */
bool VfsPipeRead(VfsFile *file, void *buffer, uint64_t length, uint64_t *read);

/* Whether a read of this end would return without sleeping, for `poll` of
 * sub-task 9.6. An end whose last writer has closed is readable: the zero a
 * read returns there is an answer. */
bool VfsPipeIsReadable(const VfsFile *file);
bool VfsPipeWrite(VfsFile *file, const void *buffer, uint64_t length, uint64_t *written);
void VfsPipeReleaseEnd(VfsFile *file);

/* Whether anything still holds a mount, in `mount.c`. */
bool VfsMountIsBusy(const VfsMount *mount);

/* The registered filesystem of a given name, in `mount.c`. */
VfsFilesystem *VfsFindFilesystem(const char *name);

#endif /* OXYS_FS_VFS_INTERNAL_H */
