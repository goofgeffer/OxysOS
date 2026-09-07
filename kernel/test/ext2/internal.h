/*
 * File: kernel/test/ext2/internal.h
 * Purpose: Declares what the parts of the EXT2 self-test share with one another
 *          and with the entry point that runs them: the five chapters of
 *          assertions, the restoration of the composed volume between those that
 *          alter it, and the two helpers more than one chapter judges through.
 * Key definitions: KernelVerifyExt2Groups, KernelVerifyExt2Inodes,
 *          KernelVerifyExt2Directories, KernelVerifyExt2Files,
 *          KernelVerifyExt2Writes, KernelVerifyExt2DirectoryWrites,
 *          KernelVerifyExt2VolumeRefusedWith, KernelVerifyExt2PathIs,
 *          KernelRestoreVolume.
 * References:
 *   - docs/storage/EXT2.md, Section 14: every assertion these files make, paired
 *     with the silent failure it catches. The reasoning belongs there and is not
 *     restated here.
 *   - kernel/test/README.md: the arrangement of the boot-time self-tests, and
 *     the distinction between a test and a probe that `probe.c` sits upon the
 *     far side of.
 *
 * Why this header exists.
 *
 *   `kernel/test/verify_ext2.c` reached 2,618 lines, of which the entry point
 *   was the last 291. These declarations were file-scope statics until it was
 *   divided; they are shared now for the same reason the implementation's own
 *   `internal.h` shares its helpers, and are placed beside the tests rather than
 *   in `kernel/include/oxys/verify.h` because nothing outside this directory has
 *   any business calling a chapter of one subsystem's self-test.
 *
 *   `verify.h` continues to declare `KernelVerifyExt2` and `KernelReportVolumes`
 *   alone, which is the whole of what `KernelMain` knows about any of this.
 */

#ifndef OXYS_TEST_EXT2_INTERNAL_H
#define OXYS_TEST_EXT2_INTERNAL_H

#include <oxys/types.h>
#include <oxys/block.h>
#include <oxys/ext2.h>

/*
 * The chapters of the self-test, in the order the entry point runs them.
 *
 * Each returns false having already said what failed and why, so that the entry
 * point decides only whether the whole passed. None halts the machine: a kernel
 * that stopped at the first failed assertion would report one failure where it
 * might have reported six, and the run that matters most is the one where
 * several things are broken at once.
 */
bool KernelVerifyExt2Groups(BlockDevice *device, const Ext2Superblock *superblock);
bool KernelVerifyExt2Inodes(BlockDevice *device, const Ext2Superblock *superblock);
bool KernelVerifyExt2Directories(BlockDevice *device, const Ext2Superblock *superblock);
bool KernelVerifyExt2Files(BlockDevice *device, const Ext2Superblock *superblock);
bool KernelVerifyExt2Writes(BlockDevice *device, Ext2Superblock *superblock);
bool KernelVerifyExt2DirectoryWrites(BlockDevice *device, Ext2Superblock *superblock);

/*
 * Composes the fixture volume afresh and empties the cache of it.
 *
 * Shared because the chapters that alter a volume must each begin from the
 * volume the others assert against, and because the entry point restores it
 * between them. The cache is emptied first and not afterwards: a buffer holding
 * the previous contents would answer the next read from memory the composition
 * never reached.
 */
void KernelRestoreVolume(BlockDevice *device);

/*
 * Whether a volume is refused when one field of its superblock is made wrong.
 *
 * Implemented with the superblock assertions in `format.c` and shared with the
 * entry point, which applies it to every field that must be refused. It writes
 * the value, reads the volume, restores the field and invalidates the cache on
 * both sides of the alteration.
 */
bool KernelVerifyExt2VolumeRefusedWith(BlockDevice *device, size_t offset, uint32_t value,
                                       bool half);

/*
 * Whether a path resolves to the inode expected of it.
 *
 * Implemented with the directory assertions in `directory.c` and shared with the
 * file and writing chapters, both of which judge their work by what a path
 * afterwards names — a file whose contents are right and whose name reaches
 * something else being a failure neither of them would otherwise see.
 */
bool KernelVerifyExt2PathIs(BlockDevice *device, const Ext2Superblock *superblock,
                            const char *path, uint32_t expected);

#endif /* OXYS_TEST_EXT2_INTERNAL_H */
