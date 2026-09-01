/*
 * File: kernel/include/oxys/buffer.h
 * Purpose: Declares the buffer cache: the store of recently used blocks that
 *          stands between the block layer and everything that reads a medium,
 *          together with the reference discipline that governs how long a
 *          caller may hold one and the write-back discipline that governs when
 *          a modified block reaches the device.
 * Key definitions: Buffer, BufferInitialise, BufferGet, BufferRelease,
 *          BufferMarkDirty, BufferFlush, BufferSync, BufferInvalidateDevice,
 *          BufferReport.
 * References:
 *   - docs/storage/BUFFER.md: the design of the cache, the eviction policy and the
 *     reasons for write-back rather than write-through.
 *   - docs/storage/BLOCK.md: the layer beneath, whose devices this cache holds blocks
 *     of, and whose withdrawal discipline requires BufferInvalidateDevice.
 */

#ifndef OXYS_BUFFER_H
#define OXYS_BUFFER_H

#include <oxys/types.h>
#include <oxys/block.h>

/* The number of blocks held at once, and the size of each. */
#define BUFFER_CAPACITY   64U
#define BUFFER_BLOCK_SIZE BLOCK_SIZE_DEFAULT

/*
 * One cached block.
 *
 * The structure is exposed because a caller reads and writes through `data`
 * directly; the remaining fields are the cache's own and are not to be modified,
 * a caller that adjusted the block number or the flags by hand having told the
 * cache that some other block's contents are these.
 */
typedef struct Buffer
{
    BlockDevice *device; /* Null while the buffer holds nothing. */
    uint64_t block;
    uint8_t *data;
    uint32_t references; /* Non-zero while a caller holds the buffer. */
    bool valid;
    bool dirty;
} Buffer;

/*
 * Prepares the cache, allocating the storage of every buffer from the kernel
 * heap. Returns false if that allocation failed, in which case every operation
 * below reports failure and nothing is cached.
 */
bool BufferInitialise(void);

/*
 * Obtains the buffer holding a block of a device, reading it from the device if
 * it is not already held, and returns it with a reference taken. Every buffer so
 * returned must be given back by BufferRelease.
 *
 * Returns null if the device or the block is not addressable, if the read
 * failed, or if every buffer is presently held by somebody — a cache cannot
 * evict what a caller is still using, and the alternative to refusing is to hand
 * out the same buffer twice.
 */
Buffer *BufferGet(BlockDevice *device, uint64_t block);

/*
 * Returns a buffer to the cache. The contents remain cached and may be found by
 * a later BufferGet; only the caller's claim upon it is given up.
 */
void BufferRelease(Buffer *buffer);

/*
 * Records that the buffer's contents differ from the device's. The block is
 * written back when the buffer is evicted, flushed or synchronised, and not
 * before; see docs/storage/BUFFER.md, Section 4.
 */
void BufferMarkDirty(Buffer *buffer);

/*
 * Writes a buffer back to its device if it is dirty, and clears the mark.
 * Returns false if the write failed, in which case the buffer remains dirty.
 * A buffer that is not dirty succeeds and does nothing.
 */
bool BufferFlush(Buffer *buffer);

/*
 * Writes back every dirty buffer. Returns false if any write failed, having
 * attempted all of them: a failure to write one block is no reason to abandon
 * the rest.
 */
bool BufferSync(void);

/*
 * Writes back every dirty buffer of a device and then discards all of its
 * buffers, so that nothing in the cache refers to it. It must be called before
 * a device is withdrawn from the block layer: a buffer written back afterwards
 * would be written to whatever was registered in its place.
 *
 * Returns false, having discarded nothing, if any buffer of the device is
 * presently held by a caller, and false if a write-back failed.
 */
bool BufferInvalidateDevice(BlockDevice *device);

/* The number of buffers the cache holds in total, and how many are in use. */
size_t BufferCount(void);
size_t BufferValidCount(void);
size_t BufferDirtyCount(void);
size_t BufferHeldCount(void);

/* Accounting, read by the boot-time self-test and by BufferReport. */
uint64_t BufferHits(void);
uint64_t BufferMisses(void);
uint64_t BufferEvictions(void);
uint64_t BufferWriteBacks(void);

/*
 * The number of transfers that failed beneath the cache: a block that could not
 * be read, or a dirty block that could not be written back. It is counted apart
 * from the refusals of the block layer, which are the layer working.
 */
uint64_t BufferFailures(void);

/* Writes the state of the cache and its accounting to the console. */
void BufferReport(void);

#endif /* OXYS_BUFFER_H */
