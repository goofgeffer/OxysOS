/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/vfs/node.c
 * Purpose: Implements the node cache: the identity a file has within the kernel,
 *          the reference by which it is held, its release, and the attributes a
 *          caller reads from it.
 * Key functions: VfsNodeHold, VfsNodeAcquire, VfsNodeRelease, VfsNodesHeld,
 *          VfsNodeAttributes.
 * References:
 *   - docs/storage/VFS.md, Section 6: the node, why one file must be one node,
 *     and why nothing is retained after its last reference goes.
 *
 * A node is the identity of a file within the kernel: two callers reaching one
 * file by any route hold one node. That is a correctness requirement and not a
 * convenience. Were each caller to hold a copy of the file's description, a
 * write through one that extended the file would leave the other's copy holding
 * the old size and the old block pointers, and the next write through that copy
 * would restore them — truncating the file and orphaning every block the first
 * write allocated, silently.
 *
 * VfsNodeAcquire is therefore the only route by which a node comes into
 * existence, and it searches before it allocates.
 */

#include "internal.h"

#include <oxys/fs/vfs.h>
#include <oxys/kernel.h>
#include <oxys/block/block.h>
#include <oxys/mm/heap.h>

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

void VfsNodeHold(VfsNode *node)
{
    ++node->references;
}

/*
 * Produces the node for a file, reading it where it is not already held, and
 * takes a reference upon it.
 */
VfsNode *VfsNodeAcquire(VfsMount *mount, uint64_t number)
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

