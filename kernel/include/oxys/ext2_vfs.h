/*
 * File: kernel/include/oxys/ext2_vfs.h
 * Purpose: Declares the binding of the EXT2 implementation to the virtual
 *          filesystem layer: the one call that registers "ext2" as a type a
 *          volume may be mounted as.
 * Key definitions: Ext2VfsInitialise.
 * References:
 *   - docs/storage/VFS.md, Section 3: what a filesystem must supply to be
 *     mountable, and why the binding stands apart from the format.
 *   - docs/storage/VFS.md, Section 8: the mount, and what it records upon a
 *     volume it has opened for writing.
 *
 * This header exists rather than the declaration being added to `ext2.h`
 * because it states a dependency that `ext2.h` must not acquire. `ext2.c`
 * implements a format and knows nothing of mounts, nodes or descriptors;
 * `ext2_vfs.c` knows both and is the only file that does. Were the declaration
 * placed in `ext2.h`, every consumer of the format would compile against the
 * filesystem layer as well, and the direction of the dependency — the layer
 * depends upon the format, and not the reverse — would no longer be legible
 * from the includes.
 */

#ifndef OXYS_EXT2_VFS_H
#define OXYS_EXT2_VFS_H

#include <oxys/types.h>

/*
 * Registers "ext2" with the virtual filesystem layer, so that a volume may be
 * mounted as one. VfsInitialise must have run first.
 *
 * Returns false only where the registration itself was refused, which means the
 * filesystem table is full or a type of that name stands already.
 */
bool Ext2VfsInitialise(void);

#endif /* OXYS_EXT2_VFS_H */
