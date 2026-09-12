/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/internal.h
 * Purpose: Declares what the translation units of the EXT2 implementation share
 *          with one another and with nothing else: the record of the most recent
 *          refusal, the accounting each of them adds to, the decoding and
 *          encoding of the volume's stored byte order, and the block-level
 *          transfer through which every one of them reaches the medium.
 * Key definitions: Ext2Error, the accounting counters, Ext2Refuse and its
 *          siblings, Ext2ReadHalf, Ext2ReadWord, Ext2ReadText, Ext2WriteHalf,
 *          Ext2WriteWord, Ext2ReadBytes, Ext2WriteBytes, Ext2ZeroBlock,
 *          Ext2BlockExists, Ext2Writable, Ext2FillZero, Ext2DivideRoundingUp,
 *          Ext2PointersPerBlock, Ext2SectorsPerBlock.
 * References:
 *   - The specifications are cited where the behaviour they govern is
 *     implemented. This header declares an interface internal to the
 *     implementation and states no hardware or format behaviour of its own;
 *     `kernel/include/oxys/ext2.h` is the public one.
 *
 * Why this header exists, and why it is not in kernel/include/oxys/.
 *
 *   `kernel/include/oxys/` is the corpus a *consumer* depends upon, and nothing
 *   outside `kernel/fs/ext2/` may depend upon anything declared here. These are
 *   the seams left by dividing one translation unit into several: before the
 *   division they were file-scope statics, and they would be statics still if C
 *   offered any way of sharing them among a group of files without also
 *   offering them to everybody.
 *
 *   Placing it beside the implementation is what records that limit. A header in
 *   the public corpus is an invitation; one here is a note between the parts of
 *   a single subsystem, and the directory boundary is the whole of its
 *   enforcement.
 */

#ifndef OXYS_FS_EXT2_INTERNAL_H
#define OXYS_FS_EXT2_INTERNAL_H

#include <oxys/fs/ext2.h>
#include <oxys/types.h>

/* The greatest value of s_log_block_size this kernel will accept. */
#define EXT2_MAXIMUM_LOG_BLOCK_SIZE 2U

/* How many entries of a directory a report writes out before it summarises. */
#define EXT2_REPORTED_ENTRIES 16U

/*
 * A description of the most recent refusal, and the accounting.
 *
 * One record of the last refusal is kept for the whole implementation rather
 * than one per module, because a caller asks `Ext2LastError` what went wrong
 * without knowing which part of the format refused it — and the answer must be
 * the reason it was actually refused for, whichever that was.
 *
 * These are defined in `core.c`. They are not guarded against concurrent access
 * and must acquire the lock of sub-task 6.13 with the rest of the filesystem
 * layer's state.
 */
extern const char *Ext2Error;
extern uint64_t Ext2Read;
extern uint64_t Ext2Refused;
extern uint64_t Ext2GroupsReadCount;
extern uint64_t Ext2GroupsRefusedCount;
extern uint64_t Ext2InodesReadCount;
extern uint64_t Ext2InodesRefusedCount;
extern uint64_t Ext2EntriesReadCount;
extern uint64_t Ext2EntriesRefusedCount;
extern uint64_t Ext2PathsResolvedCount;
extern uint64_t Ext2PathsRefusedCount;
extern uint64_t Ext2FilesReadCount;
extern uint64_t Ext2BytesReadCount;
extern uint64_t Ext2ReadsRefusedCount;
extern uint64_t Ext2BlocksAllocatedCount;
extern uint64_t Ext2BlocksFreedCount;
extern uint64_t Ext2InodesAllocatedCount;
extern uint64_t Ext2InodesFreedCount;
extern uint64_t Ext2BytesWrittenCount;
extern uint64_t Ext2WritesRefusedCount;
extern uint64_t Ext2NamesInsertedCount;
extern uint64_t Ext2NamesRemovedCount;
extern uint64_t Ext2FilesCreatedCount;
extern uint64_t Ext2FilesDestroyedCount;

/*
 * The refusals.
 *
 * Each records the reason and increments the counter belonging to the kind of
 * operation refused, then returns false, so that a caller may write
 * `return Ext2Refuse("...")` and neither forget the accounting nor state the
 * reason twice. They are counted apart from one another because a volume that
 * cannot be read at all and a single directory entry that contradicts the format
 * are different conditions, and a report that added them together would say
 * neither.
 */
bool Ext2Refuse(const char *reason);
bool Ext2GroupRefuse(const char *reason);
bool Ext2InodeRefuse(const char *reason);
bool Ext2EntryRefuse(const char *reason);
bool Ext2PathRefuse(const char *reason);
bool Ext2ReadRefuse(const char *reason);
bool Ext2WriteRefuse(const char *reason);

/* The decoders: the volume's stored order into the processor's. */
uint16_t Ext2ReadHalf(const uint8_t *raw, size_t offset);
uint32_t Ext2ReadWord(const uint8_t *raw, size_t offset);
void Ext2ReadText(const uint8_t *raw, size_t offset, size_t length, char *destination);

/* The encoders, the exact inverses of the two above. */
void Ext2WriteHalf(uint8_t *raw, size_t offset, uint16_t value);
void Ext2WriteWord(uint8_t *raw, size_t offset, uint32_t value);

/* The number of groups needed to hold a quantity divided into fixed parts. */
uint32_t Ext2DivideRoundingUp(uint32_t quantity, uint32_t divisor);

/* Fills a run of bytes with zeroes. There is no C library until Phase 7. */
void Ext2FillZero(uint8_t *destination, uint32_t length);

/* Whether a block number names a block the volume actually has. */
bool Ext2BlockExists(const Ext2Superblock *superblock, uint32_t block);

/* Whether the volume may be written at all, which its feature flags decide. */
bool Ext2Writable(const Ext2Superblock *superblock);

/* The number of block pointers one block holds, by which every level of the
 * indirection branches. */
uint32_t Ext2PointersPerBlock(const Ext2Superblock *superblock);

/* How many 512-byte sectors one block of the volume occupies. */
uint32_t Ext2SectorsPerBlock(const Ext2Superblock *superblock);

/* The block-level transfer every part of this implementation reaches the medium
 * through, in both directions, and the zeroing of a whole block. */
bool Ext2ReadBytes(BlockDevice *device, const Ext2Superblock *superblock,
                   uint32_t block, uint32_t offset, uint32_t length, uint8_t *destination);
bool Ext2WriteBytes(BlockDevice *device, const Ext2Superblock *superblock,
                    uint32_t block, uint32_t offset, uint32_t length, const uint8_t *source);
bool Ext2ZeroBlock(BlockDevice *device, const Ext2Superblock *superblock, uint32_t block);

/*
 * Releases every block a file holds beyond a given size, without touching the
 * size the inode records.
 *
 * Implemented in `file.c` and shared with `directory.c`, which is the one seam
 * between the two: unlinking the last name of a file must release the blocks the
 * file held, and removing a directory must release the block its own entries
 * lived in. The public `Ext2TruncateFile` is not what those callers want — it
 * maintains `i_size`, and a file being destroyed has no size left to maintain.
 */
bool Ext2TruncateBlocks(BlockDevice *device, Ext2Superblock *superblock,
                        Ext2Inode *inode, uint64_t size);

/*
 * The directory entry codec, implemented in `directory.c`.
 *
 * `name.c` shares it because inserting and removing a record means reading the
 * records already there: a name is inserted into the slack of an existing entry,
 * and removing one means lengthening the entry before it to cover it. Both are
 * operations upon records this file's traversal already knows how to decode, and
 * a second decoder written for the mutating side would be a second place for the
 * format to be misread.
 *
 * `Ext2VolumeStatesFileType` is shared with them because the two bytes at offset
 * 6 of a record are either a name length of eight bits and a file type, or a
 * name length of sixteen — and which of the two applies must be decided
 * identically wherever a record is read or written.
 */
bool Ext2VolumeStatesFileType(const Ext2Superblock *superblock);
bool Ext2ReadEntryHeader(BlockDevice *device, const Ext2Superblock *superblock,
                         uint32_t block, uint32_t offset, Ext2DirectoryEntry *entry);
bool Ext2ReadEntryName(BlockDevice *device, const Ext2Superblock *superblock,
                       Ext2DirectoryEntry *entry);

#endif /* OXYS_FS_EXT2_INTERNAL_H */
