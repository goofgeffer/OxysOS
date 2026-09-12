/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/ext2/core.c
 * Purpose: Implements what every other translation unit of the EXT2
 *          implementation stands upon: the record of the most recent refusal and
 *          the accounting beside it, the decoding and encoding of the volume's
 *          stored byte order, the block-level transfer through which the medium
 *          is reached in either direction, and the few pieces of geometric
 *          arithmetic that more than one of them needs.
 * Key functions: Ext2Refuse, Ext2GroupRefuse, Ext2InodeRefuse, Ext2EntryRefuse,
 *          Ext2PathRefuse, Ext2ReadRefuse, Ext2WriteRefuse, Ext2ReadHalf,
 *          Ext2ReadWord, Ext2ReadText, Ext2WriteHalf, Ext2WriteWord,
 *          Ext2DivideRoundingUp, Ext2ReadBytes, Ext2WriteBytes, Ext2ZeroBlock,
 *          Ext2BlockExists, Ext2Writable, Ext2FillZero, Ext2PointersPerBlock,
 *          Ext2SectorsPerBlock, Ext2LastError and the accounting accessors.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout": the
 *     volume records every multi-byte field in little-endian order, whatever the
 *     order of the processor reading it, which is why the decoders here exist
 *     and why no structure is ever overlaid upon a buffer of the volume's bytes.
 *     `docs/project/CODING-STANDARDS.md`, Section 7.1, records that rule and the
 *     reasons for it.
 *   - The same, the Superblock chapter: the read-only compatible feature set,
 *     upon which `Ext2Writable` turns.
 *
 * This file holds the state the implementation shares.
 *
 *   Before the division of sub-task 6.11's cleanup these were file-scope statics
 *   of a single 4,325-line translation unit. They are the reason the division
 *   needed a private header at all: a counter incremented in one file and read
 *   in another cannot be static, and C offers no way to share it with a chosen
 *   few. `internal.h` is that header, and the directory is what bounds it.
 */

#include "internal.h"

#include <oxys/block/buffer.h>
#include <oxys/kernel.h>

/* Defined here and declared in internal.h; see the note there. */
const char *Ext2Error = "none";
uint64_t Ext2Read;
uint64_t Ext2Refused;
uint64_t Ext2GroupsReadCount;
uint64_t Ext2GroupsRefusedCount;
uint64_t Ext2InodesReadCount;
uint64_t Ext2InodesRefusedCount;
uint64_t Ext2EntriesReadCount;
uint64_t Ext2EntriesRefusedCount;
uint64_t Ext2PathsResolvedCount;
uint64_t Ext2PathsRefusedCount;
uint64_t Ext2FilesReadCount;
uint64_t Ext2BytesReadCount;
uint64_t Ext2ReadsRefusedCount;
uint64_t Ext2BlocksAllocatedCount;
uint64_t Ext2BlocksFreedCount;
uint64_t Ext2InodesAllocatedCount;
uint64_t Ext2InodesFreedCount;
uint64_t Ext2BytesWrittenCount;
uint64_t Ext2WritesRefusedCount;
uint64_t Ext2NamesInsertedCount;
uint64_t Ext2NamesRemovedCount;
uint64_t Ext2FilesCreatedCount;
uint64_t Ext2FilesDestroyedCount;

/* Records a refusal, so that a report may say why and not merely that. */
bool Ext2Refuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2Refused;
    return false;
}

/*
 * The same for a group descriptor, counted apart from the volumes.
 *
 * A volume refused and a descriptor refused are different events with different
 * causes, and a single counter reporting their sum would say that something was
 * wrong without saying what kind of thing.
 */
bool Ext2GroupRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2GroupsRefusedCount;
    return false;
}

/* The same again for an inode, counted apart from both. */
bool Ext2InodeRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2InodesRefusedCount;
    return false;
}

/* The same for a directory entry whose record contradicts the format. */
bool Ext2EntryRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2EntriesRefusedCount;
    return false;
}

/*
 * The same for a lookup or a path that resolved to nothing.
 *
 * This is counted apart from the entry refusals above because the two are not
 * the same kind of event and only one of them is a fault. A path that names no
 * file is an ordinary answer to an ordinary question, and it will be the common
 * case once a shell is asking; an entry that contradicts the format is a volume
 * that cannot be trusted. A single counter would report their sum and so would
 * report neither.
 */
bool Ext2PathRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2PathsRefusedCount;
    return false;
}

/* The same for a read of a file's contents, counted apart from the rest. */
bool Ext2ReadRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2ReadsRefusedCount;
    return false;
}

/*
 * The same for anything that would alter a volume.
 *
 * Counted apart from every other refusal because it is the only one that
 * describes something not done to somebody's data. A rising count of these is
 * the sign of a volume being written by a kernel that should not be writing it.
 */
bool Ext2WriteRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2WritesRefusedCount;
    return false;
}

/*
 * The decoders.
 *
 * Every quantity upon an EXT2 volume is stored least significant byte first,
 * whatever the machine that wrote it and whatever the machine that reads it. The
 * decoding is therefore written out rather than obtained by laying a structure
 * over the bytes: a structure would be correct only upon a little-endian
 * processor and would additionally require the packing of a structure, which is
 * a compiler extension this project does not admit. Written out, the byte order
 * of the volume is stated in the code that depends upon it.
 */
uint16_t Ext2ReadHalf(const uint8_t *raw, size_t offset)
{
    return (uint16_t)((uint16_t)raw[offset] | ((uint16_t)raw[offset + 1U] << 8));
}

uint32_t Ext2ReadWord(const uint8_t *raw, size_t offset)
{
    return (uint32_t)raw[offset] | ((uint32_t)raw[offset + 1U] << 8) |
           ((uint32_t)raw[offset + 2U] << 16) | ((uint32_t)raw[offset + 3U] << 24);
}

/*
 * Copies a fixed-length character field, which is padded with zero bytes rather
 * than terminated, into a buffer that is terminated. A field that is entirely
 * full has no terminator upon the volume at all, which is why the destination is
 * one character longer than the field.
 */
void Ext2ReadText(const uint8_t *raw, size_t offset, size_t length, char *destination)
{
    size_t index = 0U;

    while ((index < length) && (raw[offset + index] != 0U))
    {
        destination[index] = (char)raw[offset + index];
        ++index;
    }

    destination[index] = '\0';
}

/* The number of groups needed to hold a quantity divided into fixed parts. */
uint32_t Ext2DivideRoundingUp(uint32_t quantity, uint32_t divisor)
{
    return (quantity + (divisor - 1U)) / divisor;
}

/*
 * Reads a run of bytes from within one block of the volume, through the buffer
 * cache.
 *
 * A filesystem block is some whole number of the device's blocks, so a run
 * within one may span several of them; the loop copies from each in turn. The
 * caller asks for the bytes it needs and no more — a group descriptor is 32
 * bytes and a block pointer is four — because the cache is holding the block
 * regardless, and copying a whole 4 KiB block onto the kernel stack to take four
 * bytes out of it would be both wasteful and a stack the kernel cannot spare.
 */
bool Ext2ReadBytes(BlockDevice *device, const Ext2Superblock *superblock,
                          uint32_t block, uint32_t offset, uint32_t length,
                          uint8_t *destination)
{
    uint64_t position;

    if (block >= superblock->block_count)
    {
        return Ext2Refuse("a read was attempted beyond the end of the volume");
    }

    if ((offset > superblock->block_size) || (length > (superblock->block_size - offset)))
    {
        return Ext2Refuse("a read was attempted beyond the end of a block");
    }

    position = ((uint64_t)block * superblock->block_size) + offset;

    while (length > 0U)
    {
        const uint64_t device_block = position / device->block_size;
        const uint32_t within = (uint32_t)(position % device->block_size);
        uint32_t take = device->block_size - within;
        Buffer *buffer;

        if (take > length)
        {
            take = length;
        }

        buffer = BufferGet(device, device_block);

        if (buffer == NULL)
        {
            return Ext2Refuse("a block of the volume could not be read");
        }

        for (uint32_t index = 0U; index < take; ++index)
        {
            destination[index] = buffer->data[within + index];
        }

        BufferRelease(buffer);

        destination += take;
        position += take;
        length -= take;
    }

    return true;
}

/*
 * Whether a block identifier read from the volume addresses a block that exists.
 *
 * Nothing of a filesystem lies before the first data block, so an identifier
 * below it is as wrong as one beyond the end, and both are checked wherever an
 * identifier is read rather than where it is used: a block number used unchecked
 * addresses somebody else's data with no symptom whatever.
 */
bool Ext2BlockExists(const Ext2Superblock *superblock, uint32_t block)
{
    return (block >= superblock->first_data_block) && (block < superblock->block_count);
}
void Ext2WriteHalf(uint8_t *raw, size_t offset, uint16_t value)
{
    raw[offset] = (uint8_t)(value & 0xFFU);
    raw[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

void Ext2WriteWord(uint8_t *raw, size_t offset, uint32_t value)
{
    raw[offset] = (uint8_t)(value & 0xFFU);
    raw[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
    raw[offset + 2U] = (uint8_t)((value >> 16) & 0xFFU);
    raw[offset + 3U] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Whether a volume may be altered at all.
 *
 * Asked before anything else by every function that writes, so that a volume
 * this kernel judged unsafe to write is refused once, in one place, rather than
 * by each caller remembering to ask.
 */
bool Ext2Writable(const Ext2Superblock *superblock)
{
    if (superblock->read_only)
    {
        return Ext2WriteRefuse("the volume is read-only and may not be altered");
    }

    return true;
}

/*
 * Writes a run of bytes into one block of the volume, through the buffer cache,
 * and marks the buffer dirty.
 *
 * The mirror of Ext2ReadBytes, and bounded by the same two tests. It writes only
 * the bytes it is given: the rest of the block is whatever it held, which is
 * what allows a structure to be altered without destroying the fields around it
 * that this kernel does not parse.
 */
bool Ext2WriteBytes(BlockDevice *device, const Ext2Superblock *superblock,
                           uint32_t block, uint32_t offset, uint32_t length,
                           const uint8_t *source)
{
    uint64_t position;

    if (block >= superblock->block_count)
    {
        return Ext2WriteRefuse("a write was attempted beyond the end of the volume");
    }

    if ((offset > superblock->block_size) || (length > (superblock->block_size - offset)))
    {
        return Ext2WriteRefuse("a write was attempted beyond the end of a block");
    }

    position = ((uint64_t)block * superblock->block_size) + offset;

    while (length > 0U)
    {
        const uint64_t device_block = position / device->block_size;
        const uint32_t within = (uint32_t)(position % device->block_size);
        uint32_t put = device->block_size - within;
        Buffer *buffer;

        if (put > length)
        {
            put = length;
        }

        buffer = BufferGet(device, device_block);

        if (buffer == NULL)
        {
            return Ext2WriteRefuse("a block of the volume could not be read to be written");
        }

        for (uint32_t index = 0U; index < put; ++index)
        {
            buffer->data[within + index] = source[index];
        }

        BufferMarkDirty(buffer);
        BufferRelease(buffer);

        source += put;
        position += put;
        length -= put;
    }

    return true;
}

/* Fills a whole block of the volume with zeroes, a block being handed out with
 * whatever it last held otherwise. */
bool Ext2ZeroBlock(BlockDevice *device, const Ext2Superblock *superblock,
                          uint32_t block)
{
    uint8_t zeroes[EXT2_MAXIMUM_BLOCK_SIZE];

    Ext2FillZero(zeroes, superblock->block_size);

    return Ext2WriteBytes(device, superblock, block, 0U, superblock->block_size, zeroes);
}

/*
 * The number of block pointers one block holds. Every level of the indirection
 * branches by this, and the file block index is decomposed in terms of it.
 */
uint32_t Ext2PointersPerBlock(const Ext2Superblock *superblock)
{
    return superblock->block_size / EXT2_BLOCK_POINTER_SIZE;
}

/* Fills a run of bytes with zeroes. There is no C library until Phase 7. */
void Ext2FillZero(uint8_t *destination, uint32_t length)
{
    for (uint32_t index = 0U; index < length; ++index)
    {
        destination[index] = 0U;
    }
}

/* How many 512-byte sectors one block of the volume occupies, i_blocks being
 * counted in sectors and not in blocks. */
uint32_t Ext2SectorsPerBlock(const Ext2Superblock *superblock)
{
    return superblock->block_size / 512U;
}

uint64_t Ext2NamesInserted(void)
{
    return Ext2NamesInsertedCount;
}

uint64_t Ext2NamesRemoved(void)
{
    return Ext2NamesRemovedCount;
}

uint64_t Ext2FilesCreated(void)
{
    return Ext2FilesCreatedCount;
}

uint64_t Ext2FilesDestroyed(void)
{
    return Ext2FilesDestroyedCount;
}

uint64_t Ext2BlocksAllocated(void)
{
    return Ext2BlocksAllocatedCount;
}

uint64_t Ext2BlocksFreed(void)
{
    return Ext2BlocksFreedCount;
}

uint64_t Ext2InodesAllocated(void)
{
    return Ext2InodesAllocatedCount;
}

uint64_t Ext2InodesFreed(void)
{
    return Ext2InodesFreedCount;
}

uint64_t Ext2BytesWritten(void)
{
    return Ext2BytesWrittenCount;
}

uint64_t Ext2WritesRefused(void)
{
    return Ext2WritesRefusedCount;
}

const char *Ext2LastError(void)
{
    return Ext2Error;
}

uint64_t Ext2VolumesRead(void)
{
    return Ext2Read;
}

uint64_t Ext2VolumesRefused(void)
{
    return Ext2Refused;
}

